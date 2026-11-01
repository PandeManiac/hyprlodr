using System.Diagnostics;
using System.Globalization;
using Avalonia.Controls;
using Avalonia.Platform.Storage;
using HyprLodr.Core;

namespace HyprLodr.Manager;

public partial class MainWindow : Window
{
    private readonly LoaderInstaller _installer = new();
    private IReadOnlyList<GameInstall> _installs = Array.Empty<GameInstall>();
    private GameInstall? _install;
    private ModCatalog? _catalog;
    private IReadOnlyList<LoaderEvent> _events = Array.Empty<LoaderEvent>();
    private List<ModProfile> _profiles = new();
    private bool _removeArmed;

    public MainWindow()
    {
        InitializeComponent();

        ScanButton.Click += (_, _) => Scan(null);
        UsePathButton.Click += (_, _) => Scan(PathBox.Text);
        InstallButton.Click += (_, _) => InstallLoader();
        RemoveButton.Click += async (_, _) => await RemoveLoaderAsync();
        LaunchButton.Click += (_, _) => LaunchGame();
        SaveDelayButton.Click += (_, _) => SaveDelay();
        RefreshModsButton.Click += (_, _) => RefreshMods();
        ImportButton.Click += async (_, _) => await ImportPackageAsync();
        ModList.SelectionChanged += (_, _) => ShowSelectedMod();
        EnableButton.Click += (_, _) => SetModEnabled(true);
        DisableButton.Click += (_, _) => SetModEnabled(false);
        RemoveModButton.Click += (_, _) => RemoveSelectedMod();
        RefreshLogsButton.Click += (_, _) => RefreshLogs();
        LogFilterBox.TextChanged += (_, _) => RenderEvents();
        SaveProfileButton.Click += (_, _) => SaveProfile();
        ApplyProfileButton.Click += (_, _) => ApplyProfile();
        DeleteProfileButton.Click += (_, _) => DeleteProfile();
        ProfileBox.SelectionChanged += (_, _) =>
            ProfileNameBox.Text = ProfileBox.SelectedItem as string ?? ProfileNameBox.Text;

        var preset = Environment.GetEnvironmentVariable("HYPRLODR_GAME_DIR");
        Scan(string.IsNullOrWhiteSpace(preset) ? null : preset);
    }

    private string? ModsDirectory => _install is null ? null : Path.Combine(_install.GamePath, "mods");

    private void Scan(string? manualPath)
    {
        _removeArmed = false;
        _installs = string.IsNullOrWhiteSpace(manualPath)
            ? SteamLocator.Find()
            : SteamLocator.Find(manualPath);

        InstallList.ItemsSource = _installs
            .Select(install => $"{install.GamePath}  (build {install.BuildId ?? "?"})")
            .ToList();

        _install = _installs.FirstOrDefault();
        if (_install is not null)
        {
            InstallList.SelectedIndex = 0;
            PathBox.Text = _install.GamePath;
            SetStatus(_installs.Count > 1
                ? $"Found {_installs.Count} installations; using the first."
                : "Installation detected.");
        }
        else
        {
            SetStatus("HYPER DEMON not found. Enter the game folder and press \"Use this folder\".");
        }
        RenderGame();
        RefreshMods();
        RefreshLogs();
    }

    private void RenderGame()
    {
        if (_install is null)
        {
            GameSummary.Text = "No installation selected.";
            LoaderSummary.Text = "";
            DiagnosisText.Text = "";
            RunSummary.Text = "";
            DelayBox.Text = "";
            DelaySummary.Text = "";
            return;
        }

        var proton = _install.ProtonPrefix is null
            ? "native Windows (no Proton prefix)"
            : $"Proton prefix: {_install.ProtonPrefix}";
        GameSummary.Text =
            $"Game folder: {_install.GamePath}\n" +
            $"Steam library: {_install.LibraryPath}\n" +
            $"Steam build id: {_install.BuildId ?? "unknown"}\n" +
            $"Executable: {_install.ExecutablePath}\n" +
            $"{proton}\n" +
            $"Proton versions: {(_install.ProtonVersions.Count == 0 ? "none detected" : string.Join(", ", _install.ProtonVersions))}";

        var state = _installer.Inspect(_install.GamePath);
        LoaderSummary.Text = state.Installed
            ? $"Installed (loader {state.LoaderVersion ?? "?"}, {state.InstalledAt ?? "date unknown"}).\n" +
              $"{state.InstalledMods.Count} mods recorded in the install manifest.\n" +
              $"Integrity: {(state.HashesMatch ? "all recorded files match their hashes" : "CHANGED since installation")}.\n" +
              $"OpenAL32.dll sha256: {Short(state.ProxySha256)}\n" +
              $"Preserved original sha256: {Short(state.OriginalSha256)}"
            : "Not installed. Installing copies the original OpenAL32.dll to OpenAL32_real.dll, " +
              "then places the HYPRLODR proxy in its place.";

        // The file hook summary belongs beside the run summary: it is the only
        // evidence of what the game actually opened during that run. The newest
        // records are listed so the paths are visible, not just the counts.
        var fileLog = FileLogReader.Read(_install.GamePath);
        var summary = RunReportReader.Read(_install.GamePath).Describe() + "\n" + fileLog.Describe();
        if (fileLog.Any)
            summary += "\n" + string.Join("\n", fileLog.Recent(6));
        RunSummary.Text = summary;

        var config = LoaderConfig.Load(_install.GamePath);
        DelayBox.Text = config.DelayMs.ToString(CultureInfo.InvariantCulture);
        DelaySummary.Text = config.Malformed
            ? $"{LoaderConfig.FileName} is malformed, so the proxy is using the default {LoaderConfig.DefaultDelayMs} ms."
            : config.Describe();

        var report = RunReportReader.Read(_install.GamePath);
        var diagnosis = RunDiagnosis.Describe(report, config, state.Installed);
        DiagnosisText.Text = diagnosis.Text;
        DiagnosisText.Foreground = diagnosis.Severity switch
        {
            DiagnosisSeverity.Ok => Avalonia.Media.Brushes.LightGreen,
            DiagnosisSeverity.Warning => Avalonia.Media.Brushes.Goldenrod,
            _ => Avalonia.Media.Brushes.OrangeRed,
        };
    }

    private void SaveDelay()
    {
        if (_install is null) { SetStatus("Select a game folder first."); return; }
        var text = (DelayBox.Text ?? "").Trim();
        if (!int.TryParse(text, NumberStyles.Integer, CultureInfo.InvariantCulture, out var delayMs))
        {
            SetStatus("Enter the start-up delay in milliseconds (0-60000).");
            return;
        }

        var saved = LoaderConfig.Save(_install.GamePath, delayMs);
        RenderGame();
        SetStatus(delayMs == saved.DelayMs
            ? $"Mod start-up delay set to {saved.DelayMs} ms."
            : $"Delay clamped to {saved.DelayMs} ms; allowed range is {LoaderConfig.MinDelayMs}-{LoaderConfig.MaxDelayMs}.");
    }

    private static string Short(string? hash) =>
        string.IsNullOrEmpty(hash) ? "(absent)" : hash[..Math.Min(16, hash.Length)] + "…";

    private void InstallLoader()
    {
        if (_install is null) { SetStatus("Select a game folder first."); return; }
        var build = BuildBox.Text ?? "build-win";
        if (!Path.IsPathRooted(build))
            build = Path.GetFullPath(Path.Combine(Environment.CurrentDirectory, build));
        var result = _installer.Install(_install.GamePath, build);
        SetStatus(result.Message);
        RenderGame();
        RefreshMods();
    }

    private async Task RemoveLoaderAsync()
    {
        if (_install is null) { SetStatus("Select a game folder first."); return; }
        if (!_removeArmed)
        {
            _removeArmed = true;
            RemoveButton.Content = "Confirm removal";
            SetStatus("Press \"Confirm removal\" to restore the original library.");
            await Task.Delay(5000);
            _removeArmed = false;
            RemoveButton.Content = "Remove loader";
            return;
        }

        _removeArmed = false;
        RemoveButton.Content = "Remove loader";
        var result = _installer.Uninstall(_install.GamePath);
        SetStatus(result.Message);
        RenderGame();
        RefreshMods();
    }

    private void LaunchGame()
    {
        try
        {
            if (OperatingSystem.IsWindows())
            {
                Process.Start(new ProcessStartInfo($"steam://rungameid/{SteamLocator.AppId}")
                {
                    UseShellExecute = true,
                });
            }
            else
            {
                Process.Start(new ProcessStartInfo
                {
                    FileName = "steam",
                    ArgumentList = { "-applaunch", SteamLocator.AppId },
                });
            }
            SetStatus("Asked Steam to launch HYPER DEMON. Evidence appears in the Logs tab.");
        }
        catch (Exception error)
        {
            SetStatus($"Could not launch Steam: {error.Message}");
        }
    }

    private void RefreshMods()
    {
        var directory = ModsDirectory;
        if (directory is null)
        {
            _catalog = null;
            ModList.ItemsSource = new List<string> { "(no game selected)" };
            ModDetails.Text = "";
            LoadOrderText.Text = "";
            ProblemsText.Text = "";
            return;
        }

        _catalog = ModCatalog.Load(directory);
        var plan = _catalog.BuildPlan();
        var rows = _catalog.Mods.Select(mod =>
            $"{(mod.Enabled ? "●" : "○")} {mod.Id,-22} {mod.Version ?? "—",-8} {mod.Status}").ToList();
        if (rows.Count == 0) rows.Add("(no mods installed)");
        ModList.ItemsSource = rows;

        LoadOrderText.Text = plan.Order.Count == 0
            ? "(nothing will load)"
            : string.Join("  →  ", plan.Order);
        ProblemsText.Text = plan.Ok
            ? "No problems: every enabled mod can initialize."
            : string.Join("\n", plan.Problems.Select(problem => "• " + problem));
        ProblemsText.Foreground = plan.Ok
            ? Avalonia.Media.Brushes.LightGreen
            : Avalonia.Media.Brushes.OrangeRed;

        RefreshProfiles();
    }

    private void RefreshProfiles()
    {
        if (_install is null)
        {
            _profiles = new List<ModProfile>();
            ProfileBox.ItemsSource = Array.Empty<string>();
            ProfileStatusText.Text = "";
            return;
        }

        _profiles = ProfileStore.Load(_install.GamePath);
        var selected = ProfileBox.SelectedItem as string;
        ProfileBox.ItemsSource = _profiles.Select(profile => profile.Name).ToList();
        if (selected is not null && _profiles.Any(profile => profile.Name == selected))
            ProfileBox.SelectedItem = selected;
        ProfileStatusText.Text = _profiles.Count == 0
            ? "No profiles saved yet."
            : $"{_profiles.Count} profile(s) stored in {ProfileStore.FileName}.";
    }

    private void SaveProfile()
    {
        if (_install is null || _catalog is null) { SetStatus("Select a game folder first."); return; }
        var name = ProfileStore.NormaliseName(ProfileNameBox.Text ?? "");
        var profile = ProfileStore.Capture(name, _catalog);
        _profiles.RemoveAll(existing => existing.Name == name);
        _profiles.Add(profile);
        ProfileStore.Save(_install.GamePath, _profiles);
        ProfileNameBox.Text = name;
        RefreshProfiles();
        ProfileBox.SelectedItem = name;
        SetStatus($"Saved profile '{name}' with {profile.EnabledMods.Count} enabled mod(s).");
    }

    private void ApplyProfile()
    {
        if (_install is null || _catalog is null) { SetStatus("Select a game folder first."); return; }
        var name = ProfileBox.SelectedItem as string ?? ProfileNameBox.Text;
        var profile = _profiles.FirstOrDefault(candidate => candidate.Name == name);
        if (profile is null) { SetStatus("Select a saved profile to apply."); return; }

        var missing = ProfileStore.Apply(profile, _catalog, Path.Combine(_install.GamePath, "mods"));
        RefreshMods();
        SetStatus(missing.Count == 0
            ? $"Applied profile '{profile.Name}'."
            : $"Applied profile '{profile.Name}'; not installed: {string.Join(", ", missing)}.");
    }

    private void DeleteProfile()
    {
        if (_install is null) { SetStatus("Select a game folder first."); return; }
        var name = ProfileBox.SelectedItem as string ?? ProfileNameBox.Text;
        if (string.IsNullOrWhiteSpace(name) || _profiles.All(profile => profile.Name != name))
        {
            SetStatus("Select a saved profile to delete.");
            return;
        }
        _profiles.RemoveAll(profile => profile.Name == name);
        ProfileStore.Save(_install.GamePath, _profiles);
        RefreshProfiles();
        SetStatus($"Deleted profile '{name}'.");
    }

    private ModEntry? SelectedMod()
    {
        if (_catalog is null) return null;
        var index = ModList.SelectedIndex;
        if (index < 0 || index >= _catalog.Mods.Count) return null;
        return _catalog.Mods[index];
    }

    private void ShowSelectedMod()
    {
        var mod = SelectedMod();
        if (mod is null) { ModDetails.Text = ""; return; }
        var dependencies = mod.Dependencies.Count == 0 ? "none" : string.Join(", ", mod.Dependencies);
        var conflicts = mod.Conflicts.Count == 0 ? "none" : string.Join(", ", mod.Conflicts);
        ModDetails.Text =
            $"{mod.Id} {mod.Version}\n" +
            $"{mod.Name ?? "(no display name)"} by {mod.Author ?? "unknown"}\n\n" +
            $"{mod.Description}\n\n" +
            $"File: {mod.FileName}\n" +
            $"State: {(mod.Enabled ? "enabled" : "disabled")} ({mod.Status})\n" +
            $"Dependencies: {dependencies}\n" +
            $"Conflicts: {conflicts}\n" +
            $"Installed: {mod.InstalledAt ?? "unknown"}\n" +
            $"sha256: {Short(mod.Sha256)}";
    }

    private void SetModEnabled(bool enabled)
    {
        var mod = SelectedMod();
        var directory = ModsDirectory;
        if (mod is null || directory is null) { SetStatus("Select a mod first."); return; }
        var result = ModInstaller.SetEnabled(directory, mod.Id, enabled);
        SetStatus(result.Message);
        RefreshMods();
    }

    private void RemoveSelectedMod()
    {
        var mod = SelectedMod();
        var directory = ModsDirectory;
        if (mod is null || directory is null) { SetStatus("Select a mod first."); return; }
        var result = ModInstaller.Remove(directory, mod.Id);
        SetStatus(result.Message);
        RefreshMods();
    }

    private async Task ImportPackageAsync()
    {
        var directory = ModsDirectory;
        if (directory is null) { SetStatus("Select a game folder first."); return; }

        var files = await StorageProvider.OpenFilePickerAsync(new FilePickerOpenOptions
        {
            Title = "Import HYPRLODR mod package",
            AllowMultiple = false,
            FileTypeFilter = new[]
            {
                new FilePickerFileType("HYPRLODR package") { Patterns = new[] { "*.zip" } },
            },
        });
        var file = files.FirstOrDefault();
        if (file is null) return;

        var inspector = new PackageInspector();
        var result = inspector.Import(file.Path.LocalPath, directory);
        SetStatus(result.Message);
        RefreshMods();
    }

    private void RefreshLogs()
    {
        if (_install is null)
        {
            _events = Array.Empty<LoaderEvent>();
        }
        else
        {
            _events = EventLogReader.Read(Path.Combine(_install.GamePath, LoaderInstaller.EventLog));
        }
        RenderEvents();
    }

    private void RenderEvents()
    {
        var filter = LogFilterBox.Text ?? "";
        var filtered = string.IsNullOrWhiteSpace(filter)
            ? _events
            : _events.Where(evt =>
                evt.Event.Contains(filter, StringComparison.OrdinalIgnoreCase) ||
                evt.Mod.Contains(filter, StringComparison.OrdinalIgnoreCase) ||
                evt.Message.Contains(filter, StringComparison.OrdinalIgnoreCase)).ToList();

        EventList.ItemsSource = filtered
            .Select(evt => $"{evt.Seq,4}  {evt.Event,-22} {evt.Mod,-16} {evt.Message}")
            .ToList();
        LogSummary.Text = _install is null
            ? "No game selected"
            : $"{filtered.Count} of {_events.Count} events";
    }

    private void SetStatus(string message) => StatusText.Text = message;
}
