#pragma once

#include "ikv/protocol.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ikv {
// Authenticated, explicitly configured TLS transport; pump is the only I/O driver
// after the bounded constructor handshake. This class is single-threaded.
class ClientSession {
public:
    ClientSession(std::string host, std::uint16_t port, std::filesystem::path ca,
                  std::string_view token, std::uint64_t character);
    ~ClientSession();
    ClientSession(const ClientSession&) = delete;
    ClientSession& operator=(const ClientSession&) = delete;

    std::uint32_t self = 0;
    std::uint32_t tick = 0;
    std::vector<Player> players;
    // Authoritative view includes zone/source_registry_id/bounds, character and
    // visible appearance/equipment metadata, and explicit event identities.
    nlohmann::json state;
    bool has_snapshot = false;
    bool eof = false;

    void pump(int wait_ms, const std::function<void(Type)>& on_message = {});
    void destination(std::int32_t x, std::int32_t y);
    void action(const nlohmann::json& body);
    // NONCANONICAL headless helper: a deterministic 100-fixed-point-unit cardinal
    // destination per snapshot, never client-side integration or native WASD.
    void intent(int dx, int dy);
    void stop();
    bool drained() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    void receive(Message message);
};
} // namespace ikv
