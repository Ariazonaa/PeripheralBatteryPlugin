#include "battery/BatteryManager.h"

#include <windows.h>
#include <objbase.h>

#include <chrono>
#include <exception>
#include <stdexcept>
#include <thread>

#include "Version.h"
#include "bluetooth/BluetoothWatcher.h"
#include "hidpp/HidppListener.h"
#include "providers/LogitechHidppProvider.h"
#include "providers/WindowsBluetoothProvider.h"
#include "utils/Lang.h"
#include "utils/Logger.h"

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace
{
    std::wstring Widen(const char* text)
    {
        // what() is ASCII/ANSI; a lossy conversion is good enough for the error message.
        std::wstring result;
        for (const char* p = text; p != nullptr && *p != '\0'; ++p)
            result.push_back(static_cast<unsigned char>(*p));
        return result;
    }

    BatteryStatus ErrorStatus(std::wstring message)
    {
        BatteryStatus status;
        status.state = BatteryState::Error;
        status.error = std::move(message);
        status.lastUpdate = std::chrono::system_clock::now();
        return status;
    }

    bool EqualsIgnoreCase(const std::wstring& a, const wchar_t* b)
    {
        return CompareStringOrdinal(a.c_str(), -1, b, -1, TRUE) == CSTR_EQUAL;
    }

    bool Uses(const DeviceConfig& config, const wchar_t* provider)
    {
        return config.enabled && EqualsIgnoreCase(config.provider, provider);
    }

    std::unique_ptr<IBatteryProvider> CreateProvider(const DeviceConfig& config, hidpp::LinkRegistry* registry)
    {
        if (EqualsIgnoreCase(config.provider, L"WindowsBluetooth"))
            return std::make_unique<WindowsBluetoothProvider>(config.nameContains);
        if (EqualsIgnoreCase(config.provider, L"LogitechHidpp"))
            return std::make_unique<LogitechHidppProvider>(config.nameContains, registry);
        return nullptr;
    }

    std::wstring Describe(const BatteryStatus& s)
    {
        const std::wstring percent = s.percentage >= 0 ? std::to_wstring(s.percentage) + L"%" : L"";
        switch (s.state)
        {
        case BatteryState::Disconnected: return L"disconnected";
        case BatteryState::Unknown: return L"unknown";
        case BatteryState::Error: return L"error";
        case BatteryState::Charging: return percent.empty() ? L"charging" : L"charging, " + percent;
        case BatteryState::Full: return L"full";
        default: return percent.empty() ? L"?" : percent;
        }
    }

    std::wstring DescribeConfig(const Config& c)
    {
        auto device = [](const DeviceConfig& d) {
            return d.provider + L" '" + d.nameContains + L"'" + (d.enabled ? L"" : L" (off)");
        };
        return L"interval " + std::to_wstring(c.updateIntervalSeconds) + L" s, headphones " + device(c.headphones)
            + L", mouse " + device(c.mouse) + L", ShowDisconnected=" + std::to_wstring(c.display.showDisconnected)
            + L" ShowPercentSign=" + std::to_wstring(c.display.showPercentSign)
            + L" ShowCharging=" + std::to_wstring(c.display.showCharging)
            + L" Colors=" + std::to_wstring(c.display.colors)
            + L" Label='" + c.display.headphonesLabel + L"'/'" + c.display.mouseLabel + L"'"
            + L" LowBatteryWarning=" + std::to_wstring(c.lowBatteryWarning)
            + L" VerboseLogging=" + std::to_wstring(c.verboseLogging);
    }

    std::wstring SiblingPath(const std::wstring& path, const wchar_t* fileName)
    {
        const size_t slash = path.find_last_of(L"\\/");
        return (slash == std::wstring::npos ? std::wstring() : path.substr(0, slash + 1)) + fileName;
    }

    // Without EI_CONFIG_DIR from the host: INI next to the DLL.
    std::wstring DefaultConfigPath()
    {
        wchar_t path[MAX_PATH] = {};
        const DWORD length = GetModuleFileNameW(reinterpret_cast<HMODULE>(&__ImageBase), path, MAX_PATH);
        std::wstring result(path, length);
        const size_t slash = result.find_last_of(L"\\/");
        return (slash == std::wstring::npos ? std::wstring() : result.substr(0, slash + 1)) + L"PeripheralBattery.ini";
    }
}

BatteryManager::BatteryManager() = default;

// Deliberately does not wait for the worker: see StartWorker(). This destructor only runs at process
// exit (DLL_PROCESS_DETACH, loader lock): event sources with WinRT and Windows registrations are
// therefore intentionally not torn down anymore.
BatteryManager::~BatteryManager()
{
    (void)hidppListener_.release();
    (void)bluetoothWatcher_.release();
}

void BatteryManager::RequestRefresh(std::wstring reason, std::chrono::milliseconds delay)
{
    try
    {
        logging::Verbose(L"Immediate query requested: " + reason);
        {
            std::lock_guard lock(scheduleMutex_);
            scheduled_.insert(std::chrono::steady_clock::now() + delay);
            pendingReason_ = std::move(reason);
        }
        wakeup_.notify_one();
    }
    catch (...)
    {
    }
}

void BatteryManager::WaitForNextRefresh(int intervalSeconds)
{
    using Clock = std::chrono::steady_clock;
    const auto nextPoll = Clock::now() + std::chrono::seconds(intervalSeconds);
    std::unique_lock lock(scheduleMutex_);
    for (;;)
    {
        auto next = nextPoll;
        if (!scheduled_.empty() && *scheduled_.begin() < next)
            next = *scheduled_.begin();
        if (Clock::now() >= next)
            break;
        // Wakes up when the time is up or RequestRefresh schedules something earlier.
        wakeup_.wait_until(lock, next);
    }
    // This one query takes care of everything that was due by now.
    scheduled_.erase(scheduled_.begin(), scheduled_.upper_bound(Clock::now()));
    if (!pendingReason_.empty())
    {
        logging::Verbose(L"Query due to: " + pendingReason_);
        pendingReason_.clear();
    }
}

void BatteryManager::StartEventSources(const Config& config)
{
    // Only start what the configuration needs; once started, they run until process exit.
    const bool needsHidpp = Uses(config.headphones, L"LogitechHidpp") || Uses(config.mouse, L"LogitechHidpp");
    const bool needsBluetooth = Uses(config.headphones, L"WindowsBluetooth") || Uses(config.mouse, L"WindowsBluetooth");

    if (needsHidpp && !hidppListener_)
    {
        hidppListener_ = std::make_unique<HidppListener>(linkRegistry_,
            [this](std::wstring reason) { RequestRefresh(std::move(reason)); });
        logging::Info(L"Instant updates: HID++ listener started");
    }
    if (hidppListener_)
        hidppListener_->Refresh();      // newly plugged-in receivers

    if (needsBluetooth && !bluetoothWatcher_)
    {
        bluetoothWatcher_ = std::make_unique<BluetoothWatcher>([this](std::wstring reason) {
            RequestRefresh(reason);
            // Windows may update the HFP battery value a little later after connecting.
            RequestRefresh(reason + L" (follow-up check)", std::chrono::seconds(15));
        });
        bluetoothWatcherOk_ = bluetoothWatcher_->Start();
        logging::Info(std::wstring(L"Instant updates: Bluetooth watcher ")
            + (bluetoothWatcherOk_ ? L"started" : L"could not start"));
    }
    else if (needsBluetooth)
    {
        // Aborted or never started: retry every cycle; only the change is logged.
        if (const std::optional<bool> restarted = bluetoothWatcher_->EnsureRunning())
        {
            if (*restarted || bluetoothWatcherOk_)
                logging::Info(std::wstring(L"Instant updates: Bluetooth watcher ")
                    + (*restarted ? L"restarted" : L"was no longer running and could not restart"));
            bluetoothWatcherOk_ = *restarted;
        }
    }
}

void BatteryManager::SetConfigPath(std::wstring path)
{
    // Read the display settings right away (GetPrivateProfile* only, fast): TrafficMonitor queries
    // IsCustomDraw()/width while building the taskbar window, before the worker runs.
    const Config config = LoadConfig(path);
    std::lock_guard lock(mutex_);
    configPath_ = std::move(path);
    displayOptions_ = config.display;
    lowBatteryWarning_ = config.lowBatteryWarning;
}

std::wstring BatteryManager::GetConfigPath() const
{
    std::lock_guard lock(mutex_);
    return configPath_.empty() ? DefaultConfigPath() : configPath_;
}

void BatteryManager::ApplyConfigChange(std::wstring reason)
{
    const Config config = LoadConfig(GetConfigPath());
    {
        std::lock_guard lock(mutex_);
        displayOptions_ = config.display;
        lowBatteryWarning_ = config.lowBatteryWarning;
    }
    RequestRefresh(std::move(reason), std::chrono::milliseconds(0));
}

void BatteryManager::SetNotifier(std::function<void(std::wstring)> notify)
{
    std::lock_guard lock(mutex_);
    notify_ = std::move(notify);
}

bool BatteryManager::IsHeadphonesEnabled() const
{
    std::lock_guard lock(mutex_);
    return headphonesEnabled_;
}

bool BatteryManager::IsMouseEnabled() const
{
    std::lock_guard lock(mutex_);
    return mouseEnabled_;
}

void BatteryManager::CheckLowBattery(Device& device, const BatteryStatus& shown, const BatteryStatus& raw,
    int threshold)
{
    // Only warn once the previous measurement also had a value: right after connecting, Windows
    // still reports the Bluetooth device's value from the last session until HFP is set up
    // (the follow-up check after 15 s confirms or corrects it).
    const bool confirmed = device.lastRawAvailable;
    device.lastRawAvailable = raw.available;

    // Only for measured values while discharging; charging/full re-arms the warning.
    constexpr int kRearmMargin = 5;
    if (shown.state == BatteryState::Charging || shown.state == BatteryState::Full)
    {
        device.lowWarned = false;
        return;
    }
    if (threshold <= 0 || !shown.available || shown.state != BatteryState::Discharging || shown.percentage < 0)
        return;

    if (shown.percentage > threshold + kRearmMargin)
    {
        device.lowWarned = false;
        return;
    }
    if (shown.percentage > threshold || device.lowWarned || !confirmed)
        return;

    device.lowWarned = true;
    const std::wstring name = shown.deviceName.empty() ? std::wstring(device.label) : shown.deviceName;
    const std::wstring message = lang::Tr(lang::Text::LowBattery) + name + L" – " + std::to_wstring(shown.percentage) + L"%";
    logging::Info(std::wstring(device.label) + L": warning shown – " + message);

    std::function<void(std::wstring)> notify;
    {
        std::lock_guard lock(mutex_);
        notify = notify_;
    }
    if (notify)
        notify(message);
}

void BatteryManager::Update()
{
    std::call_once(workerStarted_, [this] { StartWorker(); });
}

void BatteryManager::StartWorker()
{
    // On shutdown, TrafficMonitor calls FreeLibrary on the plugins in ~CPluginManager while the
    // process is still running. A running worker would then either keep executing unloaded code
    // or would have to be joined under the loader lock (deadlock). That is why the worker holds
    // its own reference to the DLL: it stays loaded until process exit, and Windows terminates
    // the thread together with the process.
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCWSTR>(&__ImageBase), &self))
    {
        throw std::runtime_error("GetModuleHandleExW failed");
    }

    try
    {
        std::thread(&BatteryManager::WorkerLoop, this).detach();
    }
    catch (...)
    {
        FreeLibrary(self);
        throw;
    }
}

void BatteryManager::WorkerLoop() noexcept
{
    // The Bluetooth query uses WinRT and needs an MTA thread.
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    (void)hr;

    bool started = false;
    for (;;)
    {
        // A constant instead of Config{}: that would allocate strings, and a bad_alloc here (outside
        // the try) would leave the noexcept function → std::terminate.
        int interval = Config::kDefaultInterval;
        try
        {
            const Config config = ReadConfig();
            interval = config.updateIntervalSeconds;
            {
                // Display right away, not only after the (possibly slow) query.
                std::lock_guard lock(mutex_);
                displayOptions_ = config.display;
                lowBatteryWarning_ = config.lowBatteryWarning;
            }
            if (!started)
            {
                logging::Info(std::wstring(L"PeripheralBatteryPlugin ") + kPluginVersion + L" started");
                started = true;
            }
            if (!lastConfig_ || !(*lastConfig_ == config))
            {
                logging::Info(L"Configuration: " + DescribeConfig(config));
                lastConfig_ = config;
            }

            Configure(headphones_, config.headphones);
            Configure(mouse_, config.mouse);
            StartEventSources(config);

            // "—" only after at least two intervals without a response (StatusFilter).
            const std::chrono::seconds minOutage(2 * config.updateIntervalSeconds);
            BatteryStatus rawHeadphones, rawMouse;
            BatteryStatus headphones = Refresh(headphones_, rawHeadphones, minOutage);
            {
                // Show right away: the mouse query takes 2 s or more when the mouse is asleep or off,
                // and must not hold up an instant update of the headphones.
                std::lock_guard lock(mutex_);
                headphonesStatus_ = headphones;
            }
            BatteryStatus mouse = Refresh(mouse_, rawMouse, minOutage);
            LogResult(headphones_, rawHeadphones, headphones);
            LogResult(mouse_, rawMouse, mouse);
            CheckChargeEnd(headphones_, rawHeadphones);
            CheckChargeEnd(mouse_, rawMouse);
            CheckLowBattery(headphones_, headphones, rawHeadphones, config.lowBatteryWarning);
            CheckLowBattery(mouse_, mouse, rawMouse, config.lowBatteryWarning);

            std::lock_guard lock(mutex_);
            headphonesStatus_ = std::move(headphones);
            mouseStatus_ = std::move(mouse);
            headphonesEnabled_ = config.headphones.enabled;
            mouseEnabled_ = config.mouse.enabled;
        }
        catch (const std::exception& e)
        {
            // The worker must never die from an exception (std::terminate takes TrafficMonitor down with it).
            try { logging::Error(L"Worker: " + Widen(e.what())); } catch (...) {}
        }
        catch (...)
        {
            try { logging::Error(L"Worker: unknown exception"); } catch (...) {}
        }
        try
        {
            WaitForNextRefresh(interval);
        }
        catch (...)
        {
            std::this_thread::sleep_for(std::chrono::seconds(interval));
        }
    }
}

Config BatteryManager::ReadConfig()
{
    std::wstring path;
    {
        std::lock_guard lock(mutex_);
        if (configPath_.empty())
            configPath_ = DefaultConfigPath();
        path = configPath_;
    }
    const bool created = WriteDefaultConfigIfMissing(path);
    const int added = created ? 0 : AddMissingKeys(path);
    Config config = LoadConfig(path);

    logging::Configure(SiblingPath(path, L"PeripheralBatteryPlugin.log"), config.verboseLogging);
    if (created)
        logging::Info(L"Default configuration created: " + path);
    if (added > 0)
        logging::Info(std::to_wstring(added) + L" new setting(s) added to " + path);
    return config;
}

void BatteryManager::LogResult(Device& device, const BatteryStatus& raw, const BatteryStatus& shown)
{
    std::wstring name = raw.deviceName.empty() ? device.config.nameContains : raw.deviceName;
    if (name.empty())
        name = L"(no device found)";
    const std::wstring context = name + (raw.detail.empty() ? L"" : L" (" + raw.detail + L")");

    if (logging::IsVerbose())
    {
        std::wstring line = std::wstring(device.label) + L": " + Describe(raw) + L" – " + context;
        if (!raw.error.empty())
            line += L" – " + raw.error;
        if (Describe(shown) != Describe(raw))
            line += L" → shown: " + Describe(shown) + L" (dropout filter)";
        logging::Verbose(line);
    }

    // Standard log: only state changes.
    std::wstring condition;
    if (raw.available)
        condition = L"ok";
    else if (raw.state == BatteryState::Disconnected)
        condition = L"disconnected:" + raw.error;
    else
        condition = L"error:" + raw.error;
    if (condition == device.lastCondition)
        return;

    const bool wasProblem = !device.lastCondition.empty() && device.lastCondition != L"ok";
    if (raw.available)
        logging::Info(std::wstring(device.label) + L": " + (wasProblem ? L"available again, " : L"") + Describe(raw)
            + L" – " + context);
    else if (raw.state == BatteryState::Disconnected)
        logging::Info(std::wstring(device.label) + L": disconnected – " + context + (raw.error.empty() ? L"" : L" – " + raw.error));
    else
        logging::Error(std::wstring(device.label) + L": " + (raw.error.empty() ? Describe(raw) : raw.error) + L" – "
            + context + L" – shown: " + Describe(shown));
    device.lastCondition = condition;
}

void BatteryManager::Configure(Device& device, const DeviceConfig& config)
{
    if (device.configured && device.config == config)
        return;

    // New device or new settings: fresh provider, the old filter state no longer applies.
    device.config = config;
    device.configured = true;
    device.filter.Reset();
    device.provider.reset();
    if (!config.enabled)
        return;

    device.provider = CreateProvider(config, &linkRegistry_);
    if (device.provider)
    {
        try
        {
            device.provider->Initialize();
        }
        catch (...)
        {
            // Query() reports the error on Refresh.
        }
    }
}

BatteryStatus BatteryManager::Refresh(Device& device, BatteryStatus& raw, std::chrono::seconds minOutage)
{
    if (!device.config.enabled)
    {
        raw = BatteryStatus{};
        raw.state = BatteryState::Disconnected;
        raw.error = L"disabled in PeripheralBattery.ini";
        return raw;
    }
    raw = device.provider ? Query(device.provider.get())
                          : ErrorStatus(L"unknown provider '" + device.config.provider + L"'");
    return device.filter.Apply(raw, minOutage);
}

void BatteryManager::CheckChargeEnd(Device& device, const BatteryStatus& raw)
{
    // After unplugging, the voltage is still above the resting voltage (captured: 4203 mV →
    // 100%, one minute later 94%). For voltage-only devices, query again soon instead of
    // leaving the too-high value in place until the next interval.
    const bool wasOnPower = device.lastRawState == BatteryState::Charging || device.lastRawState == BatteryState::Full;
    if (wasOnPower && raw.state == BatteryState::Discharging && raw.millivolts > 0)
    {
        RequestRefresh(std::wstring(device.label) + L": follow-up check after end of charging", std::chrono::seconds(20));
        RequestRefresh(std::wstring(device.label) + L": follow-up check after end of charging", std::chrono::seconds(40));
    }
    if (raw.state != BatteryState::Unknown && raw.state != BatteryState::Error)
        device.lastRawState = raw.state;
}

BatteryStatus BatteryManager::GetHeadphonesStatus() const
{
    std::lock_guard lock(mutex_);
    return headphonesStatus_;
}

BatteryStatus BatteryManager::GetMouseStatus() const
{
    std::lock_guard lock(mutex_);
    return mouseStatus_;
}

DisplayOptions BatteryManager::GetDisplayOptions() const
{
    std::lock_guard lock(mutex_);
    return displayOptions_;
}

int BatteryManager::GetLowBatteryWarning() const
{
    std::lock_guard lock(mutex_);
    return lowBatteryWarning_;
}

BatteryStatus BatteryManager::Query(IBatteryProvider* provider)
{
    try
    {
        provider->Refresh();
        return provider->GetStatus();
    }
    catch (const std::exception& e)
    {
        return ErrorStatus(provider->GetProviderName() + L": " + Widen(e.what()));
    }
    catch (...)
    {
        return ErrorStatus(provider->GetProviderName() + L": unknown error");
    }
}
