#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

#include "battery/IBatteryProvider.h"
#include "hidpp/LinkRegistry.h"
#include "hidpp/LogitechScanner.h"

// Battery level of a Logitech HID++ device (receiver or wired), selected by the device name read
// via HID++ – not by VID/PID (Plan §17). Empty name part = first device with a battery.
// Refresh() blocks and may only be called from the BatteryManager's worker thread.
//
// With LinkRegistry: if the receiver reports the last found device as disconnected (notification
// 0x41), "does not respond" counts as confirmed disconnected instead of as a dropout; in addition,
// the battery feature index is registered so that the HidppListener recognizes battery events.
class LogitechHidppProvider : public IBatteryProvider
{
public:
    LogitechHidppProvider(std::wstring nameContains, hidpp::LinkRegistry* registry = nullptr);

    bool Initialize() override;
    BatteryStatus GetStatus() override;
    void Refresh() override;
    std::wstring GetProviderName() const override;

private:
    std::wstring nameContains_;
    hidpp::LinkRegistry* registry_;
    LogitechScanner scanner_{ true };
    BatteryStatus status_;
    uint16_t lastProductId_ = 0;    // Where the device was last found
    uint8_t lastSlot_ = 0;

    // Charging estimate (voltage gives no real level near full): while a charging session runs, the
    // shown value creeps from the voltage estimate at its start up towards full over an estimated
    // charge time. Reset whenever a non-charging reading arrives.
    std::optional<std::chrono::steady_clock::time_point> chargeStart_;
    int chargeStartPercent_ = 0;
};
