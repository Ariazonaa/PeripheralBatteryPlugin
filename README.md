# PeripheralBatteryPlugin

[![Build](https://github.com/Ariazonaa/PeripheralBatteryPlugin/actions/workflows/build.yml/badge.svg)](https://github.com/Ariazonaa/PeripheralBatteryPlugin/actions/workflows/build.yml)
[![CodeQL](https://github.com/Ariazonaa/PeripheralBatteryPlugin/actions/workflows/codeql.yml/badge.svg)](https://github.com/Ariazonaa/PeripheralBatteryPlugin/actions/workflows/codeql.yml)

Plugin for [TrafficMonitor](https://github.com/zhongyang219/TrafficMonitor) (Windows, x64) that shows the battery
level of peripheral devices as separate items in the taskbar:

| Item | Source | Tested with |
|---|---|---|
| **Headphones Battery** | Bluetooth battery level as known to Windows – no vendor app needed | Sennheiser MOMENTUM 4 |
| **Mouse Battery** | Logitech mouse queried directly via HID++ through the LIGHTSPEED / Unifying / Bolt receiver – no G Hub needed | Logitech G502 LIGHTSPEED |

```
M4 100%   Mouse 92%
```

## Display

| Value | Meaning |
|---|---|
| `82%` | battery level (while the mouse charges: an estimated level, see below) |
| `⚡` | charging (devices that report a charge state; the mouse never shows it, see below) |
| `⚡100%` | full and on the cable |
| `—` | not connected, switched off or asleep |
| `?` | no value yet, or the device does not respond |

The value turns **red** when the battery is at or below the warning threshold (default 15%) and **green** while
charging. Hovering over the TrafficMonitor display shows a tooltip with the full device name, the value and the
time of the reading.

For mice that only report their voltage (e.g. G502 LIGHTSPEED) the percentage is an estimate. Under charging
current the voltage reads higher than at rest, so while charging the mouse shows no `⚡` but an estimated level
(green as a charging hint): starting from the charge-compensated voltage (offset 137 mV, calibrated on the
G502), the shown value creeps up towards full over the estimated charge time, so it does not stick near full
(where the voltage barely rises while the battery keeps filling). `100%` appears only once the mouse itself
reports "full". It is an approximation and can differ from the device's own gauge (e.g. G Hub). Windows reports
the MOMENTUM 4's battery level in 10% steps.

## Installation

Requirements: Windows 10/11 (64-bit), TrafficMonitor 1.82 or later, 64-bit version (tested with 1.86).

1. Download the zip from [Releases](https://github.com/Ariazonaa/PeripheralBatteryPlugin/releases).
2. Exit TrafficMonitor (right-click → Exit).
3. Copy `PeripheralBatteryPlugin.dll` **directly** into the `plugins` folder next to `TrafficMonitor.exe`,
   e.g. `C:\Program Files\TrafficMonitor\plugins\` (no subfolder – TrafficMonitor only loads DLLs directly inside
   `plugins`; under `C:\Program Files` this needs administrator rights).
4. Start TrafficMonitor.
5. Right-click the TrafficMonitor display → **Display Settings** → tick "Headphones Battery" and "Mouse Battery"
   (at the very bottom of the list).

The plugin manager (right-click → "Plugin-in Manage…") shows whether the plugin was loaded.

## Settings

Plugin manager → "Peripheral Battery" → **Options…** (or right-click one of the items → **Plugin Options**).
Changes take effect immediately.

The settings are stored in `PeripheralBattery.ini` in the plugin folder. The plugin creates the file on first
start; it explains every setting and can also be edited directly:

| Section | Key | Default | Meaning |
|---|---|---|---|
| `[General]` | `UpdateInterval` | `60` | poll interval in seconds (15–3600) |
| | `ShowDisconnected` | `1` | `—` for a disconnected device (`0` = empty) |
| | `ShowPercentSign` | `1` | `82%` (`0` = `82`) |
| | `ShowCharging` | `1` | `⚡` while charging (the mouse never shows it) |
| | `LowBatteryWarning` | `15` | notification and red value at or below this level (`0` = off) |
| | `Colors` | `1` | red/green value; `0` = TrafficMonitor draws the item without colors |
| | `VerboseLogging` | `0` | `1` = write every poll cycle to the log |
| `[Headphones]`, `[Mouse]` | `Enabled` | `1` | query the device |
| | `Provider` | `WindowsBluetooth` / `LogitechHidpp` | data source |
| | `NameContains` | `MOMENTUM 4` / empty | part of the device name. Empty: the first Logitech device with a battery, or the first connected Bluetooth device with a battery level |
| | `Label` | `M4` / `Mouse` | label in front of the value |

With `Colors=1` the plugin draws the item itself. TrafficMonitor does not pass its own label from the display
settings to self-drawing plugins, so `Label` applies. With `Colors=0` the label from TrafficMonitor's display
settings applies.

For other Bluetooth headphones, enter part of their name under "Device" / `NameContains` – provided Windows
knows their battery level (Settings → Bluetooth & devices).

## Behavior

- Queries run in the background, every 60 seconds by default; TrafficMonitor is never blocked.
- Instant updates: mouse on/off, charging cable, receiver plugged/unplugged and Bluetooth connection changes
  show up after about 1–3 seconds.
- Short dropouts keep showing the last value; `—` appears only after three failures spanning at least two
  intervals. If Windows or the Logitech receiver reports a device as disconnected, that applies immediately –
  also when a mouse falls asleep after a few minutes of inactivity.
- When a battery drops to the warning threshold, TrafficMonitor shows a notification once; again only after the
  battery has risen 5 percentage points above it or has been charged.
- Log: `PeripheralBatteryPlugin.log` next to the INI (start, errors and state changes).

## Troubleshooting

`BatteryProbe.exe` (included in the release) shows what the plugin sees, without TrafficMonitor:

```
BatteryProbe.exe --diag                 report: system, TrafficMonitor, plugin version, INI, log, devices
BatteryProbe.exe                        all devices
BatteryProbe.exe --logitech --verbose   with HID++ trace
BatteryProbe.exe --json                 machine-readable
```

Addresses and device IDs are only shown with `--include-sensitive`.

## Building

Visual Studio 2022 or the Build Tools 2022 with the C++ workload (v143) and a Windows 10/11 SDK:

```powershell
& "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe" `
    PeripheralBatteryPlugin.sln /p:Configuration=Release /p:Platform=x64 /m
```

Output: `bin\x64\Release\PeripheralBatteryPlugin.dll` and `bin\x64\Release\tools\BatteryProbe.exe`.

## Contributing

Issues and pull requests are welcome – bug reports, fixes, support for more devices, translations.

- **Bug reports:** please describe what the taskbar shows and attach the output of `BatteryProbe.exe --diag`.
  It contains no addresses or device IDs (those only appear with `--include-sensitive`).
- **Pull requests:** every pull request is built automatically (Debug and Release, warnings are errors) and
  analyzed with CodeQL.
- **New devices:** please don't guess protocol commands or Windows properties – new HID++ features or device
  properties should come with a source (documentation or a capture of the device's traffic).

## License and sources

MIT, see [LICENSE](LICENSE).

- `external/TrafficMonitor/PluginInterface.h` is taken unchanged from
  [TrafficMonitor](https://github.com/zhongyang219/TrafficMonitor) (V1.86) and is covered by its license
  (`external/TrafficMonitor/LICENSE`).
- HID++ protocol details based on Logitech's [cpg-docs](https://github.com/Logitech/cpg-docs) and
  [logitray](https://github.com/Ithilias/logitray) (MIT); the data points of the voltage curve come from
  [Solaar](https://github.com/pwr-Solaar/Solaar).
- The battery level of Bluetooth devices comes from an undocumented device property set by Windows (also used
  by e.g. [WmiPnp](https://github.com/nikvoronin/WmiPnp)).
