// Owned-fixture smoke: pipe one JSON object to stdin (close stdin after writing).
// Host mode ("mode":"host-server"): required install_root (new/empty owned dir), source (owned
// genuine asset directory to import), admin_username, admin_password; optional bots (default 2), base_port
// (default 27400; base..base+3 must be free), timeout_seconds, lan_address and PNG paths install_/host_/creating_/accounts_/rates_/
// host_running_/chip_/players_/backups_/reattached_/log_screenshot_path. It drives install opt-in + import,
// guided creation (base_port deliberately occupied), auto-login, the native Sunucu Yönetimi panel (status,
// live bots, broadcast, backup, restore), keep-alive reattach, stop, auto-start and Sunucuyu sil (see
// hostServerSmoke). Needs ikv-launcher (supervisor), ikv-auth and ikv-server beside it.
// Required: install_root, source, auth_url, world_host, world_port, ca,
// username, password (optional trust: update-key override). world_host/world_port are the expected
// Play arguments: the launcher derives them from the server profile and /v1/status.
// Optional: timeout_seconds (default 300), screenshot_path (play page),
// graphics_screenshot_path (settings dialog), install_screenshot_path (first-run
// page), login_screenshot_path (new PNGs in existing, owned, nonsymlink
// directories; the GL-only graphics capture shows the unavailable stored choice).
// Optional original-content strings: model_map (explicit operator id:N mapping),
// audio_path, sound_map. No scene/avatar override or fallback player mapping.
// Character selection/creation happens in the client (no launcher character list): after Play the
// smoke requires the real child's HTTPS character screen for the fresh account (characters=0).
// Optional graphics: renderer (opengl|vulkan), render_scale (.25..1),
// appearance (original|enhanced), rt (off|auto|required); defaults opengl/1/original/off.
// The ikv-client built beside this binary and a legitimate original source must already exist.
// No assets, accounts, certificates, or client executables are fabricated here.
// The temporary settings profile is removed; the real child is terminated and
// reaped on success or failure. Fixture cleanup is external.
#define main ikv_launcher_entrypoint
#include "../src/launcher_main.cpp"
#undef main
#include <QDesktopServices>
#include <QTextBrowser>

#include <QElapsedTimer>
#include <QJsonArray>
#include <QKeyEvent>
#include <QEventLoop>
#include <QMap>
#include <QPixmap>
#include <iterator>
#include <QRegularExpression>
#include <cmath>
#include <QRadioButton>
#include <QTableWidget>
#include <QListWidget>
#include <QTcpServer>
#include <csignal>
#include <fcntl.h>
#include <unistd.h>
#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <sys/file.h>
#endif

namespace {
template <class T> T* control(Launcher& launcher, const char* name) {
    auto* widget = launcher.findChild<T*>(QString::fromLatin1(name));
    if (!widget) throw std::runtime_error(std::string("Missing GUI control: ") + name);
    return widget;
}

QString requiredString(const QJsonObject& fixture, const char* key) {
    const auto value = fixture.value(QString::fromLatin1(key));
    if (!value.isString() || value.toString().isEmpty())
        throw std::runtime_error(std::string("Missing nonempty fixture string: ") + key);
    return value.toString();
}

#ifndef _WIN32 // play mode only (the Windows build runs host-server mode)
// Make a control reachable the way a user would: open the settings dialog via its
// gear button and select the tab that contains the control.
void reveal(Launcher& launcher, QWidget* widget) {
    if (widget->window() != &launcher && !widget->window()->isVisible() &&
        widget->window()->objectName() == "settingsDialog")
        control<QPushButton>(launcher, "settingsButton")->click();
    for (auto* tabs : launcher.findChildren<QTabWidget*>())
        for (int i = 0; i < tabs->count(); ++i)
            if (tabs->widget(i)->isAncestorOf(widget)) tabs->setCurrentIndex(i);
}

void click(Launcher& launcher, const char* name) {
    auto* button = control<QPushButton>(launcher, name);
    reveal(launcher, button);
    if (!button->isVisible() || !button->isEnabled())
        throw std::runtime_error(std::string("Unavailable GUI button: ") + name);
    button->click();
}
#endif

QString currentPage(Launcher& launcher) {
    return control<QStackedWidget>(launcher, "pages")->currentWidget()->objectName();
}

void processLineRegression() {
    QByteArray out, err;
    const QByteArray token(64, 'a');
    const QString vulkan = "Vulkan submitted frame=1 provider=vulkan appearance=original hardwareRayQueries=0 accelerationStructure=0 reflections=0 shadows=0 diffuseGI=0 historyEligible=0 historyEligibleFrames=0 historyResetEvents=0 historyRetainedDirtyFrames=0 historySampleCap=32 geometryMotionVectors=0 denoise=0 renderScale=0.5";
    const QString turkish = QString::fromUtf8("İstemci başlatıldı");
    QStringList lines;
    const auto publishLine = [&](const QString& line) { lines.append(line); };
    const auto feed = [&](QByteArray& pending, const QByteArray& bytes, bool finished = false) {
        if (!frameProcessLines(pending, bytes, token, finished, publishLine))
            throw std::runtime_error("Bounded process line regression unexpectedly overflowed");
    };
    const auto vulkanBytes = vulkan.toUtf8();
    const auto split = vulkanBytes.indexOf(" historyResetEvents=");
    feed(out, vulkanBytes.left(split));
    feed(err, "stderr ");
    if (!lines.isEmpty()) throw std::runtime_error("Partial process output was published");
    feed(out, vulkanBytes.mid(split, vulkanBytes.size() - split - 1));
    feed(out, vulkanBytes.right(1) + "\r\n" + turkish.toUtf8().left(1));
    feed(err, "complete\n");
    feed(out, turkish.toUtf8().mid(1) + "\nsession=" + token.left(31));
    feed(out, token.mid(31) + "\nfirst\nsecond\nEOF ");
    feed(out, "partial\r");
    feed(err, "stderr EOF session=" + token);
    feed(out, {}, true);
    feed(err, {}, true);
    if (lines != QStringList{vulkan, "stderr complete", turkish, "session=[REDACTED]",
                             "first", "second", "EOF partial", "stderr EOF session=[REDACTED]"} ||
        !out.isEmpty() || !err.isEmpty())
        throw std::runtime_error("Process chunks changed complete lines, UTF-8, redaction or EOF");
    const auto completed = lines.size();
    feed(out, QByteArray(65536, 'x'));
    if (lines.size() != completed ||
        frameProcessLines(out, token + '\n', token, false, publishLine) ||
        !out.isEmpty() || lines.size() != completed)
        throw std::runtime_error("Oversized process line was accepted or partially published");
    feed(out, QByteArray(65536, 'x') + '\n');
    if (lines.size() != completed + 1 || lines.last() != QString(65536, 'x') || !out.isEmpty())
        throw std::runtime_error("Exact 64 KiB process line boundary was rejected");
}

#if !IKV_HAS_VULKAN
class NetworkActivity final : public QObject {
public:
    bool requested = false;
    bool eventFilter(QObject* receiver, QEvent* event) override {
        if (event->type() == QEvent::ChildAdded && qobject_cast<QNetworkAccessManager*>(receiver))
            requested = true;
        return false;
    }
};
#endif

// One HTTPS exchange pinned to the server's local CA. A nested loop keeps the GUI and child pipes live.
QJsonObject pinned(const QString& method, const QString& url, const QString& ca, const QJsonObject& body = {},
                   const QByteArray& bearer = {}, int* status = nullptr, QByteArray* raw = nullptr) {
    QFile file(ca);
    if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("Cannot read local server CA");
    QSslConfiguration tls = QSslConfiguration::defaultConfiguration();
    tls.setCaCertificates(QSslCertificate::fromData(file.readAll())); tls.setPeerVerifyMode(QSslSocket::VerifyPeer);
    QNetworkAccessManager manager;
    QNetworkRequest request{QUrl(url)}; request.setSslConfiguration(tls); request.setTransferTimeout(20000);
    if (!body.isEmpty()) request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    if (!bearer.isEmpty()) request.setRawHeader("Authorization", "Bearer " + bearer);
    auto* reply = manager.sendCustomRequest(request, method.toLatin1(), body.isEmpty() ? QByteArray() : QJsonDocument(body).toJson(QJsonDocument::Compact));
    QEventLoop loop; QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit); loop.exec();
    const auto bytes = reply->readAll();
    const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    reply->deleteLater();
    if (status) *status = code; else if (code != 200) throw std::runtime_error(("HTTPS " + method + " " + url + " failed: " + QString::number(code)).toStdString());
    if (raw) *raw = bytes;
    return QJsonDocument::fromJson(bytes).object();
}

// Host mode drives the real launcher end to end on an isolated profile: install opt-in and import, guided creation
// (with one requested port deliberately occupied), automatic launcher/admin login, the native Sunucu Yönetimi panel
// (status, live bot change, broadcast, backup, restore), keep-alive detach + reattach, auto-start, stop and delete.
int hostServerSmoke(int& argc, char** argv, const QJsonObject& fixture, QString& stage) {
    const QString installRoot = requiredString(fixture, "install_root"), source = requiredString(fixture, "source");
    const QString admin = requiredString(fixture, "admin_username"), adminPassword = requiredString(fixture, "admin_password");
    const int seconds = fixture.value("timeout_seconds").toInt(900), bots = fixture.value("bots").toInt(2);
    const int basePort = fixture.value("base_port").toInt(27400);
    const QString lan = fixture.value("lan_address").toString(); // optional: LAN mode (bind 0.0.0.0); this machine's default-route IPv4
    const QString canonicalSource = QFileInfo(source).canonicalFilePath();
    if (canonicalSource.isEmpty()) throw std::runtime_error("Host fixture source must exist");
    if (basePort < 1024 || basePort > 65000) throw std::runtime_error("base_port must be in 1024..65000");
    const QString sourceParent = QFileInfo(canonicalSource).absolutePath(), sourceName = QFileInfo(canonicalSource).fileName();
    QTemporaryDir profile;
    if (!profile.isValid()) throw std::runtime_error("Cannot create temporary launcher profile");
    qputenv("XDG_CONFIG_HOME", (profile.path() + "/config").toUtf8());
    qputenv("XDG_DATA_HOME", (profile.path() + "/data").toUtf8());
    qputenv("XDG_CACHE_HOME", (profile.path() + "/cache").toUtf8());
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("IKVCommunity");
    QCoreApplication::setApplicationName("IKVLauncher");
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, profile.path() + "/config");
    QTextBrowser browser; // captures "Yönetim paneli (tarayıcı)" instead of opening a real browser
    QDesktopServices::setUrlHandler("https", &browser, "setSource");
    QUrl opened;
    QObject::connect(&browser, &QTextBrowser::sourceChanged, &browser, [&](const QUrl& url) { opened = url; });
    const QString serverDir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/server";
    std::unique_ptr<Launcher> launcher;
    const auto ctl = [&]<class T>(T*, const char* name) { return control<T>(*launcher, name); };
    const auto label = [&](const char* name) { return ctl(static_cast<QLabel*>(nullptr), name)->text(); };
    const auto dialog = [&] { return ctl(static_cast<QDialog*>(nullptr), "hostDialog"); };
    const auto json = [](const QString& path) {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject{};
    };
    const auto supervisorAlive = [&] {
#ifdef _WIN32
        // launcher_server.cpp's flock shim locks one byte at 0x40000000 (Windows byte-range locks are mandatory).
        const int fd = _wopen(reinterpret_cast<const wchar_t*>((serverDir + "/supervisor.lock").utf16()), _O_RDONLY | _O_NOINHERIT);
        if (fd < 0) return false;
        OVERLAPPED at{}; at.Offset = 0x40000000;
        const auto handle = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
        const bool held = !LockFileEx(handle, LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &at);
        if (!held) UnlockFileEx(handle, 0, 1, 0, &at);
        _close(fd);
#else
        const int fd = ::open(QFile::encodeName(serverDir + "/supervisor.lock").constData(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) return false;
        const bool held = ::flock(fd, LOCK_SH | LOCK_NB) != 0;
        ::close(fd);
#endif
        return held;
    };
    const auto running = [&] { return label("hostStatus").startsWith(QString::fromUtf8("Çalışıyor")) && supervisorAlive(); };
    QStringList expectedBoxes; // prefixes of confirmation boxes the current step may raise; answered Yes
    QString failureText;
    QTimer boxes; boxes.setInterval(50);
    QObject::connect(&boxes, &QTimer::timeout, &app, [&] {
        for (auto* widget : QApplication::topLevelWidgets())
            if (auto* message = qobject_cast<QMessageBox*>(widget); message && message->isVisible()) {
                const auto text = message->text();
                if (std::any_of(expectedBoxes.begin(), expectedBoxes.end(), [&](const QString& prefix) { return text.startsWith(prefix); })) {
                    std::fprintf(stderr, "TRACE host-server {\"confirmed\":%s}\n", QJsonDocument(QJsonArray{text.left(60)}).toJson(QJsonDocument::Compact).constData());
                    message->button(QMessageBox::Yes)->click();
                } else { if (failureText.isEmpty()) failureText = "Launcher modal error: " + text; message->done(QMessageBox::Ok); }
            }
    });
    boxes.start();
    QElapsedTimer total; total.start();
    // Timer-driven so checks (and their side effects, e.g. accepting the file picker) also run inside the launcher's
    // own nested modal loops; a plain polling loop would be stuck below a modal exec.
    const auto waitUntil = [&](const std::function<bool()>& ready, int limitSeconds, const char* what) {
        QElapsedTimer started; started.start();
        QEventLoop loop; QTimer check; check.setInterval(50);
        std::string problem; bool done = false, inside = false;
        QObject::connect(&check, &QTimer::timeout, &loop, [&] {
            if (done || inside) return;
            inside = true; // ready() may run a nested HTTPS loop
            try {
                if (!failureText.isEmpty()) throw std::runtime_error(failureText.toStdString());
                if (ready()) done = true;
                else if (started.elapsed() > limitSeconds * 1000LL || total.elapsed() > seconds * 1000LL)
                    throw std::runtime_error(std::string("Timed out waiting for ") + what);
            } catch (const std::exception& error) { problem = error.what(); done = true; }
            inside = false;
            if (!done) return;
            if (!problem.empty()) // unblock any modal the launcher still shows so this loop can return
                for (auto* widget : QApplication::topLevelWidgets())
                    if (auto* open = qobject_cast<QDialog*>(widget); open && open->isModal() && open->isVisible()) open->reject();
            loop.quit();
        });
        check.start(); loop.exec();
        if (!problem.empty()) throw std::runtime_error(problem);
    };
    const auto click = [&](const char* name) {
        auto* button = ctl(static_cast<QPushButton*>(nullptr), name);
        if (!button->isEnabled()) throw std::runtime_error(std::string("Disabled host button: ") + name);
        QTimer::singleShot(0, button, &QPushButton::click);
    };
    const auto tab = [&](const char* title) {
        auto* tabs = ctl(static_cast<QTabWidget*>(nullptr), "hostTabs");
        for (int i = 0; i < tabs->count(); ++i) if (tabs->tabText(i) == QString::fromUtf8(title)) { tabs->setCurrentIndex(i); return; }
        throw std::runtime_error(std::string("Missing tab ") + title);
    };
    const auto capture = [&](QWidget* widget, const char* field) {
        QEventLoop settle; QTimer::singleShot(300, &settle, &QEventLoop::quit); settle.exec();
        if (!fixture.contains(field)) return;
        const auto image = widget->grab();
        QFile output(requiredString(fixture, field));
        if (image.isNull() || !output.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
            !output.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) || !image.save(&output, "PNG"))
            throw std::runtime_error(std::string("Cannot save ") + field);
    };
    const auto adminToken = [&] {
        const auto state = json(serverDir + "/state.json");
        const QString url = QString("https://127.0.0.1:%1/v1/login").arg(state.value("admin_port").toInt());
        const auto token = pinned("POST", url, serverDir + "/secrets/client-ca.pem", {{"username", admin}, {"password", adminPassword}}).value("session").toString().toLatin1();
        if (token.size() != 64) throw std::runtime_error("Admin login on the management port returned no session");
        return token;
    };
    const auto adminStatus = [&](const QByteArray& token) {
        return pinned("GET", QString("https://127.0.0.1:%1/v1/admin/status").arg(json(serverDir + "/state.json").value("admin_port").toInt()),
                      serverDir + "/secrets/client-ca.pem", {}, token);
    };
    const auto checkPrivate = [&] {
#ifdef _WIN32
        // NONCANONICAL wave 19: no POSIX modes on Windows (per-user ACL); PostgreSQL listens on 127.0.0.1 only, no socket.
        const QStringList expectedSecrets{"client-ca.pem", "development-ca-key", "dsn", "pg-superuser", "tls-ca", "tls-cert", "tls-key"};
#else
        const QStringList expectedSecrets{"client-ca.pem", "development-ca-key", "dsn", "tls-ca", "tls-cert", "tls-key"};
        const auto dirMode = QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner | QFileDevice::ReadUser | QFileDevice::WriteUser | QFileDevice::ExeUser;
        const auto fileMode = QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadUser | QFileDevice::WriteUser;
        for (const auto& dir : {serverDir, serverDir + "/secrets", serverDir + "/sock", serverDir + "/backups", serverDir + "/pg"})
            if (QFileInfo(dir).permissions() != dirMode) throw std::runtime_error(("Server directory is not mode 0700: " + dir).toStdString());
        for (const auto& name : {"host.json", "state.json", "server.log", "supervisor.lock", "launcher.lock"})
            if (QFileInfo(serverDir + "/" + name).permissions() != fileMode) throw std::runtime_error(std::string("Server file is not mode 0600: ") + name);
#endif
        const QStringList secrets = QDir(serverDir + "/secrets").entryList(QDir::Files | QDir::Hidden);
        if (secrets != expectedSecrets)
            throw std::runtime_error(("Unexpected secret set: " + secrets.join(',')).toStdString());
#ifndef _WIN32
        for (const auto& name : secrets)
            if (QFileInfo(serverDir + "/secrets/" + name).permissions() != fileMode) throw std::runtime_error(("Secret is not mode 0600: " + name).toStdString());
#endif
        QFile dsn(serverDir + "/secrets/dsn"), log(serverDir + "/server.log");
        if (!dsn.open(QIODevice::ReadOnly) || !log.open(QIODevice::ReadOnly)) throw std::runtime_error("Cannot read DSN/server.log");
        const auto match = QRegularExpression("password=([0-9a-f]{64}) ").match(QString::fromUtf8(dsn.readAll()));
        const auto logs = QString::fromUtf8(log.readAll()) + ctl(static_cast<QPlainTextEdit*>(nullptr), "hostLogs")->toPlainText();
        if (!match.hasMatch() || logs.contains(match.captured(1)) || logs.contains(adminPassword))
            throw std::runtime_error("DSN must hold a random 256-bit password; neither it nor the admin password may reach the logs");
        QFile pid(serverDir + "/pg/postmaster.pid");
        const auto lines = pid.open(QIODevice::ReadOnly) ? pid.readAll().split('\n') : QList<QByteArray>{};
#ifdef _WIN32
        if (lines.size() < 6 || !lines[4].trimmed().isEmpty() || lines[5].trimmed() != "127.0.0.1")
            throw std::runtime_error("Private PostgreSQL must listen only on 127.0.0.1 (no Unix socket)");
#else
        if (lines.size() < 6 || lines[4] != QFile::encodeName(serverDir + "/sock") || !lines[5].trimmed().isEmpty())
            throw std::runtime_error("Private PostgreSQL must listen only on its 0700 Unix socket directory (no TCP)");
#endif
    };
    const auto verifyServer = [&](const char* run) {
        const auto saved = json(serverDir + "/host.json");
        if (saved.value("admin").toString() != admin || saved.value("bind").toString() != (lan.isEmpty() ? "127.0.0.1" : "0.0.0.0") ||
            (!lan.isEmpty() && !saved.value("sans").toVariant().toStringList().contains("IP:" + lan)))
            throw std::runtime_error("host.json lacks admin/loopback bind (or LAN 0.0.0.0 bind + IP SAN)");
        const auto stored = QJsonDocument::fromJson(QSettings{}.value("profiles/Localhost").toByteArray()).object();
        const QUrl auth(stored.value("auth").toString());
        if (auth.host() != (lan.isEmpty() ? "localhost" : lan) || auth.port() != json(serverDir + "/state.json").value("auth_port").toInt() ||
            stored.value("ca").toString() != serverDir + "/secrets/client-ca.pem")
            throw std::runtime_error("Localhost profile was not pinned to the running server and CA");
        const QString ca = stored.value("ca").toString();
        int code = 0; QByteArray html;
        click("hostAdmin");
        waitUntil([&] { return !opened.isEmpty(); }, 5, "the browser admin button");
        const QUrl dashboard = std::exchange(opened, {});
        if (dashboard.host() != "127.0.0.1" || dashboard.path() != "/admin") throw std::runtime_error("Yönetim paneli (tarayıcı) did not open the loopback dashboard");
        pinned("GET", dashboard.toString(), ca, {}, {}, &code, &html);
        if (code != 200 || !html.contains("<")) throw std::runtime_error("Private dashboard was not served over the local CA");
        const auto token = adminToken();
        QUrl accounts(dashboard); accounts.setPath("/v1/admin/accounts");
        pinned("GET", accounts.toString(), ca, {}, token, &code);
        if (code != 200) throw std::runtime_error("Bootstrapped account lacks the server-assigned admin role: " + std::to_string(code));
        pinned("GET", auth.toString() + "/v1/admin/accounts", ca, {}, token, &code);
        if (code != 404) throw std::runtime_error("Player listener exposed management routes");
        const auto status = pinned("GET", auth.toString() + "/v1/status", ca);
        if (!status.value("online").toBool()) throw std::runtime_error("World is not online in /v1/status");
        std::fprintf(stderr, "TRACE host-server %s\n", QJsonDocument(QJsonObject{{"run", run}, {"status", status}, {"dashboard", dashboard.toString()},
            {"auth", auth.toString()}, {"state", json(serverDir + "/state.json")}}).toJson(QJsonDocument::Compact).constData());
        return token;
    };
    const auto botCount = [&] { bool ok = false; const int count = label("hostBotCount").toInt(&ok); return ok ? count : -1; };
    const auto newLauncher = [&] {
        launcher = std::make_unique<Launcher>();
        control<QLineEdit>(*launcher, "installRoot")->setText(installRoot);
        launcher->show();
    };
    int result = 1;
    QString reason;
    try {
        newLauncher();
        stage = "install-page";
        if (currentPage(*launcher) != "installPage") throw std::runtime_error("First run did not show the install page");
        auto* also = control<QCheckBox>(*launcher, "hostAfterInstall");
        if (also->isChecked()) throw std::runtime_error("'Also set up a local server' must be opt-in");
        also->setChecked(true);
        if (control<QPushButton>(*launcher, "hostButton")->text() != "Sunucu Kur") throw std::runtime_error("Fresh profile chip must offer Sunucu Kur");
        capture(launcher.get(), "install_screenshot_path");
        stage = "import"; click("importButton");
        bool pickerSeen = false;
        waitUntil([&] {
            for (auto* widget : QApplication::topLevelWidgets()) {
                auto* picker = qobject_cast<QFileDialog*>(widget);
                if (!picker || !picker->isVisible()) continue;
                if (!pickerSeen) { picker->setDirectory(sourceParent); pickerSeen = true; break; }
                if (picker->directory().canonicalPath() == sourceParent) { picker->setFocus(Qt::OtherFocusReason); picker->selectFile(sourceName); }
                const auto chosen = picker->selectedFiles();
                if (chosen.size() == 1 && QFileInfo(chosen.first()).canonicalFilePath() == canonicalSource && picker->directory().canonicalPath() == sourceParent)
                    QMetaObject::invokeMethod(picker, "accept", Qt::DirectConnection);
            }
            return control<QLabel>(*launcher, "status")->text().contains("Assets verified");
        }, 600, "asset import");

        stage = "guided-create";
        if (!dialog()->isVisible()) throw std::runtime_error("Import with 'also set up a local server' did not open Sunucu Kur");
        if (!ctl(static_cast<QWidget*>(nullptr), "hostCreatePage")->isVisible())
            throw std::runtime_error("Fresh profile must show the guided creation page");
        ctl(static_cast<QLineEdit*>(nullptr), "hostName")->setText("Duman Testi");
        ctl(static_cast<QLineEdit*>(nullptr), "hostAdminUser")->setText(admin);
        ctl(static_cast<QLineEdit*>(nullptr), "hostAdminPassword")->setText(adminPassword);
        ctl(static_cast<QSpinBox*>(nullptr), "hostBots")->setValue(bots);
        ctl(static_cast<QCheckBox*>(nullptr), "hostAdvanced")->setChecked(true);
        ctl(static_cast<QSpinBox*>(nullptr), "hostAuthPort")->setValue(basePort);
        ctl(static_cast<QSpinBox*>(nullptr), "hostAdminPort")->setValue(basePort + 1);
        ctl(static_cast<QSpinBox*>(nullptr), "hostWorldPort")->setValue(basePort + 2);
        if (!lan.isEmpty()) ctl(static_cast<QRadioButton*>(nullptr), "hostLan")->setChecked(true);
        QTcpServer occupied; // the requested account port is taken: creation must pick the next free one
        if (!occupied.listen(QHostAddress(lan.isEmpty() ? QString("127.0.0.1") : lan), static_cast<quint16>(basePort)))
            throw std::runtime_error("Cannot occupy the fixture base port");
        capture(dialog(), "host_screenshot_path");
        click("hostCreate");
        waitUntil([&] { return ctl(static_cast<QListWidget*>(nullptr), "hostSteps")->item(3)->text().startsWith(QString::fromUtf8("●")); }, 120,
                  "the PostgreSQL step in progress"); // provisioning takes seconds: the progress list is observable
        if (!ctl(static_cast<QListWidget*>(nullptr), "hostSteps")->item(2)->text().startsWith(QString::fromUtf8("✓"))) throw std::runtime_error("Steps out of order");
        capture(dialog(), "creating_screenshot_path");
        waitUntil([&] { return running() && control<QLabel>(*launcher, "status")->text() == QString::fromUtf8("Giriş yapıldı") &&
                               label("hostIdentity").startsWith(QString::fromUtf8("Yönetici:")); }, 900, "creation, auto-login and admin session");
        occupied.close();
        const auto state = json(serverDir + "/state.json");
        const QList<int> ports{state.value("auth_port").toInt(), state.value("admin_port").toInt(), state.value("world_port").toInt()};
        if (ports[0] == basePort || ports[0] <= 0 || ports[1] <= 0 || ports[2] <= 0 || ports[0] == ports[1] || ports[1] == ports[2] || ports[0] == ports[2])
            throw std::runtime_error("Occupied port was not replaced by a distinct free port");
        for (int i = 0; i < 8; ++i)
            if (!ctl(static_cast<QListWidget*>(nullptr), "hostSteps")->item(i)->text().startsWith(QString::fromUtf8("✓")))
                throw std::runtime_error("Creation step not completed: " + ctl(static_cast<QListWidget*>(nullptr), "hostSteps")->item(i)->text().toStdString());
        if (!ctl(static_cast<QLineEdit*>(nullptr), "hostAdminPassword")->text().isEmpty()) throw std::runtime_error("Admin password field was not cleared");
        if (currentPage(*launcher) != "playPage" || control<QComboBox>(*launcher, "profiles")->currentText() != "Localhost")
            throw std::runtime_error("Automatic launcher login on Localhost did not reach the play page");
        checkPrivate();
        auto token = verifyServer("create");

        stage = "status";
        waitUntil([&] { return botCount() == bots && label("hostPlayers") == "0" && label("hostTick").contains("ms"); }, 120, "live status with the configured bots");
        if (!control<QPushButton>(*launcher, "hostButton")->text().contains(QString::fromUtf8("Çalışıyor")) ||
            control<QPushButton>(*launcher, "hostButton")->property("hostState").toString() != "running")
            throw std::runtime_error("Header chip does not show the running server");
        capture(dialog(), "host_running_screenshot_path");
        capture(launcher.get(), "chip_screenshot_path");

        stage = "bots";
        tab("Oyuncular");
        ctl(static_cast<QSpinBox*>(nullptr), "hostBotTarget")->setValue(bots + 1);
        click("hostBotApply");
        waitUntil([&] { return botCount() == bots + 1; }, 60, "live bot count increase");
        if (json(serverDir + "/host.json").value("bots").toInt() != bots + 1) throw std::runtime_error("Bot target was not kept for the next start");

        stage = "broadcast";
        ctl(static_cast<QLineEdit*>(nullptr), "hostAnnounceText")->setText(QString::fromUtf8("Duman testi duyurusu"));
        click("hostAnnounce");
        waitUntil([&] {
            for (const auto& value : adminStatus(token).value("commands").toArray()) {
                const auto command = value.toObject();
                if (command.value("command").toString() == "announce" && command.value("arguments").toObject().value("message").toString() == QString::fromUtf8("Duman testi duyurusu") &&
                    command.value("consumed").toBool() && command.value("result").toObject().value("ok").toBool(true) && !command.value("result").isNull())
                    return true;
            }
            return false;
        }, 30, "the world to consume the announcement");
        capture(dialog(), "players_screenshot_path");

        stage = "backup";
        tab("Yedekler");
        auto* backups = ctl(static_cast<QListWidget*>(nullptr), "hostBackupsList");
        const int before = backups->count();
        click("hostBackupCreate");
        waitUntil([&] { return backups->count() == before + 1; }, 300, "a new backup in the list");
        if (QDir(serverDir + "/backups").entryList({"ikv-*.sql"}, QDir::Files).size() != before + 1) throw std::runtime_error("Backup file missing");
        capture(dialog(), "backups_screenshot_path");

        stage = "restore";
        backups->setCurrentRow(0);
        expectedBoxes = {QString::fromUtf8("Yedek geri yüklensin mi")};
        click("hostBackupRestore");
        waitUntil([&] { return !supervisorAlive(); }, 120, "the server to stop for the offline restore");
        waitUntil([&] { return running(); }, 600, "the server to start again after restore");
        expectedBoxes.clear();
        if (!label("hostNotice").contains(QString::fromUtf8("Yedek geri yüklendi")) && !ctl(static_cast<QPlainTextEdit*>(nullptr), "hostLogs")->toPlainText().contains(QString::fromUtf8("Yedek geri yüklendi")))
            throw std::runtime_error("Restore completion was not reported");
        token = verifyServer("restore");

        stage = "keep-alive";
        tab("Durum");
        ctl(static_cast<QCheckBox*>(nullptr), "hostKeepAlive")->setChecked(true);
        if (!json(serverDir + "/host.json").value("keep_alive").toBool()) throw std::runtime_error("Keep-alive was not stored");
        const auto supervisorPid = json(serverDir + "/state.json").value("pid").toInteger();
        launcher.reset();
        { QEventLoop idle; QTimer::singleShot(3000, &idle, &QEventLoop::quit); idle.exec(); } // longer than the supervisor's launcher-lock check
        if (!supervisorAlive() || json(serverDir + "/state.json").value("phase").toString() != "running")
            throw std::runtime_error("Keep-alive server did not survive the launcher");
        newLauncher();
        waitUntil([&] { return running(); }, 30, "the new launcher to reattach");
        if (json(serverDir + "/state.json").value("pid").toInteger() != supervisorPid) throw std::runtime_error("Reattach started a different supervisor");
        waitUntil([&] { return control<QPushButton>(*launcher, "hostButton")->text().contains(QString::fromUtf8("Çalışıyor")); }, 30, "the reattached chip");
        ctl(static_cast<QLineEdit*>(nullptr), "hostLoginPassword")->setText(adminPassword);
        dialog()->show();
        click("hostLogin");
        waitUntil([&] { return label("hostIdentity").startsWith(QString::fromUtf8("Yönetici:")) && botCount() == bots + 1; }, 60, "admin login after reattach");
        capture(dialog(), "reattached_screenshot_path");
        tab("Hesaplar");
        waitUntil([&] { return ctl(static_cast<QTableWidget*>(nullptr), "hostAccountsTable")->rowCount() >= 1; }, 30, "the account list");
        capture(dialog(), "accounts_screenshot_path");
        tab("Oranlar");
        waitUntil([&] { return ctl(static_cast<QListWidget*>(nullptr), "hostZonesList")->count() > 0; }, 30, "the zone list");
        capture(dialog(), "rates_screenshot_path");
        tab("Kayıt");
        capture(dialog(), "log_screenshot_path");
        tab("Durum");

        stage = "stop";
        click("hostStop");
        waitUntil([&] { return !supervisorAlive() && label("hostStatus") == "Durduruldu"; }, 120, "stop");
        if (QFileInfo::exists(serverDir + "/pg/postmaster.pid")) throw std::runtime_error("PostgreSQL survived Durdur");
        if (json(serverDir + "/state.json").value("phase").toString() != "stopped") throw std::runtime_error("Supervisor did not record a clean stop");
        {
            QFile log(serverDir + "/server.log"); // the world finished its loop and saved (not killed after the 20 s grace)
            const auto text = log.open(QIODevice::ReadOnly) ? QString::fromUtf8(log.readAll()) : QString();
            const auto at = text.lastIndexOf(QString::fromUtf8("durdurma isteği alındı"));
            if (at < 0 || text.indexOf("ikv-server: STOP tick=", at) < 0 || text.indexOf(QString::fromUtf8("ikv-server çıktı: kod=0"), at) < 0)
                throw std::runtime_error("Durdur did not stop the world gracefully (STOP tick + exit 0)");
        }

        stage = "autostart";
        ctl(static_cast<QCheckBox*>(nullptr), "hostKeepAlive")->setChecked(false);
        ctl(static_cast<QCheckBox*>(nullptr), "hostAutostart")->setChecked(true);
        launcher.reset();
        newLauncher();
        waitUntil([&] { return running(); }, 600, "auto-start on launcher open");
        if (!control<QPushButton>(*launcher, "hostButton")->text().contains(QString::fromUtf8("Çalışıyor"))) throw std::runtime_error("Auto-started chip not running");
        ctl(static_cast<QCheckBox*>(nullptr), "hostAutostart")->setChecked(false);
        launcher.reset(); // not kept alive: closing the launcher stops it
        if (supervisorAlive() || QFileInfo::exists(serverDir + "/pg/postmaster.pid")) throw std::runtime_error("Server survived closing the launcher without keep-alive");

        stage = "delete";
        newLauncher();
        dialog()->show();
        waitUntil([&] { return label("hostStatus") == "Durduruldu"; }, 10, "stopped panel");
        expectedBoxes = {QString::fromUtf8("Yerel sunucunun")};
        click("hostDelete");
        waitUntil([&] { return !QFileInfo::exists(serverDir); }, 60, "Sunucuyu sil");
        if (!QJsonDocument::fromJson(QSettings{}.value("profiles/Localhost").toByteArray()).object().value("ca").toString().isEmpty())
            throw std::runtime_error("Deleted server CA is still pinned in the Localhost profile");
        waitUntil([&] { return control<QPushButton>(*launcher, "hostButton")->text() == "Sunucu Kur"; }, 5, "chip reset");
        result = 0;
        reason = "Install opt-in, guided creation with port auto-pick, auto-login, native panel status, live bots, broadcast, backup, "
                 "restore, keep-alive reattach, admin re-login, stop, auto-start, close-stops-server and delete passed";
    } catch (const std::exception& error) {
        reason = QString::fromUtf8(error.what());
        if (launcher) reason += "; host status=" + label("hostStatus") + "; notice=" + label("hostNotice") + "; logs=" + ctl(static_cast<QPlainTextEdit*>(nullptr), "hostLogs")->toPlainText().right(2500);
    }
    boxes.stop();
    for (auto* widget : QApplication::topLevelWidgets())
        if (auto* open = qobject_cast<QDialog*>(widget); open && open->isVisible() && open->objectName() != "hostDialog") open->reject();
    launcher.reset();
    if (supervisorAlive()) { // failure path: never leave a (possibly kept-alive) server behind
#ifdef _WIN32
        if (QFile request(serverDir + "/stop.request"); !request.open(QIODevice::WriteOnly)) reason += "; cannot request the supervisor stop";
#else
        QFile lock(serverDir + "/supervisor.lock");
        if (lock.open(QIODevice::ReadOnly)) ::kill(static_cast<pid_t>(lock.readAll().trimmed().toInt()), SIGTERM);
#endif
        for (int i = 0; i < 600 && supervisorAlive(); ++i) QThread::msleep(100);
        result = 1;
    }
    if (QFileInfo::exists(serverDir + "/pg/postmaster.pid")) { result = 1; reason += "; PostgreSQL survived the launcher"; }
    std::fprintf(stderr, "%s launcher-gui stage=%s: %s\n", result ? "FAIL" : "PASS", stage.toUtf8().constData(), reason.toUtf8().constData());
    return result;
}
} // namespace

int main(int argc, char** argv) {
    QString stage = "fixture";
    QString secretPassword, ownedUsername;
    const auto redact = [&](QString message) {
        if (!secretPassword.isEmpty()) message.replace(secretPassword, "[REDACTED]");
        if (!ownedUsername.isEmpty()) message.replace(ownedUsername, "[OWNED-ACCOUNT]");
        message.replace(QRegularExpression("[0-9a-fA-F]{64}"), "[REDACTED]");
        message.replace(QRegularExpression("Bearer\\s+\\S+", QRegularExpression::CaseInsensitiveOption), "Bearer [REDACTED]");
        return message;
    };
    try {
        processLineRegression();
        QFile input;
        if (!input.open(stdin, QIODevice::ReadOnly)) throw std::runtime_error("Cannot read fixture stdin");
        QByteArray bytes = input.read(65537);
        if (bytes.size() > 65536) throw std::runtime_error("Fixture JSON exceeds 64 KiB");
        QJsonParseError parse;
        const auto document = QJsonDocument::fromJson(bytes, &parse);
        bytes.fill('\0');
        if (parse.error != QJsonParseError::NoError || !document.isObject())
            throw std::runtime_error("Stdin must contain one fixture JSON object");
        const auto fixture = document.object();
        if (fixture.value("mode").toString() == "host-server") {
            secretPassword = requiredString(fixture, "admin_password");
            ownedUsername = requiredString(fixture, "admin_username");
            return hostServerSmoke(argc, argv, fixture, stage);
        }
#ifdef _WIN32
        throw std::runtime_error("The Windows build of this smoke runs only \"mode\":\"host-server\"");
#else
        secretPassword = requiredString(fixture, "password");
        ownedUsername = requiredString(fixture, "username");
        const QString installRoot = requiredString(fixture, "install_root");
        const QString source = requiredString(fixture, "source");
        const QString auth = requiredString(fixture, "auth_url");
        const QString host = requiredString(fixture, "world_host");
        const QString certificate = requiredString(fixture, "ca");
        const QString trustKey = fixture.value("trust").toString(); // optional update-trust override; Play no longer uses it
        const double portNumber = fixture.value("world_port").toDouble(-1);
        const int port = fixture.value("world_port").toInt(-1);
        if (port < 1 || port > 65535 || portNumber != port)
            throw std::runtime_error("world_port must be an integer in 1..65535");
        const double secondsNumber = fixture.value("timeout_seconds").toDouble(300);
        const int seconds = fixture.value("timeout_seconds").toInt(300);
        if ((fixture.contains("timeout_seconds") && !fixture.value("timeout_seconds").isDouble()) ||
            seconds < 1 || seconds > 3600 || secondsNumber != seconds)
            throw std::runtime_error("timeout_seconds must be an integer in 1..3600");
        const QString requestedRenderer = fixture.value("renderer").toString("opengl");
        const QString requestedAppearance = fixture.value("appearance").toString("original");
        const QString requestedRt = fixture.value("rt").toString("off");
        const QString requestedRtStyle = fixture.value("rt_style").toString("faithful");
        const double requestedScale = fixture.value("render_scale").toDouble(1);
        if ((requestedRenderer != "opengl" && requestedRenderer != "vulkan") ||
            (requestedAppearance != "original" && requestedAppearance != "enhanced") ||
            (requestedRt != "off" && requestedRt != "auto" && requestedRt != "required") ||
            (requestedRtStyle != "faithful" && requestedRtStyle != "physical") ||
            (fixture.contains("render_scale") && !fixture.value("render_scale").isDouble()) ||
            !std::isfinite(requestedScale) || requestedScale < .25 || requestedScale > 1 ||
            (requestedRenderer == "opengl" && requestedScale != 1))
            throw std::runtime_error("Invalid requested smoke graphics");
#if !IKV_HAS_VULKAN
        if (requestedRenderer != "opengl") throw std::runtime_error("Smoke launcher was built without Vulkan");
#endif
        const auto screenshotDestination = [&](const char* field) {
            if (!fixture.contains(field)) return QString{};
            const QString path = requiredString(fixture, field);
            const QFileInfo output(path), parent(output.absolutePath());
            if (!output.isAbsolute() || QDir::cleanPath(path) != path ||
                !parent.isDir() || parent.canonicalFilePath() != parent.absoluteFilePath() ||
                parent.ownerId() != ::geteuid() || output.exists() || output.isSymLink())
                throw std::runtime_error(std::string(field) + " must be new, absolute and inside an owned nonsymlink directory");
            return path;
        };
        const QString screenshotPath = screenshotDestination("screenshot_path");
        QString graphicsScreenshotPath = screenshotDestination("graphics_screenshot_path");
        const QString installScreenshotPath = screenshotDestination("install_screenshot_path");
        const QString loginScreenshotPath = screenshotDestination("login_screenshot_path");
        {
            QStringList requested;
            for (const auto& path : {screenshotPath, graphicsScreenshotPath, installScreenshotPath, loginScreenshotPath})
                if (!path.isEmpty()) requested << path;
            if (requested.removeDuplicates())
                throw std::runtime_error("Requested launcher PNG destinations must be distinct");
        }
        if (!QFileInfo(source).isDir())
            throw std::runtime_error("Existing original source required");
        const QString canonicalSource = QFileInfo(source).canonicalFilePath();
        const QString sourceParent = QFileInfo(canonicalSource).absolutePath();
        const QString sourceName = QFileInfo(canonicalSource).fileName();
        if (canonicalSource.isEmpty() || sourceName.isEmpty())
            throw std::runtime_error("Original source must have an existing canonical directory and selection name");

        QTemporaryDir profile;
        if (!profile.isValid()) throw std::runtime_error("Cannot create temporary launcher profile");
        qputenv("XDG_CONFIG_HOME", (profile.path() + "/config").toUtf8());
        qputenv("XDG_DATA_HOME", (profile.path() + "/data").toUtf8());
        qputenv("XDG_CACHE_HOME", (profile.path() + "/cache").toUtf8());
        QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
        QApplication app(argc, argv);
        QCoreApplication::setOrganizationName("IKVCommunity");
        QCoreApplication::setApplicationName("IKVLauncher");
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, profile.path() + "/config");
        // OYNA runs the client bundled beside the launcher (here: beside this smoke binary).
        const QString bundledClient = QCoreApplication::applicationDirPath() + "/ikv-client";
        if (!QFileInfo(bundledClient).isExecutable())
            throw std::runtime_error("ikv-client must be built beside launcher-gui-smoke");
        {
            QSettings saved;
            for (const auto* key : {"graphics/sceneMember", "graphics/zone", "graphics/characterModel"})
                saved.setValue(QString::fromLatin1(key), "obsolete-source-override");
            saved.sync();
            if (saved.status() != QSettings::NoError)
                throw std::runtime_error("Cannot seed private obsolete launcher settings");
        }
#if !IKV_HAS_VULKAN
        {
            QSettings saved;
            saved.setValue("graphics/renderer", "vulkan");
            saved.setValue("graphics/render_scale", .5);
            saved.sync();
            if (saved.status() != QSettings::NoError)
                throw std::runtime_error("Cannot seed private saved Vulkan profile");
        }
#endif
        Launcher launcher;
        const auto set = [&](const char* name, const QString& value) {
            control<QLineEdit>(launcher, name)->setText(value);
        };
        set("installRoot", installRoot); set("assetSource", source); set("updateTrust", trustKey);
        for (const auto& field : {
                 std::pair{"model_map", "modelMap"}, std::pair{"audio_path", "audioPath"}, std::pair{"sound_map", "soundMap"}}) {
            const auto value = fixture.value(QString::fromLatin1(field.first));
            if (value.isUndefined()) continue;
            if (!value.isString())
                throw std::runtime_error(std::string("Optional fixture field must be a string: ") + field.first);
            set(field.second, value.toString());
        }
#if IKV_HAS_VULKAN
        control<QComboBox>(launcher, "renderer")->setCurrentText(requestedRenderer);
        control<QDoubleSpinBox>(launcher, "renderScale")->setValue(requestedScale);
#endif
        control<QComboBox>(launcher, "appearance")->setCurrentText(requestedAppearance);
        control<QComboBox>(launcher, "rayTracing")->setCurrentText(requestedRt);
        control<QComboBox>(launcher, "rtStyle")->setCurrentText(requestedRtStyle);
        auto* statusLabel = control<QLabel>(launcher, "status");
        auto* logs = control<QPlainTextEdit>(launcher, "diagnosticLogs");
        launcher.show();
        const auto capture = [&](QWidget* widget, const QString& path) {
            if (path.isEmpty()) return;
            const auto image = widget->grab();
            QFile output(path);
            if (image.isNull() || !output.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
                !output.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) ||
                !image.save(&output, "PNG") || !output.flush())
                throw std::runtime_error("Cannot save requested live launcher PNG");
        };
        auto* settingsDialog = control<QDialog>(launcher, "settingsDialog");
        const auto captureSettings = [&](const QString& path) {
            if (path.isEmpty()) return;
            reveal(launcher, control<QComboBox>(launcher, "renderer"));
            QCoreApplication::processEvents();
            capture(settingsDialog, path);
            settingsDialog->hide();
        };

        enum class Step {
#if !IKV_HAS_VULKAN
            Capability, CapabilitySave, CapabilityReject, CapabilitySelect, CapabilityOpenGL,
#endif
            Start, Import, Verify, Settings, Profile, Register, Login, Play, Paint, Stable
        };
#if !IKV_HAS_VULKAN
        Step step = Step::Capability;
        NetworkActivity networkActivity;
        const auto savedValues = [] {
            QSettings saved;
            saved.sync();
            QMap<QString, QVariant> values;
            for (const auto& key : saved.allKeys()) values.insert(key, saved.value(key));
            return values;
        };
        const auto nativeGraphics = [&] {
            QFile file(installRoot + "/settings/graphics.json");
            const bool exists = file.exists();
            QByteArray contents;
            if (exists) {
                if (!file.open(QIODevice::ReadOnly) || file.size() > 65536)
                    throw std::runtime_error("Cannot snapshot native graphics settings before rejected Play");
                contents = file.readAll();
            }
            return std::pair{exists, contents};
        };
        QMap<QString, QVariant> beforeReject;
        std::pair<bool, QByteArray> graphicsBeforeReject;
        const QString unavailable = QString::fromUtf8("Stored renderer is unavailable in this OpenGL-only build; explicitly select opengl in Ayarlar → Grafik");
#else
        Step step = Step::Start;
#endif
        bool finished = false, importDialogSeen = false, importAccepted = false, importSelectionMatched = false;
        bool sourceFieldChecked = false, serverDialogFilled = false;
        QString lastReportedStage;
        QJsonObject sourceSelection{{"expected_canonical", canonicalSource},
                                    {"expected_directory_canonical", sourceParent}};
        QElapsedTimer deadline, readyTime;
        deadline.start();
        const auto trace = [&](const QString& event) {
            const QJsonObject record{{"event", event}, {"stage", stage}, {"elapsed_ms", deadline.elapsed()},
                                     {"source_selection", sourceSelection}};
            const auto output = ("TRACE launcher-gui " +
                redact(QString::fromUtf8(QJsonDocument(record).toJson(QJsonDocument::Compact))) + '\n').toUtf8();
            std::fwrite(output.constData(), 1, static_cast<std::size_t>(output.size()), stderr);
            std::fflush(stderr);
        };
        QTimer poll;
        poll.setInterval(25);
        const auto nativeChild = [&]() -> QProcess* {
            const QString expected = QFileInfo(bundledClient).canonicalFilePath();
            for (auto* process : launcher.findChildren<QProcess*>())
                if (!expected.isEmpty() && QFileInfo(process->program()).canonicalFilePath() == expected)
                    return process;
            return nullptr;
        };
        const auto finish = [&](int code, const QString& reason) {
            if (finished) return;
            finished = true;
            poll.stop();
            QString detail = reason;
            if (auto* child = nativeChild(); child && child->state() != QProcess::NotRunning) {
                child->terminate();
                if (!child->waitForFinished(5000)) {
                    child->kill();
                    child->waitForFinished(5000);
                    code = 1;
                    detail += "; native child required forced termination";
                } else if (child->exitStatus() != QProcess::NormalExit || child->exitCode() != 0) {
                    code = 1;
                    detail += "; native child did not shut down cleanly";
                }
            }
            if (!code) {
                QFile graphics(installRoot + "/settings/graphics.json");
                if (!graphics.open(QIODevice::ReadOnly)) {
                    code = 1; detail += "; native shutdown settings missing";
                } else {
                    const auto saved = QJsonDocument::fromJson(graphics.readAll()).object();
                    for (const auto* key : {"scene", "scene_member", "zone", "character_model", "model_mapping"})
                        if (saved.contains(QString::fromLatin1(key))) {
                            code = 1; detail += "; native shutdown retained an obsolete source override";
                        }
                    if (saved.value("renderer").toString() != requestedRenderer ||
                        static_cast<float>(saved.value("render_scale").toDouble()) != static_cast<float>(requestedScale)) {
                        code = 1; detail += "; real native shutdown lost selected provider/render scale";
                    }
                }
            }
            trace(code ? "failed" : "passed");
            if (code) detail += "; status=" + statusLabel->text() + "; diagnostics=" + redact(logs->toPlainText()).right(1200);
            const auto output = redact(QString("%1 launcher-gui stage=%2: %3\n")
                .arg(code ? "FAIL" : "PASS", stage, detail)).toUtf8();
            std::fwrite(output.constData(), 1, static_cast<std::size_t>(output.size()), stderr);
            // Unwind any nested QFileDialog/QMessageBox event loop before destruction.
            for (auto* widget : QApplication::topLevelWidgets())
                if (auto* dialog = qobject_cast<QDialog*>(widget); dialog && dialog->isVisible()) dialog->reject();
            QCoreApplication::exit(code);
        };
        bool clickPending = false;
        const auto queueClick = [&](const char* name) {
            clickPending = true;
            // A separate event keeps the poll timer live inside modal dialog exec().
            QTimer::singleShot(0, &launcher, [&, name] {
                clickPending = false;
                if (finished) return;
                try { click(launcher, name); }
                catch (const std::exception& error) { finish(1, QString::fromUtf8(error.what())); }
            });
        };
        QObject::connect(&poll, &QTimer::timeout, &launcher, [&] {
            if (finished) return;
            try {
                if (lastReportedStage != stage) {
                    lastReportedStage = stage;
                    trace("stage");
                }
                if (step == Step::Import && importAccepted && !sourceFieldChecked) {
                    const auto picked = control<QLineEdit>(launcher, "assetSource")->text();
                    const auto canonicalPicked = QFileInfo(picked).canonicalFilePath();
                    sourceSelection.insert("launcher_source", picked);
                    sourceSelection.insert("launcher_source_canonical", canonicalPicked);
                    trace("launcher-source");
                    if (canonicalPicked != canonicalSource)
                        throw std::runtime_error(("Launcher accepted source canonical=" + canonicalPicked +
                            "; expected=" + canonicalSource).toStdString());
                    sourceFieldChecked = true;
                }
                for (auto* widget : QApplication::topLevelWidgets()) {
                    if (auto* message = qobject_cast<QMessageBox*>(widget); message && message->isVisible()) {
#if !IKV_HAS_VULKAN
                        if (step == Step::CapabilityReject) {
                            if (message->text() != unavailable || statusLabel->text() != unavailable ||
                                savedValues() != beforeReject || nativeGraphics() != graphicsBeforeReject ||
                                !launcher.findChildren<QProcess*>().isEmpty() || networkActivity.requested)
                                throw std::runtime_error("Unavailable stored Vulkan Play lost precedence or caused settings/process/network side effects");
                            app.removeEventFilter(&networkActivity);
                            message->done(QMessageBox::Ok);
                            step = Step::CapabilitySelect;
                            return;
                        }
#endif
                        const QString reason = "Launcher modal error: " + message->text() + " " + message->informativeText();
                        message->done(QMessageBox::Ok);
                        finish(1, reason);
                        return;
                    }
                }
                if (deadline.elapsed() >= static_cast<qint64>(seconds) * 1000) {
                    finish(1, "Timed out waiting for actual GUI/native child"); return;
                }
                if (clickPending) return;
                switch (step) {
#if !IKV_HAS_VULKAN
                case Step::Capability: {
                    stage = "saved-vulkan-unavailable";
                    auto* renderer = control<QComboBox>(launcher, "renderer");
                    if (renderer->findText("vulkan") >= 0 || renderer->currentIndex() != -1 ||
                        !renderer->currentText().isEmpty() || renderer->placeholderText().isEmpty() ||
                        !renderer->isEnabled() || QSettings{}.value("graphics/renderer").toString() != "vulkan" ||
                        control<QDoubleSpinBox>(launcher, "renderScale")->isEnabled() ||
                        control<QDoubleSpinBox>(launcher, "renderScale")->value() != .5)
                        throw std::runtime_error("Actual OpenGL-only UI silently normalized or offered the unavailable stored renderer");
                    step = Step::CapabilitySave; queueClick("saveGraphics");
                    break;
                }
                case Step::CapabilitySave:
                    if (statusLabel->text() == QString::fromUtf8("Ayarlar kaydedildi")) {
                        beforeReject = savedValues();
                        if (beforeReject.value("graphics/renderer").toString() != "vulkan" ||
                            beforeReject.value("installation/root").toString() != installRoot ||
                            control<QComboBox>(launcher, "renderer")->currentIndex() != -1 ||
                            beforeReject.value("graphics/render_scale").toDouble() != .5)
                            throw std::runtime_error("Unrelated real settings Save normalized the unavailable stored Vulkan choice");
                        captureSettings(graphicsScreenshotPath);
                        graphicsScreenshotPath.clear();
                        graphicsBeforeReject = nativeGraphics();
                        // Competing invalid input and unsaved edits must not outrank capability rejection.
                        set("installRoot", profile.path() + "/unpersisted-no-install");
                        set("updateTrust", profile.path() + "/unpersisted-no-key");
                        app.installEventFilter(&networkActivity);
                        stage = "nonmutating-unavailable-play";
                        // Play is only reachable after login; expose its page for this precedence check.
                        control<QStackedWidget>(launcher, "pages")->setCurrentWidget(control<QWidget>(launcher, "playPage"));
                        step = Step::CapabilityReject; queueClick("playButton");
                    }
                    break;
                case Step::CapabilityReject:
                    break; // The real modal error above is the acceptance boundary.
                case Step::CapabilitySelect: {
                    auto* renderer = control<QComboBox>(launcher, "renderer");
                    reveal(launcher, renderer);
                    if (!renderer->isVisible() || !renderer->isEnabled())
                        throw std::runtime_error("OpenGL selection control is unavailable");
                    renderer->setFocus();
                    bool activated = false;
                    const auto selection = QObject::connect(renderer, qOverload<int>(&QComboBox::activated),
                        &launcher, [&](int) { activated = true; });
                    QKeyEvent down(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
                    QKeyEvent release(QEvent::KeyRelease, Qt::Key_Down, Qt::NoModifier);
                    QApplication::sendEvent(renderer, &down);
                    QApplication::sendEvent(renderer, &release);
                    QObject::disconnect(selection);
                    if (!activated || renderer->currentText() != "opengl" ||
                        QSettings{}.value("graphics/renderer").toString() != "vulkan")
                        throw std::runtime_error("Real Qt keyboard selection did not explicitly choose OpenGL without premature persistence");
                    set("installRoot", installRoot); set("updateTrust", trustKey);
                    stage = "explicit-opengl-save";
                    step = Step::CapabilityOpenGL; queueClick("saveGraphics");
                    break;
                }
                case Step::CapabilityOpenGL:
                    if (statusLabel->text() == QString::fromUtf8("Ayarlar kaydedildi")) {
                        if (QSettings{}.value("graphics/renderer").toString() != "opengl" ||
                            QSettings{}.value("graphics/render_scale").toDouble() != 1 ||
                            control<QDoubleSpinBox>(launcher, "renderScale")->value() != 1 ||
                            control<QDoubleSpinBox>(launcher, "renderScale")->isEnabled())
                            throw std::runtime_error("Explicit OpenGL selection was not persisted by real Save");
                        step = Step::Start;
                    }
                    break;
#endif
                case Step::Start:
                    if (currentPage(launcher) != "installPage")
                        throw std::runtime_error("First run without imported assets did not show the install page");
                    capture(&launcher, installScreenshotPath);
                    stage = "import"; step = Step::Import; queueClick("importButton"); break;
                case Step::Import:
                    if (!importAccepted) {
                        for (auto* widget : QApplication::topLevelWidgets()) {
                            auto* dialog = qobject_cast<QFileDialog*>(widget);
                            if (!dialog || !dialog->isVisible()) continue;
                            if (dialog->fileMode() != QFileDialog::Directory)
                                throw std::runtime_error("Import did not open a real directory picker");
                            if (!importDialogSeen) {
                                // Absolute selectFile resets Qt's displayed directory to the path's parent.
                                // Select a basename in that explicit parent; wait for actual model state.
                                dialog->setDirectory(sourceParent);
                                importDialogSeen = true;
                                QObject::connect(dialog, &QFileDialog::filesSelected, &launcher,
                                    [&](const QStringList& files) {
                                        QJsonArray canonicalFiles;
                                        for (const auto& path : files)
                                            canonicalFiles.append(QFileInfo(path).canonicalFilePath());
                                        sourceSelection.insert("emitted_canonical", canonicalFiles);
                                        importSelectionMatched = files.size() == 1 &&
                                            canonicalFiles.first().toString() == canonicalSource;
                                        trace("picker-files-selected");
                                    });
                                QObject::connect(dialog, &QDialog::accepted, &launcher, [&, dialog] {
                                    const auto selected = dialog->selectedFiles();
                                    const QString picked = selected.size() == 1 ? selected.first() : QString{};
                                    const auto canonicalPicked = QFileInfo(picked).canonicalFilePath();
                                    const auto directory = dialog->directory().canonicalPath();
                                    sourceSelection.insert("accepted_path", picked);
                                    sourceSelection.insert("accepted_canonical", canonicalPicked);
                                    sourceSelection.insert("accepted_directory_canonical", directory);
                                    trace("picker-accepted");
                                    if (!importSelectionMatched || selected.size() != 1 ||
                                        canonicalPicked != canonicalSource || directory != sourceParent) {
                                        // exec() has not returned: veto its result before Launcher starts scanning.
                                        dialog->setResult(QDialog::Rejected);
                                        finish(1, "Picker accepted source canonical=" + canonicalPicked +
                                            "; directory=" + directory + "; expected=" + canonicalSource +
                                            "; expected directory=" + sourceParent);
                                        return;
                                    }
                                    importAccepted = true;
                                });
                                break; // Let the existing event loop settle the directory model.
                            }
                            if (dialog->directory().canonicalPath() == sourceParent) {
                                // Qt does not update its filename field while that field has focus.
                                dialog->setFocus(Qt::OtherFocusReason);
                                dialog->selectFile(sourceName);
                            }
                            const auto selected = dialog->selectedFiles();
                            QJsonArray canonicalSelected;
                            for (const auto& path : selected)
                                canonicalSelected.append(QFileInfo(path).canonicalFilePath());
                            const auto directory = dialog->directory().canonicalPath();
                            const QJsonObject observed{{"selected_paths", QJsonArray::fromStringList(selected)},
                                                       {"selected_canonical", canonicalSelected},
                                                       {"directory_canonical", directory}};
                            if (sourceSelection.value("observed").toObject() != observed) {
                                sourceSelection.insert("observed", observed);
                                trace("picker-state");
                            }
                            if (selected.size() != 1 || canonicalSelected.first().toString() != canonicalSource ||
                                directory != sourceParent) break;
                            // Direct invocation leaves no queued event that can change the checked selection.
                            if (!QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection))
                                throw std::runtime_error("Cannot accept real import directory picker");
                            break;
                        }
                    }
                    if (sourceFieldChecked && statusLabel->text().contains("Assets verified")) {
                        if (currentPage(launcher) != "loginPage")
                            throw std::runtime_error("Successful import did not switch to the login page");
                        stage = "verify"; step = Step::Verify; queueClick("verifyButton");
                    }
                    break;
                case Step::Verify:
                    if (statusLabel->text().contains("Assets verified")) {
                        stage = "settings"; step = Step::Settings; queueClick("saveGraphics");
                    }
                    break;
                case Step::Settings:
                    if (statusLabel->text() == QString::fromUtf8("Ayarlar kaydedildi")) {
                        QSettings saved;
                        for (const auto* key : {"graphics/sceneMember", "graphics/zone", "graphics/characterModel"})
                            if (saved.contains(QString::fromLatin1(key)))
                                throw std::runtime_error("Real launcher retained obsolete graphical settings");
                        if (saved.value("installation/root").toString() != installRoot ||
                            saved.value("graphics/renderer").toString() != requestedRenderer ||
                            static_cast<float>(saved.value("graphics/render_scale").toDouble()) != static_cast<float>(requestedScale))
                            throw std::runtime_error("Real launcher settings were not persisted");
                        Launcher reloaded;
                        if (control<QComboBox>(reloaded, "renderer")->currentText() != requestedRenderer ||
                            static_cast<float>(control<QDoubleSpinBox>(reloaded, "renderScale")->value()) != static_cast<float>(requestedScale) ||
                            control<QDoubleSpinBox>(reloaded, "renderScale")->isEnabled() != (requestedRenderer == "vulkan"))
                            throw std::runtime_error("Real reloaded launcher lost the selected render scale/provider");
                        captureSettings(graphicsScreenshotPath);
                        stage = "profile"; step = Step::Profile; queueClick("addServer");
                    }
                    break;
                case Step::Profile:
                    if (!serverDialogFilled) {
                        auto* dialog = control<QDialog>(launcher, "serverDialog");
                        if (!dialog->isVisible()) break;
                        set("serverName", "Owned smoke");
                        set("serverAddress", QUrl(auth).authority());
                        set("serverCa", certificate);
                        serverDialogFilled = true;
                        queueClick("saveServer");
                        break;
                    }
                    if (control<QComboBox>(launcher, "profiles")->currentText() == "Owned smoke") {
                        QSettings saved;
                        const auto stored = QJsonDocument::fromJson(saved.value("profiles/Owned smoke").toByteArray()).object();
                        if (stored != QJsonObject{{"auth", auth}, {"ca", certificate}} ||
                            control<QDialog>(launcher, "serverDialog")->isVisible() || currentPage(launcher) != "loginPage")
                            throw std::runtime_error("Real server profile was not persisted");
                        capture(&launcher, loginScreenshotPath);
                        set("username", ownedUsername); set("password", secretPassword);
                        stage = "register"; step = Step::Register; queueClick("registerButton");
                    }
                    break;
                case Step::Register:
                    if (statusLabel->text() == QString::fromUtf8("Hesap oluşturuldu; giriş yapın")) {
                        set("password", secretPassword);
                        stage = "login"; step = Step::Login; queueClick("loginButton");
                    }
                    break;
                case Step::Login:
                    if (statusLabel->text() == QString::fromUtf8("Giriş yapıldı")) {
                        if (currentPage(launcher) != "playPage")
                            throw std::runtime_error("Login did not switch to the play page");
                        if (control<QLabel>(launcher, "account")->text().indexOf(ownedUsername.toHtmlEscaped()) < 0)
                            throw std::runtime_error("Play page does not name the authenticated account");
                        stage = "play"; step = Step::Play; queueClick("playButton");
                    }
                    break;
                case Step::Play:
                case Step::Paint:
                case Step::Stable: {
                    auto* child = nativeChild();
                    if (child && child->state() == QProcess::NotRunning)
                        throw std::runtime_error("Actual native renderer exited before acceptance");
                    if (step == Step::Play && QRegularExpression("CHARACTER_SCREEN characters=0(?:\\s|$)").match(logs->toPlainText()).hasMatch()) {
                        if (!child || child->state() != QProcess::Running || !child->arguments().contains("--session-stdin"))
                            throw std::runtime_error("In-client character screen has no real running native child");
                        const auto arguments = child->arguments();
                        const auto argumentText = arguments.join('\n');
                        if (arguments.contains("--session") || argumentText.contains(secretPassword) ||
                            argumentText.contains(ownedUsername))
                            throw std::runtime_error("Native child credentials must stay on private stdin");
                        for (const auto& field : {
                                 std::pair{QString("--host"), host}, std::pair{QString("--port"), QString::number(port)},
                                 std::pair{QString("--ca"), certificate}, std::pair{QString("--asset-root"), installRoot + "/assets"},
                                 std::pair{QString("--renderer"), requestedRenderer}, std::pair{QString("--appearance"), requestedAppearance},
                                 std::pair{QString("--rt"), requestedRt}, std::pair{QString("--rt-style"), requestedRtStyle},
                                 std::pair{QString("--auth"), QUrl(auth).toString(QUrl::StripTrailingSlash)}}) {
                            const auto position = arguments.indexOf(field.first);
                            if (position < 0 || position + 1 >= arguments.size() || arguments[position + 1] != field.second)
                                throw std::runtime_error("Native child lost pinned service, imported source or requested graphics arguments");
                        }
                        for (const auto* obsolete : {"--scene", "--zone", "--character-model", "--character"})
                            if (arguments.contains(QString::fromLatin1(obsolete)))
                                throw std::runtime_error("Real native child received an obsolete source override");
                        const auto requestedMap = fixture.value("model_map").toString();
                        const auto mapArgument = arguments.indexOf("--model-map");
                        if ((requestedMap.isEmpty() && mapArgument >= 0) ||
                            (!requestedMap.isEmpty() && (mapArgument < 0 || mapArgument + 1 >= arguments.size() ||
                                                        arguments[mapArgument + 1] != requestedMap)))
                            throw std::runtime_error("Real native child lost explicit operator model mapping or fabricated a fallback");
                        const auto scaleArgument = arguments.indexOf("--render-scale");
                        if (scaleArgument < 0 || scaleArgument + 1 >= arguments.size() ||
                            static_cast<float>(arguments[scaleArgument + 1].toDouble()) != static_cast<float>(requestedScale))
                            throw std::runtime_error("Real native child did not receive selected render scale");
                        stage = "awaiting-character-screen-frame"; step = Step::Paint;
                    }
                    if (step == Step::Paint) {
                        const auto text = logs->toPlainText();
                        if (requestedRenderer == "vulkan") {
                            const auto provider = QRegularExpression("Vulkan submitted frame=[1-9][0-9]* provider=vulkan[^\\n]* renderScale=([^\\s]+)").match(text);
                            if (!provider.hasMatch()) break;
                            if (static_cast<float>(provider.captured(1).toDouble()) != static_cast<float>(requestedScale) ||
                                !provider.captured(0).contains("appearance=" + requestedAppearance + " "))
                                throw std::runtime_error("Actual Vulkan provider did not render with selected appearance/scale");
                            if (requestedRt == "required" &&
                                (requestedAppearance == "enhanced" || requestedRtStyle == "faithful")) {
                                if (!QRegularExpression("Vulkan device=[^\\n]* hardwareRT=1(?:\\s|$)").match(text).hasMatch())
                                    throw std::runtime_error("Required RT frame has no actual hardware RT device");
                                // Enhanced faithful traces Sun shadows and diffuse GI; original faithful traces actor shadows only.
                                const QString reflections = requestedRtStyle == "physical" ? "1" : "0";
                                const QString diffuseGI = requestedAppearance == "enhanced" ? "1" : "0";
                                for (const QString& flag : {QString("rtStyle=") + requestedRtStyle, QString("hardwareRayQueries=1"),
                                                            QString("accelerationStructure=1"), QString("shadows=1"),
                                                            "reflections=" + reflections, "diffuseGI=" + diffuseGI})
                                    if (!provider.captured(0).contains(flag))
                                        throw std::runtime_error("Required RT frame lacks actual acceleration/effect evidence");
                            }
                        }
                        stage = "authenticated-native-character-screen"; step = Step::Stable; readyTime.start();
                    }
                    if (step == Step::Stable && readyTime.elapsed() >= 2000) {
                        if (!child || child->state() != QProcess::Running)
                            throw std::runtime_error("Native renderer did not survive the in-client character screen");
                        QFile graphics(installRoot + "/settings/graphics.json");
                        if (!graphics.open(QIODevice::ReadOnly))
                            throw std::runtime_error("Cannot read real native graphics settings");
                        const auto chosen = QJsonDocument::fromJson(graphics.readAll()).object();
                        if (chosen.value("renderer").toString() != requestedRenderer ||
                            static_cast<float>(chosen.value("render_scale").toDouble()) != static_cast<float>(requestedScale))
                            throw std::runtime_error("Play did not persist real native graphics scale/provider");
                        if (!screenshotPath.isEmpty()) {
                            stage = "live-launcher-screenshot";
                            capture(&launcher, screenshotPath);
                        }
                        finish(0, "Import, verify, settings, profile, register, login, Play and the client's HTTPS in-client character screen completed");
                    }
                    break;
                }
                }
            } catch (const std::exception& error) { finish(1, QString::fromUtf8(error.what())); }
        });
        poll.start();
        return app.exec();
#endif
    } catch (const std::exception& error) {
        const auto output = redact("FAIL launcher-gui stage=" + stage + ": " + QString::fromUtf8(error.what()) + '\n').toUtf8();
        std::fwrite(output.constData(), 1, static_cast<std::size_t>(output.size()), stderr);
        return 1;
    }
}
