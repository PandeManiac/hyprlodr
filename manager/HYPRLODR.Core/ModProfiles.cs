using System.Text.Json;

namespace HyprLodr.Core;

/// <summary>A named set of enabled mod ids, saved beside the game installation.</summary>
public sealed class ModProfile
{
    public string Name { get; set; } = "";
    public List<string> EnabledMods { get; set; } = new();
}

/// <summary>
/// Reads and writes mod profiles. A profile records only which mod ids were
/// enabled when it was captured; applying one re-derives the disabled list from
/// what is actually installed, so a profile still works after a mod is removed.
/// </summary>
public static class ProfileStore
{
    public const string FileName = "hyprlodr-profiles.json";
    public const int Schema = 1;
    public const int MaxNameLength = 64;

    public static string PathFor(string gamePath) => Path.Combine(gamePath, FileName);

    /// <summary>Profiles in the order they were saved. Missing or corrupt files
    /// yield an empty list rather than an error: a broken profile file must not
    /// prevent the manager from showing installed mods.</summary>
    public static List<ModProfile> Load(string gamePath)
    {
        var result = new List<ModProfile>();
        var path = PathFor(gamePath);
        if (!File.Exists(path)) return result;

        try
        {
            using var document = JsonDocument.Parse(File.ReadAllText(path));
            if (!document.RootElement.TryGetProperty("profiles", out var profiles) ||
                profiles.ValueKind != JsonValueKind.Array)
                return result;

            foreach (var element in profiles.EnumerateArray())
            {
                if (element.ValueKind != JsonValueKind.Object) continue;
                var name = element.TryGetProperty("name", out var nameElement) &&
                           nameElement.ValueKind == JsonValueKind.String
                    ? nameElement.GetString()?.Trim() ?? ""
                    : "";
                if (name.Length == 0) continue;

                var profile = new ModProfile { Name = name };
                if (element.TryGetProperty("enabledMods", out var enabled) &&
                    enabled.ValueKind == JsonValueKind.Array)
                {
                    foreach (var id in enabled.EnumerateArray())
                    {
                        if (id.ValueKind != JsonValueKind.String) continue;
                        var value = id.GetString();
                        if (string.IsNullOrWhiteSpace(value)) continue;
                        if (!profile.EnabledMods.Contains(value, StringComparer.Ordinal))
                            profile.EnabledMods.Add(value);
                    }
                }
                result.Add(profile);
            }
        }
        catch (JsonException)
        {
            // Corrupt file: treated as "no profiles".
        }
        catch (IOException)
        {
            // Unreadable file: same.
        }
        return result;
    }

    public static void Save(string gamePath, IEnumerable<ModProfile> profiles)
    {
        Directory.CreateDirectory(gamePath);
        var payload = new
        {
            schema = Schema,
            profiles = profiles
                .Where(profile => !string.IsNullOrWhiteSpace(profile.Name))
                .Select(profile => new
                {
                    // Clamp here, not only in Capture: a caller can build a
                    // profile directly, and a persisted name must match what a
                    // reload yields.
                    name = NormaliseName(profile.Name),
                    enabledMods = profile.EnabledMods
                        .Distinct(StringComparer.Ordinal)
                        .OrderBy(id => id, StringComparer.Ordinal)
                        .ToArray(),
                })
                .ToArray(),
        };
        var json = JsonSerializer.Serialize(payload, new JsonSerializerOptions { WriteIndented = true });
        File.WriteAllText(PathFor(gamePath), json + Environment.NewLine);
    }

    /// <summary>Capture the currently enabled set.</summary>
    public static ModProfile Capture(string name, ModCatalog catalog)
    {
        var profile = new ModProfile { Name = NormaliseName(name) };
        profile.EnabledMods.AddRange(catalog.Mods
            .Where(mod => mod.Enabled)
            .Select(mod => mod.Id)
            .Distinct(StringComparer.Ordinal)
            .OrderBy(id => id, StringComparer.Ordinal));
        return profile;
    }

    /// <summary>Enabled ids recorded in the profile that are no longer installed.</summary>
    public static IReadOnlyList<string> Missing(ModProfile profile, ModCatalog catalog) =>
        profile.EnabledMods
            .Where(id => catalog.Mods.All(mod => !string.Equals(mod.Id, id, StringComparison.Ordinal)))
            .OrderBy(id => id, StringComparer.Ordinal)
            .ToList();

    /// <summary>Apply a profile: enable exactly the ids it names and rewrite the
    /// disabled list so the native loader agrees with the UI.</summary>
    public static IReadOnlyList<string> Apply(ModProfile profile, ModCatalog catalog, string modsDirectory)
    {
        var wanted = new HashSet<string>(profile.EnabledMods, StringComparer.Ordinal);
        foreach (var mod in catalog.Mods)
            mod.Enabled = wanted.Contains(mod.Id) && mod.Status != "binary missing";

        ModCatalog.SaveDisabled(modsDirectory, catalog.Mods
            .Where(mod => !mod.Enabled)
            .Select(mod => mod.Id));
        return Missing(profile, catalog);
    }

    public static string NormaliseName(string name)
    {
        var trimmed = (name ?? "").Trim();
        if (trimmed.Length == 0) return "profile";
        return trimmed.Length <= MaxNameLength ? trimmed : trimmed[..MaxNameLength];
    }
}
