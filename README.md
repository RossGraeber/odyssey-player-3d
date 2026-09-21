# Odyssey Player 3D

Odyssey is a native Windows 3D movie player for Side-by-Side (SBS) files and
Blu-ray MVC ISO media on Immersity/LeiaSR displays. It is tuned for the Samsung
G90XF workflow and keeps a visible, native Windows UI with mouse transport
controls.

The current implementation supports:

- Full-SBS and Half-SBS playback.
- Direct ISO opening for MVC titles, with read-only attachment and playlist
  selection.
- Timestamped playback scheduling with pause, seek, and audio track switching.
- Audio output through WASAPI shared mode with volume/mute.
- External UTF-8 SRT, embedded SubRip, and Blu-ray PGS captions.
- Native fullscreen and 2D fallback when stereo conditions are not met.

The player renders both movie and controls to both eyes, applies Immersity weaving,
and verifies window and runtime conditions before enabling stereo output.

## Download and install

Grab `Odyssey Player 3D-<version>-win64.exe` from the
[Releases page](https://github.com/RossGraeber/odyssey-player-3d/releases) and run it.
The installer ships `odyssey.exe`, the FFmpeg DLLs and the VC++ runtime; the
LeiaSR Platform runtime must be installed separately. Install and usage notes,
including troubleshooting, live in the
[project wiki](https://github.com/RossGraeber/odyssey-player-3d/wiki).

## Keyboard and mouse

Hovering any control shows its description and shortcut in the panel header.

| Action | Shortcut |
| --- | --- |
| Play/pause | Space / K |
| Stop | S |
| Seek 5 s (Shift 30 s, Ctrl 60 s) | Left / Right |
| Restart | Home |
| Volume 5% | Up / Down or mouse wheel |
| Mute | M |
| Fullscreen | F / F11 / Alt+Enter / double-click |
| Leave fullscreen, or quit when windowed | Esc |
| Quit | Ctrl+Q |
| Open | O |
| Audio track | A |
| Captions menu (not yet available; shortcut reserved) | C |
| Layout | L |
| Swap eyes | E |
| Blu-ray title | I |

## Prerequisites

- Windows x64 and [Visual Studio 2026](https://visualstudio.microsoft.com/downloads/)
  C++ Desktop workload matching `CMakePresets.json`.
- [CMake 3.21+](https://cmake.org/download/), [Git](https://git-scm.com/download/win)
  with submodules, [7-Zip](https://www.7-zip.org/download.html) on `PATH`.
- Immersity SDK headers/libraries (`include\sr` and `lib`) via `IMMERSITY_SDK_ROOT`.
  Get it from [immersity.ai/developers](https://immersity.ai/developers) →
  [SDK portal](https://support.immersity.ai/sdk) (account required).
- LeiaSR runtime installed locally. On a Samsung Odyssey 3D it comes with
  [Samsung Odyssey 3D Hub](https://apps.microsoft.com/detail/xp8lx6slrzb813)
  (Microsoft Store). Odyssey loads runtime DLLs from
  `%ProgramFiles%\LeiaSR\Platform\bin` or `ODYSSEY_IMMERSITY_RUNTIME_DIR`.
- Packaging only: [NSIS 3](https://nsis.sourceforge.io/Download); optionally
  [WiX 4](https://wixtoolset.org/docs/intro/) for an MSI.

Full dependency table with links: [wiki › Building](https://github.com/RossGraeber/odyssey-player-3d/wiki/Building).

Prepare vcpkg and the private MVC dependency staging before build:

```powershell
git submodule update --init --recursive
.\third_party\vcpkg\bootstrap-vcpkg.bat -disableMetrics
.\third_party\vcpkg\vcpkg.exe install `
  --triplet x64-windows `
  --x-manifest-root=. `
  --x-install-root=.\vcpkg_installed
.\tools\bootstrap-private-deps.ps1
```

The bootstrap verifies pinned private archives and stages `build\mvc_probe\deps\lav-0.83`.
It does not register filters globally, install runtime components, or copy SDK DLLs
beside the executable.

## Build and verification

```powershell
$env:IMMERSITY_SDK_ROOT = 'D:\path\to\LeiaSR-SDK'
cmake --preset windows-debug
cmake --build --preset windows-debug
ctest --preset windows-debug --output-on-failure --interactive-debug-mode 1
```

Packaging must use the Release preset: the debug CRT is not redistributable, so Debug builds are not packageable.

To generate a Windows installer (NSIS required):

```powershell
cmake --preset windows-release
cmake --build --preset windows-release
cpack -G NSIS -C Release --config build/windows-release/CPackConfig.cmake
```

The installer uses the custom icon from `src/app/odyssey.ico` (Windows XP-style anaglyph glasses).

The installed app still requires the LeiaSR / Immersity Platform runtime to be installed on the
target machine: `SimulatedReality*.dll` and `opencv_world343.dll` are delay-loaded from it, not
shipped in the MSI.

To generate an MSI installer (WiX required, install once with
`dotnet tool install wix --tool-path .dotnet-tools`, then put it on PATH):
The default CPack template also needs the UI extension once per machine:
`.dotnet-tools\wix.exe extension add WixToolset.UI.wixext -g`.

```powershell
cmake --preset windows-release
cmake --build --preset windows-release
$env:PATH = "$PWD\.dotnet-tools;$env:PATH"
cpack -G WIX -C Release --config build/windows-release/CPackConfig.cmake
```

Unit tests are part of the default preset. Full player smoke entries use a visible
desktop and real window activation.

```powershell
$env:ODYSSEY_TEST_M2_FSBS_MKV = 'H:\3D\The.Lord.of.the.Rings.The.Fellowship.of.the.Ring.3D_2001_.Full-SBS.1080p.x264.AC3.ENG-JFC.mkv'
ctest --preset windows-debug --output-on-failure --interactive-debug-mode 1
```

## Command-line entry points

```text
Usage: odyssey.exe [--smoke-test]
                   [--spike <path>]
                   [--spike-smoke <path>]
                   [--play <path>]
                   [--play-smoke [path]]
                   [--player-smoke <path> [seconds]]

Run without arguments for interactive playback.
```

- `--smoke-test` launches the built-in startup smoke.
- `--spike <path>` and `--spike-smoke <path>` run spike checks.
- `--play <path>` opens a media file directly.
- `--play-smoke [path]` runs bounded playback with either an explicit path or
  `ODYSSEY_TEST_M2_FSBS_MKV`.
- `--player-smoke <path> [seconds]` opens the full player loop for a short
  bounded run.

## Supported media

- SBS video containers: MKV and MP4, including full and half SBS frame packing.
- Blu-ray MVC ISO images and supported title selection.
- Subtitles:
  - External UTF-8 SRT.
  - Embedded SubRip.
  - Blu-ray PGS bitmaps.

Current limitations:

- ASS/SSA, WebVTT, and non-PGS bitmap subtitle formats are not accepted.
- Full Blu-ray menu/BD-J support is not included.
- ISO playback remains read-only and title-driven.

## Runtime behavior and controls

The bottom control strip includes open, play/pause, stop, volume/mute, audio
selection, captions selection, layout control, eye swap, fullscreen, and close.
The panel and cursor auto-hide during playback, and resume on mouse movement.
Captions are composed before weaving and clipped to the visible 3D content area.

## Documentation

- User and build guide: [GitHub wiki](https://github.com/RossGraeber/odyssey-player-3d/wiki)
- Internal documentation and architecture notes: [`docs/wiki.md`](docs/wiki.md)
- Test corpus and expected checks: [`docs/test-corpus.md`](docs/test-corpus.md)
- Smoke run expectations: [`docs/smoke-tests.md`](docs/smoke-tests.md)
- MVC feasibility details: [`tools/mvc_probe/README.md`](tools/mvc_probe/README.md)

