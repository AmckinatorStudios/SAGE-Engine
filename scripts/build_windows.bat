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

REM Imya igrovogo targeta zadaotsya v games\<name>\CMakeLists.txt (sage_add_game),
REM poetomu -DGAME_NAME bolshe ne nuzhen - sobiraem nuzhnyy target po imeni.
cmake -B %BUILD_DIR% -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 goto :error

cmake --build %BUILD_DIR% --config Release --target %GAME_NAME%
if errorlevel 1 goto :error

echo === Upakovyvayu ===
if exist "%PACKAGE_DIR%" rmdir /s /q "%PACKAGE_DIR%"
mkdir "%PACKAGE_DIR%"

REM Binarnik igry lezhit v build\games\<name>\ (VS - v podpapke Release);
REM ryadom s nim lezhat i assets (ih kladot POST_BUILD-shag sage_add_game).
set "EXE="
for /r "%BUILD_DIR%" %%F in (%GAME_NAME%.exe) do set "EXE=%%F"
if not defined EXE (
    echo Oshibka: ne nayden sobrannyy %GAME_NAME%.exe v %BUILD_DIR%
    goto :error
)
for %%F in ("%EXE%") do set "EXE_DIR=%%~dpF"
copy "%EXE%" "%PACKAGE_DIR%\"
xcopy /e /i /q "%EXE_DIR%assets" "%PACKAGE_DIR%\assets"

powershell -Command "Compress-Archive -Path '%PACKAGE_DIR%' -DestinationPath 'dist\windows\%PACKAGE_NAME%.zip' -Force"

echo.
echo Gotovo: dist\windows\%PACKAGE_NAME%.zip
goto :eof

:error
echo.
echo Sborka zavershilas s oshibkoy.
exit /b 1
