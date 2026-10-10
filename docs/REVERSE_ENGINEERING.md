# Original binary reconnaissance and reproducible static analysis

## Evidence boundary

The user supplied a legitimate İKV installer. Inputs and extracted assets are preserved under the local workspace and excluded from publication. Analysis used native Linux **innoextract 1.9**, **UPX 5.2.1**, Python standard-library readers and **Ghidra 12.1.2** headless on Java 27. Ghidra is the primary code/xref/decompilation tool.

No original Windows executable was run. No Wine/Proton, original login/session, packet capture, endpoint probe, official DNS lookup, replay or credential/token collection was used. Downloading public analysis-tool releases is distinct from contacting game services. The independently implemented native harness is not evidence of original behavior.

Confidence throughout: **C-static** = observed bytes, import table, xrefs or decompiled operations; **I** = interpretation; **U** = unknown. **No original behavior is dynamically confirmed.**

## Preserved inputs and identities

Installer SHA256: `34bb864018ad0c46a06a26d8bf0b4c48cd4bcf4c8db971c64b2ebda1b6fbdca7` (`istanbulkur6_0.exe`). Native extraction completed successfully into `original/app/`; the Inno package reports İstanbul Kıyamet Vakti 6.0 and data version 5.2.0. These are package metadata, not inferred wire-protocol versions.

| Payload | Bytes | SHA256 | Role / architecture |
|---|---:|---|---|
| `original/app/istanbul.exe` | 1,077,656 | `bff4cffe753a04e237e3ed32c3798eda10a602d73424a2b228024762da44aba6` | Main game candidate confirmed by game startup/D3D/network paths; x86 PE32, packed |
| `original/app/launcher.exe` | 2,259,352 | `096a9fa7c3bd701d5b486fee9862786b8c0a4cacbc0656250910364e10890e2e` | Launcher/patcher; x86 PE32 |
| `original/app/_launcher.dll` | 2,259,352 | `096a9fa7c3bd701d5b486fee9862786b8c0a4cacbc0656250910364e10890e2e` | Byte-identical to launcher.exe, including executable characteristics; extension alone is not DLL evidence |
| `original/app/launch_helper.exe` | 111,616 | `df082b4536ab552bb6445203a03cc50e7469b48c08ff0a1c462e5f3cc46890b3` | x86 helper, imports Kernel32/User32/Shell32; exact launch behavior unassigned |
| `original/app/d3dx9_31.dll` | 2,414,360 | `e2065619fe6eb0034833b1dc0369deb4a6edc3110e38a1132eeafcf430c578a5` | x86 DirectX utility DLL; imports msvcrt plus platform APIs |
| `analysis/unpacked/istanbul.exe` | 2,300,312 | `44ffa8389a9f6e4c35841103ad4f40a90513e4deb518ab5605b0a1ebfae0be2d` | Derived static-decompression analysis copy, never executed or redistributed |

All images have preferred image base `0x00400000`. PE subsystem is Windows GUI. Header timestamps: game 2018-03-01 12:27:45 UTC; launcher/copy 2018-03-01 12:11:34 UTC; helper 2018-02-12 13:58:01 UTC; D3DX DLL 2006-09-28 22:13:05 UTC. Header timestamps are stored claims, not trusted build chronology. Linker-version bytes are 7.10 for game and D3DX, 9.0 for launcher/helper. Exact compiler/optimization version is **U**. C++ runtime/exception/security-cookie/RTTI/standard-library patterns support MSVC-family code (**I**); do not treat Ghidra's `windows` compiler specification or function-ID matches as a compiler-version certificate.

## Packing and safe static cutover

Packed game sections are `UPX0` (3,182,592 virtual bytes, zero raw bytes), `UPX1` (786,432 raw bytes) and `.rsrc`. Packed entry is `0x007c9c30`. Ghidra's real first import/analysis completed, but its decompiler encountered an uninitialized-memory destination at `0x00571f47`; the packed entry exposes the unpacker, not recoverable game code.

The native upstream UPX release statically decompressed **to a new analysis file** using `-d -o`; original input was not overwritten. UPX reported `2,300,312 <- 1,077,656`, `win32/pe`, `Unpacked 1 file`. Decompressed sections are `.text`, `.rdata`, `.data`, `.rsrc`; restored PE entry is `0x00571f47`. The unpacked entry reaches application startup `0x00464dc0`.

Tool provenance: [UPX 5.2.1 upstream release](https://github.com/upx/upx/releases/tag/v5.2.1), [GitHub release metadata](https://api.github.com/repos/upx/upx/releases/latest). The downloaded `upx-5.2.1-amd64_linux.tar.xz` SHA256 was `402162aad30af47e60dbd767fb2e64ca394ace9727ba1f40283641f1d1b91657`, matching the upstream asset digest. Native binaries and archives remain local under ignored `.tools/`. No anti-cheat or authentication bypass was attempted.

## Verified APIs and recovered bounded traces

### Game

The decompressed PE import inventory and Ghidra xrefs agree on these Winsock ordinal roles:

| Ordinal | Canonical API | Import thunk VA |
|---:|---|---|
| 3 | closesocket | `0x0056db9a` |
| 19 | send | `0x0056dba0` |
| 16 | recv | `0x0056dba6` |
| 116 | WSACleanup | `0x0056dbac` |
| 4 | connect | `0x0056dbb2` |
| 7 | getsockopt | `0x0056dbb8` |
| 21 | setsockopt | `0x0056dbbe` |
| 23 | socket | `0x0056dbc4` |
| 9 | htons | `0x0056dbca` |
| 111 | WSAGetLastError | `0x0056dbd0` |
| 52 | gethostbyname | `0x0056dbd6` |
| 115 | WSAStartup | `0x0056dbdc` |

Names were resolved from the documented [WS2_32 export ordinal specification](https://raw.githubusercontent.com/wine-mirror/wine/master/dlls/ws2_32/ws2_32.spec); consulting this source did not execute Wine. Raw inventory preserves ordinals. `ApplyWinsockTypes.java` annotates names and x86 stdcall parameter footprints, with intentionally generic pointer/scalar types. It does not recover class/message structures.

Bounded network chain: startup `0x00464dc0` → Winsock/workers `0x0045c4d0` → TCP resolver/connection `0x0045a9f0`; outgoing command construction `0x0045b380` → worker `0x00459f50` → exact-send `0x00459ba0`; incoming worker `0x0045c2e0` → exact-read `0x00459c10` → queue `0x0045c170` → main-thread pump `0x0045c1e0` → **confirmed dispatcher** `0x0045b4b0`.

This recovers **LE32 body-length framing**, a raw command32 prefix in outgoing commands, one statically derived startup frame and **58 inbound selector IDs**. Incoming frame bodies can contain several handler-sized commands, not necessarily one command per frame. Details and explicit unknown fields are in [PACKET_CATALOG](network/PACKET_CATALOG.md).

The bounded ordinary import table contains no `sendto`, `recvfrom`, `select`, `WSAAsyncSelect`, `WSAEventSelect`, `WSARecv`, `WSASend`, `getaddrinfo`, WinHTTP, WinINet or named TLS-library calls. This is an import-table observation, not proof that dynamically loaded or embedded code cannot provide other transports. `LoadLibrary`/`GetProcAddress` and browser-like COM paths exist. No application encryption/compression conclusion follows solely from absent imports.

Graphics C-static evidence: `Direct3DCreate9`, `D3DPERF_*`, D3DX file/memory texture metadata and creation, surface load/save, shader assembly, matrix/project helpers. `0x00468430` creates/enumerates/checks the D3D9 device; `0x0045ef00` uploads memory texture data; `0x00467a80`/`0x00467c50` assemble vertex/pixel shaders. This is **Direct3D 9**, not a proved OpenGL original. `DirectInput8Create` is called by `0x00483ba0`; DirectSound ordinal 11 is called in `0x004596f0`. Camera/control mappings and actual audio mix behavior are unassigned.

Other verified libraries include Kernel32/User32/Shell32/OLE/OLEAUT32/PSAPI/version/iphlpapi. Asset I/O includes CreateFileW/ReadFile/SetFilePointer and CRT fread/fopen wrappers. The recursive texture-pack parser `0x00461430` reads 68-byte directory and 136-byte file records, corroborating independently inspected SKYF archive indexes. `0x00419a40` reads zone music/sound `.raw` resources as 4-MiB blocks; those are not PCM evidence. [ASSET_FORMATS](ASSET_FORMATS.md) and [ENGINE_ARCHITECTURE](ENGINE_ARCHITECTURE.md) cover asset-worker byte observations.

RTTI includes Boost bound callbacks and application texture/audio/particle types; collision source-path/assertion strings such as `collision_trimesh.cpp` suggest ODE-related embedded collision code (**I**). No exact engine/middleware version or generic third-party engine identity is established.

### Launcher

Launcher analysis completed independently on the unpacked ordinary PE. Imports cover Winsock, LDAP, COM/OLE, GDI/common-controls/dialogs, shell and platform APIs. Embedded libcurl diagnostic strings are **C-static**, so a libcurl-family static networking implementation is **I-high**, not a claim about a particular version or all enabled protocols. Relevant HTTP URL constructors at `0x0048dbb0` and `0x0048dbe0` hold update-manifest and server-list literals. Endpoint bytes, image/file offsets, exact xrefs and non-service certificate exclusions are in [ENDPOINT_DISCOVERY](network/ENDPOINT_DISCOVERY.md).

## Reproduction

Run from the repository root against your own supplied inputs. Use a fresh Ghidra project directory for a repeat import; Ghidra will otherwise refuse to overwrite an existing project/file. The commands below are the actual analysis command shapes used. They never execute the PE image.

```sh
# If extracting anew, use a new destination rather than replacing preserved inputs.
.tools/innoextract-1.9-linux/innoextract -d original istanbulkur6_0.exe
python3 tools/pe_inventory.py original/app analysis/pe_inventory.json

# Native UPX only, sourced from the upstream release above.
curl -fL https://github.com/upx/upx/releases/download/v5.2.1/upx-5.2.1-amd64_linux.tar.xz \
  -o .tools/upx-5.2.1-amd64_linux.tar.xz
printf '%s  %s\n' 402162aad30af47e60dbd767fb2e64ca394ace9727ba1f40283641f1d1b91657 \
  .tools/upx-5.2.1-amd64_linux.tar.xz | sha256sum -c -
tar -xJf .tools/upx-5.2.1-amd64_linux.tar.xz -C .tools
mkdir -p analysis/unpacked analysis/ghidra-project analysis/launcher-project analysis/unpacked-project
.tools/upx-5.2.1-amd64_linux/upx -d original/app/istanbul.exe -o analysis/unpacked/istanbul.exe
python3 tools/pe_inventory.py analysis/unpacked analysis/unpacked_inventory.json

/opt/ghidra/support/analyzeHeadless analysis/ghidra-project IKV \
  -import original/app/istanbul.exe -scriptPath tools/ghidra \
  -postScript ExportEvidence.java analysis/game -max-cpu 2 \
  -analysisTimeoutPerFile 300 -log analysis/game-ghidra.log -scriptlog analysis/game-script.log

/opt/ghidra/support/analyzeHeadless analysis/launcher-project IKVLauncher \
  -import original/app/launcher.exe -scriptPath tools/ghidra \
  -postScript ExportEvidence.java analysis/launcher -max-cpu 2 \
  -analysisTimeoutPerFile 300 -log analysis/launcher-ghidra.log -scriptlog analysis/launcher-script.log

/opt/ghidra/support/analyzeHeadless analysis/unpacked-project IKVUnpacked \
  -import analysis/unpacked/istanbul.exe -scriptPath tools/ghidra \
  -postScript ExportEvidence.java analysis/unpacked-game -max-cpu 2 \
  -analysisTimeoutPerFile 300 -log analysis/unpacked-ghidra.log -scriptlog analysis/unpacked-script.log

/opt/ghidra/support/analyzeHeadless analysis/unpacked-project IKVUnpacked \
  -process istanbul.exe -noanalysis -scriptPath tools/ghidra \
  -postScript ApplyWinsockTypes.java \
  -postScript ExportEvidence.java analysis/focused-game \
    00459f50 0045c2e0 00459ba0 00459c10 0045a9f0 0045c4d0 00464dc0 004431d0 00468430 \
  -max-cpu 2 -log analysis/focused-ghidra.log -scriptlog analysis/focused-script.log

/opt/ghidra/support/analyzeHeadless analysis/unpacked-project IKVUnpacked \
  -process istanbul.exe -noanalysis -scriptPath tools/ghidra \
  -postScript ExportEvidence.java analysis/trace-game \
    0045c170 0045c1e0 0045b380 0045a1d0 00461790 0045dba0 00419a40 00465710 004438b0 \
  -max-cpu 2 -log analysis/trace-ghidra.log -scriptlog analysis/trace-script.log

/opt/ghidra/support/analyzeHeadless analysis/unpacked-project IKVUnpacked \
  -process istanbul.exe -noanalysis -scriptPath tools/ghidra \
  -postScript ExportEvidence.java analysis/dispatch-game 0045b4b0 00461430 00446900 \
  -max-cpu 2 -log analysis/dispatch-ghidra.log -scriptlog analysis/dispatch-script.log

/opt/ghidra/support/analyzeHeadless analysis/unpacked-project IKVUnpacked \
  -process istanbul.exe -noanalysis -scriptPath tools/ghidra \
  -postScript ExportEvidence.java analysis/input-game 004d2610 \
  -max-cpu 2 -log analysis/input-ghidra.log -scriptlog analysis/input-script.log

python3 tools/command_catalog.py analysis/dispatch-game/decompile/0045b4b0.c \
  analysis/original_commands.json
```

`tools/pe_inventory.py` uses only the Python standard library and emits architecture, section bounds/characteristics, PE entry, hashes, raw imports/IAT addresses and candidate endpoint offsets. It reads the original files; it does not mutate or execute them. It is a bounded reconnaissance reader, not a hardened parser for arbitrary hostile PEs or a certificate validator.

`python3 tools/analysis_selfcheck.py` is a small dependency-free runnable check for synthetic PE/import parsing and command-byte metadata extraction. The main agent executed it successfully: exit 0, stdout `analysis reader synthetic checks passed`. It contains no proprietary bytes and checks analysis tooling only; it is not original-binary runtime evidence or original-client compatibility validation.

`ExportEvidence.java` emits `program.txt`, `imports.tsv`, `strings.tsv`, `functions.tsv` and bounded `decompile/<VA>.c` into the provided ignored output directory. It selects API/string references, one caller layer, and optional explicitly supplied function addresses; missing function boundaries at supplied code addresses are created for analysis. Export cap is 300 functions, with per-function decompilation timeout 30 seconds. `program.txt` reports selected count; not all functions in a binary are exported. The launcher initial selection had 405 functions, so the cap intentionally omitted the tail; its documented endpoint initializers were exported. Later exporter selection additions may alter counts on reproduction; this does not alter the input hashes or address-based findings. The focused game run selected 219 functions and exported the requested workers.

## Generated evidence and observed result

The packed main, launcher and unpacked game imports all completed real Ghidra analysis and saved local projects. Supplemental focused/trace/dispatcher/input exports completed and saved the processed project. Ghidra could not load many absent Windows system DLLs; ordinary PE imports remained available, and Winsock ordinals were explicitly annotated from a published export table. Logs retain warning/error history, including an initial Java method-argument compilation mismatch corrected before the successful focused run. A native Python command-catalog syntax typo was likewise corrected before its successful extraction. No warnings were hidden or counted as compatibility success.

| Local artifact | Content |
|---|---|
| `analysis/pe_inventory.json` | Five original PE inventory records and hashes |
| `analysis/unpacked_inventory.json` | Derived image hash, restored entry/imports/sections |
| `analysis/game/`, `analysis/game-ghidra.log` | Packed image evidence and actual initial analysis |
| `analysis/launcher/`, `analysis/launcher-ghidra.log` | Original launcher analysis and endpoint references |
| `analysis/unpacked-game/`, `analysis/unpacked-ghidra.log` | First real unpacked-game auto-analysis |
| `analysis/focused-game/`, `analysis/focused-ghidra.log` | Typed Winsock calls and recovered receive/send workers |
| `analysis/trace-game/`, `analysis/trace-ghidra.log` | Frame queues/pump, command builder, pack bootstrap, zone data, frame driver |
| `analysis/dispatch-game/`, `analysis/dispatch-ghidra.log` | Confirmed command dispatcher and recursive archive parser |
| `analysis/input-game/`, `analysis/input-ghidra.log` | DirectInput thunk and caller evidence |
| `analysis/original_commands.json` | 58 ID comparisons, little-endian byte text, line anchors and call targets |
| `analysis/*-project/` | Local Ghidra projects; proprietary binary analysis data, never publish |

## Reconstruction status and limits

[FUNCTION_MAP](FUNCTION_MAP.md) records original VAs, callers, logical parameters, confidence and independent-native status. The native original-envelope component is derived from bounded framing facts, not proprietary source translation. Its defensive cap, fragmented/coalesced stream handling and truncated-input rejection are new safe implementation policy, not demonstrated official-client behavior. The big-endian LOCAL TEST client/server remains a separate protocol.

This analysis establishes an actionable narrow envelope and dispatch map, not full interoperability. Original authentication/session interchange, command field layouts and accepted limits, authoritative world state, entities/combat/inventory/quests, animation/skeleton/mesh semantics, camera bindings and original rendered scenes remain unreconstructed. No Windows execution or official traffic experiment is required or implied by these gaps. Extend analysis against preserved local inputs and, when authorized, only user-owned synthetic/private test environments; preserve the distinction between static evidence and experiments.
