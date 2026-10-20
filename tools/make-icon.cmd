@echo off
setlocal
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSPATH=%%i
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
set QT_QPA_PLATFORM=offscreen
set QT_PLUGIN_PATH=%~dp0..\build\vcpkg_installed\x64-windows\Qt6\plugins
cmake --build --preset default --target brandtool || exit /b 1
build\Release\brandtool.exe "%~dp0..\assets" || exit /b 1
