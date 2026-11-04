// VU1 microprogram capture and offline replay for the Maximo boot test.
//
// MAXIMO_VU1_CAPTURE=<file>,<startSeconds>,<count>
//   Records <count> consecutive VU1 MSCAL/MSCNT invocations, starting with the first MSCAL after
//   <startSeconds>. A continuation needs the preceding interpreter pipeline state, so capture begins
//   at an MSCAL and replay retains one interpreter across the sequence.
//
// MAXIMO_VU1_REPLAY=<file>[,<expectFile>[,<repeats>]]
//   Runs every recorded invocation through the current VU1Interpreter, without booting the game, and
//   prints mismatches against the live result plus host time per invocation. Each record's outcome
//   (VU state, data memory and every PATH1 packet) is reduced to a digest. If <expectFile> does not
//   exist the digests are written to it; if it does, they are compared, so an interpreter change can be
//   checked against the digests the previous build produced.
//
// Capture files hold game data (microcode and scene data): they stay in the local run directories.
#pragma once

#include "ps2_runtime.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace vu1cap
{
    constexpr uint32_t kMagic = 0x31435556u; // "VUC1"
    constexpr uint32_t kCodeSize = PS2_VU1_CODE_SIZE;
    constexpr uint32_t kDataSize = PS2_VU1_DATA_SIZE;

    struct RecordHeader
    {
        uint32_t magic;
        uint32_t startPC;
        uint32_t top;
        uint32_t itop;
        uint32_t stateSize;
        uint32_t codeSize;
        uint32_t dataSize;
        uint32_t reserved; // 0 = MSCAL, 1 = MSCNT
        uint64_t liveNs;
    };

    inline uint64_t fnv(uint64_t h, const void *data, size_t size)
    {
        const uint8_t *p = static_cast<const uint8_t *>(data);
        for (size_t i = 0; i < size; ++i)
            h = (h ^ p[i]) * 0x100000001B3ull;
        return h;
    }

    // Everything architecturally visible after a microprogram, except the cycle counter, which is
    // absolute and differs between the live run and a replay.
    inline uint64_t stateDigest(const VU1State &s, uint64_t h = 0xCBF29CE484222325ull)
    {
        h = fnv(h, s.vf, sizeof(s.vf));
        h = fnv(h, s.vi, sizeof(s.vi));
        h = fnv(h, s.acc, sizeof(s.acc));
        const uint32_t words[] = {0, 0, 0, s.r, s.pc, s.mac, s.clip, s.status, s.top, s.itop,
                                  s.branchTarget, s.branchDelay};
        h = fnv(h, words + 3, sizeof(words) - 3 * sizeof(uint32_t));
        h = fnv(h, &s.q, sizeof(s.q));
        h = fnv(h, &s.p, sizeof(s.p));
        h = fnv(h, &s.i, sizeof(s.i));
        const uint8_t flags[] = {s.ebit, s.haltAfterDelaySlot, s.stoppedByD, s.stoppedByT, s.branchPending};
        return fnv(h, flags, sizeof(flags));
    }

    struct Capture
    {
        std::FILE *file = nullptr;
        double startSeconds = 0.0;
        uint32_t remaining = 0;
        uint32_t written = 0;
        uint64_t mscalCalls = 0;
        uint64_t mscntCalls = 0;
        std::chrono::steady_clock::time_point t0;
        std::vector<uint8_t> pre;
    };

    inline Capture g_capture;

    template <typename Callback>
    inline void captureCall(PS2Runtime &runtime, PS2Memory &memory, uint32_t startPC,
                            uint32_t top, uint32_t itop, bool continuation, Callback &&original)
    {
        const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_capture.t0).count();
        const bool record = g_capture.remaining > 0 && now >= g_capture.startSeconds &&
                            (g_capture.written != 0 || !continuation);
        RecordHeader header{kMagic, startPC, top, itop, sizeof(VU1State), kCodeSize, kDataSize,
                            continuation ? 1u : 0u, 0};
        if (record)
        {
            g_capture.pre.resize(sizeof(VU1State) + kCodeSize + kDataSize);
            std::memcpy(g_capture.pre.data(), &runtime.vu1().state(), sizeof(VU1State));
            std::memcpy(g_capture.pre.data() + sizeof(VU1State), memory.getVU1Code(), kCodeSize);
            std::memcpy(g_capture.pre.data() + sizeof(VU1State) + kCodeSize, memory.getVU1Data(), kDataSize);
        }
        const auto begin = std::chrono::steady_clock::now();
        original();
        if (!record)
            return;
        header.liveNs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin).count());
        std::fwrite(&header, sizeof(header), 1, g_capture.file);
        std::fwrite(g_capture.pre.data(), 1, g_capture.pre.size(), g_capture.file);
        std::fwrite(&runtime.vu1().state(), sizeof(VU1State), 1, g_capture.file);
        std::fwrite(memory.getVU1Data(), 1, kDataSize, g_capture.file);
        ++g_capture.written;
        if (--g_capture.remaining == 0)
            std::fflush(g_capture.file);
    }

    inline void installCapture(PS2Runtime &runtime, const char *spec, void (*logf)(const char *, ...))
    {
        char path[512] = {};
        double start = 0.0;
        unsigned count = 0;
        if (std::sscanf(spec, "%511[^,],%lf,%u", path, &start, &count) != 3 || count == 0)
        {
            logf("MAXIMO_VU1_CAPTURE: expected <file>,<startSeconds>,<count>");
            return;
        }
        g_capture.file = std::fopen(path, "wb");
        if (!g_capture.file)
        {
            logf("MAXIMO_VU1_CAPTURE: cannot write %s", path);
            return;
        }
        g_capture.startSeconds = start;
        g_capture.remaining = count;
        g_capture.t0 = std::chrono::steady_clock::now();
        logf("vu1 capture: %u consecutive MSCAL/MSCNT calls from %.1f s into %s", count, start, path);

        PS2Memory &memory = runtime.memory();
        const PS2Memory::Vu1MscalCallback original = memory.vu1MscalCallback();
        memory.setVu1MscalCallback([&runtime, &memory, original](uint32_t startPC, uint32_t top, uint32_t itop)
                                   {
            ++g_capture.mscalCalls;
            captureCall(runtime, memory, startPC, top, itop, false,
                        [&] { original(startPC, top, itop); }); });

        const PS2Memory::Vu1MscntCallback originalCnt = memory.vu1MscntCallback();
        memory.setVu1MscntCallback([&runtime, &memory, originalCnt](uint32_t top, uint32_t itop)
                                   {
            ++g_capture.mscntCalls;
            captureCall(runtime, memory, runtime.vu1().state().pc, top, itop, true,
                        [&] { originalCnt(top, itop); }); });
    }

    inline void finishCapture(void (*logf)(const char *, ...))
    {
        if (!g_capture.file)
            return;
        std::fclose(g_capture.file);
        g_capture.file = nullptr;
        logf("vu1 capture: %u records written; %llu MSCAL and %llu MSCNT calls seen", g_capture.written,
             static_cast<unsigned long long>(g_capture.mscalCalls),
             static_cast<unsigned long long>(g_capture.mscntCalls));
    }

    struct Record
    {
        RecordHeader header{};
        VU1State preState{};
        std::vector<uint8_t> code;
        std::vector<uint8_t> data;
        VU1State postState{};
        std::vector<uint8_t> postData;
    };

    inline bool loadRecords(const char *path, std::vector<Record> &records)
    {
        std::FILE *f = std::fopen(path, "rb");
        if (!f)
            return false;
        for (;;)
        {
            Record r;
            const size_t headerBytes = std::fread(&r.header, 1, sizeof(r.header), f);
            if (headerBytes == 0 && std::feof(f))
                break;
            if (headerBytes != sizeof(r.header))
            {
                std::fclose(f);
                return false;
            }
            if (r.header.magic != kMagic || r.header.reserved > 1 || r.header.stateSize != sizeof(VU1State) ||
                r.header.codeSize != kCodeSize || r.header.dataSize != kDataSize)
            {
                std::fclose(f);
                return false;
            }
            r.code.resize(kCodeSize);
            r.data.resize(kDataSize);
            r.postData.resize(kDataSize);
            const bool ok = std::fread(&r.preState, sizeof(VU1State), 1, f) == 1 &&
                            std::fread(r.code.data(), 1, kCodeSize, f) == kCodeSize &&
                            std::fread(r.data.data(), 1, kDataSize, f) == kDataSize &&
                            std::fread(&r.postState, sizeof(VU1State), 1, f) == 1 &&
                            std::fread(r.postData.data(), 1, kDataSize, f) == kDataSize;
            if (!ok)
            {
                std::fclose(f);
                return false;
            }
            records.push_back(std::move(r));
        }
        std::fclose(f);
        return true;
    }

    // Returns the process exit code.
    inline int replay(const char *spec, void (*logf)(const char *, ...))
    {
        char path[512] = {}, expectPath[512] = {};
        unsigned repeats = 1;
        std::sscanf(spec, "%511[^,],%511[^,],%u", path, expectPath, &repeats);
        if (repeats == 0)
            repeats = 1;
        std::vector<Record> records;
        if (!loadRecords(path, records) || records.empty() || records.front().header.reserved != 0)
        {
            logf("vu1 replay: no records in %s", path);
            return 1;
        }

        std::vector<uint64_t> expected;
        bool writeExpected = false;
        bool hasExpected = false;
        if (expectPath[0])
        {
            if (std::FILE *e = std::fopen(expectPath, "rb"))
            {
                hasExpected = true;
                uint64_t d = 0;
                while (std::fread(&d, sizeof(d), 1, e) == 1)
                    expected.push_back(d);
                std::fclose(e);
            }
            else
                writeExpected = true;
        }

        static PS2Memory memory;
        if (!memory.initialize())
        {
            logf("vu1 replay: memory initialisation failed");
            return 1;
        }
        std::vector<uint8_t> packets;
        memory.setGifPacketCallback([&packets](const uint8_t *data, uint32_t size)
                                    {
            const uint32_t n = size;
            packets.insert(packets.end(), reinterpret_cast<const uint8_t *>(&n), reinterpret_cast<const uint8_t *>(&n) + 4);
            packets.insert(packets.end(), data, data + size); });
        GS gs;
        uint32_t liveMismatches = 0, expectMismatches = 0;
        uint64_t totalNs = 0, totalLiveNs = 0, totalPackets = 0;
        std::vector<uint64_t> digests(records.size());
        for (unsigned pass = 0; pass < repeats; ++pass)
        {
            std::unique_ptr<VU1Interpreter> vu;
            uint64_t codeHash = 0;
            for (size_t index = 0; index < records.size(); ++index)
            {
                const Record &r = records[index];
                const uint64_t hash = fnv(0xCBF29CE484222325ull, r.code.data(), r.code.size());
                if (!vu || hash != codeHash)
                {
                    if (r.header.reserved != 0)
                    {
                        logf("vu1 replay: microcode changed before MSCNT record %zu", index);
                        return 2;
                    }
                    // A fresh interpreter rebuilds its decoded-pair cache for this microcode.
                    vu = std::make_unique<VU1Interpreter>(VU1Interpreter::Unit::VU1);
                    codeHash = hash;
                }
                std::memcpy(memory.getVU1Code(), r.code.data(), kCodeSize);
                std::memcpy(memory.getVU1Data(), r.data.data(), kDataSize);
                vu->state() = r.preState;
                packets.clear();
                const auto begin = std::chrono::steady_clock::now();
                if (r.header.reserved == 0)
                    vu->execute(memory.getVU1Code(), kCodeSize, memory.getVU1Data(), kDataSize, gs, &memory,
                                r.header.startPC, r.header.top, r.header.itop, 65536);
                else
                    vu->resume(memory.getVU1Code(), kCodeSize, memory.getVU1Data(), kDataSize, gs, &memory,
                               r.header.top, r.header.itop, 65536);
                totalNs += static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin).count());
                if (pass == 0)
                {
                    uint64_t digest = stateDigest(vu->state());
                    digest = fnv(digest, memory.getVU1Data(), kDataSize);
                    digest = fnv(digest, packets.data(), packets.size());
                    digests[index] = digest;
                    totalPackets += packets.size();
                    totalLiveNs += r.header.liveNs;
                    const bool liveMatch = stateDigest(vu->state()) == stateDigest(r.postState) &&
                                           std::memcmp(memory.getVU1Data(), r.postData.data(), kDataSize) == 0;
                    if (!liveMatch && liveMismatches++ < 8)
                        logf("vu1 replay: record %zu (pc 0x%X) differs from the live result", index, r.header.startPC);
                    if (hasExpected && (index >= expected.size() || expected[index] != digest) &&
                        expectMismatches++ < 8)
                        logf("vu1 replay: record %zu (pc 0x%X) differs from %s", index, r.header.startPC, expectPath);
                }
            }
        }
        if (hasExpected && expected.size() != digests.size())
            ++expectMismatches;

        uint64_t overall = 0xCBF29CE484222325ull;
        for (uint64_t d : digests)
            overall = fnv(overall, &d, sizeof(d));
        logf("vu1 replay: %zu records, %llu PATH1 bytes, digest %016llX", records.size(),
             static_cast<unsigned long long>(totalPackets), static_cast<unsigned long long>(overall));
        logf("vu1 replay: %u differ from live; %s", liveMismatches,
             !hasExpected ? (writeExpected ? "expected digests written" : "no expected digests")
                              : (expectMismatches ? "EXPECTED DIGESTS DIFFER" : "all expected digests match"));
        logf("vu1 replay: %.1f us per call (x%u repeats), live %.1f us per call",
             totalNs / 1000.0 / (records.size() * repeats), repeats, totalLiveNs / 1000.0 / records.size());
        if (writeExpected)
        {
            if (std::FILE *e = std::fopen(expectPath, "wb"))
            {
                const bool saved = std::fwrite(digests.data(), sizeof(uint64_t), digests.size(), e) == digests.size();
                std::fclose(e);
                if (!saved)
                    return 1;
            }
            else
                return 1;
        }
        return (liveMismatches || expectMismatches) ? 3 : 0;
    }
}
