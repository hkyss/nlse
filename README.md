# NLSE

Norland Script Extender. Loads plugins into Norland without changing the game's files.

## Install

Download the files from the [latest release](https://github.com/hkyss/nlse/releases/latest).

With Mod Organizer 2: extract `NLSE-MO2-plugin-<version>.zip` into the folder with `ModOrganizer.exe`, create an instance for Norland and install `NLSE-<version>.zip` as a mod.

Without MO2: extract `NLSE-manual-<version>.zip` into the folder with `Norland.exe`. To turn NLSE off, delete `winmm.dll`.

## Making a plugin

A plugin is a DLL in `mods\NLSE\Plugins`. It includes [nlse.h](nlse.h) and exports:

    NLSE_EXPORT const NLSEPluginVersion NLSEPlugin_Version = {NLSE_API_VERSION, "Example", "1.0", "you"};
    NLSE_EXPORT bool NLSEPlugin_Load(const NLSEInterface* nlse);

Example: [example_plugin.cpp](example_plugin.cpp), build with `cl /O2 /LD example_plugin.cpp`.

## Log

`%LOCALAPPDATA%\Strategy\nlse.log` lists every plugin and what it writes there.

## Building

Visual Studio 2022 Build Tools with C++, then `make.bat`. It builds `nlse.dll` and `winmm.dll` and runs the checks.

## License

MIT
