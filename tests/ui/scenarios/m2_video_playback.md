# M2 — UI scenarios

Verify the live behaviour of the M2 video-playback path against the
already-running `odyssey.exe`. The orchestrator launches the process
once; each task below runs against that same process unless the task
itself states otherwise.

## Launch

```bash
build/windows-debug/Debug/odyssey.exe --play "D:\Downloads\VR-SBS_3840x1080_The_Hobbit_An_Unexpected_Journey_2012_EXTENDED.mkv"
```

(`ODYSSEY_TEST_M2_FSBS_MKV` env var may override the path; the
orchestrator should substitute it before delegating the launch.)

## Tasks

### weave-visible

Wait up to 5 seconds for the player window to fill the primary display
and begin showing video. Take a screenshot of the centre region of the
window. Verify the displayed image is NOT a uniform colour (i.e. has
visible detail and contrast — confirming a frame has been decoded,
converted from NV12, and woven by the SR weaver). Do not interact with
the window in any way during this check.

**Expected:** the centre-of-screen pixels show varied colour and
luminance consistent with film content; no solid black, solid grey, or
solid weave-error colour fills the frame.

### alt-enter-toggles

The player launches in a 1280×720 window. Press Alt+Enter once and
wait one second. Take a screenshot. Verify the window now fills the
entire primary display (borderless fullscreen). Press Alt+Enter again
and wait one second. Take a final screenshot. Verify the window has
returned to its previous non-fullscreen size.

**Expected:** the first screenshot shows the window covering 100% of
the primary monitor area; the second shows it back to a windowed
size visibly smaller than the screen.

### esc-exits-cleanly

Press Esc once. Wait two seconds. Take a screenshot. Verify the
odyssey window is no longer present on the desktop (i.e. another
application or the Windows desktop is now visible where the player
used to be).

**Expected:** the post-Esc screenshot shows no `odyssey.exe` window;
the player has exited cleanly.
