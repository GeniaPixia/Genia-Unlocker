@echo off
setlocal EnableExtensions
cd /d "%~dp0"

echo === Genia Unlocker v0.6.0 Final release packager ===
echo.

call "%~dp0build-portable.cmd"
if errorlevel 1 (
  echo.
  echo [ERROR] Build failed. Release package was not created.
  exit /b %errorlevel%
)

if not exist "%~dp0dist\GeniaUnlocker.exe" (
  echo [ERROR] dist\GeniaUnlocker.exe was not produced.
  exit /b 10
)

set "RELEASE_ROOT=%~dp0release"
set "PACKAGE_DIR=%RELEASE_ROOT%\Genia-Unlocker-v0.6.0"
set "PACKAGE_ZIP=%RELEASE_ROOT%\Genia-Unlocker-v0.6.0-Windows-x64-Portable.zip"
set "HASH_FILE=%RELEASE_ROOT%\Genia-Unlocker-v0.6.0-Windows-x64-Portable-SHA256.txt"

if exist "%PACKAGE_DIR%" rmdir /s /q "%PACKAGE_DIR%"
if not exist "%RELEASE_ROOT%" mkdir "%RELEASE_ROOT%"
mkdir "%PACKAGE_DIR%"

copy /y "%~dp0dist\GeniaUnlocker.exe" "%PACKAGE_DIR%\GeniaUnlocker.exe" >nul
copy /y "%~dp0README.md" "%PACKAGE_DIR%\README.md" >nul
copy /y "%~dp0CHANGELOG.md" "%PACKAGE_DIR%\CHANGELOG.md" >nul
copy /y "%~dp0RELEASE-NOTES.md" "%PACKAGE_DIR%\RELEASE-NOTES.md" >nul
copy /y "%~dp0LICENSE" "%PACKAGE_DIR%\LICENSE" >nul

powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -Command ^
  "$ErrorActionPreference='Stop';" ^
  "if (Test-Path -LiteralPath '%PACKAGE_ZIP%') { Remove-Item -LiteralPath '%PACKAGE_ZIP%' -Force };" ^
  "Compress-Archive -LiteralPath '%PACKAGE_DIR%\GeniaUnlocker.exe','%PACKAGE_DIR%\README.md','%PACKAGE_DIR%\CHANGELOG.md','%PACKAGE_DIR%\RELEASE-NOTES.md','%PACKAGE_DIR%\LICENSE' -DestinationPath '%PACKAGE_ZIP%' -CompressionLevel Optimal;" ^
  "$h=(Get-FileHash -Algorithm SHA256 -LiteralPath '%PACKAGE_ZIP%').Hash;" ^
  "Set-Content -LiteralPath '%HASH_FILE%' -Value ($h + '  Genia-Unlocker-v0.6.0-Windows-x64-Portable.zip') -Encoding ASCII"
if errorlevel 1 (
  echo [ERROR] Could not create release ZIP.
  exit /b 11
)

echo.
echo [OK] Final release created:
echo      %PACKAGE_ZIP%
echo [OK] SHA256:
echo      %HASH_FILE%
exit /b 0
