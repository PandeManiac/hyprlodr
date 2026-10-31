using System.Text.Json;
using System.Text.Json.Nodes;

namespace HyprLodr.Core;

/// <summary>
/// The proxy's start-up settings from <c>&lt;game&gt;/hyprlodr-config.json</c>.
///
/// Only <c>delay_ms</c> exists today: the proxy waits that long after attaching
/// before it resolves mods. It matters in practice — a machine whose game
/// process exits sooner than the delay never writes a single mod event, which
/// looks exactly like "the loader does not work".
/// </summary>
public sealed record LoaderConfig
{
    public const string FileName = "hyprlodr-config.json";
    public const int DefaultDelayMs = 2500;
    public const int MinDelayMs = 0;
    public const int MaxDelayMs = 60000;

    public int DelayMs { get; init; } = DefaultDelayMs;

    /// <summary>
    /// The file exists but cannot be used, so the proxy is silently running on
    /// the default. Surfaced instead of hidden: a typo here is invisible in the
    /// game otherwise.
    /// </summary>
    public bool Malformed { get; init; }

    public bool UsingDefault => DelayMs == DefaultDelayMs && !Malformed;

    public string Describe() => DelayMs == MinDelayMs
        ? $"delay_ms {DelayMs}: mods initialize as soon as the proxy attaches. " +
          "Use this when the game exits during start-up before the default delay elapses."
        : $"delay_ms {DelayMs}: mods wait {DelayMs} ms after attach before initializing.";

    public static string PathFor(string gamePath) => Path.Combine(gamePath, FileName);

    public static int Clamp(int delayMs) =>
        Math.Clamp(delayMs, MinDelayMs, MaxDelayMs);

    /// <summary>Read the file. Absent means the proxy's defaults; unreadable or
    /// wrongly typed means the same, but flagged so the UI can say why.</summary>
    public static LoaderConfig Load(string gamePath)
    {
        var path = PathFor(gamePath);
        if (!File.Exists(path)) return new LoaderConfig();

        try
        {
            using var document = JsonDocument.Parse(File.ReadAllText(path));
            if (document.RootElement.ValueKind != JsonValueKind.Object)
                return new LoaderConfig { Malformed = true };
            if (!document.RootElement.TryGetProperty("delay_ms", out var value))
                return new LoaderConfig();
            if (value.ValueKind != JsonValueKind.Number || !value.TryGetInt32(out var delayMs))
                return new LoaderConfig { Malformed = true };
            return new LoaderConfig { DelayMs = Clamp(delayMs) };
        }
        catch (JsonException)
        {
            return new LoaderConfig { Malformed = true };
        }
        catch (IOException)
        {
            return new LoaderConfig { Malformed = true };
        }
    }

    /// <summary>
    /// Write the delay back, preserving any other keys the file already holds so
    /// a hand-written setting is not silently dropped by the UI. An unreadable
    /// file is replaced: the proxy ignores it anyway, so keeping it would only
    /// preserve a broken state.
    /// </summary>
    public static LoaderConfig Save(string gamePath, int delayMs)
    {
        var path = PathFor(gamePath);
        var clamped = Clamp(delayMs);

        JsonObject root;
        try
        {
            root = File.Exists(path) && JsonNode.Parse(File.ReadAllText(path)) is JsonObject existing
                ? existing
                : new JsonObject();
        }
        catch (JsonException)
        {
            root = new JsonObject();
        }
        catch (IOException)
        {
            root = new JsonObject();
        }

        root["delay_ms"] = clamped;
        Directory.CreateDirectory(gamePath);
        File.WriteAllText(
            path,
            root.ToJsonString(new JsonSerializerOptions { WriteIndented = true }) + Environment.NewLine);
        return new LoaderConfig { DelayMs = clamped };
    }
}
