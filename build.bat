@echo off
rem Builds DSCSPlayerModelSwap.dll (Release, x64).
rem Optional: set DSCSML_SOURCE=<path to a DSCSModLoader checkout> to build against a local copy.
setlocal
if not defined VSINSTALLDIR (
    for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALLDIR=%%i\"
)
call "%VSINSTALLDIR%VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"

set EXTRA=
if defined DSCSML_SOURCE set EXTRA=-DCPM_DSCSModLoader_SOURCE=%DSCSML_SOURCE%

rem CMake 4 + Boost 1.80: policy minimum; newer MSVC: disable Boost auto-link (CMake links explicitly)
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl ^
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 "-DCMAKE_CXX_FLAGS=/DBOOST_ALL_NO_LIB /EHsc" %EXTRA% || exit /b 1
cmake --build build --target DSCSPlayerModelSwap || exit /b 1
echo BUILD-OK
