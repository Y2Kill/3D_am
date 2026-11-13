@echo off
rem Project-local clone, pinned by vcpkg.json. Optional argument: cache directory.
setlocal DisableDelayedExpansion
for %%I in ("%~dp0..\..") do set "ROOT=%%~fI"
set "TMP=%ROOT%\.cache\tmp"
set "TEMP=%TMP%"
if not exist "%TMP%" mkdir "%TMP%" || exit /b 1
python "%~dp0bootstrap_vcpkg.py" %* || exit /b 1
endlocal
