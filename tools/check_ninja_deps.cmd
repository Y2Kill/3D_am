@echo off
rem Fails when Ninja recorded no header dependencies for an object file.
rem Cause: the cached MSVC /showIncludes prefix (localized cl.exe) does not
rem match compiler output, so header edits would silently skip recompilation.
rem build_min.cmd / build_cli.cmd prevent this by pinning code page 65001.
setlocal DisableDelayedExpansion
set "BUILD_DIR=%~1"
set "DEPS_FILE=%TMP%\am-ninja-deps.txt"
ninja -C "%BUILD_DIR%" -t deps > "%DEPS_FILE%" || exit /b 1
findstr /C:"#deps 0," "%DEPS_FILE%" >nul
if errorlevel 1 goto ok
echo ERROR: Ninja recorded no header dependencies in %BUILD_DIR%:
findstr /C:"#deps 0," "%DEPS_FILE%"
echo Build scripts pin console code page 65001; run them instead of plain cmake --build,
echo or delete %BUILD_DIR% and build again.
exit /b 1
:ok
endlocal & exit /b 0
