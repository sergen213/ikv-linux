#pragma once

#include "ikv/assets.hpp"
#include "ikv/client_realms.hpp"
#include "ikv/scene.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <stdexcept>
#include <vector>
#include <string_view>

struct SDL_Window;

namespace ikv {
class Renderer;
class GameAudio;
class EffectSystem;

// A refused account-service request: the server's message (or a generic one) and its HTTP status.
struct ApiStatusError : std::runtime_error {
    int status;
    ApiStatusError(const std::string& message, int code) : std::runtime_error(message), status(code) {}
};
// Account character API (/v1/characters) on the launcher-selected HTTPS origin. Verifies the configured CA
// and the origin host name like the world transport; the bearer session is never logged.
class CharacterApi {
public:
    CharacterApi(std::string_view origin, std::filesystem::path ca, std::string_view token);
    // Validated rows: id, name, class_id, appearance, level, zone, realm_id (1 when the server predates realms).
    nlohmann::json list();
    void create(std::string_view name, int class_id, const nlohmann::json& appearance, int realm_id = 1);
    void remove(std::uint64_t id, std::string_view confirm_name);
    // The character's shard; nullopt on 404 (pre-realm server: use the configured world host/port).
    std::optional<WorldEndpoint> world(std::uint64_t id);
    const std::string& host() const noexcept { return host_; } // the account service's host (empty world host)

private:
    std::string host_, port_;
    std::filesystem::path ca_;
    std::string_view token_;
    nlohmann::json request(std::string_view method, const std::string& target, const nlohmann::json& body);
};

struct CharacterScreenArt {
    const assets::DdsImage& ui1;  // ui/userinterface.dds
    const assets::DdsImage& logo; // ui/ikvlogo.dds
    std::filesystem::path font;   // Verdana
};
// Original cModuleCharsel/cModuleChargen on the charsel.adf backdrop. Returns the chosen character id,
// or nullopt when the window is closed or cancelled() becomes true. Shows and creates only characters of
// `realm` (NONCANONICAL wave 30: the launcher's --realm). A nonempty message opens as a notice (a refused
// world entry or a failed shard transfer returns here with the server's text).
std::optional<std::uint64_t> run_character_screen(SDL_Window* window, Renderer& renderer, GameAudio& audio,
                                                  EffectSystem* effects, Scene& scene,
                                                  const std::filesystem::path& installation,
                                                  const CharacterScreenArt& art, CharacterApi& api,
                                                  const std::function<bool()>& cancelled, int realm = 1,
                                                  std::string_view message = {});
} // namespace ikv
