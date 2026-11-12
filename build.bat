@echo off
rem Builds MkRender (Release, Ninja) and runs the tests: libraries, tests and the MkRender viewer (build\bin\MkRender.exe).
rem   build.bat          DX12 backend into build\
rem   build.bat Vulkan   Vulkan backend into build-vulkan\
setlocal
set "BUILD=build"
set "BACKEND=DX12"
if /i "%~1"=="Vulkan" (
    set "BUILD=build-vulkan"
    set "BACKEND=Vulkan"
)
if defined VCINSTALLDIR goto build
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VS=%%i"
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

:build
cd /d "%~dp0"
if not exist %BUILD%\build.ninja cmake -S . -B %BUILD% -G Ninja -DCMAKE_BUILD_TYPE=Release -DMKRHI_BACKEND=%BACKEND% || exit /b 1
cmake --build %BUILD% || exit /b 1
ctest --test-dir %BUILD% --output-on-failure || exit /b 1
echo [MkRender] Build OK
