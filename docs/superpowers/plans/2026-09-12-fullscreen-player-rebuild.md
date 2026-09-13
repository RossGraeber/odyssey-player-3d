# Fullscreen movie player implementation plan

> Current delegation: Luna implements; Sol reviews implementation and coordinates task follow-through. The parent owns planning and final review. This replaces the initial Sol implementation assignment at the user's request; completed work is preserved.

**Goal:** Play SBS files and direct Blu-ray MVC ISOs on the G90XF with correct stereo, synchronized audio, and mouse-operated pixel-art controls.

**Architecture:** Retain the existing Win32/D3D11/FFmpeg application. Establish a proven MVC decoder first. Normalize both SBS and MVC sources to timestamped eye views, schedule against consumed audio, then composite the movie and controls before Immersity weaving.

**Tech stack:** C++17, MSVC, D3D11, existing FFmpeg, WASAPI, Win32 text/input/file dialogs, installed Immersity runtime. Disc reader and MVC runtime are approved for the feasibility proof; pin the route demonstrated to work.

**Approved design:** `docs/superpowers/specs/2026-09-12-fullscreen-sbs-player-design.md`. The user approved proceeding after direct ISO/MVC was added. This sequence supersedes the conflicting ordering and non-goals in the April MVP plan. UI verification uses normal local computer control and never spends API credits.

## 1. Prove direct ISO/MVC decoding

Original feasibility ownership: Sol (completed). Further implementation belongs to Luna, with Sol review. Keep probe builds independent of the app targets.

- [x] Inventory actual decoder/runtime availability. Downloaded pinned LAV Filters 0.83 x64 and `libmfxsw64-v3`, with archive SHA-256 values recorded in `tools/mvc_probe/README.md`. Private filters load without registration.
- [x] Establish access to both AVC and dependent MVC data from the Batman Begins ISO. LAV opens the read-only mounted `BDMV/index.bdmv` as AMVC and explicitly enables paired samples. The image contains one playlist and one SSIF; native ISO-path opening fails. The wrapper detached its mount after the successful run.
- [x] Write a runnable private LAV graph probe exposing paired NV12 frames through `IMediaSample3D`, using the legacy software MVC runtime.
- [x] Have the probe return nonzero for missing dependent data, broken shared-sample timestamps, no frame progress, or sustained decode slower than the source. Every sample must have both buffers explicitly enabled by LAV. Decoder failure produces failed connection/run, insufficient frames, or timeout. Independent decoder-internal frame-order validation is not available through this interface.
- [x] Decode 60 seconds at natural source timestamps. The reviewed Batman rerun produced 1,440 paired 1920x1080 samples, PTS 0 to 600182889 (100 ns units), 1,375 distinct pairs, and 99.046 fps. External eye IDs/frame-order metadata are unavailable; both views share one LAV sample PTS.
- [x] Seek within both actual ISOs and repeat the pair checks. Big Hero playlist `00800.mpls`, seek 3090 seconds, produced 1440/1440 distinct MVC pairs over 60.018 seconds at 92.132 fps, crossing independently verified primary-clip boundaries at 3098.762333 and 3114.402956 seconds with no timestamp gaps or regressions.
- [x] Parent reviewed ownership, sample enable/reset, active-plane hashing, strict paired delivery, timestamp checks, and read-only mount cleanup; independently reran positive and negative controls. Adopt the private LAV 0.83/software MVC route for production integration. This closes decoder feasibility only, not the player milestone.

Parent's initial real-disc run produced 1,440 paired 1920x1080 NV12 samples over 60.018 seconds in both phases, with no missing timestamps, regressions, or gaps. After ownership fixes, the independent rerun passed at 99.046 fps initially and 88.795 fps after seeking to 1,800 seconds, against a 23.976 fps source. Every sample had both views. The ordinary Fellowship SBS negative control returned exit 23 with zero MVC pairs. Shared-sample timestamps are observable and segment-relative after seeking; decoder-internal view IDs and frame order are not independently exposed. Big Hero also passed initial, seek, and known clip-transition checks; detailed evidence is in `tools/mvc_probe/README.md`.

Baseline inspection command, which currently exposes only the AVC base view:

```powershell
ffprobe -v warning -probesize 10000000 -analyzeduration 5000000 -show_entries 'format=duration:stream=index,codec_type,codec_name,profile,width,height' -of json 'bluray:H:/3D/Batman_Begins__2005__Remastered_MVC_1080p_dgc.iso'
```

If the first candidate fails, preserve the failing result and test the next concrete candidate. Do not accept an offline conversion workflow as successful completion. Record remaining decoder uncertainty explicitly before selecting production integration.

For production ISO ownership, use Windows' native Virtual Disk API if its local proof succeeds: open ISO read-only, retain the attachment handle for playback, locate the corresponding mounted volume, and release only an attachment created by this player. Do not set permanent lifetime or detach a pre-existing user mount. Microsoft's [attachment flags](https://learn.microsoft.com/en-us/windows/win32/api/virtdisk/ne-virtdisk-attach_virtual_disk_flag) require read-only for ISO; absent permanent lifetime, closing the last attachment handle releases it. [GetVirtualDiskPhysicalPath](https://learn.microsoft.com/en-us/windows/win32/api/virtdisk/nf-virtdisk-getvirtualdiskphysicalpath) provides the device identity for volume matching. Keep graph/file handles closed before releasing the mount, and make mount discovery cancellable.

Parent independently ran the native `IsoMount` proof on both Batman and Big Hero: read-only attachment exposed `BDMV/index.bdmv`, a second opener reused the volume, and closing the owning handle detached it. A separate pre-mounted Batman test verified that both helper objects preserved the existing mount; the test's outer cleanup then released its own fixture. The first implementation's `GET_VIRTUAL_DISK_INFO_IS_LOADED` query returned `ERROR_INVALID_PARAMETER` for ISO and was replaced with physical-device probing. The retained harness lives under ignored `build/iso_mount_proof/`. Production graph integration subsequently passed on both ISOs; normal desktop-token access remains to be verified.

## 2. Correct stereo output and SDK ownership

Files: `src/app/ImmersityWeaver.{h,cpp}`, `Win32Window.{h,cpp}`, `D3D11Device.{h,cpp}`, `Nv12ToRgba.{h,cpp}`, `AppShell.{h,cpp}`, `src/main.cpp`, `cmake/FindImmersity.cmake`, `CMakeLists.txt`; add a focused stereo compositor and geometry tests if the existing converter cannot cleanly own the pass.

- [x] Add failing geometry checks for full-SBS 3840×1080, cropped full-SBS 3840×804, half-SBS 1280×720, non-square pixels, swapped views, and each eye's independent sample bounds.
- [x] Implement and parent-review pure geometric layout in `StereoLayout.h`. Eight tests cover source crops, aspect restoration, letterboxing, pillarboxing, sample aspect ratio, eye swap, mono duplication, and invalid dimensions. Later renderer and corpus checks verify sampling, clamping, and integration.
- [x] Initialize DPI awareness and the SR context before shared-device decode. Discover the SR monitor location and recommended view dimensions. Select the corresponding DXGI output/adapter and create a borderless window with physically matching swap-chain dimensions.
- [x] Pass actual frame format and color metadata to conversion. Preserve source resolution and avoid sampling across eye boundaries. Handle software frames explicitly instead of treating their data pointers as D3D textures.
- [x] Clamp chroma sampling within each source eye during YUV conversion as well as RGBA composition: later RGBA clamping cannot undo chroma already mixed across the seam. Verify limited-range chroma expansion (255/224 for 8-bit) rather than copying the prototype's incomplete BT.709 conversion.
- [x] Compose aspect-correct views into the SDK input dimensions. Fill unused areas black. Draw nothing and apply no scaling after weaving.
- [x] Correct `frameWeave` to pass per-eye width (`sbsWidth / 2`) to `setInputViewTexture`. The installed official DX11 stereo-image example explicitly does this at lines 1155-1163. Preserve full SBS dimensions in caller-facing methods; reject malformed odd/zero dimensions at that boundary. Parent reviewed; full CTest suite passed against the real SBS fixture, with no skips. Optical acceptance remains separate.
- [x] Replace unsynchronized event flags with thread-safe state. Handle unavailable/restored/invalid context, focus, minimize, and windowed preview. Release lens hints whenever exclusively 2D content or native dialogs are shown.
- [x] Resolve runtime libraries from the installed runtime, following SDK guidelines. Remove the build's copy-runtime behavior without deleting arbitrary DLLs.
- [x] Parent reviews resource lifetimes, callback/thread ownership, device selection, resize behavior, and the verified texture contract.

`StereoCompositor` is parent-reviewed and its WARP readback test independently passes: separate eye viewports, black bars, swapped eyes, half-texel source clamping, and encoded-color preservation through sRGB input/RTV/output. It clears inherited geometry/tessellation shaders and rejects incompatible SRV types. Full-SBS playback and M2 smoke compose from actual frame dimensions/SAR into cached SDK per-eye dimensions using physical display aspect. Later strict-gate corpus runs verify fullscreen output selection on the installed monitor/runtime.

Frame-aware conversion is parent-reviewed: hardware NV12 and uploaded software NV12/P010 use a GPU pass with source color matrix/range and independent eye chroma bounds; planar 8/10-bit YUV uses per-eye swscale. Hardware P010 uses bounded transfer followed by the P010 upload path. Unsupported wide-gamut/HDR input is rejected explicitly. Parent independently passed all 25 conversion/compositor/layout tests after the upload changes. Earlier full CTest runs with Fellowship had no skips, and the generated Main10 fixture passed hardware-transfer and forced-software modes. Optical acceptance remains separate.

Measured main-thread conversion cost justified retaining the GPU path. The all-CPU approach took median 18.435 ms / p99 32.583 ms for Fellowship and 14.089 / 26.922 ms for cropped Avengers. The corrected GPU path took median 0.060 / p99 0.141 ms and 0.058 / 0.128 ms respectively across 220 post-warmup frames; these measure CPU submission cost, not GPU execution duration. Both real 240-frame smoke runs passed. `ODYSSEY_BENCHMARK_CONVERTER=1` enables that extended check; normal smoke remains 60 frames. `ODYSSEY_FORCE_SOFTWARE_DECODE=1` exercises the software callsite.

Releasing the application context mutex before `Present` initially hung M2. A controlled comparison with the same decoder passed with `Present` inside that lock. Enabling device `ID3D10Multithread` protection, as FFmpeg's own device-creation path does, resolved the hang while allowing the application mutex to be released before v-sync. Our manually supplied device bypassed FFmpeg's creation step. The real-fixture full CTest suite then passed 4/4. Keep both device protection and the application mutex around multi-call render sequences.

The bounded startup/thread-safety patch is parent-reviewed: `runPlay` initializes the weaver before decoder startup; callback invalidation and diagnostic counters are atomic; the event stream unsubscribes before callback state is destroyed. Application build and all four CTest entries passed with the real Fellowship fixture and no skips. Later strict-gate corpus runs verify display placement, lens state, and full rendering; runtime-loss recovery is covered by focused state checks.

Geometry examples: 3840×1080 full-SBS produces two 1920×1080 eyes; 3840×804 produces two 1920×804 eyes. Half-SBS 1280×720 contains two 640×720 stored views whose displayed aspect ratio is restored to 16:9 when sample aspect is 1:1.

## 3. Timestamped playback with audio

Files: `VideoPipeline.{h,cpp}`, `AppShell.{h,cpp}`, `FrameMailbox.h`, `PtsMath.h`, existing tests; add concrete `AudioOutput.{h,cpp}` and small scheduling/queue helpers only as needed. Integrate the proven MVC adapter and disc reader here.

- [x] Write deterministic scheduling checks: early frame retained, due frame displayed, obsolete frame dropped, 24000/1001 cadence preserved, pause freezes media time, seek invalidates old frames, EOF drains before ending.
- [x] Implement and parent-review the pure timestamp decision helper in `VideoTiming.h`: early/due/late, exact fractional cadence, negative timestamps, unknown duration, and overflow boundaries. Seven tests pass; later pipeline and corpus checks verify pause/seek generation and EOF integration.
- [x] Replace latest-frame overwriting with bounded ordered delivery. Keep ownership explicit and cancellation wakeable. Protect counters read across threads.
- [x] Implement and review the bounded FIFO foundation, `FrameQueue.h`. Four tests cover order, backpressure, close cancellation/drain, and clear/deletion. Incoming ownership remains guarded across allocation failure. The production pipeline now uses bounded delivery with seek-generation invalidation.
- [x] Use exception-safe FFmpeg ownership, check allocations/return codes, distinguish corruption from EOF, and ignore attached pictures when selecting video.
- [x] Parent reviewed constructor unwind, allocation checks, attached-picture exclusion, same-packet retry after EAGAIN, explicit terminal status/error, and shutdown interrupt callback. Parent independently ran the retained short-video harness: invalid-input rejection and clean `Drained` EOF with 24 decoded frames. Teardown explicitly frees queued frames before the D3D mutex or decoder resources can be destroyed. The integrated player surfaces terminal errors while preserving those ownership guarantees.
- [x] Decode selected audio, resample/channel-map to the actual endpoint mix format, and feed event-driven WASAPI. Implement volume/mute and recoverable endpoint errors.
- [x] Implement and review the caller-thread-owned `AudioOutput` WASAPI foundation. Parent's silence-only endpoint smoke verified clock progress, a frozen clock after 100 ms paused, reset to a new 5-second PTS anchor, prefill-before-start, NaN-volume rejection, and safe overflow reporting. It uses `IAudioClock::GetFrequency` units and native endpoint format. Later playback checks verify FFmpeg resampling, clock accounting, track changes, and recoverable endpoint handling. Reproducible harness is ignored under `build/audio_smoke/`.
- [x] Derive media time from consumed audio frames plus timestamp anchoring and buffered/resampler delay. Use a monotonic fallback only for files without audio or an explicitly selected silent mode.
- [x] If using `IAudioClock::GetPosition`, divide by `GetFrequency`, not the mix sample rate: position units are not necessarily sample frames. Its correlated QPC value is in 100 ns units. Create/release the audio clock on its owning audio thread and publish a synchronized clock snapshot to the renderer. See [Microsoft's clock contract](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclock-getposition).
- [x] Add a command path for pause, seek, stop, audio change, and open. Serialize FFmpeg access on its owner thread. Flush decoder/resampler/endpoint state together on seek. Preserve paused state across track changes.
- [x] Keep D3D locks outside disk I/O, queue waits, and v-sync. Continue weaving the retained frame at display refresh while the movie frame remains unchanged.
- [x] Feed ISO playlist transitions and MVC pairs into the same timeline. Ensure input cancellation permits close/open during buffering.
- [x] Parent checks locking, bounded memory, constructor failure cleanup, EOF/error distinction, audio clock accounting, and seek races before UI integration.

Run the focused unit suite and source-linked tests after each logical change. Verify representative corpus decoding in both hardware and forced-software modes. Use small generated fixtures for missing pixel formats and color-range cases.

## 4. Complete mouse interface and captions

Files: `Win32Window.{h,cpp}`, `AppShell.{h,cpp}`, `src/main.cpp`; add a focused `PlayerControls` renderer/input unit and subtitle unit if necessary. Reuse D3D resources and native text rendering; no UI framework dependency.

Host integration decisions:

- Route normal startup and `--play` through one persistent player loop. Keep existing smoke entry points focused. A concrete session wrapper may dispatch to the two actual backends; avoid a general plugin system.
- Open SBS input asynchronously with cancellation available during format probing, not only after construction. Keep the D3D device alive until opening and all decode workers have stopped. MVC graph construction already belongs to its COM owner thread.
- Discover the SDK monitor before decode. If the initial device uses the wrong adapter, tear down the weaver and recreate the device on the matching adapter before opening media. Keep the healthy session across file changes.
- Enable the lens only with a valid stereo frame, exact native fullscreen client dimensions, matching adapter, foreground focus, no minimize, and no native dialog. Otherwise show a labelled single-eye preview. Use a bounded retry interval for unavailable SDK state.
- Keep a pending timestamped frame plus its generation in the host. Seek and track changes invalidate that frame and the displayed captions immediately. Convert/compose only newly due frames, then weave the retained output at display refresh.
- Map the physical control panel rectangle into each eye before weaving. In 2D preview, fit the movie to the current client aspect instead of stretching a monitor-aspect intermediate. Mouse coordinates remain physical client coordinates.

- [x] Implement startup Open Movie, native Unicode picker, drop-file handling, and positional CLI path. Cancel restores the previous playback state.
- [x] Render the approved charcoal/amber/green pixel panel into both eyes at identical screen-depth coordinates. Use readable Unicode labels and integer-scaled pixel geometry.
- [x] Implement mouse play/pause/stop, captured scrubber drag, volume/mute, audio menu, captions menu, layout override, eye swap, fullscreen, minimize, and close. Include actual selected track/title labels.
- [x] Add ISO title selection by duration/label without full disc menus.
- [x] Decode/render embedded SubRip, external UTF-8 SRT, and ISO PGS with Off/timing offset. Test cue start/end, overlapping cues, seek flush, and malformed input handling.
- [x] Auto-hide controls/cursor after 2.5 seconds only while playing and no control/menu/drag is active. Restore on mouse movement. Errors remain readable.
- [x] Parent reviews mouse state transitions, coordinate mapping, captions before weave, Unicode, pointer capture release, and error recovery.

## 5. Acceptance and milestone close

### Reviewed backend and host evidence

Latest caption fix: native mouse verification on SHA-256 `771985A7A6361E7E4B53DC8F455EED690825138ECD35620AFE49D1A8979D6698` loaded external UTF-8 SRT into Fellowship paused at seven seconds. Both overlapping cues, including café 日本語, appeared above the controls; selecting Off removed them without advancing media time. Text now respects the visible control surface; PGS rectangles remain unchanged. Sol reviewed the final cache and geometry handling. The final build SHA-256 is `2BF36750F269689885546B92AD2DACBD35BA72C9BB9183CD2481A4B8AA1A701D`. Build and all 77 unit tests pass. Interactive CTest passed all four entries with the real Fellowship fixture and no skips. M1 and M2 retain the strict foreground eligibility gate; the smoke host now waits briefly for ordinary Windows activation instead of treating transient focus denial as a permanent failure. UI verification uses normal local computer control and spends no API credits.

The separate Gyan FFmpeg 8.1 fixture generator displayed an application-error dialog. The corrected explicit-font generator completed with exit 0 and produced a valid 640×360, SAR 1:1, five-second Half-SBS fixture. No matching Application Error/WER record was found in the following read-only investigation; exact crash cause remains unproven. Odyssey was not the executable named in that dialog. Final cleanup query confirmed both test ISOs detached and no test player or FFmpeg process remaining.

The reviewed application SHA-256 is `6EEFC143FDA360C2DB6F37300AA49AB4AF665D5A81A3EA57380BA483D6BD982A`. All six SBS files and both MVC ISOs passed the normal fullscreen `--player-smoke` path with native exit 0, real SDK weaves, and presentation gates `11111111`. Evidence is retained in ignored `build/player-proof/final-corpus-1.log`, `final-corpus-2.log`, and `final-corpus-3-retry.log` through `final-corpus-8-retry.log`. Earlier interrupted runs remain available: an external FFmpeg application-error dialog stole focus, correctly forcing 2D fallback. The final full CTest run passed 4/4 with Fellowship supplied as the real fixture and no skips. Dedicated backend harnesses below cover seeks, track changes, clip transitions, and EOF; the short host runs do not independently cover every operation.

The 1,800-second Fellowship scheduling run used snapshot SHA-256 `78CF3E40B8421F1709AB1B4F64D48FA6642301A4EA4EEDD8656B165DE1574F2A`. It completed with native exit 0, 43,120 presented frames, nine drops, and p99 Present-return age 18.998 ms. Ten of 43,072 steady samples exceeded 40 ms; source and consumed-media-clock deltas were 1,799.958 and 1,799.966 seconds. The run included focus loss and 2D fallback, so it proves neither continuous 3D nor physical scanout timing or audible lip sync. It also did not measure the separate 500 ms post-buffer seek-recovery requirement. Log: `build/player-proof/fellowship-1800.log`.

The following foundation notes record earlier checks. Later host evidence supersedes their remaining integration qualifications; optical, listening, and the remaining local computer-control acceptance checks remain open.

- Parent independently reran synchronized SBS audio on the delayed two-track synthetic fixture and two real movies. Fellowship delivered 442,852 content frames at 44.1 kHz with no inserted silence over ten seconds; Return King switched among six available tracks and delivered 442,087 content frames. The synthetic check covered cancellation, pause, rapid seeks, real submitted tone content, and continued clock progress after audio EOF. These are muted endpoint/clock checks, not listening or thirty-minute drift acceptance.
- Production `MvcPipeline` passed the parent-run staged-runtime Batman test with bounded delivery, pause while the queue was full, audio selection, seek to 1,800 seconds, EOF drain, and replay. Big Hero's explicit `00800.mpls` passed thirty seconds before and after seeking to 3,090 seconds, crossing both independently verified clip boundaries. The DirectSound renderer owns the graph reference clock; MVC diagnostics confirm decoded audio content rather than measuring endpoint samples. The later active-Load close matrix verifies bounded close during LAV source loading.
- Parent reran all 25 converter/compositor/layout tests successfully after software NV12/P010 GPU upload and paired overlay/left-eye preview support. P010 matrix/range output is compared with planar ten-bit conversion. Sol measured forced-software Fellowship conversion at 8.252/11.796/13.864 ms median/p95/p99 over 220 warm samples; these are CPU submission times, not GPU execution latency.
- The installed Immersity runtime now supplies SDK DLLs through the explicit loader. Twelve exact stale SDK DLL copies were removed from the build output; rebuilding did not restore them. Sol verified missing-runtime startup/skip behavior, installed module paths, and all four CTest entries with a real movie. Normal player focus, dialog, recovery, and optical behavior remain integration acceptance work.
- Parent independently repeated full CTest after the persistent host first linked: all four entries passed with Fellowship, including the actual installed SDK runtime. This covers the isolated legacy smoke paths; it does not substitute for testing the new normal player loop.
- The normal fullscreen MVC host independently passed a three-second Batman run: 74 presented frames, zero drops, 385 weaves, active lens/runtime and all native presentation gates satisfied. After 48 warmup frames, presentation-return scheduling error was 15.506 ms p95 / 15.625 ms p99, with zero samples outside +/-40 ms. Source and SDK stereo texture were 3840x1080; native client/output was 3840x2160. This measures successful `Present` return relative to the media clock, not physical scanout or audible lip sync. Evidence: ignored `build/player-proof/batman-caption-guard.log`, native exit 0. A caption geometry exception before the first frame had incorrectly paused playback; caption rendering now waits for valid frame geometry and contains its own errors.
- Parent independently ran the current unit executable: 75 tests across 13 suites passed. ISO title readiness and subtitle rendering cache fixes also passed Sol review. These checks do not replace the remaining real-media and live UI gates.
- Parent independently passed seven renderer/compositor tests, including PGS rectangle geometry and linear-sRGB alpha blending. Subtitle rendering preserves original bitmap rectangles and draws Unicode text with grayscale antialiasing on an opaque backing. Parent also reran the source-linked caption harness: Avengers embedded SubRip decoded after seeking to 26 seconds; seek cleared pending cues; external SRT offset and Off preserved A/V generation. Later production-harness evidence verifies ISO PGS backend delivery; caption mouse and optical acceptance remain open.

ISO PGS backend delivery subsequently passed on Big Hero `00800.mpls` with the production source-linked harness (SHA-256 `0DB46787A4CAC62C93A768FC13FAB9C87207F62782DA05B86C9F6C45E4EA5731`, native exit 0). The check waits for paused selection restoration, seeks to 3,090 seconds, requires an owned bitmap cue at media time, waits for a completed alternate audio selection and generation transition, then seeks back and requires a post-transition cue. Off clears selection and cues. DirectShow returned `VFW_E_NOT_STOPPED` when connecting the PGS pin during playback; selection now stops the graph with bounded frame delivery cancelled, connects, activates caption reception before restoring the absolute position and prior play/pause state. Caption failure stays local when A/V restoration succeeds. Caption mouse/optical acceptance remains outstanding.

Parent tested close while `IFileSourceFilter::Load` was actively executing, using harness SHA-256 `3EE63E8E8D82F68CCA67F6A0570ACB216DB3856813CE33604CC54AB81A3FAF10`. Big Hero delay points 0/50/250/750 ms all recorded `load_active_at_close=1`, test-owned read-only mounts, and native exit 0; destructor/join took 1,664/1,442/1,297/828 ms respectively. This proves bounded close on this local fixture, not an abort facility inside LAV. Earlier mount queries independently showed both test ISOs detached. The harness's mount reopen check is secondary evidence and cannot by itself establish detachment.

- [x] Build with `cmake --build --preset windows-debug`.
- [x] Run `ctest --preset windows-debug --output-on-failure --interactive-debug-mode 1`, providing the real SBS fixture through `ODYSSEY_TEST_M2_FSBS_MKV`; all 4/4 entries passed with no skips on final SHA-256 `2BF36750F269689885546B92AD2DACBD35BA72C9BB9183CD2481A4B8AA1A701D`.
- [x] Run the six SBS files and at least two MVC titles. Test seek, track switches, clip transitions, and end-of-file. Record actual decoder/pixel-format paths.
- [ ] Run a 30-minute synchronized playback sample. Require steady-state measured A/V scheduling drift within ±40 ms and seek recovery within 500 ms after buffering completes.
- [x] Create scenarios under `tests/ui/scenarios/` for the rebuild. Execute them through normal local computer control and spend no API credits.
- [ ] Run approved live mouse tests. Report optical stereo and listening checks separately from screenshots, API status, and clock measurements.
- [x] Resolve parent specification and code-quality review findings, rerunning affected checks only.
- [ ] Re-index jcodemunch, inspect the exact diff/status, commit source/docs only, and push per repository policy once the stage's required gates pass.

Do not mark the rebuild complete with a missing MVC decoder, untested physical stereo, skipped required UI scenarios, or conversion-only ISO support.
