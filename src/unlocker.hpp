// ============================================================================
// unlocker.hpp
// Core lock-detection, surgical handle closing, and file rescue engine.
//
// Zero static dependencies on Vista+ APIs: everything post-XP (Restart Manager,
// SystemExtendedHandleInformation, QueryFullProcessImageNameW) is resolved at
// runtime via GetProcAddress so the exact same binary runs on a dusty 2002
// Windows XP machine and a fresh Windows 11 workstation.
// ============================================================================

#pragma once

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <string>
#include <vector>
#include <cstdint>

namespace unlocker {

// What the user can choose in the dropdown menu.
enum class ActionType : int {
    UnlockAndDelete = 0, // Surgically close remote file handles + delete/recycle file
    KillAndDelete   = 1, // Terminate locking process(es) + delete/recycle file
    UnlockOnly      = 2, // Just sever the file handles so the user can edit/move/rename
    KillOnly        = 3, // Terminate locking process(es), leave the file untouched
    DeleteOnReboot  = 4, // Schedule deletion on next boot via MoveFileExW
    UnlockAndRename = 5, // Surgically close remote handles and rename file (Gamers/Modders)
    UnlockAndMove   = 6  // Surgically close remote handles and move file to another folder
};

// How a process is holding onto our file.
enum class LockKind {
    FileHandle,   // Standard open HANDLE (can be surgically closed via DUPLICATE_CLOSE_SOURCE!)
    LoadedModule, // Mapped as a DLL or running EXE image section (cannot close via handle; needs process exit)
    Unknown       // Restart Manager reported it, handle scan hasn't run yet
};

// Represents a single open kernel handle inside a remote process.
struct RemoteHandle {
    DWORD     pid        = 0;
    ULONG_PTR handleVal  = 0;
    DWORD     accessMask = 0;
};

// Everything the UI needs to know about a process that is holding our file hostage.
struct LockingProcess {
    DWORD                     pid           = 0;
    std::wstring              processName;  // e.g. "Code.exe"
    std::wstring              friendlyName; // e.g. "Visual Studio Code" (or processName if unavailable)
    std::wstring              fullImagePath;// Full path to the locking .exe for extracting its icon
    LockKind                  kind          = LockKind::Unknown;
    bool                      isSystemProtected = false; // True for csrss, lsass, System, etc.
    bool                      isExplorer    = false;     // True if explorer.exe or dllhost/prevhost thumbnailer
    bool                      isService     = false;     // True if backing a registered Windows Service
    std::wstring              serviceName;               // e.g. "Spooler"
    std::wstring              serviceDisplayName;        // e.g. "Print Spooler"
    HWND                      mainWindow    = nullptr;   // Top-level visible window (if any) for "Switch To" QoL
    std::vector<ULONG_PTR>    handles;      // Populated by the handle scanner
};

// Result of scanning a path for locks.
struct ScanResult {
    std::wstring                targetPath;
    bool                        exists        = false;
    bool                        isDirectory   = false;
    bool                        isCurrentlyLocked = false;
    double                      scanTimeMs    = 0.0;
    std::vector<LockingProcess> processes;
};

// Result of executing an action from the dropdown.
struct ActionResult {
    bool         succeeded     = false;
    int          handlesClosed = 0;
    int          procsKilled   = 0;
    bool         fileDeleted   = false;
    bool         rebootScheduled = false;
    std::wstring statusMessage;
};

// Enable SeDebugPrivilege on our current process token if we have admin rights.
// Without this, processes running as other users/services laugh at our OpenProcess calls.
bool EnableDebugPrivilege();

// Quick check: is this file actually locked right now according to CreateFileW(shareMode=0)?
bool IsFileActivelyLocked(const std::wstring& filePath);

// Fast two-stage scan:
// 1. Uses Restart Manager (RstrtMgr.dll) on Vista..11 for fast PID & DLL/mapped-section discovery.
// 2. Performs a filtered NtQuerySystemInformation handle scan (with anti-pipe-deadlock worker thread)
//    when surgical handle values are needed or when running on Windows XP.
ScanResult ScanFileLocks(const std::wstring& filePath, bool resolveExactHandles = true);

// Surgically closes all open file handles to `filePath` inside the target processes
// using DuplicateHandle(..., DUPLICATE_CLOSE_SOURCE).
// Returns the number of handles successfully severed.
int ForceCloseHandlesForFile(const std::wstring& filePath, std::vector<LockingProcess>& processes, std::wstring* outError = nullptr);

// Terminates locking processes (skipping protected OS processes like csrss/lsass).
// If explorer.exe is in the list and `allowExplorerRestart` is true, restarts Explorer cleanly.
int KillLockingProcesses(const std::vector<LockingProcess>& processes, bool allowExplorerRestart, std::wstring* outError = nullptr);

// Deletes or sends the file/folder to the Recycle Bin.
// Strips read-only attributes first if needed.
bool DeleteTargetFile(const std::wstring& filePath, bool sendToRecycleBin, std::wstring* outError = nullptr);

// Schedules the file to be nuked by smss.exe at boot time before any user-mode process starts.
bool ScheduleDeleteOnReboot(const std::wstring& filePath, std::wstring* outError = nullptr);

// High-level dispatcher that runs the user's chosen dropdown action.
// If `explorerConflictDialog` is valid (from our background interceptor), it can also click
// "Try Again" (IDRETRY) directly on Explorer's hidden conflict dialog so Explorer finishes
// the move/recycle/delete operation natively!
ActionResult ExecuteUnlockerAction(
    const std::wstring& filePath,
    ActionType action,
    std::vector<LockingProcess>& processes,
    bool useRecycleBin,
    HWND explorerConflictDialog = nullptr,
    const std::wstring& extraParam = L""
);

// UAC Elevation Helper: checks if current process has Administrator token
bool IsRunningAsAdmin();

// Relaunches unlocker with elevated privileges (verb "runas") passing exact targets
bool RelaunchAsAdmin(HWND hwndParent, const std::vector<std::wstring>& targetPaths);

// Seizes ownership and grants Administrators/Current User Full Control on stubborn files
bool TakeOwnershipAndGrantAccess(const std::wstring& path, std::wstring* outError = nullptr);

// Renames a file or folder safely after handles have been severed
bool RenameTargetFile(const std::wstring& oldPath, const std::wstring& newPath, std::wstring* outError = nullptr);

// Moves a file or folder safely into a destination directory after handles have been severed
bool MoveTargetFile(const std::wstring& oldPath, const std::wstring& destFolder, std::wstring* outError = nullptr);

// Stops a registered Windows Service cleanly via SCM to prevent 100ms auto-restart loops
bool StopWindowsService(const std::wstring& serviceName, std::wstring* outError = nullptr);

// Cleanly terminates and relaunches Windows Explorer to clear stuck shell thumbnail locks
bool RestartWindowsExplorer(std::wstring* outError = nullptr);

// Helper: brings a locking process's main window to the foreground so the user can save their work.
bool BringProcessWindowToFront(DWORD pid, HWND knownHwnd = nullptr);

// Helper: opens Windows Explorer with the locking .exe highlighted ("Open File Location").
void OpenProcessFileLocation(const std::wstring& exePath);

// Helper: reads the "FileDescription" from an .exe or .dll file's version resource
std::wstring GetFileDescription(const std::wstring& filePath);

// Helper: checks if a file/path belongs to critical Windows system folders or system files
bool IsSystemCriticalFile(const std::wstring& filePath);

// Checks if a process name or PID is a critical Windows process that must never be killed
// unless the user really enjoys looking at a Blue Screen of Death.
bool IsProtectedSystemProcess(DWORD pid, const std::wstring& procName);

} // namespace unlocker
