#include "hidpp/HidppDevice.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

namespace
{
    constexpr DWORD kRequestTimeoutMs = 1000;
    constexpr DWORD kWriteTimeoutMs = 500;
    constexpr uint8_t kPingEcho = 0x5A;

    uint8_t NextSoftwareId()
    {
        static std::atomic<unsigned> counter{ 0 };
        return hidpp::kSoftwareIds[counter.fetch_add(1) % std::size(hidpp::kSoftwareIds)];
    }

    std::wstring Hex(unsigned value, int digits)
    {
        wchar_t buffer[16];
        swprintf_s(buffer, L"0x%0*X", digits, value);
        return buffer;
    }

    using Clock = std::chrono::steady_clock;

    DWORD MillisecondsLeft(Clock::time_point deadline)
    {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        return left > 0 ? static_cast<DWORD>(left) : 0;
    }

    bool IsHidppReport(const uint8_t* buffer, int length)
    {
        return length >= 7 && (buffer[0] == hidpp::kShortReportId || buffer[0] == hidpp::kLongReportId);
    }
}

HidppDevice::HidppDevice(HidDevice& channel, uint8_t deviceIndex, HidppTrace trace)
    : channel_(channel)
    , deviceIndex_(deviceIndex)
    , trace_(std::move(trace))
{
}

std::map<uint8_t, HidppDevice::Version> HidppDevice::PingAll(HidDevice& channel, HidDevice* shortChannel,
    const std::vector<uint8_t>& indices, DWORD windowMs, const HidppTrace& trace)
{
    // A separate software ID per index, so that every response can be matched unambiguously.
    std::mutex expectedMutex;
    std::map<uint8_t, uint8_t> expected;   // Device index -> function byte
    for (uint8_t index : indices)
    {
        const uint8_t softwareId = NextSoftwareId();
        const hidpp::LongReport request = hidpp::BuildLongRequest(index, 0x00, 1, softwareId, { 0x00, 0x00, kPingEcho });
        if (trace)
            trace(true, request.data(), request.size());
        if (channel.Write(request.data(), request.size(), kWriteTimeoutMs))
            expected[index] = request[3];
    }

    // Capture output can come from both threads; a single line consists of several outputs.
    std::mutex traceMutex;
    auto traceReceived = [&](const uint8_t* data, size_t size) {
        if (!trace)
            return;
        std::lock_guard lock(traceMutex);
        trace(false, data, size);
    };

    // Listen in on the receiver's error responses on the short channel: [0x10, index, 0x8F, 0x00, fn, code].
    std::atomic<bool> done{ false };
    std::thread shortReader;
    // Stops and joins the helper thread on every path out of the function, even on an exception –
    // a still-joinable std::thread in the destructor would mean std::terminate (and TrafficMonitor would be gone).
    struct JoinOnExit
    {
        std::atomic<bool>& done;
        std::thread& thread;
        ~JoinOnExit()
        {
            done = true;
            if (thread.joinable())
                thread.join();
        }
    } joinOnExit{ done, shortReader };
    if (shortChannel != nullptr && shortChannel->IsOpen())
    {
        shortReader = std::thread([&] {
            while (!done)
            {
                // Read exactly one report length: with a larger buffer Windows returns several reports
                // in one go (captured), and only the first one would be evaluated.
                uint8_t buffer[hidpp::kShortReportLength] = {};
                const int n = shortChannel->Read(buffer, sizeof(buffer), 20);
                if (n < 0)
                    return;
                if (n < 7 || buffer[0] != hidpp::kShortReportId || buffer[2] != hidpp::kErrorHidpp10 || buffer[3] != 0x00)
                    continue;
                traceReceived(buffer, static_cast<size_t>(n));
                std::lock_guard lock(expectedMutex);
                const auto it = expected.find(buffer[1]);
                if (it != expected.end() && buffer[4] == it->second)
                    expected.erase(it);
            }
        });
    }

    std::map<uint8_t, Version> answered;
    const auto deadline = Clock::now() + std::chrono::milliseconds(windowMs);
    for (;;)
    {
        {
            std::lock_guard lock(expectedMutex);
            if (expected.empty())
                break;
        }
        const DWORD left = MillisecondsLeft(deadline);
        if (left == 0)
            break;
        uint8_t buffer[64] = {};
        // Short read intervals, so that entries completed by the helper thread count quickly.
        const int n = channel.Read(buffer, hidpp::kLongReportLength, (std::min)(left, static_cast<DWORD>(20)));
        if (n < 0)
            break;
        if (n == 0)
            continue;
        traceReceived(buffer, static_cast<size_t>(n));
        if (!IsHidppReport(buffer, n))
            continue;

        std::lock_guard lock(expectedMutex);
        const auto it = expected.find(buffer[1]);
        if (it == expected.end())
            continue;
        if (buffer[2] == 0x00 && buffer[3] == it->second && buffer[6] == kPingEcho)
        {
            answered[buffer[1]] = Version{ buffer[4], buffer[5] };
            expected.erase(it);
        }
        else if ((buffer[2] == hidpp::kErrorHidpp20 || buffer[2] == hidpp::kErrorHidpp10)
            && buffer[3] == 0x00 && buffer[4] == it->second)
        {
            expected.erase(it);     // Index in use, but not a HID++ 2.0 device or not reachable
        }
    }

    return answered;    // joinOnExit stops the helper thread
}

HidppDevice::Outcome HidppDevice::Request(uint8_t featureIndex, uint8_t function,
    std::initializer_list<uint8_t> params, hidpp::Params& reply)
{
    const hidpp::LongReport request =
        hidpp::BuildLongRequest(deviceIndex_, featureIndex, function, NextSoftwareId(), params);
    const uint8_t functionByte = request[3];

    if (trace_)
        trace_(true, request.data(), request.size());
    if (!channel_.Write(request.data(), request.size(), kWriteTimeoutMs))
    {
        lastError_ = L"write failed (" + std::to_wstring(GetLastError()) + L")";
        return Outcome::IoError;
    }

    const auto deadline = Clock::now() + std::chrono::milliseconds(kRequestTimeoutMs);
    for (;;)
    {
        const DWORD left = MillisecondsLeft(deadline);
        if (left == 0)
            break;

        uint8_t buffer[64] = {};
        const int n = channel_.Read(buffer, hidpp::kLongReportLength, left);
        if (n < 0)
        {
            lastError_ = L"read failed (" + std::to_wstring(GetLastError()) + L")";
            return Outcome::IoError;
        }
        if (n == 0)
            break;
        if (trace_)
            trace_(false, buffer, static_cast<size_t>(n));
        if (!IsHidppReport(buffer, n) || buffer[1] != deviceIndex_)
            continue;

        // Response to exactly this request (feature index + function + software ID).
        if (buffer[2] == featureIndex && buffer[3] == functionByte)
        {
            reply.fill(0);
            for (int i = 4; i < n && i - 4 < static_cast<int>(reply.size()); ++i)
                reply[static_cast<size_t>(i - 4)] = buffer[i];
            return Outcome::Ok;
        }

        // Error for exactly this request: [id, index, 0xFF|0x8F, featureIndex, functionByte, code].
        if ((buffer[2] == hidpp::kErrorHidpp20 || buffer[2] == hidpp::kErrorHidpp10)
            && buffer[3] == featureIndex && buffer[4] == functionByte)
        {
            lastError_ = std::wstring(buffer[2] == hidpp::kErrorHidpp20 ? L"HID++ 2.0" : L"HID++ 1.0")
                + L" error " + Hex(buffer[5], 2);
            return Outcome::DeviceError;
        }
        // Ignore everything else (events, late responses, traffic from other programs such as G Hub).
    }

    lastError_ = L"timeout";
    return Outcome::Timeout;
}

HidppDevice::Outcome HidppDevice::RequestWithRetry(uint8_t featureIndex, uint8_t function,
    std::initializer_list<uint8_t> params, hidpp::Params& reply)
{
    Outcome outcome = Request(featureIndex, function, params, reply);
    if (outcome == Outcome::Timeout)
        outcome = Request(featureIndex, function, params, reply);
    return outcome;
}

HidppDevice::Outcome HidppDevice::LookupFeature(uint16_t featureId, uint8_t& index)
{
    hidpp::Params reply{};
    const uint8_t hi = static_cast<uint8_t>(featureId >> 8);
    const uint8_t lo = static_cast<uint8_t>(featureId & 0xFF);
    const Outcome outcome = RequestWithRetry(0x00, 0, { hi, lo }, reply);
    index = outcome == Outcome::Ok ? reply[0] : 0;
    return outcome;
}

std::optional<uint8_t> HidppDevice::FindFeature(uint16_t featureId)
{
    uint8_t index = 0;
    if (LookupFeature(featureId, index) != Outcome::Ok)
        return std::nullopt;
    // Index 0 belongs to IRoot; for any other feature, 0 means "not present".
    if (index == 0 && featureId != hidpp::kFeatureRoot)
        return std::nullopt;
    return index;
}

std::optional<std::wstring> HidppDevice::ReadName()
{
    const auto index = FindFeature(hidpp::kFeatureDeviceName);
    if (!index)
        return std::nullopt;

    hidpp::Params reply{};
    if (RequestWithRetry(*index, 0, {}, reply) != Outcome::Ok)
        return std::nullopt;
    const size_t length = reply[0];

    std::string name;
    while (name.size() < length)
    {
        if (RequestWithRetry(*index, 1, { static_cast<uint8_t>(name.size()) }, reply) != Outcome::Ok)
            return std::nullopt;
        const size_t before = name.size();
        for (uint8_t c : reply)
        {
            if (name.size() >= length)
                break;
            name.push_back(static_cast<char>(c));
        }
        if (name.size() == before)
            break;
    }
    while (!name.empty() && name.back() == '\0')
        name.pop_back();

    // Device names are ASCII; non-ASCII is replaced rather than guessed.
    std::wstring result;
    for (char c : name)
        result.push_back(static_cast<unsigned char>(c) < 0x80 ? static_cast<wchar_t>(c) : L'?');
    return result;
}

std::optional<HidppBattery> HidppDevice::ReadBattery()
{
    // Order as per Plan §20. The functions/byte positions come from logitray (MIT), since the
    // battery features are not described in Logitech's cpg-docs:
    //   0x1004 getStatus (fn 1):             [state of charge %, level, charging status, ...]
    //   0x1000 GetBatteryLevelStatus (fn 0): [level %, next level %, status]
    //   0x1001 GetBatteryVoltage (fn 0):     [mV MSB, mV LSB, flags]
    const uint16_t features[] = {
        hidpp::kFeatureUnifiedBattery, hidpp::kFeatureBatteryStatus, hidpp::kFeatureBatteryVoltage
    };

    for (uint16_t feature : features)
    {
        uint8_t index = 0;
        if (LookupFeature(feature, index) != Outcome::Ok)
            return std::nullopt;        // Device does not respond; lastError_ is set
        if (index == 0)
            continue;                   // Feature not present
        return ReadBatteryAt(feature, index);
    }

    lastError_ = L"no battery feature (0x1004/0x1000/0x1001)";
    return std::nullopt;
}

std::optional<HidppBattery> HidppDevice::ReadBatteryAt(uint16_t feature, uint8_t featureIndex)
{
    const uint8_t function = feature == hidpp::kFeatureUnifiedBattery ? 1 : 0;
    hidpp::Params reply{};
    if (RequestWithRetry(featureIndex, function, {}, reply) != Outcome::Ok)
        return std::nullopt;

    HidppBattery battery;
    battery.feature = feature;
    battery.featureIndex = featureIndex;
    battery.rawStatus = reply[2];
    if (feature == hidpp::kFeatureBatteryVoltage)
    {
        battery.millivolts = (reply[0] << 8) | reply[1];
        battery.percentage = hidpp::MillivoltsToPercent(*battery.millivolts);
        battery.charge = hidpp::DecodeVoltageFlags(reply[2]);
    }
    else
    {
        battery.percentage = reply[0];
        battery.charge = hidpp::DecodeBatteryStatus(reply[2]);
    }
    return battery;
}
