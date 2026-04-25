# MVP Test Corpus

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
