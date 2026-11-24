#include "Plugin.h"

#include <cwchar>

#include "Version.h"
#include "bluetooth/BluetoothScanner.h"
#include "ui/OptionsDialog.h"
#include "utils/Lang.h"

namespace
{
    // Owner of the options dialog. TrafficMonitor's taskbar window is a child of the explorer.exe
    // taskbar – a window of another process as owner would couple its input to ours. Better no
    // owner at all then.
    HWND OwnerFor(void* parent)
    {
        const HWND window = static_cast<HWND>(parent);
        if (window == nullptr || !IsWindow(window))
            return nullptr;
        const HWND root = GetAncestor(window, GA_ROOT);
        DWORD process = 0;
        GetWindowThreadProcessId(root, &process);
        return process == GetCurrentProcessId() ? root : nullptr;
    }
}

PeripheralBatteryPlugin& PeripheralBatteryPlugin::Instance()
{
    // Lives until the DLL is unloaded; TrafficMonitor never frees the object.
    static PeripheralBatteryPlugin instance;
    return instance;
}

IPluginItem* PeripheralBatteryPlugin::GetItem(int index)
{
    switch (index)
    {
    case 0:
        return &headphones_;
    case 1:
        return &mouse_;
    default:
        return nullptr;
    }
}

void PeripheralBatteryPlugin::DataRequired()
{
    // Runs on the host's monitor thread: nothing here may block or let an exception
    // escape into TrafficMonitor.
    try
    {
        manager_.Update();
    }
    catch (...)
    {
    }
}

const wchar_t* PeripheralBatteryPlugin::GetInfo(PluginInfoIndex index)
{
    switch (index)
    {
    case TMI_NAME:
        return lang::Tr(lang::Text::PluginName);
    case TMI_DESCRIPTION:
        return lang::Tr(lang::Text::PluginDescription);
    case TMI_AUTHOR:
        return L"Ariazonaa";
    case TMI_COPYRIGHT:
        return L"Copyright (C) 2026 Ariazonaa";
    case TMI_VERSION:
        return kPluginVersion;
    case TMI_URL:
        return L"https://github.com/Ariazonaa/PeripheralBatteryPlugin";
    default:
        return L"";
    }
}

void PeripheralBatteryPlugin::OnExtenedInfo(ExtendedInfoIndex index, const wchar_t* data)
{
    if (data == nullptr || *data == L'\0')
        return;
    try
    {
        switch (index)
        {
        case EI_CONFIG_DIR:
        {
            // Arrives at load time (before the first DataRequired): "<config_dir>plugins\".
            std::wstring path = data;
            if (path.back() != L'\\' && path.back() != L'/')
                path.push_back(L'\\');
            manager_.SetConfigPath(path + L"PeripheralBattery.ini");
            break;
        }
        // The host sends the following on the UI thread right before DrawItem (colors as decimal COLORREF).
        case EI_DRAW_TASKBAR_WND:
            drawContext_.taskbar = *data == L'1';
            break;
        case EI_LABEL_TEXT_COLOR:
            drawContext_.labelColor = static_cast<COLORREF>(wcstoul(data, nullptr, 10));
            drawContext_.hasLabelColor = true;
            break;
        case EI_VALUE_TEXT_COLOR:
            drawContext_.valueColor = static_cast<COLORREF>(wcstoul(data, nullptr, 10));
            drawContext_.hasValueColor = true;
            break;
        case EI_TASKBAR_WND_VALUE_RIGHT_ALIGN:
            drawContext_.valueRightAlign = *data == L'1';
            break;
        default:
            break;
        }
    }
    catch (...)
    {
    }
}

void PeripheralBatteryPlugin::OnInitialize(ITrafficMonitor* pApp)
{
    // Arrives at load time from the UI thread (CPluginManager::LoadPlugins in InitInstance).
    try
    {
        // TrafficMonitor's language setting (0 = follow Windows); part of ITrafficMonitor since its first version.
        if (pApp != nullptr)
            lang::SetLanguage(pApp->GetLanguageId());
        if (notifier_.Initialize(pApp))
            manager_.SetNotifier([this](std::wstring message) { notifier_.Post(std::move(message)); });
    }
    catch (...)
    {
    }
}

ITMPlugin::OptionReturn PeripheralBatteryPlugin::ShowOptionsDialog(void* hParent)
{
    // UI thread (plugin manager or an item's context menu), modal.
    if (optionsOpen_)
    {
        // Already open (possibly hidden without an owner): bring it to the front instead of silently doing nothing.
        BringOptionsDialogToFront();
        return OR_OPTION_UNCHANGED;
    }
    optionsOpen_ = true;
    OptionReturn result = OR_OPTION_UNCHANGED;
    try
    {
        const std::wstring path = manager_.GetConfigPath();
        OptionsDialogContext context;
        context.config = LoadConfig(path);
        context.headphonesDetected = manager_.GetHeadphonesStatus().deviceName;
        context.mouseDetected = manager_.GetMouseStatus().deviceName;
        context.scanBluetoothNames = [] {
            std::vector<std::wstring> names;
            for (const BluetoothDeviceReport& device : ScanBluetoothDevices().devices)
                names.push_back(device.name);
            return names;
        };
        context.save = [path](const Config& config, std::wstring& error) {
            if (SaveConfig(path, config, error))
                return true;
            error = path + L"\n" + error;
            return false;
        };
        if (RunOptionsDialog(OwnerFor(hParent), context))
        {
            manager_.ApplyConfigChange(L"Settings changed");
            // From the taskbar menu, the host calls m_tBarDlg->WidthChanged() unchecked on
            // OR_OPTION_CHANGED. If the taskbar window was closed while the dialog (ownerless in that
            // case) was open, m_tBarDlg is null → TrafficMonitor crashes.
            // Then report UNCHANGED; the host recalculates the width every second anyway.
            const HWND parent = static_cast<HWND>(hParent);
            result = parent == nullptr || IsWindow(parent) ? OR_OPTION_CHANGED : OR_OPTION_UNCHANGED;
        }
    }
    catch (...)
    {
    }
    optionsOpen_ = false;
    return result;
}

const wchar_t* PeripheralBatteryPlugin::GetTooltipInfo()
{
    // UI thread, on every monitor update: only read the cache.
    try
    {
        tooltip_.clear();
        if (manager_.IsHeadphonesEnabled())
            tooltip_ += BatteryItem::FormatTooltipLine(lang::Tr(lang::Text::Headphones), manager_.GetHeadphonesStatus());
        if (manager_.IsMouseEnabled())
        {
            if (!tooltip_.empty())
                tooltip_ += L"\r\n";
            tooltip_ += BatteryItem::FormatTooltipLine(lang::Tr(lang::Text::Mouse), manager_.GetMouseStatus());
        }
        return tooltip_.c_str();
    }
    catch (...)
    {
        return L"";
    }
}

ITMPlugin* TMPluginGetInstance()
{
    return &PeripheralBatteryPlugin::Instance();
}
