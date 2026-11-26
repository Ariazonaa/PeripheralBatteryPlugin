#pragma once

#include <array>
#include <cstdint>
#include <initializer_list>
#include <iterator>

// HID++ 2.0 framing.
//
// Sources:
// - Logitech cpg-docs, hidpp20/README.rst: report 0x10 (7 bytes) / 0x11 (20 bytes); byte 1 device index
//   (0xFF = wired device/receiver); byte 2 feature index; byte 3 function + software ID; then
//   parameters. hidpp20/features/0x0000-IRoot.rst: GetFeature (fn 0), GetProtocolVersion (fn 1).
// - logitray (MIT, verified on a LIGHTSPEED receiver): byte 3 = (function << 4) | softwareId,
//   software ID 0 marks events; errors at byte 2 = 0x8F (HID++ 1.0) or 0xFF (HID++ 2.0);
//   on Windows the long report channel is on usage page 0xFF00, usage 0x0002.
namespace hidpp
{
    constexpr uint8_t kShortReportId = 0x10;
    constexpr uint8_t kLongReportId = 0x11;
    constexpr size_t kShortReportLength = 7;
    constexpr size_t kLongReportLength = 20;
    constexpr size_t kLongParamCount = kLongReportLength - 4;

    constexpr uint16_t kUsagePage = 0xFF00;
    constexpr uint16_t kShortUsage = 0x0001;
    constexpr uint16_t kLongUsage = 0x0002;

    // HID++ 1.0 notification from the receiver on the short channel: device connected/disconnected.
    // [0x10, slot, 0x41, info, flags, WPID LSB, WPID MSB]; flags bit 6 set = disconnected.
    // Source: logitray (MIT, after Solaar); captured on the G502 (off: 0x62, on: 0xA2).
    constexpr uint8_t kConnectionNotification = 0x41;
    constexpr uint8_t kLinkDownFlag = 0x40;

    constexpr uint8_t kCordedIndex = 0xFF;
    constexpr uint8_t kFirstReceiverSlot = 1;
    constexpr uint8_t kLastReceiverSlot = 6;

    constexpr uint8_t kErrorHidpp10 = 0x8F;
    constexpr uint8_t kErrorHidpp20 = 0xFF;

    // Software ID: 4 bits, 0 marks events. Responses carry the ID of the request; every request
    // gets its own (rotating), so that a late response to an earlier request never passes as the
    // response to the current one.
    // Responses go to all programs with an open channel. G Hub changes its ID (captured
    // 2026-09-18: 0x8, later 0x5), so collisions cannot be ruled out by the choice of ID.
    // Pings are additionally safeguarded by the echo; otherwise only low traffic helps.
    constexpr uint8_t kSoftwareIds[] = { 0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0x7, 0x8, 0x9, 0xA, 0xB, 0xC, 0xD, 0xE, 0xF };

    constexpr uint16_t kFeatureRoot = 0x0000;
    constexpr uint16_t kFeatureDeviceName = 0x0005;
    constexpr uint16_t kFeatureBatteryStatus = 0x1000;
    constexpr uint16_t kFeatureBatteryVoltage = 0x1001;
    constexpr uint16_t kFeatureUnifiedBattery = 0x1004;

    using LongReport = std::array<uint8_t, kLongReportLength>;
    using Params = std::array<uint8_t, kLongParamCount>;

    inline uint8_t FunctionByte(uint8_t function, uint8_t softwareId)
    {
        return static_cast<uint8_t>((function << 4) | (softwareId & 0x0F));
    }

    // Feature 0x1001 only returns the voltage. Estimate via a typical Li-ion discharge curve:
    // the 13 data points (mV, %) come from Solaar (lib/logitech_receiver/hidpp20.py,
    // estimate_battery_level_percentage); only the measured values were taken over, the linear
    // interpolation is our own implementation.
    inline int MillivoltsToPercent(int millivolts)
    {
        struct Point { int mv; int percent; };
        static constexpr Point kCurve[] = {
            { 4186, 100 }, { 4067, 90 }, { 3989, 80 }, { 3922, 70 }, { 3859, 60 },
            { 3811, 50 }, { 3778, 40 }, { 3751, 30 }, { 3717, 20 }, { 3671, 10 },
            { 3646, 5 }, { 3579, 2 }, { 3500, 0 },
        };
        if (millivolts >= kCurve[0].mv)
            return 100;
        for (size_t i = 1; i < std::size(kCurve); ++i)
        {
            const Point& hi = kCurve[i - 1];
            const Point& lo = kCurve[i];
            if (millivolts >= lo.mv)
            {
                const double t = static_cast<double>(millivolts - lo.mv) / (hi.mv - lo.mv);
                return static_cast<int>(lo.percent + t * (hi.percent - lo.percent) + 0.5);
            }
        }
        return 0;
    }

    enum class ChargeState { Unknown, Discharging, Charging, Full };

    // 0x1001 flags. Bit 7 = external power (Solaar and logitray agree). Bits 0-2 are only
    // interpreted by Solaar: 0 = charging, 1 = full (2 = not charging, 7 = error) – other values stay Unknown.
    inline ChargeState DecodeVoltageFlags(uint8_t flags)
    {
        if ((flags & 0x80) == 0)
            return ChargeState::Discharging;
        switch (flags & 0x07)
        {
        case 0: return ChargeState::Charging;
        case 1: return ChargeState::Full;
        default: return ChargeState::Unknown;
        }
    }

    // 0x1000/0x1004 status byte (byte 2 of the reply). Values from Solaar, lib/logitech_receiver/common.py,
    // class BatteryStatus: 0 discharging, 1 recharging, 2 almost full, 3 full, 4 slow recharge,
    // 5 invalid battery, 6 thermal error. "Almost full" and "slow recharge" are still charging; the error
    // states stay Unknown. Checked on a G502 X LIGHTSPEED (0x1004): 0x00 on battery.
    inline ChargeState DecodeBatteryStatus(uint8_t status)
    {
        switch (status)
        {
        case 0: return ChargeState::Discharging;
        case 1:
        case 2:
        case 4: return ChargeState::Charging;
        case 3: return ChargeState::Full;
        default: return ChargeState::Unknown;
        }
    }

    inline LongReport BuildLongRequest(uint8_t deviceIndex, uint8_t featureIndex, uint8_t function,
        uint8_t softwareId, std::initializer_list<uint8_t> params)
    {
        LongReport report{};
        report[0] = kLongReportId;
        report[1] = deviceIndex;
        report[2] = featureIndex;
        report[3] = FunctionByte(function, softwareId);
        size_t i = 4;
        for (uint8_t p : params)
        {
            if (i >= report.size())
                break;
            report[i++] = p;
        }
        return report;
    }
}
