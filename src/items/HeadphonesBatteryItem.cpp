#include "items/HeadphonesBatteryItem.h"

#include "battery/BatteryManager.h"
#include "utils/Lang.h"

const wchar_t* HeadphonesBatteryItem::GetItemName() const
{
    return lang::Tr(lang::Text::HeadphonesItem);
}

const wchar_t* HeadphonesBatteryItem::GetItemId() const
{
    return L"PeripheralBatteryHeadphones";
}

const wchar_t* HeadphonesBatteryItem::GetItemLableText() const
{
    // Default label when TrafficMonitor draws (Colors=0); it stores changes itself.
    return L"M4 ";
}

BatteryStatus HeadphonesBatteryItem::ReadStatus() const
{
    return manager_.GetHeadphonesStatus();
}

std::wstring HeadphonesBatteryItem::ReadLabel(const DisplayOptions& options) const
{
    return options.headphonesLabel;
}
