#pragma once

#include "items/BatteryItem.h"

class MouseBatteryItem : public BatteryItem
{
public:
    using BatteryItem::BatteryItem;

    const wchar_t* GetItemName() const override;
    const wchar_t* GetItemId() const override;
    const wchar_t* GetItemLableText() const override;

protected:
    BatteryStatus ReadStatus() const override;
    std::wstring ReadLabel(const DisplayOptions& options) const override;

    // The mouse never draws the "⚡": while charging it shows the last known percentage instead.
    bool WantsChargingIndicator() const override { return false; }
};
