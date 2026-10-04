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
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

constexpr wchar_t version[] = L"0.1.0";

HMODULE self;
bool active = false;
uintptr_t gameBase = 0;
uint64_t gameVersion = 0;
std::wstring gameDirectory;
std::wstring gameKey;
std::wstring gamePrefix;
std::wstring logPath;
std::wstring mergedDirectory;

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

std::wstring fromAnsi(const char* text)
{
    const int size = MultiByteToWideChar(CP_ACP, 0, text, -1, nullptr, 0);
    if (size <= 1) {
        return {};
    }
    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_ACP, 0, text, -1, result.data(), size);
    result.pop_back();
    return result;
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

std::wstring fullPath(const wchar_t* path)
{
    std::wstring result(MAX_PATH, L'\0');
    DWORD size = GetFullPathNameW(path, static_cast<DWORD>(result.size()), result.data(), nullptr);
    if (size > result.size()) {
        result.resize(size);
        size = GetFullPathNameW(path, size, result.data(), nullptr);
    }
    result.resize(size);
    return result;
}

bool hasExtension(const std::wstring& name, const wchar_t* extension)
{
    const size_t suffix = wcslen(extension);
    return name.size() > suffix && _wcsicmp(name.c_str() + name.size() - suffix, extension) == 0;
}

bool wildcard(const std::wstring& text, const std::wstring& mask)
{
    size_t t = 0;
    size_t m = 0;
    size_t star = std::wstring::npos;
    size_t resume = 0;
    while (t < text.size()) {
        if (m < mask.size() && (mask[m] == L'?' || mask[m] == text[t])) {
            ++t;
            ++m;
        } else if (m < mask.size() && mask[m] == L'*') {
            star = m++;
            resume = t;
        } else if (star != std::wstring::npos) {
            m = star + 1;
            t = ++resume;
        } else {
            return false;
        }
    }
    while (m < mask.size() && mask[m] == L'*') {
        ++m;
    }
    return m == mask.size();
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

void collectFiles(const std::wstring& root, const std::wstring& relative, std::vector<std::wstring>& found)
{
    const std::wstring folder = relative.empty() ? root : root + L"\\" + relative;
    for (const auto& name : listDirectory(folder, false)) {
        found.push_back(relative.empty() ? name : relative + L"\\" + name);
    }
    for (const auto& name : listDirectory(folder, true)) {
        collectFiles(root, relative.empty() ? name : relative + L"\\" + name, found);
    }
}

bool isModDocument(const std::wstring& relative)
{
    const std::wstring name = lower(relative);
    return name == L"mod.json" ||
           (name.find(L'\\') == std::wstring::npos && (hasExtension(name, L".md") || hasExtension(name, L".txt")));
}

struct Json {
    enum class Kind { Null, Boolean, Number, String, Array, Object };
    Kind kind = Kind::Null;
    std::string text;
    std::vector<std::string> keys;
    std::vector<Json> items;
};

class Parser {
public:
    explicit Parser(const std::string& text) : text_(text) {}

    std::optional<Json> parse(std::string& error)
    {
        if (text_.compare(0, 3, "\xEF\xBB\xBF") == 0) {
            at_ = 3;
        }
        Json result;
        if (value(result, 0)) {
            skip();
            if (at_ == text_.size()) {
                return result;
            }
            fail("unexpected text after the end");
        }
        error = error_;
        return std::nullopt;
    }

private:
    const std::string& text_;
    size_t at_ = 0;
    std::string error_;

    bool fail(const char* what)
    {
        size_t line = 1;
        size_t column = 1;
        for (size_t i = 0; i < at_ && i < text_.size(); ++i) {
            if (text_[i] == '\n') {
                ++line;
                column = 1;
            } else {
                ++column;
            }
        }
        error_ = std::string(what) + " at line " + std::to_string(line) + ", column " + std::to_string(column);
        return false;
    }

    bool more() const { return at_ < text_.size(); }

    void skip()
    {
        while (more()) {
            const char c = text_[at_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++at_;
            } else if (text_.compare(at_, 2, "//") == 0) {
                const size_t end = text_.find('\n', at_);
                at_ = end == std::string::npos ? text_.size() : end;
            } else if (text_.compare(at_, 2, "/*") == 0) {
                const size_t end = text_.find("*/", at_ + 2);
                at_ = end == std::string::npos ? text_.size() : end + 2;
            } else {
                return;
            }
        }
    }

    bool string(std::string& raw)
    {
        const size_t start = ++at_;
        while (more() && text_[at_] != '"') {
            at_ += text_[at_] == '\\' ? 2 : 1;
        }
        if (!more()) {
            return fail("a string never ends");
        }
        raw = text_.substr(start, at_ - start);
        ++at_;
        return true;
    }

    static bool numberCharacter(char c)
    {
        return (c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E';
    }

    bool value(Json& out, int depth)
    {
        if (depth > 256) {
            return fail("nesting is too deep");
        }
        skip();
        if (!more()) {
            return fail("the file ends where a value should be");
        }
        const char c = text_[at_];
        if (c == '{') {
            return object(out, depth);
        }
        if (c == '[') {
            return array(out, depth);
        }
        if (c == '"') {
            out.kind = Json::Kind::String;
            return string(out.text);
        }
        for (const char* word : {"true", "false", "null"}) {
            const size_t length = std::strlen(word);
            if (text_.compare(at_, length, word) == 0) {
                out.kind = word[0] == 'n' ? Json::Kind::Null : Json::Kind::Boolean;
                out.text = word;
                at_ += length;
                return true;
            }
        }
        const size_t start = at_;
        while (more() && numberCharacter(text_[at_])) {
            ++at_;
        }
        if (at_ == start) {
            return fail("unexpected character");
        }
        out.kind = Json::Kind::Number;
        out.text = text_.substr(start, at_ - start);
        return true;
    }

    bool object(Json& out, int depth)
    {
        out.kind = Json::Kind::Object;
        ++at_;
        for (;;) {
            skip();
            if (!more()) {
                return fail("an object never closes");
            }
            if (text_[at_] == '}') {
                ++at_;
                return true;
            }
            if (text_[at_] != '"') {
                return fail("expected a key in quotes");
            }
            std::string key;
            if (!string(key)) {
                return false;
            }
            skip();
            if (!more() || text_[at_] != ':') {
                return fail("expected a colon after the key");
            }
            ++at_;
            Json member;
            if (!value(member, depth + 1)) {
                return false;
            }
            out.keys.push_back(std::move(key));
            out.items.push_back(std::move(member));
            skip();
            if (more() && text_[at_] == ',') {
                ++at_;
            } else if (more() && text_[at_] == '}') {
                ++at_;
                return true;
            } else {
                return fail("expected a comma or a closing brace");
            }
        }
    }

    bool array(Json& out, int depth)
    {
        out.kind = Json::Kind::Array;
        ++at_;
        for (;;) {
            skip();
            if (!more()) {
                return fail("an array never closes");
            }
            if (text_[at_] == ']') {
                ++at_;
                return true;
            }
            Json item;
            if (!value(item, depth + 1)) {
                return false;
            }
            out.items.push_back(std::move(item));
            skip();
            if (more() && text_[at_] == ',') {
                ++at_;
            } else if (more() && text_[at_] == ']') {
                ++at_;
                return true;
            } else {
                return fail("expected a comma or a closing bracket");
            }
        }
    }
};

void writeCompact(const Json& value, std::string& out)
{
    switch (value.kind) {
    case Json::Kind::Array:
    case Json::Kind::Object: {
        const bool object = value.kind == Json::Kind::Object;
        out += object ? '{' : '[';
        for (size_t i = 0; i < value.items.size(); ++i) {
            if (i) {
                out += ", ";
            }
            if (object) {
                out += '"' + value.keys[i] + "\": ";
            }
            writeCompact(value.items[i], out);
        }
        out += object ? '}' : ']';
        return;
    }
    case Json::Kind::String:
        out += '"' + value.text + '"';
        return;
    default:
        out += value.text;
    }
}

std::string compact(const Json& value)
{
    std::string out;
    writeCompact(value, out);
    return out;
}

void writePretty(const Json& value, std::string& out, size_t depth)
{
    if ((value.kind != Json::Kind::Array && value.kind != Json::Kind::Object) || value.items.empty()) {
        writeCompact(value, out);
        return;
    }
    const bool object = value.kind == Json::Kind::Object;
    out += object ? "{\n" : "[\n";
    for (size_t i = 0; i < value.items.size(); ++i) {
        out.append(depth + 1, '\t');
        if (object) {
            out += '"' + value.keys[i] + "\": ";
        }
        writePretty(value.items[i], out, depth + 1);
        out += i + 1 < value.items.size() ? ",\n" : "\n";
    }
    out.append(depth, '\t');
    out += object ? '}' : ']';
}

struct Change {
    std::string path;
    std::string before;
    std::string after;
};

std::string shortened(std::string text)
{
    if (text.size() > 80) {
        text.resize(77);
        text += "...";
    }
    return text;
}

std::optional<size_t> lastKey(const Json& object, const std::string& key)
{
    for (size_t i = object.keys.size(); i > 0; --i) {
        if (object.keys[i - 1] == key) {
            return i - 1;
        }
    }
    return std::nullopt;
}

void merge(Json& target, const Json& patch, const std::string& path, std::vector<Change>& changes)
{
    if (patch.kind != Json::Kind::Object || target.kind != Json::Kind::Object) {
        const std::string before = compact(target);
        const std::string after = compact(patch);
        if (before != after) {
            changes.push_back({path, shortened(before), shortened(after)});
        }
        target = patch;
        return;
    }
    for (size_t i = 0; i < patch.keys.size(); ++i) {
        const std::string& key = patch.keys[i];
        const Json& value = patch.items[i];
        const std::string child = path.empty() ? key : path + "." + key;
        const std::optional<size_t> existing = lastKey(target, key);
        if (value.kind == Json::Kind::Null) {
            if (existing) {
                changes.push_back({child, shortened(compact(target.items[*existing])), std::string()});
                for (size_t j = target.keys.size(); j > 0; --j) {
                    if (target.keys[j - 1] == key) {
                        target.keys.erase(target.keys.begin() + static_cast<std::ptrdiff_t>(j - 1));
                        target.items.erase(target.items.begin() + static_cast<std::ptrdiff_t>(j - 1));
                    }
                }
            }
            continue;
        }
        if (!existing) {
            changes.push_back({child, std::string(), shortened(compact(value))});
            target.keys.push_back(key);
            target.items.push_back(value);
            continue;
        }
        merge(target.items[*existing], value, child, changes);
    }
}

std::wstring describe(const Change& change)
{
    const std::wstring path = change.path.empty() ? L"the whole file" : wide(change.path);
    if (change.before.empty()) {
        return path + L" added as " + wide(change.after) + L", the game's file has no such key";
    }
    if (change.after.empty()) {
        return path + L" removed, it was " + wide(change.before);
    }
    return path + L" " + wide(change.before) + L" to " + wide(change.after);
}

std::wstring textField(const Json& object, const char* key, const std::wstring& fallback)
{
    const std::optional<size_t> index = object.kind == Json::Kind::Object ? lastKey(object, key) : std::nullopt;
    if (index && object.items[*index].kind == Json::Kind::String && !object.items[*index].text.empty()) {
        return wide(object.items[*index].text);
    }
    return fallback;
}

std::optional<Json> parseFile(const std::wstring& path, std::string& error)
{
    const std::optional<std::string> text = readFile(path);
    if (!text) {
        error = "the file cannot be read";
        return std::nullopt;
    }
    return Parser(*text).parse(error);
}

struct Mod {
    std::wstring folder;
    std::wstring name;
    std::wstring version;
};

struct Patch {
    const Mod* mod;
    std::wstring file;
};

struct Redirect {
    std::wstring relative;
    std::wstring source;
    mutable std::atomic<int> reads{0};
};

struct Listing {
    std::wstring name;
    std::wstring source;
    bool directory;
};

std::vector<std::unique_ptr<Mod>> mods;
std::unordered_map<std::wstring, std::unique_ptr<Redirect>> redirects;
std::map<std::wstring, std::vector<Listing>> listings;
std::set<std::wstring> virtualDirectories;
std::atomic<bool> listingsActive{false};

bool insideGame(const std::wstring& key)
{
    return key.compare(0, gamePrefix.size(), gamePrefix) == 0;
}

void addRedirect(const std::wstring& key, const std::wstring& relative, const std::wstring& source)
{
    auto redirect = std::make_unique<Redirect>();
    redirect->relative = relative;
    redirect->source = source;
    redirects[key] = std::move(redirect);
}

void announce(const std::wstring& target, const std::wstring& source)
{
    std::wstring child = target;
    bool directory = false;
    for (;;) {
        const std::wstring parent = parentOf(child);
        const std::wstring parentKey = lower(parent);
        if (parentKey != gameKey && !insideGame(parentKey)) {
            return;
        }
        std::vector<Listing>& entries = listings[parentKey];
        const std::wstring name = nameOf(child);
        bool listed = false;
        for (const auto& entry : entries) {
            listed = listed || lower(entry.name) == lower(name);
        }
        if (!listed) {
            entries.push_back({name, directory ? std::wstring() : source, directory});
        }
        if (GetFileAttributesW(parent.c_str()) != INVALID_FILE_ATTRIBUTES) {
            return;
        }
        virtualDirectories.insert(parentKey);
        child = parent;
        directory = true;
    }
}

void buildFile(const std::wstring& key, const std::wstring& relative, const std::vector<Patch>& patches)
{
    const std::wstring target = gameDirectory + L"\\" + relative;
    const bool inGame = GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (!hasExtension(relative, L".json")) {
        const Patch& winner = patches.back();
        log(relative + (inGame ? L": replaced by " : L": added by ") + winner.mod->name);
        for (size_t i = 0; i + 1 < patches.size(); ++i) {
            log(L"  " + patches[i].mod->name + L": overridden by a later mod");
        }
        addRedirect(key, relative, winner.file);
        if (!inGame) {
            announce(target, winner.file);
        }
        return;
    }
    std::string error;
    std::optional<Json> merged;
    size_t next = 0;
    size_t applied = 0;
    if (inGame) {
        merged = parseFile(target, error);
        if (!merged) {
            log(relative + L": the game's file cannot be read (" + wide(error) + L"), left as it is");
            return;
        }
        log(relative + L":");
    } else {
        while (!merged && next < patches.size()) {
            const Patch& base = patches[next++];
            merged = parseFile(base.file, error);
            if (merged) {
                log(relative + L": added by " + base.mod->name);
                ++applied;
            } else {
                log(relative + L": " + base.mod->name + L" skipped, not valid JSON (" + wide(error) + L")");
            }
        }
        if (!merged) {
            return;
        }
    }
    for (; next < patches.size(); ++next) {
        const Patch& patch = patches[next];
        const std::optional<Json> json = parseFile(patch.file, error);
        if (!json) {
            log(L"  " + patch.mod->name + L": skipped, not valid JSON (" + wide(error) + L")");
            continue;
        }
        std::vector<Change> changes;
        merge(*merged, *json, std::string(), changes);
        ++applied;
        if (changes.empty()) {
            log(L"  " + patch.mod->name + L": nothing to change");
        }
        for (const auto& change : changes) {
            log(L"  " + patch.mod->name + L": " + describe(change));
        }
    }
    if (!applied) {
        return;
    }
    std::string bytes;
    writePretty(*merged, bytes, 0);
    bytes += "\n";
    const std::wstring path = mergedDirectory + L"\\" + relative;
    if (!writeFile(path, bytes)) {
        log(relative + L": cannot write " + path);
        return;
    }
    addRedirect(key, relative, path);
    if (!inGame) {
        announce(target, path);
    }
}

void loadMods()
{
    const std::wstring folder = gameDirectory + L"\\mods";
    std::map<std::wstring, std::vector<Patch>> patches;
    std::map<std::wstring, std::wstring> relatives;
    for (const auto& name : listDirectory(folder, true)) {
        if (lower(name) == L"nlse") {
            continue;
        }
        const std::wstring path = folder + L"\\" + name;
        const std::optional<std::string> manifestText = readFile(path + L"\\mod.json");
        if (!manifestText) {
            log(L"mods\\" + name + L" skipped, it has no mod.json");
            continue;
        }
        std::string error;
        const std::optional<Json> manifest = Parser(*manifestText).parse(error);
        if (!manifest) {
            log(L"mods\\" + name + L" skipped, mod.json is not valid JSON (" + wide(error) + L")");
            continue;
        }
        auto mod = std::make_unique<Mod>();
        mod->folder = name;
        mod->name = textField(*manifest, "name", name);
        mod->version = textField(*manifest, "version", L"");
        log(L"mod " + mod->name + (mod->version.empty() ? L"" : L" " + mod->version));
        std::vector<std::wstring> files;
        collectFiles(path, std::wstring(), files);
        for (const auto& relative : files) {
            if (isModDocument(relative)) {
                continue;
            }
            if (hasExtension(relative, L".csv")) {
                log(L"  " + relative + L" skipped, CSV is not supported yet");
                continue;
            }
            const std::wstring key = lower(gameDirectory + L"\\" + relative);
            patches[key].push_back({mod.get(), path + L"\\" + relative});
            relatives.emplace(key, relative);
        }
        mods.push_back(std::move(mod));
    }
    if (mods.empty()) {
        log(L"no mods found");
    }
    for (const auto& entry : patches) {
        buildFile(entry.first, relatives[entry.first], entry.second);
    }
}

std::optional<std::wstring> keyInGame(LPCWSTR name)
{
    if (!name || !*name) {
        return std::nullopt;
    }
    std::wstring key = lower(fullPath(name));
    while (key.size() > gamePrefix.size() && key.back() == L'\\') {
        key.pop_back();
    }
    if (!insideGame(key)) {
        return std::nullopt;
    }
    return key;
}

const Redirect* redirectAt(const std::wstring& key)
{
    const auto found = redirects.find(key);
    return found == redirects.end() ? nullptr : found->second.get();
}

const Redirect* redirectFor(LPCWSTR name)
{
    if (redirects.empty()) {
        return nullptr;
    }
    const std::optional<std::wstring> key = keyInGame(name);
    return key ? redirectAt(*key) : nullptr;
}

bool isVirtualDirectory(LPCWSTR name)
{
    if (virtualDirectories.empty()) {
        return false;
    }
    const std::optional<std::wstring> key = keyInGame(name);
    return key && virtualDirectories.count(*key) != 0;
}

WIN32_FIND_DATAW findData(const Listing& entry)
{
    WIN32_FIND_DATAW data{};
    WIN32_FILE_ATTRIBUTE_DATA information{};
    if (!entry.directory && GetFileAttributesExW(entry.source.c_str(), GetFileExInfoStandard, &information)) {
        data.dwFileAttributes = information.dwFileAttributes;
        data.ftCreationTime = information.ftCreationTime;
        data.ftLastAccessTime = information.ftLastAccessTime;
        data.ftLastWriteTime = information.ftLastWriteTime;
        data.nFileSizeHigh = information.nFileSizeHigh;
        data.nFileSizeLow = information.nFileSizeLow;
    } else {
        data.dwFileAttributes = entry.directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
        GetSystemTimeAsFileTime(&data.ftLastWriteTime);
        data.ftCreationTime = data.ftLastWriteTime;
        data.ftLastAccessTime = data.ftLastWriteTime;
    }
    wcsncpy_s(data.cFileName, entry.name.c_str(), _TRUNCATE);
    return data;
}

std::vector<WIN32_FIND_DATAW> listingFor(LPCWSTR pattern)
{
    if (!listingsActive || listings.empty()) {
        return {};
    }
    const std::optional<std::wstring> key = keyInGame(pattern);
    if (!key) {
        return {};
    }
    std::vector<WIN32_FIND_DATAW> entries;
    const size_t slash = key->find_last_of(L'\\');
    const auto found = listings.find(key->substr(0, slash));
    if (found != listings.end()) {
        std::wstring mask = key->substr(slash + 1);
        if (mask == L"*.*") {
            mask = L"*";
        }
        for (const auto& entry : found->second) {
            if (wildcard(lower(entry.name), mask)) {
                entries.push_back(findData(entry));
            }
        }
    }
    return entries;
}

struct Search {
    HANDLE real = INVALID_HANDLE_VALUE;
    bool realDone = false;
    std::vector<WIN32_FIND_DATAW> extra;
    size_t next = 0;
};

std::mutex searchesMutex;
std::set<Search*> searches;

Search* searchFor(HANDLE handle)
{
    std::lock_guard<std::mutex> lock(searchesMutex);
    const auto found = searches.find(reinterpret_cast<Search*>(handle));
    return found == searches.end() ? nullptr : *found;
}

using CreateFileWFunction = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using GetFileAttributesWFunction = DWORD(WINAPI*)(LPCWSTR);
using GetFileAttributesAFunction = DWORD(WINAPI*)(LPCSTR);
using GetFileAttributesExWFunction = BOOL(WINAPI*)(LPCWSTR, GET_FILEEX_INFO_LEVELS, LPVOID);
using FindFirstFileWFunction = HANDLE(WINAPI*)(LPCWSTR, LPWIN32_FIND_DATAW);
using FindFirstFileExWFunction = HANDLE(WINAPI*)(LPCWSTR, FINDEX_INFO_LEVELS, LPVOID, FINDEX_SEARCH_OPS, LPVOID,
                                                 DWORD);
using FindNextFileWFunction = BOOL(WINAPI*)(HANDLE, LPWIN32_FIND_DATAW);
using FindCloseFunction = BOOL(WINAPI*)(HANDLE);

CreateFileWFunction originalCreateFileW;
GetFileAttributesWFunction originalGetFileAttributesW;
GetFileAttributesAFunction originalGetFileAttributesA;
GetFileAttributesExWFunction originalGetFileAttributesExW;
FindFirstFileWFunction originalFindFirstFileW;
FindFirstFileExWFunction originalFindFirstFileExW;
FindNextFileWFunction originalFindNextFileW;
FindCloseFunction originalFindClose;

HANDLE wrapSearch(HANDLE real, std::vector<WIN32_FIND_DATAW> extra, LPWIN32_FIND_DATAW data)
{
    try {
        auto search = std::make_unique<Search>();
        search->real = real;
        search->extra = std::move(extra);
        if (real == INVALID_HANDLE_VALUE) {
            search->realDone = true;
            *data = search->extra[search->next++];
        }
        std::lock_guard<std::mutex> lock(searchesMutex);
        searches.insert(search.get());
        return reinterpret_cast<HANDLE>(search.release());
    } catch (...) {
        return real;
    }
}

constexpr DWORD writeAccess = GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA |
                              FILE_WRITE_ATTRIBUTES | FILE_WRITE_EA | DELETE;

HANDLE WINAPI hookedCreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security,
                                DWORD disposition, DWORD flags, HANDLE templateFile)
{
    try {
        const Redirect* redirect = redirectFor(name);
        if (redirect && !(access & writeAccess) && (disposition == OPEN_EXISTING || disposition == OPEN_ALWAYS)) {
            if (redirect->reads.fetch_add(1) == 0) {
                log(L"the game read " + redirect->relative);
            }
            return originalCreateFileW(redirect->source.c_str(), access, share, security, OPEN_EXISTING, flags,
                                       templateFile);
        }
    } catch (...) {
    }
    return originalCreateFileW(name, access, share, security, disposition, flags, templateFile);
}

DWORD WINAPI hookedGetFileAttributesW(LPCWSTR name)
{
    try {
        if (const Redirect* redirect = redirectFor(name)) {
            return originalGetFileAttributesW(redirect->source.c_str());
        }
        if (isVirtualDirectory(name)) {
            return FILE_ATTRIBUTE_DIRECTORY;
        }
    } catch (...) {
    }
    return originalGetFileAttributesW(name);
}

DWORD WINAPI hookedGetFileAttributesA(LPCSTR name)
{
    try {
        if (name && (!redirects.empty() || !virtualDirectories.empty())) {
            const std::wstring path = fromAnsi(name);
            if (const Redirect* redirect = redirectFor(path.c_str())) {
                return GetFileAttributesW(redirect->source.c_str());
            }
            if (isVirtualDirectory(path.c_str())) {
                return FILE_ATTRIBUTE_DIRECTORY;
            }
        }
    } catch (...) {
    }
    return originalGetFileAttributesA(name);
}

BOOL WINAPI hookedGetFileAttributesExW(LPCWSTR name, GET_FILEEX_INFO_LEVELS level, LPVOID information)
{
    try {
        if (const Redirect* redirect = redirectFor(name)) {
            return originalGetFileAttributesExW(redirect->source.c_str(), level, information);
        }
        if (level == GetFileExInfoStandard && information && isVirtualDirectory(name)) {
            auto* data = static_cast<WIN32_FILE_ATTRIBUTE_DATA*>(information);
            *data = {};
            data->dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
            GetSystemTimeAsFileTime(&data->ftLastWriteTime);
            data->ftCreationTime = data->ftLastWriteTime;
            data->ftLastAccessTime = data->ftLastWriteTime;
            return TRUE;
        }
    } catch (...) {
    }
    return originalGetFileAttributesExW(name, level, information);
}

HANDLE WINAPI hookedFindFirstFileW(LPCWSTR pattern, LPWIN32_FIND_DATAW data)
{
    std::vector<WIN32_FIND_DATAW> extra;
    try {
        extra = listingFor(pattern);
    } catch (...) {
    }
    const HANDLE real = originalFindFirstFileW(pattern, data);
    return extra.empty() ? real : wrapSearch(real, std::move(extra), data);
}

HANDLE WINAPI hookedFindFirstFileExW(LPCWSTR pattern, FINDEX_INFO_LEVELS level, LPVOID data, FINDEX_SEARCH_OPS search,
                                     LPVOID filter, DWORD flags)
{
    std::vector<WIN32_FIND_DATAW> extra;
    try {
        extra = listingFor(pattern);
    } catch (...) {
    }
    const HANDLE real = originalFindFirstFileExW(pattern, level, data, search, filter, flags);
    return extra.empty() ? real : wrapSearch(real, std::move(extra), static_cast<LPWIN32_FIND_DATAW>(data));
}

BOOL WINAPI hookedFindNextFileW(HANDLE handle, LPWIN32_FIND_DATAW data)
{
    Search* search = searchFor(handle);
    if (!search) {
        return originalFindNextFileW(handle, data);
    }
    if (!search->realDone) {
        if (originalFindNextFileW(search->real, data)) {
            return TRUE;
        }
        if (GetLastError() != ERROR_NO_MORE_FILES) {
            return FALSE;
        }
        search->realDone = true;
    }
    if (search->next < search->extra.size()) {
        *data = search->extra[search->next++];
        return TRUE;
    }
    SetLastError(ERROR_NO_MORE_FILES);
    return FALSE;
}

BOOL WINAPI hookedFindClose(HANDLE handle)
{
    Search* search = nullptr;
    {
        std::lock_guard<std::mutex> lock(searchesMutex);
        const auto found = searches.find(reinterpret_cast<Search*>(handle));
        if (found != searches.end()) {
            search = *found;
            searches.erase(found);
        }
    }
    if (!search) {
        return originalFindClose(handle);
    }
    if (search->real != INVALID_HANDLE_VALUE) {
        originalFindClose(search->real);
    }
    delete search;
    return TRUE;
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

struct Hook {
    const char* function;
    void* replacement;
    void** original;
};

std::wstring attach(HMODULE game, const Hook* hooks, size_t count)
{
    std::wstring missing;
    for (size_t i = 0; i < count; ++i) {
        if (!replaceImport(game, "KERNEL32.dll", hooks[i].function, hooks[i].replacement, hooks[i].original)) {
            missing += (missing.empty() ? L"" : L", ") + wide(hooks[i].function);
        }
    }
    return missing;
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
    gameKey = lower(gameDirectory);
    gamePrefix = gameKey + L"\\";
    const std::wstring logFolder = environment(L"LOCALAPPDATA") + L"\\Strategy";
    logPath = logFolder + L"\\nlse.log";
    MoveFileExW(logPath.c_str(), (logFolder + L"\\nlse.previous.log").c_str(), MOVEFILE_REPLACE_EXISTING);
    wchar_t temp[MAX_PATH + 1] = {};
    GetTempPathW(MAX_PATH + 1, temp);
    mergedDirectory = std::wstring(temp) + L"nlse";
    const HMODULE game = GetModuleHandleW(nullptr);
    gameBase = reinterpret_cast<uintptr_t>(game);
    gameVersion = fileVersion(game);

    log(L"NLSE " + std::wstring(version) + L" from " + loaderFile + L", started by " + parent);
    log(L"Norland " + versionText(gameVersion));
    if (!inGameFolder && GetFileAttributesW((gameDirectory + L"\\winmm.dll").c_str()) != INVALID_FILE_ATTRIBUTES) {
        log(L"winmm.dll in the game folder is not needed with MO2");
    }
    loadMods();

    const Hook files[] = {
        {"CreateFileW", reinterpret_cast<void*>(&hookedCreateFileW), reinterpret_cast<void**>(&originalCreateFileW)},
        {"GetFileAttributesW", reinterpret_cast<void*>(&hookedGetFileAttributesW),
         reinterpret_cast<void**>(&originalGetFileAttributesW)},
        {"GetFileAttributesA", reinterpret_cast<void*>(&hookedGetFileAttributesA),
         reinterpret_cast<void**>(&originalGetFileAttributesA)},
        {"GetFileAttributesExW", reinterpret_cast<void*>(&hookedGetFileAttributesExW),
         reinterpret_cast<void**>(&originalGetFileAttributesExW)},
    };
    const Hook folders[] = {
        {"FindNextFileW", reinterpret_cast<void*>(&hookedFindNextFileW),
         reinterpret_cast<void**>(&originalFindNextFileW)},
        {"FindClose", reinterpret_cast<void*>(&hookedFindClose), reinterpret_cast<void**>(&originalFindClose)},
        {"FindFirstFileW", reinterpret_cast<void*>(&hookedFindFirstFileW),
         reinterpret_cast<void**>(&originalFindFirstFileW)},
        {"FindFirstFileExW", reinterpret_cast<void*>(&hookedFindFirstFileExW),
         reinterpret_cast<void**>(&originalFindFirstFileExW)},
    };
    const std::wstring missingFiles = attach(game, files, sizeof(files) / sizeof(files[0]));
    const std::wstring missingFolders = attach(game, folders, sizeof(folders) / sizeof(folders[0]));
    listingsActive = missingFolders.empty();
    if (!missingFiles.empty()) {
        log(L"could not hook " + missingFiles + L", mods will not apply");
    }
    if (!missingFolders.empty()) {
        log(L"could not hook " + missingFolders + L", added files will not show up");
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
