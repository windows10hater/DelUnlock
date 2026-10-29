#include <windows.h>

// Forward-declare XP-safe replacements
static BOOL WINAPI Fallback_InitializeCriticalSectionEx(LPCRITICAL_SECTION lpCS, DWORD dwSpinCount, DWORD Flags) {
    return InitializeCriticalSectionAndSpinCount(lpCS, dwSpinCount);
}

static VOID WINAPI Fallback_AcquireSRWLockExclusive(PSRWLOCK Lock) {
    // SRWLock on XP fallback: we can use a basic spin/interlocked or critical section
    // but CRT winapi_thunks only calls this if dynamically resolved or present.
    // For safety, spin on volatile pointer:
    volatile LONG_PTR* p = (volatile LONG_PTR*)Lock;
    while (InterlockedCompareExchangePointer((PVOID volatile*)p, (PVOID)1, (PVOID)0) != 0) {
        Sleep(0);
    }
}

static VOID WINAPI Fallback_ReleaseSRWLockExclusive(PSRWLOCK Lock) {
    volatile LONG_PTR* p = (volatile LONG_PTR*)Lock;
    InterlockedExchangePointer((PVOID volatile*)p, (PVOID)0);
}

static BOOL WINAPI Fallback_SleepConditionVariableSRW(PCONDITION_VARIABLE CV, PSRWLOCK Lock, DWORD dwMilliseconds, ULONG Flags) {
    Fallback_ReleaseSRWLockExclusive(Lock);
    Sleep(dwMilliseconds > 10 ? 10 : dwMilliseconds);
    Fallback_AcquireSRWLockExclusive(Lock);
    return TRUE;
}

static VOID WINAPI Fallback_WakeAllConditionVariable(PCONDITION_VARIABLE CV) {
    // no-op for basic fallback
}

// Check at runtime if native KERNEL32 exports them, otherwise fallback!
typedef BOOL (WINAPI *PFN_InitCSEx)(LPCRITICAL_SECTION, DWORD, DWORD);
typedef VOID (WINAPI *PFN_SRW)(PSRWLOCK);
typedef BOOL (WINAPI *PFN_SleepSRW)(PCONDITION_VARIABLE, PSRWLOCK, DWORD, ULONG);
typedef VOID (WINAPI *PFN_WakeCV)(PCONDITION_VARIABLE);

static PFN_InitCSEx g_pInitCSEx = NULL;
static PFN_SRW g_pAcqSRW = NULL;
static PFN_SRW g_pRelSRW = NULL;
static PFN_SleepSRW g_pSleepSRW = NULL;
static PFN_WakeCV g_pWakeAllCV = NULL;

extern "C" {
BOOL WINAPI Proxy_InitializeCriticalSectionEx(LPCRITICAL_SECTION lpCS, DWORD dwSpinCount, DWORD Flags) {
    if (g_pInitCSEx) return g_pInitCSEx(lpCS, dwSpinCount, Flags);
    HMODULE hK32 = GetModuleHandleW(L"kernel32.dll");
    if (hK32) g_pInitCSEx = (PFN_InitCSEx)GetProcAddress(hK32, "InitializeCriticalSectionEx");
    if (g_pInitCSEx) return g_pInitCSEx(lpCS, dwSpinCount, Flags);
    return Fallback_InitializeCriticalSectionEx(lpCS, dwSpinCount, Flags);
}

VOID WINAPI Proxy_AcquireSRWLockExclusive(PSRWLOCK Lock) {
    if (g_pAcqSRW) { g_pAcqSRW(Lock); return; }
    HMODULE hK32 = GetModuleHandleW(L"kernel32.dll");
    if (hK32) g_pAcqSRW = (PFN_SRW)GetProcAddress(hK32, "AcquireSRWLockExclusive");
    if (g_pAcqSRW) { g_pAcqSRW(Lock); return; }
    Fallback_AcquireSRWLockExclusive(Lock);
}

VOID WINAPI Proxy_ReleaseSRWLockExclusive(PSRWLOCK Lock) {
    if (g_pRelSRW) { g_pRelSRW(Lock); return; }
    HMODULE hK32 = GetModuleHandleW(L"kernel32.dll");
    if (hK32) g_pRelSRW = (PFN_SRW)GetProcAddress(hK32, "ReleaseSRWLockExclusive");
    if (g_pRelSRW) { g_pRelSRW(Lock); return; }
    Fallback_ReleaseSRWLockExclusive(Lock);
}

BOOL WINAPI Proxy_SleepConditionVariableSRW(PCONDITION_VARIABLE CV, PSRWLOCK Lock, DWORD dwMilliseconds, ULONG Flags) {
    if (g_pSleepSRW) return g_pSleepSRW(CV, Lock, dwMilliseconds, Flags);
    HMODULE hK32 = GetModuleHandleW(L"kernel32.dll");
    if (hK32) g_pSleepSRW = (PFN_SleepSRW)GetProcAddress(hK32, "SleepConditionVariableSRW");
    if (g_pSleepSRW) return g_pSleepSRW(CV, Lock, dwMilliseconds, Flags);
    return Fallback_SleepConditionVariableSRW(CV, Lock, dwMilliseconds, Flags);
}

VOID WINAPI Proxy_WakeAllConditionVariable(PCONDITION_VARIABLE CV) {
    if (g_pWakeAllCV) { g_pWakeAllCV(CV); return; }
    HMODULE hK32 = GetModuleHandleW(L"kernel32.dll");
    if (hK32) g_pWakeAllCV = (PFN_WakeCV)GetProcAddress(hK32, "WakeAllConditionVariable");
    if (g_pWakeAllCV) { g_pWakeAllCV(CV); return; }
    Fallback_WakeAllConditionVariable(CV);
}

#if !defined(_M_IX86)
// Satisfy MSVC CRT import symbols for x64
BOOL (WINAPI *__imp_InitializeCriticalSectionEx)(LPCRITICAL_SECTION, DWORD, DWORD) = Proxy_InitializeCriticalSectionEx;
VOID (WINAPI *__imp_AcquireSRWLockExclusive)(PSRWLOCK) = Proxy_AcquireSRWLockExclusive;
VOID (WINAPI *__imp_ReleaseSRWLockExclusive)(PSRWLOCK) = Proxy_ReleaseSRWLockExclusive;
BOOL (WINAPI *__imp_SleepConditionVariableSRW)(PCONDITION_VARIABLE, PSRWLOCK, DWORD, ULONG) = Proxy_SleepConditionVariableSRW;
VOID (WINAPI *__imp_WakeAllConditionVariable)(PCONDITION_VARIABLE) = Proxy_WakeAllConditionVariable;
#endif
}
