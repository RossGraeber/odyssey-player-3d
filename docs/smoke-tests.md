# Odyssey Player 3D — Smoke Tests

One line per milestone for the eye-on check that can't be automated.

- **M0** — `odyssey.exe` opens a 1280x720 window that clears to `#0A0A0A`. Alt-Enter toggles borderless fullscreen on the primary monitor. Esc exits without a debug-break on live D3D11 objects.
- **M1** — `odyssey.exe --spike assets\m1_sbs_fellowship.png` on the Odyssey G9 (with LeiaSR service running) shows a correctly-woven lenticular image for the SBS Fellowship frame. Esc exits cleanly.
- **M2** — `odyssey.exe --play D:\Downloads\VR-SBS_3840x1080_The_Hobbit_An_Unexpected_Journey_2012_EXTENDED.mkv` on the Odyssey G9 plays the full-SBS MKV front-to-back with weaver output at the panel's native rate. The debug console (OutputDebugString visible in DebugView/VS Output) prints decoded FPS once per second and reports zero sustained queue-full drops over 60 s. Esc exits cleanly. Headless: `ctest -R odyssey-m2-play-smoke` with `ODYSSEY_TEST_M2_FSBS_MKV` pointing at the same file exits 0 (SKIPPED when the env var is unset or the SR service is absent).
