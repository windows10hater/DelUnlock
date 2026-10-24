// ============================================================================
// ui.cpp
// Native Per-OS UI + Win10/11 Explorer Dark/Light Mode + Life-Changing QoL.
//
// Fun fact about Windows Dark Mode: Microsoft never officially documented dark
// mode for Win32 ComCtl32 controls, even though File Explorer (explorer.exe) and
// Task Manager are Win32 apps that use it every day! They did it by hiding the
// APIs inside uxtheme.dll under unnamed ordinals 132, 133, 135, and 136.
// We load those ordinals dynamically (and safely) when running on Win10 1809+ / Win11,
// while leaving XP/Vista/7/8 completely untouched.
// ============================================================================

#include "ui.hpp"
#include "config.hpp"

#include <map>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <uxtheme.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "shell32.lib")

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif
#ifndef IDI_SHIELD
#define IDI_SHIELD MAKEINTRESOURCEW(32518)
#endif

// Embed the ComCtl32 v6 Visual Styles manifest directly into the binary so our buttons,
// dropdown, and list view automatically use XP Luna, Win7 Aero, or Win10/11 Fluent styles.
#pragma comment(linker,"\"/manifestdependency:type='win32' \
name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace unlocker {

// ----------------------------------------------------------------------------
// Control IDs
// ----------------------------------------------------------------------------
constexpr int IDC_ACTION_COMBO   = 1001;
constexpr int IDC_PROCESS_LIST   = 1002;
constexpr int IDC_RECYCLE_CHECK  = 1003;
constexpr int IDC_BTN_REFRESH    = 1004;
constexpr int IDC_BTN_SWITCH_TO  = 1005;
constexpr int IDC_BTN_OPEN_LOC   = 1006;
constexpr int IDC_BTN_SETTINGS   = 1009;
constexpr int IDC_BTN_EXECUTE    = 1007;
constexpr int IDC_BTN_CLOSE      = 1008;
constexpr int IDC_BTN_ADMIN      = 1010;

#ifndef BCM_SETSHIELD
#define BCM_SETSHIELD 0x160C
#endif

// ----------------------------------------------------------------------------
// Dynamic OS Version, DWM & UxTheme Dark Mode Support
// ----------------------------------------------------------------------------

enum class PreferredAppMode {
    Default    = 0,
    AllowDark  = 1,
    ForceDark  = 2,
    ForceLight = 3
};

using PFN_DwmSetWindowAttribute   = HRESULT(WINAPI*)(HWND hwnd, DWORD dwAttribute, LPCVOID pvAttribute, DWORD cbAttribute);
using PFN_SetPreferredAppMode     = PreferredAppMode(WINAPI*)(PreferredAppMode appMode); // uxtheme ordinal 135 (Win10 1903+)
using PFN_AllowDarkModeForApp     = bool(WINAPI*)(bool allow);                           // uxtheme ordinal 135 (Win10 1809)
using PFN_AllowDarkModeForWindow  = bool(WINAPI*)(HWND hWnd, bool allow);                // uxtheme ordinal 133
using PFN_FlushMenuThemes         = void(WINAPI*)();                                     // uxtheme ordinal 136
using PFN_RtlGetVersion           = LONG(NTAPI*)(OSVERSIONINFOW*);

struct OsThemeBridge {
    DWORD major = 5;
    DWORD minor = 1;
    DWORD build = 2600;

    HMODULE                      hDwmapi                = nullptr;
    PFN_DwmSetWindowAttribute    DwmSetWindowAttribute  = nullptr;

    HMODULE                      hUxTheme               = nullptr;
    PFN_SetPreferredAppMode      SetPreferredAppMode    = nullptr;
    PFN_AllowDarkModeForWindow   AllowDarkModeForWindow = nullptr;
    PFN_FlushMenuThemes          FlushMenuThemes        = nullptr;

    OsThemeBridge() {
        HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
        if (hNtdll) {
            auto rtlGetVer = reinterpret_cast<PFN_RtlGetVersion>(GetProcAddress(hNtdll, "RtlGetVersion"));
            if (rtlGetVer) {
                OSVERSIONINFOW osvi = {};
                osvi.dwOSVersionInfoSize = sizeof(osvi);
                if (rtlGetVer(&osvi) == 0) {
                    major = osvi.dwMajorVersion;
                    minor = osvi.dwMinorVersion;
                    build = osvi.dwBuildNumber;
                }
            }
        }

        hDwmapi = LoadLibraryW(L"dwmapi.dll");
        if (hDwmapi) {
            DwmSetWindowAttribute = reinterpret_cast<PFN_DwmSetWindowAttribute>(
                GetProcAddress(hDwmapi, "DwmSetWindowAttribute"));
        }

        // Only probe undocumented dark-mode uxtheme ordinals on Windows 10 (build 17763+) and Windows 11
        if (major >= 10 && build >= 17763) {
            hUxTheme = GetModuleHandleW(L"uxtheme.dll");
            if (!hUxTheme) hUxTheme = LoadLibraryW(L"uxtheme.dll");
            if (hUxTheme) {
                AllowDarkModeForWindow = reinterpret_cast<PFN_AllowDarkModeForWindow>(
                    GetProcAddress(hUxTheme, MAKEINTRESOURCEA(133)));
                SetPreferredAppMode = reinterpret_cast<PFN_SetPreferredAppMode>(
                    GetProcAddress(hUxTheme, MAKEINTRESOURCEA(135)));
                FlushMenuThemes = reinterpret_cast<PFN_FlushMenuThemes>(
                    GetProcAddress(hUxTheme, MAKEINTRESOURCEA(136)));
            }
        }
    }

    bool IsWin10OrLater() const { return major >= 10; }
    bool IsWin11OrLater() const { return major >= 10 && build >= 22000; }

    // Checks Explorer's Personalize registry key to see if the user is running Dark Mode
    bool IsExplorerDarkModeEnabled() const {
        if (!IsWin10OrLater()) return false;
        HKEY hKey = nullptr;
        DWORD appsUseLightTheme = 1;
        DWORD cbData = sizeof(DWORD);
        if (RegOpenKeyExW(HKEY_CURRENT_USER,
                          L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                          0, KEY_READ, &hKey) == ERROR_SUCCESS)
        {
            RegQueryValueExW(hKey, L"AppsUseLightTheme", nullptr, nullptr,
                             reinterpret_cast<LPBYTE>(&appsUseLightTheme), &cbData);
            RegCloseKey(hKey);
        }
        return (appsUseLightTheme == 0);
    }

    void ApplyAppThemePreference(bool dark) const {
        if (SetPreferredAppMode) {
            SetPreferredAppMode(dark ? PreferredAppMode::ForceDark : PreferredAppMode::ForceLight);
        }
        if (FlushMenuThemes) {
            FlushMenuThemes();
        }
    }

    void ApplyWindowChrome(HWND hwnd, bool dark) const {
        if (AllowDarkModeForWindow) {
            AllowDarkModeForWindow(hwnd, dark);
        }
        if (DwmSetWindowAttribute && IsWin10OrLater()) {
            BOOL useDark = dark ? TRUE : FALSE;
            // 20 = DWMWA_USE_IMMERSIVE_DARK_MODE (Win10 20H1+ & Win11), 19 = older Win10
            if (FAILED(DwmSetWindowAttribute(hwnd, 20, &useDark, sizeof(useDark)))) {
                DwmSetWindowAttribute(hwnd, 19, &useDark, sizeof(useDark));
            }

            if (IsWin11OrLater()) {
                // 33 = DWMWA_WINDOW_CORNER_PREFERENCE -> 2 (DWMWCP_ROUND)
                DWORD cornerPref = 2;
                DwmSetWindowAttribute(hwnd, 33, &cornerPref, sizeof(cornerPref));

                // 38 = DWMWA_SYSTEMBACKDROP_TYPE -> 2 (DWMSBT_MAINWINDOW = Mica)
                DWORD backdropType = 2;
                DwmSetWindowAttribute(hwnd, 38, &backdropType, sizeof(backdropType));
            }
        }
    }
};

static const OsThemeBridge& ThemeBridge() {
    static OsThemeBridge instance;
    return instance;
}

// ----------------------------------------------------------------------------
// DPI Helper (works from Windows XP all the way to Per-Monitor V2 on Win11)
// ----------------------------------------------------------------------------
static int GetWindowDpiSafe(HWND hwnd) {
    HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
    if (hUser32) {
        using PFN_GetDpiForWindow = UINT(WINAPI*)(HWND);
        auto fn = reinterpret_cast<PFN_GetDpiForWindow>(GetProcAddress(hUser32, "GetDpiForWindow"));
        if (fn && hwnd) {
            UINT dpi = fn(hwnd);
            if (dpi > 0) return static_cast<int>(dpi);
        }
    }
    HDC hdc = GetDC(hwnd);
    int dpi = GetDeviceCaps(hdc, LOGPIXELSX);
    ReleaseDC(hwnd, hdc);
    return (dpi > 0) ? dpi : 96;
}

static int ScaleDpi(int px, int dpi) {
    return MulDiv(px, dpi, 96);
}

// ----------------------------------------------------------------------------
// Dialog State
// ----------------------------------------------------------------------------
struct DialogContext {
    HINSTANCE   hInstance           = nullptr;
    HWND        hwnd                = nullptr;
    HWND        explorerConflictDlg = nullptr;
    int         dpi                 = 96;
    bool        isDark              = false;

    // Fonts & Brushes
    HFONT       hFontTitle          = nullptr;
    HFONT       hFontBold           = nullptr;
    HFONT       hFontBody           = nullptr;
    HFONT       hFontSmall          = nullptr;
    HBRUSH      hBgBrush            = nullptr;
    HBRUSH      hCardBrush          = nullptr;

    // Target file & scan data
    std::vector<std::wstring> targetPaths;
    bool         isMultiple         = false;
    std::wstring filePath;
    std::wstring fileName;
    std::wstring fileDescription;
    std::wstring folderPath;
    bool         isSystemCritical   = false;
    HICON        hFileIcon          = nullptr;
    ScanResult   scan;
    HIMAGELIST   hProcImageList     = nullptr;

    // Child Controls
    HWND        hListProcs          = nullptr;
    HWND        hComboAction        = nullptr;
    HWND        hCheckRecycle       = nullptr;
    HWND        hBtnRefresh         = nullptr;
    HWND        hBtnSwitchTo        = nullptr;
    HWND        hBtnOpenLoc         = nullptr;
    HWND        hBtnSettings        = nullptr;
    HWND        hBtnExecute         = nullptr;
    HWND        hBtnClose           = nullptr;
    HWND        hBtnAdmin           = nullptr;

    // Dynamic status / warning text
    std::wstring warningText;
    bool         warningIsCritical  = false;
    bool         actionSucceeded    = false;

    ~DialogContext() {
        if (hFontTitle) DeleteObject(hFontTitle);
        if (hFontBold)  DeleteObject(hFontBold);
        if (hFontBody)  DeleteObject(hFontBody);
        if (hFontSmall) DeleteObject(hFontSmall);
        if (hBgBrush)   DeleteObject(hBgBrush);
        if (hCardBrush) DeleteObject(hCardBrush);
        if (hFileIcon)  DestroyIcon(hFileIcon);
        if (hProcImageList) ImageList_Destroy(hProcImageList);
    }
};

// ----------------------------------------------------------------------------
// Theme & Font Setup
// ----------------------------------------------------------------------------
static HFONT CreateUiFont(int ptSize, int weight, int dpi) {
    // Use Segoe UI on Vista/7/8/10/11; automatically falls back to Tahoma on Windows XP
    const wchar_t* faceName = (ThemeBridge().major >= 6) ? L"Segoe UI" : L"Tahoma";
    int height = -MulDiv(ptSize, dpi, 72);
    return CreateFontW(
        height, 0, 0, 0, weight, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, faceName);
}

static void RefreshFonts(DialogContext* ctx) {
    if (ctx->hFontTitle) DeleteObject(ctx->hFontTitle);
    if (ctx->hFontBold)  DeleteObject(ctx->hFontBold);
    if (ctx->hFontBody)  DeleteObject(ctx->hFontBody);
    if (ctx->hFontSmall) DeleteObject(ctx->hFontSmall);

    ctx->hFontTitle = CreateUiFont(12, FW_SEMIBOLD, ctx->dpi);
    ctx->hFontBold  = CreateUiFont(9,  FW_SEMIBOLD, ctx->dpi);
    ctx->hFontBody  = CreateUiFont(9,  FW_NORMAL,   ctx->dpi);
    ctx->hFontSmall = CreateUiFont(8,  FW_NORMAL,   ctx->dpi);
}

static void ApplyThemeToControls(DialogContext* ctx) {
    ctx->isDark = ThemeBridge().IsExplorerDarkModeEnabled();
    ThemeBridge().ApplyAppThemePreference(ctx->isDark);
    ThemeBridge().ApplyWindowChrome(ctx->hwnd, ctx->isDark);

    if (ctx->hBgBrush)   DeleteObject(ctx->hBgBrush);
    if (ctx->hCardBrush) DeleteObject(ctx->hCardBrush);

    if (ctx->isDark) {
        ctx->hBgBrush   = CreateSolidBrush(RGB(32, 32, 32)); // Win11 Dark Mica surface (#202020)
        ctx->hCardBrush = CreateSolidBrush(RGB(43, 43, 43)); // Elevated card (#2B2B2B)
    } else {
        // On Win10/11 Light mode use crisp #F3F3F3; on XP/7 use native COLOR_3DFACE / window surface
        COLORREF bgCol = ThemeBridge().IsWin10OrLater() ? RGB(243, 243, 243) : GetSysColor(COLOR_BTNFACE);
        ctx->hBgBrush   = CreateSolidBrush(bgCol);
        ctx->hCardBrush = CreateSolidBrush(RGB(255, 255, 255));
    }

    // Apply native Explorer/CFD dark or light visual styles to child controls on Win10/11
    if (ThemeBridge().IsWin10OrLater()) {
        const wchar_t* btnTheme   = ctx->isDark ? L"DarkMode_Explorer" : L"Explorer";
        const wchar_t* comboTheme = ctx->isDark ? L"DarkMode_CFD"      : L"CFD";
        const wchar_t* listTheme  = ctx->isDark ? L"DarkMode_Explorer" : L"Explorer";

        if (ctx->hComboAction)  SetWindowTheme(ctx->hComboAction,  comboTheme, nullptr);
        if (ctx->hListProcs) {
            SetWindowTheme(ctx->hListProcs, listTheme, nullptr);
            HWND hHeader = ListView_GetHeader(ctx->hListProcs);
            if (hHeader) {
                SetWindowTheme(hHeader, listTheme, nullptr);
            }
        }
        if (ctx->hCheckRecycle) SetWindowTheme(ctx->hCheckRecycle, btnTheme,   nullptr);
        if (ctx->hBtnRefresh)   SetWindowTheme(ctx->hBtnRefresh,   btnTheme,   nullptr);
        if (ctx->hBtnSettings)  SetWindowTheme(ctx->hBtnSettings,  btnTheme,   nullptr);
        if (ctx->hBtnSwitchTo)  SetWindowTheme(ctx->hBtnSwitchTo,  btnTheme,   nullptr);
        if (ctx->hBtnOpenLoc)   SetWindowTheme(ctx->hBtnOpenLoc,   btnTheme,   nullptr);
        if (ctx->hBtnExecute)   SetWindowTheme(ctx->hBtnExecute,   btnTheme,   nullptr);
        if (ctx->hBtnClose)     SetWindowTheme(ctx->hBtnClose,     btnTheme,   nullptr);
        if (ctx->hBtnAdmin)     SetWindowTheme(ctx->hBtnAdmin,     btnTheme,   nullptr);
    } else if (ctx->hListProcs) {
        SetWindowTheme(ctx->hListProcs, L"Explorer", nullptr);
    }

    if (ctx->hListProcs) {
        COLORREF listBg   = ctx->isDark ? RGB(43, 43, 43)   : RGB(255, 255, 255);
        COLORREF listText = ctx->isDark ? RGB(245, 245, 245) : RGB(20, 20, 20);
        ListView_SetBkColor(ctx->hListProcs, listBg);
        ListView_SetTextBkColor(ctx->hListProcs, listBg);
        ListView_SetTextColor(ctx->hListProcs, listText);
    }

    InvalidateRect(ctx->hwnd, nullptr, TRUE);
}

// ----------------------------------------------------------------------------
// Warning Text & Smart Default Action Logic
// ----------------------------------------------------------------------------
static void UpdateWarningTextForSelection(DialogContext* ctx) {
    int sel = static_cast<int>(SendMessageW(ctx->hComboAction, CB_GETCURSEL, 0, 0));
    if (sel < 0) sel = 0;
    ActionType action = static_cast<ActionType>(sel);

    bool hasSystemProtected = false;
    bool hasOnlyExplorer    = !ctx->scan.processes.empty();

    for (const auto& p : ctx->scan.processes) {
        if (p.isSystemProtected) hasSystemProtected = true;
        if (!p.isExplorer) hasOnlyExplorer = false;
    }

    ctx->warningIsCritical = false;

    if (ctx->scan.processes.empty()) {
        if (ctx->scan.isCurrentlyLocked) {
            ctx->warningText = L"Locked by a kernel/system driver. Run as Administrator or use 'Delete on Next Reboot'.";
            ctx->warningIsCritical = true;
        } else {
            ctx->warningText = L"No active locks detected! You can delete this file right now.";
        }
        return;
    }

    switch (action) {
        case ActionType::UnlockAndDelete:
        case ActionType::UnlockOnly:
            if (hasOnlyExplorer) {
                ctx->warningText = L"Tip: Windows Explorer (Preview/Thumbnail) is holding this file. Surgical unlock is 100% safe.";
            } else {
                ctx->warningText = L"Warning: Force-closing open handles may cause the app to lose unsaved changes to this file.";
            }
            break;

        case ActionType::KillAndDelete:
        case ActionType::KillOnly:
            if (hasSystemProtected) {
                ctx->warningText = L"Blocked: Critical Windows system process detected. Cannot kill without crashing Windows!";
                ctx->warningIsCritical = true;
            } else {
                ctx->warningText = L"Warning: Terminating the locking application will immediately close it without saving.";
                ctx->warningIsCritical = true;
            }
            break;

        case ActionType::DeleteOnReboot:
            ctx->warningText = L"Windows will delete this file automatically during the next restart before any apps load.";
            break;

        case ActionType::UnlockAndRename:
            ctx->warningText = L"Sever remote file handles and immediately rename the file on disk (ideal for game modding & DLL replacement).";
            break;

        case ActionType::UnlockAndMove:
            ctx->warningText = L"Sever remote file handles and move the file into a backup or alternative directory.";
            break;
    }
}

// Populates the ListView with locking processes and selects the smartest default action
static void PopulateLockListAndDefaults(DialogContext* ctx, bool initialLoad) {
    if (ctx->isMultiple) {
        // Aggregate scan across all selected files/folders
        ctx->scan = {};
        ctx->scan.targetPath = std::to_wstring(ctx->targetPaths.size()) + L" selected items";
        ctx->scan.exists = true;

        std::map<DWORD, LockingProcess> aggregated;
        double totalScanTime = 0.0;

        for (const auto& path : ctx->targetPaths) {
            ScanResult r = ScanFileLocks(path, true);
            totalScanTime += r.scanTimeMs;
            if (r.isCurrentlyLocked) ctx->scan.isCurrentlyLocked = true;
            for (const auto& proc : r.processes) {
                LockingProcess& dest = aggregated[proc.pid];
                dest.pid           = proc.pid;
                dest.processName   = proc.processName;
                dest.friendlyName  = proc.friendlyName;
                dest.fullImagePath = proc.fullImagePath;
                dest.kind          = proc.kind;
                dest.isSystemProtected = proc.isSystemProtected;
                dest.isExplorer    = proc.isExplorer;
                if (!dest.mainWindow && proc.mainWindow) dest.mainWindow = proc.mainWindow;
                for (ULONG_PTR h : proc.handles) {
                    dest.handles.push_back(h);
                }
            }
        }
        ctx->scan.scanTimeMs = totalScanTime;
        for (auto& kv : aggregated) {
            ctx->scan.processes.push_back(std::move(kv.second));
        }
    } else {
        // Single target file/folder scan
        ctx->scan = ScanFileLocks(ctx->filePath, true);
    }

    ListView_DeleteAllItems(ctx->hListProcs);
    if (ctx->hProcImageList) {
        ImageList_RemoveAll(ctx->hProcImageList);
    }

    bool allModuleLocks = !ctx->scan.processes.empty();

    for (size_t i = 0; i < ctx->scan.processes.size(); ++i) {
        const auto& proc = ctx->scan.processes[i];
        if (!proc.handles.empty() || proc.kind == LockKind::FileHandle) {
            allModuleLocks = false;
        }

        // Extract small icon for the locking process
        int iconIndex = -1;
        SHFILEINFOW sfi = {};
        if (!proc.fullImagePath.empty() &&
            SHGetFileInfoW(proc.fullImagePath.c_str(), 0, &sfi, sizeof(sfi), SHGFI_ICON | SHGFI_SMALLICON) && sfi.hIcon)
        {
            iconIndex = ImageList_AddIcon(ctx->hProcImageList, sfi.hIcon);
            DestroyIcon(sfi.hIcon);
        } else {
            HICON hDef = LoadIconW(nullptr, IDI_APPLICATION);
            iconIndex = ImageList_AddIcon(ctx->hProcImageList, hDef);
        }

        std::wstring appDisplay;
        if (proc.isService) {
            appDisplay = L"Service: " + proc.friendlyName;
            if (!proc.processName.empty() && _wcsicmp(proc.friendlyName.c_str(), proc.processName.c_str()) != 0) {
                appDisplay += L" (" + proc.processName + L")";
            }
        } else {
            appDisplay = proc.friendlyName;
            if (_wcsicmp(proc.friendlyName.c_str(), proc.processName.c_str()) != 0) {
                appDisplay += L" (" + proc.processName + L")";
            }
        }
        if (proc.isExplorer) {
            appDisplay += L" [Explorer Preview]";
        } else if (proc.isSystemProtected) {
            appDisplay += L" [System Protected]";
        }

        LVITEMW lvi = {};
        lvi.mask    = LVIF_TEXT | LVIF_IMAGE | LVIF_PARAM;
        lvi.iItem   = static_cast<int>(i);
        lvi.iImage  = iconIndex;
        lvi.lParam  = static_cast<LPARAM>(i);
        lvi.pszText = const_cast<LPWSTR>(appDisplay.c_str());
        ListView_InsertItem(ctx->hListProcs, &lvi);

        std::wstring pidStr = std::to_wstring(proc.pid);
        ListView_SetItemText(ctx->hListProcs, static_cast<int>(i), 1, const_cast<LPWSTR>(pidStr.c_str()));

        std::wstring typeStr;
        if (proc.isService) {
            typeStr = L"Windows Service";
        } else if (!proc.handles.empty()) {
            typeStr = std::to_wstring(proc.handles.size()) + L" File Handle(s)";
        } else if (proc.kind == LockKind::LoadedModule) {
            typeStr = L"Loaded EXE/DLL";
        } else {
            typeStr = L"Active Lock";
        }
        ListView_SetItemText(ctx->hListProcs, static_cast<int>(i), 2, const_cast<LPWSTR>(typeStr.c_str()));
    }

    if (!ctx->scan.processes.empty()) {
        ListView_SetItemState(ctx->hListProcs, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    } else {
        // Helpful empty state row so the table isn't an intimidating blank void!
        LVITEMW lvi = {};
        lvi.mask    = LVIF_TEXT;
        lvi.iItem   = 0;
        bool isAdmin = IsRunningAsAdmin();
        std::wstring emptyMsg;
        if (ctx->scan.isCurrentlyLocked) {
            emptyMsg = L"Locked by elevated process / kernel driver (run Unlocker as Administrator)";
        } else if (ctx->isSystemCritical && !isAdmin) {
            emptyMsg = L"No active file handles found (run in elevated mode to scan memory-mapped modules)";
        } else {
            emptyMsg = L"No active locking processes found (file is free)";
        }
        lvi.pszText = const_cast<LPWSTR>(emptyMsg.c_str());
        ListView_InsertItem(ctx->hListProcs, &lvi);

        std::wstring dash = L"-";
        ListView_SetItemText(ctx->hListProcs, 0, 1, const_cast<LPWSTR>(dash.c_str()));
        std::wstring lockHint = ctx->scan.isCurrentlyLocked ? L"Kernel / Driver" : L"None";
        ListView_SetItemText(ctx->hListProcs, 0, 2, const_cast<LPWSTR>(lockHint.c_str()));
    }

    // QoL: Smart default dropdown selection on initial load!
    // - If locked by a normal file handle (or Explorer preview), default to "Unlock & Delete" (0).
    // - If locked purely as a running EXE/DLL module, default to "Kill Process & Delete" (1).
    if (initialLoad) {
        int defaultIdx = (allModuleLocks && !ctx->scan.processes.empty()) ?
            static_cast<int>(ActionType::KillAndDelete) :
            static_cast<int>(ActionType::UnlockAndDelete);
        SendMessageW(ctx->hComboAction, CB_SETCURSEL, defaultIdx, 0);
    }

    // Enable/disable QoL buttons based on selection
    bool hasProcs = !ctx->scan.processes.empty();
    EnableWindow(ctx->hBtnSwitchTo, hasProcs && ctx->scan.processes[0].mainWindow != nullptr);
    EnableWindow(ctx->hBtnOpenLoc,  hasProcs && !ctx->scan.processes[0].fullImagePath.empty());

    UpdateWarningTextForSelection(ctx);
    InvalidateRect(ctx->hwnd, nullptr, TRUE);
}

// ----------------------------------------------------------------------------
// Custom Header & Card Painting (Crisp File Icon, Filename, Scan Speed Badge)
// ----------------------------------------------------------------------------
static void PaintDialogSurface(DialogContext* ctx, HDC hdc, const RECT& rcClient) {
    int dpi = ctx->dpi;

    // 1. Fill main background
    FillRect(hdc, &rcClient, ctx->hBgBrush);

    // 2. Draw Top File Info Card
    int cardH = ctx->isSystemCritical ? ScaleDpi(122, dpi) : ScaleDpi(104, dpi);
    RECT rcCard = {
        ScaleDpi(16, dpi),
        ScaleDpi(14, dpi),
        rcClient.right - ScaleDpi(16, dpi),
        ScaleDpi(14, dpi) + cardH
    };

    COLORREF borderCol = ctx->isDark ? RGB(60, 60, 60) : RGB(218, 218, 218);
    HPEN hBorderPen = CreatePen(PS_SOLID, 1, borderCol);
    HGDIOBJ hOldPen = SelectObject(hdc, hBorderPen);
    HGDIOBJ hOldBr  = SelectObject(hdc, ctx->hCardBrush);

    // Subtle 6px rounded card on Win10/11, classic crisp card on XP/7
    int radius = ThemeBridge().IsWin11OrLater() ? ScaleDpi(8, dpi) : ScaleDpi(4, dpi);
    RoundRect(hdc, rcCard.left, rcCard.top, rcCard.right, rcCard.bottom, radius, radius);

    SelectObject(hdc, hOldBr);
    SelectObject(hdc, hOldPen);
    DeleteObject(hBorderPen);

    // 3. Draw 48x48 File Icon inside the card
    int iconSize = ScaleDpi(44, dpi);
    int iconX    = rcCard.left + ScaleDpi(14, dpi);
    int iconY    = rcCard.top  + ScaleDpi(15, dpi);
    if (ctx->hFileIcon) {
        DrawIconEx(hdc, iconX, iconY, ctx->hFileIcon, iconSize, iconSize, 0, nullptr, DI_NORMAL);
    }

    SetBkMode(hdc, TRANSPARENT);
    COLORREF primaryText   = ctx->isDark ? RGB(255, 255, 255) : RGB(20, 20, 20);
    COLORREF secondaryText = ctx->isDark ? RGB(175, 175, 175) : RGB(95, 95, 95);
    COLORREF accentBadge   = ctx->isDark ? RGB(96, 205, 255)  : RGB(0, 103, 192);

    // 4. Draw File Name (Pure clean title, no clutter)
    int titleRight = ctx->hBtnAdmin ? (rcCard.right - ScaleDpi(150, dpi)) : (rcCard.right - ScaleDpi(14, dpi));
    RECT rcTitle = {
        iconX + iconSize + ScaleDpi(14, dpi),
        rcCard.top + ScaleDpi(10, dpi),
        titleRight,
        rcCard.top + ScaleDpi(32, dpi)
    };
    SetTextColor(hdc, primaryText);
    HGDIOBJ hOldFont = SelectObject(hdc, ctx->hFontTitle);
    DrawTextW(hdc, ctx->fileName.c_str(), -1, &rcTitle, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);

    // 5. Draw Metadata Description (e.g. "NT Kernel & System" from version info)
    int nextY = rcCard.top + ScaleDpi(33, dpi);
    if (!ctx->fileDescription.empty()) {
        RECT rcDesc = {
            rcTitle.left,
            nextY,
            rcCard.right - ScaleDpi(14, dpi),
            nextY + ScaleDpi(18, dpi)
        };
        SetTextColor(hdc, primaryText);
        SelectObject(hdc, ctx->hFontBold);
        DrawTextW(hdc, ctx->fileDescription.c_str(), -1, &rcDesc, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        nextY += ScaleDpi(18, dpi);
    }

    // Draw Directory Path
    RECT rcFolder = {
        rcTitle.left,
        nextY,
        rcCard.right - ScaleDpi(14, dpi),
        nextY + ScaleDpi(17, dpi)
    };
    SetTextColor(hdc, secondaryText);
    SelectObject(hdc, ctx->hFontSmall);
    DrawTextW(hdc, ctx->folderPath.c_str(), -1, &rcFolder, DT_SINGLELINE | DT_VCENTER | DT_PATH_ELLIPSIS | DT_NOPREFIX);
    nextY += ScaleDpi(17, dpi);

    // If file is system critical, draw universal Windows Warning icon (IDI_WARNING) + warning line right under file path!
    if (ctx->isSystemCritical) {
        int warnIconSize = ScaleDpi(14, dpi);
        int warnIconY    = nextY + ScaleDpi(1, dpi);
        HICON hWarnIcon  = LoadIconW(nullptr, IDI_WARNING); // Universal Win32 Warning icon across XP, 7, 10, 11!
        if (hWarnIcon) {
            DrawIconEx(hdc, rcTitle.left, warnIconY, hWarnIcon, warnIconSize, warnIconSize, 0, nullptr, DI_NORMAL);
        }

        RECT rcCritWarn = {
            rcTitle.left + warnIconSize + ScaleDpi(6, dpi),
            nextY,
            rcCard.right - ScaleDpi(14, dpi),
            nextY + ScaleDpi(17, dpi)
        };
        COLORREF warnCol = ctx->isDark ? RGB(255, 175, 100) : RGB(200, 75, 0);
        SetTextColor(hdc, warnCol);
        SelectObject(hdc, ctx->hFontSmall);
        DrawTextW(hdc, L"Windows System File - Modifying or deleting may affect system stability", -1, &rcCritWarn, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        nextY += ScaleDpi(17, dpi);
    }

    // 6. Draw Status & Benchmark Speed line inside the card
    wchar_t statusLine[256] = {};
    const wchar_t* bullet = (ThemeBridge().major >= 6) ? L"\x2022" : L"-";
    bool isAdmin = IsRunningAsAdmin();
    const wchar_t* adminSuffix = isAdmin ? L"  [Admin]" : L"";
    int procCount = static_cast<int>(ctx->scan.processes.size());
    if (procCount > 0) {
        wsprintfW(statusLine, L"Locked by %d process%s  %s  Scanned in %d.%d ms%s",
                  procCount, (procCount == 1 ? L"" : L"es"), bullet,
                  static_cast<int>(ctx->scan.scanTimeMs),
                  static_cast<int>(ctx->scan.scanTimeMs * 10.0) % 10,
                  adminSuffix);
    } else if (ctx->scan.isCurrentlyLocked) {
        wsprintfW(statusLine, L"Locked (System / Elevated handle)  %s  Scanned in %d.%d ms%s",
                  bullet,
                  static_cast<int>(ctx->scan.scanTimeMs),
                  static_cast<int>(ctx->scan.scanTimeMs * 10.0) % 10,
                  adminSuffix);
    } else {
        if (ctx->isSystemCritical && !isAdmin) {
            wsprintfW(statusLine, L"Unlocked & ready  %s  Scanned in %d.%d ms  (run in elevated mode to scan memory-mapped modules)",
                      bullet,
                      static_cast<int>(ctx->scan.scanTimeMs),
                      static_cast<int>(ctx->scan.scanTimeMs * 10.0) % 10);
        } else {
            wsprintfW(statusLine, L"Unlocked & ready  %s  Scanned in %d.%d ms%s",
                      bullet,
                      static_cast<int>(ctx->scan.scanTimeMs),
                      static_cast<int>(ctx->scan.scanTimeMs * 10.0) % 10,
                      adminSuffix);
        }
    }

    RECT rcScanInfo = {
        rcTitle.left,
        nextY,
        rcCard.right - ScaleDpi(14, dpi),
        nextY + ScaleDpi(17, dpi)
    };
    SetTextColor(hdc, accentBadge);
    SelectObject(hdc, ctx->hFontSmall);
    DrawTextW(hdc, statusLine, -1, &rcScanInfo, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);

    // Calculate vertical offset so controls smoothly adjust if the warning badge is shown
    int topOffset = ctx->isSystemCritical ? ScaleDpi(18, dpi) : 0;

    int btnSwitchW = ScaleDpi(96, dpi);
    int btnLocW    = ScaleDpi(86, dpi);
    int btnRefW    = ScaleDpi(62, dpi);
    int btnSetW    = ScaleDpi(66, dpi);

    // 7. Section Label: "Processes holding this file:" (or "Processes holding selected items:" for batch)
    // Clamped width with plenty of margin before the buttons start!
    int labelRight = (rcClient.right - ScaleDpi(16, dpi)) - (btnSetW + btnRefW + btnLocW + btnSwitchW + ScaleDpi(35, dpi));
    if (labelRight < ScaleDpi(120, dpi)) labelRight = ScaleDpi(120, dpi);

    RECT rcListLabel = {
        ScaleDpi(16, dpi),
        ScaleDpi(116, dpi) + topOffset,
        labelRight,
        ScaleDpi(138, dpi) + topOffset
    };
    SetTextColor(hdc, primaryText);
    SelectObject(hdc, ctx->hFontBold);
    const wchar_t* listHeading = ctx->isMultiple ? L"Processes holding selected items:" : L"Processes holding this file:";
    DrawTextW(hdc, listHeading, -1, &rcListLabel, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);

    // 8. Section Label: "Choose Action:"
    RECT rcActionLabel = {
        ScaleDpi(16, dpi),
        ScaleDpi(260, dpi) + topOffset,
        rcClient.right - ScaleDpi(16, dpi),
        ScaleDpi(280, dpi) + topOffset
    };
    SetTextColor(hdc, primaryText);
    SelectObject(hdc, ctx->hFontBold);
    DrawTextW(hdc, L"Action to perform:", -1, &rcActionLabel, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);

    // 9. Warning / Safety Banner below the Dropdown
    RECT rcWarn = {
        ScaleDpi(16, dpi),
        ScaleDpi(314, dpi) + topOffset,
        rcClient.right - ScaleDpi(16, dpi),
        ScaleDpi(346, dpi) + topOffset
    };
    COLORREF warnColor = ctx->warningIsCritical
        ? (ctx->isDark ? RGB(255, 153, 164) : RGB(196, 43, 28))
        : secondaryText;
    SetTextColor(hdc, warnColor);
    SelectObject(hdc, ctx->hFontSmall);
    DrawTextW(hdc, ctx->warningText.c_str(), -1, &rcWarn, DT_WORDBREAK | DT_NOPREFIX);

    SelectObject(hdc, hOldFont);
}

// ----------------------------------------------------------------------------
// Layout & Control Creation
// ----------------------------------------------------------------------------
static void LayoutControls(DialogContext* ctx) {
    int dpi = ctx->dpi;
    RECT rc = {};
    GetClientRect(ctx->hwnd, &rc);
    int w = rc.right - rc.left;
    int margin = ScaleDpi(16, dpi);

    // If running non-elevated, position the "Restart as Admin" button in the upper right of the card
    if (ctx->hBtnAdmin) {
        int adminW = ScaleDpi(136, dpi);
        int adminH = ScaleDpi(26, dpi);
        int adminX = w - margin - adminW - ScaleDpi(10, dpi);
        MoveWindow(ctx->hBtnAdmin, adminX, ScaleDpi(22, dpi), adminW, adminH, TRUE);
    }

    // QoL Mini Buttons aligned to the right above the process list
    // Generous width so labels ("Switch to App", "File Location", etc.) never get clipped or overlap!
    int btnSwitchW = ScaleDpi(96, dpi);
    int btnLocW    = ScaleDpi(86, dpi);
    int btnRefW    = ScaleDpi(62, dpi);
    int btnSetW    = ScaleDpi(66, dpi);
    int miniBtnH   = ScaleDpi(24, dpi);
    // Calculate vertical offset so controls smoothly adjust if the warning badge is shown
    int topOffset = ctx->isSystemCritical ? ScaleDpi(18, dpi) : 0;

    int miniY = ScaleDpi(112, dpi) + topOffset;

    int curX = w - margin;
    curX -= btnSetW;
    MoveWindow(ctx->hBtnSettings, curX, miniY, btnSetW, miniBtnH, TRUE);

    curX -= (btnRefW + ScaleDpi(6, dpi));
    MoveWindow(ctx->hBtnRefresh, curX, miniY, btnRefW, miniBtnH, TRUE);

    curX -= (btnLocW + ScaleDpi(6, dpi));
    MoveWindow(ctx->hBtnOpenLoc, curX, miniY, btnLocW, miniBtnH, TRUE);

    curX -= (btnSwitchW + ScaleDpi(6, dpi));
    MoveWindow(ctx->hBtnSwitchTo, curX, miniY, btnSwitchW, miniBtnH, TRUE);

    // Locking Process ListView
    MoveWindow(ctx->hListProcs, margin, ScaleDpi(142, dpi) + topOffset, w - margin * 2, ScaleDpi(112, dpi), TRUE);

    // Auto-size columns to cleanly stretch across the ListView width
    RECT rcList = {};
    GetClientRect(ctx->hListProcs, &rcList);
    int totalW = rcList.right - rcList.left;
    if (totalW > ScaleDpi(100, dpi)) {
        int colPidW  = ScaleDpi(75, dpi);
        int colTypeW = ScaleDpi(145, dpi);
        int colAppW  = totalW - colPidW - colTypeW;
        if (colAppW > 50) {
            ListView_SetColumnWidth(ctx->hListProcs, 0, colAppW);
            ListView_SetColumnWidth(ctx->hListProcs, 1, colPidW);
            ListView_SetColumnWidth(ctx->hListProcs, 2, colTypeW);
        }
    }

    // Action Dropdown
    MoveWindow(ctx->hComboAction, margin, ScaleDpi(282, dpi) + topOffset, w - margin * 2, ScaleDpi(200, dpi), TRUE);

    // Bottom Bar: "Send to Recycle Bin" checkbox on the left, "Execute" and "Close" buttons on the right
    int bottomY = ScaleDpi(354, dpi) + topOffset;
    int btnW    = ScaleDpi(108, dpi);
    int btnH    = ScaleDpi(32, dpi);

    MoveWindow(ctx->hCheckRecycle, margin, bottomY + ScaleDpi(4, dpi), ScaleDpi(230, dpi), ScaleDpi(24, dpi), TRUE);
    MoveWindow(ctx->hBtnClose,     w - margin - btnW,                        bottomY, btnW, btnH, TRUE);
    MoveWindow(ctx->hBtnExecute,   w - margin - btnW * 2 - ScaleDpi(10, dpi), bottomY, btnW, btnH, TRUE);

    // Send fonts to all controls
    HWND ctrls[] = {
        ctx->hListProcs, ctx->hComboAction, ctx->hCheckRecycle,
        ctx->hBtnRefresh, ctx->hBtnSwitchTo, ctx->hBtnOpenLoc, ctx->hBtnSettings, ctx->hBtnClose, ctx->hBtnAdmin
    };
    for (HWND h : ctrls) {
        if (h) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontBody), TRUE);
    }
    if (ctx->hBtnExecute) {
        SendMessageW(ctx->hBtnExecute, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontBold), TRUE);
    }
}

static void InitializeDialogControls(DialogContext* ctx) {
    ctx->dpi = GetWindowDpiSafe(ctx->hwnd);
    RefreshFonts(ctx);

    // Extract file name, folder path, and high-res shell icon
    if (ctx->isMultiple) {
        size_t folderCount = 0;
        size_t fileCount   = 0;

        for (const auto& p : ctx->targetPaths) {
            DWORD attr = GetFileAttributesW(p.c_str());
            if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
                folderCount++;
            } else {
                fileCount++;
            }
            if (IsSystemCriticalFile(p)) {
                ctx->isSystemCritical = true;
            }
        }

        if (folderCount > 0 && fileCount == 0) {
            ctx->fileName = std::to_wstring(folderCount) + L" folders selected";
            ctx->fileDescription = L"Batch Folder Operation";
            // Get high-res standard directory shell icon
            SHFILEINFOW sfi = {};
            if (SHGetFileInfoW(L"dummy_dir", FILE_ATTRIBUTE_DIRECTORY, &sfi, sizeof(sfi),
                               SHGFI_USEFILEATTRIBUTES | SHGFI_ICON | SHGFI_LARGEICON) && sfi.hIcon) {
                ctx->hFileIcon = sfi.hIcon;
            } else {
                ctx->hFileIcon = LoadIconW(nullptr, IDI_APPLICATION);
            }
        } else if (fileCount > 0 && folderCount == 0) {
            ctx->fileName = std::to_wstring(fileCount) + L" files selected";
            ctx->fileDescription = L"Batch File Operation";
            SHFILEINFOW sfi = {};
            if (SHGetFileInfoW(L"dummy_file.dat", FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi),
                               SHGFI_USEFILEATTRIBUTES | SHGFI_ICON | SHGFI_LARGEICON) && sfi.hIcon) {
                ctx->hFileIcon = sfi.hIcon;
            } else {
                ctx->hFileIcon = LoadIconW(nullptr, IDI_APPLICATION);
            }
        } else {
            ctx->fileName = std::to_wstring(ctx->targetPaths.size()) + L" items selected (" +
                            std::to_wstring(folderCount) + L" folders, " + std::to_wstring(fileCount) + L" files)";
            ctx->fileDescription = L"Batch Files & Folders Operation";
            ctx->hFileIcon = LoadIconW(nullptr, IDI_APPLICATION);
        }

        if (!ctx->targetPaths.empty()) {
            wchar_t dirBuf[MAX_PATH * 2] = {};
            wcscpy_s(dirBuf, ctx->targetPaths[0].c_str());
            PathRemoveFileSpecW(dirBuf);
            ctx->folderPath = dirBuf;
        }
    } else {
        wchar_t fullBuf[MAX_PATH * 2] = {};
        if (GetFullPathNameW(ctx->filePath.c_str(), MAX_PATH * 2, fullBuf, nullptr)) {
            ctx->filePath = fullBuf;
        }
        const wchar_t* fname = PathFindFileNameW(ctx->filePath.c_str());
        ctx->fileName = (fname && *fname) ? fname : ctx->filePath;

        ctx->fileDescription  = GetFileDescription(ctx->filePath);
        ctx->isSystemCritical = IsSystemCriticalFile(ctx->filePath);

        wchar_t dirBuf[MAX_PATH * 2] = {};
        wcscpy_s(dirBuf, ctx->filePath.c_str());
        PathRemoveFileSpecW(dirBuf);
        ctx->folderPath = dirBuf;

        SHFILEINFOW sfi = {};
        DWORD fileAttrs = GetFileAttributesW(ctx->filePath.c_str());
        UINT flags = SHGFI_ICON | SHGFI_LARGEICON;
        if (fileAttrs == INVALID_FILE_ATTRIBUTES) {
            flags |= SHGFI_USEFILEATTRIBUTES;
            fileAttrs = FILE_ATTRIBUTE_NORMAL;
        }
        if (SHGetFileInfoW(ctx->filePath.c_str(), fileAttrs, &sfi, sizeof(sfi), flags) && sfi.hIcon) {
            ctx->hFileIcon = sfi.hIcon;
        } else {
            ctx->hFileIcon = LoadIconW(nullptr, IDI_WARNING);
        }
    }

    // Create QoL Header Buttons
    ctx->hBtnSwitchTo = CreateWindowExW(0, L"BUTTON", L"Switch to App",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 0, 0, ctx->hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_SWITCH_TO)), ctx->hInstance, nullptr);

    ctx->hBtnOpenLoc = CreateWindowExW(0, L"BUTTON", L"File Location",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 0, 0, ctx->hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_OPEN_LOC)), ctx->hInstance, nullptr);

    ctx->hBtnRefresh = CreateWindowExW(0, L"BUTTON", L"Rescan",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 0, 0, ctx->hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_REFRESH)), ctx->hInstance, nullptr);

    ctx->hBtnSettings = CreateWindowExW(0, L"BUTTON", L"Settings",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 0, 0, ctx->hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_SETTINGS)), ctx->hInstance, nullptr);

    // Create Locking Processes ListView
    ctx->hListProcs = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
        0, 0, 0, 0, ctx->hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PROCESS_LIST)), ctx->hInstance, nullptr);

    ListView_SetExtendedListViewStyle(ctx->hListProcs, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

    int iconPx = ScaleDpi(16, ctx->dpi);
    ctx->hProcImageList = ImageList_Create(iconPx, iconPx, ILC_COLOR32 | ILC_MASK, 8, 8);
    ListView_SetImageList(ctx->hListProcs, ctx->hProcImageList, LVSIL_SMALL);

    LVCOLUMNW col = {};
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
    col.fmt  = LVCFMT_LEFT;

    col.cx      = ScaleDpi(260, ctx->dpi);
    col.pszText = const_cast<LPWSTR>(L"Application / Process");
    ListView_InsertColumn(ctx->hListProcs, 0, &col);

    col.cx      = ScaleDpi(75, ctx->dpi);
    col.pszText = const_cast<LPWSTR>(L"PID");
    ListView_InsertColumn(ctx->hListProcs, 1, &col);

    col.cx      = ScaleDpi(150, ctx->dpi);
    col.pszText = const_cast<LPWSTR>(L"Lock Type");
    ListView_InsertColumn(ctx->hListProcs, 2, &col);

    // Auto-size columns to perfectly fill the ListView with no stray empty column!
    RECT rcList = {};
    GetClientRect(ctx->hListProcs, &rcList);
    int totalW = rcList.right - rcList.left;
    if (totalW > ScaleDpi(100, ctx->dpi)) {
        int colPidW  = ScaleDpi(75, ctx->dpi);
        int colTypeW = ScaleDpi(135, ctx->dpi);
        int colAppW  = totalW - colPidW - colTypeW;
        if (colAppW < ScaleDpi(120, ctx->dpi)) colAppW = ScaleDpi(120, ctx->dpi);

        ListView_SetColumnWidth(ctx->hListProcs, 0, colAppW);
        ListView_SetColumnWidth(ctx->hListProcs, 1, colPidW);
        ListView_SetColumnWidth(ctx->hListProcs, 2, colTypeW);
    }

    // Create the Action Dropdown (ComboBox)
    ctx->hComboAction = CreateWindowExW(0, WC_COMBOBOXW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
        0, 0, 0, 0, ctx->hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_ACTION_COMBO)), ctx->hInstance, nullptr);

    SendMessageW(ctx->hComboAction, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Unlock & Delete File (Surgical Handle Close)"));
    SendMessageW(ctx->hComboAction, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Kill Locking Process & Delete File"));
    SendMessageW(ctx->hComboAction, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Unlock Only (Release Lock, Keep File)"));
    SendMessageW(ctx->hComboAction, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Kill Locking Process Only (Keep File)"));
    SendMessageW(ctx->hComboAction, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Delete File on Next Reboot"));
    SendMessageW(ctx->hComboAction, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Unlock & Rename File..."));
    SendMessageW(ctx->hComboAction, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Unlock & Move File to Folder..."));

    // If not running elevated, create the "Restart as Admin" button with UAC shield
    if (!IsRunningAsAdmin()) {
        ctx->hBtnAdmin = CreateWindowExW(0, L"BUTTON", L"Restart as Admin",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            0, 0, 0, 0, ctx->hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_ADMIN)), ctx->hInstance, nullptr);
        SendMessageW(ctx->hBtnAdmin, BCM_SETSHIELD, 0, TRUE);
    }

    // Create Recycle Bin Checkbox (QoL safeguard against accidental permanent deletion)
    ctx->hCheckRecycle = CreateWindowExW(0, L"BUTTON", L"Send to Recycle Bin (Safe Delete)",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
        0, 0, 0, 0, ctx->hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_RECYCLE_CHECK)), ctx->hInstance, nullptr);
    SendMessageW(ctx->hCheckRecycle, BM_SETCHECK, BST_CHECKED, 0);

    // Create "Execute" and "Close" Buttons (Exact names requested!)
    ctx->hBtnExecute = CreateWindowExW(0, L"BUTTON", L"Execute",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
        0, 0, 0, 0, ctx->hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_EXECUTE)), ctx->hInstance, nullptr);

    ctx->hBtnClose = CreateWindowExW(0, L"BUTTON", L"Close",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 0, 0, ctx->hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_CLOSE)), ctx->hInstance, nullptr);

    LayoutControls(ctx);
    ApplyThemeToControls(ctx);
    PopulateLockListAndDefaults(ctx, true);
}

static void UpdateSelectionButtonState(DialogContext* ctx) {
    if (!ctx || !ctx->hBtnSwitchTo || !ctx->hListProcs) return;

    int sel = ListView_GetNextItem(ctx->hListProcs, -1, LVNI_SELECTED);
    if (sel >= 0 && sel < static_cast<int>(ctx->scan.processes.size())) {
        const auto& p = ctx->scan.processes[sel];
        EnableWindow(ctx->hBtnSwitchTo, TRUE);
        EnableWindow(ctx->hBtnOpenLoc, TRUE);

        if (p.isService) {
            SetWindowTextW(ctx->hBtnSwitchTo, L"Stop Service");
        } else if (p.isExplorer) {
            SetWindowTextW(ctx->hBtnSwitchTo, L"Restart Shell");
        } else {
            SetWindowTextW(ctx->hBtnSwitchTo, L"Switch to App");
            if (!p.mainWindow) {
                EnableWindow(ctx->hBtnSwitchTo, FALSE);
            }
        }
    } else {
        SetWindowTextW(ctx->hBtnSwitchTo, L"Switch to App");
        EnableWindow(ctx->hBtnSwitchTo, FALSE);
        EnableWindow(ctx->hBtnOpenLoc, FALSE);
    }
}

struct RenameDialogContext {
    std::wstring originalName;
    std::wstring resultName;
    bool confirmed = false;
};

static LRESULT CALLBACK RenameDialogWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* ctx = reinterpret_cast<RenameDialogContext*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
        case WM_CREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            ctx = reinterpret_cast<RenameDialogContext*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));

            int dpi = GetWindowDpiSafe(hwnd);
            HFONT hFont = CreateUiFont(9, FW_NORMAL, dpi);
            HFONT hBold = CreateUiFont(9, FW_SEMIBOLD, dpi);

            HWND hLabel = CreateWindowExW(0, L"STATIC", L"Enter new name for the file / folder:",
                WS_CHILD | WS_VISIBLE | SS_LEFT,
                ScaleDpi(16, dpi), ScaleDpi(16, dpi), ScaleDpi(340, dpi), ScaleDpi(20, dpi),
                hwnd, nullptr, cs->hInstance, nullptr);
            SendMessageW(hLabel, WM_SETFONT, reinterpret_cast<WPARAM>(hBold), TRUE);

            HWND hEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", ctx->originalName.c_str(),
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                ScaleDpi(16, dpi), ScaleDpi(42, dpi), ScaleDpi(340, dpi), ScaleDpi(26, dpi),
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(2001)), cs->hInstance, nullptr);
            SendMessageW(hEdit, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), TRUE);
            SendMessageW(hEdit, EM_SETSEL, 0, -1);
            SetFocus(hEdit);

            HWND hBtnOk = CreateWindowExW(0, L"BUTTON", L"Rename",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                ScaleDpi(170, dpi), ScaleDpi(80, dpi), ScaleDpi(90, dpi), ScaleDpi(28, dpi),
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDOK)), cs->hInstance, nullptr);
            SendMessageW(hBtnOk, WM_SETFONT, reinterpret_cast<WPARAM>(hBold), TRUE);

            HWND hBtnCancel = CreateWindowExW(0, L"BUTTON", L"Cancel",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                ScaleDpi(266, dpi), ScaleDpi(80, dpi), ScaleDpi(90, dpi), ScaleDpi(28, dpi),
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDCANCEL)), cs->hInstance, nullptr);
            SendMessageW(hBtnCancel, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), TRUE);

            if (ThemeBridge().IsExplorerDarkModeEnabled()) {
                ThemeBridge().ApplyWindowChrome(hwnd, true);
                SetWindowTheme(hBtnOk, L"DarkMode_Explorer", nullptr);
                SetWindowTheme(hBtnCancel, L"DarkMode_Explorer", nullptr);
                SetWindowTheme(hEdit, L"DarkMode_CFD", nullptr);
            }
            return 0;
        }

        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (id == IDOK) {
                wchar_t buf[MAX_PATH] = {};
                GetDlgItemTextW(hwnd, 2001, buf, MAX_PATH);
                if (buf[0]) {
                    ctx->resultName = buf;
                    ctx->confirmed = true;
                    DestroyWindow(hwnd);
                }
                return 0;
            } else if (id == IDCANCEL) {
                ctx->confirmed = false;
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

static bool PromptRenameDialog(HWND hwndParent, const std::wstring& currentPath, std::wstring& outNewFullPath) {
    const wchar_t* curFileName = PathFindFileNameW(currentPath.c_str());
    if (!curFileName) curFileName = currentPath.c_str();

    RenameDialogContext rCtx;
    rCtx.originalName = curFileName;

    const wchar_t* kRenameClass = L"FileUnlockerRenamePromptClass";
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc   = RenameDialogWndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = kRenameClass;
    RegisterClassExW(&wc);

    int dpi = GetWindowDpiSafe(hwndParent);
    int dlgW = ScaleDpi(390, dpi);
    int dlgH = ScaleDpi(160, dpi);

    RECT rcParent = {};
    GetWindowRect(hwndParent, &rcParent);
    int posX = rcParent.left + (rcParent.right - rcParent.left - dlgW) / 2;
    int posY = rcParent.top + (rcParent.bottom - rcParent.top - dlgH) / 2;

    EnableWindow(hwndParent, FALSE);

    HWND hDlg = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
        kRenameClass,
        L"Unlock & Rename File",
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        posX, posY, dlgW, dlgH,
        hwndParent, nullptr, wc.hInstance, &rCtx);

    ShowWindow(hDlg, SW_SHOWNORMAL);
    UpdateWindow(hDlg);

    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hDlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    EnableWindow(hwndParent, TRUE);
    SetForegroundWindow(hwndParent);
    UnregisterClassW(kRenameClass, wc.hInstance);

    if (rCtx.confirmed && !rCtx.resultName.empty()) {
        wchar_t dirBuf[MAX_PATH * 2] = {};
        wcscpy_s(dirBuf, currentPath.c_str());
        PathRemoveFileSpecW(dirBuf);
        PathAppendW(dirBuf, rCtx.resultName.c_str());
        outNewFullPath = dirBuf;
        return true;
    }
    return false;
}

static bool PickFolderDialog(HWND hwndParent, std::wstring& outFolder) {
    BROWSEINFOW bi = {};
    bi.hwndOwner = hwndParent;
    bi.lpszTitle = L"Select destination folder to move the unlocked file/folder:";
    bi.ulFlags   = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_USENEWUI;

    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (pidl) {
        wchar_t path[MAX_PATH * 2] = {};
        if (SHGetPathFromIDListW(pidl, path)) {
            outFolder = path;
            CoTaskMemFree(pidl);
            return true;
        }
        CoTaskMemFree(pidl);
    }
    return false;
}

// ----------------------------------------------------------------------------
// Executing the Selected Dropdown Action
// ----------------------------------------------------------------------------
static void OnExecuteClicked(DialogContext* ctx) {
    int sel = static_cast<int>(SendMessageW(ctx->hComboAction, CB_GETCURSEL, 0, 0));
    if (sel < 0) sel = 0;
    ActionType action = static_cast<ActionType>(sel);

    // Safety check #1: Block killing critical system processes
    if (action == ActionType::KillAndDelete || action == ActionType::KillOnly) {
        for (const auto& p : ctx->scan.processes) {
            if (p.isSystemProtected) {
                MessageBoxW(ctx->hwnd,
                    (L"Cannot terminate '" + p.processName + L"' (PID " + std::to_wstring(p.pid) +
                     L") because it is a critical Windows system process.\n\n"
                     L"Please choose 'Unlock & Delete File' or 'Delete File on Next Reboot' instead.").c_str(),
                    L"System Process Protected",
                    MB_ICONWARNING | MB_OK);
                return;
            }
        }

        // Safety check #2: Confirm before terminating user applications (except Explorer preview hosts)
        bool hasNonExplorerApp = false;
        std::wstring appList;
        for (const auto& p : ctx->scan.processes) {
            if (!p.isExplorer) {
                hasNonExplorerApp = true;
                if (!appList.empty()) appList += L", ";
                appList += p.friendlyName + L" (" + p.processName + L")";
            }
        }
        if (hasNonExplorerApp) {
            std::wstring prompt = L"Are you sure you want to forcibly terminate:\n\n" + appList +
                                  L"\n\nAny unsaved work in that application will be lost.";
            if (MessageBoxW(ctx->hwnd, prompt.c_str(), L"Confirm Process Termination",
                            MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) != IDYES)
            {
                return;
            }
        }
    }

    bool useRecycleBin = (SendMessageW(ctx->hCheckRecycle, BM_GETCHECK, 0, 0) == BST_CHECKED);
    // If Shift is held down while clicking Execute, bypass Recycle Bin like Explorer does!
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000) {
        useRecycleBin = false;
    }

    ActionResult res;
    if (action == ActionType::UnlockAndRename) {
        if (ctx->isMultiple) {
            MessageBoxW(ctx->hwnd, L"Batch Rename is not supported for multiple items simultaneously.\nPlease select a single file to rename.", L"Unlock & Rename", MB_ICONINFORMATION);
            return;
        }
        std::wstring newFullPath;
        if (!PromptRenameDialog(ctx->hwnd, ctx->filePath, newFullPath)) {
            return; // Canceled by user
        }
        res = ExecuteUnlockerAction(ctx->filePath, action, ctx->scan.processes, false, nullptr, newFullPath);
    } else if (action == ActionType::UnlockAndMove) {
        std::wstring destFolder;
        if (!PickFolderDialog(ctx->hwnd, destFolder)) {
            return; // Canceled by user
        }
        if (ctx->isMultiple) {
            int totalMoved = 0;
            for (const auto& p : ctx->targetPaths) {
                ActionResult r = ExecuteUnlockerAction(p, action, ctx->scan.processes, false, nullptr, destFolder);
                if (r.succeeded) totalMoved++;
            }
            res.succeeded = (totalMoved > 0);
            res.statusMessage = L"Moved " + std::to_wstring(totalMoved) + L" item(s) to:\n" + destFolder;
        } else {
            res = ExecuteUnlockerAction(ctx->filePath, action, ctx->scan.processes, false, nullptr, destFolder);
        }
    } else if (ctx->isMultiple) {
        // Execute chosen action across all selected files/folders
        int totalClosed = 0;
        int totalKilled = 0;
        int totalDeleted = 0;
        bool allSucceeded = true;

        for (const auto& path : ctx->targetPaths) {
            ActionResult r = ExecuteUnlockerAction(path, action, ctx->scan.processes, useRecycleBin, nullptr);
            totalClosed += r.handlesClosed;
            totalKilled += r.procsKilled;
            if (r.fileDeleted) totalDeleted++;
            if (!r.succeeded) allSucceeded = false;
        }

        res.succeeded = allSucceeded || totalDeleted > 0 || totalClosed > 0;
        res.statusMessage = L"Completed on " + std::to_wstring(ctx->targetPaths.size()) + L" items (" +
                            std::to_wstring(totalClosed) + L" handles closed, " +
                            std::to_wstring(totalDeleted) + L" items deleted).";
    } else {
        res = ExecuteUnlockerAction(
            ctx->filePath,
            action,
            ctx->scan.processes,
            useRecycleBin,
            ctx->explorerConflictDlg
        );
    }

    if (res.succeeded) {
        ctx->actionSucceeded = true;
        DestroyWindow(ctx->hwnd);
    } else {
        PopulateLockListAndDefaults(ctx, false);
        MessageBoxW(ctx->hwnd,
            res.statusMessage.c_str(),
            L"Action Incomplete",
            MB_ICONERROR | MB_OK);
    }
}

// ----------------------------------------------------------------------------
// Window Procedure
// ----------------------------------------------------------------------------
static LRESULT CALLBACK UnlockerWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* ctx = reinterpret_cast<DialogContext*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
        case WM_CREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            ctx = reinterpret_cast<DialogContext*>(cs->lpCreateParams);
            ctx->hwnd = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));
            InitializeDialogControls(ctx);
            return 0;
        }

        case WM_DPICHANGED: {
            if (ctx) {
                ctx->dpi = HIWORD(wParam);
                RefreshFonts(ctx);
                auto* suggested = reinterpret_cast<RECT*>(lParam);
                SetWindowPos(hwnd, nullptr,
                    suggested->left, suggested->top,
                    suggested->right - suggested->left,
                    suggested->bottom - suggested->top,
                    SWP_NOZORDER | SWP_NOACTIVATE);
                LayoutControls(ctx);
                InvalidateRect(hwnd, nullptr, TRUE);
            }
            return 0;
        }

        case WM_SETTINGCHANGE: {
            // Live switch between Dark and Light mode when user toggles Windows 10/11 theme!
            if (ctx && lParam && wcscmp(reinterpret_cast<LPCWSTR>(lParam), L"ImmersiveColorSet") == 0) {
                ApplyThemeToControls(ctx);
            }
            break;
        }

        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN: {
            if (ctx) {
                HDC hdcCtrl = reinterpret_cast<HDC>(wParam);
                SetBkMode(hdcCtrl, TRANSPARENT);
                SetTextColor(hdcCtrl, ctx->isDark ? RGB(245, 245, 245) : GetSysColor(COLOR_BTNTEXT));
                return reinterpret_cast<LRESULT>(ctx->hBgBrush);
            }
            break;
        }

        case WM_ERASEBKGND:
            return 1; // Handled in WM_PAINT double-buffer to avoid flicker

        case WM_PAINT: {
            PAINTSTRUCT ps = {};
            HDC hdc = BeginPaint(hwnd, &ps);
            if (ctx) {
                RECT rc = {};
                GetClientRect(hwnd, &rc);
                HDC hMemDC = CreateCompatibleDC(hdc);
                HBITMAP hBmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
                HGDIOBJ hOldBmp = SelectObject(hMemDC, hBmp);

                PaintDialogSurface(ctx, hMemDC, rc);
                BitBlt(hdc, 0, 0, rc.right, rc.bottom, hMemDC, 0, 0, SRCCOPY);

                SelectObject(hMemDC, hOldBmp);
                DeleteObject(hBmp);
                DeleteDC(hMemDC);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_NOTIFY: {
            if (ctx && wParam == IDC_PROCESS_LIST) {
                auto* nmhdr = reinterpret_cast<NMHDR*>(lParam);
                if (nmhdr->code == LVN_ITEMCHANGED) {
                    UpdateSelectionButtonState(ctx);
                } else if (nmhdr->code == NM_DBLCLK) {
                    // Double-clicking a locking process brings its window to the front!
                    int sel = ListView_GetNextItem(ctx->hListProcs, -1, LVNI_SELECTED);
                    if (sel >= 0 && sel < static_cast<int>(ctx->scan.processes.size())) {
                        const auto& p = ctx->scan.processes[sel];
                        BringProcessWindowToFront(p.pid, p.mainWindow);
                    }
                }
            }
            break;
        }

        case WM_COMMAND: {
            if (!ctx) break;
            int id   = LOWORD(wParam);
            int code = HIWORD(wParam);

            if (id == IDC_ACTION_COMBO && code == CBN_SELCHANGE) {
                UpdateWarningTextForSelection(ctx);
                int sel = static_cast<int>(SendMessageW(ctx->hComboAction, CB_GETCURSEL, 0, 0));
                ActionType act = static_cast<ActionType>(sel);
                bool deletesFile = (act == ActionType::UnlockAndDelete || act == ActionType::KillAndDelete);
                EnableWindow(ctx->hCheckRecycle, deletesFile);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }

            switch (id) {
                case IDC_BTN_REFRESH:
                    PopulateLockListAndDefaults(ctx, false);
                    return 0;

                case IDC_BTN_ADMIN: {
                    std::vector<std::wstring> targets = ctx->isMultiple ? ctx->targetPaths : std::vector<std::wstring>{ ctx->filePath };
                    if (RelaunchAsAdmin(hwnd, targets)) {
                        ctx->actionSucceeded = true;
                        DestroyWindow(hwnd);
                    }
                    return 0;
                }

                case IDC_BTN_SWITCH_TO: {
                    int sel = ListView_GetNextItem(ctx->hListProcs, -1, LVNI_SELECTED);
                    if (sel >= 0 && sel < static_cast<int>(ctx->scan.processes.size())) {
                        const auto& p = ctx->scan.processes[sel];
                        if (p.isService && !p.serviceName.empty()) {
                            std::wstring err;
                            if (StopWindowsService(p.serviceName, &err)) {
                                PopulateLockListAndDefaults(ctx, false);
                            } else {
                                MessageBoxW(hwnd, err.c_str(), L"Stop Service Failed", MB_ICONERROR);
                            }
                        } else if (p.isExplorer) {
                            std::wstring err;
                            RestartWindowsExplorer(&err);
                            PopulateLockListAndDefaults(ctx, false);
                        } else {
                            BringProcessWindowToFront(p.pid, p.mainWindow);
                        }
                    }
                    return 0;
                }

                case IDC_BTN_OPEN_LOC: {
                    int sel = ListView_GetNextItem(ctx->hListProcs, -1, LVNI_SELECTED);
                    if (sel >= 0 && sel < static_cast<int>(ctx->scan.processes.size())) {
                        OpenProcessFileLocation(ctx->scan.processes[sel].fullImagePath);
                    }
                    return 0;
                }

                case IDC_BTN_SETTINGS:
                    ShowSettingsDialog(hwnd, ctx->hInstance);
                    return 0;

                case IDC_BTN_EXECUTE:
                case IDOK:
                    OnExecuteClicked(ctx);
                    return 0;

                case IDC_BTN_CLOSE:
                case IDCANCEL:
                    // User clicked "Close": dismiss Explorer's conflict dialog cleanly or show it back
                    if (ctx->explorerConflictDlg && IsWindow(ctx->explorerConflictDlg)) {
                        PostMessageW(ctx->explorerConflictDlg, WM_CLOSE, 0, 0);
                        PostMessageW(ctx->explorerConflictDlg, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
                    }
                    DestroyWindow(hwnd);
                    return 0;
            }
            break;
        }

        case WM_CLOSE:
            if (ctx && ctx->explorerConflictDlg && IsWindow(ctx->explorerConflictDlg) && !ctx->actionSucceeded) {
                PostMessageW(ctx->explorerConflictDlg, WM_CLOSE, 0, 0);
                PostMessageW(ctx->explorerConflictDlg, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
            }
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            PostQuitMessage(ctx && ctx->actionSucceeded ? 0 : 1);
            return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ----------------------------------------------------------------------------
// Public Entry Point for Launching the Dialog
// ----------------------------------------------------------------------------
int ShowUnlockerDialog(HINSTANCE hInstance, const std::vector<std::wstring>& targetPaths, HWND explorerConflictHwnd) {
    if (targetPaths.empty()) return 1;
    if (targetPaths.size() == 1) {
        return ShowUnlockerDialog(hInstance, targetPaths[0], explorerConflictHwnd);
    }

    INITCOMMONCONTROLSEX icex = {};
    icex.dwSize = sizeof(icex);
    icex.dwICC  = ICC_WIN95_CLASSES | ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icex);

    const wchar_t* kClassName = L"FileUnlockerNativeDialogClass";

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = UnlockerWndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon         = LoadIconW(hInstance, MAKEINTRESOURCEW(101));
    if (!wc.hIcon)   wc.hIcon = LoadIconW(nullptr, IDI_SHIELD);
    wc.hIconSm       = LoadIconW(hInstance, MAKEINTRESOURCEW(101));
    if (!wc.hIconSm) wc.hIconSm = LoadIconW(nullptr, IDI_SHIELD);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);

    DialogContext ctx;
    ctx.hInstance           = hInstance;
    ctx.isMultiple          = true;
    ctx.targetPaths         = targetPaths;
    ctx.filePath            = targetPaths[0];
    ctx.explorerConflictDlg = explorerConflictHwnd;

    int dpi = GetWindowDpiSafe(nullptr);
    int clientW = ScaleDpi(540, dpi);
    int clientH = ScaleDpi(404, dpi);

    DWORD style   = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    DWORD exStyle = WS_EX_APPWINDOW;

    RECT rcWin = { 0, 0, clientW, clientH };
    AdjustWindowRectEx(&rcWin, style, FALSE, exStyle);
    int winW = rcWin.right - rcWin.left;
    int winH = rcWin.bottom - rcWin.top;

    int posX = (GetSystemMetrics(SM_CXSCREEN) - winW) / 2;
    int posY = (GetSystemMetrics(SM_CYSCREEN) - winH) / 2;

    HWND hwnd = CreateWindowExW(
        exStyle,
        kClassName,
        L"File In Use \x2014 DelUnlock (Batch Selection)",
        style,
        posX, posY, winW, winH,
        nullptr, nullptr, hInstance, &ctx);

    if (!hwnd) return 1;

    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);
    SetForegroundWindow(hwnd);
    BringWindowToTop(hwnd);

    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return static_cast<int>(msg.wParam);
}

int ShowUnlockerDialog(HINSTANCE hInstance, const std::wstring& targetPath, HWND explorerConflictHwnd) {
    INITCOMMONCONTROLSEX icex = {};
    icex.dwSize = sizeof(icex);
    icex.dwICC  = ICC_WIN95_CLASSES | ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icex);

    const wchar_t* kClassName = L"FileUnlockerNativeDialogClass";

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = UnlockerWndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon         = LoadIconW(hInstance, MAKEINTRESOURCEW(101));
    if (!wc.hIcon)   wc.hIcon = LoadIconW(nullptr, IDI_SHIELD);
    wc.hIconSm       = LoadIconW(hInstance, MAKEINTRESOURCEW(101));
    if (!wc.hIconSm) wc.hIconSm = LoadIconW(nullptr, IDI_SHIELD);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);

    DialogContext ctx;
    ctx.hInstance           = hInstance;
    ctx.filePath            = targetPath;
    ctx.explorerConflictDlg = explorerConflictHwnd;

    int dpi = GetWindowDpiSafe(nullptr);
    int clientW = ScaleDpi(540, dpi);
    int clientH = ScaleDpi(404, dpi) + (ctx.isSystemCritical ? ScaleDpi(18, dpi) : 0);

    DWORD style   = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    DWORD exStyle = WS_EX_APPWINDOW;

    RECT rcWin = { 0, 0, clientW, clientH };
    AdjustWindowRectEx(&rcWin, style, FALSE, exStyle);
    int winW = rcWin.right - rcWin.left;
    int winH = rcWin.bottom - rcWin.top;

    int posX = (GetSystemMetrics(SM_CXSCREEN) - winW) / 2;
    int posY = (GetSystemMetrics(SM_CYSCREEN) - winH) / 2;

    HWND hwnd = CreateWindowExW(
        exStyle,
        kClassName,
        L"File In Use \x2014 DelUnlock",
        style,
        posX, posY, winW, winH,
        nullptr, nullptr, hInstance, &ctx);

    if (!hwnd) {
        DWORD err = GetLastError();
        wchar_t msg[128];
        wsprintfW(msg, L"CreateWindowEx failed with error %lu", err);
        MessageBoxW(nullptr, msg, L"Error", MB_ICONERROR);
        return 1;
    }

    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);
    SetForegroundWindow(hwnd);
    BringWindowToTop(hwnd);
    SetForegroundWindow(hwnd);

    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return static_cast<int>(msg.wParam);
}

} // namespace unlocker
