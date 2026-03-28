using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace MonitorSwitch;

/// <summary>
/// Rôle assigné à un écran par l'utilisateur.
/// </summary>
public enum MonitorRole
{
    [JsonStringEnumMemberName("ignore")]
    Ignore,

    [JsonStringEnumMemberName("bureau")]
    Bureau,

    [JsonStringEnumMemberName("tv")]
    Tv
}

/// <summary>
/// Données persistées pour un écran dans layout.json.
/// </summary>
public class MonitorEntry
{
    [JsonPropertyName("device_path")]
    public string DevicePath { get; set; } = "";

    [JsonPropertyName("friendly_name")]
    public string FriendlyName { get; set; } = "";

    [JsonPropertyName("role")]
    public MonitorRole Role { get; set; } = MonitorRole.Ignore;
}

/// <summary>
/// Racine du fichier layout.json (côté C#).
/// </summary>
public class LayoutJson
{
    [JsonPropertyName("monitors")]
    public List<MonitorEntry> Monitors { get; set; } = new();

    [JsonPropertyName("bureau_order")]
    public List<string> BureauOrder { get; set; } = new();

    [JsonPropertyName("primary_index")]
    public int PrimaryIndex { get; set; }
}

/// <summary>
/// Écran CCD détecté par la DLL (résultat de EnumererEcrans).
/// </summary>
public class DetectedMonitor
{
    public uint TargetId { get; set; }
    public uint AdapterIdLo { get; set; }
    public int AdapterIdHi { get; set; }
    public string FriendlyName { get; set; } = "";
    public string DevicePath { get; set; } = "";
    public bool Active { get; set; }

    /// <summary>
    /// Clé unique CCD pour identifier cette cible (format passé à ActiverEcrans).
    /// </summary>
    public string CcdKey => $"{AdapterIdLo}:{AdapterIdHi}:{TargetId}";
}

/// <summary>
/// Gestion de la configuration layout.json et pont entre les écrans
/// détectés par la DLL et les rôles assignés par l'utilisateur.
/// </summary>
public static class MonitorConfig
{
    private static readonly JsonSerializerOptions s_jsonOptions = new()
    {
        WriteIndented = true,
        Converters = { new JsonStringEnumConverter(JsonNamingPolicy.CamelCase) }
    };

    /// <summary>
    /// Chemin vers layout.json, à côté de l'exécutable.
    /// </summary>
    public static string ConfigPath
    {
        get
        {
            var dir = AppContext.BaseDirectory;
            return Path.Combine(dir, "layout.json");
        }
    }

    /// <summary>
    /// Charge layout.json. Retourne un objet vide si le fichier n'existe pas.
    /// </summary>
    public static LayoutJson Load()
    {
        try
        {
            if (!File.Exists(ConfigPath))
                return new LayoutJson();

            var json = File.ReadAllText(ConfigPath);
            return JsonSerializer.Deserialize<LayoutJson>(json, s_jsonOptions)
                   ?? new LayoutJson();
        }
        catch
        {
            return new LayoutJson();
        }
    }

    /// <summary>
    /// Sauvegarde layout.json.
    /// </summary>
    public static void Save(LayoutJson config)
    {
        try
        {
            var json = JsonSerializer.Serialize(config, s_jsonOptions);
            File.WriteAllText(ConfigPath, json);
        }
        catch
        {
            // Silently ignore write errors
        }
    }

    /// <summary>
    /// Parse la sortie brute de EnumererEcrans() en liste d'objets.
    /// </summary>
    public static List<DetectedMonitor> ParseEnumResult(string raw)
    {
        var result = new List<DetectedMonitor>();
        if (string.IsNullOrWhiteSpace(raw)) return result;

        foreach (var line in raw.Split('\n', StringSplitOptions.RemoveEmptyEntries))
        {
            var parts = line.Split('|');
            if (parts.Length < 6) continue;

            if (!uint.TryParse(parts[0], out var tId)) continue;
            if (!uint.TryParse(parts[1], out var adLo)) continue;
            if (!int.TryParse(parts[2], out var adHi)) continue;

            result.Add(new DetectedMonitor
            {
                TargetId = tId,
                AdapterIdLo = adLo,
                AdapterIdHi = adHi,
                FriendlyName = parts[3],
                DevicePath = parts[4],
                Active = parts[5].Trim() == "1"
            });
        }

        return result;
    }

    /// <summary>
    /// Cherche le rôle assigné à un écran détecté dans la config.
    /// Matching par device_path (stable) puis fallback par friendly_name.
    /// </summary>
    public static MonitorRole GetRole(LayoutJson config, DetectedMonitor mon)
    {
        // Priorité : match par device_path
        var entry = config.Monitors.FirstOrDefault(
            m => string.Equals(m.DevicePath, mon.DevicePath, StringComparison.OrdinalIgnoreCase));

        if (entry != null) return entry.Role;

        // Fallback : match par friendly_name
        entry = config.Monitors.FirstOrDefault(
            m => string.Equals(m.FriendlyName, mon.FriendlyName, StringComparison.OrdinalIgnoreCase));

        return entry?.Role ?? MonitorRole.Ignore;
    }

    /// <summary>
    /// Met à jour (ou ajoute) le rôle d'un écran dans la config.
    /// </summary>
    public static void SetRole(LayoutJson config, DetectedMonitor mon, MonitorRole role)
    {
        var entry = config.Monitors.FirstOrDefault(
            m => string.Equals(m.DevicePath, mon.DevicePath, StringComparison.OrdinalIgnoreCase));

        if (entry != null)
        {
            entry.Role = role;
            entry.FriendlyName = mon.FriendlyName; // refresh
        }
        else
        {
            config.Monitors.Add(new MonitorEntry
            {
                DevicePath = mon.DevicePath,
                FriendlyName = mon.FriendlyName,
                Role = role
            });
        }
    }

    /// <summary>
    /// Construit la chaîne à passer à ActiverEcrans() pour un rôle donné.
    /// Format : "adLo:adHi:tId:isPrimary\nadLo:adHi:tId:isPrimary\n..."
    /// </summary>
    public static string BuildTargetList(LayoutJson config, List<DetectedMonitor> detected, MonitorRole role)
    {
        var targets = detected
            .Where(m => GetRole(config, m) == role)
            .ToList();

        // Pour le mode Bureau, respecter bureau_order si disponible
        if (role == MonitorRole.Bureau && config.BureauOrder.Count > 0)
        {
            var ordered = new List<DetectedMonitor>();

            // D'abord les écrans dans l'ordre configuré
            foreach (var name in config.BureauOrder)
            {
                var match = targets.FirstOrDefault(
                    t => string.Equals(t.FriendlyName, name, StringComparison.OrdinalIgnoreCase));
                if (match != null)
                    ordered.Add(match);
            }

            // Puis les écrans non encore dans l'ordre (nouveaux)
            foreach (var t in targets)
            {
                if (!ordered.Any(o => o.CcdKey == t.CcdKey))
                    ordered.Add(t);
            }

            targets = ordered;
        }

        var lines = new List<string>();
        for (int i = 0; i < targets.Count; i++)
        {
            var t = targets[i];
            bool isPrimary = false;
            if (role == MonitorRole.Bureau && i == config.PrimaryIndex) isPrimary = true;
            if (targets.Count <= 1) isPrimary = true;

            lines.Add($"{t.CcdKey}:{(isPrimary ? 1 : 0)}");
        }

        return string.Join("\n", lines);
    }
}
