// Fuse ExeFS patches (T4 step P0, fuse docs/decisions/0006): the .fzpatch files `fuse deploy` compiled from a
// package's IPS and pchtxt patches, written into the module images after loading and before relocation, the way a
// Switch loader patches an NSO before rtld relocates it (a relocated word overwrites a patch to it, there as here).
//
// Only files listed in TOTK_FUSE_SCRIPTS/.fuse-deploy.json with a matching SHA-256 are read, packages turned off in the
// Mods tab (.fuse-disabled) are skipped, and a file applies whole or not at all. Bytes in .text change the image (a
// mod's signature scan sees them) but not the recompiled code, which never reads its own instructions: that is step
// P1, and the log says so.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "fuse_hooks.h"
#include "host.h"
#include "platform/platform.h"

enum { MAX_RANGES = 4096, MAX_DISABLED = 64 };

typedef struct { int mod; uint32_t off, len; char pkg[72]; } Range;

static const FzPatchModule *pmods;
static int npmods;
static Range ranges[MAX_RANGES];
static int nranges;
static char disabled[MAX_DISABLED][72];
static int ndisabled;
static int files_applied, bytes_code, bytes_other;

static void read_disabled(const char *dir) {   // <dir>/.fuse-disabled: one package id per line
    char path[1024];
    snprintf(path, sizeof path, "%s/.fuse-disabled", dir);
    uint64_t size = 0;
    char *text = (char *)plat_file_read(path, &size);
    if (!text) return;
    for (uint64_t at = 0; at < size && ndisabled < MAX_DISABLED;) {
        uint64_t end = at;
        while (end < size && text[end] != '\n') end++;
        uint64_t e = end;
        while (e > at && (text[e - 1] == '\r' || text[e - 1] == ' ')) e--;
        if (e > at && e - at < sizeof disabled[0]) {
            memcpy(disabled[ndisabled], text + at, e - at);
            disabled[ndisabled++][e - at] = 0;
        }
        at = end + 1;
    }
    free(text);
}

static int is_disabled(const char *id) {
    for (int i = 0; i < ndisabled; i++)
        if (!strcmp(disabled[i], id)) return 1;
    return 0;
}

static int hexval(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

// The module a build id names: the id is zero-padded to the module id's 64 hex digits, as a Switch loader reads it.
static int module_for(const char *bid, size_t n) {
    if (n < 16 || n > 64) return -1;
    for (int i = 0; i < npmods; i++) {
        int same = 1;
        for (size_t k = 0; k < 64 && same; k++) {
            char want = k < n ? bid[k] : '0';
            int a = hexval(want), b = hexval(pmods[i].bid[k]);
            same = a >= 0 && a == b;
        }
        if (same) return i;
    }
    return -1;
}

typedef struct { uint32_t off; uint32_t len; const char *hex; } Rec;

// One deployed file: parse and check every record, then write them all.
static void apply_file(const char *id, const char *path, const char *sha, void *user) {
    const char *dir = (const char *)user;
    char full[1200], shown[700];
    snprintf(full, sizeof full, "%s/%s/%s", dir, id, path);
    snprintf(shown, sizeof shown, "%s/%s", id, path);
    if (is_disabled(id)) {
        rt_log("fuse: %s is turned off in game (.fuse-disabled); patch not applied", shown);
        return;
    }
    uint64_t size = 0;
    char *text = (char *)plat_file_read(full, &size);
    if (!text) {
        rt_log("fuse: cannot read %s", shown);
        return;
    }
    if (!fz_file_sha256_hex(text, size, sha)) {
        rt_log("fuse: %s changed since it was deployed (deployed sha256 %s); not applied", shown, sha);
        free(text);
        return;
    }
    char *buf = realloc(text, size + 1);   // NUL-terminated copy to parse in place
    if (!buf) { free(text); return; }
    buf[size] = 0;
    int mod = -1, nrec = 0, cap = 0, bad = 0, line = 0;
    Rec *recs = NULL;
    for (char *p = buf, *nl; p && *p && !bad; p = nl ? nl + 1 : NULL) {
        nl = strchr(p, '\n');
        if (nl) *nl = 0;
        line++;
        size_t n = strlen(p);
        while (n && (p[n - 1] == '\r' || p[n - 1] == ' ')) p[--n] = 0;
        if (!n || *p == '#') continue;
        if (!strncmp(p, "bid ", 4)) {
            mod = module_for(p + 4, n - 4);
            if (mod < 0) {
                rt_log("fuse: %s is for build id %s, which no loaded module has (another game version?); not applied",
                       shown, p + 4);
                bad = 1;
            }
            continue;
        }
        if (mod < 0) break;   // a record before the bid line: reported below
        char *sp = strchr(p, ' ');
        if (!sp || sp == p || sp - p > 8) { bad = 2; break; }
        *sp = 0;
        uint32_t off = 0;
        for (char *h = p; *h; h++) {
            int v = hexval(*h);
            if (v < 0) { bad = 2; break; }
            off = off << 4 | (uint32_t)v;
        }
        size_t hn = strlen(sp + 1);
        if (bad || !hn || hn % 2) { bad = 2; break; }
        for (size_t k = 0; k < hn; k++)
            if (hexval(sp[1 + k]) < 0) { bad = 2; break; }
        if (bad) break;
        uint64_t len = hn / 2, end = (uint64_t)off + len;
        if (end > pmods[mod].data_end) {
            rt_log("fuse: %s line %d writes +%#llx..+%#llx, past %s's image; not applied", shown, line,
                   (unsigned long long)off, (unsigned long long)end, pmods[mod].name);
            bad = 1;
            break;
        }
        if (nrec == cap) {
            cap = cap ? cap * 2 : 64;
            Rec *r = realloc(recs, cap * sizeof *recs);
            if (!r) { bad = 1; break; }
            recs = r;
        }
        recs[nrec++] = (Rec){off, (uint32_t)len, sp + 1};
    }
    if (bad == 2) rt_log("fuse: %s line %d is not <hex offset> <hex bytes>; not applied", shown, line);
    if (!bad && mod < 0) rt_log("fuse: %s has no bid line; not applied", shown);
    if (bad || mod < 0) {
        free(recs);
        free(buf);
        return;
    }
    const FzPatchModule *m = &pmods[mod];
    uint64_t code = 0, other = 0;
    for (int i = 0; i < nrec; i++) {
        const Rec *r = &recs[i];
        for (int k = 0; k < nranges; k++)   // a later package wins; say whose bytes it replaces
            if (ranges[k].mod == mod && r->off < ranges[k].off + ranges[k].len && ranges[k].off < r->off + r->len &&
                strcmp(ranges[k].pkg, id))
                rt_log("fuse: %s overwrites bytes %s patched at %s+%#x", shown, ranges[k].pkg, m->name, r->off);
        if (nranges < MAX_RANGES) {
            ranges[nranges] = (Range){mod, r->off, r->len, ""};
            snprintf(ranges[nranges++].pkg, sizeof ranges[0].pkg, "%s", id);
        }
        uint8_t *dst = (uint8_t *)(uintptr_t)(m->base + r->off);
        for (uint32_t k = 0; k < r->len; k++) dst[k] = (uint8_t)(hexval(r->hex[2 * k]) << 4 | hexval(r->hex[2 * k + 1]));
        uint64_t in_code = r->off >= m->text_end ? 0 : (r->off + r->len > m->text_end ? m->text_end - r->off : r->len);
        code += in_code;
        other += r->len - in_code;
    }
    files_applied++;
    bytes_code += (int)code;
    bytes_other += (int)other;
    rt_log("fuse: patched %s from %s: %d records, %llu bytes of data", m->name, shown, nrec, (unsigned long long)other);
    if (code)
        rt_log("fuse: %s: %llu bytes fall in %s's code; they are in the image (signature scans see them) but the "
               "recompiled code does not run them yet (decision 0006, step P1)", shown, (unsigned long long)code, m->name);
    free(recs);
    free(buf);
}

void fz_patches_apply(const FzPatchModule *mods, int n) {
    const char *dir = cfg_get("TOTK_FUSE_SCRIPTS");
    if (!dir || !*dir) return;
    pmods = mods;
    npmods = n;
    read_disabled(dir);
    if (fz_deployed_files(dir, ".fzpatch", apply_file, (void *)dir) > 0)
        rt_log("fuse: %d patch files applied (%d bytes of data, %d bytes in code)", files_applied, bytes_other, bytes_code);
}
