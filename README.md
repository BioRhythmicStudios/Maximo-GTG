# Maximo-GTG

<img width="2559" height="1439" alt="github shot" src="https://github.com/user-attachments/assets/2f0f4e37-8f4a-40ad-8169-e8fdebcbb734" />


An unofficial native Windows port of **Maximo: Ghosts to Glory** (PAL, SLES-50703), built on your own PC from your own
disc. It uses static recompilation: the game's code is translated to C++ and compiled, with no emulator in between.

**This repository contains no game code or data, and no copy of the game.** You need your own legally obtained disc.
Never share the `generated`, `build` or `dist` folders: they contain the game's code.

## Get started

1. Download `MaximoLauncher.zip` from the [Releases page](https://github.com/BioRhythmicStudios/Maximo-GTG/releases) and
   unzip it to a folder with a short path, such as `C:\Games\Maximo`. Keep the `art` folder beside the launcher.
2. Open `MaximoLauncher.exe` and press **Build**.
3. Choose your disc image. The launcher checks it against the PAL release, opens it read-only and never changes it.
4. When the build finishes, press **Play**.

The launcher downloads what it needs: the source code, the libraries, FFmpeg for faster movies, and Git if your PC doesn't
have it. If your PC has no C++ compiler, it offers to install Microsoft's free C++ Build Tools, and Windows will ask for your
permission. The game itself is never downloaded: it is compiled on your PC from your own disc, which can take a while.

## If Windows or your antivirus warns you

The launcher is unsigned and new, so some security tools are cautious about it. Its source is in this repository, and it
is built with the C# compiler that ships with Windows.

- **SmartScreen** may say Windows protected your PC. Choose **More info**, then **Run anyway**, if you trust this download.
- **Antivirus:** a few scanners may flag the launcher through general rules, because it downloads tools and starts a build.
  Only download it from the Releases page above. If you are unsure, read the source or build it yourself.
- **Smart App Control** blocks unsigned programs, so it stops the launcher, and probably the game built on your PC. Find it
  under Windows Security, App & browser control. Read Microsoft's notes before you change it, because it may be hard to
  turn back on. Your PC is your call: if you would rather keep it on, this port won't run for now.

## What you need

- Windows 10 or 11, 64-bit, and a CPU with AVX2
- A graphics card with Vulkan
- A disc image of the PAL release of Maximo: Ghosts to Glory (`.iso`)
- About 12 GB of free disk space, plus about 8 GB if the C++ Build Tools need installing

To update the game, open **Game Files** and press **Update and rebuild the game**.

## Build it by hand

You need [Git](https://git-scm.com/) and Visual Studio with **Desktop development with C++**, including **C++ CMake tools
for Windows**. Visual Studio 2026 is the tested version.

```bat
git clone https://github.com/BioRhythmicStudios/Maximo-GTG.git
cd Maximo-GTG
build.cmd "C:\path\to\your\Maximo.iso"
```

Then open `dist\MaximoLauncher.exe`. Add `--no-ffmpeg` to skip FFmpeg; the game then uses its own movie decoder.

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
