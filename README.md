# DelUnlock

[![Platform](https://img.shields.io/badge/Platform-Windows%20XP%20%7C%20Vista%20%7C%207%20%7C%208%20%7C%2010%20%7C%2011-blue.svg)](https://github.com/)
[![Architecture](https://img.shields.io/badge/Architecture-x64%20%2F%20x86-orange.svg)](https://github.com/)
[![Language](https://img.shields.io/badge/Language-C%2B%2B17%20%28Native%20Win32%29-00599C.svg)](https://isocpp.org/)
[![Dependencies](https://img.shields.io/badge/Dependencies-Zero%20%28Static%20CRT%29-brightgreen.svg)](https://github.com/)
[![License](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)

**DelUnlock** is a high-performance, zero-dependency, native Win32 C++ utility designed to instantly identify, unlock, rescue, or force-delete locked files and folders across all versions of Windows—from **Windows XP (x86 & x64)** to **Windows 11**.

Weighing under **300 KB**, it requires no runtime installers, no .NET Framework, no external DLLs, and leaves zero background footprint.

---

## ✨ Key Features

- **⚡ Sub-10ms Lock Scanning Engine:**
  - **Stage 1 (Vista – 11):** Windows Restart Manager (`rstrtmgr.dll`) queries active locking PIDs and memory-mapped DLL/EXE image sections.
  - **Stage 2 (XP – 11):** Direct NT Kernel Handle Scanner via `NtQuerySystemInformation` (`SystemExtendedHandleInformation` on Vista+ / `SystemHandleInformation` on XP) filtered directly to locking processes.
  - **Anti-Deadlock Worker Threads:** Duplicated handles are inspected with asynchronous timeouts to prevent hangs on stalled named pipes, network shares, or device drivers.

- **🪓 Surgical Lock Severing:**
  - Uses `DuplicateHandle(..., DUPLICATE_CLOSE_SOURCE)` to surgically release file locks directly inside the target process *without terminating the application*.
  - Also supports complete process termination (`TerminateProcess`) when required.

- **🛡️ Windows Service & Permissions Auto-Takeover:**
  - Detects if a locking process is a Windows Service via the Service Control Manager (SCM).
  - Automatically takes ownership (`SeTakeOwnershipPrivilege`, `SeBackupPrivilege`, `SeRestorePrivilege`, `SeDebugPrivilege`) and overrides restrictive DACLs if access is denied.

- **📂 Multi-Selection Batch Rendezvous Engine:**
  - When multiple files or folders are selected in Windows Explorer and sent to DelUnlock, DelUnlock utilizes an IPC rendezvous system (inter-process mutex and shared memory) to coalesce all selections into a **single consolidated window** instead of opening dozens of duplicate instances.

- **🎨 Chameleon Adaptive Native UI:**
  - **Windows XP:** Classic Luna theme with native `ComCtl32.dll` v6 visual styles.
  - **Windows Vista / 7:** Aero Glass theming.
  - **Windows 8 / 8.1:** Flat Metro styling.
  - **Windows 10 / 11:** Auto-detects Dark/Light mode, implements dark titlebars (`DWMWA_USE_IMMERSIVE_DARK_MODE`), rounded window corners (`DWMWA_WINDOW_CORNER_PREFERENCE`), and clean responsive typography.

- **💻 Full Headless CLI Mode:**
  - Automated scriptable unlocks and force deletions via Command Prompt / PowerShell for sysadmins and automation pipelines.

---

## 🖥️ Compatibility Matrix

DelUnlock is compiled targeting NT 5.02 subsystem with internal polyfills for post-XP APIs, ensuring 100% native execution across the entire Windows NT family:

| Operating System | Architecture | Supported | Notes |
|:---|:---:|:---:|:---|
| **Windows 11** | x64 / ARM64 | ✅ Yes | Full dark mode, Mica / rounded corners |
| **Windows 10** | x64 / x86 | ✅ Yes | Immersive dark titlebars, modern DPI |
| **Windows 8 / 8.1** | x64 / x86 | ✅ Yes | Native Metro controls & high-DPI scaling |
| **Windows 7** | x64 / x86 | ✅ Yes | Aero visual styling & Restart Manager |
| **Windows Vista** | x64 / x86 | ✅ Yes | Restart Manager & UAC elevation support |
| **Windows XP (SP2/SP3/x64)** | x64 / x86 | ✅ Yes | Native Luna styling, static CRT XP polyfill |
| **Windows Server 2003 – 2025** | x64 / x86 | ✅ Yes | Enterprise service detection & lock release |

---

## 🚀 Usage

### 1. Explorer Context Menu Integration
Register DelUnlock in your Windows Explorer right-click menu:

```cmd
:: Install right-click context menu (Supports single/multiple files & folders)
DelUnlock.exe --install

:: Remove context menu
DelUnlock.exe --uninstall
```

### 2. Graphical Interface (GUI)
Simply right-click any locked file or folder in Explorer and click **`DelUnlock / Force Delete...`**, or launch `DelUnlock.exe` directly to open the file selection dialog.

**Available Actions:**
1. `Unlock & Delete File (Surgical Handle Close)`
2. `Kill Locking Process & Delete File`
3. `Unlock Only (Release Lock, Keep File)`
4. `Kill Locking Process Only (Keep File)`
5. `Delete File on Next Reboot` (via `MoveFileExW`)

### 3. Command-Line Interface (CLI)
DelUnlock supports unattended scripting with zero dialog popups:

```cmd
:: Surgically close locking handles and delete file:
DelUnlock.exe --delete "C:\Path\To\LockedFile.ext"

:: Terminate locking processes and force delete file:
DelUnlock.exe --kill-and-delete "C:\Path\To\LockedFile.ext"

:: Unlock only (keep file intact):
DelUnlock.exe --unlock "C:\Path\To\LockedFile.ext"

:: Schedule file for deletion on next reboot:
DelUnlock.exe --reboot-delete "C:\Path\To\LockedFile.ext"

:: Force bypass confirmation prompts:
DelUnlock.exe --delete --force "C:\Path\To\LockedFile.ext"

:: Run built-in engine benchmark & self-test:
DelUnlock.exe --self-test
```

---

## 📁 Repository Structure

```
DelUnlock/
├── .github/
│   └── workflows/
│       └── build.yml       # GitHub Actions automated build & release pipeline
├── res/
│   ├── app.ico             # Multi-resolution application icon
│   ├── app.manifest        # UAC, DPI-Awareness & ComCtl32 v6 manifest
│   └── resource.rc         # Win32 resource script
├── src/
│   ├── config.cpp          # Settings & Explorer context menu registry verbs
│   ├── config.hpp
│   ├── main.cpp            # Entry point, batch rendezvous engine & CLI dispatcher
│   ├── ui.cpp              # Chameleon UI, Dark Mode, list views & window procedures
│   ├── ui.hpp
│   ├── unlocker.cpp        # Kernel handle scanner, Restart Manager & service controller
│   ├── unlocker.hpp
│   └── xp_compat.cpp       # Zero-overhead Windows XP CRT compatibility polyfills
├── .gitattributes          # Line ending normalizations
├── .gitignore              # Build and editor ignores
├── build.bat               # Clean one-click MSVC build script
├── LICENSE                 # MIT License
└── README.md               # Documentation
```

---

## 🔨 Building from Source

### Prerequisites
- Microsoft Visual C++ Compiler (`cl.exe`) — Available via Visual Studio 2017, 2019, 2022, or the standalone **Visual C++ Build Tools**.
- Windows SDK.

### Build Instructions
Open **x64 Native Tools Command Prompt for VS** (or standard Command Prompt) and run:

```cmd
build.bat
```

The build script will:
1. Compile resources (`rc.exe`).
2. Build all translation units with `/O2 /GL /std:c++17 /MT`.
3. Link statically against `/SUBSYSTEM:WINDOWS,5.02` with link-time code generation (`/LTCG /OPT:REF /OPT:ICF`).
4. Output the production-ready `DelUnlock.exe` in the root folder.

---

## 🛡️ Safety Guardrails

DelUnlock includes hardcoded protection rings to prevent system instability or accidental Blue Screens of Death (BSOD):
- **Protected Critical Processes:** Processes such as `System`, `smss.exe`, `csrss.exe`, `wininit.exe`, `services.exe`, `lsass.exe`, and `winlogon.exe` are flagged and cannot be terminated.
- **System File Warnings:** Files located in `%SystemRoot%` or system partitions display clear visual warnings.
- **Safe Delete Option:** Files can optionally be moved to the Windows Recycle Bin (`FO_DELETE` with `FOF_ALLOWUNDO`) rather than permanently unlinked.

---

## 📄 License

This project is licensed under the [MIT License](LICENSE).
