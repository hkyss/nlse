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

}

int wmain(int argc, wchar_t** argv)
{
    checkMerge();
    checkNames();
    const std::wstring game =
        argc > 1 ? argv[1] : L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Norland Story Generating Strategy";
    const int files = checkGame(game);
    checkGameFolder();
    if (failures) {
        wprintf(L"%d checks failed\n", failures);
        return 1;
    }
    wprintf(L"all checks passed, %d game JSON files read and written back\n", files);
    return 0;
}
