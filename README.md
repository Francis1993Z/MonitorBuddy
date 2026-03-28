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

## Build

```bash
# Native DLL
cd NativeDLL
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release

# Avalonia UI
cd ../MonitorSwitchUI
dotnet build --configuration Release
```

## Run

```bash
dotnet run --project MonitorSwitchUI
```

## License

MIT
