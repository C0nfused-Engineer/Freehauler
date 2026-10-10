# Multiplayer

## Hosting from the game

**Main menu → Multiplayer → Host a Server.** Fill in your captain name, port (default 23500), an optional password and max players, then press **Start Server**.

- **A real dedicated server.** The game starts *itself* again as a dedicated server (`freehauler --server ...`) in its own console window, and you join it like any other player. The console shows who joins, leaves and saves.
- **Stop the server when I leave** is ON by default. Leaving (Esc → Leave Server, or exiting the game) then closes the server for everyone, after saving. Turn it OFF to leave it running for your testers after you quit. Close its window or press Ctrl+C in it to stop it later; it saves on the way out.
- **Saves.** The galaxy and captains go in `server_data/` next to the game.
- **Addresses.** The Host page shows the address players on your network should use. The in-game status line shows it too: `HOSTING 192.168.1.20:23500`.
- **Port already taken.** If a server from earlier is still running on that port, the new one can't start and the game says so. Use Join with `127.0.0.1` instead.

## Joining

**Main menu → Multiplayer → Join a Server.** Fill in the address (`192.168.1.20`, or `myserver.net:23500` for another port), your captain name and the password if there is one. The game remembers your name and the last address.

The name *is* your save: join with the same name to carry on where you left off.

Skipping the menu also works: `freehauler --connect 203.0.113.7 --name Holden [--password pw]`. `play_online_example.bat` is a double-click starter for testers.

## The standalone server (console or VPS)

```
freehauler_server                         # UDP port 23500, no password
freehauler_server --password hunter2 --port 23500 --max-players 8
freehauler --server --port 23500          # the same, using the game's executable
```

`freehauler_server` has no window and doesn't use raylib, so it also builds and runs on a Linux VPS with no graphics. The server keeps everything in `server_data/`:

- `world.txt` holds the seed, the clock and the mined rocks.
- `players/<name>.txt` holds each captain's ship, fuel, credits and cargo.

It saves when a player leaves, every minute, and when stopped. Delete `world.txt` to start a fresh galaxy; `--seed N` picks the new one's seed.

## Letting testers in over the internet

Pick one:

- **Port forward.** Allow the game (or `freehauler_server.exe`) through Windows Firewall when Windows asks; tick Private *and* Public. Then forward **UDP** 23500 on your router to your PC's local IP. Testers connect to your public IP.
- **Tailscale or ZeroTier (no router changes).** Everyone joins your network, then testers connect to your machine's Tailscale address (100.x.y.z). This is the easiest option for a small closed group.
- **A VPS.** Build with `make server` on the box, open UDP 23500 in its firewall, and run it under `screen`/`tmux` or as a systemd service.

Testing alone? `freehauler --bot 127.0.0.1 --name Rocinante --idle` parks a headless ship at the start for you to fly up to. Without `--idle` it flies loops.

## In the game

- **Other captains.** Each one is a colored ship with a name tag, visible at any distance. Put the reticle on one and hold Shift to lock on and hyper/warp to them. MMB shows their speed, drive and nearest star.
- **The world doesn't stop.** Esc opens a menu with Resume, Settings, Leave Server (back to the main menu) and Exit to Desktop, and everyone keeps flying. F5/F9 do nothing; the server saves for you.
- **The status line** at the top shows the server, the number of captains and your ping.
- **Mining** is shared. A rock someone mines out disappears for everyone.

## How it works (for when you add features)

```
client                                server (authoritative, 120 Hz)
------                                ------------------------------
reads mouse/keys -> Controls + Aim
StepCaptain(predict) immediately      
remember (step#, input, result)  ---> inputs (each sent several times)
                                      StepCaptain(full) with the SAME inputs, in order
                                      mining, comms, commands applied here
correct + replay if it differs   <--- snapshot (30 Hz): your state after step N,
draw others 0.1 s in the past,         everyone else's ships
smoothly between snapshots       <--- reliable: welcome, events, comms log,
                                      mined rocks, joins/leaves
```

- **Prediction normally matches the server exactly.** The client and server run the same `StepCaptain` with the same inputs in the same order. The server doesn't step your captain until your input arrives, so a late packet only delays the server and never makes it guess. In testing, a Windows client against a Linux server flew 3,000 steps with zero corrections. Corrections happen when the server changes something you couldn't predict, e.g. another player mines away the rock you were about to bump.
- **Commands** (sell, buy, hail) carry the input step they were issued after. The server applies them at that same point in your flight, so "am I close enough to trade?" gets the same answer on both sides.
- **The protocol** is plain UDP with a small reliable-ordered channel on top (`protocol.h`). Bump `PROTOCOL_VERSION` whenever a message format changes; old clients are then ignored instead of misreading packets.

### Rules of thumb for new features

| You're adding... | Put it... |
|---|---|
| Per-player state (reputation, missions, ship type) | in `Captain` (and `Player` if it's saved), plus the server's player file |
| Something a player *does* (accept mission, fire weapon...) | a new `Command` type: `commands.h` / `ApplyCommand` |
| Something everyone shares (prices with supply/demand, NPC ships) | in `World`; the server sends changes, like `MSG_MINED` |
| Anything that moves your own ship | in `StepCaptain` (both sides run it), with only inputs and synced data as inputs, never `GetRandomValue` or wall-clock time |
| Something the server decides that you see in the HUD | in the `Snapshot` (small, every 30 Hz) or a reliable message (rare or big) |

NPC ships fit the same pattern: the server steps them with `UpdateShip` and an AI that fills in `Controls`, and adds them to `traffic`.

## Limits worth knowing

- **Trust-based identity.** The optional password keeps strangers out, but anyone who has it can join under any name and get that captain. That's fine for a test group; real accounts would be a later step.
- **Admin key.** A server started from the game accepts a "stop" request only from the client holding a random key it was launched with, i.e. the host's game.
- **IPv4 only, unencrypted.**
- **14 nearest ships per snapshot.** Each snapshot carries the 14 nearest other ships, to fit one packet. Farther ships beyond that drop out until they're among the nearest.
- **Separate saves.** Single-player saves and server captains are kept apart.
