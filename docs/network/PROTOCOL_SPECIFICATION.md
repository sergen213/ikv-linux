# Protocol specification

## Original İKV wire: statically evidenced subset

This is a **partial static reconstruction**, not a complete protocol or a live interoperability result. The separately implemented original-wire investigation component is never used by the independent movement client/server below. No original Windows execution, live capture, authentication attempt or official endpoint probing is part of this delivery.

### Provenance and confidence

The evidence input is `original/app/istanbul.exe`, SHA256 `bff4cffe753a04e237e3ed32c3798eda10a602d73424a2b228024762da44aba6`; its statically UPX-decompressed analysis copy has SHA256 `44ffa8389a9f6e4c35841103ad4f40a90513e4deb518ab5605b0a1ebfae0be2d`. The following findings are supported by static x86 little-endian decompilation in Ghidra 12.1.2. They have not been validated against original-service traffic.

See [PACKET_CATALOG.md](PACKET_CATALOG.md) for the original-client command-ID inventory and confidence labels, and [REVERSE_ENGINEERING.md](../REVERSE_ENGINEERING.md) for static-analysis provenance and reproduction. Raw ID presence does not establish gameplay semantics.

| Finding | Static source/function | What is established |
| --- | --- | --- |
| IPv4 TCP connection | `0x0045a9f0` | `socket(2,1,6)` and a 16-byte socket address passed to connect |
| Outbound frame | `0x00459f50`, send-exact `0x00459ba0` | Four-byte native `uint32` length, then exactly that many body bytes; x86 representation is LE32 |
| Inbound frame | `0x0045c2e0`, recv-exact `0x00459c10` | Read four-byte native length, then that many bytes; no receive upper bound apparent in this worker |
| Outbound command construction | `0x0045b380` | Native 32-bit command ID at body offset 0, followed by supplied opaque arguments |
| Inbound command dispatch | `0x0045b4b0` | Loop over potentially multiple commands; advance by 4 plus command-handler-reported bytes |
| One startup command construction | `0x00464dc0` | Raw command value `0x70766564` plus four argument bytes representing scalar 8 |

Local decompiler inputs are under `analysis/focused-game/decompile/`, with constructor `analysis/trace-game/decompile/0045b380.c` and dispatcher `analysis/dispatch-game/decompile/0045b4b0.c`. These proprietary analysis outputs remain ignored; reproducible summaries must identify functions and input hashes without redistributing the originals.

### Original envelope and command limits

The evidenced original envelope is **LE32 body length followed by opaque body bytes**, unlike the custom BE32/version/type envelope below. A zero-length original frame is queued by the receive worker and ignored by the dispatcher. The original dispatcher appears to read a command ID without a prior minimum-length check for bodies of 1–3 bytes; the new offline tool does not reproduce that unsafe behavior.

The outbound constructor allocates/copies `argument_length + 4` bytes but queues length `(argument_length + 4) & 0xffff`. This mask is **not** proof of a canonical 65535-byte original protocol maximum or receive bound. The independent original-wire decoder uses a documented local defensive cap of 65535 body bytes; its command encoder rejects arguments over 65531 bytes rather than silently reproducing truncation.

An inbound frame may contain multiple variable-sized commands. There is no established generic per-command length field. The offline `--client-commands` view exposes only the first raw command ID and leaves the remainder opaque; it does not split batches or claim the remainder belongs to one command. Framing acceptance and command parsing are separate: the opaque decoder accepts zero-length and 1–3-byte envelopes, but a command view needs at least four body bytes. In `--client-commands` mode, the CLI accepts and reports a zero-body envelope without a command ID, matching the dispatcher skip, while rejecting 1–3-byte command bodies defensively.

The statically derived startup fixture is:

```text
08 00 00 00  64 65 76 70  08 00 00 00
LE32 len=8  command32    opaque argument bytes
```

`64 65 76 70` is raw byte text `devp`, numerically `0x70766564`. Its descriptive meaning is unknown. Treating argument scalar 8 as a version negotiation field is only a hypothesis, not a recovered schema. These bytes are derived from static code, **not captured traffic**.

Authentication, cryptographic transforms, complete command schemas, movement semantics, production compatibility and the full original connection/world-entry state machines remain unresolved. The installer and extracted files are user-owned proprietary inputs, not redistributed runtime dependencies. Do not reinterpret any custom message below as an original opcode finding.

## Independent authenticated custom wire: version 2

This is the wire contract for the independently implemented native client and authoritative world server, not an original İKV opcode reconstruction. Version 1 anonymous/name-based entry is removed: there is no compatibility alias or version negotiation.

### Transport and framing

World connections use TLS with certificate and host/IP SAN verification against the explicitly configured CA. The client sends a session issued by the independent authentication service and a selected character ID; the world validates session lifetime, ownership, bans and concurrent character entry before welcome. There are no official endpoints or automatic service discovery. TLS and account/character authorization belong to the transport/world layers, not the codec.

Each frame is:

| Offset | Size | Field |
| --- | ---: | --- |
| 0 | 4 | Body length, unsigned big-endian, excluding this header |
| 4 | 1 | Version, exactly `2` |
| 5 | 1 | Message type |
| 6 | variable | Type-specific payload |

Body lengths are 2–65535 inclusive. This is a local defensive maximum, not a recovered original protocol limit. Exact fixed-layout payload lengths are required; trailing bytes are invalid. Unknown versions (including `1`), unknown types and invalid fields are rejected with `std::invalid_argument`. Multibyte integers are big-endian; signed coordinates and intent fields use two's-complement `int32` and `int8`.

TCP/TLS reads may split frames at any byte or coalesce any number of frames. `Decoder` holds at most one incomplete frame in its fixed 65539-byte buffer and consumes coalesced input incrementally. Any parse error permanently poisons it: subsequent feeds, including empty feeds, and finalization reject. Close the connection; never skip bytes to find another plausible frame. A malformed suffix also rejects that feed rather than returning a partial message batch.

`Decoder::finish()` accepts an empty stream or any complete-frame boundary. A partial header or body at EOF throws a sanitized truncation error and poisons the decoder. Connection-state requirements such as receiving welcome before EOF remain separate from framing.

### Message catalog

Body sizes include the two-byte version/type prefix but exclude the four-byte frame header.

| Type | Value | Direction | Body size | Payload |
| --- | ---: | --- | ---: | --- |
| `hello` | 1 | client → server | 74 | `session:64 raw ASCII bytes`, `character_id:u64` |
| `welcome` | 2 | server → client | 10 | `id:u32`, `tick:u32` |
| `input` | 3 | client → server | 8 | `sequence:u32`, `dx:i8`, `dy:i8` |
| `snapshot` | 4 | server → client | `8 + 12 * count` | `tick:u32`, `count:u16`, entries `(id:u32,x:i32,y:i32)` |
| `goodbye` | 5 | client → server | 2 | none |
| `state` | 6 | server → client | `2 + json_bytes` | raw UTF-8 JSON object |
| `destination` | 7 | client → server | 14 | `sequence:u32`, `x:i32`, `y:i32` |
| `action` | 8 | client → server | `2 + json_bytes` | raw UTF-8 JSON object |

`Message` exposes `session:string`, `character_id:uint64`, existing `id`, `tick`, `sequence`, `dx`, `dy`, `players`, plus `json:string` and destination `x`, `y`. `Message.name` no longer exists. Unused fields are not serialized; only the selected type's payload is encoded.

#### Hello and welcome

Hello's session is exactly 64 lowercase hexadecimal ASCII characters (`0`–`9`, `a`–`f`), with no length prefix, terminator, decoding into binary, uppercase alias or whitespace. Character ID is in `[1, INT64_MAX]`; the unsigned wire representation does not permit zero or values outside the positive database ID range.

Only the initial message of an unjoined connection may be hello. Syntactically valid credentials are not proof of authorization. The world must authenticate the session and selected character before assigning the nonzero `uint32` player ID in welcome. Tick is the current world tick. Durable character identity and connection-local player identity are distinct; the codec does not implement authentication or persistence.

#### Input and destination

Both messages require a nonzero sequence. The world enforces increasing sequences within the joined session and rejects replay; exhaustion requires a new connection rather than wraparound. Input axes are each `-1`, `0`, or `1`, and cannot both be nonzero; `(0,0)` means stop. Existing cardinal intent is an independent provisional behavior, not recovered original-game controls or movement formulas.

Destination coordinates are each in `[-1536000,+1536000]` inclusive (`ikv::coordinate_limit`, `include/ikv/protocol.hpp`), using the same independent-world coordinate bounds as snapshots. A destination is a request to move, not a client-authoritative position write. The world applies collision, movement rules and zone constraints. Ground-click movement and WASD camera control are distinct from the codec.

An independent destination byte fixture is:

```text
00 00 00 0e  02 07  01 02 03 04  ff ff d8 f0  00 00 27 10
BE32 len=14  v2/type sequence     x=-10000     y=10000
```

#### Snapshot

Count is 0–32. Entries have nonzero, unique, strictly ascending player IDs, and both coordinates in `[-1536000,+1536000]`. The largest snapshot body is 392 bytes. Empty snapshots are valid framing; joined-state rules, self-presence, tick progression and visibility/AOI are the responsibility of world/client handlers.

#### State and action JSON

The payload is 1–65533 bytes of raw UTF-8 JSON containing exactly one top-level object, with optional JSON whitespace. `{}` is valid. Arrays/scalars at the root, comments, trailing extra documents, malformed UTF-8, unescaped control characters, invalid escapes/surrogates and malformed JSON are rejected. Escaped control characters permitted by JSON are not rejected merely for being escaped; application fields still need their own validation.

Before calling `nlohmann::json::accept`, the codec scans quotes and escapes to bound combined object/array nesting to 32 containers, counting the root object as depth 1. Brackets inside strings do not count. Excess nesting is rejected before the parser runs. No JSON DOM or gameplay schema is constructed by the codec; accepted bytes are preserved exactly, including whitespace and key order, rather than normalized.

State carries world-authoritative state/events. Action carries an intent object such as `{"action":...}`; its action name, field types, sizes, target ownership, cooldowns, economy and allowed operations must be checked by the world handler. Valid JSON is never authorization to write arbitrary character state, inventory, currency or other gameplay outcomes. The state fields, action schemas, events and limits are listed in [CUSTOM_PROTOCOL.md](CUSTOM_PROTOCOL.md).

Independent empty-object fixtures:

```text
00 00 00 04  02 06  7b 7d    state {}
00 00 00 04  02 08  7b 7d    action {}
```

#### Goodbye

Goodbye has no payload or acknowledgement. EOF also ends the connection; clean framing EOF does not override application-state requirements or persistence/lease cleanup. The complete frame is `00 00 00 02 02 05`.

### Resource and trust limits

- The wire's 32-player snapshot cap and 65535-byte body cap are independent defensive bounds.
- The incremental decoder bounds retained framing bytes; consumers must additionally bound pending output, message batches and per-peer work.
- Illegal direction/state, replay and authorization failures are handled by world/client state machines, not by accepting an otherwise valid wire message.
- Errors and logs must not disclose sessions, passwords or raw credential-bearing payloads.
- `tests/protocol_test.cpp` defines independent byte fixtures and codec, UTF-8/JSON/depth, maximum-size, fragmentation, coalescing, poison and EOF checks. Its fixed-seed (`0x494b5632`) property loop generates 1024 valid messages across all eight types, verifies byte-exact round trips and randomized fragmented/coalesced delivery, and rejects generated invalid versions, types and per-type payloads with poison checks. This is bounded deterministic fuzz/load coverage, not an exhaustive parser proof or a claim that those checks have been executed.

See [MESSAGE_STATE_MACHINE.md](MESSAGE_STATE_MACHINE.md) and [NETWORK_TESTING.md](NETWORK_TESTING.md) for application-state and integration details. Original-wire findings above remain offline and separate from this custom authenticated protocol.
