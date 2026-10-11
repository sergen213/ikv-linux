#include "ikv/updates.hpp"

#include <archive.h>
#include <archive_entry.h>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/pem.h>

#include <algorithm>
#include <charconv>
#include <array>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <linux/fs.h>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string_view>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace ikv::updates {
namespace {
namespace fs = std::filesystem;
using Json = nlohmann::json;
constexpr std::uint64_t max_package = 1ULL << 30;
constexpr std::uint64_t max_file = 512ULL << 20;
constexpr std::uint64_t max_total = 4ULL << 30;
constexpr std::size_t max_manifest = 1U << 20;

[[noreturn]] void fail(const std::string& reason) { throw std::runtime_error("update: " + reason); }
void require(bool condition, const std::string& reason) { if (!condition) fail(reason); }
void system_check(bool success, const std::string& reason) {
    if (!success) fail(reason + ": " + std::strerror(errno));
}
struct Fd {
    int value{-1};
    explicit Fd(int fd) : value(fd) {}
    ~Fd() { if (value >= 0) ::close(value); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
};
struct Digest {
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx{EVP_MD_CTX_new(), EVP_MD_CTX_free};
    Digest() { require(ctx && EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) == 1, "SHA256 initialization failed"); }
    void add(const void* bytes, std::size_t count) {
        require(EVP_DigestUpdate(ctx.get(), bytes, count) == 1, "SHA256 update failed");
    }
    std::string finish() {
        std::array<unsigned char, EVP_MAX_MD_SIZE> bytes{};
        unsigned count = 0;
        require(EVP_DigestFinal_ex(ctx.get(), bytes.data(), &count) == 1 && count == 32, "SHA256 finalization failed");
        constexpr char hex[] = "0123456789abcdef";
        std::string result(64, '0');
        for (unsigned i = 0; i < count; ++i) {
            result[2 * i] = hex[bytes[i] >> 4];
            result[2 * i + 1] = hex[bytes[i] & 15];
        }
        return result;
    }
};

std::uint64_t regular_size(int fd) {
    struct stat st{};
    system_check(::fstat(fd, &st) == 0, "stat file");
    require(S_ISREG(st.st_mode) && st.st_size >= 0, "expected regular file");
    return static_cast<std::uint64_t>(st.st_size);
}
std::string read_bounded(const fs::path& path, std::size_t limit) {
    Fd fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    system_check(fd.value >= 0, "open " + path.string());
    require(regular_size(fd.value) <= limit, "file exceeds size limit: " + path.string());
    std::string bytes;
    std::array<char, 8192> buffer{};
    for (;;) {
        const auto n = ::read(fd.value, buffer.data(), buffer.size());
        if (n < 0 && errno == EINTR) continue;
        system_check(n >= 0, "read " + path.string());
        if (n == 0) break;
        require(bytes.size() + static_cast<std::size_t>(n) <= limit, "file grew beyond size limit");
        bytes.append(buffer.data(), static_cast<std::size_t>(n));
    }
    return bytes;
}
void write_all(int fd, const void* data, std::size_t count) {
    auto* bytes = static_cast<const char*>(data);
    while (count) {
        const auto n = ::write(fd, bytes, count);
        if (n < 0 && errno == EINTR) continue;
        system_check(n > 0, "write staged file");
        bytes += n;
        count -= static_cast<std::size_t>(n);
    }
}
void save_file(const fs::path& path, const std::string& bytes) {
    Fd fd(::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0644));
    system_check(fd.value >= 0, "create metadata");
    write_all(fd.value, bytes.data(), bytes.size());
    system_check(::fsync(fd.value) == 0, "sync metadata");
}
void sync_dir(const fs::path& path) {
    Fd fd(::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    system_check(fd.value >= 0 && ::fsync(fd.value) == 0, "sync directory " + path.string());
}
std::string hash_fd(int fd, std::uint64_t expected) {
    require(regular_size(fd) == expected, "file size mismatch");
    Digest digest;
    std::array<char, 65536> buffer{};
    std::uint64_t count = 0;
    for (;;) {
        const auto n = ::read(fd, buffer.data(), buffer.size());
        if (n < 0 && errno == EINTR) continue;
        system_check(n >= 0, "read hash input");
        if (!n) break;
        count += static_cast<std::uint64_t>(n);
        require(count <= expected, "hash input grew");
        digest.add(buffer.data(), static_cast<std::size_t>(n));
    }
    require(count == expected, "hash input truncated");
    return digest.finish();
}
bool ascii_name(std::string_view value, std::size_t max) {
    if (value.empty() || value.size() > max) return false;
    auto alnum = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); };
    if (!alnum(value.front())) return false;
    return std::all_of(value.begin(), value.end(), [&](char c) { return alnum(c) || c == '.' || c == '_' || c == '-'; });
}
bool valid_path(std::string_view path) {
    if (path.empty() || path.size() > 512) return false;
    std::size_t start = 0;
    while (start < path.size()) {
        const auto end = path.find('/', start);
        const auto component = path.substr(start, end == path.npos ? path.size() - start : end - start);
        if (component.empty() || component.size() > 128 || component == "." || component == ".." ||
            (start == 0 && component == ".ikv-update")) return false;
        for (char c : component) {
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) return false;
        }
        if (end == path.npos) return true;
        start = end + 1;
    }
    return false;
}
bool valid_hash(const std::string& hash) {
    return hash.size() == 64 && std::all_of(hash.begin(), hash.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
std::uint64_t bounded_uint(const Json& value, std::uint64_t max) {
    require(value.is_number_unsigned(), "expected unsigned integer");
    const auto result = value.get<std::uint64_t>();
    require(result <= max, "integer exceeds limit");
    return result;
}
void exact_keys(const Json& object, std::initializer_list<const char*> keys) {
    require(object.is_object() && object.size() == keys.size(), "unexpected manifest fields");
    for (auto key : keys) require(object.contains(key), std::string("missing manifest field: ") + key);
}
struct SignedManifest { Manifest manifest; std::string bytes; std::string signature; };
SignedManifest load_signed(const fs::path& manifest_path, const fs::path& signature_path, const fs::path& trusted_pem) {
    SignedManifest result;
    result.bytes = read_bounded(manifest_path, max_manifest);
    result.signature = read_bounded(signature_path, 64);
    require(!result.bytes.empty() && result.signature.size() == 64, "empty manifest or invalid signature size");
    const auto pem = read_bounded(trusted_pem, 16384);
    std::unique_ptr<BIO, decltype(&BIO_free)> bio(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())), BIO_free);
    require(static_cast<bool>(bio), "public key buffer failed");
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(PEM_read_bio_PUBKEY(bio.get(), nullptr, nullptr, nullptr), EVP_PKEY_free);
    require(key && EVP_PKEY_is_a(key.get(), "ED25519") == 1, "trusted PEM must be an Ed25519 public key");
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    require(ctx && EVP_DigestVerifyInit(ctx.get(), nullptr, nullptr, nullptr, key.get()) == 1 &&
        EVP_DigestVerify(ctx.get(), reinterpret_cast<const unsigned char*>(result.signature.data()), result.signature.size(),
                         reinterpret_cast<const unsigned char*>(result.bytes.data()), result.bytes.size()) == 1,
        "manifest Ed25519 signature verification failed");
    std::vector<std::set<std::string>> keys;
    const auto callback = [&](int depth, Json::parse_event_t event, Json& value) {
        require(depth <= 8, "manifest nesting limit exceeded");
        if (event == Json::parse_event_t::object_start) keys.emplace_back();
        if (event == Json::parse_event_t::key) require(!keys.empty() && keys.back().insert(value.get<std::string>()).second, "duplicate JSON key");
        if (event == Json::parse_event_t::object_end) keys.pop_back();
        return true;
    };
    const auto json = Json::parse(result.bytes, callback);
    exact_keys(json, {"schema", "version", "channel", "sequence", "package", "package_size", "package_sha256", "files"});
    require(bounded_uint(json.at("schema"), 2) == 2, "unsupported manifest schema");
    auto& m = result.manifest;
    m.version = json.at("version").get<std::string>();
    m.channel = json.at("channel").get<std::string>();
    m.sequence = bounded_uint(json.at("sequence"), std::numeric_limits<std::uint64_t>::max());
    require(m.sequence > 0, "release sequence must be positive");
    m.package = json.at("package").get<std::string>();
    m.package_sha256 = json.at("package_sha256").get<std::string>();
    m.package_size = bounded_uint(json.at("package_size"), max_package);
    require(ascii_name(m.version, 96) && ascii_name(m.package, 160), "unsafe release version or package basename");
    require(m.channel == "stable" || m.channel == "experimental", "unsupported release channel");
    require(m.package_size > 0 && valid_hash(m.package_sha256), "invalid package size or hash");
    const auto& files = json.at("files");
    require(files.is_array() && !files.empty() && files.size() <= 10000, "invalid manifest file count");
    m.files.reserve(files.size());
    std::set<std::string> paths;
    std::uint64_t total = 0;
    for (const auto& entry : files) {
        exact_keys(entry, {"path", "size", "sha256", "executable"});
        File file{entry.at("path").get<std::string>(), entry.at("sha256").get<std::string>(), bounded_uint(entry.at("size"), max_file), entry.at("executable").get<bool>()};
        require(valid_path(file.path) && valid_hash(file.sha256) && paths.insert(file.path).second, "unsafe/duplicate manifest path or hash");
        total += file.size;
        require(total <= max_total, "unpacked release exceeds limit");
        m.files.push_back(std::move(file));
    }
    for (const auto& file : m.files) {
        for (auto parent = fs::path(file.path).parent_path(); !parent.empty(); parent = parent.parent_path())
            require(!paths.contains(parent.generic_string()), "manifest file/directory collision");
    }
    return result;
}
void owned_directory(const fs::path& path) {
    struct stat st{};
    system_check(::lstat(path.c_str(), &st) == 0, "stat install directory");
    require(S_ISDIR(st.st_mode) && st.st_uid == ::geteuid() && !(st.st_mode & (S_IWGRP | S_IWOTH)), "install directories must be owned, non-symlink, and not group/world writable");
}
void trusted_ancestors(const fs::path& root) {
    for (auto parent = root.parent_path(); !parent.empty(); parent = parent.parent_path()) {
        struct stat st{};
        if (::lstat(parent.c_str(), &st) != 0) {
            if (errno == ENOENT) continue;
            system_check(false, "stat install ancestor");
        }
        require(S_ISDIR(st.st_mode) && (st.st_uid == 0 || st.st_uid == ::geteuid()) &&
                (!(st.st_mode & (S_IWGRP | S_IWOTH)) || (st.st_mode & S_ISVTX)),
                "install ancestors must be trusted non-symlink directories, not replaceable by other users");
        if (parent == parent.root_path()) break;
    }
}
struct InstallLock {
    fs::path root;
    Fd lock;
    explicit InstallLock(const fs::path& path) : root(fs::absolute(path).lexically_normal()), lock(-1) {
        require(!path.empty(), "empty install root");
        trusted_ancestors(root);
        fs::create_directories(root);
        trusted_ancestors(root);
        owned_directory(root);
        fs::create_directory(root / "releases");
        owned_directory(root / "releases");
        lock.value = ::open((root / ".update-lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
        system_check(lock.value >= 0, "open updater lock");
        struct stat st{};
        system_check(::fstat(lock.value, &st) == 0, "stat updater lock");
        require(S_ISREG(st.st_mode) && st.st_uid == ::geteuid() && st.st_nlink == 1 && !(st.st_mode & (S_IWGRP | S_IWOTH)), "unsafe updater lock");
        system_check(::flock(lock.value, LOCK_EX) == 0, "lock updater");
    }
};
struct Highwater {
    std::uint64_t sequence{};
    std::string manifest_sha256;
};
Highwater check_highwater(const fs::path& root, const Manifest& manifest, const std::string& hash) {
    const auto directory = root / ".update-highwater";
    const bool created = fs::create_directory(directory);
    owned_directory(directory);
    if (created) sync_dir(root);
    const auto path = directory / (manifest.channel + ".json");
    if (fs::symlink_status(path).type() == fs::file_type::not_found) return {};
    struct stat st{};
    system_check(::lstat(path.c_str(), &st) == 0, "stat update highwater");
    require(S_ISREG(st.st_mode) && st.st_uid == ::geteuid() && st.st_nlink == 1 &&
            !(st.st_mode & (S_IWGRP | S_IWOTH)), "unsafe update highwater");
    const auto json = Json::parse(read_bounded(path, 4096));
    exact_keys(json, {"sequence", "manifest_sha256"});
    Highwater result{bounded_uint(json.at("sequence"), std::numeric_limits<std::uint64_t>::max()),
                     json.at("manifest_sha256").get<std::string>()};
    require(result.sequence > 0 && valid_hash(result.manifest_sha256), "invalid update highwater");
    require(manifest.sequence >= result.sequence, "release sequence is below channel highwater");
    require(manifest.sequence != result.sequence || hash == result.manifest_sha256,
            "release sequence conflicts with authenticated manifest");
    return result;
}
void raise_highwater(const fs::path& root, const Manifest& manifest, const std::string& hash,
                     const Highwater& highwater) {
    const auto directory = root / ".update-highwater";
    // Retry a failed directory sync even when the atomic rename already made
    // the same floor visible; visibility alone is not durable acceptance.
    sync_dir(root);
    if (manifest.sequence == highwater.sequence) {
        sync_dir(directory);
        return;
    }
    std::string temporary = (directory / ".highwater-XXXXXX").string();
    Fd fd(::mkstemp(temporary.data()));
    system_check(fd.value >= 0, "create update highwater");
    try {
        const auto bytes = Json{{"sequence", manifest.sequence}, {"manifest_sha256", hash}}.dump();
        write_all(fd.value, bytes.data(), bytes.size());
        system_check(::fsync(fd.value) == 0, "sync update highwater");
        fs::rename(temporary, directory / (manifest.channel + ".json"));
        sync_dir(directory);
    } catch (...) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        throw;
    }
}
std::string link_version(const fs::path& root, const char* name, bool optional) {
    const auto path = root / name;
    const auto status = fs::symlink_status(path);
    if (status.type() == fs::file_type::not_found) {
        require(optional, std::string("missing ") + name + " release");
        return {};
    }
    require(status.type() == fs::file_type::symlink, std::string(name) + " must be a managed release symlink");
    const auto target = fs::read_symlink(path).generic_string();
    constexpr std::string_view prefix = "releases/";
    require(target.starts_with(prefix), "unsafe release pointer");
    const auto version = target.substr(prefix.size());
    require(ascii_name(version, 96) && target == "releases/" + version, "unsafe release pointer version");
    return version;
}
void switch_link(const fs::path& root, const char* name, const std::string& version) {
    std::string pattern = (root / (std::string(".") + name + "-XXXXXX")).string();
    Fd temporary(::mkstemp(pattern.data()));
    system_check(temporary.value >= 0, "reserve release pointer");
    system_check(::unlink(pattern.c_str()) == 0, "prepare release pointer");
    try {
        fs::create_symlink(fs::path("releases") / version, pattern);
        fs::rename(pattern, root / name);
        sync_dir(root);
    } catch (...) {
        std::error_code ignored;
        fs::remove(pattern, ignored);
        throw;
    }
}
void clear_transaction(const fs::path& root) {
    system_check(::unlink((root / ".update-transaction").c_str()) == 0, "clear update transaction");
    sync_dir(root);
}
void recover_transaction(const fs::path& root) {
    const auto path = root / ".update-transaction";
    if (fs::symlink_status(path).type() == fs::file_type::not_found) return;
    struct stat st{};
    system_check(::lstat(path.c_str(), &st) == 0, "stat update transaction");
    require(S_ISREG(st.st_mode) && st.st_uid == ::geteuid() && st.st_nlink == 1 &&
            !(st.st_mode & (S_IWGRP | S_IWOTH)), "unsafe update transaction journal");
    const auto json = Json::parse(read_bounded(path, 4096));
    exact_keys(json, {"version", "current", "previous"});
    require(ascii_name(json.at("version").get<std::string>(), 96), "unsafe transaction release");
    const auto current = json.at("current").get<std::string>();
    const auto previous = json.at("previous").get<std::string>();
    require((current.empty() || ascii_name(current, 96)) && (previous.empty() || ascii_name(previous, 96)), "unsafe transaction history");
    (void)link_version(root, "current", true);
    (void)link_version(root, "previous", true);
    auto restore = [&](const char* name, const std::string& version) {
        if (!version.empty()) switch_link(root, name, version);
        else { fs::remove(root / name); sync_dir(root); }
    };
    restore("previous", previous);
    restore("current", current);
    clear_transaction(root);
}
void begin_transaction(const fs::path& root, const std::string& version,
                       const std::string& current, const std::string& previous) {
    require(fs::symlink_status(root / ".update-transaction").type() == fs::file_type::not_found, "unrecovered update transaction");
    std::string temporary = (root / ".transaction-XXXXXX").string();
    Fd fd(::mkstemp(temporary.data()));
    system_check(fd.value >= 0, "create transaction journal");
    try {
        const auto bytes = Json{{"version", version}, {"current", current}, {"previous", previous}}.dump();
        write_all(fd.value, bytes.data(), bytes.size());
        system_check(::fsync(fd.value) == 0, "sync transaction journal");
        fs::rename(temporary, root / ".update-transaction");
        sync_dir(root);
    } catch (...) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        throw;
    }
}
std::set<std::string> expected_directories(const Manifest& manifest) {
    std::set<std::string> dirs;
    for (const auto& file : manifest.files)
        for (auto parent = fs::path(file.path).parent_path(); !parent.empty(); parent = parent.parent_path()) dirs.insert(parent.generic_string());
    return dirs;
}
// Authenticate the actual compressed bytes handed to libarchive, not a mutable
// inode's earlier contents; never read beyond the signed compressed size.
struct PackageStream {
    int fd;
    std::uint64_t remaining;
    Digest digest;
    std::array<char, 65536> buffer{};
    PackageStream(int source, std::uint64_t size) : fd(source), remaining(size) {}
    static la_ssize_t read(archive* reader, void* context, const void** output) {
        auto& stream = *static_cast<PackageStream*>(context);
        if (!stream.remaining) return 0;
        const auto wanted = static_cast<std::size_t>(std::min<std::uint64_t>(stream.remaining, stream.buffer.size()));
        ssize_t n;
        do { n = ::read(stream.fd, stream.buffer.data(), wanted); } while (n < 0 && errno == EINTR);
        if (n < 0 || (n > 0 && EVP_DigestUpdate(stream.digest.ctx.get(), stream.buffer.data(), static_cast<std::size_t>(n)) != 1)) {
            archive_set_error(reader, EIO, "compressed input read/hash failed");
            return -1;
        }
        stream.remaining -= static_cast<std::uint64_t>(n);
        *output = stream.buffer.data();
        return n;
    }
};
// Bound decompression BEFORE the tar parser can consume a huge extension header.
// Two streaming libarchive readers avoid a second on-disk copy of the package.
struct TarStream {
    archive* compressed;
    std::uint64_t remaining;
    std::array<char, 65536> buffer{};
    static la_ssize_t read(archive* reader, void* context, const void** output) {
        auto& stream = *static_cast<TarStream*>(context);
        const auto n = archive_read_data(stream.compressed, stream.buffer.data(), stream.buffer.size());
        if (n < 0) {
            archive_set_error(reader, EIO, "gzip decompression failed");
            return -1;
        }
        if (static_cast<std::uint64_t>(n) > stream.remaining) {
            archive_set_error(reader, EFBIG, "decompressed tar exceeds signed payload plus metadata bound");
            return -1;
        }
        stream.remaining -= static_cast<std::uint64_t>(n);
        *output = stream.buffer.data();
        return n;
    }
};
void extract_package(int package_fd, const Manifest& manifest, const fs::path& stage) {
    system_check(::lseek(package_fd, 0, SEEK_SET) == 0, "rewind package");
    std::unique_ptr<archive, decltype(&archive_read_free)> compressed(archive_read_new(), archive_read_free);
    std::unique_ptr<archive, decltype(&archive_read_free)> reader(archive_read_new(), archive_read_free);
    require(compressed && reader, "archive allocation failed");
    require(archive_read_support_filter_gzip(compressed.get()) == ARCHIVE_OK &&
            archive_read_support_format_raw(compressed.get()) == ARCHIVE_OK, "gzip initialization failed");
    PackageStream input(package_fd, manifest.package_size);
    require(archive_read_open(compressed.get(), &input, nullptr, PackageStream::read, nullptr) == ARCHIVE_OK, "cannot open bounded tar/gzip package");
    archive_entry* raw_entry = nullptr;
    require(archive_read_next_header(compressed.get(), &raw_entry) == ARCHIVE_OK, "empty package stream");
    std::uint64_t tar_limit = 64ULL << 20;
    for (const auto& file : manifest.files) tar_limit += file.size;
    TarStream stream{compressed.get(), tar_limit, {}};
    require(archive_read_support_filter_none(reader.get()) == ARCHIVE_OK &&
            archive_read_support_format_tar(reader.get()) == ARCHIVE_OK, "tar initialization failed");
    require(archive_read_open(reader.get(), &stream, nullptr, TarStream::read, nullptr) == ARCHIVE_OK, "cannot read bounded tar stream");
    std::map<std::string, const File*> expected;
    for (const auto& file : manifest.files) expected.emplace(file.path, &file);
    const auto dirs = expected_directories(manifest);
    std::set<std::string> seen;
    archive_entry* entry = nullptr;
    std::array<char, 65536> buffer{};
    std::size_t entries = 0;
    int status;
    while ((status = archive_read_next_header(reader.get(), &entry)) == ARCHIVE_OK) {
        require(++entries <= 50000, "archive entry count exceeds limit");
        const auto* raw = archive_entry_pathname(entry);
        require(raw != nullptr, "archive path missing");
        std::string path(raw);
        const auto type = archive_entry_filetype(entry);
        if (type == AE_IFDIR && path.ends_with('/')) path.pop_back();
        require(valid_path(path) && seen.insert(path).second, "unsafe or duplicate archive path");
        require(!archive_entry_symlink(entry) && !archive_entry_hardlink(entry) && !archive_entry_is_encrypted(entry), "links/encryption not allowed in update archive");
        require(archive_entry_sparse_count(entry) == 0 && archive_entry_xattr_count(entry) == 0, "sparse files/extended attributes not allowed");
        if (type == AE_IFDIR) {
            require(dirs.contains(path) && archive_entry_size(entry) == 0, "unlisted or nonempty archive directory");
            require(archive_read_data_skip(reader.get()) == ARCHIVE_OK, "invalid archive directory data");
            continue;
        }
        require(type == AE_IFREG && expected.contains(path), "special or unlisted archive file");
        const auto& file = *expected.at(path);
        require(archive_entry_size_is_set(entry) && archive_entry_size(entry) >= 0 && static_cast<std::uint64_t>(archive_entry_size(entry)) == file.size, "archive file size mismatch");
        require((archive_entry_perm(entry) & 07000) == 0 && static_cast<bool>(archive_entry_perm(entry) & 0111) == file.executable, "archive executable/privileged mode mismatch");
        const auto destination = stage / file.path;
        fs::create_directories(destination.parent_path());
        Fd output(::open(destination.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
        system_check(output.value >= 0, "create staged payload");
        Digest digest;
        std::uint64_t count = 0;
        for (;;) {
            const auto n = archive_read_data(reader.get(), buffer.data(), buffer.size());
            require(n >= 0, "archive data decompression failed");
            if (!n) break;
            count += static_cast<std::uint64_t>(n);
            require(count <= file.size, "archive file exceeds signed size");
            digest.add(buffer.data(), static_cast<std::size_t>(n));
            write_all(output.value, buffer.data(), static_cast<std::size_t>(n));
        }
        require(count == file.size && digest.finish() == file.sha256, "unpacked file hash mismatch");
        system_check(::fchmod(output.value, file.executable ? 0755 : 0644) == 0 && ::fsync(output.value) == 0, "set mode/sync payload");
        expected.erase(path);
    }
    require(status == ARCHIVE_EOF && expected.empty(), "truncated archive or missing signed files");
    require(archive_read_close(reader.get()) == ARCHIVE_OK, "archive close failed");
    // Drain even ignored tar padding through the same bound and gzip checksum check.
    const void* discarded = nullptr;
    la_ssize_t n;
    do { n = TarStream::read(reader.get(), &stream, &discarded); } while (n > 0);
    require(n == 0 && archive_read_close(compressed.get()) == ARCHIVE_OK, "invalid or oversized trailing package data");
    do { n = PackageStream::read(compressed.get(), &input, &discarded); } while (n > 0);
    require(n == 0 && input.remaining == 0 && input.digest.finish() == manifest.package_sha256, "actual extracted package SHA256/size mismatch");
    char extra;
    ssize_t tail;
    do { tail = ::read(package_fd, &extra, 1); } while (tail < 0 && errno == EINTR);
    require(tail == 0, "compressed package grew beyond signed size");
}
Manifest verify_release_locked(const fs::path& root, const std::string& version, const fs::path& trusted_pem) {
    require(ascii_name(version, 96), "unsafe release version");
    const auto release = root / "releases" / version;
    owned_directory(release);
    owned_directory(release / ".ikv-update");
    const auto signed_manifest = load_signed(release / ".ikv-update/manifest.json", release / ".ikv-update/signature.bin", trusted_pem);
    const auto& manifest = signed_manifest.manifest;
    require(manifest.version == version, "release directory/version mismatch");
    std::map<std::string, const File*> files;
    for (const auto& file : manifest.files) files.emplace(file.path, &file);
    auto dirs = expected_directories(manifest);
    dirs.insert(".ikv-update");
    for (const auto& entry : fs::recursive_directory_iterator(release)) {
        const auto relative = entry.path().lexically_relative(release).generic_string();
        const auto status = entry.symlink_status();
        if (status.type() == fs::file_type::directory) {
            require(dirs.contains(relative), "unlisted release directory");
            owned_directory(entry.path());
        } else {
            require(status.type() == fs::file_type::regular, "release contains link or special file");
            if (relative == ".ikv-update/manifest.json" || relative == ".ikv-update/signature.bin") continue;
            require(files.contains(relative), "unlisted release file");
            const auto& file = *files.at(relative);
            Fd fd(::open(entry.path().c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
            system_check(fd.value >= 0, "open installed file");
            struct stat st{};
            system_check(::fstat(fd.value, &st) == 0, "stat installed file");
            require(st.st_uid == ::geteuid() && st.st_nlink == 1 && !(st.st_mode & (S_IWGRP | S_IWOTH | 07000)) && static_cast<bool>(st.st_mode & 0111) == file.executable, "unsafe installed file mode/owner/links");
            require(hash_fd(fd.value, file.size) == file.sha256, "installed file hash mismatch");
            files.erase(relative);
        }
    }
    require(files.empty(), "installed release missing signed files");
    return manifest;
}
struct SemVer { std::array<std::uint64_t, 3> core{}; std::vector<std::string_view> pre; };
bool digits(std::string_view s) { return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; }); }
SemVer parse_version(std::string_view text) {
    const auto invalid = "invalid version (MAJOR.MINOR.PATCH[-PRERELEASE] required): " + std::string(text.substr(0, 96));
    require(text.size() <= 96, invalid);
    SemVer version;
    const auto dash = text.find('-');
    auto core = text.substr(0, dash);
    for (std::size_t i = 0; i < 3; ++i) {
        const auto dot = core.find('.');
        require((i < 2) == (dot != core.npos), invalid);
        const auto part = core.substr(0, dot);
        require(digits(part) && (part.size() == 1 || part.front() != '0') && part.size() <= 19, invalid);
        std::from_chars(part.data(), part.data() + part.size(), version.core[i]);
        core = dot == core.npos ? std::string_view{} : core.substr(dot + 1);
    }
    if (dash == text.npos) return version;
    for (auto pre = text.substr(dash + 1);;) {
        const auto dot = pre.find('.');
        const auto id = pre.substr(0, dot);
        require(!id.empty() && std::all_of(id.begin(), id.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-'; }), invalid);
        require(!digits(id) || id.size() == 1 || id.front() != '0', invalid);
        version.pre.push_back(id);
        if (dot == pre.npos) return version;
        pre = pre.substr(dot + 1);
    }
}
} // namespace

int compare_versions(std::string_view left, std::string_view right) {
    const auto a = parse_version(left), b = parse_version(right);
    if (a.core != b.core) return a.core < b.core ? -1 : 1;
    if (a.pre.empty() || b.pre.empty()) return a.pre.empty() == b.pre.empty() ? 0 : a.pre.empty() ? 1 : -1;
    for (std::size_t i = 0; i < std::min(a.pre.size(), b.pre.size()); ++i) {
        const auto x = a.pre[i], y = b.pre[i];
        if (x == y) continue;
        const bool xn = digits(x), yn = digits(y);
        if (xn != yn) return xn ? -1 : 1; // numeric identifiers sort before alphanumeric ones
        if (xn && x.size() != y.size()) return x.size() < y.size() ? -1 : 1; // no leading zeroes
        return x < y ? -1 : 1;
    }
    return a.pre.size() == b.pre.size() ? 0 : a.pre.size() < b.pre.size() ? -1 : 1;
}

Manifest verify_manifest(const fs::path& manifest, const fs::path& signature, const fs::path& trusted_pem) {
    return load_signed(manifest, signature, trusted_pem).manifest;
}
Manifest install(const fs::path& manifest_path, const fs::path& signature_path, const fs::path& package_path,
                 const fs::path& trusted_pem, const fs::path& install_root) {
    const auto signed_manifest = load_signed(manifest_path, signature_path, trusted_pem);
    const auto& manifest = signed_manifest.manifest;
    require(package_path.filename() == manifest.package, "package basename mismatch");
    Fd package(::open(package_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    system_check(package.value >= 0, "open package");
    // Reject unauthenticated packages before parsing; the extraction callback
    // independently authenticates the reread bytes to close mutable-input races.
    require(hash_fd(package.value, manifest.package_size) == manifest.package_sha256, "package SHA256 mismatch");
    InstallLock installation(install_root);
    const auto& root = installation.root;
    recover_transaction(root);
    Digest manifest_digest;
    manifest_digest.add(signed_manifest.bytes.data(), signed_manifest.bytes.size());
    const auto manifest_hash = manifest_digest.finish();
    const auto highwater = check_highwater(root, manifest, manifest_hash);
    const auto current = link_version(root, "current", true);
    const auto previous = link_version(root, "previous", true);
    const auto target = root / "releases" / manifest.version;
    const bool existing = fs::symlink_status(target).type() != fs::file_type::not_found;
    fs::path stage;
    bool committed = false;
    if (existing) {
        (void)verify_release_locked(root, manifest.version, trusted_pem);
        require(read_bounded(target / ".ikv-update/manifest.json", max_manifest) == signed_manifest.bytes &&
                read_bounded(target / ".ikv-update/signature.bin", 64) == signed_manifest.signature,
                "existing version differs from signed update; versions are never overwritten");
        if (current == manifest.version) {
            raise_highwater(root, manifest, manifest_hash, highwater);
            return manifest;
        }
    } else {
        std::string temporary = (root / "releases/.staging-XXXXXX").string();
        system_check(::mkdtemp(temporary.data()) != nullptr, "create isolated staging directory");
        stage = temporary;
    }
    bool transaction = false;
    try {
        if (!existing) {
            extract_package(package.value, manifest, stage);
            fs::create_directory(stage / ".ikv-update");
            save_file(stage / ".ikv-update/manifest.json", signed_manifest.bytes);
            save_file(stage / ".ikv-update/signature.bin", signed_manifest.signature);
            for (const auto& entry : fs::recursive_directory_iterator(stage)) {
                if (entry.is_directory()) {
                    system_check(::chmod(entry.path().c_str(), 0755) == 0, "set payload directory mode");
                    sync_dir(entry.path());
                }
            }
            system_check(::chmod(stage.c_str(), 0755) == 0, "set release directory mode");
            sync_dir(stage);
        }
        transaction = true;
        begin_transaction(root, manifest.version, current, previous);
        if (!existing) {
            fs::rename(stage, target);
            committed = true;
            sync_dir(root / "releases");
        }
        // This independently durable freshness floor is never undone by pointer
        // journal recovery: a failed cutover may only resume these same signed bytes.
        raise_highwater(root, manifest, manifest_hash, highwater);
        if (!current.empty()) switch_link(root, "previous", current);
        switch_link(root, "current", manifest.version);
        clear_transaction(root);
        transaction = false;
    } catch (...) {
        if (!existing && !committed) { std::error_code ignored; fs::remove_all(stage, ignored); }
        if (transaction) recover_transaction(root);
        throw;
    }
    return manifest;
}
Manifest verify_release(const fs::path& install_root, const std::string& version, const fs::path& trusted_pem) {
    InstallLock installation(install_root);
    recover_transaction(installation.root);
    return verify_release_locked(installation.root, version, trusted_pem);
}
Manifest rollback(const fs::path& install_root, const fs::path& trusted_pem) {
    InstallLock installation(install_root);
    const auto& root = installation.root;
    recover_transaction(root);
    const auto current = link_version(root, "current", false);
    const auto previous = link_version(root, "previous", false);
    require(current != previous, "no distinct rollback release");
    const auto manifest = verify_release_locked(root, previous, trusted_pem);
    system_check(::syscall(SYS_renameat2, AT_FDCWD, (root / "current").c_str(),
                          AT_FDCWD, (root / "previous").c_str(), RENAME_EXCHANGE) == 0,
                 "atomically exchange rollback pointers");
    sync_dir(root);
    return manifest;
}
} // namespace ikv::updates
