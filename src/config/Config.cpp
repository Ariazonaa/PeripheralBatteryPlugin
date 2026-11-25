#include "config/Config.h"

#include <windows.h>

#include <algorithm>

#include "resource.h"
#include "utils/Lang.h"

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace
{
    // config/PeripheralBattery.ini, embedded as a resource – the same file as in the release zip.
    bool DefaultFile(const void*& data, DWORD& size)
    {
        const HMODULE module = reinterpret_cast<HMODULE>(&__ImageBase);
        // MAKEINTRESOURCEW(10) = RT_RCDATA, independent of the UNICODE macro.
        HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(IDR_DEFAULT_CONFIG), MAKEINTRESOURCEW(10));
        HGLOBAL loaded = resource ? LoadResource(module, resource) : nullptr;
        data = loaded ? LockResource(loaded) : nullptr;
        size = resource ? SizeofResource(module, resource) : 0;
        return data != nullptr && size > 0;
    }

    std::wstring ReadString(const std::wstring& path, const wchar_t* section, const wchar_t* key,
        const std::wstring& fallback)
    {
        wchar_t buffer[256] = {};
        GetPrivateProfileStringW(section, key, fallback.c_str(), buffer, static_cast<DWORD>(std::size(buffer)),
            path.c_str());
        return buffer;
    }

    int ReadInt(const std::wstring& path, const wchar_t* section, const wchar_t* key, int fallback)
    {
        return static_cast<int>(GetPrivateProfileIntW(section, key, fallback, path.c_str()));
    }

    bool ReadBool(const std::wstring& path, const wchar_t* section, const wchar_t* key, bool fallback)
    {
        return ReadInt(path, section, key, fallback ? 1 : 0) != 0;
    }

    DeviceConfig ReadDevice(const std::wstring& path, const wchar_t* section, const DeviceConfig& defaults)
    {
        DeviceConfig device;
        device.enabled = ReadBool(path, section, L"Enabled", defaults.enabled);
        device.provider = ReadString(path, section, L"Provider", defaults.provider);
        device.nameContains = ReadString(path, section, L"NameContains", defaults.nameContains);
        return device;
    }
}

Config LoadConfig(const std::wstring& path)
{
    Config defaults;
    defaults.display.mouseLabel = lang::Tr(lang::Text::MouseLabelIni);
    if (path.empty() || GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
        return defaults;

    Config config = defaults;
    config.updateIntervalSeconds = std::clamp(
        ReadInt(path, L"General", L"UpdateInterval", defaults.updateIntervalSeconds),
        Config::kMinInterval, Config::kMaxInterval);
    config.display.showDisconnected = ReadBool(path, L"General", L"ShowDisconnected", defaults.display.showDisconnected);
    config.display.showPercentSign = ReadBool(path, L"General", L"ShowPercentSign", defaults.display.showPercentSign);
    config.display.showCharging = ReadBool(path, L"General", L"ShowCharging", defaults.display.showCharging);
    config.display.colors = ReadBool(path, L"General", L"Colors", defaults.display.colors);
    config.display.headphonesLabel = ReadString(path, L"Headphones", L"Label", defaults.display.headphonesLabel);
    config.display.mouseLabel = ReadString(path, L"Mouse", L"Label", defaults.display.mouseLabel);
    config.verboseLogging = ReadBool(path, L"General", L"VerboseLogging", defaults.verboseLogging);
    config.lowBatteryWarning = std::clamp(
        ReadInt(path, L"General", L"LowBatteryWarning", defaults.lowBatteryWarning), 0, 100);
    config.headphones = ReadDevice(path, L"Headphones", defaults.headphones);
    config.mouse = ReadDevice(path, L"Mouse", defaults.mouse);
    return config;
}

int AddMissingKeys(const std::wstring& path)
{
    if (path.empty() || GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
        return 0;

    struct Key { const wchar_t* section; const wchar_t* name; const wchar_t* value; };
    const Key kKeys[] = {
        { L"General", L"UpdateInterval", L"60" },
        { L"General", L"ShowDisconnected", L"1" },
        { L"General", L"ShowPercentSign", L"1" },
        { L"General", L"ShowCharging", L"1" },
        { L"General", L"LowBatteryWarning", L"15" },
        { L"General", L"Colors", L"1" },
        { L"General", L"VerboseLogging", L"0" },
        { L"Headphones", L"Label", L"M4" },
        { L"Mouse", L"Label", lang::Tr(lang::Text::MouseLabelIni) },
    };
    constexpr wchar_t kMissing[] = L"\x1F<missing>";

    int added = 0;
    for (const Key& key : kKeys)
    {
        if (ReadString(path, key.section, key.name, kMissing) == kMissing
            && WritePrivateProfileStringW(key.section, key.name, key.value, path.c_str()))
        {
            ++added;
        }
    }
    return added;
}

namespace
{
    std::wstring ErrorText(DWORD code)
    {
        wchar_t* text = nullptr;
        const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
                | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, lang::DialogLanguage(),
                reinterpret_cast<wchar_t*>(&text), 0, nullptr);
        std::wstring result = length > 0 ? std::wstring(text, length)
                                        : lang::Tr(lang::Text::ErrorCode) + std::to_wstring(code);
        if (text != nullptr)
            LocalFree(text);
        while (!result.empty() && (result.back() == L'\n' || result.back() == L'\r' || result.back() == L' '))
            result.pop_back();
        return result;
    }
}

namespace
{
    // WritePrivateProfileStringW only writes Unicode to a file with a UTF-16 BOM; in an ANSI or UTF-8
    // file (e.g. created by hand in Notepad, or 0 bytes) umlauts and other characters would silently
    // become "?". So convert such a file to UTF-16 LE before writing.
    bool EnsureUtf16File(const std::wstring& path, std::wstring& error)
    {
        std::string bytes;
        {
            HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE)
            {
                error = ErrorText(GetLastError());
                return false;
            }
            LARGE_INTEGER size{};
            const bool sized = GetFileSizeEx(file, &size) && size.QuadPart < 1024 * 1024;
            bytes.resize(sized ? static_cast<size_t>(size.QuadPart) : 0);
            DWORD read = 0;
            const bool ok = sized && (bytes.empty()
                || (ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) && read == bytes.size()));
            const DWORD code = GetLastError();
            CloseHandle(file);
            if (!ok)
            {
                error = sized ? ErrorText(code) : lang::Tr(lang::Text::SettingsFileTooLarge);
                return false;
            }
        }
        if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF && static_cast<unsigned char>(bytes[1]) == 0xFE)
            return true;

        // UTF-8 (with or without BOM), otherwise the ANSI code page – the way Windows has read the file so far.
        size_t offset = 0;
        if (bytes.size() >= 3 && bytes.compare(0, 3, "\xEF\xBB\xBF") == 0)
            offset = 3;
        const char* source = bytes.data() + offset;
        const int sourceLength = static_cast<int>(bytes.size() - offset);
        UINT codePage = CP_UTF8;
        int length = sourceLength == 0 ? 0 : MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, source, sourceLength, nullptr, 0);
        if (sourceLength > 0 && length == 0)
        {
            codePage = CP_ACP;
            length = MultiByteToWideChar(CP_ACP, 0, source, sourceLength, nullptr, 0);
        }
        std::wstring text(static_cast<size_t>(length), L'\0');
        if (length > 0)
            MultiByteToWideChar(codePage, 0, source, sourceLength, text.data(), length);

        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            error = ErrorText(GetLastError());
            return false;
        }
        const unsigned char bom[2] = { 0xFF, 0xFE };
        DWORD written = 0;
        const DWORD textBytes = static_cast<DWORD>(text.size() * sizeof(wchar_t));
        const bool ok = WriteFile(file, bom, 2, &written, nullptr) && written == 2
            && (textBytes == 0 || (WriteFile(file, text.data(), textBytes, &written, nullptr) && written == textBytes));
        const DWORD code = GetLastError();
        CloseHandle(file);
        if (!ok)
            error = ErrorText(code);
        return ok;
    }
}

bool SaveConfig(const std::wstring& path, const Config& config, std::wstring& error)
{
    if (path.empty())
    {
        error = lang::Tr(lang::Text::SettingsPathMissing);
        return false;
    }
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        if (!WriteDefaultConfigIfMissing(path))
        {
            error = ErrorText(GetLastError());
            return false;
        }
    }
    else if (!EnsureUtf16File(path, error))
    {
        return false;
    }

    auto flag = [](bool value) { return std::wstring(value ? L"1" : L"0"); };
    struct Entry { const wchar_t* section; const wchar_t* key; std::wstring value; };
    const Entry entries[] = {
        { L"General", L"UpdateInterval", std::to_wstring(config.updateIntervalSeconds) },
        { L"General", L"ShowDisconnected", flag(config.display.showDisconnected) },
        { L"General", L"ShowPercentSign", flag(config.display.showPercentSign) },
        { L"General", L"ShowCharging", flag(config.display.showCharging) },
        { L"General", L"LowBatteryWarning", std::to_wstring(config.lowBatteryWarning) },
        { L"General", L"Colors", flag(config.display.colors) },
        { L"General", L"VerboseLogging", flag(config.verboseLogging) },
        { L"Headphones", L"Enabled", flag(config.headphones.enabled) },
        { L"Headphones", L"Provider", config.headphones.provider },
        { L"Headphones", L"NameContains", config.headphones.nameContains },
        { L"Headphones", L"Label", config.display.headphonesLabel },
        { L"Mouse", L"Enabled", flag(config.mouse.enabled) },
        { L"Mouse", L"Provider", config.mouse.provider },
        { L"Mouse", L"NameContains", config.mouse.nameContains },
        { L"Mouse", L"Label", config.display.mouseLabel },
    };
    for (const Entry& entry : entries)
    {
        if (!WritePrivateProfileStringW(entry.section, entry.key, entry.value.c_str(), path.c_str()))
        {
            error = ErrorText(GetLastError());
            return false;
        }
    }
    return true;
}

bool WriteDefaultConfigIfMissing(const std::wstring& path)
{
    const void* data = nullptr;
    DWORD size = 0;
    if (path.empty() || !DefaultFile(data, size))
        return false;
    // CREATE_NEW: an existing file of the user is never overwritten.
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;

    DWORD written = 0;
    const bool ok = WriteFile(file, data, size, &written, nullptr) && written == size;
    CloseHandle(file);
    // The template is English; a new INI in a Chinese TrafficMonitor gets the Chinese mouse label.
    if (ok && lang::IsChinese())
        WritePrivateProfileStringW(L"Mouse", L"Label", lang::Tr(lang::Text::MouseLabelIni), path.c_str());
    return ok;
}
