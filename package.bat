@echo off
rem package.bat - builds dist\Freehauler.zip, the copy of the game to send to testers.
rem
rem Needs MSYS2 (ucrt64) with raylib. Set MSYS2_UCRT64 if MSYS2 isn't in C:\msys64.
rem
rem The game is built again here, linked statically, so the only DLL it needs
rem beside it is glfw3.dll (MSYS2's raylib is built against the glfw DLL, so that one
rem can't be linked in). Keep the source list in step with GAME_SRC in the Makefile.

setlocal
cd /d "%~dp0"
if "%MSYS2_UCRT64%"=="" set "MSYS2_UCRT64=C:\msys64\ucrt64"
set "PATH=%MSYS2_UCRT64%\bin;%PATH%"

where g++ >nul 2>nul || (echo g++ not found in %MSYS2_UCRT64%\bin & exit /b 1)

set SRC=game.cpp galaxy.cpp factions.cpp contracts.cpp npcs.cpp sensors.cpp shipparts.cpp shipyard.cpp ship.cpp asteroids.cpp economy.cpp comms.cpp commands.cpp format.cpp ^
 net.cpp protocol.cpp main.cpp client.cpp server.cpp server_console.cpp process.cpp targetinfo.cpp save.cpp ^
 settings.cpp input.cpp render.cpp hud.cpp ui.cpp map.cpp factionui.cpp tradeui.cpp contractsui.cpp shipyardui.cpp menus.cpp tests.cpp

if exist dist\Freehauler rmdir /s /q dist\Freehauler
if exist dist\Freehauler.zip del dist\Freehauler.zip
mkdir dist\Freehauler

echo Building the game (a couple of minutes)...
g++ -std=c++17 -O2 -Wall -static -s %SRC% -o dist\Freehauler\freehauler.exe ^
    -lraylib "%MSYS2_UCRT64%\lib\libglfw3.dll.a" -lopengl32 -lgdi32 -lwinmm -lws2_32 || exit /b 1
copy /y "%MSYS2_UCRT64%\bin\glfw3.dll" dist\Freehauler\ >nul || exit /b 1

(
echo Freehauler - test build
echo.
echo Unzip the whole Freehauler folder somewhere ^(Desktop or Documents is fine^), then run freehauler.exe.
echo Don't run it from inside the zip: it needs glfw3.dll beside it and writes its saves there.
echo.
echo If Windows shows "Windows protected your PC", click More info, then Run anyway.
echo To host a multiplayer game, allow it through the firewall when Windows asks.
) > dist\Freehauler\README.txt

powershell -NoProfile -Command "Compress-Archive -Path dist\Freehauler -DestinationPath dist\Freehauler.zip" || exit /b 1
echo.
echo Done: dist\Freehauler.zip
