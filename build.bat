@echo off
setlocal enabledelayedexpansion

echo ============================================================
echo  Building DelUnlock (Native Win32 C++17, Static CRT)
echo ============================================================

set "TARGET_ARCH=%~1"
if "%TARGET_ARCH%"=="" set "TARGET_ARCH=x64"

where cl.exe >nul 2>nul
if %ERRORLEVEL% equ 0 (
    echo [+] Microsoft Visual C++ Compiler already in PATH.
) else (
    set "VCVARS="
    if /i "%TARGET_ARCH%"=="x86" (
        for /f "delims=" %%i in ('dir /b /s "C:\Program Files (x86)\Microsoft Visual Studio\vcvars32.bat" 2^>nul') do set "VCVARS=%%i"
        if not defined VCVARS for /f "delims=" %%i in ('dir /b /s "C:\Program Files\Microsoft Visual Studio\vcvars32.bat" 2^>nul') do set "VCVARS=%%i"
    ) else (
        for /f "delims=" %%i in ('dir /b /s "C:\Program Files (x86)\Microsoft Visual Studio\vcvars64.bat" 2^>nul') do set "VCVARS=%%i"
        if not defined VCVARS for /f "delims=" %%i in ('dir /b /s "C:\Program Files\Microsoft Visual Studio\vcvars64.bat" 2^>nul') do set "VCVARS=%%i"
    )

    if not defined VCVARS (
        echo [!] Could not find vcvars script automatically for %TARGET_ARCH%.
        echo [!] Please run this from a Developer Command Prompt for VS.
        exit /b 1
    )

    echo [+] Initializing MSVC environment via: !VCVARS!
    call "!VCVARS!" >nul
)

if not exist build mkdir build

rc.exe /nologo /fo build\resource.res res\resource.rc

set "SUBSYS=5.02"
set "OUTNAME=DelUnlock.exe"
set "EXTRA_OBJS="
if /i "%TARGET_ARCH%"=="x86" (
    set "SUBSYS=5.01"
    set "OUTNAME=DelUnlock-x86.exe"
    ml.exe /nologo /c /Fo build\xp_compat_x86.obj src\xp_compat_x86.asm
    set "EXTRA_OBJS=build\xp_compat_x86.obj"
)

cl.exe /nologo /O2 /Ob2 /Oi /Os /GL /GS- /EHsc /std:c++17 /MT ^
    /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_USING_V110_SDK71_ /D_WIN32_WINNT=0x0501 ^
    src\xp_compat.cpp src\main.cpp src\unlocker.cpp src\ui.cpp src\config.cpp ^
    build\resource.res !EXTRA_OBJS! ^
    /Fe:%OUTNAME% /Fo:build\ ^
    /link /LTCG /OPT:REF /OPT:ICF /SUBSYSTEM:WINDOWS,%SUBSYS% /MAP:build\delunlock.map ^
    user32.lib gdi32.lib shell32.lib shlwapi.lib comctl32.lib comdlg32.lib ^
    advapi32.lib psapi.lib version.lib uxtheme.lib ole32.lib oleaut32.lib oleacc.lib uuid.lib

if %ERRORLEVEL% neq 0 (
    echo [!] Compilation failed!
    exit /b %ERRORLEVEL%
)

echo [+] Successfully built %OUTNAME%!
for %%A in (%OUTNAME%) do echo [+] Binary size: %%~zA bytes
