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

namespace MonitorSwitch;

public partial class MainWindow : Window
{
    // ── P/Invoke vers la DLL native ──
    [DllImport("GestionEcrans.dll", CallingConvention = CallingConvention.Cdecl)]
    private static extern bool ActiverModeTV();

    [DllImport("GestionEcrans.dll", CallingConvention = CallingConvention.Cdecl)]
    private static extern bool ActiverModeBureau();

    [DllImport("GestionEcrans.dll", CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
    private static extern void ObtenirInfoEcrans(StringBuilder buffer, int tailleMax);

    [DllImport("GestionEcrans.dll", CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
    private static extern void ObtenirEcransBureau(StringBuilder buffer, int tailleMax);

    [DllImport("GestionEcrans.dll", CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
    private static extern void ObtenirLayoutConfig(StringBuilder buffer, int tailleMax);

    [DllImport("GestionEcrans.dll", CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
    private static extern void DefinirOrdreBureau(string config);

    // ── État du layout ──
    private List<string> _layoutOrder = new();
    private int _primaryIndex = 0;

    public MainWindow()
    {
        InitializeComponent();

        PointerPressed += (_, e) =>
        {
            if (e.GetCurrentPoint(this).Properties.IsLeftButtonPressed)
                BeginMoveDrag(e);
        };

        RafraichirDiagnostic();
        ChargerLayout();
    }

    // ── Layout config ──

    private void ChargerLayout()
    {
        try
        {
            // D'abord, obtenir les écrans bureau détectés
            var sbBureau = new StringBuilder(4096);
            ObtenirEcransBureau(sbBureau, 4096);
            var ecransDetectes = sbBureau.Length > 0
                ? sbBureau.ToString().Split('|').ToList()
                : new List<string>();

            // Ensuite, charger la config sauvegardée
            var sbConfig = new StringBuilder(4096);
            ObtenirLayoutConfig(sbConfig, 4096);
            var configStr = sbConfig.ToString();

            if (!string.IsNullOrEmpty(configStr) && configStr.Contains('\n'))
            {
                var parts = configStr.Split('\n');
                _layoutOrder = parts[0].Split('|').Where(s => !string.IsNullOrEmpty(s)).ToList();
                int.TryParse(parts[1], out _primaryIndex);

                // Ajouter les écrans détectés qui ne sont pas dans la config
                foreach (var e in ecransDetectes)
                    if (!_layoutOrder.Contains(e, StringComparer.OrdinalIgnoreCase))
                        _layoutOrder.Add(e);

                // Retirer les écrans dans la config qui ne sont plus détectés
                _layoutOrder = _layoutOrder
                    .Where(n => ecransDetectes.Contains(n, StringComparer.OrdinalIgnoreCase))
                    .ToList();
            }
            else
            {
                // Pas de config → utiliser l'ordre de détection
                _layoutOrder = ecransDetectes;
                _primaryIndex = 0;
            }

            if (_primaryIndex < 0 || _primaryIndex >= _layoutOrder.Count)
                _primaryIndex = 0;

            RafraichirLayoutUI();
        }
        catch (Exception ex)
        {
            StatusText.Text = $"Erreur layout : {ex.Message}";
        }
    }

    private void SauvegarderLayout()
    {
        try
        {
            var config = string.Join("|", _layoutOrder) + "\n" + _primaryIndex;
            DefinirOrdreBureau(config);
        }
        catch { }
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

    private void OnClickMoveLeft(object? sender, RoutedEventArgs e)
    {
        if (sender is Button btn && btn.Tag is string name)
        {
            int idx = _layoutOrder.IndexOf(name);
            if (idx > 0)
            {
                (_layoutOrder[idx], _layoutOrder[idx - 1]) = (_layoutOrder[idx - 1], _layoutOrder[idx]);
                // Ajuster primaryIndex si nécessaire
                if (_primaryIndex == idx) _primaryIndex = idx - 1;
                else if (_primaryIndex == idx - 1) _primaryIndex = idx;
                SauvegarderLayout();
                RafraichirLayoutUI();
            }
        }
    }

    private void OnClickMoveRight(object? sender, RoutedEventArgs e)
    {
        if (sender is Button btn && btn.Tag is string name)
        {
            int idx = _layoutOrder.IndexOf(name);
            if (idx >= 0 && idx < _layoutOrder.Count - 1)
            {
                (_layoutOrder[idx], _layoutOrder[idx + 1]) = (_layoutOrder[idx + 1], _layoutOrder[idx]);
                if (_primaryIndex == idx) _primaryIndex = idx + 1;
                else if (_primaryIndex == idx + 1) _primaryIndex = idx;
                SauvegarderLayout();
                RafraichirLayoutUI();
            }
        }
    }

    private void OnClickSetPrimary(object? sender, RoutedEventArgs e)
    {
        if (sender is Button btn && btn.Tag is string name)
        {
            int idx = _layoutOrder.IndexOf(name);
            if (idx >= 0)
            {
                _primaryIndex = idx;
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
            bool ok = ActiverModeTV();
            StatusText.Text = ok
                ? "✅ Mode TV activé"
                : "❌ Échec du basculement en mode TV";
        }
        catch (Exception ex)
        {
            StatusText.Text = $"❌ Erreur : {ex.Message}";
        }
        RafraichirDiagnostic();
    }

    private void OnClickModeBureau(object? sender, RoutedEventArgs e)
    {
        try
        {
            bool ok = ActiverModeBureau();
            StatusText.Text = ok
                ? "✅ Mode Bureau activé"
                : "❌ Échec du basculement en mode Bureau";
        }
        catch (Exception ex)
        {
            StatusText.Text = $"❌ Erreur : {ex.Message}";
        }
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