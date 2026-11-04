// maximo_boot.exe --hdtex-convert <pack .zip or folder> <output folder> [--cap N] [--format rgba8|bc7] [--threads N]
//
// Converts a PCSX2 texture replacement pack for SLES-50703 into textures\<pack>\pack.mhtx (hdtex_format.h).
// Every image is decoded once, scaled down to a power of two no larger than the cap (never up), given a full
// mip chain and stored. Progress goes to stdout as lines the launcher reads:
//   HDTEX files <n>              image files found
//   HDTEX progress <done> <n>
//   HDTEX done used=<u> skipped=<s> bytes=<b>
//   HDTEX error <text>
#include "hdtex_format.h"

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"
#define STB_IMAGE_RESIZE_STATIC
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image_resize2.h"
#include "miniz.h"
#include "bc7enc.h"

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <tuple>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

static std::string u8(const fs::path &p) { auto s = p.u8string(); return std::string(s.begin(), s.end()); }

namespace hdtex {
namespace {

struct Name { uint64_t tex = 0, clut = 0; uint32_t bits = 0; };

bool parseHex(const std::string &s, size_t maxDigits, uint64_t &out)
{
    if (s.empty() || s.size() > maxDigits) return false;
    out = 0;
    for (char c : s) {
        int v = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
        if (v < 0) return false;
        out = (out << 4) | uint64_t(v);
    }
    return true;
}

// "<tex>-<bits>.png" or "<tex>-<clut>-<bits>.png" (PCSX2 GSTextureReplacements); also accepts a doubled ".png".
// Returns an empty string on success, else why the name is not used.
std::string parseName(std::string file, Name &n)
{
    std::string lower = file;
    for (char &c : lower) c = char(std::tolower((unsigned char)c));
    if (lower.size() < 4 || lower.compare(lower.size() - 4, 4, ".png") != 0) {
        if (lower.size() >= 4 && lower.compare(lower.size() - 4, 4, ".dds") == 0) return "DDS files are not supported yet";
        return "not a PNG";
    }
    while (lower.size() >= 4 && lower.compare(lower.size() - 4, 4, ".png") == 0) lower.resize(lower.size() - 4);
    std::vector<std::string> parts;
    size_t a = 0;
    for (size_t b; (b = lower.find('-', a)) != std::string::npos; a = b + 1) parts.push_back(lower.substr(a, b - a));
    parts.push_back(lower.substr(a));
    for (const std::string &p : parts)
        if (!p.empty() && p[0] == 'r' && p.find('x') != std::string::npos) return "region textures are not supported yet";
    uint64_t bits = 0;
    if (parts.size() == 2 && parseHex(parts[0], 16, n.tex) && parts[1].size() == 8 && parseHex(parts[1], 8, bits)) {
        n.clut = 0; n.bits = uint32_t(bits); return {};
    }
    if (parts.size() == 3 && parseHex(parts[0], 16, n.tex) && parseHex(parts[1], 16, n.clut) && parts[2].size() == 8 &&
        parseHex(parts[2], 8, bits)) {
        n.bits = uint32_t(bits); return {};
    }
    return "name is not a PCSX2 replacement name (<hash>[-<clut hash>]-<bits>.png)";
}

// A pack source: a zip (each worker opens its own reader) or a folder (searched recursively).
struct Source {
    bool zip = false;
    std::string path;
    struct Item { std::string name; uint32_t zipIndex = 0; fs::path file; };
    std::vector<Item> items; // every file, in pack order
};

bool openZip(mz_zip_archive &z, const std::string &path)
{
    std::memset(&z, 0, sizeof(z));
    return mz_zip_reader_init_file_v2(&z, path.c_str(), 0, 0, 0) != 0;
}

bool listSource(Source &src, std::string &err)
{
    std::error_code ec;
    if (fs::is_directory(fs::u8path(src.path), ec)) {
        src.zip = false;
        for (auto it = fs::recursive_directory_iterator(fs::u8path(src.path), ec); !ec && it != fs::recursive_directory_iterator();
             it.increment(ec))
            if (it->is_regular_file(ec)) src.items.push_back({u8(it->path().filename()), 0, it->path()});
        std::sort(src.items.begin(), src.items.end(), [](const Source::Item &a, const Source::Item &b) { return a.file < b.file; });
        return true;
    }
    mz_zip_archive z;
    if (!openZip(z, src.path)) { err = "cannot open the zip: " + src.path; return false; }
    src.zip = true;
    const mz_uint n = mz_zip_reader_get_num_files(&z);
    for (mz_uint i = 0; i < n; i++) {
        if (mz_zip_reader_is_file_a_directory(&z, i)) continue;
        char name[1024];
        mz_zip_reader_get_filename(&z, i, name, sizeof(name));
        std::string s = name;
        const size_t slash = s.find_last_of("/\\");
        src.items.push_back({slash == std::string::npos ? s : s.substr(slash + 1), i, {}});
    }
    mz_zip_reader_end(&z);
    return true;
}

uint32_t prevPow2(uint32_t v) { uint32_t p = 1; while (p * 2 <= v) p *= 2; return p; }

// 2x2 box filter to the next level (power-of-two sizes; a 1-wide axis stays 1).
std::vector<uint8_t> halve(const std::vector<uint8_t> &src, uint32_t w, uint32_t h)
{
    const uint32_t nw = std::max(1u, w / 2), nh = std::max(1u, h / 2);
    std::vector<uint8_t> dst(size_t(nw) * nh * 4);
    for (uint32_t y = 0; y < nh; y++)
        for (uint32_t x = 0; x < nw; x++) {
            const uint32_t x0 = std::min(w - 1, x * 2), x1 = std::min(w - 1, x * 2 + 1);
            const uint32_t y0 = std::min(h - 1, y * 2), y1 = std::min(h - 1, y * 2 + 1);
            const uint8_t *p[4] = {&src[(size_t(y0) * w + x0) * 4], &src[(size_t(y0) * w + x1) * 4],
                                   &src[(size_t(y1) * w + x0) * 4], &src[(size_t(y1) * w + x1) * 4]};
            // Alpha-weighted colour so fully transparent texels do not darken the edges of cut-outs.
            uint32_t a = p[0][3] + p[1][3] + p[2][3] + p[3][3];
            uint8_t *d = &dst[(size_t(y) * nw + x) * 4];
            for (int c = 0; c < 3; c++) {
                uint32_t s = 0, plain = 0;
                for (int k = 0; k < 4; k++) { s += p[k][c] * p[k][3]; plain += p[k][c]; }
                d[c] = uint8_t(a ? (s + a / 2) / a : (plain + 2) / 4);
            }
            d[3] = uint8_t((a + 2) / 4);
        }
    return dst;
}

using EncodeFn = bool (*)(const uint8_t *rgba, uint32_t w, uint32_t h, uint8_t *out);

// BC7 with bc7enc (modes 1, 5, 6, 7, perceptual weights); levels smaller than 4x4 repeat their last texel.
bool encodeBc7(const uint8_t *rgba, uint32_t w, uint32_t h, uint8_t *out)
{
    bc7enc_compress_block_params params;
    bc7enc_compress_block_params_init(&params);
    // Games alpha-test against the GS's 0x80 (opaque): a few units of BC7 alpha error on opaque texels punch holes
    // (seen on the HUD orb and the Graveyard leaves), so alpha weighs far more than colour.
    params.m_weights[3] = 1024;
    uint8_t block[16 * 4];
    for (uint32_t by = 0; by < (h + 3) / 4; by++)
        for (uint32_t bx = 0; bx < (w + 3) / 4; bx++) {
            for (uint32_t y = 0; y < 4; y++)
                for (uint32_t x = 0; x < 4; x++) {
                    const uint32_t sx = std::min(w - 1, bx * 4 + x), sy = std::min(h - 1, by * 4 + y);
                    std::memcpy(block + (y * 4 + x) * 4, rgba + (size_t(sy) * w + sx) * 4, 4);
                }
            bc7enc_compress_block(out, block, &params);
            out += 16;
        }
    return true;
}
EncodeFn g_bc7 = encodeBc7;

struct Result {
    size_t item = 0;
    Name name;
    std::string skip; // non-empty: not used, and why
    uint32_t w = 0, h = 0, srcW = 0, srcH = 0;
    uint8_t levels = 0, alphaMin = 255, alphaMax = 0;
    std::vector<uint8_t> data;
};

Result convertOne(const std::vector<uint8_t> &file, const Name &name, uint32_t cap, uint8_t format)
{
    Result r;
    r.name = name;
    int w = 0, h = 0, comp = 0;
    stbi_uc *px = stbi_load_from_memory(file.data(), int(file.size()), &w, &h, &comp, 4);
    if (!px) { r.skip = std::string("PNG decode failed: ") + stbi_failure_reason(); return r; }
    r.srcW = uint32_t(w); r.srcH = uint32_t(h);
    // A PNG without alpha (grey or RGB) is GS alpha 0x80 (1.0), as in PCSX2; stb_image would give 255 (2.0).
    if (comp == 1 || comp == 3)
        for (size_t i = 3; i < size_t(w) * h * 4; i += 4) px[i] = 0x80;
    for (size_t i = 3; i < size_t(w) * h * 4; i += 4) {
        r.alphaMin = std::min(r.alphaMin, px[i]); r.alphaMax = std::max(r.alphaMax, px[i]);
        // Near-opaque / near-clear (upscaler noise) become exact, so they stay exact through BC7 and alpha tests.
        if (px[i] >= 248) px[i] = 255;
        else if (px[i] <= 4) px[i] = 0;
    }
    // Largest power of two not above the image (never upscaled), longest side at most the cap, aspect kept.
    uint32_t tw = prevPow2(uint32_t(w)), th = prevPow2(uint32_t(h));
    while (std::max(tw, th) > cap) { tw = std::max(1u, tw / 2); th = std::max(1u, th / 2); }
    std::vector<uint8_t> level(size_t(tw) * th * 4);
    if (tw == uint32_t(w) && th == uint32_t(h))
        std::memcpy(level.data(), px, level.size());
    else
        stbir_resize(px, w, h, 0, level.data(), int(tw), int(th), 0, STBIR_RGBA, STBIR_TYPE_UINT8, STBIR_EDGE_WRAP,
                     STBIR_FILTER_MITCHELL);
    stbi_image_free(px);
    r.w = tw; r.h = th;
    uint32_t lw = tw, lh = th;
    for (;;) {
        const size_t at = r.data.size();
        r.data.resize(at + size_t(levelBytes(format, lw, lh)));
        if (format == FormatBC7) {
            if (!g_bc7 || !g_bc7(level.data(), lw, lh, r.data.data() + at)) { r.skip = "BC7 encoding failed"; r.data.clear(); return r; }
        } else {
            std::memcpy(r.data.data() + at, level.data(), level.size());
        }
        r.levels++;
        if (lw == 1 && lh == 1) break;
        level = halve(level, lw, lh);
        lw = std::max(1u, lw / 2); lh = std::max(1u, lh / 2);
    }
    return r;
}

void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    std::vprintf(fmt, ap);
    va_end(ap);
    std::fflush(stdout);
}

} // namespace

int convertMain(int argc, char **argv)
{
    // argv[0] = "--hdtex-convert"
    if (argc < 3) {
        say("HDTEX error usage: --hdtex-convert <pack.zip|folder> <output folder> [--cap N] [--format rgba8|bc7] [--threads N]\n");
        return 2;
    }
    Source src;
    src.path = argv[1];
    const fs::path outDir = fs::u8path(argv[2]);
    uint32_t cap = 2048;
    uint8_t format = g_bc7 ? FormatBC7 : FormatRGBA8;
    unsigned threads = std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
    for (int i = 3; i + 1 < argc; i += 2) {
        const std::string k = argv[i], v = argv[i + 1];
        if (k == "--cap") cap = std::max(16u, std::min(8192u, prevPow2(uint32_t(std::atoi(v.c_str())))));
        else if (k == "--format") format = v == "bc7" ? FormatBC7 : FormatRGBA8;
        else if (k == "--threads") threads = std::max(1u, std::min(64u, unsigned(std::atoi(v.c_str()))));
    }
    if (format == FormatBC7 && !g_bc7) { say("HDTEX error this build has no BC7 encoder\n"); return 2; }
    if (format == FormatBC7) bc7enc_compress_block_init();

    std::string err;
    if (!listSource(src, err)) { say("HDTEX error %s\n", err.c_str()); return 1; }

    // Names first: what is used, in pack order; the first file with a name wins (as in PCSX2).
    std::vector<std::string> skipReason(src.items.size());
    std::vector<Name> names(src.items.size());
    std::vector<size_t> work;
    std::map<std::tuple<uint64_t, uint64_t, uint32_t>, size_t> seen;
    for (size_t i = 0; i < src.items.size(); i++) {
        const std::string &fn = src.items[i].name;
        if (fn == "desktop.ini" || fn == "Thumbs.db" || fn == ".DS_Store") { skipReason[i] = "-"; continue; }
        skipReason[i] = parseName(fn, names[i]);
        if (!skipReason[i].empty()) continue;
        auto key = std::make_tuple(names[i].tex, names[i].clut, names[i].bits);
        if (auto it = seen.find(key); it != seen.end()) { skipReason[i] = "same texture as " + src.items[it->second].name; continue; }
        seen[key] = i;
        work.push_back(i);
    }
    say("HDTEX files %zu\n", work.size());

    std::error_code ec;
    fs::create_directories(outDir, ec);
    const fs::path tmp = outDir / "pack.mhtx.tmp", final = outDir / "pack.mhtx";
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) { say("HDTEX error cannot write %s\n", u8(tmp).c_str()); return 1; }
    Header hdr{kMagic, kVersion, 0, 0, 0, 0};
    out.write(reinterpret_cast<const char *>(&hdr), sizeof(hdr));
    uint64_t pos = sizeof(hdr);

    // Workers decode and scale; this thread writes in completion order. At most 2 results per worker wait in memory.
    std::mutex m;
    std::condition_variable cvDone, cvRoom;
    std::deque<Result> done;
    std::atomic<size_t> next{0};
    const size_t maxQueued = size_t(threads) * 2;
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < threads; t++)
        pool.emplace_back([&] {
            mz_zip_archive z;
            bool zipOk = src.zip && openZip(z, src.path);
            for (size_t w; (w = next.fetch_add(1)) < work.size();) {
                const size_t idx = work[w];
                Result r;
                std::vector<uint8_t> bytes;
                if (src.zip) {
                    size_t sz = 0;
                    void *p = zipOk ? mz_zip_reader_extract_to_heap(&z, src.items[idx].zipIndex, &sz, 0) : nullptr;
                    if (p) { bytes.assign(static_cast<uint8_t *>(p), static_cast<uint8_t *>(p) + sz); mz_free(p); }
                } else {
                    std::ifstream f(src.items[idx].file, std::ios::binary);
                    bytes.assign(std::istreambuf_iterator<char>(f), {});
                }
                if (bytes.empty()) { r.skip = "cannot read the file"; r.name = names[idx]; }
                else r = convertOne(bytes, names[idx], cap, format);
                r.item = idx;
                std::unique_lock<std::mutex> lk(m);
                cvRoom.wait(lk, [&] { return done.size() < maxQueued; });
                done.push_back(std::move(r));
                cvDone.notify_one();
            }
            if (zipOk) mz_zip_reader_end(&z);
        });

    std::vector<Entry> index;
    std::vector<std::string> notes(src.items.size());
    for (size_t finished = 0; finished < work.size(); finished++) {
        Result r;
        {
            std::unique_lock<std::mutex> lk(m);
            cvDone.wait(lk, [&] { return !done.empty(); });
            r = std::move(done.front());
            done.pop_front();
            cvRoom.notify_one();
        }
        if (!r.skip.empty()) {
            skipReason[r.item] = r.skip;
        } else {
            Entry e{};
            e.tex = r.name.tex; e.clut = r.name.clut; e.bits = r.name.bits;
            e.width = uint16_t(r.w); e.height = uint16_t(r.h); e.format = format; e.levels = r.levels;
            e.flags = r.alphaMax - r.alphaMin <= 8 ? EntryFlatAlpha : 0;
            e.alphaMin = r.alphaMin; e.alphaMax = r.alphaMax;
            e.offset = pos; e.size = r.data.size();
            out.write(reinterpret_cast<const char *>(r.data.data()), std::streamsize(r.data.size()));
            pos += r.data.size();
            index.push_back(e);
            char buf[96];
            std::snprintf(buf, sizeof(buf), "%ux%u -> %ux%u, %u mips, alpha %u-%u", r.srcW, r.srcH, r.w, r.h, r.levels, r.alphaMin, r.alphaMax);
            notes[r.item] = buf;
        }
        say("HDTEX progress %zu %zu\n", finished + 1, work.size());
    }
    for (auto &t : pool) t.join();

    std::sort(index.begin(), index.end(), keyLess);
    hdr.count = uint32_t(index.size());
    hdr.indexOffset = pos;
    out.write(reinterpret_cast<const char *>(index.data()), std::streamsize(index.size() * sizeof(Entry)));
    out.seekp(0);
    out.write(reinterpret_cast<const char *>(&hdr), sizeof(hdr));
    out.close();
    if (!out) { say("HDTEX error writing the pack failed (disk full?)\n"); fs::remove(tmp, ec); return 1; }
    fs::remove(final, ec);
    fs::rename(tmp, final, ec);
    if (ec) { say("HDTEX error cannot rename the pack: %s\n", ec.message().c_str()); return 1; }

    size_t skipped = 0;
    {
        std::ofstream log(outDir / "convert_log.txt", std::ios::trunc);
        log << "Source: " << src.path << "\nCap: " << cap << " px, format " << (format == FormatBC7 ? "BC7" : "RGBA8") << "\n\n";
        for (size_t i = 0; i < src.items.size(); i++) {
            if (skipReason[i] == "-") continue;
            if (skipReason[i].empty()) log << "used     " << src.items[i].name << "  " << notes[i] << "\n";
            else { log << "SKIPPED  " << src.items[i].name << "  " << skipReason[i] << "\n"; skipped++; }
        }
    }
    {
        std::ofstream info(outDir / "pack.txt", std::ios::trunc);
        info << "source=" << src.path << "\ncap=" << cap << "\nformat=" << (format == FormatBC7 ? "bc7" : "rgba8")
             << "\ntextures=" << index.size() << "\nskipped=" << skipped << "\nbytes=" << pos + index.size() * sizeof(Entry) << "\n";
    }
    say("HDTEX done used=%zu skipped=%zu bytes=%llu\n", index.size(), skipped,
        (unsigned long long)(pos + index.size() * sizeof(Entry)));
    return 0;
}

} // namespace hdtex

#ifdef HDTEX_STANDALONE
int main(int argc, char **argv) { return hdtex::convertMain(argc - 1, argv + 1); }
#endif
