# Original RAW audio: fixed-format recovery

## Provenance and method

This is original-loader static evidence, not a format guessed from file length or plausible playback. Ghidra 12.1.2 headless / Java 27 processed an **isolated copy** of the saved `IKVUnpacked` project. The analyzed PE SHA256 is `44ffa8389a9f6e4c35841103ad4f40a90513e4deb518ab5605b0a1ebfae0be2d`, image base `0x00400000`. Original installer/extracted files were not modified or executed; no Windows runtime, official endpoints or audio playback was used here. Local decompilation/disassembly remains ignored under `analysis/` and is not distributed.

`tools/audio_ghidra/AudioEvidence.java` exports bounded function listings, references and decompilation. First export covered the saved sound functions; the second followed sample constructors. The original saved analysis omitted virtual method `0x00453530`; one focused export therefore failed on its missing function. The isolated script then disassembled/created missing functions and cleared known false no-return propagation on allocator cleanup helpers. Final `analysis/audio-raw/` export succeeded. This correction changes analysis only, not the PE bytes. Assembly stores and direct read calls below are the decisive evidence; incompletely inferred C++ signatures are not treated as original source prototypes.

For a fresh output/project location:

```sh
cp -a analysis/unpacked-project analysis/audio-project
/opt/ghidra/support/analyzeHeadless analysis/audio-project IKVUnpacked \
  -process istanbul.exe -noanalysis -scriptPath tools/audio_ghidra \
  -postScript AudioEvidence.java analysis/audio-raw \
  004534f0 00453500 00453530 00453580 00452e60 00452d90 00452d60 00453140 004543d0 \
  -max-cpu 2 -log analysis/audio-raw-ghidra.log
python3 tools/audio_inventory.py docs/evidence/asset_inventory.json \
  --root original/app --output docs/evidence/audio-format.json
```

Use a different destination when the copied project already exists. RAW metadata comes from the existing read-only inventory and recovered loader descriptor, not a rehash or fresh read of RAW payloads. Optional `--root` probes each inventoried audio Ogg using `ffprobe -v error -show_entries stream=codec_name,sample_rate,channels:format=duration -of json`, with a 20-second per-file timeout and checked exit/JSON metadata. Paths resolving outside that installation root (including escaping symlinks) reject, as does an output inside the installation. Probe failures are recorded with their paths and make the command exit unsuccessfully; codec/duration facts are never guessed.

## Loader dataflow and exact format

1. `0x004580f0` selects the extension string `raw` at `0x005937b0`, calls `0x00457770`, and constructs the sample via `0x004534f0`. That constructor installs vtable `0x00593580`; its first method is `0x00453580`. Separate branches select `wav` and `ogg`; the WAV loader reads RIFF/FMT/DATA while RAW does not.
2. The RAW method prefixes the resource with UTF-16 `audio\\` (`0x005935d0`), opens using `0x00453140` (`fopen(...,"rb")`), and obtains the entire file length at `0x0045365a` via `0x00452d90` (`fseek`/`fgetpos`, restoring position).
3. `0x004535db–0x00453600` writes an 18-byte `WAVEFORMATEX` passed by descriptor pointer to DirectSound buffer creation at `0x00453753` (device vtable `+0x0c`):

| Field | Observed store | Meaning |
|---|---|---|
| `wFormatTag` | word `1`, `0x004535e0` | PCM |
| `nChannels` | word `1`, `0x004535e5` | mono |
| `nSamplesPerSec` | dword `0xac44`, `0x004535ea` | 44100 Hz |
| `nAvgBytesPerSec` | dword `0x15888`, `0x00453600` | 88200 bytes/s |
| `nBlockAlign` | word `2`, `0x004535f9` | two bytes/frame |
| `wBitsPerSample` | word `0x10`, `0x004535f2` | signed 16-bit PCM under WAVE PCM semantics |
| `cbSize` | word `0`, `0x004535db` | no codec extension |

4. The buffer byte count is the full file length. `0x004537a3` locks the complete buffer (`IDirectSoundBuffer::Lock`, vtable `+0x2c`, flags `2`). `0x004537be` calls `0x00452e60`, which is directly `fread(destination,1,count,file)`; `0x00453803` unlocks it (vtable `+0x4c`). **No header skip, decompressor, delta transform or codec blocks intervene.** On this x86/WAVE path the stored samples are little-endian. The only block unit is one mono frame = two bytes.

Consequently all loader-selected audio `.raw` families use **headerless PCM signed LE16, mono, 44100 Hz**. There is no separate recovered RAW music codec: the supplied music directory contains Ogg tracks; long spoken/ambient RAW resources use the same PCM path.

## Corpus and native boundaries

`audio-format.json` records exact aggregate counts, family counts, representative hashes/durations, every rejected original file and all non-audio RAW grid paths. The saved inventory contains **371 RAW**: **347 under `base/audio/`** and **24 root music/sound selection grids**, plus **77 Ogg**. Of the audio RAW files, **328 have complete nonempty two-byte frames** and **19 have odd byte counts**. Odd file lengths do not establish a second codec; the original extension-based loader passes those bytes to DirectSound with the same fixed descriptor. Original DirectSound malformed-size acceptance/failure was not exercised.

The independent native policy rejects input with no complete frame (0–1 bytes), larger-than-32-MiB input and non-audio-root paths. An odd-length file keeps its whole two-byte frames, and the trailing incomplete byte is excluded, never padded. Three of the 19 odd files are referenced by the original (`ui\windowopen.raw`, `ui\windowclose.raw` and `ui\warning.raw`, loaded by `0048eb70`), so rejecting them would silence original UI sounds. This is a defensive native boundary, **not** a claim that the original validates those cases identically. Root `musicNN.raw` and `soundNN.raw` are resource-selection grids, never PCM; their loader `0x00419a40` reads separate 4-MiB grid buffers. No RAW audio family remains codec-opaque after this recovery. Spatial and event associations are recovered separately [below](#original-sound-events-and-playback).

## Original duration and native-limit audit

The command above completed against `original/app`: **77/77 Ogg probed, zero errors**, without decoding or playback. All report **Vorbis**; 76 report 44100 Hz and one 24000 Hz; 68 are stereo and nine mono. Container durations sum to **15700.942026 seconds**. Actual file-size metadata totals **243787574 encoded bytes**. RAW lengths remain saved-inventory metadata: 347 audio files total **72396875 bytes**, or **820.8262471655329 nominal seconds** at 88200 bytes/s. The 19 odd lengths are `incomplete_frame` in the inventory; native playback uses their whole frames only. The 24 non-audio selection grids are excluded.

| Corpus | Longest resource | Duration | Above 300 s | Above 10 s |
|---|---|---:|---:|---:|
| Ogg, actual ffprobe metadata | `base/audio/music/tuneller.ogg` | 527.281633 s | 17 | 76 |
| RAW, fixed descriptor + inventory length | `base/audio/ambient/hakirpropaganda.raw` | 81.69691609977325 s | 0 | 9 |

`audio-format.json` stores every per-file duration and **exact exceeding paths**, under `ogg_duration_audit.summary` and `raw_duration_audit.summary`. Longest/largest encoded Ogg is `tuneller.ogg`, **7929237 bytes**; no Ogg exceeds the native 32-MiB encoded-input bound. The decoded stereo48000 S16LE estimates use **`ceil(ffprobe duration * 48000) * 4`**, not measured decoded lengths: total **3014581032 bytes**, maximum **101238076 bytes** for `tuneller.ogg`. **13 Ogg estimates exceed 64 MiB**; their exact paths are also in the JSON.

The **300-second background/loop**, **10-second action/FX**, **32-MiB input** and **64-MiB PCM-cache** boundaries are independent native policies, **not canonical limits recovered from the original**. Consequently these bounds do not cover the entire supplied corpus: 17 original Ogg durations exceed the loop bound, while 13 estimates exceed the entire PCM-cache capacity. A cache capacity is not evidence of an original duration ceiling. Both duration thresholds were compared against every audio resource; exceeding the FX threshold does not establish that a music/ambient resource is actually assigned to FX playback. Full decoder, cache, audible-output and canonical-mixer behavior were not exercised by this metadata audit.

## Native API, integration and check

`include/ikv/audio_asset.hpp` / `src/audio_asset.cpp` expose standard-library C++20 `ikv::assets::RawAudioView { sample_rate, channels, pcm }`, `inspect_raw_audio(bytes,asset_path)` (zero-copy) and `read_raw_audio(stream,byte_size,asset_path)` (bounded exact read). Provenance paths must be `audio/...raw` or `base/audio/...raw`, with ASCII case-insensitive comparison and slash/backslash normalization; traversal and empty/dot path components reject. A RAW payload has no magic, so path provenance is essential and this API does not claim to identify arbitrary bytes as audio.

The client audio adapter uses the confirmed `SDL_AUDIO_S16LE`, mono44100 source descriptor in the real SDL3 playback stream; original Ogg decoding remains the existing real ffmpeg/Vorbis path. No sample reinterpret-cast or byte-to-host-int16 conversion is needed. Distribution contains code and metadata, not proprietary samples.

Parent integration check (not built/run by this recovery worker):

```sh
g++ -std=c++20 -Iinclude tools/audio_asset_check.cpp src/audio_asset.cpp -o /tmp/ikv-audio-asset-check
/tmp/ikv-audio-asset-check
/tmp/ikv-audio-asset-check original/app/base/audio/ambient/break_desk.raw base/audio/ambient/break_desk.raw
```

The check uses synthetic bytes for malformed/path/short-read boundaries and optionally reads a user-owned original RAW for frame metadata. Parent must separately exercise actual SDL playback; dummy output proves queue/decoder behavior, not audible output or canonical original mixer parity.

## Original sound events and playback

All of this is static evidence from `analysis/unpacked/istanbul.exe`, exported with `tools/audio_ghidra/AudioEvidence.java` on the isolated `analysis/audio-project` copy. `@ADDR` selects the function containing an instruction. Each export's `program.txt` records the PE SHA256 and the instruction listing. The exports are `analysis/audio-events/`, `analysis/audio-source3d/` and `analysis/audio-ui/`.

| Behaviour | Original evidence | Native |
|---|---|---|
| Sound tables | `004596f0` opens DirectSound and the listener: distance factor 1.0, rolloff 1.0, Doppler 1.0. It loads `data\playlist.dat` and `data\playlistsound.dat` (vectors of `0x1c` strings in file order) and `base\audio\sounds.xml` (`<Sounds><Sound name><file chance>`, parsed by `00458eb0`). | `load_source_playlist` (shared `read_source_table` decoder); `GameAudio` parses `sounds.xml` with Boost. The shipped `sounds.xml` holds only `test_sound*`. |
| Zone grids | `00419a40` reads `music%.2d.raw` and `sound%.2d.raw` (4 MiB each; zeroed when missing) for the current zone index. | `GameAudio::frame` loads them on a zone change. |
| Music/ambient selection | `004573e0`, every frame: player `+0x168`/`+0x170` → `00444af0(v*0.6666667+1024)`, which rounds half away from zero; byte at `iz*0x800+ix`. Without a player (`DAT_005ea950` 0, login/character screen) music index is 8 while the credits window `0x77d604` (built by `004afed0`) has `+0xd4` set, else 2, and ambient index 0. MusicVolume ≤ 0.01 or byte 0 stops. Byte 255 keeps the current track. Music switches only when its index differs from the current one (`+0x68`). Ambient restarts when the index differs or the stream is not playing (`+0x6c`, `+0x360`). | Same rules; `audio_grid_index`; `GameAudio::frontend` on the character screen (2 `music\anatema.ogg`, credits 8 `music\istanbulda.ogg`). |
| Background streams | `00455eb0` (music) and `00456130` (ambient) delete the old stream object, then open `audio\`+playlist path; `nothing` loads nothing. The `004541f0` refill calls stop at Ogg EOF, so music plays once. There is no crossfade. | Off-thread decode, immediate replacement, music `loop=false`, ambient looped. |
| Clip sounds | `0045dba0` loads `audio`/`audio2`/`audio3` samples into the clip's list (`+0x7c`). `0045c980` (clip entry) plays when the clip is not looping. `0045cb9e` (loop wrap in the per-frame update) replays. `0045c890` checks entity visibility, picks `rand()%n`, creates a 3D source (`00456b10`), places it at the actor and plays. | `ActorClip::audio`, `ActorAnimation::audio_cue`, `GameAudio::frame`. |
| 3D source | `00454070`: `SetMinDistance(25.0)`, `SetMaxDistance(1e9)`, `SetMode(NORMAL)`. `00454140`: `SetPosition`. `00454890`, every frame: listener `SetPosition`/`SetOrientation`/`SetVelocity` from camera globals `0x77c07c`/`0x77c160`. | Inverse distance `min(1,25/d)` from the camera eye, plus a balance pan on the camera right axis [INFERENCE: DirectSound's pan law is not reproduced exactly]. |
| Effect volume | `00453d80` play sets volume from EffectVolume (`0x5eab88`). `00453f20` converts the linear value to DirectSound hundredths of a dB. Prefs `EffectVolume`/`MusicVolume` (`0x41b4a0`). | Master `volume`, plus `music_volume` on the beds. |
| UI sounds | `0048eb70` loads into `0x77d2f4..0x77d328` (base `0x77d250` + `0xa4..0xd8`): `alert`, `click`, `craft`, `drink`, `eat`, `gold`, `eat` again (`0x77d30c`), `repair`, `warning`, `itemdrop`, `weapon\handgun\ateslisilah1`, `windowclose`, `windowopen`, `click` again. Every play checks the sound flag `0x614038`; the complete set of users: window toggle `0042e970` (open/close); button callbacks `00490120`, `00490170`, `004901b0`, `004909a0` (click); Dikkat box `004b8230` (param 0 yes/no → `alert`, param 1 one button → `warning`; 22 callers: buy, sell, destroy, group/lonca invites, Kabadayı, group kick/leave, klan leave, recipe/quest delete, character delete → alert; disconnect 'sjfr', version refusal, 'rfor' message, name checks, character count/selection → warning); `00430610` itsu (`0x77d30c`); 'mkcy' `004a9690` / 'skua' `004a9880` confirmed moves (`0x77d30c` when exactly one side is a worn slot < 0xd, else `itemdrop`); `0040b170` channel-9 "Kritik!" line (`ateslisilah1`). `craft`, `drink`, `gold`, `repair` and the first `eat` are never played. | All wired: `ask()` Dikkat confirms and the character screen's box (alert/warning), `disconnect()` (warning), `use_item` (eat), server `ack` of equip/unequip (eat) and item_move/bank_deposit/bank_withdraw (itemdrop), own critical hits (ateslisilah1, receiver NONCANONICAL). `craft.raw`/`repair.raw`/gather `itemdrop.raw` cues are NONCANONICAL. |
| Skill sounds | `0044dad0` loads skills.dat `sound`/`femalesound`. `004a6a80` (skill/effect message) calls `00456c00` (play a sample at x,y,z), using the female sample for even source classes when present. | `SourceSkill::sound`/`female_sound`, played by the skill event path. |

Coverage: the recovered tables and code reference 360 of the 424 supplied `.raw`/`.ogg` resources. Of the unreferenced ones, `male_damage1..9`, `male_melee*`, `weapon\barehand\hit*`/`dmg*`, `ui\itemuse.raw` and `ui\kritik.raw` have no trigger in this client. `animation.dat` also names three files that do not exist (`monster\fare\damage.ogg`, `monster\gizit\h_break.raw`, `monster\tirtil\hit1-3.raw`); the native client traces them as failed. Level-up and chat-message sounds do not exist in the original client. Scripted proof of the UI slots and character-screen music: `analysis/porting-wave8/AudioResidue/session.py` → `run/trace.tsv`.

Checks:

- `ctest -R original-audio-events` (in `build-audio`) runs `tests/audio_check.cpp` on SDL dummy output. It covers table order, grid lookup (Yeni Cami → music 3 `music\avlu.ogg`, ambient 8 `nothing`), the rat clip cue through the real `set_actor_action`, UI/odd/sounds.xml events, rejections, voice exhaustion and the MusicVolume stop.
- `analysis/porting-wave1/Audio/session.py BUILD OUT` runs a scripted native session on its own disposable stack and Xvfb, with `SDL_AUDIO_DRIVER=disk`. It writes `trace.tsv` (one line per sound) and `session.wav`.
