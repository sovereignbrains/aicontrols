@echo off
rem Build helper: the VS dev environment, then a CMake preset (dev, dist, analyze).
rem Usage: tools\build.cmd <preset>
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
set VSLANG=1033
cd /d "%~dp0.."
cmake --preset %1 >"%TEMP%\aic-configure-%1.log" 2>&1 || (type "%TEMP%\aic-configure-%1.log" & exit /b 1)
cmake --build --preset %1 2>&1
