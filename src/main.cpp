// ============================================================================
// main.cpp
// Entry point for File Unlocker (Clean, context-menu driven, zero bloat):
//   unlocker.exe "<path>"      -> Opens the native Unlocker UI for <path>
//   unlocker.exe --install     -> Registers Explorer right-click context menu
//   unlocker.exe --uninstall   -> Removes Explorer right-click context menu
//   unlocker.exe --self-test   -> Runs automated lock-detection & surgical unlock test
//   unlocker.exe (double-click)-> First-time setup / file picker
// ============================================================================

#include "unlocker.hpp"
#include "ui.hpp"
#include "config.hpp"

#include <objbase.h>
#include <shlobj.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <commdlg.h>
#include <cstdio>

#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shlwapi.lib")

// Set up Per-Monitor V2 DPI awareness dynamically so the dialog is razor-sharp
// on high-DPI / 4K laptops while still maintaining backward compatibility with XP/7.
static void EnableHighDpiAwarenessSafe() {
    HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
    if (hUser32) {
        using PFN_SetProcessDpiAwarenessContext = BOOL(WINAPI*)(HANDLE);
        auto setCtx = reinterpret_cast<PFN_SetProcessDpiAwarenessContext>(
            GetProcAddress(hUser32, "SetProcessDpiAwarenessContext"));
        if (setCtx) {
            // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 = (HANDLE)-4
            if (setCtx(reinterpret_cast<HANDLE>(static_cast<LONG_PTR>(-4)))) {
                return;
            }
        }
        using PFN_SetProcessDPIAware = BOOL(WINAPI*)();
        auto setAware = reinterpret_cast<PFN_SetProcessDPIAware>(
            GetProcAddress(hUser32, "SetProcessDPIAware"));
        if (setAware) {
            setAware();
        }
    }
}

// Attach to parent console or redirected pipe if run from cmd.exe or PowerShell, so CLI output prints cleanly!
static HANDLE g_hConsoleOut = INVALID_HANDLE_VALUE;

static void EnsureConsoleOutput() {
    if (g_hConsoleOut != INVALID_HANDLE_VALUE) {
        return;
    }

    // Check if we already have a redirected stdout (e.g. powershell runner, cmd pipe, or file redirect)
    HANDLE hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hStdOut != INVALID_HANDLE_VALUE && hStdOut != nullptr) {
        DWORD type = GetFileType(hStdOut);
        if (type == FILE_TYPE_DISK || type == FILE_TYPE_PIPE || type == FILE_TYPE_CHAR) {
            g_hConsoleOut = hStdOut;
            return;
        }
    }

    // If not already redirected, attach to the parent cmd/powershell console
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);
        if (hStdOut == INVALID_HANDLE_VALUE || hStdOut == nullptr) {
            hStdOut = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
            SetStdHandle(STD_OUTPUT_HANDLE, hStdOut);
        }
        g_hConsoleOut = hStdOut;
        FILE* fp = nullptr;
        freopen_s(&fp, "CONOUT$", "w", stdout);
        freopen_s(&fp, "CONOUT$", "w", stderr);
        return;
    }

    // Standalone fallback: Allocate a new console if run directly
    if (AllocConsole()) {
        hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);
        g_hConsoleOut = hStdOut;
        FILE* fp = nullptr;
        freopen_s(&fp, "CONOUT$", "w", stdout);
        freopen_s(&fp, "CONOUT$", "w", stderr);
    }
}

static void ConsolePrint(const wchar_t* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    wchar_t buf[2048];
    vswprintf_s(buf, fmt, args);
    va_end(args);

    if (g_hConsoleOut != INVALID_HANDLE_VALUE && g_hConsoleOut != nullptr) {
        DWORD mode = 0;
        if (GetConsoleMode(g_hConsoleOut, &mode)) {
            DWORD written = 0;
            WriteConsoleW(g_hConsoleOut, buf, static_cast<DWORD>(wcslen(buf)), &written, nullptr);
        } else {
            int len = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
            if (len > 1) {
                std::vector<char> utf8(len);
                WideCharToMultiByte(CP_UTF8, 0, buf, -1, utf8.data(), len, nullptr, nullptr);
                DWORD written = 0;
                WriteFile(g_hConsoleOut, utf8.data(), static_cast<DWORD>(len - 1), &written, nullptr);
            }
        }
    } else {
        wprintf(L"%s", buf);
        fflush(stdout);
    }
}

static void PrintHelp() {
    ConsolePrint(L"DelUnlock \x2014 Fast & Safe File Rescue Utility\n\n");
    ConsolePrint(L"Usage:\n");
    ConsolePrint(L"  DelUnlock.exe <path1> [path2 ...]      Open GUI for one or multiple files/folders\n");
    ConsolePrint(L"  DelUnlock.exe --cli <action> <path>    Execute action directly via command line\n");
    ConsolePrint(L"  DelUnlock.exe --scan <path>            List processes locking a file in console\n");
    ConsolePrint(L"  DelUnlock.exe --install                Add right-click context menu\n");
    ConsolePrint(L"  DelUnlock.exe --uninstall              Remove right-click context menu\n");
    ConsolePrint(L"  DelUnlock.exe --self-test              Run engine benchmark & self-test\n");
    ConsolePrint(L"  DelUnlock.exe --help                   Show this help screen\n\n");
    ConsolePrint(L"CLI Actions:\n");
    ConsolePrint(L"  unlock-delete   Surgically sever remote handles and delete file\n");
    ConsolePrint(L"  kill-delete     Terminate locking process and delete file\n");
    ConsolePrint(L"  unlock-only     Surgically sever remote handles (keep file)\n");
    ConsolePrint(L"  kill-only       Terminate locking process (keep file)\n");
    ConsolePrint(L"  reboot-delete   Schedule file deletion on next Windows reboot\n");
    ConsolePrint(L"  rename          Unlock and rename: --cli rename <file> <newName>\n");
    ConsolePrint(L"  move            Unlock and move:   --cli move <file> <destFolder>\n");
    ConsolePrint(L"  take-ownership  Seize NTFS ownership and grant Full Control\n");
    ConsolePrint(L"  stop-service    Stop registered Windows service: --cli stop-service <svcName>\n");
    ConsolePrint(L"  restart-explorer Cleanly restart Windows Explorer shell\n\n");
    ConsolePrint(L"Examples:\n");
    ConsolePrint(L"  DelUnlock.exe --scan \"C:\\file.txt\"\n");
    ConsolePrint(L"  DelUnlock.exe --cli unlock-delete \"C:\\locked.pdf\"\n");
    ConsolePrint(L"  DelUnlock.exe --cli rename \"C:\\game\\dxgi.dll\" \"dxgi.dll.bak\"\n");
    ConsolePrint(L"  DelUnlock.exe --cli take-ownership \"C:\\Windows.old\"\n");
}

static int RunCliScan(const std::wstring& targetPath) {
    EnsureConsoleOutput();
    ConsolePrint(L"Scanning locks for: %s\n", targetPath.c_str());

    unlocker::ScanResult res = unlocker::ScanFileLocks(targetPath, true);
    ConsolePrint(L"Scan completed in %.2f ms (Locked: %s)\n\n", res.scanTimeMs, res.isCurrentlyLocked ? L"YES" : L"NO");

    if (res.processes.empty()) {
        ConsolePrint(L"No user-mode processes found holding this file.\n");
        return 0;
    }

    ConsolePrint(L"Locking Processes (%zu):\n", res.processes.size());
    ConsolePrint(L"--------------------------------------------------------------------\n");
    ConsolePrint(L"PID\tProcess Name\t\tType\t\tFriendly Name\n");
    ConsolePrint(L"--------------------------------------------------------------------\n");
    for (const auto& p : res.processes) {
        const wchar_t* kindStr = p.isService ? L"Windows Service" :
            ((p.kind == unlocker::LockKind::FileHandle || !p.handles.empty()) ? L"File Handle" : L"Loaded DLL/EXE");
        ConsolePrint(L"%lu\t%-20s\t%-15s\t%s\n", p.pid, p.processName.c_str(), kindStr, p.friendlyName.c_str());
    }
    return 0;
}

static int RunCliAction(const std::wstring& actionName, const std::wstring& targetPath, const std::wstring& extraParam = L"") {
    EnsureConsoleOutput();

    if (actionName == L"restart-explorer") {
        ConsolePrint(L"[+] Restarting Windows Explorer...\n");
        std::wstring err;
        if (unlocker::RestartWindowsExplorer(&err)) {
            ConsolePrint(L"[SUCCESS] Windows Explorer restarted successfully.\n");
            return 0;
        } else {
            ConsolePrint(L"[FAILED] %s\n", err.c_str());
            return 1;
        }
    }

    if (actionName == L"stop-service") {
        ConsolePrint(L"[+] Stopping Windows Service '%s'...\n", targetPath.c_str());
        std::wstring err;
        if (unlocker::StopWindowsService(targetPath, &err)) {
            ConsolePrint(L"[SUCCESS] Service stopped successfully.\n");
            return 0;
        } else {
            ConsolePrint(L"[FAILED] %s\n", err.c_str());
            return 1;
        }
    }

    if (actionName == L"take-ownership") {
        ConsolePrint(L"[+] Seizing ownership and resetting DACL on: %s...\n", targetPath.c_str());
        std::wstring err;
        if (unlocker::TakeOwnershipAndGrantAccess(targetPath, &err)) {
            ConsolePrint(L"[SUCCESS] Ownership acquired and Full Control granted.\n");
            return 0;
        } else {
            ConsolePrint(L"[FAILED] %s\n", err.c_str());
            return 1;
        }
    }

    unlocker::ActionType act;
    if (actionName == L"unlock-delete") act = unlocker::ActionType::UnlockAndDelete;
    else if (actionName == L"kill-delete") act = unlocker::ActionType::KillAndDelete;
    else if (actionName == L"unlock-only") act = unlocker::ActionType::UnlockOnly;
    else if (actionName == L"kill-only") act = unlocker::ActionType::KillOnly;
    else if (actionName == L"reboot-delete") act = unlocker::ActionType::DeleteOnReboot;
    else if (actionName == L"rename") act = unlocker::ActionType::UnlockAndRename;
    else if (actionName == L"move") act = unlocker::ActionType::UnlockAndMove;
    else {
        ConsolePrint(L"[!] Unknown action: %s\n", actionName.c_str());
        ConsolePrint(L"    Valid actions: unlock-delete, kill-delete, unlock-only, kill-only, reboot-delete,\n");
        ConsolePrint(L"                   rename, move, take-ownership, stop-service, restart-explorer\n");
        return 1;
    }

    ConsolePrint(L"[+] Executing '%s' on %s...\n", actionName.c_str(), targetPath.c_str());
    unlocker::ScanResult scan = unlocker::ScanFileLocks(targetPath, true);
    unlocker::ActionResult res = unlocker::ExecuteUnlockerAction(targetPath, act, scan.processes, false, nullptr, extraParam);

    if (res.succeeded) {
        ConsolePrint(L"[SUCCESS] %s\n", res.statusMessage.c_str());
        return 0;
    } else {
        ConsolePrint(L"[FAILED] %s\n", res.statusMessage.c_str());
        return 1;
    }
}
static int RunAutomatedBenchmarkAndSelfTest() {
    EnsureConsoleOutput();

    ConsolePrint(L"============================================================\n");
    ConsolePrint(L" DelUnlock - Engine Benchmark & Self-Test\n");
    ConsolePrint(L"============================================================\n");

    wchar_t tempDir[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tempDir);
    std::wstring testFile = std::wstring(tempDir) + L"delunlock_benchmark_test.tmp";

    HANDLE hInit = CreateFileW(testFile.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hInit != INVALID_HANDLE_VALUE) {
        const char* payload = "Test file payload.\r\n";
        DWORD written = 0;
        WriteFile(hInit, payload, static_cast<DWORD>(strlen(payload)), &written, nullptr);
        CloseHandle(hInit);
    }

    // Spawn dummy powershell process locking the file exclusively
    wchar_t cmdLine[MAX_PATH * 3] = {};
    wsprintfW(cmdLine,
        L"powershell.exe -NoProfile -WindowStyle Hidden -Command \"$f = [System.IO.File]::Open('%s', 'Open', 'ReadWrite', 'None'); Start-Sleep -Seconds 30\"",
        testFile.c_str());

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, cmdLine, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        ConsolePrint(L"[!] Failed to spawn test process.\n");
        return 1;
    }

    // Wait for the lock to take effect
    bool locked = false;
    for (int i = 0; i < 60; ++i) {
        if (unlocker::IsFileActivelyLocked(testFile)) {
            locked = true;
            break;
        }
        Sleep(50);
    }

    if (!locked) {
        ConsolePrint(L"[!] Failed to acquire test lock.\n");
        TerminateProcess(pi.hProcess, 0);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        DeleteFileW(testFile.c_str());
        return 1;
    }

    ConsolePrint(L"[+] Test file locked exclusively by PID %lu: %s\n", pi.dwProcessId, testFile.c_str());

    unlocker::ScanResult scan = unlocker::ScanFileLocks(testFile, true);
    ConsolePrint(L"[+] Scan completed in %.2f ms\n", scan.scanTimeMs);
    ConsolePrint(L"[+] Found %zu locking process(es):\n", scan.processes.size());
    for (const auto& p : scan.processes) {
        ConsolePrint(L"    - %s (PID %lu) | %s | Handles: %zu\n",
                p.processName.c_str(), p.pid, p.friendlyName.c_str(), p.handles.size());
    }

    std::wstring err;
    int closed = unlocker::ForceCloseHandlesForFile(testFile, scan.processes, &err);
    ConsolePrint(L"[+] ForceCloseHandlesForFile() closed %d remote handle(s).\n", closed);

    bool stillLocked = unlocker::IsFileActivelyLocked(testFile);
    ConsolePrint(L"[+] IsFileActivelyLocked() after surgical unlock: %s\n", stillLocked ? L"YES (FAILED)" : L"NO (UNLOCKED!)");

    bool deleted = unlocker::DeleteTargetFile(testFile, false, &err);
    ConsolePrint(L"[+] DeleteTargetFile() succeeded: %s\n", deleted ? L"YES" : L"NO");

    TerminateProcess(pi.hProcess, 0);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    ConsolePrint(L"============================================================\n");
    return (closed > 0 && !stillLocked && deleted) ? 0 : 1;
}

// ----------------------------------------------------------------------------
// Explorer Multi-Select Batch Aggregator
// ----------------------------------------------------------------------------
// When a user selects multiple files or folders in Windows Explorer and clicks
// the context menu, Windows Explorer spawns N separate processes in a rapid burst
// (<50ms).
// To prevent opening 10-20 separate dialog windows on the user's screen, we use
// an instant Mutex + WM_COPYDATA rendezvous. The first process (Master) holds
// the mutex and waits a tiny debouncing window (100ms) collecting paths from the
// sibling processes. The sibling processes (Slaves) send their paths to the Master
// and terminate immediately. The Master then opens ONE unified dialog containing
// all selected files/folders!

static const wchar_t* kBatchWindowClassName = L"DelUnlockBatchClass_7B9A";
static const wchar_t* kBatchWindowName      = L"DelUnlockBatchAggregator";
static const wchar_t* kBatchMutexName       = L"Local\\DelUnlock_BatchMutex_7B9A";
static const UINT     WM_BATCH_DONE         = WM_USER + 42;

struct BatchCollectorContext {
    std::vector<std::wstring> paths;
    bool done = false;
};

static LRESULT CALLBACK BatchAggregatorWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* ctx = reinterpret_cast<BatchCollectorContext*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
        case WM_COPYDATA: {
            if (ctx && lParam) {
                auto* cds = reinterpret_cast<COPYDATASTRUCT*>(lParam);
                if (cds->lpData && cds->cbData > 0) {
                    const wchar_t* receivedPath = reinterpret_cast<const wchar_t*>(cds->lpData);
                    ctx->paths.emplace_back(receivedPath);
                    // Reset debounce timer on each incoming sibling process
                    SetTimer(hwnd, 1, 80, nullptr);
                }
            }
            return TRUE;
        }

        case WM_TIMER: {
            if (wParam == 1) {
                KillTimer(hwnd, 1);
                if (ctx) ctx->done = true;
                // Post wake-up message so GetMessage wakes without quitting the thread!
                PostMessageW(hwnd, WM_BATCH_DONE, 0, 0);
            }
            return 0;
        }

        case WM_BATCH_DONE: {
            return 0;
        }

        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

static std::vector<std::wstring> AggregateExplorerBatch(HINSTANCE hInstance, const std::vector<std::wstring>& initialTargets) {
    if (initialTargets.empty()) return {};

    HANDLE hMutex = CreateMutexW(nullptr, FALSE, kBatchMutexName);
    DWORD waitRes = WaitForSingleObject(hMutex, 0);

    if (waitRes == WAIT_OBJECT_0 || waitRes == WAIT_ABANDONED) {
        // --- MASTER PROCESS ---
        WNDCLASSEXW wc = {};
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = BatchAggregatorWndProc;
        wc.hInstance     = hInstance;
        wc.lpszClassName = kBatchWindowClassName;
        RegisterClassExW(&wc);

        BatchCollectorContext ctx;
        ctx.paths = initialTargets;

        // Hidden top-level window so FindWindow can locate it across processes
        HWND hAggregator = CreateWindowExW(
            WS_EX_TOOLWINDOW,
            kBatchWindowClassName,
            kBatchWindowName,
            WS_POPUP,
            -10000, -10000, 0, 0,
            nullptr, nullptr, hInstance, nullptr
        );

        if (hAggregator) {
            SetWindowLongPtrW(hAggregator, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&ctx));
            // 90ms debounce: plenty of time for Explorer to spawn all sibling processes in the burst
            SetTimer(hAggregator, 1, 90, nullptr);

            MSG msg = {};
            while (!ctx.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }

            DestroyWindow(hAggregator);
        }

        // Drain any pending messages and purge any stray WM_QUIT
        MSG drainMsg = {};
        while (PeekMessageW(&drainMsg, nullptr, 0, 0, PM_REMOVE)) {
            if (drainMsg.message != WM_QUIT) {
                TranslateMessage(&drainMsg);
                DispatchMessageW(&drainMsg);
            }
        }

        UnregisterClassW(kBatchWindowClassName, hInstance);
        ReleaseMutex(hMutex);
        CloseHandle(hMutex);

        return ctx.paths;
    } else {
        // --- SIBLING (SLAVE) PROCESS ---
        // Find the Master Aggregator window. Explorer spawns processes in parallel,
        // so if the Master is still creating its window, retry for up to 500ms.
        HWND hMaster = nullptr;
        for (int i = 0; i < 50; ++i) {
            hMaster = FindWindowW(kBatchWindowClassName, kBatchWindowName);
            if (hMaster) break;
            Sleep(10);
        }

        if (hMaster) {
            for (const auto& path : initialTargets) {
                COPYDATASTRUCT cds = {};
                cds.dwData = 1;
                cds.cbData = static_cast<DWORD>((path.size() + 1) * sizeof(wchar_t));
                cds.lpData = const_cast<wchar_t*>(path.c_str());
                SendMessageW(hMaster, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cds));
            }
        }

        CloseHandle(hMutex);
        // Slave process exits immediately without opening any UI!
        return {};
    }
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE /*hPrevInstance*/, LPWSTR /*lpCmdLine*/, int /*nCmdShow*/) {
    EnableHighDpiAwarenessSafe();

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 1;

    // 1. Process command line arguments
    if (argc >= 2) {
        std::wstring arg1 = argv[1];

        if (arg1 == L"--install" || arg1 == L"/install") {
            bool ok = unlocker::InstallContextMenu();
            unlocker::AppConfig cfg = unlocker::LoadConfig();
            cfg.contextMenuEnabled = true;
            cfg.isFirstRun = false;
            unlocker::SaveConfig(cfg);
            EnsureConsoleOutput();
            if (g_hConsoleOut != INVALID_HANDLE_VALUE) {
                ConsolePrint(L"[+] %s\n", ok ? L"Right-click context menu installed successfully!" : L"Failed to install context menu.");
            } else {
                MessageBoxW(nullptr, L"Right-click context menu installed successfully!", L"DelUnlock", ok ? MB_ICONINFORMATION : MB_ICONERROR);
            }
            LocalFree(argv);
            return ok ? 0 : 1;
        }

        if (arg1 == L"--uninstall" || arg1 == L"/uninstall") {
            unlocker::UninstallContextMenu();
            unlocker::AppConfig cfg = unlocker::LoadConfig();
            cfg.contextMenuEnabled = false;
            unlocker::SaveConfig(cfg);
            EnsureConsoleOutput();
            if (g_hConsoleOut != INVALID_HANDLE_VALUE) {
                ConsolePrint(L"[+] Right-click context menu removed successfully.\n");
            } else {
                MessageBoxW(nullptr, L"Right-click context menu removed successfully.", L"DelUnlock", MB_ICONINFORMATION);
            }
            LocalFree(argv);
            return 0;
        }

        if (arg1 == L"--self-test") {
            LocalFree(argv);
            return RunAutomatedBenchmarkAndSelfTest();
        }

        if (arg1 == L"--help" || arg1 == L"-h" || arg1 == L"/?") {
            EnsureConsoleOutput();
            PrintHelp();
            LocalFree(argv);
            return 0;
        }

        if (arg1 == L"--scan" && argc >= 3) {
            std::wstring scanTarget = argv[2];
            LocalFree(argv);
            return RunCliScan(scanTarget);
        }

        if (arg1 == L"--cli" && argc >= 3) {
            std::wstring action = argv[2];
            std::wstring target = (argc >= 4) ? argv[3] : L"";
            std::wstring extra  = (argc >= 5) ? argv[4] : L"";
            LocalFree(argv);
            return RunCliAction(action, target, extra);
        }

        // Multi-file / Multi-folder selection or single file selection!
        // When user selects 2+ files/folders in Windows Explorer, all paths are passed as arguments!
        std::vector<std::wstring> targets;
        for (int i = 1; i < argc; ++i) {
            if (argv[i] && argv[i][0] != L'-') {
                targets.emplace_back(argv[i]);
            }
        }
        LocalFree(argv);

        std::vector<std::wstring> collected = AggregateExplorerBatch(hInstance, targets);
        if (collected.empty()) {
            // Sibling process that forwarded its path to the master: exit cleanly!
            return 0;
        }

        if (collected.size() == 1) {
            return unlocker::ShowUnlockerDialog(hInstance, collected[0], nullptr);
        } else if (collected.size() > 1) {
            return unlocker::ShowUnlockerDialog(hInstance, collected, nullptr);
        }
        return 0;
    }

    LocalFree(argv);

    // 2. If double-clicked with no arguments:
    unlocker::AppConfig cfg = unlocker::LoadConfig();

    if (cfg.isFirstRun) {
        if (!unlocker::ShowFirstRunSetupDialog(hInstance)) {
            return 0;
        }
        cfg = unlocker::LoadConfig();
    } else {
        if (cfg.contextMenuEnabled) {
            unlocker::InstallContextMenu();
        }
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int res = unlocker::ShowHomeScreen(hInstance);
    CoUninitialize();
    return res;
}
