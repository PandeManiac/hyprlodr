using System.IO.Compression;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace HyprLodr.Core;

public sealed class PackageInfo
{
    public required string Id { get; init; }
    public required string Name { get; init; }
    public required string Version { get; init; }
    public required string Author { get; init; }
    public string Description { get; init; } = "";
    public required string Platform { get; init; }
    public required string Entry { get; init; }
    public IReadOnlyList<string> Dependencies { get; init; } = Array.Empty<string>();
    public IReadOnlyList<string> Conflicts { get; init; } = Array.Empty<string>();
    public IReadOnlyList<string> Capabilities { get; init; } = Array.Empty<string>();
    public string? Path { get; init; }
}

public sealed record ImportResult(bool Ok, string Message, PackageInfo? Package);

/// <summary>
/// Read-only inspection and import of schema-1 mod packages. Mirrors the guards in
/// tools/hyprl_package.py; the native descriptor remains authoritative at runtime.
/// </summary>
public sealed partial class PackageInspector
{
    public const int Schema = 1;
    private static readonly string[] PlatformSuffixes = { "windows-x86_64", "linux-x86_64" };
    private static readonly string[] AllowedRootFiles = { "manifest.json", "README.md", "LICENSE" };
    private static readonly string[] AllowedPrefixes = { "native/", "assets/", "config/" };

    [GeneratedRegex("^[a-z0-9._-]{1,127}$")]
    private static partial Regex IdRegex();

    [GeneratedRegex("^\\d+(\\.\\d+){0,2}$")]
    private static partial Regex VersionRegex();

    public static (PackageInfo? Package, string? Error) Inspect(string packagePath)
    {
        if (!File.Exists(packagePath)) return (null, $"package not found: {packagePath}");
        try
        {
            using var archive = ZipFile.OpenRead(packagePath);
            var manifestEntry = archive.GetEntry("manifest.json");
            if (manifestEntry is null) return (null, "manifest.json missing from the archive root");

            var names = new List<string>();
            foreach (var entry in archive.Entries)
            {
                var error = CheckEntryName(entry.FullName);
                if (error is not null) return (null, error);
                names.Add(entry.FullName);
            }

            using var reader = new StreamReader(manifestEntry.Open());
            using var document = JsonDocument.Parse(reader.ReadToEnd());
            var root = document.RootElement;
            if (root.ValueKind != JsonValueKind.Object) return (null, "manifest.json must contain an object");

            string? Field(string name) =>
                root.TryGetProperty(name, out var value) && value.ValueKind == JsonValueKind.String
                    ? value.GetString()
                    : null;

            var id = Field("id");
            var name = Field("name");
            var version = Field("version");
            var author = Field("author");
            var platform = Field("platform");
            var entryPath = Field("entry");
            if (id is null || !IdRegex().IsMatch(id)) return (null, $"invalid mod id {id ?? "(missing)"}");
            if (name is null) return (null, "manifest is missing name");
            if (version is null || !VersionRegex().IsMatch(version)) return (null, $"invalid version {version}");
            if (author is null) return (null, "manifest is missing author");
            if (platform is null || !PlatformSuffixes.Contains(platform))
                return (null, $"unsupported platform {platform}");
            if (root.TryGetProperty("schema", out var schema) && schema.GetInt32() != Schema)
                return (null, $"unsupported manifest schema {schema.GetInt32()}");
            if (root.TryGetProperty("api", out var api) && api.GetInt32() < 1)
                return (null, $"invalid api {api.GetInt32()}");
            if (entryPath is null || !entryPath.StartsWith("native/", StringComparison.Ordinal))
                return (null, "entry must live under native/");
            var suffix = platform == "windows-x86_64" ? ".dll" : ".so";
            if (!entryPath.EndsWith(suffix, StringComparison.OrdinalIgnoreCase))
                return (null, $"entry {entryPath} does not match platform {platform}");
            if (!names.Contains(entryPath)) return (null, $"entry {entryPath} is not present in the archive");

            var package = new PackageInfo
            {
                Id = id,
                Name = name,
                Version = version,
                Author = author,
                Description = Field("description") ?? "",
                Platform = platform,
                Entry = entryPath,
                Dependencies = ReadStringArray(root, "dependencies"),
                Conflicts = ReadStringArray(root, "conflicts"),
                Capabilities = ReadStringArray(root, "capabilities"),
                Path = packagePath,
            };
            return (package, null);
        }
        catch (InvalidDataException)
        {
            return (null, "not a valid ZIP archive");
        }
        catch (JsonException)
        {
            return (null, "manifest.json is not valid JSON");
        }
    }

    public ImportResult Import(string packagePath, string modsDirectory, string? hostPath = null)
    {
        var (package, error) = Inspect(packagePath);
        if (package is null) return new ImportResult(false, error ?? "invalid package", null);

        try
        {
            using var archive = ZipFile.OpenRead(packagePath);
            var entry = archive.GetEntry(package.Entry)!;
            Directory.CreateDirectory(modsDirectory);
            var suffix = package.Platform == "windows-x86_64" ? ".dll" : ".so";
            var destination = Path.Combine(modsDirectory, package.Id + suffix);
            entry.ExtractToFile(destination, overwrite: true);

            if (hostPath is not null && File.Exists(hostPath))
            {
                var verification = DescriptorVerifier.Verify(hostPath, destination, package);
                if (!verification.Ok) return new ImportResult(false, verification.Message, package);
            }

            var registry = ModRegistryFile.Load(modsDirectory);
            registry.Upsert(package, destination);
            registry.Save(modsDirectory);
            return new ImportResult(true, $"imported {package.Id} {package.Version}", package);
        }
        catch (IOException failure)
        {
            return new ImportResult(false, $"import failed: {failure.Message}", package);
        }
    }

    private static string? CheckEntryName(string name)
    {
        if (string.IsNullOrEmpty(name) || name.EndsWith('/')) return $"directory entries are not allowed: {name}";
        if (name.Contains('\\')) return $"backslash separators are not allowed: {name}";
        if (name.StartsWith('/') || (name.Length > 1 && name[1] == ':')) return $"absolute paths are not allowed: {name}";
        var parts = name.Split('/');
        if (parts.Any(part => part is "" or "." or "..")) return $"unsafe path component in {name}";
        if (AllowedRootFiles.Contains(name)) return null;
        return AllowedPrefixes.Any(prefix => name.StartsWith(prefix, StringComparison.Ordinal))
            ? null
            : $"file not allowed in a mod package: {name}";
    }

    private static List<string> ReadStringArray(JsonElement root, string property)
    {
        if (!root.TryGetProperty(property, out var array) || array.ValueKind != JsonValueKind.Array)
            return new List<string>();
        return array.EnumerateArray()
            .Where(item => item.ValueKind == JsonValueKind.String)
            .Select(item => item.GetString() ?? "")
            .Where(item => item.Length > 0)
            .ToList();
    }
}

/// <summary>Registry file shared with tools/hyprl_package.py (mods/hyprlodr-mods.json).</summary>
public sealed class ModRegistryFile
{
    public Dictionary<string, Record> Mods { get; init; } = new(StringComparer.Ordinal);

    public sealed class Record
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
    }

    public static ModRegistryFile Load(string modsDirectory)
    {
        var path = Path.Combine(modsDirectory, ModCatalog.RegistryName);
        if (!File.Exists(path)) return new ModRegistryFile();
        try
        {
            var parsed = JsonSerializer.Deserialize<ModRegistryFile>(File.ReadAllText(path),
                new JsonSerializerOptions { PropertyNameCaseInsensitive = true });
            return parsed ?? new ModRegistryFile();
        }
        catch (JsonException)
        {
            return new ModRegistryFile();
        }
    }

    public void Upsert(PackageInfo package, string installedFile)
    {
        Mods[package.Id] = new Record
        {
            Id = package.Id,
            Name = package.Name,
            Version = package.Version,
            Author = package.Author,
            Description = package.Description,
            Dependencies = package.Dependencies.ToList(),
            Conflicts = package.Conflicts.ToList(),
            Capabilities = package.Capabilities.ToList(),
            File = Path.GetFileName(installedFile),
            Sha256 = LoaderInstaller.Sha256(installedFile),
            InstalledAt = DateTimeOffset.UtcNow.ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ"),
        };
    }

    public void Save(string modsDirectory)
    {
        Directory.CreateDirectory(modsDirectory);
        var path = Path.Combine(modsDirectory, ModCatalog.RegistryName);
        var payload = new Dictionary<string, object> { ["schema"] = 1, ["mods"] = Mods };
        File.WriteAllText(path, JsonSerializer.Serialize(payload,
            new JsonSerializerOptions { WriteIndented = true, PropertyNamingPolicy = JsonNamingPolicy.SnakeCaseLower }));
    }
}

/// <summary>
/// Reads the real ABI descriptor by running `hyprlodr-host --describe`, which loads
/// the library and calls the descriptor query only - never initialize.
/// </summary>
public static class DescriptorVerifier
{
    public static OperationResult Verify(string hostPath, string libraryPath, PackageInfo package)
    {
        var temporary = Path.Combine(Path.GetTempPath(), "hyprlodr-verify-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(temporary);
        try
        {
            var mods = Path.Combine(temporary, "mods");
            Directory.CreateDirectory(mods);
            var copy = Path.Combine(mods, Path.GetFileName(libraryPath));
            File.Copy(libraryPath, copy, overwrite: true);

            var startInfo = new System.Diagnostics.ProcessStartInfo
            {
                FileName = hostPath,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                UseShellExecute = false,
            };
            startInfo.ArgumentList.Add("--describe");
            startInfo.ArgumentList.Add("--log");
            startInfo.ArgumentList.Add(Path.Combine(temporary, "events.jsonl"));
            startInfo.ArgumentList.Add(mods);

            using var process = System.Diagnostics.Process.Start(startInfo);
            if (process is null) return OperationResult.Failure("could not start hyprlodr-host");
            var output = process.StandardOutput.ReadToEnd();
            process.WaitForExit(30000);
            var line = output.Split('\n').FirstOrDefault(text => text.TrimStart().StartsWith('{'));
            if (line is null) return OperationResult.Failure("descriptor query produced no output");

            using var document = JsonDocument.Parse(line);
            var modsArray = document.RootElement.GetProperty("mods");
            if (modsArray.GetArrayLength() != 1)
                return OperationResult.Failure("expected exactly one mod descriptor");
            var descriptor = modsArray[0];
            var id = descriptor.GetProperty("id").GetString();
            var version = descriptor.GetProperty("version").GetString();
            if (id != package.Id)
                return OperationResult.Failure($"manifest id '{package.Id}' does not match descriptor id '{id}'");
            if (version != package.Version)
                return OperationResult.Failure(
                    $"manifest version '{package.Version}' does not match descriptor version '{version}'");
            return OperationResult.Success("descriptor verified");
        }
        catch (JsonException)
        {
            return OperationResult.Failure("could not parse descriptor output");
        }
        finally
        {
            try { Directory.Delete(temporary, recursive: true); } catch (IOException) { }
        }
    }
}
