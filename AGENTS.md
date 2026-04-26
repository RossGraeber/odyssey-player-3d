# AGENTS.md

> Single-page entry point for AI coding agents working in this repo.
> Project-specific behaviour, conventions, and the milestone gating rule
> live in `CLAUDE.md` — read it first.

## Project

Windows-only glasses-free 3D media player. C++17, MSVC x64, CMake + vcpkg
(submodule at `third_party/vcpkg`), Direct3D 11, FFmpeg (LGPL), Immersity
(LeiaSR) SDK weaver. See `docs/superpowers/plans/2026-04-19-odyssey-player-3d-mvp.md`
for the milestone ladder (M0–M12).

## Commands

```bash
# Configure (first run pulls FFmpeg + GTest via vcpkg, ~20 min cold)
cmake --preset windows-debug

# Build (default target = odyssey.exe + odyssey_tests.exe)
cmake --build --preset windows-debug

# Run automated tests (M0 smoke, M1 weave smoke, M2 video smoke, units)
ctest --preset windows-debug --output-on-failure

# Run the M2 smoke test against a real MKV (file path optional via env)
ODYSSEY_TEST_M2_FSBS_MKV=<path> ctest --preset windows-debug -R odyssey-m2

# Live playback for hand-on inspection
build/windows-debug/Debug/odyssey.exe --play <path-to-fsbs.mkv>
```

## Code style

- C++17, `/W4 /WX /permissive-` (warnings are errors). Match existing file
  layout in `src/app/` (one class per .h/.cpp pair; namespace `odyssey`).
- 4-space indent, brace-on-same-line, no exceptions for control flow.
- Direct3D / FFmpeg interop comments explain *why* the call is shaped this
  way (see `Nv12ToRgba.cpp` and `VideoPipeline.cpp` for the house style).
- Default to no comments. Explain non-obvious WHY only, never WHAT.

## Testing

- Unit tests in `tests/*.cpp` (gtest). Behaviour-focused — never write
  tests that just exercise lines for coverage.
- Smoke tests are real e2e CTest entries on the binary itself
  (`--smoke-test`, `--spike-smoke`, `--play-smoke`). Soft-skip via exit
  code 77 when fixtures or the SR service are absent.
- **UI integration tests** live in `tests/ui/` and run on a visible window
  via the Anthropic computer-use API. Invoke at the end of every major
  milestone — see `.claude/agents/ui-integration-tester.md` and CLAUDE.md.

## Boundaries

- ✅ Always: read `CLAUDE.md` for behaviour rules; verify `git status`
  clean before commits; re-index via jcodemunch at milestone close.
- ⚠️ Ask first: introducing new dependencies; restructuring CMake targets;
  changing the milestone plan; spending API credits on the UI test agent.
- 🚫 Never: bypass `/W4 /WX` with pragmas; add `--no-verify` to commits;
  commit binaries (PNGs in `assets/` are the only blessed exception);
  write to `mood_board/` or `.claude/` from automated scripts.

## Git

- Branch: `main`. Push after each milestone closes (`git push`); see
  CLAUDE.md "commit and push per stage" rule.
- Commit messages: imperative subject ≤72 chars, body explains the *why*,
  trailing `Co-Authored-By: Claude ...` line.
