#include "app/services/CliIntegration.h"
#include "app/services/PreferenceDocument.h"
#include "common/RuntimePaths.h"
#include "tools/services/HandOrderTestRuntime.h"
#include <QCoreApplication>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTextStream>

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir directory(QDir(QStringLiteral(MIACODE_TEST_OUTPUT_ROOT)).filePath(QStringLiteral("cli-spec-XXXXXX")));
    if (!directory.isValid()) return 1;
    bool ok = true;
    auto expect = [&ok](bool value, const char* message) {
        if (!value) { ok = false; QTextStream(stderr) << "FAIL: " << message << '\n'; }
    };
    auto write = [](const QString& path, const QByteArray& bytes) {
        QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
    };
    QString error;
    if (!miacode::test_runtime::configureHandOrderRuntime(QDir(directory.path()), &error)) {
        QTextStream(stderr) << error << Qt::endl;
        return 1;
    }
    auto preferences = PreferenceDocument::loadPreferencesObject();
    preferences.insert(QStringLiteral("preserve_sentinel"), QStringLiteral("unchanged"));
    expect(PreferenceDocument::savePreferencesObject(preferences), "fixture preferences");
    miacode::CliIntegration integration(nullptr, directory.filePath(QStringLiteral("commands")));
    expect(integration.commandName() == QStringLiteral("miacode-handorder-public"), "public command does not collide with Sticking");
    expect(!integration.enabled(), "CLI disabled by default");
    expect(integration.setEnabled(true), "install without changing user PATH");
    expect(integration.enabled() && QFileInfo::exists(integration.commandPath()), "enabled and command exists");
    expect(integration.setEnabled(true), "owned command reinstall is idempotent");
    expect(PreferenceDocument::loadPreferencesObject().value(QStringLiteral("preserve_sentinel")).toString()
        == QStringLiteral("unchanged"), "unrelated preferences preserved");
    expect(write(integration.commandPath(), "foreign command\n"), "collision fixture");
    expect(!integration.setEnabled(true), "foreign command cannot be overwritten");
    expect(integration.setEnabled(false) && !integration.enabled(), "disable gate even if command was replaced");
    QFile foreign(integration.commandPath());
    expect(foreign.open(QIODevice::ReadOnly) && foreign.readAll() == QByteArray("foreign command\n"), "foreign command preserved");
    foreign.close();
    QFile::remove(integration.commandPath());
    expect(integration.setEnabled(true) && integration.setEnabled(false), "owned install uninstall");
    expect(!QFileInfo::exists(integration.commandPath()), "owned command removed");
#ifdef Q_OS_WIN
    expect(integration.setEnabled(true), "install Unicode-safe wrapper for inspection");
    QFile wrapper(integration.commandPath());
    expect(wrapper.open(QIODevice::ReadOnly), "read generated wrapper");
    const QByteArray wrapperText = wrapper.readAll();
    wrapper.close();
    expect(wrapperText.contains("chcp 65001") && wrapperText.contains("chcp %_HAND_ORDER_CP%")
        && wrapperText.contains("exit /b %_HAND_ORDER_EXIT%"), "wrapper restores code page and preserves process exit status");
    expect(integration.setEnabled(false), "remove inspected wrapper");
#endif
    // Inject profile paths under the temporary workspace. These tests never
    // access the real home directory or Windows environment registry.
    const QString profiles = directory.filePath(QStringLiteral("profiles"));
    expect(QDir().mkpath(profiles), "isolated profile directory");
    const QString zprofile = QDir(profiles).filePath(QStringLiteral(".zprofile"));
    const QString bashProfile = QDir(profiles).filePath(QStringLiteral(".bash_profile"));
    const QByteArray userZ = "# user configuration\nexport SENTINEL=keep\n";
    const QByteArray userBash = "# another user configuration\n";
    expect(write(zprofile, userZ) && write(bashProfile, userBash), "profile fixtures");
    miacode::CliIntegration withProfiles(nullptr, directory.filePath(QStringLiteral("profile-commands")), profiles);
    auto read = [](const QString& path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray(); };
    expect(withProfiles.setEnabled(true), "profile registration");
    expect(read(zprofile).startsWith(userZ) && read(bashProfile).startsWith(userBash), "profile registration preserves all user bytes");
    expect(withProfiles.setEnabled(false) && read(zprofile) == userZ && read(bashProfile) == userBash,
        "unregister removes only owned blocks");
    QFile::remove(bashProfile);
    expect(QDir().mkdir(bashProfile), "invalid second profile fixture");
    expect(!withProfiles.setEnabled(true) && read(zprofile) == userZ && !QFileInfo::exists(withProfiles.commandPath()),
        "preflight failure leaves first profile and command untouched");
    QDir().rmdir(bashProfile);
    expect(write(bashProfile, userBash), "restore second profile");
    QFile::remove(bashProfile);
    const QString fallbackProfile = QDir(profiles).filePath(QStringLiteral(".profile"));
    expect(write(fallbackProfile, userBash), "Bash fallback profile fixture");
    expect(withProfiles.setEnabled(true) && !QFileInfo::exists(bashProfile)
        && read(fallbackProfile).startsWith(userBash), "registration preserves Bash login profile precedence");
    expect(withProfiles.setEnabled(false) && read(fallbackProfile) == userBash,
        "fallback profile unregister preserves user configuration");
    QFile::remove(fallbackProfile);
    expect(write(bashProfile, userBash), "restore regular Bash profile fixture");
    const QString preferencePath = miacode::runtime_paths::configFile("preferences_v2.json");
    const QString heldPath = preferencePath + QStringLiteral(".held");
    const auto beforePreferences = PreferenceDocument::loadPreferencesObject();
    expect(QFile::rename(preferencePath, heldPath) && QDir().mkdir(preferencePath), "block preference writes");
    expect(!withProfiles.setEnabled(true) && !QFileInfo::exists(withProfiles.commandPath())
        && read(zprofile) == userZ && read(bashProfile) == userBash,
        "preference failure rolls back command and both profiles");
    expect(PreferenceDocument::loadPreferencesObject() == beforePreferences, "failed preference write restores memory snapshot");
    QDir().rmdir(preferencePath);
    expect(QFile::rename(heldPath, preferencePath), "restore preference fixture");
    return ok ? 0 : 1;
}
