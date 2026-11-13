@echo off
rem Builds and tests the dependency-free base layers (preset win-debug-min).
rem --ci requires Python checks and writes JUnit; --offline uses cached GoogleTest.
setlocal DisableDelayedExpansion
for %%I in ("%~dp0..") do set "ROOT=%%~fI"
set "TMP=%ROOT%\.cache\tmp"
set "TEMP=%ROOT%\.cache\tmp"
if not exist "%TMP%" mkdir "%TMP%" || exit /b 1
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo ERROR: vswhere.exe not found. Install Visual Studio 2022 with C++ tools.
  exit /b 1
)
set "VSINSTALL="
"%VSWHERE%" -latest -products * -version "[17.0,18.0)" -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath > "%TMP%\am-vswhere-min.txt" || exit /b 1
for /f "usebackq delims=" %%I in ("%TMP%\am-vswhere-min.txt") do set "VSINSTALL=%%I"
if not defined VSINSTALL (
  echo ERROR: Visual Studio 2022 with x64 C++ tools was not found by vswhere.
  exit /b 1
)
echo Visual Studio: %VSINSTALL%
call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "CI_CONFIG=-DAM_REQUIRE_PYTHON_TESTS=OFF"
set "CI_CTEST="
set "FETCH_CONFIG=-DFETCHCONTENT_FULLY_DISCONNECTED=OFF"
set "CTEST_ARGS="
:collect_args
if "%~1"=="" goto build
if "%~1"=="--ci" (
  set "CI_CONFIG=-DAM_REQUIRE_PYTHON_TESTS=ON"
  set CI_CTEST=--output-on-failure --output-junit "%ROOT%/build/win-debug-min/junit.xml"
  shift
  goto collect_args
)
if "%~1"=="--offline" (
  set "FETCH_CONFIG=-DFETCHCONTENT_FULLY_DISCONNECTED=ON"
  shift
  goto collect_args
)
set "CTEST_ARGS=%CTEST_ARGS% %1"
shift
goto collect_args

:build
set "PYTHON_CONFIG="
if defined AM_PYTHON_EXTRA_PATH set PYTHON_CONFIG="-DAM_PYTHON_EXTRA_PATH=%AM_PYTHON_EXTRA_PATH%"
cd /d "%ROOT%" || exit /b 1
cmake --preset win-debug-min %CI_CONFIG% %FETCH_CONFIG% %PYTHON_CONFIG% || exit /b 1
cmake --build --preset win-debug-min || exit /b 1
call "%ROOT%\tools\check_ninja_deps.cmd" "%ROOT%\build\win-debug-min" || exit /b 1
ctest --preset win-debug-min %CI_CTEST% %CTEST_ARGS% || exit /b 1
endlocal
