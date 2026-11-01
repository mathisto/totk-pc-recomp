// totk-modmerge data: applies Fuse data edits (the JSON `fuse data compile` writes) to the vanilla game files and writes
// the changed files as an ordinary mod folder, which the normal merge then takes like any other mod.
//
//   totk-modmerge data --keys DIR --base FILE --update FILE --version 1.2.1 --edits FILE --out DIR [--over DIR]
//
// The edits file: {"format": 1, "edits": [{"source": "...", "target": "...", "pack": null, "ops": [[op, path, value]]}]}
//   target  rsdb:<Table>/<RowId>    a row of RSDB/<Table>.Product.<ver>.rstbl.byml.zs, found by __RowId
//           file:<canonical path>   a BYML file anywhere in the RomFS, e.g. GameParameter/....bgyml
//           param:<Class>/<Name>    Component/<Class>/<Name>.game__component__<Class>.bgyml inside the actor pack
//                                   Pack/Actor/<Name>.pack.zs (or the pack named by "pack")
//           text:<Locale|*>/<message file>/<label>   a message (an item's name) in Mals/<Locale>.Product.<ver>.sarc.zs,
//                                   op set on path "Text"; * is every language the game has
//   select  {"path": array, "match": {field: value}}: the ops apply to the one record of that array that matches
//   ops     set, add or mul; path is dot-separated map keys and array indexes ("List.0.Value"). Values keep the type
//           the game file already has (an Int stays an Int); set on a missing key adds it with the value's own type.
//           append (path, record): a record at the end of the array, its fields typed like the array's records;
//           remove (path, match): every record that matches (at least one must).
// Edits apply in order to one in-memory copy of each file, so two mods' mul edits stack. Only changed files are written,
// uncompressed under <out>/romfs/<path> (the merge recompresses). --over DIR reads a file from DIR/romfs/<path> (a merge
// output, compressed or not) in place of vanilla when it is there. The op "get" prints the value at a path instead of
// changing it (for checks). Exit codes: 0 written, 1 failed, 2 bad command line.
using System.Text.Json;
using BymlLibrary;
using BymlLibrary.Nodes.Containers;
using MessageStudio.Formats.BinaryText;
using Revrs;
using SarcLibrary;
using TkSharp.Core;
using TkSharp.Core.IO.Caching;
using TkSharp.Data.Embedded;
using TkSharp.Extensions.LibHac;

static class DataEdits
{
    sealed class EditError(string message) : Exception(message);

    // One game file being edited: a loose BYML file, or a pack holding edited BYML members.
    sealed class Doc
    {
        public required string RelativePath;   // where it goes in the mod folder (and where vanilla came from)
        public Byml? Root;                     // a loose BYML file
        public Endianness Endian;
        public ushort Version;
        public Sarc? Pack;                     // a pack, with its edited members below
        public readonly Dictionary<string, (Byml Root, Endianness Endian, ushort Version)> Members = [];
        public readonly Dictionary<string, Msbt> Texts = [];   // a message archive's edited message files
        public bool Changed;
    }

    static readonly string[] Locales = ["USen", "USes", "USfr", "EUen", "EUde", "EUes", "EUfr", "EUit", "EUnl", "EUru",
                                        "JPja", "KRko", "CNzh", "TWzh"];

    static string? over;   // --over: a merge output read in place of vanilla

    public static int Run(string[] args)
    {
        var options = new Dictionary<string, string>();
        for (int i = 0; i < args.Length; i++) {
            if (!args[i].StartsWith("--") || i + 1 >= args.Length) return Usage($"unexpected argument {args[i]}");
            options[args[i][2..]] = args[++i];
        }
        foreach (string required in new[] { "keys", "base", "update", "version", "edits", "out" })
            if (!options.ContainsKey(required)) return Usage($"--{required} is required");

        try {
            using var json = JsonDocument.Parse(File.ReadAllBytes(options["edits"]));
            var root = json.RootElement;
            if (root.GetProperty("format").GetInt32() != 1) throw new EditError("unknown edits format");

            using var checksums = TkEmbeddedDataSource.GetChecksumsBin();
            using var packFileLookup = TkEmbeddedDataSource.GetPackFileLookup();
            var romProvider = TkExtensibleRomProviderBuilder
                .Create(TkChecksums.FromStream(checksums), new TkPackFileLookup(packFileLookup))
                .WithPreferredVersion(options["version"])
                .WithKeysFolder(options["keys"])
                .WithPackagedBaseGame([options["base"]])
                .WithPackagedUpdate([options["update"]])
                .Build();
            using ITkRom rom = romProvider.GetRom();

            over = options.TryGetValue("over", out var o) ? Path.GetFullPath(o) : null;
            var docs = new Dictionary<string, Doc>(StringComparer.Ordinal);
            int applied = 0;
            foreach (var edit in root.GetProperty("edits").EnumerateArray()) {
                string source = edit.GetProperty("source").GetString()!;
                string target = edit.GetProperty("target").GetString()!;
                string? pack = edit.TryGetProperty("pack", out var p) && p.ValueKind == JsonValueKind.String ? p.GetString() : null;
                try {
                    if (target.StartsWith("text:")) {
                        applied += ApplyText(rom, docs, target, edit.GetProperty("ops"));
                        continue;
                    }
                    Byml node = Resolve(rom, docs, target, pack, out var doc);
                    if (edit.TryGetProperty("select", out var sel)) node = Select(node, sel);
                    foreach (var op in edit.GetProperty("ops").EnumerateArray()) {
                        if (op[0].GetString() == "get") {
                            Console.WriteLine($"get {target} {op[1].GetString()} = {Show(Get(node, op[1].GetString()!))}");
                            continue;
                        }
                        Apply(node, op[0].GetString()!, op[1].GetString()!, op[2]);
                        applied++;
                        doc.Changed = true;
                    }
                }
                catch (EditError e) {
                    throw new EditError($"{source}: {target}: {e.Message}");
                }
            }

            string outFolder = Path.GetFullPath(options["out"]);
            if (Directory.Exists(outFolder)) {   // only a folder this mode wrote (nothing but romfs/) is replaced
                if (Directory.EnumerateFileSystemEntries(outFolder).Any(e => Path.GetFileName(e) != "romfs"))
                    throw new EditError($"{outFolder} holds more than an earlier data-edit output; give an empty or new --out folder");
                Directory.Delete(outFolder, recursive: true);
            }
            foreach (var doc in docs.Values) {
                if (!doc.Changed) continue;
                string path = Inside(Path.Combine(outFolder, "romfs"), doc.RelativePath);
                Directory.CreateDirectory(Path.GetDirectoryName(path)!);
                using var ms = new MemoryStream();
                if (doc.Pack is { } sarc) {
                    foreach (var (name, m) in doc.Members) {
                        using var member = new MemoryStream();
                        m.Root.WriteBinary(member, m.Endian, m.Version);
                        sarc[name] = member.ToArray();
                    }
                    foreach (var (name, msbt) in doc.Texts) sarc[name] = msbt.ToBinary(null, null);
                    sarc.Write(ms);
                }
                else {
                    doc.Root!.WriteBinary(ms, doc.Endian, doc.Version);
                }
                File.WriteAllBytes(path, ms.ToArray());
                Console.WriteLine($"wrote {doc.RelativePath}" + (doc.Pack is null ? "" :
                    $" ({string.Join(", ", doc.Members.Keys.Concat(doc.Texts.Keys))})"));
            }
            Console.WriteLine($"{applied} operations on {docs.Values.Count(d => d.Changed)} files into {outFolder}");
            return 0;
        }
        catch (EditError e) {
            Console.Error.WriteLine($"totk-modmerge data: {e.Message}");
            return 1;
        }
        catch (Exception e) {
            Console.Error.WriteLine($"totk-modmerge data: {e}");
            return 1;
        }
    }

    static int Usage(string message)
    {
        Console.Error.WriteLine($"totk-modmerge data: {message}");
        Console.Error.WriteLine("usage: totk-modmerge data --keys DIR --base FILE --update FILE --version VER --edits FILE --out DIR [--over DIR]");
        return 2;
    }

    // The node an edit's paths start from.
    static Byml Resolve(ITkRom rom, Dictionary<string, Doc> docs, string target, string? pack, out Doc doc)
    {
        int colon = target.IndexOf(':');
        if (colon < 0) throw new EditError("a target is namespace:path (rsdb:, file:, param:)");
        string ns = target[..colon], rest = target[(colon + 1)..];
        // The path part names a file in the RomFS: forward slashes, no "..", nothing rooted, no drive or stream.
        if (rest.Contains('\\') || rest.Contains(':') || rest.StartsWith('/') || rest.Split('/').Any(p => p is "" or "." or ".."))
            throw new EditError("the target's path must be a plain RomFS path");
        if (pack is not null && (pack.Contains('/') || pack.Contains('\\') || pack.Contains(':') || pack.Contains("..")))
            throw new EditError("pack must be an actor name");
        switch (ns) {
            case "rsdb": {
                int slash = rest.IndexOf('/');
                if (slash <= 0 || slash == rest.Length - 1) throw new EditError("rsdb targets are rsdb:<Table>/<RowId>");
                string table = rest[..slash], row = rest[(slash + 1)..];
                doc = LooseDoc(rom, docs, $"RSDB/{table}.Product.rstbl.byml",
                    TkFileAttributes.HasZsExtension | TkFileAttributes.IsProductFile);
                foreach (var r in doc.Root!.GetArray()) {
                    var map = r.GetMap();
                    if (map.TryGetValue("__RowId", out var id) && id.Type == BymlNodeType.String && id.GetString() == row) return r;
                }
                throw new EditError($"no row {row} in {table}");
            }
            case "file": {
                if (rest.Length == 0) throw new EditError("file targets name a canonical path");
                foreach (var attrs in new[] { TkFileAttributes.None, TkFileAttributes.HasZsExtension,
                             TkFileAttributes.HasZsExtension | TkFileAttributes.IsProductFile }) {
                    var found = TryLooseDoc(rom, docs, rest, attrs);
                    if (found is not null) {
                        doc = found;
                        return doc.Root!;
                    }
                }
                throw new EditError($"no such file in the game: {rest}");
            }
            case "param": {
                int slash = rest.IndexOf('/');
                if (slash <= 0 || slash == rest.Length - 1) throw new EditError("param targets are param:<Class>/<Name>");
                string cls = rest[..slash], name = rest[(slash + 1)..];
                string member = $"Component/{cls}/{name}.game__component__{cls}.bgyml";
                string packCanonical = $"Pack/Actor/{pack ?? name}.pack";
                string packPath = rom.CanonicalToRelativePath(packCanonical, TkFileAttributes.HasZsExtension);
                if (!docs.TryGetValue(packPath, out doc!)) {
                    byte[]? bytes = Read(rom, packPath);
                    if (bytes is null) throw new EditError($"no actor pack {packPath} (set pack = \"<actor>\")");
                    doc = new Doc { RelativePath = packPath, Pack = Sarc.FromBinary(bytes) };
                    docs[packPath] = doc;
                }
                if (!doc.Members.TryGetValue(member, out var m)) {
                    if (!doc.Pack!.TryGetValue(member, out var data))
                        throw new EditError($"{packPath} has no {member}");
                    var b = Byml.FromBinary(data.ToArray(), out var endian, out var version);
                    m = (b, endian, version);
                    doc.Members[member] = m;
                }
                return m.Root;
            }
            default:
                throw new EditError($"unknown target namespace '{ns}' (rsdb, file, param)");
        }
    }

    // text:<locale|*>/<file>/<label>: sets the label's text in each language's message archive. The ops applied.
    static int ApplyText(ITkRom rom, Dictionary<string, Doc> docs, string target, JsonElement ops)
    {
        string[] parts = target[5..].Split('/');
        if (parts.Length < 3 || parts.Any(p => p is "" or "." or "..") || target.Contains('\\') || target[5..].Contains(':'))
            throw new EditError("text targets are text:<locale>/<message file>/<label>");
        string locale = parts[0], label = parts[^1], file = string.Join('/', parts[1..^1]) + ".msbt";
        if (locale != "*" && !Locales.Contains(locale)) throw new EditError($"unknown locale {locale}");
        int applied = 0, archives = 0;
        foreach (string loc in locale == "*" ? Locales : [locale]) {
            string rel = rom.CanonicalToRelativePath($"Mals/{loc}.Product.sarc",
                TkFileAttributes.HasZsExtension | TkFileAttributes.IsProductFile);
            if (!docs.TryGetValue(rel, out var doc)) {
                byte[]? bytes = Read(rom, rel);
                if (bytes is null) continue;   // the game has no such language
                doc = new Doc { RelativePath = rel, Pack = Sarc.FromBinary(bytes) };
                docs[rel] = doc;
            }
            if (!doc.Texts.TryGetValue(file, out var msbt)) {
                if (!doc.Pack!.TryGetValue(file, out var data)) throw new EditError($"{rel} has no {file}");
                msbt = Msbt.FromBinary(data.ToArray(), MsbtOptions.Default);
                doc.Texts[file] = msbt;
            }
            archives++;
            foreach (var op in ops.EnumerateArray()) {
                string kind = op[0].GetString()!, path = op[1].GetString()!;
                if (path != "Text") throw new EditError("a text edit sets Text");
                if (kind == "get") {
                    Console.WriteLine($"get text:{loc}/{file}/{label} = " + (msbt.TryGetValue(label, out var e) ? $"\"{e.Text}\"" : "(absent)"));
                    continue;
                }
                if (kind != "set" || op[2].ValueKind != JsonValueKind.String) throw new EditError("a text edit sets Text to a string");
                if (!msbt.TryGetValue(label, out var entry)) msbt[label] = entry = new MsbtEntry();
                entry.Text = KeepTags(entry.Text ?? "", op[2].GetString()!);
                doc.Changed = true;
                applied++;
            }
        }
        if (archives == 0) throw new EditError($"the game has no message archive for {locale}");
        return applied;
    }

    // A message's text keeps its control tags (<…/>: grammatical gender, formatting): the new plain text replaces what
    // lies between the leading tags and the trailing whitespace and tags. A value with tags of its own replaces it all.
    static string KeepTags(string old, string value)
    {
        if (value.Contains("/>")) return value;
        int lead = 0;
        while (lead < old.Length && old[lead] == '<') {
            int end = old.IndexOf("/>", lead, StringComparison.Ordinal);
            if (end < 0) break;
            lead = end + 2;
        }
        int tail = old.Length;
        for (;;) {
            int t = tail;
            while (t > lead && char.IsWhiteSpace(old[t - 1])) t--;
            if (t >= 2 && t > lead && old[t - 1] == '>' && old[t - 2] == '/') {
                int open = old.LastIndexOf('<', t - 1);
                if (open >= lead) {
                    tail = open;
                    continue;
                }
            }
            break;
        }
        int keep = tail;   // the whitespace just before the trailing tags stays with them
        while (keep > lead && char.IsWhiteSpace(old[keep - 1])) keep--;
        return old[..lead] + value + old[keep..];
    }

    static Doc LooseDoc(ITkRom rom, Dictionary<string, Doc> docs, string canonical, TkFileAttributes attrs)
        => TryLooseDoc(rom, docs, canonical, attrs) ?? throw new EditError($"no such file in the game: {canonical}");

    static Doc? TryLooseDoc(ITkRom rom, Dictionary<string, Doc> docs, string canonical, TkFileAttributes attrs)
    {
        string rel = rom.CanonicalToRelativePath(canonical, attrs);
        if (docs.TryGetValue(rel, out var doc)) return doc;
        byte[]? bytes = Read(rom, rel);
        if (bytes is null) return null;
        var root = Byml.FromBinary(bytes, out var endian, out var version);
        doc = new Doc { RelativePath = rel, Root = root, Endian = endian, Version = version };
        docs[rel] = doc;
        return doc;
    }

    // The file's decompressed bytes: from --over when it has the file, else vanilla. null when neither has it.
    static byte[]? Read(ITkRom rom, string rel)
    {
        if (over is not null) {
            string path = Inside(Path.Combine(over, "romfs"), rel);
            if (File.Exists(path)) {
                byte[] raw = File.ReadAllBytes(path);
                return TkZstd.IsCompressed(raw) ? rom.Zstd.Decompress(raw) : raw;
            }
        }
        using var vanilla = rom.GetVanilla(rel);
        return vanilla.IsEmpty ? null : vanilla.Segment.ToArray();
    }

    // root/rel, refusing anything that resolves outside root.
    static string Inside(string root, string rel)
    {
        string full = Path.GetFullPath(Path.Combine(root, rel)), r = Path.GetFullPath(root) + Path.DirectorySeparatorChar;
        if (!full.StartsWith(r, StringComparison.OrdinalIgnoreCase)) throw new EditError($"{rel} resolves outside {root}");
        return full;
    }

    // ---- operations

    // ---- records in arrays (a shop's goods, a drop table), matched by their fields

    static bool Matches(Byml rec, JsonElement match)
    {
        if (rec.Type != BymlNodeType.Map) return false;
        var map = rec.GetMap();
        foreach (var f in match.EnumerateObject()) {
            if (!map.TryGetValue(f.Name, out var v)) return false;
            bool eq = (f.Value.ValueKind, v.Type) switch {
                (JsonValueKind.String, BymlNodeType.String) => v.GetString() == f.Value.GetString(),
                (JsonValueKind.True or JsonValueKind.False, BymlNodeType.Bool) => v.GetBool() == f.Value.GetBoolean(),
                (JsonValueKind.Number, BymlNodeType.Int) => v.GetInt() == f.Value.GetDouble(),
                (JsonValueKind.Number, BymlNodeType.UInt32) => v.GetUInt32() == f.Value.GetDouble(),
                (JsonValueKind.Number, BymlNodeType.Int64) => v.GetInt64() == f.Value.GetDouble(),
                (JsonValueKind.Number, BymlNodeType.UInt64) => v.GetUInt64() == f.Value.GetDouble(),
                (JsonValueKind.Number, BymlNodeType.Float) => v.GetFloat() == (float)f.Value.GetDouble(),
                (JsonValueKind.Number, BymlNodeType.Double) => v.GetDouble() == f.Value.GetDouble(),
                _ => false
            };
            if (!eq) return false;
        }
        return true;
    }

    static BymlArray ArrayAt(Byml node, string path)
    {
        Byml? a = Get(node, path);
        if (a is null || a.Type != BymlNodeType.Array) throw new EditError($"{path}: not an array of records");
        return a.GetArray();
    }

    static Byml Select(Byml node, JsonElement sel)
    {
        string path = sel.GetProperty("path").GetString()!;
        var hits = ArrayAt(node, path).Where(r => Matches(r, sel.GetProperty("match"))).ToList();
        if (hits.Count != 1)
            throw new EditError($"select {path}: {hits.Count} records match {sel.GetProperty("match").GetRawText()} (need exactly one)");
        return hits[0];
    }

    static void Append(Byml node, string path, JsonElement record)
    {
        var arr = ArrayAt(node, path);
        var model = arr.FirstOrDefault(r => r.Type == BymlNodeType.Map)?.GetMap();
        var map = new BymlMap();
        foreach (var f in record.EnumerateObject())
            map[f.Name] = model is not null && model.TryGetValue(f.Name, out var like) ? Convert(f.Value, like.Type, $"{path}.{f.Name}")
                                                                                         : FromJson(f.Value, $"{path}.{f.Name}");
        arr.Add(new Byml(map));
    }

    static void Remove(Byml node, string path, JsonElement match)
    {
        var arr = ArrayAt(node, path);
        int n = arr.RemoveAll(r => Matches(r, match));
        if (n == 0) throw new EditError($"remove {path}: no record matches {match.GetRawText()}");
    }

    static Byml? Get(Byml node, string path)
    {
        Byml? cur = node;
        foreach (string part in path.Split('.')) {
            if (cur is null) return null;
            cur = Child(cur, part, path);
        }
        return cur;
    }

    static string Show(Byml? b) => b is null ? "(absent)" : b.Type switch {
        BymlNodeType.Int => $"{b.GetInt()} (Int)", BymlNodeType.UInt32 => $"{b.GetUInt32()} (UInt32)",
        BymlNodeType.Int64 => $"{b.GetInt64()} (Int64)", BymlNodeType.UInt64 => $"{b.GetUInt64()} (UInt64)",
        BymlNodeType.Float => $"{b.GetFloat()} (Float)", BymlNodeType.Double => $"{b.GetDouble()} (Double)",
        BymlNodeType.Bool => $"{b.GetBool()} (Bool)", BymlNodeType.String => $"\"{b.GetString()}\" (String)",
        BymlNodeType.Array => $"(Array, {b.GetArray().Count} items)",
        _ => $"({b.Type})"
    };

    static void Apply(Byml node, string op, string path, JsonElement value)
    {
        if (op == "append") {
            Append(node, path, value);
            return;
        }
        if (op == "remove") {
            Remove(node, path, value);
            return;
        }
        string[] parts = path.Split('.');
        Byml parent = node;
        for (int i = 0; i < parts.Length - 1; i++) parent = Child(parent, parts[i], path) ?? throw new EditError($"{path}: no {parts[i]}");
        string last = parts[^1];
        Byml? current = Child(parent, last, path);
        Byml result = op switch {
            "set" => current is null ? FromJson(value, path) : Convert(value, current.Type, path),
            "add" or "mul" => Arith(current ?? throw new EditError($"{path}: {op} needs an existing value"), op, value, path),
            _ => throw new EditError($"unknown operation '{op}'")
        };
        Store(parent, last, result, path);
    }

    static Byml? Child(Byml node, string key, string path)
    {
        switch (node.Type) {
            case BymlNodeType.Map:
                return node.GetMap().TryGetValue(key, out var v) ? v : null;
            case BymlNodeType.Array: {
                var arr = node.GetArray();
                if (!int.TryParse(key, out int i) || i < 0) throw new EditError($"{path}: '{key}' is not an array index");
                if (i >= arr.Count) throw new EditError($"{path}: index {i} is past the end ({arr.Count} items)");
                return arr[i];
            }
            default:
                throw new EditError($"{path}: '{key}' is inside a {node.Type}, not a map or array");
        }
    }

    static void Store(Byml parent, string key, Byml value, string path)
    {
        if (parent.Type == BymlNodeType.Map) parent.GetMap()[key] = value;
        else parent.GetArray()[int.Parse(key)] = value;
    }

    static bool IsInteger(JsonElement v) => v.ValueKind == JsonValueKind.Number && v.TryGetInt64(out _)
                                            && !v.GetRawText().Contains('.') && !v.GetRawText().Contains('e')
                                            && !v.GetRawText().Contains('E');

    static Byml FromJson(JsonElement v, string path) => v.ValueKind switch {
        JsonValueKind.True => new Byml(true),
        JsonValueKind.False => new Byml(false),
        JsonValueKind.String => new Byml(v.GetString()!),
        JsonValueKind.Number when IsInteger(v) => v.GetInt64() is >= int.MinValue and <= int.MaxValue
            ? new Byml((int)v.GetInt64()) : new Byml(v.GetInt64()),
        JsonValueKind.Number => new Byml(v.GetSingle()),
        _ => throw new EditError($"{path}: values are numbers, strings or booleans")
    };

    // The value as the type the game file uses there.
    static Byml Convert(JsonElement v, BymlNodeType type, string path)
    {
        switch (type) {
            case BymlNodeType.Bool:
                return v.ValueKind is JsonValueKind.True or JsonValueKind.False ? new Byml(v.GetBoolean())
                    : throw new EditError($"{path}: the game stores a Bool here");
            case BymlNodeType.String:
                return v.ValueKind == JsonValueKind.String ? new Byml(v.GetString()!)
                    : throw new EditError($"{path}: the game stores a String here");
            case BymlNodeType.Float or BymlNodeType.Double:
                if (v.ValueKind != JsonValueKind.Number) throw new EditError($"{path}: the game stores a number here");
                return type == BymlNodeType.Float ? new Byml(v.GetSingle()) : new Byml(v.GetDouble());
            case BymlNodeType.Int or BymlNodeType.UInt32 or BymlNodeType.Int64 or BymlNodeType.UInt64:
                if (!IsInteger(v)) throw new EditError($"{path}: the game stores a whole number ({type}) here");
                return Integer(v.GetInt64(), type, path);
            default:
                throw new EditError($"{path}: cannot set a {type} (only numbers, strings and booleans)");
        }
    }

    static Byml Integer(long x, BymlNodeType type, string path) => type switch {
        BymlNodeType.Int when x is >= int.MinValue and <= int.MaxValue => new Byml((int)x),
        BymlNodeType.UInt32 when x is >= 0 and <= uint.MaxValue => new Byml((uint)x),
        BymlNodeType.Int64 => new Byml(x),
        BymlNodeType.UInt64 when x >= 0 => new Byml((ulong)x),
        _ => throw new EditError($"{path}: {x} does not fit a {type}")
    };

    static Byml Arith(Byml cur, string op, JsonElement v, string path)
    {
        if (v.ValueKind != JsonValueKind.Number) throw new EditError($"{path}: {op} takes a number");
        double by = v.GetDouble();
        switch (cur.Type) {
            case BymlNodeType.Float: return new Byml((float)(op == "add" ? cur.GetFloat() + by : cur.GetFloat() * by));
            case BymlNodeType.Double: return new Byml(op == "add" ? cur.GetDouble() + by : cur.GetDouble() * by);
            case BymlNodeType.Int or BymlNodeType.UInt32 or BymlNodeType.Int64 or BymlNodeType.UInt64: {
                double x = cur.Type switch {
                    BymlNodeType.Int => cur.GetInt(), BymlNodeType.UInt32 => cur.GetUInt32(),
                    BymlNodeType.Int64 => cur.GetInt64(), _ => cur.GetUInt64()
                };
                if (op == "add" && !IsInteger(v)) throw new EditError($"{path}: the game stores a whole number here; add a whole number");
                double r = Math.Round(op == "add" ? x + by : x * by, MidpointRounding.AwayFromZero);
                if (r is < long.MinValue or > long.MaxValue) throw new EditError($"{path}: result out of range");
                return Integer((long)r, cur.Type, path);
            }
            default:
                throw new EditError($"{path}: {op} needs a number, the game stores a {cur.Type} here");
        }
    }
}
