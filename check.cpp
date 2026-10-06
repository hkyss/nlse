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

std::optional<std::string> readFile(const std::wstring& path)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }
    LARGE_INTEGER size{};
    GetFileSizeEx(file, &size);
    std::string bytes(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    const BOOL ok = ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr);
    CloseHandle(file);
    if (!ok) {
        return std::nullopt;
    }
    bytes.resize(read);
    return bytes;
}

void createDirectories(const std::wstring& path)
{
    if (path.empty() || GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return;
    }
    createDirectories(parentOf(path));
    CreateDirectoryW(path.c_str(), nullptr);
}

bool writeFile(const std::wstring& path, const std::string& bytes)
{
    createDirectories(parentOf(path));
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    const BOOL ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    CloseHandle(file);
    return ok && written == bytes.size();
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
    const auto* code = reinterpret_cast<const BYTE*>(&checkVersions);
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

int wmain()
{
    checkVersions();
    checkPatterns();
    checkHooks();
    checkPlugins();
    if (failures) {
        wprintf(L"%d checks failed\n", failures);
        return 1;
    }
    wprintf(L"all checks passed\n");
    return 0;
}
