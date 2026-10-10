@echo off
rem Builds the game and the dedicated server with MinGW-w64 g++ (same as the Makefile).
rem If raylib isn't on g++'s default paths, add  -I C:\raylib\include  and  -L C:\raylib\lib  below.
rem For testers: copy the DLLs listed by  "ldd freehauler.exe | grep ucrt64"  (MSYS2 shell; e.g. libstdc++-6.dll, libgcc_s_seh-1.dll, libwinpthread-1.dll, glfw3.dll)
rem from C:\msys64\ucrt64\bin into the folder with freehauler.exe.

set SIM=game.cpp galaxy.cpp ship.cpp asteroids.cpp economy.cpp comms.cpp commands.cpp format.cpp net.cpp protocol.cpp

echo Building freehauler.exe ...
g++ -std=c++17 -O2 -Wall %SIM% main.cpp client.cpp server.cpp server_console.cpp process.cpp targetinfo.cpp save.cpp settings.cpp input.cpp ^
    render.cpp hud.cpp ui.cpp menus.cpp tests.cpp -o freehauler.exe ^
    -lraylib -lopengl32 -lgdi32 -lwinmm -lws2_32
if errorlevel 1 goto :fail

echo Building freehauler_server.exe ...
g++ -std=c++17 -O2 -Wall %SIM% server.cpp server_console.cpp process.cpp server_main.cpp headless_raylib.cpp -o freehauler_server.exe ^
    -lws2_32 -lwinmm
if errorlevel 1 goto :fail

echo Done.
goto :eof

:fail
echo Build failed.
exit /b 1
