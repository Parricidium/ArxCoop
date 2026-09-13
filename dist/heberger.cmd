@echo off
rem Heberge une partie coop. Usage : heberger.cmd [pseudo] [port]
set NICK=%1
if "%NICK%"=="" set NICK=Hote
set PORT=%2
if "%PORT%"=="" set PORT=27015
"%~dp0arx.exe" --coop-host %PORT% --nickname %NICK%
