@echo off
rem Removes Gitmo from SnowRunner (close the game first).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install-Gitmo.ps1" -Uninstall
