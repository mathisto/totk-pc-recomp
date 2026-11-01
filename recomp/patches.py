"""Code patch table for the generator: guest instructions replaced in the lifted C, switchable at run time.

Lifted code cannot be byte-patched when the game runs, so a patch is applied when the C is generated. A patch file
(gen.py --patches) has these lines; `#` starts a comment, fields are separated by whitespace:

  set  <name> <env> <default>               a patch set: a run-time switch <env>=0/1, <default> when it is unset
  site <set> <module> <address> <original> <replacement> [<replacement> ...]
  hook <set> <module> <address> <original> <callback>
  data <set> <module> <address> <original> <replacement>

  spin <module> <head> <back-edge> [<back-edge> ...]

<address> is a module offset (hex), <original> the instruction word the module has there (checked; a mismatch stops
the generator), each <replacement> an instruction word (hex) or `@page:x<n>`, which is `adrp x<n>, <patch page>`
encoded for the site's address. The patch page is the first 4 KiB page after main's image (layout.py leaves the rest
of the 2 MiB-aligned module slot free); the runtime commits it and owns its contents. Words in use, by byte offset:
+0..15 the frame-rate step (runtime/frame_rate.c), +16..55 the output, swapchain and scene sizes
(runtime/resolution.c res_init), +56..59 the camera's aspect ratio (runtime/resolution.c res_init, set aspect), +60..63
the field-of-view factor (runtime/resolution.c res_fov_live, set fov), +92..95 the sun shadow map size
(runtime/resolution.c shadow_init, set shadow), +256..263 the stamina delay timer (runtime/game_hooks.c, set
tsq). A new set takes words outside these.

The replacement words are lifted at the site's address in place of the original instruction, and the generated C reads
`if (rt_patch_on[<set>]) { replacement } else { original }`: one byte load and a branch per executed site. Replacement
words must not branch except a final `bl` (lifted with the site's return address); the original must not be a branch
other than `bl`. So the function's control flow, labels and jump tables are the original's. The generator writes
patches.h (set names, switches, the page address) next to layout.h for the runtime.

A `hook` line runs C before a guest instruction: the generated C reads `if (rt_patch_on[<set>]) { <callback>(c); }`
ahead of the instruction at <address> (after its label), with the function's registers synced to the Cpu around the
call, and then lifts the instruction itself unchanged. <callback> is a runtime function `void <callback>(Cpu *c)`
(runtime/game_hooks.c); it may read and change any register, the flags excepted, and call guest code with rt_call.
This is what an exlaunch inline hook does on the console, so code mods that install those (`exefs/subsdk*`) port as
one hook line and one C function each. <original> is the instruction word at <address> (checked).

A `data` line replaces a 32-bit word outside .text (rodata or data, which the lifted C reads at run time; .text is
lifted, so a word there needs a `site`): when the set is on, runtime/patches.c rt_patches_init writes <replacement>
over <original> once the modules are loaded, after checking that the module holds <original> there. patches.h lists
them as RT_PATCH_DATA(X).

A `spin` line names a spin wait found by tools/spin_waits.py: a loop with head <head> that only another thread can end,
and the direct branches back to it (module offsets). Each back-edge (checked: a b, b.cond, cbz / cbnz or tbz / tbnz to
<head>) is lifted as `rt_spin_gate(c, <head>); goto <head>`, which keeps every register and only adds time
(runtime/kernel.c kern_spin_gate).
"""
from pathlib import Path

PAGE = 0x1000


class PatchError(ValueError):
    pass


def adrp(pc, target, rd):
    imm = (target >> 12) - (pc >> 12)
    if not -(1 << 20) <= imm < (1 << 20):
        raise PatchError(f'{pc:#x}: patch page {target:#x} out of ADRP range')
    imm &= 0x1FFFFF
    return 0x90000000 | (imm & 3) << 29 | (imm >> 2) << 5 | rd


class Table:
    """sets: [(name, env, default)]; sites: {module: {offset: (set index, [word | ('page', reg)], original)}}; page."""

    def __init__(self, sets=(), sites=None, page=0, spins=None, hooks=None, data=None):
        self.sets = list(sets)
        self.sites = sites or {}
        self.page = page
        self.spins = spins or {}   # {module: {back-edge offset: head offset}}
        self.hooks = hooks or {}   # {module: {offset: (set index, callback, original)}}
        self.data = data or {}     # {module: {offset: (set index, original, replacement)}}
        self.bases = {}            # {module: base}, for the absolute addresses of RT_PATCH_DATA

    def hooks_for(self, name, base):
        """{absolute address: (set index, callback)}."""
        return {base + off: (k, cb) for off, (k, cb, _) in self.hooks.get(name, {}).items()}

    def spins_for(self, name, base):
        """{absolute back-edge address: absolute head address}."""
        return {base + b: base + h for b, h in self.spins.get(name, {}).items()}

    def for_module(self, name, base):
        """{absolute address: (set index, [words])} with @page resolved."""
        out = {}
        for off, (k, repl, _) in self.sites.get(name, {}).items():
            a = base + off
            out[a] = (k, [adrp(a, self.page, w[1]) if isinstance(w, tuple) else w for w in repl])
        return out


def load(path, mods):
    """Table from a patch file; mods is layout() output [(name, base, size)]."""
    sets, index, sites, spins, hooks, data = [], {}, {}, {}, {}, {}
    bases = {n: (b, s) for n, b, s in mods}
    for no, raw in enumerate(Path(path).read_text().splitlines(), 1):
        f = raw.split('#', 1)[0].split()
        if not f:
            continue
        where = f'{path}:{no}'
        if f[0] == 'set' and len(f) == 4:
            if f[1] in index:
                raise PatchError(f'{where}: set {f[1]} defined twice')
            index[f[1]] = len(sets)
            sets.append((f[1], f[2], int(f[3])))
        elif f[0] == 'site' and len(f) >= 6:
            if f[1] not in index:
                raise PatchError(f'{where}: unknown set {f[1]}')
            if f[2] not in bases:
                raise PatchError(f'{where}: module {f[2]} not in the layout')
            off, orig = int(f[3], 16), int(f[4], 16)
            repl = []
            for w in f[5:]:
                if w.startswith('@page:x'):
                    repl.append(('page', int(w[7:])))
                else:
                    repl.append(int(w, 16))
            mod = sites.setdefault(f[2], {})
            if off in mod:
                raise PatchError(f'{where}: {f[2]}+{off:#x} patched twice')
            mod[off] = (index[f[1]], repl, orig)
        elif f[0] in ('hook', 'data') and len(f) == 6:
            if f[1] not in index:
                raise PatchError(f'{where}: unknown set {f[1]}')
            if f[2] not in bases:
                raise PatchError(f'{where}: module {f[2]} not in the layout')
            off, orig = int(f[3], 16), int(f[4], 16)
            if f[0] == 'hook':
                if not f[5].isidentifier():
                    raise PatchError(f'{where}: callback {f[5]!r} is not a C identifier')
                mod = hooks.setdefault(f[2], {})
                if off in mod:
                    raise PatchError(f'{where}: {f[2]}+{off:#x} hooked twice')
                mod[off] = (index[f[1]], f[5], orig)
            else:
                mod = data.setdefault(f[2], {})
                if off in mod or off & 3:
                    raise PatchError(f'{where}: {f[2]}+{off:#x} patched twice or not word aligned')
                mod[off] = (index[f[1]], orig, int(f[5], 16))
        elif f[0] == 'spin' and len(f) >= 4:
            if f[1] not in bases:
                raise PatchError(f'{where}: module {f[1]} not in the layout')
            mod, head = spins.setdefault(f[1], {}), int(f[2], 16)
            for b in f[3:]:
                b = int(b, 16)
                if b in mod:
                    raise PatchError(f'{where}: {f[1]}+{b:#x} is a back-edge twice')
                mod[b] = head
        else:
            raise PatchError(f'{where}: cannot parse {raw!r}')
    base, size = bases['main'][0], bases['main'][1]
    page = (base + size + PAGE - 1) & ~(PAGE - 1)
    nxt = min((b for _, b, _ in mods if b > base), default=None)
    if nxt is not None and page + PAGE > nxt:
        raise PatchError(f'no room for the patch page after main ({page:#x})')
    table = Table(sets, sites, page, spins, hooks, data)
    table.bases = {n: b for n, (b, _) in bases.items()}
    return table


def check(table, mod, name):
    """Stops on a site whose original word differs from the module's."""
    for off, (_, _, want) in table.sites.get(name, {}).items():
        a = mod.base + off
        if not mod.in_text(a):
            raise PatchError(f'{name}+{off:#x}: patch site outside .text')
        have = mod.word(a)
        if have != want:
            raise PatchError(f'{name}+{off:#x}: module has {have:08x}, the patch expects {want:08x}')
    for off, (_, _, want) in table.hooks.get(name, {}).items():
        a = mod.base + off
        if not mod.in_text(a):
            raise PatchError(f'{name}+{off:#x}: hook outside .text')
        have = mod.word(a)
        if have != want:
            raise PatchError(f'{name}+{off:#x}: module has {have:08x}, the hook expects {want:08x}')
    for off, (_, want, _) in table.data.get(name, {}).items():
        a = mod.base + off
        if mod.in_text(a) or not mod.in_image(a, 4):
            raise PatchError(f'{name}+{off:#x}: data patch inside .text (lifted code: use a site) or outside the image')
        have = mod.read(a, 4)
        if have != want:
            raise PatchError(f'{name}+{off:#x}: module has {have:08x}, the data patch expects {want:08x}')
    for b, head in table.spins.get(name, {}).items():
        a = mod.base + b
        if not mod.in_text(a) or branch_target(mod.word(a), a) != mod.base + head:
            raise PatchError(f'{name}+{b:#x}: not a direct branch to the spin wait head {name}+{head:#x}')


def branch_target(w, a):
    """Target of the direct branch w at a (b, b.cond, cbz / cbnz, tbz / tbnz), or None."""
    if w >> 26 == 0x05:
        return a + ((((w & 0x3FFFFFF) ^ 0x2000000) - 0x2000000) << 2)
    if w & 0xFF000010 == 0x54000000 or w & 0x7E000000 == 0x34000000:
        return a + ((((w >> 5) & 0x7FFFF) ^ 0x40000) - 0x40000 << 2)
    if w & 0x7E000000 == 0x36000000:
        return a + ((((w >> 5) & 0x3FFF) ^ 0x2000) - 0x2000 << 2)
    return None


def header_text(table, marker):
    lines = [f'// {marker}: code patch sets, generated by recomp/gen.py --patches. Do not edit or commit.',
             '#pragma once', '', f'#define RT_PATCH_COUNT {len(table.sets)}',
             f'#define RT_PATCH_PAGE {table.page:#x}ull',
             '// X(index, name, env switch, default)',
             '#define RT_PATCH_SETS(X) ' + ' '.join(f'X({i}, {n}, "{e}", {d})' for i, (n, e, d) in
                                                    enumerate(table.sets)), '']
    words = [(k, table.bases[m] + off, orig, repl) for m, offs in sorted(table.data.items())
             for off, (k, orig, repl) in sorted(offs.items())]
    lines += ['// X(set index, absolute address, original word, replacement word): recomp/patches.py `data` lines',
              '#define RT_PATCH_DATA(X) ' + ' '.join(f'X({k}, {a:#x}ull, {o:#010x}u, {r:#010x}u)'
                                                     for k, a, o, r in words), '']
    return '\n'.join(lines)
