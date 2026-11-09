# Changelog

## 0.6.0 Final — 2026-11-09

### Added
- Read-only Batch Scan for up to 64 unique file/folder targets.
- Multiple files/folders can be selected or dragged into the window.
- Per-target asynchronous results with progressive status, blocker PIDs and scan timing.
- Dark/light-themed Batch table, complete-path hover details and selection pane.
- Open selected / double-click bridges to the original single-target workflow.

### Release scope
- No bulk Unlock, Force Unlock, Terminate or Delete; no kernel driver/service.
- Previous single-target scanner and actions retain v0.5.0 behavior.
- CI PASS and real Windows Batch/Word navigation/Unlock verification.
- Author and repository links use GeniaPixia.
- EXE FileVersion 0.6.0.0 and ProductVersion 0.6.0 Final.
- CLI and extended failure classification are deferred.


## 0.5.0 Final — 2026-10-05

### Added
- Flat UX across the main window, Settings, About, Details and confirmation dialogs with automatic dark/light theme support.
- Target-level delete-share verification and clearer diagnostic scan coverage.
- Scan duration in status/details/reports.
- Portable persistence for main-window size and process-table column widths.
- Adaptive application icon layers for small Explorer/tray/context-menu sizes.
- Full target-path tooltip and structured Details view with Copy report.

### Fixed
- Native file-handle type detection now identifies the File ObjectTypeIndex from a probe that exists in the captured system handle snapshot.
- Restart Manager enumeration retries if the process list changes between sizing and data calls.
- Microsoft Word / Office documents that genuinely block deletion are detected correctly.
- System-wide inaccessible file-handle owners are no longer treated as evidence that the selected target is locked.
- Windows 11 tray hover tooltip explicitly restores the Genia Unlocker name.
- Dark process-table header and read-only Details surface now follow the application palette.

### Improved
- Force Unlock remains process-preserving: only matching target handles are force-closed and the application stays running when possible.
- Destructive actions use clearer themed confirmations.
- Delete / Unlock & Delete reflects verified target state.
- Per-monitor DPI behavior was hardened for tool windows.
- Light-theme surfaces and disabled/secondary control contrast were refined.

### Release status
- Promoted from v0.5.0 RC1 after successful real-Windows regression testing.
- No scanner, unlock or delete logic changes were made after RC1 acceptance; Final changes are release/version metadata and packaging only.


## 0.4.1 Final — 2026-09-06

### Fixed
- `Unlock & Delete` no longer treats unrelated inaccessible processes as proof that the selected target is still locked.
- Explorer context-menu invocation now restores and foregrounds the existing Genia Unlocker window.
- Numeric `FILEVERSION` and `PRODUCTVERSION` metadata correctly report `0.4.1.0`.

### Improved
- Delete completion is verified after Windows finishes the file operation, preventing false-success reporting while the target still exists.
- Settings uses a compact custom caption with a DPI-aware inset Close button.
- Post-unlock rescans remain authoritative before escalation to Force Unlock or process termination.

### Release status
- Promoted from v0.4.1 RC2 after successful stress-path validation, including administrator elevation and locked-folder deletion.
- No application-code changes were made between the tested RC2 candidate and v0.4.1 Final.
