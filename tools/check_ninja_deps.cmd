@echo off
rem Fails when Ninja recorded no header dependencies for an object file.
rem Cause: the cached MSVC /showIncludes prefix (localized cl.exe) does not
rem match compiler output, so header edits would silently skip recompilation.
setlocal DisableDelayedExpansion
set "BUILD_DIR=%~1"
set "DEPS_FILE=%TMP%\am-ninja-deps.txt"
ninja -C "%BUILD_DIR%" -t deps > "%DEPS_FILE%" || exit /b 1
findstr /C:"#deps 0," "%DEPS_FILE%" >nul
if errorlevel 1 goto ok
echo ERROR: Ninja recorded no header dependencies in %BUILD_DIR%:
findstr /C:"#deps 0," "%DEPS_FILE%"
echo Delete %BUILD_DIR% and build again from one console code page.
exit /b 1
:ok
endlocal & exit /b 0
