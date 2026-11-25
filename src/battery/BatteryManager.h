#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>

#include "battery/BatteryStatus.h"
#include "battery/IBatteryProvider.h"
#include "battery/StatusFilter.h"
#include "config/Config.h"
#include "hidpp/LinkRegistry.h"

class BluetoothWatcher;
class HidppListener;

// Holds the last known battery level per device.
//
// Threads:
// - SetConfigPath() comes from the UI thread at load time (before the first Update()).
// - Update() comes via ITMPlugin::DataRequired() from TrafficMonitor's monitor thread.
//   It starts the worker on the first call and returns immediately – if the monitor thread
//   blocks, all of TrafficMonitor's measurement stalls.
// - The worker (MTA) is the only thread that touches configuration, providers and filters.
//   Each cycle it reads the INI, queries the providers outside the lock and writes the
//   result into the cache under mutex_.
// - Get*() comes from the UI thread and only reads the cache.
class BatteryManager
{
public:
    BatteryManager();
    ~BatteryManager();

    BatteryManager(const BatteryManager&) = delete;
    BatteryManager& operator=(const BatteryManager&) = delete;

    void SetConfigPath(std::wstring path);
    std::wstring GetConfigPath() const;

    // After saving in the options dialog (UI thread): apply the display settings right away and
    // trigger a query that reads the rest of the configuration.
    void ApplyConfigChange(std::wstring reason);

    // Recipient of low-battery warnings; called from the worker and must switch to the UI thread
    // itself (UiNotifier). Set before the first Update().
    void SetNotifier(std::function<void(std::wstring)> notify);

    void Update();

    // Thread-safe (also from thread pool callbacks): schedules a query in `delay`. Several
    // requests in quick succession (e.g. duplicate events) result in a single query.
    void RequestRefresh(std::wstring reason, std::chrono::milliseconds delay = std::chrono::milliseconds(700));

    BatteryStatus GetHeadphonesStatus() const;
    BatteryStatus GetMouseStatus() const;
    DisplayOptions GetDisplayOptions() const;
    int GetLowBatteryWarning() const;
    bool IsHeadphonesEnabled() const;
    bool IsMouseEnabled() const;

private:
    struct Device
    {
        const wchar_t* label;       // for the log
        DeviceConfig config;
        bool configured = false;
        std::unique_ptr<IBatteryProvider> provider;
        StatusFilter filter;
        std::wstring lastCondition; // last logged state, so that nothing repeats every minute
        bool lowWarned = false;     // warning already shown for the current low
        bool lastRawAvailable = false;  // previous measurement had a value (warn only on a confirmed value)
        BatteryState lastRawState = BatteryState::Unknown;  // for detecting "end of charging"
    };

    void StartWorker();
    void WorkerLoop() noexcept;
    void WaitForNextRefresh(int intervalSeconds);
    void StartEventSources(const Config& config);
    Config ReadConfig();
    void Configure(Device& device, const DeviceConfig& config);
    // Returns the filtered status; raw receives the provider's unfiltered result.
    static BatteryStatus Refresh(Device& device, BatteryStatus& raw, std::chrono::seconds minOutage);
    void CheckChargeEnd(Device& device, const BatteryStatus& raw);
    static BatteryStatus Query(IBatteryProvider* provider);
    static void LogResult(Device& device, const BatteryStatus& raw, const BatteryStatus& shown);
    void CheckLowBattery(Device& device, const BatteryStatus& shown, const BatteryStatus& raw, int threshold,
        const wchar_t* fallbackName);

    std::once_flag workerStarted_;

    // Worker only.
    Device headphones_{ L"Headphones" };
    Device mouse_{ L"Mouse" };
    std::optional<Config> lastConfig_;
    std::unique_ptr<HidppListener> hidppListener_;
    std::unique_ptr<BluetoothWatcher> bluetoothWatcher_;
    bool bluetoothWatcherOk_ = false;

    // Event sources (HidppListener) and Logitech providers share the connection state.
    hidpp::LinkRegistry linkRegistry_;

    // Scheduled queries (RequestRefresh), separate lock so that UI reads don't wait.
    std::mutex scheduleMutex_;
    std::condition_variable wakeup_;
    std::multiset<std::chrono::steady_clock::time_point> scheduled_;
    std::wstring pendingReason_;

    mutable std::mutex mutex_;
    std::wstring configPath_;
    std::function<void(std::wstring)> notify_;
    BatteryStatus headphonesStatus_;
    BatteryStatus mouseStatus_;
    DisplayOptions displayOptions_;
    int lowBatteryWarning_ = Config{}.lowBatteryWarning;
    bool headphonesEnabled_ = true;
    bool mouseEnabled_ = true;
};
