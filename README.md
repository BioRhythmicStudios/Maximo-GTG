# Maximo-GTG

An unofficial native Windows port of **Maximo: Ghosts to Glory** (PAL, SLES-50703), built on your own PC from your own
disc. It uses static recompilation: the game's code is translated to C++ and compiled, with no emulator in between.

**This repository contains no game code or data, and no copy of the game.** You need your own legally obtained disc.
Never share the `generated`, `build` or `dist` folders: they contain the game's code.

## What you need

- Windows 10 or 11, 64-bit, and a CPU with AVX2
- A graphics card with Vulkan
- [Git](https://git-scm.com/)
- Visual Studio with the **Desktop development with C++** workload, including **C++ CMake tools for Windows**.
  Visual Studio 2026 is the tested version; the free Community edition is fine.
- A disc image of the PAL release of Maximo: Ghosts to Glory (`.iso`), or a folder with the disc's files
- About 10 GB of free disk space, and a good while for the first build

## Build it

```bat
git clone https://github.com/BioRhythmicStudios/Maximo-GTG.git
cd Maximo-GTG
build.cmd "C:\path\to\your\Maximo.iso"
```

The build downloads its source dependencies, then checks your disc against the PAL release. It opens the disc read-only
and never changes it. When it finishes, open `dist\MaximoLauncher.exe` and choose your disc under **Game Files**. Then
press **Play**.

The build downloads an LGPL build of FFmpeg, which makes the movies faster. Add `--no-ffmpeg` to skip it; the game then
uses its own movie decoder.

The launcher is also offered as a download on the Releases page, but it can't play anything until you have built the
game here. The game itself is never offered as a download.

## How it works

1. `tools\kit\maximo_kit.cpp` reads the game's executable from your disc and verifies it.
2. PS2Recomp recompiles the executable using the function map in `data\funcs.csv`.
3. The VU1 microcode is assembled from your executable by `data\vu1\recipe.txt` and compiled ahead of time.
4. Everything is compiled against the runtime in `external\PS2Recomp` and the paraLLEl-GS renderer in
   `external\parallel-gs`.

## In the game

| Key | Action |
| --- | --- |
| WASD | Left stick |
| X, Z, C, V | Cross, Square, Circle, Triangle |
| Q, E, 1, 3 | L1, R1, L2, R2 |
| Enter | Start |
| F12 | Screenshot for the launcher's banner |
| Alt+Enter | Fullscreen or window |
| Esc | Quit |

Controllers work too: plug yours in before pressing Play. The launcher's banner is built from your own disc and your
F12 screenshots, on your PC only.

## HD textures

The launcher can install Marcus' Ultimate Remaster texture pack from archive.org (9.7 GB), or your own PCSX2-format pack.

## Credits

- **Biorhythmic Studios:** port
- **Santins:** testing
- **Marcus of "Maximo Capcom Videos":** owner and creator of the HD textures
- Built with [PS2Recomp](https://github.com/ran-j/PS2Recomp),
  [paraLLEl-GS](https://github.com/Arntzen-Software/parallel-gs) and the projects in `THIRD_PARTY_NOTICES.txt`

## Legal

Unofficial fan project. Not affiliated with or endorsed by Capcom. All rights to Maximo: Ghosts to Glory are reserved to
Capcom.

The code in this repository is licensed under the GNU General Public License v3.0, see `LICENSE`. The launcher's art is
by Biorhythmic Studios and is included under the same licence.
