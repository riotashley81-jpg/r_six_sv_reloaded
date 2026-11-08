@echo off
rem Launches Rainbow Six: Shadow Vanguard. Extra arguments are passed through,
rem e.g.  R6SV.bat --console
cd /d "%~dp0"
rem r6sv.exe next to this file (release download) or in build\Release (own build)
set "EXE=%~dp0r6sv.exe"
if not exist "%EXE%" set "EXE=%~dp0build\Release\r6sv.exe"
if not exist "%EXE%" (
    echo r6sv.exe not found - download a release or build the port first.
    pause
    exit /b 1
)
if not exist "apk\xperia-play\lib\armeabi-v7a\librainbowsix.so" (
    echo Game files missing: extract the game's APK to apk\xperia-play
    echo and put its data folder at gamedata\GloftR6HP - see README.
    pause
    exit /b 1
)
"%EXE%" apk\xperia-play gamedata\GloftR6HP %*
if errorlevel 1 (
    echo.
    echo The game exited with an error. See the messages above.
    pause
)
