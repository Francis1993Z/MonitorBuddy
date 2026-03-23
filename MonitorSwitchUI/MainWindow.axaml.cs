using System;
using System.Runtime.InteropServices;
using System.Text;
using Avalonia.Controls;
using Avalonia.Interactivity;
using Avalonia.Input;

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

    public MainWindow()
    {
        InitializeComponent();

        // Permettre de déplacer la fenêtre en cliquant n'importe où
        PointerPressed += (_, e) =>
        {
            if (e.GetCurrentPoint(this).Properties.IsLeftButtonPressed)
                BeginMoveDrag(e);
        };

        // Afficher le diagnostic au démarrage
        RafraichirDiagnostic();
    }

    private void RafraichirDiagnostic()
    {
        try
        {
            var sb = new StringBuilder(4096);
            ObtenirInfoEcrans(sb, 4096);
            DiagText.Text = sb.Length > 0
                ? sb.ToString().TrimEnd()
                : "Aucun écran détecté.";
        }
        catch (Exception ex)
        {
            DiagText.Text = $"Erreur détection : {ex.Message}";
        }
    }

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
    }

    private void OnClickQuitter(object? sender, RoutedEventArgs e)
    {
        Close();
    }
}