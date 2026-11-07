# Race Broadcast (F1 25)

Watch races in F1 25 as if you were watching on TV.

An F1 25 addon for the Swofty Launcher that lets you watch AI races like a TV broadcast: the AI drives your car, the race carries on after a crash, an auto director follows the action, and a four-voice commentary team calls it. It can also drive the Project Aeternum overlays as the broadcast graphics.

This repository is the full source of the addon published in Swofty's addon browser as **Race Broadcast** (`apoc.race-broadcast`), so anyone can see exactly what it does on their computer.

## What it does on your computer

The addon is a native DLL that Swofty loads into F1 25, plus a PowerShell script for the voices.

**Inside the game** (`native/src`)
- Uses Swofty's own APIs for the spectator camera, AI control of your car, the overlay, settings and session events.
- Wraps two game functions through Swofty's mixin system: `Ne_Post_BusMessage` (to keep the race going when your car is wrecked) and `Game_Pitlane_SetVehicleStatus` (to send a wreck to the garage). See `race_broadcast.c`.
- Scans the game's code for a few byte patterns (`g_api->scan`) to find the same kind of internals: the terminal damage message, the spectator HUD and the replay system (`hud_fix.c`, `replay_try.c`).
- Reads the game's own UDP telemetry on `127.0.0.1` (the port in your F1 25 telemetry settings; 20777, or 20778 when Project Aeternum is running and passes it on).

**Outside the game**
- Presses keys with `SendInput`: the camera and replay keys in the game, and the Project Aeternum hotkeys (F13 to F24 with modifiers, keys no keyboard has) when Aeternum graphics are on (`gfx.c`).
- Finds the Project Aeternum windows by title, and reads and writes Aeternum's settings in `%APPDATA%\ProjectAeternum` when its options are on.
- Starts `addon/commentary/race_broadcast_commentary.ps1` in PowerShell for the commentary (`commentary_launch.c`) and talks to it through a file in the addon's storage folder.
- The commentary script plays the recorded voices from `voices.zip`. Only if you choose the Piper voices does it download Piper and its voice files, from GitHub (rhasspy/piper) and Hugging Face (rhasspy/piper-voices).

Nothing is sent anywhere else: no accounts, no analytics, no other network use.

## Layout

| Path | What it is |
|---|---|
| `native/src/race_broadcast.c` | The addon: telemetry, stories, the director, AI drive, keep race, settings |
| `native/src/gfx.c` | The graphics director for Project Aeternum |
| `native/src/commentary.c`, `cm_round2.c` | The commentary brain: what is said, when, and by whom |
| `native/src/cm_phrases.h`, `cm_phrases2.h` | The commentary lines (each one matches a recorded clip) |
| `native/src/replays*.c`, `hud_fix.c` | Replays and the spectator HUD |
| `native/include/swofty_api.h` | Swofty's addon API header (MIT) |
| `native/lib/*.def` | The Windows functions the DLL imports |
| `addon/` | Everything else in the package: `swofty.json`, the addon README, the commentary script and settings |

The recorded voices (`addon/commentary/voices.zip`, about 70 MB of MP3s) are not in the repository; they are in the published package.

## Building

Needs clang and lld (LLVM 15 or later), on Linux, macOS or Windows (use Git Bash or WSL for the script):

```
sh build.sh
```

The build is reproducible: `build/race_broadcast.dll` is byte-for-byte the same as `native/race_broadcast.dll` in the published package of the same version, so you can check the two match.

To make a package: copy `addon/` to a folder, add `native/race_broadcast.dll` and `commentary/voices.zip` (from the published package), and zip it as `apoc.race-broadcast-<version>.swoftyaddon`.

## Credits and licence

- AI drive and keep race started from **Spectator/Broadcast Mode** by Kizi and RK (MIT); its licence is in `addon/LICENSE.kizi-spectator-broadcast-mode`.
- The Project Aeternum graphics need Project Aeternum, a separate app by its own creator; this addon only drives it.
- Race Broadcast is MIT licensed: see `LICENSE`.
