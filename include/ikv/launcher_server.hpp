#pragma once

#include <QByteArray>
#include <QString>
#include <functional>
#include <utility>

class QDialog;
class QPushButton;
class QWidget;

// Launcher-hosted local server (docs/SELF_HOSTING.md "Sunucu Kur"): guided creation, the native
// "Sunucu Yönetimi" panel over the private admin API, and the detached supervisor that owns
// PostgreSQL, ikv-auth and ikv-server.
namespace ikv::launcher_server {

struct Hooks {
    std::function<QString()> installRoot; // the world reads <installRoot>/assets
    std::function<bool()> hasAssets;
    std::function<void(const QString&)> log; // launcher diagnostics
    // Stores the Localhost profile (empty auth resets it to the default origin). select switches to it;
    // a nonempty password then also logs the launcher in.
    std::function<void(const QString& auth, const QString& ca, const QString& user, const QString& password, bool select)> useProfile;
    // The launcher's current login on the Localhost profile: {auth origin, session token}, or empty. Sunucu Yönetimi reuses
    // it for the private admin API when it is this hosted server (wave 31: no second login).
    std::function<std::pair<QString, QByteArray>()> launcherSession;
};

// The "hostDialog" panel. chip (a header button) shows the live server state and opens the panel.
QDialog* createHostPanel(QWidget* parent, QPushButton* chip, Hooks hooks);

// `ikv-launcher --host-supervisor DIR --asset-root PATH [--bootstrap-admin NAME]` (password on stdin):
// daemonizes, holds DIR/supervisor.lock and runs the server until stopped.
// `ikv-launcher --host-lan on|off DIR`: the host page's LAN switch (applied at the server's next start).
bool isSupervisorInvocation(int argc, char** argv);
int supervisorMain(int argc, char** argv);

} // namespace ikv::launcher_server
