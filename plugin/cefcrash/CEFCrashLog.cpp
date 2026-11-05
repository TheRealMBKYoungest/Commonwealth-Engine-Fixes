// CEFCrashLog: local crash log for Commonwealth Engine Fixes. Patches no game code, so it runs on any game version.
//
// On an unhandled exception it writes Documents\My Games\Fallout4\F4SE\CEF-Crash-<date>_<time>.log: exception code, faulting
// module+offset, call stack (x64 unwind data, so exact frames), a stack scan, registers, memory, and the loaded modules by base
// name only (no user paths). Fallout4.exe frames also get their function's Address Library ID, which stays the same across game
// versions. The log stays on this PC; analyze_logs.py explains it in plain English. Nothing is ever sent anywhere.
//
// Filter placement: the game replaces the unhandled-exception filter after plugins load (F4SE Hooks_Debug.cpp notes "something
// else is overriding the exception filter"), so, like Buffout 4 (CrashHandler.cpp VectoredExceptions), a vectored handler re-arms
// ours on every exception and returns CONTINUE_SEARCH: ours is the active filter whenever an exception goes unhandled. The filter
// it replaced is chained after the log is written. If Buffout 4 is loaded it already does this; CEF stands down so the two
// don't fight, and analyze_logs.py reads Buffout's log instead.
//
// Not catchable by any filter: __fastfail exits (0xC0000409 from security checks), TerminateProcess, and hangs.

#include <windows.h>
#include <shlobj.h>
#include <psapi.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <share.h>

// ---------------------------------------------------------------- F4SE plugin interface (f4se 0.7.9 PluginAPI.h)
struct F4SEInterface {
    uint32_t f4seVersion, runtimeVersion, editorVersion, isEditor;
    void* (*QueryInterface)(uint32_t);
    uint32_t (*GetPluginHandle)();
    uint32_t (*GetReleaseIndex)();
    const void* (*GetPluginInfo)(const char*);
    const char* (*GetSaveFolderName)();
};
struct F4SEMessage { const char* sender; uint32_t type; uint32_t dataLen; void* data; };
struct F4SEMessagingInterface {
    uint32_t interfaceVersion;
    bool (*RegisterListener)(uint32_t listener, const char* sender, void (*handler)(F4SEMessage*));
};
constexpr uint32_t kInterface_Messaging = 1;
constexpr uint32_t kMessage_InputLoaded = 7, kMessage_GameDataReady = 10;
struct F4SEPluginVersionData {
    uint32_t dataVersion, pluginVersion;
    char name[256], author[256];
    uint32_t addressIndependence, structureIndependence;
    uint32_t compatibleVersions[16];
    uint32_t seVersionRequired, reservedNonBreaking, reservedBreaking;
    uint8_t reserved[512];
};
extern "C" __declspec(dllexport) F4SEPluginVersionData F4SEPlugin_Version = {
    1, 0x00010000, "CEFCrashLog", "Commonwealth Engine Fixes",
    1 << 2,                  // kAddressIndependence_AddressLibrary_1_11_137 (touches no game code; IDs used only for the report)
    1 << 0,                  // kStructureIndependence_NoStructs
    { 0x010B0F00, 0 }, 0, 0, 0, {}
};

// Functions CEF redirects calls to (FCStruct2 1.1.3), by Address Library ID: named in the report when they appear on the stack.
struct Known { uint64_t id; const char* name; };
static const Known kKnown[] = {
    { 2209308, "UsePreprocessedFaceGen (FCStruct2 hooks calls to it)" },
    { 2209533, "FaceGen head NIF load (FCStruct2 hooks calls to it)" },
    { 2209317, "face-paint request enqueue (FCStruct2 hooks calls to it)" },
    { 2209525, "StartFaceCustomizationGenerationForNPC (FCStruct2 hooks calls to it)" },
    { 2209527, "face tint GPU merge (FCStruct2 hooks calls to it)" },
    { 2209523, "CreateMergeTintTextures (FCStruct2 hooks calls to it)" },
    { 2209309, "BSFaceGenManager::Update (FCStruct2 hooks calls to it)" },
    { 2228913, "main frame function (FCStruct2 hooks one call in it)" },
    { 2318474, "BSMTAManager::Submit (render registration)" },
};

// ---------------------------------------------------------------- state (all precomputed at load: the crash path allocates nothing)
static char g_dir[MAX_PATH];             // ...\My Games\Fallout4\F4SE
static char g_addrLib[MAX_PATH];         // Address Library bin for this runtime
static uint32_t g_runtime, g_f4se;
static DWORD g_mainTid;
static ULONGLONG g_startTick;
static LPTOP_LEVEL_EXCEPTION_FILTER g_prev;   // the filter ours replaced (chained after writing)
static volatile LONG g_busy;
static FILE* g_log;

static void Log(const char* fmt, ...) {
    if (!g_log) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list a; va_start(a, fmt); vfprintf(g_log, fmt, a); va_end(a);
    fputc('\n', g_log); fflush(g_log);
}

// ---------------------------------------------------------------- report buffer and module table
static char g_out[256 * 1024];
static size_t g_len;
static void P(const char* fmt, ...) {
    if (g_len >= sizeof(g_out) - 1) return;
    va_list a; va_start(a, fmt);
    int n = _vsnprintf_s(g_out + g_len, sizeof(g_out) - g_len, _TRUNCATE, fmt, a);
    va_end(a);
    g_len = n < 0 ? sizeof(g_out) - 1 : g_len + size_t(n);
}

struct Mod { uintptr_t base, end; char name[64]; };
static Mod g_mods[1024];
static int g_nMods;
static uintptr_t g_exeBase, g_exeEnd;

static void LoadModules() {
    static HMODULE hm[1024];
    DWORD need = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), hm, sizeof(hm), &need)) return;
    int n = int(need / sizeof(HMODULE)); if (n > 1024) n = 1024;
    for (int i = 0; i < n; ++i) {
        MODULEINFO mi = {};
        if (!K32GetModuleInformation(GetCurrentProcess(), hm[i], &mi, sizeof(mi))) continue;
        Mod& m = g_mods[g_nMods++];
        m.base = reinterpret_cast<uintptr_t>(mi.lpBaseOfDll); m.end = m.base + mi.SizeOfImage;
        if (!K32GetModuleBaseNameA(GetCurrentProcess(), hm[i], m.name, sizeof(m.name))) strcpy_s(m.name, "?");
    }
    g_exeBase = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    for (int i = 0; i < g_nMods; ++i) if (g_mods[i].base == g_exeBase) g_exeEnd = g_mods[i].end;
}
static const Mod* FindMod(uintptr_t a) {
    for (int i = 0; i < g_nMods; ++i) if (a >= g_mods[i].base && a < g_mods[i].end) return &g_mods[i];
    return nullptr;
}
static void Where(uintptr_t a, char* buf, size_t len) {
    const Mod* m = FindMod(a);
    if (m) _snprintf_s(buf, len, _TRUNCATE, "%s+0x%llX", m->name, (unsigned long long)(a - m->base));
    else _snprintf_s(buf, len, _TRUNCATE, "0x%016llX", (unsigned long long)a);
}

// ---------------------------------------------------------------- Fallout4.exe frames -> function start -> Address Library ID
constexpr int kMaxFrames = 48;
static uintptr_t g_frames[kMaxFrames];
static int g_nFrames;
static uint32_t g_fnRva[kMaxFrames];     // function start RVA (exe frames only, else 0)
static uint64_t g_fnId[kMaxFrames];      // its Address Library ID (0 = unknown)

static void ResolveIds() {
    HANDLE f = CreateFileA(g_addrLib, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    static uint64_t buf[2 * 4096];
    uint64_t count = 0; DWORD got = 0;
    if (ReadFile(f, &count, 8, &got, nullptr) && got == 8 && count < 10000000) {
        while (count) {
            DWORD want = DWORD((count < 4096 ? count : 4096) * 16);
            if (!ReadFile(f, buf, want, &got, nullptr) || got != want) break;
            for (DWORD e = 0; e < want / 16; ++e)
                for (int i = 0; i < g_nFrames; ++i)
                    if (g_fnRva[i] && !g_fnId[i] && buf[2 * e + 1] == g_fnRva[i]) g_fnId[i] = buf[2 * e];
            count -= want / 16;
        }
    }
    CloseHandle(f);
}

static void Unwind(const CONTEXT* start) {
    static CONTEXT c;
    c = *start;
    g_nFrames = 0;
    __try {
        while (g_nFrames < kMaxFrames && c.Rip) {
            g_frames[g_nFrames] = c.Rip;
            DWORD64 imageBase = 0;
            PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(c.Rip, &imageBase, nullptr);
            g_fnRva[g_nFrames] = (fe && imageBase == g_exeBase) ? fe->BeginAddress : 0;
            g_fnId[g_nFrames] = 0;
            ++g_nFrames;
            if (fe) {
                void* handlerData = nullptr; DWORD64 establisher = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, c.Rip, fe, &c, &handlerData, &establisher, nullptr);
            } else {                                          // leaf function: return address on top of the stack
                c.Rip = *reinterpret_cast<DWORD64*>(c.Rsp);
                c.Rsp += 8;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static const char* KnownName(uint64_t id) {
    for (const Known& k : kKnown) if (k.id == id) return k.name;
    return nullptr;
}

static const char* ExceptionName(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:       return "EXCEPTION_ACCESS_VIOLATION";
    case EXCEPTION_STACK_OVERFLOW:         return "EXCEPTION_STACK_OVERFLOW";
    case EXCEPTION_ILLEGAL_INSTRUCTION:    return "EXCEPTION_ILLEGAL_INSTRUCTION";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:     return "EXCEPTION_INT_DIVIDE_BY_ZERO";
    case EXCEPTION_PRIV_INSTRUCTION:       return "EXCEPTION_PRIV_INSTRUCTION";
    case EXCEPTION_IN_PAGE_ERROR:          return "EXCEPTION_IN_PAGE_ERROR";
    case EXCEPTION_BREAKPOINT:             return "EXCEPTION_BREAKPOINT";
    case 0xC0000374:                       return "STATUS_HEAP_CORRUPTION";
    case 0xC0000409:                       return "STATUS_STACK_BUFFER_OVERRUN";
    case 0xC0000417:                       return "STATUS_INVALID_CRUNTIME_PARAMETER";
    case 0xE06D7363:                       return "C++ exception (MSVC)";
    case 0xC0000017:                       return "STATUS_NO_MEMORY";
    default:                               return "unknown";
    }
}

// ---------------------------------------------------------------- the report
static void WriteReport(EXCEPTION_POINTERS* ep) {
    g_len = 0;
    LoadModules();
    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    const CONTEXT* cx = ep->ContextRecord;
    char w[128];
    SYSTEMTIME t; GetLocalTime(&t);

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_exeBase);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(g_exeBase + dos->e_lfanew);
    P("CEF crash log 1.0 (CEFCrashLog.dll). Written on this PC only; nothing is sent anywhere.\n");
    P("Explain it in plain English: run analyze_logs.py (see the CEF README).\n\n");
    P("time: %04d-%02d-%02d %02d:%02d:%02d, %llu s after game start\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
      (GetTickCount64() - g_startTick) / 1000);
    P("game: runtime %u.%u.%u, exe stamp %08X, image size %08X\n", g_runtime >> 24, (g_runtime >> 16) & 0xFF,
      (g_runtime >> 4) & 0xFFF, nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage);
    P("F4SE: %u.%u.%u\n\n", g_f4se >> 24, (g_f4se >> 16) & 0xFF, (g_f4se >> 4) & 0xFFF);

    Where(reinterpret_cast<uintptr_t>(er->ExceptionAddress), w, sizeof(w));
    P("exception: %08X (%s)\n", er->ExceptionCode, ExceptionName(er->ExceptionCode));
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
        const ULONG_PTR kind = er->ExceptionInformation[0];
        P("access: %s address 0x%016llX\n", kind == 0 ? "read of" : kind == 1 ? "write to" : "execute at",
          (unsigned long long)er->ExceptionInformation[1]);
    }
    P("at: %s\n", w);
    const DWORD tid = GetCurrentThreadId();
    P("thread: %lu%s\n", tid, tid == g_mainTid ? " (main thread)" : "");
    P("CEF plugins loaded: FCStruct2 %s, FCRender2 %s\n\n", GetModuleHandleA("FCStruct2.dll") ? "yes" : "no",
      GetModuleHandleA("FCRender2.dll") ? "yes" : "no");

    MEMORYSTATUSEX ms = { sizeof(ms) };
    PROCESS_MEMORY_COUNTERS_EX pm = {}; pm.cb = sizeof(pm);
    if (GlobalMemoryStatusEx(&ms))
        P("memory: physical %llu / %llu MB used, commit %llu / %llu MB used\n",
          (ms.ullTotalPhys - ms.ullAvailPhys) >> 20, ms.ullTotalPhys >> 20, (ms.ullTotalPageFile - ms.ullAvailPageFile) >> 20,
          ms.ullTotalPageFile >> 20);
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pm), sizeof(pm)))
        P("game process: private %llu MB, working set %llu MB\n\n", (unsigned long long)(pm.PrivateUsage >> 20),
          (unsigned long long)(pm.WorkingSetSize >> 20));

    Unwind(cx);
    ResolveIds();
    P("call stack (innermost first):\n");
    for (int i = 0; i < g_nFrames; ++i) {
        Where(g_frames[i], w, sizeof(w));
        P("  %2d  %s", i, w);
        if (g_fnRva[i]) {
            P("   [function Fallout4.exe+0x%X", g_fnRva[i]);
            if (g_fnId[i]) {
                P(", Address Library ID %llu", (unsigned long long)g_fnId[i]);
                if (const char* k = KnownName(g_fnId[i])) P(": %s", k);
            }
            P("]");
        }
        P("\n");
    }

    P("\nstack scan (code addresses found on the stack; may include stale values):\n");
    __try {
        const uintptr_t* sp = reinterpret_cast<const uintptr_t*>(cx->Rsp);
        int shown = 0; uintptr_t last = 0;
        for (int i = 0; i < 1024 && shown < 40; ++i) {
            const uintptr_t v = sp[i];
            const Mod* m = FindMod(v);
            if (!m || v == last || v - m->base < 0x1000) continue;
            Where(v, w, sizeof(w)); P("  rsp+0x%03X  %s\n", i * 8, w); last = v; ++shown;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { P("  (stack unreadable)\n"); }

    P("\nregisters:\n");
    const DWORD64 regs[] = { cx->Rax, cx->Rbx, cx->Rcx, cx->Rdx, cx->Rsi, cx->Rdi, cx->Rbp, cx->Rsp,
                             cx->R8, cx->R9, cx->R10, cx->R11, cx->R12, cx->R13, cx->R14, cx->R15, cx->Rip };
    const char* names[] = { "RAX", "RBX", "RCX", "RDX", "RSI", "RDI", "RBP", "RSP", "R8 ", "R9 ", "R10", "R11", "R12", "R13",
                            "R14", "R15", "RIP" };
    for (int i = 0; i < 17; ++i) {
        const Mod* m = FindMod(uintptr_t(regs[i]));
        if (m) { Where(uintptr_t(regs[i]), w, sizeof(w)); P("  %s 0x%016llX  %s\n", names[i], regs[i], w); }
        else P("  %s 0x%016llX\n", names[i], regs[i]);
    }

    P("\nmodules (%d):\n", g_nMods);
    for (int i = 0; i < g_nMods; ++i)
        P("  %-40s 0x%016llX  size 0x%llX\n", g_mods[i].name, (unsigned long long)g_mods[i].base,
          (unsigned long long)(g_mods[i].end - g_mods[i].base));

    char path[MAX_PATH];
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\CEF-Crash-%04d-%02d-%02d_%02d-%02d-%02d.log", g_dir, t.wYear, t.wMonth, t.wDay,
                t.wHour, t.wMinute, t.wSecond);
    HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD done = 0;
        WriteFile(f, g_out, DWORD(g_len), &done, nullptr);
        FlushFileBuffers(f);
        CloseHandle(f);
    }
    Log("crash: %08X at %s -> %s", er->ExceptionCode, w, path);
}

static DWORD WINAPI ReportThread(LPVOID ep) { WriteReport(static_cast<EXCEPTION_POINTERS*>(ep)); return 0; }

static LONG WINAPI Filter(EXCEPTION_POINTERS* ep) {
    if (InterlockedExchange(&g_busy, 1) == 0) {
        if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_STACK_OVERFLOW) {
            // almost no stack left on this thread: write the report from a fresh one
            HANDLE h = CreateThread(nullptr, 1 << 20, ReportThread, ep, 0, nullptr);
            if (h) { WaitForSingleObject(h, 15000); CloseHandle(h); }
        } else {
            WriteReport(ep);
        }
        if (g_prev && g_prev != Filter) return g_prev(ep);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static void Arm() {
    LPTOP_LEVEL_EXCEPTION_FILTER old = SetUnhandledExceptionFilter(Filter);
    if (old && old != Filter) g_prev = old;
}

// Runs first on every exception, handled or not: only re-arms the filter, never handles anything.
static LONG WINAPI Rearm(EXCEPTION_POINTERS*) { Arm(); return EXCEPTION_CONTINUE_SEARCH; }

#if defined(CEF_TEST_CRASH)
// test builds only: 1 = crash in the F4SE game-data-ready message (main thread, during startup loading), 2 = crash a worker thread 45 s
// after load
static void TestCrashNow() { Log("TEST: forcing a null write"); *static_cast<volatile int*>(nullptr) = 1; }
static DWORD WINAPI TestCrashThread(LPVOID) { Sleep(45000); TestCrashNow(); return 0; }
#endif

static void OnMessage(F4SEMessage* m) {
    if (!m) return;
    if (m->type == kMessage_InputLoaded || m->type == kMessage_GameDataReady) {
        char w[128] = "none";
        LPTOP_LEVEL_EXCEPTION_FILTER cur = SetUnhandledExceptionFilter(Filter);
        if (cur != Filter) {
            if (cur) { g_prev = cur; LoadModules(); Where(reinterpret_cast<uintptr_t>(cur), w, sizeof(w)); }
            Log("message %u: the active filter had been replaced (by %s); CEF re-armed and chains to it", m->type, w);
        }
#if defined(CEF_TEST_CRASH) && CEF_TEST_CRASH == 1
        if (m->type == kMessage_GameDataReady) { Log("TEST: message on thread %lu (main %lu)", GetCurrentThreadId(), g_mainTid); TestCrashNow(); }
#endif
    }
}

extern "C" __declspec(dllexport) bool F4SEPlugin_Load(const F4SEInterface* f4se) {
    g_startTick = GetTickCount64();
    g_mainTid = GetCurrentThreadId();
    g_runtime = f4se->runtimeVersion; g_f4se = f4se->f4seVersion;
    char docs[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_PERSONAL, nullptr, SHGFP_TYPE_CURRENT, docs))) {
        _snprintf_s(g_dir, sizeof(g_dir), _TRUNCATE, "%s\\My Games\\Fallout4\\F4SE", docs);
        char lp[MAX_PATH];
        _snprintf_s(lp, sizeof(lp), _TRUNCATE, "%s\\CEFCrashLog.log", g_dir);
        g_log = _fsopen(lp, "w", _SH_DENYWR);
    }
    if (f4se->isEditor) return false;

    char exe[MAX_PATH]; GetModuleFileNameA(nullptr, exe, MAX_PATH);
    char* slash = strrchr(exe, '\\'); if (slash) *slash = 0;
    const uint32_t rt = g_runtime;
    _snprintf_s(g_addrLib, sizeof(g_addrLib), _TRUNCATE, "%s\\Data\\F4SE\\Plugins\\version-%u-%u-%u-%u.bin", exe, rt >> 24,
                (rt >> 16) & 0xFF, (rt >> 4) & 0xFFF, rt & 0xF);
    char ini[MAX_PATH]; _snprintf_s(ini, sizeof(ini), _TRUNCATE, "%s\\Data\\F4SE\\Plugins\\CEFCrashLog.ini", exe);

    Log("CEFCrashLog 1.0.0, runtime %08X, F4SE %08X", g_runtime, g_f4se);
    if (!GetPrivateProfileIntA("General", "bEnable", 1, ini)) { Log("disabled in CEFCrashLog.ini (bEnable=0)"); return true; }
    if (GetModuleHandleA("Buffout4.dll")) {
        Log("Buffout 4 is loaded and writes its own crash logs; CEF crash logging stands down (analyze_logs.py reads Buffout's logs)");
        return true;
    }
    Arm();
    AddVectoredExceptionHandler(1, Rearm);
    if (auto* msg = static_cast<F4SEMessagingInterface*>(f4se->QueryInterface(kInterface_Messaging)))
        msg->RegisterListener(f4se->GetPluginHandle(), "F4SE", OnMessage);
#if defined(CEF_TEST_CRASH) && CEF_TEST_CRASH == 2
    CreateThread(nullptr, 0, TestCrashThread, nullptr, 0, nullptr);
#endif
    Log("armed: crash logs go to %s\\CEF-Crash-<date>_<time>.log", g_dir);
    return true;
}
