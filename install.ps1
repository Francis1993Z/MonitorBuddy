# install.ps1 - Create desktop and Start Menu shortcuts for MonitorBuddy
# Run this AFTER publish.ps1

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$appDir = "$root\publish\MonitorBuddy"
$exeName = "MonitorSwitch.exe"
$exePath = Join-Path $appDir $exeName
$shortcutName = "MonitorBuddy.lnk"

Write-Host "=== MonitorBuddy - Install Shortcut ===" -ForegroundColor Cyan

# Verify the app exists
if (-not (Test-Path $exePath)) {
    Write-Host "ERROR: $exePath not found." -ForegroundColor Red
    Write-Host "Run publish.ps1 first to build the portable app."
    exit 1
}

# Create desktop shortcut
$desktopPath = [Environment]::GetFolderPath("Desktop")
$shortcutPath = Join-Path $desktopPath $shortcutName

$shell = New-Object -ComObject WScript.Shell
$shortcut = $shell.CreateShortcut($shortcutPath)
$shortcut.TargetPath = $exePath
$shortcut.WorkingDirectory = $appDir
$shortcut.Description = "MonitorBuddy - Switch between TV and Desktop modes"
$shortcut.Save()

Write-Host "Desktop shortcut created: $shortcutPath" -ForegroundColor Green

# Also create a Start Menu shortcut
$startMenuDir = Join-Path ([Environment]::GetFolderPath("StartMenu")) "Programs"
$startShortcutPath = Join-Path $startMenuDir $shortcutName

$shortcut2 = $shell.CreateShortcut($startShortcutPath)
$shortcut2.TargetPath = $exePath
$shortcut2.WorkingDirectory = $appDir
$shortcut2.Description = "MonitorBuddy - Switch between TV and Desktop modes"
$shortcut2.Save()

Write-Host "Start Menu shortcut created: $startShortcutPath" -ForegroundColor Green
Write-Host "=== Done! ===" -ForegroundColor Green
