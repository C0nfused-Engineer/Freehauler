# Freehauler - builds the game and the dedicated server.
#
#   make            both
#   make game       freehauler(.exe)          the game (it can also host: Multiplayer > Host a Server)
#   make server     freehauler_server(.exe)   the dedicated server on its own (no window, no raylib needed)
#   make test       builds the game and runs the headless tests
#
# Windows: run from a MinGW-w64 shell as "mingw32-make" (or use build_windows.bat).
# If raylib isn't on the compiler's default paths, point at it:
#   make RAYLIB_INC=C:/raylib/include RAYLIB_LIB=C:/raylib/lib

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall
RAYLIB_INC ?=
RAYLIB_LIB ?=
INC = $(if $(RAYLIB_INC),-isystem $(RAYLIB_INC),)
LIB = $(if $(RAYLIB_LIB),-L$(RAYLIB_LIB),)

# The game rules: shared by the game and the server
SIM = game.cpp galaxy.cpp ship.cpp asteroids.cpp economy.cpp comms.cpp commands.cpp format.cpp
NET = net.cpp protocol.cpp
GAME_SRC   = $(SIM) $(NET) main.cpp client.cpp server.cpp server_console.cpp process.cpp targetinfo.cpp save.cpp \
             settings.cpp input.cpp render.cpp hud.cpp ui.cpp map.cpp menus.cpp tests.cpp
SERVER_SRC = $(SIM) $(NET) server.cpp server_console.cpp process.cpp server_main.cpp headless_raylib.cpp

ifeq ($(OS),Windows_NT)
    EXE         = .exe
    # Linked against MSYS2's DLLs. To hand the game to testers, copy the DLLs that
    # "ldd freehauler.exe | grep ucrt64" lists (libstdc++-6, libgcc_s_seh-1,
    # libwinpthread-1, glfw3...) from C:/msys64/ucrt64/bin next to the exe.
    GAME_LIBS   = -lraylib -lopengl32 -lgdi32 -lwinmm -lws2_32
    SERVER_LIBS = -lws2_32 -lwinmm
else
    EXE         =
    GAME_LIBS   = -lraylib -lGL -lm -lpthread -ldl -lrt -lX11
    SERVER_LIBS = -lm
endif

all: game server

game: freehauler$(EXE)
server: freehauler_server$(EXE)

freehauler$(EXE): $(GAME_SRC) *.h
	$(CXX) $(CXXFLAGS) $(INC) $(GAME_SRC) -o $@ $(LIB) $(GAME_LIBS)

freehauler_server$(EXE): $(SERVER_SRC) *.h
	$(CXX) $(CXXFLAGS) $(INC) $(SERVER_SRC) -o $@ $(SERVER_LIBS)

test: freehauler$(EXE)
	./freehauler$(EXE) --test

clean:
	rm -f freehauler$(EXE) freehauler_server$(EXE)

.PHONY: all game server test clean
