# İKV Initial Execution Implementation Plan

**Goal:** Execute Goal §20 against the supplied installer, with reproducible static evidence and native local client/server executables. The full Goal remains the project acceptance contract, not a claim that these first executables are a playable reconstruction.

**Architecture:** Preserve the original installer and keep extracted proprietary inputs and Ghidra projects local and excluded from distribution. Recover original interfaces only from evidence. Where original packet semantics are not established, use an explicitly independent, versioned local protocol rather than pretend to implement the production protocol. Client and server share the custom wire implementation; original binary findings remain separate from clean-room implementation.

**Tech Stack:** C++20, CMake, POSIX TCP, Ghidra, native installer extraction; SDL3/OpenGL and Qt6 subject to installed dependencies.

## Constraints and defaults

- No official-server connections, endpoint probes, credential collection, authentication bypasses, or Windows binary execution.
- Loopback-only local service by default. The initial transport exchanges no credentials and must not be represented as authentication.
- Initial simulation is an independently defined bounded test world; no inferred canonical movement, combat, or economy rules.
- No proprietary assets in source control/releases. Source inputs are never modified.
- Evidence includes input hashes, tool versions, reproducible commands, original addresses, confidence, and explicit unknowns.
- First deliverables prioritize parsing correctness and two-client synchronization. Performance targets beyond this require profiling; no speculative distributed architecture.

## Execution slices

1. Discover installer format and available toolchain in parallel. Select format-specific extraction and Ghidra setup from observations.
2. Extract natively; inventory executables, libraries, archives and asset formats. Record hashes and PE metadata in machine-readable evidence.
3. Analyze networking and rendering/asset paths in Ghidra. Record function addresses, callers, parameters where recoverable, and confidence. An imported API alone does not prove packet framing.
4. Specify an evidence-backed original packet catalog. If no original message is established, state that explicitly and implement a documented custom protocol without original-compatibility claims.
5. Implement native C++ transport/server and client in independent file slices under one CMake project. Shared interface contracts are fixed before delegation; the integration owner owns root build files and smoke execution.
6. Exercise actual binaries locally: fragmented and invalid packets, two clients, authoritative movement visibility, disconnect/reconnect and clean shutdown. Add focused deterministic regression checks for parser/state transitions.
7. Inspect the real graphical surface if a graphical path is implemented. Do not call an abstract visualization original game rendering.
8. Update requested documentation with measured outcomes and specific missing evidence/prerequisites for later phases. Keep future subsystems unimplemented rather than provide fake APIs, no-op renderers, or invented original behavior.

## Verification commands

Final commands are recorded after the discovered paths and targets exist. Run CMake/Ninja once after each independent implementation batch, then CTest and a real multi-process local smoke scenario. Run Ghidra headless on the extracted main executable and retain reproducible analysis outputs. Report only observed results.
