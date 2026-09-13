# Odyssey Player 3D Internal Wiki

This repository is the current software basis for full-screen glasses-free 3D
playback on Immersity/LeiaSR displays. This wiki is the practical runbook for
developers and QA working on runtime behavior, media handling, and player
delivery.

## What the product does

Odyssey opens SBS and MVC media, schedules playback to audio time, renders stereo
frames as two views, and sends the composed pair to the Immersity weaver. The UI is
native Win32 and mouse-first, including track and caption controls.

Fallback behavior is intentional:

- If stereo output is unavailable or focus conditions are not met, the player
  keeps presenting a valid 2D mode rather than failing.
- If an ISO cannot be mounted or decoded as MVC, playback remains a normal SBS
  path failure.

## Top-level architecture

### Process entry

- `src/main.cpp` parses command-line modes and creates a single `AppShell` host.
- Modes include smoke entry points, spike checks, and full interactive playback.

### Host and window layer

- `src/app/AppShell.cpp` drives application state:
  - media open/close lifecycle
  - track switching
  - pause/seek/stop commands
  - error surfacing and rendering state transitions
  - SR runtime eligibility checks and recovery
- `src/app/Win32Window.cpp` owns the native surface and input handling.

### Rendering stack

- `src/app/D3D11Device.cpp` owns the shared Direct3D 11 device/context.
- `src/app/ImmersityWeaver.cpp` owns SR runtime loading and stereo session
  management.
- `src/app/StereoLayout.cpp` computes left/right viewport rectangles, aspect
  correction, and sample clamping per layout mode.
- `src/app/StereoCompositor.cpp` renders both eyes, overlays captions and UI, then
  hands the frame to the weaver.

### Media and timing

- `src/app/VideoPipeline.cpp` owns FFmpeg decode/pipeline worker wiring for SBS
  inputs.
- `src/app/AudioOutput.cpp` owns WASAPI shared-mode output and clock extraction.
- `src/app/MvcPipeline.cpp` integrates direct read-only ISO playback through the
  MVC adapter.
- `src/app/VideoTiming.h` and queue helpers determine frame eligibility, drop
  policy, and late/early frame handling.

## Media flow overview

1. Open request is accepted from the UI, command line, or drag/drop.
2. AppShell resolves SBS vs MVC path and starts the selected pipeline.
3. Pipeline workers decode media into bounded queues with explicit timestamps.
4. Scheduler checks frame age against decoded clock and selects a due frame.
5. Video and captions are composed into the per-eye render targets.
6. Immersity weaver applies monitor-tied stereo output if conditions allow.
7. Playback state is rendered into both eyes and updated on input/clock events.

## Feature map

### Implemented

- Native file picker, drag/drop, and direct `--play` path.
- Play/pause/stop, seek, and generation-safe cancellation.
- Full/half SBS layout plus eye swap and 2D mode override.
- ISO title discovery and playlist-driven selection.
- Mouse transport panel with auto-hide and caption/audio menus.
- Captions:
  - external UTF-8 SRT
  - embedded SubRip
  - PGS bitmap (Blu-ray)

### Under active acceptance

- 30-minute drift measurement and synchronized seek-recovery proof are in
  ongoing acceptance scope.
- Optical stereo quality and audible lip-sync checks are manual physical checks.

## Control summary (UI)

From the control strip:

- `OPEN` opens native picker.
- `PLAY/PAUSE`, `STOP`.
- Scrubber and click-to-seek.
- `AUDIO` selector with labeled track list.
- `CAPTIONS` selector with Off and offset.
- `LAYOUT` selector (Auto/Full-SBS/Half-SBS/2D).
- `SWAP EYES`.
- `FULL`, `MIN`, `CLOSE`.

Keyboard accelerators are intentionally minimal and can be added only if they do not
compete with normal media transport behavior.

## Verification workflow

### Automated

- CMake build and unit tests run via:

```powershell
cmake --preset windows-debug
cmake --build --preset windows-debug
ctest --preset windows-debug --output-on-failure --interactive-debug-mode 1
```

- Full player media smoke path uses the real SBS fixture env var:

```powershell
$env:ODYSSEY_TEST_M2_FSBS_MKV = 'H:\3D\The.Lord.of.the.Rings.The.Fellowship.of.the.Ring.3D_2001_.Full-SBS.1080p.x264.AC3.ENG-JFC.mkv'
ctest --preset windows-debug -R odyssey-m2 --output-on-failure --interactive-debug-mode 1
```

- Long-duration and advanced acceptance evidence is tracked in
  `docs/superpowers/plans/2026-09-12-fullscreen-player-rebuild.md`.

### Manual / local

- Use a visible desktop and local mouse control.
- Confirm controls, subtitle overlays, and transport behavior live.
- Confirm stereo geometry on target monitor and listen for sync.

## Known folder map

- Source: `src/app`
- Tests: `tests`
- Internal design: `docs/superpowers/`
- UI scenarios: `tests/ui/scenarios/`
- Media probe notes: `tools/mvc_probe/README.md`
- Test corpus: `docs/test-corpus.md`

## Deployment and safety notes

- Do not copy Immersity runtime DLLs into the executable directory. Use local
  installed runtime location.
- ISO playback uses read-only mount semantics and must not detach pre-existing user
  mounts.
- Keep any manual UI verification local and visible; API-driven paid verification
  is not used for this code path.
- Build outputs and downloaded probe artifacts remain ignored outside source.

## Current operational defaults

- Foreground-sensitive tests should be run on an interactive desktop.
- Media playback should start from normal OS focus and remain in explicit fullscreen
  for the target monitor when stereo is active.

