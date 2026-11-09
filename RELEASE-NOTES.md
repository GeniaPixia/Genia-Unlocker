# Genia Unlocker v0.6.0 Final

**Release date:** 2026-11-09  
**Publisher:** GeniaPixia  
**Platform:** Windows 11 x64  
**Build:** native Win32 C++, portable, static MSVC runtime (`/MT`)  
**FileVersion:** `0.6.0.0`  
**ProductVersion:** `0.6.0 Final`

## New in this release

- Read-only Batch Scan for up to 64 unique file or folder targets.
- Select multiple files/folders or drag multiple targets into the main window.
- Independent asynchronous results with progressive updates and scan timing.
- Individual Clean / Locked / Locked (unknown) / Missing / Review status.
- Blocking process names and PIDs; full target paths and lock-object details.
- Themed dark/light Batch header, selection details, hover diagnostics.
- Open selected / double-click to switch back to the original single-target workflow.
- Author identity, source code and issue URLs updated to GeniaPixia.

## Verification

- Windows x64 portable build verified in GitHub Actions.
- Real-Windows Batch test checked ten files: nine clean, one blocked by Microsoft Word.
- Batch-to-single-target navigation and ordinary Unlock tested with Word;
  the Word process closed normally and the file was subsequently deleted.
- Full automated end-to-end GUI regression coverage is not claimed.

## Safety and scope

Batch is diagnostic only: no bulk Force Unlock, Terminate, Delete or Unlock.
The proven single-target action engine and its confirmations remain unchanged.
CLI commands and the proposed advanced failure-classification module are **not**
included in v0.6.0 Final.

The program installs no service, driver or heavy runtime. Some kernel/protected
process, minifilter, image-mapped and unusual filesystem locks cannot be
resolved safely from user mode.

## Portable package

Exactly five files: `GeniaUnlocker.exe`, `README.md`, `CHANGELOG.md`,
`RELEASE-NOTES.md`, and `LICENSE`.

The binary is unsigned; SmartScreen might show an unknown-publisher or
unknown-reputation warning. Official GitHub Releases provide the ZIP SHA256.

**Source:** https://github.com/GeniaPixia/Genia-Unlocker
