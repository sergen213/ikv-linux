#pragma once

#include "ikv/assets.hpp"
#include "ikv/scene.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

struct SDL_Window;

namespace ikv {
class Renderer;
class GameAudio;
class EffectSystem;

// Account character API (/v1/characters) on the launcher-selected HTTPS origin. Verifies the configured CA
// and the origin host name like the world transport; the bearer session is never logged.
class CharacterApi {
public:
    CharacterApi(std::string_view origin, std::filesystem::path ca, std::string_view token);
    // Validated rows: id, name, class_id, appearance, level, zone.
    nlohmann::json list();
    void create(std::string_view name, int class_id, const nlohmann::json& appearance);
    void remove(std::uint64_t id, std::string_view confirm_name);

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
// or nullopt when the window is closed or cancelled() becomes true.
std::optional<std::uint64_t> run_character_screen(SDL_Window* window, Renderer& renderer, GameAudio& audio,
                                                  EffectSystem* effects, Scene& scene,
                                                  const std::filesystem::path& installation,
                                                  const CharacterScreenArt& art, CharacterApi& api,
                                                  const std::function<bool()>& cancelled);
} // namespace ikv
