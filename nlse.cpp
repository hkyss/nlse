#define NOMINMAX
#include <windows.h>
#include <intrin.h>
#include <tlhelp32.h>
#include <wincodec.h>
#include <wrl/client.h>

#include "nlse.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "windowscodecs.lib")

namespace {

constexpr wchar_t version[] = L"0.3.0";

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

class BitReader {
public:
    BitReader(const BYTE* data, size_t size) : data_(data), size_(size) {}

    uint32_t bits(int count)
    {
        while (available_ < count) {
            buffer_ = (buffer_ << 8) | (at_ < size_ ? data_[at_] : 0);
            ++at_;
            available_ += 8;
        }
        available_ -= count;
        return static_cast<uint32_t>((buffer_ >> available_) & ((1ull << count) - 1));
    }

    bool bit() { return bits(1) != 0; }
    bool overrun() const { return at_ > size_; }

private:
    const BYTE* data_;
    size_t size_;
    size_t at_ = 0;
    uint64_t buffer_ = 0;
    int available_ = 0;
};

struct HuffmanTable {
    int32_t limit[24] = {};
    int32_t base[24] = {};
    int32_t symbols[258] = {};
    int shortest = 0;
};

bool buildHuffmanTable(const BYTE* lengths, int alphabet, HuffmanTable& table)
{
    int longest = 0;
    table.shortest = 32;
    for (int symbol = 0; symbol < alphabet; ++symbol) {
        table.shortest = lengths[symbol] < table.shortest ? lengths[symbol] : table.shortest;
        longest = lengths[symbol] > longest ? lengths[symbol] : longest;
    }
    int next = 0;
    for (int length = table.shortest; length <= longest; ++length) {
        for (int symbol = 0; symbol < alphabet; ++symbol) {
            if (lengths[symbol] == length) {
                table.symbols[next++] = symbol;
            }
        }
    }
    int32_t counts[24] = {};
    for (int symbol = 0; symbol < alphabet; ++symbol) {
        ++counts[lengths[symbol] + 1];
    }
    for (int length = 1; length < 24; ++length) {
        counts[length] += counts[length - 1];
    }
    int32_t code = 0;
    for (int length = table.shortest; length <= longest; ++length) {
        code += counts[length + 1] - counts[length];
        table.limit[length] = code - 1;
        code <<= 1;
    }
    for (int length = table.shortest; length <= longest; ++length) {
        table.base[length] = length == table.shortest ? counts[length] : ((table.limit[length - 1] + 1) << 1) - counts[length];
    }
    return table.shortest >= 1;
}

int decodeSymbol(BitReader& in, const HuffmanTable& table)
{
    int length = table.shortest;
    int32_t code = static_cast<int32_t>(in.bits(length));
    while (code > table.limit[length]) {
        if (++length > 20) {
            return -1;
        }
        code = (code << 1) | static_cast<int32_t>(in.bits(1));
    }
    const int32_t index = code - table.base[length];
    return index >= 0 && index < 258 ? table.symbols[index] : -1;
}

constexpr uint16_t bzipRandom[512] = {
    619, 720, 127, 481, 931, 816, 813, 233, 566, 247, 985, 724, 205, 454, 863, 491,
    741, 242, 949, 214, 733, 859, 335, 708, 621, 574, 73,  654, 730, 472, 419, 436,
    278, 496, 867, 210, 399, 680, 480, 51,  878, 465, 811, 169, 869, 675, 611, 697,
    867, 561, 862, 687, 507, 283, 482, 129, 807, 591, 733, 623, 150, 238, 59,  379,
    684, 877, 625, 169, 643, 105, 170, 607, 520, 932, 727, 476, 693, 425, 174, 647,
    73,  122, 335, 530, 442, 853, 695, 249, 445, 515, 909, 545, 703, 919, 874, 474,
    882, 500, 594, 612, 641, 801, 220, 162, 819, 984, 589, 513, 495, 799, 161, 604,
    958, 533, 221, 400, 386, 867, 600, 782, 382, 596, 414, 171, 516, 375, 682, 485,
    911, 276, 98,  553, 163, 354, 666, 933, 424, 341, 533, 870, 227, 730, 475, 186,
    263, 647, 537, 686, 600, 224, 469, 68,  770, 919, 190, 373, 294, 822, 808, 206,
    184, 943, 795, 384, 383, 461, 404, 758, 839, 887, 715, 67,  618, 276, 204, 918,
    873, 777, 604, 560, 951, 160, 578, 722, 79,  804, 96,  409, 713, 940, 652, 934,
    970, 447, 318, 353, 859, 672, 112, 785, 645, 863, 803, 350, 139, 93,  354, 99,
    820, 908, 609, 772, 154, 274, 580, 184, 79,  626, 630, 742, 653, 282, 762, 623,
    680, 81,  927, 626, 789, 125, 411, 521, 938, 300, 821, 78,  343, 175, 128, 250,
    170, 774, 972, 275, 999, 639, 495, 78,  352, 126, 857, 956, 358, 619, 580, 124,
    737, 594, 701, 612, 669, 112, 134, 694, 363, 992, 809, 743, 168, 974, 944, 375,
    748, 52,  600, 747, 642, 182, 862, 81,  344, 805, 988, 739, 511, 655, 814, 334,
    249, 515, 897, 955, 664, 981, 649, 113, 974, 459, 893, 228, 433, 837, 553, 268,
    926, 240, 102, 654, 459, 51,  686, 754, 806, 760, 493, 403, 415, 394, 687, 700,
    946, 670, 656, 610, 738, 392, 760, 799, 887, 653, 978, 321, 576, 617, 626, 502,
    894, 679, 243, 440, 680, 879, 194, 572, 640, 724, 926, 56,  204, 700, 707, 151,
    457, 449, 797, 195, 791, 558, 945, 679, 297, 59,  87,  824, 713, 663, 412, 693,
    342, 606, 134, 108, 571, 364, 631, 212, 174, 643, 304, 329, 343, 97,  430, 751,
    497, 314, 983, 374, 822, 928, 140, 206, 73,  263, 980, 736, 876, 478, 430, 305,
    170, 514, 364, 692, 829, 82,  855, 953, 676, 246, 369, 970, 294, 750, 807, 827,
    150, 790, 288, 923, 804, 378, 215, 828, 592, 281, 565, 555, 710, 82,  896, 831,
    547, 261, 524, 462, 293, 465, 502, 56,  661, 821, 976, 991, 658, 869, 905, 758,
    745, 193, 768, 550, 608, 933, 378, 286, 215, 979, 792, 961, 61,  688, 793, 644,
    986, 403, 106, 366, 905, 644, 372, 567, 466, 434, 645, 210, 389, 550, 919, 135,
    780, 773, 635, 389, 707, 100, 626, 958, 165, 504, 920, 176, 193, 713, 857, 265,
    203, 50,  668, 108, 645, 990, 626, 197, 510, 357, 358, 850, 858, 364, 936, 638,
};

uint32_t bzipCrc(uint32_t crc, BYTE value)
{
    static const auto table = [] {
        std::vector<uint32_t> entries(256);
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t entry = i << 24;
            for (int bit = 0; bit < 8; ++bit) {
                entry = entry & 0x80000000 ? (entry << 1) ^ 0x04C11DB7 : entry << 1;
            }
            entries[i] = entry;
        }
        return entries;
    }();
    return (crc << 8) ^ table[(crc >> 24) ^ value];
}

std::optional<std::string> bunzip(const BYTE* data, size_t size, size_t expected)
{
    BitReader in(data, size);
    if (size < 4 || in.bits(8) != 'B' || in.bits(8) != 'Z' || in.bits(8) != 'h') {
        return std::nullopt;
    }
    const uint32_t level = in.bits(8);
    if (level < '1' || level > '9') {
        return std::nullopt;
    }
    const size_t blockLimit = (level - '0') * 100000;
    std::vector<uint32_t> block(blockLimit);
    std::string out;
    out.reserve(expected);
    uint32_t streamCrc = 0;
    for (;;) {
        const uint32_t magicHigh = in.bits(24);
        const uint32_t magicLow = in.bits(24);
        const uint32_t crcHigh = in.bits(16);
        const uint32_t crcLow = in.bits(16);
        const uint32_t storedCrc = crcHigh << 16 | crcLow;
        if (magicHigh == 0x177245 && magicLow == 0x385090) {
            if (storedCrc != streamCrc || in.overrun()) {
                return std::nullopt;
            }
            return out;
        }
        if (magicHigh != 0x314159 || magicLow != 0x265359) {
            return std::nullopt;
        }
        const uint32_t blockCrc = storedCrc;
        const bool randomised = in.bit();
        const uint32_t origin = in.bits(24);
        BYTE used[256];
        int usedCount = 0;
        const uint32_t ranges = in.bits(16);
        for (int range = 0; range < 16; ++range) {
            if (ranges & (0x8000u >> range)) {
                const uint32_t values = in.bits(16);
                for (int value = 0; value < 16; ++value) {
                    if (values & (0x8000u >> value)) {
                        used[usedCount++] = static_cast<BYTE>(range * 16 + value);
                    }
                }
            }
        }
        const int alphabet = usedCount + 2;
        const int groups = static_cast<int>(in.bits(3));
        const uint32_t selectorCount = in.bits(15);
        if (!usedCount || groups < 2 || groups > 6 || !selectorCount) {
            return std::nullopt;
        }
        std::vector<BYTE> selectors(selectorCount);
        BYTE order[6] = {0, 1, 2, 3, 4, 5};
        for (auto& selector : selectors) {
            int position = 0;
            while (in.bit()) {
                if (++position >= groups) {
                    return std::nullopt;
                }
            }
            const BYTE chosen = order[position];
            for (; position > 0; --position) {
                order[position] = order[position - 1];
            }
            order[0] = chosen;
            selector = chosen;
        }
        HuffmanTable tables[6];
        for (int group = 0; group < groups; ++group) {
            BYTE lengths[258];
            int length = static_cast<int>(in.bits(5));
            for (int symbol = 0; symbol < alphabet; ++symbol) {
                for (;;) {
                    if (length < 1 || length > 20) {
                        return std::nullopt;
                    }
                    if (!in.bit()) {
                        break;
                    }
                    length += in.bit() ? -1 : 1;
                }
                lengths[symbol] = static_cast<BYTE>(length);
            }
            if (!buildHuffmanTable(lengths, alphabet, tables[group])) {
                return std::nullopt;
            }
        }
        BYTE recent[256];
        for (int i = 0; i < 256; ++i) {
            recent[i] = static_cast<BYTE>(i);
        }
        uint32_t counts[256] = {};
        size_t length = 0;
        size_t selector = 0;
        int left = 0;
        const HuffmanTable* table = nullptr;
        const auto next = [&]() {
            if (!left) {
                if (selector >= selectors.size()) {
                    return -1;
                }
                table = &tables[selectors[selector++]];
                left = 50;
            }
            --left;
            return decodeSymbol(in, *table);
        };
        int symbol = next();
        while (symbol != alphabet - 1) {
            if (symbol < 0) {
                return std::nullopt;
            }
            if (symbol <= 1) {
                size_t run = 0;
                size_t weight = 1;
                do {
                    if (weight > blockLimit) {
                        return std::nullopt;
                    }
                    run += symbol == 0 ? weight : weight * 2;
                    weight <<= 1;
                    symbol = next();
                } while (symbol == 0 || symbol == 1);
                const BYTE value = used[recent[0]];
                if (run > blockLimit - length) {
                    return std::nullopt;
                }
                counts[value] += static_cast<uint32_t>(run);
                for (; run > 0; --run) {
                    block[length++] = value;
                }
                continue;
            }
            const int position = symbol - 1;
            const BYTE index = recent[position];
            std::memmove(recent + 1, recent, static_cast<size_t>(position));
            recent[0] = index;
            if (length >= blockLimit) {
                return std::nullopt;
            }
            ++counts[used[index]];
            block[length++] = used[index];
            symbol = next();
        }
        if (origin >= length) {
            return std::nullopt;
        }
        uint32_t starts[256];
        uint32_t total = 0;
        for (int value = 0; value < 256; ++value) {
            starts[value] = total;
            total += counts[value];
        }
        for (size_t i = 0; i < length; ++i) {
            block[starts[block[i] & 0xFF]++] |= static_cast<uint32_t>(i << 8);
        }
        uint32_t crc = 0xFFFFFFFF;
        uint32_t position = block[origin] >> 8;
        int last = -1;
        int repeated = 0;
        int randomLeft = 0;
        size_t randomAt = 0;
        for (size_t i = 0; i < length; ++i) {
            if (position >= length) {
                return std::nullopt;
            }
            position = block[position];
            BYTE value = static_cast<BYTE>(position & 0xFF);
            position >>= 8;
            if (randomised) {
                if (!randomLeft) {
                    randomLeft = bzipRandom[randomAt];
                    randomAt = (randomAt + 1) % 512;
                }
                --randomLeft;
                value = static_cast<BYTE>(value ^ (randomLeft == 1 ? 1 : 0));
            }
            if (repeated == 4) {
                out.append(value, static_cast<char>(last));
                for (int copy = 0; copy < value; ++copy) {
                    crc = bzipCrc(crc, static_cast<BYTE>(last));
                }
                repeated = 0;
                continue;
            }
            out += static_cast<char>(value);
            crc = bzipCrc(crc, value);
            if (value == last) {
                ++repeated;
            } else {
                last = value;
                repeated = 1;
            }
        }
        if (~crc != blockCrc) {
            return std::nullopt;
        }
        streamCrc = ((streamCrc << 1) | (streamCrc >> 31)) ^ blockCrc;
    }
}

struct Image {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<BYTE> pixels;
};

std::optional<Image> decodeQoi(const BYTE* data, size_t size)
{
    if (size < 12 || std::memcmp(data, "fioq", 4) != 0) {
        return std::nullopt;
    }
    uint32_t length = 0;
    std::memcpy(&length, data + 8, sizeof(length));
    if (length > size - 12) {
        return std::nullopt;
    }
    Image image;
    image.width = static_cast<uint32_t>(data[4] | data[5] << 8);
    image.height = static_cast<uint32_t>(data[6] | data[7] << 8);
    image.pixels.resize(static_cast<size_t>(image.width) * image.height * 4);
    const BYTE* at = data + 12;
    const BYTE* end = at + length;
    const auto next = [&]() { return at < end ? *at++ : 0; };
    const auto add = [](BYTE& channel, int field, int bits) {
        channel = static_cast<BYTE>(channel + (field >= 1 << (bits - 1) ? field - (1 << bits) : field));
    };
    BYTE index[64][4] = {};
    BYTE pixel[4] = {0, 0, 0, 255};
    uint32_t run = 0;
    for (size_t position = 0; position < image.pixels.size(); position += 4) {
        if (run) {
            --run;
        } else if (at < end) {
            const int b1 = *at++;
            if ((b1 & 0xC0) == 0x00) {
                std::memcpy(pixel, index[b1 & 0x3F], 4);
            } else if ((b1 & 0xE0) == 0x40) {
                run = b1 & 0x1F;
            } else if ((b1 & 0xE0) == 0x60) {
                run = static_cast<uint32_t>(((b1 & 0x1F) << 8) | next()) + 32;
            } else if ((b1 & 0xC0) == 0x80) {
                add(pixel[0], (b1 >> 4) & 3, 2);
                add(pixel[1], (b1 >> 2) & 3, 2);
                add(pixel[2], b1 & 3, 2);
            } else if ((b1 & 0xE0) == 0xC0) {
                const int b2 = next();
                add(pixel[0], b1 & 0x1F, 5);
                add(pixel[1], b2 >> 4, 4);
                add(pixel[2], b2 & 0x0F, 4);
            } else if ((b1 & 0xF0) == 0xE0) {
                const int b2 = next();
                const int b3 = next();
                add(pixel[0], ((b1 & 0x0F) << 1) | (b2 >> 7), 5);
                add(pixel[1], (b2 >> 2) & 0x1F, 5);
                add(pixel[2], ((b2 & 0x03) << 3) | (b3 >> 5), 5);
                add(pixel[3], b3 & 0x1F, 5);
            } else {
                for (int channel = 0; channel < 4; ++channel) {
                    if (b1 & (8 >> channel)) {
                        pixel[channel] = static_cast<BYTE>(next());
                    }
                }
            }
            std::memcpy(index[(pixel[0] ^ pixel[1] ^ pixel[2] ^ pixel[3]) & 63], pixel, 4);
        }
        std::memcpy(&image.pixels[position], pixel, 4);
    }
    if (at != end) {
        return std::nullopt;
    }
    return image;
}

std::string encodeQoi(const Image& image)
{
    std::string out(12, '\0');
    std::memcpy(out.data(), "fioq", 4);
    out[4] = static_cast<char>(image.width & 0xFF);
    out[5] = static_cast<char>(image.width >> 8);
    out[6] = static_cast<char>(image.height & 0xFF);
    out[7] = static_cast<char>(image.height >> 8);
    out.reserve(image.pixels.size() / 2);
    const auto put = [&out](int value) { out += static_cast<char>(value & 0xFF); };
    BYTE index[64][4] = {};
    BYTE previous[4] = {0, 0, 0, 255};
    uint32_t run = 0;
    const size_t count = image.pixels.size();
    for (size_t position = 0; position < count; position += 4) {
        const BYTE* pixel = &image.pixels[position];
        const bool same = std::memcmp(pixel, previous, 4) == 0;
        if (same) {
            ++run;
        }
        if (run && (run == 0x2020 || !same || position + 4 == count)) {
            if (run < 33) {
                put(0x40 | static_cast<int>(run - 1));
            } else {
                const int value = static_cast<int>(run - 33);
                put(0x60 | value >> 8);
                put(value);
            }
            run = 0;
        }
        if (!same) {
            const int hash = (pixel[0] ^ pixel[1] ^ pixel[2] ^ pixel[3]) & 63;
            if (std::memcmp(index[hash], pixel, 4) == 0) {
                put(hash);
            } else {
                std::memcpy(index[hash], pixel, 4);
                const int dr = pixel[0] - previous[0];
                const int dg = pixel[1] - previous[1];
                const int db = pixel[2] - previous[2];
                const int da = pixel[3] - previous[3];
                const auto within = [](int value, int low, int high) { return value > low && value < high; };
                if (within(dr, -17, 16) && within(dg, -17, 16) && within(db, -17, 16) && within(da, -17, 16)) {
                    if (!da && within(dr, -3, 2) && within(dg, -3, 2) && within(db, -3, 2)) {
                        put(0x80 | (dr & 3) << 4 | (dg & 3) << 2 | (db & 3));
                    } else if (!da && within(dg, -9, 8) && within(db, -9, 8)) {
                        put(0xC0 | (dr & 0x1F));
                        put((dg & 0x0F) << 4 | (db & 0x0F));
                    } else {
                        put(0xE0 | (dr & 0x1F) >> 1);
                        put((dr & 1) << 7 | (dg & 0x1F) << 2 | (db & 0x1F) >> 3);
                        put((db & 7) << 5 | (da & 0x1F));
                    }
                } else {
                    put(0xF0 | (dr ? 8 : 0) | (dg ? 4 : 0) | (db ? 2 : 0) | (da ? 1 : 0));
                    for (int channel = 0; channel < 4; ++channel) {
                        if (pixel[channel] != previous[channel]) {
                            put(pixel[channel]);
                        }
                    }
                }
            }
        }
        std::memcpy(previous, pixel, 4);
    }
    const auto length = static_cast<uint32_t>(out.size() - 12);
    std::memcpy(&out[8], &length, sizeof(length));
    return out;
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

struct SpriteFile {
    const Mod* mod;
    std::wstring file;
};

std::vector<std::unique_ptr<Mod>> mods;
std::unordered_map<std::wstring, std::unique_ptr<Redirect>> redirects;
std::map<std::wstring, std::vector<Listing>> listings;
std::set<std::wstring> virtualDirectories;
std::atomic<bool> listingsActive{false};
std::map<std::pair<std::string, uint32_t>, std::vector<SpriteFile>> spriteFiles;
std::map<std::string, std::vector<SpriteFile>> fontFiles;

std::optional<std::string> fontOf(const std::wstring& relative)
{
    const std::wstring name = lower(relative);
    if (name.compare(0, 6, L"fonts\\") != 0 || name.find(L'\\', 6) != std::wstring::npos ||
        !(hasExtension(name, L".ttf") || hasExtension(name, L".otf")) ||
        GetFileAttributesW((gameDirectory + L"\\" + relative).c_str()) != INVALID_FILE_ATTRIBUTES) {
        return std::nullopt;
    }
    return utf8(name.substr(6, name.size() - 10));
}

bool isSpriteFile(const std::wstring& relative)
{
    return lower(relative).compare(0, 8, L"sprites\\") == 0;
}

std::optional<std::pair<std::string, uint32_t>> spriteOf(const std::wstring& relative)
{
    const std::wstring name = lower(relative);
    if (!isSpriteFile(name) || !hasExtension(name, L".png") || name.size() <= 12) {
        return std::nullopt;
    }
    const std::wstring stem = name.substr(8, name.size() - 12);
    const size_t slash = stem.find(L'\\');
    if (slash == std::wstring::npos) {
        return std::make_pair(utf8(stem), 0u);
    }
    const std::wstring frame = stem.substr(slash + 1);
    if (!slash || frame.empty() || frame.size() > 4 || frame.find_first_not_of(L"0123456789") != std::wstring::npos) {
        return std::nullopt;
    }
    return std::make_pair(utf8(stem.substr(0, slash)), static_cast<uint32_t>(std::stoul(frame)));
}

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

enum class CodeChange { Value, Skip, Call, Member, Read, New, Picture };

constexpr uint32_t valueReal = 0;
constexpr uint32_t valueWhole = 10;
constexpr uint32_t valueTruth = 13;

struct CodeEdit {
    const Mod* mod;
    std::string function;
    CodeChange change;
    Json find;
    Json set;
    std::string name;
    std::string to;
    uint32_t nth;
    uint32_t count;
    std::string label;
    uint32_t at = 0;
    bool pin = false;
    Json align;
    Json margin;
    Json size;
    double color = 16777215;
    double alpha = 1;
};

std::vector<CodeEdit> codeEdits;

const std::array<const char*, 3> acrossWords = {"left", "center", "right"};
const std::array<const char*, 3> downWords = {"top", "middle", "bottom"};

int wordIn(const Json& value, const std::array<const char*, 3>& words)
{
    for (int i = 0; value.kind == Json::Kind::String && i < 3; ++i) {
        if (value.text == words[i]) {
            return i;
        }
    }
    return -1;
}

std::optional<double> numberOf(const Json& value)
{
    if (value.kind != Json::Kind::Number) {
        return std::nullopt;
    }
    char* end = nullptr;
    const double number = std::strtod(value.text.c_str(), &end);
    if (end != value.text.c_str() + value.text.size() || !std::isfinite(number)) {
        return std::nullopt;
    }
    return number;
}

std::optional<uint32_t> countOf(const Json& value)
{
    const std::optional<double> number = numberOf(value);
    if (!number || *number < 1 || *number > 1000000 || *number != std::floor(*number)) {
        return std::nullopt;
    }
    return static_cast<uint32_t>(*number);
}

struct ReadValue {
    double low = 0;
    double high = 0;
    double seconds = 0;
    uint32_t kind = valueReal;
    uint64_t seed = 0;
};

std::optional<ReadValue> readValueOf(const Json& value)
{
    if (value.kind == Json::Kind::Boolean) {
        const double truth = value.text == "true" ? 1.0 : 0.0;
        return ReadValue{truth, truth, 0, valueTruth};
    }
    if (const std::optional<double> number = numberOf(value)) {
        return ReadValue{*number, *number, 0, valueReal};
    }
    if (value.kind != Json::Kind::Object || value.keys.size() != 2) {
        return std::nullopt;
    }
    const std::optional<size_t> noise = lastKey(value, "noise");
    const std::optional<size_t> seconds = lastKey(value, "seconds");
    if (!noise || !seconds || value.items[*noise].kind != Json::Kind::Array || value.items[*noise].items.size() != 2) {
        return std::nullopt;
    }
    const std::optional<double> low = numberOf(value.items[*noise].items[0]);
    const std::optional<double> high = numberOf(value.items[*noise].items[1]);
    const std::optional<double> period = numberOf(value.items[*seconds]);
    if (!low || !high || !period || *period <= 0 || *period > 86400) {
        return std::nullopt;
    }
    return ReadValue{*low, *high, *period, valueReal};
}

void readCodeEdits(const Mod* mod, const std::wstring& file)
{
    std::string error;
    const std::optional<Json> json = parseFile(file, error);
    if (!json) {
        log(L"  code.json skipped, not valid JSON (" + wide(error) + L")");
        return;
    }
    if (json->kind != Json::Kind::Object) {
        log(L"  code.json skipped, expected functions with a list of changes each");
        return;
    }
    for (size_t i = 0; i < json->keys.size(); ++i) {
        const std::string& function = json->keys[i];
        const Json& list = json->items[i];
        if (list.kind != Json::Kind::Array) {
            log(L"  code.json: " + wide(function) + L" skipped, expected a list of changes");
            continue;
        }
        for (const Json& item : list.items) {
            const auto field = [&item](const char* key) -> const Json* {
                const std::optional<size_t> index = item.kind == Json::Kind::Object ? lastKey(item, key) : std::nullopt;
                return index ? &item.items[*index] : nullptr;
            };
            const Json* find = field("find");
            const Json* set = field("set");
            const Json* skip = field("skip");
            const Json* call = field("call");
            const Json* member = field("member");
            const Json* made = field("new");
            const Json* show = field("show");
            const Json* label = field("name");
            const Json* into = field("into");
            const Json* at = field("at");
            const Json* pin = field("pin");
            const Json* align = field("align");
            const Json* margin = field("margin");
            const Json* size = field("size");
            const Json* picture = field("picture");
            const Json* color = field("color");
            const Json* alpha = field("alpha");
            const Json* to = field("to");
            const Json* nth = field("nth");
            const Json* count = field("count");
            const auto named = [](const Json* value) {
                return value && value->kind == Json::Kind::String && !value->text.empty();
            };
            const auto numbers = [](const Json* value, size_t length, bool gaps) {
                return value->kind == Json::Kind::Array && value->items.size() == length &&
                       std::all_of(value->items.begin(), value->items.end(), [gaps](const Json& side) {
                           return (gaps && side.kind == Json::Kind::Null) || numberOf(side).has_value();
                       });
            };
            const int kinds = (find != nullptr) + (skip != nullptr) + (call != nullptr) + (member != nullptr) +
                              (made != nullptr) + (picture != nullptr);
            const bool showable = !show || (show->kind == Json::Kind::Boolean && show->text == "false") ||
                                  (show->kind == Json::Kind::String && show->text == "hover");
            const bool alignable = !align || (align->kind == Json::Kind::Array && align->items.size() == 2 &&
                                              wordIn(align->items[0], acrossWords) >= 0 &&
                                              wordIn(align->items[1], downWords) >= 0);
            const bool placeable = alignable && (!margin || numbers(margin, 4, true)) && (!size || numbers(size, 2, false));
            if (picture) {
                if (kinds != 1 || !named(picture) || !named(into) || (at && !countOf(*at)) || !placeable ||
                    (color && !numberOf(*color)) || (alpha && !numberOf(*alpha)) || show || label || pin || set || to ||
                    nth || count) {
                    log(L"  code.json: a change in " + wide(function) +
                        L" skipped, expected picture with a sprite and into, and size, color, alpha, align, margin or at");
                    continue;
                }
                CodeEdit edit{mod, function, CodeChange::Picture, Json(), Json(), picture->text, into->text, 0, 0,
                              std::string(), at ? *countOf(*at) : 0, false, align ? *align : Json(),
                              margin ? *margin : Json(), size ? *size : Json()};
                edit.color = color ? *numberOf(*color) : edit.color;
                edit.alpha = alpha ? *numberOf(*alpha) : edit.alpha;
                codeEdits.push_back(edit);
                continue;
            }
            if (made || show || label || into || at || pin || align || margin || size || color || alpha) {
                const bool hover = show && show->kind == Json::Kind::String;
                if (kinds != 1 || !named(made) || !showable || (label && !named(label)) || (into && !named(into)) ||
                    (!show && !label && !into && !align && !margin && !size) || (at && (!into || !countOf(*at))) ||
                    (pin && (!hover || pin->kind != Json::Kind::Boolean)) || !placeable || color || alpha || set ||
                    to || (nth && !countOf(*nth)) || (count && !countOf(*count))) {
                    log(L"  code.json: a change in " + wide(function) +
                        L" skipped, expected new with what is made and show set to false or \"hover\", a name, into, "
                        L"align, margin or size, pin only with hover, and at, nth and count from 1");
                    continue;
                }
                codeEdits.push_back({mod, function, CodeChange::New, Json(), show ? *show : Json(), made->text,
                                     into ? into->text : std::string(), nth ? *countOf(*nth) : 0,
                                     count ? *countOf(*count) : 0, label ? label->text : std::string(),
                                     at ? *countOf(*at) : 0, pin && pin->text == "true", align ? *align : Json(),
                                     margin ? *margin : Json(), size ? *size : Json()});
                continue;
            }
            std::optional<CodeChange> change;
            if (kinds == 1 && find && set && !to && find->kind == set->kind &&
                ((find->kind == Json::Kind::Number && numberOf(*find) && numberOf(*set)) ||
                 find->kind == Json::Kind::Boolean)) {
                change = CodeChange::Value;
            } else if (kinds == 1 && named(skip) && !set && !to) {
                change = CodeChange::Skip;
            } else if (kinds == 1 && named(call) && named(to) && !set) {
                change = CodeChange::Call;
            } else if (kinds == 1 && named(member) && named(to) && !set) {
                change = CodeChange::Member;
            } else if (kinds == 1 && named(member) && set && !to && readValueOf(*set)) {
                change = CodeChange::Read;
            }
            if (!change || (nth && !countOf(*nth)) || (count && !countOf(*count))) {
                log(L"  code.json: a change in " + wide(function) +
                    L" skipped, expected find and set of the same kind, skip, call and to, or member with to or set, "
                    L"with nth and count from 1");
                continue;
            }
            const bool value = *change == CodeChange::Value;
            const bool read = *change == CodeChange::Read;
            const Json* source = *change == CodeChange::Skip ? skip : *change == CodeChange::Call ? call : member;
            codeEdits.push_back({mod, function, *change, value ? *find : Json(), value || read ? *set : Json(),
                                 value ? std::string() : source->text, to ? to->text : std::string(),
                                 nth ? *countOf(*nth) : 0, count ? *countOf(*count) : 0});
        }
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
            if (lower(relative) == L"code.json") {
                readCodeEdits(mod.get(), path + L"\\" + relative);
                continue;
            }
            if (const auto font = fontOf(relative)) {
                fontFiles[*font].push_back({mod.get(), path + L"\\" + relative});
                continue;
            }
            if (isSpriteFile(relative)) {
                if (const auto sprite = spriteOf(relative)) {
                    spriteFiles[*sprite].push_back({mod.get(), path + L"\\" + relative});
                } else {
                    log(L"  " + relative + L" skipped, expected sprites\\<sprite>.png or sprites\\<sprite>\\<frame>.png");
                }
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

class MappedFile {
public:
    explicit MappedFile(const std::wstring& path)
    {
        file_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                            nullptr);
        LARGE_INTEGER size{};
        if (file_ == INVALID_HANDLE_VALUE || !GetFileSizeEx(file_, &size) || size.QuadPart < 8) {
            return;
        }
        mapping_ = CreateFileMappingW(file_, nullptr, PAGE_READONLY, 0, 0, nullptr);
        data_ = mapping_ ? static_cast<const BYTE*>(MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, 0)) : nullptr;
        size_ = data_ ? static_cast<size_t>(size.QuadPart) : 0;
    }

    ~MappedFile()
    {
        if (data_) {
            UnmapViewOfFile(data_);
        }
        if (mapping_) {
            CloseHandle(mapping_);
        }
        if (file_ != INVALID_HANDLE_VALUE) {
            CloseHandle(file_);
        }
    }

    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    const BYTE* at(size_t offset, size_t length) const
    {
        if (!data_ || offset > size_ || length > size_ - offset) {
            throw std::runtime_error("it ends too early");
        }
        return data_ + offset;
    }

    uint32_t u32(size_t offset) const
    {
        uint32_t value = 0;
        std::memcpy(&value, at(offset, sizeof(value)), sizeof(value));
        return value;
    }

    size_t size() const { return size_; }

private:
    HANDLE file_ = INVALID_HANDLE_VALUE;
    HANDLE mapping_ = nullptr;
    const BYTE* data_ = nullptr;
    size_t size_ = 0;
};

struct Chunk {
    size_t header = 0;
    size_t data = 0;
    uint32_t size = 0;
};

std::map<std::string, Chunk> chunksOf(const MappedFile& file)
{
    if (!file.size()) {
        throw std::runtime_error("it cannot be read");
    }
    if (std::memcmp(file.at(0, 4), "FORM", 4) != 0 || 8 + size_t{file.u32(4)} != file.size()) {
        throw std::runtime_error("it is not a GameMaker data file");
    }
    std::map<std::string, Chunk> chunks;
    for (size_t at = 8; at < file.size();) {
        const uint32_t size = file.u32(at + 4);
        chunks[std::string(reinterpret_cast<const char*>(file.at(at, 4)), 4)] = {at, at + 8, size};
        at += 8 + size_t{size};
    }
    return chunks;
}

const Chunk& chunkNamed(const std::map<std::string, Chunk>& chunks, const char* name)
{
    const auto found = chunks.find(name);
    if (found == chunks.end()) {
        throw std::runtime_error(std::string("it has no ") + name + " chunk");
    }
    return found->second;
}

std::string assetName(const MappedFile& file, uint32_t at)
{
    if (at < 4) {
        return {};
    }
    const uint32_t length = file.u32(at - 4);
    std::string name(reinterpret_cast<const char*>(file.at(at, length)), length);
    for (auto& c : name) {
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    }
    return name;
}

std::map<std::string, std::vector<uint32_t>> spritesOf(const MappedFile& file, const Chunk& chunk)
{
    std::map<std::string, std::vector<uint32_t>> sprites;
    const uint32_t count = file.u32(chunk.data);
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t at = file.u32(chunk.data + 4 + 4 * size_t{i});
        if (!at) {
            continue;
        }
        size_t frames = size_t{at} + 56;
        if (file.u32(frames) == 0xFFFFFFFF) {
            const uint32_t layout = file.u32(frames + 4);
            if (file.u32(frames + 8) != 0) {
                continue;
            }
            frames += 20 + (layout >= 2 ? 4 : 0) + (layout >= 3 ? 4 : 0);
        }
        std::vector<uint32_t> items(file.u32(frames));
        for (size_t frame = 0; frame < items.size(); ++frame) {
            items[frame] = file.u32(frames + 4 + 4 * frame);
        }
        sprites[assetName(file, file.u32(at))] = std::move(items);
    }
    return sprites;
}

struct PageItem {
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
    uint16_t targetX;
    uint16_t targetY;
    uint16_t targetWidth;
    uint16_t targetHeight;
    uint16_t boundWidth;
    uint16_t boundHeight;
    uint16_t page;
};

PageItem pageItemAt(const MappedFile& file, uint32_t at)
{
    PageItem item{};
    std::memcpy(&item, file.at(at, sizeof(item)), sizeof(item));
    return item;
}

struct TexturePage {
    size_t entry;
    size_t data;
    uint32_t size;
};

std::vector<TexturePage> texturePagesOf(const MappedFile& file, const Chunk& chunk)
{
    std::vector<TexturePage> pages;
    const uint32_t count = file.u32(chunk.data);
    for (uint32_t i = 0; i < count; ++i) {
        const size_t entry = file.u32(chunk.data + 4 + 4 * size_t{i});
        if (i && entry - pages.back().entry != 28) {
            throw std::runtime_error("its texture pages are laid out in a way NLSE does not know");
        }
        pages.push_back({entry, file.u32(entry + 24), file.u32(entry + 8)});
    }
    return pages;
}

std::optional<Image> decodePage(const MappedFile& file, const TexturePage& page)
{
    const BYTE* blob = file.at(page.data, page.size);
    if (page.size >= 12 && std::memcmp(blob, "2zoq", 4) == 0) {
        uint32_t expected = 0;
        std::memcpy(&expected, blob + 8, sizeof(expected));
        const std::optional<std::string> qoi = bunzip(blob + 12, page.size - 12, expected);
        return qoi ? decodeQoi(reinterpret_cast<const BYTE*>(qoi->data()), qoi->size()) : std::nullopt;
    }
    return decodeQoi(blob, page.size);
}

bool paste(Image& page, const Image& sprite, const PageItem& item)
{
    if (!item.width || !item.height || uint32_t{item.x} + item.width > page.width ||
        uint32_t{item.y} + item.height > page.height || uint32_t{item.targetX} + item.targetWidth > sprite.width ||
        uint32_t{item.targetY} + item.targetHeight > sprite.height) {
        return false;
    }
    for (uint32_t row = 0; row < item.height; ++row) {
        const uint32_t sourceRow = item.targetY + row * item.targetHeight / item.height;
        for (uint32_t column = 0; column < item.width; ++column) {
            const uint32_t sourceColumn = item.targetX + column * item.targetWidth / item.width;
            std::memcpy(&page.pixels[(size_t{item.y + row} * page.width + item.x + column) * 4],
                        &sprite.pixels[(size_t{sourceRow} * sprite.width + sourceColumn) * 4], 4);
        }
    }
    return true;
}

std::optional<Image> loadPicture(IWICImagingFactory* factory, const std::wstring& path, uint32_t width,
                                 uint32_t height, bool& scaled)
{
    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    Microsoft::WRL::ComPtr<IWICBitmapScaler> scaler;
    Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
    UINT pictureWidth = 0;
    UINT pictureHeight = 0;
    if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                                  &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) || FAILED(frame->GetSize(&pictureWidth, &pictureHeight))) {
        return std::nullopt;
    }
    IWICBitmapSource* source = frame.Get();
    scaled = pictureWidth != width || pictureHeight != height;
    if (scaled) {
        if (FAILED(factory->CreateBitmapScaler(&scaler)) ||
            FAILED(scaler->Initialize(source, width, height, WICBitmapInterpolationModeFant))) {
            return std::nullopt;
        }
        source = scaler.Get();
    }
    Image image{width, height, std::vector<BYTE>(size_t{width} * height * 4)};
    if (FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(source, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom)) ||
        FAILED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(image.pixels.size()), image.pixels.data()))) {
        return std::nullopt;
    }
    return image;
}

struct ComScope {
    HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~ComScope()
    {
        if (SUCCEEDED(result)) {
            CoUninitialize();
        }
    }
};

struct PageBlob {
    size_t entry;
    std::string bytes;
};

std::wstring stampOf(const std::wstring& path)
{
    WIN32_FILE_ATTRIBUTE_DATA information{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &information)) {
        return L"missing";
    }
    const uint64_t size = (uint64_t{information.nFileSizeHigh} << 32) | information.nFileSizeLow;
    const uint64_t time =
        (uint64_t{information.ftLastWriteTime.dwHighDateTime} << 32) | information.ftLastWriteTime.dwLowDateTime;
    return std::to_wstring(size) + L" " + std::to_wstring(time);
}

struct BytePatch {
    size_t offset;
    std::string bytes;
};

struct Glyph {
    size_t entry;
    uint16_t character;
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
    int16_t shift;
    int16_t offset;
    uint16_t kernings;
};

struct FontAsset {
    PageItem atlas{};
    std::vector<Glyph> glyphs;
};

uint16_t u16At(const MappedFile& file, size_t at)
{
    uint16_t value = 0;
    std::memcpy(&value, file.at(at, sizeof(value)), sizeof(value));
    return value;
}

std::map<std::string, FontAsset> fontsOf(const MappedFile& file, const Chunk& chunk)
{
    std::map<std::string, FontAsset> fonts;
    const uint32_t count = file.u32(chunk.data);
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t at = file.u32(chunk.data + 4 + 4 * size_t{i});
        if (!at) {
            continue;
        }
        const uint32_t glyphs = file.u32(size_t{at} + 52);
        if (glyphs && file.u32(size_t{at} + 56) != at + 56 + 4 * glyphs) {
            throw std::runtime_error("its fonts are laid out in a way NLSE does not know");
        }
        FontAsset font;
        font.atlas = pageItemAt(file, file.u32(size_t{at} + 28));
        for (uint32_t g = 0; g < glyphs; ++g) {
            const size_t entry = file.u32(size_t{at} + 56 + 4 * size_t{g});
            font.glyphs.push_back({entry, u16At(file, entry), u16At(file, entry + 2), u16At(file, entry + 4),
                                   u16At(file, entry + 6), u16At(file, entry + 8),
                                   static_cast<int16_t>(u16At(file, entry + 10)),
                                   static_cast<int16_t>(u16At(file, entry + 12)), u16At(file, entry + 14)});
        }
        fonts[assetName(file, file.u32(at))] = std::move(font);
    }
    return fonts;
}

struct FontShape {
    int baseline = 0;
    int capHeight = 0;
    int cellHeight = 0;
    double slope = 255;
};

FontShape shapeOf(const Image& page, const FontAsset& font)
{
    FontShape shape;
    std::vector<int> steps;
    const auto alpha = [&](uint32_t x, uint32_t y) {
        return page.pixels[(size_t{font.atlas.y + y} * page.width + font.atlas.x + x) * 4 + 3];
    };
    for (const char capital : {'N', 'E', 'I', 'H'}) {
        for (const auto& glyph : font.glyphs) {
            if (glyph.character != static_cast<uint16_t>(capital)) {
                continue;
            }
            int top = -1;
            int bottom = -1;
            for (uint32_t y = 0; y < glyph.height; ++y) {
                for (uint32_t x = 0; x < glyph.width; ++x) {
                    if (alpha(glyph.x + x, glyph.y + y) >= 128) {
                        top = top < 0 ? static_cast<int>(y) : top;
                        bottom = static_cast<int>(y);
                    }
                }
            }
            if (top >= 0) {
                shape.baseline = bottom + 1;
                shape.capHeight = bottom - top + 1;
            }
        }
    }
    for (const auto& glyph : font.glyphs) {
        shape.cellHeight = std::max<int>(shape.cellHeight, glyph.height);
        for (uint32_t y = 0; y < glyph.height; ++y) {
            for (uint32_t x = 0; x + 1 < glyph.width; ++x) {
                const int a = alpha(glyph.x + x, glyph.y + y);
                const int b = alpha(glyph.x + x + 1, glyph.y + y);
                if ((a < 128) != (b < 128) && a > 8 && b > 8 && a < 247 && b < 247) {
                    steps.push_back(std::abs(a - b));
                }
            }
        }
    }
    if (steps.size() > 50) {
        std::nth_element(steps.begin(), steps.begin() + steps.size() / 2, steps.end());
        shape.slope = steps[steps.size() / 2];
    }
    return shape;
}

struct Face {
    std::wstring family;
    int weight = FW_NORMAL;
    bool italic = false;
};

std::optional<Face> faceOf(const std::string& data)
{
    const auto be16 = [&data](size_t at) -> uint32_t {
        return at + 2 <= data.size() ? static_cast<uint32_t>(static_cast<BYTE>(data[at]) << 8 | static_cast<BYTE>(data[at + 1]))
                                     : 0;
    };
    const auto be32 = [&be16](size_t at) { return be16(at) << 16 | be16(at + 2); };
    size_t names = 0;
    size_t metrics = 0;
    for (uint32_t i = 0, tables = be16(4); i < tables && 28 + 16 * size_t{i} <= data.size(); ++i) {
        const size_t record = 12 + 16 * size_t{i};
        names = data.compare(record, 4, "name") == 0 ? be32(record + 8) : names;
        metrics = data.compare(record, 4, "OS/2") == 0 ? be32(record + 8) : metrics;
    }
    Face face;
    if (metrics) {
        face.weight = static_cast<int>(be16(metrics + 4));
        face.italic = (be16(metrics + 62) & 1) != 0;
    }
    if (!names) {
        return std::nullopt;
    }
    const size_t strings = names + be16(names + 4);
    for (uint32_t i = 0, count = be16(names + 2); i < count; ++i) {
        const size_t record = names + 6 + 12 * size_t{i};
        if (be16(record) != 3 || be16(record + 2) != 1 || be16(record + 6) != 1 ||
            (!face.family.empty() && be16(record + 4) != 0x409)) {
            continue;
        }
        face.family.clear();
        const size_t start = strings + be16(record + 10);
        for (size_t c = 0; c + 1 < be16(record + 8); c += 2) {
            face.family += static_cast<wchar_t>(be16(start + c));
        }
    }
    if (face.family.empty()) {
        return std::nullopt;
    }
    return face;
}

struct GlyphBitmap {
    GLYPHMETRICS metrics{};
    std::vector<BYTE> gray;
    int pitch = 0;
};

class FaceRenderer {
public:
    FaceRenderer(const std::string& data, const Face& face) : face_(face)
    {
        DWORD count = 0;
        resource_ = AddFontMemResourceEx(const_cast<char*>(data.data()), static_cast<DWORD>(data.size()), nullptr, &count);
        dc_ = CreateCompatibleDC(nullptr);
    }

    ~FaceRenderer()
    {
        release();
        if (dc_) {
            DeleteDC(dc_);
        }
        if (resource_) {
            RemoveFontMemResourceEx(resource_);
        }
    }

    FaceRenderer(const FaceRenderer&) = delete;
    FaceRenderer& operator=(const FaceRenderer&) = delete;

    bool select(int pixels)
    {
        release();
        if (!resource_ || !dc_) {
            return false;
        }
        font_ = CreateFontW(-pixels, 0, 0, 0, face_.weight, face_.italic, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_ONLY_PRECIS,
                            CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, face_.family.c_str());
        previous_ = font_ ? SelectObject(dc_, font_) : nullptr;
        wchar_t selected[LF_FACESIZE] = {};
        return font_ && GetTextFaceW(dc_, LF_FACESIZE, selected) && _wcsicmp(selected, face_.family.c_str()) == 0;
    }

    bool has(wchar_t character)
    {
        WORD index = 0xFFFF;
        return GetGlyphIndicesW(dc_, &character, 1, &index, GGI_MARK_NONEXISTING_GLYPHS) != GDI_ERROR && index != 0xFFFF;
    }

    std::optional<GlyphBitmap> render(wchar_t character)
    {
        const MAT2 identity = {{0, 1}, {0, 0}, {0, 0}, {0, 1}};
        GlyphBitmap glyph;
        const DWORD size = GetGlyphOutlineW(dc_, character, GGO_GRAY8_BITMAP, &glyph.metrics, 0, nullptr, &identity);
        if (size == GDI_ERROR) {
            return std::nullopt;
        }
        glyph.pitch = static_cast<int>((glyph.metrics.gmBlackBoxX + 3) & ~3u);
        if (size) {
            glyph.gray.resize(size);
            if (GetGlyphOutlineW(dc_, character, GGO_GRAY8_BITMAP, &glyph.metrics, size, glyph.gray.data(), &identity) ==
                GDI_ERROR) {
                return std::nullopt;
            }
        }
        return glyph;
    }

private:
    void release()
    {
        if (font_) {
            SelectObject(dc_, previous_);
            DeleteObject(font_);
            font_ = nullptr;
        }
    }

    Face face_;
    HANDLE resource_ = nullptr;
    HDC dc_ = nullptr;
    HFONT font_ = nullptr;
    HGDIOBJ previous_ = nullptr;
};

std::vector<double> squaredDistances(const std::vector<BYTE>& sources, int width, int height)
{
    const double distant = 1e20;
    std::vector<double> grid(size_t(width) * height);
    for (size_t i = 0; i < grid.size(); ++i) {
        grid[i] = sources[i] ? 0 : distant;
    }
    const int longest = std::max(width, height);
    std::vector<double> f(longest);
    std::vector<double> d(longest);
    std::vector<double> z(longest + 1);
    std::vector<int> v(longest);
    const auto pass = [&](int n) {
        int k = 0;
        v[0] = 0;
        z[0] = -distant;
        z[1] = distant;
        for (int q = 1; q < n; ++q) {
            double s = ((f[q] + double(q) * q) - (f[v[k]] + double(v[k]) * v[k])) / (2.0 * q - 2.0 * v[k]);
            while (k > 0 && s <= z[k]) {
                --k;
                s = ((f[q] + double(q) * q) - (f[v[k]] + double(v[k]) * v[k])) / (2.0 * q - 2.0 * v[k]);
            }
            ++k;
            v[k] = q;
            z[k] = s;
            z[k + 1] = distant;
        }
        k = 0;
        for (int q = 0; q < n; ++q) {
            while (z[k + 1] < q) {
                ++k;
            }
            d[q] = double(q - v[k]) * (q - v[k]) + f[v[k]];
        }
    };
    for (int x = 0; x < width; ++x) {
        for (int y = 0; y < height; ++y) {
            f[y] = grid[size_t(y) * width + x];
        }
        pass(height);
        for (int y = 0; y < height; ++y) {
            grid[size_t(y) * width + x] = d[y];
        }
    }
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            f[x] = grid[size_t(y) * width + x];
        }
        pass(width);
        for (int x = 0; x < width; ++x) {
            grid[size_t(y) * width + x] = d[x];
        }
    }
    return grid;
}

struct Cell {
    int width = 1;
    int height = 1;
    int shift = 0;
    int offset = 0;
    std::vector<BYTE> pixels;
};

Cell renderCell(const GlyphBitmap& glyph, const FontShape& shape, int pad, int scale)
{
    const GLYPHMETRICS& metrics = glyph.metrics;
    Cell cell;
    cell.height = shape.cellHeight;
    cell.shift = static_cast<int>(std::lround(metrics.gmCellIncX / double(scale)));
    if (glyph.gray.empty()) {
        cell.width = std::max(1, cell.shift);
        cell.pixels.assign(size_t(cell.width) * cell.height * 4, 0);
        return cell;
    }
    const int left = static_cast<int>(std::floor(metrics.gmptGlyphOrigin.x / double(scale))) - pad;
    const int right =
        static_cast<int>(std::ceil((metrics.gmptGlyphOrigin.x + int(metrics.gmBlackBoxX)) / double(scale))) + pad;
    cell.width = right - left;
    cell.offset = left;
    const int gridWidth = cell.width * scale;
    const int gridHeight = cell.height * scale;
    std::vector<BYTE> inside(size_t(gridWidth) * gridHeight);
    std::vector<BYTE> outside(inside.size(), 1);
    const int originX = metrics.gmptGlyphOrigin.x - left * scale;
    const int originY = shape.baseline * scale - metrics.gmptGlyphOrigin.y;
    for (int by = 0; by < int(metrics.gmBlackBoxY); ++by) {
        for (int bx = 0; bx < int(metrics.gmBlackBoxX); ++bx) {
            const int gx = originX + bx;
            const int gy = originY + by;
            if (gx >= 0 && gy >= 0 && gx < gridWidth && gy < gridHeight && glyph.gray[size_t(by) * glyph.pitch + bx] >= 32) {
                inside[size_t(gy) * gridWidth + gx] = 1;
                outside[size_t(gy) * gridWidth + gx] = 0;
            }
        }
    }
    const bool field = shape.slope < 120;
    std::vector<double> toInside;
    std::vector<double> toOutside;
    if (field) {
        toInside = squaredDistances(inside, gridWidth, gridHeight);
        toOutside = squaredDistances(outside, gridWidth, gridHeight);
    }
    cell.pixels.assign(size_t(cell.width) * cell.height * 4, 255);
    for (int cy = 0; cy < cell.height; ++cy) {
        for (int cx = 0; cx < cell.width; ++cx) {
            double value = 0;
            if (field) {
                const size_t i = size_t(cy * scale + scale / 2) * gridWidth + cx * scale + scale / 2;
                const double d = inside[i] ? (std::sqrt(toOutside[i]) - 0.5) / scale : -(std::sqrt(toInside[i]) - 0.5) / scale;
                value = 128 + shape.slope * d;
            } else {
                int covered = 0;
                for (int sy = 0; sy < scale; ++sy) {
                    for (int sx = 0; sx < scale; ++sx) {
                        covered += inside[size_t(cy * scale + sy) * gridWidth + cx * scale + sx];
                    }
                }
                value = 255.0 * covered / (scale * scale);
            }
            cell.pixels[(size_t(cy) * cell.width + cx) * 4 + 3] = static_cast<BYTE>(std::clamp(std::lround(value), 0L, 255L));
        }
    }
    return cell;
}

std::vector<BytePatch> replaceFont(Image& page, const FontAsset& font, const std::string& data, size_t& kept, int& shrink)
{
    const PageItem& atlas = font.atlas;
    if (atlas.targetX || atlas.targetY || atlas.width != atlas.boundWidth || atlas.height != atlas.boundHeight ||
        uint32_t{atlas.x} + atlas.width > page.width || uint32_t{atlas.y} + atlas.height > page.height) {
        throw std::runtime_error("its atlas is laid out in a way NLSE does not know");
    }
    const FontShape shape = shapeOf(page, font);
    const std::optional<Face> face = faceOf(data);
    if (!shape.capHeight || !face) {
        throw std::runtime_error(face ? "the game's font has no capital letters to measure" : "the font file cannot be read");
    }
    constexpr int scale = 8;
    FaceRenderer renderer(data, *face);
    std::optional<GlyphBitmap> probe = renderer.select(512) ? renderer.render(L'H') : std::nullopt;
    if (!probe || !probe->metrics.gmBlackBoxY) {
        throw std::runtime_error("Windows cannot use the font file");
    }
    const double pixels = 512.0 * shape.capHeight / probe->metrics.gmBlackBoxY;
    const int pad = shape.slope < 120 ? std::max(1, static_cast<int>(std::ceil(128 / shape.slope))) : 1;
    std::vector<Cell> cells;
    std::vector<std::pair<int, int>> places;
    for (shrink = 0; places.size() != font.glyphs.size(); shrink += 3) {
        if (shrink > 20) {
            throw std::runtime_error("the new letters do not fit the atlas of the game's font");
        }
        if (!renderer.select(static_cast<int>(std::lround(pixels * (100 - shrink) / 100 * scale)))) {
            throw std::runtime_error("Windows cannot use the font file");
        }
        cells.clear();
        places.clear();
        kept = 0;
        for (const auto& glyph : font.glyphs) {
            const wchar_t character = static_cast<wchar_t>(glyph.character);
            std::optional<GlyphBitmap> bitmap = renderer.has(character) ? renderer.render(character) : std::nullopt;
            if (bitmap) {
                cells.push_back(renderCell(*bitmap, shape, pad, scale));
                continue;
            }
            Cell original;
            original.width = glyph.width;
            original.height = glyph.height;
            original.shift = glyph.shift;
            original.offset = glyph.offset;
            original.pixels.resize(size_t(glyph.width) * glyph.height * 4);
            for (uint32_t y = 0; y < glyph.height; ++y) {
                std::memcpy(&original.pixels[size_t(y) * glyph.width * 4],
                            &page.pixels[(size_t{atlas.y + glyph.y + y} * page.width + atlas.x + glyph.x) * 4],
                            size_t{glyph.width} * 4);
            }
            cells.push_back(std::move(original));
            ++kept;
        }
        int x = 0;
        int y = 0;
        int row = 0;
        for (const auto& cell : cells) {
            if (x + cell.width > atlas.width) {
                x = 0;
                y += row + 1;
                row = 0;
            }
            if (y + cell.height > atlas.height || cell.width > atlas.width) {
                places.clear();
                break;
            }
            places.emplace_back(x, y);
            x += cell.width + 1;
            row = std::max(row, cell.height);
        }
    }
    shrink -= 3;
    for (uint32_t line = 0; line < atlas.height; ++line) {
        for (uint32_t column = 0; column < atlas.width; ++column) {
            BYTE* pixel = &page.pixels[(size_t{atlas.y + line} * page.width + atlas.x + column) * 4];
            pixel[0] = pixel[1] = pixel[2] = 255;
            pixel[3] = 0;
        }
    }
    std::vector<BytePatch> patches;
    for (size_t i = 0; i < cells.size(); ++i) {
        const Cell& cell = cells[i];
        for (int cy = 0; cy < cell.height; ++cy) {
            std::memcpy(&page.pixels[(size_t(atlas.y + places[i].second + cy) * page.width + atlas.x + places[i].first) * 4],
                        &cell.pixels[size_t(cy) * cell.width * 4], size_t(cell.width) * 4);
        }
        const int16_t fields[6] = {static_cast<int16_t>(places[i].first), static_cast<int16_t>(places[i].second),
                                   static_cast<int16_t>(cell.width), static_cast<int16_t>(cell.height),
                                   static_cast<int16_t>(cell.shift), static_cast<int16_t>(cell.offset)};
        patches.push_back({font.glyphs[i].entry + 2, std::string(reinterpret_cast<const char*>(fields), sizeof(fields))});
        for (uint16_t k = 0; k < font.glyphs[i].kernings; ++k) {
            patches.push_back({font.glyphs[i].entry + 16 + 4 * size_t{k} + 2, std::string(2, '\0')});
        }
    }
    return patches;
}

bool writePatchedData(const std::wstring& source, const std::wstring& target, const Chunk& last,
                      const std::vector<PageBlob>& blobs, const std::vector<BytePatch>& patches)
{
    const std::wstring part = target + L".part";
    createDirectories(parentOf(target));
    if (!CopyFileW(source.c_str(), part.c_str(), FALSE)) {
        return false;
    }
    HANDLE file =
        CreateFileW(part.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        DeleteFileW(part.c_str());
        return false;
    }
    const auto writeAt = [file](uint64_t at, const void* bytes, size_t size) {
        LARGE_INTEGER position;
        position.QuadPart = static_cast<LONGLONG>(at);
        DWORD written = 0;
        return SetFilePointerEx(file, position, nullptr, FILE_BEGIN) &&
               WriteFile(file, bytes, static_cast<DWORD>(size), &written, nullptr) && written == size;
    };
    uint64_t end = last.data + last.size;
    bool written = true;
    for (const auto& blob : blobs) {
        const uint64_t at = (end + 127) & ~uint64_t{127};
        const std::string padding(static_cast<size_t>(at - end), '\0');
        const auto size = static_cast<uint32_t>(blob.bytes.size());
        const auto offset = static_cast<uint32_t>(at);
        written = written && at + blob.bytes.size() < 0xFFFFFFFF && writeAt(end, padding.data(), padding.size()) &&
                  writeAt(at, blob.bytes.data(), blob.bytes.size()) && writeAt(blob.entry + 8, &size, 4) &&
                  writeAt(blob.entry + 24, &offset, 4);
        end = at + blob.bytes.size();
    }
    for (const auto& patch : patches) {
        written = written && writeAt(patch.offset, patch.bytes.data(), patch.bytes.size());
    }
    const auto chunkSize = static_cast<uint32_t>(end - last.data);
    const auto formSize = static_cast<uint32_t>(end - 8);
    written = written && writeAt(last.header + 4, &chunkSize, 4) && writeAt(4, &formSize, 4);
    CloseHandle(file);
    written = written && MoveFileExW(part.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING);
    if (!written) {
        DeleteFileW(part.c_str());
    }
    return written;
}

std::wstring spriteTitle(const std::pair<std::string, uint32_t>& sprite)
{
    return wide(sprite.first) + (sprite.second ? L" frame " + std::to_wstring(sprite.second) : L"");
}

struct Rebuild {
    std::vector<PageBlob> blobs;
    std::vector<BytePatch> patches;
};

struct FontJob {
    std::string name;
    const FontAsset* font;
    const std::vector<SpriteFile>* files;
};

Rebuild rebuildPages(const MappedFile& file, const std::map<std::string, Chunk>& chunks, std::wstring& lines)
{
    const auto note = [&lines](const std::wstring& line) {
        log(line);
        lines += line + L"\n";
    };
    const auto sprites = spritesOf(file, chunkNamed(chunks, "SPRT"));
    const auto pages = texturePagesOf(file, chunkNamed(chunks, "TXTR"));
    const auto fonts = fontFiles.empty() ? std::map<std::string, FontAsset>() : fontsOf(file, chunkNamed(chunks, "FONT"));
    std::map<uint16_t, std::vector<std::pair<PageItem, Image>>> work;
    std::map<uint16_t, std::vector<FontJob>> fontWork;
    for (const auto& entry : fontFiles) {
        const auto found = fonts.find(entry.first);
        if (found == fonts.end() || found->second.atlas.page >= pages.size()) {
            note(L"font " + wide(entry.first) + L": the game has no such font");
            continue;
        }
        fontWork[found->second.atlas.page].push_back({entry.first, &found->second, &entry.second});
        work[found->second.atlas.page];
    }
    {
        ComScope com;
        Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) {
            throw std::runtime_error("Windows Imaging Component is not available");
        }
        for (const auto& entry : spriteFiles) {
            const std::wstring title = L"sprite " + spriteTitle(entry.first);
            const SpriteFile& winner = entry.second.back();
            const auto found = sprites.find(entry.first.first);
            if (found == sprites.end() || entry.first.second >= found->second.size()) {
                note(title + L": the game has no such " + (found == sprites.end() ? L"sprite" : L"frame"));
                continue;
            }
            if (!found->second[entry.first.second]) {
                note(title + L": the game has no picture for it");
                continue;
            }
            const PageItem item = pageItemAt(file, found->second[entry.first.second]);
            bool scaled = false;
            std::optional<Image> picture =
                item.page < pages.size()
                    ? loadPicture(factory.Get(), winner.file, item.boundWidth, item.boundHeight, scaled)
                    : std::nullopt;
            if (!picture) {
                note(title + L": the picture from " + winner.mod->name + L" cannot be read");
                continue;
            }
            note(title + L": replaced by " + winner.mod->name +
                 (scaled ? L", scaled to " + std::to_wstring(item.boundWidth) + L"x" + std::to_wstring(item.boundHeight)
                         : L""));
            for (size_t i = 0; i + 1 < entry.second.size(); ++i) {
                note(L"  " + entry.second[i].mod->name + L": overridden by a later mod");
            }
            work[item.page].push_back({item, std::move(*picture)});
        }
    }
    Rebuild rebuild;
    for (const auto& page : work) {
        std::optional<Image> image = decodePage(file, pages[page.first]);
        if (!image) {
            throw std::runtime_error("texture page " + std::to_string(page.first) + " cannot be read");
        }
        bool changed = !page.second.empty();
        for (const auto& replacement : page.second) {
            if (!paste(*image, replacement.second, replacement.first)) {
                throw std::runtime_error("a sprite does not fit texture page " + std::to_string(page.first));
            }
        }
        for (const auto& job : fontWork[page.first]) {
            const std::wstring title = L"font " + wide(job.name);
            const SpriteFile& winner = job.files->back();
            const std::optional<std::string> data = readFile(winner.file);
            try {
                if (!data) {
                    throw std::runtime_error("the font file cannot be read");
                }
                size_t kept = 0;
                int shrink = 0;
                std::vector<BytePatch> patches = replaceFont(*image, *job.font, *data, kept, shrink);
                rebuild.patches.insert(rebuild.patches.end(), patches.begin(), patches.end());
                changed = true;
                note(title + L": replaced by " + winner.mod->name +
                     (kept ? L", " + std::to_wstring(kept) + (kept == 1 ? L" letter" : L" letters") + L" kept from the game"
                           : L"") +
                     (shrink ? L", " + std::to_wstring(shrink) + L"% smaller to fit" : L""));
                for (size_t i = 0; i + 1 < job.files->size(); ++i) {
                    note(L"  " + (*job.files)[i].mod->name + L": overridden by a later mod");
                }
            } catch (const std::exception& error) {
                note(title + L": " + wide(error.what()) + L", the game's letters stay");
            }
        }
        if (changed) {
            rebuild.blobs.push_back({pages[page.first].entry, encodeQoi(*image)});
        }
    }
    return rebuild;
}

void buildSprites()
{
    const std::wstring source = gameDirectory + L"\\data.win";
    const std::wstring target = mergedDirectory + L"\\data.win";
    const std::wstring keyFile = target + L".key";
    const std::wstring linesFile = target + L".log";
    if (spriteFiles.empty() && fontFiles.empty()) {
        DeleteFileW(target.c_str());
        DeleteFileW(keyFile.c_str());
        DeleteFileW(linesFile.c_str());
        return;
    }
    std::wstring key = std::wstring(version) + L"\n" + stampOf(source) + L"\n";
    for (const auto& entry : spriteFiles) {
        key += entry.second.back().file + L" " + stampOf(entry.second.back().file) + L"\n";
    }
    for (const auto& entry : fontFiles) {
        key += entry.second.back().file + L" " + stampOf(entry.second.back().file) + L"\n";
    }
    if (readFile(keyFile) == utf8(key) && GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES) {
        const std::wstring lines = wide(readFile(linesFile).value_or(std::string()));
        for (size_t start = 0, end; (end = lines.find(L'\n', start)) != std::wstring::npos; start = end + 1) {
            log(lines.substr(start, end - start));
        }
        addRedirect(lower(source), L"data.win", target);
        return;
    }
    DeleteFileW(keyFile.c_str());
    try {
        const ULONGLONG started = GetTickCount64();
        std::wstring lines;
        Rebuild rebuild;
        Chunk last;
        {
            const MappedFile file(source);
            const auto chunks = chunksOf(file);
            for (const auto& chunk : chunks) {
                last = chunk.second.header > last.header ? chunk.second : last;
            }
            rebuild = rebuildPages(file, chunks, lines);
        }
        if (rebuild.blobs.empty()) {
            return;
        }
        if (!writePatchedData(source, target, last, rebuild.blobs, rebuild.patches)) {
            log(L"data.win: cannot write " + target + L", it stays as it is");
            return;
        }
        writeFile(linesFile, utf8(lines));
        writeFile(keyFile, utf8(key));
        addRedirect(lower(source), L"data.win", target);
        const ULONGLONG tenths = (GetTickCount64() - started + 50) / 100;
        log(L"data.win rebuilt in " + std::to_wstring(tenths / 10) + L"." + std::to_wstring(tenths % 10) + L" s");
    } catch (const std::exception& error) {
        log(L"data.win: " + wide(error.what()) + L", it stays as it is");
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

struct AddressRange {
    uintptr_t begin = 0;
    uintptr_t end = 0;

    bool holds(uintptr_t address, size_t size) const { return address >= begin && address <= end && size <= end - address; }
};

struct GameImage {
    uintptr_t base = 0;
    AddressRange code;
    AddressRange text;
    AddressRange values;
};

GameImage imageAt(uintptr_t base)
{
    GameImage image;
    image.base = base;
    const IMAGE_NT_HEADERS* headers = headersOf(base);
    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(headers);
    for (WORD i = 0; i < headers->FileHeader.NumberOfSections; ++i, ++section) {
        const std::string name(reinterpret_cast<const char*>(section->Name), strnlen(reinterpret_cast<const char*>(section->Name), 8));
        const uintptr_t begin = base + section->VirtualAddress;
        if (name == ".text") {
            image.code = {begin, begin + section->Misc.VirtualSize};
        } else if (name == ".rdata") {
            image.text = {begin, begin + section->Misc.VirtualSize};
        } else if (name == ".data") {
            image.values = {begin, begin + std::min<uintptr_t>(section->Misc.VirtualSize, section->SizeOfRawData)};
        }
    }
    return image;
}

template <typename T>
T readAt(uintptr_t address)
{
    T value;
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(value));
    return value;
}

std::map<std::string, AddressRange> gmlFunctions(const GameImage& image)
{
    std::map<std::string, uintptr_t> starts;
    for (uintptr_t at = image.values.begin; at + 16 <= image.values.end; at += 8) {
        const auto name = readAt<uintptr_t>(at);
        const auto code = readAt<uintptr_t>(at + 8);
        if (!image.text.holds(name, 5) || !image.code.holds(code, 1) ||
            std::memcmp(reinterpret_cast<const void*>(name), "gml_", 4) != 0) {
            continue;
        }
        const char* text = reinterpret_cast<const char*>(name);
        starts.emplace(std::string(text, strnlen(text, std::min<uintptr_t>(image.text.end - name, 1024))), code);
    }
    std::vector<uintptr_t> sorted;
    for (const auto& entry : starts) {
        sorted.push_back(entry.second);
    }
    std::sort(sorted.begin(), sorted.end());
    std::map<std::string, AddressRange> functions;
    for (const auto& entry : starts) {
        const auto next = std::upper_bound(sorted.begin(), sorted.end(), entry.second);
        const uintptr_t end = next == sorted.end() ? image.code.end : *next;
        functions[entry.first] = {entry.second, std::min<uintptr_t>(end, entry.second + 0x100000)};
    }
    return functions;
}

struct CodeValue {
    uintptr_t site;
    uintptr_t value;
    bool immediate = false;
};

std::vector<CodeValue> valuesIn(const GameImage& image, const AddressRange& function)
{
    std::vector<CodeValue> found;
    for (uintptr_t at = function.begin; at + 7 <= function.end; ++at) {
        const auto* code = reinterpret_cast<const BYTE*>(at);
        if ((code[0] != 0x48 && code[0] != 0x4C) || code[1] != 0x8D || (code[2] & 0xC7) != 0x05) {
            continue;
        }
        const uintptr_t target = at + 7 + static_cast<intptr_t>(readAt<int32_t>(at + 3));
        if (!image.values.holds(target, 16) || (target & 7) || readAt<uint32_t>(target + 8)) {
            continue;
        }
        const auto kind = readAt<uint32_t>(target + 12);
        if (kind == valueReal || kind == valueWhole || kind == valueTruth) {
            found.push_back({at, target});
        }
    }
    return found;
}

std::vector<CodeValue> immediatesIn(const AddressRange& function)
{
    std::vector<CodeValue> found;
    for (uintptr_t at = function.begin; at + 10 <= function.end; ++at) {
        const auto* code = reinterpret_cast<const BYTE*>(at);
        if ((code[0] == 0x48 || code[0] == 0x49) && code[1] >= 0xB8 && code[1] <= 0xBF) {
            found.push_back({at, at + 2, true});
        }
    }
    return found;
}

bool immediateMatches(uintptr_t value, const Json& find)
{
    if (find.kind != Json::Kind::Number) {
        return false;
    }
    const double stored = readAt<double>(value);
    const double wanted = numberOf(find).value_or(NAN);
    return std::isfinite(stored) && (stored == wanted || std::fabs(stored - wanted) <= 1e-9 * std::max(1.0, std::fabs(wanted)));
}

bool valueMatches(uintptr_t value, const Json& find)
{
    const auto kind = readAt<uint32_t>(value + 12);
    if (find.kind == Json::Kind::Boolean) {
        return kind == valueTruth && (readAt<double>(value) != 0) == (find.text == "true");
    }
    const double wanted = numberOf(find).value_or(NAN);
    if (kind == valueReal) {
        const double stored = readAt<double>(value);
        return stored == wanted || std::fabs(stored - wanted) <= 1e-9 * std::max(1.0, std::fabs(wanted));
    }
    return kind == valueWhole && static_cast<double>(readAt<int64_t>(value)) == wanted;
}

std::optional<std::wstring> replacement(uintptr_t value, const Json& set, std::array<BYTE, 16>& bytes)
{
    std::memcpy(bytes.data(), reinterpret_cast<const void*>(value), bytes.size());
    const auto kind = readAt<uint32_t>(value + 12);
    if (kind == valueTruth) {
        const double truth = set.text == "true" ? 1.0 : 0.0;
        std::memcpy(bytes.data(), &truth, sizeof(truth));
        return std::nullopt;
    }
    const double number = numberOf(set).value_or(0);
    if (kind == valueReal) {
        std::memcpy(bytes.data(), &number, sizeof(number));
        return std::nullopt;
    }
    if (number != std::floor(number) || std::fabs(number) > 9.0e15) {
        return L"the game keeps a whole number there";
    }
    const auto whole = static_cast<int64_t>(number);
    std::memcpy(bytes.data(), &whole, sizeof(whole));
    return std::nullopt;
}

struct ValuePool {
    BYTE* next;
    BYTE* end;
};

std::vector<ValuePool> valuePools;

uintptr_t storeValue(uintptr_t site, const std::array<BYTE, 16>& bytes)
{
    ValuePool* pool = nullptr;
    for (auto& candidate : valuePools) {
        const auto next = reinterpret_cast<uintptr_t>(candidate.next);
        if (candidate.end - candidate.next >= 16 && reaches(site + 7, next) && reaches(site + 7, next + 16)) {
            pool = &candidate;
            break;
        }
    }
    if (!pool) {
        constexpr size_t poolSize = 0x10000;
        BYTE* memory = allocateNear(site, poolSize);
        if (!memory) {
            return 0;
        }
        valuePools.push_back({memory, memory + poolSize});
        pool = &valuePools.back();
    }
    BYTE* stored = pool->next;
    pool->next += 16;
    std::memcpy(stored, bytes.data(), bytes.size());
    return reinterpret_cast<uintptr_t>(stored);
}

std::wstring ordinal(uint32_t number)
{
    const uint32_t tens = number % 100;
    const wchar_t* suffix = tens >= 11 && tens <= 13 ? L"th"
                            : number % 10 == 1     ? L"st"
                            : number % 10 == 2     ? L"nd"
                            : number % 10 == 3     ? L"rd"
                                                   : L"th";
    return std::to_wstring(number) + suffix;
}

std::wstring times(size_t number)
{
    return number == 1 ? L"once" : number == 2 ? L"twice" : std::to_wstring(number) + L" times";
}

const AddressRange* functionNamed(const std::map<std::string, AddressRange>& functions, const std::string& name)
{
    auto found = functions.find(name);
    if (found == functions.end()) {
        found = functions.find("gml_Script_" + name);
    }
    return found == functions.end() ? nullptr : &found->second;
}

std::map<uintptr_t, std::vector<CodeValue>> callsFrom(const AddressRange& function)
{
    std::map<uintptr_t, std::vector<CodeValue>> found;
    for (uintptr_t at = function.begin; at + 5 <= function.end; ++at) {
        if (*reinterpret_cast<const BYTE*>(at) == 0xE8) {
            found[at + 5 + static_cast<intptr_t>(readAt<int32_t>(at + 1))].push_back({at, 0});
        }
    }
    return found;
}

std::vector<CodeValue> callsIn(const AddressRange& function, uintptr_t target)
{
    const auto calls = callsFrom(function);
    const auto found = calls.find(target);
    return found == calls.end() ? std::vector<CodeValue>() : found->second;
}

std::map<std::string, std::vector<CodeValue>> membersIn(const GameImage& image, const AddressRange& function)
{
    std::map<std::string, std::vector<CodeValue>> found;
    for (uintptr_t at = function.begin; at + 6 <= function.end; ++at) {
        const auto* code = reinterpret_cast<const BYTE*>(at);
        if (code[0] != 0x8B || (code[1] & 0xC7) != 0x05) {
            continue;
        }
        const uintptr_t field = at + 2;
        const uintptr_t slot = field + 4 + static_cast<intptr_t>(readAt<int32_t>(field));
        if (slot < 8 || (slot & 7) || !image.values.holds(slot - 8, 12)) {
            continue;
        }
        const auto name = readAt<uintptr_t>(slot - 8);
        if (!image.text.holds(name, 1)) {
            continue;
        }
        const char* text = reinterpret_cast<const char*>(name);
        const size_t length = strnlen(text, std::min<uintptr_t>(image.text.end - name, 256));
        if (length && length < 256) {
            found[std::string(text, length)].push_back({field, slot});
        }
    }
    return found;
}

uintptr_t memberSlot(const GameImage& image, const std::string& name)
{
    for (uintptr_t at = image.values.begin; at + 16 <= image.values.end; at += 8) {
        const auto text = readAt<uintptr_t>(at);
        if (image.text.holds(text, name.size() + 1) &&
            std::memcmp(reinterpret_cast<const void*>(text), name.c_str(), name.size() + 1) == 0 &&
            readAt<uint64_t>(at + 8) == 0xFFFFFFFFull) {
            return at + 8;
        }
    }
    return 0;
}

size_t moveLength(const BYTE* code)
{
    if ((code[0] & 0xF8) != 0x48 || (code[1] != 0x8B && code[1] != 0x8D)) {
        return 0;
    }
    const BYTE mode = code[2] >> 6;
    const BYTE place = code[2] & 7;
    if (mode == 3) {
        return code[1] == 0x8B ? 3 : 0;
    }
    const bool scaled = place == 4;
    const bool wide = mode == 2 || (mode == 0 && (place == 5 || (scaled && (code[3] & 7) == 5)));
    return 3 + (scaled ? 1 : 0) + (mode == 1 ? 1 : wide ? 4 : 0);
}

std::optional<uintptr_t> readCallAt(const AddressRange& function, uintptr_t field)
{
    const uintptr_t load = field - 2;
    if (load < function.begin + 30 || field + 24 > function.end || readAt<BYTE>(load) != 0x8B ||
        readAt<BYTE>(load + 1) != 0x15 || readAt<BYTE>(load - 6) != 0x41 || readAt<BYTE>(load - 5) != 0xB8 ||
        readAt<uint32_t>(load - 4) != 0x80000000u) {
        return std::nullopt;
    }
    bool first = false;
    bool second = false;
    for (uintptr_t at = load - 30; at + 4 <= load - 6; ++at) {
        const auto* code = reinterpret_cast<const BYTE*>(at);
        if ((code[0] == 0xC6 || code[0] == 0x88) && (code[1] & 0xC7) == 0x44 && code[2] == 0x24) {
            first = first || code[3] == 0x20;
            second = second || code[3] == 0x28;
        }
    }
    uintptr_t at = field + 4;
    bool object = readAt<BYTE>(at) == 0xE8;
    for (int step = 0; step < 2 && !object; ++step) {
        const auto* code = reinterpret_cast<const BYTE*>(at);
        const size_t length = moveLength(code);
        if (!length) {
            return std::nullopt;
        }
        object = (code[0] & 0x04) == 0 && ((code[2] >> 3) & 7) == 1;
        at += length;
    }
    if (!first || !second || !object || readAt<BYTE>(at) != 0xE8) {
        return std::nullopt;
    }
    return at;
}

std::map<std::string, std::vector<CodeValue>> readsIn(const AddressRange& function,
                                                       const std::map<std::string, std::vector<CodeValue>>& members)
{
    std::map<std::string, std::vector<CodeValue>> found;
    for (const auto& member : members) {
        for (const auto& use : member.second) {
            if (const auto site = readCallAt(function, use.site)) {
                found[member.first].push_back({*site, use.value});
            }
        }
    }
    return found;
}

const auto codeStart = std::chrono::steady_clock::now();
std::unordered_map<uintptr_t, ReadValue> readValues;

double latticeAt(uint64_t seed, int64_t index)
{
    uint64_t x = seed ^ (static_cast<uint64_t>(index) * 0x9E3779B97F4A7C15ull);
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    x ^= x >> 31;
    return static_cast<double>(x >> 11) / 9007199254740992.0;
}

double smoothAt(uint64_t seed, double x)
{
    const double whole = std::floor(x);
    const double part = x - whole;
    const double eased = part * part * part * (part * (part * 6 - 15) + 10);
    const auto index = static_cast<int64_t>(whole);
    const double from = latticeAt(seed, index);
    return from + (latticeAt(seed, index + 1) - from) * eased;
}

double noiseAt(uint64_t seed, double x)
{
    return (2 * smoothAt(seed, x) + smoothAt(seed ^ 0x5851F42D4C957F2Dull, x * 2.17 + 0.5)) / 3;
}

void writeRead(uintptr_t returnAddress, void* destination)
{
    const auto found = readValues.find(returnAddress);
    if (found == readValues.end() || !destination) {
        return;
    }
    const ReadValue& value = found->second;
    double number = value.low;
    if (value.seconds > 0) {
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - codeStart).count();
        number += (value.high - value.low) * noiseAt(value.seed, elapsed / value.seconds);
    }
    BYTE bytes[16] = {};
    std::memcpy(bytes, &number, sizeof(number));
    std::memcpy(bytes + 12, &value.kind, sizeof(value.kind));
    std::memcpy(destination, bytes, sizeof(bytes));
}

void* readMember(void*, int, int, void* destination, bool, bool)
{
    writeRead(reinterpret_cast<uintptr_t>(_ReturnAddress()), destination);
    return destination;
}

void* skippedScript(void*, void*, void* result, int, void*)
{
    return result;
}

struct GmlValue {
    union {
        double real;
        int64_t whole;
        void* pointer;
    };
    uint32_t flags;
    uint32_t kind;
};

constexpr uint32_t valueUndefined = 5;
constexpr uint32_t valueObject = 6;
constexpr uint32_t valueShort = 7;

using GmlGet = void* (*)(GmlValue*, int, int, GmlValue*, bool, bool);
using GmlMethod = GmlValue* (*)(void*, void*, GmlValue*, int, GmlValue*, GmlValue**);
using GmlBuiltin = GmlValue* (*)(void*, void*, GmlValue*, int, int, GmlValue**);
using GmlRelease = void (*)(GmlValue*);
using GmlScript = GmlValue* (*)(void*, void*, GmlValue*, int, GmlValue**);
using GmlVariable = GmlValue* (*)(void*, int);

struct GmlRuntime {
    GmlGet get = nullptr;
    GmlMethod method = nullptr;
    GmlBuiltin builtin = nullptr;
    GmlRelease release = nullptr;
    GmlScript loading = nullptr;
    GmlScript unit = nullptr;
    std::unordered_map<std::string, uintptr_t> slots;
};

GmlRuntime gml;
std::wstring runtimeProblem;

const char* const gmlNames[] = {"set_sizes_affects_disabled", "draw_set_enable", "set_dirty", "set_parent", "relocate",
                                "is_separated_render", "on_remove", "resize_and_refresh", "alignment", "margin",
                                "parent", "children", "layout", "x", "y", "width", "height", "get", "size", "swap",
                                "view", "input_mouse_x", "input_mouse_y", "input_mouse_check"};
const char* const pictureNames[] = {"gml_Script_gw_Image", "gml_Script_gw_SpriteImage", "@@NewGMLObject@@"};

uint32_t kindOf(const GmlValue& value)
{
    return value.kind & 0xFFFFFF;
}

std::optional<double> numberIn(const GmlValue& value)
{
    switch (kindOf(value)) {
    case valueReal:
    case valueTruth:
        return value.real;
    case valueShort:
        return static_cast<double>(static_cast<int32_t>(value.whole));
    case valueWhole:
        return static_cast<double>(value.whole);
    default:
        return std::nullopt;
    }
}

class Held {
public:
    Held() { clear(); }
    ~Held() { reset(); }
    Held(const Held&) = delete;
    Held& operator=(const Held&) = delete;

    GmlValue* get() { return &value_; }

    void reset()
    {
        if (gml.release && ((1u << (value_.kind & 0x1F)) & 0x46)) {
            gml.release(&value_);
        }
        clear();
    }

    bool object() const { return kindOf(value_) == valueObject && value_.pointer; }
    void* address() const { return object() ? value_.pointer : nullptr; }
    std::optional<double> number() const { return numberIn(value_); }

private:
    void clear()
    {
        std::memset(&value_, 0, sizeof(value_));
        value_.kind = valueUndefined;
    }

    GmlValue value_;
};

GmlValue realValue(double number)
{
    GmlValue value;
    std::memset(&value, 0, sizeof(value));
    value.real = number;
    value.kind = valueReal;
    return value;
}

GmlValue truthValue(bool truth)
{
    GmlValue value = realValue(truth ? 1 : 0);
    value.kind = valueTruth;
    return value;
}

int slotOf(const char* name)
{
    const auto found = gml.slots.find(name);
    return found == gml.slots.end() ? -1 : readAt<int32_t>(found->second);
}

bool getField(GmlValue* holder, const char* name, Held& out)
{
    out.reset();
    const int slot = slotOf(name);
    if (slot < 0 || kindOf(*holder) != valueObject) {
        return false;
    }
    gml.get(holder, slot, static_cast<int>(0x80000000u), out.get(), false, false);
    return kindOf(*out.get()) != valueUndefined;
}

std::optional<double> numberField(GmlValue* holder, const char* name)
{
    Held value;
    getField(holder, name, value);
    return value.number();
}

bool callField(void* other, GmlValue* holder, const char* name, std::initializer_list<GmlValue> arguments, Held* result)
{
    Held method;
    if (!getField(holder, name, method) || !method.object() || arguments.size() > 4) {
        return false;
    }
    GmlValue copies[4];
    GmlValue* pointers[4] = {};
    int count = 0;
    for (const GmlValue& argument : arguments) {
        copies[count] = argument;
        pointers[count] = &copies[count];
        ++count;
    }
    Held spare;
    Held& target = result ? *result : spare;
    target.reset();
    gml.method(holder->pointer, other, target.get(), count, method.get(), pointers);
    return true;
}

GmlValue* variableOf(void* instance, const char* name)
{
    const int slot = slotOf(name);
    if (slot < 0 || !instance) {
        return nullptr;
    }
    const auto table = *reinterpret_cast<GmlVariable* const*>(instance);
    return table[1](instance, slot);
}

std::optional<int> sizeOf(void* other, GmlValue* list)
{
    Held size;
    if (!getField(list, "size", size)) {
        return std::nullopt;
    }
    if (!size.object()) {
        const auto number = size.number();
        return number ? std::optional<int>(static_cast<int>(*number)) : std::nullopt;
    }
    Held result;
    if (!callField(other, list, "size", {}, &result)) {
        return std::nullopt;
    }
    const auto number = result.number();
    return number ? std::optional<int>(static_cast<int>(*number)) : std::nullopt;
}

bool childAt(void* other, GmlValue* list, int index, Held& out)
{
    return callField(other, list, "get", {realValue(index)}, &out) && out.object();
}

std::optional<int> indexIn(void* other, GmlValue* parent, void* child)
{
    Held children;
    if (!getField(parent, "children", children) || !children.object()) {
        return std::nullopt;
    }
    const int count = sizeOf(other, children.get()).value_or(0);
    for (int i = count - 1; i >= 0; --i) {
        Held item;
        if (childAt(other, children.get(), i, item) && item.address() == child) {
            return i;
        }
    }
    return std::nullopt;
}

struct Box {
    double x = 0;
    double y = 0;
    double width = 0;
    double height = 0;
};

std::optional<Box> boxOf(GmlValue* element)
{
    Held layout;
    if (!getField(element, "layout", layout) || !layout.object()) {
        return std::nullopt;
    }
    const auto x = numberField(layout.get(), "x");
    const auto y = numberField(layout.get(), "y");
    const auto width = numberField(layout.get(), "width");
    const auto height = numberField(layout.get(), "height");
    if (!x || !y || !width || !height) {
        return std::nullopt;
    }
    return Box{*x, *y, *width, *height};
}

void showElement(void* other, GmlValue* element, bool visible)
{
    callField(other, element, "draw_set_enable", {truthValue(visible)}, nullptr);
    callField(other, element, "set_sizes_affects_disabled", {truthValue(!visible)}, nullptr);
}

std::unordered_set<void*> collapsed;
GmlScript measureOriginal = nullptr;

void foldElement(void* other, GmlValue* element, bool open)
{
    showElement(other, element, open);
    if (open) {
        collapsed.erase(element->pointer);
    } else {
        collapsed.insert(element->pointer);
    }
}

GmlValue* measuredChild(void* instance, void* other, GmlValue* result, int count, GmlValue** arguments)
{
    if (count > 0 && arguments && arguments[0] && kindOf(*arguments[0]) == valueObject &&
        collapsed.count(arguments[0]->pointer)) {
        *result = realValue(0);
        return result;
    }
    return measureOriginal(instance, other, result, count, arguments);
}

size_t instructionLength(const BYTE* code)
{
    if (code[0] == 0x53 || code[0] == 0x55 || code[0] == 0x56 || code[0] == 0x57) {
        return 1;
    }
    if (code[0] == 0x41 && code[1] >= 0x54 && code[1] <= 0x57) {
        return 2;
    }
    if (code[0] == 0x48 && code[1] == 0x8B && code[2] == 0xC4) {
        return 3;
    }
    const bool wide = code[0] == 0x48 || code[0] == 0x4C || code[0] == 0x44;
    if (wide && code[1] == 0x89 && (code[2] & 0xC7) == 0x44 && code[3] == 0x24) {
        return 5;
    }
    if (wide && code[1] == 0x89 && (code[2] & 0xC7) == 0x40) {
        return 4;
    }
    if (code[0] == 0x48 && code[1] == 0x83 && code[2] == 0xEC) {
        return 4;
    }
    return 0;
}

GmlScript hookFunction(uintptr_t start, void* replacement)
{
    size_t length = 0;
    while (length < 5) {
        const size_t next = instructionLength(reinterpret_cast<const BYTE*>(start + length));
        if (!next) {
            return nullptr;
        }
        length += next;
    }
    auto* trampoline = static_cast<BYTE*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline) {
        return nullptr;
    }
    const BYTE jump[6] = {0xFF, 0x25, 0, 0, 0, 0};
    const uintptr_t back = start + length;
    std::memcpy(trampoline, reinterpret_cast<const void*>(start), length);
    std::memcpy(trampoline + length, jump, sizeof(jump));
    std::memcpy(trampoline + length + sizeof(jump), &back, sizeof(back));
    FlushInstructionCache(GetCurrentProcess(), trampoline, 64);
    if (!writeJump(start, replacement)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return nullptr;
    }
    return reinterpret_cast<GmlScript>(trampoline);
}

uintptr_t methodMadeAfter(const GameImage& image, const AddressRange& function, const std::string& member)
{
    const auto members = membersIn(image, function);
    const auto found = members.find(member);
    if (found == members.end()) {
        return 0;
    }
    for (const auto& use : found->second) {
        for (uintptr_t at = use.site; at + 7 <= function.end && at < use.site + 96; ++at) {
            const auto* code = reinterpret_cast<const BYTE*>(at);
            if (code[0] == 0x48 && code[1] == 0x8D && code[2] == 0x15) {
                const uintptr_t target = at + 7 + static_cast<intptr_t>(readAt<int32_t>(at + 3));
                return image.code.holds(target, 16) ? target : 0;
            }
        }
    }
    return 0;
}

struct Link {
    void* address;
    int index;
};

struct Body {
    void* address;
    int order;
    int age;
    bool seen;
};

struct HoverGroup {
    std::vector<Link> path;
    std::vector<Body> bodies;
    std::set<int> pinned;
    std::set<int> shown;
    int open = -1;
    int made = 0;
    uint64_t build = 0;
    double leftAt = 0;
    int misses = 0;
    bool pin = false;
};

struct MadeSite {
    int show = 0;
    std::string label;
    std::string into;
    uint32_t at = 0;
    bool pin = false;
    int across = -1;
    int down = -1;
    std::vector<std::optional<double>> sides;
    std::vector<double> size;
    bool apart = false;
    std::vector<void*> last;
};

struct Move {
    std::vector<void*> chain;
    MadeSite* site;
    int age = 0;
    bool sent = false;
};

struct Picture {
    uint32_t sprite;
    double color;
    double alpha;
    MadeSite place;
};

struct Make {
    Picture* picture;
    int age;
};

std::map<void*, HoverGroup> hoverGroups;
std::unordered_map<uintptr_t, MadeSite> newSites;
std::map<std::string, std::vector<void*>> namedElements;
std::vector<Move> moves;
std::list<Picture> pictures;
std::vector<Make> makes;
bool keepers = false;
uint64_t hoverTicks = 1;
bool mouseWasDown = false;
bool interfaceBroken = false;

bool wantedOpen(const HoverGroup& group, int order)
{
    return group.open == order || group.pinned.count(order);
}

void placeHover(void* other, GmlValue* element, bool pin)
{
    std::vector<std::unique_ptr<Held>> chain;
    chain.push_back(std::make_unique<Held>());
    if (!getField(element, "parent", *chain.back()) || !chain.back()->object()) {
        showElement(other, element, false);
        return;
    }
    for (int depth = 0; depth < 24; ++depth) {
        auto next = std::make_unique<Held>();
        if (!getField(chain.back()->get(), "parent", *next) || !next->object()) {
            break;
        }
        chain.push_back(std::move(next));
    }
    std::vector<Link> path{{chain.back()->address(), -1}};
    for (size_t i = chain.size() - 1; i-- > 0;) {
        path.push_back({chain[i]->address(), indexIn(other, chain[i + 1]->get(), chain[i]->address()).value_or(-1)});
    }
    Held children;
    const int count = getField(chain.front()->get(), "children", children) && children.object()
                          ? sizeOf(other, children.get()).value_or(0)
                          : 0;
    HoverGroup& group = hoverGroups[chain.front()->address()];
    group.path = path;
    group.misses = 0;
    group.pin = pin;
    if (count == 0 && group.build != hoverTicks) {
        group.build = hoverTicks;
        group.made = 0;
        group.bodies.clear();
        group.shown.clear();
    }
    const int order = group.made++;
    group.bodies.push_back({element->pointer, order, 0, false});
    const bool open = wantedOpen(group, order);
    foldElement(other, element, open);
    if (open) {
        group.shown.insert(order);
    }
}

void foldClosed(const HoverGroup& group, std::unordered_set<void*>& folded)
{
    for (const Body& body : group.bodies) {
        if (!wantedOpen(group, body.order)) {
            folded.insert(body.address);
        }
    }
}

bool hoverIn(void* other, GmlValue* parent, HoverGroup& group, double mouseX, double mouseY, bool click, double now,
             std::unordered_set<void*>& folded)
{
    Held children;
    if (!getField(parent, "children", children) || !children.object()) {
        foldClosed(group, folded);
        return false;
    }
    const int count = sizeOf(other, children.get()).value_or(0);
    std::vector<std::unique_ptr<Held>> items;
    std::unordered_set<void*> present;
    std::map<int, int> targets;
    for (int i = 0; i < count; ++i) {
        items.push_back(std::make_unique<Held>());
        if (!childAt(other, children.get(), i, *items.back())) {
            continue;
        }
        void* address = items.back()->address();
        present.insert(address);
        for (Body& body : group.bodies) {
            if (body.address == address) {
                body.seen = true;
                targets[i] = body.order;
            }
        }
    }
    for (Body& body : group.bodies) {
        body.age += body.seen ? 0 : 1;
    }
    group.bodies.erase(std::remove_if(group.bodies.begin(), group.bodies.end(),
                                      [&](const Body& body) {
                                          return body.seen ? !present.count(body.address) : body.age > 300;
                                      }),
                       group.bodies.end());
    const auto area = boxOf(parent);
    int hovered = -1;
    for (const auto& [index, order] : targets) {
        std::optional<Box> top;
        for (int back = index - 1; back >= 0 && back >= index - 4 && !top; --back) {
            top = boxOf(items[back]->get());
            if (top && top->height <= 0) {
                top.reset();
            }
        }
        if (!top) {
            continue;
        }
        const double left = area ? area->x : top->x;
        const double right = area ? area->x + area->width : top->x + top->width;
        const bool across = mouseX >= left && mouseX < right;
        bool inside = across && mouseY >= top->y && mouseY < top->y + top->height;
        if (inside && click && group.pin && !group.pinned.erase(order)) {
            group.pinned.insert(order);
        }
        if (!inside && group.shown.count(order)) {
            const auto bottom = boxOf(items[index]->get());
            inside = bottom && across && mouseY >= bottom->y && mouseY < bottom->y + bottom->height;
        }
        if (inside) {
            hovered = order;
        }
    }
    if (hovered >= 0) {
        group.open = hovered;
        group.leftAt = now;
    } else if (group.open >= 0 && now - group.leftAt > 0.25) {
        group.open = -1;
    }
    bool changed = false;
    for (const auto& [index, order] : targets) {
        const bool open = wantedOpen(group, order);
        if (open == (group.shown.count(order) > 0)) {
            continue;
        }
        foldElement(other, items[index]->get(), open);
        callField(other, items[index]->get(), "set_dirty", {}, nullptr);
        if (open) {
            group.shown.insert(order);
        } else {
            group.shown.erase(order);
        }
        changed = true;
    }
    foldClosed(group, folded);
    return changed;
}

struct Frame {
    void* self;
    void* other;
};

std::vector<void*> chainOf(GmlValue* element)
{
    std::vector<void*> chain{element->pointer};
    std::vector<std::unique_ptr<Held>> held;
    GmlValue* at = element;
    for (int depth = 0; depth < 24; ++depth) {
        auto next = std::make_unique<Held>();
        if (!getField(at, "parent", *next) || !next->object()) {
            break;
        }
        chain.push_back(next->address());
        at = next->get();
        held.push_back(std::move(next));
    }
    std::reverse(chain.begin(), chain.end());
    return chain;
}

template <typename Work>
bool withLive(void* other, GmlValue* root, const std::vector<void*>& chain, Work work)
{
    if (chain.empty() || root->pointer != chain.front()) {
        return false;
    }
    std::vector<std::unique_ptr<Held>> held;
    GmlValue* at = root;
    for (size_t level = 1; level < chain.size(); ++level) {
        Held children;
        if (!getField(at, "children", children) || !children.object()) {
            return false;
        }
        const int count = sizeOf(other, children.get()).value_or(0);
        auto next = std::make_unique<Held>();
        bool hit = false;
        for (int i = count - 1; i >= 0 && !hit; --i) {
            hit = childAt(other, children.get(), i, *next) && next->address() == chain[level];
        }
        if (!hit) {
            return false;
        }
        at = next->get();
        held.push_back(std::move(next));
    }
    work(at);
    return true;
}

bool startsWith(const std::vector<void*>& chain, const std::vector<void*>& prefix)
{
    return chain.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), chain.begin());
}

void rechain(const std::vector<void*> from, const std::vector<void*>& to)
{
    const auto moved = [&](std::vector<void*>& chain) {
        if (startsWith(chain, from)) {
            std::vector<void*> next = to;
            next.insert(next.end(), chain.begin() + (from.size() - 1), chain.end());
            chain.swap(next);
        }
    };
    for (auto& entry : namedElements) {
        moved(entry.second);
    }
    for (Move& move : moves) {
        moved(move.chain);
    }
    for (auto& entry : newSites) {
        moved(entry.second.last);
    }
    for (auto& entry : hoverGroups) {
        std::vector<void*> path;
        for (const Link& link : entry.second.path) {
            path.push_back(link.address);
        }
        if (startsWith(path, from)) {
            moved(path);
            entry.second.path.clear();
            for (void* address : path) {
                entry.second.path.push_back({address, -1});
            }
        }
    }
}

bool placeAt(void* other, GmlValue* place, void* element, uint32_t at)
{
    Held children;
    if (!getField(place, "children", children) || !children.object()) {
        return false;
    }
    const int count = sizeOf(other, children.get()).value_or(0);
    int index = -1;
    for (int i = count - 1; i >= 0 && index < 0; --i) {
        Held item;
        if (childAt(other, children.get(), i, item) && item.address() == element) {
            index = i;
        }
    }
    if (index < 0) {
        return false;
    }
    const int wanted = at ? std::min(static_cast<int>(at) - 1, count - 1) : index;
    for (; index > wanted; --index) {
        callField(other, children.get(), "swap", {realValue(index - 1), realValue(index)}, nullptr);
    }
    for (; index < wanted; ++index) {
        callField(other, children.get(), "swap", {realValue(index), realValue(index + 1)}, nullptr);
    }
    callField(other, place, "set_dirty", {}, nullptr);
    return true;
}

void keepAbove(void* other, GmlValue* view, const std::vector<void*>& chain)
{
    Held children;
    if (chain.size() < 3 || !getField(view, "children", children) || !children.object()) {
        return;
    }
    int host = -1;
    int element = -1;
    for (int i = sizeOf(other, children.get()).value_or(0) - 1; i >= 0 && (host < 0 || element < 0); --i) {
        Held item;
        if (childAt(other, children.get(), i, item)) {
            host = item.address() == chain[1] ? i : host;
            element = item.address() == chain.back() ? i : element;
        }
    }
    for (; element >= 0 && element < host; ++element) {
        callField(other, children.get(), "swap", {realValue(element), realValue(element + 1)}, nullptr);
    }
}

bool viewUnits(void* instance, void* other, const std::vector<std::optional<double>>& values, Held* units,
               GmlValue* sides)
{
    for (size_t k = 0; k < values.size(); ++k) {
        sides[k] = realValue(0);
        if (values[k]) {
            GmlValue number = realValue(*values[k]);
            GmlValue* argument = &number;
            gml.unit(instance, other, units[k].get(), 1, &argument);
            if (!units[k].object()) {
                return false;
            }
            sides[k] = *units[k].get();
        }
    }
    return true;
}

void placeElement(void* instance, void* other, GmlValue* element, const MadeSite& site)
{
    if (site.size.size() == 2 && gml.unit) {
        Held units[2];
        GmlValue sides[2];
        if (viewUnits(instance, other, {site.size[0], site.size[1]}, units, sides)) {
            callField(other, element, "size", {sides[0], sides[1]}, nullptr);
        }
    }
    if (site.across >= 0) {
        callField(other, element, "alignment", {realValue(site.across), realValue(site.down)}, nullptr);
    }
    if (site.sides.size() == 4 && gml.unit) {
        Held units[4];
        GmlValue sides[4];
        if (viewUnits(instance, other, site.sides, units, sides)) {
            callField(other, element, "margin", {sides[0], sides[1], sides[2], sides[3]}, nullptr);
        }
    }
    callField(other, element, "set_dirty", {}, nullptr);
    Held parent;
    if (getField(element, "parent", parent) && parent.object()) {
        callField(other, parent.get(), "set_dirty", {}, nullptr);
    }
}

bool makePicture(void* instance, void* other, GmlValue* parent, const Picture& picture, Held& image)
{
    const int sourceMaker = slotOf("gml_Script_gw_SpriteImage");
    const int imageMaker = slotOf("gml_Script_gw_Image");
    const int maker = slotOf("@@NewGMLObject@@");
    if (sourceMaker < 0 || imageMaker < 0 || maker < 0) {
        return false;
    }
    GmlValue sourceArguments[5] = {realValue(sourceMaker), realValue(picture.sprite), realValue(0),
                                   realValue(picture.color), realValue(picture.alpha)};
    GmlValue* sourcePointers[5] = {&sourceArguments[0], &sourceArguments[1], &sourceArguments[2], &sourceArguments[3],
                                   &sourceArguments[4]};
    Held source;
    gml.builtin(instance, other, source.get(), 5, maker, sourcePointers);
    if (!source.object()) {
        return false;
    }
    GmlValue imageArguments[3] = {realValue(imageMaker), *parent, *source.get()};
    GmlValue* imagePointers[3] = {&imageArguments[0], &imageArguments[1], &imageArguments[2]};
    gml.builtin(instance, other, image.get(), 3, maker, imagePointers);
    return image.object();
}

void checkMakes(void* instance, void* other, GmlValue* view)
{
    for (size_t i = 0; i < makes.size();) {
        Make& make = makes[i];
        Picture& picture = *make.picture;
        bool done = false;
        const auto target = namedElements.find(picture.place.into);
        if (target != namedElements.end()) {
            const std::vector<void*> place = target->second;
            withLive(other, view, place, [&](GmlValue* parent) {
                Held image;
                if (makePicture(instance, other, parent, picture, image)) {
                    std::vector<void*> chain = place;
                    chain.push_back(image.address());
                    moves.push_back({chain, &picture.place, 0, true});
                }
                done = true;
            });
        }
        if (done || ++make.age > 600) {
            makes.erase(makes.begin() + i);
        } else {
            ++i;
        }
    }
}

void checkMoves(void* instance, void* other, GmlValue* view)
{
    for (size_t i = 0; i < moves.size();) {
        Move& move = moves[i];
        MadeSite& site = *move.site;
        bool done = false;
        const auto target = namedElements.find(site.into);
        if (site.into.empty()) {
            withLive(other, view, move.chain, [&](GmlValue* element) {
                placeElement(instance, other, element, site);
                done = true;
            });
        } else if (target != namedElements.end()) {
            const std::vector<void*> place = target->second;
            if (std::find(place.begin(), place.end(), move.chain.back()) != place.end()) {
                done = true;
            } else if (!move.sent) {
                withLive(other, view, move.chain, [&](GmlValue* element) {
                    withLive(other, view, place, [&](GmlValue* destination) {
                        if (!site.last.empty() && site.last.back() != element->pointer) {
                            withLive(other, view, site.last,
                                     [&](GmlValue* old) { callField(other, old, "on_remove", {}, nullptr); });
                        }
                        Held parent;
                        getField(element, "parent", parent);
                        Held apart;
                        callField(other, element, "is_separated_render", {}, &apart);
                        site.apart = apart.number().value_or(0) != 0;
                        keepers = keepers || site.apart;
                        callField(other, element, site.apart ? "set_parent" : "relocate", {*destination}, nullptr);
                        if (parent.object()) {
                            callField(other, parent.get(), "set_dirty", {}, nullptr);
                        }
                        move.sent = true;
                    });
                });
                if (move.sent) {
                    rechain(move.chain, place);
                    site.last = move.chain;
                }
            } else {
                withLive(other, view, place, [&](GmlValue* destination) {
                    done = placeAt(other, destination, move.chain.back(), site.at);
                });
                if (done) {
                    withLive(other, view, move.chain,
                             [&](GmlValue* element) { placeElement(instance, other, element, site); });
                }
            }
        }
        if (done || ++move.age > 600) {
            moves.erase(moves.begin() + i);
        } else {
            ++i;
        }
    }
}

struct Layers {
    std::vector<void*> backdrops;
    std::vector<Box> above;
};

Layers layersOf(void* other, GmlValue* canvas, GmlValue* content)
{
    Layers layers;
    Held children;
    const auto inner = boxOf(content);
    if (!inner || inner->width <= 0 || inner->height <= 0 || !getField(canvas, "children", children) ||
        !children.object()) {
        return layers;
    }
    const int count = sizeOf(other, children.get()).value_or(0);
    for (int i = 0; i < count; ++i) {
        Held child;
        if (!childAt(other, children.get(), i, child) || child.address() == content->pointer) {
            continue;
        }
        const auto outer = boxOf(child.get());
        if (!outer) {
            continue;
        }
        if (outer->x <= inner->x && outer->y <= inner->y && outer->x + outer->width >= inner->x + inner->width &&
            outer->y + outer->height >= inner->y + inner->height) {
            layers.backdrops.push_back(child.address());
        } else {
            layers.above.push_back(*outer);
        }
    }
    return layers;
}

void checkHover(void* context)
{
    const auto* frame = static_cast<const Frame*>(context);
    GmlValue* view = variableOf(frame->self, "view");
    GmlValue* mouseX = variableOf(frame->self, "input_mouse_x");
    GmlValue* mouseY = variableOf(frame->self, "input_mouse_y");
    GmlValue* button = variableOf(frame->self, "input_mouse_check");
    if (!view || kindOf(*view) != valueObject || !mouseX || !mouseY) {
        return;
    }
    const double x = numberIn(*mouseX).value_or(-1e9);
    const double y = numberIn(*mouseY).value_or(-1e9);
    const bool down = button && numberIn(*button).value_or(0) != 0;
    const bool click = down && !mouseWasDown;
    mouseWasDown = down;
    ++hoverTicks;
    if (!moves.empty()) {
        checkMoves(frame->self, frame->other, view);
    }
    if (!makes.empty()) {
        checkMakes(frame->self, frame->other, view);
    }
    if (keepers && hoverTicks % 15 == 0) {
        for (const auto& entry : newSites) {
            if (entry.second.apart) {
                keepAbove(frame->other, view, entry.second.last);
            }
        }
    }
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - codeStart).count();
    std::unordered_set<void*> folded;
    for (auto entry = hoverGroups.begin(); entry != hoverGroups.end();) {
        HoverGroup& group = entry->second;
        std::vector<std::unique_ptr<Held>> held;
        GmlValue* current = view;
        bool found = !group.path.empty() && group.path.front().address == view->pointer;
        for (size_t level = 1; found && level < group.path.size(); ++level) {
            Held children;
            Link& link = group.path[level];
            auto child = std::make_unique<Held>();
            found = getField(current, "children", children) && children.object();
            bool hit = found && link.index >= 0 && childAt(frame->other, children.get(), link.index, *child) &&
                       child->address() == link.address;
            const int count = found && !hit ? sizeOf(frame->other, children.get()).value_or(0) : 0;
            for (int i = 0; i < count && !hit; ++i) {
                if (childAt(frame->other, children.get(), i, *child) && child->address() == link.address) {
                    link.index = i;
                    hit = true;
                }
            }
            found = hit;
            if (found) {
                current = child->get();
                held.push_back(std::move(child));
            }
        }
        if (!found) {
            foldClosed(group, folded);
            entry = ++group.misses > 600 ? hoverGroups.erase(entry) : std::next(entry);
            continue;
        }
        group.misses = 0;
        const Layers layers = held.size() >= 2 ? layersOf(frame->other, held[0]->get(), held[1]->get()) : Layers();
        for (void* backdrop : layers.backdrops) {
            folded.insert(backdrop);
            collapsed.insert(backdrop);
        }
        const bool covered = std::any_of(layers.above.begin(), layers.above.end(), [&](const Box& box) {
            return x >= box.x && x < box.x + box.width && y >= box.y && y < box.y + box.height;
        });
        const bool changed = hoverIn(frame->other, current, group, covered ? -1e9 : x, y, click && !covered, now, folded);
        if (changed) {
            for (auto up = held.rbegin(); up != held.rend(); ++up) {
                callField(frame->other, (*up)->get(), "resize_and_refresh", {}, nullptr);
            }
        }
        ++entry;
    }
    collapsed.swap(folded);
}

struct Made {
    void* other;
    GmlValue* element;
    MadeSite* site;
};

void shapeMade(void* context)
{
    const auto* made = static_cast<const Made*>(context);
    MadeSite& site = *made->site;
    if (site.show == 2) {
        placeHover(made->other, made->element, site.pin);
    } else if (site.show == 1) {
        showElement(made->other, made->element, false);
    }
    const bool placed = !site.into.empty() || site.across >= 0 || !site.sides.empty() || !site.size.empty();
    if (!site.label.empty() || placed) {
        const std::vector<void*> chain = chainOf(made->element);
        if (!site.label.empty()) {
            namedElements[site.label] = chain;
            for (Picture& picture : pictures) {
                if (picture.place.into == site.label) {
                    makes.push_back({&picture, 0});
                }
            }
        }
        if (placed) {
            moves.push_back({chain, &site, 0, false});
        }
    }
}

bool shielded(void (*work)(void*), void* context)
{
    __try {
        work(context);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

GmlValue* createdElement(void* instance, void* other, GmlValue* result, int count, int function, GmlValue** arguments)
{
    const auto site = newSites.find(reinterpret_cast<uintptr_t>(_ReturnAddress()));
    GmlValue* made = gml.builtin(instance, other, result, count, function, arguments);
    if (site != newSites.end() && made && kindOf(*made) == valueObject && !interfaceBroken) {
        Made context{other, made, &site->second};
        if (!shielded(&shapeMade, &context)) {
            interfaceBroken = true;
            log(L"changes to new parts stopped, the game's interface did not answer as expected");
        }
    }
    return made;
}

GmlValue* checkedLoading(void* instance, void* other, GmlValue* result, int count, GmlValue** arguments)
{
    GmlValue* answer = gml.loading(instance, other, result, count, arguments);
    if (!interfaceBroken && (!hoverGroups.empty() || !moves.empty() || !makes.empty() || keepers) && answer &&
        numberIn(*answer).value_or(1) == 0) {
        Frame frame{instance, other};
        if (!shielded(&checkHover, &frame)) {
            interfaceBroken = true;
            log(L"interface checks stopped, the game's interface did not answer as expected");
        }
    }
    return answer;
}

uintptr_t callTarget(uintptr_t site)
{
    return site + 5 + static_cast<intptr_t>(readAt<int32_t>(site + 1));
}

uintptr_t mostCommon(const std::map<uintptr_t, int>& tally)
{
    uintptr_t best = 0;
    int most = 0;
    for (const auto& entry : tally) {
        if (entry.second > most) {
            best = entry.first;
            most = entry.second;
        }
    }
    return best;
}

bool kindTestAt(const BYTE* code)
{
    if (code[0] == 0xA8 && code[1] == 0x46) {
        return true;
    }
    if (code[0] == 0xF6 && (code[1] & 0xF8) == 0xC0 && code[2] == 0x46) {
        return true;
    }
    return (code[0] == 0x40 || code[0] == 0x41) && code[1] == 0xF6 && (code[2] & 0xF8) == 0xC0 && code[3] == 0x46;
}

bool objectTestAt(const BYTE* code, size_t& length)
{
    const BYTE mask[] = {0xFF, 0xFF, 0xFF, 0x00};
    size_t at = 0;
    if (code[0] == 0x25) {
        at = 1;
    } else if (code[0] == 0x81 && (code[1] & 0xF8) == 0xE0) {
        at = 2;
    } else if (code[0] == 0x41 && code[1] == 0x81 && (code[2] & 0xF8) == 0xE0) {
        at = 3;
    } else {
        return false;
    }
    if (std::memcmp(code + at, mask, sizeof(mask)) != 0) {
        return false;
    }
    at += sizeof(mask);
    if (code[at] == 0x41) {
        ++at;
    }
    if (code[at] != 0x83 || (code[at + 1] & 0xF8) != 0xF8 || code[at + 2] != 0x06) {
        return false;
    }
    length = at + 3;
    return true;
}

std::wstring findRuntime(const GameImage& image, const std::map<std::string, AddressRange>& functions)
{
    if (gml.get && gml.method && gml.release) {
        return std::wstring();
    }
    std::map<uintptr_t, int> getters;
    std::map<uintptr_t, int> releases;
    std::map<uintptr_t, int> methods;
    for (const char* name : {"gml_Object_obj_gw_controller_Step_0", "gw_Spoiler", "gui_top_lord_card",
                             "gui_hud_resources_create", "gui_button_hover_add"}) {
        const AddressRange* range = functionNamed(functions, name);
        if (!range) {
            continue;
        }
        for (const auto& read : readsIn(*range, membersIn(image, *range))) {
            for (const auto& site : read.second) {
                getters[callTarget(site.site)]++;
            }
        }
        for (uintptr_t at = range->begin; at + 64 <= range->end; ++at) {
            const auto* code = reinterpret_cast<const BYTE*>(at);
            size_t length = 0;
            if (kindTestAt(code)) {
                for (uintptr_t next = at + 2; next < at + 24; ++next) {
                    if (readAt<BYTE>(next) == 0xE8) {
                        releases[callTarget(next)]++;
                        break;
                    }
                }
            } else if (objectTestAt(code, length)) {
                for (uintptr_t next = at + length; next < at + 64; ++next) {
                    if (readAt<BYTE>(next) == 0xE8) {
                        methods[callTarget(next)]++;
                        break;
                    }
                }
            }
        }
    }
    const uintptr_t getter = mostCommon(getters);
    const uintptr_t release = mostCommon(releases);
    methods.erase(getter);
    methods.erase(release);
    const uintptr_t method = mostCommon(methods);
    if (!image.code.holds(getter, 1) || !image.code.holds(release, 1) || !image.code.holds(method, 1)) {
        return L"the game's own calls for members and methods are not found";
    }
    for (const char* name : gmlNames) {
        const uintptr_t slot = memberSlot(image, name);
        if (!slot) {
            return L"the game has no member " + wide(name);
        }
        gml.slots[name] = slot;
    }
    if (const AddressRange* maker = functionNamed(functions, "gw_Image")) {
        const auto members = membersIn(image, *maker);
        for (const char* name : pictureNames) {
            const auto found = members.find(name);
            if (found != members.end()) {
                gml.slots[name] = found->second.front().value;
            }
        }
    }
    gml.get = reinterpret_cast<GmlGet>(getter);
    gml.release = reinterpret_cast<GmlRelease>(release);
    gml.method = reinterpret_cast<GmlMethod>(method);
    return std::wstring();
}

struct FunctionScan {
    const AddressRange* range = nullptr;
    std::vector<CodeValue> values;
    std::vector<CodeValue> immediates;
    std::map<uintptr_t, std::vector<CodeValue>> calls;
    std::map<std::string, std::vector<CodeValue>> members;
    std::map<std::string, std::vector<CodeValue>> reads;
};

std::vector<CodeValue> newSitesIn(const FunctionScan& scan, const std::string& name)
{
    std::vector<CodeValue> found;
    const auto loads = scan.members.find("gml_Script_" + name);
    const auto makers = scan.members.find("@@NewGMLObject@@");
    if (loads == scan.members.end() || makers == scan.members.end()) {
        return found;
    }
    for (const auto& load : loads->second) {
        uintptr_t maker = 0;
        for (const auto& candidate : makers->second) {
            if (candidate.site > load.site && (!maker || candidate.site < maker)) {
                maker = candidate.site;
            }
        }
        for (uintptr_t at = maker; maker && at < maker + 64 && at + 5 <= scan.range->end; ++at) {
            if (readAt<BYTE>(at) == 0xE8) {
                found.push_back({at, 0});
                break;
            }
        }
    }
    return found;
}

std::wstring readText(const Json& set)
{
    if (set.kind != Json::Kind::Object) {
        return wide(compact(set));
    }
    const Json& range = set.items[*lastKey(set, "noise")];
    return L"noise between " + wide(compact(range.items[0])) + L" and " + wide(compact(range.items[1]));
}

std::optional<uint32_t> spriteIndexOf(const std::string& name)
{
    static std::map<std::string, uint32_t> indices;
    static std::wstring readFrom;
    if (readFrom != gameDirectory) {
        readFrom = gameDirectory;
        indices.clear();
        try {
            const MappedFile file(gameDirectory + L"\\data.win");
            const auto chunks = chunksOf(file);
            const Chunk& sprites = chunkNamed(chunks, "SPRT");
            const uint32_t count = file.u32(sprites.data);
            for (uint32_t i = 0; i < count; ++i) {
                const uint32_t at = file.u32(sprites.data + 4 + 4 * size_t{i});
                if (at) {
                    indices[assetName(file, file.u32(at))] = i;
                }
            }
        } catch (const std::exception&) {
            indices.clear();
        }
    }
    std::string key = name;
    for (auto& c : key) {
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    }
    const auto found = indices.find(key);
    return found == indices.end() ? std::nullopt : std::optional<uint32_t>(found->second);
}

std::wstring madeText(const CodeEdit& edit)
{
    std::vector<std::wstring> parts;
    if (edit.set.kind == Json::Kind::String) {
        parts.push_back(edit.pin ? L"shows on hover and stays open on a click" : L"shows on hover");
    } else if (edit.set.kind == Json::Kind::Boolean) {
        parts.push_back(L"is hidden");
    }
    if (!edit.label.empty()) {
        parts.push_back(L"is named " + wide(edit.label));
    }
    if (!edit.to.empty()) {
        parts.push_back((edit.change == CodeChange::Picture ? L"goes into " : L"moves into ") + wide(edit.to) +
                        (edit.at ? L" as its " + ordinal(edit.at) + L" part" : L""));
    }
    if (edit.size.kind == Json::Kind::Array) {
        parts.push_back(L"is " + wide(compact(edit.size.items[0])) + L" by " + wide(compact(edit.size.items[1])));
    }
    if (edit.align.kind == Json::Kind::Array) {
        parts.push_back(L"sits " + wide(edit.align.items[0].text) + L" " + wide(edit.align.items[1].text));
    }
    if (edit.margin.kind == Json::Kind::Array) {
        std::wstring sides;
        for (const Json& side : edit.margin.items) {
            sides += (sides.empty() ? L"" : L" ") + (side.kind == Json::Kind::Null ? L"0" : wide(compact(side)));
        }
        parts.push_back(L"takes the margin " + sides);
    }
    std::wstring text;
    for (size_t i = 0; i < parts.size(); ++i) {
        text += (i == 0 ? L" " : i + 1 == parts.size() ? L" and " : L", ") + parts[i];
    }
    return text;
}

struct CodePlan {
    const CodeEdit* edit;
    std::vector<CodeValue> targets;
    std::vector<std::array<BYTE, 16>> payloads;
    uintptr_t destination;
    std::wstring done;
};

std::variant<CodePlan, std::wstring> planEdit(const GameImage& image, const CodeEdit& edit, const FunctionScan& scan,
                                              const std::map<std::string, AddressRange>& functions)
{
    const CodeChange change = edit.change;
    const std::wstring name = wide(edit.name);
    const std::wstring subject = change == CodeChange::Value    ? wide(compact(edit.find))
                                 : change == CodeChange::Member ? L"the use of " + name
                                 : change == CodeChange::Read   ? name
                                 : change == CodeChange::New    ? L"new " + name
                                                                : L"the call to " + name;
    if (!scan.range) {
        return std::wstring(L"the game has no such function");
    }
    const bool units = edit.margin.kind == Json::Kind::Array || edit.size.kind == Json::Kind::Array;
    if (change == CodeChange::Picture) {
        const auto sprite = spriteIndexOf(edit.name);
        if (!runtimeProblem.empty()) {
            return runtimeProblem;
        }
        if (!sprite) {
            return L"the game has no sprite " + name;
        }
        if (std::any_of(std::begin(pictureNames), std::end(pictureNames),
                        [](const char* slot) { return !gml.slots.count(slot); })) {
            return std::wstring(L"the game does not make pictures the way NLSE knows");
        }
        if (units && !gml.unit) {
            return std::wstring(L"the game has no gw_ABSV for a size or a margin");
        }
        if (std::none_of(codeEdits.begin(), codeEdits.end(),
                         [&](const CodeEdit& other) { return other.label == edit.to; })) {
            return L"no change names " + wide(edit.to);
        }
        return CodePlan{&edit, {}, {}, *sprite, L"a picture of " + name + madeText(edit)};
    }
    std::vector<CodeValue> matches;
    uintptr_t destination = 0;
    if (change == CodeChange::New) {
        if (!runtimeProblem.empty()) {
            return runtimeProblem;
        }
        if (units && !gml.unit) {
            return std::wstring(L"the game has no gw_ABSV for a size or a margin");
        }
        matches = newSitesIn(scan, edit.name);
        for (const auto& match : matches) {
            const auto maker = reinterpret_cast<GmlBuiltin>(callTarget(match.site));
            if (!gml.builtin) {
                gml.builtin = maker;
            }
            if (maker != gml.builtin) {
                return L"new " + name + L" is not made the way the game makes the rest";
            }
        }
    } else if (change == CodeChange::Value) {
        for (const auto& value : scan.values) {
            if (valueMatches(value.value, edit.find)) {
                matches.push_back(value);
            }
        }
        for (const auto& value : scan.immediates) {
            if (immediateMatches(value.value, edit.find)) {
                matches.push_back(value);
            }
        }
        std::sort(matches.begin(), matches.end(),
                  [](const CodeValue& a, const CodeValue& b) { return a.site < b.site; });
    } else if (change == CodeChange::Member) {
        const auto other = scan.members.find(edit.to);
        destination = other != scan.members.end() ? other->second.front().value : memberSlot(image, edit.to);
        if (!destination) {
            return L"the game has no member " + wide(edit.to);
        }
        const auto found = scan.members.find(edit.name);
        if (found != scan.members.end()) {
            matches = found->second;
        }
    } else if (change == CodeChange::Read) {
        const auto found = scan.reads.find(edit.name);
        if (found != scan.reads.end()) {
            matches = found->second;
        }
    } else {
        const AddressRange* target = functionNamed(functions, edit.name);
        const AddressRange* other = change == CodeChange::Call ? functionNamed(functions, edit.to) : nullptr;
        if (!target || (change == CodeChange::Call && !other)) {
            return L"the game has no function " + (target ? wide(edit.to) : name);
        }
        destination = other ? other->begin : 0;
        const auto found = scan.calls.find(target->begin);
        if (found != scan.calls.end()) {
            matches = found->second;
        }
    }
    const std::wstring counted = change == CodeChange::Value    ? subject + L" appears "
                                 : change == CodeChange::Member ? name + L" is used "
                                 : change == CodeChange::Read   ? name + L" is read "
                                 : change == CodeChange::New    ? name + L" is made "
                                                                : name + L" is called ";
    if (matches.empty()) {
        return change == CodeChange::Value    ? subject + L" not found"
               : change == CodeChange::Member ? L"no use of " + name
               : change == CodeChange::Read   ? L"no read of " + name
               : change == CodeChange::New    ? L"no new " + name
                                              : L"no call to " + name;
    }
    if (edit.count && matches.size() != edit.count) {
        return counted + times(matches.size()) + L" instead of " + times(edit.count);
    }
    if (edit.nth > matches.size()) {
        return counted + L"only " + times(matches.size());
    }
    if (!edit.label.empty() && !edit.nth && matches.size() > 1) {
        return L"a name needs one place, " + name + L" is made " + times(matches.size());
    }
    if (change == CodeChange::New && !edit.to.empty() &&
        std::none_of(codeEdits.begin(), codeEdits.end(), [&](const CodeEdit& other) { return other.label == edit.to; })) {
        return L"no change names " + wide(edit.to);
    }
    CodePlan plan{&edit, edit.nth ? std::vector<CodeValue>{matches[edit.nth - 1]} : matches, {}, destination, {}};
    for (const auto& target : plan.targets) {
        std::array<BYTE, 16> bytes{};
        if (change == CodeChange::Value && target.immediate) {
            const double number = numberOf(edit.set).value_or(NAN);
            if (!std::isfinite(number)) {
                return subject + L": the game keeps a number there";
            }
            std::memcpy(bytes.data(), &number, sizeof(number));
        } else if (change == CodeChange::Value) {
            if (const auto refusal = replacement(target.value, edit.set, bytes)) {
                return subject + L": " + *refusal;
            }
        } else if ((change == CodeChange::Call && !reaches(target.site + 5, destination)) ||
                   (change == CodeChange::Member && !reaches(target.site + 4, destination))) {
            return wide(edit.to) + L" is out of reach";
        }
        plan.payloads.push_back(bytes);
    }
    const size_t places = plan.targets.size();
    const std::wstring shown =
        edit.nth ? L"the " + ordinal(edit.nth) + L" " +
                       (change == CodeChange::Value    ? subject
                        : change == CodeChange::Member ? L"use of " + name
                        : change == CodeChange::Read   ? L"read of " + name
                        : change == CodeChange::New    ? L"new " + name
                                                       : L"call to " + name)
        : change != CodeChange::Value && places > 1
            ? (change == CodeChange::Member ? L"uses of "
               : change == CodeChange::Read ? L"reads of "
               : change == CodeChange::New  ? L"each new "
                                            : L"calls to ") +
                  name
            : subject;
    plan.done = shown +
                (change == CodeChange::Skip     ? L" skipped"
                 : change == CodeChange::Call   ? L" now goes to " + wide(edit.to)
                 : change == CodeChange::Member ? L" now reads " + wide(edit.to)
                 : change == CodeChange::Read   ? L" now gets " + readText(edit.set)
                 : change == CodeChange::New    ? madeText(edit)
                                                : L" to " + wide(compact(edit.set))) +
                (places > 1 ? L", " + std::to_wstring(places) + L" places" : L"");
    return plan;
}

MadeSite placeOf(const CodeEdit& edit)
{
    MadeSite place;
    place.into = edit.to;
    place.at = edit.at;
    if (edit.align.kind == Json::Kind::Array) {
        place.across = wordIn(edit.align.items[0], acrossWords);
        place.down = wordIn(edit.align.items[1], downWords);
    }
    for (const Json& side : edit.margin.items) {
        place.sides.push_back(side.kind == Json::Kind::Null ? std::nullopt : numberOf(side));
    }
    for (const Json& side : edit.size.items) {
        place.size.push_back(numberOf(side).value_or(0));
    }
    return place;
}

bool writePlan(const CodePlan& plan)
{
    if (plan.edit->change == CodeChange::Picture) {
        pictures.push_back({static_cast<uint32_t>(plan.destination), plan.edit->color, plan.edit->alpha,
                            placeOf(*plan.edit)});
        return true;
    }
    for (size_t i = 0; i < plan.targets.size(); ++i) {
        const uintptr_t site = plan.targets[i].site;
        bool done = false;
        if (plan.edit->change == CodeChange::Skip) {
            done = writeCall(site, reinterpret_cast<void*>(&skippedScript)) != 0;
        } else if (plan.edit->change == CodeChange::Call) {
            const auto displacement = static_cast<int32_t>(static_cast<intptr_t>(plan.destination - (site + 5)));
            done = writeMemory(site + 1, &displacement, sizeof(displacement));
        } else if (plan.edit->change == CodeChange::Member) {
            const auto displacement = static_cast<int32_t>(static_cast<intptr_t>(plan.destination - (site + 4)));
            done = writeMemory(site, &displacement, sizeof(displacement));
        } else if (plan.edit->change == CodeChange::Read) {
            ReadValue value = *readValueOf(plan.edit->set);
            std::random_device random;
            value.seed = (static_cast<uint64_t>(random()) << 32 | random()) ^ site;
            readValues[site + 5] = value;
            done = writeCall(site, reinterpret_cast<void*>(&readMember)) != 0;
        } else if (plan.edit->change == CodeChange::New) {
            const CodeEdit& edit = *plan.edit;
            MadeSite& made = newSites[site + 5];
            made = placeOf(edit);
            made.show = edit.set.kind == Json::Kind::String ? 2 : edit.set.kind == Json::Kind::Boolean ? 1 : 0;
            made.label = edit.label;
            made.pin = edit.pin;
            done = writeCall(site, reinterpret_cast<void*>(&createdElement)) != 0;
        } else if (plan.targets[i].immediate) {
            done = writeMemory(plan.targets[i].value, plan.payloads[i].data(), sizeof(double));
        } else {
            const uintptr_t stored = storeValue(site, plan.payloads[i]);
            const auto displacement = static_cast<int32_t>(static_cast<intptr_t>(stored - (site + 7)));
            done = stored && writeMemory(site + 3, &displacement, sizeof(displacement));
        }
        if (!done) {
            return false;
        }
    }
    return true;
}

void applyCodeEdits(const GameImage& image)
{
    if (codeEdits.empty()) {
        return;
    }
    const auto key = [](const CodeEdit& edit) {
        const std::string what = edit.change == CodeChange::Value     ? compact(edit.find)
                                 : edit.change == CodeChange::Member  ? "member " + edit.name
                                 : edit.change == CodeChange::Read    ? "read " + edit.name
                                 : edit.change == CodeChange::New     ? "new " + edit.name
                                 : edit.change == CodeChange::Picture ? "picture " + std::to_string(reinterpret_cast<uintptr_t>(&edit))
                                                                      : "call " + edit.name;
        return std::make_tuple(edit.function, what, edit.nth);
    };
    std::vector<const Mod*> order;
    std::map<const Mod*, std::vector<const CodeEdit*>> byMod;
    for (const auto& edit : codeEdits) {
        if (!byMod.count(edit.mod)) {
            order.push_back(edit.mod);
        }
        byMod[edit.mod].push_back(&edit);
    }
    const std::map<std::string, AddressRange> functions = gmlFunctions(image);
    std::map<std::string, FunctionScan> scans;
    const auto scanOf = [&](const std::string& name) -> const FunctionScan& {
        auto found = scans.find(name);
        if (found == scans.end()) {
            FunctionScan scan;
            scan.range = functionNamed(functions, name);
            if (scan.range) {
                scan.values = valuesIn(image, *scan.range);
                scan.immediates = immediatesIn(*scan.range);
                scan.calls = callsFrom(*scan.range);
                scan.members = membersIn(image, *scan.range);
                scan.reads = readsIn(*scan.range, scan.members);
            }
            found = scans.emplace(name, std::move(scan)).first;
        }
        return found->second;
    };
    if (std::any_of(codeEdits.begin(), codeEdits.end(), [](const CodeEdit& edit) {
            return edit.change == CodeChange::New || edit.change == CodeChange::Picture;
        })) {
        runtimeProblem = findRuntime(image, functions);
        const AddressRange* unit = functionNamed(functions, "gw_ABSV");
        gml.unit = unit ? reinterpret_cast<GmlScript>(unit->begin) : nullptr;
    }
    std::map<const Mod*, std::vector<CodePlan>> plans;
    std::map<const Mod*, std::vector<std::wstring>> problems;
    for (const Mod* mod : order) {
        std::set<uintptr_t> claimed;
        for (const CodeEdit* edit : byMod[mod]) {
            const std::wstring where = L"  " + wide(edit->function) + L": ";
            auto result = planEdit(image, *edit, scanOf(edit->function), functions);
            if (const auto* problem = std::get_if<std::wstring>(&result)) {
                problems[mod].push_back(where + *problem);
                continue;
            }
            CodePlan& plan = std::get<CodePlan>(result);
            bool clash = false;
            for (const auto& target : plan.targets) {
                clash = clash || !claimed.insert(target.site).second;
            }
            if (clash) {
                problems[mod].push_back(where + L"two of its changes take the same place");
                continue;
            }
            plans[mod].push_back(std::move(plan));
        }
    }
    std::map<std::tuple<std::string, std::string, uint32_t>, const CodeEdit*> latest;
    for (const Mod* mod : order) {
        for (const auto& plan : plans[mod]) {
            if (problems[mod].empty()) {
                latest[key(*plan.edit)] = plan.edit;
            }
        }
    }
    for (const Mod* mod : order) {
        if (!problems[mod].empty()) {
            const size_t count = problems[mod].size();
            log(L"code " + mod->name + L": none made, " + std::to_wstring(count) +
                (count == 1 ? L" change does" : L" changes do") + L" not match the game");
            for (const auto& problem : problems[mod]) {
                log(problem);
            }
            continue;
        }
        log(L"code " + mod->name + L":");
        for (const auto& plan : plans[mod]) {
            const std::wstring where = L"  " + wide(plan.edit->function) + L": ";
            const CodeEdit* winner = latest[key(*plan.edit)];
            if (winner != plan.edit) {
                log(where + L"a change overridden by " + winner->mod->name);
                continue;
            }
            log(where + (writePlan(plan) ? plan.done : L"the code cannot be written"));
        }
    }
    const bool hovers =
        std::any_of(newSites.begin(), newSites.end(), [](const auto& site) { return site.second.show == 2; });
    if (!hovers && pictures.empty() && std::all_of(newSites.begin(), newSites.end(), [](const auto& site) {
            return site.second.into.empty() && site.second.across < 0 && site.second.sides.empty() &&
                   site.second.size.empty();
        })) {
        return;
    }
    const AddressRange* step = functionNamed(functions, "gml_Object_obj_gw_controller_Step_0");
    const AddressRange* loading = functionNamed(functions, "game_is_loading");
    const std::vector<CodeValue> sites = step && loading ? callsIn(*step, loading->begin) : std::vector<CodeValue>();
    const uintptr_t original = sites.size() == 1 ? writeCall(sites[0].site, reinterpret_cast<void*>(&checkedLoading)) : 0;
    gml.loading = reinterpret_cast<GmlScript>(original);
    log(original ? L"the interface is checked every frame from obj_gw_controller"
                 : L"the interface cannot be checked every frame, obj_gw_controller has changed");
    if (!hovers) {
        return;
    }
    const AddressRange* container = functionNamed(functions, "gw_Container");
    const uintptr_t measure = container ? methodMadeAfter(image, *container, "_measure_size_child") : 0;
    measureOriginal = measure ? hookFunction(measure, reinterpret_cast<void*>(&measuredChild)) : nullptr;
    log(measureOriginal ? L"folded parts give up their room" : L"folded parts keep their room, gw_Container has changed");
}

using EntryFunction = DWORD(WINAPI*)(void*);

EntryFunction gameEntry;
BYTE gameEntryBytes[14];

DWORD WINAPI enterGame(void* parameter)
{
    writeMemory(reinterpret_cast<uintptr_t>(gameEntry), gameEntryBytes, sizeof(gameEntryBytes));
    try {
        buildSprites();
    } catch (...) {
        log(L"sprites stopped on an error");
    }
    try {
        applyCodeEdits(imageAt(gameBase));
    } catch (...) {
        log(L"code changes stopped on an error");
    }
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
