@echo off
REM Собирает игру на Windows и упаковывает в готовый к раздаче zip-архив.
REM Требует: CMake и Visual Studio (Desktop development with C++) уже установлены.
REM
REM Использование:
REM   scripts\build_windows.bat
REM   scripts\build_windows.bat MyOtherGame

setlocal

set GAME_NAME=%1
if "%GAME_NAME%"=="" set GAME_NAME=TheBoat

cd /d "%~dp0\.."

set /p VERSION=<VERSION
set BUILD_DIR=build-windows-native
set PACKAGE_NAME=%GAME_NAME%-%VERSION%-windows-x64
set PACKAGE_DIR=dist\windows\%PACKAGE_NAME%

echo === Sobirayu %GAME_NAME% v%VERSION% pod Windows ===

cmake -B %BUILD_DIR% -DCMAKE_BUILD_TYPE=Release -DGAME_NAME=%GAME_NAME%
if errorlevel 1 goto :error

cmake --build %BUILD_DIR% --config Release
if errorlevel 1 goto :error

echo === Upakovyvayu ===
if exist "%PACKAGE_DIR%" rmdir /s /q "%PACKAGE_DIR%"
mkdir "%PACKAGE_DIR%"

REM Visual Studio кладёт .exe в подпапку Release, MinGW - прямо в build_dir
if exist "%BUILD_DIR%\Release\%GAME_NAME%.exe" (
    copy "%BUILD_DIR%\Release\%GAME_NAME%.exe" "%PACKAGE_DIR%\"
) else (
    copy "%BUILD_DIR%\%GAME_NAME%.exe" "%PACKAGE_DIR%\"
)
xcopy /e /i /q assets "%PACKAGE_DIR%\assets"

powershell -Command "Compress-Archive -Path '%PACKAGE_DIR%' -DestinationPath 'dist\windows\%PACKAGE_NAME%.zip' -Force"

echo.
echo Gotovo: dist\windows\%PACKAGE_NAME%.zip
goto :eof

:error
echo.
echo Sborka zavershilas s oshibkoy.
exit /b 1
