@echo off
rem Launches Rainbow Six: Shadow Vanguard. Extra arguments are passed through,
rem e.g.  R6SV.bat --console
cd /d "%~dp0"
if not exist "build\Release\r6sv.exe" (
    echo build\Release\r6sv.exe not found - build the port first.
    pause
    exit /b 1
)
"build\Release\r6sv.exe" apk\xperia-play gamedata\GloftR6HP %*
if errorlevel 1 (
    echo.
    echo The game exited with an error. See the messages above.
    pause
)
