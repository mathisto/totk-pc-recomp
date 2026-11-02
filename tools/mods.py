"""Manage, merge and deploy the player's mods into an install as one RomFS overlay (runtime/romfs.c, TOTK_ROMFS_PATCHES).

    python tools/mods.py init --install DIR --base FILE --update FILE [--keys DIR] [--profile FILE]
    python tools/mods.py list
    python tools/mods.py add ZIP|7Z|RAR|TKCL|FOLDER [--name NAME] [--pos N] [--variant TEXT] [--disabled]
    python tools/mods.py enable|disable|remove NAME
    python tools/mods.py order NAME POS
    python tools/mods.py check [--all]
    python tools/mods.py apply [--play] [--install DIR] [--romfs DIR]
    python tools/mods.py verify --install DIR [--romfs DIR]
    python tools/mods.py deploy --install DIR --merged DIR [--res WxH]
    python tools/mods.py undeploy --install DIR

The profile (--profile, default $TOTK_MODS_PROFILE or C:\\Games\\TotK-Mods\\profile.json) is a JSON file with the
install, the merge inputs (keys, base game, update, game version) and the mods in load order, each with an enabled
flag. Paths in it are relative to its folder. Later mods win: each is merged over the ones before it. add unpacks an
archive into downloads/<name> (a new folder; a mod folder from elsewhere is copied into curated/<name>) and finds the
mod's root, the folder with romfs/ or exefs/ (--variant picks one when the archive holds several). check compares the
game versions the mods' file names target with the install's RomFS. apply runs the version check, merges the enabled
mods with tools/modmerge (totk-modmerge, TkSharp: BYML, packs, SARC, MSBT, GameData, RSDB and the RESTBL are merged
format by format, not replaced), prints every file two or more mods change and how it was merged, deploys the result
and checks the overlay as totk.exe's start check does; --play then starts totk.exe. With no mod enabled it undeploys.
apply pins itself and the merge to four CPUs (--cpus, default 0xF00) and sets PYTHON_CPU_COUNT and
DOTNET_PROCESSOR_COUNT to 4: an unthrottled merge once froze the PC. The game started by --play is not pinned.

deploy: --merged is the output of tools/modmerge: romfs/<path> files merged from the player's mods by TkSharp, plus
exefs/ and exefs_patches/ for code mods and patches, which this step does not apply yet and lists instead.

totk.exe loads one patch overlay (romfs-patches, or romfs-patches-<w>x<h> for a game resolution of another shape) and
stops when two overlays replace the same file. So deploy writes <install>/mods-overlay with both: the port's own patch
set for the game resolution, then every merged mod file, and a manifest.tsv that lists each with its size, its
SHA-256 and the SHA-256 of the RomFS original it replaces ("-" for a file the RomFS lacks). It then sets romfs_patches
in totk.ini to that folder. When the mods replace files the port patches (a UI mod rewriting the Common layout archive,
say), deploy makes the port's patch set again on top of the mods' versions (tools/make_romfs_patches.py --source, in the
same shape: the game resolution's layout or the Xbox prompts) and uses that in place of both. That step needs Python
3.14 (--patch-python, or patch_python in the profile).

The RomFS is <install>/romfs, or the romfs line of totk.ini, or --romfs (an install that runs on another install's
RomFS, like a development copy). The port's patch folders and totk-install.json come from the install, or from the
folder that holds that RomFS when the install has none. The originals' hashes come from romfs/romfs.bin through
romfs/index.json, read only for the files the mods replace. undeploy removes the folder and the totk.ini line.
Standard library only.
"""
import argparse
import ctypes
import datetime
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
from collections import Counter
from pathlib import Path

OVERLAY = 'mods-overlay'
INI_KEY = 'romfs_patches'
CHUNK = 8 << 20
REPO = Path(__file__).resolve().parent.parent
MODMERGE = REPO / 'tools' / 'modmerge'
MODMERGE_DLL = MODMERGE / 'bin' / 'Release' / 'net10.0' / 'totk-modmerge.dll'
DEFAULT_PROFILE = Path(os.environ.get('TOTK_MODS_PROFILE') or r'C:\Games\TotK-Mods\profile.json')
ARCHIVES = ('.zip', '.7z', '.rar')
VERSION_TOKEN = re.compile(r'\.Product\.(\d{3})(?=\.)')
# How TkSharp merges a file two mods change, by the merger class tools/modmerge reports.
METHODS = {
    'BymlMerger': 'BYML merged key by key',
    'GameDataMerger': 'GameData flags merged',
    'RsdbTagMerger': 'RSDB tags merged',
    'RsdbRowMerger': 'RSDB rows merged',
    'PackMerger': 'pack merged file by file',
    'SarcMerger': 'archive merged file by file',
    'MsbtMerger': 'text merged label by label',
    'BntxMerger': 'textures merged',
    'BfresMcMerger': 'model merged',
}


def sha256_file(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        while block := f.read(CHUNK):
            h.update(block)
    return h.hexdigest()


def sha256_range(image, offset, size):
    h = hashlib.sha256()
    image.seek(offset)
    left = size
    while left:
        block = image.read(min(CHUNK, left))
        if not block:
            raise SystemExit(f'romfs.bin ends early at {offset + size - left}')
        h.update(block)
        left -= len(block)
    return h.hexdigest()


def read_manifest(folder):
    """{romfs path: manifest line} of an overlay folder's manifest.tsv."""
    lines = {}
    manifest = folder / 'manifest.tsv'
    for line in manifest.read_text(encoding='utf-8').splitlines():
        if line and not line.startswith('#'):
            lines[line.split('\t', 1)[0]] = line
    return lines


def ini_value(ini_text, key):
    m = re.search(rf'^\s*{key}\s*=\s*(.*?)\s*$', ini_text, re.M | re.I)
    return m[1] if m else None


def set_ini(ini, key, value):
    """Sets key = value in totk.ini (value None removes the line), keeping every other line as it is."""
    text = ini.read_text(encoding='utf-8') if ini.is_file() else ''
    pattern = re.compile(rf'^\s*{key}\s*=.*(?:\r?\n|$)', re.M | re.I)
    text = pattern.sub('', text)
    if value is not None:
        line = f'{key} = {value}\n'
        m = re.search(r'^\[files\]\s*$', text, re.M | re.I)
        if m:
            text = text[:m.end()] + '\n' + line.rstrip('\n') + text[m.end():]
        else:
            text = text.rstrip('\n') + '\n[files]\n' + line
    ini.write_text(text, encoding='utf-8', newline='\n')


def ini_text(install):
    ini = install / 'totk.ini'
    return ini.read_text(encoding='utf-8') if ini.is_file() else ''


def game_res(install, override):
    if override:
        return override.lower()
    value = ini_value(ini_text(install), 'output_res')
    return value.lower() if value else None


def romfs_dir(install, override=None):
    """The RomFS folder totk.exe reads: --romfs, else the romfs line of totk.ini, else <install>/romfs."""
    value = override or ini_value(ini_text(install), 'romfs')
    path = Path(value) if value else install / 'romfs'
    return (path if path.is_absolute() else install / path).resolve()


def game_root(install, romfs):
    """The folder with the port's patch sets and totk-install.json: the install, or the one that holds its RomFS."""
    if (install / 'totk-install.json').is_file() or not (romfs.parent / 'totk-install.json').is_file():
        return install
    return romfs.parent


def load_index(romfs):
    index = json.loads((romfs / 'index.json').read_text(encoding='utf-8'))
    if index.get('status') != 'pass':
        raise SystemExit(f'{romfs / "index.json"} is not a verified extraction')
    return {f['path']: f for f in index['files']}


def install_version(root):
    record = root / 'totk-install.json'
    return json.loads(record.read_text(encoding='utf-8'))['version'] if record.is_file() else None


# ---- deploy

def deploy_overlay(install, merged, res=None, patch_python=None, romfs=None, files=None):
    """Writes <install>/mods-overlay from a modmerge output and points totk.ini at it. Returns the summary."""
    install, merged = install.resolve(), merged.resolve()
    romfs = romfs_dir(install, romfs)
    root = game_root(install, romfs)
    mod_romfs = merged / 'romfs'
    if not mod_romfs.is_dir():
        raise SystemExit(f'{mod_romfs} missing: run totk-modmerge first')

    res = game_res(install, res)
    port = root / f'romfs-patches-{res}' if res else None
    if not (port and (port / 'manifest.tsv').is_file()):
        port = root / 'romfs-patches'
    port_lines = read_manifest(port) if (port / 'manifest.tsv').is_file() else {}

    files = files or load_index(romfs)

    mod_files = sorted((p.relative_to(mod_romfs).as_posix(), p) for p in mod_romfs.rglob('*') if p.is_file())
    junk = [rel for rel, _ in mod_files if Path(rel).name.lower() in ('desktop.ini', 'thumbs.db', '.ds_store')]
    mod_files = [(rel, p) for rel, p in mod_files if rel not in junk]
    port_dir = {rel: port for rel in port_lines}
    clashes = [rel for rel, _ in mod_files if rel in port_lines]
    repatched = []
    if clashes:
        port_lines, port_dir = repatch(patch_python, install, root, romfs, port, mod_romfs, port_lines, port_dir)
        repatched = sorted(rel for rel in clashes if port_dir[rel] != port)
        left = [rel for rel in clashes if port_dir[rel] == port]
        if left:
            raise SystemExit('these mod files replace files the port patches, and patching them again did not cover '
                             'them:\n  ' + '\n  '.join(left))
        mod_files = [(rel, p) for rel, p in mod_files if rel not in port_lines]

    out = install / OVERLAY
    if out.exists():
        shutil.rmtree(out)
    out.mkdir()

    lines = []
    for rel, line in port_lines.items():
        target = out / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(port_dir[rel] / rel, target)
        lines.append(line)

    replaced = added = 0
    with (romfs / 'romfs.bin').open('rb') as image:
        for rel, src in mod_files:
            target = out / rel
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, target)
            entry = files.get(rel)
            if entry:
                original = sha256_range(image, entry['offset'], entry['size'])
                replaced += 1
            else:
                original = '-'
                added += 1
            lines.append('\t'.join([rel, str(target.stat().st_size), sha256_file(target), original]))

    (out / 'manifest.tsv').write_text('# path\tsize\tsha256\toriginal_sha256 (tools/mods.py deploy)\n'
                                      + ''.join(line + '\n' for line in lines), encoding='utf-8', newline='\n')
    set_ini(install / 'totk.ini', INI_KEY, str(out))
    if (install / f'{OVERLAY}.repatch').exists():
        shutil.rmtree(install / f'{OVERLAY}.repatch')

    skipped = sorted(p.relative_to(merged).as_posix() for d in ('exefs', 'exefs_patches') if (merged / d).is_dir()
                     for p in (merged / d).rglob('*') if p.is_file())
    return dict(status='pass', overlay=str(out), port_patches=port.name, port_files=len(port_lines),
                repatched_on_mods=repatched, replaced=replaced, added=added, junk_skipped=junk,
                code_not_applied=skipped)


def repatch(patch_python, install, root, romfs, port, mod_romfs, port_lines, port_dir):
    """Makes the port's patch set again with the mods' versions of its files as input (make_romfs_patches.py --source),
    in the same shape as `port` (its game resolution or the Xbox prompts). Returns the manifest lines and source
    folder of each patched file, the new ones in place of the port's."""
    python = patch_python or (sys.executable if sys.version_info >= (3, 14) else None)
    if not python:
        raise SystemExit('mod files replace files the port patches; patching them again needs Python 3.14 '
                         '(compression.zstd): pass --patch-python, e.g. the setup\'s python\\python.exe or .venv-py314')
    version = 'v' + install_version(root).replace('.', '')
    shape = port.name.removeprefix('romfs-patches').removeprefix('-')
    work = install / f'{OVERLAY}.repatch'
    if work.exists():
        shutil.rmtree(work)
    command = [str(python), '-I', str(Path(__file__).with_name('make_romfs_patches.py')), '--version', version,
               '--romfs', str(romfs), '--source', str(mod_romfs), '--out', str(work)]
    if shape == 'xbox':
        command.append('--xbox-prompts')
    elif shape:
        command += ['--aspect', shape]
    done = subprocess.run(command, capture_output=True, text=True)
    if done.returncode:
        raise SystemExit(f'make_romfs_patches.py --source failed ({done.returncode}):\n{done.stdout}{done.stderr}')
    lines, dirs = dict(port_lines), dict(port_dir)
    for rel, line in read_manifest(work).items():
        lines[rel], dirs[rel] = line, work
    return lines, dirs


def undeploy_overlay(install):
    install = install.resolve()
    out = install / OVERLAY
    if out.exists():
        shutil.rmtree(out)
    set_ini(install / 'totk.ini', INI_KEY, None)
    return dict(status='pass', removed=str(out))


def verify_overlay(install, romfs=None, files=None):
    """Checks the install's patch overlays the way totk.exe's start does (runtime/romfs.c overlay_load): every
    manifest line well formed, each file there with its size and SHA-256, each original's SHA-256 matching the RomFS,
    "-" only for files the RomFS lacks, and no file replaced twice across romfs_patches and romfs_mods. Returns the
    problems (empty when the start check passes) and the number of files checked."""
    install = install.resolve()
    romfs = romfs_dir(install, romfs)
    text = ini_text(install)
    dirs = [ini_value(text, key) for key in (INI_KEY, 'romfs_mods')]
    dirs = [Path(d) if Path(d).is_absolute() else install / d for d in dirs if d and d != '0']
    files = files or load_index(romfs)
    line_re = re.compile(r'^([^\t]+)\t(\d+)\t([0-9a-f]{64})\t([0-9a-f]{64}|-)$')
    problems, seen, checked = [], {}, 0
    with (romfs / 'romfs.bin').open('rb') as image:
        for folder in dirs:
            manifest = folder / 'manifest.tsv'
            if not manifest.is_file():
                continue
            for line in manifest.read_text(encoding='utf-8').splitlines():
                if not line or line.startswith('#'):
                    continue
                m = line_re.match(line)
                if not m:
                    problems.append(f'{manifest}: bad line {line[:80]!r}')
                    continue
                name, size, sha, orig = m[1], int(m[2]), m[3], m[4]
                if name.startswith('/') or '\\' in name or '..' in name.split('/'):
                    problems.append(f'{manifest}: bad path {name}')
                    continue
                entry = files.get(name)
                if entry and orig == '-':
                    problems.append(f'{manifest} adds {name}, which the RomFS already has')
                elif not entry and orig != '-':
                    problems.append(f'{manifest} names {name}, which the RomFS lacks')
                elif entry and sha256_range(image, entry['offset'], entry['size']) != orig:
                    problems.append(f'the original of {name} does not match {manifest}')
                path = folder / name
                if not path.is_file():
                    problems.append(f'{path} missing')
                elif path.stat().st_size != size:
                    problems.append(f'{path} has {path.stat().st_size} bytes, its manifest says {size}')
                elif sha256_file(path) != sha:
                    problems.append(f'{path} does not match its SHA-256')
                if name in seen:
                    problems.append(f'{seen[name]} and {folder} replace the same file {name}')
                seen[name] = folder
                checked += 1
    return problems, checked


# ---- profile

def profile_path(args):
    return (args.profile or DEFAULT_PROFILE).resolve()


def load_profile(path):
    if not path.is_file():
        raise SystemExit(f'{path} missing: make one with `mods.py init` (or pass --profile)')
    profile = json.loads(path.read_text(encoding='utf-8'))
    profile.setdefault('mods', [])
    return profile


def save_profile(path, profile):
    path.write_text(json.dumps(profile, indent=1, ensure_ascii=False) + '\n', encoding='utf-8', newline='\n')


def resolve(path, base):
    path = Path(os.path.expandvars(os.path.expanduser(str(path))))
    return path if path.is_absolute() else base / path


def relative(path, base):
    try:
        return path.relative_to(base).as_posix()
    except ValueError:
        return str(path)


def find_mod(profile, name):
    """Index of the mod named `name`: its exact name, else the one name it begins or contains."""
    mods = profile['mods']
    key = name.lower()
    for test in (lambda n: n == key, lambda n: n.startswith(key), lambda n: key in n):
        hits = [i for i, m in enumerate(mods) if test(m['name'].lower())]
        if len(hits) == 1:
            return hits[0]
        if len(hits) > 1:
            raise SystemExit(f'"{name}" matches ' + ', '.join(mods[i]['name'] for i in hits))
    raise SystemExit(f'no mod named "{name}" (mods.py list)')


def slug(text):
    return re.sub(r'[^a-z0-9]+', '-', text.lower()).strip('-') or 'mod'


def tar():
    """Windows' own bsdtar, which lists and unpacks .zip, .7z and .rar and refuses absolute and .. paths."""
    system = Path(os.environ.get('SystemRoot', r'C:\Windows')) / 'System32' / 'tar.exe'
    return str(system) if system.is_file() else 'tar'


def mod_roots(folder):
    """The mod roots under folder: each folder with a romfs/ or exefs/ child, and each .tkcl."""
    roots = {p.parent for p in folder.rglob('*') if p.is_dir() and p.name.lower() in ('romfs', 'exefs')}
    roots |= {p for p in folder.rglob('*.tkcl') if p.is_file()}
    return sorted(r for r in roots if not any(o != r and o in r.parents for o in roots))


def pick_root(folder, variant):
    roots = mod_roots(folder)
    if variant:
        roots = [r for r in roots if variant.lower() in relative(r, folder).lower()]
    if len(roots) == 1:
        return roots[0]
    if not roots:
        raise ValueError(f'no mod root {"matching " + repr(variant) + " " if variant else ""}in {folder}: an '
                         'emulator-format mod has a romfs/ or exefs/ folder (or is a .tkcl)')
    raise ValueError(f'{folder} holds {len(roots)} variants; pick one with --variant TEXT:\n  '
                     + '\n  '.join(relative(r, folder) or '.' for r in roots))


# ---- version check

def mod_file_list(path):
    """Paths of the files in a mod (folder or archive), or None for a format this cannot list (.tkcl)."""
    if path.is_dir():
        return [p.relative_to(path).as_posix() for p in path.rglob('*') if p.is_file()]
    if path.suffix.lower() not in ARCHIVES:
        return None
    done = subprocess.run([tar(), '-tf', str(path)], capture_output=True, text=True, encoding='utf-8',
                          errors='replace')
    if done.returncode:
        return None
    return [line for line in done.stdout.splitlines() if line and not line.endswith('/')]


def vanilla_versions(files):
    """{RomFS path with its version as {v}: the versions the RomFS has for it}, for the version-named files."""
    versions = {}
    for path in files:
        if m := VERSION_TOKEN.search(path):
            versions.setdefault(path[:m.start(1)] + '{v}' + path[m.end(1):], set()).add(m[1])
    return versions


def dotted(token):
    return '.'.join(token)


def check_mod(path, vanilla, version):
    """Compares the game versions a mod targets with the install's (version, '1.2.1'). A mod's version-named RomFS
    files (RSDB/Tag.Product.112..., Mals/USen.Product.120...) count when none of the copies it ships of a file is one
    the install's RomFS has: a mod for several versions ships one per version and TkSharp takes the best. RESTBLs do
    not count, TkSharp makes its own. A mod that names no version in its files is checked by its file name (..._112.zip).
    Returns a warning, or None."""
    names = mod_file_list(path)
    families = {}
    for name in names or []:
        m = re.search(r'(?:^|/)romfs/(.+)$', name, re.I)
        v = VERSION_TOKEN.search(m[1]) if m else None
        if not v or m[1].endswith(('.rsizetable', '.rsizetable.zs')):
            continue
        key = m[1][:v.start(1)] + '{v}' + m[1][v.end(1):]
        if key in vanilla:
            families.setdefault(key, set()).add(v[1])
    foreign = {key: tokens for key, tokens in families.items() if not tokens & vanilla[key]}
    if foreign:
        made = Counter(max(tokens) for tokens in foreign.values())
        key, tokens = next(iter(sorted(foreign.items())))
        example = key.replace('{v}', max(tokens))
        return (f'{len(foreign)} of its {len(families)} version-named files are made for '
                + ', '.join(f'{dotted(t)} ({n})' for t, n in sorted(made.items()))
                + f', e.g. {example} where {version} has {key.replace("{v}", max(vanilla[key]))}; TkSharp moves what '
                'it merges (RSDB, GameData, BYML, text) to this version, but files it copies whole stay as made')
    if families:
        return None
    install = version.replace('.', '')
    named = {m[1].replace('.', '') for m in re.finditer(r'(?<!\d)(?<!\d\.)(1\.?[0-4]\.?\d)(?!\d)(?!\.\d)', path.name)}
    named &= {'100', '110', '111', '112', '120', '121', '140', '141', '142', '143'}
    if named and install not in named:
        return (f'its file name says {", ".join(dotted(t) for t in sorted(named))} and its files name no version: '
                'check that it works on ' + version)
    return None


def version_warnings(profile, base, mods, files, version):
    vanilla = vanilla_versions(files)
    warnings = []
    for mod in mods:
        if warning := check_mod(resolve(mod['path'], base), vanilla, version):
            warnings.append((mod['name'], warning))
    for name, warning in warnings:
        print(f'  ! {name}: {warning} (install {version})')
    if not warnings:
        print(f'  every mod matches the install\'s game version {version}, or names none')
    return warnings


# ---- CPU pinning

def pin_cpus(mask):
    """Pins this process, and so everything it starts, to the CPUs in mask (the highest four available when none of
    them are). Returns the mask it had, for unpin_cpus."""
    os.environ['PYTHON_CPU_COUNT'] = os.environ['DOTNET_PROCESSOR_COUNT'] = '4'
    if os.name != 'nt' or not mask:
        return None
    from ctypes import wintypes
    k32 = ctypes.WinDLL('kernel32', use_last_error=True)
    k32.GetCurrentProcess.restype = wintypes.HANDLE
    k32.GetProcessAffinityMask.argtypes = [wintypes.HANDLE, ctypes.POINTER(ctypes.c_size_t),
                                           ctypes.POINTER(ctypes.c_size_t)]
    k32.SetProcessAffinityMask.argtypes = [wintypes.HANDLE, ctypes.c_size_t]
    process, system = ctypes.c_size_t(), ctypes.c_size_t()
    k32.GetProcessAffinityMask(k32.GetCurrentProcess(), ctypes.byref(process), ctypes.byref(system))
    use = mask & system.value
    if not use:
        cpus = [b for b in range(system.value.bit_length()) if system.value >> b & 1][-4:]
        use = sum(1 << b for b in cpus)
    k32.SetProcessAffinityMask(k32.GetCurrentProcess(), use)
    return process.value, use, k32


def unpin_cpus(pinned):
    for key in ('PYTHON_CPU_COUNT', 'DOTNET_PROCESSOR_COUNT'):
        os.environ.pop(key, None)
    if pinned:
        old, _, k32 = pinned
        k32.SetProcessAffinityMask(k32.GetCurrentProcess(), old)


def cpu_list(mask):
    return ','.join(str(b) for b in range(mask.bit_length()) if mask >> b & 1)


# ---- modmerge

def dotnet():
    """The first dotnet (PATH, then the per-user install) with a .NET 10 or newer SDK: modmerge targets net10.0, and a
    machine-wide older SDK on PATH cannot build it."""
    local = Path(os.environ.get('LOCALAPPDATA', '')) / 'Microsoft' / 'dotnet' / 'dotnet.exe'
    for exe in (shutil.which('dotnet'), str(local) if local.is_file() else None):
        if not exe:
            continue
        try:
            sdks = subprocess.run([exe, '--list-sdks'], capture_output=True, text=True, timeout=30).stdout
        except (OSError, subprocess.TimeoutExpired):
            continue
        if any(line.split('.')[0].isdigit() and int(line.split('.')[0]) >= 10 for line in sdks.splitlines()):
            return exe
    return None


def build_modmerge(dn):
    """Builds tools/modmerge when its program is missing or older than its source."""
    sources = [MODMERGE / 'Program.cs', MODMERGE / 'DataEdits.cs', MODMERGE / 'Catalog.cs', MODMERGE / 'TotkModMerge.csproj']
    if MODMERGE_DLL.is_file() and MODMERGE_DLL.stat().st_mtime >= max(s.stat().st_mtime for s in sources):
        return
    if not (MODMERGE / 'TkSharp' / 'TkSharp.sln').is_file():
        raise SystemExit('tools/modmerge/TkSharp is empty: git submodule update --init tools/modmerge/TkSharp')
    print('building tools/modmerge ...', flush=True)
    done = subprocess.run([dn, 'build', str(MODMERGE), '-c', 'Release', '-m:2', '-nologo', '-v:quiet'],
                          capture_output=True, text=True,
                          env=dict(os.environ, DOTNET_CLI_TELEMETRY_OPTOUT='1', DOTNET_NOLOGO='1'))
    if done.returncode:
        errors = [line for line in done.stdout.splitlines() if ' error ' in line][:10]
        raise SystemExit('tools/modmerge build failed:\n' + '\n'.join(errors or [done.stdout[-2000:]]))


def run_modmerge(profile, base, mods, out, data, log):
    merge = profile['merge']
    dn = dotnet()
    if not dn:
        raise SystemExit('dotnet missing: tools/modmerge needs the .NET 10 SDK')
    build_modmerge(dn)
    command = [dn, str(MODMERGE_DLL), '--keys', str(resolve(merge.get('keys', '~/.switch'), base)),
               '--base', str(resolve(merge['base'], base)), '--update', str(resolve(merge['update'], base)),
               '--version', merge['version'], '--data', str(data), '--out', str(out),
               '--lang', merge.get('lang', 'USen'), '--report', str(out / 'report.json')]
    command += [str(resolve(m['path'], base)) for m in mods]
    log.parent.mkdir(parents=True, exist_ok=True)
    with log.open('w', encoding='utf-8') as f:
        done = subprocess.run(command, stdout=f, stderr=subprocess.STDOUT,
                              env=dict(os.environ, DOTNET_CLI_TELEMETRY_OPTOUT='1', DOTNET_NOLOGO='1'))
    if done.returncode:
        tail = log.read_text(encoding='utf-8', errors='replace').splitlines()[-15:]
        raise SystemExit(f'totk-modmerge failed ({done.returncode}), log {log}:\n  ' + '\n  '.join(tail))
    return json.loads((out / 'report.json').read_text(encoding='utf-8'))


def canonical(rel):
    """A RomFS path as TkSharp names it in its changelogs: no version, no .zs."""
    return VERSION_TOKEN.sub('.Product', rel).removesuffix('.zs').removesuffix('.mc')


def print_conflicts(report, mods, verbose):
    conflicts = report['conflicts']
    names = [m['name'] for m in mods]
    replaced = [c for c in conflicts if c['method'] == 'replace']
    merged = [c for c in conflicts if c['method'] != 'replace']
    print(f'conflicts: {len(conflicts)} file{"s" * (len(conflicts) != 1)} changed by two or more mods '
          f'({len(merged)} merged, {len(replaced)} replaced whole)')
    by_method = Counter(c['method'] for c in merged)
    for method, n in by_method.most_common():
        print(f'  {n:5}  {METHODS.get(method, method)}')
    shown = merged if verbose else merged[:25]
    for c in shown:
        print(f'  merged    {c["file"]}: {" < ".join(names[i] for i in c["mods"])} '
              f'({METHODS.get(c["method"], c["method"])})')
    if len(shown) < len(merged):
        print(f'  ... {len(merged) - len(shown)} more merged (--verbose lists them all)')
    for c in replaced:
        print(f'  REPLACED  {c["file"]}: {names[c["mods"][-1]]} wins over '
              f'{", ".join(names[i] for i in c["mods"][:-1])} (TkSharp cannot merge this format; reorder to change '
              'the winner)')


def report_patches(report, mods):
    for mod, entry in zip(mods, report['mods']):
        for patch in entry['patches']:
            if not patch['applies']:
                print(f'  ! {mod["name"]}: ExeFS patch for build {patch["build_id"]} skipped: this game\'s main is '
                      f'{report["build_id"]}, so it was made for another version')


# ---- commands

def cmd_init(args):
    path = profile_path(args)
    if path.exists():
        raise SystemExit(f'{path} exists')
    path.parent.mkdir(parents=True, exist_ok=True)
    install = args.install.resolve()
    version = install_version(game_root(install, romfs_dir(install, args.romfs))) or args.version
    profile = dict(format=1, install=str(install),
                   merge=dict(keys=args.keys, base=str(args.base), update=str(args.update), version=version,
                              lang='USen', data='.tkdata', out='merged', log='merge.log'),
                   mods=[])
    if args.romfs:
        profile['romfs'] = str(args.romfs)
    save_profile(path, profile)
    print(f'wrote {path} (game version {version})')


def cmd_list(args):
    path = profile_path(args)
    profile = load_profile(path)
    mods = profile['mods']
    if not mods:
        print(f'{path}: no mods (mods.py add)')
        return
    width = max(len(m['name']) for m in mods)
    print(f'{path}: {sum(m.get("enabled", True) for m in mods)} of {len(mods)} enabled, install '
          f'{profile.get("install")}')
    for i, m in enumerate(mods, 1):
        state = 'on ' if m.get('enabled', True) else 'off'
        missing = '' if resolve(m['path'], path.parent).exists() else '  (missing)'
        print(f'  {i:2}  {state}  {m["name"]:<{width}}  {m["path"]}{missing}')
    print('later mods win where two change the same data; apply merges what TkSharp can merge and lists the rest')


def cmd_add(args):
    path = profile_path(args)
    profile = load_profile(path)
    base = path.parent
    src = args.source.resolve()
    if not src.exists():
        raise SystemExit(f'{src} missing')
    name = slug(args.name or (src.stem if src.is_file() else src.name))
    if any(m['name'].lower() == name for m in profile['mods']):
        raise SystemExit(f'a mod named {name} is already in the profile (--name gives another)')
    made = None
    if src.is_file():
        ext = src.suffix.lower()
        if ext not in ARCHIVES + ('.tkcl',):
            raise SystemExit(f'{src.name}: add takes a .zip, .7z, .rar, .tkcl or a mod folder')
        made = dest = base / 'downloads' / name
        if dest.exists():
            raise SystemExit(f'{dest} exists: pass --name for another folder')
        dest.mkdir(parents=True)
        if ext == '.tkcl':
            shutil.copy2(src, dest / src.name)
        else:
            done = subprocess.run([tar(), '-xf', str(src), '-C', str(dest)], capture_output=True, text=True)
            if done.returncode:
                shutil.rmtree(dest)
                raise SystemExit(f'could not unpack {src}: {done.stderr.strip()}')
    elif base in src.parents:
        dest = src
    else:
        made = dest = base / 'curated' / name
        if dest.exists():
            raise SystemExit(f'{dest} exists: pass --name for another folder')
        shutil.copytree(src, dest)
    try:
        root = pick_root(dest, args.variant)
    except ValueError as e:
        if made:
            shutil.rmtree(made)
        raise SystemExit(str(e))
    entry = dict(name=name, path=relative(root, base), enabled=not args.disabled, source=str(src),
                 added=datetime.date.today().isoformat())
    pos = len(profile['mods']) if args.pos is None else max(0, min(len(profile['mods']), args.pos - 1))
    profile['mods'].insert(pos, entry)
    save_profile(path, profile)
    print(f'added {name} at {pos + 1} of {len(profile["mods"])}: {entry["path"]}'
          + ('' if entry['enabled'] else ' (disabled)'))
    install = resolve(args.install or profile['install'], base).resolve()
    romfs = romfs_dir(install, args.romfs or profile.get('romfs'))
    if (romfs / 'index.json').is_file():
        files = load_index(romfs)
        version_warnings(profile, base, [entry], files,
                         install_version(game_root(install, romfs)) or profile['merge']['version'])


def cmd_switch(args):
    path = profile_path(args)
    profile = load_profile(path)
    i = find_mod(profile, args.name)
    mod = profile['mods'][i]
    if args.command == 'remove':
        del profile['mods'][i]
        save_profile(path, profile)
        print(f'removed {mod["name"]} from the profile; its files stay in {resolve(mod["path"], path.parent)}')
        return
    mod['enabled'] = args.command == 'enable'
    save_profile(path, profile)
    print(f'{mod["name"]} {args.command}d (apply to deploy)')


def cmd_order(args):
    path = profile_path(args)
    profile = load_profile(path)
    mods = profile['mods']
    i = find_mod(profile, args.name)
    pos = len(mods) if args.pos == 'last' else 1 if args.pos == 'first' else int(args.pos)
    mod = mods.pop(i)
    pos = max(1, min(len(mods) + 1, pos))
    mods.insert(pos - 1, mod)
    save_profile(path, profile)
    print(f'{mod["name"]} is now {pos} of {len(mods)} (later wins)')


def setup(args, profile, path):
    base = path.parent
    install = resolve(args.install or profile['install'], base).resolve()
    if not install.is_dir():
        raise SystemExit(f'install {install} missing')
    romfs = romfs_dir(install, args.romfs or profile.get('romfs'))
    if not (romfs / 'romfs.bin').is_file():
        raise SystemExit(f'{romfs} has no romfs.bin: pass --romfs, or set romfs in totk.ini or the profile')
    version = install_version(game_root(install, romfs))
    want = profile['merge']['version']
    if version and version != want:
        raise SystemExit(f'the install runs game version {version}, the profile merges for {want}: set merge.version '
                         'and merge.update in the profile to that version\'s update')
    return base, install, romfs, version or want


def cmd_check(args):
    path = profile_path(args)
    profile = load_profile(path)
    base, install, romfs, version = setup(args, profile, path)
    mods = [m for m in profile['mods'] if args.all or m.get('enabled', True)]
    print(f'version check against {install} (game version {version}):')
    version_warnings(profile, base, mods, load_index(romfs), version)


def cmd_apply(args):
    path = profile_path(args)
    profile = load_profile(path)
    base, install, romfs, version = setup(args, profile, path)
    mods = [m for m in profile['mods'] if m.get('enabled', True)]
    missing = [m['name'] for m in mods if not resolve(m['path'], base).exists()]
    if missing:
        raise SystemExit('these enabled mods are missing their files: ' + ', '.join(missing))
    print(f'{path}: {len(mods)} of {len(profile["mods"])} mods enabled; install {install} (game version {version})')
    if not mods and not args.data_edits and not args.extra_mod:
        print(json.dumps(undeploy_overlay(install)))
        return launch(install, args, None) if args.play else None

    pinned = pin_cpus(args.cpus)
    if pinned:
        print(f'pinned to CPUs {cpu_list(pinned[1])}, PYTHON_CPU_COUNT=DOTNET_PROCESSOR_COUNT=4')
    files = load_index(romfs)
    print('version check:')
    version_warnings(profile, base, mods, files, version)

    merge = profile['merge']
    work = args.work.resolve() if args.work else None
    out = work / 'merged' if work else resolve(merge.get('out', 'merged'), base)
    data = work / '.tkdata' if work else resolve(merge.get('data', '.tkdata'), base)
    log = work / 'merge.log' if work else resolve(merge.get('log', 'merge.log'), base)
    for extra in args.extra_mod or []:   # Fuse packages' asset and TKCL content (fuse apply), after the profile's mods
        mods = mods + [dict(name='fuse:' + extra.name, path=str(extra.resolve()), enabled=True)]
    if args.data_edits:   # Fuse data edits: applied to vanilla by totk-modmerge's data mode, merged last as one more mod
        edited = out.parent / (out.name + '-fuse-data')
        run_data_edits(profile, base, args.data_edits.resolve(), edited, log.with_name(log.stem + '-data.log'))
        if (edited / 'romfs').is_dir():
            mods = mods + [dict(name='fuse-data', path=str(edited), enabled=True)]
    print(f'merging {len(mods)} mods with totk-modmerge (log {log}) ...', flush=True)
    t0 = time.monotonic()
    report = run_modmerge(profile, base, mods, out, data, log)
    print(f'merged in {time.monotonic() - t0:.0f} s into {out}: vanilla {dotted(str(report["game_version"]))}, '
          f'main {report["build_id"]}')
    report_patches(report, mods)
    print_conflicts(report, mods, args.verbose)

    print('deploying ...', flush=True)
    patch_python = args.patch_python or (str(resolve(profile['patch_python'], base))
                                         if profile.get('patch_python') else None)
    result = deploy_overlay(install, out, args.res, patch_python, romfs, files)
    for rel in result['repatched_on_mods']:
        owners = [m['name'] for m, e in zip(mods, report['mods']) if canonical(rel) in e['files']]
        print(f'  port patch {result["port_patches"]} re-applied on top of {", ".join(owners) or "a mod"}: {rel}')
    total = result['port_files'] + result['replaced'] + result['added']
    print(f'  {result["overlay"]}: {total} files ({result["replaced"]} replaced, {result["added"]} added, '
          f'{result["port_files"]} from the port\'s {result["port_patches"]}); totk.ini {INI_KEY} set')
    if result['code_not_applied']:
        print(f'  not applied (ExeFS code, which deploy does not install yet): {", ".join(result["code_not_applied"])}')

    problems, checked = verify_overlay(install, romfs, files)
    if problems:
        raise SystemExit(f'the overlay fails totk.exe\'s start check:\n  ' + '\n  '.join(problems[:20]))
    print(f'verified {checked} files as totk.exe\'s start check does: sizes, hashes and originals match')
    if args.play:
        launch(install, args, pinned)


def run_data_edits(profile, base, edits, out, log):
    """totk-modmerge data: the edits file `fuse deploy --data-out` wrote, applied to the vanilla files, into out."""
    merge = profile['merge']
    dn = dotnet()
    if not dn:
        raise SystemExit('dotnet missing: tools/modmerge needs the .NET 10 SDK')
    build_modmerge(dn)
    command = [dn, str(MODMERGE_DLL), 'data', '--keys', str(resolve(merge.get('keys', '~/.switch'), base)),
               '--base', str(resolve(merge['base'], base)), '--update', str(resolve(merge['update'], base)),
               '--version', merge['version'], '--edits', str(edits), '--out', str(out)]
    log.parent.mkdir(parents=True, exist_ok=True)
    with log.open('w', encoding='utf-8') as f:
        done = subprocess.run(command, stdout=f, stderr=subprocess.STDOUT,
                              env=dict(os.environ, DOTNET_CLI_TELEMETRY_OPTOUT='1', DOTNET_NOLOGO='1'))
    text = log.read_text(encoding='utf-8', errors='replace').splitlines()
    if done.returncode:
        raise SystemExit(f'totk-modmerge data failed ({done.returncode}), log {log}:\n  ' + '\n  '.join(text[-10:]))
    print('fuse data edits: ' + (text[-1] if text else 'done'))


def cmd_catalog(args):
    """totk-modmerge catalog: the names Fuse data edits can target, read from the player's game into one JSON file."""
    path = profile_path(args)
    profile = load_profile(path)
    base, install, romfs, version = setup(args, profile, path)
    merge = profile['merge']
    dn = dotnet()
    if not dn:
        raise SystemExit('dotnet missing: tools/modmerge needs the .NET 10 SDK')
    build_modmerge(dn)
    command = [dn, str(MODMERGE_DLL), 'catalog', '--keys', str(resolve(merge.get('keys', '~/.switch'), base)),
               '--base', str(resolve(merge['base'], base)), '--update', str(resolve(merge['update'], base)),
               '--version', merge['version'], '--index', str(romfs / 'index.json'), '--out', str(args.out.resolve())]
    if args.no_params:
        command.append('--no-params')
    done = subprocess.run(command, env=dict(os.environ, DOTNET_CLI_TELEMETRY_OPTOUT='1', DOTNET_NOLOGO='1'))
    return done.returncode


def launch(install, args, pinned):
    exe = install / 'totk.exe'
    if not exe.is_file():
        raise SystemExit(f'{exe} missing')
    unpin_cpus(pinned)
    flags = getattr(subprocess, 'DETACHED_PROCESS', 0) | getattr(subprocess, 'CREATE_NEW_PROCESS_GROUP', 0)
    game = subprocess.Popen([str(exe)], cwd=install, creationflags=flags, close_fds=True)
    print(f'started {exe} (pid {game.pid})')


def cmd_verify(args):
    problems, checked = verify_overlay(args.install, args.romfs)
    print(json.dumps(dict(status='fail' if problems else 'pass', checked=checked, problems=problems), indent=1))
    return 1 if problems else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--profile', type=Path, help=f'profile file (default {DEFAULT_PROFILE})')
    sub = ap.add_subparsers(dest='command', required=True)

    i = sub.add_parser('init', help='write a new profile')
    i.add_argument('--install', type=Path, required=True)
    i.add_argument('--base', type=Path, required=True, help='the base game (.xci or .nsp)')
    i.add_argument('--update', type=Path, required=True, help='the update matching the install\'s game version')
    i.add_argument('--keys', default='~/.switch', help='folder with prod.keys and title.keys')
    i.add_argument('--version', default='1.2.1', help='game version when the install has no totk-install.json')
    i.add_argument('--romfs', type=Path)
    sub.add_parser('list', help='show the mods in load order')
    a = sub.add_parser('add', help='unpack a mod into the profile\'s folder and add it')
    a.add_argument('source', type=Path)
    a.add_argument('--name')
    a.add_argument('--pos', type=int, help='load-order position, 1 = first (lowest priority); default last')
    a.add_argument('--variant', help='text in the path of the variant to use when the mod has several')
    a.add_argument('--disabled', action='store_true')
    for name in ('enable', 'disable', 'remove'):
        s = sub.add_parser(name, help=f'{name} a mod' + (' (leaves its files)' if name == 'remove' else ''))
        s.add_argument('name')
    o = sub.add_parser('order', help='move a mod to a load-order position (1 = first, last wins)')
    o.add_argument('name')
    o.add_argument('pos', help='1-based position, or first / last')
    c = sub.add_parser('check', help='compare the mods\' game versions with the install\'s')
    c.add_argument('--all', action='store_true', help='disabled mods too')
    p = sub.add_parser('apply', help='merge the enabled mods, deploy them and verify the overlay')
    p.add_argument('--play', action='store_true', help='then start totk.exe')
    p.add_argument('--verbose', action='store_true', help='list every merged conflict')
    p.add_argument('--work', type=Path, help='folder for the merge output, TkSharp\'s store and the log (default: the '
                                             'profile\'s merge.out, merge.data and merge.log)')
    p.add_argument('--cpus', type=lambda s: int(s, 0), default=0xF00, help='CPU mask to pin to (0 = none)')
    p.add_argument('--extra-mod', type=Path, action='append', help='one more mod (a folder with romfs/, or a .tkcl), '
                                                                     'merged after the profile\'s mods (repeatable)')
    p.add_argument('--data-edits', type=Path, help='a Fuse data-edits file (fuse deploy --data-out): applied to the '
                                                   'vanilla files and merged last')
    p.add_argument('--res', help='game resolution WxH (default: output_res of totk.ini)')
    p.add_argument('--patch-python', help='Python 3.14 for make_romfs_patches.py (default: the profile\'s '
                                          'patch_python, else this Python when it is 3.14 or newer)')
    k = sub.add_parser('catalog', help='write the names Fuse data edits can target (tables, rows, params, text)')
    k.add_argument('--out', type=Path, required=True)
    k.add_argument('--no-params', action='store_true', help='skip the actor packs (much faster)')
    v = sub.add_parser('verify',help='check an install\'s overlays as totk.exe\'s start does')
    v.add_argument('--install', type=Path, required=True)
    d = sub.add_parser('deploy', help='deploy a modmerge output')
    d.add_argument('--install', type=Path, required=True)
    d.add_argument('--merged', type=Path, required=True)
    d.add_argument('--res', help='game resolution WxH (default: output_res of totk.ini)')
    d.add_argument('--patch-python', help='Python 3.14 for make_romfs_patches.py when mods replace files the port '
                                          'patches (default: this Python when it is 3.14 or newer)')
    u = sub.add_parser('undeploy')
    u.add_argument('--install', type=Path, required=True)
    for s in (a, c, p, k):
        s.add_argument('--install', type=Path, help='the install (default: the profile\'s)')
    for s in (a, c, p, v, d, k):
        s.add_argument('--romfs', type=Path, help='the RomFS folder (default: the profile\'s romfs, else romfs in '
                                                  'totk.ini, else <install>/romfs)')
    args = ap.parse_args()

    if args.command == 'deploy':
        print(json.dumps(deploy_overlay(args.install, args.merged, args.res, args.patch_python, args.romfs), indent=1))
    elif args.command == 'undeploy':
        print(json.dumps(undeploy_overlay(args.install)))
    else:
        handlers = dict(init=cmd_init, list=cmd_list, add=cmd_add, enable=cmd_switch, disable=cmd_switch,
                        remove=cmd_switch, order=cmd_order, check=cmd_check, apply=cmd_apply, verify=cmd_verify,
                        catalog=cmd_catalog)
        return handlers[args.command](args)


if __name__ == '__main__':
    sys.exit(main())
