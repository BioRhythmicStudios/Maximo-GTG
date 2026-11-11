// Native boot test for Maximo: Ghosts to Glory (SLES-50703).
//
// Runs the recompiled executable from _start through main on the PS2Recomp runtime, headless, with
// the IOP-side services replaced at the EE SDK boundary by logging stubs. Nothing is rendered.
//
// usage: maximo_boot <disc folder containing SLES_507.03> [seconds=20] [log=boot_log.txt]
//
// What is replaced (addresses identified from SLES_507.03, see BOOT_FINDINGS.md):
//   SIF / IOP     sceSifInitRpc, sceSifBindRpc, sceSifCallRpc, sceSifCheckStatRpc, sceSifRebootIop,
//                 sceSifSyncIop, sceSifLoadModule. IOP services are answered by
//                 small native handlers keyed by RPC server id; anything unknown is logged as such.
//   libcdvd       sceCdInit, sceCdMmode, sceCdSearchFile, sceCdRead, sceCdSync, sceCdSyncS,
//                 sceCdDiskReady, sceCdSt*: the runtime's host-file implementation, reading the
//                 extracted disc folder. Streams read past the end of their file (zeros beyond
//                 it), as the drive does: Maximo always streams whole 100-sector banks.
//   libpad        scePadInit, scePadPortOpen, scePadRead, scePadGetState, scePadPortClose, scePadEnd:
//                 the runtime's implementation (no controller attached).
//   libmc         sceMcInit: the runtime's implementation.
//   libc          printf (to the log), __assert (logged, run stops).
// Everything else, including the game's own GS/DMA register programming, runs as recompiled code
// against the runtime's memory map; every hardware register access is logged by the MMIO hook.

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "ps2_stubs.h"
#include "runtime/ee_scheduler.h"
#include "runtime/ps2_host_profile.h"
#include "runtime/ps2_vu1_overlap_model.h"
#include "runtime/ps2_vu1_thread.h"
#include "runtime/ps2_pacing_trace.h"
#include "runtime/ps2_exit_reason.h"
#include "runtime/ps2_display_aspect.h"
#include "runtime/gs/gs_cpu_profile.h"
#include "vu1_capture.h"
#ifdef MAXIMO_ENABLE_GPU_SHADOW
#include "gpu_shadow.h"
#include "runtime/gs/gs_backend.h"
#endif

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <condition_variable>
#include <thread>
#include <vector>
#include "hdtex/hdtex_format.h"
#ifdef MAXIMO_HAS_FFMPEG
#include <deque>
#include "runtime/ps2_ipu.h"
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
}
#endif

namespace ps2_stubs
{
    void setCdStreamPastFileEnd(bool enabled); // runtime CD layer (boot-runtime branch)
    // Pad test hooks (Pad.cpp): override what scePadRead reports. Buttons are active-low.
    void setPadOverrideState(uint16_t buttons, uint8_t lx, uint8_t ly, uint8_t rx, uint8_t ry);
    void clearPadOverrideState();
}

extern const uint32_t g_ps2RecompiledFunctionTableBase;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount;
extern PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[];

namespace
{
    namespace fs = std::filesystem;

    // ---- logging -------------------------------------------------------------------------------

    FILE *g_log = stdout;
    std::mutex g_logMutex;
    auto g_start = std::chrono::steady_clock::now();

    void logf(const char *fmt, ...)
    {
        std::lock_guard<std::mutex> lock(g_logMutex);
        const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
        std::fprintf(g_log, "[%9.4f] ", t);
        va_list ap;
        va_start(ap, fmt);
        std::vfprintf(g_log, fmt, ap);
        va_end(ap);
        std::fputc('\n', g_log);
        std::fflush(g_log);
    }

    PS2Runtime *g_runtime = nullptr;
#ifdef MAXIMO_ENABLE_GPU_SHADOW
    std::unique_ptr<GpuShadow> g_gpuShadow;
    std::vector<uint64_t> g_gpuShadowFrames; // capture frames, ascending; the run stops after the last
    // MAXIMO_GS_BACKEND=gpu: paraLLEl-GS renders and supplies the window frames; the CPU GS keeps only register,
    // privileged-register and interrupt semantics (GSDiscardBackend). g_gpuFailed records a fatal GPU-mode error.
    bool g_gpuMode = false;
    std::atomic<bool> g_gpuFailed{false};

    // Raster backend for GPU mode. Draws, CLUT loads and uploads are dropped (the GPU device receives the same
    // packets). Anything that needs the CPU's VRAM contents cannot be answered and fails the run instead.
    class GSDiscardBackend final : public GSRasterBackend
    {
    public:
        void Initialize(uint8_t *, uint32_t) override {}
        void Reset() override {}
        void Submit(const GSPrimitiveBatch &) override {}
        void LoadClut(const GSTex0Reg &, const GSTexClutReg &) override {}
        void BeginTransfer(const GSTransferCommand &command) override
        {
            if (command.direction == 1u)
                fail("local-to-host transfer");
        }
        void UploadImage(const uint8_t *, uint32_t) override {}
        void Flush() override {}
        void TextureFlush() override {}
        void Sync(GSSyncReason) override {}
        PresentationFrame Present(const GSPresentationRequest &) override { return {}; }
        bool ClearFramebuffer(const GSContext &, uint32_t) override
        {
            fail("HLE framebuffer clear");
            return false;
        }
        uint32_t ConsumeLocalToHostBytes(uint8_t *, uint32_t) override
        {
            fail("local-to-host read");
            return 0u;
        }
        uint32_t ReadVram(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) const override
        {
            fail("CPU VRAM read");
            return 0u;
        }
        void WriteVram(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) override { fail("CPU VRAM write"); }
        void SnapshotVram(std::vector<uint8_t> &out) const override
        {
            fail("CPU VRAM snapshot");
            out.clear();
        }
        GSTransferSnapshot GetTransferSnapshot() const override { return {}; }

    private:
        static void fail(const char *what)
        {
            if (!g_gpuFailed.exchange(true))
                std::fprintf(stderr, "[gpu] %s is not supported with MAXIMO_GS_BACKEND=gpu; stopping\n", what);
            if (g_runtime)
                g_runtime->requestStop();
        }
    };
#endif
    uint8_t *g_rdram = nullptr;
    fs::path g_disc;

    uint32_t reg(R5900Context *ctx, int r) { return GPR_U32(ctx, r); }

    void ret(R5900Context *ctx, int32_t v0)
    {
        SET_GPR_S32(ctx, 2, v0);
        ctx->pc = GPR_U32(ctx, 31);
    }

    uint8_t *mem(uint32_t addr) { return g_rdram + (addr & PS2_RAM_MASK); }

    uint32_t rd32(uint32_t addr)
    {
        uint32_t v;
        std::memcpy(&v, mem(addr), 4);
        return v;
    }

    void wr32(uint32_t addr, uint32_t v) { std::memcpy(mem(addr), &v, 4); }

    std::string cstr(uint32_t addr, size_t max = 256)
    {
        std::string s;
        if (addr == 0)
            return "(null)";
        for (size_t i = 0; i < max; ++i)
        {
            const char c = static_cast<char>(*mem(addr + static_cast<uint32_t>(i)));
            if (c == 0)
                break;
            s += c;
        }
        return s;
    }

    std::string printable(std::string s)
    {
        for (char &c : s)
            if (c == '\n' || c == '\r')
                c = '|';
            else if (static_cast<unsigned char>(c) < 0x20)
                c = '.';
        return s;
    }

    // ---- execution trace: every guest function entry passes through traceEntry ------------------

    std::vector<PS2Runtime::RecompiledFunction> g_original;
    std::vector<uint32_t> g_entryCount;
    std::array<uint32_t, 128> g_ring{};
    uint32_t g_ringPos = 0;
    uint32_t g_distinct = 0;
    uint32_t g_lastGameFn = 0; // last entered function below the SDK range
    constexpr uint32_t kSdkStart = 0x00190000u;

    uint32_t slotOf(uint32_t pc) { return (pc - g_ps2RecompiledFunctionTableBase) >> 2; }

    // Interrupt handlers (and what they call) are counted but kept out of the ring, so the ring
    // shows what the main thread was doing.
    bool g_inHandler = false;
    bool isHandlerEntry(uint32_t pc) { return pc == 0x00160960u || pc == 0x00160B40u; }

    // Memory guard: a word that must not change (set from the command line); checked on every
    // function entry, so the first entry after a stray write is reported with the call ring.
    uint32_t g_guardAddr = 0, g_guardValue = 0;
    bool g_guardTripped = false;
    void dumpRing(const char *why);

    void checkGuard(uint32_t pc)
    {
        if (g_guardAddr == 0 || g_guardTripped)
            return;
        uint32_t v;
        std::memcpy(&v, g_rdram + g_guardAddr, 4);
        if (v != g_guardValue)
        {
            g_guardTripped = true;
            logf("GUARD 0x%06X changed 0x%08X -> 0x%08X, noticed on entry to 0x%06X", g_guardAddr, g_guardValue, v, pc);
            dumpRing("guard");
        }
    }

    // Report a write of [dst, dst+len) that covers the guarded word, before it happens.
    void checkGuardWrite(const char *who, uint32_t dst, uint32_t len, uint32_t ra)
    {
        if (g_guardAddr != 0 && g_guardAddr >= (dst & PS2_RAM_MASK) && g_guardAddr < (dst & PS2_RAM_MASK) + len)
            logf("GUARD write by %s: dst=0x%07X len=0x%X covers 0x%06X [ra=0x%06X]", who, dst, len, g_guardAddr, ra);
    }

    // Value watch (diagnostic): MAXIMO_VALWATCH=first:last:addr[,addr...] reports every change of these words, as
    // noticed on function entry, while the pad-read clock is in [first, last]. Unlike the guard it re-arms.
    uint64_t g_padReads = 0; // scePadRead calls so far: the scripted-input clock
    struct ValWatch
    {
        uint32_t addr, value;
    };
    std::vector<ValWatch> g_valWatches;
    uint64_t g_valWatchFirst = 0, g_valWatchLast = 0;
    uint32_t g_valWatchPrevPc = 0, g_valWatchPrevRa = 0;

    void checkValWatches(uint32_t pc, uint32_t ra)
    {
        if (g_valWatches.empty() || g_padReads < g_valWatchFirst || g_padReads > g_valWatchLast)
            return;
        for (ValWatch &w : g_valWatches)
        {
            uint32_t v;
            std::memcpy(&v, g_rdram + w.addr, 4);
            if (v == w.value)
                continue;
            float before, after;
            std::memcpy(&before, &w.value, 4);
            std::memcpy(&after, &v, 4);
            logf("VALWATCH #%llu 0x%06X %g -> %g (0x%08X), noticed entering 0x%06X [ra=0x%06X], previous entry 0x%06X "
                 "[ra=0x%06X]",
                 static_cast<unsigned long long>(g_padReads), w.addr, before, after, v, pc, ra, g_valWatchPrevPc,
                 g_valWatchPrevRa);
            w.value = v;
        }
        g_valWatchPrevPc = pc;
        g_valWatchPrevRa = ra;
    }

    void traceEntry(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t pc = ctx->pc;
        checkGuard(pc);
        checkValWatches(pc, reg(ctx, 31));
        if (pc == 0x0019B100u) // memcpy(dst, src, n)
            checkGuardWrite("memcpy", reg(ctx, 4), reg(ctx, 6), reg(ctx, 31));
        const uint32_t slot = slotOf(pc);
        if (g_entryCount[slot]++ == 0)
            ++g_distinct;
        const bool handler = isHandlerEntry(pc);
        if (handler)
            g_inHandler = true;
        if (!g_inHandler)
        {
            g_ring[g_ringPos++ % g_ring.size()] = pc;
            if (pc < kSdkStart)
                g_lastGameFn = pc;
        }
        g_original[slot](rdram, ctx, runtime);
        if (handler)
            g_inHandler = false;
    }

    // Milestones: the steps of main (0x1000C0) in order. Entering step N+1 proves step N returned.
    struct Milestone
    {
        uint32_t addr;
        const char *what;
        bool reached = false;
    };
    std::vector<Milestone> g_milestones = {
        {0x00100008u, "_start (crt0)"},
        {0x001ACCF0u, "crt0: libc/kernel init"},
        {0x001000C0u, "main"},
        {0x0016D450u, "main: engine init"},
        {0x001B3960u, "main: sceSifRebootIop(IOPRP22.IMG)"},
        {0x001B3460u, "main: first sceSifLoadModule"},
        {0x00158260u, "main: graphics init (GS/DMA setup)"},
        {0x001C0110u, "main: scePadInit"},
        {0x001C0370u, "main: scePadPortOpen"},
        {0x001B6D40u, "main: sceMcInit"},
        {0x00147860u, "main: font load (PSXDATA\\GAME\\FONT.PRS/PRT)"},
        {0x001202C0u, "main: FLAG.PSX load"},
        {0x0016D760u, "main: sound init"},
        {0x0014B2C0u, "main: fade"},
        {0x0011F160u, "main: frontend"},
        {0x0014AD60u, "main: post-frontend"},
        {0x00147FE0u, "main: 0x147FE0"},
        {0x0014AEE0u, "main: game session"},
        {0x001C0558u, "main: shutdown (scePadPortClose)"},
        {0x001ABAC0u, "exit() (main returned or exit called)"},
        {0x00197F50u, "abort()"},
    };

    void milestoneEntry(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t pc = ctx->pc;
        const uint32_t ra = reg(ctx, 31);
        const bool fromMain = ra >= 0x001000C0u && ra < 0x00100450u;
        for (Milestone &m : g_milestones)
            if (m.addr == pc && !m.reached && (fromMain || std::strncmp(m.what, "main:", 5) != 0))
            {
                m.reached = true;
                logf("MILESTONE 0x%06X %s (ra=0x%06X)", pc, m.what, reg(ctx, 31));
            }
        traceEntry(rdram, ctx, runtime);
    }

    // Watch points: game functions whose entry (and arguments) are logged on every call, capped.
    struct Watch
    {
        const char *what;
        int nargs;
        bool logReturn = false; // log v0 whenever it differs from the previous completed call
        uint64_t calls = 0;
        int64_t lastReturn = INT64_MIN;
    };
    // Frame timing. A frame is the interval between entries to sceGsSyncV (0x1AAA18), which the game calls once
    // per displayed frame (0x158430). The wait runs from that entry to the next VBlank (the runtime timestamps
    // it); "busy" is the rest of the frame, split into VU1 and GS (runtime profile scopes) and EE (the remainder).
    // The wrapper cannot time the call itself: the dispatcher leaves the polling loop at cycle checkpoints.
    // With the GS worker (PS2X_GS_THREAD=1) GS time is spent on the worker, in parallel; the game thread's share
    // is the stall (kGsWait) it spends waiting on the worker, and EE is busy minus VU1 minus that stall.
    // With the VU1 worker (PS2X_VU1_THREAD=1) VU1 and GS time are both spent on the worker; the game thread's share
    // is its wait for the worker (kVu1Wait, reported in the VU1 column) and EE is busy minus that wait.
    struct FrameStats
    {
        uint64_t frames = 0, slowFrames = 0;
        double frameMs = 0, worstFrameMs = 0, busyMs = 0, worstBusyMs = 0, waitMs = 0, vu1Ms = 0, gsMs = 0, stallMs = 0;
        double worstVu1Ms = 0, worstGsMs = 0; // split of the worst busy frame
        double vu1WaitMs = 0;                 // game thread waiting for the VU1 worker
        uint64_t presentNs0 = 0;
        uint64_t vblanks = 0; // guest VBlanks elapsed over these frames (guest time, see logFrameStats)
        std::vector<float> frameSamples, busySamples; // per frame, for percentiles
    };
    FrameStats g_perfWindow, g_perfTotal;
    uint64_t g_lastSyncEntryNs = 0, g_perfWindowStartNs = 0, g_lastVu1Ns = 0, g_lastGsNs = 0, g_lastStallNs = 0;
    uint64_t g_lastVBlankTick = 0;
    bool g_asyncGs = false;
    bool g_vu1Thread = false;
    uint64_t g_lastVu1WaitNs = 0;
    double vblankMs() // the runtime's current field period (PAL 20 ms, NTSC 16.7 ms)
    {
        const double hz = g_runtime ? g_runtime->eeScheduler().vblankHz() : 0.0;
        return hz > 0.0 ? 1000.0 / hz : 1000.0 / 60.0;
    }

    void addFrame(FrameStats &s, double frameMs, double waitMs, double vu1Ms, double gsMs, double stallMs,
                  uint64_t vblanks, double vu1WaitMs)
    {
        const double busyMs = frameMs - waitMs;
        s.vu1WaitMs += vu1WaitMs;
        s.frameSamples.push_back(static_cast<float>(frameMs));
        s.busySamples.push_back(static_cast<float>(busyMs));
        ++s.frames;
        s.vblanks += vblanks;
        s.frameMs += frameMs;
        s.waitMs += waitMs;
        s.busyMs += busyMs;
        s.vu1Ms += vu1Ms;
        s.gsMs += gsMs;
        s.stallMs += stallMs;
        s.worstFrameMs = std::max(s.worstFrameMs, frameMs);
        if (frameMs > vblankMs() * 1.5)
            ++s.slowFrames;
        if (busyMs > s.worstBusyMs)
        {
            s.worstBusyMs = busyMs;
            s.worstVu1Ms = vu1Ms;
            s.worstGsMs = gsMs;
        }
    }

    double percentile(std::vector<float> samples, double q)
    {
        if (samples.empty())
            return 0.0;
        const size_t k = static_cast<size_t>(q * static_cast<double>(samples.size() - 1));
        std::nth_element(samples.begin(), samples.begin() + static_cast<std::ptrdiff_t>(k), samples.end());
        return samples[k];
    }

    uint64_t mmioAccesses(uint32_t phys, bool writes); // MMIO hook counters, defined with g_mmio below
    void logAudioWindow(const char *label, double seconds); // [audio-perf], defined with the audio section below

    void logFrameStats(const char *label, const FrameStats &s, double seconds)
    {
        if (s.frames == 0)
            return;
        const double n = static_cast<double>(s.frames);
        const double presentMs =
            static_cast<double>(ps2_host_profile::total(ps2_host_profile::kPresent) - s.presentNs0) / 1e6;
        if (g_vu1Thread)
        {
            // Same layout as inline, so the bench parsers keep working: VU1 is the game thread's wait for the VU1
            // worker and GS is 0 (both run on the worker; their times are on the next line).
            logf("[perf] %s frames=%llu fps=%.1f frame avg=%.2fms worst=%.2fms slow(>1.5 VBlank)=%llu | busy avg=%.2fms "
                 "(EE %.2f, VU1 %.2f, GS %.2f) worst=%.2fms (VU1 %.2f, GS %.2f) | VBlank wait avg=%.2fms | "
                 "present %.2fms/s",
                 label, static_cast<unsigned long long>(s.frames), n / seconds, s.frameMs / n, s.worstFrameMs,
                 static_cast<unsigned long long>(s.slowFrames), s.busyMs / n, (s.busyMs - s.vu1WaitMs) / n,
                 s.vu1WaitMs / n, 0.0, s.worstBusyMs, s.worstVu1Ms, s.worstGsMs, s.waitMs / n, presentMs / seconds);
            logf("[perf] %s VU1 worker (in parallel): VU1 %.2f GS %.2f ms/frame | game thread waits for it %.2f ms/frame",
                 label, s.vu1Ms / n, s.gsMs / n, s.vu1WaitMs / n);
        }
        else
        logf("[perf] %s frames=%llu fps=%.1f frame avg=%.2fms worst=%.2fms slow(>1.5 VBlank)=%llu | busy avg=%.2fms "
             "(EE %.2f, VU1 %.2f, %s %.2f) worst=%.2fms (VU1 %.2f, GS %.2f) | VBlank wait avg=%.2fms | "
             "present %.2fms/s",
             label, static_cast<unsigned long long>(s.frames), n / seconds, s.frameMs / n, s.worstFrameMs,
             static_cast<unsigned long long>(s.slowFrames), s.busyMs / n,
             (s.busyMs - s.vu1Ms - (g_asyncGs ? s.stallMs : s.gsMs)) / n, s.vu1Ms / n,
             g_asyncGs ? "GS stall" : "GS", (g_asyncGs ? s.stallMs : s.gsMs) / n, s.worstBusyMs, s.worstVu1Ms,
             s.worstGsMs, s.waitMs / n, presentMs / seconds);
        logf("[perf] %s p95 frame=%.2fms busy=%.2fms | p99 frame=%.2fms busy=%.2fms", label,
             percentile(s.frameSamples, 0.95), percentile(s.busySamples, 0.95), percentile(s.frameSamples, 0.99),
             percentile(s.busySamples, 0.99));
        if (g_asyncGs)
            logf("[perf] %s GS worker avg=%.2fms/frame (in parallel with the game thread)", label, s.gsMs / n);
        {
            const VU1Interpreter::AotStats a = VU1Interpreter::aotStats();
            if (a.calls != 0 || a.verified != 0)
                logf("[perf] %s VU1 AOT totals: calls=%llu completed=%llu handovers=%llu notCompiled=%llu verified=%llu "
                     "mismatches=%llu cycles compiled=%llu interpreted=%llu suspended=%llu resumed=%llu",
                     label, static_cast<unsigned long long>(a.calls), static_cast<unsigned long long>(a.completed),
                     static_cast<unsigned long long>(a.handovers), static_cast<unsigned long long>(a.notCompiled),
                     static_cast<unsigned long long>(a.verified), static_cast<unsigned long long>(a.mismatches),
                     static_cast<unsigned long long>(a.compiledCycles),
                     static_cast<unsigned long long>(a.interpretedCycles), static_cast<unsigned long long>(a.suspended),
                     static_cast<unsigned long long>(a.resumed));
        }
        // Guest time: the game runs at full speed when guest VBlanks elapse at the video mode's field rate in wall
        // time. Rendered frames per second alone do not show this (a game may draw once per two fields).
        const double hz = g_runtime ? g_runtime->eeScheduler().vblankHz() : 0.0;
        if (hz > 0.0)
            logf("[perf] %s guest vblanks=%llu (%.2f per frame) guest time %.2fs in %.2fs wall: speed %.1f%% of %.2f Hz",
                 label, static_cast<unsigned long long>(s.vblanks), s.vblanks / n, s.vblanks / hz, seconds,
                 100.0 * s.vblanks / hz / seconds, hz);
    }

    std::atomic<HANDLE> g_gameThread{nullptr}; // the host thread running guest code, for the sampling profiler
    std::atomic<uint64_t> g_guestFrame{0};     // sceGsSyncV calls so far, for MAXIMO_PROFILE=f<first>,<last>
    uint64_t g_lastGpuVsyncTick = 0;           // VBlank tick of the last GPU device vsync (game thread only)

    // ---- widescreen (MAXIMO_ASPECT=16:9, WIDESCREEN.md) -----------------------------------------------------------
    // func_15B100 builds the 3D camera's projection; its only caller (0x147860) passes the horizontal scale 1.0 in
    // $f13. Scaling it by (4/3)/(16/9) shows more at the sides and keeps the vertical framing (Hor+).
    constexpr float kWideXScale = 0.75f;
    // func_16AE00 sets up the visibility frustum whenever the 3D world is drawn. It reads the horizontal angle
    // factor (1.4) from this data word; widened by 4/3 so objects at the new screen edges are not culled.
    constexpr uint32_t kCullFactorAddr = 0x001E25F4u;
    constexpr float kWideCullFactor = 1.4f * 4.0f / 3.0f;
    bool g_widescreen = false;

    void wideCameraEntry(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ctx->f[13] *= kWideXScale;
        g_original[slotOf(ctx->pc)](rdram, ctx, runtime);
    }

    // Also the presenter's sign that this frame shows the widescreen 3D view (ps2_display_aspect.h).
    void wideCullEntry(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        std::memcpy(rdram + kCullFactorAddr, &kWideCullFactor, sizeof(kWideCullFactor));
        ps2_display_aspect::noteWideFrame();
        g_original[slotOf(ctx->pc)](rdram, ctx, runtime);
    }

    // func_17E8C0 draws the full-screen flash (lightning; func_17E6C0 starts it, func_17E7B0 fades it): a quad of
    // +-0.5 in the screen view, scaled by 2.0 with func_15AF20 to cover the 4:3 picture. The widened camera leaves it
    // a 4:3 square in the middle, so its horizontal scale is widened back by 4/3.
    constexpr uint32_t kScaleMatrixFn = 0x0015AF20u;
    constexpr uint32_t kFlashScaleRa = 0x0017E90Cu;

    void wideFlashEntry(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (reg(ctx, 31) == kFlashScaleRa)
            ctx->f[12] /= kWideXScale;
        g_original[slotOf(ctx->pc)](rdram, ctx, runtime);
    }

    // func_148470 draws the in-level overlay layer (HUD; the darkened backdrop and menu while paused). Its two
    // callers are the in-level frame loops, so it runs on every gameplay and pause frame but not on the title,
    // loading screens or movies. The pause screen stops drawing the world (no func_16AE00), so without this the
    // paused 16:9 picture would be presented squeezed into 4:3.
    void wideOverlayEntry(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_display_aspect::noteWideFrame();
        g_original[slotOf(ctx->pc)](rdram, ctx, runtime);
    }

    // ---- widescreen HUD -----------------------------------------------------------------------------------------------
    // func_14AEE0 draws the screen-space layer of every screen: HUD models, then the level's stencil shadow volumes, then
    // the text list. Both HUD and text go through the 3D screen transform that wideCameraEntry narrows, so in 16:9 they
    // already keep their 4:3 proportions, grouped in the middle of the picture. The loading screen (func_1482A0) draws
    // its shield and caption the same way. Both mark the frame as widescreen, so a loading screen without a 3D world
    // is shown 16:9 too; logos, title screens and movies draw neither and stay 4:3. What is left is moving the gameplay
    // HUD out to the screen edges.
    //
    // The HUD models are a tree under renderer +0x6878 (func_16A6B0's root), the loading screen's under +0x7910, both
    // drawn by func_1670E0; node +0x30 is the child count, +0x38 the child array, +0x60 the node's absolute matrix,
    // which func_1670E0 selects with func_15FF00 right before drawing the node. HUD units are 4:3 pixels at depth 362
    // (x = -320..320 is the 4:3 width). Each top-level group of a root (the gauge, the counters, the item ring...) that
    // is laid out from a screen edge (its own origin at the edge, like the item ring at x = -320) or lies wholly left or
    // right of the centre gets copies of its node matrices moved toward that edge by a quarter of the 16:9 buffer's
    // half width: its distance from the screen edge is then the same as in 4:3, and its size and internal layout are
    // unchanged. Other groups and all text (menus, dialogue, level titles) stay centred. The renderer reads selected
    // matrices by address when it sends the frame, so the copies live in their own memory, double-buffered per frame.
    constexpr uint32_t kHudRootOffset = 0x6878u;
    constexpr uint32_t kLoadingRootOffset = 0x7910u;
    constexpr uint32_t kHudViewOffset = 0x6C90u;
    constexpr uint32_t kSetViewFn = 0x0015FF30u;
    constexpr uint32_t kSetModelFn = 0x0015FF00u;
    constexpr uint32_t kLayerStartRa = 0x0014AF34u;   // func_14AEE0's func_15FF30 call (HUD view; renderer in $s3)
    constexpr uint32_t kLoadingStartRa = 0x00148360u; // func_1482A0's func_15FF30 call (HUD view; renderer in $s1)
    constexpr uint32_t kNodeMatrixRa = 0x0016722Cu;   // func_1670E0's func_15FF00(node + 0x60) call
    constexpr uint32_t kAnchorPool = 0x00074000u;     // 2 x 256 matrices in the EE kernel area, unused by game and runtime
    constexpr uint32_t kAnchorPoolSlots = 256u;
    constexpr float kHudHalfWidth = 320.0f;
    constexpr float kHudDepth = 362.0f;
    constexpr float kHudCentreMargin = 32.0f; // a group reaching further than this past the centre stays centred
    constexpr float kHudEdgeOrigin = 300.0f;  // a group whose own origin is this far out is laid out from that edge
    bool g_hudWide = false;                   // the picture is 16:9: move the HUD's side groups to the edges
    bool g_hudEdges = true;                   // MAXIMO_WIDE_HUD=0 leaves the HUD in the 4:3 area
    std::unordered_map<uint32_t, int> g_hudAnchors; // node -> -1 left edge, +1 right edge
    uint32_t g_hudView = 0;
    uint32_t g_anchorPoolBase = kAnchorPool;
    uint32_t g_anchorSlot = 0;

    float guestFloat(uint8_t *rdram, uint32_t address)
    {
        float f;
        std::memcpy(&f, rdram + (address & 0x1FFFFFFu), sizeof(f));
        return f;
    }

    uint32_t guestWord(uint8_t *rdram, uint32_t address)
    {
        uint32_t w;
        std::memcpy(&w, rdram + (address & 0x1FFFFFFu), sizeof(w));
        return w;
    }

    template <typename Visit> void forEachHudNode(uint8_t *rdram, uint32_t node, int depth, const Visit &visit)
    {
        if (!node || depth > 16)
            return;
        visit(node);
        const uint32_t count = guestWord(rdram, node + 0x30);
        const uint32_t children = guestWord(rdram, node + 0x38);
        for (uint32_t i = 0; i < count && i < 256; ++i)
            forEachHudNode(rdram, guestWord(rdram, children + i * 4), depth + 1, visit);
    }

    void classifyHudGroups(uint8_t *rdram, uint32_t renderer, uint32_t rootOffset)
    {
        g_hudAnchors.clear();
        g_hudView = renderer + kHudViewOffset;
        g_anchorPoolBase = g_anchorPoolBase == kAnchorPool ? kAnchorPool + kAnchorPoolSlots * 64 : kAnchorPool;
        g_anchorSlot = 0;
        const uint32_t root = guestWord(rdram, renderer + rootOffset);
        if (!root)
            return;
        const uint32_t count = guestWord(rdram, root + 0x30);
        const uint32_t children = guestWord(rdram, root + 0x38);
        for (uint32_t i = 0; i < count && i < 256; ++i)
        {
            const uint32_t group = guestWord(rdram, children + i * 4);
            float lo = 1e9f, hi = -1e9f;
            forEachHudNode(rdram, group, 0, [&](uint32_t node) {
                if (guestWord(rdram, node + 0x30) != 0)
                    return;
                const float x = guestFloat(rdram, node + 0x60 + 48);
                if (std::isfinite(x))
                {
                    lo = std::min(lo, x);
                    hi = std::max(hi, x);
                }
            });
            const float origin = guestFloat(rdram, group + 0x60 + 48);
            int side = 0;
            if (std::isfinite(origin) && std::fabs(origin) >= kHudEdgeOrigin)
                side = origin < 0.0f ? -1 : 1;
            else if (lo <= hi && hi <= kHudCentreMargin && lo < -kHudCentreMargin)
                side = -1;
            else if (lo <= hi && lo >= -kHudCentreMargin && hi > kHudCentreMargin)
                side = 1;
            if (side)
                forEachHudNode(rdram, group, 0, [&](uint32_t node) { g_hudAnchors[node] = side; });
        }
    }

    // $a0 of func_1670E0's func_15FF00 call: a side group's node matrix is replaced by a moved copy.
    void anchorNodeMatrix(uint8_t *rdram, R5900Context *ctx)
    {
        const uint32_t matrix = reg(ctx, 4);
        const auto it = g_hudAnchors.find(matrix - 0x60);
        if (it == g_hudAnchors.end() || g_anchorSlot >= kAnchorPoolSlots)
            return;
        float m[16];
        std::memcpy(m, rdram + (matrix & 0x1FFFFFFu), sizeof(m));
        // The node's depth in the HUD view (column 2 of a row-vector matrix): under perspective the move scales with it.
        float depth = guestFloat(rdram, g_hudView + 48 + 8);
        for (int r = 0; r < 3; ++r)
            depth += m[12 + r] * guestFloat(rdram, g_hudView + r * 16 + 8);
        if (!std::isfinite(depth) || depth <= 0.0f)
            return;
        // A third of the half width in HUD units is a quarter of it once the screen transform narrows x by 3/4.
        m[12] += static_cast<float>(it->second) * (kHudHalfWidth / 3.0f) * (depth / kHudDepth);
        const uint32_t copy = g_anchorPoolBase + g_anchorSlot++ * 64;
        std::memcpy(rdram + copy, m, sizeof(m));
        SET_GPR_U32(ctx, 4, copy);
    }

    // func_15FF30 (view) and func_15FF00 (model matrix), acting only on the calls above.
    void wideHudEntry(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t ra = reg(ctx, 31);
        if (ctx->pc == kSetViewFn && (ra == kLayerStartRa || ra == kLoadingStartRa))
        {
            ps2_display_aspect::noteWideFrame();
            if (g_hudWide && g_hudEdges)
            {
                if (ra == kLayerStartRa)
                    classifyHudGroups(rdram, reg(ctx, 19), kHudRootOffset);
                else
                    classifyHudGroups(rdram, reg(ctx, 17), kLoadingRootOffset);
            }
        }
        else if (ctx->pc == kSetModelFn && ra == kNodeMatrixRa && g_hudWide && g_hudEdges)
            anchorNodeMatrix(rdram, ctx);
        g_original[slotOf(ctx->pc)](rdram, ctx, runtime);
    }

    // ---- movie bit reader (MAXIMO_MOVIE_BITS, MOVIE_SPEED.md) ----------------------------------------------------
    // The FMV player reads its stream through a byte-fed bit reader of tiny functions that it calls for nearly every
    // field: func_1BD870 (peek n bits), func_1BD890 (skip n bits, refill byte by byte), func_1BD928 (read n bits) and
    // func_1BD978 (read 1 bit). Their per-call overhead in the port dwarfs their work, so they run natively here, with
    // the same results and the same EE cycles charged (8 per call they made, 32 per refill loop back edge).
    // Reader: +0x00 u64 bit buffer (msb first), +0x0C u32 read pointer, +0x10 u32 bits in the buffer, +0x18 u64 bits
    // consumed, +0x20 ring start, +0x24 ring end. MAXIMO_MOVIE_BITS=0 keeps the game's code; =verify runs both and
    // logs any difference.
    constexpr uint32_t kBitsPeekFn = 0x001BD870u;
    constexpr uint32_t kBitsSkipFn = 0x001BD890u;
    constexpr uint32_t kBitsReadFn = 0x001BD928u;
    constexpr uint32_t kBitsReadBitFn = 0x001BD978u;
    bool g_movieBitsVerify = false;
    uint64_t g_movieBitsCalls = 0, g_movieBitsChecked = 0, g_movieBitsMismatches = 0;

    struct BitReader
    {
        uint64_t buffer, consumed;
        uint32_t ptr, count, start, end;
    };

    BitReader loadBitReader(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t r)
    {
        return {READ64(r), READ64(r + 0x18u), READ32(r + 0x0Cu), READ32(r + 0x10u), READ32(r + 0x20u),
                READ32(r + 0x24u)};
    }

    void storeBitReader(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t r, const BitReader &b)
    {
        WRITE64(r, b.buffer);
        WRITE32(r + 0x0Cu, b.ptr);
        WRITE32(r + 0x10u, b.count);
        WRITE64(r + 0x18u, b.consumed);
    }

    // func_1BD870: the top n bits, as a sign-extended 32-bit value.
    uint64_t bitsPeek(const BitReader &b, uint64_t n)
    {
        const uint64_t v = b.buffer >> ((64u - static_cast<uint32_t>(n)) & 63u);
        return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(v)));
    }

    // func_1BD890: drop n bits and refill to more than 56; returns the new consumed count. Counts loop back edges.
    template <typename ReadByte> uint64_t bitsSkip(BitReader &b, uint64_t n, ReadByte readByte, uint32_t &backEdges)
    {
        b.buffer <<= (n & 63u);
        b.count -= static_cast<uint32_t>(n);
        if (b.count < 57u)
        {
            for (;;)
            {
                b.buffer |= static_cast<uint64_t>(readByte(b.ptr)) << ((0x38u - b.count) & 63u);
                ++b.ptr;
                if (!(b.ptr < b.end))
                    b.ptr = b.start;
                b.count += 8u;
                if (b.count >= 57u)
                    break;
                ++backEdges;
            }
        }
        b.consumed += n;
        return b.consumed;
    }

    bool sameBitReader(const BitReader &a, const BitReader &b)
    {
        return a.buffer == b.buffer && a.consumed == b.consumed && a.ptr == b.ptr && a.count == b.count;
    }

    void movieBitsEntry(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t fn = ctx->pc;
        const uint32_t r = reg(ctx, 4);
        const uint64_t n = fn == kBitsReadBitFn ? 1u : GPR_U64(ctx, 5);
        BitReader b = loadBitReader(rdram, ctx, runtime, r);
        uint32_t backEdges = 0;
        const auto readByte = [&](uint32_t address) { return READ8(address); };
        uint64_t result;
        if (fn == kBitsPeekFn)
            result = bitsPeek(b, n);
        else if (fn == kBitsSkipFn)
            result = bitsSkip(b, n, readByte, backEdges);
        else
        {
            result = bitsPeek(b, n);
            bitsSkip(b, n, readByte, backEdges);
        }
        ++g_movieBitsCalls;
        if (g_movieBitsVerify)
        {
            const uint32_t ra = reg(ctx, 31);
            g_original[slotOf(fn)](rdram, ctx, runtime);
            if (ctx->pc != ra)
                return; // paused at a loop checkpoint; it resumes later from there and is not compared
            ++g_movieBitsChecked;
            const BitReader game = loadBitReader(rdram, ctx, runtime, r);
            if ((!sameBitReader(game, b) || GPR_U64(ctx, 2) != result) && ++g_movieBitsMismatches <= 16)
                logf("movie bits: 0x%06X(n=%llu) differs: v0 %016llX vs %016llX, buffer %016llX vs %016llX, ptr %08X vs "
                     "%08X, count %u vs %u, consumed %llu vs %llu",
                     fn, static_cast<unsigned long long>(n), static_cast<unsigned long long>(GPR_U64(ctx, 2)),
                     static_cast<unsigned long long>(result), static_cast<unsigned long long>(game.buffer),
                     static_cast<unsigned long long>(b.buffer), game.ptr, b.ptr, game.count, b.count,
                     static_cast<unsigned long long>(game.consumed), static_cast<unsigned long long>(b.consumed));
            return;
        }
        if (fn != kBitsPeekFn)
            storeBitReader(rdram, ctx, runtime, r, b);
        // The game's func_1BD928 and func_1BD978 call the other two (two dispatches of 8 cycles each).
        const uint32_t cycles = backEdges * 32u + (fn == kBitsReadFn || fn == kBitsReadBitFn ? 16u : 0u);
        if (cycles)
            runtime->eeScheduler().accountCycles(cycles);
        SET_GPR_U64(ctx, 2, result);
        ctx->pc = reg(ctx, 31);
    }

    // ---- movie packet headers (MAXIMO_MOVIE_BITS, MOVIE_SPEED.md) ------------------------------------------------
    // The player polls the demuxer func_1BDCD0 about 80,000 times a second while its video buffer is full. Each call
    // parses a pack header (func_1BE0D8, with func_1BE228 for a system header) and a PES packet header (func_1BE298)
    // through the bit reader, about 40 reader calls, before the stream callback turns the packet down. Those two
    // parsers run natively here with the same results and the same EE cycles charged: 8 per call they made, 16 more
    // for each func_1BD928/func_1BD978 (the two calls those make) and 32 per loop back edge. Where func_1BE298 prints
    // an error (a pack header inside a PES header) the game's code runs instead.
    constexpr uint32_t kPackHeaderFn = 0x001BE0D8u;
    constexpr uint32_t kPesHeaderFn = 0x001BE298u;
    constexpr uint32_t kPesFlagBitsTable = 0x001E70B0u; // bits taken by the ES rate, trick mode, copy info and CRC fields
    uint64_t g_movieHeaderCalls = 0, g_movieHeaderChecked = 0, g_movieHeaderMismatches = 0;

    // A copy of the game's bit reader (+0x08 start of the stream, +0x28 ring size) with the cost of the calls made.
    struct HeaderBits
    {
        uint8_t *rdram;
        R5900Context *ctx;
        PS2Runtime *runtime;
        BitReader b;
        uint32_t streamStart, ringSize;
        uint32_t cycles = 0;

        uint64_t peek(uint64_t n)
        {
            cycles += 8u;
            return bitsPeek(b, n);
        }
        uint64_t read(uint64_t n) // func_1BD928, and func_1BD978 for n = 1
        {
            const uint64_t v = bitsPeek(b, n);
            uint32_t backEdges = 0;
            bitsSkip(b, n, [&](uint32_t address) { return READ8(address); }, backEdges);
            cycles += 24u + backEdges * 32u;
            return v;
        }
        void backEdge() { cycles += 32u; }
        uint8_t byte(uint32_t address) { return READ8(address); }
        // func_1BD9C0: move n bytes on (n may be negative), then refill the empty buffer through func_1BD890.
        void skipBytes(int32_t n)
        {
            cycles += 8u;
            b.consumed += static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(static_cast<uint32_t>(n) << 3)));
            b.ptr = streamStart + static_cast<uint32_t>(b.consumed >> 3);
            if (!(b.ptr < b.end))
                b.ptr -= ringSize;
            b.buffer = 0;
            b.count = 0;
            uint32_t backEdges = 0;
            bitsSkip(b, 0, [&](uint32_t address) { return READ8(address); }, backEdges);
            cycles += backEdges * 32u;
        }
    };

    // The fields a header parser stores, applied (or compared) after it has run.
    struct HeaderOut
    {
        struct Field
        {
            uint32_t offset, size;
            uint64_t value;
        };
        Field fields[8];
        uint32_t n = 0;

        void set(uint32_t offset, uint32_t size, uint64_t value)
        {
            for (uint32_t i = 0; i < n; ++i)
                if (fields[i].offset == offset)
                {
                    fields[i].value = value;
                    return;
                }
            fields[n++] = {offset, size, value};
        }
    };

    // func_1BE298 timestamp: 4 bits, then 3, 15 and 15 bits of the 33-bit value, each followed by a marker bit.
    uint64_t readTimestamp(HeaderBits &h)
    {
        h.read(4);
        const uint32_t hi = static_cast<uint32_t>(h.read(3));
        h.read(1);
        const uint32_t mid = static_cast<uint32_t>(h.read(15));
        h.read(1);
        const uint32_t lo = static_cast<uint32_t>(h.read(15));
        h.read(1);
        return (static_cast<uint64_t>((hi >> 2) & 1u) << 32) | ((hi << 30) | (mid << 15) | lo);
    }

    // func_1BE298(reader, out): a PES packet header, skipping the payload. Out: +0x00 stream id << 32 (| sub-stream
    // word for 0xBD and 0xBF), +0x08 length, +0x0C scrambling, +0x10 PTS, +0x18 DTS (-1 if none), +0x20 bit position of
    // the payload, +0x24 payload length - 3, +0x28 bit position of the packet. False where the game prints an error.
    bool parsePesHeader(HeaderBits &h, HeaderOut &o)
    {
        o.set(0x28, 4, static_cast<uint32_t>(h.b.consumed));
        h.read(24);
        const uint64_t id = h.read(8) << 32;
        o.set(0x00, 8, id);
        const uint32_t length = static_cast<uint32_t>(h.read(16));
        o.set(0x08, 4, length);
        o.set(0x10, 8, ~0ull);
        o.set(0x18, 8, ~0ull);
        switch (id >> 32)
        {
        case 0xBC: case 0xBE: case 0xBF: case 0xF0: case 0xF1: case 0xF2: case 0xF8: case 0xFF:
        {
            uint32_t rest = length;
            if (id >> 32 == 0xBF)
            {
                o.set(0x00, 8, id | static_cast<uint32_t>(h.read(32)));
                rest -= 4u;
            }
            if (rest)
                h.skipBytes(static_cast<int32_t>(rest));
            return true;
        }
        default:
            break;
        }
        h.read(2);
        o.set(0x0C, 4, static_cast<uint32_t>(h.read(2)));
        h.read(4);
        const uint64_t ptsDts = h.read(2);
        const uint64_t escr = h.read(1);
        const uint64_t flags = h.read(4);
        const uint64_t extension = h.read(1);
        const uint32_t headerLength = static_cast<uint32_t>(h.read(8));
        const uint64_t dataStart = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(h.b.consumed)));
        if (ptsDts & 2u)
            o.set(0x10, 8, readTimestamp(h));
        if (ptsDts == 3u)
            o.set(0x18, 8, readTimestamp(h));
        if (escr == 1u)
            h.read(48);
        if (flags)
            h.read(h.byte(kPesFlagBitsTable + static_cast<uint32_t>(flags)));
        if (extension == 1u)
        {
            const uint64_t privateData = h.read(1), packHeader = h.read(1), sequenceCounter = h.read(1),
                           pstdBuffer = h.read(1);
            h.read(3);
            const uint64_t extension2 = h.read(1);
            if (privateData == 1u)
            {
                h.read(48);
                h.read(48);
                h.read(32);
            }
            if (packHeader == 1u)
                return false;
            if (sequenceCounter == 1u)
                h.read(16);
            if (pstdBuffer == 1u)
                h.read(16);
            if (extension2 == 1u)
            {
                h.read(1);
                const uint32_t count = static_cast<uint32_t>(h.read(7));
                for (uint32_t i = 0; i < count; ++i)
                {
                    h.read(8);
                    if (i + 1u < count)
                        h.backEdge();
                }
            }
        }
        const uint32_t headerRead = static_cast<uint32_t>((h.b.consumed - dataStart) >> 3);
        if (const uint32_t rest = headerLength - headerRead)
            h.skipBytes(static_cast<int32_t>(rest));
        const uint32_t payload = length - headerLength;
        o.set(0x24, 4, payload - 3u);
        o.set(0x20, 4, static_cast<uint32_t>(h.b.consumed));
        uint32_t rest = payload - 3u;
        if (id >> 32 == 0xBD)
        {
            o.set(0x00, 8, id | static_cast<uint32_t>(h.read(32)));
            rest = payload - 7u;
        }
        if (rest)
            h.skipBytes(static_cast<int32_t>(rest));
        return true;
    }

    // func_1BE0D8(reader, out): a pack header, and the system header after it if there is one. Out: +0x00 SCR
    // extension, +0x04 SCR bits 0-31, +0x08 SCR bit 32, +0x0C 1 if a system header followed.
    bool parsePackHeader(HeaderBits &h, HeaderOut &o)
    {
        h.read(34);
        const uint32_t hi = static_cast<uint32_t>(h.read(3));
        h.read(1);
        const uint32_t mid = static_cast<uint32_t>(h.read(15));
        h.read(1);
        const uint32_t lo = static_cast<uint32_t>(h.read(15));
        h.read(1);
        o.set(0x00, 4, static_cast<uint32_t>(h.read(9)));
        h.read(30);
        const uint32_t stuffing = static_cast<uint32_t>(h.read(3));
        o.set(0x08, 4, (hi >> 2) & 1u);
        o.set(0x04, 4, (hi << 30) | (mid << 15) | lo);
        for (uint32_t i = 0; i < stuffing; ++i)
        {
            h.read(8);
            if (i + 1u < stuffing)
                h.backEdge();
        }
        if (h.peek(32) != 0x1BBu)
        {
            o.set(0x0C, 4, 0);
            return true;
        }
        o.set(0x0C, 4, 1);
        h.cycles += 8u; // func_1BE228: the system header, skipped
        h.read(56);
        h.read(40);
        while (h.peek(1) == 1u)
        {
            h.backEdge();
            h.read(24);
        }
        return true;
    }

#ifdef MAXIMO_HAS_FFMPEG
    // ---- movie pictures through FFmpeg (MAXIMO_MOVIE_FFMPEG, MOVIE_SPEED.md) ---------------------------------------
    // sceMpegGetPicture (func_1BEB40) decodes the next picture with FFmpeg instead of the game's libmpeg, which drives
    // the IPU macroblock by macroblock. The stream still arrives the game's way, DMA channel 4 from the video buffer
    // into the IPU input FIFO, and is taken from there in whole bytes (Ps2Ipu::peekInput), so the buffer accounting
    // and the timestamp lookup (func_190A20, which reads D4_MADR and IPU_BP) see the input consumed as libmpeg would.
    // What the game reads afterwards is what libmpeg leaves:
    //  - the picture as RGBA32 macroblocks in column order (x outer), through the IPU's colour conversion with alpha
    //    0x80 (the movies set no thresholds);
    //  - sceMpeg +0 width, +4 height, +8 pictures returned before this one, +0x10/+0x18 PTS/DTS from this picture's
    //    timestamp callback, +0x20 flags 0x1D8 | picture type (unused by the player); sys +0 end of stream (set with
    //    the last picture), +4 picture headers read, +8 = 2;
    //  - the timestamp callback (type 5) once per picture header, with the input consumed up to its start code, and
    //    the no-data callback (type 1) whenever the input runs dry. Callbacks run as guest code: ra points back at
    //    func_1BEB40, where the decoder resumes from its saved state once the callback returns.
    // sceMpegCreate (func_1BE8C0) and sceMpegReset (func_1BEC78) also reset the decoder.
    constexpr uint32_t kGetPictureFn = 0x001BEB40u;
    constexpr uint32_t kMpegCreateFn = 0x001BE8C0u;
    constexpr uint32_t kMpegResetFn = 0x001BEC78u;
    constexpr uint32_t kMpegCbNodata = 1u, kMpegCbTimeStamp = 5u;
    constexpr uint32_t kMpegCbFrame = 0x40u; // the callback's data block, below the caller's stack pointer

    struct MovieDecoder
    {
        AVCodecContext *codec = nullptr;
        AVPacket *packet = nullptr;
        std::deque<AVFrame *> frames;       // decoded, in display order
        std::vector<uint8_t> unit;          // the access unit being gathered, from its first start code
        uint32_t scan = 0xFFFFFFFFu;        // the last four stream bytes, for start codes
        bool inSlices = false;              // slice data seen since the unit's picture header
        bool ended = false;                 // sequence end code read and the decoder drained
        int64_t unitPicture = -1;           // decode index of the unit's picture
        struct Stamp
        {
            int64_t pts = -1, dts = -1;
        };
        std::vector<Stamp> stamps;          // by decode index
        uint32_t returned = 0;
        // The call in progress while a callback runs.
        uint32_t pending = 0;               // callback type, 0 = none
        uint32_t mp = 0, buf = 0, mbCount = 0, ra = 0, sp = 0;
        uint64_t pictures = 0, units = 0, errors = 0; // totals for the exit summary
    };
    MovieDecoder g_movie;
    bool g_movieFfmpeg = false;

    void movieDecoderReset(MovieDecoder &d)
    {
        for (AVFrame *f : d.frames)
            av_frame_free(&f);
        d.frames.clear();
        if (d.codec)
            avcodec_flush_buffers(d.codec);
        d.unit.clear();
        d.scan = 0xFFFFFFFFu;
        d.inSlices = d.ended = false;
        d.unitPicture = -1;
        d.stamps.clear();
        d.returned = 0;
        d.pending = 0;
    }

    bool movieDecoderOpen(MovieDecoder &d)
    {
        av_log_set_level(AV_LOG_ERROR);
        const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_MPEG2VIDEO);
        d.codec = codec ? avcodec_alloc_context3(codec) : nullptr;
        d.packet = av_packet_alloc();
        if (!d.codec || !d.packet)
            return false;
        d.codec->thread_count = 1;
        return avcodec_open2(d.codec, codec, nullptr) == 0;
    }

    void movieReceiveFrames(MovieDecoder &d)
    {
        for (;;)
        {
            AVFrame *f = av_frame_alloc();
            if (avcodec_receive_frame(d.codec, f) != 0)
            {
                av_frame_free(&f);
                return;
            }
            d.frames.push_back(f);
        }
    }

    // Sends the first n bytes of the unit (one picture with the headers before it) to the decoder.
    void movieSendUnit(MovieDecoder &d, size_t n)
    {
        if (d.unitPicture >= 0 && n != 0u)
        {
            d.packet->data = d.unit.data();
            d.packet->size = static_cast<int>(n);
            d.packet->pts = d.unitPicture;
            if (avcodec_send_packet(d.codec, d.packet) != 0 && ++d.errors <= 8)
                logf("movie decoder: picture %lld rejected", static_cast<long long>(d.unitPicture));
            d.packet->data = nullptr;
            d.packet->size = 0;
            ++d.units;
            movieReceiveFrames(d);
        }
        d.unit.erase(d.unit.begin(), d.unit.begin() + static_cast<std::ptrdiff_t>(n));
        d.unitPicture = -1;
    }

    // Takes input until a picture header has been read (true: its timestamp callback is due), the sequence ends or
    // the input runs dry.
    bool movieReadInput(MovieDecoder &d, Ps2Ipu &ipu)
    {
        uint8_t window[160];
        while (!d.ended)
        {
            const uint32_t n = ipu.peekInput(window, sizeof window);
            if (n == 0u)
                return false;
            uint32_t used = 0;
            bool header = false;
            while (used < n && !header && !d.ended)
            {
                const uint8_t b = window[used++];
                d.unit.push_back(b);
                d.scan = (d.scan << 8) | b;
                if ((d.scan & 0xFFFFFF00u) != 0x00000100u)
                    continue;
                if (b >= 0x01u && b <= 0xAFu)
                {
                    d.inSlices = true;
                    continue;
                }
                if (d.inSlices && (b == 0x00u || b == 0xB3u || b == 0xB8u || b == 0xB7u))
                {
                    movieSendUnit(d, d.unit.size() - 4u); // the previous picture ends at this start code
                    d.inSlices = false;
                }
                if (b == 0x00u)
                {
                    d.unitPicture = static_cast<int64_t>(d.stamps.size());
                    d.stamps.emplace_back();
                    header = true;
                }
                else if (b == 0xB7u)
                {
                    d.unit.clear();
                    avcodec_send_packet(d.codec, nullptr);
                    movieReceiveFrames(d);
                    d.ended = true;
                }
            }
            ipu.skipInput(used);
            if (header)
                return true;
        }
        return false;
    }

    // One picture into the game's buffer: RGBA32 macroblocks in column order, through the IPU's colour conversion
    // (ps2_ipu::macroblockToRgb32: BT.601 with 7-bit coefficients, alpha 0x80), eight pixels at a time.
    void movieWritePicture(const AVFrame *f, uint32_t buf, uint32_t mbCount)
    {
        const int mbw = (f->width + 15) / 16, mbh = (f->height + 15) / 16;
        const uint32_t count = std::min<uint32_t>(mbCount, static_cast<uint32_t>(mbw * mbh));
        const uint32_t base = buf & PS2_RAM_MASK;
        if (base + count * 1024u > PS2_RAM_SIZE)
            return;
        const __m256i k16 = _mm256_set1_epi32(16), k128 = _mm256_set1_epi32(128), k64 = _mm256_set1_epi32(64);
        const __m256i zero = _mm256_setzero_si256(), max = _mm256_set1_epi32(255), alpha = _mm256_set1_epi32(INT32_MIN);
        const auto channel = [&](__m256i sum)
        { return _mm256_min_epi32(_mm256_max_epi32(_mm256_srai_epi32(_mm256_add_epi32(sum, k64), 7), zero), max); };
        for (uint32_t i = 0; i < count; ++i)
        {
            const int mx = static_cast<int>(i) / mbh, my = static_cast<int>(i) % mbh;
            uint32_t *out = reinterpret_cast<uint32_t *>(g_rdram + base + i * 1024u);
            for (int py = 0; py < 16; ++py)
            {
                const int y = std::min(my * 16 + py, f->height - 1);
                const uint8_t *lumRow = f->data[0] + y * f->linesize[0] + mx * 16;
                const uint8_t *cbRow = f->data[1] + (y >> 1) * f->linesize[1] + mx * 8;
                const uint8_t *crRow = f->data[2] + (y >> 1) * f->linesize[2] + mx * 8;
                for (int px = 0; px < 16; px += 8)
                {
                    const __m256i lum = _mm256_mullo_epi32(
                        _mm256_sub_epi32(_mm256_cvtepu8_epi32(_mm_loadl_epi64(reinterpret_cast<const __m128i *>(lumRow + px))), k16),
                        _mm256_set1_epi32(149));
                    int32_t cbBytes, crBytes;
                    std::memcpy(&cbBytes, cbRow + px / 2, 4);
                    std::memcpy(&crBytes, crRow + px / 2, 4);
                    const __m128i cb8 = _mm_cvtsi32_si128(cbBytes), cr8 = _mm_cvtsi32_si128(crBytes);
                    const __m256i cb = _mm256_sub_epi32(_mm256_cvtepu8_epi32(_mm_unpacklo_epi8(cb8, cb8)), k128);
                    const __m256i cr = _mm256_sub_epi32(_mm256_cvtepu8_epi32(_mm_unpacklo_epi8(cr8, cr8)), k128);
                    const __m256i r = channel(_mm256_add_epi32(lum, _mm256_mullo_epi32(cr, _mm256_set1_epi32(204))));
                    const __m256i g = channel(_mm256_sub_epi32(_mm256_sub_epi32(lum, _mm256_mullo_epi32(cb, _mm256_set1_epi32(50))),
                                                               _mm256_mullo_epi32(cr, _mm256_set1_epi32(104))));
                    const __m256i b = channel(_mm256_add_epi32(lum, _mm256_mullo_epi32(cb, _mm256_set1_epi32(258))));
                    const __m256i pixel = _mm256_or_si256(_mm256_or_si256(r, _mm256_slli_epi32(g, 8)),
                                                          _mm256_or_si256(_mm256_slli_epi32(b, 16), alpha));
                    _mm256_storeu_si256(reinterpret_cast<__m256i *>(out + py * 16 + px), pixel);
                }
            }
        }
    }

    // Runs mpeg callback `type` as guest code; func_1BEB40 resumes when it returns.
    void movieCallCallback(MovieDecoder &d, R5900Context *ctx, uint8_t *rdram, PS2Runtime *runtime, uint32_t type)
    {
        const uint32_t sys = READ32(d.mp + 0x40u);
        const uint32_t frame = d.sp - kMpegCbFrame;
        WRITE32(frame, type);
        WRITE64(frame + 0x08u, ~0ull);
        WRITE64(frame + 0x10u, ~0ull);
        SET_GPR_U32(ctx, 29, frame);
        SET_GPR_U32(ctx, 4, d.mp);
        SET_GPR_U32(ctx, 5, frame);
        SET_GPR_U32(ctx, 6, READ32(sys + 0x10u + type * 8u));
        SET_GPR_U32(ctx, 31, kGetPictureFn);
        ctx->pc = READ32(sys + 0x0Cu + type * 8u);
        d.pending = type;
    }

    void movieGetPictureEntry(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        MovieDecoder &d = g_movie;
        if (d.pending != 0u && reg(ctx, 29) == d.sp - kMpegCbFrame)
        {
            if (d.pending == kMpegCbTimeStamp && !d.stamps.empty())
                d.stamps.back() = {static_cast<int64_t>(READ64(d.sp - kMpegCbFrame + 0x08u)),
                                   static_cast<int64_t>(READ64(d.sp - kMpegCbFrame + 0x10u))};
            SET_GPR_U32(ctx, 29, d.sp);
            d.pending = 0;
        }
        else
        {
            d.pending = 0;
            d.mp = reg(ctx, 4);
            d.buf = reg(ctx, 5);
            d.mbCount = reg(ctx, 6);
            d.ra = reg(ctx, 31);
            d.sp = reg(ctx, 29);
        }
        const uint32_t sys = READ32(d.mp + 0x40u);
        Ps2Ipu &ipu = runtime->memory().ipu();
        while (d.frames.empty() && !d.ended)
        {
            const uint32_t callback = movieReadInput(d, ipu) ? kMpegCbTimeStamp : d.ended ? 0u : kMpegCbNodata;
            WRITE32(sys + 4u, static_cast<uint32_t>(d.stamps.size()));
            if (callback != 0u)
            {
                movieCallCallback(d, ctx, rdram, runtime, callback);
                return;
            }
        }
        if (d.frames.empty())
        {
            // The sequence ended with no picture left to return.
            WRITE32(sys, 1u);
            SET_GPR_U64(ctx, 2, ~0ull);
            ctx->pc = d.ra;
            return;
        }
        AVFrame *f = d.frames.front();
        d.frames.pop_front();
        movieWritePicture(f, d.buf, d.mbCount);
        const MovieDecoder::Stamp stamp = f->pts >= 0 && static_cast<size_t>(f->pts) < d.stamps.size()
                                       ? d.stamps[static_cast<size_t>(f->pts)]
                                       : MovieDecoder::Stamp{};
        WRITE32(d.mp + 0x00u, static_cast<uint32_t>(f->width));
        WRITE32(d.mp + 0x04u, static_cast<uint32_t>(f->height));
        WRITE32(d.mp + 0x08u, d.returned);
        ++d.returned;
        WRITE64(d.mp + 0x10u, static_cast<uint64_t>(stamp.pts));
        WRITE64(d.mp + 0x18u, static_cast<uint64_t>(stamp.dts));
        WRITE64(d.mp + 0x20u, 0x1D8u | static_cast<uint32_t>(f->pict_type));
        WRITE32(sys + 0u, d.ended && d.frames.empty() ? 1u : 0u);
        WRITE32(sys + 4u, static_cast<uint32_t>(d.stamps.size()));
        WRITE32(sys + 8u, 2u);
        av_frame_free(&f);
        ++d.pictures;
        SET_GPR_U64(ctx, 2, 1u);
        ctx->pc = d.ra;
    }

    // sceMpegCreate, sceMpegReset: a new stream for the decoder too.
    void movieMpegResetEntry(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        movieDecoderReset(g_movie);
        g_original[slotOf(ctx->pc)](rdram, ctx, runtime);
    }
#endif

    void movieHeaderEntry(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t fn = ctx->pc;
        const uint32_t r = reg(ctx, 4), out = reg(ctx, 5), ra = reg(ctx, 31);
        HeaderBits h{rdram, ctx, runtime, loadBitReader(rdram, ctx, runtime, r), READ32(r + 0x08u), READ32(r + 0x28u)};
        HeaderOut o;
        const bool native = fn == kPesHeaderFn ? parsePesHeader(h, o) : parsePackHeader(h, o);
        if (!native || g_movieBitsVerify)
        {
            const uint64_t before = runtime->eeScheduler().currentCycle();
            g_original[slotOf(fn)](rdram, ctx, runtime);
            if (!native || ctx->pc != ra)
                return; // the game's error path, or paused at a checkpoint (resumes there, not compared)
            ++g_movieHeaderChecked;
            const uint64_t spent = runtime->eeScheduler().currentCycle() - before;
            const BitReader game = loadBitReader(rdram, ctx, runtime, r);
            bool same = sameBitReader(game, h.b) && GPR_U64(ctx, 2) == 1u && spent == h.cycles;
            for (uint32_t i = 0; i < o.n; ++i)
            {
                const HeaderOut::Field &f = o.fields[i];
                same &= (f.size == 8 ? READ64(out + f.offset) : READ32(out + f.offset)) == f.value;
            }
            if (!same && ++g_movieHeaderMismatches <= 16)
                logf("movie headers: 0x%06X differs: cycles %llu vs %llu, consumed %llu vs %llu, v0 %llu", fn,
                     static_cast<unsigned long long>(spent), static_cast<unsigned long long>(h.cycles),
                     static_cast<unsigned long long>(game.consumed), static_cast<unsigned long long>(h.b.consumed),
                     static_cast<unsigned long long>(GPR_U64(ctx, 2)));
            return;
        }
        ++g_movieHeaderCalls;
        storeBitReader(rdram, ctx, runtime, r, h.b);
        for (uint32_t i = 0; i < o.n; ++i)
        {
            const HeaderOut::Field &f = o.fields[i];
            if (f.size == 8)
                WRITE64(out + f.offset, f.value);
            else
                WRITE32(out + f.offset, static_cast<uint32_t>(f.value));
        }
        runtime->eeScheduler().accountCycles(h.cycles);
        SET_GPR_U64(ctx, 2, 1u);
        ctx->pc = ra;
    }

    // ---- frame rate (MAXIMO_FRAME_RATE=60, FRAME_RATE.md) ----------------------------------------------------------
    // The game times every frame itself: func_1480C0 turns the EE cycles since the last frame into a delta in 1/60 s
    // (0x25B9F0; 1.18 at 50 Hz, 0.98 at 60 Hz) and moves, animates and counts down by it. A 60 Hz field rate therefore
    // keeps the game's speed and only shows more frames. Only gameplay runs at 60 Hz; the rest stays at 50 Hz:
    // - the front end (boot logos, title, menus, loading screens) times several of its screens in frames, so at
    //   60 Hz they would end early;
    // - the level loop func_1221F0(mode) stores its mode in 0x238FD4; above 0 (the attract demo replaying recorded
    //   input) the delta is fixed at 1.2 per frame, so at 60 Hz it would run 20% fast;
    // - FMV pictures are 25 fps; the player flips the display without calling sceGsSyncV, and 50 Hz keeps its cadence.
    constexpr uint32_t kLevelLoopFn = 0x001221F0u;
    constexpr uint32_t kFixedStepModeAddr = 0x00238FD4u;
    constexpr uint32_t k60HzFieldMicroseconds = 16667u; // the runtime's NTSC field period
    uint32_t g_frameRateHz = 50;   // the requested rate
    uint32_t g_fieldRateHz = 50;   // the rate in effect
    uint64_t g_lastSyncVTick = 0;  // VBlank tick of the last sceGsSyncV
    uint64_t g_frameRateSwitches = 0;
    bool g_levelLoopRan = false;   // func_1221F0 ran since the last sceGsSyncV

    void setFieldRate(PS2Runtime *runtime, uint32_t hz, const char *why)
    {
        if (hz == g_fieldRateHz)
            return;
        g_fieldRateHz = hz;
        runtime->eeScheduler().setVBlankPeriodOverride(hz == 60 ? k60HzFieldMicroseconds : 0u);
        const uint64_t n = ++g_frameRateSwitches;
        if (n <= 16 || (n & (n - 1)) == 0)
            logf("[frame-rate] %u Hz (%s) at VBlank %llu, switch #%llu", hz, why,
                 static_cast<unsigned long long>(runtime->eeScheduler().currentVSyncTick()), static_cast<unsigned long long>(n));
    }

    // The game also keeps two frame counters, incremented once per frame by func_1480C0 (0x25B9E0: game frames, held
    // while paused; 0x25B9E4: all frames; both reset by func_121050). Some code times by them in PAL frames: deadlines
    // of "counter + seconds * 50" (func_178F30), countdowns run on even frames (func_157F10, func_12CB10), effects
    // spawned every 8th frame (func_125180), blinking (counter & 8). At 60 Hz each counter skips one increment in six,
    // so it advances 50 times per second as at 50 Hz. Only an increment to an odd value is skipped: the game then sees
    // that odd value for two frames, while every even value, on which the every-2nd/4th/8th-frame work runs, still
    // appears exactly once. The player's idle counter (gp-0x7740, func_13D020: +1 per standing frame, the idle fidget
    // animation after 150) is paced the same way, so it plays after 3 s as at 50 Hz.
    struct PacedCounter
    {
        uint32_t addr;
        uint32_t last = 0;
        uint32_t increments = 0; // since the last skipped one
        bool valid = false;
    };
    PacedCounter g_pacedCounters[3] = {{0x0025B9E0u}, {0x0025B9E4u}, {0x00239030u}};
    uint64_t g_counterHolds = 0;

    void paceFrameCounter(PacedCounter &c, bool pace)
    {
        uint32_t v = rd32(c.addr);
        if (!pace || !c.valid || v - c.last > 1) // 50 Hz, the first frame, or a reset
        {
            c = {c.addr, v, 0, true};
            return;
        }
        if (v == c.last)
            return;
        if (++c.increments >= 6 && (v & 1))
        {
            wr32(c.addr, --v); // undo this frame's increment: the game sees the odd value again next frame
            c.increments -= 6;
            ++g_counterHolds;
        }
        c.last = v;
    }

    // Every frame: the requested rate for a gameplay frame, otherwise 50 Hz.
    void frameRateOnSyncV(PS2Runtime *runtime)
    {
        g_lastSyncVTick = runtime->eeScheduler().currentVSyncTick();
        for (PacedCounter &c : g_pacedCounters) // for the frame that just ran, at the rate it ran at
            paceFrameCounter(c, g_fieldRateHz == 60);
        const bool level = std::exchange(g_levelLoopRan, false);
        if (!level)
            setFieldRate(runtime, 50, "front end");
        else if (static_cast<int32_t>(rd32(kFixedStepModeAddr)) > 0)
            setFieldRate(runtime, 50, "attract demo: fixed step");
        else
            setFieldRate(runtime, g_frameRateHz, "gameplay");
    }

    void levelLoopEntry(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_levelLoopRan = true;
        g_original[slotOf(ctx->pc)](rdram, ctx, runtime);
    }

    // A display flip two or more fields after the last sceGsSyncV is the FMV player's.
    void frameRateOnFlip(PS2Runtime *runtime)
    {
        if (runtime->eeScheduler().currentVSyncTick() - g_lastSyncVTick >= 2)
            setFieldRate(runtime, 50, "movie");
    }

    // A few gameplay steps are per frame rather than by the delta; at 60 Hz they are scaled to 50 per second.
    // - func_167650(node, f12 = step) advances a node's animation. func_16A500 advances a model's animations by the
    //   delta, except the one at +0x928, which it steps by 1.0 per frame. (The front end's constant steps, such as the
    //   loading screen's shield at 1.2 per frame, run at 50 Hz and are left alone.)
    constexpr uint32_t kAnimStepFn = 0x00167650u;
    constexpr uint32_t kModelFrameAnimRa = 0x0016A598u;
    uint64_t g_fixedAnimSteps = 0;
    void animStepEntry(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (g_fieldRateHz == 60 && reg(ctx, 31) == kModelFrameAnimRa)
        {
            ctx->f[12] *= 50.0f / 60.0f;
            ++g_fixedAnimSteps;
        }
        g_original[slotOf(ctx->pc)](rdram, ctx, runtime);
    }

    // - Camera shake (func_1406C0, once per frame on the camera object, mode +0x28 == 0). Before a shake starts, a
    //   delay (+0x610) counts down by 1 per frame; at 60 Hz it is given back a sixth of that step first. The shake
    //   itself (+0x60C, of +0x614 steps, which also fades its amplitude) is throttled by the global gp-0x773C: while
    //   it is above 0 the frame only takes 1.2 off it, otherwise the frame adds 2.0 and moves the shake one step. That
    //   is a step on 1.2 / 3.2 of the frames, 18.75 per second at 50 Hz. At 60 Hz the frame takes 10/11 instead, a
    //   step on (10/11) / (32/11) = 5/16 of the frames: 18.75 per second again, so the shake lasts as long.
    // - Style 1 (the checkpoint slam) is a scripted move instead: while +0x640 is set, func_140530 walks a table at
    //   +0x2C of 20-byte segments {dx, dy, dz, frames, scale}. Starting a segment (+0x0 set) sets the per-frame move
    //   +0x10..+0x18 = d * scale / frames and the counter +0x20 = frames; each frame with the counter above 0 takes 1
    //   off it and adds the move to the eye (+0x1A0) and target (+0x1D0). The update builds the eye on the previous
    //   frame's eye (+0x220), so the moves add up. At 60 Hz each frame moves 5/6 of the step and takes 5/6 off the
    //   counter: the same distance in the same time. (A segment's first frame is set up inside the call, so its sixth
    //   is given back on the next frame.)
    constexpr uint32_t kCameraShakeFn = 0x001406C0u;
    constexpr uint32_t kShakeThrottleAddr = 0x00239034u; // gp-0x773C
    uint64_t g_shakeFrames = 0;
    float g_slamCounterOwed = 0.0f;
    void slamShakePace(uint8_t *rdram, uint32_t cam)
    {
        auto rf = [&](uint32_t a) { float v; std::memcpy(&v, rdram + a, sizeof(v)); return v; };
        auto wf = [&](uint32_t a, float v) { std::memcpy(rdram + a, &v, sizeof(v)); };
        float move[3];
        if (rdram[cam] != 0) // a segment starts in this call: work out its move the way func_140530 does
        {
            const uint32_t seg = (rd32(cam + 0x2C) + rd32(cam + 0x24) * 20u) & 0x01FFFFFFu;
            if (seg + 20u > PS2_RAM_SIZE)
                return;
            for (int i = 0; i < 3; ++i)
                move[i] = rf(seg + 4u * i) * rf(seg + 0x10u) / rf(seg + 0xCu);
            g_slamCounterOwed = 1.0f / 6.0f;
        }
        else
        {
            const float counter = rf(cam + 0x20);
            if (!(counter > 0.0f))
                return;
            for (int i = 0; i < 3; ++i)
                move[i] = rf(cam + 0x10 + 4u * i);
            wf(cam + 0x20, counter + 1.0f / 6.0f + g_slamCounterOwed);
            g_slamCounterOwed = 0.0f;
        }
        for (int i = 0; i < 3; ++i)
        {
            if (!std::isfinite(move[i]))
                return;
        }
        for (int i = 0; i < 3; ++i)
        {
            wf(cam + 0x1A0 + 4u * i, rf(cam + 0x1A0 + 4u * i) - move[i] / 6.0f);
            wf(cam + 0x1D0 + 4u * i, rf(cam + 0x1D0 + 4u * i) - move[i] / 6.0f);
        }
        ++g_shakeFrames;
    }

    void cameraShakeEntry(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t cam = reg(ctx, 4);
        if (g_fieldRateHz == 60 && cam && rd32(cam + 0x28) == 1 && rdram[cam + 0x640] != 0)
            slamShakePace(rdram, cam);
        else if (g_fieldRateHz == 60 && cam && rd32(cam + 0x28) == 0)
        {
            const bool shaking = rdram[cam + 0x640] != 0;
            const uint32_t addr = shaking ? kShakeThrottleAddr : cam + 0x610u;
            float t;
            std::memcpy(&t, rdram + addr, sizeof(t));
            if (t > 0.0f)
            {
                t += shaking ? 1.2f - 10.0f / 11.0f : 1.0f / 6.0f;
                std::memcpy(rdram + addr, &t, sizeof(t));
                ++g_shakeFrames;
            }
        }
        g_original[slotOf(ctx->pc)](rdram, ctx, runtime);
    }

    // ---- camera probe and free camera (x-camera) ----------------------------------------------------------------------
    // func_13D020 is the camera update, called once per frame from func_1221F0 with the camera object (0x239890). It
    // starts by copying +0x1A0 to +0x220: +0x1A0 is the eye, +0x1D0 the look-at target, +0x1F0 the followed point
    // (Maximo); y is up. The update often returns to the run loop part way: its line-of-sight query (func_153170)
    // jumps into another recompiled piece (0x1530F4), and the run loop then finishes the update without returning
    // through a hook (an EE checkpoint can do the same). So nothing is done after it returns. Instead the change is made on entry to func_13C8B0(cam), which the
    // update calls at the end of each camera branch to build the view matrix (+0xA0, via func_13C6C0) from the eye and
    // target and to copy the eye to +0x1B0; the update's own entry puts the game's eye back first. func_1483C0, the
    // level loop's next call (0x1222EC), ends the frame's update. So the change is draw-only for the game's camera
    // logic, while everything after the matrix (Maximo's camera-relative movement included) sees the turned view.
    // MAXIMO_CAMERA_LOG=1 logs the vectors twice a second and camera type changes. MAXIMO_CAMERA_TEST=<deg>[,x|y|z]
    // turns the eye around the target by that angle (probe).
    constexpr uint32_t kCameraUpdateFn = 0x0013D020u;
    constexpr uint32_t kCameraApplyFn = 0x001483C0u; // the level loop's next call: the frame's update is over
    constexpr uint32_t kCameraViewFn = 0x0013C8B0u;  // (cam): view matrix from the eye and target
    constexpr uint32_t kLineOfSightFn = 0x00153640u; // (from, to): 1 and `to` moved to the hit when blocked
    bool g_camLog = false;
    float g_camTestDeg = 0.0f;
    int g_camTestAxis = 1;
    bool g_camSaved = false;
    uint32_t g_camObj = 0;
    uint32_t g_camPending = 0; // camera object of the update in progress (until func_1483C0)
    bool g_camAppliedThisUpdate = false;
    float g_camSavedEye[4]{};
    uint64_t g_camCalls = 0;

    void camVec(uint32_t addr, float *v) { std::memcpy(v, mem(addr), 16); }

    // ---- free camera (MAXIMO_CAMERA=orbit) ---------------------------------------------------------------------------
    // The right stick orbits the eye around the game's look-at target: left/right turns (yaw about y), up/down tilts
    // (pitch, kept between kPitchMin and kPitchMax). Until the stick is first moved the game's own camera shows
    // unchanged. Then the view direction is the player's (the game's camera keeps chasing behind Maximo underneath; only
    // its distance to the target is used). Turning speeds up and slows down smoothly (kTurnSmoothing) and small stick
    // moves turn slowly (response curve). After kReturnDelay seconds without camera input the view swings back to the
    // game's (behind Maximo) on a soft critically damped spring; R3 does the same at once on a stiffer one. The game's
    // own line-of-sight query (func_153640, called from the hook with EE checkpoints held) keeps the eye in front of
    // walls: the camera pulls in at once and eases back out when the way is clear.
    // It only acts on the follow camera (+0x248 type 0) in normal play: any other camera type (scripted shots, level-
    // start views), the attract demo (fixed-step mode 0x238FD4 > 0) or a jump of the followed point (level start,
    // respawn, teleport) cuts straight back to the game's camera. While the game is paused the camera holds.
    // MAXIMO_CAMERA_SPEED = turn speed in degrees per second at full stick (default 150; tilting runs at 60% of it).
    // Stick up looks up (the eye goes down), as in most current games; MAXIMO_CAMERA_INVERT_Y=1 flips that.
    // MAXIMO_CAMERA_RETURN = seconds without camera input before the view swings back (default 3, 0 = never).
    enum class OrbitMode
    {
        Game,      // the game's camera, untouched
        Player,    // the player's view direction
        Returning, // swinging back to the game's
    };
    struct OrbitState
    {
        bool on = false;
        bool invertY = false;
        float speed = 150.0f * 3.14159265f / 180.0f; // radians per second at full stick
        float returnDelay = 3.0f;
        OrbitMode mode = OrbitMode::Game;
        bool fastReturn = false;
        float yaw = 0.0f, pitch = 0.0f;       // the view shown (eye around the target)
        float yawVel = 0.0f, pitchVel = 0.0f; // radians per second
        float idle = 0.0f;                    // seconds without camera input
        float shownDist = 0.0f;               // eye distance shown (pulled in by walls)
        float lift = 0.0f;                    // extra pitch while pulled in (looks down over Maximo)
        uint32_t lastFrame = 0;
        int sameFrame = 0;
        float lastFollow[3]{};
        bool haveFollow = false;
        // right stick and R3 from the last scePadRead (analog mode only), written by scriptedPadRead
        uint8_t rx = 0x80, ry = 0x80;
        bool r3 = false, r3Was = false;
        uint64_t takes = 0, wallHits = 0, losCalls = 0, losFailures = 0;
    };
    OrbitState g_orbit;
    constexpr float kPitchMin = -0.17f;      // ~-10 degrees: a little below the target
    constexpr float kPitchMax = 1.22f;       // ~70 degrees: high above
    constexpr float kStickDeadZone = 0.18f;
    constexpr float kTurnSmoothing = 0.08f;  // seconds: time constant of the turn speed following the stick
    constexpr float kReturnSpringSlow = 2.2f; // rad/s: the spring after kReturnDelay (settles in ~2 s)
    constexpr float kReturnSpringFast = 7.0f; // rad/s: the spring after R3 (~0.6 s)
    constexpr float kWallMargin = 0.6f;      // eye kept this far in front of a wall hit
    constexpr float kMinDist = 1.2f;
    constexpr float kWallEaseOut = 0.35f;    // seconds: time constant of moving back out from a wall
    constexpr float kWallLift = 0.7f;        // radians (~40 degrees) of extra pitch when pulled all the way in
    constexpr float kLiftSmoothing = 0.15f;  // seconds
    constexpr uint32_t kGameFrameCounterAddr = 0x0025B9E0u;
    constexpr uint32_t kFrameDeltaAddr = 0x0025B9F0u; // this frame's step in 1/60 s

    float orbitStick(uint8_t b)
    {
        const float v = std::clamp((static_cast<int>(b) - 128) / 127.0f, -1.0f, 1.0f);
        const float a = std::fabs(v);
        if (a < kStickDeadZone)
            return 0.0f;
        const float t = (a - kStickDeadZone) / (1.0f - kStickDeadZone);
        return std::copysign(t * (0.35f + 0.65f * t), v); // gentle near the centre, full at the edge
    }

    float wrapAngle(float a)
    {
        constexpr float kPi = 3.14159265f;
        while (a > kPi)
            a -= 2.0f * kPi;
        while (a < -kPi)
            a += 2.0f * kPi;
        return a;
    }

    void orbitSetMode(OrbitMode mode, const char *why)
    {
        if (mode == g_orbit.mode)
            return;
        if (g_camLog || g_orbit.on)
            logf("camera orbit: %s -> %s (%s) at frame %llu",
                 g_orbit.mode == OrbitMode::Game ? "game" : g_orbit.mode == OrbitMode::Player ? "player" : "returning",
                 mode == OrbitMode::Game ? "game" : mode == OrbitMode::Player ? "player" : "returning", why,
                 static_cast<unsigned long long>(g_guestFrame.load()));
        g_orbit.mode = mode;
    }

    extern "C" void ps2xHoldEeCheckpoints(int hold);

    // The game's line-of-sight query from the host: func_153640(from, to) on vectors below the guest stack, with EE
    // checkpoints held (nothing would resume it if one stopped it) and every register put back afterwards.
    bool guestLineOfSight(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, const float *from, float *to)
    {
        static R5900Context saved;
        saved = *ctx;
        const uint32_t sp = (GPR_U32(ctx, 29) - 0x400u) & ~0xFu;
        const uint32_t a = sp + 0x10u, b = sp + 0x20u;
        constexpr uint32_t kReturnMark = 0x00100004u;
        std::memcpy(mem(a), from, 16);
        std::memcpy(mem(b), to, 16);
        SET_GPR_U32(ctx, 29, sp);
        SET_GPR_U32(ctx, 4, a);
        SET_GPR_U32(ctx, 5, b);
        SET_GPR_U32(ctx, 31, kReturnMark);
        ctx->pc = kLineOfSightFn;
        ps2xHoldEeCheckpoints(1);
        // Like the run loop: a jump into another recompiled piece returns with $pc there, so dispatch until the call
        // reaches its return mark.
        for (int steps = 0; steps < 100000 && ctx->pc != kReturnMark && ctx->pc != 0u; ++steps)
        {
            const uint32_t pc = ctx->pc;
            if (!runtime->hasFunction(pc))
                break;
            runtime->lookupFunction(pc)(rdram, ctx, runtime);
        }
        ps2xHoldEeCheckpoints(-1);
        const bool ok = ctx->pc == kReturnMark;
        const bool hit = ok && GPR_U32(ctx, 2) != 0;
        if (hit)
            std::memcpy(to, mem(b), 16);
        ++g_orbit.losCalls;
        if (!ok && ++g_orbit.losFailures <= 4)
            logf("camera orbit: line-of-sight call ended at pc %06X, not the return mark", ctx->pc);
        *ctx = saved;
        return hit;
    }

    // Once per frame (first view-matrix build of the update). A later build in the same update only places the eye
    // again from the same state (placeOnly).
    void orbitCamera(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t cam, bool placeOnly)
    {
        OrbitState &o = g_orbit;
        float eye[4], tgt[4], fol[4];
        camVec(cam + 0x1A0, eye);
        camVec(cam + 0x1D0, tgt);
        camVec(cam + 0x1F0, fol);
        if (placeOnly)
        {
            if (o.mode == OrbitMode::Game || o.shownDist <= 0.0f)
                return;
            float out[4];
            std::memcpy(out, eye, 16);
            const float p = std::min(o.pitch + o.lift, kPitchMax);
            out[0] = tgt[0] + std::cos(p) * std::sin(o.yaw) * o.shownDist;
            out[1] = tgt[1] + std::sin(p) * o.shownDist;
            out[2] = tgt[2] + std::cos(p) * std::cos(o.yaw) * o.shownDist;
            if (!g_camSaved)
                std::memcpy(g_camSavedEye, eye, 16);
            g_camSaved = true;
            g_camObj = cam;
            std::memcpy(mem(cam + 0x1A0), out, 16);
            return;
        }
        const bool r3Edge = o.r3 && !o.r3Was;
        o.r3Was = o.r3;
        // Eligibility: the follow camera in normal play, no jump of the followed point.
        const float jx = fol[0] - o.lastFollow[0], jy = fol[1] - o.lastFollow[1], jz = fol[2] - o.lastFollow[2];
        const bool jumped = o.haveFollow && jx * jx + jy * jy + jz * jz > 20.0f * 20.0f;
        std::memcpy(o.lastFollow, fol, sizeof o.lastFollow);
        o.haveFollow = true;
        const uint32_t type = rd32(cam + 0x248);
        // (+0x28 is not a camera mode: func_13C660 starts a shake and stores the shake's style there, e.g. 1 for the
        // checkpoint slam, and it stays set after the shake.)
        if (type != 0 || static_cast<int32_t>(rd32(kFixedStepModeAddr)) > 0 || jumped)
        {
            orbitSetMode(OrbitMode::Game, type != 0 ? "scripted camera" : jumped ? "followed point jumped" : "attract demo");
            o.yawVel = o.pitchVel = 0.0f;
            return;
        }
        // The game's own view, as yaw/pitch/distance of the eye around the target.
        const float dx = eye[0] - tgt[0], dy = eye[1] - tgt[1], dz = eye[2] - tgt[2];
        const float flat = std::sqrt(dx * dx + dz * dz), dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (dist < 0.01f)
            return;
        const float gameYaw = std::atan2(dx, dz), gamePitch = std::atan2(dy, flat);
        // Time: the game's frame step (1/60 s units). Paused = the game frame counter held for two calls running (at
        // 60 Hz the frame-rate code holds it for one frame in six, which is not a pause).
        const uint32_t frame = rd32(kGameFrameCounterAddr);
        o.sameFrame = frame == o.lastFrame ? o.sameFrame + 1 : 0;
        o.lastFrame = frame;
        float stepDt;
        std::memcpy(&stepDt, mem(kFrameDeltaAddr), 4);
        const float dt = o.sameFrame >= 2 || !(stepDt > 0.0f && stepDt < 10.0f) ? 0.0f : stepDt / 60.0f;

        const float sx = orbitStick(o.rx), sy = orbitStick(o.ry) * (o.invertY ? -1.0f : 1.0f);
        const bool input = sx != 0.0f || sy != 0.0f;
        if (o.mode == OrbitMode::Game)
        {
            o.yaw = gameYaw;
            o.pitch = gamePitch;
            o.shownDist = dist;
            o.lift = 0.0f;
        }
        if (input && dt > 0.0f)
        {
            if (o.mode == OrbitMode::Game)
                ++o.takes;
            orbitSetMode(OrbitMode::Player, "right stick");
            o.idle = 0.0f;
        }
        else if (r3Edge && o.mode != OrbitMode::Game)
        {
            o.fastReturn = true;
            orbitSetMode(OrbitMode::Returning, "R3");
        }
        else if (o.mode == OrbitMode::Player && dt > 0.0f)
        {
            o.idle += dt;
            if (o.returnDelay > 0.0f && o.idle >= o.returnDelay)
            {
                o.fastReturn = false;
                orbitSetMode(OrbitMode::Returning, "no camera input");
            }
        }
        if (o.mode == OrbitMode::Game)
            return;

        if (dt > 0.0f)
        {
            if (o.mode == OrbitMode::Player)
            {
                // Turn speed eases toward the stick's.
                const float k = 1.0f - std::exp(-dt / kTurnSmoothing);
                o.yawVel += (-sx * o.speed - o.yawVel) * k;
                o.pitchVel += (sy * o.speed * 0.6f - o.pitchVel) * k;
            }
            else
            {
                // Critically damped spring toward the game's view.
                const float w = o.fastReturn ? kReturnSpringFast : kReturnSpringSlow;
                const float ey = wrapAngle(gameYaw - o.yaw), ep = gamePitch - o.pitch;
                o.yawVel += (w * w * ey - 2.0f * w * o.yawVel) * dt;
                o.pitchVel += (w * w * ep - 2.0f * w * o.pitchVel) * dt;
                if (std::fabs(ey) < 0.01f && std::fabs(ep) < 0.01f && std::fabs(o.yawVel) < 0.05f &&
                    std::fabs(o.pitchVel) < 0.05f)
                {
                    orbitSetMode(OrbitMode::Game, "back behind Maximo");
                    o.yawVel = o.pitchVel = 0.0f;
                    return;
                }
            }
            o.yaw = wrapAngle(o.yaw + o.yawVel * dt);
            o.pitch += o.pitchVel * dt;
            if (o.pitch < kPitchMin || o.pitch > kPitchMax)
            {
                o.pitch = std::clamp(o.pitch, kPitchMin, kPitchMax);
                o.pitchVel = 0.0f;
            }
        }
        // Pulled in by a wall: tilt up toward kWallLift so the view looks down over Maximo instead of into him.
        if (dt > 0.0f)
        {
            const float pulled = std::clamp(1.0f - (o.shownDist - kMinDist) / std::max(0.01f, dist - kMinDist), 0.0f, 1.0f);
            o.lift += (pulled * kWallLift - o.lift) * (1.0f - std::exp(-dt / kLiftSmoothing));
        }
        const float shownPitch = std::min(o.pitch + o.lift, kPitchMax);
        const float dir[3] = {std::cos(shownPitch) * std::sin(o.yaw), std::sin(shownPitch),
                              std::cos(shownPitch) * std::cos(o.yaw)};
        // Walls: the game's line of sight from the target to where the eye would be.
        float want[4] = {tgt[0] + dir[0] * dist, tgt[1] + dir[1] * dist, tgt[2] + dir[2] * dist, eye[3]};
        float allowed = dist;
        if (guestLineOfSight(rdram, ctx, runtime, tgt, want))
        {
            const float hx = want[0] - tgt[0], hy = want[1] - tgt[1], hz = want[2] - tgt[2];
            allowed = std::max(kMinDist, std::sqrt(hx * hx + hy * hy + hz * hz) - kWallMargin);
            ++o.wallHits;
        }
        if (allowed < o.shownDist || o.shownDist <= 0.0f)
            o.shownDist = allowed;
        else if (dt > 0.0f)
            o.shownDist += (allowed - o.shownDist) * (1.0f - std::exp(-dt / kWallEaseOut));
        float out[4];
        std::memcpy(out, eye, 16);
        for (int i = 0; i < 3; ++i)
            out[i] = tgt[i] + dir[i] * o.shownDist;
        std::memcpy(g_camSavedEye, eye, 16);
        g_camSaved = true;
        g_camObj = cam;
        std::memcpy(mem(cam + 0x1A0), out, 16);
    }

    void cameraTestTurn(uint32_t cam)
    {
        float eye[4], tgt[4];
        camVec(cam + 0x1A0, eye);
        camVec(cam + 0x1D0, tgt);
        std::memcpy(g_camSavedEye, eye, 16);
        g_camSaved = true;
        g_camObj = cam;
        const float a = g_camTestDeg * 3.14159265f / 180.0f, c = std::cos(a), s = std::sin(a);
        const int i = g_camTestAxis == 0 ? 1 : 0, j = g_camTestAxis == 2 ? 1 : 2; // the plane turned
        const float di = eye[i] - tgt[i], dj = eye[j] - tgt[j];
        float out[4];
        std::memcpy(out, eye, 16);
        out[i] = tgt[i] + di * c - dj * s;
        out[j] = tgt[j] + di * s + dj * c;
        std::memcpy(mem(cam + 0x1A0), out, 16);
    }

    void cameraUpdateEntry(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t cam = reg(ctx, 4);
        if (g_camSaved && cam == g_camObj)
            std::memcpy(mem(cam + 0x1A0), g_camSavedEye, 16);
        g_camSaved = false;
        g_camPending = cam;
        g_camAppliedThisUpdate = false;
        g_original[slotOf(ctx->pc)](rdram, ctx, runtime);
    }

    void cameraViewEntry(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t cam = reg(ctx, 4);
        if (cam && cam == g_camPending)
        {
            const bool again = std::exchange(g_camAppliedThisUpdate, true);
            if (g_orbit.on)
                orbitCamera(rdram, ctx, runtime, cam, again);
            else if (g_camTestDeg != 0.0f && !again)
                cameraTestTurn(cam);
            SET_GPR_U32(ctx, 4, cam); // the line-of-sight call put every register back; keep a0 explicit
        }
        g_original[slotOf(ctx->pc)](rdram, ctx, runtime);
    }

    void cameraApplyEntry(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (const uint32_t cam = std::exchange(g_camPending, 0u))
        {
            ++g_camCalls;
            // Events (camera type, shake style +0x28, whether the update built its view through func_13C8B0) are logged
            // whenever the free camera is on, so a tester's log shows why it handed back.
            if (g_camLog || g_orbit.on)
            {
                static uint32_t lastType = ~0u, lastReq = ~0u, lastMode = ~0u;
                static int lastBuilt = -1;
                const uint32_t type = rd32(cam + 0x248), req = rd32(cam + 0x5FC), mode = rd32(cam + 0x28);
                const int built = g_camAppliedThisUpdate ? 1 : 0;
                if (type != lastType || req != lastReq || mode != lastMode || built != lastBuilt)
                    logf("camera %llu type %u -> %u, request %d -> %d, shake style %u -> %u, view built %d, +0x604 %u, "
                         "frame %llu",
                         static_cast<unsigned long long>(g_camCalls), lastType, type, static_cast<int>(lastReq),
                         static_cast<int>(req), lastMode, mode, built, rd32(cam + 0x604),
                         static_cast<unsigned long long>(g_guestFrame.load()));
                lastType = type;
                lastReq = req;
                lastMode = mode;
                lastBuilt = built;
            }
            if (g_camLog)
            {
                float eye[4], tgt[4], fol[4];
                camVec(cam + 0x1A0, eye);
                camVec(cam + 0x1D0, tgt);
                camVec(cam + 0x1F0, fol);
                if (g_camCalls % 25 == 0)
                {
                    const float dx = eye[0] - tgt[0], dy = eye[1] - tgt[1], dz = eye[2] - tgt[2];
                    logf("camera %llu obj %08X mode %u: eye (%.1f %.1f %.1f %.2f) target (%.1f %.1f %.1f) follow (%.1f "
                         "%.1f %.1f) eye-target (%.1f %.1f %.1f) dist %.1f | orbit %d yaw %.0f pitch %.0f shown %.1f "
                         "walls %llu/%llu",
                         static_cast<unsigned long long>(g_camCalls), cam, rd32(cam + 0x28), eye[0], eye[1], eye[2],
                         eye[3], tgt[0], tgt[1], tgt[2], fol[0], fol[1], fol[2], dx, dy, dz,
                         std::sqrt(dx * dx + dy * dy + dz * dz), static_cast<int>(g_orbit.mode),
                         g_orbit.yaw * 57.2958f, g_orbit.pitch * 57.2958f, g_orbit.shownDist,
                         static_cast<unsigned long long>(g_orbit.wallHits),
                         static_cast<unsigned long long>(g_orbit.losCalls));
                }
            }
        }
        g_original[slotOf(ctx->pc)](rdram, ctx, runtime);
    }

    // - Lightning (the weather object's func_170AC0, once per frame): while lightning is on (+0xA1, +0x2F0 and +0x2F4
    //   set) a countdown at +0x300 drops by 1 per frame and, at 0, strikes (func_17E6C0: flash and thunder) and draws
    //   the next wait from +0x2F8..+0x2FC frames. At 60 Hz every sixth frame gives a step back, so it counts 50 a
    //   second and lightning strikes as often as at 50 Hz. (The flash itself lasts 2 to 4 frames and is left alone.)
    constexpr uint32_t kLightningFn = 0x00170AC0u;
    uint64_t g_lightningFrames = 0;
    void lightningEntry(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t obj = reg(ctx, 4);
        if (g_fieldRateHz == 60 && obj && rdram[(obj + 0xA1) & 0x1FFFFFFu] && rd32(obj + 0x2F0) && rd32(obj + 0x2F4))
        {
            static uint32_t frames = 0;
            const uint32_t wait = rd32(obj + 0x300);
            if (++frames >= 6 && wait > 1 && wait < 0x80000000u)
            {
                wr32(obj + 0x300, wait + 1);
                frames = 0;
                ++g_lightningFrames;
            }
        }
        g_original[slotOf(ctx->pc)](rdram, ctx, runtime);
    }

    void gsSyncVEntry(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (g_frameRateHz != 50)
            frameRateOnSyncV(runtime);
        // Frame boundary: the frame's GS work must be complete before it is presented. With pipelined frames the
        // worker presents it once that work is done (vu1ThreadFrame below) and the game goes on to the next frame.
        ps2_vu1_overlap_model::sync(ps2_vu1_overlap_model::kSyncVSync);
#ifdef MAXIMO_ENABLE_GPU_SHADOW
        const bool pipelined = g_gpuMode && g_gpuShadow && runtime->memory().vu1ThreadPipelined();
#else
        const bool pipelined = false;
#endif
        if (!pipelined)
            runtime->memory().vu1ThreadWait(Vu1Thread::kWaitFrame);
        static uint64_t guestFrame = 0;
        static const char *captureFrames = std::getenv("PS2_GS_CAPTURE_FRAMES");
        static const char *benchRange = std::getenv("PS2_GS_BENCH_RANGE");
        static uint64_t benchBegin = 0, benchEnd = 0;
        static bool benchParsed = false;
        static FrameStats benchStats;
        if (!benchParsed)
        {
            benchParsed = true;
            if (benchRange)
                std::sscanf(benchRange, "%llu:%llu", &benchBegin, &benchEnd);
        }
        ++guestFrame;
        g_guestFrame.store(guestFrame, std::memory_order_relaxed);
        if (g_widescreen)
        {
            // The HUD's side groups move to the edges while the picture is 16:9 (the presenter's rule: a widescreen
            // frame within the last 10).
            static uint64_t lastWide = 0;
            static int sinceWide = 10;
            const uint64_t wide = ps2_display_aspect::wideFrameCount();
            sinceWide = wide != lastWide ? 0 : std::min(sinceWide + 1, 10);
            lastWide = wide;
            g_hudWide = sinceWide < 10;
        }
#ifdef MAXIMO_ENABLE_GPU_SHADOW
        if (captureFrames && !g_gpuMode)
#else
        if (captureFrames)
#endif
        {
            const char *p = captureFrames;
            while (*p)
            {
                char *end = nullptr;
                const uint64_t wanted = std::strtoull(p, &end, 10);
                if (end == p) break;
                if (wanted == guestFrame)
                {
                    // An asynchronous GS must first reach this point in the stream, so captures match inline runs.
                    runtime->gs().waitForQueuedWork();
                    runtime->gs().latchHostPresentationFrame();
                    std::vector<uint8_t> pixels;
                    uint32_t width = 0, height = 0;
                    if (runtime->gs().copyLatchedHostPresentationFrame(pixels, width, height))
                    {
                        char name[64];
                        std::snprintf(name, sizeof(name), "gs_sync_%04llu_%ux%u.rgba",
                                      static_cast<unsigned long long>(guestFrame), width, height);
                        std::ofstream file(name, std::ios::binary);
                        file.write(reinterpret_cast<const char *>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
                        logf("[gs-capture] frame=%llu size=%ux%u bytes=%llu", static_cast<unsigned long long>(guestFrame),
                             width, height, static_cast<unsigned long long>(pixels.size()));
                    }
                    break;
                }
                p = *end == ',' ? end + 1 : end;
                if (*p && *p != ',' && p == end) break;
            }
        }
#ifdef MAXIMO_ENABLE_GPU_SHADOW
        if (g_gpuShadow)
        {
            const bool capture = std::find(g_gpuShadowFrames.begin(), g_gpuShadowFrames.end(), guestFrame) !=
                                 g_gpuShadowFrames.end();
            if (capture && !g_gpuMode)
            {
                // The CPU GS is inline, so its VRAM is current once pending backend work is flushed.
                runtime->gs().latchHostPresentationFrame();
            }
            if (pipelined)
            {
                const uint64_t csr = runtime->memory().gs().csr.load(std::memory_order_relaxed);
                const uint64_t frame = guestFrame;
                runtime->memory().vu1ThreadFrame(
                    [runtime, frame, capture, csr]
                    {
                        const bool ok = g_gpuShadow->onGuestFrame(frame, capture, csr);
                        if (capture)
                            logf("[gs-capture] frame=%llu gpu %s", static_cast<unsigned long long>(frame), ok ? "ok" : "failed");
                        if (!ok && !g_gpuFailed.exchange(true))
                        {
                            logf("[gpu] frame=%llu: GPU frame failed; stopping", static_cast<unsigned long long>(frame));
                            runtime->requestStop();
                        }
                    },
                    capture);
                g_lastGpuVsyncTick = runtime->eeScheduler().currentVSyncTick();
            }
            else
            {
                const bool ok = g_gpuShadow->onGuestFrame(guestFrame, capture);
                g_lastGpuVsyncTick = runtime->eeScheduler().currentVSyncTick();
                if (g_gpuMode)
                {
                    if (capture)
                        logf("[gs-capture] frame=%llu gpu %s", static_cast<unsigned long long>(guestFrame), ok ? "ok" : "failed");
                    if (!ok && !g_gpuFailed.exchange(true))
                    {
                        logf("[gpu] frame=%llu: GPU frame failed; stopping", static_cast<unsigned long long>(guestFrame));
                        runtime->requestStop();
                    }
                }
                else if (capture)
                {
                    logf("[gpu-shadow] frame=%llu packets=%llu capture=%s",
                         static_cast<unsigned long long>(guestFrame),
                         static_cast<unsigned long long>(g_gpuShadow->packetCount()), ok ? "ok" : "failed");
                    if (guestFrame == g_gpuShadowFrames.back())
                        runtime->requestStop();
                }
            }
        }
#endif
        if (!g_gameThread.load(std::memory_order_relaxed))
        {
            HANDLE self = nullptr;
            DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &self,
                            THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
            g_gameThread.store(self, std::memory_order_release);
        }
        const uint64_t enter = ps2_host_profile::nowNs();
        ps2_pacing_trace::note(ps2_pacing_trace::kSyncEntry, static_cast<uint32_t>(guestFrame));
        const uint64_t release = ps2_host_profile::g_waitReleaseNs.load(std::memory_order_acquire);
        ps2_host_profile::g_waitMarkNs.store(enter, std::memory_order_release);
        const uint64_t vu1 = ps2_host_profile::total(ps2_host_profile::kVu1);
        const uint64_t gs = ps2_host_profile::total(ps2_host_profile::kGs);
        const uint64_t stall = ps2_host_profile::total(ps2_host_profile::kGsWait);
        const uint64_t vu1Wait = ps2_host_profile::total(ps2_host_profile::kVu1Wait);
        const uint64_t vblankTick = runtime->eeScheduler().currentVSyncTick();
        g_asyncGs = runtime->gs().asyncEnabled();
        g_vu1Thread = runtime->memory().vu1ThreadActive();
        if (g_lastSyncEntryNs != 0)
        {
            const double frameMs = static_cast<double>(enter - g_lastSyncEntryNs) / 1e6;
            const double waitMs = release > g_lastSyncEntryNs ? static_cast<double>(release - g_lastSyncEntryNs) / 1e6 : 0.0;
            const double vu1Ms = static_cast<double>(vu1 - g_lastVu1Ns) / 1e6;
            const double gsMs = static_cast<double>(gs - g_lastGsNs) / 1e6;
            const double stallMs = static_cast<double>(stall - g_lastStallNs) / 1e6;
            const uint64_t vblanks = vblankTick - g_lastVBlankTick;
            const double vu1WaitMs = static_cast<double>(vu1Wait - g_lastVu1WaitNs) / 1e6;
            addFrame(g_perfWindow, frameMs, waitMs, vu1Ms, gsMs, stallMs, vblanks, vu1WaitMs);
            addFrame(g_perfTotal, frameMs, waitMs, vu1Ms, gsMs, stallMs, vblanks, vu1WaitMs);
            if (benchBegin != 0 && guestFrame >= benchBegin && guestFrame < benchEnd)
            {
                if (guestFrame == benchBegin)
                    benchStats.presentNs0 = ps2_host_profile::total(ps2_host_profile::kPresent);
                addFrame(benchStats, frameMs, waitMs, vu1Ms, gsMs, stallMs, vblanks, vu1WaitMs);
            }
            if (benchBegin != 0 && guestFrame == benchEnd)
            {
                logf("[gs-bench] guest frames %llu:%llu", static_cast<unsigned long long>(benchBegin),
                     static_cast<unsigned long long>(benchEnd));
                logFrameStats("guest-range", benchStats, benchStats.frameMs / 1000.0);
                // PS2_GS_BENCH_EXIT=1 ends the run once the range is measured.
                if (std::getenv("PS2_GS_BENCH_EXIT"))
                    runtime->requestStop();
            }
        }
        else
        {
            g_perfWindowStartNs = enter;
            g_perfTotal.presentNs0 = g_perfWindow.presentNs0 = ps2_host_profile::total(ps2_host_profile::kPresent);
        }
        g_lastSyncEntryNs = enter;
        g_lastVu1Ns = vu1;
        g_lastGsNs = gs;
        g_lastStallNs = stall;
        g_lastVu1WaitNs = vu1Wait;
        g_lastVBlankTick = vblankTick;
        if (enter - g_perfWindowStartNs >= 1000000000ull)
        {
            logFrameStats("1s", g_perfWindow, static_cast<double>(enter - g_perfWindowStartNs) / 1e9);
            logAudioWindow("1s", static_cast<double>(enter - g_perfWindowStartNs) / 1e9);
#ifdef MAXIMO_ENABLE_GPU_SHADOW
            if (g_gpuMode && g_gpuShadow)
            {
                // What reached the window: a black screen with the game running shows up here.
                const GpuShadow::Health h = g_gpuShadow->takeHealth();
                logf("[gpu-health] 1s published=%llu shown=%llu black=%llu noScanout=%llu failed=%llu | last frame %ux%u "
                     "brightness mean %u max %u",
                     static_cast<unsigned long long>(h.published), static_cast<unsigned long long>(h.taken),
                     static_cast<unsigned long long>(h.black), static_cast<unsigned long long>(h.noScanout),
                     static_cast<unsigned long long>(h.failed), h.width, h.height, h.brightnessMean, h.brightnessMax);
            }
#endif
            // PS2_GS_SYNC_STATS=1: per-second GS stream events that an asynchronous GS would have to order or
            // wait for, plus the game's CSR/SIGLBLID/BUSDIR reads (see GS_THREADING_PLAN.md).
            static const bool syncStats = std::getenv("PS2_GS_SYNC_STATS") != nullptr;
            if (syncStats)
            {
                const GSStreamStats &st = runtime->gs().streamStats();
                const uint64_t now[] = {st.packets.load(), st.packetBytes.load(), st.directRegWrites.load(),
                                        st.signal.load(), st.finish.load(), st.label.load(), st.trxdir[0].load(),
                                        st.trxdir[1].load(), st.trxdir[2].load(), st.localToHostReads.load(),
                                        st.presentationLatches.load(), runtime->gs().nativeImageUploadCount(),
                                        runtime->gs().nativePackedGIFPacketCount(), mmioAccesses(0x12001000u, false),
                                        mmioAccesses(0x12001080u, false),
                                        mmioAccesses(0x12001040u, false) + mmioAccesses(0x12001040u, true)};
                static uint64_t last[std::size(now)]{};
                uint64_t d[std::size(now)];
                for (size_t i = 0; i < std::size(now); ++i)
                    d[i] = now[i] - last[i], last[i] = now[i];
                logf("[gs-sync] frames=%llu packets=%llu bytes=%llu directRegs=%llu signal=%llu finish=%llu label=%llu "
                     "trx(h2l=%llu l2h=%llu l2l=%llu) l2hReads=%llu latches=%llu nativeUpload=%llu nativePacked=%llu "
                     "csrReads=%llu siglblidReads=%llu busdir=%llu",
                     static_cast<unsigned long long>(g_perfWindow.frames), d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7],
                     d[8], d[9], d[10], d[11], d[12], d[13], d[14], d[15]);
            }
            if (gs_cpu_profile::enabled())
            {
                const auto stages = gs_cpu_profile::take();
                const double frames = static_cast<double>(std::max<uint64_t>(g_perfWindow.frames, 1));
                static const char *names[] = {"triangle", "sprite", "pixel", "texture", "texture-read",
                                              "frame-read", "depth-read", "blend", "frame-write", "depth-write", "page-copy"};
                const uint64_t timerOverhead = gs_cpu_profile::timerOverhead();
                logf("[gs-profile] timer overhead %llu ns/sample", static_cast<unsigned long long>(timerOverhead));
                for (unsigned stage = 0; stage < gs_cpu_profile::Count; ++stage)
                {
                    const double scale = stage == gs_cpu_profile::Triangle || stage == gs_cpu_profile::Sprite ||
                                                 stage == gs_cpu_profile::PageCopy ? 1.0 : 64.0;
                    const uint64_t adjustedNs = stages.nanoseconds[stage] > stages.calls[stage] * timerOverhead
                                                    ? stages.nanoseconds[stage] - stages.calls[stage] * timerOverhead : 0;
                    logf("[gs-profile] %s %.3f ms/frame sampled=%llu raw=%.3f ms/frame", names[stage],
                         static_cast<double>(adjustedNs) * scale / frames / 1e6,
                         static_cast<unsigned long long>(stages.calls[stage]),
                         static_cast<double>(stages.nanoseconds[stage]) * scale / frames / 1e6);
                }
            }
            g_perfWindow = FrameStats{};
            g_perfWindow.presentNs0 = ps2_host_profile::total(ps2_host_profile::kPresent);
            g_perfWindowStartNs = enter;
        }
        traceEntry(rdram, ctx, runtime);
    }

    // 0x1AA1C0 sceGsPutDispEnv: writes PMODE, SMODE2, DISPFB2, DISPLAY2 and BGCOLOR (the display flip).
    // GPU mode runs the device vsync, and so publishes a picture, from sceGsSyncV. The FMV player (OPENING.PSS and the
    // in-game movies) never calls sceGsSyncV: it flips once per VBlank through this function, so the window kept the
    // last pre-movie frame (black after the fade) for the whole movie. A flip with no device vsync in the previous
    // field runs one, after the new display registers are written. In gameplay every flip follows a sceGsSyncV in the
    // same or the next field, so this path does not run there (counted in g_flipVsyncs).
    uint64_t g_flipVsyncs = 0, g_flipVsyncsLogged = 0, g_flipHealthNs = 0;

    void putDispEnvEntry(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (g_frameRateHz != 50)
            frameRateOnFlip(runtime);
        traceEntry(rdram, ctx, runtime);
#ifdef MAXIMO_ENABLE_GPU_SHADOW
        if (!g_gpuMode || !g_gpuShadow || g_gpuFailed.load())
            return;
        const uint64_t tick = runtime->eeScheduler().currentVSyncTick();
        if (tick - g_lastGpuVsyncTick < 2)
            return;
        g_lastGpuVsyncTick = tick;
        ++g_flipVsyncs;
        const uint64_t frame = g_guestFrame.load(std::memory_order_relaxed);
        const uint64_t csr = runtime->memory().gs().csr.load(std::memory_order_relaxed);
        runtime->memory().vu1ThreadFrame(
            [runtime, frame, tick, csr]
            {
                if (!g_gpuShadow->onGuestFrame(frame, false, csr) && !g_gpuFailed.exchange(true))
                {
                    logf("[gpu] display flip at VBlank %llu: GPU frame failed; stopping", static_cast<unsigned long long>(tick));
                    runtime->requestStop();
                }
            },
            false);
        if (g_gpuFailed.load())
            return;
        // sceGsSyncV, which writes the per-second lines, is not called during a movie: report from here instead.
        const uint64_t now = ps2_host_profile::nowNs();
        if (now - g_flipHealthNs >= 1000000000ull)
        {
            const GpuShadow::Health h = g_gpuShadow->takeHealth();
            logf("[gpu-health] 1s published=%llu shown=%llu black=%llu noScanout=%llu failed=%llu | last frame %ux%u "
                 "brightness mean %u max %u | flip vsyncs %llu (no sceGsSyncV)",
                 static_cast<unsigned long long>(h.published), static_cast<unsigned long long>(h.taken),
                 static_cast<unsigned long long>(h.black), static_cast<unsigned long long>(h.noScanout),
                 static_cast<unsigned long long>(h.failed), h.width, h.height, h.brightnessMean, h.brightnessMax,
                 static_cast<unsigned long long>(g_flipVsyncs - g_flipVsyncsLogged));
            g_flipVsyncsLogged = g_flipVsyncs;
            g_flipHealthNs = now;
        }
#else
        (void)rdram, (void)ctx, (void)runtime;
#endif
    }

    // 0x1AC320, called by sceGsSyncV: clear INTC_STAT.VBLANK_START, spin until it is set again, clear it, return.
    //   sw 4,INTC_STAT; loop: lw v0,INTC_STAT; andi v0,4; beqz v0,loop; sw 4,INTC_STAT; jr ra
    // The loop body only reads INTC_STAT. Default: the same 32-cycle checkpoint per spin as the generated code (the
    // IOP is stepped by the EE's checkpoint size, so this keeps the guest trajectory identical), reading INTC_STAT
    // straight from PS2Memory instead of through Load32's MMIO trace hook (those reads are left out of the hardware
    // register report).
    // MAXIMO_SPIN_SKIP=chunk: account up to kVBlankSpinChunk cycles per spin, never past the next scheduled event,
    //   EE timer interrupt or time-slice end. Much cheaper, but the coarser IOP stepping changes the trajectory
    //   (reference frame 1620 differs by 42 pixels), so it is not the default.
    // MAXIMO_SPIN_SKIP=0: the generated loop.
    constexpr uint32_t kVBlankSpinChunk = 1024u;
    bool g_vblankSpinChunked = false;
    void vblankSpinEntry(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        constexpr uint32_t kIntcStat = 0x1000F000u, kVBlankStart = 4u;
        if (ctx->pc != 0x1AC330u) // a resume after a checkpoint enters at the loop
        {
            ctx->pc = 0x1AC32Cu;
            SET_GPR_U32(ctx, 2, kIntcStat);
            SET_GPR_U32(ctx, 3, kVBlankStart);
            runtime->Store32(rdram, ctx, kIntcStat, kVBlankStart);
        }
        for (;;)
        {
            ctx->pc = 0x1AC334u;
            const uint32_t stat = runtime->memory().intcStatus(); // = read32(kIntcStat)
            SET_GPR_U32(ctx, 2, stat & kVBlankStart);
            if ((stat & kVBlankStart) != 0u)
                break;
            ctx->pc = 0x1AC330u;
            if (runtime->eeCheckpointDue(g_vblankSpinChunked ? runtime->eeScheduler().idleCycleBudget(kVBlankSpinChunk)
                                                             : 32u))
                return;
        }
        ctx->pc = 0x1AC35Cu;
        SET_GPR_U32(ctx, 2, kVBlankStart);
        SET_GPR_U32(ctx, 1, 0x10010000u);
        runtime->Store32(rdram, ctx, kIntcStat, kVBlankStart);
        ctx->pc = reg(ctx, 31);
    }

    uint64_t g_frameTimeouts = 0; // frame waits that ended through the game's 2-VBlank reset path
    std::map<uint32_t, Watch> g_watches = {
        {0x001201E0u, {"FLAG screen: show option (a0=option, a1=screen object)", 2}},
        {0x001201A0u, {"FLAG screen: confirm (a0=0x239880 settings, a1=table index)", 2}},
        {0x0014A610u, {"FLAG screen: after confirm 0x14A610", 1}},
        {0x001492D0u, {"FLAG screen: cleanup 0x1492D0", 1}},
        {0x00147AD0u, {"asset/screen open 0x147AD0 (a1=name?)", 2}},
        {0x0011B380u, {"frontend: 0x11B380 (a1=mode)", 2}},
        {0x0011BAB0u, {"frontend: per-frame state 0x11BAB0", 1, true}},
        {0x00181DD0u, {"allocator 0x181DD0 (a0=size)", 1, true}},
        {0x001ABE80u, {"EndOfHeap wrapper 0x1ABE80", 0, true}},
        {0x0019A540u, {"libc malloc 0x19A540", 1, true}},
        {0x00192FB0u, {"stream file Read (a0=file, a1=dst, a2=bytes)", 3}},
        {0x0014ABD0u, {"sound effect request 0x14ABD0 (a1=id)", 2}},
        {0x00160960u, {"VIF1 interrupt handler 0x160960 (a0=cause)", 1}},
        {0x00160DA0u, {"frame wait 0x160DA0 (a0=render context)", 1}},
        {0x00160F20u, {"packet close 0x160F20 (a0=render context)", 1}},
        {0x001610F0u, {"packet DMA start 0x1610F0 (a0=render context)", 1}},
        {0x001611D0u, {"packet reset 0x1611D0 (a0=render context)", 1}},
        {0x001A3C68u, {"DMA reset 0x1A3C68 (ra=0x160E14: frame-wait 2-VBlank timeout path)", 0}},
    };
    constexpr uint64_t kWatchLogCap = 64;

    void watchEntry(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t pc = ctx->pc, ra = reg(ctx, 31);
        Watch &w = g_watches.at(pc);
        if (pc == 0x001A3C68u && ra == 0x00160E14u)
            ++g_frameTimeouts;
        if ((pc == 0x00160F20u || pc == 0x001610F0u || pc == 0x001611D0u || pc == 0x00160DA0u) &&
            std::getenv("PS2_VIF1_IRQ_TRACE") && w.calls < 40)
        {
            const uint32_t rc = reg(ctx, 4) & 0x1FFFFFFu;
            uint32_t cur = 0, size = 0;
            std::memcpy(&cur, g_rdram + rc + 0x80200u, 4);
            std::memcpy(&size, g_rdram + rc + (cur % 32u) * 0x4010u + 0x4000u, 4);
            const uint8_t *slot = g_rdram + rc + (cur % 32u) * 0x4010u;
            std::fprintf(stderr, "[slots] 0x%06X ra=0x%06X cur=%u size=%u inflight=%u queued=%u idle=%u d1chcr=0x%X\n", pc,
                         ra, cur, size, slot[0x4004], slot[0x4005], g_rdram[rc + 0x80208u],
                         runtime->memory().readIORegister(0x10009000u));
        }
        if (pc == 0x00160960u && w.calls < 160)
        {
            // What the handler is about to see: VIF1 mark/stat, the render context's end slot
            // (ctx+0x80200) and the "frame closed" byte behind gp-0x76AC.
            const uint32_t gp = reg(ctx, 28);
            uint32_t rc = 0, flagPtr = 0, endSlot = 0;
            std::memcpy(&rc, g_rdram + ((gp - 0x76F0u) & 0x1FFFFFFu), 4);
            std::memcpy(&flagPtr, g_rdram + ((gp - 0x76ACu) & 0x1FFFFFFu), 4);
            std::memcpy(&endSlot, g_rdram + ((rc + 0x80200u) & 0x1FFFFFFu), 4);
            const auto &v = runtime->memory().vif1_regs;
            logf("VIF1IRQ #%llu mark=%u stat=0x%X endSlot=%u closed=%u d1chcr=0x%X",
                 static_cast<unsigned long long>(w.calls + 1), v.mark, v.stat, endSlot,
                 g_rdram[flagPtr & 0x1FFFFFFu], runtime->memory().readIORegister(0x10009000u));
            if (std::getenv("PS2_VIF1_IRQ_TRACE"))
                std::fprintf(stderr, "[handler] #%llu mark=%u stat=0x%X endSlot=%u\n",
                             static_cast<unsigned long long>(w.calls + 1), v.mark, v.stat, endSlot);
        }
        if (!w.logReturn && w.calls++ < kWatchLogCap)
        {
            char args[96] = {};
            int len = 0;
            for (int i = 0; i < w.nargs; ++i)
                len += std::snprintf(args + len, sizeof(args) - len, "%s0x%X", i ? ", " : "", reg(ctx, 4 + i));
            logf("WATCH 0x%06X %s (%s) [ra=0x%06X, call #%llu]", ctx->pc, w.what, args, reg(ctx, 31),
                 static_cast<unsigned long long>(w.calls));
        }
        if (!w.logReturn)
        {
            traceEntry(rdram, ctx, runtime);
            return;
        }
        ++w.calls;
        traceEntry(rdram, ctx, runtime);
        // A call that yielded (thread switch) returns here before it finishes; only completed calls count.
        if (ctx->pc == ra && GPR_S32(ctx, 2) != w.lastReturn)
        {
            w.lastReturn = GPR_S32(ctx, 2);
            logf("WATCH 0x%06X %s returned %d [call #%llu, pad read #%llu]", pc, w.what, GPR_S32(ctx, 2),
                 static_cast<unsigned long long>(w.calls), static_cast<unsigned long long>(g_padReads));
        }
    }

    // ---- scripted controller input ---------------------------------------------------------------
    //
    // Script: comma-separated "start:BUTTONS:length" steps, BUTTONS joined by '+'. Times count
    // scePadRead calls (the game polls once per frame), so a script replays identically however fast
    // the host runs. Example: "90:RIGHT:4,150:CROSS:4". LUP/LDOWN/LLEFT/LRIGHT push the left analog stick
    // to its limit (the game steers Maximo with it in DualShock analog mode). LX=hh / LY=hh set an exact stick byte.
    // A start written "f<n>" is a guest frame (sceGsSyncV count) instead: the step holds its buttons for the first
    // `length` pad reads from that frame on, or from the <d>th read after it with "f<n>+<d>". Movies poll the pad
    // without drawing frames, so this keeps steps after a movie in place however many reads it took.
    // A step written "t<ms>:BUTTONS:<ms>" is timed in guest milliseconds on the EE clock, so it replays the same at any
    // field rate (A/B runs of MAXIMO_FRAME_RATE).

    struct PadStep
    {
        uint64_t start, length;
        uint16_t pressed; // active-high mask in libpad bit order
        std::string text;
        uint8_t lx = 0x80, ly = 0x80; // left stick, 0x80 = centred
        uint8_t rx = 0x80, ry = 0x80; // right stick
        bool byFrame = false;
        bool byTime = false;    // "t<ms>:...:<ms>": start and length in guest milliseconds (EE clock)
        bool anchored = false;  // "a<ms>:...:<ms>": the same, counted from the MAXIMO_PAD_ANCHOR moment
        uint64_t delay = 0;     // pad reads from the frame to the first press, for a frame-keyed step
        uint64_t readsHeld = 0; // pad reads so far of a frame-keyed step, from its frame on
    };
    std::vector<PadStep> g_padScript;
    uint16_t g_padApplied = 0;

    uint16_t padButton(const std::string &name)
    {
        static const std::map<std::string, uint16_t> names = {
            {"SELECT", 1u << 0}, {"L3", 1u << 1},       {"R3", 1u << 2},      {"START", 1u << 3},
            {"UP", 1u << 4},     {"RIGHT", 1u << 5},    {"DOWN", 1u << 6},    {"LEFT", 1u << 7},
            {"L2", 1u << 8},     {"R2", 1u << 9},       {"L1", 1u << 10},     {"R1", 1u << 11},
            {"TRIANGLE", 1u << 12}, {"CIRCLE", 1u << 13}, {"CROSS", 1u << 14}, {"SQUARE", 1u << 15},
        };
        const auto it = names.find(name);
        return it == names.end() ? 0 : it->second;
    }

    bool parsePadScript(const std::string &script)
    {
        size_t pos = 0;
        while (pos < script.size())
        {
            size_t end = script.find(',', pos);
            if (end == std::string::npos)
                end = script.size();
            const std::string step = script.substr(pos, end - pos);
            pos = end + 1;
            const size_t a = step.find(':'), b = step.rfind(':');
            if (a == std::string::npos || a == b)
                return false;
            const bool byFrame = step[0] == 'f', anchored = step[0] == 'a', byTime = step[0] == 't' || anchored;
            PadStep s{std::stoull(step.substr(byFrame || byTime ? 1 : 0, a)), std::stoull(step.substr(b + 1)), 0, step};
            s.byFrame = byFrame;
            s.byTime = byTime;
            s.anchored = anchored;
            if (const size_t plus = step.find('+'); byFrame && plus < a)
                s.delay = std::stoull(step.substr(plus + 1, a - plus - 1));
            const std::string buttons = step.substr(a + 1, b - a - 1);
            size_t p = 0;
            while (p <= buttons.size())
            {
                size_t q = buttons.find('+', p);
                if (q == std::string::npos)
                    q = buttons.size();
                const std::string name = buttons.substr(p, q - p);
                const uint16_t m = padButton(name);
                if (name == "LUP")
                    s.ly = 0x00;
                else if (name == "LDOWN")
                    s.ly = 0xFF;
                else if (name == "LLEFT")
                    s.lx = 0x00;
                else if (name == "LRIGHT")
                    s.lx = 0xFF;
                else if (name == "RUP")
                    s.ry = 0x00;
                else if (name == "RDOWN")
                    s.ry = 0xFF;
                else if (name == "RLEFT")
                    s.rx = 0x00;
                else if (name == "RRIGHT")
                    s.rx = 0xFF;
                else if (name.size() > 3 && (name.compare(0, 3, "LX=") == 0 || name.compare(0, 3, "LY=") == 0))
                    (name[1] == 'X' ? s.lx : s.ly) = static_cast<uint8_t>(std::stoul(name.substr(3), nullptr, 16));
                else if (!m)
                    return false;
                s.pressed |= m;
                p = q + 1;
            }
            g_padScript.push_back(s);
        }
        return true;
    }

    void scriptedPadRead(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint64_t n = g_padReads++;
        const uint64_t guestMs = runtime->eeScheduler().currentCycle() / (EeScheduler::kEeClockHz / 1000u);
        // MAXIMO_PAD_ANCHOR=<hex address>: "a" steps count from the first pad read at which that word is non-zero
        // (for A/B runs: e.g. the player's position, so input lines up with the level start at any frame rate).
        static const uint32_t anchorAddr = []
        {
            const char *text = std::getenv("MAXIMO_PAD_ANCHOR");
            return text ? static_cast<uint32_t>(std::strtoul(text, nullptr, 16)) & ~3u : 0u;
        }();
        static uint64_t anchorMs = UINT64_MAX;
        if (anchorAddr && anchorMs == UINT64_MAX && rd32(anchorAddr) != 0)
        {
            anchorMs = guestMs;
            logf("pad script anchor: 0x%06X set at guest %llu ms, pad read #%llu", anchorAddr,
                 static_cast<unsigned long long>(guestMs), static_cast<unsigned long long>(n));
        }
        // MAXIMO_RAM_DUMP_MS=ms0,ms1,...: RDRAM to ram_a<ms>.bin at the first pad read that many guest ms after the
        // anchor (A/B runs compare the game's state at the same game time).
        if (static const char *dumps = std::getenv("MAXIMO_RAM_DUMP_MS"); dumps && anchorMs != UINT64_MAX)
        {
            static size_t next = 0;
            static std::vector<uint64_t> times = [&]
            {
                std::vector<uint64_t> v;
                for (const char *p = dumps; *p;)
                {
                    char *end = nullptr;
                    v.push_back(std::strtoull(p, &end, 10));
                    if (end == p)
                        break;
                    p = *end == ',' ? end + 1 : end;
                }
                return v;
            }();
            while (next < times.size() && guestMs - anchorMs >= times[next])
            {
                char name[48];
                std::snprintf(name, sizeof(name), "ram_a%llu.bin", static_cast<unsigned long long>(times[next]));
                std::ofstream(name, std::ios::binary).write(reinterpret_cast<const char *>(g_rdram), PS2_RAM_SIZE);
                logf("RAM dump %s at anchor + %llu ms, frame %llu", name,
                     static_cast<unsigned long long>(guestMs - anchorMs), static_cast<unsigned long long>(g_guestFrame.load()));
                ++next;
            }
        }
        uint16_t pressed = 0;
        uint8_t lx = 0x80, ly = 0x80, rx = 0x80, ry = 0x80;
        for (PadStep &s : g_padScript)
        {
            if (s.anchored && anchorMs == UINT64_MAX)
                continue;
            const uint64_t ms = s.anchored ? guestMs - anchorMs : guestMs;
            if (s.byTime ? ms >= s.start && ms < s.start + s.length
                : s.byFrame ? g_guestFrame.load(std::memory_order_relaxed) >= s.start && s.readsHeld++ >= s.delay &&
                                  s.readsHeld <= s.delay + s.length
                            : n >= s.start && n < s.start + s.length)
            {
                pressed |= s.pressed;
                lx = s.lx != 0x80 ? s.lx : lx;
                ly = s.ly != 0x80 ? s.ly : ly;
                rx = s.rx != 0x80 ? s.rx : rx;
                ry = s.ry != 0x80 ? s.ry : ry;
            }
        }
        const bool stickMoved = lx != 0x80 || ly != 0x80 || rx != 0x80 || ry != 0x80;
        // MAXIMO_RAM_DUMP=n0,n1,...: write all of RDRAM to ram_<n>.bin at these pad reads (diagnostic snapshots).
        if (static const char *dumps = std::getenv("MAXIMO_RAM_DUMP"); dumps)
            for (const char *p = dumps; *p;)
            {
                char *end = nullptr;
                const uint64_t wanted = std::strtoull(p, &end, 10);
                if (end == p)
                    break;
                if (wanted == n)
                {
                    char name[48];
                    std::snprintf(name, sizeof(name), "ram_%llu.bin", static_cast<unsigned long long>(n));
                    std::ofstream(name, std::ios::binary).write(reinterpret_cast<const char *>(g_rdram), PS2_RAM_SIZE);
                    logf("RAM dump %s at pad read #%llu, frame %llu", name, static_cast<unsigned long long>(n),
                         static_cast<unsigned long long>(g_guestFrame.load()));
                }
                p = *end == ',' ? end + 1 : end;
            }
        // MAXIMO_PEEK=first:last:addr/count[,addr#count...]: log `count` floats (/) or hex words (#) at each addr on
        // pad reads [first, last].
        if (static const char *peek = std::getenv("MAXIMO_PEEK"); peek)
        {
            unsigned long long first = 0, last = 0;
            int used = 0;
            if (std::sscanf(peek, "%llu:%llu:%n", &first, &last, &used) == 2 && n >= first && n <= last)
            {
                std::string line;
                for (const char *p = peek + used; *p;)
                {
                    unsigned addr = 0, count = 0;
                    char kind = 0;
                    int len = 0;
                    if (std::sscanf(p, "%x%c%u%n", &addr, &kind, &count, &len) != 3 || (kind != '/' && kind != '#'))
                        break;
                    char b[32];
                    std::snprintf(b, sizeof b, " %X:", addr);
                    line += b;
                    for (unsigned i = 0; i < count; ++i)
                    {
                        if (kind == '#')
                            std::snprintf(b, sizeof b, " %X", rd32(addr + 4 * i));
                        else
                        {
                            float f;
                            std::memcpy(&f, mem(addr + 4 * i), 4);
                            std::snprintf(b, sizeof b, " %.4g", f);
                        }
                        line += b;
                    }
                    p += len;
                    p += *p == ',';
                }
                logf("PEEK #%llu f%llu t%llu%s", static_cast<unsigned long long>(n),
                     static_cast<unsigned long long>(g_guestFrame.load()), static_cast<unsigned long long>(guestMs),
                     line.c_str());
            }
        }
        // The probes below look at most once per guest VBlank. In play the game reads the pad once per frame, but
        // during a movie it polls it ~30,000 times a second, and probing every read cost 5-7% of the game thread.
        static uint64_t probeTick = ~0ull;
        const uint64_t tick = runtime->eeScheduler().currentVSyncTick();
        const bool probe = tick != probeTick;
        probeTick = tick;
        // SFX slot monitor: the game's 7 effect slots (halfwords at 0x26271C: 0x3FFF free, |0x4000 requested,
        // |0x8000 playing, 0x3FFE/0xBFFE stopping) and their voice status bytes (0x1E5A42.., one per slot), logged
        // whenever they change. A slot stuck non-free is a leaked voice ("Out of free channels").
        if (probe)
        {
            static uint8_t lastSlots[34] = {};
            uint8_t now[34] = {};
            // MJOLNIR's own per-channel state (SPU_KeyStatus, IOP 0x15EA4; 2 = owned by a stream).
            uint8_t *keyStatus = now + 25;
            (void)g_runtime->readIopMemory(0x15EA4u, keyStatus, 9);
            std::memcpy(now, mem(0x26271Cu), 14);
            std::memcpy(now + 14, mem(0x1E5A42u), 7);
            // MJOLNIR's reply (0x2EC980): stream count, 3 words per stream, 5 words, then 2 status bits per channel.
            const uint32_t replyStatus = rd32(0x2EC980u + 4u * (3u * (rd32(0x2EC980u) & 0xFFu) + 6u));
            std::memcpy(now + 21, &replyStatus, 4);
            if (std::memcmp(now, lastSlots, sizeof now) != 0)
            {
                std::memcpy(lastSlots, now, sizeof now);
                char line[160];
                int len = 0;
                for (int i = 0; i < 7; ++i)
                {
                    uint16_t s;
                    std::memcpy(&s, now + 2 * i, 2);
                    len += std::snprintf(line + len, sizeof line - len, " %04X/%u", s, now[14 + i]);
                }
                logf("SFX slots #%llu f%llu%s | reply streams %u ch0-15 %08X | IOP keystatus ch0-8 %u%u%u%u%u%u%u%u%u",
                     static_cast<unsigned long long>(n), static_cast<unsigned long long>(g_guestFrame.load()), line,
                     rd32(0x2EC980u), replyStatus, keyStatus[0], keyStatus[1], keyStatus[2], keyStatus[3],
                     keyStatus[4], keyStatus[5], keyStatus[6], keyStatus[7], keyStatus[8]);
            }
        }
        // Game-side view: FLAG screen fade-busy byte and the pad object's raw/edge button words
        // (pad object 0x263540: +10 raw, +14 newly pressed), as left by the previous frame.
        const bool periodic = probe && tick % 30 == 0;
        if (periodic || pressed != g_padApplied)
            logf("PAD probe read #%llu: fadeBusy[0x2633B6]=%u raw=0x%04X edge=0x%04X type=%u", static_cast<unsigned long long>(n),
                 *mem(0x002633B6u), rd32(0x0026354Au) & 0xFFFFu, rd32(0x0026354Eu) & 0xFFFFu, *mem(0x00263558u));
        if (periodic)
        {
            // Fade state in the game object 0x25B9E0: +16 frame delta, +0x79D8 rate, +0x79DC value, +0x79E0 target.
            auto f = [](uint32_t a) { float v; std::memcpy(&v, mem(a), 4); return v; };
            logf("FADE probe: dt=%g rate=%g value=%g target=%g", f(0x0025B9F0u), f(0x002633B8u), f(0x002633BCu),
                 f(0x002633C0u));
        }
        if (pressed != g_padApplied)
        {
            logf("PAD read #%llu: buttons 0x%04X -> 0x%04X (active-high, libpad order)",
                 static_cast<unsigned long long>(n), g_padApplied, pressed);
            g_padApplied = pressed;
        }
        if (pressed || stickMoved)
            ps2_stubs::setPadOverrideState(static_cast<uint16_t>(~pressed), lx, ly, rx, ry);
        else
            ps2_stubs::clearPadOverrideState();
        const uint32_t dataAddr = reg(ctx, 6);
        ps2_stubs::scePadRead(rdram, ctx, runtime);
        // The free camera's input: right stick and R3 from the data the game just got (analog mode, ID 0x7_).
        if (g_orbit.on && GPR_S32(ctx, 2) != 0 && dataAddr)
        {
            const uint8_t *d = mem(dataAddr);
            static int lastId = -1;
            if (d[1] != lastId)
            {
                logf("camera orbit: pad ID %02X -> %02X (%s) at frame %llu", lastId & 0xFF, d[1],
                     (d[1] & 0xF0) == 0x70 ? "analog: right stick read" : "not analog: no right stick",
                     static_cast<unsigned long long>(g_guestFrame.load()));
                lastId = d[1];
            }
            if ((d[1] & 0xF0) == 0x70)
            {
                g_orbit.rx = d[4];
                g_orbit.ry = d[5];
                g_orbit.r3 = (d[2] & 0x04) == 0; // active-low, R3 = bit 2 of the first button byte
            }
            else
            {
                g_orbit.rx = g_orbit.ry = 0x80;
                g_orbit.r3 = false;
            }
        }
    }

    // ---- MMIO trace ----------------------------------------------------------------------------

    struct MmioStat
    {
        uint64_t reads = 0, writes = 0;
        uint64_t lastWrite = 0;
        uint32_t lastPc = 0;
    };
    std::map<uint32_t, MmioStat> g_mmio;

    uint64_t mmioAccesses(uint32_t phys, bool writes)
    {
        const auto it = g_mmio.find(phys);
        return it == g_mmio.end() ? 0u : (writes ? it->second.writes : it->second.reads);
    }
    uint64_t g_scratchAccesses = 0, g_uncachedAccesses = 0;
    constexpr uint64_t kMmioEventsPerReg = 8;

    const char *mmioUnit(uint32_t a)
    {
        if (a >= 0x12000000u && a < 0x12002000u) return "GS-priv";
        if (a >= 0x10000000u && a < 0x10002000u) return "TIMER";
        if (a >= 0x10002000u && a < 0x10003000u) return "IPU";
        if (a >= 0x10003000u && a < 0x10003800u) return "GIF";
        if (a >= 0x10003800u && a < 0x10003C00u) return "VIF0";
        if (a >= 0x10003C00u && a < 0x10004000u) return "VIF1";
        if (a >= 0x10004000u && a < 0x10008000u) return "FIFO";
        if (a >= 0x10008000u && a < 0x1000E000u) return "DMAC-ch";
        if (a >= 0x1000E000u && a < 0x1000F000u) return "DMAC";
        if (a >= 0x1000F000u && a < 0x10010000u) return "INTC/SIF/misc";
        if (a >= 0x11000000u && a < 0x11010000u) return "VU-mem";
        return "other";
    }

    void mmioHook(bool isWrite, uint32_t address, uint32_t size, uint64_t lo, uint64_t, const R5900Context *ctx)
    {
        if (address >= 0x70000000u && address < 0x70004000u)
        {
            ++g_scratchAccesses;
            return;
        }
        const uint32_t phys = address & 0x1FFFFFFFu;
        if (phys < PS2_RAM_SIZE)
        {
            ++g_uncachedAccesses; // kseg1 / uncached-accelerated RAM mirrors
            return;
        }
        // Polling loops hit the same register repeatedly; std::map nodes are stable, so cache the last one.
        thread_local uint32_t lastPhys = 0xFFFFFFFFu;
        thread_local MmioStat *lastStat = nullptr;
        if (phys != lastPhys)
        {
            lastStat = &g_mmio[phys];
            lastPhys = phys;
        }
        MmioStat &s = *lastStat;
        const uint64_t n = isWrite ? s.writes++ : s.reads++;
        if (isWrite)
            s.lastWrite = lo;
        s.lastPc = ctx ? ctx->pc : 0;
        if (n < kMmioEventsPerReg)
            logf("HW %-5s %-13s 0x%08X size=%u %s0x%llX pc~0x%06X", isWrite ? "WRITE" : "READ", mmioUnit(phys), phys,
                 size, isWrite ? "value=" : "", static_cast<unsigned long long>(isWrite ? lo : 0), s.lastPc);
        else if (n == kMmioEventsPerReg)
            logf("HW %-5s %-13s 0x%08X further %s not logged individually", isWrite ? "WRITE" : "READ", mmioUnit(phys),
                 phys, isWrite ? "writes" : "reads");
    }

    // ---- wrappers around runtime SDK implementations ---------------------------------------------

    using Fn = PS2Runtime::RecompiledFunction;

    struct Bound
    {
        const char *name;
        Fn impl;
        int nargs;
        uint64_t calls = 0;
    };
    std::map<uint32_t, Bound> g_bound;

    void boundEntry(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t pc = ctx->pc;
        const uint32_t ra = reg(ctx, 31);
        Bound &b = g_bound.at(pc);
        const uint64_t n = b.calls++;
        static const int argRegs[] = {4, 5, 6, 7, 8, 9, 10, 11};
        char args[160] = {};
        int len = 0;
        for (int i = 0; i < b.nargs; ++i)
            len += std::snprintf(args + len, sizeof(args) - len, "%s0x%X", i ? ", " : "", reg(ctx, argRegs[i]));
        if (pc == 0x001A2BF8u) // sceCdStRead(sectors, buffer, mode, err)
            checkGuardWrite("sceCdStRead", reg(ctx, 5), reg(ctx, 4) * 2048u, ra);
        b.impl(rdram, ctx, runtime);
        if (ctx->pc == pc)
            ctx->pc = ra;
        if (n < 32 || (n & (n - 1)) == 0)
            logf("SDK %s(%s) -> %d  [call #%llu, ra=0x%06X]", b.name, args, GPR_S32(ctx, 2),
                 static_cast<unsigned long long>(n + 1), ra);
    }

    // ---- Native audio (MAXIMO_AUDIO=1) ---------------------------------------------------------------
    // The game's own sound driver MJOLNIR.IRX runs in the emulated IOP (ps2xIOP) over a native libsd and SPU2,
    // and serves the sound RPC (sid 0x12345) itself. Without the switch the driver is not run and its RPC is
    // logged and answered with zeroes, as before.
    // MAXIMO_AUDIO_WAV=<file>: also writes the mixed SPU2 output (48 kHz stereo, guest time order) as a WAV file.
    bool g_iopAudio = false;
    constexpr uint32_t kSoundDriverSid = 0x00012345u;
    struct AudioCapture
    {
        std::FILE *wav = nullptr;
        uint64_t frames = 0;
        uint64_t hash = 0xCBF29CE484222325ull; // FNV-1a over the little-endian samples
        int32_t peak = 0;
        double sumSquares = 0.0;
        uint64_t nonzeroFrames = 0;
        uint64_t firstNonzeroFrame = UINT64_MAX;
        // per guest second
        int32_t secondPeak = 0;
        double secondSquares = 0.0;
        uint64_t secondNonzero = 0;
    };
    AudioCapture g_audio;
    uint64_t g_soundRpcCalls = 0;

    // MAXIMO_AUDIO_WAV is written by its own thread: the game thread only appends to a queue, so disk writes (and
    // their occasional stalls) never hold up the emulation.
    struct WavWriter
    {
        std::mutex mutex;
        std::condition_variable wake;
        std::vector<int16_t> pending;
        bool stop = false;
        std::thread thread;

        void start(std::FILE *file)
        {
            thread = std::thread([this, file]()
                                 {
                std::vector<int16_t> chunk;
                for (;;)
                {
                    {
                        std::unique_lock<std::mutex> lock(mutex);
                        wake.wait(lock, [this]() { return stop || !pending.empty(); });
                        if (pending.empty() && stop)
                            return;
                        chunk.swap(pending);
                    }
                    std::fwrite(chunk.data(), sizeof(int16_t), chunk.size(), file);
                    chunk.clear();
                } });
        }
        void append(const int16_t *samples, size_t count)
        {
            {
                std::lock_guard<std::mutex> lock(mutex);
                pending.insert(pending.end(), samples, samples + count);
            }
            wake.notify_one();
        }
        void finish()
        {
            if (!thread.joinable())
                return;
            {
                std::lock_guard<std::mutex> lock(mutex);
                stop = true;
            }
            wake.notify_one();
            thread.join();
        }
    };
    WavWriter g_wavWriter;

    void writeWavHeader(std::FILE *f, uint64_t frames)
    {
        const uint32_t dataBytes = static_cast<uint32_t>(std::min<uint64_t>(frames * 4u, 0xFFFFFFF0ull - 36u));
        auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
        auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
        std::fseek(f, 0, SEEK_SET);
        std::fwrite("RIFF", 1, 4, f);
        u32(36u + dataBytes);
        std::fwrite("WAVEfmt ", 1, 8, f);
        u32(16u);
        u16(1u);     // PCM
        u16(2u);     // stereo
        u32(48000u); // SPU2 output rate
        u32(48000u * 4u);
        u16(4u);
        u16(16u);
        std::fwrite("data", 1, 4, f);
        u32(dataBytes);
        std::fseek(f, 0, SEEK_END);
    }

    void onIopAudio(const int16_t *samples, size_t frames)
    {
        AudioCapture &a = g_audio;
        for (size_t i = 0; i < frames; ++i)
        {
            const int16_t l = samples[2 * i], r = samples[2 * i + 1];
            for (const int16_t v : {l, r})
            {
                const uint16_t u = static_cast<uint16_t>(v);
                a.hash = (a.hash ^ (u & 0xFFu)) * 0x100000001B3ull;
                a.hash = (a.hash ^ (u >> 8)) * 0x100000001B3ull;
                const int32_t m = std::abs(static_cast<int32_t>(v));
                a.peak = std::max(a.peak, m);
                a.secondPeak = std::max(a.secondPeak, m);
                a.sumSquares += static_cast<double>(v) * v;
                a.secondSquares += static_cast<double>(v) * v;
            }
            if (l || r)
            {
                ++a.nonzeroFrames;
                ++a.secondNonzero;
                if (a.firstNonzeroFrame == UINT64_MAX)
                    a.firstNonzeroFrame = a.frames;
            }
            ++a.frames;
            if (a.frames % 48000u == 0u)
            {
                logf("[audio] guest second %llu: peak=%d rms=%.1f nonzero frames=%llu",
                     static_cast<unsigned long long>(a.frames / 48000u), a.secondPeak,
                     std::sqrt(a.secondSquares / (2.0 * 48000.0)), static_cast<unsigned long long>(a.secondNonzero));
                a.secondPeak = 0;
                a.secondSquares = 0.0;
                a.secondNonzero = 0;
            }
        }
        if (a.wav)
            g_wavWriter.append(samples, frames * 2u);
    }

    // Per-window audio pipeline report: IOP interpretation and SPU2 mixing cost on the game thread, samples produced
    // against samples the host device consumed, and the host ring's fill level (frames at 48 kHz).
    void logAudioWindow(const char *label, double seconds)
    {
        if (!g_iopAudio || seconds <= 0.0)
            return;
        static ps2x::iop::HostTiming last{};
        static uint64_t lastIopCycles = 0, lastEeCycles = 0, lastVBlank = 0;
        const ps2x::iop::DebugSnapshot iop = g_runtime->iopDebugSnapshot();
        const ps2x::iop::HostTiming &t = iop.hostTiming;
        const PS2Runtime::IopAudioStats a = g_runtime->takeIopAudioStats();
        const uint64_t eeCycles = g_runtime->eeScheduler().currentCycle();
        const uint64_t vblank = g_runtime->eeScheduler().currentVSyncTick();
        const uint64_t samples = t.spu2Samples - last.spu2Samples;
        const double runMs = static_cast<double>(t.runNs - last.runNs) / 1e6;
        const double spu2Ms = static_cast<double>(t.spu2Ns - last.spu2Ns) / 1e6;
        const double outputMs = static_cast<double>(t.outputNs - last.outputNs) / 1e6;
        const uint64_t vblanks = vblank - lastVBlank;
        logf("[audio-perf] %s IOP %.1f ms/s (%llu active runs, %.2f M IOP cycles) SPU2 %.1f ms/s (mix %.0f ns/sample, "
             "output %.1f ms/s, longest sync %.2f ms, longest output %.2f ms) | guest: %llu samples in %llu VBlanks "
             "(%.0f per VBlank, %.0f EE cycles per VBlank) | host: produced %llu consumed %llu silent %llu dropped %llu "
             "underruns %llu callbacks %llu max gap %.1f ms | ring min %llu max %llu now %llu frames, pacing %+d ppm | "
             "clock %s (ticks %llu fallbacks %llu anchors %llu) IOP-EE lead %+.1f ms",
             label, runMs / seconds, static_cast<unsigned long long>(t.activeRuns - last.activeRuns),
             static_cast<double>(iop.emulatorCycles - lastIopCycles) / 1e6, spu2Ms / seconds,
             samples ? (spu2Ms - outputMs) * 1e6 / static_cast<double>(samples) : 0.0, outputMs / seconds,
             static_cast<double>(t.spu2MaxNs) / 1e6, static_cast<double>(t.outputMaxNs) / 1e6,
             static_cast<unsigned long long>(samples), static_cast<unsigned long long>(vblanks),
             vblanks ? static_cast<double>(samples) / static_cast<double>(vblanks) : 0.0,
             vblanks ? static_cast<double>(eeCycles - lastEeCycles) / static_cast<double>(vblanks) : 0.0,
             static_cast<unsigned long long>(a.framesQueued), static_cast<unsigned long long>(a.framesRequested),
             static_cast<unsigned long long>(a.silentFrames), static_cast<unsigned long long>(a.droppedFrames),
             static_cast<unsigned long long>(a.underruns), static_cast<unsigned long long>(a.callbacks),
             static_cast<double>(a.maxCallbackGapUs) / 1e3, static_cast<unsigned long long>(a.minBuffered),
             static_cast<unsigned long long>(a.maxBuffered), static_cast<unsigned long long>(a.buffered), a.pacingPpm,
             a.realTime ? "realtime" : "lockstep", static_cast<unsigned long long>(a.realTimeTicks),
             static_cast<unsigned long long>(a.realTimeFallbacks), static_cast<unsigned long long>(a.realTimeAnchors),
             (static_cast<double>(iop.emulatorCycles) - static_cast<double>(eeCycles) / 8.0) / 36864.0);
        last = t;
        lastIopCycles = iop.emulatorCycles;
        lastEeCycles = eeCycles;
        lastVBlank = vblank;
    }

    void finishAudio()
    {
        if (!g_iopAudio)
            return;
        AudioCapture &a = g_audio;
        logf("[audio] SPU2 output: %llu frames (%.2f s), hash=%016llX peak=%d rms=%.1f nonzero frames=%llu first at "
             "%.3f s; sound RPC calls served by MJOLNIR: %llu",
             static_cast<unsigned long long>(a.frames), a.frames / 48000.0, static_cast<unsigned long long>(a.hash),
             a.peak, a.frames ? std::sqrt(a.sumSquares / (2.0 * a.frames)) : 0.0,
             static_cast<unsigned long long>(a.nonzeroFrames),
             a.firstNonzeroFrame == UINT64_MAX ? -1.0 : a.firstNonzeroFrame / 48000.0,
             static_cast<unsigned long long>(g_soundRpcCalls));
        logf("[audio] host playback: %llu underruns, %llu frames dropped",
             static_cast<unsigned long long>(g_runtime->iopAudioUnderruns()),
             static_cast<unsigned long long>(g_runtime->iopAudioDroppedFrames()));
        if (a.wav)
        {
            g_wavWriter.finish();
            writeWavHeader(a.wav, a.frames);
            std::fclose(a.wav);
            a.wav = nullptr;
        }
        const ps2x::iop::DebugSnapshot iop = g_runtime->iopDebugSnapshot();
        logf("[audio] IOP: %llu cycles, %llu instructions, %u modules, %u threads, %u RPC servers",
             static_cast<unsigned long long>(iop.emulatorCycles), static_cast<unsigned long long>(iop.emulatorInstructions),
             iop.emulatorLoadedModules, iop.emulatorThreads, iop.emulatorRpcServers);
    }

    // ---- SIF / IOP stubs -----------------------------------------------------------------------

    struct RpcServer
    {
        uint32_t sid;
        uint64_t calls = 0;
    };
    std::vector<RpcServer> g_servers; // index = fake IOP server address slot
    constexpr uint32_t kFakeServerBase = 0x001F0000u; // IOP-side addresses handed back to the EE
    std::set<std::string> g_loadedModules;
    int32_t g_nextModuleId = 1;
    std::map<uint32_t, uint64_t> g_unhandledRpc; // (sid << 8 | fn) -> count

    const char *sidName(uint32_t sid)
    {
        switch (sid)
        {
        case 0x80000001: return "FILEIO";
        case 0x80000003: return "IOP heap";
        case 0x80000006: return "LOADFILE";
        case 0x80000100: return "PADMAN";
        case 0x80000101: return "PADMAN ext";
        case 0x80000400: return "MCSERV";
        case 0x80000592: return "CDVD init/N";
        case 0x80000593: return "CDVD S-cmd";
        case 0x80000595: return "CDVD N-cmd";
        case 0x80000597: return "CDVD search";
        case 0x80000701: return "LIBSD";
        default: return "?";
        }
    }

    // "cdrom0:\PSXIRX\SIO2MAN.IRX;1" -> <disc>/PSXIRX/SIO2MAN.IRX
    fs::path resolveDiscPath(const std::string &ps2)
    {
        std::string p = ps2;
        const size_t colon = p.find(':');
        if (colon != std::string::npos)
            p = p.substr(colon + 1);
        const size_t semi = p.rfind(';');
        if (semi != std::string::npos)
            p = p.substr(0, semi);
        std::replace(p.begin(), p.end(), '\\', '/');
        while (!p.empty() && p.front() == '/')
            p.erase(p.begin());
        return g_disc / fs::path(p);
    }

    void stub_sceSifInitRpc(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        logf("IOP sceSifInitRpc(mode=%u) [ra=0x%06X]", reg(ctx, 4), reg(ctx, 31));
        ret(ctx, 0);
    }

    void stub_sceSifRebootIop(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        const std::string img = cstr(reg(ctx, 4));
        const fs::path host = resolveDiscPath(img);
        const bool ok = fs::is_regular_file(host);
        logf("IOP sceSifRebootIop(\"%s\") host=%s exists=%d -> %d (IOP reset: modules cleared, replacement "
             "image not interpreted)",
             img.c_str(), host.string().c_str(), ok, ok ? 1 : 0);
        g_loadedModules.clear();
        g_servers.clear();
        ret(ctx, ok ? 1 : 0);
    }

    void stub_sceSifSyncIop(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        logf("IOP sceSifSyncIop() -> 1");
        ret(ctx, 1);
    }

    void stub_sceSifLoadModule(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        const std::string path = cstr(reg(ctx, 4));
        const int32_t argLen = static_cast<int32_t>(reg(ctx, 5));
        const fs::path host = resolveDiscPath(path);
        const bool ok = fs::is_regular_file(host);
        const int32_t id = ok ? g_nextModuleId++ : -203; // -ENOENT as returned by LOADFILE
        if (ok)
            g_loadedModules.insert(host.filename().string());
        if (g_iopAudio && ok && host.filename() == "MJOLNIR.IRX")
        {
            std::vector<uint8_t> args(argLen > 0 ? static_cast<size_t>(argLen) : 0u);
            if (!args.empty())
                std::memcpy(args.data(), mem(reg(ctx, 6)), args.size());
            const ps2x::iop::ModuleLoadResult r =
                g_runtime->loadIopModule(path, args.empty() ? nullptr : args.data(), static_cast<uint32_t>(args.size()));
            logf("IOP sceSifLoadModule(\"%s\", arglen=%d) -> %d: executed in the emulated IOP (IRX id %d, start %d)",
                 path.c_str(), argLen, id, r.moduleId, r.startResult);
            ret(ctx, r.moduleId > 0 ? id : -1);
            return;
        }
        logf("IOP sceSifLoadModule(\"%s\", arglen=%d, args=0x%X) host=%s size=%lld -> %d (module not executed)",
             path.c_str(), argLen, reg(ctx, 6), host.string().c_str(),
             ok ? static_cast<long long>(fs::file_size(host)) : -1LL, id);
        ret(ctx, id);
    }

    // SifRpcClientData: hdr{pkt_addr, rpc_id, sema_id, mode} command buff cbuff end_function end_param server
    constexpr uint32_t kCdCommand = 16, kCdBuff = 20, kCdCbuff = 24, kCdServer = 36;

    void stub_sceSifBindRpc(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        const uint32_t cd = reg(ctx, 4), sid = reg(ctx, 5), mode = reg(ctx, 6);
        uint32_t index = 0;
        while (index < g_servers.size() && g_servers[index].sid != sid)
            ++index;
        if (index == g_servers.size())
            g_servers.push_back({sid});
        const uint32_t server = kFakeServerBase + index * 0x40u;
        wr32(cd + 0, 0);
        wr32(cd + 8, 0xFFFFFFFFu);
        wr32(cd + kCdCommand, 0);
        wr32(cd + kCdBuff, server + 0x20u);
        wr32(cd + kCdCbuff, server + 0x30u);
        wr32(cd + kCdServer, server);
        logf("IOP sceSifBindRpc(cd=0x%X, sid=0x%08X %s, mode=%u) -> 0, server=0x%X [ra=0x%06X]", cd, sid, sidName(sid),
             mode, server, reg(ctx, 31));
        ret(ctx, 0);
    }

    // FILEIO 2200 (sid 0x80000001), read-only, backed by the extracted disc. Every request packet starts with
    // {sema, dest, destSize}: the IOP acknowledges the RPC with a nonzero reply word, later sends the result
    // (copied to dest by the EE handler 0x1AF7F0 from the buffer registered by fn 255) and signals sema. The
    // HLE completes the request before the RPC returns, so the library's WaitSema passes at once.
    // Packets: open {.., flags, mode, name}, close {.., fd}, read {.., fd, buf, size}, lseek {.., fd, offset, whence}.
    struct FioFile
    {
        std::string name;
        std::vector<uint8_t> data; // whole file; Maximo's FILEIO use is the FMV files
        uint32_t pos = 0;
    };
    std::map<int32_t, FioFile> g_fioFiles;
    int32_t g_fioNextFd = 1;
    uint64_t g_fioReads = 0, g_fioReadBytes = 0;

    constexpr int32_t kFioENOENT = -2, kFioEBADF = -9, kFioEACCES = -13, kFioENODEV = -19, kFioEINVAL = -22;

    void completeFio(uint32_t send, uint32_t recv, uint32_t rsize, int32_t result)
    {
        if (rsize >= 4)
            wr32(recv, 1); // accepted
        const uint32_t dest = rd32(send + 4), destSize = rd32(send + 8);
        if (dest != 0 && destSize >= 4)
            wr32(dest, static_cast<uint32_t>(result));
        g_runtime->eeScheduler().signalSemaphore(static_cast<int>(rd32(send)), true);
    }

    bool serveFileIo(uint32_t fn, uint32_t send, uint32_t ssize, uint32_t recv, uint32_t rsize, std::string &note)
    {
        char b[512];
        if (fn == 255 && rsize >= 4) // init: the module's version, compared with "2200" at 0x1E6E1C
        {
            std::memcpy(mem(recv), "2200", 4);
            std::snprintf(b, sizeof b, "version \"2200\", result buffer 0x%X", ssize >= 4 ? rd32(send) : 0);
            note = b;
            return true;
        }
        if (ssize < 12)
            return false;
        if (fn == 0 && ssize > 20) // open
        {
            const std::string name = cstr(send + 20);
            const uint32_t flags = rd32(send + 12);
            int32_t result;
            if (name.rfind("cdrom0:", 0) != 0)
                result = kFioENODEV; // only the disc is provided (no rom0:, host:, mc0: through FILEIO)
            else if ((flags & 3u) != 1u)
                result = kFioEACCES; // the disc is read-only
            else
            {
                const fs::path host = resolveDiscPath(name);
                std::ifstream in(host, std::ios::binary);
                if (!in)
                    result = kFioENOENT;
                else
                {
                    FioFile file;
                    file.name = name;
                    file.data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
                    result = g_fioNextFd++;
                    g_fioFiles[result] = std::move(file);
                }
            }
            std::snprintf(b, sizeof b, "open \"%s\" flags=0x%X -> %d", name.c_str(), flags, result);
            note = b;
            completeFio(send, recv, rsize, result);
            return true;
        }
        const int32_t fd = static_cast<int32_t>(rd32(send + 12));
        const auto it = g_fioFiles.find(fd);
        if (fn == 1 && ssize >= 16) // close
        {
            int32_t result = kFioEBADF;
            if (it != g_fioFiles.end())
            {
                g_fioFiles.erase(it);
                result = 0;
            }
            std::snprintf(b, sizeof b, "close %d -> %d", fd, result);
            note = b;
            completeFio(send, recv, rsize, result);
            return true;
        }
        if (fn == 2 && ssize >= 24) // read
        {
            const uint32_t buf = rd32(send + 16), size = rd32(send + 20);
            int32_t result = kFioEBADF;
            if (it != g_fioFiles.end())
            {
                FioFile &file = it->second;
                const uint32_t n = std::min<uint32_t>(size, static_cast<uint32_t>(file.data.size()) - file.pos);
                for (uint32_t i = 0; i < n; ++i) // byte-wise: buf need not be aligned and may wrap the mask
                    *mem(buf + i) = file.data[file.pos + i];
                file.pos += n;
                result = static_cast<int32_t>(n);
                ++g_fioReads;
                g_fioReadBytes += n;
            }
            std::snprintf(b, sizeof b, "read %d buf=0x%X size=%u -> %d", fd, buf, size, result);
            note = b;
            completeFio(send, recv, rsize, result);
            return true;
        }
        if (fn == 4 && ssize >= 24) // lseek
        {
            const int32_t offset = static_cast<int32_t>(rd32(send + 16));
            const uint32_t whence = rd32(send + 20);
            int32_t result = kFioEBADF;
            if (it != g_fioFiles.end())
            {
                FioFile &file = it->second;
                const int64_t base = whence == 0 ? 0 : whence == 1 ? file.pos : static_cast<int64_t>(file.data.size());
                const int64_t target = base + offset;
                if (whence > 2 || target < 0 || target > static_cast<int64_t>(file.data.size()))
                    result = kFioEINVAL;
                else
                {
                    file.pos = static_cast<uint32_t>(target);
                    result = static_cast<int32_t>(target);
                }
            }
            std::snprintf(b, sizeof b, "lseek %d offset=%d whence=%u -> %d", fd, offset, whence, result);
            note = b;
            completeFio(send, recv, rsize, result);
            return true;
        }
        return false;
    }

    // ---- movie-audio drain (LIBSD, SID 0x80000701), for runs without the emulated sound system ---------------
    // For the FMV path only. This is not a sound implementation: with
    // MAXIMO_AUDIO the calls go to the emulated IOP instead (serveSdRemote below) and the soundtrack plays.
    // The movie player (0x191960.., 0x192130..) streams the PSS PCM into an IOP ring started with
    // sceSdBlockTrans(core 0, WRITE_FROM|LOOP, 0x196040, 0xC000, start) and refills it as far as
    // sceSdBlockTransStatus says SPU2 has read (reply word 0 = bank << 24 | IOP read address; the player
    // keeps the low 24 bits). With no reply the read address never moves, the ring stays full, and playback
    // stops after ~1 s. The drain models only that read cursor: it advances at the SPU2 output rate on the
    // emulated EE clock and discards the samples. OPENING.PSS's SShd header says 48000 Hz, 2 channels,
    // 16-bit PCM, so 192000 bytes/s. Every other LIBSD command stays unhandled. Delete this block when a
    // real SPU2/LIBSD path exists.
    constexpr uint32_t kSdBlockTrans = 0x80E0, kSdBlockTransStatus = 0x8100;
    constexpr uint32_t kSdTransStop = 2, kSdTransWriteFrom = 3, kSdBlockLoop = 0x10;
    constexpr uint64_t kSpu2BytesPerSecond = 48000ull * 2 * 2;

    struct MovieAudioDrain
    {
        bool active = false;
        uint32_t base = 0, size = 0, startOffset = 0;
        uint64_t startCycle = 0;
        uint64_t statusCalls = 0, transfers = 0;
    };
    MovieAudioDrain g_movieAudio[2]; // per SPU2 core
    uint64_t g_movieAudioDrainedBytes = 0;

    uint32_t movieAudioOffset(const MovieAudioDrain &d)
    {
        const uint64_t cycles = g_runtime->eeScheduler().currentCycle() - d.startCycle;
        const uint64_t bytes = cycles * kSpu2BytesPerSecond / EeScheduler::kEeClockHz & ~3ull; // whole L/R frames
        return static_cast<uint32_t>((d.startOffset + bytes) % d.size);
    }

    uint32_t movieAudioStatus(const MovieAudioDrain &d)
    {
        const uint32_t offset = movieAudioOffset(d);
        return (offset >= d.size / 2 ? 1u << 24 : 0u) | ((d.base + offset) & 0xFFFFFFu);
    }

    // The whole reply is the result in word 0, the rest zero (recv may alias send, so write it last).
    void movieAudioReply(uint32_t recv, uint32_t rsize, uint32_t result)
    {
        std::memset(mem(recv), 0, std::min<uint32_t>(rsize, 16));
        wr32(recv, result);
    }

    bool serveMovieAudio(uint32_t fn, uint32_t send, uint32_t ssize, uint32_t recv, uint32_t rsize, std::string &note)
    {
        // Packet built by sceSdRemote (0x1C22B8): {self, core, mode, addr, size, start}; the result is recv word 0.
        if (ssize < 12 || rsize < 4)
            return false;
        const uint32_t core = rd32(send + 4);
        if (core > 1)
            return false;
        MovieAudioDrain &d = g_movieAudio[core];
        char b[160];
        if (fn == kSdBlockTransStatus)
        {
            if (!d.active)
                return false;
            const uint32_t status = movieAudioStatus(d);
            movieAudioReply(recv, rsize, status);
            const uint64_t n = ++d.statusCalls;
            if ((n & (n - 1)) == 0) // the player polls continuously; log powers of two only
            {
                std::snprintf(b, sizeof b, "movie-audio drain status #%llu -> 0x%08X (EE cycle %llu, VBlank %llu)",
                              static_cast<unsigned long long>(n), status,
                              static_cast<unsigned long long>(g_runtime->eeScheduler().currentCycle()),
                              static_cast<unsigned long long>(g_runtime->eeScheduler().currentVSyncTick()));
                note = b;
            }
            return true;
        }
        if (fn != kSdBlockTrans || ssize < 24)
            return false;
        const uint32_t mode = rd32(send + 8);
        if ((mode & 3) == kSdTransStop)
        {
            if (!d.active)
                return false;
            const uint32_t status = movieAudioStatus(d);
            g_movieAudioDrainedBytes += (g_runtime->eeScheduler().currentCycle() - d.startCycle) * kSpu2BytesPerSecond /
                                        EeScheduler::kEeClockHz;
            d.active = false;
            movieAudioReply(recv, rsize, status);
            std::snprintf(b, sizeof b, "movie-audio drain stop core %u -> 0x%08X", core, status);
            note = b;
            return true;
        }
        const uint32_t addr = rd32(send + 12), size = rd32(send + 16), start = rd32(send + 20);
        // Only the looping write the movie player uses; one-shot and read transfers remain unhandled.
        if ((mode & 3) != kSdTransWriteFrom && (mode & 3) != 0)
            return false;
        if (!(mode & kSdBlockLoop) || size == 0 || (size & 3) != 0)
            return false;
        d.active = true;
        d.base = addr;
        d.size = size;
        d.startOffset = (mode & 3) == kSdTransWriteFrom && start - addr < size ? (start - addr) & ~3u : 0;
        d.startCycle = g_runtime->eeScheduler().currentCycle();
        ++d.transfers;
        movieAudioReply(recv, rsize, 0);
        std::snprintf(b, sizeof b, "movie-audio drain start core %u ring 0x%X+0x%X from +0x%X at %llu B/s (silent)", core,
                      addr, size, d.startOffset, static_cast<unsigned long long>(kSpu2BytesPerSecond));
        note = b;
        return true;
    }

    // With the emulated sound system (MAXIMO_AUDIO), the EE's LIBSD calls run on the emulated IOP's libsd and SPU2,
    // as LIBSD's RPC server runs them on a PS2. The movie player initialises the SPU2, sets the master and input
    // volumes and streams the soundtrack through core 0's sound data input from its IOP ring; the ring's read
    // position (sceSdBlockTransStatus) follows the SPU2 as it mixes, on the audio clock. Packet built by
    // sceSdRemote (0x1C22B8): {self, arg0..arg4}; the result is recv word 0.
    std::map<uint32_t, uint64_t> g_sdRemoteCalls;

    bool serveSdRemote(uint32_t fn, uint32_t send, uint32_t ssize, uint32_t recv, uint32_t rsize, std::string &note)
    {
        if (ssize < 8 || rsize < 4)
            return false;
        std::array<uint32_t, 5> args{};
        for (uint32_t i = 0; i < args.size() && 8 + i * 4 <= ssize; ++i)
            args[i] = rd32(send + 4 + i * 4);
        uint32_t result = 0;
        if (!g_runtime->iopSdRemote(fn, args, result))
            return false;
        movieAudioReply(recv, rsize, result);
        const uint64_t n = ++g_sdRemoteCalls[fn];
        // The player polls the status continuously, and a skipped movie fades MVOL with a busy loop of SetParam calls
        // (~130,000 a second): past the first 16 calls of a function, log only power-of-two call numbers.
        if (n <= 16 || (n & (n - 1)) == 0)
        {
            char b[200];
            std::snprintf(b, sizeof b, "libsd 0x%X(0x%X, 0x%X, 0x%X, 0x%X, 0x%X) -> 0x%X [call #%llu] (VBlank %llu)", fn,
                          args[0], args[1], args[2], args[3], args[4], result, static_cast<unsigned long long>(n),
                          static_cast<unsigned long long>(g_runtime->eeScheduler().currentVSyncTick()));
            note = b;
        }
        return true;
    }

    // Native IOP services. Returns false if the call is not understood (reply left zeroed).
    bool serveRpc(uint32_t sid, uint32_t fn, uint32_t send, uint32_t ssize, uint32_t recv, uint32_t rsize, std::string &note)
    {
        // The IOP heap services allocate from the emulated IOP's own heap, which its modules (MJOLNIR.IRX) share,
        // so blocks never overlap and freed ones are used again. 0 means the heap is full, as on the IOP.
        if (sid == 0x80000003 && fn == 1 && ssize >= 4 && rsize >= 4) // sceSifAllocIopHeap(size)
        {
            const uint32_t addr = g_runtime->allocateIopMemory(rd32(send), 64u);
            wr32(recv, addr);
            char b[80];
            std::snprintf(b, sizeof b, addr ? "alloc %u bytes at IOP 0x%X" : "alloc %u bytes FAILED (IOP heap full)",
                          rd32(send), addr);
            note = b;
            return true;
        }
        if (sid == 0x80000003 && fn == 2 && ssize >= 4 && rsize >= 4) // sceSifFreeIopHeap(addr)
        {
            const bool freed = g_runtime->freeIopMemory(rd32(send));
            wr32(recv, freed ? 0u : 0xFFFFFFFFu);
            note = freed ? "free" : "free FAILED (not an allocation)";
            return true;
        }
        // FILEIO init (sceFsInit): the IOP module answers with its 4-character version, which the EE library
        // compares with the versions it supports ("2200" at 0x1E6E1C, matching the disc's IOPRP22.IMG).
        if (sid == 0x80000001 && fn == 255 && rsize >= 4)
        {
            std::memcpy(mem(recv), "2200", 4);
            char b[64];
            std::snprintf(b, sizeof b, "version \"2200\", result buffer 0x%X", ssize >= 4 ? rd32(send) : 0);
            note = b;
            return true;
        }
        if (sid == 0x80000001)
            return serveFileIo(fn, send, ssize, recv, rsize, note);
        if (sid == 0x80000701)
            return g_iopAudio ? serveSdRemote(fn, send, ssize, recv, rsize, note)
                              : serveMovieAudio(fn, send, ssize, recv, rsize, note);
        return false;
    }

    // Sound driver (SDRDRV/MJOLNIR, SID 0x12345) command packets, built by 0x18D220: halfword 0 is the
    // command count, then per command {id, nargs, args...}. Decoded for the log only; the driver
    // itself is not emulated, so no reply is fabricated (see BOOT_FINDINGS.md).
    struct SoundCmdStat
    {
        uint64_t count = 0;
        uint32_t nargs = 0;
    };
    std::map<uint32_t, SoundCmdStat> g_soundCmds;
    uint64_t g_soundPackets = 0, g_soundMalformed = 0;

    uint16_t rd16(uint32_t addr)
    {
        uint16_t v;
        std::memcpy(&v, mem(addr), 2);
        return v;
    }

    void decodeSoundPacket(uint32_t send, uint32_t size)
    {
        ++g_soundPackets;
        const uint32_t halfwords = size / 2;
        const uint32_t count = rd16(send);
        uint32_t i = 1;
        for (uint32_t c = 0; c < count; ++c)
        {
            if (i + 2 > halfwords)
            {
                ++g_soundMalformed;
                return;
            }
            const uint32_t id = rd16(send + i * 2), n = rd16(send + i * 2 + 2);
            if (i + 2 + n > halfwords)
            {
                ++g_soundMalformed;
                return;
            }
            SoundCmdStat &st = g_soundCmds[id];
            // MAXIMO_SOUND_TRACE=1: every command except the per-frame 2 and 9 (MAXIMO_SOUND_TRACE=2: all).
            static const int soundTrace = []
            {
                const char *value = std::getenv("MAXIMO_SOUND_TRACE");
                return value ? std::atoi(value) : 0;
            }();
            // Music start (1), pause/resume (10/11) and volume (31) are rare and always logged, for bug reports.
            const bool always = id == 1u || id == 10u || id == 11u || id == 31u;
            if (st.count++ < 3 || always || soundTrace >= 2 || (soundTrace == 1 && id != 2u && id != 9u))
            {
                char args[160] = {};
                int len = 0;
                for (uint32_t a = 0; a < n && a < 12; ++a)
                    len += std::snprintf(args + len, sizeof(args) - len, " %04X", rd16(send + (i + 2 + a) * 2));
                logf("SOUND cmd %u nargs=%u args:%s (packet #%llu)", id, n, args, static_cast<unsigned long long>(g_soundPackets));
            }
            st.nargs = n;
            i += 2 + n;
        }
    }

    void stub_sceSifCallRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        const uint32_t cd = reg(ctx, 4), fn = reg(ctx, 5), mode = reg(ctx, 6), send = reg(ctx, 7);
        const uint32_t ssize = reg(ctx, 8), recv = reg(ctx, 9), rsize = reg(ctx, 10), endFn = reg(ctx, 11);
        const uint32_t endParam = rd32(reg(ctx, 29));
        const uint32_t server = rd32(cd + kCdServer);
        const uint32_t index = (server - kFakeServerBase) / 0x40u;
        const bool bound = server >= kFakeServerBase && index < g_servers.size();
        const uint32_t sid = bound ? g_servers[index].sid : 0;
        if (bound)
            ++g_servers[index].calls;

        // The sound driver runs in the emulated IOP: its own RPC server takes the packet and writes the reply.
        if (g_iopAudio && sid == kSoundDriverSid && g_runtime->canBindIopRpc(sid))
        {
            if (fn == 0)
                decodeSoundPacket(send, ssize);
            ps2x::iop::RpcRequest request{};
            request.clientAddress = cd;
            request.sid = sid;
            request.function = fn;
            request.mode = mode;
            request.send = {send, ssize};
            request.receive = {recv, rsize};
            request.endFunction = endFn;
            request.endParameter = endParam;
            const ps2x::iop::RpcResult result = g_runtime->handleIopRpc(rdram, ctx, request);
            const uint64_t n = ++g_soundRpcCalls;
            if (n <= 16 || (n & (n - 1)) == 0)
                logf("IOP sceSifCallRpc(sid=0x%08X MJOLNIR, fn=%u, mode=%u, send=0x%X/%u, recv=0x%X/%u, end=0x%X) -> "
                     "served by the emulated IOP (handled=%d) call #%llu [ra=0x%06X]",
                     sid, fn, mode, send, ssize, recv, rsize, endFn, result.handled ? 1 : 0,
                     static_cast<unsigned long long>(n), reg(ctx, 31));
            ret(ctx, 0);
            // The reply is in place, so the completion callback runs now, as the SIF interrupt would run it when the
            // reply DMA ends. The game unpacks each voice's state there (0x18D2E0); without it, it only unpacks the
            // previous reply at the start of its next frame. A sound it has just keyed on then reads as off twice,
            // one more than it tolerates, so it frees the effect slot without keying the voice off: looping
            // effects (the attract demo's bone, effect 0x75) pile up on the voices and the channels run out.
            if (result.handled && endFn != 0 && g_runtime->hasFunction(endFn))
            {
                GuestInvocation callback{};
                callback.kind = GuestInvocationKind::RpcCallback;
                callback.context = *ctx;
                callback.context.pc = endFn;
                SET_GPR_U32(&callback.context, 4, endParam);
                SET_GPR_U32(&callback.context, 29, g_runtime->eeScheduler().invocationStackTop());
                SET_GPR_U32(&callback.context, 31, 0u);
                callback.onComplete = [](const R5900Context &, R5900Context &) {};
                g_runtime->eeScheduler().invokeCurrent(std::move(callback));
            }
            return;
        }
        if (g_iopAudio && sid == kSoundDriverSid)
            logf("IOP sceSifCallRpc(sid=0x%08X): the emulated sound driver has not registered its server yet", sid);

        // The first send words, for the log; formatted only for calls that are logged (pollers make millions).
        uint32_t sent[6] = {};
        const uint32_t sentWords = std::min<uint32_t>(ssize / 4, 6);
        for (uint32_t i = 0; i < sentWords; ++i)
            sent[i] = rd32(send + i * 4);

        if (sid == 0x00012345u && fn == 0)
            decodeSoundPacket(send, ssize);
        std::string note;
        // Unhandled calls get a zeroed reply. LIBSD's sceSdRemote receives into its own send buffer, so the
        // packet must be served before anything is cleared; zero up front only when the buffers are disjoint.
        const bool zeroable = recv != 0 && rsize != 0 && rsize < 0x100000u;
        const bool overlap = recv < send + ssize && send < recv + rsize;
        if (zeroable && !overlap)
            std::memset(mem(recv), 0, rsize);
        const bool handled = bound && serveRpc(sid, fn, send, ssize, recv, rsize, note);
        if (zeroable && overlap && !handled)
            std::memset(mem(recv), 0, rsize);
        if (!handled)
            ++g_unhandledRpc[(static_cast<uint64_t>(sid) << 16) | (fn & 0xFFFF)];
        const uint64_t n = bound ? g_servers[index].calls : 0;
        // Handled calls log when they leave a note (high-rate pollers such as the movie-audio drain stay quiet).
        const bool logCall = (handled && !note.empty()) || n <= 16 || (n & (n - 1)) == 0;
        if (logCall)
        {
            char words[96] = {};
            int len = 0;
            for (uint32_t i = 0; i < sentWords; ++i)
                len += std::snprintf(words + len, sizeof(words) - len, " %08X", sent[i]);
            logf("IOP sceSifCallRpc(sid=0x%08X %s, fn=%u, mode=%u, send=0x%X/%u:%s, recv=0x%X/%u, end=0x%X(0x%X)) -> 0 "
                 "%s%s [ra=0x%06X]",
                 sid, bound ? sidName(sid) : "UNBOUND", fn, mode, send, ssize, words, recv, rsize, endFn, endParam,
                 handled ? "served: " : "UNHANDLED (reply zeroed)", note.c_str(), reg(ctx, 31));
        }
        if (endFn != 0 && logCall)
            logf("IOP   note: completion callback 0x%X not invoked (no reply to deliver)", endFn);
        ret(ctx, 0);
    }

    void stub_sceSifCheckStatRpc(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        ret(ctx, 0); // every stubbed RPC completes synchronously
    }

    // ---- libc ----------------------------------------------------------------------------------

    // Minimal printf for the EE ABI: integer varargs in a1..a3, t0..t3, then the caller's stack.
    std::string formatGuest(R5900Context *ctx, const std::string &fmt, int firstArg)
    {
        static const int argRegs[] = {4, 5, 6, 7, 8, 9, 10, 11};
        int argi = firstArg;
        auto next32 = [&]() -> uint32_t
        {
            const int i = argi++;
            return i < 8 ? reg(ctx, argRegs[i]) : rd32(reg(ctx, 29) + (i - 8) * 8);
        };
        std::string out;
        for (size_t i = 0; i < fmt.size(); ++i)
        {
            if (fmt[i] != '%')
            {
                out += fmt[i];
                continue;
            }
            std::string spec = "%";
            ++i;
            while (i < fmt.size() && std::strchr("-+ #0123456789.lhzjt", fmt[i]))
                spec += fmt[i++];
            if (i >= fmt.size())
                break;
            const char c = fmt[i];
            spec.erase(std::remove_if(spec.begin() + 1, spec.end(), [](char ch) { return std::strchr("lhzjt", ch); }),
                       spec.end());
            char buf[512];
            switch (c)
            {
            case 'd': case 'i':
                std::snprintf(buf, sizeof buf, (spec + "d").c_str(), static_cast<int32_t>(next32()));
                out += buf;
                break;
            case 'u': case 'x': case 'X': case 'o': case 'c':
                std::snprintf(buf, sizeof buf, (spec + c).c_str(), next32());
                out += buf;
                break;
            case 'p':
                std::snprintf(buf, sizeof buf, "0x%08X", next32());
                out += buf;
                break;
            case 's':
                std::snprintf(buf, sizeof buf, (spec + "s").c_str(), cstr(next32()).c_str());
                out += buf;
                break;
            case 'f': case 'e': case 'g':
                out += "<float>";
                next32();
                break;
            case '%':
                out += '%';
                break;
            default:
                out += spec + c;
            }
        }
        return out;
    }

    void stub_printf(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        const std::string s = formatGuest(ctx, cstr(reg(ctx, 4), 1024), 1);
        logf("GAME printf: %s  [ra=0x%06X]", printable(s).c_str(), reg(ctx, 31));
        ret(ctx, static_cast<int32_t>(s.size()));
    }

    void stub_assert(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        logf("GAME ASSERTION FAILED: \"%s\" file \"%s\" line %d [ra=0x%06X] -- stopping",
             cstr(reg(ctx, 6)).c_str(), cstr(reg(ctx, 4)).c_str(), GPR_S32(ctx, 5), reg(ctx, 31));
        runtime->requestStop();
        ctx->pc = 0;
    }

    // ---- crash reporting -----------------------------------------------------------------------

    void dumpRing(const char *why)
    {
        logf("---- %s: last %zu guest function entries (oldest first) ----", why, g_ring.size());
        std::string line;
        for (size_t i = 0; i < g_ring.size(); ++i)
        {
            const uint32_t pc = g_ring[(g_ringPos + i) % g_ring.size()];
            if (pc == 0)
                continue;
            char b[16];
            std::snprintf(b, sizeof b, " %06X", pc);
            line += b;
            if (line.size() > 100)
            {
                logf("  %s", line.c_str());
                line.clear();
            }
        }
        if (!line.empty())
            logf("  %s", line.c_str());
        logf("  last game function entered: 0x%06X", g_lastGameFn);
        for (const auto &[addr, w] : g_watches)
            if (w.calls != 0)
                logf("  WATCH total 0x%06X %-60.60s %llu calls", addr, w.what,
                     static_cast<unsigned long long>(w.calls));
        logf("  frame waits ended by the 2-VBlank timeout path: %llu",
             static_cast<unsigned long long>(g_frameTimeouts));
        if (g_perfTotal.frames != 0)
            logFrameStats("whole run", g_perfTotal,
                          std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count());
        // MAXIMO_DUMP_CLUT=cbp0,cbp1,i0,i1: log CT32 CSM1 palette entries i0 and i1 for every CLUT in [cbp0, cbp1).
        if (const char *dump = std::getenv("MAXIMO_DUMP_CLUT"))
        {
            unsigned cbp0 = 0, cbp1 = 0, i0 = 0, i1 = 0;
            if (std::sscanf(dump, "%x,%x,%x,%x", &cbp0, &cbp1, &i0, &i1) == 4)
                for (unsigned cbp = cbp0; cbp < cbp1; cbp += 4)
                {
                    auto entry = [&](unsigned i)
                    {
                        const unsigned p = (i & ~0x18u) | ((i & 0x08u) << 1) | ((i & 0x10u) >> 1);
                        return g_runtime->gs().ReadVram(0u, cbp, 1u, p & 15u, p >> 4);
                    };
                    logf("  CLUT cbp=0x%X [0x%02X]=0x%08X [0x%02X]=0x%08X", cbp, i0, entry(i0), i1, entry(i1));
                }
        }
        // MAXIMO_DUMP_T8=tbp,tbw,width,height: write GS memory read as PSMT8 indices to gs_t8.pgm.
        if (const char *dump = std::getenv("MAXIMO_DUMP_T8"))
        {
            unsigned tbp = 0, tbw = 0, width = 0, height = 0;
            if (std::sscanf(dump, "%x,%u,%u,%u", &tbp, &tbw, &width, &height) == 4 && width && height)
                if (FILE *f = std::fopen("gs_t8.pgm", "wb"))
                {
                    std::fprintf(f, "P5\n%u %u\n255\n", width, height);
                    std::vector<uint8_t> row(width);
                    for (unsigned y = 0; y < height; ++y)
                    {
                        for (unsigned x = 0; x < width; ++x)
                            row[x] = static_cast<uint8_t>(g_runtime->gs().ReadVram(0x13u, tbp, tbw, x, y));
                        std::fwrite(row.data(), 1, row.size(), f);
                    }
                    std::fclose(f);
                    logf("  wrote gs_t8.pgm: PSMT8 tbp=0x%X tbw=%u %ux%u", tbp, tbw, width, height);
                }
        }
        const GSRegisters &gsr = g_runtime->memory().gs();
        logf("  GS display: PMODE=0x%llX SMODE2=0x%llX DISPFB1=0x%llX DISPLAY1=0x%llX DISPFB2=0x%llX "
             "DISPLAY2=0x%llX BGCOLOR=0x%llX",
             static_cast<unsigned long long>(gsr.pmode), static_cast<unsigned long long>(gsr.smode2),
             static_cast<unsigned long long>(gsr.dispfb1), static_cast<unsigned long long>(gsr.display1),
             static_cast<unsigned long long>(gsr.dispfb2), static_cast<unsigned long long>(gsr.display2),
             static_cast<unsigned long long>(gsr.bgcolor));
    }

    LONG WINAPI crashFilter(EXCEPTION_POINTERS *info)
    {
        const R5900Context &c = g_runtime->cpu();
        logf("HOST CRASH: exception 0x%08lX at host address %p (guest pc register 0x%06X, ra 0x%06X)",
             info->ExceptionRecord->ExceptionCode, info->ExceptionRecord->ExceptionAddress, c.pc,
             static_cast<uint32_t>(_mm_cvtsi128_si32(c.r[31])));
        if (info->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION)
            logf("  access violation: %s host address 0x%llX", info->ExceptionRecord->ExceptionInformation[0] ? "write" : "read",
                 static_cast<unsigned long long>(info->ExceptionRecord->ExceptionInformation[1]));
        dumpRing("crash");
        return EXCEPTION_EXECUTE_HANDLER;
    }

    bool isMilestone(uint32_t addr)
    {
        return std::any_of(g_milestones.begin(), g_milestones.end(), [&](const Milestone &m) { return m.addr == addr; });
    }

    // A milestone keeps its wrapper in the table; the replacement goes behind it.
    void bindStub(PS2Runtime &rt, uint32_t addr, Fn fn)
    {
        if (isMilestone(addr))
            g_original[slotOf(addr)] = fn;
        else
            rt.replaceFunction(addr, fn);
    }

    void bindRuntime(PS2Runtime &rt, uint32_t addr, const char *name, Fn impl, int nargs)
    {
        g_bound[addr] = {name, impl, nargs};
        bindStub(rt, addr, boundEntry);
    }
}

// ---- sampling profiler (MAXIMO_PROFILE=<start seconds>[,<end seconds>] or f<first frame>,<last frame>) ----
// Suspends the game thread about once a millisecond, records its instruction pointer and up to 12 callers
// (x64 unwind tables), and names them with the PDB at exit: exclusive (self) and inclusive counts per function.
namespace
{
    constexpr int kProfileFrames = 12;
    std::mutex g_profileMutex;
    std::vector<std::array<DWORD64, kProfileFrames>> g_profileSamples;
    std::vector<bool> g_profileInWait; // sample taken between sceGsSyncV entry and the VBlank that releases it
    std::atomic<bool> g_profileStop{false};

    int unwindStack(CONTEXT context, std::array<DWORD64, kProfileFrames> &frames)
    {
        int n = 0;
        while (n < kProfileFrames && context.Rip != 0)
        {
            frames[n++] = context.Rip;
            DWORD64 imageBase = 0;
            PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
            if (!function)
            {
                // Leaf function without unwind data: the return address is at [rsp].
                context.Rip = *reinterpret_cast<DWORD64 *>(context.Rsp);
                context.Rsp += 8;
                continue;
            }
            PVOID handlerData = nullptr;
            DWORD64 establisherFrame = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, function, &context, &handlerData,
                             &establisherFrame, nullptr);
        }
        return n;
    }

    void profileThread(double startSeconds, double endSeconds, bool guestFrames)
    {
        while (!g_profileStop.load())
        {
            const double t = guestFrames ? static_cast<double>(g_guestFrame.load(std::memory_order_relaxed))
                                         : std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
            HANDLE thread = g_gameThread.load();
            if (thread && t >= startSeconds && t < endSeconds && SuspendThread(thread) != static_cast<DWORD>(-1))
            {
                CONTEXT context{};
                context.ContextFlags = CONTEXT_FULL;
                std::array<DWORD64, kProfileFrames> frames{};
                if (GetThreadContext(thread, &context))
                    unwindStack(context, frames); // the stack is frozen while suspended
                ResumeThread(thread);
                const bool inWait = ps2_host_profile::g_waitMarkNs.load(std::memory_order_acquire) >
                                    ps2_host_profile::g_waitReleaseNs.load(std::memory_order_acquire);
                std::lock_guard<std::mutex> lock(g_profileMutex);
                g_profileSamples.push_back(frames);
                g_profileInWait.push_back(inWait);
            }
            Sleep(1);
        }
    }

    void reportProfile()
    {
        std::lock_guard<std::mutex> lock(g_profileMutex);
        if (g_profileSamples.empty())
            return;
        HANDLE process = GetCurrentProcess();
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        SymInitialize(process, nullptr, TRUE);
        std::map<DWORD64, std::string> names;
        auto nameOf = [&](DWORD64 address) -> const std::string &
        {
            auto it = names.find(address);
            if (it != names.end())
                return it->second;
            alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512] = {};
            auto *symbol = reinterpret_cast<SYMBOL_INFO *>(buffer);
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = 511;
            DWORD64 displacement = 0;
            char fallback[32];
            std::snprintf(fallback, sizeof fallback, "0x%llX", static_cast<unsigned long long>(address));
            return names[address] = SymFromAddr(process, address, &displacement, symbol) ? symbol->Name : fallback;
        };
        std::map<std::string, uint32_t> self, inclusive;
        for (const auto &frames : g_profileSamples)
        {
            if (!frames[0])
                continue;
            ++self[nameOf(frames[0])];
            std::set<std::string> seen;
            for (DWORD64 address : frames)
                if (address && seen.insert(nameOf(address)).second)
                    ++inclusive[nameOf(address)];
        }
        auto print = [&](const char *title, const std::map<std::string, uint32_t> &counts)
        {
            std::vector<std::pair<uint32_t, std::string>> sorted;
            for (const auto &[name, count] : counts)
                sorted.push_back({count, name});
            std::sort(sorted.rbegin(), sorted.rend());
            logf("---- profile: %s (%zu samples) ----", title, g_profileSamples.size());
            for (size_t i = 0; i < sorted.size() && i < 45; ++i)
                logf("  %5.1f%%  %6u  %.400s", 100.0 * sorted[i].first / g_profileSamples.size(), sorted[i].first,
                     sorted[i].second.c_str());
        };
        print("self", self);
        print("inclusive", inclusive);
        // MAXIMO_PROFILE_CALLERS=a,b,...: for samples whose innermost function contains one of these names, the
        // chain of the next callers outside it (MAXIMO_PROFILE_CALLER_DEPTH of them, default 3).
        if (const char *patterns = std::getenv("MAXIMO_PROFILE_CALLERS"))
        {
            const char *depthText = std::getenv("MAXIMO_PROFILE_CALLER_DEPTH");
            const int maxDepth = depthText ? std::clamp(std::atoi(depthText), 1, kProfileFrames - 1) : 3;
            std::string list = patterns;
            for (size_t start = 0; start < list.size();)
            {
                size_t end = list.find(',', start);
                if (end == std::string::npos)
                    end = list.size();
                const std::string pattern = list.substr(start, end - start);
                start = end + 1;
                std::map<std::string, uint32_t> callers;
                for (const auto &frames : g_profileSamples)
                {
                    if (!frames[0] || nameOf(frames[0]).find(pattern) == std::string::npos)
                        continue;
                    std::string chain;
                    int depth = 0;
                    for (int i = 1; i < kProfileFrames && frames[i] && depth < maxDepth; ++i)
                    {
                        const std::string &name = nameOf(frames[i]);
                        if (depth == 0 && name.find(pattern) != std::string::npos)
                            continue;
                        chain += (depth ? " < " : "") + name.substr(0, 48);
                        ++depth;
                    }
                    ++callers[chain];
                }
                print(("callers of " + pattern).c_str(), callers);
            }
        }
        // MAXIMO_PROFILE_LINES=1: self samples by source line (generated VU1 code is one function per image).
        if (std::getenv("MAXIMO_PROFILE_LINES"))
        {
            std::map<std::string, uint32_t> lines;
            for (const auto &frames : g_profileSamples)
            {
                if (!frames[0])
                    continue;
                IMAGEHLP_LINE64 line{};
                line.SizeOfStruct = sizeof line;
                DWORD displacement = 0;
                char key[300];
                if (SymGetLineFromAddr64(process, frames[0], &displacement, &line))
                {
                    const char *file = std::strrchr(line.FileName, '\\');
                    std::snprintf(key, sizeof key, "%s:%lu", file ? file + 1 : line.FileName, line.LineNumber);
                }
                else
                    std::snprintf(key, sizeof key, "%s", nameOf(frames[0]).c_str());
                ++lines[key];
            }
            print("self by line", lines);
        }
        {
            std::map<std::string, uint32_t> waiting;
            size_t waitSamples = 0;
            for (size_t i = 0; i < g_profileSamples.size(); ++i)
                if (g_profileInWait[i] && g_profileSamples[i][0])
                {
                    ++waitSamples;
                    std::set<std::string> seen;
                    for (DWORD64 address : g_profileSamples[i])
                        if (address && seen.insert(nameOf(address)).second)
                            ++waiting[nameOf(address)];
                }
            logf("---- profile: %zu of the samples were in a VBlank wait ----", waitSamples);
            print("inclusive, VBlank wait only", waiting);
        }
        SymCleanup(process);
    }
}

void registerMaximoVu1Aot(); // vu1aot/vu1aot_registry.cpp

int main(int argc, char **argv)
{
    // Launcher tool mode: convert an HD texture pack (hdtex/hdtex_convert.cpp); the game does not start.
    if (argc > 1 && std::strcmp(argv[1], "--hdtex-convert") == 0)
        return hdtex::convertMain(argc - 1, argv + 1);
    // Compiled VU1 microprograms; PS2X_VU1_AOT=0 runs the interpreter only.
    registerMaximoVu1Aot();
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: maximo_boot <disc folder> [seconds=20] [log=boot_log.txt]\n");
        return 2;
    }
    g_disc = fs::absolute(argv[1]);
    const double seconds = argc > 2 ? std::atof(argv[2]) : 20.0;
    const std::string logPath = argc > 3 ? argv[3] : "boot_log.txt";
    const std::string padScript = argc > 4 ? argv[4] : "";
    if (!parsePadScript(padScript))
    {
        std::fprintf(stderr, "bad pad script \"%s\" (expected start:BUTTON[+BUTTON]:length,...)\n", padScript.c_str());
        return 2;
    }
    g_log = std::fopen(logPath.c_str(), "w");
    if (!g_log)
        g_log = stdout;
    SetUnhandledExceptionFilter(crashFilter);
    if (const char *replay = std::getenv("MAXIMO_VU1_REPLAY"))
        return vu1cap::replay(replay, logf);
    if (const char *vu1Thread = std::getenv("PS2X_VU1_THREAD"); vu1Thread && vu1Thread[0] == '1')
    {
        // VU1 capture and the GPU shadow diagnostic hook VU1 and the GS from the game thread.
        if (std::getenv("MAXIMO_VU1_CAPTURE") || std::getenv("MAXIMO_GPU_SHADOW_FRAME"))
        {
            _putenv("PS2X_VU1_THREAD=0");
            logf("PS2X_VU1_THREAD=1 ignored: not supported with MAXIMO_VU1_CAPTURE or MAXIMO_GPU_SHADOW_FRAME");
        }
        else
            logf("PS2X_VU1_THREAD=1: VU1 and the GS back end run on a worker thread (experimental)");
    }

    const fs::path elf = g_disc / "SLES_507.03";
    logf("maximo_boot: disc=%s elf=%s limit=%.1fs", g_disc.string().c_str(), elf.string().c_str(), seconds);
    for (const PadStep &s : g_padScript)
        logf("pad script: %s", s.text.c_str());

    static PS2Runtime runtime;
    g_runtime = &runtime;
    // MAXIMO_WINDOW=1 runs the runtime's windowed path (CPU GS backend presented through raylib);
    // the default stays headless.
    const bool windowed = std::getenv("MAXIMO_WINDOW") != nullptr;
    runtime.setHeadless(!windowed, seconds);
    logf("presentation: %s", windowed ? "window (CPU GS backend)" : "headless");
    runtime.setMissingFunctionPolicy(PS2Runtime::MissingFunctionPolicy::Stop);
    // Memory cards live in the run directory, never next to the extracted disc files. MAXIMO_MC_ROOT=<dir> uses a
    // card that persists across runs instead (play_with_audio.cmd keeps one in boot\memcard\mc0).
    const char *mcRoot = std::getenv("MAXIMO_MC_ROOT");
    PS2Runtime::setMemoryCardRoot(mcRoot && *mcRoot ? fs::path(mcRoot) : fs::current_path() / "mc0");
    // MAXIMO_ASPECT=16:9: widescreen (WIDESCREEN.md). Set before initialize, which opens a 16:9 window for it.
    const char *aspect = std::getenv("MAXIMO_ASPECT");
    const bool widescreen = aspect && std::strcmp(aspect, "16:9") == 0;
    if (widescreen)
        ps2_display_aspect::enableWide();
    if (!runtime.initialize("Maximo boot test") || !runtime.loadELF(elf.string()))
    {
        logf("runtime initialisation or ELF load failed");
        return 1;
    }
#ifdef MAXIMO_ENABLE_GPU_SHADOW
    if (const char *frameText = std::getenv("MAXIMO_GPU_SHADOW_FRAME"))
    {
        if (!windowed || std::getenv("PS2X_GS_THREAD"))
        {
            logf("GPU shadow capture requires MAXIMO_WINDOW=1 and inline CPU GS");
            return 2;
        }
        for (const char *p = frameText; *p;)
        {
            char *end = nullptr;
            const uint64_t frame = std::strtoull(p, &end, 10);
            if (end == p || frame == 0)
            {
                logf("MAXIMO_GPU_SHADOW_FRAME must be a comma-separated list of positive guest frame numbers");
                return 2;
            }
            g_gpuShadowFrames.push_back(frame);
            p = *end == ',' ? end + 1 : end;
        }
        std::sort(g_gpuShadowFrames.begin(), g_gpuShadowFrames.end());
        g_gpuShadow = std::make_unique<GpuShadow>(runtime);
        runtime.gifArbiter().setPathPacketFn([](GifPathId path, const uint8_t *data, uint32_t size, bool syntheticImageTag)
        {
            // Diagnostic shadow: CPU remains the authoritative GS, including all fallback semantics. The shadow goes
            // first so its lazy VRAM snapshot is taken before the CPU applies the packet that triggers it.
            // paraLLEl-GS keeps GIF state across packets like the real GIF, so it must not see host-added tags.
            const uint32_t skip = syntheticImageTag ? 16u : 0u;
            if (size > skip)
                g_gpuShadow->submitPacket(path, data + skip, size - skip);
            g_runtime->gs().processGIFPacket(data, size);
        });
        logf("[gpu-shadow] opt-in capture at guest frames %s", frameText);
    }
    else if (const char *backend = std::getenv("MAXIMO_GS_BACKEND"); backend && std::strcmp(backend, "gpu") == 0)
    {
        if (!windowed || std::getenv("PS2X_GS_THREAD"))
        {
            logf("MAXIMO_GS_BACKEND=gpu requires MAXIMO_WINDOW=1 and no PS2X_GS_THREAD");
            return 2;
        }
        // PS2_GS_CAPTURE_FRAMES is honoured with GPU frames (gpu_sync_NNNN_WxH.rgba); the run does not stop early.
        if (const char *frames = std::getenv("PS2_GS_CAPTURE_FRAMES"))
            for (const char *p = frames; *p;)
            {
                char *end = nullptr;
                const uint64_t frame = std::strtoull(p, &end, 10);
                if (end == p)
                    break;
                g_gpuShadowFrames.push_back(frame);
                p = *end == ',' ? end + 1 : end;
            }
        g_gpuMode = true;
        g_gpuShadow = std::make_unique<GpuShadow>(runtime);
        g_gpuShadow->enablePresentation();
        runtime.gs().setRasterBackend(std::make_unique<GSDiscardBackend>());
        runtime.gifArbiter().setPathPacketFn([](GifPathId path, const uint8_t *data, uint32_t size, bool syntheticImageTag)
        {
            const uint32_t skip = syntheticImageTag ? 16u : 0u;
            if (size > skip)
                g_gpuShadow->submitPacket(path, data + skip, size - skip);
            // Register state, SIGNAL/FINISH/LABEL and CSR stay with the CPU frontend; its backend discards drawing.
            g_runtime->gs().processGIFPacket(data, size);
        });
        runtime.setHostFrameSource([](std::vector<uint8_t> &pixels, uint32_t &width, uint32_t &height)
                                   { return g_gpuShadow->takeFrame(pixels, width, height); });
        logf("presentation: paraLLEl-GS (MAXIMO_GS_BACKEND=gpu); CPU GS rasterisation disabled");
    }
#else
    if (std::getenv("MAXIMO_GPU_SHADOW_FRAME") || std::getenv("MAXIMO_GS_BACKEND"))
    {
        logf("GPU shadow/backend requested, but this build has MAXIMO_ENABLE_GPU_SHADOW=OFF");
        return 2;
    }
#endif
    if (const char *capture = std::getenv("MAXIMO_VU1_CAPTURE"))
        vu1cap::installCapture(runtime, capture, logf);
    g_rdram = runtime.memory().getRDRAM();
    if (const char *audio = std::getenv("MAXIMO_AUDIO"); audio && std::strcmp(audio, "0") != 0)
    {
        g_iopAudio = true;
        if (const char *wav = std::getenv("MAXIMO_AUDIO_WAV"))
        {
            g_audio.wav = std::fopen(wav, "wb");
            if (g_audio.wav)
            {
                writeWavHeader(g_audio.wav, 0);
                g_wavWriter.start(g_audio.wav);
            }
            logf("audio: capturing the SPU2 output to %s%s", wav, g_audio.wav ? "" : " (open FAILED)");
        }
        runtime.setIopAudioSink(onIopAudio);
        runtime.setIopAudioPlayback(true);
        // MAXIMO_AUDIO_CLOCK=lockstep|realtime: what times the IOP while its audio plays (see PS2Runtime::IopAudioClock).
        const char *clock = std::getenv("MAXIMO_AUDIO_CLOCK");
        const bool realTime = !(clock && std::strcmp(clock, "lockstep") == 0);
        runtime.setIopAudioClock(realTime ? PS2Runtime::IopAudioClock::RealTime : PS2Runtime::IopAudioClock::Lockstep);
        logf("audio: IOP clock %s", realTime ? "realtime (the audio device's)" : "lockstep (EE time / 8)");
        logf("audio: MJOLNIR.IRX runs in the emulated IOP over native libsd/SPU2 (MAXIMO_AUDIO)");
    }
    // crt0 puts the main thread stack at 0x1F4B000-0x2000000, where the runtime would otherwise carve
    // interrupt/callback stacks. Move them below the ELF image (the real kernel uses its own memory).
    constexpr uint32_t kCbStackFloor = 0x000F0000u, kCbStackTop = 0x00100000u;
    runtime.setAsyncCallbackStackRegion(kCbStackFloor, kCbStackTop);
    logf("interrupt/callback stacks: 0x%06X-0x%06X", kCbStackFloor, kCbStackTop);
    const PS2Runtime::IoPaths &io = PS2Runtime::getIoPaths();
    logf("io paths: cdRoot=%s hostRoot=%s mcRoot=%s", io.cdRoot.string().c_str(), io.hostRoot.string().c_str(),
         io.mcRoot.string().c_str());
    logf("entry pc=0x%06X", runtime.cpu().pc);
    if (const char *guard = std::getenv("MAXIMO_GUARD"))
    {
        g_guardAddr = static_cast<uint32_t>(std::strtoul(guard, nullptr, 16)) & ~3u;
        g_guardValue = rd32(g_guardAddr);
        logf("memory guard on 0x%06X (initial 0x%08X)", g_guardAddr, g_guardValue);
    }
    if (const char *watch = std::getenv("MAXIMO_VALWATCH"))
    {
        int used = 0;
        unsigned long long first = 0, last = 0;
        if (std::sscanf(watch, "%llu:%llu:%n", &first, &last, &used) == 2)
        {
            g_valWatchFirst = first;
            g_valWatchLast = last;
            for (const char *p = watch + used; *p;)
            {
                char *end = nullptr;
                const uint32_t addr = static_cast<uint32_t>(std::strtoul(p, &end, 16)) & PS2_RAM_MASK & ~3u;
                if (end == p)
                    break;
                g_valWatches.push_back({addr, rd32(addr)});
                p = *end == ',' ? end + 1 : end;
            }
        }
    }

    // Wrap every recompiled entry point with the tracer (milestones get their own wrapper). MAXIMO_TRACE=0 leaves the
    // entry points unwrapped (no call ring, entry counts or guard checks).
    g_original.assign(g_ps2RecompiledFunctionTable, g_ps2RecompiledFunctionTable + g_ps2RecompiledFunctionTableSlotCount);
    g_entryCount.assign(g_ps2RecompiledFunctionTableSlotCount, 0);
    const char *traceSetting = std::getenv("MAXIMO_TRACE");
    const bool traceCalls = !traceSetting || std::strcmp(traceSetting, "0") != 0;
    uint32_t populated = 0;
    for (uint32_t i = 0; i < g_ps2RecompiledFunctionTableSlotCount; ++i)
        if (g_original[i] && traceCalls)
        {
            g_ps2RecompiledFunctionTable[i] = traceEntry;
            ++populated;
        }
    for (const Milestone &m : g_milestones)
        runtime.replaceFunction(m.addr, milestoneEntry);
    logf("function table: %u entry points wrapped", populated);

    // SIF / IOP boundary.
    bindStub(runtime, 0x001AE738u, stub_sceSifInitRpc);
    bindStub(runtime, 0x001AEDA8u, stub_sceSifBindRpc);
    bindStub(runtime, 0x001AEF78u, stub_sceSifCallRpc);
    bindStub(runtime, 0x001AF168u, stub_sceSifCheckStatRpc);
    bindStub(runtime, 0x001B3918u, stub_sceSifSyncIop);
    bindStub(runtime, 0x001B3960u, stub_sceSifRebootIop);
    bindStub(runtime, 0x001B3460u, stub_sceSifLoadModule);
    // libcdvd -> runtime host-file implementation.
    bindRuntime(runtime, 0x001A1BA8u, "sceCdInit", ps2_stubs::sceCdInit, 1);
    bindRuntime(runtime, 0x001A2598u, "sceCdMmode", ps2_stubs::sceCdMmode, 1);
    bindRuntime(runtime, 0x001A08A0u, "sceCdSearchFile", ps2_stubs::sceCdSearchFile, 2);
    // libcdvd S-commands (CDVD S-cmd server 0x80000593): fn 12 status, fn 3 disc type (the disc is a
    // 2.27 GB DVD, so the runtime's SCECdPS2DVD answer is the right one).
    bindRuntime(runtime, 0x001A21D0u, "sceCdStatus", ps2_stubs::sceCdStatus, 0);
    bindRuntime(runtime, 0x001A2138u, "sceCdGetDiskType", ps2_stubs::sceCdGetDiskType, 0);
    bindRuntime(runtime, 0x001A10B0u, "sceCdRead", ps2_stubs::sceCdRead, 4);
    bindRuntime(runtime, 0x001A1928u, "sceCdSync", ps2_stubs::sceCdSync, 1);
    bindRuntime(runtime, 0x001A19C8u, "sceCdSyncS", ps2_stubs::sceCdSyncS, 1);
    bindRuntime(runtime, 0x001A1E88u, "sceCdDiskReady", ps2_stubs::sceCdDiskReady, 1);
    bindRuntime(runtime, 0x001A2AF8u, "sceCdStInit", ps2_stubs::sceCdStInit, 3);
    bindRuntime(runtime, 0x001A2B28u, "sceCdStStart", ps2_stubs::sceCdStStart, 2);
    bindRuntime(runtime, 0x001A2B60u, "sceCdStSeekF", ps2_stubs::sceCdStSeekF, 1);
    bindRuntime(runtime, 0x001A2B90u, "sceCdStSeek", ps2_stubs::sceCdStSeek, 1);
    bindRuntime(runtime, 0x001A2BC0u, "sceCdStStop", ps2_stubs::sceCdStStop, 0);
    bindRuntime(runtime, 0x001A2BF8u, "sceCdStRead", ps2_stubs::sceCdStRead, 4);
    bindRuntime(runtime, 0x001A2D80u, "sceCdStPause", ps2_stubs::sceCdStPause, 0);
    bindRuntime(runtime, 0x001A2DD0u, "sceCdStResume", ps2_stubs::sceCdStResume, 0);
    bindRuntime(runtime, 0x001A2E28u, "sceCdStStat", ps2_stubs::sceCdStStat, 0);
    // libpad / libmc -> runtime implementation.
    bindRuntime(runtime, 0x001C0110u, "scePadInit", ps2_stubs::scePadInit, 1);
    bindRuntime(runtime, 0x001C0370u, "scePadPortOpen", ps2_stubs::scePadPortOpen, 3);
    bindRuntime(runtime, 0x001B6D40u, "sceMcInit", ps2_stubs::sceMcInit, 0);
    bindRuntime(runtime, 0x001C06C0u, "scePadRead", scriptedPadRead, 3);
    bindRuntime(runtime, 0x001C0740u, "scePadGetState", ps2_stubs::scePadGetState, 2);
    bindRuntime(runtime, 0x001C0558u, "scePadPortClose", ps2_stubs::scePadPortClose, 2);
    bindRuntime(runtime, 0x001C02F0u, "scePadEnd", ps2_stubs::scePadEnd, 0);
    // The game's poller (0x128CB0) learns the controller type through these before it ever reads.
    bindRuntime(runtime, 0x001C07B8u, "scePadGetReqState", ps2_stubs::scePadGetReqState, 2);
    bindRuntime(runtime, 0x001C07F0u, "scePadSetReqState", ps2_stubs::scePadSetReqState, 3);
    bindRuntime(runtime, 0x001C08E0u, "scePadInfoAct", ps2_stubs::scePadInfoAct, 4);
    bindRuntime(runtime, 0x001C0B20u, "scePadInfoMode", ps2_stubs::scePadInfoMode, 4);
    bindRuntime(runtime, 0x001C0C58u, "scePadSetMainMode", ps2_stubs::scePadSetMainMode, 4);
    bindRuntime(runtime, 0x001C0D10u, "scePadSetActDirect", ps2_stubs::scePadSetActDirect, 3);
    bindRuntime(runtime, 0x001C0DC8u, "scePadSetActAlign", ps2_stubs::scePadSetActAlign, 3);
    // libmc client (identified by the MCSERV RPC function each one sends: 1 GetInfo, 2 Open, 3 Close,
    // 4 Seek, 5 Read, 6 Write, 12 ChDir, 13 GetDir, 16 Format; Mkdir is Open with mode 0x40; Sync
    // polls sceSifCheckStatRpc). The original client talks to MCSERV, which is not running.
    bindRuntime(runtime, 0x001B77F8u, "sceMcGetInfo", ps2_stubs::sceMcGetInfo, 5);
    bindRuntime(runtime, 0x001B7068u, "sceMcOpen", ps2_stubs::sceMcOpen, 4);
    bindRuntime(runtime, 0x001B7178u, "sceMcMkdir", ps2_stubs::sceMcMkdir, 3);
    bindRuntime(runtime, 0x001B71B0u, "sceMcClose", ps2_stubs::sceMcClose, 1);
    bindRuntime(runtime, 0x001B7268u, "sceMcSeek", ps2_stubs::sceMcSeek, 3);
    bindRuntime(runtime, 0x001B73D0u, "sceMcRead", ps2_stubs::sceMcRead, 3);
    bindRuntime(runtime, 0x001B74F0u, "sceMcWrite", ps2_stubs::sceMcWrite, 3);
    bindRuntime(runtime, 0x001B7B38u, "sceMcChdir", ps2_stubs::sceMcChdir, 4);
    bindRuntime(runtime, 0x001B7980u, "sceMcGetDir", ps2_stubs::sceMcGetDir, 6);
    bindRuntime(runtime, 0x001B7C68u, "sceMcFormat", ps2_stubs::sceMcFormat, 2);
    bindRuntime(runtime, 0x001B76C0u, "sceMcSync", ps2_stubs::sceMcSync, 3);
    // MAXIMO_WATCHFN=addr[,addr...]: extra watch points (first four arguments logged), for diagnostics.
    if (const char *extra = std::getenv("MAXIMO_WATCHFN"))
        for (const char *p = extra; *p;)
        {
            char *end = nullptr;
            const uint32_t addr = static_cast<uint32_t>(std::strtoul(p, &end, 16));
            if (end == p)
                break;
            g_watches.emplace(addr, Watch{"MAXIMO_WATCHFN", 4});
            p = *end == ',' ? end + 1 : end;
        }
    for (const auto &[addr, w] : g_watches)
        if (!isMilestone(addr))
            runtime.replaceFunction(addr, watchEntry);
    runtime.replaceFunction(0x001AAA18u, gsSyncVEntry); // sceGsSyncV: frame timing
    runtime.replaceFunction(0x001AA1C0u, putDispEnvEntry); // sceGsPutDispEnv: GPU-mode movie presentation
    if (widescreen)
    {
        runtime.replaceFunction(0x0015B100u, wideCameraEntry);
        runtime.replaceFunction(0x0016AE00u, wideCullEntry);
        runtime.replaceFunction(0x00148470u, wideOverlayEntry);
        runtime.replaceFunction(kScaleMatrixFn, wideFlashEntry);
        runtime.replaceFunction(kSetViewFn, wideHudEntry);
        runtime.replaceFunction(kSetModelFn, wideHudEntry);
        const char *hud = std::getenv("MAXIMO_WIDE_HUD");
        g_hudEdges = !hud || std::strcmp(hud, "0") != 0;
        logf("widescreen 16:9: HUD side groups %s (MAXIMO_WIDE_HUD=0 keeps them in the 4:3 area)",
             g_hudEdges ? "at the screen edges" : "in the 4:3 area");
        g_widescreen = true;
        logf("widescreen 16:9: camera x scale %.4f, culling factor %.4f", kWideXScale, kWideCullFactor);
    }
    if (const char *bits = std::getenv("MAXIMO_MOVIE_BITS"); !bits || std::strcmp(bits, "0") != 0)
    {
        g_movieBitsVerify = bits && std::strcmp(bits, "verify") == 0;
        for (uint32_t fn : {kBitsPeekFn, kBitsSkipFn, kBitsReadFn, kBitsReadBitFn})
            runtime.replaceFunction(fn, movieBitsEntry);
        for (uint32_t fn : {kPackHeaderFn, kPesHeaderFn})
            runtime.replaceFunction(fn, movieHeaderEntry);
        logf("movie bit reader and packet headers: native%s (MAXIMO_MOVIE_BITS=0 keeps the game's code)",
             g_movieBitsVerify ? ", checked against the game's code" : "");
    }
#ifdef MAXIMO_HAS_FFMPEG
    if (const char *ff = std::getenv("MAXIMO_MOVIE_FFMPEG"); !ff || std::strcmp(ff, "0") != 0)
    {
        // The FFmpeg DLLs are delay-loaded: without them the game's decoder stays.
        if (LoadLibraryA("avutil-59.dll") && LoadLibraryA("avcodec-61.dll") && movieDecoderOpen(g_movie))
        {
            runtime.replaceFunction(kGetPictureFn, movieGetPictureEntry);
            for (uint32_t fn : {kMpegCreateFn, kMpegResetFn})
                runtime.replaceFunction(fn, movieMpegResetEntry);
            g_movieFfmpeg = true;
            logf("movie pictures: FFmpeg %s (MAXIMO_MOVIE_FFMPEG=0 keeps the game's decoder)", av_version_info());
        }
        else
        {
            logf("movie pictures: FFmpeg not available, the game's decoder decodes them");
        }
    }
#endif
    if (const char *rate = std::getenv("MAXIMO_FRAME_RATE"); rate && std::strcmp(rate, "50") != 0)
    {
        if (std::strcmp(rate, "60") == 0)
        {
            g_frameRateHz = 60;
            runtime.replaceFunction(kLevelLoopFn, levelLoopEntry);
            runtime.replaceFunction(kAnimStepFn, animStepEntry);
            runtime.replaceFunction(kCameraShakeFn, cameraShakeEntry);
            runtime.replaceFunction(kLightningFn, lightningEntry);
            logf("frame rate: 60 Hz in gameplay, 50 Hz in the front end, attract demo and movies (MAXIMO_FRAME_RATE=60)");
            if (std::getenv("PS2X_VBLANK_HZ"))
                logf("frame rate: PS2X_VBLANK_HZ is set and pins the field rate; MAXIMO_FRAME_RATE has no effect");
        }
        else
            logf("frame rate: MAXIMO_FRAME_RATE=%s is not supported (50 or 60); keeping 50 Hz", rate);
    }
    {
        const char *camLog = std::getenv("MAXIMO_CAMERA_LOG");
        const char *camTest = std::getenv("MAXIMO_CAMERA_TEST");
        g_camLog = camLog && std::strcmp(camLog, "0") != 0;
        if (camTest)
        {
            g_camTestDeg = static_cast<float>(std::atof(camTest));
            if (const char *comma = std::strchr(camTest, ','))
                g_camTestAxis = comma[1] == 'x' ? 0 : comma[1] == 'z' ? 2 : 1;
        }
        if (const char *mode = std::getenv("MAXIMO_CAMERA"); mode && std::strcmp(mode, "orbit") == 0)
        {
            g_orbit.on = true;
            if (const char *speed = std::getenv("MAXIMO_CAMERA_SPEED"))
            {
                const float deg = static_cast<float>(std::atof(speed));
                if (deg > 0.0f && deg <= 1000.0f)
                    g_orbit.speed = deg * 3.14159265f / 180.0f;
            }
            if (const char *ret = std::getenv("MAXIMO_CAMERA_RETURN"))
                g_orbit.returnDelay = std::max(0.0f, static_cast<float>(std::atof(ret)));
            const char *invert = std::getenv("MAXIMO_CAMERA_INVERT_Y");
            g_orbit.invertY = invert && std::strcmp(invert, "0") != 0;
            logf("camera: free camera on (right stick orbits, R3 hands back), %.0f degrees/s%s, back after %.1f s",
                 g_orbit.speed * 57.2958f, g_orbit.invertY ? ", inverted up/down" : "", g_orbit.returnDelay);
        }
        if (g_camLog || g_camTestDeg != 0.0f || g_orbit.on)
        {
            runtime.replaceFunction(kCameraUpdateFn, cameraUpdateEntry);
            runtime.replaceFunction(kCameraApplyFn, cameraApplyEntry);
            runtime.replaceFunction(kCameraViewFn, cameraViewEntry);
            logf("camera probe: log %d, test %.1f degrees about %c", g_camLog ? 1 : 0, g_camTestDeg,
                 "xyz"[g_camTestAxis]);
        }
    }
    if (const char *spin = std::getenv("MAXIMO_SPIN_SKIP"); !spin || std::strcmp(spin, "0") != 0)
    {
        g_vblankSpinChunked = spin && std::strcmp(spin, "chunk") == 0;
        runtime.replaceFunction(0x001AC320u, vblankSpinEntry); // sceGsSyncV's VBlank spin: idle cycles in chunks
        runtime.replaceFunction(0x001AC330u, vblankSpinEntry); // its resume point after a checkpoint
    }
    // libc.
    bindStub(runtime, 0x0019C3E8u, stub_printf);
    bindStub(runtime, 0x00197F88u, stub_assert);

    ps2_stubs::setCdStreamPastFileEnd(true);
    // MAXIMO_MMIO_TRACE=0 leaves out the hardware-register access log (the report at exit).
    if (const char *trace = std::getenv("MAXIMO_MMIO_TRACE"); !trace || std::strcmp(trace, "0") != 0)
        PS2Runtime::setMmioTraceHook(mmioHook);
    logf("---- run ----");
    // The windowed loop has no time limit of its own.
    std::thread windowTimer;
    if (windowed)
        windowTimer = std::thread([seconds]()
                                  {
            std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
            g_runtime->requestStop(); });
    if (windowTimer.joinable())
        windowTimer.detach(); // closing the window early must not wait out the timer
    std::thread profiler;
    if (const char *profile = std::getenv("MAXIMO_PROFILE"))
    {
        double start = 0.0, end = 1e9;
        // f<first>,<last>: a guest frame range (sceGsSyncV count) instead of wall-clock seconds.
        const bool guestFrames = profile[0] == 'f';
        std::sscanf(profile + (guestFrames ? 1 : 0), "%lf,%lf", &start, &end);
        profiler = std::thread(profileThread, start, end, guestFrames);
    }
    if (const char *pipeline = std::getenv("PS2X_VU1_PIPELINE"); pipeline && pipeline[0] == '1')
    {
        // The worker presents frames itself, which only GPU mode supports.
        const char *vu1Thread = std::getenv("PS2X_VU1_THREAD");
#ifdef MAXIMO_ENABLE_GPU_SHADOW
        const bool gpuMode = g_gpuMode;
#else
        const bool gpuMode = false;
#endif
        if (!gpuMode || !vu1Thread || vu1Thread[0] != '1')
        {
            _putenv("PS2X_VU1_PIPELINE=0");
            logf("PS2X_VU1_PIPELINE=1 ignored: needs PS2X_VU1_THREAD=1 and MAXIMO_GS_BACKEND=gpu");
        }
        else
            logf("PS2X_VU1_PIPELINE=1: the VU1 worker presents frame N while the game builds frame N+1 (experimental)");
    }
    runtime.run();
    logf("shutdown: window loop ended: %s", ps2_exit_reason::get());
#ifdef MAXIMO_ENABLE_GPU_SHADOW
    runtime.setHostFrameSource(nullptr);
    runtime.gifArbiter().setPathPacketFn(nullptr);
    g_gpuShadow.reset();
    if (g_gpuFailed.load())
        logf("[gpu] run stopped by a GPU-mode error (see stderr)");
#endif
    g_profileStop.store(true);
    if (profiler.joinable())
        profiler.join();
    PS2Runtime::setMmioTraceHook(nullptr);
    logf("---- stopped ----");
    vu1cap::finishCapture(logf);
    reportProfile();

    // ---- report ------------------------------------------------------------------------------
    const R5900Context &c = runtime.cpu();
    logf("final pc register 0x%06X; distinct guest functions entered: %u", c.pc, g_distinct);
    for (const Milestone &m : g_milestones)
        logf("  milestone %-3s 0x%06X %s", m.reached ? "YES" : "no", m.addr, m.what);
    dumpRing("end of run");

    std::vector<std::pair<uint32_t, uint32_t>> hot;
    for (uint32_t i = 0; i < g_entryCount.size(); ++i)
        if (g_entryCount[i])
            hot.push_back({g_entryCount[i], g_ps2RecompiledFunctionTableBase + i * 4});
    std::sort(hot.rbegin(), hot.rend());
    // Heuristic backtrace: words on the main thread's stack that point just past a jal/jalr.
    for (const EeThreadSnapshot &t : runtime.eeScheduler().snapshot().threads)
    {
        if (t.id != 1 || t.sp == 0)
            continue;
        logf("---- main thread backtrace (stack scan from sp=0x%07X) ----", t.sp);
        logf("  pc 0x%06X  ra 0x%06X", t.pc, t.ra);
        int frames = 0;
        for (uint32_t a = t.sp & ~3u; a < PS2_RAM_SIZE && frames < 24; a += 4)
        {
            const uint32_t v = rd32(a);
            if (v < 0x00100008u || v >= 0x001C3E80u || (v & 3u))
                continue;
            const uint32_t call = rd32(v - 8u);
            const bool jal = (call >> 26) == 3u;
            const bool jalr = (call & 0xFC00003Fu) == 0x00000009u;
            if (!jal && !jalr)
                continue;
            if (jal)
                logf("  [sp+0x%05X] return 0x%06X  (call to 0x%06X)", a - t.sp, v, (call & 0x03FFFFFFu) << 2);
            else
                logf("  [sp+0x%05X] return 0x%06X  (indirect call)", a - t.sp, v);
            ++frames;
        }
    }

    logf("---- most-entered functions ----");
    for (size_t i = 0; i < std::min<size_t>(hot.size(), 25); ++i)
        logf("  0x%06X entered %u times", hot[i].second, hot[i].first);

    const EeKernelSnapshot snap = runtime.eeScheduler().snapshot();
    logf("---- EE threads (cycle %llu) ----", static_cast<unsigned long long>(snap.eeCycle));
    static const char *status[] = {"Running", "Ready", "Waiting", "WaitingSuspended", "Suspended", "Dormant"};
    static const char *wait[] = {"None", "Sleep", "Semaphore", "EventFlag", "VSync", "External", "Mpeg"};
    for (const EeThreadSnapshot &t : snap.threads)
        logf("  thread %d pc=0x%06X ra=0x%06X entry=0x%06X prio=%d %s wait=%s(%d)", t.id, t.pc, t.ra, t.entry,
             t.currentPriority, status[static_cast<int>(t.status)], wait[static_cast<int>(t.waitReason)], t.waitId);

    finishAudio();
    logf("---- SDK boundary calls ----");
    for (const auto &[addr, b] : g_bound)
        if (b.calls)
            logf("  0x%06X %-16s %llu calls", addr, b.name, static_cast<unsigned long long>(b.calls));
    for (const RpcServer &s : g_servers)
        logf("  RPC server 0x%08X %-12s %llu calls", s.sid, sidName(s.sid), static_cast<unsigned long long>(s.calls));
    for (const auto &[key, n] : g_unhandledRpc)
        logf("  UNHANDLED RPC sid=0x%08X fn=0x%X: %llu calls", static_cast<uint32_t>(key >> 16),
             static_cast<uint32_t>(key & 0xFFFF), static_cast<unsigned long long>(n));
    for (uint32_t core = 0; core < 2; ++core)
        if (g_movieAudio[core].transfers)
            logf("  movie-audio drain core %u: %llu block transfers, %llu status replies, %s, %llu bytes discarded at stop",
                 core, static_cast<unsigned long long>(g_movieAudio[core].transfers),
                 static_cast<unsigned long long>(g_movieAudio[core].statusCalls),
                 g_movieAudio[core].active ? "still running" : "stopped",
                 static_cast<unsigned long long>(g_movieAudioDrainedBytes));
    for (const auto &[fn, n] : g_sdRemoteCalls)
        logf("  LIBSD remote 0x%X (emulated IOP libsd): %llu calls", fn, static_cast<unsigned long long>(n));
    logf("  GPU vsyncs from display flips without sceGsSyncV (movies): %llu", static_cast<unsigned long long>(g_flipVsyncs));
    if (g_movieBitsCalls)
        logf("  movie bit reader: %llu native calls%s", static_cast<unsigned long long>(g_movieBitsCalls),
             g_movieBitsVerify ? (", " + std::to_string(g_movieBitsChecked) + " checked against the game's code, " +
                                  std::to_string(g_movieBitsMismatches) + " differ").c_str()
                               : "");
    if (g_movieHeaderCalls || g_movieHeaderChecked)
        logf("  movie packet headers: %llu native calls%s", static_cast<unsigned long long>(g_movieHeaderCalls),
             g_movieBitsVerify ? (", " + std::to_string(g_movieHeaderChecked) + " checked against the game's code, " +
                                  std::to_string(g_movieHeaderMismatches) + " differ").c_str()
                               : "");
#ifdef MAXIMO_HAS_FFMPEG
    if (g_movieFfmpeg)
        logf("  movie pictures: %llu decoded by FFmpeg from %llu access units, %llu rejected",
             static_cast<unsigned long long>(g_movie.pictures), static_cast<unsigned long long>(g_movie.units),
             static_cast<unsigned long long>(g_movie.errors));
#endif
    if (g_frameRateHz != 50)
        logf("  frame rate %u Hz: %llu rate switches, %llu frame-counter increments skipped, %llu animation steps, "
             "%llu camera shake frames and %llu lightning frames paced", g_frameRateHz, static_cast<unsigned long long>(g_frameRateSwitches),
             static_cast<unsigned long long>(g_counterHolds), static_cast<unsigned long long>(g_fixedAnimSteps),
             static_cast<unsigned long long>(g_shakeFrames), static_cast<unsigned long long>(g_lightningFrames));
    logf("  sound driver: %llu packets, %llu malformed", static_cast<unsigned long long>(g_soundPackets),
         static_cast<unsigned long long>(g_soundMalformed));
    for (const auto &[id, st] : g_soundCmds)
        logf("    sound cmd %3u nargs=%u: %llu", id, st.nargs, static_cast<unsigned long long>(st.count));
    for (const std::string &m : g_loadedModules)
        logf("  module accepted: %s", m.c_str());

    logf("---- game heap object 0x2393B0 (pools at +0x20, +0x140, +0x260) ----");
    for (uint32_t off = 0; off < 0x60; off += 16)
        logf("  +0x%03X: %08X %08X %08X %08X", off, rd32(0x2393B0u + off), rd32(0x2393B4u + off), rd32(0x2393B8u + off),
             rd32(0x2393BCu + off));
    logf("---- RAM below the ELF image (0x000000-0x100000) written by the run ----");
    for (uint32_t page = 0; page < 0x100000u; page += 0x1000u)
    {
        uint32_t nonzero = 0;
        for (uint32_t a = page; a < page + 0x1000u; a += 4)
            nonzero += rd32(a) != 0;
        if (nonzero)
            logf("  page 0x%06X: %u nonzero words", page, nonzero);
    }

    logf("---- hardware registers touched ----");
    for (const auto &[addr, s] : g_mmio)
        logf("  %-13s 0x%08X reads=%llu writes=%llu last=0x%llX pc~0x%06X", mmioUnit(addr), addr,
             static_cast<unsigned long long>(s.reads), static_cast<unsigned long long>(s.writes),
             static_cast<unsigned long long>(s.lastWrite), s.lastPc);
    logf("  scratchpad accesses via slow path: %llu; uncached RAM mirror accesses: %llu",
         static_cast<unsigned long long>(g_scratchAccesses), static_cast<unsigned long long>(g_uncachedAccesses));
    // The launcher treats this line as "the game finished shutting down": a process still alive well after it is
    // hanging in process exit. stdout and stderr are redirected to a file by the launcher, so flush them too.
    logf("shutdown: log complete, exiting process");
    std::fflush(stdout);
    std::fflush(stderr);
    std::fflush(g_log);
    std::_Exit(0); // guest threads may still be parked; skip static destructors
}
