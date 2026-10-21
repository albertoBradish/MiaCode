#pragma once

#include <QString>

namespace miacode::runtime_paths {

// The hand-order build only runs with a dedicated workspace runtime root.
// Initialize this before startup diagnostics or any preference access.
bool initializeFromEnvironment(QString* error = nullptr);

QString rootDirectory();
QString workspaceDirectory();
QString configDirectory();
QString cacheDirectory();
QString dataDirectory();
QString logDirectory();
QString tempDirectory();
QString exportsDirectory();
QString projectDirectoryForChart(const QString& chartFilePath);

}  // namespace miacode::runtime_paths
