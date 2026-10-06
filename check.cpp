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

bool savePicture(const std::wstring& path, const Image& image)
{
    std::vector<BYTE> bgra(image.pixels);
    for (size_t i = 0; i < bgra.size(); i += 4) {
        std::swap(bgra[i], bgra[i + 2]);
    }
    ComScope com;
    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    Microsoft::WRL::ComPtr<IWICStream> stream;
    Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder;
    Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> frame;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    return SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
           SUCCEEDED(factory->CreateStream(&stream)) &&
           SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
           SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
           SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
           SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
           SUCCEEDED(frame->SetSize(image.width, image.height)) && SUCCEEDED(frame->SetPixelFormat(&format)) &&
           format == GUID_WICPixelFormat32bppBGRA &&
           SUCCEEDED(frame->WritePixels(image.height, image.width * 4, static_cast<UINT>(bgra.size()), bgra.data())) &&
           SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
}

const BYTE* pixelAt(const Image& image, uint32_t x, uint32_t y)
{
    return &image.pixels[(size_t{y} * image.width + x) * 4];
}

void checkSprites(const std::wstring& gameFolder)
{
    expect(spriteOf(L"sprites\\s_main_menu_bg.png") == std::make_pair(std::string("s_main_menu_bg"), 0u),
           L"a sprite file names the sprite");
    expect(spriteOf(L"Sprites\\S_Dust\\12.PNG") == std::make_pair(std::string("s_dust"), 12u),
           L"a file in a sprite folder names the frame");
    expect(!spriteOf(L"sprites\\a\\b\\1.png") && !spriteOf(L"sprites\\a\\one.png") && !spriteOf(L"sprites\\a.jpg") &&
               !spriteOf(L"sprites\\.png") && !spriteOf(L"knowledge\\a.png"),
           L"other names are not sprites");

    Image image{301, 37, {}};
    uint32_t seed = 12345;
    for (uint32_t i = 0; i < image.width * image.height; ++i) {
        seed = seed * 1103515245 + 12345;
        const uint32_t kind = i / 1500;
        const BYTE noise = static_cast<BYTE>(seed >> 16);
        const BYTE pixel[4] = {static_cast<BYTE>(kind % 2 ? noise : i / 3), static_cast<BYTE>(kind % 3 ? i : noise),
                               static_cast<BYTE>(kind == 4 ? 7 : i / 7), static_cast<BYTE>(kind == 2 ? noise : 255)};
        image.pixels.insert(image.pixels.end(), pixel, pixel + 4);
    }
    const std::string encoded = encodeQoi(image);
    const std::optional<Image> decoded = decodeQoi(reinterpret_cast<const BYTE*>(encoded.data()), encoded.size());
    expect(decoded && decoded->pixels == image.pixels, L"a picture survives QOI encoding");

    const std::wstring source = gameFolder + L"\\data.win";
    if (GetFileAttributesW(source.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return;
    }
    std::string name;
    std::string blank;
    PageItem item{};
    Image original;
    {
        const MappedFile file(source);
        const auto chunks = chunksOf(file);
        const auto sprites = spritesOf(file, chunkNamed(chunks, "SPRT"));
        const auto pages = texturePagesOf(file, chunkNamed(chunks, "TXTR"));
        const auto menu = sprites.find("s_main_menu_bg");
        if (menu != sprites.end()) {
            const TexturePage& page = pages[pageItemAt(file, menu->second[0]).page];
            const BYTE* blob = file.at(page.data, page.size);
            uint32_t expected = 0;
            std::memcpy(&expected, blob + 8, sizeof(expected));
            const std::optional<std::string> qoi = bunzip(blob + 12, page.size - 12, expected);
            const std::optional<Image> menuPage =
                qoi ? decodeQoi(reinterpret_cast<const BYTE*>(qoi->data()), qoi->size()) : std::nullopt;
            expect(menuPage && encodeQoi(*menuPage) == *qoi, L"a texture page encodes back to the bytes GameMaker wrote");
        }
        for (const auto& sprite : sprites) {
            if (!sprite.second.empty() && !sprite.second[0]) {
                blank = sprite.first;
                break;
            }
        }
        for (const auto& sprite : sprites) {
            const PageItem candidate =
                sprite.second.empty() || !sprite.second[0] ? PageItem{} : pageItemAt(file, sprite.second[0]);
            if (candidate.width > 4 && candidate.height > 4 && candidate.page < pages.size() &&
                pages[candidate.page].size < 8000) {
                name = sprite.first;
                item = candidate;
                original = decodePage(file, pages[candidate.page]).value_or(Image{});
                break;
            }
        }
    }
    expect(!name.empty() && original.width, L"a small texture page decodes");
    if (name.empty() || !original.width) {
        return;
    }

    wchar_t temp[MAX_PATH + 1] = {};
    GetTempPathW(MAX_PATH + 1, temp);
    const std::wstring root = std::wstring(temp) + L"nlse_check_sprites";
    removeTree(root);
    createDirectories(root);
    gameDirectory = gameFolder;
    gameKey = lower(gameFolder);
    gamePrefix = gameKey + L"\\";
    mergedDirectory = root + L"\\merged";
    logPath = root + L"\\check.log";
    Image picture{item.boundWidth, item.boundHeight, {}};
    for (uint32_t y = 0; y < picture.height; ++y) {
        for (uint32_t x = 0; x < picture.width; ++x) {
            const BYTE pixel[4] = {static_cast<BYTE>(x * 9), static_cast<BYTE>(y * 5), static_cast<BYTE>((x ^ y) * 3), 255};
            picture.pixels.insert(picture.pixels.end(), pixel, pixel + 4);
        }
    }
    const std::wstring pictureFile = root + L"\\picture.png";
    expect(savePicture(pictureFile, picture), L"a test picture is written");
    Mod mod{L"Test", L"Test", L"1"};
    redirects.clear();
    spriteFiles.clear();
    spriteFiles[{name, 0}].push_back({&mod, pictureFile});
    if (!blank.empty()) {
        spriteFiles[{blank, 0}].push_back({&mod, pictureFile});
    }
    buildSprites();
    const Redirect* redirect = redirectAt(lower(gameFolder + L"\\data.win"));
    expect(redirect != nullptr, L"data.win goes to the rebuilt copy");
    if (redirect) {
        const MappedFile patched(redirect->source);
        const auto chunks = chunksOf(patched);
        const auto pages = texturePagesOf(patched, chunkNamed(chunks, "TXTR"));
        const std::optional<Image> page = decodePage(patched, pages[item.page]);
        expect(page.has_value(), L"the rebuilt texture page reads back");
        if (page) {
            bool inside = true;
            for (uint32_t y = 0; y < item.height; ++y) {
                for (uint32_t x = 0; x < item.width; ++x) {
                    inside = inside && std::memcmp(pixelAt(*page, item.x + x, item.y + y),
                                                   pixelAt(picture, item.targetX + x, item.targetY + y), 4) == 0;
                }
            }
            expect(inside, L"the sprite on the page is the new picture");
            bool outside = true;
            const uint32_t left = item.x;
            const uint32_t top = item.y;
            for (uint32_t y = 0; y < page->height; y += 7) {
                for (uint32_t x = 0; x < page->width; x += 7) {
                    const bool covered = x >= left && x < left + item.width && y >= top && y < top + item.height;
                    outside = outside && (covered || std::memcmp(pixelAt(*page, x, y), pixelAt(original, x, y), 4) == 0);
                }
            }
            expect(outside, L"the rest of the page stays as it was");
        }
    }
    redirects.clear();
    buildSprites();
    const std::string text = readFile(logPath).value_or("");
    const std::string replaced = "sprite " + name + ": replaced by Test";
    const size_t first = text.find(replaced);
    expect(first != std::string::npos && text.find(replaced, first + 1) != std::string::npos &&
               redirectAt(lower(gameFolder + L"\\data.win")) != nullptr,
           L"the next start reuses the rebuilt copy and logs the same lines");
    expect(text.find("data.win rebuilt") == text.rfind("data.win rebuilt"), L"the copy is rebuilt only once");
    expect(blank.empty() || text.find("sprite " + blank + ": the game has no picture for it") != std::string::npos,
           L"a sprite with no picture in the game says so");
    spriteFiles.clear();
    redirects.clear();
    buildSprites();
    expect(GetFileAttributesW((mergedDirectory + L"\\data.win").c_str()) == INVALID_FILE_ATTRIBUTES,
           L"the copy goes away with the last sprite mod");
    removeTree(root);
}

void checkFonts(const std::wstring& gameFolder)
{
    std::vector<BYTE> sources(7 * 5, 0);
    sources[2 * 7 + 3] = 1;
    const std::vector<double> distances = squaredDistances(sources, 7, 5);
    expect(distances[2 * 7 + 3] == 0 && distances[2 * 7 + 6] == 9 && distances[0] == 13,
           L"squared distances are exact");

    const std::wstring georgia = L"C:\\Windows\\Fonts\\georgia.ttf";
    const std::optional<std::string> data = readFile(georgia);
    if (!data) {
        return;
    }
    const std::optional<Face> face = faceOf(*data);
    expect(face && face->family == L"Georgia" && face->weight == 400 && !face->italic, L"a font file names its family");

    gameDirectory = gameFolder;
    gameKey = lower(gameFolder);
    gamePrefix = gameKey + L"\\";
    expect(fontOf(L"fonts\\f_fontin_pt16_regular_sdf.ttf") == std::string("f_fontin_pt16_regular_sdf"),
           L"a font file named after a font of the game replaces that font");
    expect(!fontOf(L"fonts\\Scada-Regular.ttf") && !fontOf(L"fonts\\a\\b.ttf") && !fontOf(L"fonts\\f_x.png"),
           L"a font file the game has, a nested file or a picture are ordinary files");
    if (GetFileAttributesW((gameFolder + L"\\data.win").c_str()) == INVALID_FILE_ATTRIBUTES) {
        return;
    }

    wchar_t temp[MAX_PATH + 1] = {};
    GetTempPathW(MAX_PATH + 1, temp);
    const std::wstring root = std::wstring(temp) + L"nlse_check_fonts";
    removeTree(root);
    createDirectories(root);
    mergedDirectory = root + L"\\merged";
    logPath = root + L"\\check.log";
    Mod mod{L"Test", L"Test", L"1"};
    redirects.clear();
    spriteFiles.clear();
    fontFiles.clear();
    fontFiles["f_fontin_pt16_regular_sdf"].push_back({&mod, georgia});
    buildSprites();
    const std::string text = readFile(logPath).value_or("");
    expect(text.find("font f_fontin_pt16_regular_sdf: replaced by Test") != std::string::npos,
           L"a font of the game is replaced");
    const Redirect* redirect = redirectAt(lower(gameFolder + L"\\data.win"));
    expect(redirect != nullptr, L"data.win goes to the rebuilt copy for a font");
    if (redirect) {
        const MappedFile original(gameFolder + L"\\data.win");
        const MappedFile patched(redirect->source);
        const FontAsset before = fontsOf(original, chunkNamed(chunksOf(original), "FONT")).at("f_fontin_pt16_regular_sdf");
        const auto chunks = chunksOf(patched);
        const FontAsset after = fontsOf(patched, chunkNamed(chunks, "FONT")).at("f_fontin_pt16_regular_sdf");
        const std::optional<Image> page = decodePage(patched, texturePagesOf(patched, chunkNamed(chunks, "TXTR"))[after.atlas.page]);
        expect(after.glyphs.size() == before.glyphs.size() && page.has_value(), L"the font keeps its letters and its page reads back");
        std::vector<BYTE> used(size_t{after.atlas.width} * after.atlas.height);
        bool inside = true;
        bool apart = true;
        bool moved = false;
        for (size_t i = 0; i < after.glyphs.size(); ++i) {
            const Glyph& glyph = after.glyphs[i];
            moved = moved || glyph.shift != before.glyphs[i].shift;
            inside = inside && glyph.x + glyph.width <= after.atlas.width && glyph.y + glyph.height <= after.atlas.height;
            for (uint32_t y = glyph.y; inside && y < uint32_t{glyph.y} + glyph.height; ++y) {
                for (uint32_t x = glyph.x; x < uint32_t{glyph.x} + glyph.width; ++x) {
                    apart = apart && !used[size_t{y} * after.atlas.width + x]++;
                }
            }
        }
        expect(inside && apart, L"the new letters sit inside the atlas without overlapping");
        expect(moved, L"the new letters have their own widths");
        for (const auto& glyph : after.glyphs) {
            if (glyph.character == 'H' && page) {
                const PageItem& atlas = after.atlas;
                int strongest = 0;
                for (uint32_t x = 0; x < glyph.width; ++x) {
                    const size_t at = (size_t{atlas.y + glyph.y + glyph.height / 2u} * page->width + atlas.x + glyph.x + x) * 4 + 3;
                    strongest = std::max<int>(strongest, page->pixels[at]);
                }
                expect(strongest > 200, L"the stem of a new letter is solid in the distance field");
            }
        }
    }
    fontFiles.clear();
    redirects.clear();
    buildSprites();
    removeTree(root);
}

Json number(const char* text)
{
    Json value;
    value.kind = Json::Kind::Number;
    value.text = text;
    return value;
}

void checkCode(const std::wstring& gameFolder)
{
    wchar_t temp[MAX_PATH + 1] = {};
    GetTempPathW(MAX_PATH + 1, temp);
    const std::wstring root = std::wstring(temp) + L"nlse_check_code";
    removeTree(root);
    createDirectories(root);
    logPath = root + L"\\check.log";
    Mod mod{L"Test", L"Test", L"1"};
    const std::wstring file = root + L"\\code.json";
    writeFile(file, R"({
        "gui_main_menu": [{"find": 350, "set": 420}, {"find": 2, "nth": 3, "count": 9, "set": 0}],
        "other": [{"find": true, "set": false}, {"find": 1, "set": "x"}, {"find": 1}, {"find": 1, "set": 2, "nth": 0},
                  {"skip": "gui_menu_frame_create", "nth": 1}, {"skip": 5}, {"skip": "x", "find": 1, "set": 2},
                  {"call": "gw_AUTO", "to": "gw_REL", "nth": 1}, {"call": "gw_AUTO"}, {"call": "a", "to": "b", "skip": "c"},
                  {"member": "padding", "to": "alignment", "nth": 2}, {"member": "padding"}],
        "broken": 5,
    })");
    codeEdits.clear();
    readCodeEdits(&mod, file);
    expect(codeEdits.size() == 6, L"code.json keeps the well formed changes");
    expect(codeEdits.size() == 6 && codeEdits[1].nth == 3 && codeEdits[1].count == 9 && codeEdits[2].find.text == "true" &&
               codeEdits[3].change == CodeChange::Skip && codeEdits[3].name == "gui_menu_frame_create" &&
               codeEdits[3].nth == 1 && codeEdits[4].change == CodeChange::Call && codeEdits[4].name == "gw_AUTO" &&
               codeEdits[4].to == "gw_REL" && codeEdits[5].change == CodeChange::Member &&
               codeEdits[5].name == "padding" && codeEdits[5].to == "alignment",
           L"nth, count, true or false, skip, call, member and to are read");
    const std::string parsed = readFile(logPath).value_or("");
    size_t skipped = 0;
    for (size_t at = parsed.find("a change in other skipped"); at != std::string::npos;
         at = parsed.find("a change in other skipped", at + 1)) {
        ++skipped;
    }
    expect(skipped == 8 && parsed.find("broken skipped") != std::string::npos,
           L"malformed changes are skipped one by one with a reason");
    codeEdits.clear();
    writeFile(file, R"({
        "reads": [{"member": "zoom", "set": {"noise": [0, 0.03], "seconds": 8}, "count": 1}, {"member": "a", "set": 5},
                  {"member": "a", "set": false, "nth": 2}, {"member": "a", "set": {"noise": [0], "seconds": 8}},
                  {"member": "a", "set": {"noise": [0, 1], "seconds": 0}}, {"member": "a", "set": {"noise": [0, 1]}},
                  {"member": "a", "set": "x"}, {"member": "a", "to": "b", "set": 1}, {"skip": "f", "set": 1}],
    })");
    readCodeEdits(&mod, file);
    const std::string readLog = readFile(logPath).value_or("");
    size_t readSkipped = 0;
    for (size_t at = readLog.find("a change in reads skipped"); at != std::string::npos;
         at = readLog.find("a change in reads skipped", at + 1)) {
        ++readSkipped;
    }
    expect(codeEdits.size() == 3 && readSkipped == 6 && codeEdits[0].change == CodeChange::Read &&
               codeEdits[0].name == "zoom" && codeEdits[0].count == 1 && codeEdits[1].set.text == "5" &&
               codeEdits[2].set.kind == Json::Kind::Boolean && codeEdits[2].nth == 2,
           L"member with set takes a number, true or false, or noise between two numbers");
    codeEdits.clear();
    writeFile(file, R"({
        "made": [{"new": "gw_Label", "show": false, "count": 1}, {"new": "gw_WrapPanel", "show": "hover", "nth": 2},
                 {"new": "gw_Canvas", "name": "news", "count": 1}, {"new": "gw_Canvas", "into": "news", "at": 2},
                 {"new": "gw_WrapPanel", "show": "hover", "pin": true},
                 {"new": "gw_Canvas", "into": "news", "align": ["right", "top"], "margin": [null, -72, 234, null]},
                 {"picture": "s_white_pixel", "into": "news", "at": 1, "size": [2, 48], "color": 6461622, "alpha": 0.7,
                  "align": ["center", "middle"], "margin": [6, 0, 6, 0]},
                 {"new": "gw_Canvas", "into": "news", "size": [10, 20]},
                 {"picture": "s_white_pixel"}, {"picture": "s_white_pixel", "into": "news", "show": false},
                 {"picture": "s_white_pixel", "into": "news", "size": [1]}, {"new": "gw_Canvas", "into": "news", "color": 1},
                 {"new": "gw_Label"}, {"new": "gw_Label", "show": true}, {"new": "gw_Label", "show": "always"},
                 {"new": 5, "show": false}, {"show": false}, {"new": "gw_Label", "show": false, "skip": "f"},
                 {"new": "gw_Label", "show": false, "nth": 0}, {"new": "gw_Canvas", "at": 1},
                 {"new": "gw_Canvas", "into": "news", "at": 0}, {"new": "gw_Canvas", "name": ""},
                 {"new": "gw_Label", "show": false, "pin": true}, {"new": "gw_WrapPanel", "show": "hover", "pin": 1},
                 {"new": "gw_Canvas", "align": ["top", "right"]}, {"new": "gw_Canvas", "margin": [1, 2, 3]},
                 {"new": "gw_Canvas", "margin": ["a", 0, 0, 0]}],
    })");
    readCodeEdits(&mod, file);
    const std::string madeLog = readFile(logPath).value_or("");
    size_t madeSkipped = 0;
    for (size_t at = madeLog.find("a change in made skipped"); at != std::string::npos;
         at = madeLog.find("a change in made skipped", at + 1)) {
        ++madeSkipped;
    }
    expect(codeEdits.size() == 8 && madeSkipped == 19 && codeEdits[0].change == CodeChange::New &&
               codeEdits[0].name == "gw_Label" && codeEdits[0].set.kind == Json::Kind::Boolean &&
               codeEdits[0].count == 1 && codeEdits[1].set.text == "hover" && codeEdits[1].nth == 2 &&
               !codeEdits[1].pin && codeEdits[2].label == "news" && codeEdits[2].set.kind == Json::Kind::Null &&
               codeEdits[3].to == "news" && codeEdits[3].at == 2 && codeEdits[4].pin &&
               codeEdits[5].align.items.size() == 2 && codeEdits[5].margin.items.size() == 4 &&
               codeEdits[6].change == CodeChange::Picture && codeEdits[6].name == "s_white_pixel" &&
               codeEdits[6].to == "news" && codeEdits[6].at == 1 && codeEdits[6].color == 6461622 &&
               codeEdits[6].alpha == 0.7 && codeEdits[6].size.items.size() == 2 && codeEdits[7].size.items.size() == 2,
           L"new takes what is made, show set to false or hover, pin with hover, a name, into with its place, "
           L"align, margin and size, and a picture takes a sprite and where it goes");
    codeEdits.clear();
    double lowest = 1;
    double highest = 0;
    double steepest = 0;
    for (int i = 0; i <= 4000; ++i) {
        const double value = noiseAt(42, i * 0.01);
        lowest = std::min(lowest, value);
        highest = std::max(highest, value);
        steepest = i ? std::max(steepest, std::fabs(value - noiseAt(42, (i - 1) * 0.01))) : 0;
    }
    expect(lowest >= 0 && highest < 1 && highest - lowest > 0.5 && steepest < 0.05 &&
               noiseAt(42, 3.3) != noiseAt(43, 3.3),
           L"noise stays between 0 and 1, moves smoothly and depends on its seed");
    readValues.clear();
    readValues[1] = ReadValue{2.5, 2.5, 0, valueReal};
    readValues[2] = ReadValue{0, 0.03, 8, valueReal, 7};
    BYTE slot[16];
    std::memset(slot, 0xCC, sizeof(slot));
    writeRead(1, slot);
    const auto slotAt = reinterpret_cast<uintptr_t>(slot);
    const bool fixed = readAt<double>(slotAt) == 2.5 && readAt<uint32_t>(slotAt + 8) == 0 &&
                       readAt<uint32_t>(slotAt + 12) == valueReal;
    writeRead(2, slot);
    const double drifting = readAt<double>(slotAt);
    expect(fixed && drifting >= 0 && drifting <= 0.03, L"a read gets its number, or noise between its two numbers");
    readValues.clear();

    const std::wstring exe = gameFolder + L"\\Norland.exe";
    const HMODULE game = GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES
                             ? nullptr
                             : LoadLibraryExW(exe.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!game) {
        removeTree(root);
        return;
    }
    const GameImage image = imageAt(reinterpret_cast<uintptr_t>(game));
    const auto functions = gmlFunctions(image);
    const auto menu = functions.find("gml_Script_gui_main_menu");
    expect(functions.size() > 10000 && menu != functions.end(), L"the game's GML functions are found by name");
    if (menu != functions.end()) {
        const auto sites = [&](const char* value) {
            std::vector<uintptr_t> found;
            for (const auto& entry : valuesIn(image, menu->second)) {
                if (valueMatches(entry.value, number(value))) {
                    found.push_back(entry.site);
                }
            }
            return found;
        };
        const std::vector<uintptr_t> before = sites("350");
        expect(before.size() == 1 && sites("45").size() == 1, L"the main menu passes 350 and 45 once each");
        const AddressRange* frame = functionNamed(functions, "gui_menu_frame_create");
        const std::vector<CodeValue> frames = frame ? callsIn(menu->second, frame->begin) : std::vector<CodeValue>();
        expect(frames.size() == 1, L"the main menu calls gui_menu_frame_create once");
        DeleteFileW(logPath.c_str());
        const AddressRange* base = functionNamed(functions, "gui_main_menu_base");
        const AddressRange* automatic = functionNamed(functions, "gw_AUTO");
        const AddressRange* relative = functionNamed(functions, "gw_REL");
        const auto calls = [&](const AddressRange* to) {
            return base && to ? callsIn(*base, to->begin).size() : 0;
        };
        const size_t automaticBefore = calls(automatic);
        const size_t relativeBefore = calls(relative);
        const auto uses = [&](const char* member) {
            if (!base) {
                return size_t{0};
            }
            const auto members = membersIn(image, *base);
            const auto found = members.find(member);
            return found == members.end() ? size_t{0} : found->second.size();
        };
        const size_t paddingBefore = uses("padding");
        const size_t alignmentBefore = uses("alignment");
        using Members = std::map<std::string, std::vector<CodeValue>>;
        const AddressRange* draw = functionNamed(functions, "gml_Object_o_menu_background_render_Draw_64");
        const Members drawReads = draw ? readsIn(*draw, membersIn(image, *draw)) : Members();
        const Members baseMembers = base ? membersIn(image, *base) : Members();
        const Members baseReads = base ? readsIn(*base, baseMembers) : Members();
        const std::vector<CodeValue> zoomReads = drawReads.count("zoom") ? drawReads.at("zoom") : std::vector<CodeValue>();
        expect(zoomReads.size() == 1 && baseMembers.count("is_not_intersect") && !baseReads.count("is_not_intersect") &&
                   baseReads.count("style"),
               L"reads of a member are told apart from writes");
        std::string error;
        const Json noise = parseText(R"({"noise": [0, 0.03], "seconds": 8})", error).value_or(Json());
        const Json hidden = parseText("false", error).value_or(Json());
        const Json hover = parseText(R"("hover")", error).value_or(Json());
        const AddressRange* container = functionNamed(functions, "gw_Container");
        const uintptr_t measure = container ? methodMadeAfter(image, *container, "_measure_size_child") : 0;
        Mod broken{L"Broken", L"Broken", L"1"};
        Mod later{L"Later", L"Later", L"1"};
        codeEdits = {{&mod, "gui_top_lord_card", CodeChange::New, Json(), hidden, "gw_Label", "", 0, 1},
                     {&mod, "gui_top_lord_card", CodeChange::New, Json(), hover, "gw_StackPanel", "", 4, 4},
                     {&mod, "__encyclopedia_gui_init", CodeChange::New, Json(), Json(), "gw_Canvas", "", 0, 1, "news"},
                     {&mod, "gui_hud_anchor_advisor_create", CodeChange::New, Json(), Json(), "gw_Canvas", "news", 0, 1,
                      "", 1, false, parseText(R"(["right", "top"])", error).value_or(Json()),
                      parseText("[null, -72, 234, null]", error).value_or(Json())},
                     {&mod, "__encyclopedia_gui_init", CodeChange::Picture, Json(), Json(), "s_white_pixel", "news", 0, 0,
                      "", 1, false, parseText(R"(["center", "middle"])", error).value_or(Json()),
                      parseText("[6, 0, 6, 0]", error).value_or(Json()), parseText("[2, 48]", error).value_or(Json())},
                     {&mod, "gui_main_menu", CodeChange::Value, number("350"), number("420"), "", "", 0, 0},
                     {&mod, "gui_main_menu", CodeChange::Value, number("19"), number("20"), "", "", 0, 0},
                     {&mod, "gui_main_menu", CodeChange::Skip, Json(), Json(), "gui_menu_frame_create", "", 1, 0},
                     {&mod, "gui_main_menu_base", CodeChange::Call, Json(), Json(), "gw_AUTO", "gw_REL", 1, 0},
                     {&mod, "gui_main_menu_base", CodeChange::Call, Json(), Json(), "gw_AUTO", "gw_REL", 2, 0},
                     {&mod, "gui_main_menu_base", CodeChange::Member, Json(), Json(), "padding", "alignment", 2, 0},
                     {&mod, "gui_main_menu_beta_branches_switcher", CodeChange::Member, Json(), Json(), "pt20_bold_bright",
                      "pt20_main_menu_external_links_not_hovered", 0, 1},
                     {&mod, "gui_menu_font", CodeChange::Value, number("40"), number("26"), "", "", 0, 1},
                     {&mod, "gml_Object_o_menu_background_render_Draw_64", CodeChange::Read, Json(), noise, "zoom", "",
                      0, 1},
                     {&broken, "gui_main_menu_base", CodeChange::Read, Json(), number("1"), "is_not_intersect", "", 0, 0},
                     {&broken, "gui_main_menu", CodeChange::Value, number("22"), number("30"), "", "", 0, 0},
                     {&broken, "gui_main_menu", CodeChange::Value, number("45"), number("50"), "", "", 0, 2},
                     {&broken, "gui_main_menu", CodeChange::Skip, Json(), Json(), "gui_main_menu_base", "", 2, 0},
                     {&broken, "gui_main_menu_base", CodeChange::Member, Json(), Json(), "padding", "no_such_member", 1, 0},
                     {&broken, "gui_main_menu", CodeChange::Value, number("350"), number("500"), "", "", 0, 0},
                     {&broken, "gui_no_such_menu", CodeChange::Value, number("1"), number("2"), "", "", 0, 0},
                     {&broken, "gui_hud_actors_create", CodeChange::New, Json(), Json(), "gw_Canvas", "nowhere", 0, 1},
                     {&broken, "gui_main_menu", CodeChange::Picture, Json(), Json(), "s_no_such_sprite", "news", 0, 0},
                     {&broken, "gui_top_lord_card", CodeChange::New, Json(), Json(), "gw_StackPanel", "", 0, 0, "twice"},
                     {&later, "gui_main_menu", CodeChange::Value, number("19"), number("25"), "", "", 0, 0}};
        gameDirectory = gameFolder;
        applyCodeEdits(image);
        const std::string lines = readFile(logPath).value_or("");
        const auto inGame = [&](const void* address) {
            return image.code.holds(reinterpret_cast<uintptr_t>(address), 1);
        };
        expect(lines.find("  gui_top_lord_card: new gw_Label is hidden") != std::string::npos &&
                   lines.find("  gui_top_lord_card: the 4th new gw_StackPanel shows on hover") != std::string::npos &&
                   newSites.size() == 4 && inGame(reinterpret_cast<void*>(gml.get)) &&
                   inGame(reinterpret_cast<void*>(gml.method)) && inGame(reinterpret_cast<void*>(gml.release)) &&
                   inGame(reinterpret_cast<void*>(gml.builtin)),
               L"what a script makes with new is found, and the game's own calls for members and methods with it");
        expect(lines.find("  __encyclopedia_gui_init: new gw_Canvas is named news") != std::string::npos &&
                   lines.find("  gui_hud_anchor_advisor_create: new gw_Canvas moves into news as its 1st part, sits "
                              "right top and takes the margin 0 -72 234 0") != std::string::npos &&
                   inGame(reinterpret_cast<void*>(gml.unit)) &&
                   lines.find("  gui_hud_actors_create: no change names nowhere") != std::string::npos &&
                   lines.find("  gui_top_lord_card: a name needs one place, gw_StackPanel is made 4 times") !=
                       std::string::npos &&
                   lines.find("  __encyclopedia_gui_init: a picture of s_white_pixel goes into news as its 1st part, "
                              "is 2 by 48, sits center middle and takes the margin 6 0 6 0") != std::string::npos &&
                   pictures.size() == 1 && pictures.front().sprite > 0,
               L"a new part can be named once and moved into a named part, and a picture added there");
        expect(lines.find("the interface is checked every frame from obj_gw_controller") != std::string::npos &&
                   lines.find("folded parts give up their room") != std::string::npos && measure &&
                   readAt<BYTE>(measure) == 0xE9 && inGame(reinterpret_cast<void*>(gml.loading)),
               L"hover is checked once a frame, and folded parts are measured as nothing");
        expect(lines.find("code Test:") != std::string::npos &&
                   lines.find("  gui_main_menu_base: the 2nd use of padding now reads alignment") != std::string::npos &&
                   paddingBefore == 2 && alignmentBefore == 1 && uses("padding") == 1 && uses("alignment") == 2,
               L"a function reads another member");
        expect(lines.find("  gui_main_menu_base: the 1st call to gw_AUTO now goes to gw_REL") != std::string::npos &&
                   lines.find("  gui_main_menu_base: the 2nd call to gw_AUTO now goes to gw_REL") != std::string::npos &&
                   automaticBefore > 2 && calls(automatic) == automaticBefore - 2 &&
                   calls(relative) == relativeBefore + 2,
               L"calls go to another function, counted as the game wrote them");
        expect(lines.find("  gui_main_menu: 350 to 420") != std::string::npos, L"a value in the game's code is changed");
        expect(lines.find("  gui_main_menu: the 1st call to gui_menu_frame_create skipped") != std::string::npos,
               L"a call is skipped");
        expect(lines.find("code Broken: none made, 8 changes do not match the game") != std::string::npos &&
                   lines.find("  gui_main_menu: the game has no sprite s_no_such_sprite") != std::string::npos,
               L"a mod whose changes do not all match makes none");
        expect(lines.find("  gml_Object_o_menu_background_render_Draw_64: zoom now gets noise between 0 and 0.03") !=
                       std::string::npos &&
                   lines.find("  gui_main_menu_base: no read of is_not_intersect") != std::string::npos,
               L"a member read gets noise, and a member that is only written has no read to change");
        if (zoomReads.size() == 1) {
            const uintptr_t site = zoomReads[0].site;
            const uintptr_t thunk = site + 5 + static_cast<intptr_t>(readAt<int32_t>(site + 1));
            BYTE zoom[16] = {};
            writeRead(site + 5, zoom);
            const auto zoomAt = reinterpret_cast<uintptr_t>(zoom);
            expect(readAt<BYTE>(site) == 0xE8 && readAt<uint16_t>(thunk) == 0x25FF &&
                       readAt<uintptr_t>(thunk + 6) == reinterpret_cast<uintptr_t>(&readMember) &&
                       readAt<double>(zoomAt) >= 0 && readAt<double>(zoomAt) <= 0.03 &&
                       readAt<uint32_t>(zoomAt + 12) == valueReal,
                   L"the read now calls NLSE, which hands back the noise");
        }
        const AddressRange* font = functionNamed(functions, "gui_menu_font");
        size_t written = 0;
        for (const auto& value : font ? immediatesIn(*font) : std::vector<CodeValue>()) {
            written += readAt<double>(value.value) == 26 ? 1 : 0;
        }
        expect(lines.find("  gui_menu_font: 40 to 26") != std::string::npos && written == 1,
               L"a number written into an instruction is changed in place");
        const AddressRange* switcher = functionNamed(functions, "gui_main_menu_beta_branches_switcher");
        const auto switcherMembers = switcher ? membersIn(image, *switcher) : std::map<std::string, std::vector<CodeValue>>();
        expect(lines.find("  gui_main_menu_beta_branches_switcher: the use of pt20_bold_bright now reads "
                          "pt20_main_menu_external_links_not_hovered") != std::string::npos &&
                   !switcherMembers.count("pt20_bold_bright") &&
                   switcherMembers.count("pt20_main_menu_external_links_not_hovered"),
               L"a function reads a member it never used before");
        expect(lines.find("  gui_main_menu: 45 appears once instead of twice") != std::string::npos &&
                   lines.find("  gui_main_menu: gui_main_menu_base is called only once") != std::string::npos &&
                   lines.find("  gui_main_menu_base: the game has no member no_such_member") != std::string::npos &&
                   lines.find("  gui_no_such_menu: the game has no such function") != std::string::npos,
               L"each change that does not match says why");
        expect(lines.find("Broken: none made") != std::string::npos && sites("22").size() == 1 && sites("45").size() == 1,
               L"the changes of that mod that would match stay unmade");
        expect(lines.find("  gui_main_menu: a change overridden by Later") != std::string::npos &&
                   lines.find("code Later:") != std::string::npos &&
                   lines.find("  gui_main_menu: 19 to 25") != std::string::npos && sites("19").empty(),
               L"a later mod overrides the same change");
        expect(sites("350").empty(), L"the changed place no longer reads the old value");
        if (before.size() == 1) {
            const uintptr_t target = before[0] + 7 + static_cast<intptr_t>(readAt<int32_t>(before[0] + 3));
            expect(readAt<double>(target) == 420 && readAt<uint32_t>(target + 12) == valueReal,
                   L"the changed place reads the new value");
        }
        if (frames.size() == 1) {
            expect(callsIn(menu->second, frame->begin).empty(), L"the skipped call goes elsewhere");
            const uintptr_t thunk = frames[0].site + 5 + static_cast<intptr_t>(readAt<int32_t>(frames[0].site + 1));
            int value = 7;
            expect(reinterpret_cast<void* (*)(void*, void*, void*, int, void*)>(thunk)(nullptr, nullptr, &value, 0,
                                                                                       nullptr) == &value &&
                       value == 7,
                   L"a skipped call returns the result it was given, untouched");
        }
        codeEdits.clear();
    }
    FreeLibrary(game);
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
    checkSprites(game);
    checkFonts(game);
    checkCode(game);
    if (failures) {
        wprintf(L"%d checks failed\n", failures);
        return 1;
    }
    wprintf(L"all checks passed, %d game JSON files read and written back\n", files);
    return 0;
}
