@echo off
rem Builds and tests the default vcpkg dependencies (preset win-cli).
rem --offline uses project caches read-only and blocks origin downloads.
setlocal DisableDelayedExpansion
for %%I in ("%~dp0..") do set "ROOT=%%~fI"
set "TMP=%ROOT%\.cache\tmp"
set "TEMP=%ROOT%\.cache\tmp"
if not exist "%TMP%" mkdir "%TMP%" || exit /b 1
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

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
if "%~1"=="--offline" (
  set "VCPKG_BINARY_SOURCES=clear;files,%ROOT%/.cache/vcpkg-bin,read"
  set "X_VCPKG_ASSET_SOURCES=clear;x-azurl,file:///%CACHE_URI%/.cache/vcpkg-assets,,read;x-block-origin"
  shift
)
set "CTEST_ARGS="
:collect_args
if "%~1"=="" goto build
set "CTEST_ARGS=%CTEST_ARGS% %1"
shift
goto collect_args

:build
cd /d "%ROOT%" || exit /b 1
echo VCPKG_BINARY_SOURCES=%VCPKG_BINARY_SOURCES%
echo X_VCPKG_ASSET_SOURCES=%X_VCPKG_ASSET_SOURCES%
cmake --preset win-cli || exit /b 1
cmake --build --preset win-cli || exit /b 1
ctest --preset win-cli %CTEST_ARGS% || exit /b 1
endlocal
