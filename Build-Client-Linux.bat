@echo off
rem Compiles the Linux build from Windows, inside WSL (Windows Subsystem for Linux), by running build_client.sh there.
rem On a Linux PC, run ./build_client.sh instead.
setlocal
cd /d "%~dp0"
where wsl >nul 2>nul || goto nowsl
set "build_client.sh="
for /f "usebackq delims=" %%p in (`wsl wslpath -a "%~dp0build_client.sh"`) do set "build_client.sh=%%p"
if not defined build_client.sh goto nowsl
wsl -e bash "%build_client.sh%" || goto fail
pause
exit /b 0

:nowsl
echo WSL (Windows Subsystem for Linux) with a Linux distribution is needed: run "wsl --install" in an
echo administrator command prompt, restart, then run this again.
:fail
echo.
echo Compiling for Linux failed (see the messages above).
pause
exit /b 1
