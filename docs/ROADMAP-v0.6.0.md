# Genia Unlocker v0.6.0 Roadmap

Development branch: `v0.6.0-dev`

Baseline: `v0.5.0 Final` remains frozen on `main`.

## Product rule

Every v0.6.0 feature must do at least one of two things:

1. Save the user meaningful actions.
2. Explain a lock/delete failure more clearly.

If a feature does neither, it does not belong in v0.6.0.

## Phase 1 implementation status

- [x] Read-only Batch window with isolated per-target results.
- [x] Multiple file/folder selection and multi-drop entry point.
- [x] Open a Batch row in the existing single-target UI.
- [ ] Per-target details, progress updates and cancellation UI.
- [ ] Destructive Batch actions (require a separate safety review and Windows tests).

## Phase 1 — Multi-target / Batch

Goal: inspect and act on multiple selected files/folders without turning Genia Unlocker into a file manager.

Planned behavior:
- accept multiple paths from Explorer/context-menu invocation;
- accept multiple drag-and-drop targets;
- allow multi-select in the file picker where practical;
- represent each target independently: Clean / Locked / Missing / Error;
- keep blocker ownership and exact locked-object paths per target;
- provide safe batch actions only when their meaning is unambiguous;
- preserve single-target UX as the fast default path.

Acceptance:
- one locked item cannot corrupt the state/report of another;
- batch scan stays asynchronous and UI remains responsive;
- destructive batch actions are explicitly confirmed;
- critical-process protections remain unchanged.

## Phase 2 — CLI

Goal: make the same proven engine usable from PowerShell, BAT files and automation.

Planned public commands:
- `--scan <path>`
- `--unlock <path>`
- `--force-unlock <path>`
- `--delete <path>`
- batch input for multiple paths after the single-target contract is stable;
- machine-readable exit codes;
- optional text/JSON diagnostic output if it stays lightweight.

CLI rules:
- no silent escalation to destructive actions;
- stable documented exit codes;
- CLI and GUI use the same scanner/action implementation;
- no hidden success: filesystem/rescan verification stays authoritative.

## Phase 3 — Extended diagnostics

Goal: explain *why* Windows still refuses an operation when no ordinary process handle can be safely released.

Planned classification where Windows exposes reliable evidence:
- sharing violation / process lock;
- access denied / permissions;
- read-only/system attributes;
- missing target / path errors;
- reparse point / junction;
- network/remote filesystem limitations;
- cloud placeholder/provider behavior;
- likely kernel/minifilter/antivirus-style lock when user-mode evidence is exhausted.

Diagnostics must distinguish:
- confirmed facts;
- Windows error codes;
- best-effort interpretation.

No speculative process attribution.

## Supporting improvements

Only after the three core phases are stable:
- Save diagnostics to a text file for GitHub issues.
- Small recent-session action history.
- Optional manual/opt-in GitHub Releases update check.
- Explorer submenu only if it stays compact and useful.

## Non-goals

v0.6.0 will not add:
- kernel drivers;
- permanent Windows services;
- filesystem monitoring daemons;
- ACL reset/ownership takeover features;
- registry/system cleaners;
- a custom file manager;
- forced automatic updates;
- heavy runtimes such as .NET, Qt or WebView2.

Genia Unlocker remains a small native Win32 portable utility.

## Quality bar inherited from v0.5.0

Must remain true:
- Windows x64 native C++;
- Portable configuration with static MSVC runtime;
- no helper service/driver;
- asynchronous scans;
- Restart Manager + native handle coverage;
- target-level delete-share verification;
- verified post-action rescans;
- critical Windows process protection;
- safe reparse/junction delete behavior;
- Flat UX in Light/Dark;
- DPI-aware tool windows;
- portable preferences in `GeniaUnlocker.ini`.

## Development order

1. Design the internal target model for one-or-many paths.
2. Implement Batch scan without destructive actions.
3. Add safe Batch actions.
4. Extract/reuse the action engine for CLI.
5. Define stable CLI exit codes.
6. Add extended failure classification.
7. Regression test against all v0.5.0 scenarios.
8. Only then consider optional supporting improvements.
