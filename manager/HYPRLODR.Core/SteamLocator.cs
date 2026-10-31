using System.Text.RegularExpressions;

namespace HyprLodr.Core;

/// <summary>A HYPER DEMON installation discovered through Steam (or entered manually).</summary>
public sealed class GameInstall
{
    public required string GamePath { get; init; }
    public required string LibraryPath { get; init; }
    public string? BuildId { get; init; }
    public string? Name { get; init; }
    public string? ProtonPrefix { get; init; }
    public IReadOnlyList<string> ProtonVersions { get; init; } = Array.Empty<string>();
    public string ExecutablePath => Path.Combine(GamePath, SteamLocator.Executable);
}

/// <summary>
/// Locates Steam libraries and the HYPER DEMON installation. Mirrors the rules of
/// tools/hyprl_diagnose.py: no hardcoded single path, multiple libraries, manual override.
/// </summary>
public static partial class SteamLocator
{
    public const string AppId = "1743850";
    public const string Executable = "hyperdemon.exe";

    [GeneratedRegex("\"path\"\\s+\"([^\"]+)\"")]
    private static partial Regex LibraryPathRegex();

    [GeneratedRegex("^\\s*\"\\d+\"\\s+\"([^\"]+)\"\\s*$", RegexOptions.Multiline)]
    private static partial Regex LegacyLibraryPathRegex();

    public static IReadOnlyList<string> SteamRoots()
    {
        var home = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
        var candidates = new List<string>();
        if (OperatingSystem.IsWindows())
        {
            var programFilesX86 = Environment.GetEnvironmentVariable("PROGRAMFILES(X86)");
            var programFiles = Environment.GetEnvironmentVariable("PROGRAMFILES");
            if (!string.IsNullOrEmpty(programFilesX86)) candidates.Add(Path.Combine(programFilesX86, "Steam"));
            if (!string.IsNullOrEmpty(programFiles)) candidates.Add(Path.Combine(programFiles, "Steam"));
        }
        else
        {
            candidates.Add(Path.Combine(home, ".steam", "steam"));
            candidates.Add(Path.Combine(home, ".steam", "root"));
            candidates.Add(Path.Combine(home, ".local", "share", "Steam"));
            candidates.Add(Path.Combine(home, ".var", "app", "com.valvesoftware.Steam", ".local", "share", "Steam"));
        }

        var roots = new List<string>();
        foreach (var candidate in candidates)
        {
            if (!Directory.Exists(candidate)) continue;
            var full = Path.GetFullPath(candidate);
            if (!roots.Contains(full)) roots.Add(full);
        }
        return roots;
    }

    public static IReadOnlyList<string> Libraries(string root)
    {
        var libraries = new List<string> { Path.GetFullPath(root) };
        foreach (var vdf in new[]
                 {
                     Path.Combine(root, "steamapps", "libraryfolders.vdf"),
                     Path.Combine(root, "config", "libraryfolders.vdf"),
                 })
        {
            if (!File.Exists(vdf)) continue;
            string text;
            try { text = File.ReadAllText(vdf); }
            catch (IOException) { continue; }

            foreach (Match match in LibraryPathRegex().Matches(text))
                AddLibrary(libraries, match.Groups[1].Value.Replace("\\\\", "\\"));
            foreach (Match match in LegacyLibraryPathRegex().Matches(text))
                AddLibrary(libraries, match.Groups[1].Value.Replace("\\\\", "\\"));
        }
        return libraries;
    }

    private static void AddLibrary(List<string> libraries, string candidate)
    {
        if (string.IsNullOrWhiteSpace(candidate)) return;
        if (!Directory.Exists(Path.Combine(candidate, "steamapps"))) return;
        var full = Path.GetFullPath(candidate);
        if (!libraries.Contains(full)) libraries.Add(full);
    }

    public static IReadOnlyList<GameInstall> Find(string? manualPath = null)
    {
        if (!string.IsNullOrWhiteSpace(manualPath))
        {
            var game = Path.GetFullPath(manualPath);
            if (!File.Exists(Path.Combine(game, Executable))) return Array.Empty<GameInstall>();
            var library = Directory.GetParent(game)?.Parent?.Parent?.FullName ?? game;
            return new[] { Build(game, library, new Dictionary<string, string>()) };
        }
        return FindInRoots(SteamRoots());
    }

    /// <summary>Discovery against explicit Steam roots, used by tests and portable installs.</summary>
    public static IReadOnlyList<GameInstall> FindInRoots(IEnumerable<string> roots)
    {
        var found = new List<GameInstall>();
        foreach (var root in roots)
        foreach (var library in Libraries(root))
        {
            var manifest = Path.Combine(library, "steamapps", $"appmanifest_{AppId}.acf");
            var info = ParseAcf(manifest);
            var installDir = info.TryGetValue("installdir", out var dir) && dir.Length > 0 ? dir : "hyperdemon";
            var game = Path.Combine(library, "steamapps", "common", installDir);
            if (!File.Exists(Path.Combine(game, Executable))) continue;
            if (found.Any(existing => existing.GamePath == Path.GetFullPath(game))) continue;
            found.Add(Build(game, library, info));
        }
        return found;
    }

    private static GameInstall Build(string game, string library, Dictionary<string, string> info)
    {
        var compatdata = Path.Combine(library, "steamapps", "compatdata", AppId);
        var prefix = Path.Combine(compatdata, "pfx");
        var common = Path.Combine(library, "steamapps", "common");
        var protons = Directory.Exists(common)
            ? Directory.GetDirectories(common)
                .Select(Path.GetFileName)
                .Where(name => name is not null && name.StartsWith("Proton", StringComparison.OrdinalIgnoreCase))
                .Select(name => name!)
                .OrderBy(name => name, StringComparer.Ordinal)
                .ToArray()
            : Array.Empty<string>();

        return new GameInstall
        {
            GamePath = Path.GetFullPath(game),
            LibraryPath = Path.GetFullPath(library),
            BuildId = info.TryGetValue("buildid", out var buildId) ? buildId : null,
            Name = info.TryGetValue("name", out var name) ? name : null,
            ProtonPrefix = Directory.Exists(prefix) ? prefix : null,
            ProtonVersions = protons,
        };
    }

    public static Dictionary<string, string> ParseAcf(string path)
    {
        var values = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        if (!File.Exists(path)) return values;
        string text;
        try { text = File.ReadAllText(path); }
        catch (IOException) { return values; }
        foreach (Match match in AcfRegex().Matches(text))
            values.TryAdd(match.Groups[1].Value, match.Groups[2].Value);
        return values;
    }

    [GeneratedRegex("\"([A-Za-z0-9_]+)\"\\s+\"([^\"]*)\"")]
    private static partial Regex AcfRegex();
}
