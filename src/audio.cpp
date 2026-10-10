#include "ikv/audio.hpp"
#include "ikv/audio_asset.hpp"
#include "ikv/renderer.hpp"
#include "ikv/scene.hpp"

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_init.h>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace ikv {
namespace {
constexpr std::size_t input_limit = 32 * 1024 * 1024;
constexpr std::size_t bytes_per_second = 48000 * 2 * sizeof(std::int16_t);
const SDL_AudioSpec pcm_format{SDL_AUDIO_S16LE, 2, 48000};
constexpr std::size_t grid_bytes = 2048 * 2048;

[[noreturn]] void system_error(const char* operation) {
    throw std::runtime_error(std::string(operation) + ": " + std::strerror(errno));
}
void sdl_check(bool result, const char* operation) {
    if (!result) throw std::runtime_error(std::string(operation) + ": " + SDL_GetError());
}
void check_volume(float value) {
    if (!std::isfinite(value) || value < 0.0f || value > 1.0f)
        throw std::invalid_argument("audio volume must be finite and in [0, 1]");
}
std::vector<std::uint8_t> read_file(const std::filesystem::path& path, std::size_t limit) {
    const auto size = std::filesystem::file_size(path);
    if (size == 0 || size > limit) throw std::runtime_error("original audio file is empty or too large: " + path.string());
    std::ifstream file(path, std::ios::binary);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (!file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
        throw std::runtime_error("original audio read failed: " + path.string());
    return bytes;
}

// Unlinked, owner-only scratch files bound both encoded input and decoded output.
// A regular output file lets us allocate the exact PCM size once, without growing
// a vector or risking deadlock between ffmpeg's input and output pipes.
struct Scratch {
    int fd = -1;
    Scratch() {
        char path[] = "/tmp/ikv-audio-XXXXXX";
        fd = ::mkostemp(path, O_CLOEXEC);
        if (fd == -1) system_error("creating audio scratch file");
        if (::unlink(path) == -1) {
            const int saved = errno;
            ::close(fd);
            errno = saved;
            system_error("unlinking audio scratch file");
        }
        if (fd <= STDERR_FILENO) {
            const int replacement = ::fcntl(fd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
            const int saved = errno;
            ::close(fd);
            fd = replacement;
            if (fd == -1) { errno = saved; system_error("duplicating audio descriptor"); }
        }
    }
    ~Scratch() { if (fd != -1) ::close(fd); }
    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;
};
struct Child {
    pid_t pid;
    ~Child() {
        if (pid <= 0) return;
        ::kill(pid, SIGKILL);
        while (::waitpid(pid, nullptr, 0) == -1 && errno == EINTR) {}
    }
};

} // namespace

struct Audio::PcmData {
    Bytes owner; // shared original RAW file bytes, or empty for a decoder mapping
    std::span<const std::uint8_t> view;
    void* mapping = MAP_FAILED;
    std::size_t mapped_size = 0;

    PcmData(Bytes bytes, std::span<const std::uint8_t> frames) : owner(std::move(bytes)), view(frames) {}
    PcmData(int fd, std::size_t size)
        : mapping(::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0)), mapped_size(size) {
        if (mapping == MAP_FAILED) system_error("mapping decoded audio");
        view = {static_cast<const std::uint8_t*>(mapping), size};
    }
    ~PcmData() { if (mapping != MAP_FAILED) ::munmap(mapping, mapped_size); }
    PcmData(const PcmData&) = delete;
    PcmData& operator=(const PcmData&) = delete;
    const std::uint8_t* data() const noexcept { return view.data(); }
    std::size_t size() const noexcept { return view.size(); }
};

Audio::Pcm Audio::decode(std::vector<std::uint8_t> ogg) {
    const std::size_t decoded_limit = bytes_per_second * 600;
    Scratch input;
    Scratch output;
    std::size_t offset = 0;
    while (offset < ogg.size()) {
        const ssize_t count = ::write(input.fd, ogg.data() + offset, ogg.size() - offset);
        if (count == -1 && errno == EINTR) continue;
        if (count <= 0) system_error("writing Ogg scratch input");
        offset += static_cast<std::size_t>(count);
    }
    if (::lseek(input.fd, 0, SEEK_SET) == -1) system_error("rewinding Ogg input");
    const int errors = ::open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (errors == -1) system_error("opening decoder diagnostic sink");
    // All arguments and limits are prepared before fork: no shell, allocation or
    // C++ runtime work in the child while SDL's audio thread is running.
    const char* const argv[] = {
        "/usr/bin/ffmpeg", "-hide_banner", "-loglevel", "error", "-nostdin",
        "-threads", "1", "-protocol_whitelist", "pipe", "-f", "ogg",
        "-i", "pipe:0", "-map", "0:a:0", "-vn", "-sn", "-dn",
        "-threads", "1", "-t", "600.001",
        "-ac", "2", "-ar", "48000", "-acodec", "pcm_s16le",
        "-f", "s16le", "pipe:1", nullptr
    };
    const rlimit output_bound{decoded_limit + 65536, decoded_limit + 65536};
    const rlimit memory_bound{1024ULL * 1024 * 1024, 1024ULL * 1024 * 1024};
    const rlimit cpu_bound{12, 12};
    const rlimit core_bound{0, 0};
    const pid_t pid = ::fork();
    if (pid == 0) {
        if (::dup2(input.fd, STDIN_FILENO) == -1 ||
            ::dup2(output.fd, STDOUT_FILENO) == -1 ||
            ::dup2(errors, STDERR_FILENO) == -1 ||
            ::setrlimit(RLIMIT_FSIZE, &output_bound) == -1 ||
            ::setrlimit(RLIMIT_AS, &memory_bound) == -1 ||
            ::setrlimit(RLIMIT_CPU, &cpu_bound) == -1 ||
            ::setrlimit(RLIMIT_CORE, &core_bound) == -1)
            ::_exit(126);
        ::close(input.fd);
        ::close(output.fd);
        if (errors > STDERR_FILENO) ::close(errors);
        ::execv(argv[0], const_cast<char* const*>(argv));
        ::_exit(127);
    }
    const int saved = errno;
    ::close(errors);
    if (pid == -1) { errno = saved; system_error("forking Ogg decoder"); }
    Child child{pid};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    int status = 0;
    for (;;) {
        const pid_t result = ::waitpid(pid, &status, WNOHANG);
        if (result == pid) { child.pid = -1; break; }
        if (result == -1) {
            if (errno == EINTR) continue;
            if (errno == ECHILD) child.pid = -1;
            system_error("waiting for Ogg decoder");
        }
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("Ogg decoder exceeded its 15-second wall-time limit");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        throw std::runtime_error("ffmpeg Ogg decoding failed (missing decoder, corrupt data, or resource limit)");
    struct stat info {};
    if (::fstat(output.fd, &info) == -1) system_error("measuring decoded audio");
    if (info.st_size <= 0 || static_cast<std::uint64_t>(info.st_size) > decoded_limit || info.st_size % 4 != 0)
        throw std::runtime_error("Ogg clip is empty, invalid, or exceeds 600 seconds");
    // MAP_PRIVATE/PROT_READ keeps the unlinked scratch file alive through shared
    // PCM ownership, without a second full allocation/read/copy of long music.
    return std::make_shared<const PcmData>(output.fd, static_cast<std::size_t>(info.st_size));
}

void Audio::StreamDeleter::operator()(SDL_AudioStream* stream) const noexcept {
    SDL_DestroyAudioStream(stream);
}

Audio::Audio(float value) {
    check_volume(value);
    sdl_check(SDL_InitSubSystem(SDL_INIT_AUDIO), "initializing SDL audio");
    try {
        device_ = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &pcm_format);
        sdl_check(device_ != 0, "opening SDL audio output");
        SDL_AudioSpec device{};
        sdl_check(SDL_GetAudioDeviceFormat(device_, &device, nullptr), "querying SDL audio output");
        stereo_ = device.channels == 2; // Panning needs a plain stereo device; otherwise attenuation only.
        volume(value);
        pause(false);
    } catch (...) {
        if (device_ != 0) SDL_CloseAudioDevice(device_);
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        throw;
    }
}
Audio::~Audio() {
    tickets_ = {};
    for (auto& voice : voices_) voice.reset();
    SDL_CloseAudioDevice(device_);
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    // Pending futures join their bounded (15 s) decoders here; results are discarded.
}
void Audio::volume(float value) {
    check_volume(value);
    sdl_check(SDL_SetAudioDeviceGain(device_, value), "setting audio volume");
}
void Audio::music_volume(float value) {
    check_volume(value);
    music_gain_ = value;
    for (std::size_t slot = 0; slot < 2; ++slot)
        if (voices_[slot]) sdl_check(SDL_SetAudioStreamGain(voices_[slot]->stream.get(), value), "setting music volume");
}
void Audio::pause(bool value) {
    sdl_check(value ? SDL_PauseAudioDevice(device_) : SDL_ResumeAudioDevice(device_), "setting audio pause");
}
void Audio::listener(const std::array<float, 3>& eye, const std::array<float, 3>& right) {
    for (float c : eye) if (!std::isfinite(c)) throw std::invalid_argument("audio listener must be finite");
    for (float c : right) if (!std::isfinite(c)) throw std::invalid_argument("audio listener must be finite");
    eye_ = eye;
    right_ = right;
}
void Audio::refill(Voice& voice) {
    const int queued = SDL_GetAudioStreamQueued(voice.stream.get());
    sdl_check(queued >= 0, "querying background audio queue");
    std::size_t remaining = queued >= static_cast<int>(voice.queue_target) ? 0 :
                            voice.queue_target - static_cast<std::size_t>(queued);
    while (remaining != 0) {
        const std::size_t count = std::min(remaining, voice.pcm->size() - voice.cursor);
        sdl_check(SDL_PutAudioStreamDataNoCopy(voice.stream.get(), voice.pcm->data() + voice.cursor,
                                            static_cast<int>(count), nullptr, nullptr), "queueing background audio");
        voice.cursor = (voice.cursor + count) % voice.pcm->size();
        remaining -= count;
    }
}
// DirectSound3D inverse-distance model as configured by the original: min distance 25
// (0x00454070), rolloff/distance factor 1 (0x004596f0). Pan is a balance law from the
// listener's right axis: the far channel fades, the near channel stays at full gain.
void Audio::place(Voice& voice) const {
    float left = 1, right = 1;
    if (voice.position) {
        std::array<float, 3> d{};
        float distance = 0;
        for (std::size_t i = 0; i < 3; ++i) { d[i] = (*voice.position)[i] - eye_[i]; distance += d[i] * d[i]; }
        distance = std::sqrt(distance);
        const float gain = distance > 25.0f ? 25.0f / distance : 1.0f;
        const float pan = distance > 1e-4f ?
            std::clamp((d[0] * right_[0] + d[1] * right_[1] + d[2] * right_[2]) / distance, -1.0f, 1.0f) : 0.0f;
        left = gain * std::min(1.0f, 1.0f - pan);
        right = gain * std::min(1.0f, 1.0f + pan);
        if (!voice.right) left = gain;
    }
    sdl_check(SDL_SetAudioStreamGain(voice.stream.get(), left), "setting sound gain");
    if (voice.right) sdl_check(SDL_SetAudioStreamGain(voice.right.get(), right), "setting sound gain");
}
void Audio::start(std::size_t slot, Pcm pcm, const SDL_AudioSpec& format, bool loop,
                  std::optional<std::array<float, 3>> position) {
    voices_[slot].reset();
    Voice voice;
    voice.pcm = std::move(pcm);
    voice.loop = loop;
    voice.position = position;
    voice.queue_target = static_cast<std::size_t>(format.freq) * static_cast<std::size_t>(format.channels);
    const bool panned = position && stereo_;
    // A panned mono voice is two streams sharing the PCM; each output map silences one channel.
    // Maps and data are set before the atomic bind, so both channels start on the same frame.
    const SDL_AudioSpec output{format.format, 2, format.freq};
    voice.stream.reset(SDL_CreateAudioStream(&format, panned ? &output : &format));
    sdl_check(voice.stream != nullptr, "creating SDL audio stream");
    if (panned) {
        voice.right.reset(SDL_CreateAudioStream(&format, &output));
        sdl_check(voice.right != nullptr, "creating SDL audio stream");
        sdl_check(SDL_LockAudioStream(voice.stream.get()) && SDL_LockAudioStream(voice.right.get()), "locking sound");
    }
    if (loop) refill(voice);
    else for (auto* stream : {voice.stream.get(), voice.right.get()}) {
        if (!stream) continue;
        sdl_check(SDL_PutAudioStreamDataNoCopy(stream, voice.pcm->data(), static_cast<int>(voice.pcm->size()), nullptr, nullptr),
                  "queueing sound");
        sdl_check(SDL_FlushAudioStream(stream), "flushing sound");
    }
    if (slot < 2) sdl_check(SDL_SetAudioStreamGain(voice.stream.get(), music_gain_), "setting music volume");
    else place(voice);
    SDL_AudioStream* streams[2]{voice.stream.get(), voice.right.get()};
    // Binding replaces the destination spec (and its channel map), so the maps are set afterwards while both
    // streams stay locked: the device cannot pull either channel before both are panned.
    const bool bound = SDL_BindAudioStreams(device_, streams, panned ? 2 : 1);
    if (panned) {
        const int left_map[2]{0, -1}, right_map[2]{-1, 1};
        const bool mapped = bound && SDL_SetAudioStreamOutputChannelMap(voice.stream.get(), left_map, 2) &&
                            SDL_SetAudioStreamOutputChannelMap(voice.right.get(), right_map, 2);
        SDL_UnlockAudioStream(voice.right.get());
        SDL_UnlockAudioStream(voice.stream.get());
        sdl_check(mapped, "panning sound");
    }
    sdl_check(bound, "binding SDL audio stream");
    voices_[slot].emplace(std::move(voice));
}
void Audio::bed(Bed bed, std::vector<std::uint8_t> ogg, bool loop) {
    if (ogg.size() < 27 || ogg.size() > input_limit || std::memcmp(ogg.data(), "OggS", 4) != 0 || ogg[4] != 0)
        throw std::invalid_argument("audio requires an original OggS payload between 27 bytes and 32 MiB");
    // Decoding long original music takes ffmpeg up to seconds; the frame loop must not wait.
    pending_.push_back({bed, ++tickets_[bed], loop, std::async(std::launch::async, decode, std::move(ogg))});
}
void Audio::stop(Bed bed) {
    ++tickets_[bed];
    voices_[bed].reset();
}
bool Audio::effect(Bytes raw, std::string_view asset_path, std::optional<std::array<float, 3>> position) {
    if (!raw) throw std::invalid_argument("missing RAW audio");
    const auto view = assets::inspect_raw_audio(*raw, asset_path);
    if (view.pcm.size() > static_cast<std::size_t>(view.sample_rate) * view.channels * sizeof(std::int16_t) * 600U)
        throw std::invalid_argument("RAW clip exceeds 600 seconds");
    if (position) for (float c : *position) if (!std::isfinite(c)) throw std::invalid_argument("sound position must be finite");
    std::size_t slot = 2;
    while (slot < voices_.size() && voices_[slot]) ++slot;
    if (slot == voices_.size()) return false;
    const SDL_AudioSpec format{SDL_AUDIO_S16LE, static_cast<int>(view.channels), static_cast<int>(view.sample_rate)};
    start(slot, std::make_shared<const PcmData>(std::move(raw), view.pcm), format, false, position);
    return true;
}
void Audio::update() {
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (it->pcm.wait_for(std::chrono::seconds(0)) != std::future_status::ready) { ++it; continue; }
        auto ready = std::move(*it);
        it = pending_.erase(it);
        auto pcm = ready.pcm.get(); // Rethrows a decoder failure to the caller.
        if (ready.ticket == tickets_[ready.bed]) start(ready.bed, std::move(pcm), pcm_format, ready.loop, std::nullopt);
    }
    for (std::size_t slot = 0; slot < voices_.size(); ++slot) {
        if (!voices_[slot]) continue;
        auto& voice = *voices_[slot];
        if (voice.loop) { refill(voice); continue; }
        bool done = true;
        for (auto* stream : {voice.stream.get(), voice.right.get()}) {
            if (!stream) continue;
            const int queued = SDL_GetAudioStreamQueued(stream);
            const int available = SDL_GetAudioStreamAvailable(stream);
            sdl_check(queued >= 0 && available >= 0, "querying sound completion");
            done = done && queued == 0 && available == 0;
        }
        if (done) voices_[slot].reset();
        else if (voice.position) place(voice); // The listener moves; the source stays where it started.
    }
}

std::string original_audio_path(std::string_view source) {
    std::string path;
    std::size_t begin = 0;
    while (begin <= source.size()) {
        const auto end = std::min(source.find_first_of("/\\", begin), source.size());
        const auto segment = source.substr(begin, end - begin);
        if (segment == "." || segment == "..") return {};
        if (!segment.empty()) { if (!path.empty()) path += '/'; path += segment; }
        begin = end + 1;
    }
    return path;
}
std::optional<std::size_t> audio_grid_index(float x, float z) {
    // 004573e0 -> 00444af0(x * 0.6666667f + 1024.0f): round half away from zero.
    const auto cell = [](float c) { return std::lround(c * 0.6666667f + 1024.0f); };
    if (!std::isfinite(x) || !std::isfinite(z)) return std::nullopt;
    const long ix = cell(x), iz = cell(z);
    if (ix < 0 || iz < 0 || ix >= 2048 || iz >= 2048) return std::nullopt;
    return static_cast<std::size_t>(iz) * 2048 + static_cast<std::size_t>(ix);
}

GameAudio::GameAudio(std::filesystem::path base, float volume, float music_volume)
    : mixer(volume), base_(std::move(base)),
      songs_(load_source_playlist(base_, "playlist.dat")), beds_(load_source_playlist(base_, "playlistsound.dat")) {
    if (const char* path = std::getenv("IKV_AUDIO_TRACE"); path && *path) {
        trace_.open(path, std::ios::app);
        if (!trace_) throw std::runtime_error(std::string("cannot open IKV_AUDIO_TRACE: ") + path);
    }
    this->music_volume(music_volume);
    // 00459510/00458eb0 sound map: <Sounds><Sound name><file chance>path</file>...
    boost::property_tree::ptree tree;
    std::ifstream xml(base_ / "audio" / "sounds.xml", std::ios::binary);
    if (!xml) throw std::runtime_error("original installation has no audio/sounds.xml");
    boost::property_tree::read_xml(xml, tree, boost::property_tree::xml_parser::trim_whitespace);
    for (const auto& [tag, sound] : tree.get_child("Sounds")) {
        if (tag != "Sound") continue;
        auto& entry = sounds_.emplace_back(sound.get<std::string>("<xmlattr>.name"), decltype(sounds_)::value_type::second_type{});
        for (const auto& [file_tag, file] : sound)
            if (file_tag == "file") entry.second.emplace_back(file.data(), file.get<float>("<xmlattr>.chance"));
    }
    trace("init", "tables", std::to_string(songs_.size()) + " songs, " + std::to_string(beds_.size()) +
          " ambient beds, " + std::to_string(sounds_.size()) + " sound map entries");
}
void GameAudio::trace(std::string_view kind, std::string_view what, std::string_view detail,
                      const std::optional<std::array<float, 3>>& position) {
    if (!trace_) return;
    char time[32];
    std::snprintf(time, sizeof time, "%.3f", std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count());
    trace_ << time << '\t' << kind << '\t' << what << '\t' << detail;
    if (position) trace_ << '\t' << (*position)[0] << ',' << (*position)[1] << ',' << (*position)[2];
    trace_ << '\n' << std::flush;
}
void GameAudio::music_volume(float value) {
    mixer.music_volume(value);
    music_volume_ = value;
}
void GameAudio::update() {
    try { mixer.update(); }
    catch (const std::exception& error) { trace("failed", "bed", error.what()); }
}
void GameAudio::music_override(const std::filesystem::path& ogg) {
    mixer.bed(Audio::music, read_file(ogg, input_limit), true);
    override_ = true;
    trace("music", "override", ogg.string());
}
void GameAudio::play_event(std::string_view event, std::optional<std::array<float, 3>> position) {
    std::string_view source = event;
    const auto mapped = std::find_if(sounds_.begin(), sounds_.end(), [&](const auto& entry) { return entry.first == event; });
    if (mapped != sounds_.end()) {
        // [INFERENCE] 00458eb0 stores per-file chance; selection walks cumulative chances.
        float roll = std::uniform_real_distribution<float>(0, 1)(random_);
        source = {};
        for (const auto& [file, chance] : mapped->second) {
            if (roll < chance) { source = file; break; }
            roll -= chance;
        }
        if (source.empty()) return trace("silent", event, "sound map roll selected no file", position);
    }
    const auto path = original_audio_path(source);
    if (path.empty()) return trace("failed", event, "invalid original audio path", position);
    try {
        auto& bytes = files_[path];
        if (!bytes) {
            auto loaded = std::make_shared<const std::vector<std::uint8_t>>(read_file(base_ / "audio" / path, assets::max_raw_audio_bytes));
            // ponytail: whole-cache reset at 64 MiB; playing voices keep their shared bytes. LRU if churn shows.
            if (file_bytes_ + loaded->size() > 64 * 1024 * 1024) {
                files_.clear();
                file_bytes_ = 0;
            }
            file_bytes_ += loaded->size();
            bytes = files_[path] = std::move(loaded);
        }
        if (mixer.effect(bytes, "audio/" + path, position)) trace("effect", event, path, position);
        else trace("dropped", event, path + " (32 voices busy)", position);
    } catch (const std::exception& error) {
        files_.erase(path);
        trace("failed", event, path + ": " + error.what(), position);
    }
}
void GameAudio::select_bed(Audio::Bed bed, std::size_t index) {
    const auto& list = bed == Audio::music ? songs_ : beds_;
    const auto* what = bed == Audio::music ? "music" : "ambient";
    if (bed == Audio::music) song_ = index; else bed_ = index;
    if (list[index] == "nothing") {
        mixer.stop(bed);
        return trace(what, std::to_string(index), "nothing");
    }
    const auto path = original_audio_path(list[index]);
    try {
        mixer.bed(bed, read_file(base_ / "audio" / path, input_limit), bed == Audio::ambient);
        trace(what, std::to_string(index), path);
    } catch (const std::exception& error) {
        mixer.stop(bed); // 00455eb0: "couldn't load background music" leaves no stream.
        trace("failed", what, path + ": " + error.what());
    }
}
// 004573e0: 0 (or MusicVolume <= 0.01) stops, 255 keeps, otherwise a playlist index.
// Music changes only when the index changes and plays once; ambient restarts while silent.
void GameAudio::choose(std::uint8_t music, std::uint8_t sound) {
    const bool muted = music_volume_ <= 0.01f;
    if (muted || music == 0) {
        if (music_on_) { mixer.stop(Audio::music); music_on_ = false; trace("music", "stop", muted ? "muted" : "grid 0"); }
    } else if (music != 255 && music < songs_.size() && music != song_) {
        select_bed(Audio::music, music);
        music_on_ = true;
    }
    if (muted || sound == 0) {
        if (bed_on_) { mixer.stop(Audio::ambient); bed_on_ = false; trace("ambient", "stop", muted ? "muted" : "grid 0"); }
    } else if (sound != 255 && sound < beds_.size() && (!bed_on_ || sound != bed_)) {
        select_bed(Audio::ambient, sound);
        bed_on_ = true;
    }
}
void GameAudio::frontend(bool credits) {
    // 004573e0 with DAT_005ea950 == 0: 8 while the credits window 0x77d604 (built by 004afed0) is visible (+0xd4), else 2.
    if (!override_) choose(credits ? 8 : 2, 0);
}
void GameAudio::frame(const Scene& scene, std::span<const RenderEntity> poses, std::uint32_t self,
                      const CameraBasis& listener) {
    mixer.listener(listener.eye, listener.right);
    const auto pose = [&](std::uint32_t id) {
        const auto found = std::find_if(poses.begin(), poses.end(), [id](const RenderEntity& p) { return p.id == id; });
        return found == poses.end() ? nullptr : &*found;
    };
    const auto* player = pose(self);
    if (!override_ && player && scene.source_support < 18) {
        if (grid_zone_ != scene.source_support) {
            // 00419a40: musicNN.raw / soundNN.raw are 2048x2048 byte grids; a missing file is all zero.
            for (auto [grid, name] : {std::pair{&music_grid_, "music"}, std::pair{&sound_grid_, "sound"}}) {
                char file[32];
                std::snprintf(file, sizeof file, "%s%02u.raw", name, scene.source_support);
                grid->assign(grid_bytes, 0);
                std::ifstream input(base_ / file, std::ios::binary);
                if (input) input.read(reinterpret_cast<char*>(grid->data()), static_cast<std::streamsize>(grid_bytes));
            }
            grid_zone_ = scene.source_support;
            trace("zone", std::to_string(grid_zone_), "music/sound grids");
        }
        if (const auto cell = audio_grid_index(player->x, player->z)) choose(music_grid_[*cell], sound_grid_[*cell]);
    }
    // 0045c890: clip audio at the actor when a non-looping clip starts or a looping clip wraps.
    next_cues_.clear();
    for (const auto& assembly : scene.actor_assemblies) {
        if (assembly.controller >= scene.actors.size()) continue;
        const auto& actor = scene.actors[assembly.controller];
        next_cues_.emplace_back(assembly.entity_id, actor.audio_cue);
        const auto old = std::find_if(cues_.begin(), cues_.end(), [&](const auto& cue) { return cue.first == assembly.entity_id; });
        if (old == cues_.end() || old->second == actor.audio_cue || actor.library >= scene.actor_tables.libraries.size()) continue;
        const auto& clips = scene.actor_tables.libraries[actor.library].clips;
        if (actor.current_clip >= clips.size() || clips[actor.current_clip].audio.empty()) continue;
        const auto* at = pose(assembly.entity_id);
        if (!at) continue; // The original plays only for an entity in the visible scene.
        const auto& audio = clips[actor.current_clip].audio;
        play_event(audio[random_() % audio.size()], std::array{at->x, at->ground_y, at->z});
    }
    cues_.swap(next_cues_);
}
} // namespace ikv
