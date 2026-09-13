# UI Integration Tests

Local computer-control scenarios for the running `odyssey.exe`. Use them at
the end of every major milestone to verify visible behaviour that automated
CTest cannot.

## Quick start

Build and launch from the repository root:

```powershell
Start-Process build/windows-debug/Debug/odyssey.exe
```

Use normal local computer control to execute each task in the selected scenario
file. Capture the visible result and record pass/fail plus any physical checks
that screenshots cannot establish. This workflow spends no API credits and
requires no API key.

Do not invoke the disabled `computer_use_runner.py`; its retired implementation
used a paid remote API.
Do not add live UI control to CI or `ctest` because it requires a visible,
foreground desktop.

## Scenario format

One Markdown file per milestone under `scenarios/`. The file declares:

- `# <Milestone> — UI scenarios` — H1 heading.
- `## Launch` — fenced bash block with the exe command.
- `## Tasks` — one `### <task-id>` subsection per scenario, each
  containing a one-paragraph natural-language instruction the local operator
  will follow, and an `**Expected:**` line stating
  the visible outcome.

Execute each task independently and record its visible outcome before moving to
the next task.
