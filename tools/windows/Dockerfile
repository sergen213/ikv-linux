# IKV Windows (x86_64) cross toolchain: MinGW-w64 GCC 14 (posix threads, SEH, msvcrt) on a pinned Debian 13 snapshot.
# Build context: tools/windows (this file + mingw-w64.cmake). The source tree enters only as a read-only bind mount
# at run time (tools/package_windows.py). Every downloaded archive is SHA-256 pinned.
FROM debian:trixie-slim@sha256:a29215f6a35e51e22adffa17f89e9d2ef06214e64a2bad10d765c46aea49f11f
SHELL ["/bin/bash", "-o", "pipefail", "-c"]
ENV DEBIAN_FRONTEND=noninteractive
RUN <<'PROVISION'
set -eu
rm -f /etc/apt/sources.list /etc/apt/sources.list.d/*
cat > /etc/apt/sources.list.d/debian.sources <<'SOURCES'
Types: deb
URIs: http://snapshot.debian.org/archive/debian/20261007T000000Z/
Suites: trixie trixie-updates
Components: main
Check-Valid-Until: no
Signed-By: /usr/share/keyrings/debian-archive-keyring.gpg

Types: deb
URIs: http://snapshot.debian.org/archive/debian-security/20261007T000000Z/
Suites: trixie-security
Components: main
Check-Valid-Until: no
Signed-By: /usr/share/keyrings/debian-archive-keyring.gpg
SOURCES
apt-get -o Acquire::Retries=5 update --error-on=any
printf '%s\n' \
 '0584fba32e13e0ab8285fb16c27adea1ec03a73669c18702821094fd6ca86675  /var/lib/apt/lists/snapshot.debian.org_archive_debian_20261007T000000Z_dists_trixie_InRelease' \
 '09efe67a4892d8b94393f21a628a51f94ae9d92ad364de8645c965a1a2c1533c  /var/lib/apt/lists/snapshot.debian.org_archive_debian_20261007T000000Z_dists_trixie-updates_InRelease' \
 'a3ed6f5fa240f62d421e4afe1987ae35a5b1aceee52d44149f7d58bb3282af4a  /var/lib/apt/lists/snapshot.debian.org_archive_debian-security_20261007T000000Z_dists_trixie-security_InRelease' \
 | sha256sum --check
apt-get -o Acquire::Retries=5 install -y --no-install-recommends \
 g++-mingw-w64-x86-64-posix gcc-mingw-w64-x86-64-posix binutils-mingw-w64-x86-64 mingw-w64-x86-64-dev \
 cmake ninja-build make pkgconf perl nasm python3 glslc nsis 7zip xz-utils
update-alternatives --set x86_64-w64-mingw32-gcc /usr/bin/x86_64-w64-mingw32-gcc-posix
update-alternatives --set x86_64-w64-mingw32-g++ /usr/bin/x86_64-w64-mingw32-g++-posix
mkdir -p /opt/ikv-baseline /opt/w64/share/licenses
dpkg-query -W -f='${binary:Package}\t${Version}\t${source:Package}\t${source:Version}\n' > /opt/ikv-baseline/installed-packages.tsv
rm -rf /var/lib/apt/lists/*
PROVISION
COPY mingw-w64.cmake /opt/w64/mingw-w64.cmake
ADD --checksum=sha256:bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16 https://github.com/madler/zlib/releases/download/v1.3.2/zlib-1.3.2.tar.gz /opt/ikv-baseline/sources/
ADD --checksum=sha256:36bc4f1cc413335368ee656c42afca65c5a3987e8768cc28cf11ba775e785a5f https://download.savannah.gnu.org/releases/freetype/freetype-2.14.3.tar.xz /opt/ikv-baseline/sources/
ADD --checksum=sha256:6f30092cef9fb839779646608f4ee14ae3cbac989c47fa05e841b0841f09878e https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/3.2.0/libjpeg-turbo-3.2.0.tar.gz /opt/ikv-baseline/sources/
ADD --checksum=sha256:42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz /opt/ikv-baseline/sources/nlohmann-json-3.12.0.tar.xz
ADD --checksum=sha256:cbaf687d3c59b9666fe080e7f8396b6b4f4d344a768ee6b337c59852f4526b68 https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/v1.4.363.tar.gz /opt/ikv-baseline/sources/Vulkan-Headers-1.4.363.tar.gz
ADD --checksum=sha256:cc520304a4d8de9a6a1699553d6090aef0c25f87ef6cbc16b7c84ebd99093264 https://github.com/KhronosGroup/Vulkan-Loader/archive/refs/tags/v1.4.363.tar.gz /opt/ikv-baseline/sources/Vulkan-Loader-1.4.363.tar.gz
RUN <<'SMALL'
set -eu
T=/opt/w64/mingw-w64.cmake L=/opt/w64/share/licenses S=/opt/ikv-baseline/sources
mkdir /tmp/d && cd /tmp/d
tar -xzf $S/zlib-1.3.2.tar.gz
make -C zlib-1.3.2 -f win32/Makefile.gcc -j"$(nproc)" PREFIX=x86_64-w64-mingw32- libz.a
make -C zlib-1.3.2 -f win32/Makefile.gcc install PREFIX=x86_64-w64-mingw32- \
 BINARY_PATH=/opt/w64/bin INCLUDE_PATH=/opt/w64/include LIBRARY_PATH=/opt/w64/lib
install -Dm644 zlib-1.3.2/LICENSE $L/zlib/LICENSE
tar -xJf $S/freetype-2.14.3.tar.xz
cmake -S freetype-2.14.3 -B ft -G Ninja -DCMAKE_TOOLCHAIN_FILE=$T -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/opt/w64 \
 -DBUILD_SHARED_LIBS=OFF -DFT_DISABLE_ZLIB=ON -DFT_DISABLE_BZIP2=ON -DFT_DISABLE_PNG=ON -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BROTLI=ON
cmake --build ft && cmake --install ft
install -Dm644 freetype-2.14.3/LICENSE.TXT $L/freetype/LICENSE.TXT && install -Dm644 freetype-2.14.3/docs/FTL.TXT $L/freetype/FTL.TXT
tar -xzf $S/libjpeg-turbo-3.2.0.tar.gz
cmake -S libjpeg-turbo-3.2.0 -B jpeg -G Ninja -DCMAKE_TOOLCHAIN_FILE=$T -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/opt/w64 \
 -DCMAKE_INSTALL_LIBDIR=lib -DENABLE_SHARED=OFF -DENABLE_STATIC=ON -DWITH_TOOLS=OFF -DWITH_TESTS=OFF
cmake --build jpeg && cmake --install jpeg
install -Dm644 libjpeg-turbo-3.2.0/LICENSE.md $L/libjpeg-turbo/LICENSE.md && install -Dm644 libjpeg-turbo-3.2.0/README.ijg $L/libjpeg-turbo/README.ijg
tar -xJf $S/nlohmann-json-3.12.0.tar.xz
cmake -S json -B json-build -G Ninja -DCMAKE_TOOLCHAIN_FILE=$T -DCMAKE_INSTALL_PREFIX=/opt/w64 -DJSON_BuildTests=OFF -DJSON_Install=ON
cmake --install json-build
install -Dm644 json/LICENSE.MIT $L/nlohmann_json/LICENSE.MIT
tar -xzf $S/Vulkan-Headers-1.4.363.tar.gz
cmake -S Vulkan-Headers-1.4.363 -B vkh -G Ninja -DCMAKE_TOOLCHAIN_FILE=$T -DCMAKE_INSTALL_PREFIX=/opt/w64
cmake --install vkh
install -Dm644 Vulkan-Headers-1.4.363/LICENSE.md $L/Vulkan-Headers/LICENSE.md
# Only the loader import library is used: vulkan-1.dll comes from the GPU driver (packaging/windows/README.txt).
tar -xzf $S/Vulkan-Loader-1.4.363.tar.gz
cmake -S Vulkan-Loader-1.4.363 -B vkl -G Ninja -DCMAKE_TOOLCHAIN_FILE=$T -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/opt/w64 \
 -DBUILD_TESTS=OFF -DUPDATE_DEPS=OFF
cmake --build vkl && cmake --install vkl
install -Dm644 Vulkan-Loader-1.4.363/LICENSE.txt $L/Vulkan-Loader/LICENSE.txt
rm -rf /tmp/d
SMALL
ADD --checksum=sha256:603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a https://github.com/openssl/openssl/releases/download/openssl-3.5.9/openssl-3.5.9.tar.gz /opt/ikv-baseline/sources/
RUN <<'OPENSSL'
set -eu
mkdir /tmp/ossl && tar -xzf /opt/ikv-baseline/sources/openssl-3.5.9.tar.gz -C /tmp/ossl
cd /tmp/ossl/openssl-3.5.9
# Wave 19: with apps: the launcher's hosting issues its development CA/leaf through the bundled openssl.exe.
./Configure mingw64 --cross-compile-prefix=x86_64-w64-mingw32- --prefix=/opt/w64 --libdir=lib no-shared no-tests no-docs
make -j"$(nproc)" build_programs # depends on build_libs; naming both races on the same objects
make install_dev
install -Dm755 apps/openssl.exe /opt/w64/bin/openssl.exe
install -Dm644 LICENSE.txt /opt/w64/share/licenses/openssl/LICENSE.txt
rm -rf /tmp/ossl
OPENSSL
ADD --checksum=sha256:9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3 https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.tar.gz /opt/ikv-baseline/sources/
RUN <<'SDL'
set -eu
mkdir /tmp/sdl && tar -xzf /opt/ikv-baseline/sources/SDL3-3.4.18.tar.gz -C /tmp/sdl
cmake -S /tmp/sdl/SDL3-3.4.18 -B /tmp/sdl/build -G Ninja -DCMAKE_TOOLCHAIN_FILE=/opt/w64/mingw-w64.cmake -DCMAKE_BUILD_TYPE=Release \
 -DCMAKE_INSTALL_PREFIX=/opt/w64 -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_VULKAN=ON
cmake --build /tmp/sdl/build && cmake --install /tmp/sdl/build
install -Dm644 /tmp/sdl/SDL3-3.4.18/LICENSE.txt /opt/w64/share/licenses/SDL3/LICENSE.txt
rm -rf /tmp/sdl
SDL
ADD --checksum=sha256:9bed76128d4e46755dbe818487788c6fceb6f72b378f4daa49b7e1e600d9088d https://github.com/boostorg/boost/releases/download/boost-1.92.0/boost-1.92.0-cmake.tar.xz /opt/ikv-baseline/sources/
RUN <<'BOOST'
set -eu
# Header-only use (property_tree, asio, beast and their dependencies); Boost::headers comes from the installed BoostConfig.
mkdir /tmp/boost && tar -xJf /opt/ikv-baseline/sources/boost-1.92.0-cmake.tar.xz -C /tmp/boost
cmake -S /tmp/boost/boost-1.92.0 -B /tmp/boost/build -G Ninja -DCMAKE_TOOLCHAIN_FILE=/opt/w64/mingw-w64.cmake -DCMAKE_BUILD_TYPE=Release \
 -DCMAKE_INSTALL_PREFIX=/opt/w64 -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF -DBOOST_INCLUDE_LIBRARIES="property_tree;asio;beast"
cmake --build /tmp/boost/build && cmake --install /tmp/boost/build
install -Dm644 /tmp/boost/boost-1.92.0/LICENSE_1_0.txt /opt/w64/share/licenses/boost/LICENSE_1_0.txt
rm -rf /tmp/boost
BOOST
# Qt 6.11.3 official win64_mingw qtbase (built with MinGW 13.1.0 posix/seh/msvcrt: same ABI; the GCC 14 runtime DLLs
# shipped beside it are newer supersets). Qt's own .sha1 for this archive: f051a4dfa3dc0ea6edd1afa1cb26fdffcd3ac81d.
ADD --checksum=sha256:3b37753d8ee6d19ba181d6cd1ef464e23863bc4b3a1d5f1fad6f69093218452a https://download.qt.io/online/qtsdkrepository/windows_x86/desktop/qt6_6113/qt6_6113_mingw/qt.qt6.6113.win64_mingw/6.11.3-0-202609240253qtbase-Windows-Windows_11_24H2-Mingw-Windows-Windows_11_24H2-X86_64.7z /opt/ikv-baseline/sources/qtbase-6.11.3-win64_mingw.7z
# innoextract 1.9 for Windows (signed upstream; GOODSIG ADE9653703D4ADE0E997758128555A66D7E1DEC9, same key as
# packaging/helper-innoextract), used by the launcher's original-installer import.
ADD --checksum=sha256:6989342c9b026a00a72a38f23b62a8e6a22cc5de69805cf47d68ac2fec993065 https://constexpr.org/innoextract/files/innoextract-1.9-windows.zip /opt/ikv-baseline/sources/
# The prebuilt qtbase archive carries no licence text: Qt's own LGPL-3.0 (+ referenced GPL-3.0) files from qtbase.
ADD --checksum=sha256:da7eabb7bafdf7d3ae5e9f223aa5bdc1eece45ac569dc21b3b037520b4464768 https://raw.githubusercontent.com/qt/qtbase/v6.12.0/LICENSES/LGPL-3.0-only.txt /opt/w64/share/licenses/qt/
ADD --checksum=sha256:8ceb4b9ee5adedde47b31e975c1d90c73ad27b6b165a1dcd80c7c545eb65b903 https://raw.githubusercontent.com/qt/qtbase/v6.12.0/LICENSES/GPL-3.0-only.txt /opt/w64/share/licenses/qt/
RUN <<'PREBUILT'
set -eu
7z x -bd -o/opt/w64/qt /opt/ikv-baseline/sources/qtbase-6.11.3-win64_mingw.7z > /dev/null
chmod 644 /opt/w64/share/licenses/qt/*
7z x -bd -o/opt/w64/innoextract /opt/ikv-baseline/sources/innoextract-1.9-windows.zip > /dev/null
test -f /opt/w64/innoextract/innoextract.exe
PREBUILT
# Wave 19 Windows hosting: EDB PostgreSQL 16.15 binaries (server, initdb/pg_ctl/psql/pg_dump, libpq + import lib) and the
# Microsoft Visual C++ 2015-2022 x64 runtime they import (app-local vcruntime140/msvcp140 from the redist's own cabinet).
ADD --checksum=sha256:840b9d265f6ab6c0a971c1d8e9096de564950d38dc2a5ccd98c8820179ecf115 https://get.enterprisedb.com/postgresql/postgresql-16.15-2-windows-x64-binaries.zip /opt/ikv-baseline/sources/
ADD --checksum=sha256:cc0ff0eb1dc3f5188ae6300faef32bf5beeba4bdd6e8e445a9184072096b713b https://download.visualstudio.microsoft.com/download/pr/990c720a-46ce-4dc7-87b9-c2eb615d34b9/CC0FF0EB1DC3F5188AE6300FAEF32BF5BEEBA4BDD6E8E445A9184072096B713B/VC_redist.x64.exe /opt/ikv-baseline/sources/
RUN <<'POSTGRES'
set -eu
7z x -bd -o/opt/w64 /opt/ikv-baseline/sources/postgresql-16.15-2-windows-x64-binaries.zip 'pgsql/bin' 'pgsql/lib' 'pgsql/share' 'pgsql/include' \
  pgsql/server_license.txt pgsql/commandlinetools_3rd_party_licenses.txt > /dev/null
mkdir -p /opt/w64/share/licenses/postgresql && mv /opt/w64/pgsql/*.txt /opt/w64/share/licenses/postgresql/
mkdir /tmp/vc && cd /tmp/vc
7z x -bd -t# /opt/ikv-baseline/sources/VC_redist.x64.exe > /dev/null # burn bundle: the attached container is 4.cab
7z x -bd 4.cab a12 > /dev/null && 7z x -bd -ort a12 > /dev/null
mkdir -p /opt/w64/vcredist && for dll in vcruntime140 vcruntime140_1 msvcp140 msvcp140_1; do cp rt/$dll.dll_amd64 /opt/w64/vcredist/$dll.dll; done
7z x -bd /opt/ikv-baseline/sources/VC_redist.x64.exe u4 > /dev/null && grep -q 'MICROSOFT SOFTWARE LICENSE TERMS' u4
install -Dm644 u4 /opt/w64/share/licenses/vcredist/LICENSE.rtf
rm -rf /tmp/vc
POSTGRES
WORKDIR /work
CMD ["/bin/bash"]
