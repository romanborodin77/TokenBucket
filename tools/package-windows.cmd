@echo off
setlocal
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSPATH=%%i
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
cmake --build --preset default || exit /b 1
if exist dist\TokenBucket rmdir /s /q dist\TokenBucket
mkdir dist\TokenBucket 2>nul
copy build\Release\TokenBucket.exe dist\TokenBucket\ || exit /b 1
build\vcpkg_installed\x64-windows\tools\Qt6\bin\windeployqt.exe --release --no-translations dist\TokenBucket\TokenBucket.exe || exit /b 1
copy build\vcpkg_installed\x64-windows\bin\libsodium.dll dist\TokenBucket\ || exit /b 1
mkdir dist\TokenBucket\licenses 2>nul
copy docs\licenses\* dist\TokenBucket\licenses\ || exit /b 1
powershell -NoProfile -Command "Compress-Archive -Force -Path dist\TokenBucket -DestinationPath dist\TokenBucket-windows-x64.zip"
