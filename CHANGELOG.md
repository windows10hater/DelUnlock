# Changelog

All notable changes to the DelUnlock project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [1.1.0] - 2026-10-29

### Added
- **Native Home Screen & Dashboard**:
  - When double-clicking `DelUnlock.exe` with no target arguments, DelUnlock now opens a clean, adaptive Home Screen instead of immediately prompting with a Windows Explorer file dialog.
  - Features an application header, elevation status badge, quick actions, and integration toggles.
- **Drag-and-Drop File/Folder Unlocking**:
  - Drag and drop one or multiple locked files or folders directly onto the DelUnlock window via `WM_DROPFILES` to trigger lock inspection immediately.
- **Dedicated Browse Actions**:
  - Added dedicated "Select File to Unlock..." and "Select Folder to Unlock..." buttons inside the Home Screen card.
- **Context Menu Integration Toggle**:
  - Live interactive checkbox on the Home Screen to install or remove the Explorer right-click menu ("DelUnlock / Force Delete...") on the fly.
- **Windows XP 32-bit (x86) Compatibility Layer**:
  - Created `src/xp_compat_x86.asm` utilizing MASM to provide internal forwarders for modern MSVC CRT imports (`InitializeCriticalSectionEx`, `AcquireSRWLockExclusive`, `ReleaseSRWLockExclusive`, `SleepConditionVariableSRW`, `WakeAllConditionVariable`).
  - Completely removed post-XP static imports from `KERNEL32.dll` in `DelUnlock-x86.exe`, restoring true native execution on Windows XP 32-bit (SP2/SP3).
- **Stage 3 In-Memory Module Scanner**:
  - Added an elevated-mode inspection pass via `EnumProcessModulesEx` / `EnumProcessModules` to detect running processes mapping system DLLs (such as `shell32.dll` and `ntdll.dll`).
  - Added a subtle hint in the interface when inspecting system libraries in non-elevated mode.

### Changed
- Refactored `main.cpp` to cleanly separate direct file execution (via context menu or CLI) from interactive standalone execution.
- Maintained zero-click bypass: launching DelUnlock via Explorer right-click or command-line parameters continues to bypass the Home Screen and opens the unlocker interface directly.
- Updated official SHA-256 release checksums for both x64 and x86 builds.

---

## [1.0.0] - 2026-10-24

### Added
- Initial public release of DelUnlock.
- Standalone, driverless Win32 C++ utility targeting Windows XP through Windows 11.
- Sub-millisecond lock detection combining Restart Manager and NT Kernel handle enumeration.
- Surgical handle severing via `DuplicateHandle(..., DUPLICATE_CLOSE_SOURCE)`.
- Batch multi-select consolidation using inter-process mutex rendezvous.
- Chameleon OS-adaptive user interface supporting Windows 10/11 Dark/Light mode, Windows 7 Aero, and Windows XP Luna.
- Full command-line interface (CLI) for headless and scripted usage.
- Hardcoded critical process safeguards to prevent operating system instability.
- Licensed under the Apache License, Version 2.0.
