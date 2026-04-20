# Odyssey Player 3D — MVP Implementation Plan

> **For agentic workers:** This is a milestone-level build plan, not a task-by-task checklist. Each milestone has a concrete "run the exe and verify" success criterion. Retire milestones in the stated order — the ordering is risk-first, not feature-first.

**Goal:** Ship a Windows-only, glasses-free 3D media player that plays Blu-ray 3D ISOs and stereo MKVs on a Samsung Odyssey 3D G9 via the Immersity SDK weaver.

**Architecture:** Single-process C++17 app. UI/main thread owns the D3D11 device, window, Immersity weaver, and present. A decode thread runs FFmpeg and hands GPU textures to a stereo compositor that produces a left/right pair; the weaver consumes the pair. Audio runs on the WASAPI callback thread. State is held in a plain `PlayerState` struct mutated only from the main thread; the decode thread communicates via a bounded frame queue.

**Tech Stack:** C++17, MSVC x64, CMake + vcpkg, Direct3D 11, FFmpeg (LGPL), libbluray, Immersity SDK (DX11 backend), WASAPI, Dear ImGui (DX11 + Win32 backend).

---

## 1. Milestone Ladder

### M0 — Build scaffolding
**Goal:** Launch an empty fullscreen-capable window with a D3D11 device and a clear color.
**In:** Git repo layout, `CMakeLists.txt`, vcpkg manifest (`vcpkg.json`), Immersity SDK found via `find_package` or a vendored `ImmersityConfig.cmake` that points at the local SDK install, a single `odyssey.exe` target, a `Win32Window` wrapping `CreateWindowExW` + `WM_` pump, a `D3D11Device` creating `ID3D11Device` / `ID3D11DeviceContext` / `IDXGISwapChain` (flip-discard, 2 buffers, sRGB back buffer), ESC-to-exit, Alt-Enter fullscreen toggle.
**Success (runnable):** `odyssey.exe` opens, window fills the primary monitor, clears to `#0A0A0A`, 60+ FPS present, Alt-Enter toggles borderless fullscreen, ESC exits cleanly with zero `ID3D11Debug` live-object leaks.
**Depends on:** Immersity SDK installed at a known path (headers + import libs + runtime DLLs).
**Risks retired:** none yet — this is the foundation.

### M1 — Weaver spike (retires Risk 1)
**Goal:** Prove Immersity SDK weaver accepts a D3D11 texture and outputs lenticular weave fullscreen on the Odyssey G9.
**In:** A hardcoded test path. Load a 3840x1080 full-SBS PNG from disk via WIC into a single `ID3D11Texture2D` (DXGI_FORMAT_R8G8B8A8_UNORM_SRGB). Wrap the Immersity weaver init exactly as in `d:\Sources\Immersity-Demos\demos\dx11_cube\main.cpp`. Each frame: set the SBS texture as the weaver input, call weave, present.
**Success (runnable):** With Odyssey G9 connected and SR service running, `odyssey.exe --spike path\to\test_sbs.png` shows a correctly-woven lenticular image at the native panel resolution. The 3D-status readout (printed once to stdout) reports the equivalent of `ACTIVE`. Leaving the binary running for 60 s produces zero D3D validation errors in a debug build.
**Depends on:** M0, physical Odyssey G9 + SR service installed, one known-good SBS test still (a screenshot grabbed from a full-SBS MKV will do).
**Risks retired:** #1 (Immersity ↔ D3D11 texture interop contract). If this fails, the stack is wrong before anything else is built.

### M2 — FFmpeg decode into the SBS target
**Goal:** Replace the hardcoded PNG with a live H.264 full-SBS MKV decode.
**In:** FFmpeg demux (`avformat_open_input`, `av_read_frame`), H.264 decode with D3D11VA hardware acceleration (`AV_HWDEVICE_TYPE_D3D11VA`, hwframes context shared with the app's `ID3D11Device`), fallback to software (`AV_PIX_FMT_YUV420P`) with a CPU->GPU upload path. A tiny `VideoPipeline` that owns the decode thread + a single-slot "latest decoded frame" handoff (not a full queue yet). On the main thread, bind the decoded NV12/RGBA texture as the weaver input via a YUV->RGB shader into the SBS render target.
**Success (runnable):** `odyssey.exe F:\3D\<full-sbs-mkv>` plays the file front-to-back at native rate with weaver output. Pressing ESC exits cleanly. A debug overlay (cout is fine for now) prints decoded FPS and shows no sustained queue-full events for 60 s of playback.
**Depends on:** M1, FFmpeg built/installed via vcpkg with the decoder set below.
**Risks retired:** D3D11VA ↔ Immersity zero-copy path. Partially retires #4 (the full-SBS case is the easy one).

### M3 — Stereo compositor
**Goal:** Accept any source layout and produce a normalized left-right pair for the weaver.
**In:** A `StereoCompositor` that, given (decoded texture, source format enum {FullSBS, HalfSBS, TAB, Mono2D}, params {convergence, parallax, swapLR}), renders into two views of an internal `L_R` SBS render target. Full-pixel shader pass with four branches. Convergence = horizontal pixel offset per eye, parallax = uniform scale of that offset, swap = exchange viewports. 2D passes the same image to both eyes.
**Success (runnable):** Hardcode the source format via a CLI flag `--format fsbs|hsbs|tab|2d` and hardcode convergence/parallax/swap via `--conv N --par N --swap`. For each of four test files (one per format), the woven output is correct. Convergence nudges produce a visible depth shift without restarting the pipeline.
**Depends on:** M2.
**Risks retired:** the compositor is the seam where live tuning happens; proving it now prevents a rewrite in M11.

### M4 — Input router + transport + seek + speed
**Goal:** Keyboard-driven playback control, no chrome yet.
**In:** A `PlayerStateMachine` with states {Stopped, Playing, Paused, Seeking, Ended} and a pure `Transport` API (play, pause, seek(delta), stepFrame, setSpeed). Hotkeys: Space (play/pause), Left/Right (-5/+5s), Shift+Left/Right (-60/+60s), `,`/`.` (prev/next frame when paused), `[`/`]` (speed down/up across 0.5/1/2x), `+`/`-` (volume, stubbed for now since no audio), `M` (mute, stubbed). Seek path calls `av_seek_frame` on the nearest keyframe then decodes-and-drops to the requested PTS.
**Success (runnable):** Playing a 2-hour MKV, every hotkey performs the documented action. `Shift+Right` ten times lands within ±1 s of the expected PTS. Frame-step advances exactly one displayed frame per keypress. Speed 2x sustains without queue stalls; speed 0.5x displays each frame twice (no true slow-motion interpolation needed for MVP).
**Depends on:** M2.

### M5 — UI layer
**Goal:** Minimal on-screen chrome matching the Swiss-minimal spec.
**In:** Dear ImGui (DX11 + Win32 backend) loaded next to the app, custom font (Inter Tight TTF embedded) at UI-scale-aware sizes, and a separate bitmap-font renderer for the `ODY-Pixel-8` wordmark (4x nearest-neighbor). Screens: boot splash (2 s on launch), top title bar (48 px at 4K), bottom HUD (112 px at 4K) with scrubber + time + play/pause glyph + 3D-status LED, transient toast (center, 1.2 s fade), error banner (top, persistent until dismissed). Auto-hide: after 2.5 s of no mouse motion during `Playing` state, fade chrome out; any mouse motion or state transition to `Paused` reveals it.
**Success (runnable):** Launch, watch splash, open a file via hotkey `O` (native picker), confirm title bar shows filename + duration, scrubber tracks playback, clicking scrubber seeks within ±1 s, 2.5 s of idle hides the HUD, mouse wiggle restores it, convergence keypress shows a tuning toast with the numeric value.
**Depends on:** M4. Font asset sourced at this milestone (Inter Tight from Google Fonts, OFL).

### M6 — File-open path + session state
**Goal:** Open files three ways; restore last session.
**In:** Drag-and-drop via `RegisterDragDrop` + `IDropTarget`, native picker via `IFileDialog`, command-line argv[1]. A `SessionState` serializer (JSON, `%APPDATA%\OdysseyPlayer3D\session.json` OR next-to-exe `session.json` — pick next-to-exe per the "portable config" requirement) storing `{lastPath, lastPositionSeconds, lastFormat, lastConv, lastPar, lastSwap}`. On startup, if `session.json` exists and the file still exists on disk, show a toast `Resume at HH:MM:SS? [Enter]/[Esc]` for 4 s; Enter restores, Esc starts from 0.
**Success (runnable):** Close mid-playback at 00:27:13, relaunch, confirm toast appears with `00:27:13`, Enter resumes within ±2 s of that mark with the same stereo settings.
**Depends on:** M4 (so resume has a transport API to call), M5 (for the toast).

### M7 — 2D preview fallback (retires Risk 2)
**Goal:** The app is usable without the Odyssey + SR service.
**In:** On Immersity weaver init failure (or at runtime if the SR status downgrades to `UNAVAILABLE`), fall back to a plain non-woven presentation: render the left half of the decoded SBS into the window, letterboxed, with a fixed banner `SR SERVICE UNAVAILABLE — 2D PREVIEW`. All transport + UI remain functional.
**Success (runnable):** On a dev machine with no Odyssey connected, `odyssey.exe F:\3D\<full-sbs-mkv>` still opens, plays, seeks, and shows the 2D banner. On the Odyssey machine with SR running, no banner; full weave.
**Depends on:** M5. This milestone unlocks dev iteration without the target hardware.

### M8 — libbluray ISO playback
**Goal:** Accept `.iso` and reach a 3D m2ts stream.
**In:** `libbluray` opened against the ISO path (libbluray supports `.iso` directly on Windows). Enumerate titles, pick the title flagged as 3D (`BLURAY_TITLE_FLAGS_3D` or equivalent). Within that title pick the main playlist (longest duration). Feed the m2ts byte stream into FFmpeg via a custom `AVIOContext` backed by `bd_read`. Still hardcode the stereo format to `MVC` (even though we can't decode it yet) for these files.
**Success (runnable):** `odyssey.exe F:\3D\<one-known-iso>` opens the ISO, picks a 3D playlist without user input, begins streaming the m2ts into FFmpeg. Decode will fail at the MVC stream (M9's job), but the demux pipeline proves out — verify by logging picked title index, duration, and the first 10 `av_read_frame` packet sizes/PIDs.
**Depends on:** M2 (FFmpeg demux/decode already integrated).

### M9 — MVC decode path (retires Risk 5)
**Goal:** Decode MVC frame-packed Blu-ray 3D into a left/right pair the compositor already handles.
**In:** Driven by the research sub-plan in Section 5 below. Whatever option survives, the result is: given an MVC m2ts from M8, produce two `ID3D11Texture2D` views per frame (left + right) and route them into the compositor as a new source format `MVC`. If all options fail, ship the fallback banner `MVC detected — please pre-convert to Full-SBS MKV`.
**Success (runnable):** `odyssey.exe F:\3D\<known-mvc-iso>` plays with correct stereo on the Odyssey. OR, if the fallback ladder is triggered, the binary displays the pre-convert message instead of crashing, and playing the same title's pre-converted SBS MKV works.
**Depends on:** M8. This is the largest single unknown.

### M10 — Audio + A/V sync
**Goal:** Synchronized audio playback.
**In:** WASAPI shared-mode client behind an `IAudioBackend` interface (one method set: open, start, stop, write, getClockPositionSeconds, close). Resample to the mix format via libswresample. A/V sync is master-audio: video frames display-or-drop to track the audio clock within ±40 ms. Volume + mute implemented here.
**Success (runnable):** Lip-sync stays visually correct over a 30-minute playback. `+`/`-` change volume audibly; `M` mutes instantly. Seeks resync within 500 ms.
**Depends on:** M4 (transport), M2 (decode).

### M11 — Format auto-detect + hotkey cycle (partially retires Risk 4)
**Goal:** Users don't have to think about format.
**In:** Detection stack (in order): (a) Blu-ray title flag (definitive for ISOs), (b) container tags (`stereo_mode` in Matroska), (c) filename heuristics — regex set matching `\b(SBS|HSBS|Half-SBS|Full-SBS|TAB|OU|3D)\b` with case-insensitive rules, (d) aspect-ratio sanity check (≥3.2:1 ⇒ full-SBS, ~1.78:1 with `3D` in name ⇒ half-SBS). Expose a single hotkey `F` that cycles `auto → FullSBS → HalfSBS → TAB → Mono2D`. A compact format indicator in the top-right of the title bar (e.g. `AUTO · FSBS`).
**Success (runnable):** For each of the 4 MVP smoke files (Section 6), opening with no override picks the correct format. Pressing `F` cycles visibly and the image changes as expected.
**Depends on:** M3, M5, M6, M8.

### M12 — Polish + config + about + ship errors
**Goal:** MVP-shippable.
**In:** A portable `config.json` next to exe (keybinds, default convergence, HUD opacity, auto-hide delay) read at startup, written on graceful exit. An About surface (hotkey `F1`) listing build hash (injected via CMake `configure_file`), version, FFmpeg + Immersity SDK versions (queried at runtime where APIs exist, else baked at build). Ship-quality error messages for: unsupported file, codec not found, SR service lost mid-playback, MVC detected-but-failed. All placeholder `printf` + `OutputDebugString` calls removed or gated behind a debug flag.
**Success (runnable):** Fresh Windows VM, drop the build folder on the desktop, double-click exe, every MVP scenario runs. About screen shows real version strings. Unplugging the Odyssey mid-playback swaps to the 2D banner within 2 s; reconnecting restores weave within 2 s of SR reporting ready.
**Depends on:** all prior.

---

## 2. Subsystem Boundaries

**Video pipeline.** Owns the FFmpeg contexts, D3D11VA hwframes, the decode thread, and a single-slot "latest frame" handoff (bounded queue of 2 frames is sufficient for MVP). Consumes a file path + seek requests. Emits frame-ready events with a GPU texture handle + PTS + source format hints. Owns no UI state. The single-slot design is deliberate — the display clock is audio-driven, so holding a deep queue gains nothing.

**Stereo compositor.** Owns the SBS render target and the four shader permutations (FullSBS, HalfSBS, TAB, Mono2D). Consumes {source texture, source format, tuning params}. Emits the woven-ready SBS texture. Owns only GPU resources; no timing, no file state.

**3D presentation (weaver wrapper).** Thin C++ wrapper around the Immersity SDK DX11 API. Owns the weaver handle. Consumes the compositor's SBS texture + the back buffer. Emits a synthesized `SrStatus` enum (`Active`, `DegradedFrameLatency`, `DegradedTrackingLost`, `Unavailable`) polled once per frame. Everything SDK-specific stays inside this subsystem.

**UI layer.** Owns ImGui context, fonts, the pixel-font renderer, HUD layout, auto-hide timer, toast queue, error banner state. Consumes `PlayerState` (read-only snapshot) and `SrStatus`. Emits user intents (`SeekRequested`, `TransportRequested`, `OpenFileRequested`, `FormatCycled`, `TuningChanged`) to the input router.

**Input router.** Owns keyboard + mouse state + drag-drop target + command-line args. Consumes raw Win32 messages. Emits the same intent events as the UI layer. Single place where a key-chord-to-intent table lives — UI layer and router share this table so the About screen can render it.

**Audio.** Owns the WASAPI client + resampler. Consumes decoded audio packets from the video pipeline's demuxer (audio stream shares the same `AVFormatContext`; a second decode thread is *not* needed — decode audio on the audio callback thread with a small lock-free ring buffer from demux). Emits a monotonic audio clock in seconds, which the video pipeline reads to decide display-or-drop.

**Session state.** Owns the resume record. Consumes `{path, position, format, tuning}` on every pause/exit. Emits a restore offer on startup. Plain JSON blob, written atomically (write-temp + rename).

**App shell.** Owns the window, the D3D11 device, the frame loop, and the wiring between subsystems. Nothing else. When a subsystem needs another, the shell hands it the reference at construction.

---

## 3. Threading Model

- **UI/main thread:** window pump, D3D11 immediate context, weaver, present, ImGui, input routing, session state.
- **Decode thread:** single thread running `avcodec_send_packet` / `avcodec_receive_frame`. Hands off the latest decoded GPU texture to the main thread via a `std::mutex` + `std::condition_variable` guarding a 2-slot ring. Reads `seekRequested` atomic.
- **Audio thread:** WASAPI event-driven callback. Decodes audio packets (pulled from a lock-free ring fed by the demuxer side of the decode thread) and writes the mix buffer. Publishes the audio clock via an `std::atomic<int64_t>` in sample units.
- **No background I/O thread for MVP.** The demuxer blocks on `av_read_frame`; `libbluray` reads are fast enough from SSD/HDD that decoupling is Phase 2.

**MVP reconsideration:** folding the demux + decode onto a single thread (rather than splitting demux and decode) is the right call for MVP. A separate demux thread is only worth it once we need seekless audio-ahead or chapter scrubbing.

---

## 4. Dependency Procurement

- **FFmpeg** — vcpkg `ffmpeg[avcodec,avformat,avfilter,avdevice,swresample,swscale]`. Explicitly **no** `[gpl]`, **no** `[x264]`, **no** `[x265]` — LGPL-only. Enabled demuxers: `matroska`, `mpegts`, `mov`. Enabled decoders: `h264`, `aac`, `ac3`, `eac3`, `dts`, `truehd`. Hardware accel: `d3d11va`. Dynamic link (`--enable-shared` equivalent via vcpkg triplet `x64-windows`) so LGPL dynamic-link compliance is automatic.
- **libbluray** — vcpkg `libbluray`. LGPL-2.1. Dynamic link. No AACS/BD+ (commercial discs encrypted with AACS are out of scope for MVP — ISOs in the test corpus are assumed decrypted rips).
- **Immersity SDK** — vendored. A `cmake/FindImmersity.cmake` module points at the SDK install via an `IMMERSITY_SDK_ROOT` env var (default `C:\Program Files\Immersity SDK`). Headers and import libs linked; runtime DLLs copied next to `odyssey.exe` as a post-build step. Reference integration: `d:\Sources\Immersity-Demos\demos\dx11_cube\main.cpp`.
- **Dear ImGui** — git submodule at `third_party/imgui`, pinned to a tagged release. `imgui_impl_dx11.cpp` + `imgui_impl_win32.cpp` used as-is. MIT license.
- **Inter Tight font** — vendored TTF under `assets/fonts/`. SIL OFL.
- **WIL / wrl** — Windows SDK only, no extra dep.
- **JSON** — vcpkg `nlohmann-json`. MIT.

**UI choice + justification:** Dear ImGui. The UI is ~6 surfaces (splash, HUD, title, toast, banner, about), it's developer-tool-grade chrome, and ImGui has a first-class DX11 backend that shares our device. Direct2D + DirectWrite would be fine but costs a second text renderer and a second swapchain pattern for no gain. A hand-rolled sprite batcher is out — the delta from ImGui isn't worth the weeks. The `ODY-Pixel-8` wordmark is intentionally *not* drawn through ImGui — it's a dedicated 1-shader, 1-vertex-buffer sprite path so the pixels stay crisp at 4x integer scale.

---

## 5. MVC Research Sub-Plan

Execute before writing any M9 code. Time-boxed to 3 days.

**Option A — FFmpeg MVC fork.** Evaluate the `jeeb/mvc` and related community branches. Criteria: builds cleanly against the same FFmpeg version vcpkg gives us; decodes a known MVC m2ts to two output streams; integrates with D3D11VA hwframes (likely not — MVC patches are usually software-only).

**Option B — Custom NAL-level demux + dual H.264 decode.** Split the MVC base + dependent views at the NAL unit level (prefix NAL 14/20, subset SPS), feed the base view to a stock H.264 decoder and the dependent view to a second decoder instance configured to accept the subset SPS. Criteria: two time-correlated decoded streams emerge; latency ≤1 frame; no codec ABI surprises.

**Option C — Nvidia Video Codec SDK.** NVDEC has documented MVC support on older driver branches; verify current driver retains it. Criteria: Nvidia-only (a hard limit — acceptable if we document the requirement for MVP), decodes directly to a D3D11 texture via `CUDA/D3D11 interop`, two-view output in one call.

**Option D — Offline transcode requirement.** Ship a small helper `odyssey-convert.exe` (or document an `ffmpeg` command line) that transmuxes MVC to full-SBS MKV. User runs it once per disc. Zero runtime complexity.

**Decision criteria (in order):** correctness on the test corpus → hardware-accelerated → runtime-only (no offline step) → LGPL-compatible → not Nvidia-locked.

**Fallback ladder:**
1. Option C on the dev Nvidia machine, if the box is Nvidia and the driver works.
2. Option B if C fails or the user is on AMD/Intel.
3. Option A if B's dual-decoder sync is unreliable.
4. Option D as last resort: the app detects MVC, refuses playback, and shows `MVC Blu-ray 3D detected — this build requires pre-conversion. Run 'odyssey-convert.exe <iso>' or see <url>.`

We commit to fallback D shipping no matter what — it's the safety net that makes M9 unblockable.

---

## 6. Testability Strategy

**MVP smoke corpus** (four files, all under `F:\3D\`):

1. `F:\3D\<a full-SBS MKV, ~1080p>` — M2/M3/M5/M11 primary.
2. `F:\3D\<the VR ultra-wide SBS MKV>` — aspect-ratio edge case for M11 auto-detect.
3. `F:\3D\<one Blu-ray 3D ISO known to be AVC half-SBS>` — M8 demux + M3 half-SBS path (this exists on some 3D discs as an alternate presentation).
4. `F:\3D\<one Blu-ray 3D ISO that is MVC frame-packed>` — M9 stress test.

Select the exact four filenames at the start of M2 and record them in `docs/test-corpus.md`. Don't change them mid-project — regression comparisons depend on stability.

**Per-milestone "done looks like":** each milestone's success criterion in Section 1 already states a runnable, observable outcome. Keep a `docs/smoke-tests.md` with one paragraph per milestone describing the exact launch command, expected on-screen state, and how to fail it. Run the full smoke sheet before merging any milestone.

**Automated tests:** unit tests only where the input is trivially mockable — filename-heuristic parser (M11), session JSON round-trip (M6), stereo-format cycle state (M11), transport state machine transitions (M4). Everything GPU-adjacent is verified by eye on hardware. Do not invest in a GPU-image-diff harness for MVP.

---

## 7. Explicit Non-Goals for MVP

The following are deliberately out. A reviewer who sees these in a PR should reject it:

- HEVC decode.
- Recents/library/playlists/queue/watch-history.
- Subtitles (any kind — SRT, PGS, embedded).
- HDR (SDR only; 8-bit output).
- Track-selection UI (audio or subtitle).
- Format-override menu beyond the single `F` hotkey cycle.
- Stereo photos / screenshots / A-B loop / chapter menus.
- Graphical settings UI — config is file-only.
- Multi-monitor UX beyond "fullscreen on the primary display."
- Network streams (no http/rtsp/udp inputs).
- Encrypted Blu-ray (AACS/BD+) support.
- Localization — English only.
- Accessibility features beyond stock Windows behavior.

---

## 8. Open Questions Blocking Plan Execution

Only questions that change milestone ordering or scope:

1. **Do we have an Nvidia GPU on the dev/target box?** This single answer determines whether Option C enters the MVC fallback ladder at position 1 or is dropped. Blocks M9 start.
2. **Are the test ISOs in `F:\3D\` already AACS-decrypted rips, or raw disc images?** If encrypted, we either require libaacs + keys (out of MVP scope) or restrict the test corpus. Blocks M8 success definition.
3. **Does the Immersity SDK expose a "SR service status" query that distinguishes `DegradedFrameLatency` vs `DegradedTrackingLost`, or do we synthesize these from a single "degraded" bit?** Changes the 3D-status LED design in M5. Non-blocking for M1 but must resolve before M5.
4. **Target Windows version floor — Windows 10 22H2 or Windows 11 only?** D3D11VA + WASAPI work on both; the question is whether we test on Windows 10 at all. Affects QA scope in M12, not milestone content.

Everything else (font licensing, ImGui version pin, config file name) is decided in this document and does not block execution.
