#include "common/RuntimePaths.h"

#include <QDir>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QSettings>

namespace miacode::runtime_paths {
namespace {

QString initializedRoot;
QString initializedWorkspace;

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

bool markerMatches(const QString& path, const QByteArray& expected)
{
    QFile marker(path);
    return !QFileInfo(marker).isSymLink() && marker.open(QIODevice::ReadOnly)
        && marker.read(128).trimmed() == expected && marker.atEnd();
}

}  // namespace

bool initializeFromEnvironment(QString* error)
{
    const QString requested = qEnvironmentVariable("MIACODE_HAND_ORDER_RUNTIME_ROOT").trimmed();
    if (requested.isEmpty()) {
        return fail(error, QStringLiteral("MIACODE_HAND_ORDER_RUNTIME_ROOT is required"));
    }

    const QFileInfo requestedInfo(QDir::cleanPath(requested));
    if (!requestedInfo.isAbsolute() || requestedInfo.isSymLink()
        || QFileInfo(requestedInfo.absolutePath()).isSymLink()) {
        return fail(error, QStringLiteral("runtime root must be an absolute hand-order runtime directory"));
    }

    QDir workspace = requestedInfo.dir();
    const bool portablePackage =
        requestedInfo.fileName() == QLatin1String("runtime")
        && markerMatches(workspace.filePath(QStringLiteral("HandOrderPackage.marker")),
            QByteArrayLiteral("MiaCodeHandOrder portable package v1"))
        && QFileInfo::exists(workspace.filePath(QStringLiteral("app/MiaCode.exe")));
    const bool developmentWorkspace = !portablePackage && workspace.dirName() == QLatin1String("runtime")
        && workspace.cdUp()
        && QFileInfo::exists(workspace.filePath(QStringLiteral("baseline.json")))
        && QFileInfo::exists(workspace.filePath(QStringLiteral("source/AGENTS.md")))
        && markerMatches(QDir(requestedInfo.absoluteFilePath()).filePath(QStringLiteral(".hand-order-runtime")),
            QByteArrayLiteral("MiaCodeHandOrder.runtime.v1"));
    if (!developmentWorkspace && !portablePackage) {
        return fail(error, QStringLiteral("runtime root parent is not a hand-order workspace or package"));
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
    const QString expectedRoot = QDir(workspace.canonicalPath()).filePath(portablePackage ? QStringLiteral("runtime")
        : QStringLiteral("runtime/") + requestedInfo.fileName());
    if (canonicalRoot.isEmpty()
        || canonicalRoot.compare(QDir::cleanPath(expectedRoot), Qt::CaseInsensitive) != 0) {
        return fail(error, QStringLiteral("runtime root resolves outside its hand-order directory: %1").arg(root));
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
            return fail(error, QStringLiteral("runtime directory resolves outside its hand-order directory: %1").arg(path));
        }
    }

    const QString exports = QDir(workspace.canonicalPath()).filePath(QStringLiteral("exports"));
    if (!QDir().mkpath(exports)
        || QFileInfo(exports).canonicalFilePath().compare(exports, Qt::CaseInsensitive) != 0) {
        return fail(error, QStringLiteral("cannot prepare hand-order exports directory: %1").arg(exports));
    }

    initializedRoot = canonicalRoot;
    initializedWorkspace = workspace.canonicalPath();
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, configDirectory());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, configDirectory());
    return true;
}

QString rootDirectory() { return initializedRoot; }
QString workspaceDirectory() { return initializedWorkspace; }
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
