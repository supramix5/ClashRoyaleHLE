@echo off
rem launch.bat <save folder>: starts the client with that save folder (its account login, settings and downloaded
rem content updates). The server address comes from server.txt (host:port).
setlocal
cd /d "%~dp0.."
if not exist "%~1" mkdir "%~1"
set "HLE_SANDBOX=%~1"
set "HLE_SERVER=127.0.0.1:9339"
if exist server.txt set /p HLE_SERVER=<server.txt
set "HLE_QUIET=bridge,data,file,keychain,msg,touch"
title Clash Royale - %~nx1 (%HLE_SERVER%)
"%~dp0ClashRoyaleHLE.exe" "%~dp0..\game\library"
