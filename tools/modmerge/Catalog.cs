// totk-modmerge catalog: what a Fuse data edit can name, read from the player's own game (nothing of the game's is
// shipped). Fuse checks edits against it ("did you mean ...") and turns it into a JSON Schema for editors.
//
//   totk-modmerge catalog --keys DIR --base FILE --update FILE --version 1.2.1 --index FILE --out FILE [--no-params]
//
// --index is the recomp's romfs/index.json (it lists every RomFS path; TkSharp reads files but cannot list them). The
// catalog, JSON:
//   rsdb     {Table: {"rows": [__RowId, ...], "fields": {name: BYML type}}}   targets rsdb:<Table>/<RowId>
//   params   {Class: {Name: actor pack}}   param:<Class>/<Name>, in Pack/Actor/<pack>.pack (the pack is usually Name)
//   text     {message file: [label, ...]}  text:<locale>/<file>/<label>, from the USen archive
//   names    {actor: English item name}    from ActorMsg/PouchContent's <actor>_Name labels, for search
//   files    [canonical path, ...]          file: targets (BYML files outside packs)
// --no-params skips opening the actor packs (the slow part: about 15,000 packs).
using System.Text.Json;
using BymlLibrary;
using MessageStudio.Formats.BinaryText;
using SarcLibrary;
using TkSharp.Core;
using TkSharp.Core.IO.Caching;
using TkSharp.Data.Embedded;
using TkSharp.Extensions.LibHac;

static class Catalog
{
    public static int Run(string[] args)
    {
        var o = new Dictionary<string, string>();
        bool noParams = false;
        for (int i = 0; i < args.Length; i++) {
            if (args[i] == "--no-params") { noParams = true; continue; }
            if (!args[i].StartsWith("--") || i + 1 >= args.Length) return Usage($"unexpected argument {args[i]}");
            o[args[i][2..]] = args[++i];
        }
        foreach (string required in new[] { "keys", "base", "update", "version", "index", "out" })
            if (!o.ContainsKey(required)) return Usage($"--{required} is required");
        try {
            var paths = new List<string>();
            using (var index = JsonDocument.Parse(File.ReadAllBytes(o["index"])))
                foreach (var f in index.RootElement.GetProperty("files").EnumerateArray())
                    paths.Add(f.GetProperty("path").GetString()!);

            using var checksums = TkEmbeddedDataSource.GetChecksumsBin();
            using var packFileLookup = TkEmbeddedDataSource.GetPackFileLookup();
            using ITkRom rom = TkExtensibleRomProviderBuilder
                .Create(TkChecksums.FromStream(checksums), new TkPackFileLookup(packFileLookup))
                .WithPreferredVersion(o["version"]).WithKeysFolder(o["keys"])
                .WithPackagedBaseGame([o["base"]]).WithPackagedUpdate([o["update"]])
                .Build().GetRom();

            string outPath = Path.GetFullPath(o["out"]), tmp = outPath + ".tmp";
            using (var stream = File.Create(tmp))
            using (var w = new Utf8JsonWriter(stream)) {
                w.WriteStartObject();
                w.WriteNumber("format", 1);
                w.WriteString("game_version", o["version"]);

                w.WriteStartObject("rsdb");
                int rows = 0;
                foreach (string rel in paths.Where(p => p.StartsWith("RSDB/") && p.Contains(".rstbl.byml")).Order()) {
                    string table = Path.GetFileName(rel).Split('.')[0];
                    byte[]? bytes = Read(rom, rel);
                    if (bytes is null) continue;
                    var root = Byml.FromBinary(bytes);
                    if (root.Type != BymlNodeType.Array) continue;   // not a table of rows (rsdb: cannot target it)
                    var ids = new List<string>();
                    var fields = new SortedDictionary<string, string>(StringComparer.Ordinal);
                    foreach (var r in root.GetArray()) {
                        if (r.Type != BymlNodeType.Map) continue;
                        var map = r.GetMap();
                        if (map.TryGetValue("__RowId", out var id) && id.Type == BymlNodeType.String) ids.Add(id.GetString());
                        foreach (var (k, v) in map) fields.TryAdd(k, v.Type.ToString());
                    }
                    w.WriteStartObject(table);
                    w.WriteStartArray("rows");
                    foreach (string id in ids) w.WriteStringValue(id);
                    w.WriteEndArray();
                    w.WriteStartObject("fields");
                    foreach (var (k, t) in fields) w.WriteString(k, t);
                    w.WriteEndObject();
                    w.WriteEndObject();
                    rows += ids.Count;
                }
                w.WriteEndObject();

                w.WriteStartObject("params");
                int packs = 0, members = 0;
                if (!noParams) {
                    var byClass = new SortedDictionary<string, SortedDictionary<string, string>>(StringComparer.Ordinal);
                    foreach (string rel in paths.Where(p => p.StartsWith("Pack/Actor/") && p.EndsWith(".pack.zs"))) {
                        string actor = Path.GetFileName(rel)[..^".pack.zs".Length];
                        byte[]? bytes = Read(rom, rel);
                        if (bytes is null) continue;
                        packs++;
                        foreach (var (name, _) in Sarc.FromBinary(bytes)) {
                            // Component/<Class>/<Name>.game__component__<Class>.bgyml
                            string[] part = name.Split('/');
                            if (part.Length != 3 || part[0] != "Component") continue;
                            string suffix = $".game__component__{part[1]}.bgyml";
                            if (!part[2].EndsWith(suffix)) continue;
                            string member = part[2][..^suffix.Length];
                            if (!byClass.TryGetValue(part[1], out var names)) byClass[part[1]] = names = new(StringComparer.Ordinal);
                            // Packs share components: keep the member's own-named pack (param's default) when it has one.
                            if (actor == member) names[member] = actor;
                            else names.TryAdd(member, actor);
                            members++;
                        }
                    }
                    foreach (var (cls, names) in byClass) {
                        w.WriteStartObject(cls);
                        foreach (var (name, actor) in names) w.WriteString(name, actor);
                        w.WriteEndObject();
                    }
                }
                w.WriteEndObject();

                var itemNames = new SortedDictionary<string, string>(StringComparer.Ordinal);
                w.WriteStartObject("text");
                string mals = paths.FirstOrDefault(p => p.StartsWith("Mals/USen.") && p.EndsWith(".sarc.zs")) ?? "";
                byte[]? malsBytes = mals.Length > 0 ? Read(rom, mals) : null;
                int labels = 0;
                if (malsBytes is not null) {
                    foreach (var (file, data) in Sarc.FromBinary(malsBytes).OrderBy(kv => kv.Key, StringComparer.Ordinal)) {
                        if (!file.EndsWith(".msbt")) continue;
                        var msbt = Msbt.FromBinary(data.ToArray(), MsbtOptions.Default);
                        w.WriteStartArray(file[..^".msbt".Length]);
                        foreach (var (label, entry) in msbt.OrderBy(kv => kv.Key, StringComparer.Ordinal)) {
                            w.WriteStringValue(label);
                            labels++;
                            if (file == "ActorMsg/PouchContent.msbt" && label.EndsWith("_Name") && entry.Text is { Length: > 0 } t)
                                itemNames[label[..^"_Name".Length]] = t;
                        }
                        w.WriteEndArray();
                    }
                }
                w.WriteEndObject();

                w.WriteStartObject("names");
                foreach (var (actor, name) in itemNames) w.WriteString(actor, name);
                w.WriteEndObject();

                w.WriteStartArray("files");
                foreach (string rel in paths.Where(p => !p.StartsWith("Pack/") && (p.EndsWith(".bgyml") || p.EndsWith(".byml")
                             || p.EndsWith(".bgyml.zs") || p.EndsWith(".byml.zs")) && !p.Contains(".Product.")).Order())
                    w.WriteStringValue(rel.EndsWith(".zs") ? rel[..^3] : rel);
                w.WriteEndArray();
                w.WriteEndObject();
                Console.WriteLine($"catalog: {rows} rows in RSDB, {members} params in {packs} actor packs, {labels} message labels, " +
                                  $"{itemNames.Count} item names");
            }
            File.Move(tmp, outPath, overwrite: true);
            Console.WriteLine($"wrote {outPath}");
            return 0;
        }
        catch (Exception e) {
            Console.Error.WriteLine($"totk-modmerge catalog: {e.Message}");
            return 1;
        }
    }

    // totk-modmerge list --keys DIR --base FILE --update FILE --version VER --file <RomFS path> [--grep TEXT]: the member
    // names of a pack (any SARC) in the game, for finding where a shared file lives.
    public static int List(string[] args)
    {
        var o = new Dictionary<string, string>();
        for (int i = 0; i + 1 < args.Length; i += 2) o[args[i].TrimStart('-')] = args[i + 1];
        foreach (string required in new[] { "keys", "base", "update", "version", "file" })
            if (!o.ContainsKey(required)) {
                Console.Error.WriteLine($"totk-modmerge list: --{required} is required");
                return 2;
            }
        try {
            using var checksums = TkEmbeddedDataSource.GetChecksumsBin();
            using var packFileLookup = TkEmbeddedDataSource.GetPackFileLookup();
            using ITkRom rom = TkExtensibleRomProviderBuilder
                .Create(TkChecksums.FromStream(checksums), new TkPackFileLookup(packFileLookup))
                .WithPreferredVersion(o["version"]).WithKeysFolder(o["keys"])
                .WithPackagedBaseGame([o["base"]]).WithPackagedUpdate([o["update"]])
                .Build().GetRom();
            byte[]? bytes = Read(rom, o["file"]);
            if (bytes is null) {
                Console.Error.WriteLine($"totk-modmerge list: no {o["file"]} in the game");
                return 1;
            }
            string grep = o.GetValueOrDefault("grep", "");
            int n = 0;
            foreach (var (name, data) in Sarc.FromBinary(bytes).OrderBy(kv => kv.Key, StringComparer.Ordinal))
                if (name.Contains(grep, StringComparison.OrdinalIgnoreCase)) {
                    Console.WriteLine($"{name}\t{data.Count}");
                    n++;
                }
            Console.WriteLine($"{n} members");
            return 0;
        }
        catch (Exception e) {
            Console.Error.WriteLine($"totk-modmerge list: {e.Message}");
            return 1;
        }
    }

    static byte[]? Read(ITkRom rom, string rel)
    {
        using var vanilla = rom.GetVanilla(rel);
        return vanilla.IsEmpty ? null : vanilla.Segment.ToArray();
    }

    static int Usage(string message)
    {
        Console.Error.WriteLine($"totk-modmerge catalog: {message}");
        Console.Error.WriteLine("usage: totk-modmerge catalog --keys DIR --base FILE --update FILE --version VER --index FILE --out FILE [--no-params]");
        return 2;
    }
}
