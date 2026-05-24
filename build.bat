@echo off
setlocal

echo.
echo  [*] Building latency.exe...
echo.

:: ─── Try MSVC (cl.exe) first ───────────────────────────────────────────────
where cl.exe >nul 2>&1
if %errorlevel% == 0 (
    echo  [*] Compiler: MSVC
    cl.exe main.cpp ^
        /O2 /MT /W3 /std:c++17 ^
        /Fe:latency.exe ^
        /link ^
            powrprof.lib ^
            shlwapi.lib ^
            advapi32.lib ^
            ole32.lib ^
        /subsystem:console ^
        /machine:x64 ^
        /nologo
    if %errorlevel% == 0 goto success
    echo  [!] MSVC build failed.
    goto try_mingw
)

:try_mingw
:: ─── Try MinGW (g++) ────────────────────────────────────────────────────────
where g++.exe >nul 2>&1
if %errorlevel% == 0 (
    echo  [*] Compiler: MinGW g++
    g++.exe main.cpp ^
        -O2 -std=c++17 -Wall -static ^
        -o latency.exe ^
        -ladvapi32 ^
        -lpowrprof ^
        -lshlwapi ^
        -lole32
    if %errorlevel% == 0 goto success
    echo  [!] MinGW build failed.
    goto no_compiler
)

:no_compiler
echo.
echo  [!] No compiler found.
echo      Install either:
echo        - MSVC  : Visual Studio Build Tools (cl.exe in PATH)
echo        - MinGW : https://winlibs.com  (g++ in PATH)
echo.
exit /b 1

:success
echo.
echo  [+] Build OK  ^>^>  latency.exe
echo.
echo  Usage:
echo    latency.exe apply           ^(apply all tweaks, needs Admin^)
echo    latency.exe apply --boot    ^(include bcdedit tweaks^)
echo    latency.exe restore         ^(undo everything^)
echo    latency.exe status          ^(current state^)
echo    latency.exe timer start     ^(0.5ms background daemon^)
echo    latency.exe timer stop
echo    latency.exe help
echo.
endlocal
