#pragma once

#include <windows.h>

#include <string>

#include <PluginInterface.h>

#include "battery/BatteryStatus.h"
#include "config/Config.h"

class BatteryManager;

// What TrafficMonitor passes via ITMPlugin::OnExtenedInfo before every DrawItem(). Owned by the plugin,
// UI thread only (the host calls OnExtenedInfo and DrawItem there directly one after the other).
struct DrawContext
{
    bool taskbar = true;            // EI_DRAW_TASKBAR_WND: taskbar (1) or main window (0)
    bool hasLabelColor = false;     // EI_LABEL_TEXT_COLOR is only sent for the taskbar
    bool hasValueColor = false;
    COLORREF labelColor = 0;
    COLORREF valueColor = 0;
    bool valueRightAlign = false;   // EI_TASKBAR_WND_VALUE_RIGHT_ALIGN (taskbar only)
};

// Shared display logic of the battery items. Reads only the BatteryManager cache, never hardware.
class BatteryItem : public IPluginItem
{
public:
    BatteryItem(const BatteryManager& manager, const DrawContext& drawContext);

    const wchar_t* GetItemValueText() const override;
    const wchar_t* GetItemValueSampleText() const override;

    // Colors=1: the item draws label and value itself (otherwise TrafficMonitor does).
    bool IsCustomDraw() const override;
    int GetItemWidth() const override;
    int GetItemWidthEx(void* hDC) const override;
    void DrawItem(void* hDC, int x, int y, int w, int h, bool dark_mode) override;

    static std::wstring FormatValue(const BatteryStatus& status, const DisplayOptions& options);

    // One tooltip line, e.g. "Mouse: G502 LIGHTSPEED … – 93% (4106 mV), as of 19:16".
    static std::wstring FormatTooltipLine(const wchar_t* label, const BatteryStatus& status);

    enum class Tone { Normal, Low, Charging };
    // Low: discharging and <= lowThreshold (0 = off). Charging: charging, or full on the cable.
    static Tone ToneFor(const BatteryStatus& status, int lowThreshold);
    static COLORREF ToneColor(Tone tone, COLORREF normal, bool darkMode);

    // Width of label + gap + value in the font currently selected into the DC.
    static int MeasureWidth(HDC dc, const std::wstring& label, const std::wstring& value);
    // Label on the left, value after it (or right-aligned); DrawTextW only, because TrafficMonitor
    // redirects exactly this function in D2D mode.
    static void DrawLabelValue(HDC dc, const RECT& rect, const std::wstring& label, COLORREF labelColor,
        const std::wstring& value, COLORREF valueColor, bool valueRightAlign);

protected:
    virtual BatteryStatus ReadStatus() const = 0;
    virtual std::wstring ReadLabel(const DisplayOptions& options) const = 0;

    // Whether this item may draw the "⚡" while charging. The mouse overrides this to false: it only
    // measures its voltage, which under charging current gives no real level, so it shows the last
    // known percentage instead of the indicator. Default: follow the global ShowCharging option.
    virtual bool WantsChargingIndicator() const { return true; }

    // The global display options with the item-specific charging indicator applied.
    DisplayOptions EffectiveDisplayOptions() const;

    const BatteryManager& manager_;

private:
    const DrawContext& drawContext_;

    // Used only on the UI thread (sole caller of GetItemValueText), hence no lock.
    // The returned pointer stays valid until the next call.
    mutable std::wstring valueText_;
};
