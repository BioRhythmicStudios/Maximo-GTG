// HD texture replacement at run time (HD_TEXTURES.md): reads the packs made by --hdtex-convert, recognises the
// game's textures by their PCSX2 hashes as paraLLEl-GS caches them, loads the replacements on a background thread
// and hands them to paraLLEl-GS. Opt-in: MAXIMO_HDTEX_PACKS=<pack folder or pack.mhtx>[;...] (first = highest priority).
#pragma once
#include <memory>

namespace Vulkan { class Device; }
namespace ParallelGS { class GSInterface; }

namespace hdtex {

class Runtime
{
public:
    // Null when MAXIMO_HDTEX_PACKS is unset or no pack could be opened.
    static std::unique_ptr<Runtime> fromEnv();
    ~Runtime();
    // Registers with paraLLEl-GS; call on the thread that drives it. initialVram = the GS VRAM right now (4 MiB).
    void attach(ParallelGS::GSInterface &gs, Vulkan::Device &device, const void *initialVram);
    // Once per presented frame, on the same thread: refreshes the upload budget, logs progress now and then.
    void endFrame(unsigned long long guestFrame);

    struct Impl;
private:
    explicit Runtime(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl;
};

} // namespace hdtex
