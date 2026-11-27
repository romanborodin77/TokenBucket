@echo off
setlocal
pushd "%~dp0.." || exit /b 1
call :package
set "result=%errorlevel%"
popd
exit /b %result%

:package
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cmake --build --preset default || exit /b 1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0package-windows.ps1"
exit /b %errorlevel%
