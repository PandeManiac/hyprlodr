namespace HyprLodr.Core;

/// <summary>
/// Enable, disable and remove installed mods without touching the game itself.
/// Enabling state is persisted in mods/disabled.txt, which the native loader reads.
/// </summary>
public static class ModInstaller
{
    public static OperationResult SetEnabled(string modsDirectory, string modId, bool enabled)
    {
        var catalog = ModCatalog.Load(modsDirectory);
        var disabled = catalog.Mods.Where(mod => !mod.Enabled)
            .Select(mod => mod.Id)
            .ToHashSet(StringComparer.Ordinal);
        if (enabled) disabled.Remove(modId);
        else disabled.Add(modId);
        ModCatalog.SaveDisabled(modsDirectory, disabled);
        return OperationResult.Success($"{modId} {(enabled ? "enabled" : "disabled")}");
    }

    public static OperationResult Remove(string modsDirectory, string modId)
    {
        var registry = ModRegistryFile.Load(modsDirectory);
        registry.Mods.TryGetValue(modId, out var record);
        var fileName = record?.File;
        if (string.IsNullOrEmpty(fileName))
        {
            var guess = Directory.EnumerateFiles(modsDirectory)
                .FirstOrDefault(path => Path.GetFileNameWithoutExtension(path) == modId);
            fileName = guess is null ? null : Path.GetFileName(guess);
        }

        if (fileName is not null)
        {
            var path = Path.Combine(modsDirectory, fileName);
            if (File.Exists(path))
            {
                if (record is { Sha256.Length: > 0 } && LoaderInstaller.Sha256(path) != record.Sha256)
                    return OperationResult.Failure(
                        $"{fileName} changed since installation; refusing to delete it");
                File.Delete(path);
            }
        }

        if (record is not null)
        {
            registry.Mods.Remove(modId);
            registry.Save(modsDirectory);
        }
        return OperationResult.Success($"removed {modId}");
    }
}
