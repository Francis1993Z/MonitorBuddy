# MonitorBuddy

Open-source Windows utility to seamlessly switch between TV and multi-monitor desktop configurations. Built with the modern CCD API and a sleek Avalonia UI with system tray support.

## Features

- **Instant mode switching** — Toggle between TV mode (single display) and Desktop mode (extended dual monitors) in one click
- **Modern CCD API** — Uses `QueryDisplayConfig` / `SetDisplayConfig` instead of legacy `ChangeDisplaySettingsExW`, eliminating coordinate conflicts
- **Auto-detection** — Identifies monitors by EDID name and hardware ID via CCD target device info
- **Configurable layout** — Choose left/right order and primary monitor from the UI, saved in `layout.json`
- **System tray** — Runs in the background, switch modes from the tray menu without opening the window
- **Diagnostic panel** — See detected monitors, their roles, and CCD path states in real-time

## Architecture

- **NativeDLL/** — C++ engine (`GestionEcrans.dll`) using CCD API for topology read/modify/apply
- **MonitorSwitchUI/** — Avalonia UI (C#) with dark theme, system tray, and layout config panel

## Configuration

Edit keywords in `NativeDLL/GestionEcrans.cpp` to match your hardware:

```cpp
static const wchar_t* KEYWORDS_BUREAU[] = { L"LG", L"GSM", L"23EA53", NULL };
static const wchar_t* KEYWORDS_TV[]     = { L"HISENSE", L"HEC", NULL };
```

## Prerequisites

- **Windows 10/11** (x64)
- **Visual Studio 2022** with the C++ Desktop workload (for the native DLL)
- **.NET 9 SDK** — [download](https://dotnet.microsoft.com/download/dotnet/9.0)
- **CMake 3.20+** — [download](https://cmake.org/download/) (or via Visual Studio)
- **PowerShell 5.1+** (included with Windows)

## Installation (portable)

1. Clone the repository:
   ```powershell
   git clone https://github.com/your-user/MonitorBuddy.git
   cd MonitorBuddy
   ```

2. Build and publish the self-contained portable app:
   ```powershell
   .\publish.ps1
   ```
   This compiles the native DLL and publishes the .NET app into `.\publish\MonitorBuddy\`.

3. Create desktop and Start Menu shortcuts:
   ```powershell
   .\install.ps1
   ```

The app is now available from the desktop shortcut or the Start Menu. No system-wide installation required — everything lives in the `publish\MonitorBuddy` folder.

> **Tip:** You can copy the `publish\MonitorBuddy` folder to any location (e.g. a USB drive) and run `MonitorSwitch.exe` directly.

## Build (development)

```bash
# Native DLL
cd NativeDLL
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release

# Avalonia UI
cd ../MonitorSwitchUI
dotnet build --configuration Release
```

## Run (development)

```bash
dotnet run --project MonitorSwitchUI
```

## License

MIT
