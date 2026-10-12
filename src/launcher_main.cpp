#include "ikv/release_defaults.hpp"
#include "ikv/updates.hpp"
#include "ikv/launcher_assets.hpp"
#include "ikv/launcher_extraction.hpp"

#include <QDesktopServices>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <csignal>
#include <cerrno>
#include <QApplication>
#include <QCommandLineParser>
#include <QDeadlineTimer>
#include <QMap>
#include <QMetaObject>
#include <QSslSocket>
#include <QTextDocument>
#include <QThread>
#include <cstring>
#include <vector>
#include <cstdio>
#include <algorithm>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QSettings>
#include <QSpinBox>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>
#include <QBitmap>
#include <QFontDatabase>
#include <QMouseEvent>
#include <QPainter>
#include <QWindow>
#include "ikv/launcher_skin.hpp"
#include "ikv/launcher_server.hpp"
#include <openssl/x509.h>
#include <filesystem>
#include <cmath>
#include <functional>
#include <memory>
#include <stdexcept>

namespace {
using Callback = std::function<void(const QByteArray&)>;
QString failure(const std::exception& e) { return QString::fromUtf8(e.what()); }
std::filesystem::path fs(const QString& p) { return std::filesystem::path(p.toStdU16String()); }
#ifdef _WIN32
// NONCANONICAL: server hosting (launcher_server.cpp: PostgreSQL + ikv-auth/ikv-server supervisor) and the
// AppImage self-update are Linux-only; the Windows launcher joins servers through Sunucu ekle.
constexpr char platformName[] = "Windows", innoextractName[] = "innoextract.exe", innoextractLabel[] = "innoextract.exe",
               innoextractPrompt[] = "innoextract.exe programını seçin (Windows)", innoextractMagic[] = "MZ";
#else
constexpr char platformName[] = "Linux", innoextractName[] = "innoextract", innoextractLabel[] = "innoextract (ELF)",
               innoextractPrompt[] = "Linux innoextract programını seçin (ELF)", innoextractMagic[] = "\x7f" "ELF";
#endif
constexpr char hostingNote[] = "Sunucu barındırma (Sunucu Kur) Linux sürümündedir. Ağdaki bir İKV sunucusuna katılmak için "
                               "Sunucu ekle ile adresini ve CA sertifika dosyasını girin.";
QString dataRoot() { return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation); }
void saveJson(const QString& path, const QJsonObject& value) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(value).toJson()) < 0 || !file.commit())
        throw std::runtime_error("Cannot atomically write settings");
}
template <class Emit>
bool frameProcessLines(QByteArray& pending, const QByteArray& chunk, const QByteArray& secret,
                       bool finished, Emit publishLine) {
    const auto publish = [&] {
        if (pending.endsWith('\r')) pending.chop(1);
        if (!secret.isEmpty()) pending.replace(secret, "[REDACTED]");
        publishLine(QString::fromUtf8(pending));
        pending.clear();
    };
    for (qsizetype offset = 0; offset < chunk.size();) {
        const auto newline = chunk.indexOf('\n', offset);
        const auto size = (newline < 0 ? chunk.size() : newline) - offset;
        if (size > 65536 - pending.size()) {
            pending.fill('\0'); pending.clear();
            return false;
        }
        pending.append(chunk.constData() + offset, size);
        if (newline < 0) break;
        publish();
        offset = newline + 1;
    }
    if (finished && !pending.isEmpty()) publish();
    return true;
}
#ifndef _WIN32
// Startup argv before QApplication strips Qt options; handoff/restart re-exec with exactly these.
std::vector<QByteArray> startArguments;
// ikv-updater and verify_release take a PEM path: an empty trust setting selects the compiled-in
// release key, materialised once per process in a private temporary file (removed before exec).
std::unique_ptr<QTemporaryFile> builtinTrust;
QString trustPem(const QString& configured) {
    if (!configured.isEmpty()) return configured;
    if (!builtinTrust) {
        auto file = std::make_unique<QTemporaryFile>(QDir::tempPath() + "/ikv-release-trust-XXXXXX.pem");
        const QByteArray pem(ikv::release::trust_pem);
        if (!file->open() || file->write(pem) != pem.size() || !file->flush())
            throw std::runtime_error("Yerleşik sürüm anahtarı hazırlanamadı");
        builtinTrust = std::move(file);
    }
    return builtinTrust->fileName();
}
// The signed, hash-verified bin/ikv-linux.AppImage of <root>/current; empty when nothing is installed
// or (onlyNewer) the installed release is not SemVer-newer than this build.
QString installedAppImage(const QString& root, const QString& trust, bool onlyNewer) {
    if (!QFileInfo(root + "/current").isSymLink()) return {};
    const auto release = std::filesystem::canonical(fs(root) / "current");
    const auto version = release.filename().string();
    if (onlyNewer && ikv::updates::compare_versions(version, ikv::release::version) <= 0) return {};
    const auto manifest = ikv::updates::verify_release(fs(root), version, fs(trustPem(trust)));
    if (release != std::filesystem::canonical(fs(root) / "releases" / version))
        throw std::runtime_error("Kurulu sürüm işaretçisi releases/ dışına çıkıyor");
    if (std::none_of(manifest.files.begin(), manifest.files.end(), [](const auto& file) { return file.path == "bin/ikv-linux.AppImage" && file.executable; }))
        throw std::runtime_error("Kurulu " + version + " sürümü bir AppImage (bin/ikv-linux.AppImage) içermiyor");
    return QString::fromStdString((release / "bin/ikv-linux.AppImage").string());
}
// Replace this process with appImage (same argv). IKV_UPDATE_HANDOFF stops a handoff loop even if a
// release's manifest version disagrees with its compiled version. Returns only by throwing.
[[noreturn]] void execInto(const QString& appImage) {
    builtinTrust.reset();
    ::setenv("IKV_UPDATE_HANDOFF", "1", 1);
    const QByteArray path = QFile::encodeName(appImage);
    std::vector<char*> argv{const_cast<char*>(path.constData())};
    for (std::size_t i = 1; i < startArguments.size(); ++i) argv.push_back(startArguments[i].data());
    argv.push_back(nullptr);
    std::fflush(nullptr);
    ::execv(path.constData(), argv.data());
    throw std::runtime_error("Yeni sürüm başlatılamadı (" + path.toStdString() + "): " + std::strerror(errno));
}
// Startup handoff: an installed, verified release newer than this build takes over. Returns the
// Turkish reason when an installed release exists but cannot be used (shown, never ignored).
QString handoff() {
    if (qEnvironmentVariableIsSet("IKV_UPDATE_HANDOFF")) return {};
    try {
        QSettings settings;
        const QString target = installedAppImage(settings.value("installation/root", dataRoot()).toString(),
                                                 settings.value("updates/trust").toString(), true);
        if (!target.isEmpty()) execInto(target);
        return {};
    } catch (const std::exception& e) {
        return "Kurulu güncelleme kullanılamadı; bu sürümle devam ediliyor: " + failure(e);
    }
}
#endif
class Launcher final : public QWidget {
    QSettings settings;
    QNetworkAccessManager network;
    QLineEdit *root{}, *assetSource{}, *extractor{}, *username{}, *password{}, *repo{}, *trust{},
        *updateCa{}, *releaseApi{}, *modelMap{}, *audioPath{}, *soundMap{}, *serverName{}, *serverAddress{}, *serverCa{};
    QComboBox *profiles{}, *renderer{}, *appearance{}, *rayTracing{}, *rtStyle{}, *channel{};
    QComboBox *textureDetail{}, *lightmapDetail{}; // index = original TextureDetail/LightmapDetail pref (0 = *_4.DAT .. 2 = *_1.DAT)
    QSpinBox *width{}, *height{}, *volume{};
    QDoubleSpinBox *renderScale{}, *fsrSharpness{};
    QComboBox *upscaler{}; // data: off (bilinear render scale) or an FSR 3.1.4 preset
    QCheckBox *fullscreen{}, *frameGeneration{}, *hostAfterInstall{};
    QPlainTextEdit *logs{};
    QLabel *status{}, *installNote{}, *updateLabel{};
    QWidget* updateBanner{}; // 'Güncelleme mevcut' + Güncelle / Yeniden başlat
    QPushButton* updateNow{};
    QJsonObject offeredRelease; // newest channel release newer than this build (from the release API)
    QString offeredVersion;
    bool updateInstalled = false;
    QProgressBar *progress{};
    QStackedWidget *pages{};
    QWidget *installPage{}, *loginPage{}, *playPage{};
    QLabel *account{};
    QDialog *settingsDialog{}, *serverDialog{}, *hostDialog{}; // hostDialog: ikv::launcher_server panel
    QByteArray session;
    QProcess *client{};
    bool busy = false;
    bool automation = false;
    int networkTimeoutMs = 60000;
    struct TransferBudget {
        QDeadlineTimer deadline;
        qint64 bytes = 0, limit = 0;
        QUrl origin;
        bool github = false;
        std::function<void(const QString&)> fail; // set: report failures here instead of error()
        bool allows(const QUrl& url) const {
            if (github) return url.port(443) == 443 && (url.host() == "api.github.com" || url.host() == "github.com" || url.host().endsWith(".githubusercontent.com"));
            return url.scheme() == origin.scheme() && url.host() == origin.host() && url.port(443) == origin.port(443);
        }
    };
    QList<QThread*> workers;
    QList<QNetworkReply*> replies;
    QString authSessionOrigin;
    // Original-launcher skin (read at runtime from the user's launcher.exe; null = plain UI).
    QImage skinFrame;
    QString skinTried;
    QWidget *skinActions{}, *skinLeft{};
    QPushButton *skinLogin{}, *skinPlay{};
    QPlainTextEdit* skinLog{};
    // Paints a skin bitmap instead of the button's own look; a lighter copy on hover/press.
    class SkinFace final : public QObject {
        QPixmap normal, lit;
    public:
        SkinFace(QAbstractButton* button, const QImage& image) : QObject(button), normal(QPixmap::fromImage(image)) {
            QImage bright = image.convertToFormat(QImage::Format_RGB32); // scale, so dark artwork edges stay dark
            const auto up = [](int c) { return std::min(255, c * 3 / 2); };
            for (int y = 0; y < bright.height(); ++y) {
                auto* line = reinterpret_cast<QRgb*>(bright.scanLine(y));
                for (int x = 0; x < bright.width(); ++x) line[x] = qRgb(up(qRed(line[x])), up(qGreen(line[x])), up(qBlue(line[x])));
            }
            lit = QPixmap::fromImage(bright);
            button->setAttribute(Qt::WA_Hover); button->setFixedSize(image.size()); button->setCursor(Qt::PointingHandCursor);
            button->setFocusPolicy(Qt::NoFocus); button->installEventFilter(this);
        }
        bool eventFilter(QObject* object, QEvent* event) override {
            if (event->type() != QEvent::Paint) return false;
            auto* button = static_cast<QAbstractButton*>(object);
            QPainter painter(button);
            if (!button->isEnabled()) painter.setOpacity(.45);
            painter.drawPixmap(0, 0, button->isEnabled() && (button->underMouse() || button->isDown()) ? lit : normal);
            return true;
        }
    };
    static QLineEdit* text(QFormLayout* f, const QString& title, const QString& name, const QString& value = {}) {
        auto* w = new QLineEdit(value); w->setObjectName(name); f->addRow(title, w); return w;
    }
    QPushButton* button(QBoxLayout* row, const QString& title, const QString& name, std::function<void()> fn) {
        auto* b = new QPushButton(title); b->setObjectName(name); row->addWidget(b);
        connect(b, &QPushButton::clicked, this, [this, fn = std::move(fn)] { try { fn(); } catch (const std::exception& e) { error(failure(e)); } });
        return b;
    }
    void log(const QString& message) {
        const QString line = QDateTime::currentDateTimeUtc().toString(Qt::ISODate) + " " + message;
        logs->appendPlainText(line);
        if (skinLog) skinLog->appendPlainText(line);
        if (automation) fprintf(stderr, "%s\n", line.toUtf8().constData()); // --update-once: the log is the diagnostic
    }
    void setBusy(bool value) {
        busy = value; pages->setEnabled(!value); settingsDialog->setEnabled(!value);
        if (updateBanner) updateBanner->setEnabled(!value);
        progress->setRange(0, value ? 0 : 1); progress->setVisible(value || !skinFrame.isNull());
        syncSkin();
    }
    void error(const QString& message) {
        setBusy(false); status->setText(message); log(message);
        if (automation) QCoreApplication::exit(1);
        else { auto* owner = QApplication::activeModalWidget(); QMessageBox::warning(owner ? owner : this, "İKV", message); }
    }
    auto attachProcessLog(QProcess* process, const QByteArray& secret = {}, std::function<void(const QString&)> sink = {}) {
        struct Output {
            QByteArray pending[2], secret;
            bool failed = false;
            ~Output() { secret.fill('\0'); }
        };
        auto output = std::make_shared<Output>();
        output->secret = secret;
        process->setProcessChannelMode(QProcess::SeparateChannels);
        const auto consume = [this, process, output, sink](QProcess::ProcessChannel processChannel, bool finished) {
            if (output->failed) return;
            const char* issue = nullptr;
            try {
                process->setReadChannel(processChannel);
                auto& pending = output->pending[processChannel == QProcess::StandardOutput ? 0 : 1];
                const auto publishLine = [this, &sink](const QString& line) { if (sink) sink(line); else log(line); };
                while (process->bytesAvailable() > 0) {
                    const auto chunk = process->read(65536);
                    if (chunk.isEmpty()) break;
                    if (!frameProcessLines(pending, chunk, output->secret, false, publishLine)) {
                        issue = "Process output line exceeds 64 KiB; process terminated";
                        break;
                    }
                }
                if (!issue && finished) frameProcessLines(pending, {}, output->secret, true, publishLine);
            } catch (...) { issue = "Cannot log process output; process terminated"; }
            if (!issue) return;
            output->failed = true;
            for (auto& pending : output->pending) { pending.fill('\0'); pending.clear(); }
            process->kill();
            error(QString::fromLatin1(issue));
        };
        connect(process, &QProcess::readyReadStandardOutput, this, [consume] { consume(QProcess::StandardOutput, false); });
        connect(process, &QProcess::readyReadStandardError, this, [consume] { consume(QProcess::StandardError, false); });
        connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [consume](int, QProcess::ExitStatus) {
                consume(QProcess::StandardOutput, true);
                consume(QProcess::StandardError, true);
            });
        return output;
    }
    void persist() {
        if (renderer->currentText() == "opengl" && renderScale->value() != 1)
            throw std::runtime_error("OpenGL requires render_scale=1; select Vulkan for render scaling");
        if (renderer->currentText() == "opengl" && upscaler->currentData().toString() != "off")
            throw std::runtime_error("OpenGL renders natively; select Vulkan for FSR 3.1.4");
        settings.setValue("installation/root", root->text()); settings.setValue("assets/source", assetSource->text());
        settings.setValue("assets/extractor", extractor->text());
#ifndef _WIN32
        settings.setValue("updates/repo", repo->text());
        settings.setValue("updates/trust", trust->text()); settings.setValue("updates/ca", updateCa->text());
        // release_channel replaces 0.3's updates/channel, which saved an unchosen "stable" default.
        settings.setValue("updates/api", releaseApi->text()); settings.setValue("updates/release_channel", channel->currentText()); settings.remove("updates/channel");
#endif
        if (renderer->currentIndex() >= 0) settings.setValue("graphics/renderer", renderer->currentText());
        if (renderer->currentIndex() >= 0) settings.setValue("graphics/render_scale", renderScale->value());
        if (renderer->currentIndex() >= 0) settings.setValue("graphics/fsr", upscaler->currentData().toString());
        settings.setValue("graphics/fsr_sharpness", fsrSharpness->value());
        settings.setValue("graphics/frame_generation", frameGeneration->isChecked());
        settings.setValue("graphics/appearance", appearance->currentText());
        settings.setValue("graphics/rt", rayTracing->currentText()); settings.setValue("graphics/rt_style", rtStyle->currentText());
        settings.setValue("graphics/texture_detail", textureDetail->currentIndex()); settings.setValue("graphics/lightmap_detail", lightmapDetail->currentIndex());
        settings.setValue("graphics/width", width->value());
        settings.setValue("graphics/height", height->value()); settings.setValue("graphics/fullscreen", fullscreen->isChecked());
        settings.setValue("graphics/volume", volume->value());
        for (const auto* key : {"graphics/sceneMember", "graphics/zone", "graphics/characterModel"}) settings.remove(key);
        for (auto* field : {modelMap, audioPath, soundMap}) settings.setValue("graphics/" + field->objectName(), field->text());
        settings.sync();
    }
    // Server profile: HTTPS account origin plus optional CA PEM (blank = system trust).
    // The world shares the account host; its port comes from /v1/status at Play.
    QString fsrMode() const { return upscaler->currentData().toString(); }
    QJsonObject profile() const { return QJsonDocument::fromJson(settings.value("profiles/" + profiles->currentText()).toByteArray()).object(); }
    QString authUrl() const { return profile().value("auth").toString(); }
    QString serverCertificate() const { return profile().value("ca").toString(); }
    bool hasAssets() const { return QFileInfo(ikv::launcher_assets::manifestPath(root->text())).isFile(); }
    void showPage() {
        applySkin();
        installNote->setText("Kurulum klasörü: " + root->text());
        pages->setCurrentWidget(!hasAssets() ? installPage : session.isEmpty() ? loginPage : playPage);
    }
    void clearSession() { session.fill('\0'); session.clear(); password->clear(); authSessionOrigin.clear(); showPage(); }
    void paintEvent(QPaintEvent* event) override {
        if (skinFrame.isNull()) QWidget::paintEvent(event); else QPainter(this).drawImage(0, 0, skinFrame);
    }
    void mousePressEvent(QMouseEvent* event) override { // frameless skin: drag anywhere on the artwork
        if (!skinFrame.isNull() && event->button() == Qt::LeftButton && windowHandle()) windowHandle()->startSystemMove();
        else QWidget::mousePressEvent(event);
    }
    // OYNA is the current page's primary action; the server box only applies before login.
    void syncSkin() {
        if (skinFrame.isNull()) return;
        const auto* page = pages->currentWidget();
        skinLogin->setVisible(page == loginPage); skinPlay->setVisible(page == playPage);
        skinActions->setEnabled(!busy); skinLeft->setEnabled(!busy && page == loginPage);
    }
    // Once original files are imported, re-present the existing widgets inside the original
    // launcher artwork (launcher.exe RT_BITMAP 139 frame + buttons). Any problem keeps the plain UI.
    void applySkin() {
        const QString source = settings.value("assets/source").toString();
        if (!skinFrame.isNull() || source == skinTried || !hasAssets()) return;
        skinTried = source;
        const QString app = ikv::launcher_skin::originalAppDirectory(source);
        if (app.isEmpty()) return;
        std::map<std::uint32_t, QImage> art;
        try {
            QFile exe(app + "/launcher.exe");
            if (!exe.open(QIODevice::ReadOnly) || exe.size() > 64 * 1024 * 1024) throw std::runtime_error("launcher.exe okunamadı");
            for (const auto& [id, bmp] : ikv::launcher_skin::bitmaps(exe.readAll())) art[id] = QImage::fromData(bmp, "BMP");
        } catch (const std::exception& e) { log("Özgün başlatıcı görünümü kullanılamadı: " + failure(e)); return; }
        const std::pair<std::uint32_t, QSize> needed[] = {{139, {640, 705}}, {141, {136, 49}}, {146, {116, 27}}, {144, {70, 24}}, {143, {26, 26}}, {140, {26, 26}}};
        for (const auto& [id, size] : needed)
            if (art[id].size() != size) { log(QString("Özgün başlatıcı görünümü kullanılamadı: bitmap %1").arg(id)); return; }
        QImage frame = art[139].convertToFormat(QImage::Format_ARGB32);
        const QRgb key = frame.pixel(0, 0); // the artwork's transparency colour (#FF007F)
        for (int y = 0; y < frame.height(); ++y) {
            auto* line = reinterpret_cast<QRgb*>(frame.scanLine(y));
            for (int x = 0; x < frame.width(); ++x) if (line[x] == key) line[x] = 0;
        }
        QString family = "Verdana";
        for (const char* file : {"verdana.ttf", "verdanab.ttf"}) {
            const auto families = QFontDatabase::applicationFontFamilies(QFontDatabase::addApplicationFont(ikv::launcher_assets::assetRoot(root->text()) + "/fonts/" + file));
            if (!families.isEmpty()) family = families.first();
        }

        delete layout(); // widgets stay; they are re-placed into the artwork's panels below
        for (auto* child : findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly)) if (!child->isWindow()) child->hide();
        auto* skin = new QWidget(this); skin->setObjectName("skin"); skin->setGeometry(frame.rect());
        skin->setFont(QFont(family, 8));
        skin->setStyleSheet(
            "QLabel, QCheckBox { color: #e6d7b8; background: transparent; }"
            "QLabel#skinTitle { color: #f4c542; font-weight: bold; } QLabel#skinBody { color: #ff9d2e; }"
            "QLabel:disabled, QCheckBox:disabled { color: #6f6070; }"
            "QLineEdit, QComboBox, QPlainTextEdit { background: #0d050f; color: #f2e8d2; border: 1px solid #6b3a7a; border-radius: 2px; padding: 1px 3px; selection-background-color: #8a2f8f; }"
            "QLineEdit:focus, QComboBox:focus { border-color: #c27ad6; }"
            "QLineEdit:disabled, QComboBox:disabled { color: #6f6070; border-color: #3a2240; }"
            "QComboBox QAbstractItemView { background: #0d050f; color: #f2e8d2; selection-background-color: #8a2f8f; }"
            "QPushButton { color: #e9d27c; background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #5c2468, stop:1 #2a0b31); border: 1px solid #9a5aae; border-radius: 3px; padding: 2px 8px; }"
            "QPushButton:hover { background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #7a3488, stop:1 #3c1046); }"
            "QPushButton:pressed { background: #24082a; } QPushButton:disabled { color: #6f6070; border-color: #3a2240; }"
            "QPushButton:flat { border: none; background: transparent; color: #c79bd6; text-decoration: underline; padding: 0; }"
            "QPushButton#hostButton[hostState=\"running\"] { color: #8ee08e; } QPushButton#hostButton[hostState=\"failed\"] { color: #ff6b5b; }"
            "QProgressBar { background: white; border: none; } QProgressBar::chunk { background: #b0306f; }"
            "QPlainTextEdit { font-size: 7pt; }"
            "QScrollBar:vertical { width: 8px; background: #0d050f; } QScrollBar::handle:vertical { background: #6b3a7a; min-height: 16px; }"
            "QScrollBar::add-line, QScrollBar::sub-line { height: 0; }");
        const auto place = [skin](QWidget* widget, const QRect& where) { widget->setParent(skin); widget->setGeometry(where); widget->show(); };
        const auto box = [&](const QRect& where, const QString& title) {
            auto* w = new QWidget; place(w, where);
            auto* v = new QVBoxLayout(w); v->setContentsMargins(0, 0, 0, 0); v->setSpacing(4);
            auto* heading = new QLabel(title); heading->setObjectName("skinTitle"); v->addWidget(heading);
            return v;
        };
        const auto bitmapButton = [&](const QString& name, const QImage& image, QPoint at, std::function<void()> fn) {
            auto* b = new QPushButton; b->setObjectName(name); new SkinFace(b, image); place(b, QRect(at, image.size()));
            connect(b, &QPushButton::clicked, this, [this, fn = std::move(fn)] { try { fn(); } catch (const std::exception& e) { error(failure(e)); } });
            return b;
        };
        place(pages, {286, 105, 289, 244});
        place(status, {230, 378, 344, 38}); status->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        place(progress, {226, 420, 352, 18}); progress->setVisible(true);
        if (updateBanner) { place(updateBanner, {226, 442, 352, 30}); updateBanner->setVisible(!offeredVersion.isEmpty()); }

        auto* left = box({44, 355, 127, 118}, "Sunucu");
        skinLeft = left->parentWidget();
        auto* form = loginPage->findChild<QFormLayout*>();
        auto* addServer = findChild<QPushButton*>("addServer");
        left->addWidget(profiles); left->addWidget(addServer); left->addStretch();
        form->removeRow(0); // the emptied "Sunucu" row of the login form

        auto* activity = box({64, 558, 229, 112}, "Etkinlik");
        skinLog = new QPlainTextEdit(logs->toPlainText()); skinLog->setObjectName("skinLog"); skinLog->setReadOnly(true);
        skinLog->document()->setMaximumBlockCount(500); activity->addWidget(skinLog, 1);
        skinLog->moveCursor(QTextCursor::End);
        auto* news = box({344, 558, 243, 112}, "Sunucu Kur");
        auto* hostButton = findChild<QPushButton*>("hostButton"); // absent on Windows (hostingNote)
        auto* body = new QLabel(hostButton ? "Bu bilgisayarda kendi özel topluluk sunucunuzu kurun, başlatın ve yönetin." : hostingNote);
        body->setObjectName("skinBody"); body->setWordWrap(true); news->addWidget(body);
        if (hostButton) { news->addWidget(hostButton, 0, Qt::AlignLeft); hostButton->show(); }
        news->addStretch();

        skinActions = new QWidget; place(skinActions, {39, 481, 136, 49});
        skinPlay = findChild<QPushButton*>("playButton"); skinPlay->setParent(skinActions); new SkinFace(skinPlay, art[141]);
        skinLogin = new QPushButton(skinActions); skinLogin->setObjectName("skinLogin"); new SkinFace(skinLogin, art[141]);
        connect(skinLogin, &QPushButton::clicked, findChild<QPushButton*>("loginButton"), &QPushButton::click);
        const QString guide = app + "/ikv_oyun_rehberi.pdf";
        bitmapButton("skinGuide", art[146], {287, 41}, [guide] {
            if (!QFileInfo(guide).isFile() || !QDesktopServices::openUrl(QUrl::fromLocalFile(guide))) throw std::runtime_error("Oyun rehberi açılamadı: " + guide.toStdString());
        });
        bitmapButton("skinSettings", art[144], {429, 41}, [this] { findChild<QPushButton*>("settingsButton")->click(); });
        bitmapButton("skinMinimize", art[143], {518, 40}, [this] { showMinimized(); });
        bitmapButton("skinClose", art[140], {555, 42}, [this] { close(); });
        skin->show();

        skinFrame = frame;
        const bool shown = isVisible();
        setAttribute(Qt::WA_TranslucentBackground); setWindowFlag(Qt::FramelessWindowHint);
        setFixedSize(frame.size()); setMask(QBitmap::fromImage(frame.createAlphaMask()));
        connect(pages, &QStackedWidget::currentChanged, this, [this] { syncSkin(); });
        if (shown) show();
        syncSkin();
        log("Özgün başlatıcı görünümü: " + app + "/launcher.exe");
    }
    QUrl validatedUrl(const QString& input) {
        QUrl url(input);
        if (!url.isValid() || url.scheme() != "https" || url.host().isEmpty() || !url.userInfo().isEmpty() || url.hasFragment())
            throw std::runtime_error("HTTPS URL with host and no credentials/fragment required");
        return url;
    }
    void fetch(const QUrl& url, const QString& certificate, const QByteArray& method, const QByteArray& body,
               bool bearer, qint64 maximum, const QString& destination, Callback done, int redirects = 0,
               std::shared_ptr<TransferBudget> budget = {}) {
        validatedUrl(url.toString());
        if (!budget) {
            budget = std::make_shared<TransferBudget>();
            budget->deadline = QDeadlineTimer(networkTimeoutMs);
            budget->limit = maximum + 6 * 65536;
            budget->origin = url;
        }
        if (!budget->allows(url)) throw std::runtime_error("HTTPS origin refused");
        if (budget->deadline.hasExpired()) throw std::runtime_error("HTTPS operation wall deadline exceeded");
        QSslConfiguration tls = QSslConfiguration::defaultConfiguration();
        tls.setProtocol(QSsl::TlsV1_2OrLater); tls.setPeerVerifyMode(QSslSocket::VerifyPeer);
        if (!certificate.isEmpty()) { // Blank keeps the system trust store of the default configuration.
            QFile caFile(certificate);
            if (!caFile.open(QIODevice::ReadOnly)) throw std::runtime_error("Cannot read configured CA file");
            const auto certs = QSslCertificate::fromData(caFile.readAll());
            if (certs.isEmpty()) throw std::runtime_error("Configured CA contains no PEM certificates");
            tls.setCaCertificates(certs);
        }
        QNetworkRequest request(url); request.setSslConfiguration(tls);
        request.setTransferTimeout(networkTimeoutMs); request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
        request.setRawHeader("User-Agent", "IKV-native-launcher/2"); request.setRawHeader("Accept", "application/json");
        if (!body.isEmpty()) request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        if (bearer) {
            if (session.isEmpty() || authSessionOrigin != authUrl()) throw std::runtime_error("Önce seçili sunucuya giriş yapın");
            request.setRawHeader("Authorization", "Bearer " + session);
        }
        auto* reply = network.sendCustomRequest(request, method, body); replies.append(reply);
        reply->setReadBufferSize(65536);
        struct Download { QByteArray bytes; std::unique_ptr<QSaveFile> file; qint64 size = 0; QString issue; QUrl redirect; QDeadlineTimer deadline; };
        auto state = std::make_shared<Download>();
        // Staged downloads (the ~60 MB update package) also get 1 s per 128 KiB of their signed bound.
        const qint64 wall = networkTimeoutMs + (destination.isEmpty() ? 0 : maximum / 128);
        state->deadline = QDeadlineTimer(wall);
        if (!destination.isEmpty()) {
            state->file = std::make_unique<QSaveFile>(destination);
            if (!state->file->open(QIODevice::WriteOnly)) { reply->abort(); reply->deleteLater(); replies.removeOne(reply); throw std::runtime_error("Cannot stage HTTPS download"); }
        }
        auto drain = [reply, state, budget, maximum] {
            const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const qint64 cap = code >= 200 && code < 300 ? maximum : 65536;
            while (reply->bytesAvailable() > 0 && state->issue.isEmpty()) {
                const auto bytes = reply->read(65536);
                state->size += bytes.size(); budget->bytes += bytes.size();
                if (state->size > cap) state->issue = "HTTPS response exceeds configured bound";
                else if (budget->bytes > budget->limit) state->issue = "HTTPS operation exceeds configured byte bound";
                else if (budget->deadline.hasExpired()) state->issue = "HTTPS operation wall deadline exceeded";
                else if (state->deadline.hasExpired()) state->issue = "HTTPS request wall deadline exceeded";
                if (!state->issue.isEmpty()) { reply->abort(); return; }
                if (code >= 300 && code <= 399) continue;
                if (state->file) {
                    if (state->file->write(bytes) != bytes.size()) { state->issue = "Cannot write staged download"; reply->abort(); return; }
                } else state->bytes.append(bytes);
            }
        };
        auto* timer = new QTimer(reply); timer->setSingleShot(true);
        connect(timer, &QTimer::timeout, this, [reply, state, budget] {
            state->issue = budget->deadline.hasExpired() ? "HTTPS operation wall deadline exceeded" : "HTTPS request wall deadline exceeded";
            reply->abort();
        });
        timer->start(static_cast<int>(std::min<qint64>(wall, std::max<qint64>(1, budget->deadline.remainingTime()))));
        connect(reply, &QNetworkReply::readyRead, this, drain);
        connect(reply, &QNetworkReply::metaDataChanged, this, [this, reply, state, budget, maximum, method, bearer, redirects, drain] {
            const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const qint64 cap = code >= 200 && code < 300 ? maximum : 65536;
            const QVariant length = reply->header(QNetworkRequest::ContentLengthHeader);
            if (length.isValid() && length.toLongLong() > cap) { state->issue = "HTTPS response exceeds configured bound"; reply->abort(); return; }
            if (code < 300 || code > 399 || !state->redirect.isEmpty() || !state->issue.isEmpty()) return;
            try {
                if (bearer || method != "GET" || redirects >= 5) throw std::runtime_error("HTTPS redirect refused");
                const auto location = reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
                if (location.isEmpty()) throw std::runtime_error("HTTPS redirect refused");
                const auto target = validatedUrl(reply->url().resolved(location).toString());
                if (!budget->allows(target)) throw std::runtime_error("HTTPS origin refused");
                state->redirect = target;
                drain();
            } catch (const std::exception& e) { state->issue = failure(e); }
            reply->abort(); // Never wait for a redirect/error body to finish before enforcing policy.
        });
        connect(reply, &QNetworkReply::finished, this, [this, reply, timer, state, budget, drain, done = std::move(done), certificate, method, body, bearer, maximum, destination, redirects] {
            timer->stop();
            if (state->issue.isEmpty() && budget->deadline.hasExpired()) state->issue = "HTTPS operation wall deadline exceeded";
            else if (state->issue.isEmpty() && state->deadline.hasExpired()) state->issue = "HTTPS request wall deadline exceeded";
            drain(); replies.removeOne(reply);
            const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (!state->redirect.isEmpty() && state->issue.isEmpty()) {
                reply->deleteLater(); state->file.reset();
                try { fetch(state->redirect, certificate, method, body, false, maximum, destination, done, redirects + 1, budget); }
                catch (const std::exception& e) { budget->fail ? budget->fail(failure(e)) : error(failure(e)); }
                return;
            }
            QString issue = state->issue;
            if (issue.isEmpty() && (reply->error() != QNetworkReply::NoError || code < 200 || code >= 300)) {
                const auto object = QJsonDocument::fromJson(state->bytes).object();
                issue = object.value("error").toObject().value("message").toString();
                if (issue.isEmpty()) issue = QString("Sunucu isteği reddetti (%1): %2").arg(code).arg(reply->errorString());
            }
            if (issue.isEmpty() && state->file && !state->file->commit()) issue = "Cannot commit staged HTTPS file";
            reply->deleteLater();
            if (!issue.isEmpty()) { budget->fail ? budget->fail(issue) : error(issue); return; }
            try { done(state->bytes); } catch (const std::exception& e) { budget->fail ? budget->fail(failure(e)) : error(failure(e)); }
        });
    }
    void api(const QString& route, const QByteArray& method, const QJsonObject& body, bool bearer, Callback done) {
        const auto origin = authUrl(), certificate = serverCertificate();
        const QUrl base = validatedUrl(origin);
        if (base.hasQuery() || (base.path() != "" && base.path() != "/")) throw std::runtime_error("Auth endpoint must be an HTTPS origin (no path/query)");
        fetch(base.resolved(QUrl(route)), certificate, method, body.isEmpty() ? QByteArray() : QJsonDocument(body).toJson(QJsonDocument::Compact), bearer, 2 * 1024 * 1024, {}, [this, origin, certificate, done = std::move(done)](const QByteArray& bytes) {
            if (authUrl() != origin || serverCertificate() != certificate) throw std::runtime_error("Server profile changed; stale response discarded");
            done(bytes);
        });
    }
#ifndef _WIN32
    void runHelper(const QStringList& arguments, std::function<void()> done) {
        if (busy || !replies.isEmpty()) throw std::runtime_error("Another installation/update or HTTPS request is active");
        QString helper = QCoreApplication::applicationDirPath() + "/ikv-updater";
        if (!QFileInfo(helper).isExecutable()) throw std::runtime_error("Native signed updater helper missing beside launcher");
        setBusy(true); auto* p = new QProcess(this); p->setProgram(helper); p->setArguments(arguments);
        const auto output = attachProcessLog(p);
        connect(p, &QProcess::errorOccurred, this, [this, p](QProcess::ProcessError e) { if (e == QProcess::FailedToStart) { busy = false; error(p->errorString()); p->deleteLater(); } });
        connect(p, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [this, p, output, done = std::move(done)](int code, QProcess::ExitStatus exit) {
            setBusy(false); p->deleteLater(); if (output->failed) return;
            if (code || exit != QProcess::NormalExit) { error("İmzalı güncelleyici başarısız oldu; önceki sürüm korundu (ayrıntılar tanılama kaydında)"); return; }
            status->setText("Kurulum tamamlandı");
            try { done(); } catch (const std::exception& e) { error(failure(e)); }
        });
        p->start();
    }
    void installBundle() {
        persist();
        const QString key = trustPem(trust->text());
        const QString manifest = QFileDialog::getOpenFileName(this, "Signed manifest", {}, "JSON (*.json)"); if (manifest.isEmpty()) return;
        const QString signature = QFileDialog::getOpenFileName(this, "Detached signature", QFileInfo(manifest).absolutePath()); if (signature.isEmpty()) return;
        const auto m = ikv::updates::verify_manifest(fs(manifest), fs(signature), fs(key));
        const QString package = QFileDialog::getOpenFileName(this, "Package: " + QString::fromStdString(m.package), QFileInfo(manifest).absolutePath()); if (package.isEmpty()) return;
        runHelper({"install", "--manifest", manifest, "--signature", signature, "--package", package, "--trust", key, "--root", root->text()},
                  [this] { status->setText("Kurulum tamamlandı; yeni sürüm başlatıcının bir sonraki açılışında kullanılır"); });
    }
    QString updateChannel() const { return channel->currentText(); }
    // Release API: explicit HTTPS override, else GitHub Releases of the configured (default compiled-in) repository.
    QUrl releaseEndpoint() {
        if (!releaseApi->text().isEmpty()) return validatedUrl(releaseApi->text());
        const QString repository = repo->text().isEmpty() ? QString(ikv::release::repository) : repo->text();
        const auto parts = repository.split('/');
        if (parts.size() != 2 || parts[0].isEmpty() || parts[1].isEmpty()) throw std::runtime_error("Depo OWNER/REPOSITORY biçiminde olmalı");
        for (const auto& part : parts) for (const QChar c : part) if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')) throw std::runtime_error("Geçersiz depo adı");
        return QUrl("https://api.github.com/repos/" + repository + "/releases");
    }
    std::shared_ptr<TransferBudget> releaseBudget(const QUrl& endpoint) {
        auto budget = std::make_shared<TransferBudget>();
        budget->deadline = QDeadlineTimer(3 * networkTimeoutMs); budget->limit = 7 * 1024 * 1024;
        budget->origin = endpoint; budget->github = releaseApi->text().isEmpty();
        return budget;
    }
    // Ask the release API for the newest channel release (highest vX.Y.Z tag; drafts and other tags
    // ignored) and offer it when SemVer-newer than this build. Background (startup) failures go to the
    // status line and log instead of a dialog; the launcher stays usable either way.
    void checkUpdates(bool background) {
        if (busy || !replies.isEmpty()) throw std::runtime_error("Başka bir kurulum, güncelleme veya HTTPS isteği sürüyor");
        const QUrl endpoint = releaseEndpoint();
        auto budget = releaseBudget(endpoint);
        if (background) budget->fail = [this](const QString& issue) { status->setText("Güncellemeler denetlenemedi: " + issue); log(status->text()); };
        status->setText("Güncellemeler denetleniyor…");
        fetch(endpoint, updateCa->text(), "GET", {}, false, 4 * 1024 * 1024, {}, [this](const QByteArray& bytes) {
            QJsonParseError parse; const auto doc = QJsonDocument::fromJson(bytes, &parse);
            if (parse.error != QJsonParseError::NoError || !doc.isArray()) throw std::runtime_error("Sürüm API'si GitHub sürüm dizisi döndürmedi");
            QJsonObject newest; QString newestVersion;
            for (const auto& value : doc.array()) {
                const auto release = value.toObject();
                const QString tag = release.value("tag_name").toString();
                if (release.value("draft").toBool() || release.value("prerelease").toBool() != (updateChannel() == "experimental") || !tag.startsWith('v')) continue;
                const auto version = tag.mid(1).toStdString(); // other tags than vX.Y.Z[-PRE] are not releases
                const auto newer = [&] { return newestVersion.isEmpty() ? ikv::updates::compare_versions(version, version) == 0
                                                                        : ikv::updates::compare_versions(version, newestVersion.toStdString()) > 0; };
                try { if (!newer()) continue; } catch (const std::exception&) { continue; }
                newest = release; newestVersion = QString::fromStdString(version);
            }
            if (newestVersion.isEmpty()) throw std::runtime_error("Seçili kanalda (" + updateChannel().toStdString() + ") sürüm bulunamadı");
            if (ikv::updates::compare_versions(newestVersion.toStdString(), ikv::release::version) <= 0) {
                status->setText(QString("Başlatıcı güncel (v%1)").arg(ikv::release::version)); log(status->text());
                if (automation) { std::printf("ikv-launcher %s: güncel (en yeni v%s)\n", ikv::release::version, qPrintable(newestVersion)); QCoreApplication::exit(0); }
                return;
            }
            offeredRelease = newest; offeredVersion = newestVersion; updateInstalled = false;
            updateLabel->setText("Güncelleme mevcut: v" + newestVersion); updateNow->setText("Güncelle"); updateBanner->show();
            status->setText(updateLabel->text()); log(updateLabel->text() + QString(" (çalışan v%1)").arg(ikv::release::version));
            if (automation) updateNow->click(); // --update-once drives the same button
        }, 0, budget);
    }
    // Güncelle: download the offered release's signed manifest, signature and package, verify, install
    // through ikv-updater into the install root (assets/settings untouched), then restart into it.
    void installUpdate() {
        persist(); if (busy || !replies.isEmpty()) throw std::runtime_error("Başka bir kurulum, güncelleme veya HTTPS isteği sürüyor");
        if (offeredVersion.isEmpty()) throw std::runtime_error("Önce güncellemeleri denetleyin");
        const QString version = offeredVersion, key = trustPem(trust->text());
        auto temp = std::make_shared<QTemporaryDir>(); if (!temp->isValid()) throw std::runtime_error("Güncelleme hazırlama klasörü oluşturulamadı");
        auto budget = releaseBudget(releaseEndpoint());
        budget->fail = [this](const QString& issue) { error("Güncelleme başarısız: " + issue); };
        auto urls = std::make_shared<QMap<QString, QUrl>>();
        for (const auto& value : offeredRelease.value("assets").toArray()) { const auto a = value.toObject(); urls->insert(a.value("name").toString(), validatedUrl(a.value("browser_download_url").toString())); }
        if (!urls->contains("manifest.json") || !urls->contains("manifest.sig")) throw std::runtime_error("Sürümde imzalı manifest (manifest.json/manifest.sig) yok");
        const QString manifest = temp->filePath("manifest.json"), sig = temp->filePath("manifest.sig");
        setBusy(true); status->setText("v" + version + " indiriliyor…");
        fetch(urls->value("manifest.json"), updateCa->text(), "GET", {}, false, 2 * 1024 * 1024, manifest, [this, temp, urls, manifest, sig, budget, version, key](const QByteArray&) {
            fetch(urls->value("manifest.sig"), updateCa->text(), "GET", {}, false, 64, sig, [this, temp, urls, manifest, sig, budget, version, key](const QByteArray&) {
                const auto m = ikv::updates::verify_manifest(fs(manifest), fs(sig), fs(key));
                if (QString::fromStdString(m.channel) != updateChannel()) throw std::runtime_error("İmzalı manifest kanalı uyuşmuyor");
                if (QString::fromStdString(m.version) != version) throw std::runtime_error("İmzalı manifest sürümü (" + m.version + ") sürüm etiketiyle uyuşmuyor");
                budget->limit = static_cast<qint64>(m.package_size) + 7 * 1024 * 1024;
                budget->deadline = QDeadlineTimer(3 * networkTimeoutMs + static_cast<qint64>(m.package_size / 128));
                const QString name = QString::fromStdString(m.package); if (!urls->contains(name)) throw std::runtime_error("İmzalı paket sürüm dosyaları arasında yok");
                const QString package = temp->filePath(name);
                fetch(urls->value(name), updateCa->text(), "GET", {}, false, static_cast<qint64>(m.package_size), package, [this, temp, manifest, sig, package, version, key](const QByteArray&) {
                    setBusy(false);
                    runHelper({"install", "--manifest", manifest, "--signature", sig, "--package", package, "--trust", key, "--root", root->text()}, [this, temp, version] {
                        updateInstalled = true;
                        updateLabel->setText("v" + version + " kuruldu"); updateNow->setText("Yeniden başlat");
                        log(updateLabel->text() + "; yeni sürüm başlatılıyor");
                        if (automation) std::printf("ikv-launcher %s: v%s kuruldu, yeniden başlatılıyor\n", ikv::release::version, qPrintable(version));
                        restartIntoInstalled();
                    });
                }, 0, budget);
            }, 0, budget);
        }, 0, budget);
    }
    // Exec the verified AppImage of <install-root>/current (after Güncelle or Önceki sürüme dön).
    void restartIntoInstalled() {
        if (client && client->state() != QProcess::NotRunning) throw std::runtime_error("Yeniden başlatmak için önce oyunu kapatın");
        const QString target = installedAppImage(root->text(), trust->text(), false);
        if (target.isEmpty()) throw std::runtime_error("Kurulu sürüm bulunamadı: " + root->text().toStdString() + "/current");
        settings.sync();
        execInto(target);
    }
#endif
    void play() {
#if !IKV_HAS_VULKAN
        if (renderer->currentIndex() < 0)
            throw std::runtime_error("Stored renderer is unavailable in this OpenGL-only build; explicitly select opengl in Ayarlar → Grafik");
#endif
        if (renderer->currentText() == "opengl" && rayTracing->currentText() == "required")
            throw std::runtime_error("Required hardware ray tracing is incompatible with OpenGL; select off or auto");
        persist();
        if (client && client->state() != QProcess::NotRunning) throw std::runtime_error("Client is already running");
        if (session.isEmpty() || authSessionOrigin != authUrl()) throw std::runtime_error("Önce seçili sunucuya giriş yapın");
        const QUrl origin = validatedUrl(authUrl());
        const QString host = origin.host();
        const QString assets = root->text() + "/assets";
        if (!QFileInfo(assets).isDir()) throw std::runtime_error("Import legitimate original assets first");
        // OYNA runs this launcher's own client: after a startup handoff that is the newest installed release.
        const QString executable = QStandardPaths::findExecutable("ikv-client", {QCoreApplication::applicationDirPath()});
        if (executable.isEmpty()) throw std::runtime_error("ikv-client başlatıcının yanında bulunamadı: " + QCoreApplication::applicationDirPath().toStdString() + "/ikv-client");
        QString certificate = serverCertificate();
        if (certificate.isEmpty()) {
            certificate = qEnvironmentVariable(X509_get_default_cert_file_env(), QString::fromLocal8Bit(X509_get_default_cert_file()));
            if (!QFileInfo(certificate).isFile()) throw std::runtime_error("System CA bundle not found; set the server CA file with Sunucu ekle");
        }
        const QString graphics = root->text() + "/settings/graphics.json";
        QJsonObject savedGraphics;
        if (QFileInfo::exists(graphics)) {
            QFile existing(graphics);
            if (!existing.open(QIODevice::ReadOnly) || existing.size() > 65536) throw std::runtime_error("Cannot safely read preserved client settings");
            QJsonParseError parse; const auto document = QJsonDocument::fromJson(existing.readAll(), &parse);
            if (parse.error != QJsonParseError::NoError || !document.isObject()) throw std::runtime_error("Existing client settings are invalid; not overwritten");
            savedGraphics = document.object();
        }
        for (const auto* key : {"scene", "scene_member", "zone", "character_model", "model_mapping"}) savedGraphics.remove(key);
        const QJsonObject chosenGraphics{{"width", width->value()}, {"height", height->value()}, {"fullscreen", fullscreen->isChecked()}, {"volume", volume->value() / 100.0}, {"renderer", renderer->currentText()}, {"appearance", appearance->currentText()}, {"rt", rayTracing->currentText()}, {"rt_style", rtStyle->currentText()}, {"render_scale", renderScale->value()}, {"upscaler", fsrMode() == "off" ? "bilinear" : "fsr3"}, {"fsr_sharpness", fsrSharpness->value()}, {"frame_generation", frameGeneration->isChecked()}, {"texture_detail", textureDetail->currentIndex()}, {"lightmap_detail", lightmapDetail->currentIndex()}};
        for (auto it = chosenGraphics.begin(); it != chosenGraphics.end(); ++it) savedGraphics.insert(it.key(), it.value());
        if (fsrMode() != "off") savedGraphics.insert("fsr_mode", fsrMode());
        saveJson(graphics, savedGraphics);
        // The client selects/creates the character itself over the same HTTPS account API (docs/LAUNCHER_UPDATES.md).
        QStringList args{"--ca", certificate, "--session-stdin", "--auth", origin.toString(QUrl::StripTrailingSlash), "--asset-root", assets, "--settings", graphics, "--renderer", renderer->currentText(), "--appearance", appearance->currentText(), "--rt", rayTracing->currentText(), "--rt-style", rtStyle->currentText()};
        args << "--render-scale" << QString::number(renderScale->value(), 'g', 17);
        args << "--upscaler" << (fsrMode() == "off" ? "bilinear" : "fsr3") << "--fsr-sharpness" << QString::number(fsrSharpness->value(), 'g', 17);
        args << "--frame-generation" << (frameGeneration->isChecked() ? "on" : "off");
        if (fsrMode() != "off") args << "--fsr-mode" << fsrMode();
        if (!modelMap->text().isEmpty()) args << "--model-map" << modelMap->text();
        if (!soundMap->text().isEmpty()) args << "--sound-map" << soundMap->text();
        if (!audioPath->text().isEmpty()) {
            const auto relative = fs(audioPath->text());
            if (relative.is_absolute() || std::any_of(relative.begin(), relative.end(), [](const auto& p) { return p == ".."; })) throw std::runtime_error("Original audio path must stay inside imported assets");
            args << "--audio" << QString::fromStdU16String((fs(assets) / relative).u16string());
        }
        status->setText("Sunucu denetleniyor…");
        api("/v1/status", "GET", {}, false, [this, executable, host, args](const QByteArray& bytes) {
            const auto state = QJsonDocument::fromJson(bytes).object();
            if (!state.value("online").toBool()) throw std::runtime_error("Dünya sunucusu çevrimdışı");
            const int port = state.value("world").toObject().value("world_port").toInt(7000);
            if (port < 1 || port > 65535) throw std::runtime_error("Server reported an invalid world port");
            if (client && client->state() != QProcess::NotRunning) throw std::runtime_error("Client is already running");
            if (session.isEmpty()) throw std::runtime_error("Önce seçili sunucuya giriş yapın");
            launch(executable, QStringList{"--host", host, "--port", QString::number(port)} + args);
        });
    }
    void launch(const QString& executable, const QStringList& args) {
        if (client) client->deleteLater();
        client = new QProcess(this); client->setProgram(executable); client->setArguments(args);
        auto* process = client;
        const auto output = attachProcessLog(process, session);
        connect(process, &QProcess::started, this, [this, process] { process->write(session + '\n'); process->closeWriteChannel(); status->setText("İstemci başlatıldı"); });
        connect(process, &QProcess::errorOccurred, this, [this, process, output](QProcess::ProcessError) { if (!output->failed) error(process->errorString()); });
        connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [this, output](int code, QProcess::ExitStatus) {
            log(QString("Client exit %1").arg(code)); if (!output->failed) status->setText("İstemci kapandı");
        });
        client->start();
    }
    void assetTask(std::function<ikv::launcher_assets::Report()> work, std::function<void()> onSuccess = {}) {
        if (busy || !replies.isEmpty()) throw std::runtime_error("Another installation/update or HTTPS request is active");
        setBusy(true); status->setText("Oyun dosyaları işleniyor…");
        auto* thread = QThread::create([this, work = std::move(work), onSuccess = std::move(onSuccess)] {
            QString issue; ikv::launcher_assets::Report report;
            try { report = work(); } catch (const std::exception& e) { issue = failure(e); }
            QMetaObject::invokeMethod(this, [this, report, issue, onSuccess] {
                setBusy(false);
                if (!issue.isEmpty()) { error(issue); return; }
                log(QString("Asset files=%1 bytes=%2 missing=%3 changed=%4 extra=%5").arg(report.files).arg(report.bytes).arg(report.missing.size()).arg(report.changed.size()).arg(report.extra.size()));
                for (const auto& list : {report.errors, report.missing, report.changed, report.extra}) for (const auto& line : list) log(line);
                if (!report.ok()) { error("Varlık doğrulaması başarısız; tanılama ayrıntılarını inceleyin"); return; }
                if (onSuccess) onSuccess();
                status->setText("Varlıklar doğrulandı / Assets verified");
                showPage();
            }, Qt::QueuedConnection);
        });
        thread->setParent(this);
        workers.append(thread);
        connect(thread, &QThread::finished, this, [this, thread] { workers.removeOne(thread); thread->deleteLater(); });
        thread->start();
    }
    // First native innoextract among: the configured path, beside the launcher, the source tree's .tools, PATH.
    QString innoextract() {
        const QString bundled = "/.tools/innoextract-1.9-linux/bin/amd64/innoextract", dir = QCoreApplication::applicationDirPath();
        const QByteArray magic(innoextractMagic);
        for (const QString& candidate : {extractor->text(), dir + "/" + innoextractName, dir + "/.." + bundled, QDir::currentPath() + bundled, QStandardPaths::findExecutable("innoextract")}) {
            QFile file(candidate);
            if (QFileInfo(candidate).isFile() && QFileInfo(candidate).isExecutable() && file.open(QIODevice::ReadOnly) && file.read(magic.size()) == magic) return candidate;
        }
        const QString chosen = QFileDialog::getOpenFileName(this, innoextractPrompt);
        if (!chosen.isEmpty()) { extractor->setText(chosen); persist(); }
        return chosen;
    }
    void importAssets(bool installer) {
        persist();
        if (busy || !replies.isEmpty()) throw std::runtime_error("Another installation/update or HTTPS request is active");
        if (!installer) {
            const auto source = QFileDialog::getExistingDirectory(this, "Mevcut özgün oyun kurulum klasörü", assetSource->text()); if (source.isEmpty()) return;
            assetSource->setText(source); persist(); const auto target = root->text();
            assetTask([source, target] { return ikv::launcher_assets::importAssets(source, target); }, [this] { offerHost(); }); return;
        }
        const auto source = QFileDialog::getOpenFileName(this, "istanbulkur6_0.exe kurulum dosyası (çalıştırılmaz)", {}, "Inno (*.exe)"); if (source.isEmpty()) return;
        const auto nativeExtractor = innoextract(); if (nativeExtractor.isEmpty()) return;
        const auto target = root->text();
        auto result = std::make_shared<ikv::launcher_extraction::Result>();
        assetTask([result, source, target, nativeExtractor] {
            *result = ikv::launcher_extraction::extractAndImport(source, target, nativeExtractor);
            return result->report;
        }, [this, result, source] {
            assetSource->setText(result->sourcePath); settings.setValue("assets/installer", source); persist(); offerHost();
        });
    }
    void verifyAssets(bool repair) {
        persist(); const auto target = root->text();
        if (repair) {
            const auto source = QFileDialog::getExistingDirectory(this, "Select preserved source for hash-checked repair", assetSource->text()); if (source.isEmpty()) return;
            assetSource->setText(source); persist(); assetTask([source, target] { return ikv::launcher_assets::repairAssets(source, target); });
        } else assetTask([target] { return ikv::launcher_assets::verifyAssets(target); });
    }
    void saveServer() {
        const QString name = serverName->text().trimmed();
        if (name.isEmpty() || name.contains('/')) throw std::runtime_error("Sunucu adı boş olamaz ve / içeremez");
        QString address = serverAddress->text().trimmed();
        if (!address.contains("://")) address.prepend("https://");
        const QUrl parsed(address, QUrl::StrictMode);
        if (!parsed.isValid() || parsed.scheme() != "https" || parsed.host().isEmpty() || !parsed.userInfo().isEmpty() ||
            (!parsed.path().isEmpty() && parsed.path() != "/") || parsed.hasQuery() || parsed.hasFragment())
            throw std::runtime_error("Adres: sunucu veya sunucu:port (hesap servisi, varsayılan 7443)");
        QUrl auth; auth.setScheme("https"); auth.setHost(parsed.host()); auth.setPort(parsed.port(7443));
        const QString ca = serverCa->text().trimmed();
        if (!ca.isEmpty() && !QFileInfo(ca).isFile()) throw std::runtime_error("CA sertifika dosyası bulunamadı");
        settings.setValue("profiles/" + name, QJsonDocument(QJsonObject{{"auth", auth.toString()}, {"ca", ca}}).toJson(QJsonDocument::Compact));
        if (profiles->findText(name) < 0) profiles->addItem(name);
        QStringList names; for (int i = 0; i < profiles->count(); ++i) names << profiles->itemText(i);
        settings.setValue("profileNames", names); settings.sync();
        serverDialog->accept();
        if (profiles->currentText() == name) clearSession(); else profiles->setCurrentText(name);
    }
    void buildSettings() {
        settingsDialog = new QDialog(this); settingsDialog->setObjectName("settingsDialog"); settingsDialog->setWindowTitle("Ayarlar");
        settingsDialog->resize(600, 540);
        auto* sv = new QVBoxLayout(settingsDialog); auto* tabs = new QTabWidget; tabs->setObjectName("settingsTabs"); sv->addWidget(tabs);
        auto* graphics = new QWidget; auto* gf = new QFormLayout(graphics);
        renderer = new QComboBox; renderer->setObjectName("renderer"); renderer->addItem("opengl");
#if IKV_HAS_VULKAN
        renderer->addItem("vulkan");
        renderer->setCurrentText(settings.value("graphics/renderer", "opengl").toString());
#else
        renderer->setPlaceholderText("Kayıtlı motor bu derlemede yok; opengl seçin");
        renderer->setCurrentIndex(renderer->findText(settings.value("graphics/renderer", "opengl").toString()));
#endif
        gf->addRow("Görüntü motoru", renderer);
        bool scaleOk = false;
        const double savedScale = settings.value("graphics/render_scale", 1).toDouble(&scaleOk);
        if (!scaleOk || !std::isfinite(savedScale) || savedScale < .25 || savedScale > 1 ||
            (renderer->currentText() == "opengl" && savedScale != 1))
            throw std::runtime_error("Invalid saved render_scale: finite [.25,1] required; OpenGL requires 1");
        renderScale = new QDoubleSpinBox; renderScale->setObjectName("renderScale"); renderScale->setDecimals(2);
        renderScale->setRange(.25, 1); renderScale->setSingleStep(.05); renderScale->setValue(savedScale);
        renderScale->setEnabled(renderer->currentText() == "vulkan");
        renderScale->setToolTip("Vulkan scene resolution scale; window and HUD remain full resolution. OpenGL is fixed at 1.");
        gf->addRow("Çizim ölçeği (Vulkan)", renderScale);
        connect(renderer, &QComboBox::currentTextChanged, this, [this](const QString& provider) {
            renderScale->setEnabled(provider == "vulkan");
            if (provider == "opengl") renderScale->setValue(1);
        });
        // AMD FSR 3.1.4 (Vulkan): the preset replaces the render scale; OpenGL always renders natively.
        upscaler = new QComboBox; upscaler->setObjectName("upscaler");
        for (const auto& [label, mode] : {std::pair{"Kapalı (çizim ölçeği)", "off"}, std::pair{"FSR 3.1.4 Doğal AA", "native"},
                 std::pair{"FSR 3.1.4 Kalite", "quality"}, std::pair{"FSR 3.1.4 Dengeli", "balanced"},
                 std::pair{"FSR 3.1.4 Performans", "performance"}, std::pair{"FSR 3.1.4 Ultra Performans", "ultra_performance"}})
            upscaler->addItem(QString::fromUtf8(label), QString(mode));
        const int savedFsr = upscaler->findData(settings.value("graphics/fsr", "off").toString());
        bool sharpOk = false;
        const double savedSharpness = settings.value("graphics/fsr_sharpness", .5).toDouble(&sharpOk);
        if (savedFsr < 0 || !sharpOk || !std::isfinite(savedSharpness) || savedSharpness < 0 || savedSharpness > 1 ||
            (renderer->currentText() == "opengl" && savedFsr != 0))
            throw std::runtime_error("Invalid saved FSR settings: preset name and sharpness [0,1] required; OpenGL requires off");
        upscaler->setCurrentIndex(savedFsr);
        upscaler->setToolTip("AMD FidelityFX Super Resolution 3.1.4 temporal upscaling (Vulkan); replaces the render scale.");
        gf->addRow("Ölçekleme (Vulkan)", upscaler);
        fsrSharpness = new QDoubleSpinBox; fsrSharpness->setObjectName("fsrSharpness"); fsrSharpness->setDecimals(2);
        fsrSharpness->setRange(0, 1); fsrSharpness->setSingleStep(.05); fsrSharpness->setValue(savedSharpness);
        gf->addRow("FSR keskinlik", fsrSharpness);
        frameGeneration = new QCheckBox; frameGeneration->setObjectName("frameGeneration");
        frameGeneration->setChecked(settings.value("graphics/frame_generation", false).toBool());
        frameGeneration->setToolTip("AMD FSR 3.1.4 frame generation (Vulkan, requires an FSR mode): one generated frame between rendered frames, paced by IKV. Adds about half a frame of latency.");
        gf->addRow("Kare üretimi (FSR 3.1.4)", frameGeneration);
        const auto syncUpscaler = [this] {
            const bool vulkan = renderer->currentText() == "vulkan";
            if (!vulkan) upscaler->setCurrentIndex(0);
            upscaler->setEnabled(vulkan); fsrSharpness->setEnabled(vulkan && fsrMode() != "off");
            renderScale->setEnabled(vulkan && fsrMode() == "off");
            frameGeneration->setEnabled(vulkan && fsrMode() != "off");
            if (!frameGeneration->isEnabled()) frameGeneration->setChecked(false);
        };
        syncUpscaler();
        connect(renderer, &QComboBox::currentTextChanged, this, syncUpscaler);
        connect(upscaler, &QComboBox::currentIndexChanged, this, syncUpscaler);
        appearance = new QComboBox; appearance->setObjectName("appearance"); appearance->addItems({"original", "enhanced"}); appearance->setCurrentText(settings.value("graphics/appearance", "original").toString()); gf->addRow("Görünüm", appearance);
        rayTracing = new QComboBox; rayTracing->setObjectName("rayTracing"); rayTracing->addItems({"off", "auto", "required"}); rayTracing->setCurrentText(settings.value("graphics/rt", "off").toString()); gf->addRow("Işın izleme (donanıma bağlı)", rayTracing);
        // faithful: the original lighting with RT actor shadows (any appearance); physical: enhanced GGX/GI/reflections.
        const QString savedRtStyle = settings.value("graphics/rt_style", "faithful").toString();
        if (savedRtStyle != "faithful" && savedRtStyle != "physical")
            throw std::runtime_error("Invalid saved rt_style: faithful or physical required");
        rtStyle = new QComboBox; rtStyle->setObjectName("rtStyle"); rtStyle->addItems({"faithful", "physical"}); rtStyle->setCurrentText(savedRtStyle); gf->addRow("Işın izleme ışığı", rtStyle);
        // Original launcher "Doku Detay" / "Gölge Detay" (00483d50 writes TextureDetail/LightmapDetail; 004619d0 opens
        // *_4/_2/_1.dat). Defaults are its >=1700 MB RAM tier (0x483330): texture 2, lightmap 1.
        const auto detailCombo = [&](const char* name, const char* key, int fallback, const char* label) {
            bool ok = false;
            const int saved = settings.value(key, fallback).toInt(&ok);
            if (!ok || saved < 0 || saved > 2) throw std::runtime_error(std::string("Invalid saved ") + key + ": 0..2 required");
            auto* combo = new QComboBox; combo->setObjectName(name);
            combo->addItems({"Düşük (_4.DAT)", "Orta (_2.DAT)", "Yüksek (_1.DAT)"}); combo->setCurrentIndex(saved);
            gf->addRow(label, combo);
            return combo;
        };
        textureDetail = detailCombo("textureDetail", "graphics/texture_detail", 2, "Doku detay");
        lightmapDetail = detailCombo("lightmapDetail", "graphics/lightmap_detail", 1, "Gölge detay");
        auto* resolution = new QHBoxLayout;
        width = new QSpinBox; width->setObjectName("width"); width->setRange(640, 3840); width->setValue(settings.value("graphics/width", 1280).toInt());
        height = new QSpinBox; height->setObjectName("height"); height->setRange(480, 2160); height->setValue(settings.value("graphics/height", 720).toInt());
        resolution->addWidget(width); resolution->addWidget(new QLabel("×")); resolution->addWidget(height); resolution->addStretch();
        gf->addRow("Çözünürlük", resolution);
        fullscreen = new QCheckBox; fullscreen->setObjectName("fullscreen"); fullscreen->setChecked(settings.value("graphics/fullscreen", false).toBool()); gf->addRow("Tam ekran", fullscreen);
        volume = new QSpinBox; volume->setObjectName("volume"); volume->setRange(0, 100); volume->setSuffix(" %"); volume->setValue(settings.value("graphics/volume", 80).toInt()); gf->addRow("Ses düzeyi", volume);
        tabs->addTab(graphics, "Grafik");

        auto* scroll = new QScrollArea; scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame);
        auto* advanced = new QWidget; auto* av = new QVBoxLayout(advanced); scroll->setWidget(advanced);
        auto* install = new QGroupBox("Kurulum"); auto* iv = new QVBoxLayout(install); auto* iff = new QFormLayout; iv->addLayout(iff); av->addWidget(install);
        root = text(iff, "Kurulum klasörü", "installRoot", settings.value("installation/root", dataRoot()).toString());
        assetSource = text(iff, "Özgün dosya kaynağı", "assetSource", settings.value("assets/source").toString());
        extractor = text(iff, innoextractLabel, "extractor", settings.value("assets/extractor").toString());
        extractor->setPlaceholderText("otomatik bulunur");
        auto* ir = new QHBoxLayout; iv->addLayout(ir);
        button(ir, "Dosyaları doğrula", "verifyButton", [this] { verifyAssets(false); });
        button(ir, "Onar", "repairButton", [this] { verifyAssets(true); });
#ifndef _WIN32
        button(ir, "İmzalı paket kur", "installButton", [this] { installBundle(); });
        auto* updates = new QGroupBox("Güncellemeler"); auto* uv = new QVBoxLayout(updates); auto* uf = new QFormLayout; uv->addLayout(uf); av->addWidget(updates);
        repo = text(uf, "GitHub OWNER/REPOSITORY", "updateRepo", settings.value("updates/repo").toString());
        repo->setPlaceholderText(QString("boş = ") + ikv::release::repository);
        channel = new QComboBox; channel->setObjectName("channel"); channel->addItems({"stable", "experimental"}); channel->setCurrentText(settings.value("updates/release_channel", ikv::release::channel).toString()); uf->addRow("Kanal", channel);
        trust = text(uf, "Güvenilen Ed25519 public PEM", "updateTrust", settings.value("updates/trust").toString());
        trust->setPlaceholderText("boş = yerleşik IKV sürüm anahtarı");
        updateCa = text(uf, "HTTPS CA PEM", "updateCa", settings.value("updates/ca").toString());
        updateCa->setPlaceholderText("boş = sistem güveni");
        releaseApi = text(uf, "Kendi HTTPS release API (isteğe bağlı)", "releaseApi", settings.value("updates/api").toString());
        auto* ur = new QHBoxLayout; uv->addLayout(ur);
        button(ur, "Güncellemeleri denetle", "updateButton", [this] { persist(); checkUpdates(false); });
        button(ur, "Önceki sürüme dön", "rollbackButton", [this] {
            persist();
            runHelper({"rollback", "--trust", trustPem(trust->text()), "--root", root->text()}, [this] { restartIntoInstalled(); });
        });
#endif
        auto* content = new QGroupBox("Özgün içerik eşlemeleri (isteğe bağlı)"); auto* cf = new QFormLayout(content); av->addWidget(content);
        modelMap = text(cf, "Model eşlemesi (id:N → JSON)", "modelMap", settings.value("graphics/modelMap").toString());
        audioPath = text(cf, "Ses klasörü (göreli)", "audioPath", settings.value("graphics/audioPath").toString());
        soundMap = text(cf, "Ses olayı eşlemesi (JSON)", "soundMap", settings.value("graphics/soundMap").toString());
        auto* diagnostics = new QGroupBox("Tanılama"); auto* dv = new QVBoxLayout(diagnostics); av->addWidget(diagnostics);
        logs = new QPlainTextEdit; logs->setObjectName("diagnosticLogs"); logs->setReadOnly(true); logs->setMinimumHeight(160); logs->document()->setMaximumBlockCount(5000); dv->addWidget(logs);
        auto* dr = new QHBoxLayout; dv->addLayout(dr); dr->addStretch();
        button(dr, "Tanılama kaydet", "saveDiagnostics", [this] { const auto file = QFileDialog::getSaveFileName(this, "Diagnostics", "ikv-diagnostics.txt"); if (file.isEmpty()) return; QSaveFile out(file); const auto bytes = logs->toPlainText().toUtf8(); if (!out.open(QIODevice::WriteOnly) || out.write(bytes) != bytes.size() || !out.commit()) throw std::runtime_error("Cannot save diagnostics"); });
        tabs->addTab(scroll, "Gelişmiş");

        auto* br = new QHBoxLayout; sv->addLayout(br); br->addStretch();
        button(br, "Kaydet", "saveGraphics", [this] { persist(); status->setText("Ayarlar kaydedildi"); settingsDialog->hide(); showPage(); });
        button(br, "Kapat", "closeSettings", [this] { settingsDialog->hide(); });
    }
    void buildServerDialog() {
        serverDialog = new QDialog(this); serverDialog->setObjectName("serverDialog"); serverDialog->setWindowTitle("Sunucu ekle / düzenle"); serverDialog->setMinimumWidth(460);
        auto* v = new QVBoxLayout(serverDialog); auto* f = new QFormLayout; v->addLayout(f);
        serverName = text(f, "Ad", "serverName");
        serverAddress = text(f, "Adres", "serverAddress"); serverAddress->setPlaceholderText("sunucu.ornek.com veya sunucu:7443");
        auto* caRow = new QHBoxLayout; serverCa = new QLineEdit; serverCa->setObjectName("serverCa"); serverCa->setPlaceholderText("boş = sistem güveni"); caRow->addWidget(serverCa, 1);
        button(caRow, "Seç…", "serverCaBrowse", [this] { const auto file = QFileDialog::getOpenFileName(serverDialog, "Sunucu CA sertifikası", {}, "PEM (*.pem *.crt);;Tümü (*)"); if (!file.isEmpty()) serverCa->setText(file); });
        f->addRow("CA sertifikası", caRow);
        auto* r = new QHBoxLayout; v->addLayout(r); r->addStretch();
        button(r, "İptal", "cancelServer", [this] { serverDialog->reject(); });
        button(r, "Kaydet", "saveServer", [this] { saveServer(); })->setDefault(true);
    }
    // ---- Sunucu Kur / Sunucu Yönetimi: src/launcher_server.cpp (docs/SELF_HOSTING.md) ----
    void showHost() { hostDialog->show(); hostDialog->raise(); hostDialog->activateWindow(); }
    void offerHost() { if (hostAfterInstall && hostAfterInstall->isChecked()) showHost(); }
    ikv::launcher_server::Hooks hostHooks() {
        return {[this] { return root->text(); }, [this] { return hasAssets(); }, [this](const QString& line) { log(line); },
            [this](const QString& auth, const QString& ca, const QString& user, const QString& secret, bool select) {
                const auto value = QJsonDocument(QJsonObject{{"auth", auth.isEmpty() ? QString("https://localhost:7443") : auth}, {"ca", ca}}).toJson(QJsonDocument::Compact);
                const bool changed = settings.value("profiles/Localhost").toByteArray() != value;
                settings.setValue("profiles/Localhost", value); settings.sync();
                if (!select) { if (changed && profiles->currentText() == "Localhost") clearSession(); return; }
                if (!user.isEmpty() && (username->text().isEmpty() || !secret.isEmpty())) username->setText(user);
                if (profiles->currentText() == "Localhost") clearSession(); else profiles->setCurrentText("Localhost");
                status->setText("Yerel sunucu çalışıyor");
                if (secret.isEmpty()) return;
                password->setText(secret); // automatic launcher login with the just-created administrator
                if (auto* login = findChild<QPushButton*>("loginButton")) login->click();
            }};
    }
public:
    Launcher() {
        setWindowTitle("İstanbul Kıyamet Vakti"); setObjectName("ikvLauncher"); resize(520, 620);
        buildSettings(); buildServerDialog();
        auto* main = new QVBoxLayout(this);
        auto* header = new QHBoxLayout; main->addLayout(header);
        auto* title = new QLabel(QString("<span style='font-size:20pt;font-weight:bold'>İstanbul Kıyamet Vakti</span><br>Bağımsız %1 başlatıcısı").arg(platformName));
        header->addWidget(title, 1);
        button(header, "⚙ Ayarlar", "settingsButton", [this] { settingsDialog->show(); settingsDialog->raise(); settingsDialog->activateWindow(); });
#ifndef _WIN32
        hostDialog = ikv::launcher_server::createHostPanel(this, button(header, "Sunucu Kur", "hostButton", [] {}), hostHooks()); // live status chip
#endif
        pages = new QStackedWidget; pages->setObjectName("pages"); main->addWidget(pages, 1);

        installPage = new QWidget; installPage->setObjectName("installPage"); auto* iv = new QVBoxLayout(installPage);
        iv->addStretch();
        auto* intro = new QLabel("<h2>Oyunu Kur</h2>Oynamak için özgün İstanbul Kıyamet Vakti dosyaları gerekir. Kurulum dosyası çalıştırılmaz, yalnızca güvenli biçimde açılır; özgün dosyalar değiştirilmez.");
        intro->setWordWrap(true); iv->addWidget(intro);
        button(iv, "Kurulum dosyasını seç (istanbulkur6_0.exe)", "extractButton", [this] { importAssets(true); })->setMinimumHeight(48);
        button(iv, "Mevcut kurulum klasörünü seç", "importButton", [this] { importAssets(false); });
#ifdef _WIN32
        auto* hostNote = new QLabel(hostingNote); hostNote->setObjectName("hostingNote"); hostNote->setWordWrap(true); iv->addWidget(hostNote);
#else
        hostAfterInstall = new QCheckBox("Bu bilgisayarda yerel sunucu da kur (Sunucu Kur)"); hostAfterInstall->setObjectName("hostAfterInstall");
        iv->addWidget(hostAfterInstall);
#endif
        installNote = new QLabel; installNote->setWordWrap(true); installNote->setEnabled(false); iv->addWidget(installNote);
        iv->addStretch(); pages->addWidget(installPage);

        loginPage = new QWidget; loginPage->setObjectName("loginPage"); auto* lv = new QVBoxLayout(loginPage);
        lv->addStretch(); auto* lf = new QFormLayout; lv->addLayout(lf);
        auto* server = new QHBoxLayout;
        profiles = new QComboBox; profiles->setObjectName("profiles"); server->addWidget(profiles, 1);
        button(server, "Sunucu ekle", "addServer", [this] {
            const auto p = profile();
            serverName->setText(profiles->currentText()); serverAddress->setText(QUrl(p.value("auth").toString()).authority());
            serverCa->setText(p.value("ca").toString()); serverDialog->open();
        });
        lf->addRow("Sunucu", server);
        username = text(lf, "Kullanıcı adı", "username", settings.value("account/username").toString());
        password = text(lf, "Parola", "password"); password->setEchoMode(QLineEdit::Password);
        auto* ar = new QHBoxLayout; lv->addLayout(ar);
        auto* login = button(ar, "Giriş Yap", "loginButton", [this] { api("/v1/login", "POST", {{"username", username->text()}, {"password", password->text()}}, false, [this](const QByteArray& bytes) { session.fill('\0'); session = QJsonDocument::fromJson(bytes).object().value("session").toString().toUtf8(); password->clear(); if (session.size() != 64 || std::any_of(session.begin(), session.end(), [](char c) { return !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')); })) { clearSession(); throw std::runtime_error("Sunucudan geçersiz oturum yanıtı geldi"); } authSessionOrigin = authUrl(); settings.setValue("account/username", username->text()); account->setText("<h3>" + username->text().toHtmlEscaped() + "</h3>Karakter seçimi ve yaratma oyunun içinde yapılır."); status->setText("Giriş yapıldı"); showPage(); }); });
        login->setMinimumHeight(40);
        button(ar, "Kayıt Ol", "registerButton", [this] { api("/v1/register", "POST", {{"username", username->text()}, {"password", password->text()}}, false, [this](const QByteArray&) { password->clear(); status->setText("Hesap oluşturuldu; giriş yapın"); }); })->setMinimumHeight(40);
        connect(password, &QLineEdit::returnPressed, login, &QPushButton::click);
        auto* reset = button(lv, "Parola sıfırla (yönetici kodu ile)", "resetButton", [this] { bool ok; const QString token = QInputDialog::getText(this, "Parola sıfırla", "Yöneticinin verdiği tek kullanımlık kod (yeni parolayı Parola alanına yazın)", QLineEdit::Password, {}, &ok); if (!ok) return; api("/v1/reset", "POST", {{"username", username->text()}, {"token", token}, {"new_password", password->text()}}, false, [this](const QByteArray&) { clearSession(); status->setText("Parola değiştirildi"); }); });
        reset->setFlat(true);
        lv->addStretch(); pages->addWidget(loginPage);

        // Character selection/creation is the client's original 3D charsel/chargen module (no launcher list).
        playPage = new QWidget; playPage->setObjectName("playPage"); auto* cv = new QVBoxLayout(playPage);
        cv->addStretch();
        account = new QLabel; account->setObjectName("account"); account->setWordWrap(true); cv->addWidget(account);
        auto* play = button(cv, "OYNA", "playButton", [this] { this->play(); });
        play->setMinimumHeight(56); play->setStyleSheet("font-size:18pt;font-weight:bold");
        auto* cr = new QHBoxLayout; cv->addLayout(cr); cr->addStretch();
        button(cr, "Çıkış", "logoutButton", [this] { api("/v1/logout", "POST", {}, true, [this](const QByteArray&) { clearSession(); status->setText("Oturum kapatıldı"); }); });
        cv->addStretch();
        pages->addWidget(playPage);

#ifndef _WIN32
        updateBanner = new QWidget; updateBanner->setObjectName("updateBanner"); updateBanner->hide();
        auto* ub = new QHBoxLayout(updateBanner); ub->setContentsMargins(0, 0, 0, 0);
        updateLabel = new QLabel; updateLabel->setObjectName("updateLabel"); ub->addWidget(updateLabel, 1);
        updateNow = button(ub, "Güncelle", "updateNow", [this] { updateInstalled ? restartIntoInstalled() : installUpdate(); });
        main->addWidget(updateBanner);
#endif
        status = new QLabel("Hazır"); status->setObjectName("status"); status->setWordWrap(true); main->addWidget(status);
        progress = new QProgressBar; progress->setObjectName("progress"); progress->setTextVisible(false); progress->setVisible(false); main->addWidget(progress);

        QStringList names = settings.value("profileNames").toStringList();
        if (!settings.contains("profiles/Localhost"))
            settings.setValue("profiles/Localhost", QJsonDocument(QJsonObject{{"auth", "https://localhost:7443"}, {"ca", ""}}).toJson(QJsonDocument::Compact));
        if (!names.contains("Localhost")) { names.prepend("Localhost"); settings.setValue("profileNames", names); }
        profiles->addItems(names); profiles->setCurrentText(settings.value("lastProfile").toString());
        connect(profiles, &QComboBox::currentIndexChanged, this, [this] { settings.setValue("lastProfile", profiles->currentText()); clearSession(); });
        showPage();
        log("Native Qt " QT_VERSION_STR " platform=" + QApplication::platformName());
    }
#ifndef _WIN32
    // Normal start: report an unusable installed release, then check for updates without blocking.
    void startup(const QString& handoffIssue) {
        QTimer::singleShot(0, this, [this, handoffIssue] {
            if (!handoffIssue.isEmpty()) error(handoffIssue);
            try { checkUpdates(true); } catch (const std::exception& e) { status->setText("Güncellemeler denetlenemedi: " + failure(e)); log(status->text()); }
        });
    }
    // --update-once: the startup check with explicit configuration; a newer release is installed through
    // the real Güncelle button and the launcher restarts into it (which then reports itself current).
    void updateOnce(const QCommandLineParser& parser) {
        automation = true;
        networkTimeoutMs = parser.value("network-timeout-ms").toInt();
        root->setText(parser.value("install-root")); repo->setText(parser.value("repo")); trust->setText(parser.value("trust")); updateCa->setText(parser.value("update-ca")); releaseApi->setText(parser.value("release-api")); channel->setCurrentText(parser.value("channel"));
        QTimer::singleShot(0, this, [this] { try { persist(); checkUpdates(false); } catch (const std::exception& e) { error(failure(e)); } });
    }
#endif
    ~Launcher() override { for (auto* worker : workers) worker->wait(); session.fill('\0'); }
};
} // namespace
int main(int argc, char** argv) {
#ifndef _WIN32
    if (ikv::launcher_server::isSupervisorInvocation(argc, argv)) return ikv::launcher_server::supervisorMain(argc, argv);
#endif
    QCoreApplication::setOrganizationName("IKVCommunity"); QCoreApplication::setApplicationName("IKVLauncher");
    QCoreApplication::setApplicationVersion(ikv::release::version);
#ifndef _WIN32
    for (int i = 0; i < argc; ++i) startArguments.emplace_back(argv[i]);
    const QString handoffIssue = handoff(); // returns only when this build stays in charge
    if (!handoffIssue.isEmpty()) fprintf(stderr, "ikv-launcher: %s\n", handoffIssue.toUtf8().constData());
#endif
    QApplication app(argc, argv);
    QCommandLineParser parser; parser.setApplicationDescription("Native own-server İKV launcher; no official credentials or endpoints"); parser.addHelpOption(); parser.addVersionOption();
#ifndef _WIN32
    parser.addOption({"update-once", "Check the release API, install a newer release through Güncelle and restart into it; exit when current (no dialogs)"});
    for (const char* option : {"install-root", "repo", "trust", "update-ca", "release-api"}) parser.addOption({option, "Explicit update configuration", "value"});
    parser.addOption({"network-timeout-ms", "Reduce HTTPS wall deadlines (1..60000); operation deadline is three times this value", "milliseconds", "60000"});
    parser.addOption({"channel", "Release channel stable|experimental", "channel", ikv::release::channel});
#endif
    parser.process(app);
    try {
    Launcher launcher;
#ifdef _WIN32
    launcher.show();
#else
    if (parser.isSet("update-once")) {
        for (const char* required : {"install-root", "repo", "trust", "update-ca"}) if (parser.value(required).isEmpty()) { fprintf(stderr, "--%s required\n", required); return 2; }
        if (parser.value("channel") != "stable" && parser.value("channel") != "experimental") return 2;
        bool timeoutOk = false; const int timeout = parser.value("network-timeout-ms").toInt(&timeoutOk);
        if (!timeoutOk || timeout < 1 || timeout > 60000) return 2;
        launcher.updateOnce(parser);
    } else { launcher.show(); launcher.startup(handoffIssue); }
#endif
    return app.exec();
    } catch (const std::exception& e) {
        fprintf(stderr, "ikv-launcher: %s\n", e.what()); return 1;
    }
}
