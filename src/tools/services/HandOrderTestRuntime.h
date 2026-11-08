#pragma once

#include "common/RuntimePaths.h"

#include <QDir>
#include <QFile>

namespace miacode::test_runtime {

inline bool configureHandOrderRuntime(const QDir& owner, QString* error)
{
    const QString runtime = owner.filePath(QStringLiteral("runtime/handorder_spec"));
    const auto write = [](const QString& path, const QByteArray& bytes) {
        QFile file(path);
        return file.open(QIODevice::WriteOnly | QIODevice::NewOnly) && file.write(bytes) == bytes.size();
    };
    if (!QDir().mkpath(runtime) || !owner.mkpath(QStringLiteral("source"))
        || !write(owner.filePath(QStringLiteral("baseline.json")), QByteArrayLiteral("{}\n"))
        || !write(owner.filePath(QStringLiteral("source/AGENTS.md")), QByteArrayLiteral("Synthetic workspace\n"))
        || !write(QDir(runtime).filePath(QStringLiteral(".hand-order-runtime")), QByteArrayLiteral("MiaCodeHandOrder.runtime.v1\n"))) {
        if (error) *error = QStringLiteral("Cannot create synthetic hand-order runtime markers");
        return false;
    }
    return qputenv("MIACODE_HAND_ORDER_RUNTIME_ROOT", runtime.toUtf8())
        && runtime_paths::initializeFromEnvironment(error) && runtime_paths::configureProcessEnvironment(error);
}

} // namespace miacode::test_runtime
