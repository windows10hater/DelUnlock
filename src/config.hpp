// ============================================================================
// config.hpp
// Lightweight INI configuration + Context Menu manager.
//
// Stored next to unlocker.exe as `unlocker.ini`.
// If the user transfers unlocker.exe to a new machine or friend,
// FirstRun=1 triggers a friendly, fast welcome dialog explaining how to use it
// and letting them enable/disable the context menu with a single click.
// ============================================================================

#pragma once

#include <windows.h>
#include <string>

namespace unlocker {

struct AppConfig {
    bool isFirstRun         = true;
    bool contextMenuEnabled = true;
};

// Gets the full path to `unlocker.ini` next to the executable
std::wstring GetIniFilePath();

// Gets the full path of `unlocker.exe`
std::wstring GetExecutablePath();

// Loads or initializes configuration
AppConfig LoadConfig();

// Saves configuration to `unlocker.ini`
void SaveConfig(const AppConfig& cfg);

// Checks if the Explorer right-click context menu is currently registered
bool IsContextMenuInstalled();

// Adds or updates Explorer right-click context menu under HKCU
bool InstallContextMenu();

// Removes Explorer right-click context menu from HKCU
bool UninstallContextMenu();

// Shows the first-time setup / welcome window
// Returns true if the user clicked "Continue", false if closed.
bool ShowFirstRunSetupDialog(HINSTANCE hInstance);

// Shows the Settings dialog (toggle context menu, view version/about)
void ShowSettingsDialog(HWND hParent, HINSTANCE hInstance);

} // namespace unlocker
