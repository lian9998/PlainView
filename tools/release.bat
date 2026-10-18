@echo off
rem Build a portable release archive with the release build of the toolchain.
rem Usage: tools\release.bat [-SkipBuild]
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0release.ps1" %*
exit /b %ERRORLEVEL%
