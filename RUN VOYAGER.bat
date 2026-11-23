@echo off
title Voyager OS (QEMU)
cd /d "%~dp0"
setlocal EnableDelayedExpansion

set "ISO=Your ISO is here\voyager.iso"
set "VDI=voyager-hd.vdi"

if not exist "%ISO%" (
    echo ERROR: voyager.iso not found in "Your ISO is here" folder.
    pause
    exit /b 1
)

rem --- locate QEMU: PATH first, then every common install location ---
call :findqemu

rem --- not found: try winget, then direct installer download ---
if not defined QEMU (
    echo QEMU not found - installing automatically via winget...
    winget install --id SoftwareFreedomConservancy.QEMU -e --accept-source-agreements --accept-package-agreements >nul 2>&1
    call :findqemu
)
if not defined QEMU (
    echo winget unavailable - downloading QEMU installer...
    powershell -NoProfile -ExecutionPolicy Bypass -Command "Invoke-WebRequest -Uri 'https://qemu.weilnetz.de/w64/qemu-w64-setup-20260811.exe' -OutFile '%TEMP%\voyager-qemu-setup.exe'"
    if exist "%TEMP%\voyager-qemu-setup.exe" (
        echo Running silent install - this can take a few minutes...
        start "" /wait "%TEMP%\voyager-qemu-setup.exe" /S
        del "%TEMP%\voyager-qemu-setup.exe" >nul 2>&1
    )
    call :findqemu
)
if not defined QEMU (
    echo Could not find or install QEMU.
    echo Install it manually from https://www.qemu.org/download/
    pause
    exit /b 1
)
echo Using QEMU: !QEMU!

rem --- qemu-img: PATH, then same folder as QEMU ---
set "QIMG="
for /f "delims=" %%I in ('where qemu-img.exe 2^>nul') do if not defined QIMG set "QIMG=%%I"
for /f "delims=" %%I in ('where qemu-img 2^>nul') do if not defined QIMG set "QIMG=%%I"
if not defined QIMG (
    for %%I in ("!QEMU!") do set "QDIR=%%~dpI"
    if exist "!QDIR!qemu-img.exe" set "QIMG=!QDIR!qemu-img.exe"
)

rem --- create the persistent settings disk if missing ---
set "DISKARGS="
if not exist "%VDI%" (
    echo Creating Voyager settings disk voyager-hd.vdi...
    if defined QIMG (
        "%QIMG%" create -f vdi "%VDI%" 16M
    ) else (
        echo qemu-img not found - continuing without a settings disk.
    )
)
if exist "%VDI%" set "DISKARGS=-drive file="%VDI%",format=vdi,if=ide,index=0,media=disk"

echo Booting Voyager OS in QEMU - networking and sound enabled. Close the window to exit.
"!QEMU!" -machine pc -m 1024 !DISKARGS! -cdrom "%ISO%" -boot d -display sdl -netdev user,id=n0 -device e1000,netdev=n0 -audiodev sdl,id=snd0 -device AC97,audiodev=snd0 -serial file:qemu-serial.log -monitor none -no-reboot

pause
exit /b 0

rem ============================================================
:findqemu
set "QEMU="
for /f "delims=" %%I in ('where qemu-system-x86_64.exe 2^>nul') do if not defined QEMU set "QEMU=%%I"
for /f "delims=" %%I in ('where qemu-system-x86_64 2^>nul') do if not defined QEMU set "QEMU=%%I"
if not defined QEMU for %%D in (
    "C:\Program Files\qemu"
    "C:\Program Files (x86)\qemu"
    "%ProgramFiles%\qemu"
    "%ProgramFiles(x86)%\qemu"
    "%LOCALAPPDATA%\Programs\qemu"
    "%USERPROFILE%\scoop\apps\qemu\current"
    "%USERPROFILE%\scoop\apps\qemu\current\bin"
    "C:\qemu"
    "C:\tools\qemu"
    "%ChocolateyInstall%\lib\qemu"
) do (
    if exist "%%~D\qemu-system-x86_64.exe" if not defined QEMU set "QEMU=%%~D\qemu-system-x86_64.exe"
)
exit /b 0
