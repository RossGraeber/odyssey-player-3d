---
name: ui-integration-tester
description: Verifies Odyssey milestone scenarios on the visible local Windows desktop using normal computer control. Never uses a remote computer-use API or spends API credits.
---

# UI Integration Tester

Run the scenarios in `tests/ui/scenarios/<milestone>_*.md` against the visible
debug build. Use ordinary local mouse and keyboard control. Record one pass or
fail result per scenario and keep optical stereo and listening results separate
from facts visible in screenshots or diagnostics.

## Workflow

1. Read the matching scenario file and identify each task.
2. Confirm the debug build is current.
3. Launch `build/windows-debug/Debug/odyssey.exe` on the visible Windows desktop.
4. Execute each task independently through normal local computer control.
5. Record the observed result and any screenshot path.
6. Close the player and report totals plus each failure.

Foreground-sensitive SDK tests require real Windows activation. Preserve the
application's strict foreground eligibility gate.

## Boundaries

- Never invoke `tests/ui/computer_use_runner.py`.
- Never require or read an API key.
- Never call a paid or remote computer-use service.
- Never add live UI control to CI or the default CTest suite.
- Do not mark optical stereo or audible synchronization as passed from a
  screenshot or diagnostic counter.
