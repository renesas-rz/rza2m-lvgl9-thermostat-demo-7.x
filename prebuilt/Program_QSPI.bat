@echo off
rem Double-click to start the PowerShell flasher (Program_QSPI.ps1 in this folder)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Program_QSPI.ps1" %*
pause
