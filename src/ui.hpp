// ============================================================================
// ui.hpp
// Chameleon OS-Native Dialog (XP Luna -> Win7 Aero -> Win10/11 Dark & Light Mica)
//
// Why fight the OS when you can let the OS dress itself?
// - On Windows XP/Vista/7/8, ComCtl32 v6 automatically paints genuine Luna/Aero
//   buttons, dropdowns, and ClearType fonts.
// - On Windows 10 & 11, we read Explorer's live Dark/Light mode setting from the
//   registry, enable DWM Dark Titlebar + Windows 11 Mica backdrop + rounded corners,
//   and hook into uxtheme.dll's DarkMode_Explorer / DarkMode_CFD classes so every
//   control looks 100% native in both Dark and Light mode.
// ============================================================================

#pragma once

#include "unlocker.hpp"

namespace unlocker {

// Launches the interactive Unlocker window for a single target or multiple selected targets.
int ShowUnlockerDialog(
    HINSTANCE hInstance,
    const std::vector<std::wstring>& targetPaths,
    HWND explorerConflictHwnd = nullptr
);

int ShowUnlockerDialog(
    HINSTANCE hInstance,
    const std::wstring& singlePath,
    HWND explorerConflictHwnd = nullptr
);

} // namespace unlocker
