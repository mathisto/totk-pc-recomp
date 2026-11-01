// totk-modmerge: merges TotK mods with TkSharp (TKMM's merge library) into one folder.
//
//   totk-modmerge --keys DIR --base FILE --update FILE --version 1.2.1 --data DIR --out DIR [--lang USen] MOD...
//
// MOD is a mod folder (with romfs/ and optionally exefs/), a .zip, .7z or .tkcl. Later mods win: each is merged over
// the ones before it, file format by file format (BYML, packs, SARC, MSBT, GameData, RSDB, Mals, RESTBL), against the
// vanilla files TkSharp reads from the base game and update with the keys in DIR (prod.keys and title.keys).
// The merge writes <out>/romfs/... and, for ExeFS patches, <out>/exefs_patches/*.ips. --data holds TkSharp's mod store
// (the changelogs of imported mods) and is emptied first, so each run starts from the listed mods alone.
// Exit codes: 0 merged, 1 failed, 2 bad command line.
using Microsoft.Extensions.Logging;
using TkSharp;
using TkSharp.Core;
using TkSharp.Core.Extensions;
using TkSharp.Core.IO.Caching;
using TkSharp.Core.Models;
using TkSharp.Data.Embedded;
using TkSharp.Extensions.LibHac;
using TkSharp.IO.Writers;
using TkSharp.Merging;

static int Usage(string message)
{
    Console.Error.WriteLine($"totk-modmerge: {message}");
    Console.Error.WriteLine("usage: totk-modmerge --keys DIR --base FILE --update FILE --version VER --data DIR --out DIR [--lang CODE] MOD...");
    return 2;
}

var options = new Dictionary<string, string>();
var mods = new List<string>();
for (int i = 0; i < args.Length; i++) {
    if (args[i].StartsWith("--")) {
        if (i + 1 >= args.Length) return Usage($"{args[i]} needs a value");
        options[args[i][2..]] = args[++i];
    }
    else {
        mods.Add(args[i]);
    }
}
foreach (string required in new[] { "keys", "base", "update", "version", "data", "out" })
    if (!options.ContainsKey(required)) return Usage($"--{required} is required");
if (mods.Count == 0) return Usage("no mods given");
foreach (string mod in mods)
    if (!File.Exists(mod) && !Directory.Exists(mod)) return Usage($"{mod} does not exist");

TkLog.Instance.Register(new StderrLogger());
TkLog.Instance.LogLevel = LogLevel.Information;

string dataFolder = Path.GetFullPath(options["data"]);
string outFolder = Path.GetFullPath(options["out"]);
if (Directory.Exists(dataFolder)) Directory.Delete(dataFolder, recursive: true);
if (Directory.Exists(outFolder)) Directory.Delete(outFolder, recursive: true);
Directory.CreateDirectory(dataFolder);
Directory.CreateDirectory(outFolder);

try {
    using var checksums = TkEmbeddedDataSource.GetChecksumsBin();
    using var packFileLookup = TkEmbeddedDataSource.GetPackFileLookup();
    var romProvider = TkExtensibleRomProviderBuilder
        .Create(TkChecksums.FromStream(checksums), new TkPackFileLookup(packFileLookup))
        .WithPreferredVersion(options["version"])
        .WithKeysFolder(options["keys"])
        .WithPackagedBaseGame([options["base"]])
        .WithPackagedUpdate([options["update"]])
        .Build();

    TkChangelogBuilder.Init(romProvider, dataFolder);
    var manager = TkModManager.Create(dataFolder);
    var profile = manager.GetCurrentProfile();
    var readers = new TkModReaderProvider(manager, romProvider);

    var imported = new List<TkMod>();
    foreach (string path in mods) {
        Console.WriteLine($"import {path}");
        if (await readers.ReadFromInput(Path.GetFullPath(path)) is not TkMod mod) {
            Console.Error.WriteLine($"totk-modmerge: no reader understands {path}");
            return 1;
        }
        manager.Import(mod, profile);
        imported.Add(mod);
    }

    // TkModManager.GetMergeTargets merges profile.Mods from last to first, so the first entry wins. The command line
    // lists mods lowest priority first, so the profile holds them in reverse.
    var byId = profile.Mods.ToDictionary(m => m.Mod.Id);
    profile.Mods.Clear();
    foreach (var mod in Enumerable.Reverse(imported)) profile.Mods.Add(byId[mod.Id]);
    manager.Save();

    using ITkRom rom = romProvider.GetRom();
    Console.WriteLine($"vanilla: game version {rom.GameVersion}, main build id {rom.NsoBinaryId}");
    var merger = new TkMerger(new FolderModWriter(outFolder), rom, [options.GetValueOrDefault("lang", "USen")],
        Path.Combine(outFolder, "exefs_patches"));
    // The sequential Merge, not MergeAsync: the overlay manifest needs the same output on every run, and a failure
    // names its file in order.
    merger.Merge(TkModManager.GetMergeTargets(profile, _ => true));
    Console.WriteLine($"merged {imported.Count} mods into {outFolder}");
    return 0;
}
catch (Exception ex) {
    Console.Error.WriteLine($"totk-modmerge: {ex}");
    return 1;
}

sealed class StderrLogger : ILogger
{
    public void Log<TState>(LogLevel level, EventId id, TState state, Exception? ex, Func<TState, Exception?, string> format)
        => Console.Error.WriteLine($"[{level}] {format(state, ex)}{(ex is null ? "" : $": {ex.Message}")}");

    public bool IsEnabled(LogLevel level) => level >= LogLevel.Information;

    public IDisposable? BeginScope<TState>(TState state) where TState : notnull => null;
}
