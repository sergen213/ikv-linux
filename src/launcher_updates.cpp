#include "ikv/updates.hpp"

#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string_view>
#include <vector>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

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

// ---- Platform file primitives: no-follow regular-file reads, exclusive creation, durable writes ----
#ifdef _WIN32
// NONCANONICAL (Windows self-update): per-user %LOCALAPPDATA% ACLs replace POSIX owner/mode checks; reparse points
// (symlinks, junctions) are refused everywhere instead.
using Native = HANDLE;
[[noreturn]] void win_fail(const std::string& reason) { fail(reason + ": Windows error " + std::to_string(GetLastError())); }
struct Fd {
    HANDLE value{INVALID_HANDLE_VALUE};
    explicit Fd(HANDLE handle) : value(handle) {}
    Fd(Fd&& other) noexcept : value(std::exchange(other.value, INVALID_HANDLE_VALUE)) {}
    ~Fd() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
};
BY_HANDLE_FILE_INFORMATION file_info(HANDLE handle) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle, &info)) win_fail("stat file");
    return info;
}
Fd open_regular(const fs::path& path) {
    Fd fd(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (fd.value == INVALID_HANDLE_VALUE) win_fail("open " + path.string());
    const auto info = file_info(fd.value);
    require(GetFileType(fd.value) == FILE_TYPE_DISK && !(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)),
            "expected regular file: " + path.string());
    return fd;
}
Fd create_exclusive(const fs::path& path, bool) {
    Fd fd(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (fd.value == INVALID_HANDLE_VALUE) win_fail("create " + path.string());
    return fd;
}
std::uint64_t regular_size(HANDLE handle) {
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle, &size) || size.QuadPart < 0) win_fail("stat file size");
    return static_cast<std::uint64_t>(size.QuadPart);
}
std::size_t read_some(HANDLE handle, char* buffer, std::size_t count) {
    DWORD got = 0;
    if (!ReadFile(handle, buffer, static_cast<DWORD>(std::min<std::size_t>(count, 1U << 20)), &got, nullptr)) win_fail("read file");
    return got;
}
void write_all(HANDLE handle, const void* data, std::size_t count) {
    auto* bytes = static_cast<const char*>(data);
    while (count) {
        DWORD written = 0;
        if (!WriteFile(handle, bytes, static_cast<DWORD>(std::min<std::size_t>(count, 1U << 20)), &written, nullptr) || !written) win_fail("write staged file");
        bytes += written;
        count -= written;
    }
}
void finish_file(HANDLE handle, bool) { if (!FlushFileBuffers(handle)) win_fail("sync file"); }
void rewind(HANDLE handle) { if (!SetFilePointerEx(handle, LARGE_INTEGER{}, nullptr, FILE_BEGIN)) win_fail("rewind package"); }
#else
using Native = int;
void system_check(bool success, const std::string& reason) {
    if (!success) fail(reason + ": " + std::strerror(errno));
}
struct Fd {
    int value{-1};
    explicit Fd(int fd) : value(fd) {}
    Fd(Fd&& other) noexcept : value(std::exchange(other.value, -1)) {}
    ~Fd() { if (value >= 0) ::close(value); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
};
Fd open_regular(const fs::path& path) {
    Fd fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    system_check(fd.value >= 0, "open " + path.string());
    return fd;
}
Fd create_exclusive(const fs::path& path, bool) {
    Fd fd(::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
    system_check(fd.value >= 0, "create " + path.string());
    return fd;
}
std::uint64_t regular_size(int fd) {
    struct stat st{};
    system_check(::fstat(fd, &st) == 0, "stat file");
    require(S_ISREG(st.st_mode) && st.st_size >= 0, "expected regular file");
    return static_cast<std::uint64_t>(st.st_size);
}
std::size_t read_some(int fd, char* buffer, std::size_t count) {
    for (;;) {
        const auto n = ::read(fd, buffer, count);
        if (n < 0 && errno == EINTR) continue;
        system_check(n >= 0, "read file");
        return static_cast<std::size_t>(n);
    }
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
// Signed executable flags normalize payload modes to 0755/0644.
void finish_file(int fd, bool executable) {
    system_check(::fchmod(fd, executable ? 0755 : 0644) == 0 && ::fsync(fd) == 0, "set mode/sync file");
}
void rewind(int fd) { system_check(::lseek(fd, 0, SEEK_SET) == 0, "rewind package"); }
void sync_dir(const fs::path& path) {
    Fd fd(::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    system_check(fd.value >= 0 && ::fsync(fd.value) == 0, "sync directory " + path.string());
}
#endif

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
std::string sha256_hex(std::string_view bytes) {
    Digest digest;
    digest.add(bytes.data(), bytes.size());
    return digest.finish();
}
std::string read_bounded(const fs::path& path, std::size_t limit) {
    const auto fd = open_regular(path);
    require(regular_size(fd.value) <= limit, "file exceeds size limit: " + path.string());
    std::string bytes;
    std::array<char, 8192> buffer{};
    while (const auto n = read_some(fd.value, buffer.data(), buffer.size())) {
        require(bytes.size() + n <= limit, "file grew beyond size limit");
        bytes.append(buffer.data(), n);
    }
    return bytes;
}
void save_file(const fs::path& path, const std::string& bytes) {
    const auto fd = create_exclusive(path, false);
    write_all(fd.value, bytes.data(), bytes.size());
    finish_file(fd.value, false);
}
std::string hash_fd(Native fd, std::uint64_t expected) {
    require(regular_size(fd) == expected, "file size mismatch");
    Digest digest;
    std::array<char, 65536> buffer{};
    std::uint64_t count = 0;
    while (const auto n = read_some(fd, buffer.data(), buffer.size())) {
        count += n;
        require(count <= expected, "hash input grew");
        digest.add(buffer.data(), n);
    }
    require(count == expected, "hash input truncated");
    return digest.finish();
}
#ifdef _WIN32 // unique staging/pointer names (POSIX uses mkstemp/mkdtemp)
std::string random_token() {
    std::array<unsigned char, 8> raw{};
    require(RAND_bytes(raw.data(), static_cast<int>(raw.size())) == 1, "random name failed");
    constexpr char hex[] = "0123456789abcdef";
    std::string token;
    for (const auto byte : raw) { token += hex[byte >> 4]; token += hex[byte & 15]; }
    return token;
}
#endif
bool ascii_name(std::string_view value, std::size_t max) {
    if (value.empty() || value.size() > max) return false;
    auto alnum = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); };
    if (!alnum(value.front())) return false;
    return std::all_of(value.begin(), value.end(), [&](char c) { return alnum(c) || c == '.' || c == '_' || c == '-'; });
}
// Components: letters, digits, '.', '_', '-' and '+' (libstdc++-6.dll, PostgreSQL's Etc/GMT+1).
bool valid_path(std::string_view path) {
    if (path.empty() || path.size() > 512) return false;
    std::size_t start = 0;
    while (start < path.size()) {
        const auto end = path.find('/', start);
        const auto component = path.substr(start, end == path.npos ? path.size() - start : end - start);
        if (component.empty() || component.size() > 128 || component == "." || component == ".." ||
            (start == 0 && component == ".ikv-update")) return false;
        for (char c : component) {
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' || c == '+')) return false;
        }
        if (end == path.npos) return true;
        start = end + 1;
    }
    return false;
}
// Windows packages additionally avoid DOS device names, trailing dots and case-insensitive collisions.
bool windows_component(std::string_view component) {
    if (component.back() == '.') return false;
    std::string stem(component.substr(0, component.find('.')));
    for (auto& c : stem) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    static const std::set<std::string> reserved{"CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7",
                                                "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
    return !reserved.contains(stem);
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
    auto& m = result.manifest;
    // Linux manifests have exactly the eight schema-2 fields (every launcher since 0.4 rejects others); a Windows
    // package names its platform, so neither launcher can install the other's payload.
    if (json.is_object() && json.contains("platform")) {
        exact_keys(json, {"schema", "version", "channel", "sequence", "package", "package_size", "package_sha256", "files", "platform"});
        m.platform = json.at("platform").get<std::string>();
        require(m.platform == "windows", "unsupported release platform");
    } else {
        exact_keys(json, {"schema", "version", "channel", "sequence", "package", "package_size", "package_sha256", "files"});
    }
    require(bounded_uint(json.at("schema"), 2) == 2, "unsupported manifest schema");
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
    std::set<std::string> paths, folded;
    std::uint64_t total = 0;
    for (const auto& entry : files) {
        exact_keys(entry, {"path", "size", "sha256", "executable"});
        File file{entry.at("path").get<std::string>(), entry.at("sha256").get<std::string>(), bounded_uint(entry.at("size"), max_file), entry.at("executable").get<bool>()};
        require(valid_path(file.path) && valid_hash(file.sha256) && paths.insert(file.path).second, "unsafe/duplicate manifest path or hash");
        if (m.platform == "windows") {
            auto lower = file.path;
            for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            require(folded.insert(lower).second, "manifest paths collide case-insensitively");
            for (const auto& component : fs::path(file.path)) require(windows_component(component.string()), "manifest path is not a safe Windows name");
        }
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
std::set<std::string> expected_directories(const Manifest& manifest) {
    std::set<std::string> dirs;
    for (const auto& file : manifest.files)
        for (auto parent = fs::path(file.path).parent_path(); !parent.empty(); parent = parent.parent_path()) dirs.insert(parent.generic_string());
    return dirs;
}

// ---- Bounded tar/gzip extraction (POSIX ustar only), shared by both platforms ----
// Authenticates the actual compressed bytes handed to zlib, never reads beyond the signed compressed size, and bounds
// decompressed tar bytes to the signed payload plus 64 MiB before any header is parsed.
class PackageStream {
public:
    PackageStream(Native fd, const Manifest& manifest) : fd_(fd), remaining_(manifest.package_size) {
        tar_remaining_ = 64ULL << 20;
        for (const auto& file : manifest.files) tar_remaining_ += file.size;
        require(inflateInit2(&zlib_, 16 + MAX_WBITS) == Z_OK, "gzip initialization failed");
    }
    ~PackageStream() { inflateEnd(&zlib_); }
    PackageStream(const PackageStream&) = delete;
    PackageStream& operator=(const PackageStream&) = delete;
    // Up to count decompressed bytes; 0 only at the end of the gzip member.
    std::size_t read(char* out, std::size_t count) {
        if (ended_ || !count) return 0;
        zlib_.next_out = reinterpret_cast<Bytef*>(out);
        zlib_.avail_out = static_cast<uInt>(std::min<std::size_t>(count, 1U << 20));
        const auto wanted = zlib_.avail_out;
        while (zlib_.avail_out == wanted) {
            if (!zlib_.avail_in) {
                require(remaining_ > 0, "truncated gzip package");
                const auto got = read_some(fd_, input_.data(), static_cast<std::size_t>(std::min<std::uint64_t>(remaining_, input_.size())));
                require(got > 0, "package shorter than signed size");
                remaining_ -= got;
                digest_.add(input_.data(), got);
                zlib_.next_in = reinterpret_cast<Bytef*>(input_.data());
                zlib_.avail_in = static_cast<uInt>(got);
            }
            const int status = inflate(&zlib_, Z_NO_FLUSH);
            if (status == Z_STREAM_END) { ended_ = true; break; }
            require(status == Z_OK || status == Z_BUF_ERROR, "gzip decompression failed");
        }
        const std::size_t produced = wanted - zlib_.avail_out;
        require(produced <= tar_remaining_, "decompressed tar exceeds signed payload plus metadata bound");
        tar_remaining_ -= produced;
        return produced;
    }
    void read_exact(char* out, std::size_t count) {
        while (count) {
            const auto n = read(out, count);
            require(n > 0, "truncated tar archive");
            out += n;
            count -= n;
        }
    }
    // Ignored tar padding still passes the same bound and the gzip CRC/size check; then the compressed bytes must be
    // exactly the signed package.
    void finish(const std::string& sha256) {
        std::array<char, 65536> scratch{};
        while (const auto n = read(scratch.data(), scratch.size()))
            require(std::all_of(scratch.begin(), scratch.begin() + static_cast<std::ptrdiff_t>(n), [](char c) { return c == 0; }), "invalid trailing tar data");
        require(zlib_.avail_in == 0 && remaining_ == 0, "invalid or oversized trailing package data");
        require(digest_.finish() == sha256, "actual extracted package SHA256/size mismatch");
        char extra;
        require(read_some(fd_, &extra, 1) == 0, "compressed package grew beyond signed size");
    }
private:
    Native fd_;
    std::uint64_t remaining_, tar_remaining_;
    z_stream zlib_{};
    bool ended_ = false;
    Digest digest_;
    std::array<char, 65536> input_{};
};
std::uint64_t octal(const char* field, std::size_t size) {
    std::size_t i = 0;
    while (i < size && field[i] == ' ') ++i;
    std::uint64_t value = 0;
    bool any = false;
    for (; i < size && field[i] >= '0' && field[i] <= '7'; ++i, any = true) {
        require(value <= (std::numeric_limits<std::uint64_t>::max() >> 3), "tar number overflow");
        value = value * 8 + static_cast<std::uint64_t>(field[i] - '0');
    }
    for (; i < size; ++i) require(field[i] == ' ' || field[i] == '\0', "invalid tar number field");
    require(any, "empty tar number field");
    return value;
}
std::string tar_string(const char* field, std::size_t size) { return std::string(field, strnlen(field, size)); }
void extract_package(Native package_fd, const Manifest& manifest, const fs::path& stage) {
    rewind(package_fd);
    PackageStream stream(package_fd, manifest);
    std::map<std::string, const File*> expected;
    for (const auto& file : manifest.files) expected.emplace(file.path, &file);
    const auto dirs = expected_directories(manifest);
    std::set<std::string> seen;
    std::array<char, 512> header{};
    std::array<char, 65536> buffer{};
    for (std::size_t entries = 0;; ++entries) {
        require(entries <= 50000, "archive entry count exceeds limit");
        stream.read_exact(header.data(), header.size());
        if (std::all_of(header.begin(), header.end(), [](char c) { return c == 0; })) break;
        std::uint64_t sum = 0;
        for (std::size_t i = 0; i < header.size(); ++i) sum += (i >= 148 && i < 156) ? ' ' : static_cast<unsigned char>(header[i]);
        require(octal(header.data() + 148, 8) == sum, "tar header checksum mismatch");
        require(std::memcmp(header.data() + 257, "ustar\0" "00", 8) == 0, "update archives must be POSIX ustar");
        const auto name = tar_string(header.data(), 100), prefix = tar_string(header.data() + 345, 155);
        std::string path = prefix.empty() ? name : prefix + "/" + name;
        const char type = header[156];
        require(tar_string(header.data() + 157, 100).empty() && (type == '0' || type == '\0' || type == '5'),
                "links, special files and tar extensions are not allowed in update archive");
        const auto mode = octal(header.data() + 100, 8);
        const auto size = octal(header.data() + 124, 12);
        if (type == '5' && path.ends_with('/')) path.pop_back();
        require(valid_path(path) && seen.insert(path).second, "unsafe or duplicate archive path");
        if (type == '5') {
            require(dirs.contains(path) && size == 0, "unlisted or nonempty archive directory");
            continue;
        }
        require(expected.contains(path), "special or unlisted archive file");
        const auto& file = *expected.at(path);
        require(size == file.size, "archive file size mismatch");
        require((mode & 07000) == 0 && static_cast<bool>(mode & 0111) == file.executable, "archive executable/privileged mode mismatch");
        const auto destination = stage / fs::path(file.path);
        fs::create_directories(destination.parent_path());
        const auto output = create_exclusive(destination, file.executable);
        Digest digest;
        for (std::uint64_t left = size; left;) {
            const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(left, buffer.size()));
            stream.read_exact(buffer.data(), n);
            digest.add(buffer.data(), n);
            write_all(output.value, buffer.data(), n);
            left -= n;
        }
        require(digest.finish() == file.sha256, "unpacked file hash mismatch");
        finish_file(output.value, file.executable);
        if (const auto padding = (512 - size % 512) % 512) stream.read_exact(buffer.data(), static_cast<std::size_t>(padding));
        expected.erase(path);
    }
    require(expected.empty(), "truncated archive or missing signed files");
    stream.finish(manifest.package_sha256);
}

#ifdef _WIN32
// ---- Windows installation layout: releases/<version>, one atomic current.json {"current","previous"} pointer ----
void plain_directory(const fs::path& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    require(attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT),
            "install directories must be real directories, not links or junctions: " + path.string());
}
void replace_file(const fs::path& source, const fs::path& target) {
    if (!MoveFileExW(source.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const auto error = GetLastError();
        std::error_code ignored;
        fs::remove(source, ignored);
        SetLastError(error);
        win_fail("replace " + target.string());
    }
}
struct InstallLock {
    fs::path root;
    Fd lock{INVALID_HANDLE_VALUE};
    explicit InstallLock(const fs::path& path) : root(fs::absolute(path).lexically_normal()) {
        require(!path.empty(), "empty install root");
        fs::create_directories(root);
        plain_directory(root);
        fs::create_directory(root / "releases");
        plain_directory(root / "releases");
        lock.value = CreateFileW((root / ".update-lock").c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                 OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (lock.value == INVALID_HANDLE_VALUE) win_fail("open updater lock");
        require(!(file_info(lock.value).dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT), "unsafe updater lock");
        OVERLAPPED overlapped{};
        if (!LockFileEx(lock.value, LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0, &overlapped)) win_fail("lock updater");
    }
};
struct Highwater {
    std::uint64_t sequence{};
    std::string manifest_sha256;
};
Highwater check_highwater(const fs::path& root, const Manifest& manifest, const std::string& hash) {
    const auto directory = root / ".update-highwater";
    fs::create_directory(directory);
    plain_directory(directory);
    const auto path = directory / (manifest.channel + ".json");
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND) return {};
    const auto json = Json::parse(read_bounded(path, 4096));
    exact_keys(json, {"sequence", "manifest_sha256"});
    Highwater result{bounded_uint(json.at("sequence"), std::numeric_limits<std::uint64_t>::max()), json.at("manifest_sha256").get<std::string>()};
    require(result.sequence > 0 && valid_hash(result.manifest_sha256), "invalid update highwater");
    require(manifest.sequence >= result.sequence, "release sequence is below channel highwater");
    require(manifest.sequence != result.sequence || hash == result.manifest_sha256, "release sequence conflicts with authenticated manifest");
    return result;
}
void raise_highwater(const fs::path& root, const Manifest& manifest, const std::string& hash, const Highwater& highwater) {
    if (manifest.sequence == highwater.sequence) return;
    const auto directory = root / ".update-highwater";
    const auto temporary = directory / (".highwater-" + random_token());
    save_file(temporary, Json{{"sequence", manifest.sequence}, {"manifest_sha256", hash}}.dump());
    replace_file(temporary, directory / (manifest.channel + ".json"));
}
struct Pointers { std::string current, previous; };
Pointers read_pointers(const fs::path& root) {
    const auto path = root / "current.json";
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND) return {};
    const auto json = Json::parse(read_bounded(path, 4096));
    exact_keys(json, {"current", "previous"});
    Pointers result{json.at("current").get<std::string>(), json.at("previous").get<std::string>()};
    require(ascii_name(result.current, 96) && (result.previous.empty() || ascii_name(result.previous, 96)), "unsafe release pointer");
    return result;
}
// One file holds both pointers, so install and rollback switch them in a single atomic replace (no journal needed).
void write_pointers(const fs::path& root, const Pointers& pointers) {
    const auto temporary = root / (".current-" + random_token());
    save_file(temporary, Json{{"current", pointers.current}, {"previous", pointers.previous}}.dump());
    replace_file(temporary, root / "current.json");
}
Manifest verify_release_locked(const fs::path& root, const std::string& version, const fs::path& trusted_pem) {
    require(ascii_name(version, 96), "unsafe release version");
    const auto release = root / "releases" / version;
    plain_directory(release);
    plain_directory(release / ".ikv-update");
    const auto signed_manifest = load_signed(release / ".ikv-update/manifest.json", release / ".ikv-update/signature.bin", trusted_pem);
    const auto& manifest = signed_manifest.manifest;
    require(manifest.version == version, "release directory/version mismatch");
    require(manifest.platform == platform, "installed release is for another platform");
    std::map<std::string, const File*> files;
    for (const auto& file : manifest.files) files.emplace(file.path, &file);
    auto dirs = expected_directories(manifest);
    dirs.insert(".ikv-update");
    // Manual recursion checks every entry's attributes before descending, so a junction is never followed.
    const std::function<void(const fs::path&)> walk = [&](const fs::path& directory) {
        for (const auto& entry : fs::directory_iterator(directory)) {
            const auto relative = entry.path().lexically_relative(release).generic_string();
            const DWORD attributes = GetFileAttributesW(entry.path().c_str());
            require(attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT), "release contains a link or junction");
            if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
                require(dirs.contains(relative), "unlisted release directory");
                walk(entry.path());
                continue;
            }
            if (relative == ".ikv-update/manifest.json" || relative == ".ikv-update/signature.bin") continue;
            require(files.contains(relative), "unlisted release file");
            const auto& file = *files.at(relative);
            const auto fd = open_regular(entry.path());
            require(file_info(fd.value).nNumberOfLinks == 1, "installed file has hard links");
            require(hash_fd(fd.value, file.size) == file.sha256, "installed file hash mismatch");
            files.erase(relative);
        }
    };
    walk(release);
    require(files.empty(), "installed release missing signed files");
    return manifest;
}
Manifest install_locked(const SignedManifest& signed_manifest, Native package, const fs::path& trusted_pem, const fs::path& root) {
    const auto& manifest = signed_manifest.manifest;
    const auto manifest_hash = sha256_hex(signed_manifest.bytes);
    const auto highwater = check_highwater(root, manifest, manifest_hash);
    const auto pointers = read_pointers(root);
    const auto target = root / "releases" / manifest.version;
    const bool existing = GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (existing) {
        (void)verify_release_locked(root, manifest.version, trusted_pem);
        require(read_bounded(target / ".ikv-update/manifest.json", max_manifest) == signed_manifest.bytes &&
                read_bounded(target / ".ikv-update/signature.bin", 64) == signed_manifest.signature,
                "existing version differs from signed update; versions are never overwritten");
    } else {
        const auto stage = root / "releases" / (".staging-" + random_token());
        if (!CreateDirectoryW(stage.c_str(), nullptr)) win_fail("create isolated staging directory");
        try {
            extract_package(package, manifest, stage);
            fs::create_directory(stage / ".ikv-update");
            save_file(stage / ".ikv-update/manifest.json", signed_manifest.bytes);
            save_file(stage / ".ikv-update/signature.bin", signed_manifest.signature);
            if (!MoveFileExW(stage.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH)) win_fail("publish release directory");
        } catch (...) {
            std::error_code ignored;
            fs::remove_all(stage, ignored);
            throw;
        }
    }
    // The freshness floor is raised before the pointer switch; an interrupted switch only resumes these signed bytes.
    raise_highwater(root, manifest, manifest_hash, highwater);
    if (pointers.current != manifest.version)
        write_pointers(root, {manifest.version, pointers.current.empty() ? pointers.previous : pointers.current});
    return manifest;
}
Manifest rollback_locked(const fs::path& root, const fs::path& trusted_pem) {
    const auto pointers = read_pointers(root);
    require(!pointers.current.empty() && !pointers.previous.empty() && pointers.current != pointers.previous, "no distinct rollback release");
    const auto manifest = verify_release_locked(root, pointers.previous, trusted_pem);
    write_pointers(root, {pointers.previous, pointers.current});
    return manifest;
}
std::string current_locked(const fs::path& root) { return read_pointers(root).current; }
void recover_transaction(const fs::path&) {}
#else
// ---- Linux installation layout: releases/<version>, current/previous symlinks, durable transaction journal ----
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
Manifest verify_release_locked(const fs::path& root, const std::string& version, const fs::path& trusted_pem) {
    require(ascii_name(version, 96), "unsafe release version");
    const auto release = root / "releases" / version;
    owned_directory(release);
    owned_directory(release / ".ikv-update");
    const auto signed_manifest = load_signed(release / ".ikv-update/manifest.json", release / ".ikv-update/signature.bin", trusted_pem);
    const auto& manifest = signed_manifest.manifest;
    require(manifest.version == version, "release directory/version mismatch");
    require(manifest.platform == platform, "installed release is for another platform");
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
            const auto fd = open_regular(entry.path());
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
Manifest install_locked(const SignedManifest& signed_manifest, Native package, const fs::path& trusted_pem, const fs::path& root) {
    const auto& manifest = signed_manifest.manifest;
    const auto manifest_hash = sha256_hex(signed_manifest.bytes);
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
            extract_package(package, manifest, stage);
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
Manifest rollback_locked(const fs::path& root, const fs::path& trusted_pem) {
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
std::string current_locked(const fs::path& root) { return link_version(root, "current", true); }
#endif

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
    require(manifest.platform == platform, "signed package is for another platform (" + manifest.platform + ")");
    require(package_path.filename() == manifest.package, "package basename mismatch");
    const auto package = open_regular(package_path);
    // Reject unauthenticated packages before parsing; the extraction stream
    // independently authenticates the reread bytes to close mutable-input races.
    require(hash_fd(package.value, manifest.package_size) == manifest.package_sha256, "package SHA256 mismatch");
    InstallLock installation(install_root);
    recover_transaction(installation.root);
    return install_locked(signed_manifest, package.value, trusted_pem, installation.root);
}
Manifest verify_release(const fs::path& install_root, const std::string& version, const fs::path& trusted_pem) {
    InstallLock installation(install_root);
    recover_transaction(installation.root);
    return verify_release_locked(installation.root, version, trusted_pem);
}
Manifest rollback(const fs::path& install_root, const fs::path& trusted_pem) {
    InstallLock installation(install_root);
    recover_transaction(installation.root);
    return rollback_locked(installation.root, trusted_pem);
}
std::string current_release(const fs::path& install_root) {
    InstallLock installation(install_root);
    recover_transaction(installation.root);
    return current_locked(installation.root);
}
} // namespace ikv::updates
