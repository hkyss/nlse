#include "nlse.cpp"

#include <cstdio>

namespace {

int failures = 0;

void expect(bool condition, const wchar_t* what)
{
    if (!condition) {
        wprintf(L"FAIL %s\n", what);
        ++failures;
    }
}

std::optional<Json> parseText(const std::string& text, std::string& error)
{
    return Parser(text).parse(error);
}

void checkMerge()
{
    std::string error;
    std::optional<Json> target = parseText(R"({"a": 1, "b": {"c": 2, "d": [1, 2],}, "e": "x",})", error);
    const std::optional<Json> patch = parseText(R"({"a": 5, "b": {"d": [3], "new": true}, "e": null, /* x */})", error);
    expect(target && patch, L"trailing commas and comments parse");
    if (!target || !patch) {
        return;
    }
    std::vector<Change> changes;
    merge(*target, *patch, std::string(), changes);
    expect(compact(*target) == R"({"a": 5, "b": {"c": 2, "d": [3], "new": true}})", L"merge result");
    expect(changes.size() == 4, L"four changes reported");

    std::optional<Json> duplicated = parseText(R"({"k": 1, "k": 2})", error);
    const std::optional<Json> override = parseText(R"({"k": 3})", error);
    changes.clear();
    merge(*duplicated, *override, std::string(), changes);
    expect(compact(*duplicated) == R"({"k": 1, "k": 3})", L"a duplicated key changes where the game reads it, the last one");

    const std::optional<Json> unchanged = parseText(R"({"a": 5})", error);
    changes.clear();
    merge(*target, *unchanged, std::string(), changes);
    expect(changes.empty(), L"an equal value is not reported as a change");

    expect(!parseText(R"({"a": 1 "b": 2})", error), L"a missing comma is an error");
    expect(error.find("line 1, column 9") != std::string::npos, L"the error points at the missing comma");
}

void checkNames()
{
    expect(wildcard(L"new.json", L"*.json"), L"*.json matches a JSON file");
    expect(!wildcard(L"main.csv", L"*.json"), L"*.json does not match a CSV file");
    expect(!wildcard(L"economic", L"*.json"), L"*.json does not match a folder");
    expect(wildcard(L"abc", L"a?c"), L"? matches one character");
    expect(wildcard(L"any.name.at.all", L"*"), L"* matches everything");
    expect(isModDocument(L"mod.json") && isModDocument(L"README.md") && isModDocument(L"notes.txt"),
           L"mod.json and documents at the mod root are not game files");
    expect(!isModDocument(L"knowledge\\notes.txt") && !isModDocument(L"debug_params.json"),
           L"deeper files and game files are not documents");
}

void removeTree(const std::wstring& folder)
{
    for (const auto& name : listDirectory(folder, true)) {
        removeTree(folder + L"\\" + name);
    }
    for (const auto& name : listDirectory(folder, false)) {
        DeleteFileW((folder + L"\\" + name).c_str());
    }
    RemoveDirectoryW(folder.c_str());
}

std::set<std::wstring> listed(const std::wstring& pattern, bool extended)
{
    std::set<std::wstring> names;
    WIN32_FIND_DATAW data{};
    HANDLE handle = extended ? hookedFindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data,
                                                       FindExSearchNameMatch, nullptr, 0)
                             : hookedFindFirstFileW(pattern.c_str(), &data);
    if (handle == INVALID_HANDLE_VALUE) {
        return names;
    }
    do {
        const std::wstring name = data.cFileName;
        if (name != L"." && name != L"..") {
            names.insert(name);
        }
    } while (hookedFindNextFileW(handle, &data));
    expect(GetLastError() == ERROR_NO_MORE_FILES, L"a listing ends with ERROR_NO_MORE_FILES");
    hookedFindClose(handle);
    return names;
}

void checkGameFolder()
{
    wchar_t temp[MAX_PATH + 1] = {};
    GetTempPathW(MAX_PATH + 1, temp);
    const std::wstring root = std::wstring(temp) + L"nlse_check";
    removeTree(root);
    const std::wstring game = root + L"\\game";
    const std::wstring mod = game + L"\\mods\\Test";
    writeFile(game + L"\\Norland.exe", "");
    writeFile(game + L"\\knowledge\\technology\\economic\\vanilla.json", "{\"name\": \"vanilla\", \"tag\": 1,}");
    writeFile(mod + L"\\mod.json", "{\"name\": \"Test\", \"version\": \"1\"}");
    writeFile(mod + L"\\README.md", "not a game file");
    writeFile(mod + L"\\knowledge\\technology\\economic\\vanilla.json", "{\"tag\": 2}");
    writeFile(mod + L"\\knowledge\\technology\\economic\\added.json", "{\"name\": \"added\", // note\n}");
    writeFile(mod + L"\\knowledge\\technology\\fresh\\deep.json", "{\"name\": \"deep\"}");
    writeFile(mod + L"\\sounds\\new.ogg", "not really audio");

    gameDirectory = game;
    gameKey = lower(game);
    gamePrefix = gameKey + L"\\";
    logPath = root + L"\\check.log";
    mergedDirectory = root + L"\\merged";
    originalCreateFileW = &CreateFileW;
    originalGetFileAttributesW = &GetFileAttributesW;
    originalGetFileAttributesA = &GetFileAttributesA;
    originalGetFileAttributesExW = &GetFileAttributesExW;
    originalFindFirstFileW = &FindFirstFileW;
    originalFindFirstFileExW = &FindFirstFileExW;
    originalFindNextFileW = &FindNextFileW;
    originalFindClose = &FindClose;
    loadMods();
    listingsActive = true;

    const std::wstring economic = game + L"\\knowledge\\technology\\economic";
    const std::wstring fresh = game + L"\\knowledge\\technology\\fresh";
    for (const bool extended : {false, true}) {
        expect(listed(economic + L"\\*.json", extended) == std::set<std::wstring>{L"added.json", L"vanilla.json"},
               L"an added file is listed beside the game's own");
        const std::set<std::wstring> technology = listed(game + L"\\knowledge\\technology\\*", extended);
        expect(technology.count(L"economic") && technology.count(L"fresh"), L"an added folder is listed");
        expect(listed(fresh + L"\\*.json", extended) == std::set<std::wstring>{L"deep.json"},
               L"a folder that exists only in a mod lists its files");
        expect(listed(game + L"/knowledge/technology/economic/*.json", extended).size() == 2,
               L"forward slashes find the same listing");
        expect(listed(game + L"\\sounds\\*.ogg", extended) == std::set<std::wstring>{L"new.ogg"},
               L"a file that is not JSON can be added too");
    }
    expect(listed(game + L"\\*.md", false).empty(), L"a README in the mod does not reach the game folder");
    expect(hookedGetFileAttributesW(fresh.c_str()) == FILE_ATTRIBUTE_DIRECTORY, L"an added folder exists");
    expect(hookedGetFileAttributesW((fresh + L"\\").c_str()) == FILE_ATTRIBUTE_DIRECTORY,
           L"an added folder exists with a trailing backslash");
    expect(hookedGetFileAttributesW((economic + L"\\added.json").c_str()) != INVALID_FILE_ATTRIBUTES,
           L"an added file exists");
    expect(hookedGetFileAttributesA(utf8(economic + L"\\added.json").c_str()) != INVALID_FILE_ATTRIBUTES,
           L"an added file exists for the ANSI call too");
    expect(hookedGetFileAttributesW((economic + L"\\missing.json").c_str()) == INVALID_FILE_ATTRIBUTES,
           L"a file nobody added still does not exist");

    const auto readThroughGame = [](const std::wstring& path) {
        std::string bytes;
        HANDLE file = hookedCreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                        FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            return bytes;
        }
        WIN32_FILE_ATTRIBUTE_DATA size{};
        hookedGetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &size);
        bytes.resize(size.nFileSizeLow);
        DWORD read = 0;
        ReadFile(file, bytes.data(), size.nFileSizeLow, &read, nullptr);
        CloseHandle(file);
        bytes.resize(read);
        return bytes;
    };
    std::string error;
    const std::optional<Json> added = parseText(readThroughGame(economic + L"\\added.json"), error);
    expect(added && textField(*added, "name", L"") == L"added", L"the game reads the added file, comments gone");
    const std::optional<Json> patched = parseText(readThroughGame(economic + L"\\vanilla.json"), error);
    expect(patched && compact(*patched) == R"({"name": "vanilla", "tag": 2})",
           L"the game's own file in the same folder is patched, and its size matches");
    removeTree(root);
}

int checkGame(const std::wstring& folder)
{
    if (GetFileAttributesW((folder + L"\\Norland.exe").c_str()) == INVALID_FILE_ATTRIBUTES) {
        wprintf(L"no game at %s, its files were not checked\n", folder.c_str());
        return 0;
    }
    std::vector<std::wstring> files;
    collectFiles(folder, std::wstring(), files);
    int checked = 0;
    for (const auto& relative : files) {
        if (!hasExtension(relative, L".json")) {
            continue;
        }
        ++checked;
        const std::optional<std::string> text = readFile(folder + L"\\" + relative);
        std::string error;
        const std::optional<Json> json = text ? parseText(*text, error) : std::nullopt;
        if (!json) {
            wprintf(L"FAIL %s: %S\n", relative.c_str(), error.c_str());
            ++failures;
            continue;
        }
        std::string pretty;
        writePretty(*json, pretty, 0);
        const std::optional<Json> again = parseText(pretty, error);
        if (!again || compact(*again) != compact(*json)) {
            wprintf(L"FAIL %s changes when written back\n", relative.c_str());
            ++failures;
        }
    }
    return checked;
}

void checkVersions()
{
    expect(versionText(NLSE_GAME_VERSION(0, 5588, 9777, 0)) == L"0.5588.9777.0", L"a game version reads as four numbers");
    expect(fileVersion(GetModuleHandleW(L"kernel32.dll")) != 0, L"a file with a version resource has a version");
    expect(fileVersion(GetModuleHandleW(nullptr)) == 0, L"a file without a version resource has none");

    gameVersion = NLSE_GAME_VERSION(0, 5588, 9777, 0);
    NLSEPluginVersion declared{NLSE_API_VERSION, "Test"};
    expect(refusal(declared).empty(), L"a plugin for any game version loads");
    declared.gameVersions[0] = NLSE_GAME_VERSION(0, 5500, 1, 0);
    declared.gameVersions[1] = NLSE_GAME_VERSION(0, 5588, 9777, 0);
    expect(refusal(declared).empty(), L"a plugin that lists this game version loads");
    declared.gameVersions[1] = 0;
    expect(refusal(declared) == L"it is made for Norland 0.5500.1.0, this is 0.5588.9777.0",
           L"a plugin for another game version is skipped");
    declared.gameVersions[0] = 0;
    declared.apiVersion = NLSE_API_VERSION + 1;
    expect(!refusal(declared).empty(), L"a plugin for a newer NLSE is skipped");
    declared.apiVersion = NLSE_API_VERSION;
    declared.name[0] = '\0';
    expect(!refusal(declared).empty(), L"a plugin without a name is skipped");
}

void checkPatterns()
{
    expect(parsePattern("48 8b ?? 05 ?") == std::vector<int>{0x48, 0x8B, -1, 0x05, -1},
           L"a pattern reads bytes and both kinds of wildcards");
    expect(!parsePattern("4G") && !parsePattern("1 2") && !parsePattern("") && !parsePattern(nullptr),
           L"a broken pattern is refused");
    const BYTE bytes[] = {1, 2, 3, 4, 2, 3, 5};
    const auto find = [&bytes](const char* pattern) {
        const uintptr_t found = findPattern(bytes, bytes + sizeof(bytes), *parsePattern(pattern));
        return found ? static_cast<int>(found - reinterpret_cast<uintptr_t>(bytes)) : -1;
    };
    expect(find("02 ?? 05") == 4, L"a pattern with a wildcard is found");
    expect(find("?? 03") == 1, L"a pattern that starts with a wildcard is found");
    expect(find("03 02") == -1, L"a pattern that is not there is not found");
    expect(find("03 05 ??") == -1, L"a pattern does not run past the end");

    gameBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto* code = reinterpret_cast<const BYTE*>(&checkMerge);
    std::string exact;
    std::string loose = "?? ??";
    for (int i = 0; i < 24; ++i) {
        char hex[4];
        sprintf_s(hex, "%02X ", code[i]);
        exact += hex;
        loose += i < 2 ? "" : std::string(" ") + hex;
    }
    const auto* found = reinterpret_cast<const BYTE*>(findInGame(exact.c_str()));
    expect(found && std::memcmp(found, code, 24) == 0, L"FindPattern finds the game's code");
    found = reinterpret_cast<const BYTE*>(findInGame(loose.c_str()));
    expect(found && std::memcmp(found + 2, code + 2, 22) == 0, L"FindPattern finds the game's code with wildcards");
    expect(!findInGame("ZZ"), L"FindPattern returns nothing for a broken pattern");
}

using NumberFunction = int (*)();
NumberFunction originalNumber;

int plusOne()
{
    return originalNumber() + 1;
}

int three()
{
    return 3;
}

DWORD WINAPI fakeProcessId()
{
    return 7;
}

void checkHooks()
{
    gameBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    BYTE* code = allocateNear(gameBase, 0x1000);
    expect(code && reaches(gameBase, reinterpret_cast<uintptr_t>(code)), L"memory is found near the game");
    if (!code) {
        return;
    }
    const BYTE caller[] = {0x48, 0x83, 0xEC, 0x28, 0xE8, 0x07, 0x00, 0x00, 0x00, 0x48, 0x83,
                           0xC4, 0x28, 0xC3, 0xCC, 0xCC, 0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3};
    std::memcpy(code, caller, sizeof(caller));
    const auto number = reinterpret_cast<NumberFunction>(code);
    const uintptr_t site = reinterpret_cast<uintptr_t>(code + 4);
    const uintptr_t one = reinterpret_cast<uintptr_t>(code + 16);
    expect(number() == 1, L"the test code runs");
    originalNumber = reinterpret_cast<NumberFunction>(writeCall(site, reinterpret_cast<void*>(&plusOne)));
    expect(reinterpret_cast<uintptr_t>(originalNumber) == one, L"WriteCall returns where the call went");
    expect(number() == 2, L"a call goes to the hook, and the hook calls the original");
    expect(!writeCall(one, reinterpret_cast<void*>(&plusOne)), L"WriteCall leaves a site without a call alone");
    expect(writeJump(one, reinterpret_cast<void*>(&three)) && number() == 4, L"WriteJump replaces a whole function");

    void* original = nullptr;
    expect(hookImport("kernel32.dll", "GetCurrentProcessId", reinterpret_cast<void*>(&fakeProcessId), &original) &&
               GetCurrentProcessId() == 7,
           L"HookImport changes what the game calls");
    hookImport("kernel32.dll", "GetCurrentProcessId", original, &original);
    expect(GetCurrentProcessId() != 7, L"HookImport puts the original back");
    expect(!hookImport("kernel32.dll", "NoSuchFunction", reinterpret_cast<void*>(&fakeProcessId), &original),
           L"HookImport reports a function the game does not import");
}

std::vector<std::string> received;

void remember(const NLSEMessage* message)
{
    std::string text = std::string(message->sender) + " " + std::to_string(message->type);
    if (message->data) {
        text += " " + std::string(static_cast<const char*>(message->data), message->size);
    }
    received.push_back(text);
}

NLSEPluginHandle addFakePlugin(const char* name)
{
    auto plugin = std::make_unique<Plugin>();
    plugin->name = name;
    plugin->loaded = true;
    plugin->info = {static_cast<NLSEPluginHandle>(plugins.size() + 1), plugin->name.c_str(), "", ""};
    plugins.push_back(std::move(plugin));
    return static_cast<NLSEPluginHandle>(plugins.size());
}

void checkPlugins()
{
    wchar_t temp[MAX_PATH + 1] = {};
    GetTempPathW(MAX_PATH + 1, temp);
    const std::wstring root = std::wstring(temp) + L"nlse_check_plugins";
    removeTree(root);
    const std::wstring game = root + L"\\game";
    const std::wstring folder = game + L"\\mods\\NLSE\\Plugins";
    createDirectories(game);
    gameDirectory = game;
    logPath = root + L"\\check.log";

    BYTE* start = allocateNear(gameBase, 0x1000);
    if (!start) {
        expect(false, L"memory is found near the game");
        return;
    }
    const BYTE entry[16] = {0x89, 0xC8, 0xC3, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
                            0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC};
    std::memcpy(start, entry, sizeof(entry));
    expect(hookEntry(reinterpret_cast<uintptr_t>(start)) && start[0] == 0xFF, L"the game start jumps to NLSE");
    expect(reinterpret_cast<EntryFunction>(start)(reinterpret_cast<void*>(42)) == 42,
           L"the game starts after the plugins, with its own argument");
    expect(std::memcmp(start, entry, sizeof(entry)) == 0, L"the game start is put back as it was");
    expect(readFile(logPath).value_or("").find("no plugins found") != std::string::npos,
           L"an empty plugin folder is reported");

    writeFile(folder + L"\\Broken.dll", "not a DLL");
    const std::wstring example = parentOf(modulePath(nullptr)) + L"\\example_plugin.dll";
    expect(CopyFileW(example.c_str(), (folder + L"\\Example.dll").c_str(), FALSE) != 0,
           L"the example plugin is built beside check.exe");
    const NLSEPluginHandle observer = addFakePlugin("Observer");
    expect(addListener(observer, nullptr, remember), L"a plugin listens to everyone");
    loadPlugins();
    std::string text = readFile(logPath).value_or("");
    expect(text.find("plugin Example 1.0") != std::string::npos, L"the example plugin loads");
    expect(text.find("Example: loaded into Norland 0.5588.9777.0") != std::string::npos, L"a plugin writes to the log");
    expect(text.find("Example: every plugin is loaded") != std::string::npos, L"a plugin hears that all are loaded");
    expect(text.find("Broken.dll cannot be loaded") != std::string::npos, L"a file that is not a DLL is reported");
    expect(received == std::vector<std::string>{"NLSE 1", "NLSE 2"}, L"NLSE sends both messages, in order");
    expect(currentPlugin() == 0, L"no plugin is loading after the load");

    const NLSEPluginInfo* info = pluginInfo("Example");
    expect(info && std::string(info->version) == "1.0" && std::string(info->author) == "hkyss",
           L"a plugin finds another by name");
    expect(!pluginInfo("Nobody"), L"a plugin nobody loaded is not found");

    received.clear();
    const NLSEPluginHandle sender = addFakePlugin("Sender");
    expect(dispatch(sender, 7, "hi", 2, "Observer") && received == std::vector<std::string>{"Sender 7 hi"},
           L"a plugin sends a message to another");
    expect(!dispatch(sender, 7, "hi", 2, "Example"), L"a message reaches only plugins listening to its sender");
    expect(!dispatch(0, 7, nullptr, 0, nullptr) && !dispatch(99, 7, nullptr, 0, nullptr),
           L"an unknown plugin cannot send");
    expect(!addListener(99, nullptr, remember), L"an unknown plugin cannot listen");

    CopyFileW(example.c_str(), (folder + L"\\Example2.dll").c_str(), FALSE);
    loadPlugin(folder + L"\\Example2.dll", L"Example2.dll");
    text = readFile(logPath).value_or("");
    expect(text.find("Example2.dll skipped, a plugin named Example is already loaded") != std::string::npos,
           L"a second plugin with the same name is skipped");

    listeners.clear();
    for (const auto& plugin : plugins) {
        if (plugin->module) {
            FreeLibrary(plugin->module);
        }
    }
    plugins.clear();
    removeTree(root);
}

}

int wmain(int argc, wchar_t** argv)
{
    checkMerge();
    checkNames();
    const std::wstring game =
        argc > 1 ? argv[1] : L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Norland Story Generating Strategy";
    const int files = checkGame(game);
    checkGameFolder();
    checkVersions();
    checkPatterns();
    checkHooks();
    checkPlugins();
    if (failures) {
        wprintf(L"%d checks failed\n", failures);
        return 1;
    }
    wprintf(L"all checks passed, %d game JSON files read and written back\n", files);
    return 0;
}
