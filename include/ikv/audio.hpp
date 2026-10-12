#pragma once

#include <SDL3/SDL_audio.h>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ikv {
struct Scene;
struct RenderEntity;
struct CameraBasis;

// SDL3.4 output and the installed /usr/bin/ffmpeg decoder. Call on the client
// thread; call update each frame and destroy before the application's SDL_Quit.
// Two background beds (music, ambient) plus 32 one-shot effects mix on one device.
class Audio {
public:
    enum Bed : std::size_t { music = 0, ambient = 1 };
    using Bytes = std::shared_ptr<const std::vector<std::uint8_t>>;
    explicit Audio(float volume);
    ~Audio();
    Audio(const Audio&) = delete;
    Audio& operator=(const Audio&) = delete;
    // Original Ogg file (OggS, <=32 MiB, <=600 s, 15 s decoder wall time) read and decoded off-thread;
    // replaces the bed when ready (a failure stops it). loop=false plays once, as the original music stream does.
    void bed(Bed bed, std::filesystem::path ogg, bool loop);
    void stop(Bed bed);
    // Original audio/...raw (mono S16LE 44100). With a world position the voice is attenuated
    // and panned from the listener like the original DirectSound3D source (0x00454070: min
    // distance 25, max 1e9, rolloff 1). Shares the bytes without copying. False when all
    // 32 effect voices are busy (the sound is dropped). Invalid input throws.
    bool effect(Bytes raw, std::string_view asset_path, std::optional<std::array<float, 3>> position);
    void listener(const std::array<float, 3>& eye, const std::array<float, 3>& right);
    void volume(float value);       // Master device gain, finite in [0, 1].
    void music_volume(float value); // Music+ambient bed gain (original MusicVolume), [0, 1].
    void pause(bool value);         // Pauses the logical device without discarding queues.
    void update();                  // Starts decoded beds, refills loops, releases finished voices.

private:
    struct StreamDeleter {
        void operator()(SDL_AudioStream* stream) const noexcept;
    };
    using Stream = std::unique_ptr<SDL_AudioStream, StreamDeleter>;
    struct PcmData;
    using Pcm = std::shared_ptr<const PcmData>;
    struct Voice {
        // Streams must be destroyed before their zero-copy PCM storage.
        Pcm pcm;
        Stream stream, right; // right: second (right-channel) stream of a panned voice
        std::size_t cursor = 0, queue_target = 0;
        bool loop = false;
        std::optional<std::array<float, 3>> position;
    };
    struct Pending {
        Bed bed;
        std::uint64_t ticket;
        bool loop;
        std::future<Pcm> pcm;
    };
    SDL_AudioDeviceID device_ = 0;
    bool stereo_ = false;
    float music_gain_ = 1;
    std::array<float, 3> eye_{}, right_{1, 0, 0};
    std::array<std::optional<Voice>, 34> voices_; // 0 music, 1 ambient, 2.. effects
    std::array<std::uint64_t, 2> tickets_{};
    std::vector<Pending> pending_;
    static void refill(Voice& voice);
    static Pcm decode(std::vector<std::uint8_t> ogg);
    void start(std::size_t slot, Pcm pcm, const SDL_AudioSpec& format, bool loop,
               std::optional<std::array<float, 3>> position);
    void place(Voice& voice) const;
};

// Client audio owner: original sound tables, zone music/ambient grids and actor clip cues.
// Set IKV_AUDIO_TRACE=FILE to append one line per started, stopped or failed sound.
class GameAudio {
public:
    // base: original app/base. Reads data/playlist.dat, data/playlistsound.dat, audio/sounds.xml.
    GameAudio(std::filesystem::path base, float volume, float music_volume);
    Audio mixer;
    // sounds.xml <Sound name> (original 00458eb0 table; chance-weighted) or an original
    // audio-relative path as stored in the source tables ("ui\\click.raw", "pc\\\\common_run.raw").
    // Failures (missing/invalid file, busy voices) are traced, never thrown.
    void play_event(std::string_view event, std::optional<std::array<float, 3>> position = std::nullopt);
    // Per frame after poses: listener from the camera, original 004573e0 music/ambient grid
    // selection at the controlled player, and clip audio for every changed ActorAnimation::audio_cue.
    void frame(const Scene& scene, std::span<const RenderEntity> poses, std::uint32_t self,
               const CameraBasis& listener);
    // NONCANONICAL wave 16: reads a zone's music/sound grids on a worker before the player enters it (one zone ahead), so
    // frame() switches grids without its synchronous 8 MiB read.
    void preload(unsigned support);
    // Per frame without a local player (character screen): the 004573e0 no-player branch plays
    // playlist index 8 while the credits window is shown, else 2, and ambient index 0 (silence).
    void frontend(bool credits);
    void music_override(const std::filesystem::path& ogg); // Explicit --audio file, looped; disables grid music.
    void music_volume(float value);
    void update(); // Mixer update; a failed background decode is traced, not thrown.
    float music_volume() const { return music_volume_; }

private:
    std::filesystem::path base_;
    std::vector<std::string> songs_, beds_;
    std::vector<std::pair<std::string, std::vector<std::pair<std::string, float>>>> sounds_;
    std::vector<std::uint8_t> music_grid_, sound_grid_;
    unsigned grid_zone_ = UINT32_MAX;
    using Grids = std::pair<std::vector<std::uint8_t>, std::vector<std::uint8_t>>;
    std::future<Grids> next_grids_;
    unsigned next_zone_ = UINT32_MAX;
    std::size_t song_ = SIZE_MAX, bed_ = SIZE_MAX;
    bool override_ = false, music_on_ = false, bed_on_ = false;
    float music_volume_ = 1;
    std::unordered_map<std::string, Audio::Bytes> files_;
    std::size_t file_bytes_ = 0;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> cues_, next_cues_;
    std::minstd_rand random_{1};
    std::ofstream trace_;
    std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
    void trace(std::string_view kind, std::string_view what, std::string_view detail,
               const std::optional<std::array<float, 3>>& position = std::nullopt);
    void select_bed(Audio::Bed bed, std::size_t index);
    void choose(std::uint8_t music, std::uint8_t sound); // 004573e0 selection from playlist indices
};

// Original source path ("pc\\\\common_run.raw", "music/akrep.ogg") to a relative audio path with
// '/' separators. Empty, '.' and '..' segments are rejected (empty result).
std::string original_audio_path(std::string_view source);
// 004573e0: grid index of a world position, (round(x*2/3+1024), round(z*2/3+1024)) on 2048x2048.
std::optional<std::size_t> audio_grid_index(float x, float z);
} // namespace ikv
