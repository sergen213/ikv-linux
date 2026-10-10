#include "ikv/https_service.hpp"
#include "ikv/store.hpp"

#include <charconv>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

namespace {
struct File {
    int fd;
    ~File() { if (fd >= 0) ::close(fd); }
};

std::string read_dsn(const std::filesystem::path& path) {
    File file{::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW)};
    if (file.fd < 0) throw std::runtime_error("DSN file cannot be opened");
    struct stat info{};
    if (::fstat(file.fd, &info) != 0 || !S_ISREG(info.st_mode) ||
        (info.st_mode & 0777) != 0600 || info.st_uid != ::geteuid() ||
        info.st_size <= 0 || info.st_size > 65536)
        throw std::runtime_error("DSN file must be an owned regular 0600 file of at most 64 KiB");
    std::string dsn(static_cast<std::size_t>(info.st_size), '\0');
    std::size_t offset = 0;
    while (offset < dsn.size()) {
        const auto count = ::read(file.fd, dsn.data() + offset, dsn.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) throw std::runtime_error("DSN file could not be fully read");
        offset += static_cast<std::size_t>(count);
    }
    while (!dsn.empty() && std::isspace(static_cast<unsigned char>(dsn.back()))) dsn.pop_back();
    if (dsn.empty() || dsn.find('\0') != std::string::npos)
        throw std::runtime_error("DSN file must contain a nonempty connection string");
    return dsn;
}

std::uint16_t port(std::string_view text) {
    unsigned value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value > 65535)
        throw std::runtime_error("Port must be an integer between 0 and 65535");
    return static_cast<std::uint16_t>(value);
}

void usage() {
    std::cout << "Usage: ikv-auth --cert FILE --key FILE --dsn-file FILE\n"
                 "  [--bind 127.0.0.1] [--port 7443]\n"
                 "  [--admin-bind 127.0.0.1] [--admin-port 7444]\n"
                 "  [--migrations database/migrations] [--admin-bootstrap EXISTING_USERNAME]\n"
                 "  [--web-root web] [--backup-dir backups]\n"
                 "Offline restore (stop auth/world first):\n"
                 "  ikv-auth --dsn-file FILE --restore-backup FILE\n"
                 "Use separate flags and values; never put the DSN or session tokens in argv.\n";
}
} // namespace

int main(int argc, char** argv) {
    ikv::HttpsConfig config;
    std::filesystem::path dsn_file;
    std::filesystem::path migrations{"database/migrations"};
    std::filesystem::path backups{"backups"};
    std::filesystem::path restore;
    std::string bootstrap;
    std::set<std::string> seen;
    try {
        for (int i = 1; i < argc; ++i) {
            const std::string flag(argv[i]);
            if (flag == "--help") { usage(); return 0; }
            if (!seen.insert(flag).second) throw std::runtime_error("Duplicate option");
            if (i + 1 >= argc) throw std::runtime_error("Each option requires a separate value");
            const std::string value(argv[++i]);
            if (value.empty()) throw std::runtime_error("Option values must not be empty");
            if (flag == "--bind") config.bind = value;
            else if (flag == "--port") config.port = port(value);
            else if (flag == "--admin-bind") config.admin_bind = value;
            else if (flag == "--admin-port") config.admin_port = port(value);
            else if (flag == "--cert") config.certificate = value;
            else if (flag == "--key") config.private_key = value;
            else if (flag == "--dsn-file") dsn_file = value;
            else if (flag == "--migrations") migrations = value;
            else if (flag == "--admin-bootstrap") bootstrap = value;
            else if (flag == "--web-root") config.web_root = value;
            else if (flag == "--backup-dir") backups = value;
            else if (flag == "--restore-backup") restore = value;
            else throw std::runtime_error("Unknown option; use --help");
        }
        if (dsn_file.empty()) throw std::runtime_error("--dsn-file is required");
        if (!restore.empty()) {
            for (const auto& flag : seen)
                if (flag != "--dsn-file" && flag != "--restore-backup")
                    throw std::runtime_error("Offline restore accepts only --dsn-file and --restore-backup");
        } else if (config.certificate.empty() || config.private_key.empty()) {
            throw std::runtime_error("--cert and --key are required for HTTPS service mode");
        }
    } catch (const std::exception& e) {
        std::cerr << "ikv-auth: " << e.what() << '\n';
        return 2;
    }

    std::string stage{"DSN file validation"};
    try {
        auto dsn = read_dsn(dsn_file);
        stage = "database connection";
        // Private configuration only. pg_dump/pg_restore receive secrets through the Store's environment/file handling.
        if (::setenv("IKV_BACKUP_DIR", backups.c_str(), 1) != 0)
            throw std::runtime_error("Cannot set backup directory");
        ikv::Store store(std::move(dsn));
        if (!restore.empty()) {
            stage = "offline database restore (auth and world must be stopped)";
            store.restore(restore);
            std::cout << "RESTORED database backup\n";
            return 0;
        }
        stage = "database migrations";
        store.migrate(migrations);
        if (!bootstrap.empty()) {
            stage = "explicit existing-account administrator bootstrap";
            store.bootstrap_admin(bootstrap);
        }
        stage = "HTTPS certificate, private listeners and dashboard initialization";
        ikv::run_https(store, config);
        return 0;
    } catch (...) {
        // libpq/OpenSSL errors may contain connection strings or filesystem secrets; never echo them.
        std::cerr << "ikv-auth: failed during " << stage << "; check private service configuration\n";
        return 1;
    }
}
