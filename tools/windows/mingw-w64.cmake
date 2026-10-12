# MinGW-w64 x86_64 (posix threads, SEH, msvcrt) cross toolchain for the container in tools/windows/Dockerfile.
# Dependencies live in /opt/w64 (static libs, Vulkan loader import lib) and /opt/w64/qt (Qt 6.11.3 win64_mingw).
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc-posix)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++-posix)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)
set(CMAKE_FIND_ROOT_PATH /opt/w64 /opt/w64/qt /usr/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(ENV{PKG_CONFIG_LIBDIR} /opt/w64/lib/pkgconfig)
set(ENV{PKG_CONFIG_PATH} "")
