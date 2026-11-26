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

    constexpr uint8_t kWiredIndex = 0xFF;

    // G502 LIGHTSPEED: wired it is its own device (PID C08D, index 0xFF), wireless it sits in a slot of the
    // LIGHTSPEED receiver (PID C539). Known pair, in case the HID++ name differs between the two. C539 is
    // the generic LIGHTSPEED receiver other mice use as well, hence the name check on both sides.
    constexpr uint16_t kLightspeedReceiverPid = 0xC539;
    constexpr uint16_t kG502LightspeedWiredPid = 0xC08D;

    bool IsG502LightspeedIdentity(uint16_t productId, uint8_t deviceIndex, const std::wstring& name)
    {
        if (ToLower(name).find(L"g502") == std::wstring::npos)
            return false;
        if (deviceIndex == kWiredIndex)
            return productId == kG502LightspeedWiredPid;
        return productId == kLightspeedReceiverPid && deviceIndex != 0;
    }
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
    if (!needle.empty())
    {
        // Name filter: the first device whose name contains it. Wired and receiver identity of the same
        // mouse both match, so switching between cable and receiver needs no further handling here.
        for (const auto& device : scan.devices)
        {
            if (ToLower(device.name).find(needle) != std::wstring::npos)
            {
                match = &device;
                break;
            }
        }
    }
    else
    {
        // Without a name filter, only devices with a battery (e.g. no wired keyboard) – and keep the one
        // chosen once: while the mouse is asleep, no other Logitech device with a battery (e.g. a
        // wireless keyboard) should take its place. Applies until restart or until the configuration changes.
        // "The one" is the device, not its USB identity: a mouse that also runs on the cable shows up as
        // the wired device (index 0xFF) while plugged in and under its receiver's PID and slot otherwise.
        // Follow it across that switch and prefer the wired identity while it answers.
        const std::wstring lastName = ToLower(lastName_);
        const bool lastIsG502 = IsG502LightspeedIdentity(lastProductId_, lastSlot_, lastName_);
        auto isRemembered = [&](const LogitechDeviceReport& device)
        {
            if (device.productId == lastProductId_ && device.deviceIndex == lastSlot_)
                return true;
            // Only across the connection type (cable <-> receiver), never from one receiver slot to
            // another: two devices of the same model must not take each other's place.
            if ((device.deviceIndex == kWiredIndex) == (lastSlot_ == kWiredIndex))
                return false;
            if (!lastName.empty() && ToLower(device.name) == lastName)
                return true;
            return lastIsG502 && IsG502LightspeedIdentity(device.productId, device.deviceIndex, device.name);
        };

        const LogitechDeviceReport* sameIdentity = nullptr;
        const LogitechDeviceReport* otherIdentity = nullptr;
        for (const auto& device : scan.devices)
        {
            if (!(device.battery && device.battery->percentage))
                continue;
            if (lastSlot_ == 0)
            {
                match = &device;    // Nothing chosen yet: the first device with a battery
                break;
            }
            if (!isRemembered(device))
                continue;
            if (device.deviceIndex == kWiredIndex)
            {
                match = &device;
                break;
            }
            if (device.productId == lastProductId_ && device.deviceIndex == lastSlot_)
                sameIdentity = &device;
            else if (otherIdentity == nullptr)
                otherIdentity = &device;
        }
        if (match == nullptr)
            match = sameIdentity != nullptr ? sameIdentity : otherIdentity;
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

        // Charging estimates are connection-specific. A switch between USB and the receiver
        // must not carry the previous connection's charging timer into the new one.
        if (lastProductId_ != match->productId || lastSlot_ != match->deviceIndex)
            chargeStart_.reset();
        lastProductId_ = match->productId;
        lastSlot_ = match->deviceIndex;
        if (!match->name.empty())
            lastName_ = match->name;
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
        else if (battery && battery->charge == hidpp::ChargeState::Charging && battery->percentage)
        {
            // 0x1000/0x1004: the device reports a real charge level, no estimate needed. As above,
            // 100% is reserved for the device's own "full".
            status.available = true;
            status.state = BatteryState::Charging;
            status.percentage = std::min(*battery->percentage, 99);
            chargeStart_.reset();
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
