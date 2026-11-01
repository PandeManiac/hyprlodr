using System.Text.Json;

namespace HyprLodr.Core;

/// <summary>One record from <c>hyprlodr-files.jsonl</c>, written by the in-game file hook.</summary>
public sealed record FileOpenRecord
{
    public int Seq { get; init; }
    public long? ElapsedMs { get; init; }
    public int? Thread { get; init; }
    public string? Via { get; init; }
    public bool Redirected { get; init; }
    public bool Ok { get; init; }
    public string? Requested { get; init; }
    public string? Opened { get; init; }
}

/// <summary>
/// What the proxy's file hook observed during the last run. The counts are over
/// the whole log; <see cref="Records"/> keeps only the most recent entries so a
/// long session cannot exhaust memory in the UI.
/// </summary>
public sealed class FileLog
{
    public const int DefaultLimit = 200;

    public IReadOnlyList<FileOpenRecord> Records { get; init; } = Array.Empty<FileOpenRecord>();
    public int TotalOpens { get; init; }
    public int RedirectedOpens { get; init; }
    public int DistinctPaths { get; init; }
    public int Truncated { get; init; }
    public long? FirstElapsedMs { get; init; }

    public bool Any => TotalOpens > 0;

    public string Describe()
    {
        if (TotalOpens == 0)
            return "File hook: no opens recorded. Either the game did not open a file, or it opened one " +
                   "without going through CreateFileA, fopen/fopen_s or _wfopen in any loaded module.";

        var text = $"File hook: {TotalOpens} open(s) over {DistinctPaths} path(s), {RedirectedOpens} redirected";
        if (FirstElapsedMs is { } first) text += $", first at {first} ms";
        text += ".";
        if (Truncated > 0)
            text += $" Showing the last {Records.Count} of {TotalOpens} records.";
        return text;
    }
}

/// <summary>Parses the file hook's JSON Lines log. Never throws on bad input.</summary>
public static class FileLogReader
{
    public static string FileName => "hyprlodr-files.jsonl";

    public static FileLog Read(string gamePath, int limit = FileLog.DefaultLimit)
    {
        var path = Path.Combine(gamePath, FileName);
        if (!File.Exists(path)) return new FileLog();
        try
        {
            return Parse(File.ReadAllLines(path), limit);
        }
        catch (IOException)
        {
            return new FileLog();
        }
        catch (UnauthorizedAccessException)
        {
            return new FileLog();
        }
    }

    public static FileLog Parse(IEnumerable<string> lines, int limit = FileLog.DefaultLimit)
    {
        var records = new List<FileOpenRecord>();
        var paths = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var total = 0;
        var redirected = 0;
        long? firstElapsed = null;

        foreach (var line in lines)
        {
            if (string.IsNullOrWhiteSpace(line)) continue;
            try
            {
                using var document = JsonDocument.Parse(line);
                var root = document.RootElement;
                if (root.ValueKind != JsonValueKind.Object) continue;
                // Only the file-hook records matter; a log that mixes event kinds
                // must not inflate the counts.
                if (Text(root, "event") is { } kind && kind != "file_open") continue;

                var record = new FileOpenRecord
                {
                    Seq = Integer(root, "seq") ?? 0,
                    ElapsedMs = Long(root, "t_ms"),
                    Thread = Integer(root, "thread"),
                    Via = Text(root, "via"),
                    Redirected = Boolean(root, "redirected"),
                    Ok = Boolean(root, "ok"),
                    Requested = Text(root, "requested"),
                    Opened = Text(root, "opened"),
                };
                total++;
                if (record.Redirected) redirected++;
                if (!string.IsNullOrEmpty(record.Requested)) paths.Add(record.Requested);
                firstElapsed ??= record.ElapsedMs;
                records.Add(record);
            }
            catch (JsonException)
            {
                // A truncated final line (process killed mid-write) is expected.
            }
        }

        var shown = limit > 0 && records.Count > limit
            ? records.GetRange(records.Count - limit, limit)
            : records;

        return new FileLog
        {
            Records = shown,
            TotalOpens = total,
            RedirectedOpens = redirected,
            DistinctPaths = paths.Count,
            Truncated = records.Count - shown.Count,
            FirstElapsedMs = firstElapsed,
        };
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

    private static long? Long(JsonElement element, string property) =>
        element.TryGetProperty(property, out var value) && value.ValueKind == JsonValueKind.Number &&
        value.TryGetInt64(out var number)
            ? number
            : null;

    private static bool Boolean(JsonElement element, string property) =>
        element.TryGetProperty(property, out var value) &&
        value.ValueKind is JsonValueKind.True or JsonValueKind.False && value.GetBoolean();
}
