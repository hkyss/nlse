#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>

#include "nlse.h"

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace {

constexpr wchar_t version[] = L"0.3.0";

HMODULE self;
bool active = false;
uintptr_t gameBase = 0;
uint64_t gameVersion = 0;
std::wstring gameDirectory;
std::wstring logPath;

std::wstring modulePath(HMODULE module)
{
    std::wstring path(32768, L'\0');
    path.resize(GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size())));
    return path;
}

std::wstring parentOf(const std::wstring& path)
{
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

std::wstring nameOf(const std::wstring& path)
{
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::wstring environment(const wchar_t* name)
{
    std::wstring value(32768, L'\0');
    value.resize(GetEnvironmentVariableW(name, value.data(), static_cast<DWORD>(value.size())));
    return value;
}

std::string utf8(const std::wstring& text)
{
    if (text.empty()) {
        return {};
    }
    const int length = static_cast<int>(text.size());
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), length, nullptr, 0, nullptr, nullptr);
    std::string bytes(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), length, bytes.data(), size, nullptr, nullptr);
    return bytes;
}

std::wstring wide(const std::string& bytes)
{
    if (bytes.empty()) {
        return {};
    }
    const int length = static_cast<int>(bytes.size());
    const int size = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), length, nullptr, 0);
    std::wstring text(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes.data(), length, text.data(), size);
    return text;
}
std::wstring lower(const std::wstring& text)
{
    if (text.empty()) {
        return text;
    }
    std::wstring result(text.size(), L'\0');
    const int length = static_cast<int>(text.size());
    LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, text.data(), length, result.data(), length, nullptr,
                  nullptr, 0);
    return result;
}

bool hasExtension(const std::wstring& name, const wchar_t* extension)
{
    const size_t suffix = wcslen(extension);
    return name.size() > suffix && _wcsicmp(name.c_str() + name.size() - suffix, extension) == 0;
}

void log(const std::wstring& line)
{
    SYSTEMTIME now;
    GetLocalTime(&now);
    wchar_t stamp[16];
    swprintf_s(stamp, L"%02u:%02u:%02u.%03u ", now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
    const std::string bytes = utf8(stamp + line + L"\r\n");
    HANDLE file = CreateFileW(logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD written = 0;
    WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    CloseHandle(file);
}

std::vector<std::wstring> listDirectory(const std::wstring& folder, bool directories)
{
    std::map<std::wstring, std::wstring> sorted;
    WIN32_FIND_DATAW found;
    HANDLE search = FindFirstFileW((folder + L"\\*").c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) {
        return {};
    }
    do {
        const std::wstring name = found.cFileName;
        const bool directory = (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (name != L"." && name != L".." && directory == directories) {
            sorted.emplace(lower(name), name);
        }
    } while (FindNextFileW(search, &found));
    FindClose(search);
    std::vector<std::wstring> names;
    for (const auto& entry : sorted) {
        names.push_back(entry.second);
    }
    return names;
}

bool replaceImport(HMODULE module, const char* library, const char* function, void* replacement, void** original)
{
    auto* base = reinterpret_cast<BYTE*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* headers = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& imports = headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!imports.VirtualAddress) {
        return false;
    }
    for (auto* entry = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + imports.VirtualAddress); entry->Name;
         ++entry) {
        if (_stricmp(reinterpret_cast<const char*>(base + entry->Name), library) != 0 ||
            !entry->OriginalFirstThunk) {
            continue;
        }
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + entry->OriginalFirstThunk);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + entry->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) {
                continue;
            }
            const auto* byName = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(byName->Name), function) != 0) {
                continue;
            }
            DWORD protection = 0;
            if (!VirtualProtect(&slots->u1.Function, sizeof(slots->u1.Function), PAGE_READWRITE, &protection)) {
                return false;
            }
            *original = reinterpret_cast<void*>(slots->u1.Function);
            slots->u1.Function = reinterpret_cast<ULONG_PTR>(replacement);
            VirtualProtect(&slots->u1.Function, sizeof(slots->u1.Function), protection, &protection);
            return true;
        }
    }
    return false;
}

const IMAGE_NT_HEADERS* headersOf(uintptr_t module)
{
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    return reinterpret_cast<const IMAGE_NT_HEADERS*>(module + dos->e_lfanew);
}

std::wstring versionText(uint64_t value)
{
    if (!value) {
        return L"unknown";
    }
    std::wstring text;
    for (int shift = 48; shift >= 0; shift -= 16) {
        text += (text.empty() ? L"" : L".") + std::to_wstring((value >> shift) & 0xFFFF);
    }
    return text;
}

uint64_t fileVersion(HMODULE module)
{
    const HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(VS_VERSION_INFO), MAKEINTRESOURCEW(16));
    const HGLOBAL loaded = resource ? LoadResource(module, resource) : nullptr;
    const auto* bytes = static_cast<const BYTE*>(loaded ? LockResource(loaded) : nullptr);
    if (!bytes) {
        return 0;
    }
    const DWORD size = SizeofResource(module, resource);
    for (DWORD at = 0; at + sizeof(VS_FIXEDFILEINFO) <= size; at += 4) {
        VS_FIXEDFILEINFO information;
        std::memcpy(&information, bytes + at, sizeof(information));
        if (information.dwSignature == VS_FFI_SIGNATURE) {
            return (static_cast<uint64_t>(information.dwFileVersionMS) << 32) | information.dwFileVersionLS;
        }
    }
    return 0;
}

bool writeMemory(uintptr_t address, const void* bytes, size_t size)
{
    void* target = reinterpret_cast<void*>(address);
    DWORD protection = 0;
    if (!address || !bytes || !size || !VirtualProtect(target, size, PAGE_EXECUTE_READWRITE, &protection)) {
        return false;
    }
    std::memcpy(target, bytes, size);
    VirtualProtect(target, size, protection, &protection);
    FlushInstructionCache(GetCurrentProcess(), target, size);
    return true;
}

bool reaches(uintptr_t from, uintptr_t to)
{
    const auto distance = static_cast<int64_t>(to - from);
    return distance >= INT32_MIN && distance <= INT32_MAX;
}

BYTE* allocateNear(uintptr_t address, size_t size)
{
    SYSTEM_INFO system;
    GetSystemInfo(&system);
    const uintptr_t step = system.dwAllocationGranularity;
    const auto lowest = reinterpret_cast<uintptr_t>(system.lpMinimumApplicationAddress);
    const auto highest = reinterpret_cast<uintptr_t>(system.lpMaximumApplicationAddress);
    const uintptr_t start = address & ~(step - 1);
    for (uintptr_t distance = step; distance < 0x7FF00000; distance += step) {
        for (const uintptr_t at : {start - distance, start + distance}) {
            MEMORY_BASIC_INFORMATION information;
            if (at < lowest || at > highest ||
                !VirtualQuery(reinterpret_cast<void*>(at), &information, sizeof(information)) ||
                information.State != MEM_FREE || information.RegionSize < size) {
                continue;
            }
            if (void* memory = VirtualAlloc(reinterpret_cast<void*>(at), size, MEM_RESERVE | MEM_COMMIT,
                                            PAGE_EXECUTE_READWRITE)) {
                return static_cast<BYTE*>(memory);
            }
        }
    }
    return nullptr;
}

struct ThunkBlock {
    BYTE* next;
    BYTE* end;
};

std::mutex thunksMutex;
std::vector<ThunkBlock> thunkBlocks;

BYTE* makeThunk(uintptr_t site, void* function)
{
    constexpr ptrdiff_t thunkSize = 16;
    constexpr size_t blockSize = 0x10000;
    std::lock_guard<std::mutex> lock(thunksMutex);
    ThunkBlock* block = nullptr;
    for (auto& candidate : thunkBlocks) {
        const auto next = reinterpret_cast<uintptr_t>(candidate.next);
        if (candidate.end - candidate.next >= thunkSize && reaches(site + 5, next) &&
            reaches(site + 5, next + thunkSize)) {
            block = &candidate;
            break;
        }
    }
    if (!block) {
        BYTE* memory = allocateNear(site, blockSize);
        if (!memory) {
            return nullptr;
        }
        thunkBlocks.push_back({memory, memory + blockSize});
        block = &thunkBlocks.back();
    }
    BYTE* thunk = block->next;
    block->next += thunkSize;
    const BYTE jump[6] = {0xFF, 0x25, 0, 0, 0, 0};
    const auto target = reinterpret_cast<uintptr_t>(function);
    std::memcpy(thunk, jump, sizeof(jump));
    std::memcpy(thunk + sizeof(jump), &target, sizeof(target));
    return thunk;
}

bool writeBranch(uintptr_t site, void* function, BYTE opcode)
{
    BYTE* thunk = makeThunk(site, function);
    if (!thunk) {
        return false;
    }
    BYTE code[5] = {opcode};
    const auto displacement = static_cast<int32_t>(reinterpret_cast<uintptr_t>(thunk) - (site + 5));
    std::memcpy(code + 1, &displacement, sizeof(displacement));
    return writeMemory(site, code, sizeof(code));
}

uintptr_t writeCall(uintptr_t site, void* function)
{
    if (!site || !function || *reinterpret_cast<const BYTE*>(site) != 0xE8) {
        return 0;
    }
    int32_t displacement = 0;
    std::memcpy(&displacement, reinterpret_cast<const void*>(site + 1), sizeof(displacement));
    const uintptr_t previous = site + 5 + static_cast<uintptr_t>(static_cast<intptr_t>(displacement));
    return writeBranch(site, function, 0xE8) ? previous : 0;
}

bool writeJump(uintptr_t site, void* function)
{
    return site && function && writeBranch(site, function, 0xE9);
}

std::optional<std::vector<int>> parsePattern(const char* text)
{
    const auto digit = [](char c) {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        if (c >= 'A' && c <= 'F') {
            return c - 'A' + 10;
        }
        return -1;
    };
    std::vector<int> bytes;
    for (const char* at = text; at && *at;) {
        if (*at == ' ') {
            ++at;
        } else if (*at == '?') {
            bytes.push_back(-1);
            at += at[1] == '?' ? 2 : 1;
        } else if (digit(at[0]) >= 0 && digit(at[1]) >= 0) {
            bytes.push_back(digit(at[0]) * 16 + digit(at[1]));
            at += 2;
        } else {
            return std::nullopt;
        }
    }
    if (bytes.empty()) {
        return std::nullopt;
    }
    return bytes;
}

uintptr_t findPattern(const BYTE* begin, const BYTE* end, const std::vector<int>& pattern)
{
    if (end - begin < static_cast<ptrdiff_t>(pattern.size())) {
        return 0;
    }
    const BYTE* last = end - pattern.size();
    for (const BYTE* at = begin; at <= last; ++at) {
        if (pattern[0] >= 0) {
            at = static_cast<const BYTE*>(std::memchr(at, pattern[0], static_cast<size_t>(last - at) + 1));
            if (!at) {
                return 0;
            }
        }
        size_t matched = 1;
        while (matched < pattern.size() && (pattern[matched] < 0 || at[matched] == pattern[matched])) {
            ++matched;
        }
        if (matched == pattern.size()) {
            return reinterpret_cast<uintptr_t>(at);
        }
    }
    return 0;
}

uintptr_t findInGame(const char* pattern)
{
    try {
        const std::optional<std::vector<int>> bytes = parsePattern(pattern);
        if (!bytes) {
            log(L"FindPattern cannot read the pattern " + (pattern ? wide(pattern) : std::wstring(L"null")));
            return 0;
        }
        const IMAGE_NT_HEADERS* headers = headersOf(gameBase);
        const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(headers);
        for (WORD i = 0; i < headers->FileHeader.NumberOfSections; ++i, ++section) {
            if (section->Characteristics & IMAGE_SCN_MEM_EXECUTE) {
                const auto* begin = reinterpret_cast<const BYTE*>(gameBase + section->VirtualAddress);
                if (const uintptr_t found = findPattern(begin, begin + section->Misc.VirtualSize, *bytes)) {
                    return found;
                }
            }
        }
    } catch (...) {
    }
    return 0;
}

bool hookImport(const char* library, const char* function, void* replacement, void** original)
{
    return library && function && replacement && original &&
           replaceImport(reinterpret_cast<HMODULE>(gameBase), library, function, replacement, original);
}

struct Plugin {
    HMODULE module = nullptr;
    std::string name;
    std::string version;
    std::string author;
    NLSEPluginInfo info{};
    bool loaded = false;
};

struct Listener {
    NLSEPluginHandle plugin;
    std::string sender;
    bool anySender;
    NLSEMessageCallback callback;
};

std::mutex pluginsMutex;
std::vector<std::unique_ptr<Plugin>> plugins;
std::vector<Listener> listeners;
std::atomic<NLSEPluginHandle> loadingPlugin{0};
std::string nlseVersionText;
std::string gameDirectoryText;
NLSEInterface pluginInterface{};

Plugin* pluginAt(NLSEPluginHandle handle)
{
    return handle && handle <= plugins.size() ? plugins[handle - 1].get() : nullptr;
}

bool usable(const Plugin* plugin)
{
    return plugin && (plugin->loaded || plugin->info.handle == loadingPlugin);
}

std::wstring pluginName(NLSEPluginHandle handle)
{
    std::lock_guard<std::mutex> lock(pluginsMutex);
    const Plugin* plugin = pluginAt(handle);
    return plugin ? wide(plugin->name) : L"plugin " + std::to_wstring(handle);
}

NLSEPluginHandle currentPlugin()
{
    return loadingPlugin;
}

const NLSEPluginInfo* pluginInfo(const char* name)
{
    std::lock_guard<std::mutex> lock(pluginsMutex);
    for (const auto& plugin : plugins) {
        if (name && plugin->loaded && plugin->name == name) {
            return &plugin->info;
        }
    }
    return nullptr;
}

void pluginLog(NLSEPluginHandle plugin, const char* format, ...)
{
    if (!format) {
        return;
    }
    va_list arguments;
    va_start(arguments, format);
    try {
        va_list measure;
        va_copy(measure, arguments);
        const int size = std::vsnprintf(nullptr, 0, format, measure);
        va_end(measure);
        std::string text(size > 0 ? static_cast<size_t>(size) : 0, '\0');
        if (size > 0) {
            std::vsnprintf(text.data(), text.size() + 1, format, arguments);
        }
        log(pluginName(plugin) + L": " + wide(text));
    } catch (...) {
    }
    va_end(arguments);
}

bool addListener(NLSEPluginHandle plugin, const char* sender, NLSEMessageCallback callback)
{
    try {
        std::lock_guard<std::mutex> lock(pluginsMutex);
        if (!callback || !usable(pluginAt(plugin))) {
            return false;
        }
        listeners.push_back({plugin, sender ? sender : "", !sender, callback});
        return true;
    } catch (...) {
        return false;
    }
}

size_t deliver(const std::string& sender, uint32_t type, const void* data, uint32_t size, const char* receiver)
{
    std::vector<NLSEMessageCallback> callbacks;
    {
        std::lock_guard<std::mutex> lock(pluginsMutex);
        for (const auto& listener : listeners) {
            const Plugin* plugin = pluginAt(listener.plugin);
            if ((listener.anySender || listener.sender == sender) && usable(plugin) &&
                (!receiver || plugin->name == receiver)) {
                callbacks.push_back(listener.callback);
            }
        }
    }
    const NLSEMessage message{sender.c_str(), type, size, data};
    for (const auto callback : callbacks) {
        callback(&message);
    }
    return callbacks.size();
}

bool dispatch(NLSEPluginHandle plugin, uint32_t type, const void* data, uint32_t size, const char* receiver)
{
    try {
        std::string sender;
        {
            std::lock_guard<std::mutex> lock(pluginsMutex);
            const Plugin* from = pluginAt(plugin);
            if (!usable(from)) {
                return false;
            }
            sender = from->name;
        }
        return deliver(sender, type, data, size, receiver) > 0;
    } catch (...) {
        return false;
    }
}

std::string fixedText(const char* text, size_t size)
{
    return std::string(text, strnlen(text, size));
}

std::wstring refusal(const NLSEPluginVersion& declared)
{
    if (!declared.apiVersion) {
        return L"its NLSEPlugin_Version is empty";
    }
    if (declared.apiVersion > NLSE_API_VERSION) {
        return L"it needs a newer NLSE";
    }
    if (fixedText(declared.name, sizeof(declared.name)).empty()) {
        return L"it has no name";
    }
    std::wstring wanted;
    for (const uint64_t game : declared.gameVersions) {
        if (!game) {
            break;
        }
        if (game == gameVersion) {
            return {};
        }
        wanted += (wanted.empty() ? L"" : L", ") + versionText(game);
    }
    if (wanted.empty()) {
        return {};
    }
    return L"it is made for Norland " + wanted + L", this is " + versionText(gameVersion);
}

void loadPlugin(const std::wstring& path, const std::wstring& shown)
{
    const HMODULE module = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module) {
        log(shown + L" cannot be loaded, error " + std::to_wstring(GetLastError()));
        return;
    }
    const auto* declared = reinterpret_cast<const NLSEPluginVersion*>(GetProcAddress(module, "NLSEPlugin_Version"));
    const auto load = reinterpret_cast<NLSEPluginLoad>(GetProcAddress(module, "NLSEPlugin_Load"));
    std::wstring problem = declared && load ? refusal(*declared) : std::wstring(L"it is not an NLSE plugin");
    auto plugin = std::make_unique<Plugin>();
    if (problem.empty()) {
        plugin->module = module;
        plugin->name = fixedText(declared->name, sizeof(declared->name));
        plugin->version = fixedText(declared->version, sizeof(declared->version));
        plugin->author = fixedText(declared->author, sizeof(declared->author));
        if (pluginInfo(plugin->name.c_str())) {
            problem = L"a plugin named " + wide(plugin->name) + L" is already loaded";
        }
    }
    if (!problem.empty()) {
        log(shown + L" skipped, " + problem);
        FreeLibrary(module);
        return;
    }
    Plugin* added = plugin.get();
    {
        std::lock_guard<std::mutex> lock(pluginsMutex);
        plugin->info = {static_cast<NLSEPluginHandle>(plugins.size() + 1), plugin->name.c_str(),
                        plugin->version.c_str(), plugin->author.c_str()};
        plugins.push_back(std::move(plugin));
    }
    const std::wstring name = wide(added->name);
    log(L"plugin " + name + (added->version.empty() ? L"" : L" " + wide(added->version)));
    loadingPlugin = added->info.handle;
    bool loaded = false;
    try {
        loaded = load(&pluginInterface);
        if (!loaded) {
            log(L"  " + name + L" refused to load");
        }
    } catch (...) {
        log(L"  " + name + L" failed with an exception");
    }
    loadingPlugin = 0;
    std::lock_guard<std::mutex> lock(pluginsMutex);
    added->loaded = loaded;
}

void loadPlugins()
{
    nlseVersionText = utf8(version);
    gameDirectoryText = utf8(gameDirectory);
    pluginInterface.apiVersion = NLSE_API_VERSION;
    pluginInterface.nlseVersion = nlseVersionText.c_str();
    pluginInterface.gameVersion = gameVersion;
    pluginInterface.gameBase = gameBase;
    pluginInterface.gameDirectory = gameDirectoryText.c_str();
    pluginInterface.GetPluginHandle = currentPlugin;
    pluginInterface.GetPluginInfo = pluginInfo;
    pluginInterface.Log = pluginLog;
    pluginInterface.RegisterListener = addListener;
    pluginInterface.Dispatch = dispatch;
    pluginInterface.FindPattern = findInGame;
    pluginInterface.WriteMemory = writeMemory;
    pluginInterface.WriteCall = writeCall;
    pluginInterface.WriteJump = writeJump;
    pluginInterface.HookImport = hookImport;
    const std::wstring folder = gameDirectory + L"\\mods\\NLSE\\Plugins";
    bool found = false;
    for (const auto& name : listDirectory(folder, false)) {
        if (hasExtension(name, L".dll")) {
            found = true;
            loadPlugin(folder + L"\\" + name, L"mods\\NLSE\\Plugins\\" + name);
        }
    }
    if (!found) {
        log(L"no plugins found");
        return;
    }
    deliver("NLSE", NLSE_MESSAGE_POST_LOAD, nullptr, 0, nullptr);
    deliver("NLSE", NLSE_MESSAGE_POST_POST_LOAD, nullptr, 0, nullptr);
}

using EntryFunction = DWORD(WINAPI*)(void*);

EntryFunction gameEntry;
BYTE gameEntryBytes[14];

DWORD WINAPI enterGame(void* parameter)
{
    writeMemory(reinterpret_cast<uintptr_t>(gameEntry), gameEntryBytes, sizeof(gameEntryBytes));
    try {
        loadPlugins();
    } catch (...) {
        log(L"plugins stopped loading on an error");
    }
    return gameEntry(parameter);
}

bool hookEntry(uintptr_t entry)
{
    BYTE jump[sizeof(gameEntryBytes)] = {0xFF, 0x25};
    const auto target = reinterpret_cast<uintptr_t>(&enterGame);
    std::memcpy(jump + 6, &target, sizeof(target));
    gameEntry = reinterpret_cast<EntryFunction>(entry);
    std::memcpy(gameEntryBytes, reinterpret_cast<const void*>(entry), sizeof(gameEntryBytes));
    return writeMemory(entry, jump, sizeof(jump));
}

std::wstring parentProcess()
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return L"unknown";
    }
    const DWORD current = GetCurrentProcessId();
    DWORD parent = 0;
    PROCESSENTRY32W entry{sizeof(entry)};
    for (BOOL found = Process32FirstW(snapshot, &entry); found; found = Process32NextW(snapshot, &entry)) {
        if (entry.th32ProcessID == current) {
            parent = entry.th32ParentProcessID;
        }
    }
    std::wstring name = L"unknown";
    entry = {sizeof(entry)};
    for (BOOL found = Process32FirstW(snapshot, &entry); found; found = Process32NextW(snapshot, &entry)) {
        if (entry.th32ProcessID == parent) {
            name = entry.szExeFile;
        }
    }
    CloseHandle(snapshot);
    return name;
}

void start()
{
    const std::wstring loaderFile = modulePath(self);
    const std::wstring parent = parentProcess();
    const std::wstring parentName = lower(parent);
    const bool inGameFolder = lower(nameOf(loaderFile)) == L"winmm.dll";
    if (inGameFolder && (parentName == L"modorganizer.exe" || parentName.rfind(L"usvfs_proxy", 0) == 0)) {
        return;
    }
    const std::wstring instance = L"Local\\nlse_" + std::to_wstring(GetCurrentProcessId());
    if (!CreateMutexW(nullptr, FALSE, instance.c_str()) || GetLastError() == ERROR_ALREADY_EXISTS) {
        return;
    }
    active = true;

    gameDirectory = parentOf(modulePath(nullptr));
    const std::wstring logFolder = environment(L"LOCALAPPDATA") + L"\\Strategy";
    logPath = logFolder + L"\\nlse.log";
    MoveFileExW(logPath.c_str(), (logFolder + L"\\nlse.previous.log").c_str(), MOVEFILE_REPLACE_EXISTING);
    const HMODULE game = GetModuleHandleW(nullptr);
    gameBase = reinterpret_cast<uintptr_t>(game);
    gameVersion = fileVersion(game);

    log(L"NLSE " + std::wstring(version) + L" from " + loaderFile + L", started by " + parent);
    log(L"Norland " + versionText(gameVersion));
    if (!inGameFolder && GetFileAttributesW((gameDirectory + L"\\winmm.dll").c_str()) != INVALID_FILE_ATTRIBUTES) {
        log(L"winmm.dll in the game folder is not needed with MO2");
    }
    if (!hookEntry(gameBase + headersOf(gameBase)->OptionalHeader.AddressOfEntryPoint)) {
        log(L"could not hook the game start, plugins will not load");
    }
}

}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        self = module;
        DisableThreadLibraryCalls(module);
        start();
    } else if (reason == DLL_PROCESS_DETACH && active) {
        log(L"the game closed");
    }
    return TRUE;
}
