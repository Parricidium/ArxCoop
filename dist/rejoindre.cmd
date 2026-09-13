@echo off
rem Rejoint une partie coop. Usage : rejoindre.cmd ADRESSE[:PORT] [pseudo]
set TARGET=%1
if "%TARGET%"=="" set /p TARGET=Adresse IP de l'hote (ex. 192.168.1.10 ou 192.168.1.10:27015) : 
set NICK=%2
if "%NICK%"=="" set NICK=Joueur2
"%~dp0arx.exe" --coop-join %TARGET% --nickname %NICK%
