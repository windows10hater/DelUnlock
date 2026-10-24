// ============================================================================
// config.cpp
// Implementation of INI management, Context Menu Registration, and Settings UI.
// ============================================================================

#include "config.hpp"
#include <shlwapi.h>
#include <commctrl.h>
#include <uxtheme.h>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "advapi32.lib")

namespace unlocker {

std::wstring GetExecutablePath() {
    wchar_t buf[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    return std::wstring(buf);
}

std::wstring GetIniFilePath() {
    std::wstring exe = GetExecutablePath();
    wchar_t path[MAX_PATH * 2] = {};
    wcscpy_s(path, exe.c_str());
    PathRemoveFileSpecW(path);
    PathAppendW(path, L"DelUnlock.ini");

    // If DelUnlock.ini doesn't exist yet but unlocker.ini is present, migrate
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
        wchar_t oldPath[MAX_PATH * 2] = {};
        wcscpy_s(oldPath, exe.c_str());
        PathRemoveFileSpecW(oldPath);
        PathAppendW(oldPath, L"unlocker.ini");
        if (GetFileAttributesW(oldPath) != INVALID_FILE_ATTRIBUTES) {
            CopyFileW(oldPath, path, FALSE);
        }
    }
    return std::wstring(path);
}

AppConfig LoadConfig() {
    AppConfig cfg;
    std::wstring ini = GetIniFilePath();

    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) {
        cfg.isFirstRun = true;
        cfg.contextMenuEnabled = true;
        return cfg;
    }

    int firstRun = GetPrivateProfileIntW(L"General", L"FirstRun", 1, ini.c_str());
    int ctxMenu  = GetPrivateProfileIntW(L"General", L"ContextMenu", 1, ini.c_str());

    cfg.isFirstRun         = (firstRun != 0);
    cfg.contextMenuEnabled = (ctxMenu != 0);
    return cfg;
}

void SaveConfig(const AppConfig& cfg) {
    std::wstring ini = GetIniFilePath();
    WritePrivateProfileStringW(L"General", L"FirstRun", cfg.isFirstRun ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"General", L"ContextMenu", cfg.contextMenuEnabled ? L"1" : L"0", ini.c_str());
}

static bool SetRegString(HKEY hRoot, const wchar_t* subKey, const wchar_t* valueName, const std::wstring& data) {
    HKEY hKey = nullptr;
    if (RegCreateKeyExW(hRoot, subKey, 0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    LSTATUS st = RegSetValueExW(
        hKey,
        valueName,
        0,
        REG_SZ,
        reinterpret_cast<const BYTE*>(data.c_str()),
        static_cast<DWORD>((data.size() + 1) * sizeof(wchar_t))
    );
    RegCloseKey(hKey);
    return (st == ERROR_SUCCESS);
}

bool IsContextMenuInstalled() {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\*\\shell\\DelUnlock\\command", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        RegCloseKey(hKey);
        return true;
    }
    return false;
}

bool InstallContextMenu() {
    std::wstring exePath = GetExecutablePath();
    std::wstring cmdLine = L"\"" + exePath + L"\" \"%1\"";

    // Clean up any legacy FileUnlocker keys
    SHDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Classes\\*\\shell\\FileUnlocker");
    SHDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Classes\\Directory\\shell\\FileUnlocker");
    SHDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Classes\\Folder\\shell\\FileUnlocker");
    SHDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Classes\\AllFilesystemObjects\\shell\\FileUnlocker");

    // Targets:
    // 1. All Files (*)
    // 2. All Directory Folders (Directory)
    // 3. All Folder and Drive types (Folder)
    // 4. Multi-Select / Shell items (AllFilesystemObjects)
    const wchar_t* targets[] = {
        L"Software\\Classes\\*\\shell\\DelUnlock",
        L"Software\\Classes\\Directory\\shell\\DelUnlock",
        L"Software\\Classes\\Folder\\shell\\DelUnlock",
        L"Software\\Classes\\AllFilesystemObjects\\shell\\DelUnlock"
    };

    bool ok = true;
    for (const wchar_t* baseKey : targets) {
        ok &= SetRegString(HKEY_CURRENT_USER, baseKey, nullptr, L"DelUnlock / Force Delete...");
        std::wstring iconVal = L"\"" + exePath + L"\",0";
        ok &= SetRegString(HKEY_CURRENT_USER, baseKey, L"Icon", iconVal);

        // Tell Windows Explorer that multi-select is allowed so it invokes with files grouped
        SetRegString(HKEY_CURRENT_USER, baseKey, L"MultiSelectModel", L"Player");

        std::wstring cmdKey = std::wstring(baseKey) + L"\\command";
        ok &= SetRegString(HKEY_CURRENT_USER, cmdKey.c_str(), nullptr, cmdLine);
    }
    return ok;
}

bool UninstallContextMenu() {
    SHDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Classes\\*\\shell\\DelUnlock");
    SHDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Classes\\Directory\\shell\\DelUnlock");
    SHDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Classes\\Folder\\shell\\DelUnlock");
    SHDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Classes\\AllFilesystemObjects\\shell\\DelUnlock");

    // Also purge legacy key names
    SHDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Classes\\*\\shell\\FileUnlocker");
    SHDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Classes\\Directory\\shell\\FileUnlocker");
    SHDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Classes\\Folder\\shell\\FileUnlocker");
    SHDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Classes\\AllFilesystemObjects\\shell\\FileUnlocker");
    return true;
}

// ----------------------------------------------------------------------------
// First-Run Setup Window
// ----------------------------------------------------------------------------

struct FirstRunContext {
    HWND hCheckContext = nullptr;
    bool confirmed     = false;
    bool enableContext = true;
};

static LRESULT CALLBACK FirstRunWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* ctx = reinterpret_cast<FirstRunContext*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
        case WM_CREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            ctx = reinterpret_cast<FirstRunContext*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));

            HFONT hFontBold = CreateFontW(-16, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            HFONT hFontBody = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");

            HWND hTitle = CreateWindowExW(0, L"STATIC", L"Welcome to DelUnlock",
                WS_CHILD | WS_VISIBLE | SS_LEFT, 24, 20, 390, 26, hwnd, nullptr, cs->hInstance, nullptr);
            SendMessageW(hTitle, WM_SETFONT, reinterpret_cast<WPARAM>(hFontBold), TRUE);

            const wchar_t* descText =
                L"When Windows Explorer says a file is in use and refuses to delete or move it, "
                L"DelUnlock lets you inspect the locking application and surgically sever the lock in milliseconds.\r\n\r\n"
                L"How to use:\r\n"
                L"  1. Right-click any locked file or folder in Windows Explorer\r\n"
                L"  2. Click 'DelUnlock / Force Delete...'\r\n"
                L"  3. Select your action from the dropdown and click Execute!";

            HWND hDesc = CreateWindowExW(0, L"STATIC", descText,
                WS_CHILD | WS_VISIBLE | SS_LEFT, 24, 56, 390, 115, hwnd, nullptr, cs->hInstance, nullptr);
            SendMessageW(hDesc, WM_SETFONT, reinterpret_cast<WPARAM>(hFontBody), TRUE);

            ctx->hCheckContext = CreateWindowExW(0, L"BUTTON", L"Add 'DelUnlock / Force Delete...' to right-click menu (Recommended)",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                24, 185, 400, 24, hwnd, reinterpret_cast<HMENU>(101), cs->hInstance, nullptr);
            SendMessageW(ctx->hCheckContext, WM_SETFONT, reinterpret_cast<WPARAM>(hFontBody), TRUE);
            SendMessageW(ctx->hCheckContext, BM_SETCHECK, BST_CHECKED, 0);

            HWND hBtnStart = CreateWindowExW(0, L"BUTTON", L"Get Started",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                300, 225, 110, 32, hwnd, reinterpret_cast<HMENU>(IDOK), cs->hInstance, nullptr);
            SendMessageW(hBtnStart, WM_SETFONT, reinterpret_cast<WPARAM>(hFontBold), TRUE);

            return 0;
        }

        case WM_CTLCOLORSTATIC: {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            SetBkMode(hdc, TRANSPARENT);
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
        }

        case WM_COMMAND: {
            if (LOWORD(wParam) == IDOK) {
                if (ctx) {
                    ctx->enableContext = (SendMessageW(ctx->hCheckContext, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    ctx->confirmed     = true;
                }
                DestroyWindow(hwnd);
                return 0;
            }
            if (LOWORD(wParam) == IDCANCEL) {
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        }

        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool ShowFirstRunSetupDialog(HINSTANCE hInstance) {
    const wchar_t* kCls = L"FileUnlockerFirstRunClass";
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = FirstRunWndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    wc.lpszClassName = kCls;
    RegisterClassExW(&wc);

    FirstRunContext ctx;
    int w = 450, h = 310;
    int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;

    HWND hwnd = CreateWindowExW(
        WS_EX_TOPMOST, kCls, L"DelUnlock \x2014 Quick Setup",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        x, y, w, h, nullptr, nullptr, hInstance, &ctx
    );

    if (!hwnd) return false;

    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);

    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (ctx.confirmed) {
        AppConfig cfg;
        cfg.isFirstRun = false;
        cfg.contextMenuEnabled = ctx.enableContext;
        SaveConfig(cfg);

        if (ctx.enableContext) {
            InstallContextMenu();
        } else {
            UninstallContextMenu();
        }
        return true;
    }
    return false;
}

// ----------------------------------------------------------------------------
// Settings Dialog
// ----------------------------------------------------------------------------

struct SettingsContext {
    HWND hCheckContext = nullptr;
};

static LRESULT CALLBACK SettingsWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* ctx = reinterpret_cast<SettingsContext*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
        case WM_CREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            ctx = reinterpret_cast<SettingsContext*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));

            HFONT hFontBody = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            HFONT hFontBold = CreateFontW(-13, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");

            ctx->hCheckContext = CreateWindowExW(0, L"BUTTON", L"Show 'Unlock / Force Delete...' in right-click context menu",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                24, 25, 390, 24, hwnd, reinterpret_cast<HMENU>(201), cs->hInstance, nullptr);
            SendMessageW(ctx->hCheckContext, WM_SETFONT, reinterpret_cast<WPARAM>(hFontBody), TRUE);

            bool isInstalled = IsContextMenuInstalled();
            SendMessageW(ctx->hCheckContext, BM_SETCHECK, isInstalled ? BST_CHECKED : BST_UNCHECKED, 0);

            HWND hHint = CreateWindowExW(0, L"STATIC",
                L"When enabled, right-clicking any file or folder in Explorer gives you instant one-click unlocking.",
                WS_CHILD | WS_VISIBLE | SS_LEFT, 44, 55, 360, 40, hwnd, nullptr, cs->hInstance, nullptr);
            SendMessageW(hHint, WM_SETFONT, reinterpret_cast<WPARAM>(hFontBody), TRUE);

            HWND hBtnSave = CreateWindowExW(0, L"BUTTON", L"Apply & Close",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                280, 110, 120, 30, hwnd, reinterpret_cast<HMENU>(IDOK), cs->hInstance, nullptr);
            SendMessageW(hBtnSave, WM_SETFONT, reinterpret_cast<WPARAM>(hFontBold), TRUE);

            return 0;
        }

        case WM_CTLCOLORSTATIC: {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            SetBkMode(hdc, TRANSPARENT);
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
        }

        case WM_COMMAND: {
            if (LOWORD(wParam) == IDOK) {
                if (ctx) {
                    bool enable = (SendMessageW(ctx->hCheckContext, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    AppConfig cfg = LoadConfig();
                    cfg.contextMenuEnabled = enable;
                    SaveConfig(cfg);

                    if (enable) {
                        InstallContextMenu();
                    } else {
                        UninstallContextMenu();
                    }
                }
                DestroyWindow(hwnd);
                return 0;
            }
            if (LOWORD(wParam) == IDCANCEL) {
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        }

        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void ShowSettingsDialog(HWND hParent, HINSTANCE hInstance) {
    const wchar_t* kCls = L"FileUnlockerSettingsClass";
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = SettingsWndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    wc.lpszClassName = kCls;
    RegisterClassExW(&wc);

    SettingsContext ctx;
    int w = 430, h = 190;
    RECT rcParent = {};
    if (hParent && GetWindowRect(hParent, &rcParent)) {
        w = 430;
    }
    int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;

    HWND hwnd = CreateWindowExW(
        WS_EX_DLGMODALFRAME, kCls, L"Settings \x2014 DelUnlock",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        x, y, w, h, hParent, nullptr, hInstance, &ctx
    );

    if (!hwnd) return;

    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);

    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

} // namespace unlocker
