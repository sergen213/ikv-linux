#pragma once

#include "ikv/launcher_assets.hpp"
#include <QString>

namespace ikv::launcher_extraction {

struct Result {
    QString sourcePath;
    launcher_assets::Report report;
};

// Synchronous Linux-native extraction; invoke on a worker. Only the recorded
// original installer is accepted. Throws on rejection; never retains its source.
Result extractAndImport(const QString& installer, const QString& installationRoot,
                        const QString& nativeExtractor);

} // namespace ikv::launcher_extraction
