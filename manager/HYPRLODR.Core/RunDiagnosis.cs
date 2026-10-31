namespace HyprLodr.Core;

public enum DiagnosisSeverity
{
    Ok,
    Warning,
    Error,
}

public sealed record Diagnosis(DiagnosisSeverity Severity, string Text);

/// <summary>
/// Turns the evidence of the last run into one actionable sentence.
///
/// Every input here was learned the hard way: an installed loader with an empty
/// event log looks identical to a loader that does not work, and the actual
/// cause is usually the start-up delay outliving a short-lived game process.
/// The manager should say that, rather than making the user infer it from a
/// stage string.
/// </summary>
public static class RunDiagnosis
{
    public static Diagnosis Describe(RunReport report, LoaderConfig config, bool installed)
    {
        if (!installed)
            return new Diagnosis(DiagnosisSeverity.Warning,
                "The loader is not installed. Install it, then launch the game once.");

        if (!report.AttachPresent)
            return new Diagnosis(DiagnosisSeverity.Warning,
                "The loader is installed but no run is recorded yet. Launch the game once; " +
                "the attach record and event log appear here.");

        if (report.ModsInitialized > 0)
            return report.BuildMatches == false
                ? new Diagnosis(DiagnosisSeverity.Warning,
                    $"{report.ModsInitialized} mod(s) initialized, but this game build differs from the " +
                    $"analysed one ({report.AnchorsMatched}/{report.AnchorsTotal} anchors). Address-dependent " +
                    "features must stay disabled on this build.")
                : new Diagnosis(DiagnosisSeverity.Ok,
                    $"{report.ModsInitialized} mod(s) initialized in the last run " +
                    $"({report.EventCount} event(s) recorded).");

        if (!string.IsNullOrEmpty(report.LastError))
            return new Diagnosis(DiagnosisSeverity.Error,
                $"Mods did not start: {report.LastError}");

        if (report.Stage is "attached" or "initializing")
            return config.DelayMs > 0
                ? new Diagnosis(DiagnosisSeverity.Warning,
                    $"The game process ended during the {config.DelayMs} ms start-up delay, so no mod " +
                    "initialized. Set the delay to 0 and launch the game again.")
                : new Diagnosis(DiagnosisSeverity.Warning,
                    "The proxy attached but the game process ended before it could resolve mods, " +
                    "even with no delay. Check that the fingerprint matches and that mods are installed.");

        if (report.Stage == "mods_loaded")
            return new Diagnosis(DiagnosisSeverity.Warning,
                "The loader started but initialized no mods: none are enabled in the Mods tab, " +
                "or all of them failed to load.");

        return new Diagnosis(DiagnosisSeverity.Warning,
            $"The last run stopped at stage '{report.Stage ?? "unknown"}' with no mods initialized " +
            $"(status: {report.StatusText ?? "unknown"}).");
    }
}
