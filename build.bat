@echo off
setlocal enabledelayedexpansion

echo ============================================================
echo  Building DelUnlock (Native Win32 C++17, Static CRT)
echo ============================================================

where cl.exe >nul 2>nul
if %ERRORLEVEL% equ 0 (
    echo [+] Microsoft Visual C++ Compiler already in PATH.
) else (
    set "VCVARS="
    for /f "delims=" %%i in ('dir /b /s "C:\Program Files (x86)\Microsoft Visual Studio\vcvars64.bat" 2^>nul') do (
        set "VCVARS=%%i"
    )
    if not defined VCVARS (
        for /f "delims=" %%i in ('dir /b /s "C:\Program Files\Microsoft Visual Studio\vcvars64.bat" 2^>nul') do (
            set "VCVARS=%%i"
        )
    )

    if not defined VCVARS (
        echo [!] Could not find vcvars64.bat automatically.
        echo [!] Please run this from a Developer Command Prompt for VS.
        exit /b 1
    )

    echo [+] Initializing MSVC environment via: !VCVARS!
    call "!VCVARS!" >nul
)

if not exist build mkdir build

rc.exe /nologo /fo build\resource.res res\resource.rc

:: Build 64-bit binary (compatible with 64-bit Windows XP Professional x64, Vista, 7, 8, 8.1, 10, 11)
:: /SUBSYSTEM:WINDOWS,5.02 targets Windows XP x64 & Windows Server 2003
cl.exe /nologo /O2 /Ob2 /Oi /Os /GL /GS- /EHsc /std:c++17 /MT ^
    /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_USING_V110_SDK71_ /D_WIN32_WINNT=0x0502 ^
    src\xp_compat.cpp src\main.cpp src\unlocker.cpp src\ui.cpp src\config.cpp ^
    build\resource.res ^
    /Fe:DelUnlock.exe /Fo:build\ ^
    /link /LTCG /OPT:REF /OPT:ICF /SUBSYSTEM:WINDOWS,5.02 /MAP:build\delunlock.map ^
    user32.lib gdi32.lib shell32.lib shlwapi.lib comctl32.lib comdlg32.lib ^
    advapi32.lib psapi.lib version.lib uxtheme.lib ole32.lib oleaut32.lib oleacc.lib uuid.lib

if %ERRORLEVEL% neq 0 (
    echo [!] Compilation failed!
    exit /b %ERRORLEVEL%
)

echo [+] Successfully built DelUnlock.exe!
for %%A in (DelUnlock.exe) do echo [+] Binary size: %%~zA bytes
