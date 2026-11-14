#include "ikv/launcher_release.hpp"

#include "ikv/release_defaults.hpp"
#include "ikv/updates.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryFile>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

#ifdef _WIN32
#include <QProcess>
#include <windows.h>
#include <shellapi.h>
#else
#include <unistd.h>
#endif

namespace ikv::launcher_release {
namespace {
std::filesystem::path fs(const QString& p) { return std::filesystem::path(p.toStdU16String()); }
std::unique_ptr<QTemporaryFile> builtinTrust;
#ifdef _WIN32
QStringList startArguments;
constexpr char pointerName[] = "/current.json";
#else
std::vector<QByteArray> startArguments;
constexpr char pointerName[] = "/current";
#endif
} // namespace

QString installRoot() {
    return QSettings().value("installation/root", QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).toString();
}
QString trustSetting() { return QSettings().value("updates/trust").toString(); }

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

Installed installed(const QString& root, const QString& trust, bool onlyNewer) {
    // Nothing installed: never create the install root just to look (current_release takes the installation lock).
    const QFileInfo pointer(root + pointerName);
    if (!pointer.exists() && !pointer.isSymLink()) return {};
    const auto version = ikv::updates::current_release(fs(root));
    if (version.empty()) return {};
    if (onlyNewer && ikv::updates::compare_versions(version, ikv::release::version) <= 0) return {};
    const auto manifest = ikv::updates::verify_release(fs(root), version, fs(trustPem(trust)));
    if (std::none_of(manifest.files.begin(), manifest.files.end(), [](const auto& file) {
            return file.path == ikv::updates::launcher_payload && (file.executable || std::strcmp(ikv::updates::platform, "windows") == 0); }))
        throw std::runtime_error("Kurulu " + version + " sürümü başlatıcı içermiyor (" + ikv::updates::launcher_payload + ")");
    return {QString::fromStdString(version),
            QString::fromStdU16String((fs(root) / "releases" / version / ikv::updates::launcher_payload).u16string())};
}

void rememberArguments(int argc, char** argv) {
#ifdef _WIN32
    (void)argc; (void)argv;
    int count = 0;
    if (wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &count)) {
        for (int i = 0; i < count; ++i) startArguments << QString::fromWCharArray(wide[i]);
        LocalFree(wide);
    }
#else
    for (int i = 0; i < argc; ++i) startArguments.emplace_back(argv[i]);
#endif
}

[[noreturn]] void execInto(const QString& launcher, const QStringList& arguments) {
    builtinTrust.reset();
    std::fflush(nullptr);
#ifdef _WIN32
    // NONCANONICAL (Windows): no exec. The verified release's launcher starts detached from its own versioned directory
    // (bin/ikv-launcher.exe, never the running exe, so nothing is replaced in use); this process then exits.
    qputenv("IKV_UPDATE_HANDOFF", "1");
    if (!QProcess::startDetached(launcher, arguments, QFileInfo(launcher).absolutePath()))
        throw std::runtime_error("Yeni sürüm başlatılamadı (" + launcher.toStdString() + ")");
    QSettings().sync();
    std::_Exit(0);
#else
    ::setenv("IKV_UPDATE_HANDOFF", "1", 1);
    const QByteArray path = QFile::encodeName(launcher);
    std::vector<QByteArray> encoded;
    for (const auto& argument : arguments) encoded.push_back(QFile::encodeName(argument));
    std::vector<char*> argv{const_cast<char*>(path.constData())};
    for (auto& argument : encoded) argv.push_back(argument.data());
    argv.push_back(nullptr);
    ::execv(path.constData(), argv.data());
    throw std::runtime_error("Yeni sürüm başlatılamadı (" + path.toStdString() + "): " + std::strerror(errno));
#endif
}

[[noreturn]] void execInto(const QString& launcher) {
    QStringList arguments;
#ifdef _WIN32
    arguments = startArguments.mid(1);
#else
    for (std::size_t i = 1; i < startArguments.size(); ++i) arguments << QFile::decodeName(startArguments[i]);
#endif
    execInto(launcher, arguments);
}

QString handoff() {
    if (qEnvironmentVariableIsSet("IKV_UPDATE_HANDOFF")) return {};
    try {
        const auto target = installed(installRoot(), trustSetting(), true);
        if (!target.launcher.isEmpty()) execInto(target.launcher);
        return {};
    } catch (const std::exception& e) {
        return "Kurulu güncelleme kullanılamadı; bu sürümle devam ediliyor: " + QString::fromUtf8(e.what());
    }
}

} // namespace ikv::launcher_release
