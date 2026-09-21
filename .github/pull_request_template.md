<!-- All sections are required. PRs with empty sections will not be reviewed. -->

## Summary

<!-- What does this PR change, and why? One or two sentences. -->

## Related issue / milestone

<!-- Link the issue (e.g. Closes #12) or name the MVP milestone (M0–M12). Write "None" if not applicable. -->

## Changes

<!-- Bullet list of the concrete changes. -->

-

## Testing

<!-- How was this verified? Check every box that applies and paste the command(s) used. -->

- [ ] Unit / startup tests pass (`ctest --preset windows-debug --output-on-failure`)
- [ ] SDK presentation tests pass from a visible interactive desktop (`--interactive-debug-mode 1`)
- [ ] Live UI scenario(s) in `tests/ui/scenarios/` run and reported per-scenario verdicts
- [ ] Not applicable (docs / CI only) — explain why:

## Checklist

- [ ] Change is scoped to the request; no unrelated refactors or formatting churn
- [ ] New behavior has a test or scenario that fails without it
- [ ] README / docs updated if user-facing behavior or install steps changed
- [ ] No secrets, SDK binaries, or machine-specific paths committed
