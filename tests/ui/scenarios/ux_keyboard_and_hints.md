# UX — UI scenarios

## Launch

```bash
build/windows-debug/Debug/odyssey.exe --play "D:\Downloads\VR-SBS_3840x1080_The_Hobbit_An_Unexpected_Journey_2012_EXTENDED.mkv"
```

## Tasks

### space-toggles-playback

Press Space. Confirm the panel appears, its Pause/Play button label switches from Pause to Play, and the status reads Paused. Press Space again.

**Expected:** the first press pauses playback (panel shows Play label, status Paused); the second press resumes playback (panel shows Pause label, status Playing).

### arrows-seek-and-volume

Press Right twice and read the time readout. Press Down twice and observe the green volume bar.

**Expected:** the time readout advances by roughly 10 seconds after two Right presses; the volume bar visibly shrinks after two Down presses.

### hover-shows-hint

Move the mouse over the Play/Pause button and read the panel header hint. Then move the mouse over the seek bar and read the hint again.

**Expected:** hovering Play/Pause shows a header hint reading "Pause  (Space)" or "Play  (Space)" and the button gets an amber outline; hovering the seek bar shows a hint reading "Seek to HH:MM:SS…".

### double-click-fullscreen-esc-leaves

Double-click the video area away from the control panel. Press Esc once. Press Esc again.

**Expected:** the double-click fills the window to the display; the first Esc returns the window to its windowed size with the app still running; the second Esc quits the app.

### readability-check

Screenshot the control panel and inspect the labels.

**Expected:** labels are mixed case (not all-caps), off-white text on a near-black background, crisp non-antialiased text, and chunky bevels on buttons.
