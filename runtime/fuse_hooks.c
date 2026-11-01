// Fuse runtime hooks (spike). See fuse_hooks.h.
#include "fuse_hooks.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "dispatch.h"
#include "host.h"
#include "platform/platform.h"
#if __has_include("layout.h")
#include "layout.h"
#endif

#define FZ_BUCKETS 4096   // hook chains by function, power of two
#define FZ_DEPTH 8        // nested hooked calls on one thread before functions run unhooked

typedef struct FzHook {
    uint64_t addr;
    fz_before_fn before;
    fz_after_fn after;
    void *user;
    int priority;
    volatile int dead;     // removed (fz_unhook): skipped by readers, never freed while one may hold it
    struct FzHook *next;   // the next hook of any function in this bucket, by priority within a function
} FzHook;

static FzHook *bucket[FZ_BUCKETS];   // written under lock, published with release stores, read without it
static PlatMutex lock;
static int nhooks, gate_mode = 1;

// The function fz_enter is running the original body of: its FN_ENTER lets that one call through (set and read with
// no switch between, so a thread-local is enough).
static __thread uint64_t bypass;
// Per-context state in the Cpu's spare bytes, zeroed when a thread's or fiber's Cpu is made: a guest fiber can block
// inside a hooked function and resume on another OS thread, so the nesting depth follows the context.
typedef struct { int depth; } FzCtx;
_Static_assert(sizeof(FzCtx) <= sizeof(((Cpu *)0)->_pad), "Cpu spare bytes");
static FzCtx *ctx_of(Cpu *c) { return (FzCtx *)(void *)c->_pad; }
// For the crash report (fz_crash_note): the innermost hooked function on this thread, and the callback running.
static __thread uint64_t running_addr;
static __thread const void *running_cb;

static unsigned bucket_of(uint64_t addr) { return (unsigned)((addr >> 2) * 0x9E3779B1u) & (FZ_BUCKETS - 1); }

static FzHook *first_hook(uint64_t addr) {
    for (FzHook *h = __atomic_load_n(&bucket[bucket_of(addr)], __ATOMIC_ACQUIRE); h; h = h->next)
        if (h->addr == addr) return h;
    return NULL;
}

// Sets or clears the function's bit in its module's hook bitmap (the FN_ENTER test). Under lock.
// ---- patched entries (FZ_PATCH builds, cpu.h): a lifted function compiled with -fpatchable-function-entry=7,5 has
// five one-byte no-ops before its entry and a two-byte no-op (66 90) at it. Hooking it writes a jmp rel32 to a stub
// into the five bytes (nothing runs them yet), then turns the entry into "jmp -7" (EB F9) with one two-byte store, so
// a thread sees the no-op or the whole jump, never half of one. The stub loads its site into the second argument
// register and jumps to fz_enter_patched, which runs the hooks around the body at entry + 2 (the code just past the
// no-op). Unhooking stores the no-op back. Sites and stubs are never freed: a thread may still be inside one.
typedef struct FzSite {
    uint64_t addr;            // the guest function
    uint8_t *entry;           // its lifted entry
    struct FzSite *next;      // all sites, under lock
} FzSite;

static FzSite *sites;
static uint8_t *stub_pool, *stub_end;
enum { STUB_SIZE = 32, STUB_POOL = 1 << 16 };

static void fz_enter_patched(Cpu *c, FzSite *s);

#if defined(__x86_64__) && (defined(_WIN32) || defined(__linux__))
static const uint8_t NOP5[5] = {0x90, 0x90, 0x90, 0x90, 0x90}, NOP2[2] = {0x66, 0x90}, JMP_BACK[2] = {0xEB, 0xF9};

static FzSite *site_of(uint64_t addr) {   // under lock
    for (FzSite *o = sites; o; o = o->next)
        if (o->addr == addr) return o;
    return NULL;
}

// The entry has this build's patchable layout: five no-ops before it (or our jump, once we patched it), and the
// two-byte no-op (or our jump back) inside one cache line, so the flip is one atomic store.
static int patchable(uint64_t addr, const uint8_t *e) {
    if (((uintptr_t)e & 63) == 63) return 0;
    return !memcmp(e - 5, NOP5, 5) || (e[-5] == 0xE9 && site_of(addr));
}

// Writes the stub for s; returns it, or NULL when no executable memory near the image is left.
static uint8_t *make_stub(FzSite *s) {
    if (!stub_pool || stub_pool + STUB_SIZE > stub_end) {
        stub_pool = plat_code_alloc_near(s->entry, STUB_POOL);
        if (!stub_pool) return NULL;
        stub_end = stub_pool + STUB_POOL;
    }
    uint8_t *at = stub_pool, st[24];
#ifdef _WIN32
    st[0] = 0x48, st[1] = 0xBA;   // mov rdx, imm64 (the second argument, Windows x64)
#else
    st[0] = 0x48, st[1] = 0xBE;   // mov rsi, imm64 (the second argument, System V)
#endif
    uint64_t site = (uint64_t)(uintptr_t)s, fn = (uint64_t)(uintptr_t)fz_enter_patched;
    memcpy(st + 2, &site, 8);
    st[10] = 0xFF, st[11] = 0x25;   // jmp [rip + 0]
    memset(st + 12, 0, 4);
    memcpy(st + 16, &fn, 8);
    if (!plat_code_write(at, st, sizeof st)) return NULL;   // the pool stays read-execute between writes
    stub_pool += STUB_SIZE;
    return at;
}

// Turns the patch of addr's entry on or off: 1 done, 0 when this entry is not patchable (a gated build: the bitmap is
// used), -1 when patching failed (the hook cannot run). Under lock.
static int patch_entry(uint64_t addr, int on) {
    uint8_t *e = (uint8_t *)rt_dispatch_lookup(addr);
    if (!e || !patchable(addr, e)) return 0;
    if (!on) {
        if (!memcmp(e, JMP_BACK, 2)) return plat_code_write(e, NOP2, 2), 1;
        return !memcmp(e, NOP2, 2);   // a patchable entry never hooked; anything else is a gated build
    }
    if (!memcmp(e, JMP_BACK, 2)) return 1;
    if (memcmp(e, NOP2, 2)) return 0;
    FzSite *s = site_of(addr);
    if (!s) {
        s = calloc(1, sizeof *s);
        if (!s) return -1;
        *s = (FzSite){addr, e, sites};
        uint8_t *st = make_stub(s);
        if (!st) {
            free(s);
            rt_log("fuse: no executable memory near the image for hook stubs; %#llx not hooked", (unsigned long long)addr);
            return -1;   // a patchable build has no gate to fall back on
        }
        int64_t rel = (int64_t)(intptr_t)st - (int64_t)(intptr_t)e;   // jmp rel32 at e - 5 lands at e + rel
        if (rel != (int32_t)rel) {
            free(s);
            rt_log("fuse: hook stub out of jump range for %#llx", (unsigned long long)addr);
            return -1;
        }
        uint8_t jmp[5] = {0xE9};
        int32_t r32 = (int32_t)rel;
        memcpy(jmp + 1, &r32, 4);
        if (!plat_code_write(e - 5, jmp, 5)) {
            free(s);
            return -1;
        }
        sites = s;
    }
    return plat_code_write(e, JMP_BACK, 2) ? 1 : -1;
}
#else
static int patch_entry(uint64_t addr, int on) { return (void)addr, (void)on, 0; }
#endif

// A function's hooks went from none to some (on) or back: patch its entry, or in a gated build set its bitmap bit.
// 0, or -1 when the hook cannot take effect. Under lock.
static int set_bit(uint64_t addr, int on) {
    volatile uint8_t *bits;
    uint64_t k;
    if (!rt_dispatch_fn_bit(addr, &bits, &k)) return -1;
    int p = patch_entry(addr, on);
    if (p) return p < 0 ? -1 : 0;
    if (on) __atomic_fetch_or(&bits[k >> 3], (uint8_t)(1u << (k & 7)), __ATOMIC_RELEASE);
    else __atomic_fetch_and(&bits[k >> 3], (uint8_t)~(1u << (k & 7)), __ATOMIC_RELEASE);
    return 0;
}

int fz_hook(uint64_t addr, fz_before_fn before, fz_after_fn after, void *user, int priority) {
    volatile uint8_t *bits;
    uint64_t k;
    if (!rt_dispatch_fn_bit(addr, &bits, &k)) return -1;
    FzHook *n = calloc(1, sizeof *n);
    if (!n) return -1;
    *n = (FzHook){addr, before, after, user, priority, 0, NULL};
    plat_mutex_lock(&lock);
    // After the last hook of this function with a priority >= n's, else at the head of the bucket: a function's
    // hooks stay adjacent and in order.
    FzHook **at = &bucket[bucket_of(addr)];
    for (FzHook **p = at; *p; p = &(*p)->next)
        if ((*p)->addr == addr && (*p)->priority >= priority) at = &(*p)->next;
    n->next = *at;
    __atomic_store_n(at, n, __ATOMIC_RELEASE);
    if (set_bit(addr, 1)) {   // published but dead: readers skip it, and it is never freed while one may hold it
        n->dead = 1;
        plat_mutex_unlock(&lock);
        return -1;
    }
    nhooks++;
    plat_mutex_unlock(&lock);
    fz_present();
    return 0;
}

int fz_unhook(void *user) {
    int n = 0;
    plat_mutex_lock(&lock);
    for (unsigned b = 0; b < FZ_BUCKETS; b++)
        for (FzHook *h = bucket[b]; h; h = h->next)
            if (h->user == user && !h->dead) h->dead = 1, n++;
    // A function left with no live hook loses its bit again.
    for (unsigned b = 0; n && b < FZ_BUCKETS; b++)
        for (FzHook *h = bucket[b]; h; h = h->next)
            if (h->dead) {
                int live = 0;
                for (FzHook *o = first_hook(h->addr); o && o->addr == h->addr; o = o->next) live |= !o->dead;
                if (!live) set_bit(h->addr, 0);
            }
    nhooks -= n;
    plat_mutex_unlock(&lock);
    fz_present();
    return n;
}

// The hooks of addr around its body: body(c) when given (a patched entry's code past the no-op), else the function
// through the dispatcher with bypass set so its FN_ENTER lets the call through.
static void run_hooked(Cpu *c, uint64_t addr, FzHook *h, GuestFn body) {
    ctx_of(c)->depth++;
    uint64_t outer_addr = running_addr;
    running_addr = addr;
    int skip = 0;
    for (FzHook *k = h; k && k->addr == addr && !skip; k = k->next)
        if (!k->dead && k->before) {
            running_cb = (const void *)k->before;
            skip = k->before(c, k->user);
            running_cb = NULL;
        }
    if (!skip && body) {
        body(c);
    } else if (!skip) {
        GuestFn f = rt_dispatch_lookup(addr);
        bypass = addr;
        f(c);
        bypass = 0;   // in case the gate went off before the body's FN_ENTER read it
    }
    for (FzHook *k = h; k && k->addr == addr; k = k->next)
        if (!k->dead && k->after) {
            running_cb = (const void *)k->after;
            k->after(c, k->user);
            running_cb = NULL;
        }
    running_addr = outer_addr;
    ctx_of(c)->depth--;
}

int fz_enter(Cpu *c, uint64_t addr) {
    if (bypass == addr) {   // the original body, called by run_hooked
        bypass = 0;
        return 0;
    }
    FzHook *h = ctx_of(c)->depth < FZ_DEPTH ? first_hook(addr) : NULL;
    if (!h) return 0;
    run_hooked(c, addr, h, NULL);
    return 1;
}

// Reached from a patched entry's stub, in place of the function (the caller's return address still on the stack).
static void fz_enter_patched(Cpu *c, FzSite *s) {
    GuestFn body = (GuestFn)(void *)(s->entry + 2);
    FzHook *h = ctx_of(c)->depth < FZ_DEPTH ? first_hook(s->addr) : NULL;
    if (h) run_hooked(c, s->addr, h, body);
    else body(c);
}

// ---- symbols (TOTK_FUSE_SYMBOLS): a file of `name = 0xOFFSET` lines (offsets in main; '#' starts a comment, a
// [section] line is skipped), so mods hook by name; the Fuse SDK ships one per game version.

typedef struct { char *name; uint64_t offset; } FzSym;
static FzSym *syms;
static size_t nsyms;

static int sym_order(const void *a, const void *b) { return strcmp(((const FzSym *)a)->name, ((const FzSym *)b)->name); }

static void load_symbols(void) {
    const char *path = cfg_get("TOTK_FUSE_SYMBOLS");
    if (!path || !*path) return;
    FILE *f = fopen(path, "r");
    if (!f) {
        rt_log("fuse: cannot open TOTK_FUSE_SYMBOLS %s", path);
        return;
    }
    char line[512];
    size_t cap = 0;
    while (fgets(line, sizeof line, f)) {
        char *h = strchr(line, '#');
        if (h) *h = 0;
        char name[400];
        unsigned long long off;
        // name = 0x..., the name optionally quoted (TOML keys with :: need quotes)
        if (sscanf(line, " \"%399[^\"]\" = %llx", name, &off) != 2 && sscanf(line, " %399[^= 	] = %llx", name, &off) != 2)
            continue;
        if (nsyms == cap) {
            cap = cap ? cap * 2 : 64;
            FzSym *n = realloc(syms, cap * sizeof *syms);
            if (!n) break;
            syms = n;
        }
        syms[nsyms++] = (FzSym){strdup(name), off};
    }
    fclose(f);
    qsort(syms, nsyms, sizeof *syms, sym_order);
    rt_log("fuse: %zu symbols from %s", nsyms, path);
}

uint64_t fz_sym(const char *name) {
    size_t lo = 0, hi = nsyms;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int c = strcmp(syms[mid].name, name);
        if (!c) {
#if __has_include("layout.h")
            return MOD_MAIN_BASE + syms[mid].offset;
#else
            return 0;
#endif
        }
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return 0;
}

int fz_hook_count(void) { return __atomic_load_n(&nhooks, __ATOMIC_RELAXED); }

// ---- deployed folders (.fuse-deploy.json, written by the fuse CLI)

// A package id or a package-relative path from the manifest that stays inside its folder: ids are [a-z0-9._-] and do
// not start with '.'; paths are relative, '/'-separated, with no '..' segment, no ':' (drive or stream), no '\'.
static int safe_id(const char *s) {
    if (!*s || *s == '.') return 0;
    for (; *s; s++)
        if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '.' || *s == '_' || *s == '-')) return 0;
    return 1;
}

static int safe_path(const char *s) {
    if (!*s || *s == '/' || strchr(s, '\\') || strchr(s, ':')) return 0;
    for (const char *seg = s; seg; seg = strchr(seg, '/') ? strchr(seg, '/') + 1 : NULL) {
        size_t n = strcspn(seg, "/");
        if (n == 0 || (n == 2 && seg[0] == '.' && seg[1] == '.') || (n == 1 && seg[0] == '.')) return 0;
    }
    return 1;
}

// The next JSON string token at or after *p (the manifest is our own CLI's: plain ASCII, no escapes but \" and \\);
// 0 at the end. out gets at most cap-1 bytes; a longer string is cut and flagged by *cut.
static int next_string(const char **p, char *out, size_t cap, int *cut) {
    const char *s = strchr(*p, '"');
    if (!s) return 0;
    size_t n = 0;
    *cut = 0;
    for (s++; *s && *s != '"'; s++) {
        if (*s == '\\' && s[1]) s++;
        if (n + 1 < cap) out[n++] = *s;
        else *cut = 1;
    }
    out[n] = 0;
    *p = *s ? s + 1 : s;
    return 1;
}

static char *read_deploy(const char *dir, char *path, size_t path_cap) {   // the manifest's text, or NULL
    snprintf(path, path_cap, "%s/.fuse-deploy.json", dir);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    static const size_t cap = 4u << 20;
    char *text = malloc(cap + 1);
    size_t len = text ? fread(text, 1, cap, f) : 0;
    fclose(f);
    if (text) text[len] = 0;
    return text;
}

int fz_deployed_config(const char *dir, fz_config_fn cb, void *user) {
    char path[1024], id[128] = "", val[1100];
    char *text = read_deploy(dir, path, sizeof path);
    if (!text) return -1;
    int n = 0, cut, in_config = 0;
    const char *p = text;
    while (next_string(&p, val, sizeof val, &cut)) {
        if (in_config) {   // "key=t:value" strings, until the next key ("files")
            const char *eq = strchr(val, '=');
            if (eq && val[0] >= 'a' && val[0] <= 'z' && (eq[1] == 'b' || eq[1] == 'n' || eq[1] == 's') && eq[2] == ':') {
                if (!cut) cb(id, val, user), n++;
                continue;
            }
            in_config = 0;
        }
        if (!strcmp(val, "id")) {
            if (!next_string(&p, val, sizeof val, &cut)) break;
            snprintf(id, sizeof id, "%s", !cut && safe_id(val) ? val : "");
        } else if (!strcmp(val, "config")) {
            in_config = id[0] != 0;
        }
    }
    free(text);
    return n;
}

int fz_deployed_files(const char *dir, const char *ext, fz_deployed_fn cb, void *user) {
    char path[1024];
    char *text = read_deploy(dir, path, sizeof path);
    if (!text) return -1;
    char key[64], id[128] = "", rel[512] = "", val[512];
    size_t ext_n = strlen(ext);
    int n = 0, cut;
    const char *p = text;
    while (next_string(&p, key, sizeof key, &cut)) {
        if (strcmp(key, "id") && strcmp(key, "path") && strcmp(key, "sha256")) continue;
        if (!next_string(&p, val, sizeof val, &cut)) break;
        if (cut) {   // a value longer than any real one: refuse the whole entry
            if (!strcmp(key, "id")) id[0] = 0;
            rel[0] = 0;
            continue;
        }
        if (!strcmp(key, "id")) snprintf(id, sizeof id, "%s", safe_id(val) ? val : "");
        else if (!strcmp(key, "path")) snprintf(rel, sizeof rel, "%s", val);
        else if (*id && *rel) {   // sha256: the file entry is complete
            size_t rn = strlen(rel);
            if (!safe_path(rel)) rt_log("fuse: %s lists an unsafe path %s/%s; skipped", path, id, rel);
            else if (strlen(val) == 64 && rn > ext_n && !strcmp(rel + rn - ext_n, ext)) cb(id, rel, val, user), n++;
            rel[0] = 0;
        }
    }
    free(text);
    return n;
}

int fz_file_sha256_hex(const void *data, size_t n, const char *want) {
    uint8_t h[32];
    char hex[65];
    rt_sha256(data, n, h);
    for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", h[i]);
    return !strcmp(hex, want);
}

#ifndef _WIN32
int fz_luau_stats(FzModStat *out, int max, float *budget_ms) {   // no script VM on this platform yet
    (void)out, (void)max;
    if (budget_ms) *budget_ms = 0;
    return 0;
}
int fz_luau_packages(FzPackage *out, int max) { return (void)out, (void)max, 0; }
int fz_luau_set_enabled(const char *id, int on) { return (void)id, (void)on, -1; }
int fz_manager_ready(void) { return 0; }
int fz_manager_run(const char *const *args, int nargs, char *out, size_t cap, int with_errors) {
    (void)args, (void)nargs, (void)with_errors;
    if (cap) *out = 0;
    return -1;
}
#endif

void fz_crash_note(void) {
    if (!running_addr) return;
    char at[64], who[160];
    rt_guest_name(running_addr, at, sizeof at);
    if (running_cb)
        rt_logf("  Fuse: in a hook callback at %s on %s\n", fz_owner_name(running_cb, who, sizeof who), at);
    else
        rt_logf("  Fuse: inside the original body of the hooked function %s\n", at);
}

void fz_present(void) {
    int on = nhooks > 0 && (gate_mode == 2 ? rt_ab() : gate_mode);
    if (g_fnprof != on) g_fnprof = on;
}

// ---- the spike's test hooks (TOTK_FUSE_TEST): 1.2.1 addresses, so only in a build against a game layout

#if __has_include("layout.h")
#define MAIN(off) (MOD_MAIN_BASE + (off))

static uint64_t calc_calls;

static int count_calc(Cpu *c, void *user) {
    (void)c, (void)user;
    uint64_t n = __atomic_add_fetch(&calc_calls, 1, __ATOMIC_RELAXED);
    if (n == 1 || n % 3600 == 0) rt_log("fuse: Player::calc entered %llu times", (unsigned long long)n);
    return 0;
}

static int guard_in(Cpu *c, void *user) {
    (void)user;
    rt_log("fuse: calcGuardJustSuccess(actor %#llx)", (unsigned long long)c->x[0]);
    return 0;
}

static void guard_out(Cpu *c, void *user) {
    (void)user;
    rt_log("fuse: calcGuardJustSuccess returned %llu", (unsigned long long)(c->x[0] & 0xff));
}

static void no_wear_rate(Cpu *c, void *user) {   // GetShieldDurabilityDamageRate returns a float in s0
    (void)user;
    c->v[0] &= ~(__uint128_t)0xFFFFFFFFu;   // 0.0f
}

static void attach(const char *name, uint64_t addr, fz_before_fn b, fz_after_fn a) {
    int r = fz_hook(addr, b, a, NULL, 100);
    rt_log("fuse: hook %s at %#llx %s", name, (unsigned long long)addr, r ? "FAILED (no function starts there)" : "attached");
}
#endif

void fz_init(void) {
    load_symbols();
    const char *g = cfg_get("TOTK_FUSE_GATE");
    gate_mode = !g || !*g ? 1 : !strcmp(g, "ab") ? 2 : strcmp(g, "0") != 0;
    fz_luau_init();
    fz_plugins_init();
    const char *t = cfg_get("TOTK_FUSE_TEST");
    int test = t && *t ? atoi(t) : 0;
    if (!test) return;
#ifdef MOD_MAIN_BASE
    attach("Player::calc", MAIN(0x6515cc), count_calc, NULL);
    attach("GameActor::calcGuardJustSuccess", MAIN(0x1724f10), guard_in, guard_out);
    if (test >= 2) attach("GetShieldDurabilityDamageRate", MAIN(0x11784c4), NULL, no_wear_rate);
#endif
    rt_log("fuse: hook gate %s", gate_mode == 2 ? "in the A/B test's B blocks" : gate_mode ? "on" : "off");
}
