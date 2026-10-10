# Endpoint discovery: original static evidence

No listed endpoint was queried, resolved, scanned or contacted. These are historic strings in the supplied binaries, **not working-service claims** or permission to test official infrastructure. The native applications must keep user-configured private endpoints and must not copy these addresses into default connection logic.

Confidence: **C-static** = literal bytes/references or decompiled data flow; **I** = apparent purpose; **U** = unestablished. All addresses are preferred-image virtual addresses, not ASLR runtime observations. `launcher.exe` and `_launcher.dll` are byte-identical (hash in [REVERSE_ENGINEERING](../REVERSE_ENGINEERING.md)); analyze one, not two independent service implementations.

## Launcher strings

Source: original `launcher.exe`, image base `0x00400000`; local evidence `analysis/launcher/strings.tsv`, `imports.tsv`, `functions.tsv` and `decompile/0048dbb0.c`, `0048dbe0.c`.

| Literal | File offset | VA | Exact string xref / function | Confidence and apparent purpose |
|---|---|---|---|---|
| `http://ikvptnew.istanbuloyun.com/Patcher/1000_Update.txt` | `0x9c160` | `0x0049db60` | `0x0048dbb3` / `0x0048dbb0` | C-static: constructs a persistent C++ string; I: update manifest from path/name. Download behavior and manifest schema not recovered here. |
| `http://www.istanbuloyun.com/serverlist.aspx` | `0x9c19c` | `0x0049db9c` | `0x0048dbe3` / `0x0048dbe0` | C-static: constructs a persistent C++ string; I: server-list discovery. Content, role-specific hosts and ports U. |
| `http://www.istanbuloyun.com/LauncherPromo1.aspx` | `0x9bd00` | `0x0049d700` | No direct string xref exported | C-static literal, I: launcher promotional page; runtime use U. |
| `http://www.istanbuloyun.com/LauncherPromo2.aspx` | `0x9bd60` | `0x0049d760` | No direct string xref exported | Same caveat. |
| `http://www.istanbuloyun.com/LauncherPromo3.aspx` | `0x9bdc0` | `0x0049d7c0` | No direct string xref exported | Same caveat. |

Names such as `istanbul_patcher` (VA `0x0049d6d8`, file `0x9bcd8`, xref `0x0048741c` in `0x00486e10`), temporary-file operations and statically linked libcurl diagnostic strings strengthen the patcher interpretation. They do not establish a verified HTTP exchange. No standard WinHTTP/WinINet named imports occur in the bounded PE inventory; embedded library networking remains possible and is evidenced by Winsock imports.

## Main game account URLs and configurable TCP target

Source: statically UPX-decompressed `analysis/unpacked/istanbul.exe`, image base `0x00400000`; preferred VAs remain the original image's code/data addresses. File offsets below refer to the **decompressed analysis copy**, not packed input offsets. Evidence: `analysis/focused-game/strings.tsv`, `decompile/00464dc0.c`, `decompile/0045a9f0.c`, and `analysis/trace-game/decompile/00465710.c`.

| Literal (UTF-16LE) | Analysis-copy file offset | VA | Xref / function | Confidence and apparent purpose |
|---|---|---|---|---|
| `http://www.istanbuloyun.com/mynetaccounttransfer.aspx` | `0x194b68` | `0x00594b68` | `0x00465066` / `0x00464dc0` | C-static: selected when startup mode equals 0 and forwarded to a browser-like COM virtual call; I: account transfer/login page. |
| `http://www.istanbuloyun.com/ttlogingame.aspx` | `0x194bd8` | `0x00594bd8` | `0x0046505f` / `0x00464dc0` | C-static: selected for mode 1; I: alternate login page. |
| `http://www.istanbuloyun.com/logingame.aspx` | `0x194c38` | `0x00594c38` | `0x00465045` / `0x00464dc0` | C-static: selected for mode 2; I: login page. |

`0x00464dc0` obtains the Unicode command line with `GetCommandLineW` / `CommandLineToArgvW`. When argument count is four, it copies argument 1 into host storage `0x006140a0`, converts argument 2 to port storage `0x005d6d90`, and argument 3 into mode `0x0077b984`. The host/port log references are `server: %ws : %d` at VA `0x00594cac` (file `0x194cac`, xref `0x00464f73`). This is **C-static endpoint selection**, not evidence of valid CLI values or a command to launch the original.

Connection `0x0045a9f0` converts the host, calls `gethostbyname`, takes a resolved IPv4 address, applies `htons` to the configured port and connects a TCP socket. Its status literal `Connecting to server : %ws : %d` is at VA `0x00593ba0` (file `0x193ba0`, xref `0x0045a9ff`). No official TCP host or port is asserted from these status strings. Argument validation, inherited defaults and launcher-to-client command-line construction are not fully recovered.

## Non-service strings and exclusion rules

The PE inventory's endpoint scanner is deliberately a **candidate** scanner: it can include URL suffix bytes from DER records and can miss UTF-16 strings. Ghidra's defined string/xref export is the stronger source for mapped runtime references.

- `example.com`, `ftp@example.com`, libcurl cookie-spec URLs and FTP format strings are embedded library/test/diagnostic material, not confirmed game-service endpoints.
- GlobalSign OCSP/CRL/repository URLs in executable certificate overlays are signing infrastructure, not game authentication/world addresses. For example packed `istanbul.exe` candidate at file `0x105b5a` concerns GlobalSign OCSP; this does not mean the game calls it. The certificate overlay may not map into Ghidra's image, so no image VA/runtime xref is asserted.
- Microsoft/VeriSign/Thawte URLs in `d3dx9_31.dll` signing/version material are not game endpoints.
- No live response, redirect, DNS result, TLS validation or availability evidence exists.

## Still unknown

Actual authentication interchange, session/token formats, server-list schema, login gateway versus world/zone-server separation, port values, redirects, update integrity rules, CDN topology, TLS use at application boundaries and retry/handoff behavior remain **U**. The distinct URL paths and configured TCP target must not be expanded into an invented multi-service architecture. Synthetic localhost endpoints in the native harness are documented separately in [PROTOCOL_SPECIFICATION](PROTOCOL_SPECIFICATION.md).
