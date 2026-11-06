@echo off
rem Builds and tests the dependency-free base layers (preset win-debug-min).
rem All temporary and downloaded files stay inside the project folder.
setlocal
set "ROOT=%~dp0.."
set "TMP=%ROOT%\.cache\tmp"
set "TEMP=%ROOT%\.cache\tmp"
if not exist "%TMP%" mkdir "%TMP%"
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%ROOT%"
cmake --preset win-debug-min || exit /b 1
cmake --build --preset win-debug-min || exit /b 1
ctest --preset win-debug-min %* || exit /b 1
endlocal
