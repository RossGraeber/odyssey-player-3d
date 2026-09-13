# Fullscreen rebuild — mouse acceptance scenarios

Run these scenarios against the rebuilt player through normal local computer control. They spend no API credits and require no API key.

A desktop screenshot cannot prove optical left/right separation or audible lip sync. Report those checks separately as requiring physical inspection/listening. While paused, tracking can still change woven pixels; assess the movie scene and time display rather than demanding identical screenshots.

## Launch

```powershell
build/windows-debug/Debug/odyssey.exe
```

Run on the connected G90XF. Record the target monitor and physical window/backbuffer dimensions from the player's diagnostics; do not infer that the primary desktop monitor is the SR display. Begin each independent task with the specified file open, using the mouse.

## Tasks

### open-and-cancel

Use Open to select the Fellowship full-SBS MKV in `H:\3D`. Confirm its filename and duration appear. Open the picker again, then cancel. Check that the previous movie and its playing/paused state are restored and the controls remain usable.

**Expected:** the movie opens without command-line arguments; cancelling does not stop, replace, or restart the previous movie. Native dialog and player remain legible during the transition.

### mouse-transport

During Fellowship playback, reveal the controls and click Pause. Wait two seconds and confirm the displayed media time stays fixed. Click Play and confirm time advances. Click Stop, then Play again. Use only mouse controls.

**Expected:** pause freezes media time and retains the scene; play resumes; stop returns to a documented initial position and permits replay. The UI never becomes unresponsive.

### scrub-and-capture

Drag the scrubber to a position well inside the movie. Confirm the preview timestamp follows the drag and releasing lands near it. Repeat backward. Start another drag, move the pointer outside the scrubber, then release. Move over the controls afterward.

**Expected:** seeking works in both directions, progress does not jump back to stale frames, and pointer capture is released even outside the scrubber. A paused seek stays paused.

### audio-and-captions

Open Return of the King Part One. Select two different audio tracks using their language/title labels. Confirm the selected label changes without restarting the movie. Open Avengers and select its embedded SubRip track, then Off. Seek to a different captioned passage and re-enable captions.

**Expected:** selection labels reflect actual tracks, media position is retained across audio changes, captions appear at media time and disappear when Off is selected. No stale cue remains after seeking. Record audible language change and lip sync as separate listening checks.

### volume-and-mute

While playing a movie with audio, move the volume control down and up. Click Mute twice.

**Expected:** volume and mute indicators follow the mouse actions; mute can be toggled without changing media position. Actual loudness/mute behavior requires listening and must not be inferred from the icons alone.

### layout-and-wide-movie

Open Avengers. Confirm each eye is fitted without stretching the approximately 2.39:1 image to 16:9. Exercise Full-SBS, Half-SBS, and Swap Eyes controls, then restore Full-SBS with the original eye order. Repeat with `D:\Sources\odyssey-player-3d\build\ui-fixtures\half-sbs-labeled.mp4`, selecting Half-SBS explicitly.

**Expected:** controls update the selected layout; wide content retains matching letterbox bars. Physical optical inspection confirms eye order and undistorted geometry. Do not mark that optical check passed from a screenshot alone.

### auto-hide-and-menu

Let the playing movie sit idle for more than 2.5 seconds. Move the mouse to restore controls. Keep an audio/caption menu open beyond the hide interval. Close it, pause, and wait again.

**Expected:** controls and cursor hide during idle playback, restore on movement, and remain visible for an open menu, active drag, or paused movie.

### fullscreen-focus-and-restore

Use the visible fullscreen control to switch to windowed mode and back. Switch focus to another normal application, then return. Minimize and restore the player. Do not restart the shared SR service.

**Expected:** windowed/unfocused presentation is identified as 2D when appropriate; restoring focused fullscreen returns to the target display with correct dimensions. Controls work after every transition. Lens behavior requires physical inspection or runtime event evidence, not a status label alone.

### direct-iso

Use Open to choose Batman Begins ISO directly from `H:\3D`. Inspect its title menu and select playlist `00000.mpls` by the displayed ID and duration. Seek to a later scene, pause/resume, and exercise audio/caption selection where tracks exist. Close the movie afterward.

**Expected:** the user does not extract or convert the movie. Playback resumes after seeking, and diagnostics identify the MVC decoder and both eye views. Any ISO mount created by the player is released on close; a mount that existed before the test is retained. Record optical stereo and audible synchronization as separate physical checks.

### close-during-open

Begin opening a large movie or ISO, then use Close while buffering. Repeat once after ordinary playback has started.

**Expected:** the player closes without hanging, stale dialogs, or lingering probe-owned mounts. Report process exit and cleanup evidence separately from the disappearance of the window.

### external-captions-and-offset

Open Fellowship and load `D:\Sources\odyssey-player-3d\build\ui-fixtures\overlap-utf8.srt`. It contains non-ASCII text in cues at 3-10 and 6-13 seconds. Seek to 7 seconds and pause, change the caption timing offset, then choose Off. Seek backward and reload the same external captions.

**Expected:** readable text appears in both eye views at the selected media time. Timing offset changes the cue interval without changing audio/video position. Off clears the caption immediately, including while paused. The external file survives seek; no stale cue survives outside its interval.

### iso-title-and-pgs

Open Big Hero 6 ISO. Inspect the title menu's actual playlist identifiers and durations, select `00800.mpls`, and select a supported embedded PGS subtitle track. Seek to a captioned scene and toggle captions Off/on. Seek to 3,090 seconds and play through 3,120 seconds. Select another available feature-length playlist afterward.

**Expected:** title selection opens the selected playlist directly without movie extraction. PGS geometry is preserved, captions appear in both views, and Off clears them. Playback continues across the clip boundaries at approximately 3,098.762 and 3,114.403 seconds. The selected title label follows the actual playlist; do not infer a language from its number. Record optical stereo and audible synchronization separately.
