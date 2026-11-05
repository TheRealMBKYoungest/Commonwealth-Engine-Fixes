// FCStruct2 1.1.0 (STRUCT-2 FaceGen Optimizer, design: analysis/design/STRUCT-2-FaceGenOptimizer-TDD.md)
// Shipping defaults: ON at start; A (gate) off except NPCs on the INI force list; B safety net on; C queue steps + array
// pool on (engine cache mode, VRAM guard); D VRAM logging on. Verify mode (face texture hashes) is a test switch.
//
// Fallout4.exe 1.11.240. Every hook is a byte-checked call-site redirect (E8 rel32) that calls the original:
//   P1/A gate      UsePreprocessedFaceGen(npc) 0x006DF3A0 (ID 2209308), 6 sites. Logs once per NPC. Part A: a vanilla
//                  "no" becomes "yes" when the NPC is vanilla-eligible, its winning file is a master and its FaceGeom NIF
//                  exists (the same check the engine runs before loading a pre-made head) - fewer live paint jobs.
//   P2 head        FaceGeom NIF load 0x006EDD00 (ID 2209533), 1 site: gate yes but NIF failed = head dropped
//   P3 request     live face-paint enqueue 0x006E0060 (ID 2209317), 3 sites (counted)
//   P4/B start     StartFaceCustomizationGenerationForNPC 0x006EC0D0 (ID 2209525), 4 sites. Part B: on an abort with no
//                  async load in flight, retry once; if that fails too the engine drops the request and the head keeps
//                  its plain unpainted look (logged). A test switch forces failures.
//   P5/C merge     GPU tint merge 0x006ED320 (ID 2209527), 2 sites: lost layers (never submitted) logged with VRAM.
//                  Part C pool: after an update-path merge, keep the request's 4 texture arrays and hand them to the
//                  next request (each request's FaceGenData is freed with it, so the per-array layer cache never
//                  survived from one face to the next). Experimental, off by default; any lost layer turns it off.
//   P6 blocking    CreateMergeTintTextures 0x006EBED0 (ID 2209523), 2 sites: stall duration
//   C  update      BSFaceGenManager::Update 0x006DF490 (ID 2209309), its frame-function site (0x00C33250 +0x62):
//                  call it again in the same frame while a face is ready or a new one can start (budgeted)
//   D              VRAM budget/usage (DXGI) and queue depth every 10 s and at every failure
// Face generation can run on background-load threads, so shared state is atomic or lock-protected and every read of
// engine memory is guarded.
//
// Self-contained: no CommonLibF4. Address Library is read directly.

#include <windows.h>
#include <shlobj.h>
#include <d3d11.h>
#include <dxgi1_4.h>
#include <intrin.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <share.h>
#include "../common/cef_version.h"

// ---------------------------------------------------------------- F4SE plugin interface (f4se 0.7.9 PluginAPI.h)
struct F4SEInterface {
    uint32_t f4seVersion, runtimeVersion, editorVersion, isEditor;
    void* (*QueryInterface)(uint32_t);
    uint32_t (*GetPluginHandle)();
    uint32_t (*GetReleaseIndex)();
    const void* (*GetPluginInfo)(const char*);
    const char* (*GetSaveFolderName)();
};
struct F4SEPluginVersionData {
    uint32_t dataVersion, pluginVersion;
    char name[256], author[256];
    uint32_t addressIndependence, structureIndependence;
    uint32_t compatibleVersions[16];
    uint32_t seVersionRequired, reservedNonBreaking, reservedBreaking;
    uint8_t reserved[512];
};
extern "C" __declspec(dllexport) F4SEPluginVersionData F4SEPlugin_Version = {
    1, 0x00010103, "FCStruct2", "Commonwealth Engine Fixes",
    1 << 2,                  // kAddressIndependence_AddressLibrary_1_11_137
    1 << 0,                  // kStructureIndependence_NoStructs (reads are guarded)
    { 0x010B0F00, 0 }, 0, 0, 0, {}
};

// ---------------------------------------------------------------- engine pieces (Address Library IDs, 1.11.240)
enum Tgt { kGate, kHeadNif, kRequest, kStart, kMerge, kBlocking, kUpdate, kTgtCount };
static const uint64_t kTgtId[kTgtCount] = { 2209308, 2209533, 2209317, 2209525, 2209527, 2209523, 2209309 };
static const char* const kTgtName[kTgtCount] = { "gate", "headNif", "request", "start", "merge", "blocking", "update" };

enum Aux { kGetFile, kIsMaster, kSetLookup, kFaceGeomPath, kMeshExists, kReady, kDummy1, kDummy2, kChildKw, kSet,
           kManager, kDevice, kImmediate, kRTM, kRendererObj, kAuxCount };
static const uint64_t kAuxId[kAuxCount] = {
    2193103,   // TESFile* GetFile(TESForm*, int), -1 = winning file          0x003123D0
    2192547,   // bool IsMaster(TESFile*)                                     0x002F9160
    2227898,   // bool SetContains(set, TESNPC*, 0x800)                       0x00BECA40
    2207453,   // bool FaceGeomPath(TESNPC*, char out[260])                   0x00658E60
    2192609,   // bool MeshExists(const char* path)                           0x002FC0C0
    2209526,   // bool AllLayersReady(FaceGenData*)                           0x006ED2A0
    4799415,   // TESNPC* chargen dummy 1                                     0x031F1278
    4799413,   // TESNPC* chargen dummy 2                                     0x031F1240
    4799417,   // BGSKeyword* "child player" keyword                          0x031F12B0
    2697789,   // set used by the gate                                        0x032A85A8
    4796313,   // BSFaceGenManager* singleton                                 0x030E8928
    2704430,   // ID3D11Device*                                               0x038E13A8
    2704432,   // ID3D11DeviceContext* immediate                              0x038E13B8
    2666735,   // RenderTargetManager (object; +0xDF4 slot -> target index)   0x02F4D750
    2704525,   // BSGraphics::Renderer (object; +0x10 RendererData, renderTargets at +0xA58, 0x30 each)  0x03A25D00
};

enum Site_ { kUpdate1 = 10, kUpdate2 = 11, kMergeUpdate = 14 };
struct Site { int tgt; uint64_t callerId; uint32_t offset; const char* name; };
static const Site kSites[] = {
    { kGate, 2207435, 0x03B, "skinTone" },  { kGate, 2209355, 0x062, "headBuild1" }, { kGate, 2209355, 0x088, "headBuild2" },
    { kGate, 2192002, 0x191, "preload" },   { kGate, 2209307, 0x0A7, "headHelper" }, { kGate, 2209326, 0x063, "headTask" },
    { kHeadNif, 2209355, 0x078, "headBuild" },
    { kRequest, 2209313, 0x03B, "req1" },   { kRequest, 2209314, 0x033, "req2" },    { kRequest, 2209315, 0x04D, "req3" },
    { kStart, 2209309, 0x45B, "update1" },  { kStart, 2209309, 0x5DD, "update2" },   { kStart, 2209298, 0x2BB, "other" },
    { kStart, 2209523, 0x114, "blocking" },
    { kMerge, 2209309, 0x20B, "update" },   { kMerge, 2209523, 0x1AE, "blocking" },
    { kBlocking, 2209309, 0x3EE, "update" }, { kBlocking, 2209316, 0x0AD, "virtual" },
    { kUpdate, 2228913, 0x062, "frame" },
};
constexpr int kSiteCount = sizeof(kSites) / sizeof(kSites[0]);

// layouts (CommonLibF4 + decompiled/bf0*.c.txt)
namespace off {
    constexpr size_t FormID     = 0x14;    // TESForm::formID
    constexpr size_t KeywordForm = 0x160;  // TESNPC: BGSKeywordForm subobject (vfunc 1 = HasKeyword)
    constexpr size_t FaceNPC    = 0x270;   // TESNPC::faceNPC (face template chain)
    constexpr size_t FileName   = 0x70;    // TESFile::filename[260]
    constexpr size_t FileFlags  = 0x334;   // TESFile flags, bit 0 = master
    constexpr size_t FileIndex  = 0x370;   // TESFile::compileIndex
    constexpr size_t Arrays     = 0x08;    // FaceGenData: 4 x StaticTextureIndexed* at +0x08..+0x20
    constexpr size_t ArrRef     = 0x08;    // array: reference count (vfunc 1 = delete when it drops to 0)
    constexpr size_t ArrGpu     = 0x38;    // array: GPU texture array (null = creation failed)
    constexpr size_t ArrSlices  = 0x48;    // array: GPU slice count (layer count, or 0x80 in the engine's cache mode)
    constexpr size_t ArrSlots   = 0x60;    // array: slot table, 12-byte entries {key64, version32} (count at +0x70)
    constexpr size_t ArrSlotCnt = 0x70;
    constexpr size_t FdStamps   = 0x34;    // FaceGenData: per-slot LRU stamps, 128 x uint32 (allocator 0x006E9C50)
    constexpr size_t FdGen      = 0x234;   // FaceGenData: generation counter, +1 per start (0x006EC0D0)
    constexpr size_t ArrTotal   = 0x50;    // ready check 0x017F5EA0: asyncDone + loaded + notSub == total
    constexpr size_t ArrAsync   = 0x54;    // async loads finished (success or not)
    constexpr size_t ArrLoaded  = 0x58;    // satisfied at submit: cache hit or immediate load
    constexpr size_t ArrNotSub  = 0x5C;    // never submitted (array creation failed or format mismatch): real loss
    constexpr size_t MgrRing    = 0x3100;  // manager: ring object (+0x2400 head, +0x2500 tail, low 8 bits)
    constexpr size_t MgrOverflow = 0x31B0; // manager: overflow-list count
    constexpr size_t MgrRequest = 0x31B8;  // manager: current request (+0 actor ref, +0x30 FaceGenData)
    constexpr size_t MgrNextReq = 0x31F0;  // manager: pending next request (+0, +8)
    constexpr size_t MgrFaceGen = 0x31E8;  // manager: current request's FaceGenData
    constexpr size_t MgrState   = 0x34A0;  // manager: 0 idle, 1 loading, 2 merged, 3 other path
}

using GateFn     = char (*)(void* npc);
using HeadNifFn  = uint8_t (*)(void*, void*, void*);
using RequestFn  = void (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
using StartFn    = bool (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
using MergeFn    = void (*)(uint64_t, uint64_t);
using BlockingFn = char (*)(uint64_t, uint64_t, uint64_t, uint64_t);
using UpdateFn   = int (*)(void* mgr, uint64_t p2);
using GetFileFn  = void* (*)(void* form, int index);
using IsMasterFn = char (*)(void* file);
using SetFn      = char (*)(void* set, void* npc, uint32_t flags);
using PathFn     = char (*)(void* npc, char* out);
using ExistsFn   = char (*)(const char* path);
using ReadyFn    = char (*)(void* faceGenData);
using HasKwFn    = char (*)(void* keywordForm, void* keyword, void* unused);
using DeleteFn   = void (*)(void* self);

static void* g_tgt[kTgtCount];
static void* g_aux[kAuxCount];
static uintptr_t g_siteRet[kSiteCount];

// ---------------------------------------------------------------- settings
static FILE* g_log;
static LARGE_INTEGER g_qpf;
static volatile bool g_on = false;
static int  g_toggleKey = 0x75;
static bool g_logEvery = false;
static bool g_gateOn = true, g_allowNonMasters = false;
static bool g_netOn = true, g_forceRetryToo = false;
static int  g_forceFailEvery = 0;
static bool g_queueOn = true, g_poolOn = false;
static int  g_poolMinFreeMB = 1024;               // VRAM guard: below this much free budget the pool holds nothing
static volatile bool g_poolVramLow;
static int  g_poolReleaseMarginMB = 768;          // guard releases only above limit + margin (more than the pool's own size)
static int  g_poolHoldSec = 30;                   // and only after it has been engaged this long
static ULONGLONG g_poolLowSince;                  // tick when the guard last engaged
static int  g_poolEngages;                        // engagements since start (logged)
static int  g_maxSteps = 8;
static double g_budgetMs = 2.0;
static bool g_logVram = true, g_verifyHash = false;
static uint32_t g_force[256];                     // Part A force list: these NPCs use pre-made FaceGen even with A off
static int g_nForce;
static uint32_t g_exceptions[256];
static int g_nExceptions;
static volatile LONG g_toggleReq;

// ---------------------------------------------------------------- stats
static volatile LONG64 g_gateCalls, g_gateYes, g_gateNo, g_npcYes, g_npcNo, g_gateSwitched, g_headDropped, g_requests,
                       g_starts, g_aborts, g_retryOk, g_retryFail, g_retrySkipped, g_forced, g_merges, g_asyncLayers,
                       g_cacheLayers, g_lostLayers, g_lostMerges, g_blockCalls, g_blockMaxUs, g_paintMaxUs, g_paintSumUs,
                       g_paintCount, g_extraUpdates, g_poolHandOuts, g_poolMisfits, g_poolResets, g_startCounter;

static void Log(const char* fmt, ...) {
    if (!g_log) return;
    SYSTEMTIME t; GetLocalTime(&t);
    char buf[1024];
    int n = snprintf(buf, sizeof(buf), "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list a; va_start(a, fmt); n += vsnprintf(buf + n, sizeof(buf) - n, fmt, a); va_end(a);
    if (n > (int)sizeof(buf) - 2) n = sizeof(buf) - 2;
    buf[n++] = '\n'; buf[n] = 0;
    fputs(buf, g_log); fflush(g_log);
}

static uint64_t Now() { LARGE_INTEGER c; QueryPerformanceCounter(&c); return c.QuadPart; }
static uint64_t Us(uint64_t a, uint64_t b) { return (b - a) * 1000000ull / g_qpf.QuadPart; }
static void Max64(volatile LONG64* m, LONG64 v) {
    LONG64 cur = *m;
    while (v > cur) { LONG64 seen = InterlockedCompareExchange64(m, v, cur); if (seen == cur) break; cur = seen; }
}

// ---------------------------------------------------------------- D: VRAM and queue depth
static IDXGIAdapter3* g_adapter;
static bool g_adapterTried;

static void Vram(char* out, size_t n) {
    if (!g_adapterTried) {
        g_adapterTried = true;
        __try {
            ID3D11Device* dev = *static_cast<ID3D11Device**>(g_aux[kDevice]);
            IDXGIDevice* dx = nullptr;
            if (dev && SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dx))) && dx) {
                IDXGIAdapter* ad = nullptr;
                if (SUCCEEDED(dx->GetAdapter(&ad)) && ad) {
                    ad->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&g_adapter));
                    ad->Release();
                }
                dx->Release();
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { g_adapter = nullptr; }
    }
    DXGI_QUERY_VIDEO_MEMORY_INFO mi = {};
    if (g_adapter && SUCCEEDED(g_adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &mi)))
        snprintf(out, n, "VRAM %llu / %llu MB", mi.CurrentUsage >> 20, mi.Budget >> 20);
    else
        snprintf(out, n, "VRAM n/a");
}

static void* Manager() { return *static_cast<void**>(g_aux[kManager]); }

// Free video memory in MB (budget - usage), or -1 if unknown. Cheap (DXGI keeps these counters).
static long long VramFreeMB() {
    char dummy[64];
    if (!g_adapterTried) Vram(dummy, sizeof(dummy));
    DXGI_QUERY_VIDEO_MEMORY_INFO mi = {};
    if (!g_adapter || FAILED(g_adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &mi))) return -1;
    return mi.Budget > mi.CurrentUsage ? (long long)((mi.Budget - mi.CurrentUsage) >> 20) : 0;
}

static int QueueDepth(uint8_t* mgr) {
    int n = -1;
    __try {
        uint8_t* ring = *reinterpret_cast<uint8_t**>(mgr + off::MgrRing);
        uint32_t head = *reinterpret_cast<uint32_t*>(ring + 0x2400) & 0xFF, tail = *reinterpret_cast<uint32_t*>(ring + 0x2500) & 0xFF;
        n = int((tail - head) & 0xFF) + *reinterpret_cast<int32_t*>(mgr + off::MgrOverflow);
    } __except (EXCEPTION_EXECUTE_HANDLER) { n = -1; }
    return n;
}

// ---------------------------------------------------------------- guarded reads
struct NpcInfo { uint32_t formID, rootID; uint8_t* root; char winner[64]; int master, index, chain; bool ok; };

static void ReadNpc(void* npc, NpcInfo* o) {
    __try {
        o->formID = *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(npc) + off::FormID);
        uint8_t* root = static_cast<uint8_t*>(npc);
        int chain = 0;
        while (chain < 16 && *reinterpret_cast<uint8_t**>(root + off::FaceNPC)) { root = *reinterpret_cast<uint8_t**>(root + off::FaceNPC); ++chain; }
        o->chain = chain; o->root = root;
        o->rootID = *reinterpret_cast<uint32_t*>(root + off::FormID);
        uint8_t* f = static_cast<uint8_t*>(reinterpret_cast<GetFileFn>(g_aux[kGetFile])(root, -1));
        if (f) {
            strncpy_s(o->winner, reinterpret_cast<const char*>(f + off::FileName), _TRUNCATE);
            o->master = (*reinterpret_cast<uint32_t*>(f + off::FileFlags) & 1) != 0;
            o->index = *reinterpret_cast<uint8_t*>(f + off::FileIndex);
        } else {
            strcpy_s(o->winner, "(none)"); o->master = -1; o->index = -1;
        }
        o->ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        o->ok = false;
    }
}

struct ArrStats { int arrays, gpuNull, total, async, loaded, notSub, inFlight; bool ok; };

static void ReadArrays(uint64_t faceGenData, ArrStats* s) {
    memset(s, 0, sizeof(*s));
    __try {
        uint8_t* fd = reinterpret_cast<uint8_t*>(faceGenData);
        for (int i = 0; i < 4; ++i) {
            uint8_t* a = *reinterpret_cast<uint8_t**>(fd + off::Arrays + 8 * i);
            if (!a) continue;
            ++s->arrays;
            if (!*reinterpret_cast<void**>(a + off::ArrGpu)) ++s->gpuNull;
            int t = *reinterpret_cast<int32_t*>(a + off::ArrTotal), as = *reinterpret_cast<int32_t*>(a + off::ArrAsync),
                l = *reinterpret_cast<int32_t*>(a + off::ArrLoaded), ns = *reinterpret_cast<int32_t*>(a + off::ArrNotSub);
            s->total += t; s->async += as; s->loaded += l; s->notSub += ns;
            s->inFlight += t - as - l - ns;
        }
        s->ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        s->ok = false;
    }
}

static uint32_t RequestActorID(uint8_t* mgr) {
    uint32_t id = 0;
    __try {
        uint8_t* ref = *reinterpret_cast<uint8_t**>(mgr + off::MgrRequest);
        if (ref) id = *reinterpret_cast<uint32_t*>(ref + off::FormID);
    } __except (EXCEPTION_EXECUTE_HANDLER) { id = 0; }
    return id;
}

// ---------------------------------------------------------------- small locked sets
static SRWLOCK g_lock = SRWLOCK_INIT;
constexpr int kSeen = 1 << 14;
static uint32_t g_seen[kSeen];                    // NPC root FormIDs already logged (0 = empty)
static int g_seenCount;
struct GateCache { uintptr_t root; uint32_t id; int8_t result; };
static GateCache g_gateCache[kSeen];              // Part A decision per root TESNPC* (open addressing)
static int g_gateCacheCount;

static bool FirstTime(uint32_t id) {
    if (!id) return false;
    AcquireSRWLockExclusive(&g_lock);
    uint32_t h = (id * 2654435761u) & (kSeen - 1);
    bool first = false;
    for (int n = 0; n < kSeen; ++n, h = (h + 1) & (kSeen - 1)) {
        if (g_seen[h] == id) break;
        if (!g_seen[h]) { if (g_seenCount < kSeen * 3 / 4) { g_seen[h] = id; ++g_seenCount; first = true; } break; }
    }
    ReleaseSRWLockExclusive(&g_lock);
    return first;
}

static int GateCacheGet(uintptr_t root, uint32_t id) {
    int r = -1;
    if (id >= 0xFF000000u) return -1;                 // temporary forms (leveled actors) are freed and reused: never cached
    AcquireSRWLockShared(&g_lock);
    uint32_t h = uint32_t((root >> 4) * 2654435761u) & (kSeen - 1);
    for (int n = 0; n < kSeen; ++n, h = (h + 1) & (kSeen - 1)) {
        if (g_gateCache[h].root == root) { if (g_gateCache[h].id == id) r = g_gateCache[h].result; break; }
        if (!g_gateCache[h].root) break;
    }
    ReleaseSRWLockShared(&g_lock);
    return r;
}

static void GateCachePut(uintptr_t root, uint32_t id, int8_t v) {
    if (id >= 0xFF000000u) return;
    AcquireSRWLockExclusive(&g_lock);
    uint32_t h = uint32_t((root >> 4) * 2654435761u) & (kSeen - 1);
    for (int n = 0; n < kSeen && g_gateCacheCount < kSeen * 3 / 4; ++n, h = (h + 1) & (kSeen - 1)) {
        if (g_gateCache[h].root == root) { g_gateCache[h].id = id; g_gateCache[h].result = v; break; }
        if (!g_gateCache[h].root) { g_gateCache[h] = { root, id, v }; ++g_gateCacheCount; break; }
    }
    ReleaseSRWLockExclusive(&g_lock);
}

// paint start time per FaceGenData
struct StartRec { uint64_t fd, t; };
static StartRec g_startRec[4];
static void SetStart(uint64_t fd, uint64_t t) {
    AcquireSRWLockExclusive(&g_lock);
    int slot = 0;
    for (int i = 0; i < 4; ++i) if (g_startRec[i].fd == fd || !g_startRec[i].fd) { slot = i; break; }
    g_startRec[slot] = { fd, t };
    ReleaseSRWLockExclusive(&g_lock);
}
static uint64_t TakeStart(uint64_t fd) {
    AcquireSRWLockExclusive(&g_lock);
    uint64_t t = 0;
    for (int i = 0; i < 4; ++i) if (g_startRec[i].fd == fd) { t = g_startRec[i].t; g_startRec[i].t = 0; break; }
    ReleaseSRWLockExclusive(&g_lock);
    return t;
}

// ---------------------------------------------------------------- Part A: the gate
// Vanilla 0x006DF3A0: eligible (not the player, not the chargen dummies, no child-player keyword, not in the set) AND
// winning file master AND (compile index 0 or an official Bethesda file). We drop only the official-file clause and
// require the pre-made FaceGeom NIF to exist instead.
static bool Forced(uint32_t rootID);

static int NewGate(void* npc, NpcInfo* n, const char** reason) {
    __try {
        uint8_t* root = n->root;
        if (n->rootID == 7) { *reason = "player"; return 0; }
        if (root == *static_cast<uint8_t**>(g_aux[kDummy1]) || root == *static_cast<uint8_t**>(g_aux[kDummy2])) { *reason = "chargen dummy"; return 0; }
        void* kwForm = root + off::KeywordForm;
        HasKwFn hasKw = reinterpret_cast<HasKwFn>((*static_cast<void***>(kwForm))[1]);
        if (hasKw(kwForm, *static_cast<void**>(g_aux[kChildKw]), nullptr)) { *reason = "child-player keyword"; return 0; }
        if (reinterpret_cast<SetFn>(g_aux[kSetLookup])(*static_cast<void**>(g_aux[kSet]), root, 0x800)) { *reason = "in gate set"; return 0; }
        for (int i = 0; i < g_nExceptions; ++i) if (g_exceptions[i] == n->rootID) { *reason = "INI exception"; return 0; }
        void* f = reinterpret_cast<GetFileFn>(g_aux[kGetFile])(root, -1);
        if (!f) { *reason = "no winning file"; return 0; }
        const bool forced = Forced(n->rootID);
        if (!forced && !g_allowNonMasters && !reinterpret_cast<IsMasterFn>(g_aux[kIsMaster])(f)) { *reason = "winner not a master"; return 0; }
        char path[MAX_PATH] = {};
        if (!reinterpret_cast<PathFn>(g_aux[kFaceGeomPath])(npc, path) || !reinterpret_cast<ExistsFn>(g_aux[kMeshExists])(path)) {
            *reason = "no FaceGeom NIF"; return 0;
        }
        *reason = forced ? "FaceGen files exist (INI force list)" : "FaceGen files exist";
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *reason = "unreadable"; return 0;
    }
}

static bool Forced(uint32_t rootID) {
    for (int i = 0; i < g_nForce; ++i) if (g_force[i] == rootID) return true;
    return false;
}

static char GateStub(void* npc) {
    char r = reinterpret_cast<GateFn>(g_tgt[kGate])(npc);
    InterlockedIncrement64(&g_gateCalls);
    NpcInfo n = {};
    if (npc) ReadNpc(npc, &n);
    const char* reason = "";
    bool switched = false;
    const bool forcedNpc = n.ok && Forced(n.rootID);
    if (!r && g_on && n.ok && (g_gateOn || forcedNpc)) {
        int c = GateCacheGet(reinterpret_cast<uintptr_t>(n.root), n.rootID);
        if (c < 0) { c = NewGate(npc, &n, &reason); GateCachePut(reinterpret_cast<uintptr_t>(n.root), n.rootID, int8_t(c)); }
        else reason = c ? "FaceGen files exist (cached)" : "(cached)";
        if (c == 1) { r = 1; switched = true; InterlockedIncrement64(&g_gateSwitched); }
    }
    InterlockedIncrement64(r ? &g_gateYes : &g_gateNo);
    if (n.ok && (g_logEvery || FirstTime(n.rootID))) {
        if (!g_logEvery) InterlockedIncrement64(r ? &g_npcYes : &g_npcNo);
        Log("P1 gate %s%s npc %08X root %08X (chain %d) winner '%s' master %d index %d%s%s thread %lu", r ? "YES" : "no ",
            switched ? " (A: switched to pre-made FaceGen)" : "", n.formID, n.rootID, n.chain, n.winner, n.master, n.index,
            *reason ? " | A: " : "", reason, GetCurrentThreadId());
    }
    return r;
}

static uint8_t HeadNifStub(void* a, void* b, void* c) {
    uint8_t r = reinterpret_cast<HeadNifFn>(g_tgt[kHeadNif])(a, b, c);
    if (!r) {
        InterlockedIncrement64(&g_headDropped);
        Log("P2 HEAD DROPPED: FaceGeom NIF load failed after gate=yes (args %p %p %p)", a, b, c);
    }
    return r;
}

static void RequestStub(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7,
                        uint64_t a8, uint64_t a9, uint64_t a10) {
    InterlockedIncrement64(&g_requests);
    reinterpret_cast<RequestFn>(g_tgt[kRequest])(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10);
}

// ---------------------------------------------------------------- Part C: texture-array pool (update path only)
// Runs inside BSFaceGenManager::Update, which holds the manager's global spin lock, so the pool needs no lock.
static uint8_t* g_pool[4];

static void ArrAddRef(uint8_t* a) { InterlockedIncrement(reinterpret_cast<volatile LONG*>(a + off::ArrRef)); }
static void ArrRelease(uint8_t* a) {
    if (InterlockedDecrement(reinterpret_cast<volatile LONG*>(a + off::ArrRef)) == 0)
        reinterpret_cast<DeleteFn>((*reinterpret_cast<void***>(a))[1])(a);
}

// Before Start: empty array slots get the pooled arrays, marked with total = -1. Start's Load resets total for every
// array it uses; after Start, an array still marked was not used by this face and is taken back, because the GPU merge
// reads every non-empty slot and would otherwise mix in the previous face's layers.
static void PoolGive(uint64_t fd, bool* given) {
    __try {
        uint8_t** arr = reinterpret_cast<uint8_t**>(fd + off::Arrays);
        bool any = false;
        for (int i = 0; i < 4; ++i) {
            given[i] = false;
            if (!arr[i] && g_pool[i]) {
                ArrAddRef(g_pool[i]);
                *reinterpret_cast<int32_t*>(g_pool[i] + off::ArrTotal) = -1;
                arr[i] = g_pool[i]; given[i] = any = true;
            }
        }
        if (any) InterlockedIncrement64(&g_poolHandOuts);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Prepare the request's slot allocator for pooled arrays (decompiled/bf11): 0x006E99E0 finds a slot whose arrays already
// hold a layer; 0x006E9C50 hands out the slot with the oldest stamp. With a fresh FaceGenData every stamp is 0, so new
// layers were given slots that held cached layers this face needed later -> collisions, shorter layer lists. Stamp every
// occupied slot as in use (1) and every free slot 0, so new layers always land in free slots. Returns the number of
// occupied slots, or -1 if unreadable.
static int PoolPrepareStamps(uint64_t fd) {
    int occupied = 0;
    __try {
        uint8_t* f = reinterpret_cast<uint8_t*>(fd);
        uint8_t** arr = reinterpret_cast<uint8_t**>(f + off::Arrays);
        uint32_t* stamps = reinterpret_cast<uint32_t*>(f + off::FdStamps);
        memset(stamps, 0, 128 * sizeof(uint32_t));
        *reinterpret_cast<uint32_t*>(f + off::FdGen) = 0;         // Start makes it 1
        for (int slot = 0; slot < 128; ++slot) {
            bool used = false;
            for (int i = 0; i < 4 && !used; ++i) {
                uint8_t* a = arr[i];
                if (!a || uint32_t(slot) >= *reinterpret_cast<uint32_t*>(a + off::ArrSlotCnt)) continue;
                const uint8_t* e = *reinterpret_cast<uint8_t**>(a + off::ArrSlots) + slot * 12;
                used = *reinterpret_cast<const uint64_t*>(e) != 0 || *reinterpret_cast<const uint32_t*>(e + 8) != 0;
            }
            if (used) { stamps[slot] = 1; ++occupied; }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { occupied = -1; }
    return occupied;
}

static void FdReset(uint64_t fd) {                        // the request's allocator back to its fresh state
    __try {
        uint8_t* f = reinterpret_cast<uint8_t*>(fd);
        memset(f + off::FdStamps, 0, 128 * sizeof(uint32_t));
        *reinterpret_cast<uint32_t*>(f + off::FdGen) = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void FdDetachAll(uint64_t fd) {
    __try {
        uint8_t** arr = reinterpret_cast<uint8_t**>(fd + off::Arrays);
        for (int i = 0; i < 4; ++i) if (arr[i]) { ArrRelease(arr[i]); arr[i] = nullptr; }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void PoolReclaimUnused(uint64_t fd, const bool* given) {
    __try {
        uint8_t** arr = reinterpret_cast<uint8_t**>(fd + off::Arrays);
        for (int i = 0; i < 4; ++i) {
            if (!given[i] || arr[i] != g_pool[i] || !arr[i]) continue;
            int32_t* c = reinterpret_cast<int32_t*>(arr[i] + off::ArrTotal);
            if (c[0] != -1) continue;                     // used by this face
            c[0] = c[1] = c[2] = c[3] = 0;                // total/async/loaded/notSub: consistent "nothing loaded"
            arr[i] = nullptr;
            ArrRelease(g_pool[i]);                        // drop the request's reference; the pool keeps its own
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void PoolTake(uint64_t fd) {                       // after a clean merge: remember this request's arrays
    __try {
        uint8_t** arr = reinterpret_cast<uint8_t**>(fd + off::Arrays);
        for (int i = 0; i < 4; ++i) {
            if (!arr[i] || arr[i] == g_pool[i]) continue;
            if (*reinterpret_cast<uint32_t*>(arr[i] + off::ArrSlices) < 0x80) continue;   // only 128-slice (cache-mode) arrays
            ArrAddRef(arr[i]);
            if (g_pool[i]) ArrRelease(g_pool[i]);
            g_pool[i] = arr[i];
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void PoolDrop() {
    for (int i = 0; i < 4; ++i) if (g_pool[i]) { ArrRelease(g_pool[i]); g_pool[i] = nullptr; }
}

static int SiteOf(uintptr_t ret) {
    for (int i = 0; i < kSiteCount; ++i) if (g_siteRet[i] == ret) return i;
    return -1;
}

// ---------------------------------------------------------------- Part B: start with retry
static bool StartStub(uint64_t a1, uint64_t a2, uint64_t fd, uint64_t a4, uint64_t a5, uint64_t a6) {
    const int site = SiteOf(reinterpret_cast<uintptr_t>(_ReturnAddress()));
    const bool updatePath = site == kUpdate1 || site == kUpdate2;
    const bool active = g_on;
    bool given[4] = {};
    bool vramLow = false;
    if (active && g_poolOn && updatePath && g_poolMinFreeMB > 0) {
        // Hysteresis: engage at once below the limit; release only when free memory is back above limit + margin AND
        // the guard has held for the hold time. Dropping the pool frees ~0.4 GB by itself, so without the margin the
        // guard saw that as "recovered", pooled again and flipped several times a second (1.0.0 release soak).
        const long long freeMB = VramFreeMB();
        const ULONGLONG now = GetTickCount64();
        vramLow = g_poolVramLow;
        if (freeMB >= 0) {
            if (!vramLow && freeMB < g_poolMinFreeMB) vramLow = true;
            else if (vramLow && freeMB >= (long long)g_poolMinFreeMB + g_poolReleaseMarginMB &&
                     now - g_poolLowSince >= ULONGLONG(g_poolHoldSec) * 1000) vramLow = false;
        }
        if (vramLow != g_poolVramLow) {
            g_poolVramLow = vramLow;
            if (vramLow) {
                g_poolLowSince = now;
                Log("C: VRAM guard ENGAGED #%d (free %lld MB, limit %d MB): pooled arrays released, faces painted the vanilla "
                    "way until free memory is above %d MB for %d s", ++g_poolEngages, freeMB, g_poolMinFreeMB,
                    g_poolMinFreeMB + g_poolReleaseMarginMB, g_poolHoldSec);
            } else {
                Log("C: VRAM guard released after %llu s (free %lld MB, release above %d MB): pooling again",
                    (unsigned long long)((now - g_poolLowSince) / 1000), freeMB, g_poolMinFreeMB + g_poolReleaseMarginMB);
            }
        }
        if (vramLow) PoolDrop();
    }
    const bool pooled = active && g_poolOn && updatePath && !vramLow;
    if (pooled) {
        a6 = 1;                                           // engine cache mode: 128-slice arrays, so any slot index is valid
        PoolGive(fd, given);
        const int occ = PoolPrepareStamps(fd);
        if (occ < 0 || occ > 64) {                        // keep >= 64 free slots (a face needs <= ~60): else start over
            FdDetachAll(fd); PoolDrop(); FdReset(fd);
            for (bool& g : given) g = false;
            InterlockedIncrement64(&g_poolResets);
        }
    }

    const uint64_t t = Now();
    bool forced = false;
    if (active && g_netOn && g_forceFailEvery > 0 && (InterlockedIncrement64(&g_startCounter) % g_forceFailEvery) == 0) {
        forced = true;
        InterlockedIncrement64(&g_forced);
    }
    bool r = forced ? false : reinterpret_cast<StartFn>(g_tgt[kStart])(a1, a2, fd, a4, a5, a6);
    if (pooled) PoolReclaimUnused(fd, given);
    if (!r && !forced && pooled && (given[0] || given[1] || given[2] || given[3])) {
        // Pooled arrays did not fit this face (size/format/mips differ, e.g. a different NPC type): detach every array
        // from this request, drop the pool and start again with fresh arrays. Loads still in flight keep their own
        // reference to their array (0x017C6970), so releasing ours is safe.
        FdDetachAll(fd);
        PoolDrop();
        FdReset(fd);                                      // stale stamps from the failed attempt put layers past the slices
        r = reinterpret_cast<StartFn>(g_tgt[kStart])(a1, a2, fd, a4, a5, a6);
        InterlockedIncrement64(&g_poolMisfits);
        Log("C: pooled arrays did not fit this face (npc ref %08X); restarted with fresh arrays: %s",
            RequestActorID(static_cast<uint8_t*>(Manager())), r ? "OK" : "failed");
    }
    InterlockedIncrement64(&g_starts);
    uint8_t* mgr = static_cast<uint8_t*>(Manager());
    if (!r) {
        InterlockedIncrement64(&g_aborts);
        ArrStats s; ReadArrays(fd, &s);
        char vram[64]; Vram(vram, sizeof(vram));
        Log("P4 START ABORTED%s: npc ref %08X arrays %d gpuNull %d layers total %d asyncDone %d loaded %d notSubmitted %d "
            "inFlight %d | queue %d | %s | site %s thread %lu", forced ? " (forced test failure)" : "",
            updatePath && mgr ? RequestActorID(mgr) : 0u, s.arrays, s.gpuNull, s.total, s.async, s.loaded, s.notSub,
            s.inFlight, mgr ? QueueDepth(mgr) : -1, vram, site >= 0 ? kSites[site].name : "?", GetCurrentThreadId());
        if (active && g_netOn) {
            if (s.ok && s.inFlight == 0) {
                bool r2 = (forced && g_forceRetryToo) ? false : reinterpret_cast<StartFn>(g_tgt[kStart])(a1, a2, fd, a4, a5, a6);
                if (r2) {
                    InterlockedIncrement64(&g_retryOk);
                    Log("B: retry succeeded");
                    r = true;
                } else {
                    InterlockedIncrement64(&g_retryFail);
                    Log("B: retry failed too; the engine drops the request and the face keeps its plain unpainted look");
                }
            } else {
                InterlockedIncrement64(&g_retrySkipped);
                Log("B: no retry (%s); the face keeps its plain unpainted look", s.ok ? "async loads still in flight" : "unreadable");
            }
        }
    }
    if (r) SetStart(fd, t);
    return r;
}

// ---------------------------------------------------------------- verify mode: hash the painted face textures
// After a merge, copy the face render targets (RenderTargetManager slots 0x10 diffuse, 0x11 normals) to a staging texture
// and hash the pixels. Identical NPCs must give identical hashes, with or without the array pool.
static ID3D11Texture2D* g_stage[2];
static D3D11_TEXTURE2D_DESC g_stageDesc[2];

static UINT RowBytes(DXGI_FORMAT f, UINT w) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R16G16_UNORM: case DXGI_FORMAT_R16G16_FLOAT: case DXGI_FORMAT_R32_FLOAT: case DXGI_FORMAT_B8G8R8X8_UNORM:
        return w * 4;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_UNORM: return w * 8;
    case DXGI_FORMAT_R8G8_UNORM: case DXGI_FORMAT_R16_FLOAT: return w * 2;
    case DXGI_FORMAT_R8_UNORM: return w;
    default: return 0;
    }
}

static uint64_t HashTarget(int slot, int k, UINT* wOut) {
    *wOut = 0;
    ID3D11Device* dev = *static_cast<ID3D11Device**>(g_aux[kDevice]);
    ID3D11DeviceContext* ctx = *static_cast<ID3D11DeviceContext**>(g_aux[kImmediate]);
    const uint32_t idx = *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(g_aux[kRTM]) + 0xDF4 + slot * 4);
    if (!dev || !ctx || idx >= 101) return 0;
    ID3D11Texture2D* tex = *reinterpret_cast<ID3D11Texture2D**>(static_cast<uint8_t*>(g_aux[kRendererObj]) + 0x10 + 0xA58 + idx * 0x30);
    if (!tex) return 0;
    D3D11_TEXTURE2D_DESC d; tex->GetDesc(&d);
    D3D11_TEXTURE2D_DESC sd = d;
    sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
    if (!g_stage[k] || memcmp(&g_stageDesc[k], &sd, sizeof(sd)) != 0) {
        if (g_stage[k]) { g_stage[k]->Release(); g_stage[k] = nullptr; }
        if (FAILED(dev->CreateTexture2D(&sd, nullptr, &g_stage[k]))) return 0;
        g_stageDesc[k] = sd;
    }
    ctx->CopyResource(g_stage[k], tex);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(g_stage[k], 0, D3D11_MAP_READ, 0, &m))) return 0;
    UINT rb = RowBytes(d.Format, d.Width);
    if (!rb || rb > m.RowPitch) rb = m.RowPitch;
    uint64_t h = 1469598103934665603ull;                  // FNV-1a 64
    const uint8_t* row = static_cast<const uint8_t*>(m.pData);
    for (UINT y = 0; y < d.Height; ++y, row += m.RowPitch)
        for (UINT x = 0; x < rb; ++x) { h ^= row[x]; h *= 1099511628211ull; }
    ctx->Unmap(g_stage[k], 0);
    *wOut = d.Width;
    return h;
}

static uint32_t RequestBaseID(uint8_t* mgr) {
    uint32_t id = 0;
    __try {
        uint8_t* ref = *reinterpret_cast<uint8_t**>(mgr + off::MgrRequest);
        uint8_t* base = ref ? *reinterpret_cast<uint8_t**>(ref + 0xE0) : nullptr;  // TESObjectREFR::data.objectReference
        if (base) id = *reinterpret_cast<uint32_t*>(base + off::FormID);
    } __except (EXCEPTION_EXECUTE_HANDLER) { id = 0; }
    return id;
}

static void MergeStub(uint64_t fd, uint64_t target) {
    const int site = SiteOf(reinterpret_cast<uintptr_t>(_ReturnAddress()));
    ArrStats s; ReadArrays(fd, &s);
    const uint64_t t0 = TakeStart(fd), now = Now();
    InterlockedIncrement64(&g_merges);
    if (t0) {
        LONG64 us = LONG64(Us(t0, now));
        InterlockedAdd64(&g_paintSumUs, us); InterlockedIncrement64(&g_paintCount); Max64(&g_paintMaxUs, us);
    }
    if (s.ok) { InterlockedAdd64(&g_asyncLayers, s.async); InterlockedAdd64(&g_cacheLayers, s.loaded); }
    const bool lost = s.ok && (s.notSub || s.gpuNull);
    if (lost) {
        InterlockedAdd64(&g_lostLayers, s.notSub); InterlockedIncrement64(&g_lostMerges);
        char vram[64]; Vram(vram, sizeof(vram));
        uint8_t* mgr = static_cast<uint8_t*>(Manager());
        Log("P5 MERGE WITH LOST LAYERS: npc ref %08X arrays %d gpuNull %d layers total %d asyncDone %d loaded %d "
            "notSubmitted %d (start->merge %llu us) | %s%s", site == kMergeUpdate && mgr ? RequestActorID(mgr) : 0u,
            s.arrays, s.gpuNull, s.total, s.async, s.loaded, s.notSub, t0 ? (unsigned long long)Us(t0, now) : 0ull, vram,
            g_poolOn ? " | C: array pool switched OFF" : "");
        if (g_poolOn) { g_poolOn = false; if (site == kMergeUpdate) PoolDrop(); }
    }
    reinterpret_cast<MergeFn>(g_tgt[kMerge])(fd, target);
    if (g_verifyHash && site == kMergeUpdate) {
        UINT w0 = 0, w1 = 0;
        uint64_t hd = 0, hn = 0;
        __try { hd = HashTarget(0x10, 0, &w0); hn = HashTarget(0x11, 1, &w1); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        uint8_t* mgr = static_cast<uint8_t*>(Manager());
        Log("VERIFY face npc base %08X ref %08X diffuse %016llX normals %016llX (%u px) | pool %s, cache hits %d of %d layers",
            mgr ? RequestBaseID(mgr) : 0u, mgr ? RequestActorID(mgr) : 0u, (unsigned long long)hd, (unsigned long long)hn, w0,
            (g_on && g_poolOn) ? "on" : "off", s.loaded, s.total);
    }
    if (g_on && g_poolOn && !g_poolVramLow && !lost && site == kMergeUpdate) PoolTake(fd);
}

static char BlockingStub(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    const uint64_t t = Now();
    char r = reinterpret_cast<BlockingFn>(g_tgt[kBlocking])(a1, a2, a3, a4);
    const LONG64 us = LONG64(Us(t, Now()));
    InterlockedIncrement64(&g_blockCalls); Max64(&g_blockMaxUs, us);
    if (us > 2000) Log("P6 blocking face merge stalled the calling thread %.1f ms (thread %lu)", us / 1000.0, GetCurrentThreadId());
    return r;
}

// ---------------------------------------------------------------- Part C: more Update steps per frame
static int UpdateStub(void* mgr, uint64_t p2) {
    if (g_toggleReq) {                                    // apply hotkey requests here, on the frame (main) thread
        InterlockedExchange(&g_toggleReq, 0);
        g_on = !g_on;
        Log("toggle: FaceGen optimizer %s (gate change applies to heads built from now on)", g_on ? "ON" : "OFF");
    }
    int r = reinterpret_cast<UpdateFn>(g_tgt[kUpdate])(mgr, p2);
    if (!g_on || !g_queueOn || !mgr) return r;
    const uint64_t t0 = Now();
    const uint64_t budget = uint64_t(g_budgetMs * double(g_qpf.QuadPart) / 1000.0);
    uint8_t* m = static_cast<uint8_t*>(mgr);
    for (int i = 1; i < g_maxSteps && Now() - t0 < budget; ++i) {
        bool more = false;
        __try {
            const int state = *reinterpret_cast<int32_t*>(m + off::MgrState);
            if (state == 1) {
                void* fd = *reinterpret_cast<void**>(m + off::MgrFaceGen);
                more = fd && reinterpret_cast<ReadyFn>(g_aux[kReady])(fd);
            } else if (state == 0) {
                more = QueueDepth(m) > 0;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { more = false; }
        if (!more) break;
        reinterpret_cast<UpdateFn>(g_tgt[kUpdate])(mgr, p2);
        InterlockedIncrement64(&g_extraUpdates);
    }
    return r;
}

static void* const kStubs[kTgtCount] = { reinterpret_cast<void*>(&GateStub), reinterpret_cast<void*>(&HeadNifStub),
                                         reinterpret_cast<void*>(&RequestStub), reinterpret_cast<void*>(&StartStub),
                                         reinterpret_cast<void*>(&MergeStub), reinterpret_cast<void*>(&BlockingStub),
                                         reinterpret_cast<void*>(&UpdateStub) };

// ---------------------------------------------------------------- key + stats thread
static DWORD WINAPI StatsThread(void*) {
    ULONGLONG last = GetTickCount64();
    bool wasDown = false;
    for (;;) {
        Sleep(10);
        SHORT st = g_toggleKey ? GetAsyncKeyState(g_toggleKey) : 0;
        bool down = (st & 0x8000) != 0;
        if ((st & 0x0001) || (down && !wasDown)) InterlockedExchange(&g_toggleReq, 1);
        wasDown = down;
        ULONGLONG now = GetTickCount64();
        if (now - last < 10000) continue;
        last = now;
        LONG64 calls = InterlockedExchange64(&g_gateCalls, 0), yes = InterlockedExchange64(&g_gateYes, 0),
               no = InterlockedExchange64(&g_gateNo, 0), sw = InterlockedExchange64(&g_gateSwitched, 0),
               req = InterlockedExchange64(&g_requests, 0), stc = InterlockedExchange64(&g_starts, 0),
               ab = InterlockedExchange64(&g_aborts, 0), rok = InterlockedExchange64(&g_retryOk, 0),
               rf = InterlockedExchange64(&g_retryFail, 0), rs = InterlockedExchange64(&g_retrySkipped, 0),
               fo = InterlockedExchange64(&g_forced, 0), mg = InterlockedExchange64(&g_merges, 0),
               al = InterlockedExchange64(&g_asyncLayers, 0), cl = InterlockedExchange64(&g_cacheLayers, 0),
               ll = InterlockedExchange64(&g_lostLayers, 0), lm = InterlockedExchange64(&g_lostMerges, 0),
               bc = InterlockedExchange64(&g_blockCalls, 0), bm = InterlockedExchange64(&g_blockMaxUs, 0),
               pm = InterlockedExchange64(&g_paintMaxUs, 0), ps = InterlockedExchange64(&g_paintSumUs, 0),
               pc = InterlockedExchange64(&g_paintCount, 0), hd = InterlockedExchange64(&g_headDropped, 0),
               eu = InterlockedExchange64(&g_extraUpdates, 0), ph = InterlockedExchange64(&g_poolHandOuts, 0),
               pmf = InterlockedExchange64(&g_poolMisfits, 0), prs = InterlockedExchange64(&g_poolResets, 0);
        if (!calls && !req && !stc && !mg && !bc && !hd) continue;
        char vram[64] = "VRAM off";
        if (g_logVram) Vram(vram, sizeof(vram));
        uint8_t* mgr = static_cast<uint8_t*>(Manager());
        Log("stats 10s (%s): gate calls %lld (yes %lld / no %lld, A switched %lld), NPCs seen yes %lld / no %lld | heads dropped %lld | "
            "paint requests %lld, starts %lld, ABORTED %lld (forced %lld; B retry ok %lld / failed %lld / skipped %lld), merges %lld "
            "(layers async %lld, cache hit %lld), LOST layers %lld in %lld merges | paint start->merge avg %.1f ms max %.1f ms | "
            "blocking %lld max %.1f ms | C extra updates %lld, pool hand-outs %lld, misfits %lld, resets %lld%s | queue %d | %s",
            g_on ? "ON" : "OFF", calls, yes, no, sw, (long long)g_npcYes, (long long)g_npcNo, hd, req, stc, ab, fo, rok, rf, rs,
            mg, al, cl, ll, lm, pc ? ps / 1000.0 / pc : 0.0, pm / 1000.0, bc, bm / 1000.0, eu, ph, pmf, prs, g_poolOn ? " (pool on)" : "",
            mgr ? QueueDepth(mgr) : -1, vram);
    }
}

// ---------------------------------------------------------------- install
static bool LoadOffsets(const char* path, const uint64_t* ids, uint64_t* offs, int n) {
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    uint64_t count = 0; DWORD got = 0;
    bool ok = ReadFile(f, &count, 8, &got, nullptr) && got == 8 && count < 10000000;
    for (int i = 0; i < n; ++i) offs[i] = 0;
    static uint64_t buf[2 * 4096];
    uint64_t left = ok ? count : 0;
    while (left) {
        DWORD want = DWORD((left < 4096 ? left : 4096) * 16);
        if (!ReadFile(f, buf, want, &got, nullptr) || got != want) { ok = false; break; }
        for (DWORD e = 0; e < want / 16; ++e)
            for (int i = 0; i < n; ++i)
                if (buf[2 * e] == ids[i]) offs[i] = buf[2 * e + 1];
        left -= want / 16;
    }
    CloseHandle(f);
    for (int i = 0; i < n; ++i) ok = ok && offs[i];
    return ok;
}

static void* AllocNear(uintptr_t target) {
    SYSTEM_INFO si; GetSystemInfo(&si);
    const uintptr_t gran = si.dwAllocationGranularity;
    for (uintptr_t a = (target & ~(gran - 1)) - gran; a > target - 0x7FF00000ull; a -= gran) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi))) break;
        if (mbi.State == MEM_FREE) {
            void* p = VirtualAlloc(reinterpret_cast<void*>(a), 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
            if (p) return p;
        }
    }
    return nullptr;
}

static bool Install(const char* exeDir, uint32_t rt) {
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\Data\\F4SE\\Plugins\\version-%u-%u-%u-%u.bin",
             exeDir, rt >> 24, (rt >> 16) & 0xFF, (rt >> 4) & 0xFFF, rt & 0xF);
    constexpr int kIds = kTgtCount + kAuxCount + kSiteCount;
    uint64_t ids[kIds], off[kIds];
    for (int i = 0; i < kTgtCount; ++i) ids[i] = kTgtId[i];
    for (int i = 0; i < kAuxCount; ++i) ids[kTgtCount + i] = kAuxId[i];
    for (int i = 0; i < kSiteCount; ++i) ids[kTgtCount + kAuxCount + i] = kSites[i].callerId;
    if (!LoadOffsets(path, ids, off, kIds)) { Log("ERROR: Address Library missing or incomplete: %s", path); return false; }

    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    for (int i = 0; i < kTgtCount; ++i) g_tgt[i] = reinterpret_cast<void*>(base + off[i]);
    for (int i = 0; i < kAuxCount; ++i) g_aux[i] = reinterpret_cast<void*>(base + off[kTgtCount + i]);

    uint8_t* sites[kSiteCount];
    for (int i = 0; i < kSiteCount; ++i) {
        uint8_t* s = reinterpret_cast<uint8_t*>(base + off[kTgtCount + kAuxCount + i] + kSites[i].offset);
        int32_t rel; memcpy(&rel, s + 1, 4);
        uintptr_t dest = reinterpret_cast<uintptr_t>(s) + 5 + rel;
        if (s[0] != 0xE8 || dest != reinterpret_cast<uintptr_t>(g_tgt[kSites[i].tgt])) {
            Log("ERROR: site %s/%s (RVA %08llX) is not the expected call (%02X -> %p); not installing", kTgtName[kSites[i].tgt],
                kSites[i].name, (unsigned long long)(reinterpret_cast<uintptr_t>(s) - base), s[0], reinterpret_cast<void*>(dest));
            return false;
        }
        sites[i] = s;
        g_siteRet[i] = reinterpret_cast<uintptr_t>(s) + 5;
    }

    uint8_t* slot = static_cast<uint8_t*>(AllocNear(base));
    if (!slot) { Log("ERROR: no memory within 2 GB of the exe for the jump slots"); return false; }
    const uint8_t jmp[6] = { 0xFF, 0x25, 0, 0, 0, 0 };
    for (int k = 0; k < kTgtCount; ++k) { memcpy(slot + 16 * k, jmp, 6); memcpy(slot + 16 * k + 6, &kStubs[k], 8); }
    FlushInstructionCache(GetCurrentProcess(), slot, 16 * kTgtCount);
    for (int i = 0; i < kSiteCount; ++i) {
        uint8_t* target = slot + 16 * kSites[i].tgt;
        int32_t newRel = int32_t(reinterpret_cast<intptr_t>(target) - (reinterpret_cast<intptr_t>(sites[i]) + 5));
        DWORD old;
        if (!VirtualProtect(sites[i], 5, PAGE_EXECUTE_READWRITE, &old)) { Log("ERROR: VirtualProtect failed"); return false; }
        memcpy(sites[i] + 1, &newRel, 4);
        VirtualProtect(sites[i], 5, old, &old);
        FlushInstructionCache(GetCurrentProcess(), sites[i], 5);
    }
    Log("installed: %d call sites verified and redirected (gate 6, head NIF 1, request 3, start 4, merge 2, blocking 2, update 1)",
        kSiteCount);
    return true;
}

static bool IniBool(const char* sec, const char* key, bool def, const char* ini) { return GetPrivateProfileIntA(sec, key, def ? 1 : 0, ini) != 0; }

extern "C" __declspec(dllexport) bool F4SEPlugin_Load(const F4SEInterface* f4se) {
    char docs[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_PERSONAL, nullptr, SHGFP_TYPE_CURRENT, docs))) {
        char lp[MAX_PATH];
        snprintf(lp, sizeof(lp), "%s\\My Games\\Fallout4\\F4SE\\FCStruct2.log", docs);
        g_log = _fsopen(lp, "w", _SH_DENYWR);
    }
    if (f4se->isEditor) return false;
    QueryPerformanceFrequency(&g_qpf);

    char exe[MAX_PATH]; GetModuleFileNameA(nullptr, exe, MAX_PATH);
    char* slash = strrchr(exe, '\\'); if (slash) *slash = 0;
    char ini[MAX_PATH]; snprintf(ini, sizeof(ini), "%s\\Data\\F4SE\\Plugins\\FCStruct2.ini", exe);
    bool enable       = IniBool("General", "bEnable", true, ini);
    g_on              = IniBool("General", "bStartOn", true, ini);
    g_toggleKey       = GetPrivateProfileIntA("General", "iToggleKey", 0x75, ini);
    g_logEvery        = IniBool("General", "bLogEveryGate", false, ini);
    g_gateOn          = IniBool("Gate", "bEnable", false, ini);
    g_allowNonMasters = IniBool("Gate", "bAllowNonMasters", false, ini);
    g_netOn           = IniBool("SafetyNet", "bEnable", true, ini);
    g_forceFailEvery  = GetPrivateProfileIntA("SafetyNet", "iForceFailEvery", 0, ini);
    g_forceRetryToo   = IniBool("SafetyNet", "bForceFailRetryToo", false, ini);
    g_queueOn         = IniBool("Queue", "bEnable", true, ini);
    g_maxSteps        = GetPrivateProfileIntA("Queue", "iMaxStepsPerFrame", 8, ini);
    g_poolOn          = IniBool("Queue", "bArrayPool", true, ini);   // 1.1.1 fix passed the pixel check
    g_poolMinFreeMB   = GetPrivateProfileIntA("Queue", "iPoolMinFreeVramMB", 1024, ini);
    g_poolReleaseMarginMB = GetPrivateProfileIntA("Queue", "iPoolReleaseMarginMB", 768, ini);
    g_poolHoldSec     = GetPrivateProfileIntA("Queue", "iPoolHoldSec", 30, ini);
    if (g_poolReleaseMarginMB < 0) g_poolReleaseMarginMB = 0;
    if (g_poolHoldSec < 0) g_poolHoldSec = 0;
    g_logVram         = IniBool("Diagnostics", "bLogVram", true, ini);
    g_verifyHash      = IniBool("Diagnostics", "bVerifyFaceHash", false, ini);
    char buf[2048];
    GetPrivateProfileStringA("Queue", "fBudgetMs", "2.0", buf, sizeof(buf), ini);
    g_budgetMs = atof(buf);
    if (g_maxSteps < 1) g_maxSteps = 1;
    if (g_budgetMs < 0.1) g_budgetMs = 0.1;
    GetPrivateProfileStringA("Gate", "sExceptions", "", buf, sizeof(buf), ini);
    char* ctx = nullptr;
    for (char* tok = strtok_s(buf, ", ", &ctx); tok && g_nExceptions < 256; tok = strtok_s(nullptr, ", ", &ctx))
        g_exceptions[g_nExceptions++] = uint32_t(strtoul(tok, nullptr, 16));
    GetPrivateProfileStringA("Gate", "sForceFaceGen", "", buf, sizeof(buf), ini);
    ctx = nullptr;
    for (char* tok = strtok_s(buf, ", ", &ctx); tok && g_nForce < 256; tok = strtok_s(nullptr, ", ", &ctx))
        g_force[g_nForce++] = uint32_t(strtoul(tok, nullptr, 16));

    Log("FCStruct2 1.1.4 (STRUCT-2 FaceGen optimizer), runtime %08X, F4SE %08X; start %s, toggle 0x%X | A gate %s (non-masters %s, "
        "%d exceptions, %d forced) | B safety net %s (force fail every %d%s) | C queue %s (max %d steps, %.1f ms, array pool %s, VRAM guard %d MB +%d MB / %d s) | D VRAM %s",
        f4se->runtimeVersion, f4se->f4seVersion, g_on ? "ON" : "OFF", g_toggleKey, g_gateOn ? "on" : "off",
        g_allowNonMasters ? "allowed" : "not allowed", g_nExceptions, g_nForce, g_netOn ? "on" : "off", g_forceFailEvery,
        g_forceRetryToo ? ", retry too" : "", g_queueOn ? "on" : "off", g_maxSteps, g_budgetMs, g_poolOn ? "ON" : "off", g_poolMinFreeMB,
        g_poolReleaseMarginMB, g_poolHoldSec,
        g_logVram ? "on" : "off");
    if (g_verifyHash) Log("D: verify mode ON: every update-path face is read back from the GPU and hashed (stalls; test only)");
    if (!enable) { Log("disabled in FCStruct2.ini (bEnable=0)"); return true; }
    char why[256];
    if (!cef::VersionOk(f4se->runtimeVersion, why, sizeof(why))) {
        Log("WARNING: %s. CEF disabled itself to stay safe: nothing patched, the game runs unmodified. "
            "Wait for a CEF update for this game version.", why);
        return true;
    }
    if (!Install(exe, f4se->runtimeVersion)) { Log("not installed; the game runs unmodified"); return true; }
    CreateThread(nullptr, 0, StatsThread, nullptr, 0, nullptr);
    return true;
}
