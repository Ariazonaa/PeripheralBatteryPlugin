#include "ui/OptionsDialog.h"

#include <objbase.h>

#include <algorithm>
#include <map>
#include <memory>

#include "resource.h"
#include "utils/Lang.h"

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace
{
    constexpr UINT kScanDone = WM_APP + 10;     // lParam: std::vector<std::wstring>*, then owned by the window

    const wchar_t* Title()
    {
        return lang::Tr(lang::Text::PluginName);
    }

    struct ProviderChoice
    {
        const wchar_t* id;      // value in the INI
        lang::Text text;        // display text
    };
    constexpr ProviderChoice kProviders[] = {
        { L"WindowsBluetooth", lang::Text::ProviderBluetooth },
        { L"LogitechHidpp", lang::Text::ProviderLogitech },
    };

    struct DeviceControls
    {
        int enabled;
        int provider;
        int name;
        int label;
    };
    constexpr DeviceControls kHeadphones{ IDC_HP_ENABLED, IDC_HP_PROVIDER, IDC_HP_NAME, IDC_HP_LABEL };
    constexpr DeviceControls kMouse{ IDC_MS_ENABLED, IDC_MS_PROVIDER, IDC_MS_NAME, IDC_MS_LABEL };

    struct DialogState
    {
        const OptionsDialogContext* context = nullptr;
        std::vector<std::wstring> bluetoothNames;
        // Source from the INI that matches none of the known ones (empty too): stays selectable as its
        // own entry and is written back unchanged. Key: ID of the source list.
        std::map<int, std::wstring> otherProvider;
    };

    HWND g_openDialog = nullptr;    // UI thread only

    // Provider names are case-insensitive, as in the BatteryManager.
    bool SameId(const std::wstring& a, const wchar_t* b)
    {
        return CompareStringOrdinal(a.c_str(), -1, b, -1, TRUE) == CSTR_EQUAL;
    }

    struct ScanJob
    {
        std::function<std::vector<std::wstring>()> scan;
        HWND dialog = nullptr;
        HMODULE module = nullptr;   // own reference so the DLL stays loaded during the scan
    };

    std::wstring Trim(std::wstring text)
    {
        const auto notSpace = [](wchar_t c) { return c != L' ' && c != L'\t'; };
        text.erase(text.begin(), std::find_if(text.begin(), text.end(), notSpace));
        text.erase(std::find_if(text.rbegin(), text.rend(), notSpace).base(), text.end());
        return text;
    }

    std::wstring GetText(HWND dialog, int id)
    {
        const HWND control = GetDlgItem(dialog, id);
        const int length = GetWindowTextLengthW(control);
        std::wstring text(static_cast<size_t>(length) + 1, L'\0');
        text.resize(static_cast<size_t>(GetWindowTextW(control, text.data(), length + 1)));
        return text;
    }

    void Check(HWND dialog, int id, bool value)
    {
        CheckDlgButton(dialog, id, value ? BST_CHECKED : BST_UNCHECKED);
    }

    bool IsChecked(HWND dialog, int id)
    {
        return IsDlgButtonChecked(dialog, id) == BST_CHECKED;
    }

    void FillProviders(HWND dialog, DialogState& state, const DeviceControls& controls, const std::wstring& current)
    {
        const HWND combo = GetDlgItem(dialog, controls.provider);
        LRESULT selected = -1;
        for (const ProviderChoice& choice : kProviders)
        {
            const LRESULT index = SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(lang::Tr(choice.text)));
            if (SameId(current, choice.id))
                selected = index;
        }
        // An unknown or empty value from the INI is kept instead of being silently replaced.
        if (selected < 0)
        {
            state.otherProvider[controls.provider] = current;
            const std::wstring text = current.empty() ? std::wstring(lang::Tr(lang::Text::ProviderNone)) : current;
            selected = SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
        }
        SendMessageW(combo, CB_SETCURSEL, selected, 0);
    }

    std::wstring SelectedProvider(HWND dialog, const DialogState& state, const DeviceControls& controls)
    {
        const LRESULT index = SendDlgItemMessageW(dialog, controls.provider, CB_GETCURSEL, 0, 0);
        if (index >= 0 && index < static_cast<LRESULT>(std::size(kProviders)))
            return kProviders[index].id;
        const auto other = state.otherProvider.find(controls.provider);
        return other != state.otherProvider.end() ? other->second : std::wstring(kProviders[0].id);
    }

    // Suggestions in the "Device" field: paired Bluetooth devices (source Windows Bluetooth) and the
    // last detected device if the source is still the same. The entered text is kept.
    void FillSuggestions(HWND dialog, const DialogState& state, const DeviceControls& controls,
        const DeviceConfig& original, const std::wstring& detected)
    {
        const HWND combo = GetDlgItem(dialog, controls.name);
        const std::wstring text = GetText(dialog, controls.name);
        const std::wstring provider = SelectedProvider(dialog, state, controls);

        std::vector<std::wstring> items;
        if (!detected.empty() && SameId(provider, original.provider.c_str()))
            items.push_back(detected);
        if (SameId(provider, L"WindowsBluetooth"))
            items.insert(items.end(), state.bluetoothNames.begin(), state.bluetoothNames.end());

        SendMessageW(combo, CB_RESETCONTENT, 0, 0);
        std::vector<std::wstring> added;
        for (const std::wstring& item : items)
        {
            if (item.empty() || std::find(added.begin(), added.end(), item) != added.end())
                continue;
            SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.c_str()));
            added.push_back(item);
        }
        SetWindowTextW(combo, text.c_str());
    }

    void UpdateEnabling(HWND dialog)
    {
        const bool colors = IsChecked(dialog, IDC_COLORS);
        for (const DeviceControls* controls : { &kHeadphones, &kMouse })
        {
            const bool enabled = IsChecked(dialog, controls->enabled);
            EnableWindow(GetDlgItem(dialog, controls->provider), enabled);
            EnableWindow(GetDlgItem(dialog, controls->name), enabled);
            // Only the plugin itself draws the label (Colored); otherwise TrafficMonitor's label applies.
            EnableWindow(GetDlgItem(dialog, controls->label), enabled && colors);
        }
    }

    DWORD WINAPI ScanThread(void* parameter)
    {
        HMODULE module = nullptr;
        {
            std::unique_ptr<ScanJob> job(static_cast<ScanJob*>(parameter));
            module = job->module;
            try
            {
                const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                auto names = std::make_unique<std::vector<std::wstring>>(job->scan());
                if (SUCCEEDED(hr))
                    CoUninitialize();
                // If the window is already closed, PostMessage fails and the list is freed here.
                if (PostMessageW(job->dialog, kScanDone, 0, reinterpret_cast<LPARAM>(names.get())))
                    names.release();
            }
            catch (...)
            {
            }
        }
        FreeLibraryAndExitThread(module, 0);
    }

    void StartScan(HWND dialog, const OptionsDialogContext& context)
    {
        if (!context.scanBluetoothNames)
            return;
        auto job = std::make_unique<ScanJob>();
        job->scan = context.scanBluetoothNames;
        job->dialog = dialog;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&ScanThread),
                &job->module))
        {
            return;
        }
        const HMODULE module = job->module;
        const HANDLE thread = CreateThread(nullptr, 0, ScanThread, job.get(), 0, nullptr);
        if (thread == nullptr)
        {
            FreeLibrary(module);
            return;
        }
        job.release();
        CloseHandle(thread);
    }

    void InitDialog(HWND dialog, DialogState& state)
    {
        const Config& config = state.context->config;
        SetDlgItemInt(dialog, IDC_INTERVAL, static_cast<UINT>(config.updateIntervalSeconds), FALSE);
        SetDlgItemInt(dialog, IDC_LOW_WARNING, static_cast<UINT>(config.lowBatteryWarning), FALSE);
        SendDlgItemMessageW(dialog, IDC_INTERVAL, EM_LIMITTEXT, 4, 0);
        SendDlgItemMessageW(dialog, IDC_LOW_WARNING, EM_LIMITTEXT, 3, 0);
        Check(dialog, IDC_COLORS, config.display.colors);
        Check(dialog, IDC_SHOW_PERCENT, config.display.showPercentSign);
        Check(dialog, IDC_SHOW_DISCONNECTED, config.display.showDisconnected);
        Check(dialog, IDC_SHOW_CHARGING, config.display.showCharging);
        Check(dialog, IDC_VERBOSE, config.verboseLogging);

        const struct
        {
            const DeviceControls& controls;
            const DeviceConfig& device;
            const std::wstring& label;
            const std::wstring& detected;
        } devices[] = {
            { kHeadphones, config.headphones, config.display.headphonesLabel, state.context->headphonesDetected },
            { kMouse, config.mouse, config.display.mouseLabel, state.context->mouseDetected },
        };
        for (const auto& d : devices)
        {
            Check(dialog, d.controls.enabled, d.device.enabled);
            FillProviders(dialog, state, d.controls, d.device.provider);
            SendDlgItemMessageW(dialog, d.controls.name, CB_LIMITTEXT, 100, 0);
            SetDlgItemTextW(dialog, d.controls.name, d.device.nameContains.c_str());
            FillSuggestions(dialog, state, d.controls, d.device, d.detected);
            SendDlgItemMessageW(dialog, d.controls.label, EM_LIMITTEXT, 32, 0);
            SetDlgItemTextW(dialog, d.controls.label, d.label.c_str());
        }
        UpdateEnabling(dialog);
        StartScan(dialog, *state.context);
    }

    bool ReadNumber(HWND dialog, int id, int minimum, int maximum, const wchar_t* message, int& value)
    {
        BOOL ok = FALSE;
        const UINT number = GetDlgItemInt(dialog, id, &ok, FALSE);
        if (ok && static_cast<int>(number) >= minimum && static_cast<int>(number) <= maximum)
        {
            value = static_cast<int>(number);
            return true;
        }
        MessageBoxW(dialog, message, Title(), MB_ICONWARNING | MB_OK);
        const HWND control = GetDlgItem(dialog, id);
        SetFocus(control);
        SendMessageW(control, EM_SETSEL, 0, -1);
        return false;
    }

    // true = close the window. `changed` = saved and different from before.
    bool Apply(HWND dialog, const DialogState& state, bool& changed)
    {
        Config result = state.context->config;
        if (!ReadNumber(dialog, IDC_INTERVAL, Config::kMinInterval, Config::kMaxInterval,
                lang::Tr(lang::Text::IntervalRange), result.updateIntervalSeconds)
            || !ReadNumber(dialog, IDC_LOW_WARNING, 0, 100,
                lang::Tr(lang::Text::WarningRange), result.lowBatteryWarning))
        {
            return false;
        }
        result.display.colors = IsChecked(dialog, IDC_COLORS);
        result.display.showPercentSign = IsChecked(dialog, IDC_SHOW_PERCENT);
        result.display.showDisconnected = IsChecked(dialog, IDC_SHOW_DISCONNECTED);
        result.display.showCharging = IsChecked(dialog, IDC_SHOW_CHARGING);
        result.verboseLogging = IsChecked(dialog, IDC_VERBOSE);

        auto readDevice = [dialog, &state](const DeviceControls& controls, DeviceConfig& device, std::wstring& label) {
            device.enabled = IsChecked(dialog, controls.enabled);
            // Replace only if a different source is really selected ("windowsbluetooth" stays as it is).
            const std::wstring provider = SelectedProvider(dialog, state, controls);
            if (!SameId(device.provider, provider.c_str()))
                device.provider = provider;
            device.nameContains = Trim(GetText(dialog, controls.name));
            label = Trim(GetText(dialog, controls.label));
        };
        readDevice(kHeadphones, result.headphones, result.display.headphonesLabel);
        readDevice(kMouse, result.mouse, result.display.mouseLabel);

        if (result == state.context->config)
        {
            changed = false;
            return true;
        }
        std::wstring error;
        if (!state.context->save || !state.context->save(result, error))
        {
            MessageBoxW(dialog, (lang::Tr(lang::Text::SaveFailed) + (L"\n\n" + error)).c_str(), Title(),
                MB_ICONERROR | MB_OK);
            return false;
        }
        changed = true;
        return true;
    }

    INT_PTR CALLBACK DialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
    {
        // No exception may escape into TrafficMonitor's message loop.
        try
        {
            auto* state = reinterpret_cast<DialogState*>(GetWindowLongPtrW(dialog, DWLP_USER));
            switch (message)
            {
            case WM_INITDIALOG:
                state = reinterpret_cast<DialogState*>(lParam);
                SetWindowLongPtrW(dialog, DWLP_USER, lParam);
                g_openDialog = dialog;
                InitDialog(dialog, *state);
                SetForegroundWindow(dialog);
                return TRUE;

            case kScanDone:
            {
                std::unique_ptr<std::vector<std::wstring>> names(reinterpret_cast<std::vector<std::wstring>*>(lParam));
                if (state != nullptr && names)
                {
                    state->bluetoothNames = std::move(*names);
                    FillSuggestions(dialog, *state, kHeadphones, state->context->config.headphones,
                        state->context->headphonesDetected);
                    FillSuggestions(dialog, *state, kMouse, state->context->config.mouse, state->context->mouseDetected);
                }
                return TRUE;
            }

            case WM_COMMAND:
                if (state == nullptr)
                    break;
                switch (LOWORD(wParam))
                {
                case IDC_COLORS:
                case IDC_HP_ENABLED:
                case IDC_MS_ENABLED:
                    if (HIWORD(wParam) == BN_CLICKED)
                        UpdateEnabling(dialog);
                    return TRUE;
                case IDC_HP_PROVIDER:
                    if (HIWORD(wParam) == CBN_SELCHANGE)
                        FillSuggestions(dialog, *state, kHeadphones, state->context->config.headphones,
                            state->context->headphonesDetected);
                    return TRUE;
                case IDC_MS_PROVIDER:
                    if (HIWORD(wParam) == CBN_SELCHANGE)
                        FillSuggestions(dialog, *state, kMouse, state->context->config.mouse, state->context->mouseDetected);
                    return TRUE;
                case IDOK:
                {
                    bool changed = false;
                    if (Apply(dialog, *state, changed))
                        EndDialog(dialog, changed ? IDOK : IDCANCEL);
                    return TRUE;
                }
                case IDCANCEL:
                    EndDialog(dialog, IDCANCEL);
                    return TRUE;
                default:
                    break;
                }
                break;

            case WM_DESTROY:
            {
                if (g_openDialog == dialog)
                    g_openDialog = nullptr;
                // Result of a scan that is still in the queue after closing.
                MSG pending;
                while (PeekMessageW(&pending, dialog, kScanDone, kScanDone, PM_REMOVE))
                    delete reinterpret_cast<std::vector<std::wstring>*>(pending.lParam);
                break;
            }

            default:
                break;
            }
        }
        catch (...)
        {
        }
        return FALSE;
    }
}

bool BringOptionsDialogToFront()
{
    if (g_openDialog == nullptr || !IsWindow(g_openDialog))
        return false;
    if (IsIconic(g_openDialog))
        ShowWindow(g_openDialog, SW_RESTORE);
    SetForegroundWindow(g_openDialog);
    return true;
}

bool RunOptionsDialog(HWND owner, const OptionsDialogContext& context)
{
    DialogState state;
    state.context = &context;
    const HINSTANCE module = reinterpret_cast<HINSTANCE>(&__ImageBase);
    // Template in TrafficMonitor's language. DialogBoxParamW would pick it by the Windows UI language,
    // which need not match TrafficMonitor's setting.
    HRSRC resource = FindResourceExW(module, RT_DIALOG, MAKEINTRESOURCEW(IDD_OPTIONS), lang::DialogLanguage());
    if (resource == nullptr)
        resource = FindResourceExW(module, RT_DIALOG, MAKEINTRESOURCEW(IDD_OPTIONS), MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US));
    const HGLOBAL loaded = resource != nullptr ? LoadResource(module, resource) : nullptr;
    const auto* dialogTemplate = loaded != nullptr ? static_cast<const DLGTEMPLATE*>(LockResource(loaded)) : nullptr;
    const INT_PTR result = dialogTemplate != nullptr
        ? DialogBoxIndirectParamW(module, dialogTemplate, owner, DialogProc, reinterpret_cast<LPARAM>(&state))
        : DialogBoxParamW(module, MAKEINTRESOURCEW(IDD_OPTIONS), owner, DialogProc, reinterpret_cast<LPARAM>(&state));
    return result == IDOK;
}
