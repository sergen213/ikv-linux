# Authenticated custom v2 message state machine

These transitions describe the independent custom wire in [PROTOCOL_SPECIFICATION.md](PROTOCOL_SPECIFICATION.md), not recovered original İKV authentication or wire compatibility. The original static offline decoder is separate. Anonymous v1 joining has been removed.

## Server connection states

```text
TCP accepted -> TLS handshake -> AwaitHello -> Joined -> Closing -> Closed
                   |                 |          |          |
                   +-----------------+----------+----------+-> fatal abort
```

The TLS handshake and application hello together have a five-second deadline. TLS1.2 or newer is required. Exactly one hello contains a 64-character lower-case hexadecimal session and a positive character ID. The server authenticates the session and atomically acquires the owned character's active lease before adding any player or issuing welcome. Duplicate active character entry, expired/revoked/banned sessions and foreign/deleted characters never enter the world. Tokens are not logged or stored in leases.

Successful `enter_character` immediately records the authenticated identity, character, session and `held_lease` ownership on the peer, before `world.add`. A later admission failure therefore cannot orphan an acquired lease. Cleanup releases held leases even when the peer never joined (player ID zero); lease ownership is cleared only after successful `leave_character`. A release failure fail-stops the server rather than continuing with uncertain admission ownership.

| State | Allowed client message | Effect |
| --- | --- | --- |
| AwaitHello | hello | Authenticate, acquire lease, restore persisted character, assign connection-local player ID, send welcome and state |
| Joined | input | Require strictly increasing sequence, replace bounded cardinal intent |
| Joined | destination | Require strictly increasing sequence, submit bounded ground destination for authoritative movement |
| Joined | action | Dispatch only explicit server-authoritative gameplay handlers; never accept outcome/state uploads |
| Joined | goodbye | Enter Closing, drain queued frames, perform TLS close_notify, then save/detach and release lease |
| Joined | clean EOF | Finalize decoder; if framing is complete, enter the same graceful Closing path |
| Closing | gameplay input/actions | Suppress gameplay processing and new snapshot/state output; retain joined capacity until actual detach |
| Any | malformed/disallowed direction/type, truncated EOF, fatal TLS/transport error | Abort only the affected transport immediately; persistence/release failures fail-stop the server |

A hello after joining cannot rename/reset identity or input sequence. Welcome, snapshot, state and catalog are server-only. An unjoined goodbye is not an alternate entry path. Application actions are limited to forty per second per connection; illegal state/range/resources/ownership receives a typed error event rather than client-selected outcomes.

## Authority and output

Simulation runs fixed 50ms ticks with at most four catch-up steps per event-loop cycle; excess accumulated delay is dropped rather than permitting uncontrolled catch-up. Spatially filtered snapshots preserve ascending unique player IDs and contain self. Snapshots carry tick and integer coordinates. State is emitted on entry, after actions and every fourth tick; it contains authoritative character, nearby NPCs and bounded events. State and snapshots are serialized on the same TLS stream. Clients interpolate remote motion and accept server correction; rendering never determines damage, loot, rewards or economy.

Every second the server reauthenticates active sessions, refreshes externally administered chat moderation, publishes world status and refreshes its instance leases. Authentication/lease heartbeat failure stops serving the world rather than silently continuing unauthenticated authority. Every five seconds and on orderly disconnect/shutdown the world persists its state. Transactional economy is saved before acknowledgement. Atomic trades save both owned characters through one Store batch; persistence failure leaves cached states unchanged.

Disconnect cleanup saves/removes the world player before releasing its lease. A `world.remove`/save failure retains the peer and cached world state for the final save attempt; it must not discard unsaved state or release the lease as though persistence succeeded. Final release includes every held lease, including unjoined peers with player ID zero, and clears each ownership flag only after successful leave. Any release failure fail-stops. Failed final persistence retains leases for crash recovery.

The initial status write acquires Store's exclusive world-process advisory lock before READY; a second live world process is rejected rather than creating an invisible split community. Optional `--asset-root ROOT --zone N --collision-zone WORLDZONE` uses the owned original zone's recovered accepted-cell/height grid for the named configured zone. All three options are required together, `N` is0–17, and the named world zone must exist. Protocol integer coordinates map to scene X/Z by dividing by1000. Unsupported original cells reject movement/spawns/transitions; configured collision restrictions remain active. An accepted cell establishes supported ground, not a recovered general-purpose wall or dynamic collision mesh. Other configured zones retain explicitly NONCANONICAL independent collisions.

## Client states

```text
Disconnected -> Connecting -> VerifiedTLS -> AwaitWelcome -> Joined -> Closing
```

Explicit endpoint configuration and explicit CA trust are required. Certificate chain and hostname/IP SAN verification precede hello. The session is provided through standard input, not arguments or logs. Welcome establishes self and the initial tick; snapshots must advance the tick, contain self and satisfy codec bounds. State must match the joined identity. Client-direction packets arriving from the server, duplicate welcome and malformed data fail the connection. EOF finalizes the streaming decoder; partial framing is never accepted as a clean disconnect. Headless output preserves snapshot `{self,tick,players}` and emits typed gameplay state; scripted actions traverse the same authenticated transport.

Reconnection performs a new verified TLS connection, session authentication and character lease acquisition. Player IDs and sequence are connection-local; character state, inventory, progression and position are restored from PostgreSQL. Reconnection while the previous lease remains active is rejected, not duplicated. After a process crash the thirty-second lease expiry permits recovery.

## Bounds and control

Connections are capped at64; joined players at32. A Closing peer continues counting against the joined limit until actual detach. Framing caps body size at65535 bytes and decoder storage at body plus four-byte prefix. Server pending application output is capped at262144 bytes per peer; slow-consumer overflow aborts that peer immediately. Broken/malformed peers cannot reset surviving identities or server tick.

### Nonblocking TLS operation ordering

Each peer tracks an explicit TLS operation (`handshake`, `idle`, `write`, `read-WANT_WRITE` or `shutdown`) and the poll direction needed to resume it. Clear the OpenSSL error queue with `ERR_clear_error` before every TLS operation and obtain `SSL_get_error` immediately after its return, before another TLS call or error-queue-affecting work.

| Operation | Poll interest and retry ordering |
| --- | --- |
| handshake | Retry the handshake on the last WANT_READ/WANT_WRITE direction; application reads/writes wait for completion |
| idle | Read on readable readiness; request writable readiness for queued output only when no TLS retry owns the connection |
| write | WANT_READ polls readable, WANT_WRITE polls writable; retry the pending `SSL_write` before starting any other TLS operation |
| read-WANT_WRITE | Poll writable and retry `SSL_read` using the stable per-peer read buffer before starting queued writes |
| shutdown | After queued frames drain, retry `SSL_shutdown` on its required read/write direction; a first zero return waits for the peer close_notify |

A pending `SSL_write` retains exactly the same bytes and length until that retry completes; appending queued frames cannot extend or replace the in-flight write. Advance the queue only after successful write completion. A read that needs writable readiness owns the retry ahead of output, so writing cannot invalidate its TLS operation or buffer.

### Graceful and fatal close

`goodbye`, `admin_kick`, clean EOF and `server_stop` enter graceful Closing: stop gameplay processing and new snapshot/state output, preserve and drain all already queued frames, then exchange TLS close_notify. The close has a two-second deadline; expiration aborts immediately. Protocol errors, fatal TLS/transport errors and output overflow abort immediately without attempting an application drain or graceful TLS shutdown. EOF is clean only after decoder finalization confirms no truncated frame.

SIGINT/SIGTERM and a finite `--ticks N` stop normally after final persistence. Private admin commands are consumed from Store: restart exits75 for the supervisor to restart, shutdown exits0. Server stop marks peers Closing and continues polling their pending TLS operations and queued output without simulation ticks or heartbeat output. This final drain has a hard two-second cap; remaining transports are aborted at the cap before cleanup completes. Final persistence precedes lease release; persistence failure exits1 and retains leases for crash recovery rather than falsely reporting saved data, and lease-release failure also fail-stops. No original protocol compatibility, original combat formulas, original NPC definitions or official-account access is implied by successful custom-v2 gameplay.
