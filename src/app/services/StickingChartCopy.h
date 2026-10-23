#pragma once

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>

namespace miacode::sticking {

inline constexpr auto chartFileName = "maidata_sticking.txt";

// Local project opens always edit a byte-for-byte copy of maidata.txt.
// An existing Sticking chart is authoritative and is never replaced here.
inline QString editableChartPath(const QString& requestedPath)
{
    if (requestedPath.isEmpty()) return {};
    const QFileInfo requested(requestedPath);
    QString sourcePath = requested.absoluteFilePath();
    if (requested.isDir()) {
        const QDir directory(sourcePath);
        const QString copyPath = directory.filePath(QLatin1String(chartFileName));
        if (QFileInfo(copyPath).isFile()) return copyPath;
        sourcePath = directory.filePath(QStringLiteral("maidata.txt"));
    }
    const QFileInfo source(sourcePath);
    if (source.fileName().compare(QStringLiteral("maidata.txt"), Qt::CaseInsensitive) != 0)
        return sourcePath;
    const QString copyPath = source.dir().filePath(QLatin1String(chartFileName));
    if (QFileInfo::exists(copyPath)) return QFileInfo(copyPath).isFile() ? copyPath : QString();
    if (!source.isFile()) return {};
    if (!QFile::copy(sourcePath, copyPath) && !QFileInfo(copyPath).isFile()) return {};
    return copyPath;
}

} // namespace miacode::sticking
