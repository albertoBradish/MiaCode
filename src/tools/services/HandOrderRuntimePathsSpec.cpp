#include "common/RuntimePaths.h"

#include <QDir>
#include <QFileInfo>
#include <QStringList>
#include <QTextStream>

int main()
{
    QTextStream out(stdout);
    const QByteArray expectedRoot = qgetenv("MIACODE_HAND_ORDER_RUNTIME_ROOT");
    if (expectedRoot.isEmpty()) {
        out << "FAIL: MIACODE_HAND_ORDER_RUNTIME_ROOT is required for this spec\n";
        return 1;
    }

    const QDir workspace = QFileInfo(QString::fromLocal8Bit(expectedRoot)).dir();
    const QByteArray invalidRoot = workspace.filePath(QStringLiteral("exports/runtime")).toLocal8Bit();
    qputenv("MIACODE_HAND_ORDER_RUNTIME_ROOT", invalidRoot);
    QString error;
    if (miacode::runtime_paths::initializeFromEnvironment(&error) || error.isEmpty()) {
        out << "FAIL: non-workspace root was accepted\n";
        return 1;
    }

    qputenv("MIACODE_HAND_ORDER_RUNTIME_ROOT", expectedRoot);
    if (!miacode::runtime_paths::initializeFromEnvironment(&error)) {
        out << "FAIL: workspace runtime root rejected: " << error << '\n';
        return 1;
    }

    const QString root = QDir::fromNativeSeparators(miacode::runtime_paths::rootDirectory());
    const QStringList runtimeDirectories{
        miacode::runtime_paths::configDirectory(), miacode::runtime_paths::cacheDirectory(),
        miacode::runtime_paths::dataDirectory(), miacode::runtime_paths::logDirectory(),
        miacode::runtime_paths::tempDirectory()};
    for (const QString& directory : runtimeDirectories) {
        if (!QDir::fromNativeSeparators(directory).startsWith(root + QLatin1Char('/'), Qt::CaseInsensitive)
            || !QFileInfo(directory).isDir()) {
            out << "FAIL: runtime directory escaped root: " << directory << '\n';
            return 1;
        }
    }
    if (miacode::runtime_paths::exportsDirectory().compare(
            workspace.filePath(QStringLiteral("exports")), Qt::CaseInsensitive) != 0) {
        out << "FAIL: exports directory is outside the workspace\n";
        return 1;
    }

    const QString first = workspace.filePath(QStringLiteral("fixtures/hand_order_a/maidata.txt"));
    const QString sameProject = workspace.filePath(QStringLiteral("fixtures/hand_order_a/other.txt"));
    const QString otherProject = workspace.filePath(QStringLiteral("fixtures/hand_order_b/maidata.txt"));
    const QString firstDirectory = miacode::runtime_paths::projectDirectoryForChart(first);
    if (firstDirectory.isEmpty()
        || firstDirectory != miacode::runtime_paths::projectDirectoryForChart(sameProject)
        || firstDirectory == miacode::runtime_paths::projectDirectoryForChart(otherProject)
        || !firstDirectory.startsWith(miacode::runtime_paths::dataDirectory()
                                       + QStringLiteral("/projects/"), Qt::CaseInsensitive)) {
        out << "FAIL: per-project runtime data mapping is unstable\n";
        return 1;
    }

    out << "PASS: hand-order runtime paths stay in the workspace\n";
    return 0;
}
