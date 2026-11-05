// FCShadow2 0.3 (FC-SHADOW-2 Phase 1a/1b/1c): parallel visibility walk for the previs sun-shadow cull.
// Design: analysis/design/FC-SHADOW-2-TDD.md (§6 has the Phase 0 numbers and corrections, §7 the Phase 1 results, §8 1c)
//
// The sun-shadow cull in previs areas (cascade culling 0x02250400, ID 2319338) calls cull 0x017E0270 (ID 2275918) once per
// cascade at +0x33E. Inside, loop A plane-tests each input block and then calls the visibility walk 0x017E15D0 (ID 2275926)
// at 0x017E0552 with ctx = 0, so every visible object is appended to the culler's own output list (+0xD8). The walk is
// 91-94 % of the cull (0.58 ms per frame in Boston).
//
// Bethesda's dormant async culler runs the same walk on job threads with ctx != 0: one 0x40-byte context per input block,
// held in the culler's chunk list at +0x60 (kick 0x017E41C0), first output block from 0x017E3890 (ID 2275960) with the
// culler's planes copied in (job 0x017E3F00). The rest of cull (loop D) and the finish step 0x017E08C0 already consume the
// +0x60 contexts, in order. This plugin does the same in the previs path, at the walk's call site:
//   mode 1 (Phase 1a): contexts, walk still serial on the main thread  -> proves the context path produces the same output
//   mode 2 (Phase 1b): contexts, walks run as jobs on the async culler's own (idle) job list, the main thread takes blocks
//                       too, and waits with the engine's finish before loop A returns
//   early kick (Phase 1c, bEarlyKick=1): the jobs are posted at loop A's first block instead of its last, and take each
//                       block as soon as the main thread has plane-tested it and given it a context (a published count);
//                       the main thread joins after the last block. A job that waits over 1 ms for the next block returns
//                       (the main thread walks the rest), so a stalled loop A never holds a job thread.
//   carry fix (0.3): per-block contexts lose the output append's "pending" flag between blocks (record byte 3); found by
//                       the 0.3 verify run (1 mismatch in 322, same entries, bytes 3-4 only). FixCarry restores it at the
//                       end of loop A, so the records are the serial ones again.
// Nothing else changes: no job-list fields are written (the FC-RENDER-2 deadlock lesson); jobs never wait or take locks
// besides the engine allocator's spin lock inside the walk (built for this).
// Verify mode: on 1 previs cull in N, run the original walk (ctx = 0), hash the output (entry sequence + effective block
// flags), run again with the active mode, hash, compare. An entry mismatch switches the plugin off.
//
// Self-contained: no CommonLibF4. Address Library is read directly (count u64, then {id u64, offset u64} pairs).

#include <windows.h>
#include <shlobj.h>
#include <cstdint>
#include <cstdio>
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
    1,                       // dataVersion
    0x00000300,              // plugin 0.3.0
    "FCShadow2",
    "Commonwealth Engine Fixes",
    1 << 2,                  // kAddressIndependence_AddressLibrary_1_11_137
    1 << 0,                  // kStructureIndependence_NoStructs (bytes verified at install time)
    { 0x010B0F00, 0 },       // 1.11.240 (informational; the CEF version gate decides)
    0, 0, 0, {}
};

// ---------------------------------------------------------------- engine pieces (Address Library IDs, 1.11.240)
namespace id {
    constexpr uint64_t CascadeCull = 2319338;  // sun-shadow cascade culling                 0x02250400
    constexpr uint64_t Cull        = 2275918;  // cull(culler, flag)                         0x017E0270
    constexpr uint64_t Walk        = 2275926;  // visibility walk(culler, block, ctx)         0x017E15D0
    constexpr uint64_t AsyncMgr    = 2704285;  // pointer to the async culler manager         0x038DE2D8
    constexpr uint64_t CtxBlock    = 2275960;  // context: allocate its first output block    0x017E3890
    constexpr uint64_t ChunkAlloc  = 2275924;  // allocate a 0x408 context chunk              0x017E1490
    constexpr uint64_t JobAdd      = 2268875;  // job list: add {fn, arg}                     0x01688350
    constexpr uint64_t JobKick     = 2268878;  // job list: seal + dispatch                   0x01688480
    constexpr uint64_t JobFinish   = 2268879;  // job list: wait for the batch                0x016884A0
}
constexpr uint32_t kPrevisCullOffset = 0x33E;  // CascadeCull+0x33E: E8 -> Cull (previs branch, per cascade)
constexpr uint32_t kWalkCallOffset   = 0x2E2;  // Cull+0x2E2 (0x017E0552): E8 -> Walk (loop A, per input block)

// Culler layout (from 0x017E0270 / 0x017E41C0 / 0x017E17D0 / 0x006B9440). A "list" is a 0x40-byte header:
// head, tail slot, last block, free blocks, cursor, first block, count (u32), current block.
constexpr uint32_t kInList = 0x98, kOutList = 0xD8;             // loop A input list, serial output list
constexpr uint32_t kCtxHead = 0x60, kCtxTail = 0x68, kCtxCur = 0x70, kCtxFree = 0x78, kCtxCursor = 0x80,
                   kCtxFirst = 0x88, kCtxCount = 0x90;          // context chunk list (16 contexts per 0x400 chunk)
constexpr uint32_t kCullerPlanes = 0x00, kCullerExtra = 0x158;  // 6 planes (0x60 bytes) and the value for block +0x3A60
// Block layout: planes at +0, objects at +0x2060, 5-byte records at +0x3060, owner +0x3A60, count +0x3A68,
// flags +0x3A6C/+0x3A6D/+0x3A6E, next block +0x3A70.
constexpr uint32_t kBlkObjects = 0x2060, kBlkRecords = 0x3060, kBlkOwner = 0x3A60, kBlkCount = 0x3A68,
                   kBlkFlags = 0x3A6C, kBlkNext = 0x3A70;

using CullFn   = void (*)(void* culler, char flag);
using WalkFn   = void (*)(void* culler, void* block, void* ctx);
using CtxBlkFn = uint8_t* (*)(void* ctx);
using AllocFn  = void* (*)(uint32_t size);
using JobFn    = void (*)(void* arg);
using AddFn    = void (*)(void* listRef, void* fn, void* arg, void* a4);
using KickFn   = void (*)(void* listRef, void* a2, void* a3);
using FinishFn = void (*)(void* listRef);

enum Mode { kOff = 0, kSerialCtx = 1, kParallel = 2 };

// ---------------------------------------------------------------- state
static CullFn   g_cull;
static WalkFn   g_walk;
static CtxBlkFn g_ctxBlock;
static AllocFn  g_chunkAlloc;
static AddFn    g_add;
static KickFn   g_kick;
static FinishFn g_finish;
static void**   g_asyncMgr;
static FILE*    g_log;
static LARGE_INTEGER g_qpf;
static DWORD    g_mainTid;

// settings
static bool   g_on;                    // toggled with the hotkey
static int    g_mode = kParallel;      // what ON means
static int    g_toggleKey = VK_F7;
static bool   g_verify = true;
static int    g_verifyEvery = 120;
static double g_minWalkMs = 0.08;
static int    g_minBlocks = 3;
static int    g_maxJobs = 4;
static bool   g_earlyKick = true;      // Phase 1c
static bool   g_fixCarry = true;       // give context records the serial path's carry flag (FixCarry)
static int    g_toggleWhat = 0;        // 0: the key switches the plugin on/off; 1: it switches early kick (A/B tests)
static bool   g_keyWasDown;
static bool   g_disabled;              // set after a verify mismatch or an inconsistency

// per cull call
static bool     g_inPrevis;
static int      g_active;              // mode of the current cull call
static uint8_t* g_culler;
static int      g_expected;            // input blocks loop A will visit (from the list)
static int      g_nonEmpty;            // of those, blocks with entries (the ones that get a context)
static int      g_seen;                // walk calls so far in this cull
static bool     g_parallelThisCull;
static bool     g_batchRan;            // the queued blocks of this cull have been walked
static int      g_cascade;
static LONGLONG g_lastCullEnd;
static uint64_t g_calls;

constexpr int kMaxBlocks = 256;
struct Batch {
    uint8_t*      culler;
    uint8_t*      blocks[kMaxBlocks];
    uint8_t*      ctxs[kMaxBlocks];
    volatile LONG next;                // next block to claim
    volatile LONG published;           // blocks[0..published) are ready to walk
    volatile LONG sealed;              // no more blocks will be published in this cull
    LONG          n;                   // blocks queued by the main thread
    volatile LONG64 workTicks;         // sum of per-block walk time over all threads
    volatile LONG doneByJobs;
    volatile LONG jobGiveUps;          // jobs that returned after waiting too long for the next block
    int           jobs;                // jobs posted for this cull (0 = none)
    void*         listRef;
};
static Batch g_batch;
static uint8_t* g_predicted[kMaxBlocks];   // loop A's blocks in order, from the transcribed iterator
static double g_prevWorkMs[4];         // last measured walk work per cascade index (eligibility)
static volatile LONG64 g_finishStart;  // QPC while the main thread is inside finish (watchdog)

// stats since the last log line
constexpr int kMaxCascades = 4;
struct PerCascade { uint64_t calls, callsByMode[3], cullTicks[3], walkTicks, blocks, maxBlocks; };
static PerCascade g_pc[kMaxCascades];
static uint64_t g_par, g_parJobs, g_parBlocksJobs, g_parBlocksMain, g_finishTicks, g_finishMax, g_serialFallbacks;
static uint64_t g_parEarly, g_jobGiveUps;
static uint64_t g_carryCulls, g_carryRecords, g_carryNA, g_verifyUnstable;
static uint64_t g_verifyRuns, g_verifyEntryMismatch, g_verifyFlagDiff, g_verifyEntries;
static uint64_t g_callWalkTicks, g_callBlocks;
static ULONGLONG g_lastLog;

static void Log(const char* fmt, ...) {
    if (!g_log) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list a; va_start(a, fmt); vfprintf(g_log, fmt, a); va_end(a);
    fputc('\n', g_log); fflush(g_log);
}

static double Ms(uint64_t ticks) { return 1000.0 * double(ticks) / double(g_qpf.QuadPart); }
template <class T> static T& At(uint8_t* p, uint32_t off) { return *reinterpret_cast<T*>(p + off); }

// ---------------------------------------------------------------- engine structures
// Number of blocks loop A visits: a transcription of its iterator over the input list (0x017E0270, first loop).
// Also records the blocks in visit order (g_predicted, first kMaxBlocks) and counts the non-empty ones.
static int CountInputBlocks(uint8_t* c, int* nonEmpty) {
    uint8_t* end = At<uint8_t*>(c, kInList + 0x20);       // cursor
    uint8_t* last = At<uint8_t*>(c, kInList + 0x10);      // last block
    uint8_t* seg = nullptr;
    uint8_t* cur;
    if (At<uint8_t*>(c, kInList + 0x28) == end) {         // first == cursor
        cur = end;
        if (last && last + kBlkNext <= end && At<uint8_t*>(last, kBlkNext)) cur = At<uint8_t*>(last, kBlkNext);
    } else {
        seg = At<uint8_t*>(c, kInList);
        cur = seg ? At<uint8_t*>(c, kInList + 0x28) : nullptr;
    }
    if (last && last + kBlkNext <= end && At<uint8_t*>(last, kBlkNext)) end = At<uint8_t*>(last, kBlkNext);
    int n = 0;
    *nonEmpty = 0;
    while (cur != end && n < 100000) {
        if (n < kMaxBlocks) g_predicted[n] = cur;
        if (At<uint32_t>(cur, kBlkCount) != 0) ++*nonEmpty;
        ++n;
        cur += kBlkNext;
        if (seg && seg + kBlkNext <= cur) {
            uint8_t* nx = At<uint8_t*>(seg, kBlkNext);
            if (nx) { seg = nx; cur = nx; }
        }
    }
    return n;
}

// One context slot in the culler's +0x60 chunk list, exactly as the async kick 0x017E41C0 makes it.
static uint8_t* AllocContext(uint8_t* c) {
    uint8_t*& cur = At<uint8_t*>(c, kCtxCur);
    uint8_t*& cursor = At<uint8_t*>(c, kCtxCursor);
    uint8_t* slot;
    if (!cur || cursor == cur + 0x400) {
        uint8_t* chunk = At<uint8_t*>(c, kCtxFree);
        if (!chunk) chunk = static_cast<uint8_t*>(g_chunkAlloc(0x408));
        else At<uint8_t*>(c, kCtxFree) = At<uint8_t*>(chunk, 0x400);
        if (!chunk) return nullptr;
        At<uint8_t*>(chunk, 0x400) = nullptr;
        *At<uint8_t**>(c, kCtxTail) = chunk;
        At<uint8_t*>(c, kCtxTail) = chunk + 0x400;
        cur = chunk;
        cursor = chunk;
        if (!At<uint8_t*>(c, kCtxFirst)) At<uint8_t*>(c, kCtxFirst) = chunk;
        slot = chunk;
    } else {
        slot = cursor;
    }
    ++At<int32_t>(c, kCtxCount);
    cursor = slot + 0x40;
    memset(slot, 0, 0x40);
    At<uint8_t*>(slot, 0x08) = slot;                          // tail slot -> head
    // first output block with the culler's planes, as the async job 0x017E3F00 does
    uint8_t* blk = g_ctxBlock(slot);
    At<uint8_t*>(slot, 0x38) = blk;
    memcpy(blk + 0, c + kCullerPlanes, 0x60);
    At<uint64_t>(blk, kBlkOwner) = At<uint64_t>(c, kCullerExtra);
    return slot;
}

// Hash of the cull's output in consumption order: serial list (+0xD8), then the contexts (+0x60) in order.
// entries: object pointer + 5-byte record per entry (independent of how entries are grouped into blocks);
// flags: each entry's effective block flags (+0x3A6C..E), which do depend on the grouping.
static void HashList(uint8_t* list, uint64_t& he, uint64_t& hf, uint64_t& n) {
    for (uint8_t* b = At<uint8_t*>(list, 0); b; b = At<uint8_t*>(b, kBlkNext)) {
        const uint32_t cnt = At<uint32_t>(b, kBlkCount);
        for (uint32_t i = 0; i < cnt && i < 512; ++i) {
            uint64_t o = At<uint64_t>(b, kBlkObjects + 8 * i);
            uint64_t r = 0;
            memcpy(&r, b + kBlkRecords + 5 * i, 5);
            he = (he ^ o) * 0x100000001B3ull;
            he = (he ^ r) * 0x100000001B3ull;
            uint64_t f = uint64_t(b[kBlkFlags]) | uint64_t(b[kBlkFlags + 1]) << 8 | uint64_t(b[kBlkFlags + 2]) << 16;
            hf = (hf ^ f) * 0x100000001B3ull;
        }
        n += cnt;
    }
}

// The context lists (+0x60 chunk list) in order.
template <class F> static void ForEachContext(uint8_t* c, F f) {
    const int32_t count = At<int32_t>(c, kCtxCount);
    uint8_t* chunk = At<uint8_t*>(c, kCtxHead);
    for (int32_t i = 0; i < count && chunk; ++i) {
        if (i && (i % 16) == 0) chunk = At<uint8_t*>(chunk, 0x400);
        if (!chunk) break;
        f(chunk + 0x40 * (i % 16));
    }
}

static void HashOutput(uint8_t* c, uint64_t& he, uint64_t& hf, uint64_t& n) {
    he = hf = 0xCBF29CE484222325ull;
    n = 0;
    HashList(c + kOutList, he, hf, n);
    ForEachContext(c, [&](uint8_t* list) { HashList(list, he, hf, n); });
}

// Copy of the output in consumption order (verify diagnostics: which entry differs, and how).
struct Entry { uint64_t obj; uint8_t rec[5]; uint8_t flags[3]; };
constexpr uint32_t kMaxEntries = 32768;
static Entry g_entries[2][kMaxEntries];

static uint32_t CollectList(uint8_t* list, Entry* out, uint32_t n) {
    for (uint8_t* b = At<uint8_t*>(list, 0); b; b = At<uint8_t*>(b, kBlkNext)) {
        const uint32_t cnt = At<uint32_t>(b, kBlkCount);
        for (uint32_t i = 0; i < cnt && i < 512; ++i, ++n) {
            if (n >= kMaxEntries) continue;
            out[n].obj = At<uint64_t>(b, kBlkObjects + 8 * i);
            memcpy(out[n].rec, b + kBlkRecords + 5 * i, 5);
            memcpy(out[n].flags, b + kBlkFlags, 3);
        }
    }
    return n;
}

static uint32_t CollectOutput(uint8_t* c, Entry* out) {
    uint32_t n = CollectList(c + kOutList, out, 0);
    ForEachContext(c, [&](uint8_t* list) { n = CollectList(list, out, n); });
    return n;
}

// Record byte 3 is the "pending" flag the output append 0x017E17D0 leaves in its block (+0x3A6D) after an entry whose
// walk bit 3 is set: the next entry appended to the same block takes it into its record. In the serial path all of loop
// A appends to one list, so the flag carries from the last entry of input block k to the first entry of block k+1
// (unless that entry starts a new 512-entry block, which starts with the flag clear). Per-block contexts start clear,
// so the carry is lost at every context boundary (Bethesda's async path has the same difference). This rewrites byte 3
// of every context entry to what the serial path would have written: carry = the previous entry's flag (the next
// entry's byte 3 within a block, else the block's final +0x3A6D), cleared at every 512th entry of the serial sequence.
// Returns the number of records changed (-1: not applicable, the serial list already has entries).
static int FixCarry(uint8_t* c) {
    for (uint8_t* b = At<uint8_t*>(c + kOutList, 0); b; b = At<uint8_t*>(b, kBlkNext))
        if (At<uint32_t>(b, kBlkCount)) return -1;
    uint32_t serial = 0;                                       // entries in the serial sequence so far
    uint8_t carry = 0;
    int changed = 0;
    ForEachContext(c, [&](uint8_t* list) {
        for (uint8_t* b = At<uint8_t*>(list, 0); b; b = At<uint8_t*>(b, kBlkNext)) {
            uint32_t cnt = At<uint32_t>(b, kBlkCount);
            if (cnt > 512) cnt = 512;
            uint8_t* rec = b + kBlkRecords;
            for (uint32_t i = 0; i < cnt; ++i, ++serial) {
                const uint8_t flagAfter = i + 1 < cnt ? rec[5 * (i + 1) + 3] : b[kBlkFlags + 1];  // read before writing
                const uint8_t want = (serial && serial % 512 == 0) ? 0 : carry;
                if (rec[5 * i + 3] != want) { rec[5 * i + 3] = want; ++changed; }
                carry = flagAfter;
            }
        }
    });
    return changed;
}

// ---------------------------------------------------------------- parallel walk
// Claim and walk published blocks until the batch is sealed and drained. Only jobs ever wait here (the main thread
// calls it after sealing); a job gives up after 1 ms without a new block, and the main thread walks what is left.
static void RunBlocks(bool isJob) {
    Batch& b = g_batch;
    LONGLONG idleSince = 0;
    for (;;) {
        const LONG sealed = b.sealed;                          // read before published: once sealed, published is final
        const LONG k = b.next;
        if (k < b.published) {
            if (InterlockedCompareExchange(&b.next, k + 1, k) != k) continue;
            LARGE_INTEGER t0, t1;
            QueryPerformanceCounter(&t0);
            g_walk(b.culler, b.blocks[k], b.ctxs[k]);
            QueryPerformanceCounter(&t1);
            InterlockedAdd64(&b.workTicks, t1.QuadPart - t0.QuadPart);
            if (isJob) InterlockedIncrement(&b.doneByJobs);
            idleSince = 0;
            continue;
        }
        if (sealed || !isJob) return;
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        if (!idleSince) idleSince = now.QuadPart;
        else if (Ms(uint64_t(now.QuadPart - idleSince)) > 1.0) { InterlockedIncrement(&b.jobGiveUps); return; }
        YieldProcessor();
    }
}

static void JobEntry(void*) { RunBlocks(true); }

// Post up to `want` jobs on the async culler's job list (only if it exists and is idle). Blocks are published later.
static void StartJobs(int want) {
    Batch& b = g_batch;
    uint8_t* mgr = g_asyncMgr ? static_cast<uint8_t*>(*g_asyncMgr) : nullptr;
    b.listRef = mgr ? mgr + 0x40 : nullptr;
    uint8_t* list = b.listRef ? *static_cast<uint8_t**>(b.listRef) : nullptr;
    int jobs = 0;
    if (list && At<int32_t>(list, 0x98) == 0) {               // the async culler's list exists and is idle
        jobs = int(At<uint32_t>(list, 0xC4));
        if (jobs > g_maxJobs) jobs = g_maxJobs;
        if (jobs > want) jobs = want;
    }
    b.jobs = jobs > 0 ? jobs : 0;
    if (!b.jobs) return;
    for (int j = 0; j < b.jobs; ++j) g_add(b.listRef, reinterpret_cast<void*>(&JobEntry), &b, nullptr);
    g_kick(b.listRef, nullptr, nullptr);
}

// Loop A is at its last block: seal, walk what is left on the main thread too, then wait for the jobs.
static void JoinJobs() {
    Batch& b = g_batch;
    InterlockedExchange(&b.sealed, 1);
    RunBlocks(false);
    if (!b.jobs) {                                             // no usable list: everything was walked serially
        if (b.n) ++g_serialFallbacks;
        return;
    }
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    InterlockedExchange64(&g_finishStart, t0.QuadPart);
    g_finish(b.listRef);                                       // every job has run when this returns
    InterlockedExchange64(&g_finishStart, 0);
    QueryPerformanceCounter(&t1);
    const uint64_t w = uint64_t(t1.QuadPart - t0.QuadPart);
    ++g_par;
    g_parJobs += b.jobs;
    g_parBlocksJobs += b.doneByJobs;
    g_parBlocksMain += uint64_t(b.n) - b.doneByJobs;
    g_jobGiveUps += b.jobGiveUps;
    g_finishTicks += w;
    if (w > g_finishMax) g_finishMax = w;
    b.jobs = 0;
}

// ---------------------------------------------------------------- hooks
// 0x017E0552 inside cull: the walk of one input block (loop A).
static void WalkStub(void* culler, void* block, void* ctx) {
    if (!g_inPrevis || GetCurrentThreadId() != g_mainTid) { g_walk(culler, block, ctx); return; }
    uint8_t* c = static_cast<uint8_t*>(culler);
    const int k = g_seen++;
    ++g_callBlocks;
    if (g_active != kOff && k == 0) {
        // loop A starts after cull reset the context list; the async kick also requires it empty
        if (At<int32_t>(c, kCtxCount) != 0 || g_expected <= 0 || g_expected > kMaxBlocks) {
            g_active = kOff;
            ++g_serialFallbacks;
        } else {
            g_parallelThisCull = g_active == kParallel && g_expected >= g_minBlocks &&
                                 g_prevWorkMs[g_cascade] >= g_minWalkMs;
            g_batch.culler = c;
            if (g_parallelThisCull && g_earlyKick) {           // Phase 1c: wake the jobs while loop A still runs
                StartJobs(g_nonEmpty - 1);
                ++g_parEarly;
            }
        }
    }
    if (g_parallelThisCull && !g_batchRan && (k >= kMaxBlocks || block != g_predicted[k])) {
        // loop A is not visiting the blocks our transcribed iterator predicted: finish the batch here, while still
        // inside loop A (so nothing later in cull reads a context a job is writing), and stop using the plugin
        g_batchRan = true;
        JoinJobs();
        g_callWalkTicks += uint64_t(g_batch.workTicks);
        Log("ERROR: loop A block %d is %p, predicted %p; finished the batch early and switched off", k, block,
            k < kMaxBlocks ? static_cast<void*>(g_predicted[k]) : nullptr);
        g_disabled = true;
    }
    if (g_active == kOff) {                                    // original behaviour (also the verify reference)
        LARGE_INTEGER a, b;
        QueryPerformanceCounter(&a);
        g_walk(culler, block, ctx);
        QueryPerformanceCounter(&b);
        g_callWalkTicks += uint64_t(b.QuadPart - a.QuadPart);
        return;
    }
    if (At<uint32_t>(static_cast<uint8_t*>(block), kBlkCount) != 0) {   // the async kick skips empty blocks too
        uint8_t* slot = AllocContext(c);
        if (!slot) {                                           // allocation failed: this block goes the serial way
            g_walk(culler, block, ctx);
        } else if (!g_parallelThisCull || g_batchRan) {        // mode 1, a small cull, or a block after the batch
            LARGE_INTEGER a, b;
            QueryPerformanceCounter(&a);
            g_walk(culler, block, slot);
            QueryPerformanceCounter(&b);
            g_callWalkTicks += uint64_t(b.QuadPart - a.QuadPart);
        } else {
            g_batch.blocks[g_batch.n] = static_cast<uint8_t*>(block);
            g_batch.ctxs[g_batch.n] = slot;
            ++g_batch.n;
            InterlockedExchange(&g_batch.published, g_batch.n);   // a waiting job may take it now
        }
    }
    if (g_parallelThisCull && !g_batchRan && g_seen == g_expected) {   // last block of loop A: walk them all now
        g_batchRan = true;
        if (!g_earlyKick && g_batch.n > 0) StartJobs(g_batch.n - 1);
        JoinJobs();
        g_callWalkTicks += uint64_t(g_batch.workTicks);
    }
    if (g_seen == g_expected && g_fixCarry) {                  // loop A is done: give the records their serial carry flag
        const int changed = FixCarry(c);
        if (changed < 0) ++g_carryNA;
        else if (changed > 0) { ++g_carryCulls; g_carryRecords += uint64_t(changed); }
    }
}

static void RunCull(uint8_t* c, char flag, int mode) {
    g_active = mode;
    g_expected = mode != kOff ? CountInputBlocks(c, &g_nonEmpty) : 0;
    g_seen = 0;
    g_parallelThisCull = false;
    g_batchRan = false;
    g_batch.n = g_batch.next = g_batch.published = g_batch.sealed = 0;
    g_batch.workTicks = 0;
    g_batch.doneByJobs = g_batch.jobGiveUps = 0;
    g_batch.jobs = 0;
    g_callWalkTicks = g_callBlocks = 0;
    g_inPrevis = true;
    g_cull(c, flag);
    g_inPrevis = false;
    if (g_parallelThisCull && !g_batchRan) {
        // loop A visited fewer blocks than its list held (every visited block matched the prediction): walk the queued
        // ones now (their plane test in cull's later loop was missed for this one frame) and stop using the plugin
        g_batchRan = true;
        if (!g_earlyKick && g_batch.n > 0) StartJobs(g_batch.n - 1);
        JoinJobs();
        Log("ERROR: loop A visited %d blocks, %d expected; walked the queued blocks late and switched off",
            g_seen, g_expected);
        g_disabled = true;
    } else if (g_active != kOff && g_seen != g_expected) {
        Log("WARNING: block count mismatch (visited %d, expected %d)", g_seen, g_expected);
    }
}

// 0x0225073E: cull(culler, 0) for one cascade of the previs branch.
static void PrevisCullStub(void* culler, char flag) {
    if (!g_mainTid) g_mainTid = GetCurrentThreadId();
    uint8_t* c = static_cast<uint8_t*>(culler);
    LARGE_INTEGER a, b;
    QueryPerformanceCounter(&a);
    g_cascade = (g_lastCullEnd && Ms(uint64_t(a.QuadPart - g_lastCullEnd)) < 1.0) ? g_cascade + 1 : 0;
    if (g_cascade >= kMaxCascades) g_cascade = kMaxCascades - 1;
    bool down = g_toggleKey && (GetAsyncKeyState(g_toggleKey) & 0x8000);
    if (down && !g_keyWasDown) {
        if (g_toggleWhat == 1) {
            g_earlyKick = !g_earlyKick;
            Log("toggle: early kick %s", g_earlyKick ? "ON (Phase 1c)" : "OFF (Phase 1b)");
        } else {
            g_on = !g_on;
            Log("toggle: %s", g_on ? (g_mode == kParallel ? "ON (parallel)" : "ON (serial contexts)") : "OFF");
        }
    }
    g_keyWasDown = down;
    const int mode = (g_on && !g_disabled) ? g_mode : kOff;
    ++g_calls;

    if (mode != kOff && g_verify && g_verifyEvery > 0 && (g_calls % g_verifyEvery) == 0) {
        // original, ours, original again: a difference counts only if the two original runs agree (the walk also updates
        // object state, e.g. the first-visible fade reset 0x0217D760, so a frame can legitimately change between runs)
        uint64_t he0, hf0, n0, he1, hf1, n1, he2, hf2, n2;
        RunCull(c, flag, kOff);
        HashOutput(c, he0, hf0, n0);
        const uint32_t c0 = CollectOutput(c, g_entries[0]);
        RunCull(c, flag, mode);                                // cull resets its output, so it can run again
        HashOutput(c, he1, hf1, n1);
        const uint32_t c1 = CollectOutput(c, g_entries[1]);
        RunCull(c, flag, kOff);
        HashOutput(c, he2, hf2, n2);
        ++g_verifyRuns;
        g_verifyEntries += n0;
        if (he0 != he2 || n0 != n2) {
            ++g_verifyUnstable;
        } else if (he0 != he1 || n0 != n1) {
            ++g_verifyEntryMismatch;
            uint32_t first = UINT32_MAX, diffs = 0;
            const uint32_t m = c0 < c1 ? c0 : c1;
            for (uint32_t i = 0; i < m && i < kMaxEntries; ++i)
                if (memcmp(&g_entries[0][i], &g_entries[1][i], 13) != 0) { if (first == UINT32_MAX) first = i; ++diffs; }
            Log("VERIFY MISMATCH (cascade %d, mode %d): original %llu entries hash %016llX, ours %llu entries hash "
                "%016llX; %u entries differ; switching off", g_cascade, mode, n0, he0, n1, he1, diffs);
            if (first != UINT32_MAX) {
                const Entry& eo = g_entries[0][first];
                const Entry& eu = g_entries[1][first];
                Log("  first difference at entry %u: original obj %016llX rec %02X %02X %02X %02X %02X | ours obj %016llX "
                    "rec %02X %02X %02X %02X %02X", first, eo.obj, eo.rec[0], eo.rec[1], eo.rec[2], eo.rec[3], eo.rec[4],
                    eu.obj, eu.rec[0], eu.rec[1], eu.rec[2], eu.rec[3], eu.rec[4]);
            }
            g_disabled = true;
        } else if (hf0 != hf1) {
            ++g_verifyFlagDiff;
        }
    } else {
        RunCull(c, flag, mode);
    }

    QueryPerformanceCounter(&b);
    g_lastCullEnd = b.QuadPart;
    PerCascade& p = g_pc[g_cascade];
    ++p.calls;
    ++p.callsByMode[g_active];
    p.cullTicks[g_active] += uint64_t(b.QuadPart - a.QuadPart);
    p.walkTicks += g_callWalkTicks;
    p.blocks += g_callBlocks;
    if (g_callBlocks > p.maxBlocks) p.maxBlocks = g_callBlocks;
    g_prevWorkMs[g_cascade] = Ms(g_callWalkTicks);

    ULONGLONG now = GetTickCount64();
    if (now - g_lastLog >= 10000) {
        for (int i = 0; i < kMaxCascades; ++i) {
            const PerCascade& q = g_pc[i];
            if (!q.calls) continue;
            Log("cascade %d: %llu calls (off %llu, serial-ctx %llu, parallel-mode %llu) | cull avg: off %.3f ms, "
                "serial-ctx %.3f ms, parallel-mode %.3f ms | walk work %.3f ms avg | blocks %.1f (max %llu)", i, q.calls,
                q.callsByMode[0], q.callsByMode[1], q.callsByMode[2],
                q.callsByMode[0] ? Ms(q.cullTicks[0]) / q.callsByMode[0] : 0.0,
                q.callsByMode[1] ? Ms(q.cullTicks[1]) / q.callsByMode[1] : 0.0,
                q.callsByMode[2] ? Ms(q.cullTicks[2]) / q.callsByMode[2] : 0.0,
                Ms(q.walkTicks) / q.calls, double(q.blocks) / q.calls, q.maxBlocks);
        }
        Log("state %s%s, early kick %s | parallel culls %llu (%llu kicked early): jobs %.1f avg, blocks by jobs %llu / "
            "main %llu, job give-ups %llu, finish wait avg %.3f ms max %.3f ms | serial fallbacks %llu | carry fixed in "
            "%llu culls (%llu records), n/a %llu | verify: %llu runs, %llu entries compared, %llu entry mismatches, "
            "%llu with different block-flag grouping, %llu unstable frames",
            g_on ? "ON" : "OFF", g_disabled ? " (DISABLED)" : "", g_earlyKick ? "on" : "off", g_par, g_parEarly,
            g_par ? double(g_parJobs) / g_par : 0.0, g_parBlocksJobs, g_parBlocksMain, g_jobGiveUps,
            g_par ? Ms(g_finishTicks) / g_par : 0.0, Ms(g_finishMax), g_serialFallbacks, g_carryCulls, g_carryRecords,
            g_carryNA, g_verifyRuns, g_verifyEntries, g_verifyEntryMismatch, g_verifyFlagDiff, g_verifyUnstable);
        g_carryCulls = g_carryRecords = g_carryNA = g_verifyUnstable = 0;
        memset(g_pc, 0, sizeof(g_pc));
        g_par = g_parJobs = g_parBlocksJobs = g_parBlocksMain = g_finishTicks = g_finishMax = g_serialFallbacks = 0;
        g_parEarly = g_jobGiveUps = 0;
        g_verifyRuns = g_verifyEntries = g_verifyEntryMismatch = g_verifyFlagDiff = 0;
        g_lastLog = now;
    }
}

// Watchdog: a stuck job batch would show as the main thread waiting in finish.
static DWORD WINAPI Watchdog(void*) {
    bool reported = false;
    for (;;) {
        Sleep(1000);
        LONG64 start = InterlockedCompareExchange64(&g_finishStart, 0, 0);
        if (!start) { reported = false; continue; }
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        double s = double(now.QuadPart - start) / double(g_qpf.QuadPart);
        if (s >= 5.0 && !reported) {
            Log("HANG SUSPECTED: the main thread has waited %.0f s for the shadow-walk jobs. Set bEnable=0 in "
                "FCShadow2.ini and report this log.", s);
            reported = true;
        }
    }
}

// ---------------------------------------------------------------- Address Library
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

// ---------------------------------------------------------------- patching helpers
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

static uint8_t* CheckCall(uintptr_t siteAddr, uintptr_t expected, const char* what) {
    uint8_t* site = reinterpret_cast<uint8_t*>(siteAddr);
    int32_t rel; memcpy(&rel, site + 1, 4);
    uintptr_t dest = siteAddr + 5 + rel;
    if (site[0] != 0xE8 || dest != expected) {
        Log("ERROR: unexpected bytes at %s (%02X, dest %p, expected %p); not installing", what, site[0],
            reinterpret_cast<void*>(dest), reinterpret_cast<void*>(expected));
        return nullptr;
    }
    return site;
}

static bool RedirectCall(uint8_t* site, uint8_t* slot, void* target) {
    const uint8_t jmp[6] = { 0xFF, 0x25, 0, 0, 0, 0 };
    memcpy(slot, jmp, 6);
    memcpy(slot + 6, &target, 8);
    FlushInstructionCache(GetCurrentProcess(), slot, 14);
    int32_t newRel = int32_t(reinterpret_cast<intptr_t>(slot) - (reinterpret_cast<intptr_t>(site) + 5));
    DWORD old;
    if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(site + 1, &newRel, 4);
    VirtualProtect(site, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    return true;
}

static bool Install(const F4SEInterface* f4se) {
    char exe[MAX_PATH]; GetModuleFileNameA(nullptr, exe, MAX_PATH);
    char* slash = strrchr(exe, '\\'); if (slash) *slash = 0;
    uint32_t rt = f4se->runtimeVersion;
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\Data\\F4SE\\Plugins\\version-%u-%u-%u-%u.bin",
             exe, rt >> 24, (rt >> 16) & 0xFF, (rt >> 4) & 0xFFF, rt & 0xF);

    const uint64_t ids[] = { id::CascadeCull, id::Cull, id::Walk, id::AsyncMgr, id::CtxBlock, id::ChunkAlloc,
                             id::JobAdd, id::JobKick, id::JobFinish };
    constexpr int kIds = sizeof(ids) / sizeof(ids[0]);
    uint64_t off[kIds];
    if (!LoadOffsets(path, ids, off, kIds)) { Log("ERROR: Address Library missing or incomplete: %s", path); return false; }

    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    uint8_t* cullSite = CheckCall(base + off[0] + kPrevisCullOffset, base + off[1], "CascadeCull+0x33E (previs cull)");
    uint8_t* walkSite = CheckCall(base + off[1] + kWalkCallOffset, base + off[2], "Cull+0x2E2 (walk)");
    if (!cullSite || !walkSite) return false;

    g_cull       = reinterpret_cast<CullFn>(base + off[1]);
    g_walk       = reinterpret_cast<WalkFn>(base + off[2]);
    g_asyncMgr   = reinterpret_cast<void**>(base + off[3]);
    g_ctxBlock   = reinterpret_cast<CtxBlkFn>(base + off[4]);
    g_chunkAlloc = reinterpret_cast<AllocFn>(base + off[5]);
    g_add        = reinterpret_cast<AddFn>(base + off[6]);
    g_kick       = reinterpret_cast<KickFn>(base + off[7]);
    g_finish     = reinterpret_cast<FinishFn>(base + off[8]);

    uint8_t* page = static_cast<uint8_t*>(AllocNear(base));
    if (!page) { Log("ERROR: no memory within 2 GB of the exe for the jump slots"); return false; }
    if (!RedirectCall(cullSite, page, reinterpret_cast<void*>(&PrevisCullStub)) ||
        !RedirectCall(walkSite, page + 16, reinterpret_cast<void*>(&WalkStub))) {
        Log("ERROR: VirtualProtect failed"); return false;
    }
    HANDLE w = CreateThread(nullptr, 0, Watchdog, nullptr, 0, nullptr);
    if (w) CloseHandle(w);
    Log("installed: CascadeCull+0x%X -> PrevisCullStub, Cull+0x%X -> WalkStub, slots %p | ON means %s | start %s | "
        "toggle key 0x%X switches %s | verify %s (1 in %d) | parallel when walk >= %.2f ms and >= %d blocks, up to %d "
        "jobs, early kick %s, carry fix %s", kPrevisCullOffset, kWalkCallOffset, page,
        g_mode == kParallel ? "parallel" : "serial contexts", g_on ? "ON" : "OFF", g_toggleKey,
        g_toggleWhat == 1 ? "early kick" : "on/off", g_verify ? "on" : "off", g_verifyEvery, g_minWalkMs, g_minBlocks,
        g_maxJobs, g_earlyKick ? "on" : "off", g_fixCarry ? "on" : "off");
    return true;
}

// ---------------------------------------------------------------- F4SE entry point
extern "C" __declspec(dllexport) bool F4SEPlugin_Load(const F4SEInterface* f4se) {
    char docs[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_PERSONAL, nullptr, SHGFP_TYPE_CURRENT, docs))) {
        char lp[MAX_PATH];
        snprintf(lp, sizeof(lp), "%s\\My Games\\Fallout4\\F4SE\\FCShadow2.log", docs);
        g_log = _fsopen(lp, "w", _SH_DENYWR);
    }
    if (f4se->isEditor) return false;
    QueryPerformanceFrequency(&g_qpf);

    char ini[MAX_PATH]; GetModuleFileNameA(nullptr, ini, MAX_PATH);
    char* slash = strrchr(ini, '\\'); if (slash) *slash = 0;
    strncat_s(ini, "\\Data\\F4SE\\Plugins\\FCShadow2.ini", _TRUNCATE);
    char buf[32];
    bool enable   = GetPrivateProfileIntA("General", "bEnable", 1, ini) != 0;
    g_on          = GetPrivateProfileIntA("General", "bStartOn", 0, ini) != 0;
    g_mode        = GetPrivateProfileIntA("General", "iMode", kParallel, ini) == kSerialCtx ? kSerialCtx : kParallel;
    g_toggleKey   = GetPrivateProfileIntA("General", "iToggleKey", VK_F7, ini);
    g_verify      = GetPrivateProfileIntA("General", "bVerify", 0, ini) != 0;
    g_verifyEvery = GetPrivateProfileIntA("General", "iVerifyEvery", 120, ini);
    g_minBlocks   = GetPrivateProfileIntA("General", "iMinBlocks", 3, ini);
    g_maxJobs     = GetPrivateProfileIntA("General", "iMaxJobs", 4, ini);
    g_earlyKick   = GetPrivateProfileIntA("General", "bEarlyKick", 1, ini) != 0;
    g_toggleWhat  = GetPrivateProfileIntA("General", "iToggleWhat", 0, ini) == 1 ? 1 : 0;
    g_fixCarry    = GetPrivateProfileIntA("General", "bFixCarry", 1, ini) != 0;
    GetPrivateProfileStringA("General", "fMinWalkMs", "0.08", buf, sizeof(buf), ini);
    g_minWalkMs = atof(buf);

    Log("FCShadow2 0.3.0, runtime %08X, F4SE %08X", f4se->runtimeVersion, f4se->f4seVersion);
    char why[256];
    if (!cef::VersionOk(f4se->runtimeVersion, why, sizeof(why))) {
        Log("CEF disabled itself: %s. Nothing was patched.", why);
        return true;
    }
    if (!enable) { Log("disabled in FCShadow2.ini (bEnable=0)"); return true; }
    if (!Install(f4se)) Log("not installed; the game runs unmodified");
    return true;
}
