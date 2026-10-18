@echo off
setlocal
set VCPKG_ROOT=%~dp0..\tools\vcpkg
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSPATH=%%i
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
cmake --preset default || exit /b 1
cmake --build --preset default || exit /b 1
