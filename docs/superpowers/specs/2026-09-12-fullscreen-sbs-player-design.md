# Fullscreen SBS and MVC movie player redesign

The user approved this replacement delivery sequence for the April MVP plan. The parent agent owns design and final review; Luna implements; Sol reviews implementation and handles task coordination. Execution tasks are in `../plans/2026-09-12-fullscreen-player-rebuild.md`.

## Outcome

Launch a normal Windows application, open an SBS movie with the mouse, and watch synchronized stereo video and audio fullscreen on the Samsung G90XF through the installed Immersity runtime. Playback controls use a pixel-art, classic WinAmp-inspired appearance. Every essential operation is available with the mouse.

Scope includes ordinary SBS media files, including MKV and MP4, **and direct Blu-ray ISO/MVC playback**, as explicitly confirmed by the user. No offline conversion step is an acceptable substitute. An ISO must never silently play a single MVC base view while being labelled 3D.

## Initial inspection evidence

This section records the pre-rebuild baseline. Current implementation and verification evidence are in the execution plan.

- The current source is an M2 prototype. `AppShell::runPlay` presents the newest decoded frame and has no audio output or mouse transport controls.
- `ImmersityWeaver` retrieves an SR display but does not position the window using that display's location. Fullscreen currently selects the nearest monitor.
- The current SR event sink handles only context invalidation. Its callback writes a plain Boolean that the render thread reads.
- `frameWeave` passes the full SBS texture width to `setInputViewTexture`. The installed SDK's stereo-image example divides texture width by two before this call (`examples/directx11_weaving/directx11_weaving/main.cpp:1155-1163`). The API needs the per-eye width despite its abbreviated header description.
- The ordinary playback path starts decoding before creating the weaver. The smoke path explicitly reverses this order to avoid a documented D3D initialization race.
- The conversion shader assumes NV12, BT.709, and limited range. The display path needs explicit handling of other decoded formats and color metadata.
- Fresh Debug build succeeds. All 13 existing unit tests pass. These tests do not establish optical stereo quality or synchronized movie playback.
- The installed FFmpeg CLI opens `Batman_Begins__2005__Remastered_MVC_1080p_dgc.iso` directly using its `bluray:` protocol and reports a 8400.64-second feature with one 1920×1080 AVC stream and four audio streams. This proves readable disc access for this image, not MVC output; the probe exposes no dependent eye.
- The app's FFmpeg package does not include libbluray. The CLI's disc support must not be mistaken for app support. Local audit found no LAV or legacy Media SDK runtime; NVIDIA tooling reports an RTX 3060. Dependency availability and decoding throughput remain unverified.

The six MKV/MP4 files in `H:\3D` were probed without modifying them:

| Fixture | Video | Audio and captions | Purpose |
| --- | --- | --- | --- |
| Fellowship | H.264, 3840×1080, 24 fps | AC3 5.1 | Full-SBS and integer frame rate |
| Return of the King, both parts | H.264, 3840×1080, 24000/1001 fps | Six audio tracks; attached JPEG cover images | Track changes and correct video stream selection |
| Two Towers | H.264, 3840×1080, 24000/1001 fps | DTS-HD MA 7.1 | Multichannel decode and endpoint conversion |
| Avengers | H.264, **3840×804**, 24000/1001 fps | DTS 7.1 and SubRip | Letterboxing and embedded captions |
| Captain EO | H.264, 1280×720, 30 fps | Opus stereo | MP4, alternate frame rate, manual layout selection |

Captain EO's dimensions alone do not prove its stereo packing. Confirm visually and expose an explicit layout control. Do not derive dimensions from filenames.

## Approach

Recommended: retain C++17, FFmpeg, D3D11, Win32, and the Immersity integration seam. Replace the prototype's playback scheduling and add a small native control layer. Existing FFmpeg provides ordinary video/audio decode, resampling, and software conversion; Windows provides audio, file dialogs, text rendering, and input. The SBS portion needs no new third-party dependency. ISO/MVC needs a proven disc reader and two-view decoder; dependency approval precedes installation or integration.

An ImGui interface is an alternative, with ready-made widgets but an additional dependency and custom styling work. Replacing playback with a separate player engine is another alternative, but its rendering and clock integration would need fresh proof against Immersity. Neither removes the central stereo output work.

## Direct ISO/MVC feasibility gate

Prove this route before committing to the playback architecture: a disc reader follows the selected Blu-ray playlist, exposes the AVC base view and MVC dependent view in order, and a real MVC decoder produces paired, timestamped frames for the same compositor used by SBS files.

The first evidence-backed candidate is libbluray for navigation with a legacy Intel Media SDK MVC decoder. LAV Filters provides maintained reference code using `MFX_EXTBUFF_MVC_SEQ_DESC` and software/hardware implementation selection. The software route must be tested; lack of an Intel GPU alone does not disqualify it. This is evidence of an integration route, not proof that the required runtime is installed or fast enough here. Do not assume current oneVPL or FFmpeg's ordinary H.264 decoder provides the same capability.

The spike must read one actual ISO, enumerate its playlists and streams, prove access to the dependent view across clip boundaries, decode at least 60 seconds into two distinct correctly ordered views, and measure sustained throughput at or above source frame rate. Repeat a seek and confirm paired timestamps. An AVC-only demux result fails this gate. Investigate SSIF/dependent-stream access explicitly; ordinary `bd_read` output must not be presumed sufficient.

Prefer a small direct decoder adapter if the required runtime is available. If not, evaluate LAV's MVC decoder through a private DirectShow graph; that is a concrete second candidate, with extra integration and distribution considerations. A bounded Windows Media Foundation capability probe can run without a third-party decoder, but merely finding the Windows H.264 DLL does not establish MVC support. Do not copy LAV's GPL source into the project or register filters globally as a shortcut. Select and pin the dependency only after this spike establishes compatibility. No new hand-written MVC decoder or dual independent H.264 decoder scheme: the dependent view has inter-view dependencies.

The UI opens ISO files directly and lists valid playlist IDs with duration, selecting the longest valid playlist deterministically and allowing manual title override. Duration does not establish which playlist is the main feature. Preserve playlist ordering and clip transitions. Route audio, PGS bitmap captions, and chapter/seek timestamps through the same media timeline. Full Blu-ray menus and BD-J are outside this design. Inspect protection status; the plan assumes readable/decrypted images and must report a protected-image failure distinctly if that assumption is false.

Success requires direct MVC playback. If neither candidate works, document the concrete failure and keep this requirement incomplete; do not reinstate the old plan's conversion-only fallback.

Primary evidence: [LAV MVC decoder source](https://github.com/Nevcairiel/LAVFilters/blob/master/decoder/LAVVideo/decoders/msdk_mvc.cpp) and [VideoLAN libbluray](https://images.videolan.org/developers/libbluray.html). These establish candidates and disc navigation capabilities, not local runtime success.

## Playback contract

One demux worker owns the FFmpeg format context; separate video and audio workers own their decoder contexts. Bounded packet queues prevent video delivery waits from blocking endpoint service or control commands. Serialize open, seek, pause, track-change, and stop transitions. Use bounded video and audio queues with wakeable waits. Slow rendering must not cause unbounded buffering; seek and shutdown must wake blocked producers. Exclude attached pictures when selecting the movie stream.

The main thread owns the window, D3D rendering, composition, weaver calls, and presentation. Initialize the weaver before starting shared-device decoding. Keep shared immediate-context access serialized, but do not hold that lock across v-sync waits, queue waits, disk reads, or audio waits.

Use a concrete WASAPI shared-mode audio output object. Decode the selected audio track with FFmpeg and resample/channel-map to the Windows endpoint's actual mix format. Support volume and mute. Do not add an audio backend interface with only one implementation.

For MVC, the verified private LAV graph supplies paired NV12 samples. Keep disc demux, MVC decode, and LAV audio in that graph, using the Windows audio renderer's clock rather than independently demuxing the same disc through FFmpeg. A concrete MVC adapter exposes the same playback operations and timestamp units to the host. Its streaming sink has bounded delivery and flush cancellation; the graph's COM owner thread serializes commands. The production graph and its audio clock still require their own validation beyond the completed decoder-only proof.

The master timeline follows audio actually consumed by the endpoint, anchored to decoded audio timestamps. Do not use packet arrival, decode throughput, or total submitted samples as the playback clock. Account for buffered output and resampler delay. With no audio stream, use a monotonic clock anchored to video timestamps. Audio output failure is visible and allows an explicit silent-playback choice.

Display video by presentation timestamp: retain early frames; display due frames; drop obsolete late frames. Reweave the retained stereo frame at display refresh for tracking, including while paused. Preserve 24000/1001 timing without rounding to 24 or tying movie speed to monitor refresh.

Seek flushes decoder, resampler, queues, current captions, and endpoint audio. Increment a playback generation so frames from before the seek cannot reappear. Seek to a preceding keyframe, decode to the requested time, trim audio to the same timeline, then resume. Switching audio preserves position and paused/playing state. End-of-file drains delayed video and audio and leaves the player available to replay or open another movie.

## Stereo and display contract

Discover the SR display location and recommended view texture dimensions from the installed SDK. Match its monitor and output adapter. Enable per-monitor DPI awareness before creating the window. Use borderless fullscreen at the monitor's physical client dimensions with v-sync. Do not hardcode a model's resolution or assume the primary monitor is the G90XF.

Treat recommended view width as one eye's width: allocate the SBS render target at twice that width, and pass one eye's width to `setInputViewTexture`, as the installed official DX11 example does. Keep physical display aspect ratio explicit when fitting views; do not assume a packed texture's overall aspect describes either eye.

Support Auto, Full-SBS, Half-SBS, and 2D, plus Swap Eyes. Prefer explicit stereo metadata, then unambiguous filename markers. For ambiguous input, show the selected interpretation and retain manual correction. Do not infer half-SBS from a 16:9 frame alone.

For Full-SBS, split source width into two eye regions and preserve each eye's display aspect ratio, including sample aspect ratio. For Half-SBS, restore the horizontally compressed eye aspect ratio. Fit each eye into the SDK view region, with matching letterbox bars. Clamp samples within each eye to prevent filtering across the SBS seam. The Avengers fixture must remain approximately 2.39:1 per eye.

Decode at source resolution. Prefer hardware decode for supported formats; otherwise use FFmpeg software decode and upload. Select conversion from the actual frame format and color metadata. Test software decode explicitly. Add representative HEVC Main10/P010 and non-BT.709 SDR fixtures before claiming those paths supported. HDR is not silently treated as SDR: expose an unsupported-color-mode message unless a separately verified conversion exists.

Render the movie, captions, and visible controls into both eye regions before weaving. Controls occupy the same screen-depth position in each eye. The final woven image goes directly to the physical output without scaling, filtering, post-processing, or drawing an ordinary 2D overlay over it. Higher input texture resolution does not create source detail or imply full panel resolution for each optical eye.

Use lens hints and thread-safe event state. Handle focus loss, minimize, windowed mode, SRUnavailable/SRRestored, and ContextInvalid. Release the lens hint and show an identified single-eye 2D preview when 3D is inappropriate. Rebuild an invalid session at a bounded retry rate; keep a healthy session across ordinary file changes. Never restart system services or change global runtime settings.

Load SDK runtime libraries from the installed runtime using the established Dwarf Fortress integration as a reference. Stop shipping copied runtime DLLs beside the executable, consistent with local SDK application guidelines. Any cleanup is limited to specifically identified build outputs.

## Mouse interface

Use charcoal panels, hard pixel edges, raised transport buttons, amber position text, and green status accents. Scale pixel geometry in integer steps for readability. Use Windows text rendering for Unicode filenames, track labels, and captions; do not implement a miniature text engine.

The bottom transport panel contains:

```text
+ ODYSSEY 3D ------------------------------ [MIN] [FULL] [CLOSE] +
| Movie title                                      [3D ACTIVE] |
| 00:27:13  =================|----------------------  03:48:18  |
| [OPEN] [PLAY/PAUSE] [STOP]   [MUTE] ----VOLUME----              |
| [AUDIO: English v] [CAPTIONS: Off v] [SBS: Full v] [SWAP EYES] |
+-------------------------------------------------------------+
```

Startup shows a clear Open Movie button. Use the native Unicode file picker and accept drag-and-drop plus a positional command-line path. Pause and release the lens hint while a native file dialog is active; restore playback state if cancelled.

Click or drag the scrubber to seek, with pointer capture and a preview timestamp. Keep the panel visible during hover, dragging, menus, pause, and errors. Hide it and the cursor after 2.5 seconds of inactivity during playback; mouse movement restores both. Provide visible hover, pressed, focus, and disabled states with usable hit areas. Keyboard shortcuts supplement mouse actions.

Audio and caption menus show language and track title, not only stream numbers. Captions support the embedded SubRip fixture, external UTF-8 SRT, and Blu-ray PGS bitmap captions, with Off and a timing offset. Render cues against media time into both eyes. Clear expired cues and reset them on seek. Broader subtitle formats require explicit fixtures; unsupported formats receive a readable message rather than disappearing silently.

## Delivery sequence and parent review

The proposed sequence replaces the old ordering that postponed audio until M10. Each stage is implemented by Sol and reviewed by the parent before proceeding.

1. **Direct MVC proof:** establish real two-view decode from an actual ISO, including clip transitions and seek. Select the smallest working dependency route using the gate above.
2. **Correct stereo output:** target monitor, initialization order, lens lifecycle, Full/Half-SBS and MVC composition, eye swap, aspect ratio, 2D fallback, runtime loading, and deterministic stereo tests.
3. **Watchable playback:** bounded queues, timestamps, audio output, sync, pause/seek/stop, end-of-file draining, audio selection, software fallback, ISO playlist integration, and cancellation tests.
4. **Complete mouse workflow:** native file selection, drag-and-drop, pixel transport panel, scrubber, title/track menus, text and bitmap captions, auto-hide, and readable errors.
5. **Hardware acceptance:** exercise the actual corpus and format fixtures, measure sync and dropped frames, test focus/resize/reconnect, inspect the display, and resolve parent review findings.

Keep the existing executable/test targets. Add necessary source files and system libraries to those targets without introducing a framework or changing the build architecture. Do not include playlists, a media library database, equalizers, skin engines, AI upscaling, or arbitrary speed control in this scope.

## Acceptance evidence

- Debug build passes with the existing application warning policy. Run the complete CTest preset; report skips separately from passes.
- Deterministic tests cover distinguishable left/right images, half-SBS geometry, letterboxing, eye-swap, seam sampling, timestamp scheduling, pause/seek generations, queue cancellation, caption intervals, and audio timeline accounting.
- Real-media checks cover all six files, both 23.976 and 24 fps, six-language audio selection, software fallback, embedded SubRip, and external SRT. Generate small deterministic fixtures for unsupported corpus combinations rather than adding movies to git.
- Direct-ISO checks cover at least two independently selected MVC titles, playlist/clip transitions, seek, audio selection, and PGS captions when present. Confirm two decoded views and optical stereo. No intermediate full-movie conversion or extraction is required from the user.
- In steady-state playback, measured scheduled video-to-audio drift remains within ±40 ms over 30 minutes after warm-up; seeks recover synchronization within 500 ms after buffering finishes. Report actual timing traces and endpoint assumptions. Logical timing measurements do not substitute for listening and optical inspection.
- On the physical G90XF, verify correct eye order, geometry, readable controls/captions, no stretching of the woven output, natural movie speed, and matched audio. A screenshot or an SDK Active flag alone cannot prove glasses-free stereo quality.
- Run mouse scenarios for open/cancel, pause/resume, repeated seek, audio/caption selection, volume/mute, auto-hide, fullscreen/windowed transitions, minimize/restore, and close while buffering. Do not stop the shared SR service to simulate failure; use a controlled test hook.
- Add stage scenarios under `tests/ui/scenarios/` and execute them through normal local computer control. Do not spend API credits for verification or mark a milestone closed without the required visible evidence.
- After the parent accepts code and validation, re-index with jcodemunch and commit/push per repository policy. Exclude build outputs, media, and secrets.

## References

- `src/app/AppShell.cpp`, `VideoPipeline.cpp`, `Nv12ToRgba.cpp`, `ImmersityWeaver.cpp`, `Win32Window.cpp`, and `D3D11Device.cpp`.
- `D:\Sources\ImmersityDocs\sdk_native-sdk-c-c___index_index_pages_application_guidelines.md`.
- `D:\Sources\Immersity-Demos\demos\dx11_cube\main.cpp` for display location and recommended view dimensions.
- `D:\Sources\DF-Mods\Dwarf-Fortress-3D\df3d_stereo.cpp` for runtime loading, event synchronization, and retained SR session lifecycle. Its D3D9 rendering code is reference material, not code to copy into the D3D11 player.
