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

// The function fz_enter is running the original body of: its FN_ENTER lets that one call through.
static __thread uint64_t bypass;
static __thread int depth;
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
static void set_bit(uint64_t addr, int on) {
    volatile uint8_t *bits;
    uint64_t k;
    if (!rt_dispatch_fn_bit(addr, &bits, &k)) return;
    if (on) __atomic_fetch_or(&bits[k >> 3], (uint8_t)(1u << (k & 7)), __ATOMIC_RELEASE);
    else __atomic_fetch_and(&bits[k >> 3], (uint8_t)~(1u << (k & 7)), __ATOMIC_RELEASE);
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
    set_bit(addr, 1);
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

int fz_enter(Cpu *c, uint64_t addr) {
    if (bypass == addr) {   // the original body, called below
        bypass = 0;
        return 0;
    }
    FzHook *h = depth < FZ_DEPTH ? first_hook(addr) : NULL;
    if (!h) return 0;
    depth++;
    uint64_t outer_addr = running_addr;
    running_addr = addr;
    int skip = 0;
    for (FzHook *k = h; k && k->addr == addr && !skip; k = k->next)
        if (!k->dead && k->before) {
            running_cb = (const void *)k->before;
            skip = k->before(c, k->user);
            running_cb = NULL;
        }
    if (!skip) {
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
    depth--;
    return 1;
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
