# Monitor Switch

Application Windows pour basculer entre un mode TV (écran unique) et un mode Bureau (deux moniteurs LG côte à côte).

## Architecture

- **NativeDLL/** : Moteur C++ natif avec auto-détection des écrans via Win32 API
- **MonitorSwitchUI/** : Interface Avalonia UI (C#) qui consomme la DLL native

## Fonctionnalités

- Auto-détecte les écrans via `EnumDisplayDevices` (noms EDID)
- Assigne automatiquement les rôles TV vs Bureau via mot-clé configurable
- Détecte la résolution native de chaque écran
- Calcule dynamiquement les positions (plus de valeurs en dur)
- Panneau de diagnostic dans l'UI pour vérifier la détection

## Configuration

Modifier le mot-clé dans `NativeDLL/GestionEcrans.cpp` :

```cpp
static const wchar_t* KEYWORD_BUREAU = L"LG";  // Change selon ton matériel
```

## Build

```bash
# Compiler la DLL native
cd NativeDLL
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release

# Compiler l'interface Avalonia
cd ../MonitorSwitchUI
dotnet build --configuration Release
```

## Lancer

```bash
dotnet run --project MonitorSwitchUI
```
