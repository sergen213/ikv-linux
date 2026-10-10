#include "ikv/audio.hpp"
#include "ikv/renderer.hpp"
#include "ikv/scene.hpp"

#include <SDL3/SDL.h>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unistd.h>

// audio_check ORIGINAL_APP_BASE (ctest original-audio-events, SDL_AUDIO_DRIVER=dummy).
// Exercises original table decoding, the 004573e0 grid lookup, GameAudio event/bed/clip-cue
// paths and real SDL stream lifetime. Dummy output proves queues and decoding, not audibility.
namespace {
void need(bool condition, const char* what) {
    if (!condition) throw std::runtime_error(std::string("audio check failed: ") + what);
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::invalid_argument("usage: audio_check ORIGINAL_APP_BASE");
        const std::filesystem::path base = argv[1];
        need(ikv::original_audio_path("pc\\\\common_run.raw") == "pc/common_run.raw", "source path normalization");
        need(ikv::original_audio_path("..\\x.raw").empty() && ikv::original_audio_path("a/./b.raw").empty(), "path traversal rejection");
        need(ikv::audio_grid_index(386, 806) == std::size_t{1561} * 2048 + 1281, "Yeni Cami grid cell");
        need(!ikv::audio_grid_index(1600, 0) && !ikv::audio_grid_index(0, -1600), "grid bounds");
        const auto songs = ikv::load_source_playlist(base, "playlist.dat");
        const auto beds = ikv::load_source_playlist(base, "playlistsound.dat");
        need(songs.size() == 61 && songs[3] == "music\\avlu.ogg" && songs[0] == "nothing", "playlist.dat order");
        need(beds.size() == 25 && beds[1] == "atmospheric\\ruzgarkisa.ogg" && beds[8] == "nothing", "playlistsound.dat order");

        const auto trace_path = std::filesystem::temp_directory_path() / ("ikv-audio-check-" + std::to_string(::getpid()) + ".tsv");
        std::filesystem::remove(trace_path);
        ::setenv("IKV_AUDIO_TRACE", trace_path.c_str(), 1);
        {
            ikv::GameAudio audio(base, 0.5f, 1.0f);
            const auto pump = [&audio](int milliseconds) {
                for (int elapsed = 0; elapsed < milliseconds; elapsed += 10) { audio.update(); SDL_Delay(10); }
            };
            ikv::Scene scene;
            scene.source_support = 0;
            ikv::ActorLibrary library;
            ikv::ActorClip wait, bite;
            wait.id = 1; wait.loops = true; wait.end = wait.loop_end = 1;
            bite.id = 3; bite.end = bite.loop_end = 1; bite.audio = {"monster\\\\fare\\\\attack1.raw"};
            library.clips = {wait, bite};
            scene.actor_tables.libraries.push_back(library);
            ikv::ActorAnimation rat;
            rat.library = 0; rat.current_clip = 0; rat.initialized = true;
            scene.actors.push_back(rat);
            ikv::SceneActorAssembly assembly;
            assembly.entity_id = 2; assembly.controller = 0;
            scene.actor_assemblies.push_back(assembly);
            const std::array<ikv::RenderEntity, 2> poses{{{1, 386, 10, 806, 0}, {2, 396, 10, 806, 0}}};
            ikv::Camera camera;
            camera.target = {386, 16, 806};
            const auto listener = ikv::camera_basis(camera);
            audio.frontend(false); // 004573e0 without a player: playlist index 2 (character screen).
            audio.frontend(false); // Unchanged index: no restart.
            audio.frontend(true);  // Credits window shown: index 8.
            audio.frame(scene, poses, 1, listener); // Zone 0 grids; music index 3, ambient index 8.
            ikv::set_actor_action(scene, 2, 3);       // Non-looping clip entry bumps the cue.
            audio.frame(scene, poses, 1, listener);
            audio.play_event("ui\\click.raw");
            audio.play_event("ui\\windowopen.raw"); // Odd-sized original file: whole frames play.
            // Every 0048eb70 UI slot with an original trigger (alert/warning 004b8230, eat 00430610/004a9690, itemdrop
            // 004a9690/004a9880, ateslisilah1 0040b170) decodes from the original corpus.
            for (const char* slot : {"ui\\alert.raw", "ui\\warning.raw", "ui\\eat.raw", "ui\\itemdrop.raw", "weapon\\handgun\\ateslisilah1.raw"})
                audio.play_event(slot);
            audio.play_event("test_sound", std::array{390.f, 10.f, 806.f});
            audio.play_event("../outside.raw");
            audio.play_event("monster\\fare\\damage.ogg"); // Referenced by animation.dat, absent from the corpus.
            for (int i = 0; i < 34; ++i) audio.play_event("pc\\common_walk.raw", std::array{386.f, 10.f, 806.f});
            audio.mixer.pause(true);
            pump(200);
            audio.mixer.pause(false);
            audio.mixer.volume(0.0f);
            pump(2500); // Background music decode completes off-thread.
            audio.music_volume(0.0f);
            audio.frame(scene, poses, 1, listener); // MusicVolume <= 0.01 stops the beds.
            pump(200);
        }
        std::ifstream trace(trace_path);
        std::stringstream text;
        text << trace.rdbuf();
        const auto log = text.str();
        std::filesystem::remove(trace_path);
        const auto has = [&log](const char* line) { return log.find(line) != std::string::npos; };
        need(has("\tzone\t0\t"), "zone grids traced");
        need(has("\tmusic\t3\tmusic/avlu.ogg"), "spawn music from music00.raw");
        need(has("\tambient\t8\tnothing"), "spawn ambient bed from sound00.raw");
        need(has("\teffect\tmonster\\\\fare\\\\attack1.raw\tmonster/fare/attack1.raw\t396,10,806"), "rat clip cue");
        need(has("\teffect\tui\\click.raw\tui/click.raw\n"), "UI click");
        for (const char* line : {"\teffect\tui\\alert.raw\tui/alert.raw\n", "\teffect\tui\\warning.raw\tui/warning.raw\n", "\teffect\tui\\eat.raw\tui/eat.raw\n",
                                 "\teffect\tui\\itemdrop.raw\tui/itemdrop.raw\n", "\teffect\tweapon\\handgun\\ateslisilah1.raw\tweapon/handgun/ateslisilah1.raw\n"})
            need(has(line), "UI sound slot");
        need(log.find("\tmusic\t2\t") < log.find("\tmusic\t8\t") && log.find("\tmusic\t8\t") < log.find("\tmusic\t3\t") &&
             log.find("\tmusic\t2\t") == log.rfind("\tmusic\t2\t"), "character screen music 2, credits 8, then the zone grid");
        need(has("\teffect\tui\\windowopen.raw\tui/windowopen.raw"), "odd original RAW");
        need(has("\teffect\ttest_sound\t") || has("\tsilent\ttest_sound\t"), "sounds.xml map");
        need(has("\tfailed\t../outside.raw\t") && has("\tfailed\tmonster\\fare\\damage.ogg\t"), "rejections traced");
        need(has("\tdropped\tpc\\common_walk.raw\t"), "voice limit drops instead of throwing");
        need(has("\tmusic\tstop\tmuted"), "music volume stop");
        need(!has("\tfailed\tbed\t"), "background decode");
        std::cout << log << "audio event/table/stream check completed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
