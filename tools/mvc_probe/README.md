# Direct ISO/MVC feasibility probe

This standalone DirectShow probe validates LAV Filters' software MVC decode
path without changing the player or registering filters system-wide.

## Proof contract

The probe will first ask LAV Splitter Source to open the ISO directly. If that
build cannot read an image path, `run.ps1` may mount it with Windows
`Mount-DiskImage` and open `BDMV\index.bdmv` from the resulting volume. The
fallback must record whether the image was already attached, dismount only an
image mounted by the probe, and perform that cleanup on both success and
failure. The probe will load LAV Splitter Source and LAV Video Decoder directly
from their local `.ax` files by calling `DllGetClassObject`, and connect them in
a private DirectShow graph. It must not register either filter or write LAV
settings to the registry.

The sink must accept LAV's MVC output and record, for each delivered sample:

- the shared sample timestamp;
- base-view and dependent-view checksums computed from decoded NV12 planes;
- whether the views differ;
- decode wall time, decoded duration, and effective frames per second.

A successful run requires every delivered sample to expose an MVC pair, at
least 60 seconds of consecutive shared sample timestamps, at least one differing
base/dependent checksum, and sustained decode throughput at or above the title's
source frame rate. The probe must then seek to a later point, flush, and
establish the same paired-view conditions again. `IMediaSample3D` exposes both
views under one sample timestamp; external MVC view IDs and decoder frame-order
metadata are not observable at this boundary. An AVC-only stream or two
checksums derived from one decoded plane is a failure.

The intended command is:

```powershell
.\tools\mvc_probe\run.ps1 `
  -InputPath 'H:\3D\Batman_Begins__2005__Remastered_MVC_1080p_dgc.iso' `
  -Seconds 60 `
  -SeekSeconds 1800
```

Artifacts and decoded samples belong under ignored `build/`; the probe must not
extract or convert a full movie.

From a clean checkout, prepare vcpkg's pinned 7-Zip tool and stage the private
dependencies before running the probe:

```powershell
.\third_party\vcpkg\bootstrap-vcpkg.bat -disableMetrics
.\third_party\vcpkg\vcpkg.exe install `
  --triplet x64-windows `
  --x-manifest-root=. `
  --x-install-root=.\vcpkg_installed
.\tools\bootstrap-private-deps.ps1
```

`run.ps1` calls the same bootstrap, so the player and probe cannot drift to
different archive hashes or staging layouts.

## Candidate dependencies

The selected bounded candidate is LAV Filters 0.83 x64 plus LAV's legacy Intel
Media SDK software MVC runtime. GitHub's official latest-release URL resolves
to 0.83, dated 2026-08-17. The installer requests the MVC runtime separately.

| Artifact | Origin | Expected local use | SHA-256 |
| --- | --- | --- | --- |
| `LAVFilters-0.83-x64.zip` | <https://github.com/Nevcairiel/LAVFilters/releases/download/0.83/LAVFilters-0.83-x64.zip> | Private `LAVSplitter.ax`, `LAVVideo.ax`, and adjacent DLLs | `0126982F47157BB86A6DBB43C4F332F7F98BEBA9AD552C19B65F9DB2E7D4F186` |
| `libmfxsw64-v3.7z` | <https://files.1f0.de/lavf/plugins/libmfxsw64-v3.7z> | Software H.264 MVC decoder runtime beside `LAVVideo.ax` | `5CEFD058F2523A2486C6E875E4D256563AC45554A87F3623DC84F87A09409B5B` |
| `LAVFilters-source-0.83.zip` | <https://github.com/Nevcairiel/LAVFilters/archive/refs/tags/0.83.zip> | Tagged source provenance for the public DirectShow headers | `B0889E7C7570E65ACD00C5A3677EEF2C7851B1E60F4D0F591645D81AB084A98F` |

The shared bootstrap verifies all three archives before extraction, verifies
the packaged public headers against the tagged source, and hashes the complete
staged payload before the probe builds. Do not use Intel's installed
`C:\Windows\System32\libmfxhw64.dll` as proof of software MVC availability;
no `libmfxsw64.dll` was found under Windows or Program Files during the local
audit.

LAV's source is the implementation evidence for this route:

- MVC decoder: <https://github.com/Nevcairiel/LAVFilters/blob/0.83/decoder/LAVVideo/decoders/msdk_mvc.cpp>
- Installer dependency declaration: <https://github.com/Nevcairiel/LAVFilters/blob/0.83/LAVFilters.iss>
- Release history: <https://github.com/Nevcairiel/LAVFilters/releases/tag/0.83>

The decoder tries `MFX_IMPL_AUTO_ANY` and then `MFX_IMPL_SOFTWARE`, requests
`MFX_EXTBUFF_MVC_SEQ_DESC`, and pairs outputs by MVC view ID and frame order.
LAV's changelog identifies Blu-ray/SSIF MVC demuxing and MVC decoding as paired
Splitter/Video features. Those source facts justify the candidate; they do not
prove it works on this machine.

## Measured evidence

The command above passed against the Batman Begins ISO. LAV Splitter Source
could not load the ISO path directly, so `run.ps1` mounted it read-only and
opened `BDMV\index.bdmv`. The script dismounted the image it attached; a
subsequent `Get-DiskImage` reported it detached.

| Phase | Samples / MVC pairs | Distinct pairs | Media / wall time | Decode rate |
| --- | ---: | ---: | ---: | ---: |
| Initial | 1440 / 1440 | 1375 | 60.018 s / 14.346 s | 100.379 fps |
| Seek to 1800 s | 1440 / 1440 | 1440 | 60.018 s / 16.308 s | 88.301 fps |

Both phases produced NV12 at 1920x1080 with stride 1920 from a 23.976 fps
source. Missing timestamps, timestamp regressions, timestamp gaps, malformed
pairs, and sample-without-pair counts were all zero. The result proves paired
base/dependent buffers with shared PTS at this sink boundary; it does not expose
LAV's internal MVC view IDs or decoder frame-order metadata.

After the ownership and stricter pair-validation fixes, the parent reran Batman:
both phases again delivered 1440/1440 pairs over 60.018 seconds, at 99.046 fps
initially and 88.795 fps after seeking. The Fellowship SBS negative control
returned exit 23 with zero MVC pairs, as required.

The parent also ran the same command against
`Big.Hero.6.2014.1080p.3D.BluRay.AVC.DTS-HD.MA.7.1-RARBG.iso`:

| Phase | Samples / MVC pairs | Distinct pairs | Media / wall time | Decode rate |
| --- | ---: | ---: | ---: | ---: |
| Initial | 1440 / 1440 | 1336 | 60.018 s / 13.020 s | 110.598 fps |
| Seek to 1800 s | 1440 / 1440 | 1440 | 60.018 s / 16.394 s | 87.838 fps |

Its output was also 1920x1080 NV12 at 23.976 fps, with no missing timestamps,
regressions, gaps, or malformed pairs. Both temporary mounts were confirmed
detached afterward. These runs establish decoding and seeking on two images;
they do not yet establish a particular clip transition, audio synchronization,
or optical output on the monitor. Reported PTS values are in 100 ns units and
are segment-relative after seeking.

The parent then opened Big Hero's explicit `BDMV/PLAYLIST/00800.mpls`, sought
to 3090 seconds, and decoded 60.018 seconds across the first two clip changes.
The playlist's primary clip sequence is 00300, 00302, 00304, 00306, 00308;
its MVC dependent sequence is 00301, 00303, 00305, 00307, 00309. Parsing the
primary play-item in/out times independently confirmed boundaries at
3098.762333 and 3114.402956 seconds. The transition phase passed with 1440/1440
MVC pairs, all distinct, no timestamp gaps/regressions/missing PTS, and
92.132 fps. Its segment-relative PTS range was 35781 to 600218670 in 100 ns
units. The wrapper returned exit 0 after cleanup. This checks continuous
paired output across known playlist boundaries; it does not identify clip
changes from the sample callback itself.
