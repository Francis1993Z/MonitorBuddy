using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Interactivity;
using Avalonia.Input;
using Avalonia.Media;
using Avalonia.VisualTree;

namespace MonitorSwitch;

public partial class MainWindow : Window
{
    // ── P/Invoke vers la DLL native (API agnostique) ──
    [DllImport("GestionEcrans.dll", CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
    private static extern void EnumererEcrans(StringBuilder buffer, int tailleMax);

    [DllImport("GestionEcrans.dll", CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
    private static extern bool ActiverEcrans(string targetList);

    [DllImport("GestionEcrans.dll", CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
    private static extern void ObtenirInfoEcrans(StringBuilder buffer, int tailleMax);

    // ── État ──
    private LayoutJson _config = new();
    private List<DetectedMonitor> _detected = new();
    private List<string> _layoutOrder = new();
    private int _primaryIndex = 0;

    public MainWindow()
    {
        InitializeComponent();

        PointerPressed += (_, e) =>
        {
            if (e.GetCurrentPoint(this).Properties.IsLeftButtonPressed &&
                e.Source is Avalonia.Visual v)
            {
                // Ne pas draguer si on clique sur un élément interactif (ComboBox, Button, ScrollViewer)
                bool isInteractive = false;
                var current = v;
                while (current != null)
                {
                    if (current is ComboBox || current is Button || current is ScrollViewer)
                    {
                        isInteractive = true;
                        break;
                    }
                    current = current.GetVisualParent() as Avalonia.Visual;
                }

                if (!isInteractive && (v == this || v is Avalonia.Controls.Shapes.Rectangle ||
                     v is Border || v is Panel || v is Grid || v is TextBlock))
                {
                    BeginMoveDrag(e);
                }
            }
        };

        _config = MonitorConfig.Load();
        RafraichirDetection();
        RafraichirDiagnostic();
        ChargerLayout();
    }

    // ── Détection des écrans via DLL ──

    private void RafraichirDetection()
    {
        try
        {
            var sb = new StringBuilder(32768);
            EnumererEcrans(sb, 32768);
            _detected = MonitorConfig.ParseEnumResult(sb.ToString());
            RafraichirRolesUI();
        }
        catch (Exception ex)
        {
            StatusText.Text = $"Erreur détection : {ex.Message}";
        }
    }

    // ── Rôles UI ──

    private void RafraichirRolesUI()
    {
        var items = _detected.Select(m =>
        {
            var item = new RoleItem
            {
                FriendlyName = m.FriendlyName,
                DevicePath = m.DevicePath,
                CcdKey = m.CcdKey,
                Active = m.Active,
                RoleIndex = (int)MonitorConfig.GetRole(_config, m)
            };
            item.OnRoleChanged = HandleRoleChanged;
            return item;
        }).ToList();
        RoleList.ItemsSource = items;
    }

    private void HandleRoleChanged(RoleItem item)
    {
        var mon = _detected.FirstOrDefault(m => m.CcdKey == item.CcdKey);
        if (mon == null) return;

        var role = item.RoleIndex switch
        {
            1 => MonitorRole.Bureau,
            2 => MonitorRole.Tv,
            _ => MonitorRole.Ignore
        };

        MonitorConfig.SetRole(_config, mon, role);
        MonitorConfig.Save(_config);
        ChargerLayout();
    }

    // ── Layout config (Bureau order) ──

    private void ChargerLayout()
    {
        try
        {
            // Écrans Bureau détectés
            var ecransBureau = _detected
                .Where(m => MonitorConfig.GetRole(_config, m) == MonitorRole.Bureau)
                .Select(m => m.FriendlyName)
                .ToList();

            // Ordre sauvegardé
            _layoutOrder = _config.BureauOrder.ToList();
            _primaryIndex = _config.PrimaryIndex;

            // Ajouter les écrans détectés qui ne sont pas dans la config
            foreach (var name in ecransBureau)
                if (!_layoutOrder.Contains(name, StringComparer.OrdinalIgnoreCase))
                    _layoutOrder.Add(name);

            // Retirer les écrans dans la config qui ne sont plus détectés
            _layoutOrder = _layoutOrder
                .Where(n => ecransBureau.Contains(n, StringComparer.OrdinalIgnoreCase))
                .ToList();

            if (_primaryIndex < 0 || _primaryIndex >= _layoutOrder.Count)
                _primaryIndex = 0;

            // Synchroniser vers la config
            _config.BureauOrder = _layoutOrder.ToList();
            _config.PrimaryIndex = _primaryIndex;

            RafraichirLayoutUI();
        }
        catch (Exception ex)
        {
            StatusText.Text = $"Erreur layout : {ex.Message}";
        }
    }

    private void SauvegarderLayout()
    {
        _config.BureauOrder = _layoutOrder.ToList();
        _config.PrimaryIndex = _primaryIndex;
        MonitorConfig.Save(_config);
    }

    private void RafraichirLayoutUI()
    {
        var items = new List<LayoutItem>();
        for (int i = 0; i < _layoutOrder.Count; i++)
        {
            items.Add(new LayoutItem
            {
                Name = _layoutOrder[i],
                Position = i == 0 ? "◀ G" : i == _layoutOrder.Count - 1 ? "D ▶" : $"  {i + 1} ",
                PrimaryColor = i == _primaryIndex
                    ? Brushes.Gold
                    : Brushes.Gray
            });
        }
        LayoutList.ItemsSource = items;
    }

    // ── Layout buttons ──

    private void OnClickSetPrimary(object? sender, RoutedEventArgs e)
    {
        if (sender is Button btn && btn.Tag is string name)
        {
            int idx = _layoutOrder.FindIndex(n => n == name);
            if (idx >= 0)
            {
                _primaryIndex = idx;
                SauvegarderLayout();
                RafraichirLayoutUI();
            }
        }
    }

    private void OnClickMoveLeft(object? sender, RoutedEventArgs e)
    {
        if (sender is Button btn && btn.Tag is string name)
        {
            int idx = _layoutOrder.FindIndex(n => n == name);
            if (idx > 0)
            {
                (_layoutOrder[idx], _layoutOrder[idx - 1]) = (_layoutOrder[idx - 1], _layoutOrder[idx]);
                if (_primaryIndex == idx) _primaryIndex--;
                else if (_primaryIndex == idx - 1) _primaryIndex++;
                SauvegarderLayout();
                RafraichirLayoutUI();
            }
        }
    }

    private void OnClickMoveRight(object? sender, RoutedEventArgs e)
    {
        if (sender is Button btn && btn.Tag is string name)
        {
            int idx = _layoutOrder.FindIndex(n => n == name);
            if (idx >= 0 && idx < _layoutOrder.Count - 1)
            {
                (_layoutOrder[idx], _layoutOrder[idx + 1]) = (_layoutOrder[idx + 1], _layoutOrder[idx]);
                if (_primaryIndex == idx) _primaryIndex++;
                else if (_primaryIndex == idx + 1) _primaryIndex--;
                SauvegarderLayout();
                RafraichirLayoutUI();
            }
        }
    }

    // ── Diagnostic ──

    private void RafraichirDiagnostic()
    {
        try
        {
            var sb = new StringBuilder(16384);
            ObtenirInfoEcrans(sb, 16384);
            DiagText.Text = sb.Length > 0
                ? sb.ToString().TrimEnd()
                : "Aucun écran détecté.";
        }
        catch (Exception ex)
        {
            DiagText.Text = $"Erreur détection : {ex.Message}";
        }
    }

    // ── Boutons mode ──

    private void OnClickModeTV(object? sender, RoutedEventArgs e)
    {
        try
        {
            var targetList = MonitorConfig.BuildTargetList(_config, _detected, MonitorRole.Tv);
            if (string.IsNullOrEmpty(targetList))
            {
                StatusText.Text = "❌ Aucun écran assigné au rôle TV";
                return;
            }
            bool ok = ActiverEcrans(targetList);
            StatusText.Text = ok
                ? "✅ Mode TV activé"
                : "❌ Échec du basculement en mode TV";
        }
        catch (Exception ex)
        {
            StatusText.Text = $"❌ Erreur : {ex.Message}";
        }
        RafraichirDetection();
        RafraichirDiagnostic();
    }

    private void OnClickModeBureau(object? sender, RoutedEventArgs e)
    {
        try
        {
            var targetList = MonitorConfig.BuildTargetList(_config, _detected, MonitorRole.Bureau);
            if (string.IsNullOrEmpty(targetList))
            {
                StatusText.Text = "❌ Aucun écran assigné au rôle Bureau";
                return;
            }
            bool ok = ActiverEcrans(targetList);
            StatusText.Text = ok
                ? "✅ Mode Bureau activé"
                : "❌ Échec du basculement en mode Bureau";
        }
        catch (Exception ex)
        {
            StatusText.Text = $"❌ Erreur : {ex.Message}";
        }
        RafraichirDetection();
        RafraichirDiagnostic();
        ChargerLayout();
    }

    private void OnClickMinimiser(object? sender, RoutedEventArgs e)
    {
        Hide();
    }

    private void OnClickQuitter(object? sender, RoutedEventArgs e)
    {
        if (Application.Current is App app)
            app.QuitApp();
    }

    protected override void OnClosing(WindowClosingEventArgs e)
    {
        // Fermer la fenêtre → masquer dans le systray (pas quitter)
        e.Cancel = true;
        Hide();
        base.OnClosing(e);
    }
}

public class LayoutItem
{
    public string Name { get; set; } = "";
    public string Position { get; set; } = "";
    public IBrush PrimaryColor { get; set; } = Brushes.Gray;
}

public class RoleItem : INotifyPropertyChanged
{
    private int _roleIndex;

    public string FriendlyName { get; set; } = "";
    public string DevicePath { get; set; } = "";
    public string CcdKey { get; set; } = "";
    public bool Active { get; set; }
    public string StatusIcon => Active ? "ON" : "OFF";
    public IBrush StatusColor => Active ? Brushes.LimeGreen : Brushes.Gray;

    /// <summary>Callback invoked when the user changes the role via ComboBox.</summary>
    public Action<RoleItem>? OnRoleChanged { get; set; }

    public int RoleIndex
    {
        get => _roleIndex;
        set
        {
            if (_roleIndex == value) return;
            _roleIndex = value;
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(RoleIndex)));
            OnRoleChanged?.Invoke(this);
        }
    }

    public event PropertyChangedEventHandler? PropertyChanged;
}