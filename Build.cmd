@echo off
setlocal
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Scripts\Build.ps1" %*
set "buildExitCode=%errorlevel%"
pause
exit /b %buildExitCode%
