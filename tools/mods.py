"""Deploy merged mods into an install as one RomFS overlay (runtime/romfs.c, TOTK_ROMFS_PATCHES).

    python tools/mods.py deploy --install DIR --merged DIR [--res WxH]
    python tools/mods.py undeploy --install DIR

--merged is the output of tools/modmerge (totk-modmerge): romfs/<path> files merged from the player's mods by TkSharp,
plus exefs/ and exefs_patches/ for code mods and patches, which this step does not apply yet and lists instead.

totk.exe loads one patch overlay (romfs-patches, or romfs-patches-<w>x<h> for a game resolution of another shape) and
stops when two overlays replace the same file. So deploy writes <install>/mods-overlay with both: the port's own patch
set for the game resolution, then every merged mod file, and a manifest.tsv that lists each with its size, its
SHA-256 and the SHA-256 of the RomFS original it replaces ("-" for a file the RomFS lacks). It then sets romfs_patches
in totk.ini to that folder. A mod file that replaces a file the port patches stops the deploy: the port's patch would
have to be made again on top of the mod's version (tools/make_romfs_patches.py), which this step does not do yet.

The originals' hashes come from romfs/romfs.bin through romfs/index.json, read only for the files the mods replace.
undeploy removes the folder and the totk.ini line. Standard library only.
"""
import argparse
import hashlib
import json
import re
import shutil
import sys
from pathlib import Path

OVERLAY = 'mods-overlay'
INI_KEY = 'romfs_patches'
CHUNK = 8 << 20


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


def game_res(install, override):
    if override:
        return override.lower()
    ini = install / 'totk.ini'
    value = ini_value(ini.read_text(encoding='utf-8'), 'output_res') if ini.is_file() else None
    return value.lower() if value else None


def deploy(args):
    install, merged = args.install.resolve(), args.merged.resolve()
    romfs_dir = install / 'romfs'
    mod_romfs = merged / 'romfs'
    if not mod_romfs.is_dir():
        raise SystemExit(f'{mod_romfs} missing: run totk-modmerge first')

    res = game_res(install, args.res)
    port = install / f'romfs-patches-{res}' if res else None
    if not (port and (port / 'manifest.tsv').is_file()):
        port = install / 'romfs-patches'
    port_lines = read_manifest(port) if (port / 'manifest.tsv').is_file() else {}

    index = json.loads((romfs_dir / 'index.json').read_text(encoding='utf-8'))
    if index.get('status') != 'pass':
        raise SystemExit(f'{romfs_dir / "index.json"} is not a verified extraction')
    files = {f['path']: f for f in index['files']}

    mod_files = sorted((p.relative_to(mod_romfs).as_posix(), p) for p in mod_romfs.rglob('*') if p.is_file())
    junk = [rel for rel, _ in mod_files if Path(rel).name.lower() in ('desktop.ini', 'thumbs.db', '.ds_store')]
    mod_files = [(rel, p) for rel, p in mod_files if rel not in junk]
    clashes = [rel for rel, _ in mod_files if rel in port_lines]
    if clashes:
        raise SystemExit('these mod files replace files the port patches itself, which deploy cannot combine yet:\n  '
                         + '\n  '.join(clashes))

    out = install / OVERLAY
    if out.exists():
        shutil.rmtree(out)
    out.mkdir()

    lines = []
    for rel, line in port_lines.items():
        target = out / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(port / rel, target)
        lines.append(line)

    replaced = added = 0
    with (romfs_dir / 'romfs.bin').open('rb') as image:
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

    skipped = sorted(p.relative_to(merged).as_posix() for d in ('exefs', 'exefs_patches') if (merged / d).is_dir()
                     for p in (merged / d).rglob('*') if p.is_file())
    print(json.dumps(dict(status='pass', overlay=str(out), port_patches=port.name, port_files=len(port_lines),
                          replaced=replaced, added=added, junk_skipped=junk, code_not_applied=skipped), indent=1))


def undeploy(args):
    install = args.install.resolve()
    out = install / OVERLAY
    if out.exists():
        shutil.rmtree(out)
    set_ini(install / 'totk.ini', INI_KEY, None)
    print(json.dumps(dict(status='pass', removed=str(out))))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest='command', required=True)
    d = sub.add_parser('deploy')
    d.add_argument('--install', type=Path, required=True)
    d.add_argument('--merged', type=Path, required=True)
    d.add_argument('--res', help='game resolution WxH (default: output_res of totk.ini)')
    u = sub.add_parser('undeploy')
    u.add_argument('--install', type=Path, required=True)
    args = ap.parse_args()
    deploy(args) if args.command == 'deploy' else undeploy(args)


if __name__ == '__main__':
    sys.exit(main())
