using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Markup.Xaml;
using Avalonia.Media.Imaging;
using Avalonia.Platform.Storage;

namespace MonitorSwitch;

public partial class App : Application
{
    private MainWindow? _mainWindow;
    private TrayIcon? _trayIcon;

    [DllImport("GestionEcrans.dll", CallingConvention = CallingConvention.Cdecl)]
    private static extern bool ActiverModeTV();

    [DllImport("GestionEcrans.dll", CallingConvention = CallingConvention.Cdecl)]
    private static extern bool ActiverModeBureau();

    public override void Initialize()
    {
        AvaloniaXamlLoader.Load(this);
    }

    public override void OnFrameworkInitializationCompleted()
    {
        if (ApplicationLifetime is IClassicDesktopStyleApplicationLifetime desktop)
        {
            desktop.ShutdownMode = ShutdownMode.OnExplicitShutdown;

            _mainWindow = new MainWindow();
            desktop.MainWindow = _mainWindow;

            SetupTrayIcon();
        }

        base.OnFrameworkInitializationCompleted();
    }

    private void SetupTrayIcon()
    {
        _trayIcon = new TrayIcon
        {
            ToolTipText = "Monitor Switch",
            Icon = CreateSimpleIcon(),
            IsVisible = true
        };

        var menu = new NativeMenu();

        var itemTV = new NativeMenuItem("Mode TV");
        itemTV.Click += (_, _) =>
        {
            try { ActiverModeTV(); } catch { }
        };

        var itemBureau = new NativeMenuItem("Mode Bureau");
        itemBureau.Click += (_, _) =>
        {
            try { ActiverModeBureau(); } catch { }
        };

        var itemSep = new NativeMenuItemSeparator();

        var itemShow = new NativeMenuItem("Afficher");
        itemShow.Click += (_, _) => ShowMainWindow();

        var itemQuit = new NativeMenuItem("Quitter");
        itemQuit.Click += (_, _) => QuitApp();

        menu.Items.Add(itemTV);
        menu.Items.Add(itemBureau);
        menu.Items.Add(itemSep);
        menu.Items.Add(itemShow);
        menu.Items.Add(itemQuit);

        _trayIcon.Menu = menu;
        _trayIcon.Clicked += (_, _) => ShowMainWindow();

        var icons = new TrayIcons { _trayIcon };
        SetValue(TrayIcon.IconsProperty, icons);
    }

    private void ShowMainWindow()
    {
        if (_mainWindow != null)
        {
            _mainWindow.Show();
            _mainWindow.WindowState = WindowState.Normal;
            _mainWindow.Activate();
        }
    }

    public void QuitApp()
    {
        if (_trayIcon != null)
        {
            _trayIcon.IsVisible = false;
            _trayIcon.Dispose();
            _trayIcon = null;
        }

        if (ApplicationLifetime is IClassicDesktopStyleApplicationLifetime desktop)
            desktop.Shutdown();
    }

    private static WindowIcon CreateSimpleIcon()
    {
        // Creer un bitmap 32x32 avec un simple dessin "ecran"
        var bmp = new WriteableBitmap(
            new PixelSize(32, 32),
            new Vector(96, 96),
            Avalonia.Platform.PixelFormat.Bgra8888,
            Avalonia.Platform.AlphaFormat.Premul);

        using (var buf = bmp.Lock())
        {
            unsafe
            {
                var ptr = (uint*)buf.Address;
                for (int y = 0; y < 32; y++)
                {
                    for (int x = 0; x < 32; x++)
                    {
                        uint color;
                        // Fond ecran (rectangle bleu fonce)
                        if (x >= 3 && x <= 28 && y >= 2 && y <= 22)
                            color = 0xFF3B2E1E; // BGRA: dark blue #1E2E3B
                        // Bordure ecran
                        else if (x >= 2 && x <= 29 && y >= 1 && y <= 23)
                            color = 0xFFF4D6CD; // BGRA: light #CDD6F4
                        // Pied
                        else if (x >= 12 && x <= 19 && y >= 24 && y <= 27)
                            color = 0xFFA8ADC8; // BGRA: gray
                        // Base
                        else if (x >= 9 && x <= 22 && y >= 28 && y <= 29)
                            color = 0xFFA8ADC8;
                        else
                            color = 0x00000000; // transparent
                        ptr[y * 32 + x] = color;
                    }
                }
            }
        }

        var stream = new MemoryStream();
        bmp.Save(stream);
        stream.Position = 0;
        return new WindowIcon(stream);
    }
}