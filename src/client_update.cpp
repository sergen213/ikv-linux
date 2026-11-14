#include "ikv/client_update.hpp"

#include <fstream>
#include <system_error>

namespace ikv::client_update {
namespace {
// Version tokens shown to the player: printable ASCII from the launcher's file or the server goodbye, bounded.
std::string safe_version(std::string_view text) {
    std::string version;
    for (const char c : text.substr(0, 64))
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '-') version += c;
    return version;
}
} // namespace

std::string message(std::string_view version) {
    const auto safe = safe_version(version);
    return safe.empty() ? std::string("Yeni sürüm hazır; oyun güncelleniyor.")
                        : "Yeni sürüm hazır (v" + safe + "); oyun güncelleniyor.";
}

std::optional<std::string> Watch::poll() {
    if (triggered_ || signal_.empty() || Clock::now() < next_check_) return std::nullopt;
    next_check_ = Clock::now() + std::chrono::seconds(1);
    std::error_code error;
    if (!std::filesystem::is_regular_file(signal_, error)) return std::nullopt;
    std::string version;
    std::ifstream file(signal_);
    std::getline(file, version);
    return trigger(version);
}

std::string Watch::trigger(std::string_view version) {
    triggered_ = true;
    quit_at_ = Clock::now() + std::chrono::seconds(4); // the notice stays readable before the game closes
    return message(version);
}

} // namespace ikv::client_update
