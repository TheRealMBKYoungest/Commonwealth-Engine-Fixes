// CEF hard version gate (FCStruct2 and FCShadow2 only: CEFCrashLog patches no game code, so it has no gate and stays active).
//
// Address Library IDs keep the engine *addresses* portable, but CEF also relies on things Address Library cannot cover:
// call-site offsets inside functions (byte-checked) and structure layouts (FaceGenData +0x34/+0x234, array +0x50.., not
// checkable). A new game build can keep the call bytes and still move a structure field, so a patch must not install on any
// executable it was not verified against. The gate accepts exactly one build: F4SE's runtime version AND the PE header of the
// running Fallout4.exe (link timestamp + image size), read from memory. Anything else: the plugin patches nothing.
//
// Verified build: Fallout4.exe 1.11.240.0, PE TimeDateStamp 0x6A727FCC (2026-08-05 00:11:56 UTC), SizeOfImage 0x0425A000.
#pragma once
#include <windows.h>
#include <cstdint>
#include <cstdio>

namespace cef {
constexpr uint32_t kRuntime    = 0x010B0F00;   // 1.11.240
#ifdef CEF_TEST_WRONG_EXE                      // test build only: pretend the verified exe is a different one
constexpr uint32_t kTimeStamp  = 0x6A727FCD;
#else
constexpr uint32_t kTimeStamp  = 0x6A727FCC;
#endif
constexpr uint32_t kImageSize  = 0x0425A000;

// true = this exact executable; false = why not, in `why` (one line, for the plugin's log)
inline bool VersionOk(uint32_t runtime, char* why, size_t whyLen) {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(GetModuleHandleA(nullptr));
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(reinterpret_cast<const uint8_t*>(dos) + dos->e_lfanew);
    const uint32_t ts = nt->FileHeader.TimeDateStamp, size = nt->OptionalHeader.SizeOfImage;
    if (runtime == kRuntime && ts == kTimeStamp && size == kImageSize) return true;
    snprintf(why, whyLen,
             "game executable is runtime %u.%u.%u (build stamp %08X, image size %08X); this CEF build was verified only for "
             "1.11.240 (stamp %08X, size %08X)",
             runtime >> 24, (runtime >> 16) & 0xFF, (runtime >> 4) & 0xFFF, ts, size, kTimeStamp, kImageSize);
    return false;
}
}  // namespace cef
