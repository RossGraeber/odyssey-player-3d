# Odyssey Player 3D — Smoke Tests

One line per milestone for the eye-on check that can't be automated.

- **M0** — `odyssey.exe` opens a 1280x720 window that clears to `#0A0A0A`. Alt-Enter toggles borderless fullscreen on the primary monitor. Esc exits without a debug-break on live D3D11 objects.
- **M1** — `odyssey.exe --spike assets\m1_sbs_fellowship.png` on the Odyssey G9 (with LeiaSR service running) shows a correctly-woven lenticular image for the SBS Fellowship frame. Esc exits cleanly.
