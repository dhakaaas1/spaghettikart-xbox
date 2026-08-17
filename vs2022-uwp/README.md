# SpaghettiKart UWP (Xbox Developer Mode)

This is an unofficial UWP port of [SpaghettiKart](https://github.com/HarbourMasters/SpaghettiKart),
adapted from [SternXD/2ship2harkinian-uwp](https://github.com/SternXD/2ship2harkinian-uwp)'s
UWP fork of [2 Ship 2 Harkinian](https://github.com/HarbourMasters/2ship2harkinian). It includes
no copyrighted game assets; a legally obtained Mario Kart 64 ROM is required.

## How this is built

Unlike a normal UWP app, this is a **two-stage build**, not a single MSBuild project:

1. CMake builds the game as `Spaghettify.dll` (not an .exe) with `-DSPAGHETTIKART_UWP=ON`,
   which defines `_UWP`, links a prebuilt WinRT-safe SDL2 in place of vcpkg's desktop
   build, and links `libuwp` (CoreWindow/refresh-rate/screen-size glue).
2. `uwp/uwp.vcxproj`, a hand-written AppContainer host `.exe` project (**not** driven by
   CMake), links that DLL as a prebuilt dependency, packages it plus `config.yml`,
   `yamls/`, `meta/`, and `spaghetti.o2r` (built from this repo's own `assets/` folder --
   no ROM involved, safe to build in CI) into an MSIX.

See `.github/workflows/uwp-compile.yml` for the exact commands (CMake configure/build,
then `msbuild uwp.vcxproj`) run on a `windows-latest` GitHub Actions runner.

## What's NOT built here

`mk64.o2r` -- the ROM-derived asset archive -- is **never** generated in CI and never
committed to this repo. On first launch, the boot menu (`uwp/src/bootmenu.cpp`) prompts
for a storage drive (D:\ or E:\, whichever Xbox Dev Mode exposes and grants access to),
then lets you pick your own ROM file and extracts it in-process via
`GameExtractor::SpaghettiKart_ExtractRom` (linked into the game DLL through the existing
Torch/Companion pipeline SpaghettiKart already uses for desktop's "Generate O2R" flow).

The MK64 Reloaded 4K texture pack works the same way: it's a `.o2r` mod file, dropped
into `<drive>:\SpaghettiKart\mods\` after install (same folder `ModManager::ListMods()`
scans on desktop, just rooted on the aux drive here since the package install directory
is read-only under UWP -- see `src/engine/mods/ModManager.cpp`). It is not bundled into
the MSIX either.

## Building locally in Visual Studio (Windows only)

```powershell
cmake -S . -B build\x64 -G "Visual Studio 17 2022" -T v143 -A x64 -DCMAKE_BUILD_TYPE=Release -DSPAGHETTIKART_UWP=ON
cmake --build build\x64 --config Release --target Spaghettify
cmake --build build\x64 --config Release --target GenerateO2R
```

Then open `vs2022-uwp\SpaghettiKart.slnx` in Visual Studio 2022 and build/deploy
`uwp.vcxproj` (x64, Release). It expects the CMake output above to already exist at
`build\x64\Release\`.
