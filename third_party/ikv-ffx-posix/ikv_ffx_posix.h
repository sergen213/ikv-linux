// IKV: stand-ins for the MSVC CRT names used by the vendored FidelityFX SDK v1.1.4 sources, so the upstream files
// need no portability edits (their bug-fix patches are listed in third_party/FidelityFX-SDK/IKV-PROVENANCE.txt).
// Force-included (-include) into every translation unit of ikv-fsr3, on Linux and on Windows (MinGW-w64).
#pragma once
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <codecvt>
#include <locale>
#include <new>
#include <string>
#include <FidelityFX/host/ffx_util.h>
// wchar_t is 4 bytes on Linux (2 on Windows): FfxFsr3UpscalerContext_Private's resource/pipeline names take 818 KiB, more
// than the upstream 512 KiB opaque FfxFsr3UpscalerContext (ffx_fsr3upscaler.cpp static_asserts the fit). Kept on both.
#undef FFX_SDK_DEFAULT_CONTEXT_SIZE
#define FFX_SDK_DEFAULT_CONTEXT_SIZE (1024 * 256)
#ifdef _WIN32
// MinGW-w64 has _countof, wcscpy_s, strcpy_s and sprintf_s; msvcrt declares no swprintf_s (ffx_message.cpp).
#define swprintf_s std::swprintf
#else
template<class T,std::size_t N> constexpr std::size_t _countof(T (&)[N]) noexcept {return N;}
inline int wcscpy_s(wchar_t* destination,std::size_t capacity,const wchar_t* source) {
    if(!destination||!capacity)return 22;
    if(!source||std::wcslen(source)>=capacity){destination[0]=L'\0';return 22;}
    std::wcscpy(destination,source);return 0;
}
template<std::size_t N> int wcscpy_s(wchar_t (&destination)[N],const wchar_t* source) {return wcscpy_s(destination,N,source);}
inline int strcpy_s(char* destination,std::size_t capacity,const char* source) {
    if(!destination||!capacity)return 22;
    if(!source||std::strlen(source)>=capacity){destination[0]='\0';return 22;}
    std::strcpy(destination,source);return 0;
}
#define sprintf_s std::snprintf
#endif
