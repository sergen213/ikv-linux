#include "ikv/launcher_assets.hpp"
#include "ikv/assets.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QSet>
#include <QUuid>
#include <array>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <linux/fs.h>
#include <stdexcept>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <utility>

namespace ikv::launcher_assets {
namespace {
constexpr quint64 maxBytes = 128ULL * 1024 * 1024 * 1024;
constexpr quint64 maxFileBytes = 16ULL * 1024 * 1024 * 1024;
constexpr qsizetype maxEntries = 200000;
constexpr qsizetype maxManifestBytes = 64 * 1024 * 1024;
constexpr unsigned maxDepth = 48;
constexpr char manifestName[] = "asset-manifest.json";

[[noreturn]] void fail(const QString& message) {
    throw std::runtime_error(message.toUtf8().constData());
}
void checkDeadline(const QDeadlineTimer& deadline) {
    if (deadline.hasExpired()) fail("Asset import deadline expired");
}
void checked(bool value, const QString& message) {
    if (!value) fail(message + ": " + QString::fromLocal8Bit(std::strerror(errno)));
}
class Fd {
public:
    explicit Fd(int value = -1) : value_(value) {}
    ~Fd() { if (value_ >= 0) ::close(value_); }
    Fd(Fd&& other) noexcept : value_(std::exchange(other.value_, -1)) {}
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) {
            if (value_ >= 0) ::close(value_);
            value_ = std::exchange(other.value_, -1);
        }
        return *this;
    }
    Fd(const Fd&) = delete;
    int get() const { return value_; }
private:
    int value_;
};
struct Entry { quint64 size; QByteArray hash; };
using Entries = QMap<QString, Entry>;

bool safePath(const QString& path) {
    if (path.isEmpty() || path.size() > 4096 || path.startsWith('/') ||
        path.contains('\\') || path.contains(':') || path.contains(QChar(0))) return false;
    const auto components = path.split('/');
    if (components.size() > static_cast<qsizetype>(maxDepth)) return false;
    for (const auto& part : components) {
        if (part.isEmpty() || part == "." || part == "..") return false;
        for (const QChar c : part) if (c.unicode() < 32 || c.unicode() == 127) return false;
    }
    return true;
}
QString absolute(const QString& path) {
    if (path.isEmpty()) fail("A source and installation directory must be selected");
    if (path.contains(QChar(0))) fail("NUL in filesystem path");
    return QDir::cleanPath(QDir(path).absolutePath());
}
Fd directory(int parent, const QByteArray& name, bool create = false,
             const QDeadlineTimer& deadline = QDeadlineTimer(QDeadlineTimer::Forever)) {
    checkDeadline(deadline);
    if (create && ::mkdirat(parent, name.constData(), 0700) != 0 && errno != EEXIST)
        fail("Cannot create asset directory: " + QFile::decodeName(name));
    Fd result(::openat(parent, name.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    checked(result.get() >= 0, "Unsafe or unreadable directory " + QFile::decodeName(name));
    return result;
}
Fd rootDirectory(const QString& path, bool create = false,
                 const QDeadlineTimer& deadline = QDeadlineTimer(QDeadlineTimer::Forever)) {
    checkDeadline(deadline);
    Fd result(::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    checked(result.get() >= 0, "Cannot open filesystem root");
    for (const auto& part : path.split('/', Qt::SkipEmptyParts))
        result = directory(result.get(), QFile::encodeName(part), create, deadline);
    return result;
}
Fd parentDirectory(int root, const QString& path, bool create = false,
                   const QDeadlineTimer& deadline = QDeadlineTimer(QDeadlineTimer::Forever)) {
    Fd result = directory(root, ".", false, deadline);
    const auto parts = path.split('/');
    for (qsizetype i = 0; i + 1 < parts.size(); ++i)
        result = directory(result.get(), QFile::encodeName(parts[i]), create, deadline);
    return result;
}
QByteArray leaf(const QString& path) { return QFile::encodeName(path.section('/', -1)); }
Fd regularFile(int root, const QString& path, int flags = O_RDONLY,
               const QDeadlineTimer& deadline = QDeadlineTimer(QDeadlineTimer::Forever)) {
    Fd parent = parentDirectory(root, path, false, deadline);
    checkDeadline(deadline);
    Fd result(::openat(parent.get(), leaf(path).constData(), flags | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600));
    checked(result.get() >= 0, "Unsafe or unreadable file " + path);
    struct stat info{};
    checked(::fstat(result.get(), &info) == 0 && S_ISREG(info.st_mode), "Not a regular file " + path);
    return result;
}
Fd lockRoot(int root, bool exclusive,
            const QDeadlineTimer& deadline = QDeadlineTimer(QDeadlineTimer::Forever)) {
    Fd lock = regularFile(root, ".asset-import.lock", O_RDWR | O_CREAT, deadline);
    checkDeadline(deadline);
    checked(::flock(lock.get(), (exclusive ? LOCK_EX : LOCK_SH) | LOCK_NB) == 0,
            "Another asset operation is running");
    return lock;
}
QString sourceDirectory(const QString& selected,
                        const QDeadlineTimer& deadline = QDeadlineTimer(QDeadlineTimer::Forever)) {
    checkDeadline(deadline);
    const QString base = absolute(selected);
    Fd root = rootDirectory(base, false, deadline);
    const QStringList choices{"base", "app/base", "app"};
    for (const auto& choice : choices) {
        checkDeadline(deadline);
        QString candidate = base;
        bool exists = true;
        for (const auto& component : choice.split('/')) {
            checkDeadline(deadline);
            candidate += '/' + component;
            struct stat info{};
            const QByteArray native = QFile::encodeName(candidate);
            if (::lstat(native.constData(), &info) != 0) {
                if (errno != ENOENT) fail("Cannot inspect selected source " + candidate);
                exists = false;
                break;
            }
            if (!S_ISDIR(info.st_mode)) fail("Unsafe asset source directory " + candidate);
        }
        if (exists) {
            Fd validated = rootDirectory(candidate, false, deadline);
            return candidate;
        }
    }
    return base;
}
void disjoint(const QString& source, const QString& install) {
    // Native extraction may retain its validated originals in this one subtree.
    // sourceDirectory and rootDirectory reject symlinks in every component.
    if (source.startsWith(install + "/sources/")) return;
    if (source == "/" || install == "/" || source == install ||
        source.startsWith(install + '/') || install.startsWith(source + '/'))
        fail("Source and installation directories must not overlap");
}

Entry hashFile(int fd, int output, const QString& path,
               const QDeadlineTimer& deadline = QDeadlineTimer(QDeadlineTimer::Forever)) {
    checkDeadline(deadline);
    struct stat before{};
    checked(::fstat(fd, &before) == 0 && S_ISREG(before.st_mode), "Unsafe file " + path);
    if (before.st_size < 0 || static_cast<quint64>(before.st_size) > maxFileBytes)
        fail("Asset file exceeds 16 GiB limit: " + path);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    std::array<char, 128 * 1024> buffer{};
    quint64 size = 0;
    for (;;) {
        checkDeadline(deadline);
        const ssize_t count = ::read(fd, buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR) continue;
        checked(count >= 0, "Cannot read asset " + path);
        checkDeadline(deadline);
        if (count == 0) break;
        if (static_cast<quint64>(count) > maxFileBytes - size) fail("Growing asset exceeds file limit: " + path);
        hash.addData(QByteArrayView(buffer.data(), count));
        size += static_cast<quint64>(count);
        for (ssize_t written = 0; output >= 0 && written < count;) {
            checkDeadline(deadline);
            const ssize_t amount = ::write(output, buffer.data() + written, static_cast<size_t>(count - written));
            if (amount < 0 && errno == EINTR) continue;
            checked(amount > 0, "Cannot copy asset " + path);
            written += amount;
        }
    }
    checkDeadline(deadline);
    struct stat after{};
    checked(::fstat(fd, &after) == 0, "Cannot recheck asset " + path);
    if (size != static_cast<quint64>(before.st_size) || before.st_size != after.st_size ||
        before.st_mtim.tv_sec != after.st_mtim.tv_sec || before.st_mtim.tv_nsec != after.st_mtim.tv_nsec ||
        before.st_ctim.tv_sec != after.st_ctim.tv_sec || before.st_ctim.tv_nsec != after.st_ctim.tv_nsec)
        fail("Asset changed while reading: " + path);
    if (output >= 0) checked(::fsync(output) == 0, "Cannot sync copied asset " + path);
    checkDeadline(deadline);
    return {size, hash.result().toHex()};
}
bool recognizedArchive(int fd, quint64 size, const QDeadlineTimer& deadline) {
    checkDeadline(deadline);
    std::array<char, 6> signature{};
    if (::pread(fd, signature.data(), signature.size(), 0) != static_cast<ssize_t>(signature.size()) ||
        std::memcmp(signature.data(), "SKYF", 4) != 0) return false;
    // Reuse the bounded original-format parser on the already no-follow-opened inode.
    std::ifstream input("/proc/self/fd/" + std::to_string(fd), std::ios::binary);
    if (!input) fail("Cannot inspect original asset archive");
    const auto archive = ikv::assets::read_archive(input, size);
    checkDeadline(deadline);
    if (archive.entries.empty()) return false;
    const auto& first = archive.entries.front().path;
    return first.starts_with("textures/") || first.starts_with("lightmaps/") || first.starts_with("objects/");
}
struct Scan {
    Entries entries;
    QSet<QString> folded;
    quint64 bytes = 0;
    qsizetype visited = 0;
    qsizetype pathBytes = 0;
    bool recognizable = false;
};
void scanDirectory(int fd, const QString& prefix, unsigned depth, Scan& scan, bool recognize,
                   const QDeadlineTimer& deadline) {
    checkDeadline(deadline);
    if (depth > maxDepth) fail("Asset directory nesting exceeds 48 levels");
    Fd fresh = directory(fd, ".", false, deadline);
    DIR* stream = ::fdopendir(::dup(fresh.get()));
    checked(stream != nullptr, "Cannot scan asset directory " + prefix);
    struct Close { DIR* value; ~Close() { ::closedir(value); } } close{stream};
    for (;;) {
        checkDeadline(deadline);
        errno = 0;
        const dirent* item = ::readdir(stream);
        checkDeadline(deadline);
        if (!item) { checked(errno == 0, "Cannot finish asset scan " + prefix); break; }
        const QByteArray native(item->d_name);
        if (native == "." || native == "..") continue;
        const QString name = QFile::decodeName(native);
        const QString path = prefix.isEmpty() ? name : prefix + '/' + name;
        if (!safePath(path) || QFile::encodeName(name) != native) fail("Unsafe asset path " + path);
        if (++scan.visited > maxEntries) fail("Asset tree exceeds 200000 entries");
        scan.pathBytes += path.toUtf8().size();
        if (scan.pathBytes > maxManifestBytes) fail("Asset paths exceed 64 MiB budget");
        const QString folded = path.normalized(QString::NormalizationForm_C).toCaseFolded();
        if (scan.folded.contains(folded)) fail("Colliding portable asset path " + path);
        scan.folded.insert(folded);
        struct stat info{};
        checked(::fstatat(fd, native.constData(), &info, AT_SYMLINK_NOFOLLOW) == 0, "Cannot inspect asset " + path);
        if (S_ISDIR(info.st_mode)) {
            Fd child = directory(fd, native, false, deadline);
            scanDirectory(child.get(), path, depth + 1, scan, recognize, deadline);
        } else if (S_ISREG(info.st_mode)) {
            Fd file = regularFile(fd, name, O_RDONLY, deadline);
            Entry entry = hashFile(file.get(), -1, path, deadline);
            if (entry.size > maxBytes - scan.bytes) fail("Asset tree exceeds 128 GiB limit");
            scan.bytes += entry.size;
            if (recognize && recognizedArchive(file.get(), entry.size, deadline)) scan.recognizable = true;
            scan.entries.insert(path, std::move(entry));
        } else fail("Symlinks and special files are not allowed: " + path);
    }
}
Scan scanTree(int root, bool recognize = false,
              const QDeadlineTimer& deadline = QDeadlineTimer(QDeadlineTimer::Forever)) {
    Scan result;
    scanDirectory(root, {}, 0, result, recognize, deadline);
    return result;
}
void writeAll(int fd, QByteArrayView data,
              const QDeadlineTimer& deadline = QDeadlineTimer(QDeadlineTimer::Forever)) {
    qsizetype written = 0;
    while (written < data.size()) {
        checkDeadline(deadline);
        const auto chunk = qMin<qsizetype>(data.size() - written, 128 * 1024);
        const ssize_t amount = ::write(fd, data.data() + written, static_cast<size_t>(chunk));
        if (amount < 0 && errno == EINTR) continue;
        checked(amount > 0, "Cannot write asset manifest");
        written += amount;
    }
    checkDeadline(deadline);
    checked(::fsync(fd) == 0, "Cannot sync asset manifest");
    checkDeadline(deadline);
}
QByteArray manifest(const Entries& entries, const QDeadlineTimer& deadline) {
    QJsonArray files;
    for (auto i = entries.cbegin(); i != entries.cend(); ++i) {
        checkDeadline(deadline);
        files.append(QJsonObject{{"path", i.key()}, {"size", static_cast<qint64>(i->size)},
                                 {"sha256", QString::fromLatin1(i->hash)}});
    }
    QByteArray result = QJsonDocument(QJsonObject{{"format", 1},
        {"trust", "user-selected-local-snapshot"}, {"files", files}}).toJson(QJsonDocument::Compact);
    checkDeadline(deadline);
    if (result.size() > maxManifestBytes) fail("Asset manifest exceeds 64 MiB limit");
    return result;
}
Entries readManifest(int root) {
    Fd file = regularFile(root, manifestName);
    struct stat info{};
    checked(::fstat(file.get(), &info) == 0, "Cannot read asset manifest");
    if (info.st_size <= 0 || info.st_size > maxManifestBytes) fail("Invalid asset manifest size");
    QFile input;
    checked(input.open(file.get(), QIODevice::ReadOnly, QFileDevice::DontCloseHandle), "Cannot read manifest");
    const QByteArray bytes = input.read(maxManifestBytes + 1);
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(bytes, &error);
    if (input.error() != QFileDevice::NoError || error.error != QJsonParseError::NoError || !document.isObject())
        fail("Invalid asset manifest JSON");
    const auto object = document.object();
    if (object.value("format").toInt(-1) != 1 || object.value("trust").toString() != "user-selected-local-snapshot" ||
        !object.value("files").isArray()) fail("Unsupported asset manifest");
    const auto files = object.value("files").toArray();
    if (files.isEmpty() || files.size() > maxEntries) fail("Empty or oversized asset manifest");
    Entries entries;
    QSet<QString> folded;
    quint64 total = 0;
    qsizetype paths = 0;
    for (const auto& value : files) {
        const auto entry = value.toObject();
        const QString path = entry.value("path").toString();
        const QByteArray hash = entry.value("sha256").toString().toLatin1();
        const double size = entry.value("size").toDouble(-1);
        if (!safePath(path) || hash.size() != 64 || size < 0 || size > maxFileBytes ||
            size != static_cast<double>(static_cast<quint64>(size))) fail("Invalid asset manifest entry");
        for (const char c : hash) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            fail("Invalid asset manifest SHA256");
        const auto normalized = path.normalized(QString::NormalizationForm_C).toCaseFolded();
        if (folded.contains(normalized)) fail("Duplicate asset manifest path");
        folded.insert(normalized);
        const auto count = static_cast<quint64>(size);
        if (count > maxBytes - total) fail("Manifest exceeds 128 GiB limit");
        total += count;
        paths += path.toUtf8().size();
        if (paths > maxManifestBytes) fail("Manifest paths exceed 64 MiB limit");
        entries.insert(path, {count, hash});
    }
    for (auto i = entries.cbegin(); i != entries.cend(); ++i) {
        QString parent = i.key();
        while (parent.contains('/')) {
            parent = parent.section('/', 0, -2);
            if (folded.contains(parent.normalized(QString::NormalizationForm_C).toCaseFolded()))
                fail("Asset manifest file/directory collision");
        }
    }
    return entries;
}
void removeTree(int fd) noexcept {
    const int fresh = ::openat(fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    DIR* stream = fresh >= 0 ? ::fdopendir(fresh) : nullptr;
    if (!stream) { if (fresh >= 0) ::close(fresh); return; }
    while (const dirent* item = ::readdir(stream)) {
        if (std::strcmp(item->d_name, ".") == 0 || std::strcmp(item->d_name, "..") == 0) continue;
        const int child = ::openat(fd, item->d_name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (child >= 0) { removeTree(child); ::close(child); ::unlinkat(fd, item->d_name, AT_REMOVEDIR); }
        else ::unlinkat(fd, item->d_name, 0);
    }
    ::closedir(stream);
}
struct Stage {
    int parent;
    QByteArray name;
    Fd root;
    explicit Stage(int install) : parent(install), name((".asset-stage-" + QUuid::createUuid().toString(QUuid::Id128)).toUtf8()) {
        checked(::mkdirat(parent, name.constData(), 0700) == 0, "Cannot create asset staging directory");
        root = directory(parent, name);
    }
    ~Stage() { removeTree(root.get()); ::unlinkat(parent, name.constData(), AT_REMOVEDIR); }
};
void copySnapshot(int source, int destination, const Entries& entries,
                  const QDeadlineTimer& deadline = QDeadlineTimer(QDeadlineTimer::Forever)) {
    for (auto i = entries.cbegin(); i != entries.cend(); ++i) {
        checkDeadline(deadline);
        Fd input = regularFile(source, i.key(), O_RDONLY, deadline);
        Fd parent = parentDirectory(destination, i.key(), true, deadline);
        Fd output = regularFile(parent.get(), i.key().section('/', -1), O_WRONLY | O_CREAT | O_EXCL, deadline);
        const Entry copied = hashFile(input.get(), output.get(), i.key(), deadline);
        if (copied.size != i->size || copied.hash != i->hash)
            fail("Selected source differs from trusted manifest: " + i.key());
    }
}
Report compare(int root, const Entries& expected) {
    Report report;
    const Scan actual = scanTree(root);
    report.files = expected.size();
    for (auto i = expected.cbegin(); i != expected.cend(); ++i) {
        report.bytes += i->size;
        const auto current = actual.entries.constFind(i.key());
        if (current == actual.entries.cend()) report.missing.append(i.key());
        else if (current->size != i->size || current->hash != i->hash) report.changed.append(i.key());
    }
    for (auto i = actual.entries.cbegin(); i != actual.entries.cend(); ++i)
        if (!expected.contains(i.key())) report.extra.append(i.key());
    return report;
}
bool existsAt(int root, const char* name) {
    struct stat info{};
    if (::fstatat(root, name, &info, AT_SYMLINK_NOFOLLOW) == 0) return true;
    checked(errno == ENOENT, "Cannot inspect installation path");
    return false;
}
}

QString assetRoot(const QString& installRoot) { return QDir(installRoot).filePath("assets"); }
QString manifestPath(const QString& installRoot) { return QDir(installRoot).filePath(manifestName); }

Report importAssets(const QString& selectedSource, const QString& installRoot, QDeadlineTimer deadline) {
    Report report;
    try {
        checkDeadline(deadline);
        const QString sourcePath = sourceDirectory(selectedSource, deadline);
        const QString installPath = absolute(installRoot);
        disjoint(sourcePath, installPath);
        Fd source = rootDirectory(sourcePath, false, deadline);
        const Scan snapshot = scanTree(source.get(), true, deadline);
        if (snapshot.entries.isEmpty() || !snapshot.recognizable)
            fail("No nonempty valid original SKYF textures/lightmaps/objects corpus found in selected source");
        const QByteArray encoded = manifest(snapshot.entries, deadline);
        Fd install = rootDirectory(installPath, true, deadline);
        Fd lock = lockRoot(install.get(), true, deadline);
        if (existsAt(install.get(), "assets") || existsAt(install.get(), manifestName))
            fail("Assets or manifest already exist; use verification and repair instead of overwriting");
        checkDeadline(deadline);
        Stage stage(install.get());
        Fd stagedAssets = directory(stage.root.get(), "assets", true, deadline);
        copySnapshot(source.get(), stagedAssets.get(), snapshot.entries, deadline);
        Fd stagedManifest = regularFile(stage.root.get(), manifestName, O_WRONLY | O_CREAT | O_EXCL, deadline);
        writeAll(stagedManifest.get(), encoded, deadline);
        checked(::fsync(stagedAssets.get()) == 0, "Cannot sync staged assets");
        checkDeadline(deadline);
        checked(::syscall(SYS_renameat2, stage.root.get(), manifestName, install.get(),
                          manifestName, RENAME_NOREPLACE) == 0, "Cannot commit asset manifest");
        try {
            checkDeadline(deadline);
            checked(::syscall(SYS_renameat2, stage.root.get(), "assets", install.get(),
                              "assets", RENAME_NOREPLACE) == 0, "Cannot atomically commit imported assets");
        } catch (...) {
            ::unlinkat(install.get(), manifestName, 0);
            throw;
        }
        checked(::fsync(install.get()) == 0, "Cannot sync installation directory");
        report.files = snapshot.entries.size();
        report.bytes = snapshot.bytes;
    } catch (const std::exception& error) { report.errors.append(QString::fromUtf8(error.what())); }
    return report;
}

Report verifyAssets(const QString& installRoot) {
    Report report;
    try {
        Fd install = rootDirectory(absolute(installRoot));
        Fd lock = lockRoot(install.get(), false);
        const Entries expected = readManifest(install.get());
        if (!existsAt(install.get(), "assets")) {
            report.files = expected.size();
            for (auto i = expected.cbegin(); i != expected.cend(); ++i) {
                report.missing.append(i.key());
                report.bytes += i->size;
            }
        } else {
            Fd assets = directory(install.get(), "assets");
            report = compare(assets.get(), expected);
        }
    } catch (const std::exception& error) { report.errors.append(QString::fromUtf8(error.what())); }
    return report;
}

Report repairAssets(const QString& selectedSource, const QString& installRoot) {
    Report report;
    try {
        const QString sourcePath = sourceDirectory(selectedSource);
        const QString installPath = absolute(installRoot);
        disjoint(sourcePath, installPath);
        Fd source = rootDirectory(sourcePath);
        Fd install = rootDirectory(installPath);
        Fd lock = lockRoot(install.get(), true);
        const Entries expected = readManifest(install.get());
        // Validate the entire selected tree before touching installation assets.
        const Scan sourceTree = scanTree(source.get());
        for (auto i = expected.cbegin(); i != expected.cend(); ++i) {
            const auto supplied = sourceTree.entries.constFind(i.key());
            if (supplied == sourceTree.entries.cend() || supplied->size != i->size || supplied->hash != i->hash)
                fail("Selected source differs from trusted manifest: " + i.key());
        }
        Fd assets = directory(install.get(), "assets", true);
        // Reject symlink/special-file targets; extras and intact files stay untouched.
        const Scan targetTree = scanTree(assets.get());
        Entries needed;
        for (auto i = expected.cbegin(); i != expected.cend(); ++i) {
            const auto target = targetTree.entries.constFind(i.key());
            if (target == targetTree.entries.cend() || target->size != i->size || target->hash != i->hash)
                needed.insert(i.key(), i.value());
        }
        Stage stage(install.get());
        copySnapshot(source.get(), stage.root.get(), needed);
        for (auto i = needed.cbegin(); i != needed.cend(); ++i) {
            Fd destination = parentDirectory(assets.get(), i.key(), true);
            Fd staged = parentDirectory(stage.root.get(), i.key());
            checked(::renameat(staged.get(), leaf(i.key()).constData(), destination.get(), leaf(i.key()).constData()) == 0,
                    "Cannot commit repaired asset " + i.key());
            checked(::fsync(destination.get()) == 0, "Cannot sync repaired asset directory");
        }
        checked(::fsync(install.get()) == 0, "Cannot sync repaired installation");
        report = compare(assets.get(), expected);
    } catch (const std::exception& error) { report.errors.append(QString::fromUtf8(error.what())); }
    return report;
}
} // namespace ikv::launcher_assets
