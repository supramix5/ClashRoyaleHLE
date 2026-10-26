@echo off
rem Starts a client with no account (account id 0,0): the server creates a brand-new player every time.
rem Nothing is kept on this computer: the temporary save folder is deleted when the client closes.
setlocal
set "FRESH=%TEMP%\ClashRoyaleHLE-0-0-%RANDOM%%RANDOM%"
call "%~dp0bin\launch.bat" "%FRESH%"
rmdir /s /q "%FRESH%" 2>nul
