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
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>
#ifdef _WIN32
#include <QFileInfo>
#include <filesystem>
#include <windows.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace ikv::launcher_assets {
namespace {
constexpr quint64 maxBytes = 128ULL * 1024 * 1024 * 1024;
constexpr quint64 maxFileBytes = 16ULL * 1024 * 1024 * 1024;
constexpr qsizetype maxEntries = 200000;
constexpr qsizetype maxManifestBytes = 64 * 1024 * 1024;
constexpr unsigned maxDepth = 48;
constexpr char manifestName[] = "asset-manifest.json";
#ifdef _WIN32
constexpr Qt::CaseSensitivity pathCase = Qt::CaseInsensitive; // NONCANONICAL: Windows paths ignore case
#else
constexpr Qt::CaseSensitivity pathCase = Qt::CaseSensitive;
#endif

[[noreturn]] void fail(const QString& message) {
    throw std::runtime_error(message.toUtf8().constData());
}
void checkDeadline(const QDeadlineTimer& deadline) {
    if (deadline.hasExpired()) fail("Asset import deadline expired");
}
#ifndef _WIN32
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
#endif
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
void disjoint(const QString& source, const QString& install) {
    // Native extraction may retain its validated originals in this one subtree.
    // sourceDirectory and rootDirectory reject symlinks in every component.
    if (source.startsWith(install + "/sources/", pathCase)) return;
    if (QDir(source).isRoot() || QDir(install).isRoot() || source.compare(install, pathCase) == 0 ||
        source.startsWith(install + '/', pathCase) || install.startsWith(source + '/', pathCase))
        fail("Source and installation directories must not overlap");
}
struct Scan {
    Entries entries;
    QSet<QString> folded;
    quint64 bytes = 0;
    qsizetype visited = 0;
    qsizetype pathBytes = 0;
    bool recognizable = false;
};
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
Entries parseManifest(const QByteArray& bytes) {
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) fail("Invalid asset manifest JSON");
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
Report compare(const Scan& actual, const Entries& expected) {
    Report report;
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
#ifndef _WIN32
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
Entries readManifest(int root) {
    Fd file = regularFile(root, manifestName);
    struct stat info{};
    checked(::fstat(file.get(), &info) == 0, "Cannot read asset manifest");
    if (info.st_size <= 0 || info.st_size > maxManifestBytes) fail("Invalid asset manifest size");
    QFile input;
    checked(input.open(file.get(), QIODevice::ReadOnly, QFileDevice::DontCloseHandle), "Cannot read manifest");
    const QByteArray bytes = input.read(maxManifestBytes + 1);
    if (input.error() != QFileDevice::NoError) fail("Invalid asset manifest JSON");
    return parseManifest(bytes);
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
bool existsAt(int root, const char* name) {
    struct stat info{};
    if (::fstatat(root, name, &info, AT_SYMLINK_NOFOLLOW) == 0) return true;
    checked(errno == ENOENT, "Cannot inspect installation path");
    return false;
}
#else
// NONCANONICAL (Windows): no openat()/O_NOFOLLOW/renameat2. Reparse points (symlinks, junctions) are refused
// inside the selected source and installation roots and final components are opened without following them;
// the roots themselves and their ancestors are the user's choice (OneDrive folders, volume mount points; Wine
// reports Unix mount points the same way). The check-to-use window between components is acceptable for a
// per-user AppData install. Source files are opened share-read only, so their bytes cannot change while hashed.
std::wstring native(const QString& path) { return QDir::toNativeSeparators(path).toStdWString(); }
DWORD attributes(const QString& path) { return GetFileAttributesW(native(path).c_str()); }
bool existsAt(const QString& path) { return attributes(path) != INVALID_FILE_ATTRIBUTES; }
struct Handle {
    HANDLE value;
    explicit Handle(HANDLE handle) : value(handle) {}
    Handle(Handle&& other) noexcept : value(std::exchange(other.value, INVALID_HANDLE_VALUE)) {}
    ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
// A directory; inside the selected roots (selected=false) it must not be a reparse point (create: mkpath first).
void realDirectory(const QString& path, bool create = false, bool selected = false,
                   const QDeadlineTimer& deadline = QDeadlineTimer(QDeadlineTimer::Forever)) {
    checkDeadline(deadline);
    if (create && !QDir().mkpath(path)) fail("Cannot create asset directory: " + path);
    const DWORD value = attributes(path);
    if (value == INVALID_FILE_ATTRIBUTES || !(value & FILE_ATTRIBUTE_DIRECTORY) || (!selected && (value & FILE_ATTRIBUTE_REPARSE_POINT)))
        fail("Unsafe or unreadable directory " + path);
}
Handle openFile(const QString& path, DWORD access, DWORD disposition,
                const QDeadlineTimer& deadline = QDeadlineTimer(QDeadlineTimer::Forever), DWORD share = FILE_SHARE_READ) {
    checkDeadline(deadline);
    const DWORD parent = attributes(QFileInfo(path).absolutePath());
    if (parent == INVALID_FILE_ATTRIBUTES || !(parent & FILE_ATTRIBUTE_DIRECTORY)) fail("Unsafe or unreadable directory for " + path);
    Handle file(CreateFileW(native(path).c_str(), access, share, nullptr, disposition,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    BY_HANDLE_FILE_INFORMATION info{};
    if (file.value == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(file.value, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
        fail("Unsafe or unreadable file " + path);
    return file;
}
Handle lockRoot(const QString& root, bool exclusive,
                const QDeadlineTimer& deadline = QDeadlineTimer(QDeadlineTimer::Forever)) {
    Handle lock = openFile(root + "/.asset-import.lock", GENERIC_READ | GENERIC_WRITE, OPEN_ALWAYS, deadline,
                           FILE_SHARE_READ | FILE_SHARE_WRITE);
    OVERLAPPED range{};
    if (!LockFileEx(lock.value, (exclusive ? LOCKFILE_EXCLUSIVE_LOCK : 0) | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &range))
        fail("Another asset operation is running");
    return lock;
}
QString sourceDirectory(const QString& selected,
                        const QDeadlineTimer& deadline = QDeadlineTimer(QDeadlineTimer::Forever)) {
    const QString base = absolute(selected);
    realDirectory(base, false, true, deadline);
    for (const QString choice : {"base", "app/base", "app"}) {
        if (!existsAt(base + '/' + choice)) continue;
        QString candidate = base;
        for (const QString& part : choice.split('/')) realDirectory(candidate += '/' + part, false, false, deadline);
        return candidate;
    }
    return base;
}
void writeAll(HANDLE file, QByteArrayView data, const QString& what,
              const QDeadlineTimer& deadline = QDeadlineTimer(QDeadlineTimer::Forever)) {
    for (qsizetype written = 0; written < data.size();) {
        checkDeadline(deadline);
        DWORD amount = 0;
        const auto chunk = static_cast<DWORD>(qMin<qsizetype>(data.size() - written, 128 * 1024));
        if (!WriteFile(file, data.data() + written, chunk, &amount, nullptr) || amount == 0) fail("Cannot write " + what);
        written += amount;
    }
}
void sync(HANDLE file, const QString& what) { if (!FlushFileBuffers(file)) fail("Cannot sync " + what); }
Entry hashFile(HANDLE input, HANDLE output, const QString& path,
               const QDeadlineTimer& deadline = QDeadlineTimer(QDeadlineTimer::Forever)) {
    checkDeadline(deadline);
    BY_HANDLE_FILE_INFORMATION before{}, after{};
    if (!GetFileInformationByHandle(input, &before)) fail("Unsafe file " + path);
    const quint64 expected = (static_cast<quint64>(before.nFileSizeHigh) << 32) | before.nFileSizeLow;
    if (expected > maxFileBytes) fail("Asset file exceeds 16 GiB limit: " + path);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    std::array<char, 128 * 1024> buffer{};
    quint64 size = 0;
    for (;;) {
        checkDeadline(deadline);
        DWORD count = 0;
        if (!ReadFile(input, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr)) fail("Cannot read asset " + path);
        checkDeadline(deadline);
        if (count == 0) break;
        if (count > maxFileBytes - size) fail("Growing asset exceeds file limit: " + path);
        hash.addData(QByteArrayView(buffer.data(), count));
        size += count;
        if (output != INVALID_HANDLE_VALUE) writeAll(output, QByteArrayView(buffer.data(), count), "asset " + path, deadline);
    }
    checkDeadline(deadline);
    if (!GetFileInformationByHandle(input, &after) || size != expected || after.nFileSizeHigh != before.nFileSizeHigh ||
        after.nFileSizeLow != before.nFileSizeLow || CompareFileTime(&after.ftLastWriteTime, &before.ftLastWriteTime) != 0)
        fail("Asset changed while reading: " + path);
    if (output != INVALID_HANDLE_VALUE) sync(output, "copied asset " + path);
    checkDeadline(deadline);
    return {size, hash.result().toHex()};
}
bool recognizedArchive(HANDLE file, const QString& full, quint64 size, const QDeadlineTimer& deadline) {
    checkDeadline(deadline);
    std::array<char, 6> signature{};
    OVERLAPPED start{}; // offset 0
    DWORD count = 0;
    if (!ReadFile(file, signature.data(), static_cast<DWORD>(signature.size()), &count, &start) ||
        count != signature.size() || std::memcmp(signature.data(), "SKYF", 4) != 0) return false;
    // The open share-read handle keeps writers out while the bounded parser reads the same file by path.
    std::ifstream input(std::filesystem::path(full.toStdWString()), std::ios::binary);
    if (!input) fail("Cannot inspect original asset archive");
    const auto archive = ikv::assets::read_archive(input, size);
    checkDeadline(deadline);
    if (archive.entries.empty()) return false;
    const auto& first = archive.entries.front().path;
    return first.starts_with("textures/") || first.starts_with("lightmaps/") || first.starts_with("objects/");
}
void scanDirectory(const QString& directory, const QString& prefix, unsigned depth, Scan& scan, bool recognize,
                   const QDeadlineTimer& deadline) {
    checkDeadline(deadline);
    if (depth > maxDepth) fail("Asset directory nesting exceeds 48 levels");
    for (const QString& name : QDir(directory).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDir::Name)) {
        checkDeadline(deadline);
        const QString path = prefix.isEmpty() ? name : prefix + '/' + name;
        if (!safePath(path)) fail("Unsafe asset path " + path);
        if (++scan.visited > maxEntries) fail("Asset tree exceeds 200000 entries");
        scan.pathBytes += path.toUtf8().size();
        if (scan.pathBytes > maxManifestBytes) fail("Asset paths exceed 64 MiB budget");
        const QString folded = path.normalized(QString::NormalizationForm_C).toCaseFolded();
        if (scan.folded.contains(folded)) fail("Colliding portable asset path " + path);
        scan.folded.insert(folded);
        const QString full = directory + '/' + name;
        const DWORD value = attributes(full);
        if (value == INVALID_FILE_ATTRIBUTES || (value & FILE_ATTRIBUTE_REPARSE_POINT))
            fail("Symlinks and special files are not allowed: " + path);
        if (value & FILE_ATTRIBUTE_DIRECTORY) {
            scanDirectory(full, path, depth + 1, scan, recognize, deadline);
            continue;
        }
        const Handle file = openFile(full, GENERIC_READ, OPEN_EXISTING, deadline);
        Entry entry = hashFile(file.value, INVALID_HANDLE_VALUE, path, deadline);
        if (entry.size > maxBytes - scan.bytes) fail("Asset tree exceeds 128 GiB limit");
        scan.bytes += entry.size;
        if (recognize && recognizedArchive(file.value, full, entry.size, deadline)) scan.recognizable = true;
        scan.entries.insert(path, std::move(entry));
    }
}
Scan scanTree(const QString& root, bool recognize = false,
              const QDeadlineTimer& deadline = QDeadlineTimer(QDeadlineTimer::Forever)) {
    Scan result;
    scanDirectory(root, {}, 0, result, recognize, deadline);
    return result;
}
Entries readManifest(const QString& root) {
    const Handle file = openFile(root + '/' + manifestName, GENERIC_READ, OPEN_EXISTING);
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.value, &size) || size.QuadPart <= 0 || size.QuadPart > maxManifestBytes)
        fail("Invalid asset manifest size");
    QByteArray bytes(static_cast<qsizetype>(size.QuadPart), Qt::Uninitialized);
    DWORD count = 0;
    if (!ReadFile(file.value, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr) || count != static_cast<DWORD>(bytes.size()))
        fail("Cannot read manifest");
    return parseManifest(bytes);
}
struct Stage {
    QString path;
    explicit Stage(const QString& install) : path(install + "/.asset-stage-" + QUuid::createUuid().toString(QUuid::Id128)) {
        if (!CreateDirectoryW(native(path).c_str(), nullptr)) fail("Cannot create asset staging directory");
    }
    ~Stage() { QDir(path).removeRecursively(); }
};
void copySnapshot(const QString& source, const QString& destination, const Entries& entries,
                  const QDeadlineTimer& deadline = QDeadlineTimer(QDeadlineTimer::Forever)) {
    for (auto i = entries.cbegin(); i != entries.cend(); ++i) {
        checkDeadline(deadline);
        const Handle input = openFile(source + '/' + i.key(), GENERIC_READ, OPEN_EXISTING, deadline);
        const QString target = destination + '/' + i.key();
        realDirectory(QFileInfo(target).absolutePath(), true, false, deadline);
        const Handle output = openFile(target, GENERIC_WRITE, CREATE_NEW, deadline, 0);
        const Entry copied = hashFile(input.value, output.value, i.key(), deadline);
        if (copied.size != i->size || copied.hash != i->hash)
            fail("Selected source differs from trusted manifest: " + i.key());
    }
}
// MoveFileEx without REPLACE_EXISTING fails on an existing target (the renameat2 NOREPLACE analogue).
void move(const QString& from, const QString& to, bool replace, const QString& message) {
    if (!MoveFileExW(native(from).c_str(), native(to).c_str(), MOVEFILE_WRITE_THROUGH | (replace ? MOVEFILE_REPLACE_EXISTING : 0)))
        fail(message);
}
#endif
}

QString assetRoot(const QString& installRoot) { return QDir(installRoot).filePath("assets"); }
QString manifestPath(const QString& installRoot) { return QDir(installRoot).filePath(manifestName); }

#ifndef _WIN32
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
            report = compare(scanTree(assets.get()), expected);
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
        report = compare(scanTree(assets.get()), expected);
    } catch (const std::exception& error) { report.errors.append(QString::fromUtf8(error.what())); }
    return report;
}
#else
Report importAssets(const QString& selectedSource, const QString& installRoot, QDeadlineTimer deadline) {
    Report report;
    try {
        checkDeadline(deadline);
        const QString sourcePath = sourceDirectory(selectedSource, deadline);
        const QString installPath = absolute(installRoot);
        disjoint(sourcePath, installPath);
        const Scan snapshot = scanTree(sourcePath, true, deadline);
        if (snapshot.entries.isEmpty() || !snapshot.recognizable)
            fail("No nonempty valid original SKYF textures/lightmaps/objects corpus found in selected source");
        const QByteArray encoded = manifest(snapshot.entries, deadline);
        realDirectory(installPath, true, true, deadline);
        const Handle lock = lockRoot(installPath, true, deadline);
        const QString assets = installPath + "/assets", manifestFile = installPath + '/' + manifestName;
        if (existsAt(assets) || existsAt(manifestFile))
            fail("Assets or manifest already exist; use verification and repair instead of overwriting");
        checkDeadline(deadline);
        Stage stage(installPath);
        realDirectory(stage.path + "/assets", true, false, deadline);
        copySnapshot(sourcePath, stage.path + "/assets", snapshot.entries, deadline);
        {
            const Handle staged = openFile(stage.path + '/' + manifestName, GENERIC_WRITE, CREATE_NEW, deadline, 0);
            writeAll(staged.value, encoded, "asset manifest", deadline);
            sync(staged.value, "asset manifest");
        }
        checkDeadline(deadline);
        move(stage.path + '/' + manifestName, manifestFile, false, "Cannot commit asset manifest");
        try {
            checkDeadline(deadline);
            move(stage.path + "/assets", assets, false, "Cannot atomically commit imported assets");
        } catch (...) {
            DeleteFileW(native(manifestFile).c_str());
            throw;
        }
        report.files = snapshot.entries.size();
        report.bytes = snapshot.bytes;
    } catch (const std::exception& error) { report.errors.append(QString::fromUtf8(error.what())); }
    return report;
}

Report verifyAssets(const QString& installRoot) {
    Report report;
    try {
        const QString install = absolute(installRoot);
        realDirectory(install, false, true);
        const Handle lock = lockRoot(install, false);
        const Entries expected = readManifest(install);
        if (!existsAt(install + "/assets")) {
            report.files = expected.size();
            for (auto i = expected.cbegin(); i != expected.cend(); ++i) {
                report.missing.append(i.key());
                report.bytes += i->size;
            }
        } else {
            realDirectory(install + "/assets");
            report = compare(scanTree(install + "/assets"), expected);
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
        realDirectory(installPath, false, true);
        const Handle lock = lockRoot(installPath, true);
        const Entries expected = readManifest(installPath);
        // Validate the entire selected tree before touching installation assets.
        const Scan sourceTree = scanTree(sourcePath);
        for (auto i = expected.cbegin(); i != expected.cend(); ++i) {
            const auto supplied = sourceTree.entries.constFind(i.key());
            if (supplied == sourceTree.entries.cend() || supplied->size != i->size || supplied->hash != i->hash)
                fail("Selected source differs from trusted manifest: " + i.key());
        }
        const QString assets = installPath + "/assets";
        realDirectory(assets, true);
        // Reject reparse-point/special targets; extras and intact files stay untouched.
        const Scan targetTree = scanTree(assets);
        Entries needed;
        for (auto i = expected.cbegin(); i != expected.cend(); ++i) {
            const auto target = targetTree.entries.constFind(i.key());
            if (target == targetTree.entries.cend() || target->size != i->size || target->hash != i->hash)
                needed.insert(i.key(), i.value());
        }
        Stage stage(installPath);
        copySnapshot(sourcePath, stage.path, needed);
        for (auto i = needed.cbegin(); i != needed.cend(); ++i) {
            realDirectory(QFileInfo(assets + '/' + i.key()).absolutePath(), true);
            move(stage.path + '/' + i.key(), assets + '/' + i.key(), true, "Cannot commit repaired asset " + i.key());
        }
        report = compare(scanTree(assets), expected);
    } catch (const std::exception& error) { report.errors.append(QString::fromUtf8(error.what())); }
    return report;
}
#endif
} // namespace ikv::launcher_assets
