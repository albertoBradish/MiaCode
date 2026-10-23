#include "common/RuntimePaths.h"

#include <QDir>
#include <QCryptographicHash>
#include <QFileInfo>

namespace miacode::runtime_paths {
namespace {

QString initializedRoot;

QString childDirectory(const QString& name)
{
    if (initializedRoot.isEmpty()) {
        return {};
    }
    return QDir(initializedRoot).filePath(name);
}

bool fail(QString* error, const QString& message)
{
    if (error != nullptr) {
        *error = message;
    }
    return false;
}

}  // namespace

bool initializeFromEnvironment(QString* error)
{
    const QString requested = qEnvironmentVariable("MIACODE_HAND_ORDER_RUNTIME_ROOT").trimmed();
    if (requested.isEmpty()) {
        return fail(error, QStringLiteral("MIACODE_HAND_ORDER_RUNTIME_ROOT is required"));
    }

    const QFileInfo requestedInfo(QDir::cleanPath(requested));
    if (!requestedInfo.isAbsolute() || requestedInfo.fileName() != QStringLiteral("runtime")) {
        return fail(error, QStringLiteral("runtime root must be an absolute Sticking runtime directory"));
    }

    const QDir workspace = requestedInfo.dir();
    const bool developmentWorkspace =
        QFileInfo::exists(workspace.filePath(QStringLiteral("baseline.json")))
        && QFileInfo::exists(workspace.filePath(QStringLiteral("source/AGENTS.md")));
    const bool portablePackage =
        QFileInfo::exists(workspace.filePath(QStringLiteral("StickingPackage.marker")))
        && QFileInfo::exists(workspace.filePath(QStringLiteral("app/MiaCode.exe")));
    if (!developmentWorkspace && !portablePackage) {
        return fail(error, QStringLiteral("runtime root parent is not a Sticking workspace or package"));
    }

    const QString root = requestedInfo.absoluteFilePath();
    if (!initializedRoot.isEmpty()) {
        if (initializedRoot.compare(root, Qt::CaseInsensitive) == 0) {
            return true;
        }
        return fail(error, QStringLiteral("runtime root was already initialized differently"));
    }

    if (!QDir().mkpath(root)) {
        return fail(error, QStringLiteral("cannot create runtime root: %1").arg(root));
    }

    const QString canonicalRoot = QFileInfo(root).canonicalFilePath();
    const QString expectedRoot = QDir(workspace.canonicalPath()).filePath(QStringLiteral("runtime"));
    if (canonicalRoot.isEmpty()
        || canonicalRoot.compare(QDir::cleanPath(expectedRoot), Qt::CaseInsensitive) != 0) {
        return fail(error, QStringLiteral("runtime root resolves outside its Sticking directory: %1").arg(root));
    }

    for (const QString& name : {QStringLiteral("config"), QStringLiteral("cache"),
                                QStringLiteral("data"), QStringLiteral("logs"),
                                QStringLiteral("temp")}) {
        const QString path = QDir(canonicalRoot).filePath(name);
        if (!QDir().mkpath(path)) {
            return fail(error, QStringLiteral("cannot create runtime directory: %1").arg(path));
        }
        const QString canonicalPath = QFileInfo(path).canonicalFilePath();
        if (canonicalPath.compare(path, Qt::CaseInsensitive) != 0) {
            return fail(error, QStringLiteral("runtime directory resolves outside its Sticking directory: %1").arg(path));
        }
    }

    const QString exports = QDir(workspace.canonicalPath()).filePath(QStringLiteral("exports"));
    if (!QDir().mkpath(exports)
        || QFileInfo(exports).canonicalFilePath().compare(exports, Qt::CaseInsensitive) != 0) {
        return fail(error, QStringLiteral("cannot prepare Sticking exports directory: %1").arg(exports));
    }

    initializedRoot = canonicalRoot;
    return true;
}

QString rootDirectory() { return initializedRoot; }
QString workspaceDirectory()
{
    return initializedRoot.isEmpty() ? QString() : QFileInfo(initializedRoot).dir().absolutePath();
}
QString configDirectory() { return childDirectory(QStringLiteral("config")); }
QString cacheDirectory() { return childDirectory(QStringLiteral("cache")); }
QString dataDirectory() { return childDirectory(QStringLiteral("data")); }
QString logDirectory() { return childDirectory(QStringLiteral("logs")); }
QString tempDirectory() { return childDirectory(QStringLiteral("temp")); }
QString exportsDirectory()
{
    const QString workspace = workspaceDirectory();
    return workspace.isEmpty() ? QString() : QDir(workspace).filePath(QStringLiteral("exports"));
}

QString projectDirectoryForChart(const QString& chartFilePath)
{
    if (initializedRoot.isEmpty() || chartFilePath.trimmed().isEmpty()) {
        return {};
    }
    const QFileInfo chartInfo(chartFilePath);
    const QFileInfo directoryInfo(chartInfo.absolutePath());
    const QString directory = directoryInfo.canonicalFilePath().isEmpty()
        ? directoryInfo.absoluteFilePath() : directoryInfo.canonicalFilePath();
    const QByteArray hash = QCryptographicHash::hash(
        QDir::cleanPath(directory).toUtf8(), QCryptographicHash::Sha256).toHex();
    return QDir(dataDirectory()).filePath(
        QStringLiteral("projects/%1").arg(QString::fromLatin1(hash)));
}

}  // namespace miacode::runtime_paths
