# Original-client packet catalog

This catalog concerns the supplied original client, not the native LOCAL TEST protocol. Evidence is **static Ghidra analysis**, never captured traffic. No original Windows execution, login, credentials, official-server connection or probing was performed.

Confidence: **C-static** = directly observed imports, references or decompiled operations; **I** = interpretation; **U** = unknown. No item below is dynamically confirmed. Addresses are virtual addresses in the statically UPX-decompressed `istanbul.exe`, image base `0x00400000`; provenance and reproduction are in [REVERSE_ENGINEERING](../REVERSE_ENGINEERING.md).

## Confirmed transport and framing

| Component | C-static observation | Source function |
|---|---|---|
| Transport | IPv4 TCP: `socket(2,1,6)`, then `connect` with a 16-byte socket address | `0x0045a9f0` |
| Outbound envelope | Send four bytes from a native 32-bit length scalar, then exactly that many body bytes; no endian conversion | `0x00459f50`, exact-send helper `0x00459ba0` |
| Inbound envelope | Read four bytes into a native 32-bit size, allocate that size if nonzero, read the body and queue it | `0x0045c2e0`, exact-read helper `0x00459c10` |
| Byte order | Envelope length and command IDs are little-endian, from the x86 scalar loads/stores above | Same functions and `0x0045b380` |
| Outgoing command | Body begins with raw 32-bit command ID followed by caller-supplied argument bytes | `0x0045b380`; debug string `sendGameCommand` |
| Incoming command stream | A frame may contain multiple commands. Dispatcher reads a command ID, invokes an ID-specific parser/handler and advances by its returned consumed-byte count | `0x0045b4b0`, queued through `0x0045c170` and pumped by `0x0045c1e0` |

There is **no proven generic per-command length field**. A decoder may expose an opaque frame body or a first-command view; it cannot safely split the rest without each command's schema. A transport read is not a frame boundary.

Original edge behavior, not recommended implementation policy:

- Receiver queues zero-length/null-data records; the dispatcher explicitly ignores zero-length bodies.
- For nonzero lengths below four, the dispatcher has no apparent pre-read length check before reading a command. Do not reproduce this unsafe behavior.
- The outgoing constructor allocates/copies `argument_length + 4`, but the queued/send length is `(argument_length + 4) & 0xffff`. This mask is **not** a canonical maximum and is **not** a receive limit.
- No receive allocation upper bound is apparent in the bounded receive worker. Valid size ranges and server-enforced limits are **U**. Native decoders need a documented local defensive cap.
- Compression, encryption, integrity fields, sequence numbers and acknowledgments are **U** at the application-schema level. The recovered outer worker forwards bytes without an intervening codec; that does not rule out encoding inside handlers.

## One statically derived outgoing startup frame

After connection success, startup `0x00464dc0` calls the outgoing constructor at `0x004651bc` with command `0x70766564`, argument length four, and a scalar value eight.

| Wire offset | Bytes | Confirmed interpretation |
|---|---|---|
| 0 | `08 00 00 00` | LE32 body length = 8 |
| 4 | `64 65 76 70` | LE32 command ID = `0x70766564`; raw byte text `devp` |
| 8 | `08 00 00 00` | Four argument bytes representing scalar 8 |

Derived complete bytes: `08 00 00 00 64 65 76 70 08 00 00 00`.

This is **not a packet capture**. A protocol-version/development-version interpretation is **I**, not a confirmed field name or negotiation specification. Reply, acceptance, rejection and authentication dependency are **U**.

## Confirmed inbound command IDs

The complete bounded selector at `0x0045b4b0` contains the following 58 comparisons. Each ID is **C-static** for the inbound dispatch path; all argument schemas, valid lengths, version coverage and gameplay semantics remain **U**, except the narrow branch observations below. Wire text is literal little-endian byte order, not an inferred command name. In particular `0x70696e67` has wire text `gnip`; do not mistake a human-readable hex FourCC for on-wire order.

`L` anchors the ignored local Ghidra output `analysis/dispatch-game/decompile/0045b4b0.c`. Parser/handler targets are evidence pointers, not reconstructed implementations. The machine-readable bounded extraction is `analysis/original_commands.json`, reproducible using `tools/command_catalog.py`.

| ID | Wire text | L | Selected parser / handler target |
|---|---|---:|---|
| `0x696d6766` | `fgmi` | 75 | `0x004a3bd0` / `0x004a86d0` |
| `0x6367686e` | `nhgc` | 80 | `0x004a3f10` / `0x004a3c40` |
| `0x72666a73` | `sjfr` | 85 | Direct indexed error-message branch |
| `0x70696e67` | `gnip` | 118 | `0x00563090` / `0x00466e30` |
| `0x66676874` | `thgf` | 123 | `0x004a4610` / `0x004a4c90` |
| `0x6367636c` | `lcgc` | 128 | `0x004a4630` / `0x004a4620` |
| `0x636e746b` | `ktnc` | 133 | `0x004a4600` / `0x004a4c00` |
| `0x636e6e78` | `xnnc` | 138 | `0x004a4ec0` |
| `0x6b61636b` | `kcak` | 142 | Direct `DI_SCMD_KICK` log / close branch |
| `0x776f7471` | `qtow` | 147 | `0x004a4310` / `0x004a94d0` |
| `0x706c7369` | `islp` | 152 | `0x004a4010` / `0x004a58f0` |
| `0x68616f74` | `toah` | 157 | `0x004a4010` / `0x004aaac0` |
| `0x686b6b6b` | `kkkh` | 162 | `0x004a4020` / `0x004a4030` |
| `0x63686673` | `sfhc` | 167 | `0x004a40b0` / `0x004a40c0` |
| `0x6b6e696d` | `mink` | 172 | `0x004a3e90` / `0x004a4270` |
| `0x68796164` | `dayh` | 177 | `0x004a3ed0` / `0x004a42a0` |
| `0x6d6f6f69` | `ioom` | 182 | `0x004a4360` / `0x004a6510` |
| `0x72657463` | `cter` | 187 | `0x004a3c70` / `0x004a3c80` |
| `0x726f6672` | `rfor` | 192 | `0x004a3c60` / `0x004aa950` |
| `0x676a6872` | `rhjg` | 197 | `0x004a3f10` / `0x004a3d80` |
| `0x63726a74` | `tjrc` | 202 | `0x004a3da0` / `0x004aa460` |
| `0x65706368` | `hcpe` | 207 | `0x004a6720`, then `gameloop` transition |
| `0x65777269` | `irwe` | 212 | `0x004a4340` / `0x004a6010` |
| `0x6c666367` | `gcfl` | 217 | `0x004a3ed0` / `0x004a3e60` |
| `0x696e6a6c` | `ljni` | 222 | `0x004a3db0` / `0x004a80f0` |
| `0x69756671` | `qfui` | 227 | `0x004a3dc0` / `0x004a3df0` |
| `0x6164716f` | `oqda` | 232 | `0x004a3dc0` / `0x004a3e10` |
| `0x636d6971` | `qimc` | 237 | `0x004a3f10` / `0x004a3e40` |
| `0x61666c6e` | `nlfa` | 242 | `0x004a3ed0` / `0x004a3ee0` |
| `0x7467686b` | `khgt` | 247 | `0x004a3e90` / `0x004a3ea0` |
| `0x706c7970` | `pylp` | 252 | `0x004a3f10` / `0x004a3f20` |
| `0x63736c6c` | `llsc` | 257 | `0x004a3f10` / `0x004a3d50` |
| `0x61726470` | `pdra` | 262 | `0x004a3f10` / `0x004a3d20` |
| `0x67696e66` | `fnig` | 267 | `0x004a3cf0` / `0x004a57d0` |
| `0x67756966` | `fiug` | 272 | `0x004a3d10` / `0x004a8010` |
| `0x69626670` | `pfbi` | 277 | `0x004a3d10` / `0x004a7f40` |
| `0x74727a74` | `tzrt` | 282 | `0x004a3f50` / `0x004a8940` |
| `0x74726175` | `uart` | 287 | `0x004a3f70` / `0x004a9380` |
| `0x74726f6d` | `mort` | 292 | `0x004a3e90` / `0x004a3f80` |
| `0x7772656d` | `merw` | 297 | `0x004a3f10` / `0x004a4b00` |
| `0x79636b6d` | `mkcy` | 302 | `0x004a3ed0` / `0x004a9690` |
| `0x61756b73` | `skua` | 307 | `0x004a4010` / `0x004a9880` |
| `0x726d6b74` | `tkmr` | 312 | `0x004a3f10` / `0x004a44b0` |
| `0x6f76636e` | `ncvo` | 317 | `0x004a3f10` / `0x004a4480` |
| `0x7369726f` | `oris` | 322 | `0x004a3e90` / `0x004a4a80` |
| `0x63616e74` | `tnac` | 327 | `0x004a3f10` / `0x004a4a20` |
| `0x70696f6b` | `koip` | 332 | `0x004a4370` / `0x004a4380` |
| `0x6c746166` | `fatl` | 337 | `0x004a43f0` / `0x004a4400` |
| `0x67616469` | `idag` | 342 | `0x004a4370` / `0x004a4380` |
| `0x61657472` | `rtea` | 347 | `0x004a4010` / `0x004a59d0` |
| `0x61747274` | `trta` | 352 | `0x004a4170` / `0x004a4710` |
| `0x616c7272` | `rrla` | 357 | `0x004a4180` / `0x004a4190` |
| `0x61627370` | `psba` | 362 | `0x004a3f70` / `0x004a9b30` |
| `0x74737573` | `sust` | 367 | `0x004a3f10` / `0x004a5640` |
| `0x75737168` | `hqsu` | 372 | `0x004a4860` |
| `0x77646c76` | `vldw` | 376 | `0x004a4900` |
| `0x79656f77` | `woey` | 380 | `0x004a45f0` / `0x004a6a80` |
| `0x74626473` | `sdbt` | 385 | `0x004a44e0` / `0x004a44f0` |

Narrow branch evidence:

- `0x6b61636b`: logs `Closing because of DI_SCMD_KICK.` and invokes the close/state-transition function. Kick/forced-close purpose is **C-static**; reason payload and policies are **U**.
- `0x72666a73`: reads a 32-bit value immediately after the ID to index an error-text array, and advances cursor by eight from command start. This scalar's meaning/range is **U**; bounds must not be assumed from the original array access.
- `0x65706368`: invokes `0x004a6720` and passes `gameloop` to a transition routine. Association with entering a game loop is **C-static**; world-entry payload and authoritative server rules are **U**.
- `0x70696e67`: value resembles `ping` in big-endian FourCC notation, but heartbeat purpose/period/reply is **I/U**, not established from spelling alone.

## Unknown original schemas and flows

Login/authentication tokens and interchange, character listing/creation/selection fields, movement, snapshots/entities, chat encoding, inventory, NPC interaction, combat, quests, zone handoff, retries, timeouts and heartbeat semantics are **U**. The 58 selector values do not establish those mappings. There is no authorized original capture, and the bounded analysis did not reconstruct each handler's structures, string lengths, signedness, compression or validation.

The native big-endian LOCAL TEST handshake/input/snapshot messages in [PROTOCOL_SPECIFICATION](PROTOCOL_SPECIFICATION.md) are independently designed. Their IDs, limits, movement and timing are **not original evidence**. An independently implemented original-envelope inspector can exercise LE32 framing locally; that is a synthetic implementation check, not official-service compatibility or an original-client session.
