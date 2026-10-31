using System.Text.Json;
using System.Text.RegularExpressions;

namespace HyprLodr.Core;

public sealed class ModEntry
{
    public required string Id { get; init; }
    public required string FileName { get; init; }
    public string? Name { get; set; }
    public string? Version { get; set; }
    public string? Author { get; set; }
    public string? Description { get; set; }
    public List<string> Dependencies { get; } = new();
    public List<string> OptionalDependencies { get; } = new();
    public List<string> Conflicts { get; } = new();
    public List<string> Capabilities { get; } = new();
    public bool Enabled { get; set; } = true;
    public string? Sha256 { get; set; }
    public string? InstalledAt { get; set; }
    /// <summary>Runtime readiness derived from the load plan.</summary>
    public string Status { get; set; } = "ok";
}

public sealed class LoadPlan
{
    public List<string> Order { get; } = new();
    public List<string> Problems { get; } = new();
    public bool Ok => Problems.Count == 0;
}

/// <summary>
/// Reads the mods directory (binaries + install registry + disabled list) and
/// reproduces the native loader's ordering rules so the manager can show what the
/// game will do before it is launched.
/// </summary>
public sealed partial class ModCatalog
{
    public const string RegistryName = "hyprlodr-mods.json";
    public const string DisabledListName = "disabled.txt";

    public List<ModEntry> Mods { get; } = new();

    [GeneratedRegex("^[a-z0-9._-]{1,127}$")]
    private static partial Regex IdRegex();

    [GeneratedRegex("^[a-z0-9._-]{1,127}(@(>=|<=|>|<|=|\\^|~)\\d+(\\.\\d+){0,2})?$")]
    private static partial Regex DependencyRegex();

    public static ModCatalog Load(string modsDirectory)
    {
        var catalog = new ModCatalog();
        if (!Directory.Exists(modsDirectory)) return catalog;

        var disabled = ReadDisabled(Path.Combine(modsDirectory, DisabledListName));
        var registry = ReadRegistry(Path.Combine(modsDirectory, RegistryName));

        var binaries = Directory.EnumerateFiles(modsDirectory)
            .Where(path => path.EndsWith(".dll", StringComparison.OrdinalIgnoreCase) ||
                           path.EndsWith(".so", StringComparison.OrdinalIgnoreCase))
            .OrderBy(path => Path.GetFileName(path), StringComparer.Ordinal)
            .ToList();

        foreach (var binary in binaries)
        {
            var fileName = Path.GetFileName(binary);
            var id = Path.GetFileNameWithoutExtension(fileName);
            registry.TryGetValue(id, out var record);
            var disabledEntry = disabled.Contains(id) || disabled.Contains(fileName);
            var mod = new ModEntry
            {
                Id = record?.Id is { Length: > 0 } recorded ? recorded : id,
                FileName = fileName,
                Name = record?.Name,
                Version = record?.Version,
                Author = record?.Author,
                Description = record?.Description,
                Sha256 = record?.Sha256,
                InstalledAt = record?.InstalledAt,
                Enabled = !disabledEntry,
            };
            if (record is not null)
            {
                mod.Dependencies.AddRange(record.Dependencies);
                mod.OptionalDependencies.AddRange(record.OptionalDependencies);
                mod.Conflicts.AddRange(record.Conflicts);
                mod.Capabilities.AddRange(record.Capabilities);
            }
            catalog.Mods.Add(mod);
        }

        // Registry entries whose binary is gone are shown as broken rather than hidden.
        foreach (var (id, record) in registry)
        {
            if (catalog.Mods.Any(mod => mod.Id == id)) continue;
            var entry = new ModEntry
            {
                Id = id,
                FileName = record.FileName,
                Name = record.Name,
                Version = record.Version,
                Author = record.Author,
                Description = record.Description,
                Sha256 = record.Sha256,
                Enabled = false,
                Status = "binary missing",
            };
            entry.Dependencies.AddRange(record.Dependencies);
            entry.Conflicts.AddRange(record.Conflicts);
            catalog.Mods.Add(entry);
        }

        catalog.Mods.Sort((left, right) => string.CompareOrdinal(left.Id, right.Id));
        return catalog;
    }

    public LoadPlan BuildPlan()
    {
        var plan = new LoadPlan();
        foreach (var mod in Mods) mod.Status = "ok";

        foreach (var mod in Mods)
        {
            if (!IdRegex().IsMatch(mod.Id))
            {
                mod.Status = "invalid id";
                plan.Problems.Add($"{mod.Id}: invalid mod id");
            }
            if (mod.Status == "binary missing")
                plan.Problems.Add($"{mod.Id}: binary missing from the mods directory");
            foreach (var dependency in mod.Dependencies.Where(d => !DependencyRegex().IsMatch(d)))
                plan.Problems.Add($"{mod.Id}: malformed dependency spec '{dependency}'");
        }

        var byId = Mods.GroupBy(mod => mod.Id, StringComparer.Ordinal)
            .ToDictionary(group => group.Key, group => group.ToList(), StringComparer.Ordinal);
        foreach (var (id, group) in byId.Where(pair => pair.Value.Count > 1))
            plan.Problems.Add($"{id}: duplicate mod id ({group.Count} files)");

        var enabled = Mods.Where(mod => mod.Enabled && mod.Status != "binary missing").ToList();
        var emitted = new HashSet<string>(StringComparer.Ordinal);
        // A mod blocked by a dependency problem is not a cycle; keep the two apart so
        // the preview matches how the native loader classifies failures.
        var blocked = new HashSet<string>(StringComparer.Ordinal);

        while (plan.Order.Count < enabled.Count)
        {
            ModEntry? chosen = null;
            foreach (var candidate in enabled
                         .Where(mod => !emitted.Contains(mod.Id) && !blocked.Contains(mod.Id))
                         .OrderBy(mod => mod.Id, StringComparer.Ordinal))
            {
                var ready = true;
                foreach (var spec in candidate.Dependencies)
                {
                    if (!DependencyRegex().IsMatch(spec))
                    {
                        ready = false;
                        break;
                    }
                    var (dependencyId, op, requiredVersion) = SplitSpec(spec);
                    var dependency = Mods.FirstOrDefault(mod => mod.Id == dependencyId);
                    if (dependency is null)
                    {
                        candidate.Status = "dependency missing";
                        plan.Problems.Add($"{candidate.Id}: missing dependency: {dependencyId}");
                        blocked.Add(candidate.Id);
                        ready = false;
                        break;
                    }
                    if (!dependency.Enabled)
                    {
                        candidate.Status = "dependency disabled";
                        plan.Problems.Add($"{candidate.Id}: dependency is disabled: {dependencyId}");
                        blocked.Add(candidate.Id);
                        ready = false;
                        break;
                    }
                    if (!Satisfies(op, requiredVersion, dependency.Version))
                    {
                        candidate.Status = "version mismatch";
                        plan.Problems.Add(
                            $"{candidate.Id}: dependency {dependencyId} {dependency.Version} does not satisfy '{spec}'");
                        blocked.Add(candidate.Id);
                        ready = false;
                        break;
                    }
                    if (!emitted.Contains(dependencyId))
                    {
                        ready = false;
                        break;
                    }
                }
                if (!ready) continue;
                chosen = candidate;
                break;
            }

            if (chosen is null)
            {
                var stuck = enabled
                    .Where(mod => !emitted.Contains(mod.Id) && !blocked.Contains(mod.Id))
                    .Select(mod => mod.Id)
                    .OrderBy(id => id, StringComparer.Ordinal)
                    .ToList();
                if (stuck.Count == 0) break; // everything left is blocked; already reported
                plan.Problems.Add($"cycle among: {string.Join(", ", stuck)}");
                foreach (var mod in enabled.Where(mod => !emitted.Contains(mod.Id) && !blocked.Contains(mod.Id)))
                    if (mod.Status == "ok") mod.Status = "cycle";
                break;
            }

            emitted.Add(chosen.Id);
            plan.Order.Add(chosen.Id);
        }

        foreach (var mod in Mods.Where(mod => !mod.Enabled && mod.Status == "ok")) mod.Status = "disabled";

        foreach (var mod in enabled)
        {
            foreach (var spec in mod.Conflicts)
            {
                var (otherId, _, _) = SplitSpec(spec);
                if (emitted.Contains(otherId))
                    plan.Problems.Add($"{mod.Id}: conflicts with loaded mod {otherId}");
            }
        }

        return plan;
    }

    public void SetEnabled(string modId, bool enabled)
    {
        var mod = Mods.FirstOrDefault(entry => entry.Id == modId);
        if (mod is null) return;
        mod.Enabled = enabled;
    }

    public static void SaveDisabled(string modsDirectory, IEnumerable<string> disabledIds)
    {
        Directory.CreateDirectory(modsDirectory);
        var path = Path.Combine(modsDirectory, DisabledListName);
        var lines = new List<string>
        {
            "# Managed by HYPRLODR: mods listed here are installed but not loaded.",
            "# One mod id per line; '#' starts a comment.",
        };
        lines.AddRange(disabledIds.OrderBy(id => id, StringComparer.Ordinal));
        File.WriteAllLines(path, lines);
    }

    private static HashSet<string> ReadDisabled(string path)
    {
        var result = new HashSet<string>(StringComparer.Ordinal);
        if (!File.Exists(path)) return result;
        foreach (var raw in File.ReadAllLines(path))
        {
            var line = raw.Trim();
            if (line.Length == 0 || line.StartsWith('#')) continue;
            result.Add(line);
        }
        return result;
    }

    private sealed class RegistryRecord
    {
        public string Id { get; set; } = "";
        public string Name { get; set; } = "";
        public string Version { get; set; } = "";
        public string Author { get; set; } = "";
        public string Description { get; set; } = "";
        public List<string> Dependencies { get; set; } = new();
        public List<string> OptionalDependencies { get; set; } = new();
        public List<string> Conflicts { get; set; } = new();
        public List<string> Capabilities { get; set; } = new();
        public string File { get; set; } = "";
        public string Sha256 { get; set; } = "";
        public string InstalledAt { get; set; } = "";
        public string FileName => File;
    }

    private static Dictionary<string, RegistryRecord> ReadRegistry(string path)
    {
        var result = new Dictionary<string, RegistryRecord>(StringComparer.Ordinal);
        if (!File.Exists(path)) return result;
        try
        {
            using var document = JsonDocument.Parse(File.ReadAllText(path));
            if (!document.RootElement.TryGetProperty("mods", out var mods)) return result;
            foreach (var property in mods.EnumerateObject())
            {
                var record = property.Value.Deserialize<RegistryRecord>(new JsonSerializerOptions
                {
                    PropertyNameCaseInsensitive = true,
                });
                if (record is null) continue;
                if (string.IsNullOrEmpty(record.Id)) record.Id = property.Name;
                result[property.Name] = record;
                if (!result.ContainsKey(record.Id)) result[record.Id] = record;
            }
        }
        catch (JsonException)
        {
            // A corrupt registry must not hide installed binaries.
        }
        return result;
    }

    public static (string Id, string Op, string Version) SplitSpec(string spec)
    {
        var at = spec.IndexOf('@', StringComparison.Ordinal);
        if (at < 0) return (spec, "", "");
        var id = spec[..at];
        var rest = spec[(at + 1)..];
        foreach (var op in new[] { ">=", "<=", ">", "<", "=", "^", "~" })
        {
            if (!rest.StartsWith(op, StringComparison.Ordinal)) continue;
            return (id, op, rest[op.Length..]);
        }
        return (id, "=", rest);
    }

    private static (int Major, int Minor, int Patch, bool Valid) ParseVersion(string? version)
    {
        if (string.IsNullOrEmpty(version)) return (0, 0, 0, false);
        var text = version;
        var dash = text.IndexOfAny(new[] { '-', '+' });
        if (dash >= 0) text = text[..dash];
        var parts = text.Split('.');
        if (parts.Length is < 1 or > 3) return (0, 0, 0, false);
        var numbers = new int[3];
        for (var index = 0; index < parts.Length; index++)
            if (!int.TryParse(parts[index], out numbers[index])) return (0, 0, 0, false);
        return (numbers[0], numbers[1], numbers[2], true);
    }

    public static bool Satisfies(string op, string requiredVersion, string? actualVersion)
    {
        if (string.IsNullOrEmpty(op)) return true;
        var required = ParseVersion(requiredVersion);
        var actual = ParseVersion(actualVersion);
        if (!required.Valid || !actual.Valid) return false;
        var comparison =
            actual.Major != required.Major ? actual.Major.CompareTo(required.Major) :
            actual.Minor != required.Minor ? actual.Minor.CompareTo(required.Minor) :
            actual.Patch.CompareTo(required.Patch);
        return op switch
        {
            ">=" => comparison >= 0,
            "<=" => comparison <= 0,
            ">" => comparison > 0,
            "<" => comparison < 0,
            "=" => comparison == 0,
            "^" => comparison >= 0 && actual.Major == required.Major,
            "~" => comparison >= 0 && actual.Major == required.Major && actual.Minor == required.Minor,
            _ => false,
        };
    }
}
