#pragma once

#include <windows.h>

// User-visible texts in English or Simplified Chinese. The language follows TrafficMonitor's own setting
// (ITrafficMonitor::GetLanguageId, set in OnInitialize); until then, and for "follow system", the Windows UI
// language. Every Chinese variant (zh-CN, zh-TW, zh-HK) gets Simplified Chinese. The log stays English.
// Thread-safe: the language is an atomic flag, the texts are constants.
namespace lang
{
    enum class Text
    {
        PluginName,
        PluginDescription,
        HeadphonesItem,         // item name in TrafficMonitor's display settings
        MouseItem,
        MouseLabel,             // default label of the mouse item ("Mouse "), with trailing space
        MouseLabelIni,          // default Label= for [Mouse]
        Headphones,             // tooltip
        Mouse,
        LabelSeparator,         // ": " after the device in the tooltip
        DeviceNameSeparator,    // separator between the device name and its status
        NotConnected,
        NoValue,
        Charging,
        ChargingWithPercent,    // "charging, " followed by the percentage
        Full,
        AsOf,                   // ", as of " followed by hh:mm
        Unreachable,
        LowBattery,             // notification, followed by the device name
        ProviderBluetooth,
        ProviderLogitech,
        ProviderNone,
        IntervalRange,
        WarningRange,
        SaveFailed,
        SettingsFileTooLarge,
        SettingsPathMissing,
        ErrorCode,
    };

    void SetLanguage(LANGID id);    // 0 = Windows UI language
    bool IsChinese();
    LANGID DialogLanguage();        // language of the dialog template to load
    const wchar_t* Tr(Text text);
}
