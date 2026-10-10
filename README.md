# Genia Unlocker 0.5.0 — Final

A lightweight native Windows file/folder unlocker built for **Visual Studio 2026**, x64 and the current Windows 11 SDK.

## v0.5.0 Final

v0.5.0 Final is promoted from the Windows-tested RC1 codebase. The finalization changes only version/release metadata and packaging; scanner, unlock and delete logic remain unchanged from RC1.

Highlights since v0.4.1 Final:
- fixed native File ObjectTypeIndex detection and strengthened Restart Manager retries;
- verified Microsoft Word / Office locking scenarios and target-level delete-share state;
- unrelated inaccessible system handles no longer decide whether the selected target is clean;
- new Flat UX for dark/light themes, including themed About, Details and confirmation windows;
- adaptive small-size application icon and reliable Windows 11 tray hover tooltip;
- diagnostic reports with scan timing and clearer scan coverage;
- context-sensitive Delete / Unlock & Delete action;
- portable persistence for main-window size and table-column widths;
- per-monitor DPI hardening;
- executable metadata: FileVersion **0.5.0.7**, ProductVersion **0.5.0 Final**.

RC1 was accepted after real Windows testing of Word/Office locks, normal Unlock, Force Unlock, Terminate, Delete/Unlock & Delete, tray behavior and dark/light Flat UX.


## Screenshots

### Dark theme

![Genia Unlocker — dark theme](docs/screenshots/dark-main.webp)

### Light theme

![Genia Unlocker — light theme](docs/screenshots/light-main.webp)

### Detecting a locked Microsoft Word document

![Genia Unlocker detecting Microsoft Word as a file lock owner](docs/screenshots/word-lock.webp)

The screenshots show the native Win32 interface and a real lock-detection test using a sample Office document.

## Portable policy

The distributable remains:

```text
dist\GeniaUnlocker.exe
```

No installer, service, driver, .NET runtime, Qt, WebView2 or helper DLL is required. The Portable configuration uses the static MSVC runtime (`/MT`).

`GeniaUnlocker.ini` is optional and is created **beside the EXE only** if you change a portable preference such as the default delete mode. Explorer integration and Windows autostart necessarily use per-user Windows registry entries and point back to the current portable EXE.

## Main features

- Files, folders and drives as targets.
- Drag & drop.
- Explorer context-menu integration.
- Optional tray/autostart mode.
- Asynchronous scan so the UI remains responsive.
- Windows Restart Manager scan.
- Native system file-handle scan.
- Running EXE / loaded DLL image-module scan for folder-delete blockers.
- Administrator rescan when protected/elevated file-handle owners cannot be inspected.
- Per-process icon, PID, detection method and **exact locked object path**.
- Details view includes executable and all discovered locked object paths.
- Right-click process menu: Force Unlock, Terminate, open executable location, copy paths/PID.
- Critical Windows process protection.

## Unlock modes

### Unlock

Safe first attempt. Uses Restart Manager and normal window-close requests, then performs a clean rescan before reporting success.

### Force Unlock

Closes only matching **file handles** in other processes using Windows handle duplication/close semantics. It does not automatically terminate the whole process.

This mode is intentionally guarded by a warning. An application can become unstable if it expects a force-closed handle to remain valid. Genia Unlocker refuses to force-modify known critical Windows processes and rescans after the operation.

Loaded executable/DLL image mappings are not ordinary file handles. If such a mapping remains, the process may still need to be terminated before Windows can delete the object.

### Terminate

Terminates the selected blocker after confirmation. Known critical Windows processes are blocked from this action.

## Unlock & Delete workflow

The combined action is verified at every stage:

```text
Normal Unlock
    ↓
Rescan
    ↓ still locked
Force Unlock (when matching handles exist)
    ↓
Rescan
    ↓ still locked
Ask before terminating remaining blockers
    ↓
Rescan
    ↓ clean
Delete
```

By default deletion goes to the **Recycle Bin**. Settings can switch the default to permanent deletion.

If Windows still cannot delete the item, Genia Unlocker can offer **Delete on reboot**. That fallback is permanent and is clearly confirmed first. Recursive reboot scheduling does not traverse directory reparse points/junction targets.

## Settings

- Explorer context menu.
- Start with Windows (tray).
- Permanently delete by default (otherwise Recycle Bin).

The delete preference is stored in `GeniaUnlocker.ini` beside `GeniaUnlocker.exe`.

## Build in Visual Studio 2026

Open:

```text
GeniaUnlocker.sln
```

Select:

```text
Configuration: Portable
Platform:      x64
```

Then **Build → Rebuild Solution**.

Output:

```text
dist\GeniaUnlocker.exe
```

Or run:

```text
build-portable.cmd
```

The project targets the VS 2026 `v145` toolset and the latest installed Windows 10/11 SDK (`WindowsTargetPlatformVersion=10.0`).

## Useful command-line switches

```text
GeniaUnlocker.exe --target "D:\locked\item"
GeniaUnlocker.exe --tray
GeniaUnlocker.exe --install-shell
GeniaUnlocker.exe --uninstall-shell
GeniaUnlocker.exe --enable-autostart
GeniaUnlocker.exe --disable-autostart
```

Internal elevated helper switches are intentionally undocumented for normal use; the UI invokes them through UAC only when required.

## Release notes

Genia Unlocker is a user-mode utility. It deliberately does not install a kernel driver. This keeps it portable and substantially reduces system risk, but some protected-process, kernel-mode, minifilter/antivirus, memory-mapped or unusual filesystem locks may remain beyond what a single portable user-mode EXE can safely release.

## 0.4.1 Final

Release date: **2026-09-06**

- Fixed `Unlock & Delete`: unrelated inaccessible/protected processes no longer incorrectly veto the final Windows delete attempt when no blocker matching the selected target remains.
- Delete operations use the Windows shell file-operation API and are verified afterward; success is not reported while the target still exists.
- Explorer context-menu activation restores an existing Genia Unlocker window and raises it to the foreground without leaving it permanently Always-on-Top.
- Settings uses a compact custom caption with a DPI-aware inset Close button.
- File and product metadata report **0.4.1.0**.
- The Final application code is identical to the tested RC2 candidate; only release documentation/package labels changed for Final.
