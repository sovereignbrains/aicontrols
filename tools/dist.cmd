@echo off
rem Release build and installer.
rem Usage: tools\dist.cmd <version>      e.g. tools\dist.cmd 0.1.0
rem Makes build\installer\AIControls-Setup-<version>.exe (per user, no administrator rights).
if "%~1"=="" (
  echo usage: tools\dist.cmd ^<version^>
  exit /b 2
)
setlocal
cd /d "%~dp0.."
call "%~dp0build.cmd" dist || exit /b 1
if not exist build\dist\aicontrols.exe (
  echo build\dist\aicontrols.exe is missing
  exit /b 1
)
"C:\Program Files (x86)\Inno Setup 6\ISCC.exe" /Qp /DAppVersion=%~1 /DSourceDir="%CD%\build\dist" /DOutputDir="%CD%\build\installer" installer\aicontrols.iss || exit /b 1
echo.
echo build\installer\AIControls-Setup-%~1.exe
