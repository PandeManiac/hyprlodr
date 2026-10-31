using System.Text.Json;

namespace HyprLodr.Core;

/// <summary>
/// What the last in-game run left behind: the proxy's attach record (including
/// the build fingerprint) plus the loader's event log.
/// </summary>
public sealed record RunReport
{
    public bool AttachPresent { get; init; }
    public string? Stage { get; init; }
    public int? Pid { get; init; }
    public string? StatusText { get; init; }
    public bool? BuildMatches { get; init; }
    public int? AnchorsMatched { get; init; }
    public int? AnchorsTotal { get; init; }
    public string? ModuleBase { get; init; }
    public string? BuildTimestamp { get; init; }
    public int EventCount { get; init; }
    public int ModsInitialized { get; init; }
    public string? LastError { get; init; }

    public string Describe()
    {
        if (!AttachPresent)
            return "No run recorded yet. Install the loader and launch the game; evidence appears here.";

        var lines = new List<string>
        {
            $"Last run: {Stage ?? "unknown"} (pid {Pid?.ToString() ?? "?"}) — {StatusText ?? "no status"}",
        };

        if (BuildMatches is { } matches)
        {
            lines.Add(matches
                ? $"Build fingerprint: {AnchorsMatched}/{AnchorsTotal} anchors matched at {ModuleBase}."
                : $"Build fingerprint: MISMATCH ({AnchorsMatched}/{AnchorsTotal} anchors, timestamp {BuildTimestamp}). " +
                  "Mods may still load; address-dependent features must not.");
        }

        lines.Add($"{ModsInitialized} mod(s) initialized, {EventCount} event(s) recorded.");
        if (!string.IsNullOrEmpty(LastError)) lines.Add($"Last error: {LastError}");
        return string.Join("\n", lines);
    }
}

/// <summary>Parses the evidence files written inside the game directory.</summary>
public static class RunReportReader
{
    public static RunReport Read(string gamePath)
    {
        var events = EventLogReader.Read(Path.Combine(gamePath, LoaderInstaller.EventLog));
        var initialized = events.Count(evt => evt.Event == "initialize_ok");
        var lastError = events.LastOrDefault(evt => evt.Event == "error")?.Message;

        var baseReport = new RunReport
        {
            EventCount = events.Count,
            ModsInitialized = initialized,
            LastError = lastError,
        };

        var attachPath = Path.Combine(gamePath, LoaderInstaller.AttachRecord);
        if (!File.Exists(attachPath)) return baseReport;

        try
        {
            using var document = JsonDocument.Parse(File.ReadAllText(attachPath));
            var root = document.RootElement;
            return baseReport with
            {
                AttachPresent = true,
                Stage = Text(root, "stage"),
                Pid = Integer(root, "pid"),
                StatusText = Text(root, "status_text"),
                BuildMatches = Boolean(root, "build_matches"),
                AnchorsMatched = Integer(root, "anchors_matched"),
                AnchorsTotal = Integer(root, "anchors_total"),
                ModuleBase = Text(root, "module_base"),
                BuildTimestamp = Text(root, "build_timestamp"),
            };
        }
        catch (JsonException)
        {
            // A truncated record (process killed mid-write) must not hide the log.
            return baseReport;
        }
        catch (IOException)
        {
            return baseReport;
        }
    }

    private static string? Text(JsonElement element, string property) =>
        element.TryGetProperty(property, out var value) && value.ValueKind == JsonValueKind.String
            ? value.GetString()
            : null;

    private static int? Integer(JsonElement element, string property) =>
        element.TryGetProperty(property, out var value) && value.ValueKind == JsonValueKind.Number &&
        value.TryGetInt32(out var number)
            ? number
            : null;

    private static bool? Boolean(JsonElement element, string property) =>
        element.TryGetProperty(property, out var value) &&
        value.ValueKind is JsonValueKind.True or JsonValueKind.False
            ? value.GetBoolean()
            : null;
}
