// ============================================================================
// unlocker.cpp
// The heavy lifter: Restart Manager + NT Kernel Handle Scanner + Surgical Unlocker.
//
// Fun fact: Windows has no documented API that says "give me all HANDLE values
// pointing to C:\foo.txt". Instead, you have to ask ntdll for a giant list of
// every handle in the universe, filter down to File objects, duplicate them
// into your own process, and query their kernel path—while dodging synchronous
// named pipes that will happily deadlock your thread until the heat death of
// the universe. We handle all of that cleanly here.
// ============================================================================

#include "unlocker.hpp"

#include <shellapi.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <shlwapi.h>
#include <aclapi.h>
#include <winsvc.h>
#include <algorithm>
#include <cwctype>
#include <set>
#include <map>

#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "version.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")

namespace unlocker {

// ----------------------------------------------------------------------------
// Undocumented / Semi-documented NT & Restart Manager declarations
// Loaded dynamically so we never fail to launch on Windows XP.
// ----------------------------------------------------------------------------

typedef LONG NTSTATUS;
#define STATUS_SUCCESS               ((NTSTATUS)0x00000000L)
#define STATUS_INFO_LENGTH_MISMATCH  ((NTSTATUS)0xC0000004L)

constexpr ULONG SystemHandleInformation         = 16; // Works on XP through 11 (16-bit handle IDs)
constexpr ULONG SystemExtendedHandleInformation = 64; // Vista+ (full ULONG_PTR handle IDs, never truncates!)

#pragma pack(push, 1)
// Classic XP structure (SystemHandleInformation = 16)
struct SYSTEM_HANDLE_TABLE_ENTRY_INFO {
    USHORT UniqueProcessId;
    USHORT CreatorBackTraceIndex;
    UCHAR  ObjectTypeIndex;
    UCHAR  HandleAttributes;
    USHORT HandleValue;
    PVOID  Object;
    ULONG  GrantedAccess;
};

struct SYSTEM_HANDLE_INFORMATION {
    ULONG NumberOfHandles;
    SYSTEM_HANDLE_TABLE_ENTRY_INFO Handles[1];
};
#pragma pack(pop)

// Vista+ structure (SystemExtendedHandleInformation = 64)
struct SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX {
    PVOID     Object;
    ULONG_PTR UniqueProcessId;
    ULONG_PTR HandleValue;
    ULONG     GrantedAccess;
    USHORT    CreatorBackTraceIndex;
    USHORT    ObjectTypeIndex;
    ULONG     HandleAttributes;
    ULONG     Reserved;
};

struct SYSTEM_HANDLE_INFORMATION_EX {
    ULONG_PTR NumberOfHandles;
    ULONG_PTR Reserved;
    SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX Handles[1];
};

struct UNICODE_STRING_NT {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR  Buffer;
};

struct OBJECT_NAME_INFORMATION_NT {
    UNICODE_STRING_NT Name;
};

struct IO_STATUS_BLOCK_NT {
    union {
        NTSTATUS Status;
        PVOID    Pointer;
    };
    ULONG_PTR Information;
};

// FileNameInformation (class 9) reads FILE_OBJECT->FileName from kernel memory
// and is much less prone to hanging than GetFinalPathNameByHandleW.
constexpr ULONG FileNameInformationClass = 9;

struct FILE_NAME_INFORMATION_NT {
    ULONG FileNameLength;
    WCHAR FileName[1];
};

using PFN_NtQuerySystemInformation = NTSTATUS(NTAPI*)(
    ULONG SystemInformationClass,
    PVOID SystemInformation,
    ULONG SystemInformationLength,
    PULONG ReturnLength
);

using PFN_NtQueryObject = NTSTATUS(NTAPI*)(
    HANDLE Handle,
    ULONG ObjectInformationClass,
    PVOID ObjectInformation,
    ULONG ObjectInformationLength,
    PULONG ReturnLength
);

using PFN_NtQueryInformationFile = NTSTATUS(NTAPI*)(
    HANDLE FileHandle,
    IO_STATUS_BLOCK_NT* IoStatusBlock,
    PVOID FileInformation,
    ULONG Length,
    ULONG FileInformationClass
);

using PFN_QueryFullProcessImageNameW = BOOL(WINAPI*)(
    HANDLE hProcess,
    DWORD dwFlags,
    LPWSTR lpExeName,
    PDWORD lpdwSize
);

// Restart Manager definitions (Vista+ RstrtMgr.dll)
#define CCH_RM_MAX_APP_NAME 255
#define CCH_RM_MAX_SVC_NAME 63
#define CCH_RM_SESSION_KEY  32

struct RM_UNIQUE_PROCESS_DYN {
    DWORD    dwProcessId;
    FILETIME ProcessStartTime;
};

struct RM_PROCESS_INFO_DYN {
    RM_UNIQUE_PROCESS_DYN Process;
    WCHAR                 strAppName[CCH_RM_MAX_APP_NAME + 1];
    WCHAR                 strServiceShortName[CCH_RM_MAX_SVC_NAME + 1];
    ULONG                 ApplicationType;
    ULONG                 AppStatus;
    DWORD                 TSSessionId;
    BOOL                  bRestartable;
};

using PFN_RmStartSession      = DWORD(WINAPI*)(DWORD* pSessionHandle, DWORD dwSessionFlags, WCHAR strSessionKey[]);
using PFN_RmRegisterResources = DWORD(WINAPI*)(DWORD dwSessionHandle, UINT nFiles, LPCWSTR rgsFileNames[], UINT nApplications, void* rgApplications, UINT nServices, LPCWSTR rgsServiceNames[]);
using PFN_RmGetList           = DWORD(WINAPI*)(DWORD dwSessionHandle, UINT* pnProcInfoNeeded, UINT* pnProcInfo, RM_PROCESS_INFO_DYN rgAffectedApps[], LPDWORD lpdwRebootReasons);
using PFN_RmEndSession        = DWORD(WINAPI*)(DWORD dwSessionHandle);

// ----------------------------------------------------------------------------
// Lazy-loaded function table
// ----------------------------------------------------------------------------
struct NtApiTable {
    PFN_NtQuerySystemInformation   NtQuerySystemInformation   = nullptr;
    PFN_NtQueryObject              NtQueryObject              = nullptr;
    PFN_NtQueryInformationFile     NtQueryInformationFile     = nullptr;
    PFN_QueryFullProcessImageNameW QueryFullProcessImageNameW = nullptr;

    HMODULE                  hRstrtMgr           = nullptr;
    PFN_RmStartSession       RmStartSession      = nullptr;
    PFN_RmRegisterResources  RmRegisterResources = nullptr;
    PFN_RmGetList            RmGetList           = nullptr;
    PFN_RmEndSession         RmEndSession        = nullptr;

    NtApiTable() {
        HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
        if (hNtdll) {
            NtQuerySystemInformation = reinterpret_cast<PFN_NtQuerySystemInformation>(
                GetProcAddress(hNtdll, "NtQuerySystemInformation"));
            NtQueryObject = reinterpret_cast<PFN_NtQueryObject>(
                GetProcAddress(hNtdll, "NtQueryObject"));
            NtQueryInformationFile = reinterpret_cast<PFN_NtQueryInformationFile>(
                GetProcAddress(hNtdll, "NtQueryInformationFile"));
        }

        HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
        if (hKernel32) {
            QueryFullProcessImageNameW = reinterpret_cast<PFN_QueryFullProcessImageNameW>(
                GetProcAddress(hKernel32, "QueryFullProcessImageNameW"));
        }

        // RstrtMgr.dll exists on Vista, 7, 8, 8.1, 10, 11. On XP this returns nullptr and we gracefully skip it.
        hRstrtMgr = LoadLibraryW(L"rstrtmgr.dll");
        if (hRstrtMgr) {
            RmStartSession      = reinterpret_cast<PFN_RmStartSession>(GetProcAddress(hRstrtMgr, "RmStartSession"));
            RmRegisterResources = reinterpret_cast<PFN_RmRegisterResources>(GetProcAddress(hRstrtMgr, "RmRegisterResources"));
            RmGetList           = reinterpret_cast<PFN_RmGetList>(GetProcAddress(hRstrtMgr, "RmGetList"));
            RmEndSession        = reinterpret_cast<PFN_RmEndSession>(GetProcAddress(hRstrtMgr, "RmEndSession"));
        }
    }
};

static const NtApiTable& Api() {
    static NtApiTable instance;
    return instance;
}

// ----------------------------------------------------------------------------
// String & Path Helpers
// ----------------------------------------------------------------------------

static std::wstring ToLower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return std::towlower(c); });
    return s;
}

static bool EqualsIgnoreCase(const std::wstring& a, const std::wstring& b) {
    if (a.size() != b.size()) return false;
    return _wcsicmp(a.c_str(), b.c_str()) == 0;
}

static bool StartsWithIgnoreCase(const std::wstring& str, const std::wstring& prefix) {
    if (str.size() < prefix.size()) return false;
    return _wcsnicmp(str.c_str(), prefix.c_str(), prefix.size()) == 0;
}

// Converts a Win32 path like "C:\Users\Karee\file.txt" into both:
// 1. Canonical NT Device path: "\Device\HarddiskVolume3\Users\Karee\file.txt"
// 2. Drive-less relative path: "\Users\Karee\file.txt" (for FileNameInformation comparison)
// Importantly: calls GetLongPathNameW first so 8.3 short paths like "C:\Users\Karee\AppData\Local\Temp\unlocker~1.tmp"
// are expanded to match the full kernel FILE_OBJECT path!
static bool BuildNtDevicePaths(const std::wstring& win32Path, std::wstring& outDevicePath, std::wstring& outVolumeRelativePath) {
    wchar_t fullPath[MAX_PATH * 2] = {};
    if (!GetFullPathNameW(win32Path.c_str(), MAX_PATH * 2, fullPath, nullptr)) {
        wcscpy_s(fullPath, win32Path.c_str());
    }

    wchar_t longPath[MAX_PATH * 2] = {};
    if (GetLongPathNameW(fullPath, longPath, MAX_PATH * 2) > 0) {
        wcscpy_s(fullPath, longPath);
    }

    // Strip trailing slash unless root
    size_t len = wcslen(fullPath);
    if (len > 3 && (fullPath[len - 1] == L'\\' || fullPath[len - 1] == L'/')) {
        fullPath[len - 1] = L'\0';
    }

    if (wcslen(fullPath) >= 2 && fullPath[1] == L':') {
        wchar_t drive[3] = { fullPath[0], L':', L'\0' };
        wchar_t deviceName[MAX_PATH] = {};
        if (QueryDosDeviceW(drive, deviceName, MAX_PATH) > 0) {
            outVolumeRelativePath = (fullPath + 2); // e.g. "\Users\Karee\file.txt"
            outDevicePath = std::wstring(deviceName) + outVolumeRelativePath;
            return true;
        }
    }
    outVolumeRelativePath = fullPath;
    outDevicePath = fullPath;
    return false;
}

// ----------------------------------------------------------------------------
// Privileges & Safety Guardrails
// ----------------------------------------------------------------------------

bool EnableDebugPrivilege() {
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
        return false;
    }

    const wchar_t* privNames[] = {
        SE_DEBUG_NAME,
        SE_TAKE_OWNERSHIP_NAME,
        SE_SECURITY_NAME,
        SE_BACKUP_NAME,
        SE_RESTORE_NAME
    };

    for (const wchar_t* pName : privNames) {
        TOKEN_PRIVILEGES tp = {};
        if (LookupPrivilegeValueW(nullptr, pName, &tp.Privileges[0].Luid)) {
            tp.PrivilegeCount = 1;
            tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
            AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), nullptr, nullptr);
        }
    }

    CloseHandle(hToken);
    return true;
}

bool IsRunningAsAdmin() {
    BOOL isAdmin = FALSE;
    PSID adminGroup = nullptr;
    SID_IDENTIFIER_AUTHORITY ntAuthority = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&ntAuthority, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                 DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &adminGroup)) {
        CheckTokenMembership(nullptr, adminGroup, &isAdmin);
        FreeSid(adminGroup);
    }
    return (isAdmin != FALSE);
}

bool RelaunchAsAdmin(HWND hwndParent, const std::vector<std::wstring>& targetPaths) {
    wchar_t exePath[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH * 2);

    std::wstring params;
    for (const auto& p : targetPaths) {
        if (!params.empty()) params += L" ";
        params += L"\"" + p + L"\"";
    }

    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    sei.lpVerb       = L"runas";
    sei.lpFile       = exePath;
    sei.lpParameters = params.c_str();
    sei.hwnd         = hwndParent;
    sei.nShow        = SW_SHOWNORMAL;

    return (ShellExecuteExW(&sei) != FALSE);
}

bool TakeOwnershipAndGrantAccess(const std::wstring& path, std::wstring* outError) {
    EnableDebugPrivilege();

    DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES) {
        // Clear Read-Only, Hidden, and System attributes so file system permits modification
        SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
    }

    // 1. Get Administrators group SID
    PSID pAdminSid = nullptr;
    SID_IDENTIFIER_AUTHORITY ntAuth = SECURITY_NT_AUTHORITY;
    if (!AllocateAndInitializeSid(&ntAuth, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
                                 0, 0, 0, 0, 0, 0, &pAdminSid)) {
        if (outError) *outError = L"Failed to allocate Admin SID.";
        return false;
    }

    // 2. Set Owner to Administrators
    SetNamedSecurityInfoW(
        const_cast<wchar_t*>(path.c_str()),
        SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION,
        pAdminSid,
        nullptr,
        nullptr,
        nullptr
    );

    // 3. Grant Full Control (GENERIC_ALL) to Administrators in DACL
    EXPLICIT_ACCESSW ea = {};
    ea.grfAccessPermissions = GENERIC_ALL;
    ea.grfAccessMode        = SET_ACCESS;
    ea.grfInheritance       = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    ea.Trustee.TrusteeForm  = TRUSTEE_IS_SID;
    ea.Trustee.TrusteeType  = TRUSTEE_IS_WELL_KNOWN_GROUP;
    ea.Trustee.ptstrName    = reinterpret_cast<LPWSTR>(pAdminSid);

    PACL pNewDacl = nullptr;
    if (SetEntriesInAclW(1, &ea, nullptr, &pNewDacl) == ERROR_SUCCESS) {
        SetNamedSecurityInfoW(
            const_cast<wchar_t*>(path.c_str()),
            SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION,
            nullptr,
            nullptr,
            pNewDacl,
            nullptr
        );
        LocalFree(pNewDacl);
    }

    FreeSid(pAdminSid);

    // If it's a directory, recursively take ownership and grant access to all contents
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        std::wstring searchPattern = path + L"\\*";
        WIN32_FIND_DATAW ffd = {};
        HANDLE hFind = FindFirstFileW(searchPattern.c_str(), &ffd);
        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                if (wcscmp(ffd.cFileName, L".") == 0 || wcscmp(ffd.cFileName, L"..") == 0) continue;
                std::wstring child = path + L"\\" + ffd.cFileName;
                TakeOwnershipAndGrantAccess(child, nullptr);
            } while (FindNextFileW(hFind, &ffd));
            FindClose(hFind);
        }
    }

    return true;
}

bool RenameTargetFile(const std::wstring& oldPath, const std::wstring& newPath, std::wstring* outError) {
    if (MoveFileExW(oldPath.c_str(), newPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return true;
    }

    DWORD err = GetLastError();
    if (err == ERROR_ACCESS_DENIED) {
        TakeOwnershipAndGrantAccess(oldPath);
        if (MoveFileExW(oldPath.c_str(), newPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            return true;
        }
        err = GetLastError();
    }

    if (outError) {
        wchar_t msg[256] = {};
        FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, err, 0, msg, 256, nullptr);
        *outError = L"Rename failed (Error " + std::to_wstring(err) + L"): " + msg;
    }
    return false;
}

bool MoveTargetFile(const std::wstring& oldPath, const std::wstring& destFolder, std::wstring* outError) {
    const wchar_t* fname = PathFindFileNameW(oldPath.c_str());
    if (!fname || !*fname) {
        if (outError) *outError = L"Invalid source file path.";
        return false;
    }

    wchar_t destBuf[MAX_PATH * 2] = {};
    wcscpy_s(destBuf, destFolder.c_str());
    PathAppendW(destBuf, fname);

    if (MoveFileExW(oldPath.c_str(), destBuf, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH | MOVEFILE_COPY_ALLOWED)) {
        return true;
    }

    DWORD err = GetLastError();
    if (err == ERROR_ACCESS_DENIED) {
        TakeOwnershipAndGrantAccess(oldPath);
        if (MoveFileExW(oldPath.c_str(), destBuf, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH | MOVEFILE_COPY_ALLOWED)) {
            return true;
        }
        err = GetLastError();
    }

    if (outError) {
        wchar_t msg[256] = {};
        FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, err, 0, msg, 256, nullptr);
        *outError = L"Move failed (Error " + std::to_wstring(err) + L"): " + msg;
    }
    return false;
}

struct ServiceEntry {
    std::wstring serviceName;
    std::wstring displayName;
};

static std::map<DWORD, ServiceEntry> QueryPidToServiceMap() {
    std::map<DWORD, ServiceEntry> pidMap;
    SC_HANDLE hSCM = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE | SC_MANAGER_CONNECT);
    if (!hSCM) return pidMap;

    DWORD bytesNeeded = 0;
    DWORD servicesReturned = 0;
    DWORD resumeHandle = 0;

    EnumServicesStatusExW(
        hSCM,
        SC_ENUM_PROCESS_INFO,
        SERVICE_WIN32,
        SERVICE_ACTIVE,
        nullptr,
        0,
        &bytesNeeded,
        &servicesReturned,
        &resumeHandle,
        nullptr
    );

    if (GetLastError() == ERROR_MORE_DATA && bytesNeeded > 0) {
        std::vector<BYTE> buffer(bytesNeeded);
        if (EnumServicesStatusExW(
            hSCM,
            SC_ENUM_PROCESS_INFO,
            SERVICE_WIN32,
            SERVICE_ACTIVE,
            buffer.data(),
            bytesNeeded,
            &bytesNeeded,
            &servicesReturned,
            &resumeHandle,
            nullptr
        )) {
            auto* pServices = reinterpret_cast<ENUM_SERVICE_STATUS_PROCESSW*>(buffer.data());
            for (DWORD i = 0; i < servicesReturned; ++i) {
                DWORD pid = pServices[i].ServiceStatusProcess.dwProcessId;
                if (pid > 0) {
                    if (pidMap.find(pid) == pidMap.end()) {
                        pidMap[pid] = { pServices[i].lpServiceName, pServices[i].lpDisplayName };
                    }
                }
            }
        }
    }

    CloseServiceHandle(hSCM);
    return pidMap;
}

bool StopWindowsService(const std::wstring& serviceName, std::wstring* outError) {
    SC_HANDLE hSCM = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!hSCM) {
        if (outError) *outError = L"Failed to connect to Service Control Manager (run as Administrator).";
        return false;
    }

    SC_HANDLE hSvc = OpenServiceW(hSCM, serviceName.c_str(), SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (!hSvc) {
        DWORD err = GetLastError();
        if (outError) *outError = L"Failed to open service '" + serviceName + L"' (Error " + std::to_wstring(err) + L").";
        CloseServiceHandle(hSCM);
        return false;
    }

    SERVICE_STATUS status = {};
    BOOL ok = ControlService(hSvc, SERVICE_CONTROL_STOP, &status);
    if (!ok && GetLastError() != ERROR_SERVICE_NOT_ACTIVE) {
        DWORD err = GetLastError();
        if (outError) *outError = L"ControlService failed with error " + std::to_wstring(err) + L".";
        CloseServiceHandle(hSvc);
        CloseServiceHandle(hSCM);
        return false;
    }

    // Wait up to 3 seconds for the service to report SERVICE_STOPPED
    for (int i = 0; i < 30; ++i) {
        if (QueryServiceStatus(hSvc, &status)) {
            if (status.dwCurrentState == SERVICE_STOPPED) {
                break;
            }
        }
        Sleep(100);
    }

    CloseServiceHandle(hSvc);
    CloseServiceHandle(hSCM);
    return true;
}

bool RestartWindowsExplorer(std::wstring* outError) {
    // 1. Terminate all running explorer.exe processes
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe = { sizeof(pe) };
        if (Process32FirstW(hSnap, &pe)) {
            do {
                if (_wcsicmp(pe.szExeFile, L"explorer.exe") == 0) {
                    HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
                    if (hProc) {
                        TerminateProcess(hProc, 0);
                        CloseHandle(hProc);
                    }
                }
            } while (Process32NextW(hSnap, &pe));
        }
        CloseHandle(hSnap);
    }

    Sleep(300);

    // 2. Relaunch explorer.exe clean
    wchar_t winDir[MAX_PATH] = {};
    GetWindowsDirectoryW(winDir, MAX_PATH);
    std::wstring expPath = std::wstring(winDir) + L"\\explorer.exe";

    HINSTANCE hInst = ShellExecuteW(nullptr, L"open", expPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(hInst) <= 32) {
        if (outError) *outError = L"Failed to restart explorer.exe.";
        return false;
    }
    return true;
}

bool IsProtectedSystemProcess(DWORD pid, const std::wstring& procName) {
    // PID 0 (Idle) and PID 4 (System / NT Kernel) are untouchable.
    if (pid <= 4) return true;

    // The "Kill any of these and enjoy your instant BSOD or dead session" list:
    static const wchar_t* kProtectedNames[] = {
        L"system",
        L"smss.exe",
        L"csrss.exe",
        L"wininit.exe",
        L"services.exe",
        L"lsass.exe",
        L"lsm.exe",
        L"winlogon.exe",
        L"svchost.exe",
        L"dwm.exe",
        L"fontdrvhost.exe",
        L"memory compression",
        L"registry"
    };

    std::wstring lower = ToLower(procName);
    for (const wchar_t* p : kProtectedNames) {
        if (lower == p) return true;
    }
    return false;
}

static bool IsExplorerOrThumbnailHost(const std::wstring& procName) {
    std::wstring lower = ToLower(procName);
    return (lower == L"explorer.exe" || lower == L"prevhost.exe" || lower == L"dllhost.exe" || lower == L"searchprotocolhost.exe");
}

static std::wstring GetExeFriendlyDescription(const std::wstring& exePath);

std::wstring GetFileDescription(const std::wstring& filePath) {
    return GetExeFriendlyDescription(filePath);
}

bool IsSystemCriticalFile(const std::wstring& filePath) {
    if (filePath.empty()) return false;

    // Check by file name
    const wchar_t* fname = PathFindFileNameW(filePath.c_str());
    if (fname && *fname) {
        std::wstring lowerName = ToLower(fname);
        static const wchar_t* kCritFiles[] = {
            L"ntoskrnl.exe", L"hal.dll", L"ci.dll", L"winload.exe", L"winload.efi",
            L"bootmgr", L"csrss.exe", L"wininit.exe", L"services.exe", L"lsass.exe",
            L"smss.exe", L"winlogon.exe", L"dwm.exe", L"user32.dll", L"kernel32.dll",
            L"ntdll.dll", L"gdi32.dll", L"pagefile.sys", L"swapfile.sys", L"hiberfil.sys"
        };
        for (const wchar_t* cf : kCritFiles) {
            if (lowerName == cf) return true;
        }
    }

    // Check by path location (C:\Windows\System32, C:\Windows\SysWOW64, C:\Windows\Boot, etc.)
    wchar_t winDir[MAX_PATH] = {};
    GetWindowsDirectoryW(winDir, MAX_PATH);
    std::wstring lowerWin = ToLower(winDir);
    std::wstring lowerPath = ToLower(filePath);

    std::wstring sys32 = lowerWin + L"\\system32";
    std::wstring syswow = lowerWin + L"\\syswow64";
    std::wstring boot = lowerWin + L"\\boot";

    if (StartsWithIgnoreCase(lowerPath, sys32) ||
        StartsWithIgnoreCase(lowerPath, syswow) ||
        StartsWithIgnoreCase(lowerPath, boot))
    {
        return true;
    }
    return false;
}

static std::wstring GetProcessFullPath(DWORD pid) {
    std::wstring result;
    // PROCESS_QUERY_LIMITED_INFORMATION (0x1000) works on Vista+ even for elevated/protected processes.
    // On XP, we fall back to PROCESS_QUERY_INFORMATION | PROCESS_VM_READ.
    HANDLE hProc = OpenProcess(0x1000, FALSE, pid);
    if (!hProc) {
        hProc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    }
    if (!hProc) return result;

    wchar_t buf[MAX_PATH * 2] = {};
    DWORD size = MAX_PATH * 2;
    if (Api().QueryFullProcessImageNameW && Api().QueryFullProcessImageNameW(hProc, 0, buf, &size)) {
        result = buf;
    } else if (GetModuleFileNameExW(hProc, nullptr, buf, MAX_PATH * 2) > 0) {
        result = buf;
    }
    CloseHandle(hProc);
    return result;
}

// Reads the "FileDescription" from a locking .exe's version resource so we can show
// "Visual Studio Code (Code.exe)" instead of just a cryptic binary name.
static std::wstring GetExeFriendlyDescription(const std::wstring& exePath) {
    if (exePath.empty()) return L"";
    DWORD dummy = 0;
    DWORD verSize = GetFileVersionInfoSizeW(exePath.c_str(), &dummy);
    if (verSize == 0) return L"";

    std::vector<BYTE> verData(verSize);
    if (!GetFileVersionInfoW(exePath.c_str(), 0, verSize, verData.data())) return L"";

    struct LANGANDCODEPAGE {
        WORD wLanguage;
        WORD wCodePage;
    } *lpTranslate = nullptr;
    UINT cbTranslate = 0;

    if (VerQueryValueW(verData.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<LPVOID*>(&lpTranslate), &cbTranslate) && cbTranslate >= sizeof(LANGANDCODEPAGE)) {
        wchar_t subBlock[128] = {};
        wsprintfW(subBlock, L"\\StringFileInfo\\%04x%04x\\FileDescription", lpTranslate[0].wLanguage, lpTranslate[0].wCodePage);
        LPWSTR desc = nullptr;
        UINT descLen = 0;
        if (VerQueryValueW(verData.data(), subBlock, reinterpret_cast<LPVOID*>(&desc), &descLen) && desc && descLen > 0) {
            return std::wstring(desc);
        }
    }
    return L"";
}

struct FindWindowData {
    DWORD targetPid = 0;
    HWND  foundHwnd = nullptr;
};

static BOOL CALLBACK EnumWindowsForPidProc(HWND hwnd, LPARAM lParam) {
    auto* data = reinterpret_cast<FindWindowData*>(lParam);
    DWORD winPid = 0;
    GetWindowThreadProcessId(hwnd, &winPid);
    if (winPid == data->targetPid && IsWindowVisible(hwnd) && GetWindow(hwnd, GW_OWNER) == nullptr) {
        int len = GetWindowTextLengthW(hwnd);
        if (len > 0) {
            data->foundHwnd = hwnd;
            return FALSE; // Found the main visible window
        }
    }
    return TRUE;
}

static HWND FindMainWindowForPid(DWORD pid) {
    FindWindowData data{ pid, nullptr };
    EnumWindows(EnumWindowsForPidProc, reinterpret_cast<LPARAM>(&data));
    return data.foundHwnd;
}

static void PopulateProcessDetails(LockingProcess& proc) {
    if (proc.fullImagePath.empty()) {
        proc.fullImagePath = GetProcessFullPath(proc.pid);
    }
    if (proc.processName.empty() && !proc.fullImagePath.empty()) {
        const wchar_t* fileName = PathFindFileNameW(proc.fullImagePath.c_str());
        if (fileName) proc.processName = fileName;
    }
    if (proc.processName.empty()) {
        if (proc.pid == 4) proc.processName = L"System";
        else proc.processName = L"Process_" + std::to_wstring(proc.pid);
    }

    // Check if this process is a registered Windows Service
    if (proc.pid > 4 && !proc.isService) {
        static std::map<DWORD, ServiceEntry> svcMap;
        static DWORD lastTick = 0;
        DWORD now = GetTickCount();
        if (svcMap.empty() || (now - lastTick) > 5000) {
            svcMap = QueryPidToServiceMap();
            lastTick = now;
        }
        auto it = svcMap.find(proc.pid);
        if (it != svcMap.end()) {
            proc.isService = true;
            proc.serviceName = it->second.serviceName;
            proc.serviceDisplayName = it->second.displayName;
        }
    }

    if (proc.friendlyName.empty()) {
        if (proc.isService && !proc.serviceDisplayName.empty()) {
            proc.friendlyName = proc.serviceDisplayName;
        } else {
            proc.friendlyName = GetExeFriendlyDescription(proc.fullImagePath);
            if (proc.friendlyName.empty()) {
                proc.friendlyName = proc.processName;
            }
        }
    }

    proc.isSystemProtected = IsProtectedSystemProcess(proc.pid, proc.processName);
    proc.isExplorer        = IsExplorerOrThumbnailHost(proc.processName);
    proc.mainWindow        = FindMainWindowForPid(proc.pid);
}

// ----------------------------------------------------------------------------
// Anti-Deadlock Pipe-Safe Handle Name Query
// ----------------------------------------------------------------------------
// Calling NtQueryInformationFile or NtQueryObject on a synchronous named pipe
// held by another process can block the calling thread forever.
// Instead of spawning 500 threads (slow!), we use ONE persistent background
// worker thread with two auto-reset events. If the worker blocks on a cursed pipe,
// we abandon it after 15ms and spin up a fresh worker. Fast AND un-hangable.

struct SafeQueryRequest {
    HANDLE       hDupHandle = nullptr;
    std::wstring outFileName; // Relative path from FileNameInformation (e.g. "\Users\...\file.txt")
    std::wstring outNtObjectName; // Full NT path from NtQueryObject (e.g. "\Device\HarddiskVolume3\...")
    bool         queryFullNtName = false;
    bool         success = false;
};

class SafeHandleInspector {
public:
    static SafeHandleInspector& Instance() {
        static SafeHandleInspector inst;
        return inst;
    }

    // Queries the duplicated handle's filename with a hard timeout (default 15ms).
    bool QueryHandlePath(HANDLE hDup, std::wstring& outRelPath, std::wstring& outNtPath, bool needFullNtName, DWORD timeoutMs = 15) {
        EnsureWorkerRunning();

        m_req.hDupHandle      = hDup;
        m_req.outFileName.clear();
        m_req.outNtObjectName.clear();
        m_req.queryFullNtName = needFullNtName;
        m_req.success         = false;

        SetEvent(m_hStartEvent);
        DWORD wait = WaitForSingleObject(m_hDoneEvent, timeoutMs);
        if (wait == WAIT_OBJECT_0) {
            if (m_req.success) {
                outRelPath = m_req.outFileName;
                outNtPath  = m_req.outNtObjectName;
                return true;
            }
            return false;
        }

        // Whoops! The worker thread stepped on a synchronous named pipe landmine.
        // Cut it loose so our UI never freezes, and spawn a fresh worker for the next handle.
        TerminateThread(m_hThread, 0);
        CloseHandle(m_hThread);
        m_hThread = nullptr;
        return false;
    }

private:
    HANDLE           m_hThread     = nullptr;
    HANDLE           m_hStartEvent = nullptr;
    HANDLE           m_hDoneEvent  = nullptr;
    SafeQueryRequest m_req;

    SafeHandleInspector() {
        m_hStartEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        m_hDoneEvent  = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }

    void EnsureWorkerRunning() {
        if (!m_hThread) {
            m_hThread = CreateThread(nullptr, 0, WorkerThreadProc, this, 0, nullptr);
        }
    }

    static DWORD WINAPI WorkerThreadProc(LPVOID param) {
        auto* self = reinterpret_cast<SafeHandleInspector*>(param);
        BYTE buffer[4096];

        while (WaitForSingleObject(self->m_hStartEvent, INFINITE) == WAIT_OBJECT_0) {
            HANDLE h = self->m_req.hDupHandle;
            self->m_req.success = false;

            // Fast check #1: Skip character/pipe handles if GetFileType says it's not a disk file.
            // (Note: GetFileType itself can occasionally block on exotic pipes, which is why it's inside this worker!)
            if (GetFileType(h) == FILE_TYPE_DISK) {
                if (Api().NtQueryInformationFile) {
                    IO_STATUS_BLOCK_NT iosb = {};
                    NTSTATUS st = Api().NtQueryInformationFile(
                        h, &iosb, buffer, sizeof(buffer) - sizeof(WCHAR), FileNameInformationClass);
                    if (st == STATUS_SUCCESS) {
                        auto* fni = reinterpret_cast<FILE_NAME_INFORMATION_NT*>(buffer);
                        size_t charCount = fni->FileNameLength / sizeof(WCHAR);
                        self->m_req.outFileName.assign(fni->FileName, charCount);
                        self->m_req.success = true;
                    }
                }

                if (self->m_req.queryFullNtName && Api().NtQueryObject) {
                    ULONG retLen = 0;
                    if (Api().NtQueryObject(h, 1 /*ObjectNameInformation*/, buffer, sizeof(buffer), &retLen) == STATUS_SUCCESS) {
                        auto* oni = reinterpret_cast<OBJECT_NAME_INFORMATION_NT*>(buffer);
                        if (oni->Name.Buffer && oni->Name.Length > 0) {
                            self->m_req.outNtObjectName.assign(oni->Name.Buffer, oni->Name.Length / sizeof(WCHAR));
                            self->m_req.success = true;
                        }
                    }
                }
            }
            SetEvent(self->m_hDoneEvent);
        }
        return 0;
    }
};

// ----------------------------------------------------------------------------
// Determining the Kernel's "File" ObjectTypeIndex Dynamically
// ----------------------------------------------------------------------------
// Because Microsoft changes the ObjectTypeIndex for "File" across Windows versions
// (28 on XP, 25 on Vista, 28 on Win7, 30+ on Win10/11), we open "NUL" once in our own
// process and look up its ObjectTypeIndex in the handle table. Instant and 100% exact!

static USHORT GetFileObjectTypeIndex() {
    static USHORT cachedIndex = 0;
    if (cachedIndex != 0) return cachedIndex;

    HANDLE hDummyFile = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hDummyFile == INVALID_HANDLE_VALUE) return 0;

    DWORD myPid = GetCurrentProcessId();
    ULONG_PTR myHandleVal = reinterpret_cast<ULONG_PTR>(hDummyFile);

    // Try Vista+ SystemExtendedHandleInformation first
    ULONG bufSize = 1024 * 1024;
    std::vector<BYTE> buffer(bufSize);
    ULONG retLen = 0;

    if (Api().NtQuerySystemInformation) {
        NTSTATUS status = Api().NtQuerySystemInformation(SystemExtendedHandleInformation, buffer.data(), bufSize, &retLen);
        while (status == STATUS_INFO_LENGTH_MISMATCH) {
            bufSize = (retLen > bufSize) ? (retLen + 65536) : (bufSize * 2);
            buffer.resize(bufSize);
            status = Api().NtQuerySystemInformation(SystemExtendedHandleInformation, buffer.data(), bufSize, &retLen);
        }
        if (status == STATUS_SUCCESS) {
            auto* info = reinterpret_cast<SYSTEM_HANDLE_INFORMATION_EX*>(buffer.data());
            for (ULONG_PTR i = 0; i < info->NumberOfHandles; ++i) {
                if (info->Handles[i].UniqueProcessId == myPid && info->Handles[i].HandleValue == myHandleVal) {
                    cachedIndex = info->Handles[i].ObjectTypeIndex;
                    break;
                }
            }
        } else {
            // XP fallback: SystemHandleInformation (16)
            bufSize = 1024 * 1024;
            buffer.resize(bufSize);
            status = Api().NtQuerySystemInformation(SystemHandleInformation, buffer.data(), bufSize, &retLen);
            while (status == STATUS_INFO_LENGTH_MISMATCH) {
                bufSize *= 2;
                buffer.resize(bufSize);
                status = Api().NtQuerySystemInformation(SystemHandleInformation, buffer.data(), bufSize, &retLen);
            }
            if (status == STATUS_SUCCESS) {
                auto* info = reinterpret_cast<SYSTEM_HANDLE_INFORMATION*>(buffer.data());
                for (ULONG i = 0; i < info->NumberOfHandles; ++i) {
                    if (info->Handles[i].UniqueProcessId == static_cast<USHORT>(myPid) &&
                        info->Handles[i].HandleValue == static_cast<USHORT>(myHandleVal)) {
                        cachedIndex = info->Handles[i].ObjectTypeIndex;
                        break;
                    }
                }
            }
        }
    }

    CloseHandle(hDummyFile);
    return cachedIndex;
}

// ----------------------------------------------------------------------------
// Stage 1: Fast Restart Manager Discovery (Vista / 7 / 8 / 10 / 11)
// ----------------------------------------------------------------------------
static bool QueryRestartManagerLocks(const std::wstring& filePath, std::map<DWORD, LockingProcess>& outProcs) {
    const auto& api = Api();
    if (!api.RmStartSession || !api.RmRegisterResources || !api.RmGetList || !api.RmEndSession) {
        return false; // Running on Windows XP
    }

    DWORD sessionHandle = 0;
    WCHAR sessionKey[CCH_RM_SESSION_KEY + 1] = {};
    if (api.RmStartSession(&sessionHandle, 0, sessionKey) != ERROR_SUCCESS) {
        return false;
    }

    LPCWSTR filesToCheck[1] = { filePath.c_str() };
    DWORD err = api.RmRegisterResources(sessionHandle, 1, filesToCheck, 0, nullptr, 0, nullptr);
    if (err == ERROR_SUCCESS) {
        UINT needed = 0;
        UINT count  = 0;
        DWORD rebootReasons = 0;
        err = api.RmGetList(sessionHandle, &needed, &count, nullptr, &rebootReasons);
        if (err == ERROR_MORE_DATA && needed > 0) {
            count = needed + 4;
            std::vector<RM_PROCESS_INFO_DYN> procInfos(count);
            err = api.RmGetList(sessionHandle, &needed, &count, procInfos.data(), &rebootReasons);
            if (err == ERROR_SUCCESS) {
                DWORD selfPid = GetCurrentProcessId();
                for (UINT i = 0; i < count; ++i) {
                    DWORD pid = procInfos[i].Process.dwProcessId;
                    if (pid == 0 || pid == selfPid) continue;

                    LockingProcess& lp = outProcs[pid];
                    lp.pid = pid;
                    if (procInfos[i].strAppName[0] != L'\0') {
                        lp.friendlyName = procInfos[i].strAppName;
                    }
                    // ApplicationType 4 == RmExplorer, 3 == RmService, etc.
                    lp.kind = LockKind::LoadedModule; // Will be upgraded to FileHandle if handle scanner finds open handles
                    PopulateProcessDetails(lp);
                }
            }
        }
    }

    api.RmEndSession(sessionHandle);
    return true;
}

// ----------------------------------------------------------------------------
// Stage 2: NT System Handle Scanner (Filtered & Deadlock-Proof)
// ----------------------------------------------------------------------------
// If `filterPids` is non-empty, we ONLY inspect handles belonging to those PIDs!
// That turns a 200-process scan into a 1-process laser scan.

static void ScanKernelFileHandles(
    const std::wstring& targetPath,
    bool isDirectory,
    const std::set<DWORD>* filterPids,
    std::map<DWORD, LockingProcess>& ioProcs)
{
    if (!Api().NtQuerySystemInformation) return;

    std::wstring targetDevicePath;
    std::wstring targetRelPath;
    BuildNtDevicePaths(targetPath, targetDevicePath, targetRelPath);

    USHORT fileTypeIndex = GetFileObjectTypeIndex();
    DWORD selfPid = GetCurrentProcessId();

    // Normalized directory prefix for folder locks (e.g. a process holding C:\Folder\sub\file.txt locks C:\Folder)
    std::wstring dirDevicePrefix = targetDevicePath + L"\\";
    std::wstring dirRelPrefix    = targetRelPath + L"\\";

    struct RawHandleEntry {
        DWORD     pid;
        ULONG_PTR handleVal;
        ULONG     grantedAccess;
        USHORT    typeIndex;
    };
    std::vector<RawHandleEntry> candidates;
    candidates.reserve(1024);

    ULONG bufSize = 2 * 1024 * 1024;
    std::vector<BYTE> buffer(bufSize);
    ULONG retLen = 0;

    // 1. Try Vista+ SystemExtendedHandleInformation (64)
    NTSTATUS status = Api().NtQuerySystemInformation(SystemExtendedHandleInformation, buffer.data(), bufSize, &retLen);
    while (status == STATUS_INFO_LENGTH_MISMATCH) {
        bufSize = (retLen > bufSize) ? (retLen + 131072) : (bufSize * 2);
        buffer.resize(bufSize);
        status = Api().NtQuerySystemInformation(SystemExtendedHandleInformation, buffer.data(), bufSize, &retLen);
    }

    if (status == STATUS_SUCCESS) {
        auto* info = reinterpret_cast<SYSTEM_HANDLE_INFORMATION_EX*>(buffer.data());
        for (ULONG_PTR i = 0; i < info->NumberOfHandles; ++i) {
            const auto& h = info->Handles[i];
            DWORD pid = static_cast<DWORD>(h.UniqueProcessId);
            if (pid == 0 || pid == selfPid) continue;
            if (fileTypeIndex != 0 && h.ObjectTypeIndex != fileTypeIndex) continue;
            if (filterPids && !filterPids->empty() && filterPids->find(pid) == filterPids->end()) continue;

            // Fast skip for known synchronous named pipe access masks when doing a full scan
            if ((!filterPids || filterPids->empty()) &&
                (h.GrantedAccess == 0x0012019f || h.GrantedAccess == 0x00120189 || h.GrantedAccess == 0x001a019f)) {
                continue;
            }

            candidates.push_back({ pid, h.HandleValue, h.GrantedAccess, h.ObjectTypeIndex });
        }
    } else {
        // 2. Fallback to XP SystemHandleInformation (16)
        bufSize = 2 * 1024 * 1024;
        buffer.resize(bufSize);
        status = Api().NtQuerySystemInformation(SystemHandleInformation, buffer.data(), bufSize, &retLen);
        while (status == STATUS_INFO_LENGTH_MISMATCH) {
            bufSize *= 2;
            buffer.resize(bufSize);
            status = Api().NtQuerySystemInformation(SystemHandleInformation, buffer.data(), bufSize, &retLen);
        }
        if (status == STATUS_SUCCESS) {
            auto* info = reinterpret_cast<SYSTEM_HANDLE_INFORMATION*>(buffer.data());
            for (ULONG i = 0; i < info->NumberOfHandles; ++i) {
                const auto& h = info->Handles[i];
                DWORD pid = h.UniqueProcessId;
                if (pid == 0 || pid == selfPid) continue;
                if (fileTypeIndex != 0 && h.ObjectTypeIndex != fileTypeIndex) continue;
                if (filterPids && !filterPids->empty() && filterPids->find(pid) == filterPids->end()) continue;

                candidates.push_back({ pid, h.HandleValue, h.GrantedAccess, h.ObjectTypeIndex });
            }
        }
    }

    // Group by PID so we only call OpenProcess ONCE per process!
    std::map<DWORD, std::vector<ULONG_PTR>> handlesByPid;
    for (const auto& c : candidates) {
        handlesByPid[c.pid].push_back(c.handleVal);
    }

    HANDLE hSelf = GetCurrentProcess();
    for (auto& kv : handlesByPid) {
        DWORD pid = kv.first;
        HANDLE hProc = OpenProcess(PROCESS_DUP_HANDLE, FALSE, pid);
        if (!hProc) continue;

        for (ULONG_PTR hVal : kv.second) {
            HANDLE hDup = nullptr;
            if (!DuplicateHandle(hProc, reinterpret_cast<HANDLE>(hVal), hSelf, &hDup, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
                continue;
            }

            std::wstring relPath;
            std::wstring ntPath;
            // First do a fast relative path check; if it matches, verify the full volume device path!
            if (SafeHandleInspector::Instance().QueryHandlePath(hDup, relPath, ntPath, false, 12)) {
                bool relMatch = EqualsIgnoreCase(relPath, targetRelPath) ||
                                (isDirectory && StartsWithIgnoreCase(relPath, dirRelPrefix));

                if (relMatch) {
                    // Verify volume match using NtQueryObject so D:\foo.txt doesn't falsely match C:\foo.txt
                    std::wstring dummyRel;
                    SafeHandleInspector::Instance().QueryHandlePath(hDup, dummyRel, ntPath, true, 15);
                    bool fullMatch = ntPath.empty() ? relMatch :
                        (EqualsIgnoreCase(ntPath, targetDevicePath) ||
                         (isDirectory && StartsWithIgnoreCase(ntPath, dirDevicePrefix)));

                    if (fullMatch) {
                        LockingProcess& lp = ioProcs[pid];
                        lp.pid  = pid;
                        lp.kind = LockKind::FileHandle;
                        lp.handles.push_back(hVal);
                    }
                }
            }

            CloseHandle(hDup);
        }

        CloseHandle(hProc);
    }

    for (auto& kv : ioProcs) {
        PopulateProcessDetails(kv.second);
    }
}

// Also check if the target file is itself a running .exe or loaded .dll on XP (where RstrtMgr isn't present)
static void ScanLoadedModulesForFile(const std::wstring& targetPath, std::map<DWORD, LockingProcess>& ioProcs) {
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return;

    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);
    DWORD selfPid = GetCurrentProcessId();

    if (Process32FirstW(hSnap, &pe)) {
        do {
            if (pe.th32ProcessID <= 4 || pe.th32ProcessID == selfPid) continue;
            std::wstring exePath = GetProcessFullPath(pe.th32ProcessID);
            if (!exePath.empty() && EqualsIgnoreCase(exePath, targetPath)) {
                LockingProcess& lp = ioProcs[pe.th32ProcessID];
                lp.pid           = pe.th32ProcessID;
                lp.processName   = pe.szExeFile;
                lp.fullImagePath = exePath;
                if (lp.kind == LockKind::Unknown) lp.kind = LockKind::LoadedModule;
                PopulateProcessDetails(lp);
            }
        } while (Process32NextW(hSnap, &pe));
    }
    CloseHandle(hSnap);
}

// ----------------------------------------------------------------------------
// Public Lock Detection API
// ----------------------------------------------------------------------------

bool IsFileActivelyLocked(const std::wstring& filePath) {
    DWORD attrs = GetFileAttributesW(filePath.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return false;
    if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
        // For directories, try opening with DELETE access and no sharing
        HANDLE hDir = CreateFileW(
            filePath.c_str(),
            DELETE,
            0, // Exclusive
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS,
            nullptr);
        if (hDir == INVALID_HANDLE_VALUE) {
            DWORD err = GetLastError();
            return (err == ERROR_SHARING_VIOLATION || err == ERROR_LOCK_VIOLATION || err == ERROR_ACCESS_DENIED);
        }
        CloseHandle(hDir);
        return false;
    }

    HANDLE hFile = CreateFileW(
        filePath.c_str(),
        GENERIC_READ | GENERIC_WRITE | DELETE,
        0, // Exclusive: fail immediately if ANY process has an open handle!
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (hFile == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        return (err == ERROR_SHARING_VIOLATION || err == ERROR_LOCK_VIOLATION);
    }
    CloseHandle(hFile);
    return false;
}

ScanResult ScanFileLocks(const std::wstring& filePath, bool resolveExactHandles) {
    LARGE_INTEGER freq = {}, t0 = {}, t1 = {};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);

    EnableDebugPrivilege();

    ScanResult res;
    wchar_t fullBuf[MAX_PATH * 2] = {};
    if (GetFullPathNameW(filePath.c_str(), MAX_PATH * 2, fullBuf, nullptr)) {
        res.targetPath = fullBuf;
    } else {
        res.targetPath = filePath;
    }

    DWORD attrs = GetFileAttributesW(res.targetPath.c_str());
    res.exists      = (attrs != INVALID_FILE_ATTRIBUTES);
    res.isDirectory = res.exists && ((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0);
    res.isCurrentlyLocked = res.exists && IsFileActivelyLocked(res.targetPath);

    std::map<DWORD, LockingProcess> foundProcs;

    // Step 1: Fast Restart Manager pass (on Vista..11, only for files since RmRegisterResources doesn't expand directories)
    bool rmRan = false;
    if (res.exists && !res.isDirectory) {
        rmRan = QueryRestartManagerLocks(res.targetPath, foundProcs);
    }

    // Step 2: Kernel Handle Scan
    // When Restart Manager already gave us the culprit PIDs, we filter the handle table strictly
    // by those PIDs so we only inspect the 1 or 2 locking processes (takes < 2 milliseconds!).
    if (res.exists && (resolveExactHandles || !rmRan || foundProcs.empty() || res.isDirectory)) {
        if (!foundProcs.empty()) {
            std::set<DWORD> targetPids;
            for (const auto& kv : foundProcs) targetPids.insert(kv.first);
            ScanKernelFileHandles(res.targetPath, res.isDirectory, &targetPids, foundProcs);
        }

        // Only fall back to a full-system handle scan if Restart Manager found NO locking processes at all
        // (e.g., on Windows XP, or for a folder lock).
        if (foundProcs.empty()) {
            ScanKernelFileHandles(res.targetPath, res.isDirectory, nullptr, foundProcs);
        }
    }

    // Step 3: Check if the file itself is a running .exe (for XP or if missed)
    if (res.exists && !res.isDirectory && foundProcs.empty()) {
        ScanLoadedModulesForFile(res.targetPath, foundProcs);
    }

    for (auto& kv : foundProcs) {
        res.processes.push_back(std::move(kv.second));
    }

    if (!res.processes.empty()) {
        res.isCurrentlyLocked = true;
    }

    QueryPerformanceCounter(&t1);
    res.scanTimeMs = static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
    return res;
}

// ----------------------------------------------------------------------------
// Surgical Handle Closing, Process Termination & File Deletion
// ----------------------------------------------------------------------------

int ForceCloseHandlesForFile(const std::wstring& filePath, std::vector<LockingProcess>& processes, std::wstring* outError) {
    EnableDebugPrivilege();

    // Make sure we have the exact handle values populated
    bool anyHandlesKnown = false;
    for (const auto& p : processes) {
        if (!p.handles.empty()) { anyHandlesKnown = true; break; }
    }
    if (!anyHandlesKnown) {
        ScanResult fresh = ScanFileLocks(filePath, true);
        processes = fresh.processes;
    }

    int closedCount = 0;
    bool hadLoadedModuleOnly = false;

    for (auto& proc : processes) {
        if (proc.pid <= 4) continue; // Cannot duplicate-close kernel System handles from user mode

        if (proc.handles.empty() && proc.kind == LockKind::LoadedModule) {
            hadLoadedModuleOnly = true;
            continue;
        }

        HANDLE hProc = OpenProcess(PROCESS_DUP_HANDLE, FALSE, proc.pid);
        if (!hProc) continue;

        for (ULONG_PTR hVal : proc.handles) {
            HANDLE hDummy = nullptr;
            // The magic trick: DUPLICATE_CLOSE_SOURCE tells the Windows kernel to reach into
            // the target process's handle table and close its handle before duplicating!
            if (DuplicateHandle(
                    hProc,
                    reinterpret_cast<HANDLE>(hVal),
                    GetCurrentProcess(),
                    &hDummy,
                    0,
                    FALSE,
                    DUPLICATE_CLOSE_SOURCE))
            {
                if (hDummy) CloseHandle(hDummy);
                closedCount++;
            }
        }
        CloseHandle(hProc);
    }

    if (closedCount == 0 && hadLoadedModuleOnly && outError) {
        *outError = L"This file is loaded as an active EXE/DLL module in memory (no standard file handle to close). Use 'Kill Process' or 'Delete on Reboot' instead.";
    }
    return closedCount;
}

int KillLockingProcesses(const std::vector<LockingProcess>& processes, bool allowExplorerRestart, std::wstring* outError) {
    EnableDebugPrivilege();
    int killedCount = 0;
    bool skippedProtected = false;
    bool killedExplorer = false;

    for (const auto& proc : processes) {
        if (proc.isSystemProtected) {
            skippedProtected = true;
            continue;
        }
        if (EqualsIgnoreCase(proc.processName, L"explorer.exe") && !allowExplorerRestart) {
            continue;
        }

        HANDLE hProc = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, proc.pid);
        if (hProc) {
            if (TerminateProcess(hProc, 1)) {
                // Wait up to 1.5 seconds for the kernel to clean up all handles owned by the dying process
                WaitForSingleObject(hProc, 1500);
                killedCount++;
                if (EqualsIgnoreCase(proc.processName, L"explorer.exe")) {
                    killedExplorer = true;
                }
            }
            CloseHandle(hProc);
        }
    }

    // If we had to terminate explorer.exe, relaunch it immediately so the user doesn't stare at an empty wallpaper!
    if (killedExplorer) {
        ShellExecuteW(nullptr, L"open", L"explorer.exe", nullptr, nullptr, SW_SHOWNORMAL);
    }

    if (killedCount == 0 && skippedProtected && outError) {
        *outError = L"Blocked termination of a critical Windows system process to prevent a Blue Screen.";
    }
    return killedCount;
}

bool DeleteTargetFile(const std::wstring& filePath, bool sendToRecycleBin, std::wstring* outError) {
    DWORD attrs = GetFileAttributesW(filePath.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        return true; // Already gone!
    }

    // Strip Read-Only attribute if present so deletion doesn't fail with ERROR_ACCESS_DENIED
    if (attrs & FILE_ATTRIBUTE_READONLY) {
        SetFileAttributesW(filePath.c_str(), attrs & ~FILE_ATTRIBUTE_READONLY);
    }

    if (sendToRecycleBin) {
        // SHFileOperationW requires a double-null-terminated string!
        std::vector< wchar_t > doubleNullPath(filePath.begin(), filePath.end());
        doubleNullPath.push_back(L'\0');
        doubleNullPath.push_back(L'\0');

        SHFILEOPSTRUCTW fileOp = {};
        fileOp.wFunc  = FO_DELETE;
        fileOp.pFrom  = doubleNullPath.data();
        fileOp.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;

        int res = SHFileOperationW(&fileOp);
        if (res == 0 && !fileOp.fAnyOperationsAborted) {
            return true;
        }
    }

    // Direct deletion (or fallback if Recycle Bin failed)
    if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
        std::vector<wchar_t> doubleNullPath(filePath.begin(), filePath.end());
        doubleNullPath.push_back(L'\0');
        doubleNullPath.push_back(L'\0');

        SHFILEOPSTRUCTW fileOp = {};
        fileOp.wFunc  = FO_DELETE;
        fileOp.pFrom  = doubleNullPath.data();
        fileOp.fFlags = FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
        if (SHFileOperationW(&fileOp) == 0 && !fileOp.fAnyOperationsAborted) {
            return true;
        }
        if (RemoveDirectoryW(filePath.c_str())) return true;

        // If access denied, take ownership, reset DACL, and retry
        if (GetLastError() == ERROR_ACCESS_DENIED) {
            TakeOwnershipAndGrantAccess(filePath);
            if (RemoveDirectoryW(filePath.c_str())) return true;
        }
    } else {
        if (DeleteFileW(filePath.c_str())) return true;

        // If access denied, take ownership, reset DACL, and retry
        if (GetLastError() == ERROR_ACCESS_DENIED) {
            TakeOwnershipAndGrantAccess(filePath);
            if (DeleteFileW(filePath.c_str())) return true;
        }
    }

    if (outError) {
        DWORD err = GetLastError();
        wchar_t msgBuf[256] = {};
        FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       nullptr, err, 0, msgBuf, 256, nullptr);
        *outError = L"Delete failed (Error " + std::to_wstring(err) + L"): " + msgBuf;
    }
    return false;
}

bool ScheduleDeleteOnReboot(const std::wstring& filePath, std::wstring* outError) {
    if (MoveFileExW(filePath.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT)) {
        return true;
    }
    if (outError) {
        DWORD err = GetLastError();
        if (err == ERROR_ACCESS_DENIED) {
            *outError = L"Scheduling delete on reboot requires Administrator privileges (Run as Administrator).";
        } else {
            *outError = L"MoveFileExW failed with error code " + std::to_wstring(err) + L".";
        }
    }
    return false;
}

// Helper to click "Try Again" on Explorer's hidden "File In Use" conflict dialog
// Works on both classic #32770 (IDRETRY) AND Windows 10/11 DirectUIHWND (via IAccessible::accDoDefaultAction)!
#include <oleacc.h>
#pragma comment(lib, "oleacc.lib")

static bool ClickAccessibleButtonByName(IAccessible* pAcc, const wchar_t* targetName, int depth = 0) {
    if (!pAcc || depth > 8) return false;

    VARIANT vSelf = {};
    vSelf.vt   = VT_I4;
    vSelf.lVal = CHILDID_SELF;

    BSTR bName = nullptr;
    if (SUCCEEDED(pAcc->get_accName(vSelf, &bName)) && bName) {
        bool match = (_wcsicmp(bName, targetName) == 0 || StrStrIW(bName, targetName) != nullptr);
        SysFreeString(bName);
        if (match) {
            // Verify it's a push button (ROLE_SYSTEM_PUSHBUTTON = 0x2B) or has a default action
            if (SUCCEEDED(pAcc->accDoDefaultAction(vSelf))) {
                return true;
            }
        }
    }

    long childCount = 0;
    if (FAILED(pAcc->get_accChildCount(&childCount)) || childCount <= 0 || childCount > 64) {
        return false;
    }

    std::vector<VARIANT> children(childCount);
    for (auto& v : children) VariantInit(&v);

    bool clicked = false;
    long obtained = 0;
    if (SUCCEEDED(AccessibleChildren(pAcc, 0, childCount, children.data(), &obtained))) {
        for (long i = 0; i < obtained && !clicked; ++i) {
            if (children[i].vt == VT_DISPATCH && children[i].pdispVal) {
                IAccessible* pChild = nullptr;
                if (SUCCEEDED(children[i].pdispVal->QueryInterface(IID_IAccessible, reinterpret_cast<void**>(&pChild))) && pChild) {
                    clicked = ClickAccessibleButtonByName(pChild, targetName, depth + 1);
                    pChild->Release();
                }
            }
        }
    }
    for (auto& v : children) VariantClear(&v);
    return clicked;
}

static BOOL CALLBACK ClickChildButtonProc(HWND hwndChild, LPARAM lParam) {
    const wchar_t* targetName = reinterpret_cast<const wchar_t*>(lParam);
    IAccessible* pAcc = nullptr;
    if (SUCCEEDED(AccessibleObjectFromWindow(hwndChild, OBJID_CLIENT, IID_IAccessible, reinterpret_cast<void**>(&pAcc))) && pAcc) {
        bool ok = ClickAccessibleButtonByName(pAcc, targetName, 0);
        pAcc->Release();
        if (ok) return FALSE; // Stop enumerating, we clicked it!
    }
    return TRUE;
}

static bool ClickTryAgainOnExplorerDialog(HWND hwndConflict) {
    if (!hwndConflict || !IsWindow(hwndConflict)) return false;

    // 1. Try clicking "Try Again" via MSAA IAccessible (for Windows 8/10/11 DirectUIHWND OperationStatusWindow)
    EnumChildWindows(hwndConflict, ClickChildButtonProc, reinterpret_cast<LPARAM>(L"Try Again"));

    // 2. Also send standard Win32 IDRETRY (4) for classic #32770 dialogs
    PostMessageW(hwndConflict, WM_COMMAND, MAKEWPARAM(IDRETRY, BN_CLICKED), 0);
    return true;
}

ActionResult ExecuteUnlockerAction(
    const std::wstring& filePath,
    ActionType action,
    std::vector<LockingProcess>& processes,
    bool useRecycleBin,
    HWND explorerConflictDialog,
    const std::wstring& extraParam)
{
    ActionResult result;
    std::wstring errText;

    switch (action) {
        case ActionType::UnlockOnly: {
            result.handlesClosed = ForceCloseHandlesForFile(filePath, processes, &errText);
            if (result.handlesClosed > 0 || !IsFileActivelyLocked(filePath)) {
                result.succeeded = true;
                result.statusMessage = L"Unlocked! Closed " + std::to_wstring(result.handlesClosed) + L" handle(s).";
                if (explorerConflictDialog && IsWindow(explorerConflictDialog)) {
                    ClickTryAgainOnExplorerDialog(explorerConflictDialog);
                }
            } else {
                result.succeeded = false;
                result.statusMessage = errText.empty() ? L"Could not close handles (try running as Administrator or use Kill Process)." : errText;
            }
            break;
        }

        case ActionType::UnlockAndDelete: {
            result.handlesClosed = ForceCloseHandlesForFile(filePath, processes, &errText);
            // Give the kernel a tiny breather to release the FILE_OBJECT section
            Sleep(25);

            // If we intercepted Explorer's "File In Use" dialog, let Explorer retry first!
            if (explorerConflictDialog && IsWindow(explorerConflictDialog)) {
                ClickTryAgainOnExplorerDialog(explorerConflictDialog);
                Sleep(120);
                if (GetFileAttributesW(filePath.c_str()) == INVALID_FILE_ATTRIBUTES) {
                    result.succeeded   = true;
                    result.fileDeleted = true;
                    result.statusMessage = L"Unlocked (" + std::to_wstring(result.handlesClosed) + L" handle(s)) and Explorer completed the operation!";
                    break;
                }
            }

            result.fileDeleted = DeleteTargetFile(filePath, useRecycleBin, &errText);
            result.succeeded   = result.fileDeleted;
            if (result.succeeded) {
                result.statusMessage = L"Unlocked (" + std::to_wstring(result.handlesClosed) + L" handle(s)) and deleted file.";
            } else {
                result.statusMessage = errText.empty() ? L"Handles closed, but file is still locked (possibly a loaded DLL/EXE—try 'Kill Process & Delete')." : errText;
            }
            break;
        }

        case ActionType::KillOnly: {
            result.procsKilled = KillLockingProcesses(processes, true, &errText);
            if (result.procsKilled > 0 || !IsFileActivelyLocked(filePath)) {
                result.succeeded = true;
                result.statusMessage = L"Terminated " + std::to_wstring(result.procsKilled) + L" locking process(es).";
                if (explorerConflictDialog && IsWindow(explorerConflictDialog)) {
                    ClickTryAgainOnExplorerDialog(explorerConflictDialog);
                }
            } else {
                result.succeeded = false;
                result.statusMessage = errText.empty() ? L"Failed to terminate locking process (try running as Administrator)." : errText;
            }
            break;
        }

        case ActionType::KillAndDelete: {
            // Also close any Explorer handles first if Explorer is holding a preview lock alongside another app
            ForceCloseHandlesForFile(filePath, processes, nullptr);
            result.procsKilled = KillLockingProcesses(processes, true, &errText);
            Sleep(40);

            if (explorerConflictDialog && IsWindow(explorerConflictDialog)) {
                ClickTryAgainOnExplorerDialog(explorerConflictDialog);
                Sleep(120);
                if (GetFileAttributesW(filePath.c_str()) == INVALID_FILE_ATTRIBUTES) {
                    result.succeeded   = true;
                    result.fileDeleted = true;
                    result.statusMessage = L"Terminated " + std::to_wstring(result.procsKilled) + L" process(es) and Explorer deleted the file!";
                    break;
                }
            }

            result.fileDeleted = DeleteTargetFile(filePath, useRecycleBin, &errText);
            result.succeeded   = result.fileDeleted;
            if (result.succeeded) {
                result.statusMessage = L"Terminated " + std::to_wstring(result.procsKilled) + L" process(es) and deleted file.";
            } else {
                result.statusMessage = errText;
            }
            break;
        }

        case ActionType::DeleteOnReboot: {
            result.rebootScheduled = ScheduleDeleteOnReboot(filePath, &errText);
            result.succeeded       = result.rebootScheduled;
            if (result.succeeded) {
                result.statusMessage = L"Scheduled for deletion on next Windows reboot.";
            } else {
                result.statusMessage = errText;
            }
            break;
        }

        case ActionType::UnlockAndRename: {
            if (extraParam.empty()) {
                result.succeeded = false;
                result.statusMessage = L"Rename canceled: new filename cannot be empty.";
                break;
            }
            result.handlesClosed = ForceCloseHandlesForFile(filePath, processes, &errText);
            Sleep(25);
            bool ok = RenameTargetFile(filePath, extraParam, &errText);
            result.succeeded = ok;
            if (ok) {
                result.statusMessage = L"Unlocked (" + std::to_wstring(result.handlesClosed) + L" handle(s)) and renamed successfully!";
            } else {
                result.statusMessage = errText.empty() ? L"Failed to rename file." : errText;
            }
            break;
        }

        case ActionType::UnlockAndMove: {
            if (extraParam.empty()) {
                result.succeeded = false;
                result.statusMessage = L"Move canceled: destination folder cannot be empty.";
                break;
            }
            result.handlesClosed = ForceCloseHandlesForFile(filePath, processes, &errText);
            Sleep(25);
            bool ok = MoveTargetFile(filePath, extraParam, &errText);
            result.succeeded = ok;
            if (ok) {
                result.statusMessage = L"Unlocked (" + std::to_wstring(result.handlesClosed) + L" handle(s)) and moved successfully!";
            } else {
                result.statusMessage = errText.empty() ? L"Failed to move file." : errText;
            }
            break;
        }
    }

    return result;
}

// ----------------------------------------------------------------------------
// QoL Actions
// ----------------------------------------------------------------------------

bool BringProcessWindowToFront(DWORD pid, HWND knownHwnd) {
    HWND hwnd = (knownHwnd && IsWindow(knownHwnd)) ? knownHwnd : FindMainWindowForPid(pid);
    if (!hwnd) return false;

    if (IsIconic(hwnd)) {
        ShowWindow(hwnd, SW_RESTORE);
    }
    SetForegroundWindow(hwnd);
    return true;
}

void OpenProcessFileLocation(const std::wstring& exePath) {
    if (exePath.empty()) return;
    std::wstring param = L"/select,\"" + exePath + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", param.c_str(), nullptr, SW_SHOWNORMAL);
}

} // namespace unlocker
