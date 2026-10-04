# NLSE

Norland Script Extender. Loads mods for Norland without changing the game's files.

A mod changes only the values it needs, so two mods can edit the same file and a game update does not wipe them.

## Install

With Mod Organizer 2: copy `game_norland.py` to `MO2\plugins\basic_games\games`, create an instance for Norland and install NLSE as a mod.

Without MO2: put `winmm.dll` next to `Norland.exe` and create a `mods` folder beside it. To turn NLSE off, delete `winmm.dll`.

## Making a mod

A mod is a folder in `mods` with a `mod.json`:

    {"name": "Cheap Huts", "version": "1.0", "author": "you"}

The rest of the folder mirrors the game folder:

- a JSON file the game has: write only the keys you change, `null` removes a key
- a file the game does not have: it is added
- any other file: it replaces the game's

`mods\Cheap Huts\debug_params.json` that makes a hut cost 1 wood:

    {"building_resources": {"hut_8x8_lvl_1": [["wood", 1]]}}

Mods load in alphabetical order. CSV files are not supported yet.

## Log

`%LOCALAPPDATA%\Strategy\nlse.log` lists every mod and every change it made.

## Building

Visual Studio 2022 Build Tools with C++, then `make.bat`. It builds `nlse.dll` and `winmm.dll` and checks that every JSON file of the installed game reads back unchanged.

## License

MIT
