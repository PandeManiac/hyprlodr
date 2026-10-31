using System.Text.Json;

namespace HyprLodr.Core;

public sealed record LoaderEvent(long Seq, string Event, string Mod, string Message);

/// <summary>Reads the loader's JSON Lines event log written inside the game directory.</summary>
public static class EventLogReader
{
    public static IReadOnlyList<LoaderEvent> Read(string path, int limit = 2000)
    {
        if (!File.Exists(path)) return Array.Empty<LoaderEvent>();
        var events = new List<LoaderEvent>();
        foreach (var line in File.ReadLines(path))
        {
            if (string.IsNullOrWhiteSpace(line)) continue;
            try
            {
                using var document = JsonDocument.Parse(line);
                var root = document.RootElement;
                events.Add(new LoaderEvent(
                    root.TryGetProperty("seq", out var seq) ? seq.GetInt64() : 0,
                    Text(root, "event"),
                    Text(root, "mod"),
                    Text(root, "message")));
            }
            catch (JsonException)
            {
                // A partially written final line is normal while the game is running.
            }
            if (events.Count >= limit) break;
        }
        return events;
    }

    private static string Text(JsonElement element, string property) =>
        element.TryGetProperty(property, out var value) && value.ValueKind == JsonValueKind.String
            ? value.GetString() ?? ""
            : "";
}
