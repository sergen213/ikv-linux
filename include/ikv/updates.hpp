#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ikv::updates {

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

} // namespace ikv::updates
