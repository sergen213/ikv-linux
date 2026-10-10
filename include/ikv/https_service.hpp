#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace ikv {
class Store;

struct HttpsConfig {
    std::string bind{"127.0.0.1"};
    std::uint16_t port{7443};
    std::string admin_bind{"127.0.0.1"};
    std::uint16_t admin_port{7444};
    std::filesystem::path certificate;
    std::filesystem::path private_key;
    std::filesystem::path web_root{"web"};
};

// Runs the public player API and separate private administration TLS listeners.
// Management routes are never available on the public listener.
void run_https(Store& store, const HttpsConfig& config);
} // namespace ikv
