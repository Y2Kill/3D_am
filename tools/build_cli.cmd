@echo off
rem Builds and tests the default vcpkg dependencies (preset win-cli).
rem --offline uses project caches read-only and blocks origin downloads.
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
"%VSWHERE%" -latest -products * -version "[17.0,18.0)" -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath > "%TMP%\am-vswhere-cli.txt" || exit /b 1
for /f "usebackq delims=" %%I in ("%TMP%\am-vswhere-cli.txt") do set "VSINSTALL=%%I"
if not defined VSINSTALL (
  echo ERROR: Visual Studio 2022 with x64 C++ tools was not found by vswhere.
  exit /b 1
)
echo Visual Studio: %VSINSTALL%
call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

set "VCPKG_ROOT=%ROOT%\.cache\vcpkg"
set "VCPKG_DISABLE_METRICS=1"
set "VCPKG_DOWNLOADS=%VCPKG_ROOT%\downloads"
set "VCPKG_REGISTRIES_CACHE=%ROOT%\.cache\vcpkg-registries"
set "VCPKG_DEFAULT_BINARY_CACHE=%ROOT%\.cache\vcpkg-bin"
set "CACHE_URI=%ROOT:\=/%"
set "VCPKG_BINARY_SOURCES=clear;files,%ROOT%/.cache/vcpkg-bin,readwrite"
set "X_VCPKG_ASSET_SOURCES=clear;x-azurl,file:///%CACHE_URI%/.cache/vcpkg-assets,,readwrite"
if not exist "%VCPKG_ROOT%\vcpkg.exe" (
  echo ERROR: Bootstrap .cache\vcpkg with bootstrap-vcpkg.bat -disableMetrics first.
  exit /b 1
)
for %%I in ("%VCPKG_DOWNLOADS%" "%VCPKG_REGISTRIES_CACHE%" "%VCPKG_DEFAULT_BINARY_CACHE%" "%ROOT%\.cache\vcpkg-assets") do (
  if not exist "%%~I" mkdir "%%~I" || exit /b 1
)
set "CI_CONFIG=-DAM_REQUIRE_PYTHON_TESTS=OFF"
set "CI_CTEST="
set "CTEST_ARGS="
:collect_args
if "%~1"=="" goto build
if "%~1"=="--ci" (
  set "CI_CONFIG=-DAM_REQUIRE_PYTHON_TESTS=ON"
  set CI_CTEST=--output-on-failure --output-junit "%ROOT%/build/win-cli/junit.xml"
  shift
  goto collect_args
)
if "%~1"=="--offline" (
  set "VCPKG_BINARY_SOURCES=clear;files,%ROOT%/.cache/vcpkg-bin,read"
  set "X_VCPKG_ASSET_SOURCES=clear;x-azurl,file:///%CACHE_URI%/.cache/vcpkg-assets,,read;x-block-origin"
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
echo VCPKG_BINARY_SOURCES=%VCPKG_BINARY_SOURCES%
echo X_VCPKG_ASSET_SOURCES=%X_VCPKG_ASSET_SOURCES%
cmake --preset win-cli %CI_CONFIG% %PYTHON_CONFIG% || exit /b 1
cmake --build --preset win-cli || exit /b 1
call "%ROOT%\tools\check_ninja_deps.cmd" "%ROOT%\build\win-cli" || exit /b 1
ctest --preset win-cli %CI_CTEST% %CTEST_ARGS%
set "CTEST_RESULT=%ERRORLEVEL%"
if not defined CI_CTEST exit /b %CTEST_RESULT%
python -I "%ROOT%\tools\coverage\req_report.py" --build-dir "%ROOT%\build\win-cli" --matrix "%ROOT%\docs\plan\acceptance_matrix.md"
set "REPORT_RESULT=%ERRORLEVEL%"
if not "%CTEST_RESULT%"=="0" exit /b %CTEST_RESULT%
exit /b %REPORT_RESULT%
