@echo off
rem Installs or updates Gitmo in SnowRunner (close the game first).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install-Gitmo.ps1"
