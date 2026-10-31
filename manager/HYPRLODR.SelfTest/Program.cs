using System.IO.Compression;
using System.Text;
using System.Text.Json;
using HyprLodr.Core;

namespace HyprLodr.SelfTest;

/// <summary>
/// Headless verification of the manager's core services. Runs without a display so
/// it can be wired into ctest alongside the native loader suites.
///
/// Usage: HYPRLODR.SelfTest [optional-sample-mod-path]
/// </summary>
internal static class Program
{
    private static readonly List<(string Name, Action Body)> Scenarios = new();
    private static int _failures;

    private static int Main(string[] args)
    {
        var sample = args.Length > 0 ? args[0] : null;
        Register("steam_discovery", () => SteamDiscovery(sample));
        Register("discovery_manual_path", () => DiscoveryManualPath(sample));
        Register("loader_install_and_inspect", () => LoaderInstall(sample));
        Register("loader_refuses_double_install", () => LoaderDoubleInstall(sample));
        Register("loader_uninstall_restores_original", () => LoaderUninstall(sample));
        Register("loader_refuses_changed_proxy", () => LoaderChangedProxy(sample));
        Register("catalog_reads_registry_and_disabled", () => CatalogRead(sample));
        Register("catalog_order_matches_loader", () => CatalogOrder(sample));
        Register("catalog_detects_missing_dependency", () => CatalogMissingDependency(sample));
        Register("catalog_detects_cycle", () => CatalogCycle(sample));
        Register("catalog_detects_version_mismatch", () => CatalogVersionMismatch(sample));
        Register("catalog_blocked_dependency_is_not_a_cycle", () => CatalogBlockedNotCycle(sample));
        Register("version_constraints", VersionConstraints);
        Register("event_log_reader", EventLog);
        Register("package_validation", PackageValidation);
        Register("package_import_and_registry", () => PackageImport(sample));
        Register("profiles_round_trip", ProfilesRoundTrip);
        Register("profiles_apply_rewrites_disabled_list", ProfilesApply);
        Register("profiles_tolerate_corrupt_file", ProfilesCorrupt);

        foreach (var (name, body) in Scenarios)
        {
            try
            {
                body();
                Console.WriteLine($"PASS {name}");
            }
            catch (Exception error)
            {
                _failures++;
                Console.WriteLine($"FAIL {name}: {error.GetType().Name}: {error.Message}");
            }
        }

        Console.WriteLine($"\n{Scenarios.Count - _failures}/{Scenarios.Count} manager scenarios passed");
        return _failures == 0 ? 0 : 1;
    }

    private static void Register(string name, Action body) => Scenarios.Add((name, body));

    // ---------------------------------------------------------------- profiles

    private static (string Root, string Mods, ModCatalog Catalog) ProfileFixture()
    {
        var root = NewTemp();
        var mods = Path.Combine(root, "mods");
        Directory.CreateDirectory(mods);
        foreach (var id in new[] { "a.one", "b.two" })
            File.WriteAllBytes(Path.Combine(mods, id + ".dll"), Encoding.ASCII.GetBytes("mod " + id));
        WriteRegistry(mods, ("a.one", "1.0.0", Array.Empty<string>()),
                            ("b.two", "1.0.0", Array.Empty<string>()));
        return (root, mods, ModCatalog.Load(mods));
    }

    private static void ProfilesRoundTrip()
    {
        var (root, _, catalog) = ProfileFixture();
        catalog.SetEnabled("b.two", false);

        var captured = ProfileStore.Capture("  balanced  ", catalog);
        Check(captured.Name == "balanced", $"name not normalised: '{captured.Name}'");
        Check(captured.EnabledMods.SequenceEqual(new[] { "a.one" }),
              $"capture recorded {string.Join(',', captured.EnabledMods)}");

        ProfileStore.Save(root, new[] { captured });
        var loaded = ProfileStore.Load(root);
        Check(loaded.Count == 1, $"expected one profile, got {loaded.Count}");
        Check(loaded[0].Name == "balanced", "profile name lost on save");
        Check(loaded[0].EnabledMods.SequenceEqual(new[] { "a.one" }), "enabled set lost on save");
    }

    private static void ProfilesApply()
    {
        var (root, mods, catalog) = ProfileFixture();
        var profile = ProfileStore.Capture("only-a", catalog);
        profile.EnabledMods.Remove("b.two");
        ProfileStore.Save(root, new[] { profile });

        // Start from the opposite state, then apply the stored profile.
        var reopened = ModCatalog.Load(mods);
        reopened.SetEnabled("a.one", false);
        reopened.SetEnabled("b.two", true);

        var missing = ProfileStore.Apply(ProfileStore.Load(root)[0], reopened, mods);
        Check(missing.Count == 0, $"unexpected missing mods: {string.Join(',', missing)}");
        Check(reopened.Mods.Single(mod => mod.Id == "a.one").Enabled, "profile did not enable a.one");
        Check(!reopened.Mods.Single(mod => mod.Id == "b.two").Enabled, "profile did not disable b.two");

        var disabledLines = File.ReadAllLines(Path.Combine(mods, ModCatalog.DisabledListName));
        Check(disabledLines.Any(line => line.Trim() == "b.two"), "disabled.txt missing b.two");
        Check(!disabledLines.Any(line => line.Trim() == "a.one"), "disabled.txt still lists a.one");

        // A profile naming a removed mod reports it instead of failing silently.
        var ghost = new ModProfile { Name = "ghost" };
        ghost.EnabledMods.AddRange(new[] { "a.one", "gone.mod" });
        var ghostMissing = ProfileStore.Apply(ghost, reopened, mods);
        Check(ghostMissing.SequenceEqual(new[] { "gone.mod" }),
              $"missing list was {string.Join(',', ghostMissing)}");
    }

    private static void ProfilesCorrupt()
    {
        var (root, _, _) = ProfileFixture();
        File.WriteAllText(ProfileStore.PathFor(root), "{ not json at all");
        Check(ProfileStore.Load(root).Count == 0, "a corrupt profile file must load as empty");

        ProfileStore.Save(root, new[] { new ModProfile { Name = "   " } });
        Check(ProfileStore.Load(root).Count == 0, "blank profile names must not be persisted");

        var longName = new string('x', 200);
        ProfileStore.Save(root, new[] { new ModProfile { Name = longName } });
        var stored = ProfileStore.Load(root);
        Check(stored.Count == 1 && stored[0].Name.Length == ProfileStore.MaxNameLength,
              $"long name not clamped: {stored.FirstOrDefault()?.Name.Length}");
    }

    private static void Check(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }

    // ------------------------------------------------------------------ fixtures

    private static string NewTemp()
    {
        var path = Path.Combine(Path.GetTempPath(), "hyprlodr-selftest-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(path);
        return path;
    }

    private static string MakeSteamTree(string root, string buildId = "15765018")
    {
        var steamapps = Path.Combine(root, "steamapps");
        var common = Path.Combine(steamapps, "common", "hyperdemon");
        Directory.CreateDirectory(common);
        Directory.CreateDirectory(Path.Combine(steamapps, "compatdata", SteamLocator.AppId, "pfx"));
        Directory.CreateDirectory(Path.Combine(steamapps, "common", "Proton - Experimental"));

        File.WriteAllText(Path.Combine(steamapps, "libraryfolders.vdf"),
            $"\"libraryfolders\"\n{{\n\t\"0\"\n\t{{\n\t\t\"path\"\t\t\"{root}\"\n\t}}\n}}\n");
        File.WriteAllText(Path.Combine(steamapps, $"appmanifest_{SteamLocator.AppId}.acf"),
            $"\"AppState\"\n{{\n\t\"appid\"\t\t\"{SteamLocator.AppId}\"\n\t\"name\"\t\t\"HYPER DEMON\"\n" +
            $"\t\"installdir\"\t\t\"hyperdemon\"\n\t\"buildid\"\t\t\"{buildId}\"\n}}\n");
        File.WriteAllBytes(Path.Combine(common, SteamLocator.Executable), Encoding.ASCII.GetBytes("MZ fake exe"));
        File.WriteAllBytes(Path.Combine(common, "OpenAL32.dll"), Encoding.ASCII.GetBytes("original library bytes"));
        return common;
    }

    private static string MakeBuildTree(string root)
    {
        var proxy = Path.Combine(root, "OpenAL32.dll");
        Directory.CreateDirectory(root);
        File.WriteAllBytes(proxy, Encoding.ASCII.GetBytes("hyprlodr proxy bytes"));
        var mods = Path.Combine(root, "mods");
        Directory.CreateDirectory(mods);
        foreach (var id in new[] { "z.foundation", "a.palette", "b.audio", "m.overlay", "00.final" })
            File.WriteAllBytes(Path.Combine(mods, id + ".dll"), Encoding.ASCII.GetBytes("mod " + id));
        return root;
    }

    private static void WriteRegistry(string modsDirectory, params (string Id, string Version, string[] Dependencies)[] mods)
    {
        Directory.CreateDirectory(modsDirectory);
        var payload = new Dictionary<string, object>
        {
            ["schema"] = 1,
            ["mods"] = mods.ToDictionary(
                mod => mod.Id,
                mod => new Dictionary<string, object>
                {
                    ["id"] = mod.Id,
                    ["name"] = mod.Id,
                    ["version"] = mod.Version,
                    ["author"] = "tests",
                    ["dependencies"] = mod.Dependencies,
                    ["file"] = mod.Id + ".dll",
                    ["sha256"] = "00",
                }),
        };
        File.WriteAllText(Path.Combine(modsDirectory, ModCatalog.RegistryName),
            JsonSerializer.Serialize(payload));
    }

    // ------------------------------------------------------------------ scenarios

    private static void SteamDiscovery(string? _)
    {
        var root = NewTemp();
        try
        {
            MakeSteamTree(root, "15765018");
            var installs = SteamLocator.FindInRoots(new[] { root });
            Check(installs.Count == 1, $"expected one installation, found {installs.Count}");
            var install = installs[0];
            Check(install.BuildId == "15765018", $"build id {install.BuildId}");
            Check(install.Name == "HYPER DEMON", $"name {install.Name}");
            Check(install.GamePath.EndsWith(Path.Combine("common", "hyperdemon"), StringComparison.Ordinal),
                install.GamePath);
            Check(install.ProtonPrefix is not null, "proton prefix should be detected");
            Check(install.ProtonVersions.Contains("Proton - Experimental"), "proton version missing");
        }
        finally { Directory.Delete(root, true); }
    }

    private static void DiscoveryManualPath(string? _)
    {
        var root = NewTemp();
        try
        {
            var game = MakeSteamTree(root);
            var installs = SteamLocator.Find(game);
            Check(installs.Count == 1, $"manual path should find the install, got {installs.Count}");
        }
        finally { Directory.Delete(root, true); }
    }

    private static void LoaderInstall(string? _)
    {
        var root = NewTemp();
        try
        {
            var game = MakeSteamTree(root);
            var build = MakeBuildTree(Path.Combine(root, "build"));
            var installer = new LoaderInstaller();
            var result = installer.Install(game, build);
            Check(result.Ok, result.Message);

            var state = installer.Inspect(game);
            Check(state.ProxyInstalled && state.OriginalPreserved, "files not in place");
            Check(state.ModsDirectory, "mods directory missing");
            Check(state.InstalledMods.Count == 5, $"expected 5 mods, got {state.InstalledMods.Count}");
            Check(state.HashesMatch, "manifest hashes do not match");
            Check(state.LoaderVersion == "0.2.0", state.LoaderVersion ?? "(null)");

            var original = File.ReadAllText(Path.Combine(game, "OpenAL32_real.dll"));
            Check(original == "original library bytes", "original library not preserved byte-for-byte");
            var proxy = File.ReadAllText(Path.Combine(game, "OpenAL32.dll"));
            Check(proxy == "hyprlodr proxy bytes", "proxy not installed");
        }
        finally { Directory.Delete(root, true); }
    }

    private static void LoaderDoubleInstall(string? _)
    {
        var root = NewTemp();
        try
        {
            var game = MakeSteamTree(root);
            var build = MakeBuildTree(Path.Combine(root, "build"));
            var installer = new LoaderInstaller();
            Check(installer.Install(game, build).Ok, "first install failed");
            var second = installer.Install(game, build);
            Check(!second.Ok, "second install should be refused");
            Check(second.Message.Contains("already exists", StringComparison.OrdinalIgnoreCase), second.Message);
        }
        finally { Directory.Delete(root, true); }
    }

    private static void LoaderUninstall(string? _)
    {
        var root = NewTemp();
        try
        {
            var game = MakeSteamTree(root);
            var build = MakeBuildTree(Path.Combine(root, "build"));
            var installer = new LoaderInstaller();
            Check(installer.Install(game, build).Ok, "install failed");
            File.WriteAllText(Path.Combine(game, LoaderInstaller.AttachRecord), "{}");
            var result = installer.Uninstall(game);
            Check(result.Ok, result.Message);
            Check(!File.Exists(Path.Combine(game, "OpenAL32_real.dll")), "backup left behind");
            Check(!File.Exists(Path.Combine(game, LoaderInstaller.ManifestName)), "manifest left behind");
            Check(!File.Exists(Path.Combine(game, LoaderInstaller.AttachRecord)), "attach record left behind");
            Check(File.ReadAllText(Path.Combine(game, "OpenAL32.dll")) == "original library bytes",
                "original library was not restored");
            Check(!Directory.Exists(Path.Combine(game, "mods")), "empty mods directory left behind");
        }
        finally { Directory.Delete(root, true); }
    }

    private static void LoaderChangedProxy(string? _)
    {
        var root = NewTemp();
        try
        {
            var game = MakeSteamTree(root);
            var build = MakeBuildTree(Path.Combine(root, "build"));
            var installer = new LoaderInstaller();
            Check(installer.Install(game, build).Ok, "install failed");
            File.WriteAllText(Path.Combine(game, "OpenAL32.dll"), "tampered");
            var result = installer.Uninstall(game);
            Check(!result.Ok, "uninstall should refuse a changed proxy");
            Check(result.Message.Contains("changed since installation", StringComparison.Ordinal), result.Message);
            Check(File.Exists(Path.Combine(game, "OpenAL32_real.dll")), "original must not be removed when refusing");
        }
        finally { Directory.Delete(root, true); }
    }

    private static void CatalogRead(string? _)
    {
        var root = NewTemp();
        try
        {
            var mods = Path.Combine(root, "mods");
            Directory.CreateDirectory(mods);
            File.WriteAllBytes(Path.Combine(mods, "z.foundation.dll"), new byte[] { 0 });
            File.WriteAllBytes(Path.Combine(mods, "a.palette.dll"), new byte[] { 0 });
            WriteRegistry(mods, ("z.foundation", "1.0.0", Array.Empty<string>()),
                ("a.palette", "1.0.0", new[] { "z.foundation@^1.0.0" }));
            ModCatalog.SaveDisabled(mods, new[] { "a.palette" });

            var catalog = ModCatalog.Load(mods);
            Check(catalog.Mods.Count == 2, $"mods {catalog.Mods.Count}");
            Check(catalog.Mods.All(mod => mod.Version == "1.0.0"), "registry versions not applied");
            Check(catalog.Mods.Single(mod => mod.Id == "a.palette").Enabled == false, "disabled list ignored");
            var plan = catalog.BuildPlan();
            Check(plan.Order.SequenceEqual(new[] { "z.foundation" }),
                $"disabled mod should not be planned: {string.Join(",", plan.Order)}");
        }
        finally { Directory.Delete(root, true); }
    }

    private static void CatalogOrder(string? _)
    {
        var root = NewTemp();
        try
        {
            var mods = Path.Combine(root, "mods");
            Directory.CreateDirectory(mods);
            var ids = new[] { "z.foundation", "a.palette", "b.audio", "m.overlay", "00.final" };
            foreach (var id in ids) File.WriteAllBytes(Path.Combine(mods, id + ".dll"), new byte[] { 0 });
            WriteRegistry(mods,
                ("z.foundation", "1.0.0", Array.Empty<string>()),
                ("a.palette", "1.0.0", new[] { "z.foundation" }),
                ("b.audio", "1.0.0", new[] { "z.foundation" }),
                ("m.overlay", "1.0.0", new[] { "a.palette" }),
                ("00.final", "1.0.0", new[] { "m.overlay", "b.audio" }));

            var catalog = ModCatalog.Load(mods);
            var plan = catalog.BuildPlan();
            Check(plan.Ok, $"plan should be clean: {string.Join("; ", plan.Problems)}");
            var expected = new[] { "z.foundation", "a.palette", "b.audio", "m.overlay", "00.final" };
            Check(plan.Order.SequenceEqual(expected),
                $"order mismatch: {string.Join(",", plan.Order)}");
        }
        finally { Directory.Delete(root, true); }
    }

    private static void CatalogMissingDependency(string? _)
    {
        var root = NewTemp();
        try
        {
            var mods = Path.Combine(root, "mods");
            Directory.CreateDirectory(mods);
            File.WriteAllBytes(Path.Combine(mods, "needs.missing.dll"), new byte[] { 0 });
            WriteRegistry(mods, ("needs.missing", "1.0.0", new[] { "not.there" }));
            var catalog = ModCatalog.Load(mods);
            var plan = catalog.BuildPlan();
            Check(!plan.Ok, "missing dependency should be reported");
            Check(plan.Problems.Any(problem => problem.Contains("missing dependency: not.there", StringComparison.Ordinal)),
                string.Join("; ", plan.Problems));
            Check(catalog.Mods[0].Status == "dependency missing", catalog.Mods[0].Status);
        }
        finally { Directory.Delete(root, true); }
    }

    private static void CatalogCycle(string? _)
    {
        var root = NewTemp();
        try
        {
            var mods = Path.Combine(root, "mods");
            Directory.CreateDirectory(mods);
            File.WriteAllBytes(Path.Combine(mods, "cycle.a.dll"), new byte[] { 0 });
            File.WriteAllBytes(Path.Combine(mods, "cycle.b.dll"), new byte[] { 0 });
            WriteRegistry(mods, ("cycle.a", "1.0.0", new[] { "cycle.b" }),
                ("cycle.b", "1.0.0", new[] { "cycle.a" }));
            var catalog = ModCatalog.Load(mods);
            var plan = catalog.BuildPlan();
            Check(!plan.Ok, "cycle should be reported");
            Check(plan.Problems.Any(problem => problem.Contains("cycle among", StringComparison.Ordinal)),
                string.Join("; ", plan.Problems));
        }
        finally { Directory.Delete(root, true); }
    }

    private static void CatalogVersionMismatch(string? _)
    {
        var root = NewTemp();
        try
        {
            var mods = Path.Combine(root, "mods");
            Directory.CreateDirectory(mods);
            File.WriteAllBytes(Path.Combine(mods, "z.foundation.dll"), new byte[] { 0 });
            File.WriteAllBytes(Path.Combine(mods, "ver.old.dll"), new byte[] { 0 });
            WriteRegistry(mods, ("z.foundation", "1.0.0", Array.Empty<string>()),
                ("ver.old", "1.0.0", new[] { "z.foundation@>=2.0.0" }));
            var catalog = ModCatalog.Load(mods);
            var plan = catalog.BuildPlan();
            Check(!plan.Ok, "incompatible version should be reported");
            Check(plan.Problems.Any(problem => problem.Contains("does not satisfy", StringComparison.Ordinal)),
                string.Join("; ", plan.Problems));
        }
        finally { Directory.Delete(root, true); }
    }

    private static void CatalogBlockedNotCycle(string? _)
    {
        var root = NewTemp();
        try
        {
            var mods = Path.Combine(root, "mods");
            Directory.CreateDirectory(mods);
            foreach (var id in new[] { "z.foundation", "b.audio", "00.final" })
                File.WriteAllBytes(Path.Combine(mods, id + ".dll"), new byte[] { 0 });
            WriteRegistry(mods,
                ("z.foundation", "1.0.0", Array.Empty<string>()),
                ("b.audio", "1.0.0", new[] { "z.foundation" }),
                ("00.final", "1.0.0", new[] { "b.audio" }));
            ModCatalog.SaveDisabled(mods, new[] { "b.audio" });

            var catalog = ModCatalog.Load(mods);
            var plan = catalog.BuildPlan();
            Check(!plan.Ok, "a disabled dependency must be reported");
            Check(plan.Problems.Any(problem =>
                    problem.Contains("dependency is disabled: b.audio", StringComparison.Ordinal)),
                string.Join("; ", plan.Problems));
            Check(!plan.Problems.Any(problem => problem.Contains("cycle", StringComparison.Ordinal)),
                "a blocked dependency must not be reported as a cycle: " + string.Join("; ", plan.Problems));
            Check(plan.Order.SequenceEqual(new[] { "z.foundation" }),
                "only the unblocked mod should be planned: " + string.Join(",", plan.Order));
        }
        finally { Directory.Delete(root, true); }
    }

    private static void VersionConstraints()
    {
        Check(ModCatalog.Satisfies("^", "1.0.0", "1.4.2"), "^1.0.0 accepts 1.4.2");
        Check(!ModCatalog.Satisfies("^", "1.0.0", "2.0.1"), "^1.0.0 rejects 2.0.1");
        Check(ModCatalog.Satisfies("~", "1.2.0", "1.2.9"), "~1.2.0 accepts 1.2.9");
        Check(!ModCatalog.Satisfies("~", "1.2.0", "1.3.0"), "~1.2.0 rejects 1.3.0");
        Check(ModCatalog.Satisfies(">=", "1.0.0", "1.0.0"), ">= accepts equal");
        Check(!ModCatalog.Satisfies(">", "1.0.0", "1.0.0"), "> rejects equal");
        Check(ModCatalog.Satisfies("", "", "anything"), "no constraint accepts anything");
        Check(ModCatalog.SplitSpec("z.foundation@>=1.2.0") == ("z.foundation", ">=", "1.2.0"), "spec split");
        Check(ModCatalog.SplitSpec("b.audio") == ("b.audio", "", ""), "bare spec split");
    }

    private static void EventLog()
    {
        var root = NewTemp();
        try
        {
            var path = Path.Combine(root, "events.jsonl");
            File.WriteAllLines(path, new[]
            {
                "{\"seq\":1,\"event\":\"discovered\",\"mod\":\"z.foundation\",\"message\":\"a.so\"}",
                "{\"seq\":2,\"event\":\"initialize_ok\",\"mod\":\"z.foundation\",\"message\":\"\"}",
                "{\"seq\":3,\"event\":\"shutdown\",\"mod\":\"z.foundation\",\"mess",
            });
            var events = EventLogReader.Read(path);
            Check(events.Count == 2, $"expected 2 parsable events, got {events.Count}");
            Check(events[1].Event == "initialize_ok", events[1].Event);
            Check(events[1].Seq == 2, events[1].Seq.ToString());
        }
        finally { Directory.Delete(root, true); }
    }

    private static void PackageValidation()
    {
        var root = NewTemp();
        try
        {
            var good = Path.Combine(root, "good.zip");
            using (var archive = ZipFile.Open(good, ZipArchiveMode.Create))
            {
                WriteEntry(archive, "manifest.json", ManifestJson("demo.pkg", "1.0.0"));
                WriteEntry(archive, "native/linux-x86_64/demo.pkg.so", "binary");
            }
            var (package, error) = PackageInspector.Inspect(good);
            Check(package is not null && error is null, error ?? "valid package rejected");
            Check(package!.Id == "demo.pkg" && package.Version == "1.0.0", "manifest fields");

            var traversal = Path.Combine(root, "traversal.zip");
            using (var archive = ZipFile.Open(traversal, ZipArchiveMode.Create))
            {
                WriteEntry(archive, "manifest.json", ManifestJson("demo.pkg", "1.0.0"));
                WriteEntry(archive, "../evil.so", "binary");
            }
            var (badPackage, badError) = PackageInspector.Inspect(traversal);
            Check(badPackage is null && badError!.Contains("unsafe path", StringComparison.Ordinal), badError ?? "");

            var wrongFile = Path.Combine(root, "wrong.zip");
            using (var archive = ZipFile.Open(wrongFile, ZipArchiveMode.Create))
            {
                WriteEntry(archive, "manifest.json", ManifestJson("demo.pkg", "1.0.0"));
                WriteEntry(archive, "installer.exe", "binary");
            }
            var (_, wrongError) = PackageInspector.Inspect(wrongFile);
            Check(wrongError!.Contains("not allowed", StringComparison.Ordinal), wrongError ?? "");
        }
        finally { Directory.Delete(root, true); }
    }

    private static void PackageImport(string? sample)
    {
        if (sample is null || !File.Exists(sample))
        {
            Console.WriteLine("     (skipping import verification: no sample library supplied)");
            return;
        }
        var root = NewTemp();
        try
        {
            var platform = sample.EndsWith(".dll", StringComparison.OrdinalIgnoreCase)
                ? "windows-x86_64" : "linux-x86_64";
            var suffix = platform == "windows-x86_64" ? ".dll" : ".so";
            var packagePath = Path.Combine(root, "pkg.zip");
            using (var archive = ZipFile.Open(packagePath, ZipArchiveMode.Create))
            {
                WriteEntry(archive, "manifest.json",
                    ManifestJson("z.foundation", "1.0.0", platform, "native/" + platform + "/z.foundation" + suffix));
                WriteEntry(archive, "native/" + platform + "/z.foundation" + suffix, File.ReadAllBytes(sample));
            }
            var mods = Path.Combine(root, "mods");
            var inspector = new PackageInspector();
            var result = inspector.Import(packagePath, mods);
            Check(result.Ok, result.Message);
            Check(File.Exists(Path.Combine(mods, "z.foundation" + suffix)), "binary not imported");

            var catalog = ModCatalog.Load(mods);
            Check(catalog.Mods.Count == 1 && catalog.Mods[0].Id == "z.foundation",
                "registry entry not readable by the catalog");
        }
        finally { Directory.Delete(root, true); }
    }

    private static void WriteEntry(ZipArchive archive, string name, string content) =>
        WriteEntry(archive, name, Encoding.UTF8.GetBytes(content));

    private static void WriteEntry(ZipArchive archive, string name, byte[] content)
    {
        var entry = archive.CreateEntry(name);
        using var stream = entry.Open();
        stream.Write(content);
    }

    private static string ManifestJson(string id, string version, string platform = "linux-x86_64",
        string? entry = null) =>
        JsonSerializer.Serialize(new Dictionary<string, object>
        {
            ["schema"] = 1,
            ["id"] = id,
            ["name"] = id,
            ["version"] = version,
            ["author"] = "tests",
            ["description"] = "self test package",
            ["api"] = 2,
            ["platform"] = platform,
            ["entry"] = entry ?? $"native/{platform}/{id}.so",
            ["dependencies"] = Array.Empty<string>(),
            ["conflicts"] = Array.Empty<string>(),
            ["capabilities"] = Array.Empty<string>(),
        });
}
