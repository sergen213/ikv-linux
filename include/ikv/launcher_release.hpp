#pragma once

#include <QString>
#include <QStringList>

// Installed signed releases (docs/LAUNCHER_UPDATES.md "Self-update"), shared by the launcher and the hosted-server
// supervisor: trust material, the verified launcher of <install-root>'s current release, and the process handoff into it.
namespace ikv::launcher_release {

// Settings (QSettings IKVCommunity/IKVLauncher): installation/root (default AppLocalData) and updates/trust.
QString installRoot();
QString trustSetting();
// ikv-updater and verify_release take a PEM path: an empty trust setting selects the compiled-in release key,
// materialised once per process in a private temporary file (removed by execInto).
QString trustPem(const QString& configured);

struct Installed {
    QString version;  // empty when nothing usable is installed
    QString launcher; // the verified launcher payload: bin/ikv-linux.AppImage or bin/ikv-launcher.exe
};
// The signed, hash-verified current release of root. onlyNewer: empty unless SemVer-newer than this build.
// Throws (Turkish) when a release is installed but fails verification.
Installed installed(const QString& root, const QString& trust, bool onlyNewer);

// Startup arguments before QApplication strips Qt options; handoff/restart re-run the new launcher with exactly these.
void rememberArguments(int argc, char** argv);
// Run launcher with the remembered arguments and IKV_UPDATE_HANDOFF=1 (which stops a handoff loop). Linux replaces this
// process (exec); Windows starts it detached and exits this process. Returns only by throwing.
[[noreturn]] void execInto(const QString& launcher);
// Same, with explicit arguments instead of the remembered ones (the hosted-server supervisor hands itself over).
[[noreturn]] void execInto(const QString& launcher, const QStringList& arguments);
// Startup handoff: an installed, verified release newer than this build takes over. Returns the Turkish reason when an
// installed release exists but cannot be used (shown, never ignored); empty when this build stays in charge.
QString handoff();

} // namespace ikv::launcher_release
