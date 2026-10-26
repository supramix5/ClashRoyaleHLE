@echo off
rem Creates a new saved player: an empty save folder in players\<name> (the first login is account 0,0, so the server
rem creates a new account and the save keeps it) and a Play-<name>.bat that always plays that save.
setlocal
cd /d "%~dp0"
set "NAME="
set /p "NAME=Name for the new player save: "
if not defined NAME goto :eof
for /f "usebackq delims=" %%n in (`powershell -NoProfile -Command "$env:NAME.Trim() -replace '[^A-Za-z0-9_-]', '_'"`) do set "SAFE=%%n"
if not defined SAFE goto :eof
if exist "players\%SAFE%" (
    echo A player save named "%SAFE%" already exists. Use Play-%SAFE%.bat to play it.
    pause
    exit /b 1
)
mkdir "players\%SAFE%"
> "Play-%SAFE%.bat" (
    echo @echo off
    echo rem Plays the saved player "%SAFE%" ^(save folder players\%SAFE%^).
    echo call "%%~dp0bin\launch.bat" "%%~dp0players\%SAFE%"
)
echo Created players\%SAFE% and Play-%SAFE%.bat
choice /m "Start %SAFE% now"
if errorlevel 2 goto :eof
call "Play-%SAFE%.bat"
