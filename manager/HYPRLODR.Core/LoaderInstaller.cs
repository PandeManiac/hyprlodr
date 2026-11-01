using System.Security.Cryptography;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace HyprLodr.Core;

public sealed record OperationResult(bool Ok, string Message)
{
    public static OperationResult Success(string message) => new(true, message);
    public static OperationResult Failure(string message) => new(false, message);
}

public sealed class LoaderState
{
    public bool GamePresent { get; init; }
    public bool ProxyInstalled { get; init; }
    public bool OriginalPreserved { get; init; }
    public bool ManifestPresent { get; init; }
    public bool ModsDirectory { get; init; }
    public bool HashesMatch { get; init; }
    public string? InstalledAt { get; init; }
    public string? LoaderVersion { get; init; }
    public IReadOnlyList<string> InstalledMods { get; init; } = Array.Empty<string>();
    public string? ProxySha256 { get; init; }
    public string? OriginalSha256 { get; init; }

    public bool Installed => ProxyInstalled && OriginalPreserved;
}

/// <summary>
/// Installs and removes the loader, mirroring tools/hyprl_install.py: the shipped
/// library is preserved under a second name, every written file is hashed into a
/// manifest, and removal refuses to touch files that changed underneath it.
/// </summary>
public sealed class LoaderInstaller
{
    public const string ProxyName = "OpenAL32.dll";
    public const string OriginalName = "OpenAL32_real.dll";
    public const string ManifestName = "hyprlodr-install-manifest.json";
    public const string ModsDirectoryName = "mods";
    /// <summary>Mod files that existed before installation, kept for uninstall.</summary>
    public const string BackupDirectoryName = ".hyprlodr-backup";
    public const string AttachRecord = "hyprlodr-attach.json";
    public const string EventLog = "hyprlodr-events.jsonl";

    private static readonly JsonSerializerOptions WriteOptions = new()
    {
        WriteIndented = true,
        DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull,
    };

    public static string Sha256(string path)
    {
        using var stream = File.OpenRead(path);
        return Convert.ToHexStringLower(SHA256.HashData(stream));
    }

    public LoaderState Inspect(string gamePath)
    {
        var proxyPath = Path.Combine(gamePath, ProxyName);
        var originalPath = Path.Combine(gamePath, OriginalName);
        var manifestPath = Path.Combine(gamePath, ManifestName);
        var modsDir = Path.Combine(gamePath, ModsDirectoryName);
        var manifest = ReadManifest(manifestPath);

        var proxyHash = File.Exists(proxyPath) ? Sha256(proxyPath) : null;
        var originalHash = File.Exists(originalPath) ? Sha256(originalPath) : null;
        var hashesMatch = manifest is null || manifest.Files.All(pair =>
            !File.Exists(Path.Combine(gamePath, pair.Key)) ||
            Sha256(Path.Combine(gamePath, pair.Key)) == pair.Value.InstalledSha256);

        return new LoaderState
        {
            GamePresent = File.Exists(Path.Combine(gamePath, SteamLocator.Executable)),
            ProxyInstalled = proxyHash is not null,
            OriginalPreserved = originalHash is not null,
            ManifestPresent = manifest is not null,
            ModsDirectory = Directory.Exists(modsDir),
            HashesMatch = hashesMatch,
            InstalledAt = manifest?.InstalledAt,
            LoaderVersion = manifest?.LoaderVersion,
            InstalledMods = manifest?.Mods ?? new List<string>(),
            ProxySha256 = proxyHash,
            OriginalSha256 = originalHash,
        };
    }

    public OperationResult Install(string gamePath, string buildDirectory, string loaderVersion = "0.2.0")
    {
        var proxySource = Path.Combine(buildDirectory, ProxyName);
        if (!File.Exists(proxySource))
            return OperationResult.Failure($"proxy not found: {proxySource}. Build it with tools/build_windows.sh");
        if (!File.Exists(Path.Combine(gamePath, SteamLocator.Executable)))
            return OperationResult.Failure($"{gamePath} does not contain {SteamLocator.Executable}");
        if (File.Exists(Path.Combine(gamePath, OriginalName)))
            return OperationResult.Failure(
                $"{OriginalName} already exists: the loader is installed, or a previous installation was interrupted");

        var proxyPath = Path.Combine(gamePath, ProxyName);
        var originalPath = Path.Combine(gamePath, OriginalName);
        var files = new Dictionary<string, ManifestFile>(StringComparer.Ordinal);
        var backups = new Dictionary<string, string>(StringComparer.Ordinal);
        var installedMods = new List<string>();
        var modsTarget = Path.Combine(gamePath, ModsDirectoryName);
        var createdModsDirectory = !Directory.Exists(modsTarget);
        var originalCreated = false;
        var manifestWritten = false;

        // Every change is undone on failure: the game must never be left with a
        // half-installed loader, and a mod the user already had must survive.
        void Rollback()
        {
            if (originalCreated && File.Exists(originalPath))
            {
                if (File.Exists(proxyPath)) File.Delete(proxyPath);
                File.Move(originalPath, proxyPath);
            }
            foreach (var name in installedMods)
            {
                var path = Path.Combine(modsTarget, name);
                if (File.Exists(path)) File.Delete(path);
            }
            RestoreBackups(gamePath, backups);
            if (manifestWritten)
            {
                var path = Path.Combine(gamePath, ManifestName);
                if (File.Exists(path)) File.Delete(path);
            }
            if (createdModsDirectory && Directory.Exists(modsTarget) &&
                !Directory.EnumerateFileSystemEntries(modsTarget).Any())
                Directory.Delete(modsTarget);
        }

        try
        {
            var originalHash = Sha256(proxyPath);
            File.Copy(proxyPath, originalPath, overwrite: false);
            originalCreated = true;
            if (Sha256(originalPath) != originalHash)
                throw new IOException("backup verification failed");
            files[OriginalName] = new ManifestFile(originalHash, originalHash, "original library, preserved verbatim");

            File.Copy(proxySource, proxyPath, overwrite: true);
            var proxyHash = Sha256(proxyPath);
            files[ProxyName] = new ManifestFile(proxyHash, proxyHash, "HYPRLODR proxy DLL");

            var modsSource = Path.Combine(buildDirectory, ModsDirectoryName);
            if (Directory.Exists(modsSource))
            {
                Directory.CreateDirectory(modsTarget);
                foreach (var source in Directory.GetFiles(modsSource, "*.dll").OrderBy(p => p, StringComparer.Ordinal))
                {
                    var name = Path.GetFileName(source);
                    var destination = Path.Combine(modsTarget, name);
                    if (File.Exists(destination) && !backups.ContainsKey($"{ModsDirectoryName}/{name}"))
                    {
                        var backupRoot = Path.Combine(modsTarget, BackupDirectoryName);
                        Directory.CreateDirectory(backupRoot);
                        File.Copy(destination, Path.Combine(backupRoot, name), overwrite: false);
                        backups[$"{ModsDirectoryName}/{name}"] =
                            $"{ModsDirectoryName}/{BackupDirectoryName}/{name}";
                    }
                    File.Copy(source, destination, overwrite: true);
                    installedMods.Add(name);
                    var installedHash = Sha256(destination);
                    files[$"{ModsDirectoryName}/{name}"] =
                        new ManifestFile(installedHash, installedHash, null);
                }
            }

            var manifest = new InstallManifest
            {
                LoaderVersion = loaderVersion,
                InstalledAt = DateTimeOffset.UtcNow.ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ"),
                GameDir = gamePath,
                Files = files,
                Mods = installedMods,
                Backups = backups,
            };
            File.WriteAllText(Path.Combine(gamePath, ManifestName),
                JsonSerializer.Serialize(manifest, WriteOptions));
            manifestWritten = true;
            var preserved = backups.Count == 0
                ? ""
                : $"; preserved {backups.Count} existing mod file(s)";
            return OperationResult.Success($"installed {installedMods.Count} mod(s) into {gamePath}{preserved}");
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        {
            try
            {
                Rollback();
            }
            catch (Exception rollbackError) when (rollbackError is IOException or UnauthorizedAccessException)
            {
                return OperationResult.Failure(
                    $"installation failed ({error.Message}) and rollback failed ({rollbackError.Message}); " +
                    "verify game files through Steam");
            }
            return OperationResult.Failure($"installation failed: {error.Message}; every change was rolled back");
        }
    }

    /// <summary>Put mod files that predated the installation back where they were.</summary>
    private static void RestoreBackups(string gamePath, IReadOnlyDictionary<string, string> backups)
    {
        foreach (var (targetRelative, backupRelative) in backups)
        {
            var target = Path.Combine(gamePath, targetRelative);
            var backup = Path.Combine(gamePath, backupRelative);
            if (!File.Exists(backup)) continue;
            Directory.CreateDirectory(Path.GetDirectoryName(target)!);
            File.Copy(backup, target, overwrite: true);
            File.Delete(backup);
        }
        var backupRoot = Path.Combine(gamePath, ModsDirectoryName, BackupDirectoryName);
        if (Directory.Exists(backupRoot) && !Directory.EnumerateFileSystemEntries(backupRoot).Any())
            Directory.Delete(backupRoot);
    }

    public OperationResult Uninstall(string gamePath)
    {
        var proxyPath = Path.Combine(gamePath, ProxyName);
        var originalPath = Path.Combine(gamePath, OriginalName);
        var manifestPath = Path.Combine(gamePath, ManifestName);
        var manifest = ReadManifest(manifestPath);

        if (manifest is null && !File.Exists(originalPath))
            return OperationResult.Failure("no HYPRLODR installation detected");

        if (manifest is not null && manifest.Files.TryGetValue(ProxyName, out var entry) &&
            File.Exists(proxyPath) && Sha256(proxyPath) != entry.InstalledSha256)
        {
            return OperationResult.Failure(
                $"{ProxyName} changed since installation; refusing to remove it. " +
                "Verify game files through Steam instead.");
        }

        try
        {
            if (File.Exists(originalPath))
            {
                File.Delete(proxyPath);
                File.Move(originalPath, proxyPath);
            }
            var kept = new List<string>();
            foreach (var mod in manifest?.Mods ?? new List<string>())
            {
                var name = Path.GetFileName(mod);
                var path = Path.Combine(gamePath, ModsDirectoryName, name);
                if (!File.Exists(path)) continue;
                // Only remove a mod whose bytes are still the ones we installed.
                if (manifest!.Files.TryGetValue($"{ModsDirectoryName}/{name}", out var modRecord) &&
                    Sha256(path) != modRecord.InstalledSha256)
                {
                    kept.Add(name);
                    continue;
                }
                File.Delete(path);
            }
            RestoreBackups(gamePath, manifest?.Backups ?? new Dictionary<string, string>());
            var modsDir = Path.Combine(gamePath, ModsDirectoryName);
            if (Directory.Exists(modsDir) && !Directory.EnumerateFileSystemEntries(modsDir).Any())
                Directory.Delete(modsDir);
            foreach (var stale in new[] { AttachRecord, EventLog })
            {
                var path = Path.Combine(gamePath, stale);
                if (File.Exists(path)) File.Delete(path);
            }
            if (File.Exists(manifestPath)) File.Delete(manifestPath);
            var note = kept.Count == 0
                ? ""
                : $"; kept {kept.Count} mod file(s) that changed since installation";
            return OperationResult.Success("removed the loader and restored the original library" + note);
        }
        catch (IOException error)
        {
            return OperationResult.Failure($"removal failed: {error.Message}");
        }
        catch (UnauthorizedAccessException error)
        {
            return OperationResult.Failure($"removal failed: {error.Message}");
        }
    }

    public static InstallManifest? ReadManifest(string path)
    {
        if (!File.Exists(path)) return null;
        try
        {
            return JsonSerializer.Deserialize<InstallManifest>(File.ReadAllText(path));
        }
        catch (Exception error) when (error is JsonException or IOException or NotSupportedException)
        {
            return null;
        }
    }

    public sealed class InstallManifest
    {
        [JsonPropertyName("loader_version")] public string LoaderVersion { get; set; } = "";
        [JsonPropertyName("installed_at")] public string InstalledAt { get; set; } = "";
        [JsonPropertyName("game_dir")] public string GameDir { get; set; } = "";
        [JsonPropertyName("files")] public Dictionary<string, ManifestFile> Files { get; set; } = new();
        [JsonPropertyName("mods")] public List<string> Mods { get; set; } = new();
        [JsonPropertyName("backups")] public Dictionary<string, string> Backups { get; set; } = new();
    }

    public sealed record ManifestFile(
        [property: JsonPropertyName("sha256")] string Sha256,
        [property: JsonPropertyName("installed_sha256")] string InstalledSha256,
        [property: JsonPropertyName("role")] string? Role);
}
