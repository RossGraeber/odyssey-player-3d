# UI Integration Tests

Anthropic computer-use loop driving the running `odyssey.exe`. Used at
the end of every major milestone to verify the live, on-screen behaviour
that headless CTest cannot.

## Quick start

```bash
# One-time
python -m venv tests/ui/.venv
tests/ui/.venv/Scripts/pip install -r tests/ui/requirements.txt
export ANTHROPIC_API_KEY=sk-ant-...

# Per-run (manual)
build/windows-debug/Debug/odyssey.exe --play <fsbs.mkv> &
python tests/ui/computer_use_runner.py \
    --scenario tests/ui/scenarios/m2_video_playback.md \
    --task "weave-visible"
```

The recommended invocation path is via the
[`ui-integration-tester`](../../.claude/agents/ui-integration-tester.md)
Claude Code subagent — it handles preflight, per-scenario sequencing,
and post-flight cleanup. Do not invoke this script in CI / `ctest`:
it costs Anthropic API credits per run.

## Scenario format

One Markdown file per milestone under `scenarios/`. The file declares:

- `# <Milestone> — UI scenarios` — H1 heading.
- `## Launch` — fenced bash block with the exe command.
- `## Tasks` — one `### <task-id>` subsection per scenario, each
  containing a one-paragraph natural-language instruction the
  computer-use agent will follow, and an `**Expected:**` line stating
  the visible outcome.

The runner picks one task by `--task <id>` and exits 0 (pass) or
non-zero (fail). The orchestrator iterates tasks across multiple runs.

## Costs

Default model: `claude-sonnet-4-6` (~5–15 screenshots/run, $0.05–0.20
typical). Override with `--model claude-opus-4-7` for harder scenarios.
