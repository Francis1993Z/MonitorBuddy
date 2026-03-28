# publish.ps1 - Build and publish MonitorBuddy as a portable self-contained app
# Output: .\publish\MonitorBuddy\

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$outDir = "$root\publish\MonitorBuddy"

Write-Host "=== MonitorBuddy - Publish ===" -ForegroundColor Cyan

# Clean previous publish
if (Test-Path $outDir) {
    Remove-Item $outDir -Recurse -Force
}

# 1. Build native DLL
Write-Host "[1/3] Building native DLL..." -ForegroundColor Yellow
Push-Location "$root\NativeDLL"
if (-not (Test-Path "build")) {
    cmake -B build -G "Visual Studio 17 2022" -A x64
}
cmake --build build --config Release
Pop-Location

# 2. Publish .NET app (self-contained, single-directory)
Write-Host "[2/3] Publishing .NET app (self-contained, win-x64)..." -ForegroundColor Yellow
$csproj = "$root\MonitorSwitchUI\MonitorSwitch.csproj"
dotnet publish $csproj -c Release -r win-x64 --self-contained true -o $outDir /p:PublishSingleFile=false /p:IncludeNativeLibrariesForSelfExtract=false

# 3. Copy native DLL
Write-Host "[3/3] Copying native DLL..." -ForegroundColor Yellow
Copy-Item "$root\NativeDLL\build\bin\Release\GestionEcrans.dll" -Destination $outDir -Force

Write-Host "=== Done! ===" -ForegroundColor Green
Write-Host "Portable app: $outDir"
Write-Host "Run install.ps1 to create a desktop shortcut."
