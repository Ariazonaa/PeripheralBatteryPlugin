#include "items/MouseBatteryItem.h"

#include "battery/BatteryManager.h"
#include "utils/Lang.h"

const wchar_t* MouseBatteryItem::GetItemName() const
{
    return lang::Tr(lang::Text::MouseItem);
}

const wchar_t* MouseBatteryItem::GetItemId() const
{
    return L"PeripheralBatteryMouse";
}

const wchar_t* MouseBatteryItem::GetItemLableText() const
{
    // Default label when TrafficMonitor draws (Colors=0); it stores changes itself.
    return lang::Tr(lang::Text::MouseLabel);
}

BatteryStatus MouseBatteryItem::ReadStatus() const
{
    return manager_.GetMouseStatus();
}

std::wstring MouseBatteryItem::ReadLabel(const DisplayOptions& options) const
{
    return options.mouseLabel;
}
