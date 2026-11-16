# Contributing

Issues and pull requests are welcome – bug reports, fixes and support for more devices. This page is the long
version of the Contributing section in the [README](README.md).

## Bug reports

Describe what the taskbar shows (`—`, `?`, a wrong value, nothing at all) and what you expected, and attach the
output of

```
BatteryProbe.exe --diag
```

`BatteryProbe.exe` is part of every release. The report contains system, TrafficMonitor and plugin version, the
INI, the end of the log and the devices the plugin sees – but no addresses or device IDs; those only appear with
`--include-sensitive`, so don't add that flag to a public issue. For a mouse that misbehaves,
`BatteryProbe.exe --logitech --verbose` adds the HID++ trace.

## Building

Visual Studio 2022 or the Build Tools 2022 with the C++ workload (v143) and a Windows 10/11 SDK. From the
repository root:

```powershell
msbuild PeripheralBatteryPlugin.sln /p:Configuration=Release /p:Platform=x64 /m /warnaserror
```

Output: `bin\x64\Release\PeripheralBatteryPlugin.dll` and `bin\x64\Release\tools\BatteryProbe.exe`. To try the
DLL, exit TrafficMonitor, copy it directly into its `plugins` folder and start TrafficMonitor again.

## Before you open a pull request

- **It has to build without warnings.** CI builds Debug and Release on `windows-2022` with `/warnaserror`
  (warning level 4, C++20) and runs CodeQL. Each run uploads the Release binaries as an artifact, so others
  can test your change without building it.
- **Say how you tested it.** CI cannot reach real hardware. Name the device, the receiver or Bluetooth
  connection, the Windows and TrafficMonitor version, and what you checked – ideally with `BatteryProbe`
  output before and after.
- **Small and focused.** One fix or one device per pull request is the easiest to review.

## Rules that are not style preferences

**TrafficMonitor must never wait for the plugin.** `DataRequired()` runs on TrafficMonitor's monitor thread
and `DrawItem()` on its UI thread; if either blocks, all of TrafficMonitor stalls. Hardware is only touched by
the worker thread in `BatteryManager`, which writes into a cache; everything TrafficMonitor calls only reads that
cache. The thread rules are written down at the top of `src/battery/BatteryManager.h`.

**No exception leaves the plugin.** Every entry point TrafficMonitor calls catches everything and falls back to
a harmless value. An exception crossing into TrafficMonitor takes the whole program down.

**No guessed protocol commands or Windows properties.** New HID++ features, register writes or device
properties come with a source: documentation, an existing open-source implementation, or a capture of the
device's traffic. The plugin only reads; commands that change device state (settings, firmware, pairing) are not
accepted.

**No addresses or device IDs in logs or default output.** The plugin's log never contains them; `BatteryProbe`
shows them only with `--include-sensitive`.

**`external/` stays unchanged.** `external/TrafficMonitor/PluginInterface.h` is TrafficMonitor's header as
published (V1.86).

## Adding a device

A data source is an `IBatteryProvider` (`src/battery/IBatteryProvider.h`); the existing ones are
`WindowsBluetoothProvider` and `LogitechHidppProvider` in `src/providers/`. Providers are created in
`CreateProvider()` in `src/battery/BatteryManager.cpp` from the `Provider` key in the INI.

Many devices need no code at all: Bluetooth headphones whose battery level Windows shows under Settings →
Bluetooth & devices work through `WindowsBluetooth` with a matching `NameContains`. A pull request that only adds
"tested with" information for such a device is welcome too.

## Code and commits

Code, comments and documentation are in English. Commit messages follow
[Conventional Commits](https://www.conventionalcommits.org/) (`fix: …`, `feat: …`, `docs: …`). Describe what the
change actually does; if something is a workaround, say so and why.

## How pull requests land

The public repository is generated from the maintainer's development repository. A pull request is therefore
not merged with the merge button: the change is carried over and published with the next release, and the pull
request is closed with a link to it. You are credited for it.

## Working with an AI agent

That is fine. You stay responsible for what it produces: review it,
test it on real hardware, and say in the pull request what you checked.

## License

By contributing, you confirm that your contribution is your own work and that you may submit it under the
project's [MIT license](LICENSE).
