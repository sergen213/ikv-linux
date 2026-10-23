İstanbul Kıyamet Vakti — native Windows client, launcher and server (IKV community port)
=========================================================================================

Start "İstanbul Kıyamet Vakti" from the Start Menu or desktop (bin\ikv-launcher.exe).
The portable .zip runs the same way from any folder: bin\ikv-launcher.exe.

Requirements
- 64-bit Windows 10 or 11.
- A current graphics driver with Vulkan 1.2 and OpenGL 3.3. vulkan-1.dll is installed by every current
  AMD/NVIDIA/Intel driver and is needed even for the OpenGL renderer; it is not bundled.
- Your own original game installer (istanbulkur6_0.exe) or installed game folder: the launcher imports the
  game data from it. No original game data is included in this package.
- Nothing else: the MinGW C++ runtime, Qt 6, OpenSSL, SDL3, FreeType, PostgreSQL 16 and the Visual C++ runtime it
  needs are bundled.

Notes
- The executables are not code-signed; Windows SmartScreen may warn ("More info" -> "Run anyway").
- Hosting: the launcher's "Sunucu Kur" sets up a private server (PostgreSQL 16 on 127.0.0.1 only, ikv-auth,
  ikv-server) in your user profile (%LOCALAPPDATA%\IKVCommunity\IKVLauncher\server). Windows Firewall asks on first
  start whether ikv-auth/ikv-server may accept connections: needed only for LAN mode (allow "Private networks").
  Other PCs join through the launcher's server settings (HTTPS address plus the host's exported CA file).
- No self-update on Windows yet: install a newer Setup.exe over this one (settings and game data are kept in
  your user profile, not in the install folder).
- Third-party licences: share\licenses\.
