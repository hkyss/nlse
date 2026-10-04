import mobase
from PyQt6.QtCore import QDir

from ..basic_game import BasicGame


class NorlandGame(BasicGame):
    Name = "Norland Support Plugin"
    Author = "hkyss"
    Version = "0.3.0"

    GameName = "Norland"
    GameShortName = "norland"
    GameNexusName = "norland"
    GameNexusId = 10369
    GameSteamId = 1857090
    GameBinary = "Norland.exe"
    GameDataPath = "mods"
    GameDocumentsDirectory = "%USERPROFILE%/AppData/Local/Strategy"
    GameSavesDirectory = "%GAME_DOCUMENTS%/saves"
    GameSaveExtension = "norland"

    def executableForcedLoads(self) -> list[mobase.ExecutableForcedLoadSetting]:
        loader = QDir.toNativeSeparators(self.dataDirectory().absoluteFilePath("nlse.dll"))
        return [mobase.ExecutableForcedLoadSetting(self.binaryName(), loader).withEnabled(True)]
