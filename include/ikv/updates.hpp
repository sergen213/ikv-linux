#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace ikv::updates {

// The signed package platform this build installs: Linux packages carry bin/ikv-linux.AppImage and no "platform"
// manifest field; Windows packages carry the whole portable tree (bin/ikv-launcher.exe ...) and "platform":"windows".
#ifdef _WIN32
inline constexpr char platform[] = "windows";
inline constexpr char launcher_payload[] = "bin/ikv-launcher.exe";
#else
inline constexpr char platform[] = "linux";
inline constexpr char launcher_payload[] = "bin/ikv-linux.AppImage";
#endif

struct File {
    std::string path;
    std::string sha256;
    std::uint64_t size{};
    bool executable{};
};

struct Manifest {
    std::string version;
    std::string channel;
    std::uint64_t sequence{};
    std::string package;
    std::string package_sha256;
    std::uint64_t package_size{};
    std::vector<File> files;
    std::string platform = "linux";
};

// All failures throw std::exception; a missing/untrusted key never succeeds.
// Signatures are detached raw 64-byte Ed25519 signatures over exact manifest bytes.
Manifest verify_manifest(const std::filesystem::path& manifest,
                         const std::filesystem::path& signature,
                         const std::filesystem::path& trusted_pem);
// Install enforces durable per-channel sequence freshness; equal sequence requires
// identical authenticated manifest bytes. Explicit rollback never lowers this floor.
Manifest install(const std::filesystem::path& manifest,
                 const std::filesystem::path& signature,
                 const std::filesystem::path& package,
                 const std::filesystem::path& trusted_pem,
                 const std::filesystem::path& install_root);
Manifest verify_release(const std::filesystem::path& install_root,
                        const std::string& version,
                        const std::filesystem::path& trusted_pem);
// Verify previous signed release, then atomically switch current; repeated rollback toggles.
Manifest rollback(const std::filesystem::path& install_root,
                  const std::filesystem::path& trusted_pem);
// Version named by the install root's current pointer (Linux: current symlink, Windows: current.json); empty when
// nothing is installed. Recovers an interrupted cutover first, under the installation lock.
std::string current_release(const std::filesystem::path& install_root);
// Strict SemVer 2.0 precedence of MAJOR.MINOR.PATCH[-PRERELEASE] (a prerelease sorts before its
// release; no "v" prefix, build metadata, leading zeroes or empty identifiers). Returns <0, 0 or >0;
// malformed input throws.
int compare_versions(std::string_view left, std::string_view right);

} // namespace ikv::updates
