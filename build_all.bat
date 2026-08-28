@echo off
REM Build both halves of Nokia Klatt SAPI5, then the installer.
REM
REM Both architectures are needed: the x64 half carries the emulator, the host
REM and the settings utility, and the x86 half is the 32-bit SAPI engine that
REM talks to that host. An installer with only one of them leaves half the
REM applications on the machine unable to see the voices.

setlocal
cd /d "%~dp0"

set INNO="%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe"

echo.
echo === Configuring x64 ===
cmake -S . -B build\x64 -G "Visual Studio 17 2022" -A x64 || goto :failed

echo.
echo === Building x64 (emulator, host, 64-bit SAPI, settings utility) ===
cmake --build build\x64 --config Release || goto :failed

echo.
echo === Configuring x86 ===
cmake -S . -B build\x86 -G "Visual Studio 17 2022" -A Win32 || goto :failed

echo.
echo === Building x86 (32-bit SAPI) ===
cmake --build build\x86 --config Release || goto :failed

if "%1"=="--no-installer" goto :done

if not exist %INNO% (
    echo.
    echo Inno Setup was not found at %INNO%
    echo The binaries are built; skipping the installer.
    goto :done
)

echo.
echo === Building the installer ===
if not exist dist mkdir dist
%INNO% installer\nokia_klatt.iss || goto :failed

:done
echo.
echo Build finished.
echo   x64 binaries: build\x64\bin\Release
echo   x86 binaries: build\x86\bin\Release
if exist dist\*.exe echo   installer:    dist\
exit /b 0

:failed
echo.
echo BUILD FAILED
exit /b 1
