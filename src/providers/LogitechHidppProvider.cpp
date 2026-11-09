#include "providers/LogitechHidppProvider.h"

#include <algorithm>
#include <cwctype>

#include "hidpp/LogitechScanner.h"

namespace
{
    std::wstring ToLower(std::wstring text)
    {
        std::transform(text.begin(), text.end(), text.begin(),
            [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        return text;
    }

    // While charging, the measured voltage sits above the resting voltage (charge current plus the
    // cell's overpotential), so mapping it straight through the discharge curve pins the level near
    // 100%. Subtract this offset before the mapping to get a usable, rising estimate instead.
    // One-point calibration on a G502 LIGHTSPEED: 4204 mV while charging matched 90% in G Hub, and
    // 90% sits at 4067 mV on the resting curve -> 137 mV. Approximate; the real charge gauge is
    // internal to the device and not exposed over HID++.
    constexpr int kChargingVoltageOffsetMv = 137;

    // Near full the voltage stops rising while the battery keeps filling (CV phase), so the voltage
    // estimate would stick (e.g. around 90%). Assume a full charge (0 -> 100%) takes about this long
    // and let the shown value creep up accordingly, scaled to how empty it was when charging began.
    constexpr double kEstimatedFullChargeMinutes = 150.0;
}

LogitechHidppProvider::LogitechHidppProvider(std::wstring nameContains, hidpp::LinkRegistry* registry)
    : nameContains_(std::move(nameContains))
    , registry_(registry)
{
    status_.deviceName = nameContains_;
}

bool LogitechHidppProvider::Initialize()
{
    return true;
}

BatteryStatus LogitechHidppProvider::GetStatus()
{
    return status_;
}

void LogitechHidppProvider::Refresh()
{
    BatteryStatus status;
    status.deviceName = nameContains_;
    status.lastUpdate = std::chrono::system_clock::now();

    const LogitechScanResult scan = scanner_.Scan();

    const std::wstring needle = ToLower(nameContains_);
    const LogitechDeviceReport* match = nullptr;
    for (const auto& device : scan.devices)
    {
        const bool nameMatches = needle.empty() || ToLower(device.name).find(needle) != std::wstring::npos;
        if (!nameMatches)
            continue;
        // Without a name filter, only devices with a battery (e.g. no wired keyboard) – and keep the one
        // chosen once: while the mouse is asleep, no other Logitech device with a battery (e.g. a
        // wireless keyboard) should take its place. Applies until restart or until the configuration changes.
        if (needle.empty() && !(device.battery && device.battery->percentage))
            continue;
        if (needle.empty() && lastSlot_ != 0
            && (device.productId != lastProductId_ || device.deviceIndex != lastSlot_))
        {
            continue;
        }
        match = &device;
        break;
    }

    if (match == nullptr)
    {
        if (scan.channels == 0)
        {
            // No receiver/wired device present: confirmed disconnected.
            status.state = BatteryState::Disconnected;
            status.error = L"no HID++ channel found";
        }
        else if (registry_ && lastSlot_ != 0 && registry_->GetLink(lastProductId_, lastSlot_) == false)
        {
            // The receiver itself reported the device as disconnected (0x41): confirmed disconnected.
            status.state = BatteryState::Disconnected;
            status.error = L"receiver reports device disconnected";
        }
        else
        {
            // Receiver present, device does not respond (off, out of range or just too slow):
            // not confirmed, hence Error – the StatusFilter decides after several cycles.
            // Distinguish for the log: no ping answered, or reading failed afterwards.
            status.state = BatteryState::Error;
            status.error = L"no matching device answered";
            for (const auto& device : scan.devices)
            {
                wchar_t slot[48];
                swprintf_s(slot, L"; slot %u answered ping", device.deviceIndex);
                status.error += slot;
                if (!device.name.empty())
                    status.error += L" (" + device.name + L")";
                if (!device.error.empty())
                    status.error += L", read failed: " + device.error;
            }
            if (scan.devices.empty())
                status.error += L"; no device answered ping";
        }
    }
    else
    {
        status.deviceName = match->name;
        wchar_t id[32];
        swprintf_s(id, L"046D:%04X/%u", match->productId, match->deviceIndex);
        status.deviceId = id;

        lastProductId_ = match->productId;
        lastSlot_ = match->deviceIndex;
        if (registry_ && match->battery)
            registry_->SetBatteryIndex(match->productId, match->deviceIndex, match->battery->featureIndex);

        const auto& battery = match->battery;
        wchar_t detail[160];
        swprintf_s(detail, L"PID %04X %s %u, HID++ %d.%d%s", match->productId,
            match->deviceIndex == 0xFF ? L"wired" : L"slot", match->deviceIndex == 0xFF ? 0u : match->deviceIndex,
            match->protocolMajor, match->protocolMinor, match->fromCache ? L", cache" : L"");
        status.detail = detail;
        if (battery)
        {
            status.millivolts = battery->millivolts.value_or(-1);
            wchar_t more[96];
            if (battery->millivolts)
                swprintf_s(more, L", Feature 0x%04X, %d mV, Flags 0x%02X", battery->feature, *battery->millivolts,
                    battery->rawStatus);
            else
                swprintf_s(more, L", Feature 0x%04X, Status 0x%02X", battery->feature, battery->rawStatus);
            status.detail += more;
        }

        if (battery && battery->charge == hidpp::ChargeState::Charging && battery->millivolts)
        {
            // No real charge level while charging: the voltage is inflated under charge current and,
            // near full, barely rises while the battery keeps filling. Start from the charge-
            // compensated voltage estimate and, while the session runs, creep the shown value up
            // towards full over the estimated charge time, so it does not stick (e.g. at ~90%).
            // 100% is reserved for the device's own "full". Approximate by design; the true gauge is
            // internal to the device (e.g. G Hub counts the charge current), not exposed over HID++.
            const int voltageEstimate = hidpp::MillivoltsToPercent(*battery->millivolts - kChargingVoltageOffsetMv);
            const auto now = std::chrono::steady_clock::now();
            if (!chargeStart_)
            {
                chargeStart_ = now;
                chargeStartPercent_ = voltageEstimate;
            }
            int shown = voltageEstimate;
            if (chargeStartPercent_ < 100)
            {
                const double fullMinutes = kEstimatedFullChargeMinutes * (100 - chargeStartPercent_) / 100.0;
                const double elapsedMinutes =
                    std::chrono::duration<double, std::ratio<60>>(now - *chargeStart_).count();
                const double fraction = fullMinutes > 0.0 ? std::min(1.0, elapsedMinutes / fullMinutes) : 1.0;
                const int ramp = chargeStartPercent_
                    + static_cast<int>((100 - chargeStartPercent_) * fraction + 0.5);
                shown = std::max(shown, ramp);
            }
            status.available = true;
            status.state = BatteryState::Charging;
            status.percentage = std::min(shown, 99);
        }
        else if (battery && battery->charge == hidpp::ChargeState::Full)
        {
            status.available = true;
            status.percentage = 100;
            status.state = BatteryState::Full;
            chargeStart_.reset();
        }
        else if (battery && battery->percentage)
        {
            status.available = true;
            status.percentage = *battery->percentage;
            status.state = BatteryState::Discharging;
            chargeStart_.reset();
        }
        else
        {
            status.state = BatteryState::Unknown;
            status.error = match->error.empty() ? L"no battery value" : match->error;
            chargeStart_.reset();
        }
    }
    status_ = std::move(status);
}

std::wstring LogitechHidppProvider::GetProviderName() const
{
    return L"LogitechHidpp";
}
