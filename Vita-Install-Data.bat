@echo off
rem Puts game data on the Vita memory card: drag a data archive (.rar/.zip/.7z)
rem or the extracted data folder onto this file. Start USB mode in VitaShell first.
cd /d "%~dp0"
set "SRC=%~1"
if "%SRC%"=="" (
    echo Drag the game data archive or folder onto this file,
    echo or paste its path here and press Enter.
    set /p "SRC=Data: "
)
if "%SRC%"=="" exit /b 1
python tools\vita_install_data.py "%SRC%" %2
echo.
pause
