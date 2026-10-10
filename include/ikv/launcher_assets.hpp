#pragma once

#include <QDeadlineTimer>
#include <QString>
#include <QStringList>
#include <QtGlobal>

namespace ikv::launcher_assets {

struct Report {
    QStringList errors;
    QStringList missing;
    QStringList changed;
    QStringList extra;
    qsizetype files = 0;
    quint64 bytes = 0;
    bool ok() const {
        return errors.isEmpty() && missing.isEmpty() && changed.isEmpty() && extra.isEmpty();
    }
};

// Synchronous local I/O; run on a worker when invoked from the launcher UI.
// The manifest trusts the user's selected snapshot, not publisher authenticity.
QString assetRoot(const QString& installRoot);
QString manifestPath(const QString& installRoot);
// Deadlines are checked between synchronous filesystem calls, not inside kernel I/O.
Report importAssets(const QString& selectedSource, const QString& installRoot,
                    QDeadlineTimer deadline = QDeadlineTimer(QDeadlineTimer::Forever));
Report verifyAssets(const QString& installRoot);
// Unexpected files are reported and preserved; only manifest files are replaced.
Report repairAssets(const QString& selectedSource, const QString& installRoot);

} // namespace ikv::launcher_assets
