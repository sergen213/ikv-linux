#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

// NONCANONICAL (automatic updates, docs/LAUNCHER_UPDATES.md): the client returns to the launcher when a newer release is
// ready — the launcher installed one while the game ran (--update-signal FILE appears), or the server refused this client
// as outdated (goodbye code "outdated_client"). It shows the Turkish notice, then exits with exit_code; the launcher
// installs/restarts into the new release and comes back to its ready state.
namespace ikv::client_update {

inline constexpr int exit_code = 75;

// "Yeni sürüm hazır; oyun güncelleniyor" plus the version when known.
std::string message(std::string_view version);

class Watch {
public:
    explicit Watch(std::filesystem::path signal) : signal_(std::move(signal)) {}
    // At most once per second: the notice to show, once, when the launcher's signal file has appeared.
    std::optional<std::string> poll();
    // The server refused this client as outdated (its release version, may be empty): same notice and exit.
    std::string trigger(std::string_view version);
    // The notice has been visible long enough: quit now and exit with exit_code.
    bool due() const { return triggered_ && Clock::now() >= quit_at_; }
    bool triggered() const { return triggered_; }

private:
    using Clock = std::chrono::steady_clock;
    std::filesystem::path signal_;
    Clock::time_point next_check_{}, quit_at_{};
    bool triggered_ = false;
};

} // namespace ikv::client_update
