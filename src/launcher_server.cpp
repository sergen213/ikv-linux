#include "ikv/launcher_server.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDeadlineTimer>
#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMap>
#include <QMetaObject>
#include <QNetworkAccessManager>
#include <QNetworkInterface>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSpinBox>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStyle>
#include <QTabWidget>
#include <QTableWidget>
#include <QTcpServer>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <openssl/rand.h>
#include <fcntl.h>
#include <pwd.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <tuple>
#include <cstring>
#include <memory>
#include <stdexcept>

namespace ikv::launcher_server {
namespace {
// ---- Blocking helpers: run on worker threads in the launcher and inline in the supervisor ----
std::runtime_error hostIssue(const QString& message) { return std::runtime_error(message.toStdString()); }
QString failure(const std::exception& e) { return QString::fromUtf8(e.what()); }
int runCode(const QString& program, const QStringList& args, QByteArray* output = nullptr, const QByteArray& input = {},
            const QProcessEnvironment& environment = QProcessEnvironment::systemEnvironment()) {
    QProcess process; process.setProgram(program); process.setArguments(args); process.setProcessEnvironment(environment);
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.setChildProcessModifier([] { ::umask(077); });
    process.start();
    if (!process.waitForStarted(10000)) throw hostIssue(QFileInfo(program).fileName() + " başlatılamadı");
    if (!input.isEmpty()) process.write(input);
    process.closeWriteChannel();
    const bool done = process.waitForFinished(120000);
    if (!done) { process.kill(); process.waitForFinished(5000); }
    if (output) *output = process.readAll();
    if (!done) throw hostIssue(QFileInfo(program).fileName() + " zaman aşımına uğradı");
    return process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1;
}
void run(const QString& program, const QStringList& args, const QByteArray& input = {}, const QByteArray& secret = {},
         const QProcessEnvironment& environment = QProcessEnvironment::systemEnvironment()) {
    QByteArray output;
    if (runCode(program, args, &output, input, environment) == 0) return;
    if (!secret.isEmpty()) output.replace(secret, "[REDACTED]");
    throw hostIssue(QFileInfo(program).fileName() + " başarısız: " + QString::fromUtf8(output.right(1500)).trimmed());
}
QByteArray randomHex(int bytes) {
    QByteArray raw(bytes, '\0');
    if (RAND_bytes(reinterpret_cast<unsigned char*>(raw.data()), bytes) != 1) throw hostIssue("Rastgele sayı üretilemedi");
    const auto hex = raw.toHex(); raw.fill('\0'); return hex;
}
bool privateDirectoryOk(const QString& path) {
    struct stat info {};
    return ::lstat(QFile::encodeName(path).constData(), &info) == 0 && S_ISDIR(info.st_mode) && info.st_uid == ::geteuid() && (info.st_mode & 07777) == 0700;
}
void privateDirectory(const QString& path) {
    if (::mkdir(QFile::encodeName(path).constData(), 0700) != 0 && errno != EEXIST) throw hostIssue("Özel klasör oluşturulamadı: " + path);
    if (!privateDirectoryOk(path))
        throw hostIssue("Sunucu klasörü bu kullanıcıya ait, mod 0700, sembolik bağ olmayan bir klasör olmalı: " + path);
}
void writePrivate(const QString& path, const QByteArray& bytes) {
    const int fd = ::open(QFile::encodeName(path).constData(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) throw hostIssue("Özel dosya oluşturulamadı: " + path);
    for (qsizetype done = 0; done < bytes.size();) {
        const auto written = ::write(fd, bytes.constData() + done, static_cast<std::size_t>(bytes.size() - done));
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) { ::close(fd); throw hostIssue("Özel dosya yazılamadı: " + path); }
        done += written;
    }
    if (::fsync(fd) != 0) { ::close(fd); throw hostIssue("Özel dosya yazılamadı: " + path); }
    if (::close(fd) != 0) throw hostIssue("Özel dosya yazılamadı: " + path);
}
QByteArray quoted(const QString& value) { // libpq conninfo / postgresql.conf single-quoted value
    auto bytes = value.toUtf8(); bytes.replace("\\", "\\\\").replace("'", "\\'"); return "'" + bytes + "'";
}
QString openssl() {
    const auto found = QStandardPaths::findExecutable("openssl");
    if (found.isEmpty()) throw hostIssue("openssl kurulu değil. Kurun: sudo pacman -S openssl · sudo apt install openssl");
    return found;
}
// First directory holding initdb, pg_ctl and psql: PATH, then Debian's /usr/lib/postgresql/<newest>/bin.
QString pgBinDir() {
    QStringList candidates;
    if (const auto found = QStandardPaths::findExecutable("initdb"); !found.isEmpty()) candidates << QFileInfo(found).absolutePath();
    auto versions = QDir("/usr/lib/postgresql").entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    std::sort(versions.begin(), versions.end(), [](const QString& a, const QString& b) { return a.toInt() > b.toInt(); });
    for (const auto& version : versions) candidates << "/usr/lib/postgresql/" + version + "/bin";
    for (const auto& dir : candidates)
        if (QFileInfo(dir + "/initdb").isExecutable() && QFileInfo(dir + "/pg_ctl").isExecutable() && QFileInfo(dir + "/psql").isExecutable())
            return dir;
    throw hostIssue("PostgreSQL sunucu araçları (initdb, pg_ctl, psql) kurulu değil. Kurun: sudo pacman -S postgresql · "
                    "sudo apt install postgresql · sudo dnf install postgresql-server");
}
// Mirrors deploy/prepare-secrets.py: SAN service certificate from the owned development CA (never production trust).
void issueServiceCertificate(const QString& secrets, const QStringList& sans, bool newKey) {
    const QString csr = secrets + "/request.csr", extensions = secrets + "/certificate.cnf", next = secrets + "/tls-cert.new";
    for (const auto& stale : {csr, extensions, next}) QFile::remove(stale);
    writePrivate(extensions, ("basicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature,keyEncipherment\n"
                              "extendedKeyUsage=serverAuth\nsubjectAltName=" + sans.join(',') + "\n").toUtf8());
    QStringList request{"req", "-new", "-sha256", "-subj", "/CN=IKV LAUNCHER LOCAL SERVICE", "-out", csr};
    if (newKey) request << "-newkey" << "rsa:3072" << "-noenc" << "-keyout" << secrets + "/tls-key";
    else request << "-key" << secrets + "/tls-key";
    run(openssl(), request);
    run(openssl(), {"x509", "-req", "-in", csr, "-CA", secrets + "/tls-ca", "-CAkey", secrets + "/development-ca-key",
                    "-set_serial", "0x" + QString::fromLatin1(randomHex(16)), "-days", "30", "-sha256", "-extfile", extensions, "-out", next});
    QFile::remove(csr); QFile::remove(extensions);
    if (!QFile::setPermissions(next, QFileDevice::ReadOwner | QFileDevice::WriteOwner) ||
        ::rename(QFile::encodeName(next).constData(), QFile::encodeName(secrets + "/tls-cert").constData()) != 0)
        throw hostIssue("Hizmet sertifikası yerleştirilemedi");
}
void startPostgres(const QString& dir, const QString& bin) {
    const QString pg = dir + "/pg";
    if (runCode(bin + "/pg_ctl", {"status", "-D", pg}) != 0) run(bin + "/pg_ctl", {"start", "-D", pg, "-l", pg + "/postgres.log", "-w", "-t", "60", "-s"});
}
void stopPostgres(const QString& dir, const QString& bin) {
    if (QFileInfo::exists(dir + "/pg/postmaster.pid")) runCode(bin + "/pg_ctl", {"stop", "-D", dir + "/pg", "-m", "fast", "-w", "-t", "30", "-s"});
}
// New private cluster: no TCP listener, 0700 Unix socket, the OS user is superuser by peer only,
// and the services use a least-privilege SCRAM role owning just database ikv (as deploy/init-database.sh).
void provisionHost(const QString& dir, const QStringList& sans, const QString& bin, const std::function<void(int)>& step) {
    const QString secrets = dir + "/secrets", pg = dir + "/pg", socket = dir + "/sock";
    if (QFileInfo::exists(secrets) || QFileInfo::exists(pg))
        throw hostIssue("Yarım kalmış sunucu klasörü var; önce \"Sunucuyu sil\": " + dir);
    for (const auto& path : {secrets, socket, dir + "/backups"}) privateDirectory(path);
    if (QFile::encodeName(socket + "/.s.PGSQL.5432").size() > 100) throw hostIssue("PostgreSQL soket yolu çok uzun: " + socket);
    step(2);
    run(openssl(), {"req", "-x509", "-newkey", "rsa:3072", "-noenc", "-sha256", "-days", "3650",
                    "-subj", "/CN=IKV LAUNCHER LOCAL CA - NOT PRODUCTION", "-addext", "basicConstraints=critical,CA:TRUE",
                    "-addext", "keyUsage=critical,keyCertSign,cRLSign", "-keyout", secrets + "/development-ca-key", "-out", secrets + "/tls-ca"});
    issueServiceCertificate(secrets, sans, true);
    QFile ca(secrets + "/tls-ca");
    if (!ca.open(QIODevice::ReadOnly)) throw hostIssue("CA okunamadı");
    writePrivate(secrets + "/client-ca.pem", ca.readAll());
    for (const auto& name : QDir(secrets).entryList(QDir::Files))
        QFile::setPermissions(secrets + "/" + name, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    step(3);
    const auto* account = ::getpwuid(::geteuid());
    if (!account) throw hostIssue("İşletim sistemi kullanıcısı bulunamadı");
    const QString user = QString::fromLocal8Bit(account->pw_name);
    run(bin + "/initdb", {"-D", pg, "--no-locale", "--encoding=UTF8", "--auth-local=peer", "--auth-host=reject", "--username=" + user});
    QSaveFile hba(pg + "/pg_hba.conf");
    QFile conf(pg + "/postgresql.conf");
    if (!hba.open(QIODevice::WriteOnly) || hba.write("local all \"" + user.toUtf8() + "\" peer\nlocal ikv ikv scram-sha-256\n") < 0 || !hba.commit() ||
        !conf.open(QIODevice::Append) || conf.write("\nlisten_addresses = ''\nunix_socket_directories = " + quoted(socket) +
            "\nunix_socket_permissions = 0700\nport = 5432\npassword_encryption = scram-sha-256\n") < 0 || !conf.flush())
        throw hostIssue("PostgreSQL yapılandırması yazılamadı");
    conf.close();
    startPostgres(dir, bin);
    auto password = randomHex(32);
    run(bin + "/psql", {"--no-psqlrc", "--quiet", "--set", "ON_ERROR_STOP=1", "--file", "-", "--dbname",
                        QString::fromUtf8("host=" + quoted(socket) + " port=5432 dbname=postgres user=" + quoted(user))},
        "CREATE ROLE ikv LOGIN NOSUPERUSER NOCREATEDB NOCREATEROLE NOREPLICATION NOBYPASSRLS PASSWORD '" + password + "';\n"
        "CREATE DATABASE ikv OWNER ikv;\n", password);
    writePrivate(secrets + "/dsn", "host=" + quoted(socket) + " port=5432 dbname=ikv user=ikv password=" + password + " connect_timeout=5\n");
    password.fill('\0');
}
QJsonObject readJson(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.read(65536)).object() : QJsonObject{};
}
void saveJson(const QString& path, const QJsonObject& value) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(value).toJson()) < 0 || !file.commit())
        throw hostIssue("Yazılamadı: " + path);
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
}
// Installed prefix (bin/../share/ikv) or a build directory inside the source tree (build/../).
QString hostResource(const QString& relative) {
    const QString dir = QCoreApplication::applicationDirPath();
    for (const QString& candidate : {dir + "/../share/ikv/" + relative, dir + "/../" + relative})
        if (QFileInfo::exists(candidate)) return QFileInfo(candidate).canonicalFilePath();
    throw hostIssue("Sunucu kaynağı bulunamadı: " + relative);
}
QString binary(const char* name) { return QCoreApplication::applicationDirPath() + "/" + QString::fromLatin1(name); }
QSslConfiguration pinnedTls(const QString& caPath) {
    QSslConfiguration tls = QSslConfiguration::defaultConfiguration();
    tls.setProtocol(QSsl::TlsV1_2OrLater); tls.setPeerVerifyMode(QSslSocket::VerifyPeer);
    QFile file(caPath);
    const auto certificates = file.open(QIODevice::ReadOnly) ? QSslCertificate::fromData(file.read(1 << 20)) : QList<QSslCertificate>{};
    if (certificates.isEmpty()) throw hostIssue("Yerel sunucu CA'sı okunamadı: " + caPath);
    tls.setCaCertificates(certificates); // only the host's own development CA, never the system store
    return tls;
}
// supervisor.lock: held LOCK_EX by the running supervisor (content: its pid).
// launcher.lock: held LOCK_SH by every open launcher; without one a non-keep-alive supervisor stops.
bool lockHeld(const QString& path) {
    const int fd = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;
    const bool held = ::flock(fd, LOCK_SH | LOCK_NB) != 0 && errno == EWOULDBLOCK;
    ::close(fd);
    return held;
}
bool anyHolder(const QString& path) {
    const int fd = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;
    const bool held = ::flock(fd, LOCK_EX | LOCK_NB) != 0 && errno == EWOULDBLOCK;
    ::close(fd);
    return held;
}
pid_t lockPid(const QString& path) {
    QFile file(path);
    const pid_t pid = file.open(QIODevice::ReadOnly) ? static_cast<pid_t>(file.read(32).trimmed().toInt()) : 0;
    return pid > 1 ? pid : 0;
}
QString since(qint64 seconds) {
    if (seconds < 0) return "—";
    return QString("%1 sa %2 dk %3 sn").arg(seconds / 3600).arg(seconds / 60 % 60).arg(seconds % 60);
}
QString countText(const QJsonValue& value) { return value.isDouble() ? QString::number(value.toInteger()) : QString("—"); }

// ---- Supervisor: one detached process owns PostgreSQL, ikv-auth and ikv-server ----
volatile std::sig_atomic_t stopRequested = 0;
void requestStop(int) { stopRequested = 1; }

class Supervisor final : public QObject {
public:
    Supervisor(QString dir, QString assets, QString admin, QByteArray password)
        : dir_(std::move(dir)), assets_(std::move(assets)), admin_(std::move(admin)), password_(std::move(password)) {
        watch_.setInterval(250);
        connect(&watch_, &QTimer::timeout, this, [this] {
            if (stopRequested) { say("durdurma isteği alındı"); shutdown({}); return; }
            if (++ticks_ % 4) return;
            if (!readJson(dir_ + "/host.json").value("keep_alive").toBool() && !anyHolder(dir_ + "/launcher.lock")) {
                say("başlatıcı kapandı ve 'Başlatıcı kapanınca da çalışsın' kapalı");
                shutdown({});
            }
        });
    }
    ~Supervisor() override { password_.fill('\0'); }
    void start() {
        watch_.start();
        try {
            config_ = readJson(dir_ + "/host.json");
            if (config_.isEmpty()) throw hostIssue("host.json okunamadı; sunucu kurulmamış");
            bin_ = pgBinDir();
            phase("database");
            if (runCode(openssl(), {"x509", "-checkend", "172800", "-noout", "-in", dir_ + "/secrets/tls-cert"}) != 0) {
                say("hizmet sertifikası yenileniyor");
                issueServiceCertificate(dir_ + "/secrets", config_.value("sans").toVariant().toStringList(), false);
            }
            startPostgres(dir_, bin_);
            say("PostgreSQL çalışıyor");
            startAuth(password_.isEmpty() ? Auth::Plain : Auth::Register);
        } catch (const std::exception& e) { shutdown(failure(e)); }
    }

private:
    enum class Auth { Plain, Register, Bootstrap };
    QString dir_, assets_, admin_, bin_;
    QByteArray password_;
    QJsonObject config_;
    QProcess *auth_{}, *world_{};
    bool stopping_ = false, everRunning_ = false;
    int authPort_ = 0, adminPort_ = 0, worldPort_ = 0, ticks_ = 0;
    qint64 started_ = QDateTime::currentMSecsSinceEpoch();
    QList<qint64> crashes_;
    QNetworkAccessManager network_;
    QTimer watch_;

    static void say(const QString& line) {
        std::fprintf(stdout, "%s supervisor: %s\n", QDateTime::currentDateTimeUtc().toString(Qt::ISODate).toUtf8().constData(), line.toUtf8().constData());
        std::fflush(stdout);
    }
    void phase(const QString& name, const QString& message = {}) {
        say("aşama " + name + (message.isEmpty() ? QString() : ": " + message));
        try {
            saveJson(dir_ + "/state.json", {{"pid", static_cast<qint64>(::getpid())}, {"phase", name}, {"message", message},
                {"host", config_.value("host")}, {"bind", config_.value("bind")}, {"auth_port", authPort_}, {"admin_port", adminPort_},
                {"world_port", worldPort_}, {"started_at", started_}});
        } catch (const std::exception& e) { say(failure(e)); }
    }
    QProcess* spawn(const QString& name, const QStringList& args, const QString& pattern, int seconds,
                    std::function<void(const QRegularExpressionMatch&)> ready) {
        auto* process = new QProcess(this); process->setProgram(binary(name.toLatin1().constData())); process->setArguments(args);
        auto environment = QProcessEnvironment::systemEnvironment(); // auth backups run the cluster's own pg_dump
        environment.insert("PATH", bin_ + ":" + environment.value("PATH"));
        process->setProcessEnvironment(environment);
        process->setProcessChannelMode(QProcess::MergedChannels);
        process->setChildProcessModifier([] { ::prctl(PR_SET_PDEATHSIG, SIGTERM); }); // never orphaned by a killed supervisor
        auto* timer = new QTimer(process); timer->setSingleShot(true);
        connect(timer, &QTimer::timeout, this, [this, name] { shutdown(name + " hazır olmadı (zaman aşımı); Kayıt sekmesine bakın"); });
        auto pending = std::make_shared<QByteArray>();
        auto fired = std::make_shared<bool>(false);
        connect(process, &QProcess::readyRead, this, [this, process, name, pending, fired, timer, ready = std::move(ready), expression = QRegularExpression(pattern)] {
            pending->append(process->readAll());
            for (qsizetype newline; (newline = pending->indexOf('\n')) >= 0 || pending->size() > 65536;) {
                const auto line = QString::fromUtf8(pending->left(newline < 0 ? 65536 : newline)).trimmed();
                pending->remove(0, newline < 0 ? 65536 : newline + 1);
                std::fprintf(stdout, "%s: %s\n", name.toUtf8().constData(), line.toUtf8().constData());
                std::fflush(stdout);
                if (*fired) continue;
                if (const auto match = expression.match(line); match.hasMatch()) {
                    *fired = true; timer->stop();
                    try { ready(match); } catch (const std::exception& e) { shutdown(failure(e)); return; }
                }
            }
        });
        connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this, process, name](int code, QProcess::ExitStatus status) { exited(process, name, code, status); });
        connect(process, &QProcess::errorOccurred, this, [this, process, name](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart && (process == auth_ || process == world_)) shutdown(name + " başlatılamadı: " + process->errorString());
        });
        timer->start(seconds * 1000);
        process->start();
        return process;
    }
    QStringList tls() const {
        return {"--cert", dir_ + "/secrets/tls-cert", "--key", dir_ + "/secrets/tls-key", "--dsn-file", dir_ + "/secrets/dsn"};
    }
    void startAuth(Auth mode) {
        QStringList args{"--bind", config_.value("bind").toString(), "--port", QString::number(config_.value("auth_port").toInt()),
                         "--admin-bind", "127.0.0.1", "--admin-port", QString::number(config_.value("admin_port").toInt())};
        args << tls() << "--migrations" << hostResource("database/migrations") << "--web-root" << hostResource("web") << "--backup-dir" << dir_ + "/backups";
        if (mode == Auth::Bootstrap) args << "--admin-bootstrap" << admin_;
        phase(mode == Auth::Bootstrap ? "bootstrap" : "auth");
        auth_ = spawn("ikv-auth", args, "READY auth_port=(\\d+) admin_port=(\\d+)", 120, [this, mode](const QRegularExpressionMatch& match) {
            authPort_ = match.captured(1).toInt(); adminPort_ = match.captured(2).toInt();
            if (mode == Auth::Register) { registerAdmin(); return; }
            if (mode == Auth::Bootstrap) {
                auto saved = readJson(dir_ + "/host.json"); saved.insert("admin", admin_); saveJson(dir_ + "/host.json", saved);
                say("yönetici yetkisi verildi: " + admin_);
            }
            if (world_) phase("running"); else startWorld();
        });
    }
    void post(const QString& path, const QJsonObject& body, std::function<void(int, const QJsonObject&)> done) {
        QUrl url; url.setScheme("https"); url.setHost(config_.value("bind").toString()); url.setPort(authPort_); url.setPath(path);
        QNetworkRequest request(url); request.setSslConfiguration(pinnedTls(dir_ + "/secrets/client-ca.pem"));
        request.setTransferTimeout(30000); request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        auto* reply = network_.post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
        connect(reply, &QNetworkReply::finished, this, [reply, done = std::move(done)] {
            reply->deleteLater();
            done(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), QJsonDocument::fromJson(reply->read(1 << 20)).object());
        });
    }
    // Register the typed administrator over the pinned CA, then restart auth once with --admin-bootstrap for exactly
    // that account. A retry after a half-finished creation proves ownership by logging in instead.
    void registerAdmin() {
        phase("register");
        const QJsonObject credentials{{"username", admin_}, {"password", QString::fromUtf8(password_)}};
        post("/v1/register", credentials, [this, credentials](int code, const QJsonObject& reply) {
            const auto next = [this] {
                password_.fill('\0'); password_.clear();
                if (auto* process = std::exchange(auth_, nullptr)) { process->terminate(); if (!process->waitForFinished(15000)) { process->kill(); process->waitForFinished(5000); } }
                startAuth(Auth::Bootstrap);
            };
            if (code >= 200 && code < 300) { next(); return; }
            const QString reason = reply.value("error").toObject().value("message").toString(QString("HTTP %1").arg(code));
            post("/v1/login", credentials, [this, next, reason](int loginCode, const QJsonObject&) {
                if (loginCode >= 200 && loginCode < 300) next();
                else shutdown("Yönetici hesabı kaydedilemedi: " + reason);
            });
        });
    }
    void startWorld() {
        QStringList args{"--bind", config_.value("bind").toString(), "--port", QString::number(config_.value("world_port").toInt())};
        args << tls() << "--gameplay" << hostResource("config/gameplay.json") << "--asset-root" << assets_
             << "--bots" << QString::number(readJson(dir_ + "/host.json").value("bots").toInt());
        phase("world");
        world_ = spawn("ikv-server", args, "READY world_port=(\\d+)", 600, [this](const QRegularExpressionMatch& match) {
            worldPort_ = match.captured(1).toInt(); everRunning_ = true;
            phase("running");
        });
    }
    void exited(QProcess* process, const QString& name, int code, QProcess::ExitStatus status) {
        say(QString("%1 çıktı: kod=%2%3").arg(name).arg(code).arg(status == QProcess::CrashExit ? " (sinyal/çökme)" : ""));
        process->deleteLater();
        const bool world = process == world_;
        if (process == auth_) auth_ = nullptr; else if (world) world_ = nullptr; else return; // planned restarts
        if (stopping_) return;
        if (world && status == QProcess::NormalExit && code == 75) { say("yönetici yeniden başlatması"); startWorld(); return; }
        if (world && status == QProcess::NormalExit && code == 0) { say("yönetici kapatması"); shutdown({}); return; }
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        crashes_.erase(std::remove_if(crashes_.begin(), crashes_.end(), [now](qint64 at) { return now - at > 600000; }), crashes_.end());
        crashes_.append(now);
        if (!everRunning_ || crashes_.size() > 3) {
            shutdown(QString("%1 beklenmedik biçimde kapandı (kod=%2); son satırlar için Kayıt sekmesine bakın").arg(name).arg(code));
            return;
        }
        say(name + " yeniden başlatılıyor");
        if (world) startWorld(); else startAuth(Auth::Plain);
    }
    void shutdown(const QString& problem) {
        if (stopping_) return;
        stopping_ = true; watch_.stop();
        phase("stopping", problem);
        for (QProcess** slot : {&world_, &auth_}) // world first: it persists characters through auth's database
            if (auto* process = std::exchange(*slot, nullptr)) {
                process->terminate();
                if (!process->waitForFinished(20000)) { process->kill(); process->waitForFinished(5000); }
            }
        if (!bin_.isEmpty()) {
            try { stopPostgres(dir_, bin_); say("PostgreSQL durduruldu"); } catch (const std::exception& e) { say(failure(e)); }
        }
        authPort_ = adminPort_ = worldPort_ = 0;
        phase(problem.isEmpty() ? "stopped" : "failed", problem);
        QCoreApplication::exit(problem.isEmpty() ? 0 : 1);
    }
};

// ---- Launcher panel ----
const char* const stepTitles[] = {"Gereksinimler denetleniyor", "Boş portlar seçiliyor", "Anahtarlar ve sertifikalar oluşturuluyor",
    "Özel PostgreSQL veritabanı hazırlanıyor", "Hesap servisi başlatılıyor", "Yönetici hesabı oluşturuluyor",
    "Dünya sunucusu başlatılıyor (bölgeler yükleniyor)", "Başlatıcıda oturum açılıyor"};
constexpr int stepCount = 8;

class Panel final : public QDialog {
public:
    Panel(QWidget* parent, QPushButton* chip, Hooks hooks) : QDialog(parent), hooks_(std::move(hooks)), chip_(chip) {
        setObjectName("hostDialog"); setWindowTitle("Sunucu Yönetimi"); resize(760, 680);
        connect(chip_, &QPushButton::clicked, this, [this] { tick(); show(); raise(); activateWindow(); });
        auto* v = new QVBoxLayout(this);
        title_ = new QLabel; title_->setObjectName("hostTitle"); v->addWidget(title_);
        status_ = new QLabel; status_->setObjectName("hostStatus"); status_->setWordWrap(true); v->addWidget(status_);
        notice_ = new QLabel; notice_->setObjectName("hostNotice"); notice_->setWordWrap(true); v->addWidget(notice_);
        pages_ = new QStackedWidget; v->addWidget(pages_, 1);
        buildCreatePage(); buildManagePage();
        auto* close = new QHBoxLayout; v->addLayout(close); close->addStretch();
        button(close, "Kapat", "closeHost", [this] { hide(); });
        timer_.setInterval(500);
        connect(&timer_, &QTimer::timeout, this, [this] { tick(); });
        timer_.start();
        if (QFileInfo::exists(dir())) holdLauncherLock();
        QTimer::singleShot(0, this, [this] { tick(); }); // the launcher finishes building its profile widgets first
        const auto config = readJson(dir() + "/host.json");
        if (config.value("autostart").toBool() && !config.value("admin").toString().isEmpty() && !alive())
            QTimer::singleShot(0, this, [this] { guarded([this] { note("Otomatik başlatma"); startServer(); }); });
    }
    ~Panel() override {
        timer_.stop();
        for (auto* worker : workers_) worker->wait();
        if (lockFd_ < 0) return;
        ::close(std::exchange(lockFd_, -1));
        // Closing the launcher stops a server it does not keep alive, unless another launcher still holds the lock.
        if (readJson(dir() + "/host.json").value("keep_alive").toBool() || !alive() || anyHolder(dir() + "/launcher.lock")) return;
        if (const pid_t pid = lockPid(dir() + "/supervisor.lock")) ::kill(pid, SIGTERM);
        for (int i = 0; i < 600 && alive(); ++i) QThread::msleep(100);
    }

private:
    enum class Mode { Idle, Launching, Starting, Stopping, Busy };
    Hooks hooks_;
    QPushButton* chip_;
    QNetworkAccessManager network_;
    QLabel *title_{}, *status_{}, *notice_{};
    QStackedWidget* pages_{};
    QWidget *createPage_{}, *managePage_{};
    QLineEdit *name_{}, *adminUser_{}, *adminPassword_{}, *lanName_{};
    QSpinBox *bots_{}, *authPort_{}, *adminPort_{}, *worldPort_{};
    QRadioButton *local_{}, *lan_{};
    QComboBox* lanAddress_{};
    QWidget* advanced_{};
    QPushButton* createButton_{};
    QListWidget* steps_{};
    QLabel *state_{}, *addresses_{}, *uptime_{}, *players_{}, *botCount_{}, *zones_{}, *tickLabel_{}, *memory_{}, *counts_{}, *countdown_{}, *identity_{};
    QPushButton *start_{}, *stop_{}, *restart_{}, *cancel_{}, *login_{}, *logout_{}, *browser_{}, *delete_{};
    QSpinBox* restartDelay_{};
    QLineEdit* loginPassword_{};
    QCheckBox *keepAlive_{}, *autostart_{};
    QLineEdit *announce_{}, *banReason_{}, *accountSearch_{};
    QComboBox* banDuration_{};
    QTableWidget *playersTable_{}, *accounts_{};
    QSpinBox *botTarget_{}, *xp_{}, *drop_{}, *regen_{};
    QListWidget *zonesList_{}, *backups_{};
    QPlainTextEdit* logs_{};
    QList<QWidget*> needsSession_;
    QTimer timer_;
    QByteArray session_;
    QString pendingPassword_; // creation password, until the automatic launcher/admin login
    QJsonArray bots_now_;
    std::function<void()> afterStop_;
    Mode mode_ = Mode::Idle;
    bool polling_ = false, wasRunning_ = false, ours_ = false, settingsDirty_ = false, creating_ = false;
    int lockFd_ = -1, ticks_ = 0, currentStep_ = -1, players_now_ = -1;
    qint64 logOffset_ = -1;
    QDeadlineTimer startDeadline_;
    QList<QThread*> workers_;

    static QString dir() { return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/server"; }
    static QString ca() { return dir() + "/secrets/client-ca.pem"; }
    static bool alive() { return lockHeld(dir() + "/supervisor.lock"); }
    void guarded(const std::function<void()>& fn) { try { fn(); } catch (const std::exception& e) { report(failure(e)); } }
    QPushButton* button(QBoxLayout* row, const QString& text, const char* name, std::function<void()> fn) {
        auto* b = new QPushButton(text); b->setObjectName(name); row->addWidget(b);
        connect(b, &QPushButton::clicked, this, [this, fn = std::move(fn)] { guarded(fn); });
        return b;
    }
    template <class T> T* named(T* widget, const char* name) { widget->setObjectName(name); return widget; }
    QLabel* field(QFormLayout* form, const QString& title, const char* name) {
        auto* label = named(new QLabel("—"), name); label->setTextInteractionFlags(Qt::TextSelectableByMouse); form->addRow(title, label); return label;
    }
    void note(const QString& line) {
        logs_->appendPlainText(QDateTime::currentDateTimeUtc().toString(Qt::ISODate) + " başlatıcı: " + line);
        if (hooks_.log) hooks_.log(line);
    }
    void report(const QString& message) {
        notice_->setText("<span style='color:#c0392b'>" + message.toHtmlEscaped() + "</span>"); note(message);
        QMessageBox::warning(isVisible() ? static_cast<QWidget*>(this) : parentWidget(), "Sunucu", message);
    }
    void info(const QString& message) { notice_->setText(message.toHtmlEscaped()); note(message); }
    void markStep(int index, const char* mark) {
        if (index < 0 || index >= stepCount) return;
        steps_->item(index)->setText(QString::fromUtf8(mark) + "  " + QString::fromUtf8(stepTitles[index]));
        if (QString::fromUtf8(mark) == "●") currentStep_ = index;
    }
    void advanceTo(int index) { // everything before index done, index in progress
        for (int i = 0; i < index; ++i) markStep(i, "✓");
        markStep(index, "●");
    }
    void holdLauncherLock() {
        if (lockFd_ >= 0) return;
        lockFd_ = ::open(QFile::encodeName(dir() + "/launcher.lock").constData(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (lockFd_ >= 0 && ::flock(lockFd_, LOCK_SH) != 0) ::close(std::exchange(lockFd_, -1));
    }
    void saveSetting(const char* key, const QJsonValue& value) {
        auto config = readJson(dir() + "/host.json"); if (config.isEmpty()) return;
        config.insert(key, value); saveJson(dir() + "/host.json", config);
    }

    void buildCreatePage() {
        createPage_ = named(new QWidget, "hostCreatePage"); pages_->addWidget(createPage_);
        auto* v = new QVBoxLayout(createPage_);
        auto* intro = new QLabel("Bu bilgisayarda kendi topluluk sunucunuzu kurar: yerel geliştirme CA'sı ve gizli anahtarlar, "
                                 "yalnız Unix soketli özel PostgreSQL, ikv-auth ve ikv-server. Resmî sunucu kullanılmaz; varsayılan parola yoktur.");
        intro->setWordWrap(true); v->addWidget(intro);
        auto* f = new QFormLayout; v->addLayout(f);
        name_ = named(new QLineEdit("Yerel Sunucu"), "hostName"); f->addRow("İsim", name_);
        const auto* account = ::getpwuid(::geteuid());
        adminUser_ = named(new QLineEdit(account ? QString::fromLocal8Bit(account->pw_name) : QString("yonetici")), "hostAdminUser");
        f->addRow("Yönetici hesabı", adminUser_);
        adminPassword_ = named(new QLineEdit, "hostAdminPassword"); adminPassword_->setEchoMode(QLineEdit::Password);
        adminPassword_->setPlaceholderText("en az 12 karakter; yalnız hesap servisine gönderilir");
        f->addRow("Yönetici parolası", adminPassword_);
        bots_ = named(new QSpinBox, "hostBots"); bots_->setRange(0, 256); bots_->setValue(10);
        bots_->setToolTip("Sunucu tarafı botlar (NONCANONICAL); sonradan Sunucu Yönetimi'nden canlı değiştirilebilir");
        f->addRow("Bot sayısı", bots_);
        auto* access = new QHBoxLayout;
        local_ = named(new QRadioButton("Yerel (yalnız bu bilgisayar)"), "hostLocal"); local_->setChecked(true);
        lan_ = named(new QRadioButton("LAN (yerel ağ)"), "hostLan");
        lanAddress_ = named(new QComboBox, "hostLanAddress"); lanAddress_->setEditable(true);
        for (const auto& address : QNetworkInterface::allAddresses())
            if (address.protocol() == QAbstractSocket::IPv4Protocol && !address.isLoopback()) lanAddress_->addItem(address.toString());
        lanAddress_->setEnabled(false);
        connect(lan_, &QRadioButton::toggled, lanAddress_, &QWidget::setEnabled);
        access->addWidget(local_); access->addWidget(lan_); access->addWidget(lanAddress_, 1);
        f->addRow("Erişim", access);
        auto* more = named(new QCheckBox("Gelişmiş (portlar, LAN DNS adı)"), "hostAdvanced"); v->addWidget(more);
        advanced_ = new QWidget; auto* af = new QFormLayout(advanced_); advanced_->setVisible(false); v->addWidget(advanced_);
        connect(more, &QCheckBox::toggled, advanced_, &QWidget::setVisible);
        const auto port = [&](const QString& title, const char* name, int value) {
            auto* spin = named(new QSpinBox, name); spin->setRange(0, 65535); spin->setSpecialValueText("otomatik (işletim sistemi)");
            spin->setValue(value); af->addRow(title, spin); return spin;
        };
        authPort_ = port("Hesap portu", "hostAuthPort", 7443);
        adminPort_ = port("Yönetim portu (yalnız 127.0.0.1)", "hostAdminPort", 7444);
        worldPort_ = port("Dünya portu", "hostWorldPort", 7000);
        lanName_ = named(new QLineEdit, "hostLanName"); lanName_->setPlaceholderText("isteğe bağlı, sertifikaya eklenir"); af->addRow("LAN DNS adı", lanName_);
        auto* note = new QLabel("Dolu bir port seçilirse sıradaki boş port otomatik seçilir.");
        note->setEnabled(false); af->addRow(note);
        auto* r = new QHBoxLayout; v->addLayout(r);
        createButton_ = button(r, "Sunucuyu Kur ve Başlat", "hostCreate", [this] { create(); });
        createButton_->setMinimumHeight(40); createButton_->setDefault(true);
        connect(adminPassword_, &QLineEdit::returnPressed, createButton_, &QPushButton::click);
        steps_ = named(new QListWidget, "hostSteps"); steps_->setSelectionMode(QAbstractItemView::NoSelection);
        for (int i = 0; i < stepCount; ++i) steps_->addItem("○  " + QString::fromUtf8(stepTitles[i]));
        steps_->setMinimumHeight(steps_->sizeHintForRow(0) * stepCount + 2 * steps_->frameWidth() + 4);
        v->addWidget(steps_, 1);
    }
    void buildManagePage() {
        managePage_ = named(new QWidget, "hostManagePage"); pages_->addWidget(managePage_);
        auto* v = new QVBoxLayout(managePage_);
        auto* tabs = named(new QTabWidget, "hostTabs"); v->addWidget(tabs);

        auto* overview = new QWidget; auto* ov = new QVBoxLayout(overview);
        auto* f = new QFormLayout; ov->addLayout(f);
        state_ = field(f, "Durum", "hostState"); addresses_ = field(f, "Adresler", "hostAddresses");
        uptime_ = field(f, "Çalışma süresi", "hostUptime"); players_ = field(f, "Çevrimiçi oyuncu", "hostPlayers");
        botCount_ = field(f, "Bot", "hostBotCount"); zones_ = field(f, "Bölgeler", "hostZones");
        tickLabel_ = field(f, "Tick / süre", "hostTick"); memory_ = field(f, "Bellek", "hostMemory");
        counts_ = field(f, "Hesap / karakter / oturum", "hostCounts"); countdown_ = field(f, "Geri sayım", "hostCountdown");
        auto* controls = new QHBoxLayout; ov->addLayout(controls);
        start_ = button(controls, "Başlat", "hostStartButton", [this] { startServer(); });
        stop_ = button(controls, "Durdur", "hostStop", [this] { stopServer(); });
        restart_ = button(controls, "Yeniden başlat", "hostRestart", [this] { restartServer(); });
        restartDelay_ = named(new QSpinBox, "hostRestartDelay"); restartDelay_->setRange(0, 3600); restartDelay_->setValue(10);
        restartDelay_->setPrefix("duyuru "); restartDelay_->setSuffix(" sn"); controls->addWidget(restartDelay_);
        cancel_ = button(controls, "Geri sayımı iptal et", "hostCancelCountdown", [this] { call("POST", "/v1/admin/control", {{"command", "cancel"}}, [this](const QJsonObject&) { info("Geri sayım iptal edildi"); }); });
        controls->addStretch();
        auto* loginRow = new QHBoxLayout; ov->addLayout(loginRow);
        identity_ = named(new QLabel, "hostIdentity"); loginRow->addWidget(identity_, 1);
        loginPassword_ = named(new QLineEdit, "hostLoginPassword"); loginPassword_->setEchoMode(QLineEdit::Password);
        loginPassword_->setPlaceholderText("yönetici parolası"); loginPassword_->setMinimumWidth(240); loginRow->addWidget(loginPassword_);
        login_ = button(loginRow, "Yönetici girişi", "hostLogin", [this] { adminLogin(loginPassword_->text()); });
        connect(loginPassword_, &QLineEdit::returnPressed, login_, &QPushButton::click);
        logout_ = button(loginRow, "Çıkış", "hostLogout", [this] { call("POST", "/v1/logout", {}, [](const QJsonObject&) {}); session_.fill('\0'); session_.clear(); tick(); });
        keepAlive_ = named(new QCheckBox("Başlatıcı kapanınca da çalışsın (arka planda)"), "hostKeepAlive"); ov->addWidget(keepAlive_);
        keepAlive_->setToolTip("Sunucu ayrı bir gözetmen süreçte çalışır; başlatıcı yeniden açılınca ona bağlanır. Durdurmak için Durdur'a basın.");
        connect(keepAlive_, &QCheckBox::toggled, this, [this](bool on) { guarded([&] { saveSetting("keep_alive", on); }); });
        autostart_ = named(new QCheckBox("Başlatıcı açılınca yerel sunucuyu otomatik başlat"), "hostAutostart"); ov->addWidget(autostart_);
        connect(autostart_, &QCheckBox::toggled, this, [this](bool on) { guarded([&] { saveSetting("autostart", on); }); });
        auto* extra = new QHBoxLayout; ov->addLayout(extra);
        browser_ = button(extra, "Yönetim paneli (tarayıcı, gelişmiş)", "hostAdmin", [this] { openBrowser(); });
        extra->addStretch();
        delete_ = button(extra, "Sunucuyu sil", "hostDelete", [this] { deleteServer(); });
        ov->addStretch();
        tabs->addTab(overview, "Durum");

        auto* people = new QWidget; auto* pv = new QVBoxLayout(people);
        auto* announceRow = new QHBoxLayout; pv->addLayout(announceRow);
        announce_ = named(new QLineEdit, "hostAnnounceText"); announce_->setMaxLength(256); announce_->setPlaceholderText("Tüm oyunculara duyuru");
        announceRow->addWidget(announce_, 1);
        auto* announceButton = button(announceRow, "Duyur", "hostAnnounce", [this] {
            const auto text = announce_->text().trimmed();
            if (text.isEmpty()) throw hostIssue("Duyuru metni boş");
            call("POST", "/v1/admin/world", {{"command", "announce"}, {"message", text}}, [this, text](const QJsonObject&) { announce_->clear(); info("Duyuru gönderildi: " + text); });
        });
        connect(announce_, &QLineEdit::returnPressed, announceButton, &QPushButton::click);
        playersTable_ = named(new QTableWidget(0, 5), "hostPlayersTable");
        playersTable_->setHorizontalHeaderLabels({"Karakter", "Hesap", "Seviye", "Bölge", "Durum"});
        playersTable_->setSelectionBehavior(QAbstractItemView::SelectRows); playersTable_->setSelectionMode(QAbstractItemView::SingleSelection);
        playersTable_->setEditTriggers(QAbstractItemView::NoEditTriggers); playersTable_->horizontalHeader()->setStretchLastSection(true); playersTable_->verticalHeader()->setVisible(false);
        pv->addWidget(playersTable_, 1);
        auto* moderation = new QHBoxLayout; pv->addLayout(moderation);
        button(moderation, "Oyundan at", "hostKick", [this] {
            const auto row = selected(playersTable_, "oyuncu");
            call("POST", "/v1/admin/world", {{"command", "kick"}, {"character_id", row.value(0)}}, [this](const QJsonObject&) { info("Oyuncu atıldı"); poll(); });
        });
        banReason_ = named(new QLineEdit("Kural ihlali"), "hostBanReason"); moderation->addWidget(new QLabel("Sebep")); moderation->addWidget(banReason_, 1);
        banDuration_ = named(new QComboBox, "hostBanDuration");
        for (const auto& [label, seconds] : {std::pair{"1 saat", 3600}, std::pair{"1 gün", 86400}, std::pair{"7 gün", 604800}, std::pair{"Kalıcı", 0}})
            banDuration_->addItem(QString::fromUtf8(label), seconds);
        moderation->addWidget(banDuration_);
        button(moderation, "Hesabı yasakla", "hostBanPlayer", [this] { const auto row = selected(playersTable_, "oyuncu"); ban(row.value(1), row.value(2)); });
        auto* botRow = new QHBoxLayout; pv->addLayout(botRow);
        botRow->addWidget(new QLabel("Bot sayısı"));
        botTarget_ = named(new QSpinBox, "hostBotTarget"); botTarget_->setRange(0, 256); botRow->addWidget(botTarget_);
        button(botRow, "Uygula (canlı)", "hostBotApply", [this] { applyBots(botTarget_->value()); });
        botRow->addStretch();
        tabs->addTab(people, "Oyuncular");

        auto* accountsPage = new QWidget; auto* av = new QVBoxLayout(accountsPage);
        auto* search = new QHBoxLayout; av->addLayout(search);
        accountSearch_ = named(new QLineEdit, "hostAccountSearch"); accountSearch_->setPlaceholderText("kullanıcı adında ara (boş = ilk 100)");
        search->addWidget(accountSearch_, 1);
        auto* find = button(search, "Ara", "hostAccountRefresh", [this] { refreshAccounts(); });
        connect(accountSearch_, &QLineEdit::returnPressed, find, &QPushButton::click);
        accounts_ = named(new QTableWidget(0, 5), "hostAccountsTable");
        accounts_->setHorizontalHeaderLabels({"ID", "Kullanıcı", "Rol", "Durum", "Plus"});
        accounts_->setSelectionBehavior(QAbstractItemView::SelectRows); accounts_->setSelectionMode(QAbstractItemView::SingleSelection);
        accounts_->setEditTriggers(QAbstractItemView::NoEditTriggers); accounts_->horizontalHeader()->setStretchLastSection(true); accounts_->verticalHeader()->setVisible(false);
        av->addWidget(accounts_, 1);
        auto* accountActions = new QHBoxLayout; av->addLayout(accountActions);
        button(accountActions, "Yasakla (Oyuncular sekmesindeki sebep/süre)", "hostAccountBan", [this] { const auto row = selected(accounts_, "hesap"); ban(row.value(0), row.value(1)); });
        button(accountActions, "Yasağı kaldır", "hostAccountUnban", [this] {
            const auto row = selected(accounts_, "hesap");
            call("POST", "/v1/admin/accounts/" + row.value(0) + "/unban", {}, [this](const QJsonObject&) { info("Yasak kaldırıldı"); refreshAccounts(); });
        });
        accountActions->addStretch();
        tabs->addTab(accountsPage, "Hesaplar");

        auto* rates = new QWidget; auto* rv = new QVBoxLayout(rates); auto* rf = new QFormLayout; rv->addLayout(rf);
        const auto rate = [&](const QString& title, const char* name) {
            auto* spin = named(new QSpinBox, name); spin->setRange(10, 1000); spin->setValue(100); spin->setSuffix(" %"); rf->addRow(title, spin);
            connect(spin, &QSpinBox::valueChanged, this, [this] { settingsDirty_ = true; });
            return spin;
        };
        xp_ = rate("Deneyim oranı", "hostXpRate"); drop_ = rate("Ganimet oranı", "hostDropRate"); regen_ = rate("Yenilenme oranı", "hostRegenRate");
        rv->addWidget(new QLabel("Açık bölgeler (işaretsiz = girişe kapalı):"));
        zonesList_ = named(new QListWidget, "hostZonesList"); rv->addWidget(zonesList_, 1);
        connect(zonesList_, &QListWidget::itemChanged, this, [this] { settingsDirty_ = true; });
        auto* rateRow = new QHBoxLayout; rv->addLayout(rateRow);
        button(rateRow, "Kaydet ve uygula", "hostSettingsSave", [this] { saveSettings(); });
        button(rateRow, "Yenile", "hostSettingsRefresh", [this] { loadSettings(); });
        rateRow->addStretch();
        tabs->addTab(rates, "Oranlar");

        auto* backupPage = new QWidget; auto* bv = new QVBoxLayout(backupPage);
        auto* where = new QLabel("Yedek klasörü: " + dir() + "/backups (pg_dump, mod 0600)"); where->setWordWrap(true); bv->addWidget(where);
        backups_ = named(new QListWidget, "hostBackupsList"); bv->addWidget(backups_, 1);
        auto* backupRow = new QHBoxLayout; bv->addLayout(backupRow);
        button(backupRow, "Yedek al", "hostBackupCreate", [this] {
            info("Yedek alınıyor…");
            call("POST", "/v1/admin/backups", {}, [this](const QJsonObject& reply) { info("Yedek alındı: " + reply.value("name").toString()); refreshBackups(); }, 310000);
        });
        button(backupRow, "Listeyi yenile", "hostBackupRefresh", [this] { refreshBackups(); });
        button(backupRow, "Seçili yedeği geri yükle…", "hostBackupRestore", [this] { restore(); });
        backupRow->addStretch();
        tabs->addTab(backupPage, "Yedekler");

        auto* logPage = new QWidget; auto* lv = new QVBoxLayout(logPage);
        logs_ = named(new QPlainTextEdit, "hostLogs"); logs_->setReadOnly(true); logs_->document()->setMaximumBlockCount(5000);
        logs_->setLineWrapMode(QPlainTextEdit::NoWrap); lv->addWidget(logs_);
        tabs->addTab(logPage, "Kayıt");
        connect(tabs, &QTabWidget::currentChanged, this, [this, tabs](int index) {
            if (session_.isEmpty()) return;
            const auto title = tabs->tabText(index);
            if (title == "Hesaplar") refreshAccounts(); else if (title == "Yedekler") refreshBackups(); else if (title == "Oranlar") loadSettings();
        });
        needsSession_ = {announce_, playersTable_, banReason_, banDuration_, botTarget_, accounts_, accountSearch_, xp_, drop_, regen_, zonesList_, backups_};
        for (auto* name : {"hostAnnounce", "hostKick", "hostBanPlayer", "hostBotApply", "hostAccountRefresh", "hostAccountBan", "hostAccountUnban",
                           "hostSettingsSave", "hostSettingsRefresh", "hostBackupCreate", "hostBackupRefresh", "hostBackupRestore"})
            needsSession_ << managePage_->findChild<QPushButton*>(QString::fromLatin1(name));
    }
    static QStringList selected(QTableWidget* table, const char* what) {
        const auto rows = table->selectionModel()->selectedRows();
        if (rows.isEmpty()) throw hostIssue(QString("Önce bir %1 seçin").arg(QString::fromUtf8(what)));
        QStringList values;
        for (int column = 0; column < table->columnCount(); ++column) {
            const auto* item = table->item(rows.first().row(), column);
            values << (item ? item->data(Qt::UserRole).toString() : QString());
        }
        return values;
    }
    static void fill(QTableWidget* table, const QList<QStringList>& rows, const QList<QStringList>& keys) {
        table->setRowCount(static_cast<int>(rows.size()));
        for (int r = 0; r < rows.size(); ++r)
            for (int c = 0; c < rows[r].size(); ++c) {
                auto* item = new QTableWidgetItem(rows[r][c]);
                item->setData(Qt::UserRole, c < keys[r].size() ? keys[r][c] : rows[r][c]);
                table->setItem(r, c, item);
            }
        table->resizeColumnsToContents();
    }

    // ---- Admin API over the host's own development CA (127.0.0.1 only) ----
    static QString turkish(const QString& code, const QString& message) {
        static const QMap<QString, QString> known{
            {"forbidden", "Bu hesabın yönetici yetkisi yok."}, {"admin_required", "Bu hesabın yönetici yetkisi yok."},
            {"self_ban", "Etkin yönetici hesabı yasaklanamaz."}, {"command_pending", "Bekleyen bir yeniden başlatma/kapatma komutu var; önce iptal edin."},
            {"command_queue_full", "Dünya 64 bekleyen komutu işlemedi; dünya sunucusu çalışıyor mu?"},
            {"account_not_found", "Hesap bulunamadı."}, {"rate_limited", "Çok fazla deneme; biraz bekleyin."}};
        return known.value(code, message);
    }
    void call(const QByteArray& method, const QString& path, const QJsonObject& body, std::function<void(const QJsonObject&)> done,
              int timeoutMs = 15000, std::function<void(const QString&)> failed = {}) {
        const int port = readJson(dir() + "/state.json").value("admin_port").toInt();
        if (!alive() || port <= 0) throw hostIssue("Yerel sunucu çalışmıyor");
        QNetworkRequest request(QUrl(QString("https://127.0.0.1:%1%2").arg(port).arg(path)));
        request.setSslConfiguration(pinnedTls(ca()));
        request.setTransferTimeout(timeoutMs);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
        request.setRawHeader("Accept", "application/json");
        if (!body.isEmpty() || method == "POST") request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        if (!session_.isEmpty()) request.setRawHeader("Authorization", "Bearer " + session_);
        auto* reply = network_.sendCustomRequest(request, method, method == "POST" ? QJsonDocument(body).toJson(QJsonDocument::Compact) : QByteArray());
        connect(reply, &QNetworkReply::downloadProgress, reply, [reply](qint64 received, qint64) { if (received > (4 << 20)) reply->abort(); });
        connect(reply, &QNetworkReply::finished, this, [this, reply, path, port, done = std::move(done), failed = std::move(failed)] {
            reply->deleteLater();
            const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const auto object = QJsonDocument::fromJson(reply->readAll()).object();
            QString issue;
            if (code >= 200 && code < 300 && reply->error() == QNetworkReply::NoError) {
                try { done(object); } catch (const std::exception& e) { issue = failure(e); }
            } else if (code == 401) {
                session_.fill('\0'); session_.clear();
                issue = path == "/v1/login" ? object.value("error").toObject().value("message").toString("Kullanıcı adı veya parola hatalı.")
                                            : QString("Yönetici oturumu sona erdi; yeniden giriş yapın.");
            } else if (code) {
                const auto error = object.value("error").toObject();
                issue = turkish(error.value("code").toString(), error.value("message").toString(QString("Sunucu isteği reddetti (HTTP %1)").arg(code)));
            } else switch (reply->error()) {
                case QNetworkReply::ConnectionRefusedError: issue = QString("Yönetim servisine bağlanılamadı (127.0.0.1:%1); sunucu kapanıyor olabilir.").arg(port); break;
                case QNetworkReply::SslHandshakeFailedError: issue = "TLS doğrulaması başarısız: yanıt bu sunucunun yerel CA'sıyla imzalı değil."; break;
                case QNetworkReply::OperationCanceledError: case QNetworkReply::TimeoutError: issue = "Yönetim isteği zaman aşımına uğradı."; break;
                default: issue = "Ağ hatası: " + reply->errorString();
            }
            if (issue.isEmpty()) return;
            if (failed) failed(issue); else report(issue);
            tick();
        });
    }
    void adminLogin(const QString& password) {
        const auto admin = readJson(dir() + "/host.json").value("admin").toString();
        if (password.isEmpty()) throw hostIssue("Yönetici parolasını girin");
        session_.clear();
        call("POST", "/v1/login", {{"username", admin}, {"password", password}}, [this, admin](const QJsonObject& reply) {
            const auto token = reply.value("session").toString().toLatin1();
            if (!QRegularExpression("^[0-9a-f]{64}$").match(QString::fromLatin1(token)).hasMatch()) throw hostIssue("Sunucudan geçersiz oturum yanıtı geldi");
            session_ = token; loginPassword_->clear();
            info("Yönetici olarak giriş yapıldı: " + admin);
            settingsDirty_ = false;
            poll(); loadSettings(); refreshBackups(); refreshAccounts();
        });
    }
    void poll() {
        if (polling_ || !alive()) return;
        polling_ = true;
        const auto finished = [this](const QString& issue) { polling_ = false; if (!issue.isEmpty()) notice_->setText(issue.toHtmlEscaped()); };
        const bool admin = !session_.isEmpty();
        try {
            call("GET", admin ? "/v1/admin/status" : "/v1/status", {}, [this, admin, finished](const QJsonObject& reply) {
                polling_ = false;
                if (admin) render(reply);
                else { players_now_ = reply.value("online").toBool() ? reply.value("world").toObject().value("player_count").toInt(0) : -1; players_->setText(players_now_ < 0 ? "—" : QString::number(players_now_)); }
                finished({});
            }, 15000, finished);
        } catch (const std::exception& e) { finished(failure(e)); } // a timer tick never throws into Qt
    }
    void render(const QJsonObject& reply) {
        QJsonObject world;
        for (const auto& value : reply.value("worlds").toArray()) if (world.isEmpty() || value.toObject().value("online").toBool()) world = value.toObject();
        const bool live = world.value("online").toBool();
        const auto status = world.value("status").toObject();
        const auto online = status.value("online").toArray(), zones = status.value("zones").toArray();
        bots_now_ = status.value("bots").toArray();
        players_now_ = live ? static_cast<int>(online.size()) : -1;
        players_->setText(live ? QString::number(online.size()) : QString("Dünya çevrimdışı"));
        botCount_->setText(live ? QString::number(bots_now_.size()) : QString("—"));
        if (!botTarget_->hasFocus() && live) botTarget_->setValue(static_cast<int>(bots_now_.size()));
        int disabled = 0; QStringList zoneText;
        for (const auto& value : zones) {
            const auto zone = value.toObject(); disabled += zone.value("disabled").toBool();
            if (zone.value("players").toInt() || zone.value("bots").toInt())
                zoneText << QString("%1 (%2 oyuncu, %3 bot)").arg(zone.value("zone").toString()).arg(zone.value("players").toInt()).arg(zone.value("bots").toInt());
        }
        zones_->setText(live ? QString("%1 bölge, %2 kapalı%3").arg(zones.size()).arg(disabled).arg(zoneText.isEmpty() ? QString() : " · " + zoneText.join(", ")) : QString("—"));
        tickLabel_->setText(live ? QString("%1 · son tick %2 ms (%3 Hz) · bağlantı %4").arg(countText(status.value("tick")))
                                       .arg(status.value("tick_ms").toDouble(), 0, 'f', 2).arg(status.value("tick_hz").toInt(20)).arg(status.value("connections").toInt())
                                 : QString("—"));
        memory_->setText(live && status.value("memory_kib").isDouble() ? QString("%1 MiB").arg(status.value("memory_kib").toDouble() / 1024, 0, 'f', 1) : QString("—"));
        counts_->setText(QString("%1 / %2 / %3").arg(countText(reply.value("accounts")), countText(reply.value("characters")), countText(reply.value("sessions"))));
        const auto countdown = status.value("countdown").toObject();
        countdown_->setText(countdown.isEmpty() ? QString("yok") : QString("%1 sn sonra %2").arg(countdown.value("seconds").toInt())
                                                     .arg(countdown.value("command").toString() == "restart" ? "yeniden başlatma" : "kapatma"));
        cancel_->setEnabled(!countdown.isEmpty());
        if (live) uptime_->setText(since(status.value("uptime_seconds").toInteger(-1)));
        QList<QStringList> rows, keys;
        for (const auto& value : online) {
            const auto p = value.toObject();
            rows << QStringList{QString("%1 (#%2)").arg(p.value("name").toString()).arg(p.value("character_id").toInteger()), QString::number(p.value("account_id").toInteger()),
                                QString::number(p.value("level").toInt()), p.value("zone").toString(),
                                QString("%1%2").arg(p.value("admin").toBool() ? "yönetici " : "", p.value("muted").toBool() ? "susturulmuş" : "")};
            keys << QStringList{QString::number(p.value("character_id").toInteger()), QString::number(p.value("account_id").toInteger()), p.value("name").toString()};
        }
        const auto chosen = playersTable_->selectionModel()->selectedRows();
        const int keep = chosen.isEmpty() ? -1 : chosen.first().row();
        fill(playersTable_, rows, keys);
        if (keep >= 0 && keep < playersTable_->rowCount()) playersTable_->selectRow(keep);
        if (zonesList_->count() == 0 && !zones.isEmpty()) {
            QSignalBlocker block(zonesList_);
            for (const auto& value : zones) {
                auto* item = new QListWidgetItem(value.toObject().value("zone").toString(), zonesList_);
                item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
                item->setCheckState(value.toObject().value("disabled").toBool() ? Qt::Unchecked : Qt::Checked);
            }
        }
        updateChip();
    }
    void ban(const QString& accountId, const QString& who) {
        const int seconds = banDuration_->currentData().toInt();
        const auto reason = banReason_->text().trimmed();
        if (reason.isEmpty()) throw hostIssue("Yasaklama sebebi gerekli");
        if (QMessageBox::question(this, "Yasakla", QString("%1 (hesap %2) %3 yasaklansın mı? Oturumları hemen kapatılır.")
                .arg(who, accountId, seconds ? "için " + banDuration_->currentText() : QString("kalıcı olarak"))) != QMessageBox::Yes) return;
        call("POST", "/v1/admin/accounts/" + accountId + "/ban", {{"reason", reason}, {"duration_seconds", seconds}},
             [this, who](const QJsonObject&) { info(who + " yasaklandı"); refreshAccounts(); poll(); });
    }
    // Live bot count: spawn (≤50 per command) or despawn the newest bots (≤64 ids per command); also kept for the next start.
    void applyBots(int target) {
        saveSetting("bots", target);
        const int now = static_cast<int>(bots_now_.size());
        if (target > now) {
            const int count = std::min(50, target - now);
            call("POST", "/v1/admin/bots", {{"action", "spawn"}, {"count", count}, {"behaviour", "town"}}, [this, target, count](const QJsonObject&) {
                info(QString("%1 bot ekleniyor (hedef %2)").arg(count).arg(target));
                if (target - static_cast<int>(bots_now_.size()) > count) { for (int i = 0; i < count; ++i) bots_now_.append(QJsonObject{}); applyBots(target); }
            });
        } else if (target < now) {
            QJsonArray ids;
            for (int i = now - 1; i >= target && ids.size() < 64; --i) ids.append(bots_now_.at(i).toObject().value("id"));
            call("POST", "/v1/admin/bots", {{"action", "despawn"}, {"ids", ids}}, [this, target, ids](const QJsonObject&) { info(QString("%1 bot kaldırılıyor (hedef %2)").arg(ids.size()).arg(target)); });
        } else info(QString("Bot sayısı zaten %1").arg(target));
    }
    void loadSettings() {
        call("GET", "/v1/admin/settings", {}, [this](const QJsonObject& reply) {
            const auto values = reply.value("settings").toObject();
            QSignalBlocker a(xp_), b(drop_), c(regen_), d(zonesList_);
            xp_->setValue(values.value("xp_rate").toInt(100)); drop_->setValue(values.value("drop_rate").toInt(100)); regen_->setValue(values.value("regen_rate").toInt(100));
            QStringList closed; for (const auto& zone : values.value("disabled_zones").toArray()) closed << zone.toString();
            for (int i = 0; i < zonesList_->count(); ++i) zonesList_->item(i)->setCheckState(closed.contains(zonesList_->item(i)->text()) ? Qt::Unchecked : Qt::Checked);
            settingsDirty_ = false;
        });
    }
    void saveSettings() {
        QJsonArray closed;
        for (int i = 0; i < zonesList_->count(); ++i) if (zonesList_->item(i)->checkState() != Qt::Checked) closed.append(zonesList_->item(i)->text());
        call("POST", "/v1/admin/settings", {{"xp_rate", xp_->value()}, {"drop_rate", drop_->value()}, {"regen_rate", regen_->value()}, {"disabled_zones", closed}},
             [this](const QJsonObject&) { settingsDirty_ = false; info("Oranlar kaydedildi; dünya bir sonraki saniyede uygular"); });
    }
    void refreshAccounts() {
        call("GET", "/v1/admin/accounts?search=" + QString::fromLatin1(QUrl::toPercentEncoding(accountSearch_->text().trimmed())), {}, [this](const QJsonObject& reply) {
            QList<QStringList> rows, keys;
            for (const auto& value : reply.value("accounts").toArray()) {
                const auto a = value.toObject(); const auto id = QString::number(a.value("id").toInteger());
                rows << QStringList{id, a.value("username").toString(), a.value("admin").toBool() ? "Yönetici" : "Oyuncu",
                                    a.value("banned").toBool() ? "Yasaklı: " + a.value("ban_reason").toString() : QString("Etkin"), a.value("plus").toBool() ? "evet" : ""};
                keys << QStringList{id, a.value("username").toString()};
            }
            fill(accounts_, rows, keys);
        });
    }
    void refreshBackups() {
        call("GET", "/v1/admin/backups", {}, [this](const QJsonObject& reply) {
            backups_->clear();
            for (const auto& value : reply.value("backups").toArray()) {
                const auto name = value.toObject().value("name").toString();
                const qint64 stamp = name.section('-', 1, 1).toLongLong(); // system_clock ticks (ns)
                auto* item = new QListWidgetItem(QString("%1 — %2 KiB%3").arg(name).arg(value.toObject().value("size").toInteger() / 1024)
                    .arg(stamp > 100000000000000000LL ? " — " + QDateTime::fromMSecsSinceEpoch(stamp / 1000000).toString("yyyy-MM-dd HH:mm:ss") : QString()), backups_);
                item->setData(Qt::UserRole, name);
            }
            if (backups_->count()) backups_->setCurrentRow(0);
        });
    }
    // Restore is offline (ikv-auth --restore-backup) with auth and world stopped; the server then starts again.
    void restore() {
        const auto* item = backups_->currentItem();
        if (!item) throw hostIssue("Önce bir yedek seçin");
        const auto name = item->data(Qt::UserRole).toString();
        if (!QRegularExpression("^ikv-[0-9]+-[0-9a-f]+\\.sql$").match(name).hasMatch()) throw hostIssue("Geçersiz yedek adı");
        if (QMessageBox::question(this, "Geri yükle", "Yedek geri yüklensin mi?\n" + name + "\n\nSunucu durdurulur; yedekten sonraki hesap, karakter ve "
                                  "ayar değişiklikleri kaybolur. Ardından sunucu yeniden başlatılır.") != QMessageBox::Yes) return;
        const auto file = dir() + "/backups/" + name;
        afterStop_ = [this, file, name] {
            mode_ = Mode::Busy; info("Yedek geri yükleniyor…");
            background([file] {
                const QString bin = pgBinDir(), home = dir();
                auto environment = QProcessEnvironment::systemEnvironment(); environment.insert("PATH", bin + ":" + environment.value("PATH"));
                startPostgres(home, bin);
                try { run(binary("ikv-auth"), {"--dsn-file", home + "/secrets/dsn", "--restore-backup", file}, {}, {}, environment); }
                catch (...) { stopPostgres(home, bin); throw; }
                stopPostgres(home, bin);
            }, [this, name] { mode_ = Mode::Idle; info("Yedek geri yüklendi: " + name); session_.clear(); startServer(); });
        };
        if (alive()) stopServer(); else { auto next = std::exchange(afterStop_, {}); next(); }
    }
    void background(std::function<void()> work, std::function<void()> then) {
        auto* thread = QThread::create([this, work = std::move(work), then = std::move(then)] {
            QString issue;
            try { work(); } catch (const std::exception& e) { issue = failure(e); }
            QMetaObject::invokeMethod(this, [this, issue, then] {
                if (!issue.isEmpty()) { mode_ = Mode::Idle; markStep(currentStep_, "✗"); report(issue); tick(); return; }
                guarded(then);
            }, Qt::QueuedConnection);
        });
        workers_.append(thread);
        connect(thread, &QThread::finished, this, [this, thread] { workers_.removeOne(thread); thread->deleteLater(); });
        thread->start();
    }

    // ---- Creation and lifecycle ----
    void requireTools() {
        if (!hooks_.hasAssets()) throw hostIssue("Önce özgün oyun dosyalarını kurun (Oyunu Kur): dünya sunucusu içe aktarılan dosyaları okur");
        for (const char* name : {"ikv-auth", "ikv-server"})
            if (!QFileInfo(binary(name)).isExecutable()) throw hostIssue(QString::fromLatin1(name) + " başlatıcının yanında bulunamadı; sunucu bileşenleri kurulu değil");
        for (const char* resource : {"database/migrations", "web", "config/gameplay.json"}) hostResource(resource);
        pgBinDir(); openssl();
    }
    // Configured ports that are taken move to the next free port (0 stays OS-assigned).
    void pickPorts(QJsonObject& config) {
        QList<int> used;
        const auto bind = config.value("bind").toString();
        for (const auto& [key, address, label] : {std::tuple{"auth_port", bind, "Hesap"}, std::tuple{"admin_port", QString("127.0.0.1"), "Yönetim"},
                                                  std::tuple{"world_port", bind, "Dünya"}}) {
            const int wanted = config.value(key).toInt();
            if (wanted == 0) continue;
            const auto free = [&](int port) { QTcpServer probe; return !used.contains(port) && probe.listen(QHostAddress(address), static_cast<quint16>(port)); };
            int port = wanted;
            while (port < std::min(65535, wanted + 200) && !free(port)) ++port;
            if (!free(port)) throw hostIssue(QString("%1 portu %2 ve sonraki 200 port dolu; Gelişmiş'ten başka bir port seçin").arg(QString::fromUtf8(label)).arg(wanted));
            if (port != wanted) info(QString("%1 portu %2 kullanımda; %3 seçildi").arg(QString::fromUtf8(label)).arg(wanted).arg(port));
            used << port; config.insert(key, port);
        }
    }
    void create() {
        if (mode_ != Mode::Idle || alive()) throw hostIssue("Sunucu zaten çalışıyor veya hazırlanıyor");
        for (int i = 0; i < stepCount; ++i) markStep(i, "○");
        advanceTo(0);
        const auto admin = adminUser_->text().trimmed(), password = adminPassword_->text(), title = name_->text().trimmed();
        if (title.isEmpty()) throw hostIssue("Sunucuya bir isim verin");
        if (!QRegularExpression("^[A-Za-z0-9_.-]{3,32}$").match(admin).hasMatch()) throw hostIssue("Yönetici hesabı 3–32 karakter olmalı: harf, rakam, _ . -");
        if (password.size() < 12) throw hostIssue("Yönetici parolası en az 12 karakter olmalı");
        requireTools();
        auto config = readJson(dir() + "/host.json");
        const bool provisioned = !config.isEmpty();
        if (!provisioned) { // SANs are fixed at provisioning; renewal reuses them.
            QString bind = "127.0.0.1", host = "localhost";
            QStringList sans{"DNS:localhost", "IP:127.0.0.1"};
            if (lan_->isChecked()) {
                const QHostAddress address(lanAddress_->currentText().trimmed());
                if (address.protocol() != QAbstractSocket::IPv4Protocol || address.isLoopback() || !QNetworkInterface::allAddresses().contains(address))
                    throw hostIssue("LAN için bu bilgisayarın bir IPv4 arayüz adresini seçin");
                bind = host = address.toString(); sans << "IP:" + bind;
                if (const QString dns = lanName_->text().trimmed(); !dns.isEmpty()) {
                    if (!QRegularExpression("^[A-Za-z0-9]([A-Za-z0-9.-]{0,251}[A-Za-z0-9])?$").match(dns).hasMatch()) throw hostIssue("Geçersiz LAN DNS adı");
                    sans << "DNS:" + dns; host = dns;
                }
            }
            config = {{"bind", bind}, {"host", host}, {"sans", QJsonArray::fromStringList(sans)}, {"keep_alive", false}, {"autostart", false}};
        }
        config.insert("name", title); config.insert("bots", bots_->value());
        config.insert("auth_port", authPort_->value()); config.insert("admin_port", adminPort_->value()); config.insert("world_port", worldPort_->value());
        advanceTo(1);
        pickPorts(config);
        advanceTo(2);
        mode_ = Mode::Busy; ours_ = true; creating_ = true; refreshButtons();
        const auto launch = [this, admin, password] {
            adminPassword_->clear();
            pendingPassword_ = password;
            launchSupervisor(admin, password.toUtf8());
        };
        if (provisioned) { saveJson(dir() + "/host.json", config); advanceTo(4); launch(); return; }
        info("Sunucu hazırlanıyor…");
        background([this, config] {
            const QString home = dir(), bin = pgBinDir();
            QDir().mkpath(QFileInfo(home).absolutePath());
            const bool existed = QFileInfo::exists(home);
            try {
                privateDirectory(home);
                provisionHost(home, config.value("sans").toVariant().toStringList(), bin,
                              [this](int step) { QMetaObject::invokeMethod(this, [this, step] { advanceTo(step); }, Qt::QueuedConnection); });
                saveJson(home + "/host.json", config);
            } catch (...) {
                if (QFileInfo::exists(home + "/pg/postmaster.pid")) runCode(bin + "/pg_ctl", {"stop", "-D", home + "/pg", "-m", "immediate", "-w", "-s"});
                if (!existed) QDir(home).removeRecursively();
                throw;
            }
        }, [this, launch] { advanceTo(4); launch(); });
    }
    void startServer() {
        if (mode_ != Mode::Idle || alive()) throw hostIssue("Sunucu zaten çalışıyor veya hazırlanıyor");
        auto config = readJson(dir() + "/host.json");
        if (config.value("admin").toString().isEmpty()) throw hostIssue("Sunucu henüz kurulmadı");
        requireTools();
        pickPorts(config);
        saveJson(dir() + "/host.json", config);
        ours_ = true;
        launchSupervisor({}, {});
    }
    static QString supervisorProgram() {
        const auto image = qEnvironmentVariable("APPIMAGE"); // its own mount outlives this launcher's
        return !image.isEmpty() && QFileInfo(image).isExecutable() ? image : binary("ikv-launcher");
    }
    void launchSupervisor(const QString& admin, QByteArray password) {
        holdLauncherLock();
        QStringList args{"--host-supervisor", dir(), "--asset-root", hooks_.installRoot() + "/assets"};
        if (!admin.isEmpty()) args << "--bootstrap-admin" << admin;
        auto* process = new QProcess(this); process->setProgram(supervisorProgram()); process->setArguments(args);
        process->setProcessChannelMode(QProcess::MergedChannels);
        mode_ = Mode::Launching; refreshButtons();
        info(admin.isEmpty() ? "Sunucu başlatılıyor…" : "Sunucu başlatılıyor ve yönetici hesabı oluşturuluyor…");
        connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
            if (error != QProcess::FailedToStart) return;
            process->deleteLater(); mode_ = Mode::Idle; pendingPassword_.clear();
            report("Sunucu gözetmeni başlatılamadı: " + process->errorString());
        });
        connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [this, process](int code, QProcess::ExitStatus status) {
            process->deleteLater();
            const auto output = QString::fromUtf8(process->readAll()).trimmed();
            if (status == QProcess::NormalExit && code == 0) { mode_ = Mode::Starting; startDeadline_ = QDeadlineTimer(15 * 60 * 1000); return; }
            mode_ = Mode::Idle; pendingPassword_.clear();
            report(code == 3 ? QString("Sunucu zaten başka bir başlatıcı tarafından çalıştırılıyor") : "Sunucu gözetmeni başlatılamadı: " + output);
        });
        process->start();
        if (!password.isEmpty()) process->write(password + '\n');
        password.fill('\0');
        process->closeWriteChannel();
    }
    void stopServer() {
        const pid_t pid = lockPid(dir() + "/supervisor.lock");
        if (!alive() || !pid) throw hostIssue("Yerel sunucu çalışmıyor");
        if (::kill(pid, SIGTERM) != 0) throw hostIssue("Sunucu gözetmenine durdurma sinyali gönderilemedi");
        mode_ = Mode::Stopping; info("Sunucu durduruluyor (dünya kaydediyor)…"); refreshButtons();
    }
    void restartServer() {
        if (session_.isEmpty()) { // without an admin session: full stop and start
            afterStop_ = [this] { startServer(); };
            stopServer();
            return;
        }
        call("POST", "/v1/admin/control", {{"command", "restart"}, {"delay_seconds", restartDelay_->value()}, {"message", "Bakım için kısa bir ara."}},
             [this](const QJsonObject&) { info(QString("Dünya %1 sn sonra yeniden başlatılacak (oyunculara duyuruldu)").arg(restartDelay_->value())); });
    }
    void deleteServer() {
        if (mode_ != Mode::Idle || alive()) throw hostIssue("Önce yerel sunucuyu durdurun");
        if (QMessageBox::question(this, "Sunucuyu sil", "Yerel sunucunun hesapları, karakterleri, veritabanı, yedekleri ve anahtarları kalıcı olarak silinsin mi?\n" + dir()) != QMessageBox::Yes) return;
        if (QFileInfo::exists(dir() + "/pg/postmaster.pid")) stopPostgres(dir(), pgBinDir()); // a cluster left by a killed supervisor
        if (lockFd_ >= 0) ::close(std::exchange(lockFd_, -1));
        if (!QDir(dir()).removeRecursively()) throw hostIssue("Sunucu klasörü silinemedi: " + dir());
        session_.clear(); logs_->clear(); logOffset_ = -1;
        for (int i = 0; i < stepCount; ++i) markStep(i, "○");
        hooks_.useProfile({}, {}, {}, {}, false);
        info("Yerel sunucu silindi");
        tick();
    }
    void openBrowser() {
        const QUrl url(QString("https://127.0.0.1:%1/admin").arg(readJson(dir() + "/state.json").value("admin_port").toInt()));
        note("Yönetim paneli " + url.toString() + " — tarayıcıya yalnız bu yerel geliştirme CA'sını güvenilir olarak ekleyin: " + ca());
        if (!QDesktopServices::openUrl(url)) throw hostIssue("Tarayıcı açılamadı: " + url.toString());
    }
    void onRunning(const QJsonObject& state) {
        const auto config = readJson(dir() + "/host.json");
        QUrl auth; auth.setScheme("https"); auth.setHost(state.value("host").toString()); auth.setPort(state.value("auth_port").toInt());
        const auto password = std::exchange(pendingPassword_, {});
        hooks_.useProfile(auth.toString(), ca(), config.value("admin").toString(), password, ours_);
        if (!password.isEmpty()) { advanceTo(7); markStep(7, "✓"); guarded([&] { adminLogin(password); }); }
        info(ours_ ? "Yerel sunucu çalışıyor" : "Arka planda çalışan yerel sunucuya bağlanıldı");
        ours_ = false;
    }

    // ---- Periodic state: supervisor lock + state.json, log tail, API status ----
    void tick() {
        const auto config = readJson(dir() + "/host.json");
        const bool provisioned = !config.isEmpty(), admin = !config.value("admin").toString().isEmpty(), up = alive();
        if (provisioned) holdLauncherLock();
        const auto state = readJson(dir() + "/state.json");
        const auto phase = state.value("phase").toString();
        const bool running = up && phase == "running";
        if (mode_ == Mode::Starting && up && phase != "running") {
            static const QMap<QString, int> steps{{"database", 3}, {"auth", 4}, {"register", 5}, {"bootstrap", 5}, {"world", 6}};
            if (steps.contains(phase) && steps.value(phase) > currentStep_) advanceTo(steps.value(phase));
            if (startDeadline_.hasExpired()) { mode_ = Mode::Idle; report("Sunucu 15 dakikada hazır olmadı; Kayıt sekmesine bakın"); }
        }
        if ((mode_ == Mode::Starting || mode_ == Mode::Stopping) && !up) {
            const bool stopping = mode_ == Mode::Stopping;
            mode_ = Mode::Idle;
            if (!stopping) {
                markStep(currentStep_, "✗"); pendingPassword_.clear();
                report("Sunucu başlatılamadı: " + state.value("message").toString("gözetmen beklenmedik biçimde kapandı; Kayıt sekmesine bakın"));
            } else {
                info(phase == "failed" ? "Sunucu durdu: " + state.value("message").toString() : QString("Yerel sunucu durduruldu"));
                if (auto next = std::exchange(afterStop_, {})) guarded(next);
            }
        }
        if (running && !wasRunning_) { if (mode_ == Mode::Starting) mode_ = Mode::Idle; onRunning(state); }
        if (!running && wasRunning_ && mode_ == Mode::Idle && !up && phase == "failed") info("Sunucu durdu: " + state.value("message").toString());
        wasRunning_ = running;
        if (!up) players_now_ = -1;
        if (mode_ == Mode::Idle) creating_ = false; // the creation page stays up until the server runs or fails
        pages_->setCurrentWidget(admin && !creating_ ? managePage_ : createPage_);
        title_->setText("<h2>" + (admin && !creating_ ? "Sunucu Yönetimi — " + config.value("name").toString("Yerel Sunucu").toHtmlEscaped() : QString("Sunucu Kur")) + "</h2>");
        setWindowTitle(admin && !creating_ ? "Sunucu Yönetimi" : "Sunucu Kur");
        const QString phaseText = phase == "database" ? "veritabanı" : phase == "auth" || phase == "bootstrap" ? "hesap servisi" : phase == "register" ? "yönetici kaydı" : phase == "world" ? "dünya yükleniyor" : phase;
        status_->setText(running ? QString("Çalışıyor — hesap %1:%2, yönetim 127.0.0.1:%3, dünya %1:%4")
                                       .arg(state.value("host").toString()).arg(state.value("auth_port").toInt()).arg(state.value("admin_port").toInt()).arg(state.value("world_port").toInt())
                         : up ? (phase == "stopping" ? QString("Durduruluyor…") : "Başlatılıyor (" + phaseText + ")…")
                         : mode_ == Mode::Launching || mode_ == Mode::Busy ? QString("Hazırlanıyor…")
                         : phase == "failed" ? "Durduruldu (hata: " + state.value("message").toString() + ")"
                         : provisioned ? QString("Durduruldu") : QString("Kurulmadı"));
        state_->setText(status_->text());
        addresses_->setText(running ? QString("oyuncular https://%1:%2 · dünya %1:%3 · yönetim 127.0.0.1:%4 (yalnız bu bilgisayar)")
                                          .arg(state.value("host").toString()).arg(state.value("auth_port").toInt()).arg(state.value("world_port").toInt()).arg(state.value("admin_port").toInt())
                                    : QString("—"));
        if (!running) {
            for (auto* label : {uptime_, players_, botCount_, zones_, tickLabel_, memory_, countdown_}) label->setText("—");
            if (playersTable_->rowCount()) playersTable_->setRowCount(0);
        } else if (session_.isEmpty()) uptime_->setText(since((QDateTime::currentMSecsSinceEpoch() - state.value("started_at").toInteger()) / 1000));
        {
            QSignalBlocker a(keepAlive_), b(autostart_);
            keepAlive_->setChecked(config.value("keep_alive").toBool()); autostart_->setChecked(config.value("autostart").toBool());
        }
        identity_->setText(session_.isEmpty() ? QString("Yönetim işlemleri için giriş yapın (%1):").arg(config.value("admin").toString().toHtmlEscaped())
                                              : "Yönetici: " + config.value("admin").toString().toHtmlEscaped());
        // A half-finished creation (no admin yet) keeps its access mode and ports.
        for (QWidget* widget : std::initializer_list<QWidget*>{local_, lan_, lanAddress_, lanName_}) widget->setEnabled(!provisioned && mode_ == Mode::Idle && (widget != lanAddress_ || lan_->isChecked()));
        if (provisioned && !admin) { lan_->setChecked(config.value("bind").toString() != "127.0.0.1"); local_->setChecked(!lan_->isChecked()); }
        tailLog();
        if (running && ++ticks_ % 4 == 1) poll();
        refreshButtons();
        updateChip();
    }
    void refreshButtons() {
        const bool up = alive(), idle = mode_ == Mode::Idle;
        const bool running = up && readJson(dir() + "/state.json").value("phase").toString() == "running";
        createButton_->setEnabled(idle && !up);
        for (QWidget* widget : std::initializer_list<QWidget*>{name_, adminUser_, adminPassword_, bots_, authPort_, adminPort_, worldPort_}) widget->setEnabled(idle && !up);
        start_->setEnabled(idle && !up);
        stop_->setEnabled(up && mode_ != Mode::Stopping);
        restart_->setEnabled(running && idle);
        delete_->setEnabled(idle && !up && QFileInfo::exists(dir()));
        browser_->setEnabled(running);
        const bool session = running && !session_.isEmpty();
        loginPassword_->setVisible(session_.isEmpty()); login_->setVisible(session_.isEmpty()); logout_->setVisible(!session_.isEmpty());
        login_->setEnabled(running);
        for (auto* widget : needsSession_) widget->setEnabled(session);
        if (!session) cancel_->setEnabled(false);
    }
    void tailLog() {
        QFile file(dir() + "/server.log");
        if (!file.open(QIODevice::ReadOnly)) return;
        const qint64 size = file.size();
        if (logOffset_ < 0 || size < logOffset_) logOffset_ = std::max<qint64>(0, size - 65536); // first read or rotated
        if (size == logOffset_ || !file.seek(logOffset_)) return;
        const auto bytes = file.read(std::min<qint64>(size - logOffset_, 1 << 18));
        const auto end = bytes.lastIndexOf('\n');
        if (end < 0) return;
        logOffset_ += end + 1;
        logs_->appendPlainText(QString::fromUtf8(bytes.left(end)));
    }
    void updateChip() {
        const auto config = readJson(dir() + "/host.json");
        const bool up = alive();
        const auto phase = readJson(dir() + "/state.json").value("phase").toString();
        QString state = "none", text = "Sunucu Kur";
        if (!config.value("admin").toString().isEmpty() || up || mode_ != Mode::Idle) {
            if (up && phase == "running") { state = "running"; text = "● Sunucu: Çalışıyor" + (players_now_ >= 0 ? QString(" · %1 oyuncu").arg(players_now_) : QString()); }
            else if (up && phase == "stopping") { state = "stopping"; text = "◌ Sunucu: Durduruluyor…"; }
            else if (up || mode_ != Mode::Idle) { state = "starting"; text = "◌ Sunucu: Başlatılıyor…"; }
            else if (phase == "failed") { state = "failed"; text = "✕ Sunucu: Hata"; }
            else { state = "stopped"; text = "○ Sunucu: Kapalı"; }
        }
        chip_->setText(text);
        chip_->setToolTip(config.isEmpty() ? QString("Bu bilgisayarda bir topluluk sunucusu kurun") : status_->text() + "\nSunucu Yönetimi için tıklayın");
        if (chip_->property("hostState").toString() != state) {
            chip_->setProperty("hostState", state);
            chip_->style()->unpolish(chip_); chip_->style()->polish(chip_);
        }
    }
};
} // namespace

QDialog* createHostPanel(QWidget* parent, QPushButton* chip, Hooks hooks) { return new Panel(parent, chip, std::move(hooks)); }

bool isSupervisorInvocation(int argc, char** argv) { return argc > 1 && std::strcmp(argv[1], "--host-supervisor") == 0; }

int supervisorMain(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "kullanım: ikv-launcher --host-supervisor DIR --asset-root PATH [--bootstrap-admin NAME]\n"); return 2; }
    const QString dir = QFileInfo(QFile::decodeName(argv[2])).absoluteFilePath();
    QString assets, admin;
    for (int i = 3; i + 1 < argc; i += 2) {
        if (std::strcmp(argv[i], "--asset-root") == 0) assets = QFile::decodeName(argv[i + 1]);
        else if (std::strcmp(argv[i], "--bootstrap-admin") == 0) admin = QFile::decodeName(argv[i + 1]);
        else { std::fprintf(stderr, "bilinmeyen seçenek: %s\n", argv[i]); return 2; }
    }
    if (assets.isEmpty() || !privateDirectoryOk(dir)) { std::fprintf(stderr, "Sunucu klasörü (0700) veya --asset-root geçersiz\n"); return 2; }
    QByteArray password;
    if (!admin.isEmpty()) { // the creation password arrives on stdin, never argv or disk
        char buffer[1024];
        for (ssize_t got; password.size() < 1024 && (got = ::read(STDIN_FILENO, buffer, sizeof buffer)) != 0;) {
            if (got < 0) { if (errno == EINTR) continue; break; }
            password.append(buffer, got);
        }
        std::fill(std::begin(buffer), std::end(buffer), '\0');
        while (password.endsWith('\n') || password.endsWith('\r')) password.chop(1);
        if (password.isEmpty()) { std::fprintf(stderr, "Yönetici parolası stdin'den okunamadı\n"); return 2; }
    }
    const int lock = ::open(QFile::encodeName(dir + "/supervisor.lock").constData(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lock < 0) { std::fprintf(stderr, "supervisor.lock açılamadı\n"); return 1; }
    bool locked = false;
    for (int attempt = 0; attempt < 40 && !(locked = ::flock(lock, LOCK_EX | LOCK_NB) == 0); ++attempt) ::usleep(50000); // a launcher's probe holds LOCK_SH briefly
    if (!locked) { std::fprintf(stderr, "Sunucu zaten çalışıyor\n"); return 3; }
    const QString log = dir + "/server.log";
    if (QFileInfo(log).size() > (4 << 20)) { QFile::remove(log + ".1"); QFile::rename(log, log + ".1"); }
    const int output = ::open(QFile::encodeName(log).constData(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600);
    const int null = ::open("/dev/null", O_RDWR | O_CLOEXEC);
    int ready[2];
    if (output < 0 || null < 0 || ::pipe2(ready, O_CLOEXEC) != 0) { std::fprintf(stderr, "server.log açılamadı\n"); return 1; }
    // Classic daemon: the launcher's child returns once the grandchild has written its pid into the held lock.
    if (const pid_t child = ::fork(); child < 0) { std::fprintf(stderr, "fork başarısız\n"); return 1; }
    else if (child > 0) {
        ::close(ready[1]); char byte = 0;
        while (::read(ready[0], &byte, 1) < 0 && errno == EINTR) {}
        return byte == 1 ? 0 : 1;
    }
    ::close(ready[0]);
    ::setsid();
    if (::fork() != 0) ::_exit(0);
    const auto pid = QByteArray::number(::getpid());
    if (::ftruncate(lock, 0) != 0 || ::pwrite(lock, pid.constData(), static_cast<std::size_t>(pid.size()), 0) != pid.size()) ::_exit(1);
    ::dup2(null, STDIN_FILENO); ::dup2(output, STDOUT_FILENO); ::dup2(output, STDERR_FILENO);
    ::close(null); ::close(output);
    if (::chdir("/") != 0) ::_exit(1);
    ::umask(077);
    std::signal(SIGTERM, requestStop); std::signal(SIGINT, requestStop); std::signal(SIGHUP, SIG_IGN); std::signal(SIGPIPE, SIG_IGN);
    try { saveJson(dir + "/state.json", {{"pid", static_cast<qint64>(::getpid())}, {"phase", "starting"}}); } catch (...) { ::_exit(1); } // never a stale "running"
    const char byte = 1;
    if (::write(ready[1], &byte, 1) != 1) ::_exit(1);
    ::close(ready[1]);
    QCoreApplication app(argc, argv);
    Supervisor supervisor(dir, assets, admin, std::move(password));
    QTimer::singleShot(0, &supervisor, [&supervisor] { supervisor.start(); });
    return app.exec();
}

} // namespace ikv::launcher_server
