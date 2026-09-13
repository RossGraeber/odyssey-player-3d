# MVP Test Corpus

## Fullscreen rebuild corpus

The approved September rebuild uses the files below from `H:\3D`. The historical M2 fixture is retained below for reproducibility. Probe results establish stream metadata, not correct stereo playback.

| File | Verified streams | Required check |
| --- | --- | --- |
| `The.Lord.of.the.Rings.The.Fellowship.of.the.Ring.3D_2001_.Full-SBS.1080p.x264.AC3.ENG-JFC.mkv` | H.264 3840×1080, 24 fps; AC3 5.1 | Primary SBS playback |
| `The.Lord.of.the.Rings.The.Return.of.the.King.Part.One.3D._2003_.EXTENDED.1080p.Full-SBS.x264.6xAudio.JFC.mkv` | H.264 3840×1080, 24000/1001 fps; six audio tracks; JPEG attachments | Audio selection and exclusion of cover images |
| `The.Lord.of.the.Rings.The.Return.of.the.King.Part.Two.3D._2003_.EXTENDED.1080p.Full-SBS.x264.6xAudio.JFC.mkv` | H.264 3840×1080, 24000/1001 fps; six AC3 tracks; JPEG attachments | Track/seek regression |
| `The.Lord.of.the.Rings.The.Two.Towers.3D._2002_Full-SBS.1080p.x264.mkv` | H.264 3840×1080, 24000/1001 fps; DTS-HD MA 7.1 | Multichannel endpoint conversion |
| `VR-SBS_3840x1080_Avengers_Infinity_War_2018.mkv` | H.264 **3840×804**, 24000/1001 fps; DTS 7.1; SubRip | Per-eye letterboxing and embedded captions |
| `Captain EO 3d virtual reality [AJVjp4ruGUo].mp4` | H.264 1280×720, 30 fps; Opus stereo | MP4 and manual layout; stereo packing needs visual confirmation |
| `Batman_Begins__2005__Remastered_MVC_1080p_dgc.iso` | Private LAV probe produces paired 1920×1080 NV12 views at 24000/1001 fps; duration 8400.64 seconds | Production ISO/audio/seek integration; decoder-only initial and seek checks pass |
| `Big.Hero.6.2014.1080p.3D.BluRay.AVC.DTS-HD.MA.7.1-RARBG.iso` | Paired 1920×1080 NV12 views; playlist `00800.mpls` crosses primary-clip boundaries at 3098.762333 and 3114.402956 seconds | Production clip transitions, title/audio selection, PGS captions |

Do not store movies, extracted streams, or decoder binaries in git. Put generated proof results and temporary samples under ignored `build/`. Detailed MVC decoder evidence and its limits are recorded in `tools/mvc_probe/README.md`.

Generated `build/format_fixtures/full-sbs-main10-sdr.mkv` supplies four seconds of HEVC Main10, 3840×1080 at 24000/1001 fps, limited-range BT.709, with red left and blue right views. Its purpose is to verify actual P010 hardware transfer and forced-software conversion; creation alone does not establish playback support. Reproduce with the installed FFmpeg CLI:

```powershell
ffmpeg -hide_banner -loglevel error -y -f lavfi -i 'color=c=red:size=1920x1080:rate=24000/1001:duration=4' -f lavfi -i 'color=c=blue:size=1920x1080:rate=24000/1001:duration=4' -filter_complex '[0:v][1:v]hstack,format=yuv420p10le[v]' -map '[v]' -c:v libx265 -preset ultrafast -x265-params 'log-level=error:pools=2' -color_primaries bt709 -color_trc bt709 -colorspace bt709 -color_range tv -an build/format_fixtures/full-sbs-main10-sdr.mkv
```

For the existing SBS CTest entry:

```powershell
$env:ODYSSEY_TEST_M2_FSBS_MKV = 'H:\3D\The.Lord.of.the.Rings.The.Fellowship.of.the.Ring.3D_2001_.Full-SBS.1080p.x264.AC3.ENG-JFC.mkv'
ctest --preset windows-debug --output-on-failure
```

## Historical April MVP fixtures

The implementation plan (§6) calls for four files, pinned at M2 start, that never
change through MVP so regression comparisons stay stable. Absent paths mean the
corresponding milestone smoke test exits with CTest skip code 77 rather than
failing.

| Slot | Format | Path | Purpose |
|------|--------|------|---------|
| 1 | Full-SBS H.264 MKV (3840×1080) | `D:\Downloads\VR-SBS_3840x1080_The_Hobbit_An_Unexpected_Journey_2012_EXTENDED.mkv` | M2 primary: end-to-end decode → weaver path. M3/M5/M11 reuse. |
| 2 | Ultra-wide SBS MKV | _TBD — fill in before M11_ | M11 aspect-ratio edge case for format auto-detect. |
| 3 | Blu-ray 3D ISO, AVC half-SBS | _TBD — fill in before M8_ | M8 demux + M3 half-SBS path. |
| 4 | Blu-ray 3D ISO, MVC frame-packed | _TBD — fill in before M9_ | M9 MVC decode stress. |

**Environment variable convention.** Smoke tests read the file path from a
per-slot env var so the test stays portable across machines without baking
absolute paths into CTest:

- `ODYSSEY_TEST_M2_FSBS_MKV` — slot 1 (M2).

Unset var → test exits 77 (CTest skip). This keeps the registered test
runnable by any developer while still failing loudly when the file is supposed
to be present (e.g. on the primary dev box).
