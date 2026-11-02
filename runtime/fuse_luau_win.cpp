// Fuse Luau scripts (spike): *.luau files from TOTK_FUSE_SCRIPTS hook game functions through fuse_hooks and react to
// events. A folder the fuse CLI deployed loads exactly what its .fuse-deploy.json lists, in the resolved load order, and
// refuses any file whose SHA-256 no longer matches; any other folder loads its *.luau and its subfolders' (one level).
//
// Trust model (docs/decisions/0002-trust-and-permissions.md in the Fuse repository): a script gets the whole game and
// nothing of the machine.
// - One VM, sandboxed with luaL_sandbox (read-only standard libraries; no io, os.execute, package, loadfile, require of
//   files, debug beyond tracebacks), each script in its own sandboxed thread environment. No network, no files, no
//   processes: the host gives scripts no such functions.
// - Memory access is guest memory only: every read and write is checked with mem_script_access (the guest heap, alias
//   region, stack, thread-local region or a loaded module's pages with the right permission), so a script cannot
//   reach the runtime's own data or code pointers.
// - Budgets: every call into a script (hook, event, load, console) has a CPU budget (TOTK_FUSE_SCRIPT_MS, default 5 ms;
//   the console and loading 2 s) enforced by Luau's interrupt callback; the VM's memory is capped
//   (TOTK_FUSE_SCRIPT_MB, default 256). A hook or handler that fails 10 times is disabled.
// - The VM sits behind one recursive lock, so hooks from several guest threads take turns (per-thread VMs come later).
//   ctx:call lets go of it while the game function it calls runs, so the lock cannot deadlock against game locks.
//
// The API, table `fuse`:
//   fuse.log(...)                         a log line, "lua: ..."
//   fuse.notify(text [, seconds])         a message at the top of the game's screen
//   fuse.main(offset)                     guest address of main+offset (addresses fit a Luau number exactly)
//   fuse.sym(name)                        guest address of a named function from TOTK_FUSE_SYMBOLS (errors if unknown)
//   fuse.hook(addr, before, after)        before(ctx) may return true to skip the original; after(ctx) sees the return
//                                         value. Either may be nil.
//   fuse.on(event, fn)                    "frame": fn(frame) once per presented frame; "world": fn(true) when a save's
//                                         world starts running; "menu": fn(open) when the port's menu opens or closes;
//                                         "guard": fn(kind) on a guarded hit; "perfect_guard": fn(true)
//   fuse.read8/read16/read32/read64/readf(addr), fuse.write8/write16/write32/writef(addr, v)   guest memory, checked
//   config                                a deployed package's scripts: its option values (read-only table)
//   ctx (valid only during the call): ctx:x(i) / ctx:setx(i, v) general registers (exact up to 2^53; ctx:xs(i) is the
//   hex string), ctx:s(i) / ctx:sets(i, v) the float in s<i>, ctx:call(addr, x0...x7) calls a game function and
//   returns its x0 and s0 (registers restored after).
// fz_luau_reload: unhooks every script hook, closes the VM and loads the folder again (DevLink "script_reload").
// fz_luau_eval: runs a chunk in the live VM and returns what it printed and returned (DevLink "lua", the console).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <string>
#include <vector>

#include <windows.h>

extern "C" {
#include "cfg.h"
#include "dispatch.h"
#include "fuse_hooks.h"
#include "host.h"
#include "platform/platform.h"
#include "ui/menu.h"
#if __has_include("layout.h")
#include "layout.h"
#endif
}

#include "lua.h"
#include "luacode.h"
#include "lualib.h"

// The event dispatchers (defined further down, outside the namespace); the game events call them.
void fire(int event, double value, bool is_bool);
void fire_text(int event, const char *text);

namespace {

struct ScriptRef {   // a hook or an event handler: one or two functions in the VM's registry
    int before = LUA_NOREF, after = LUA_NOREF;
    int errors = 0;
    uint64_t addr = 0;   // 0 for event handlers
    int event = 0;       // event handlers: EV_*
    bool retired = false;   // its VM is gone (reload): calls do nothing; read under vm_lock
    int pkg = 0;            // the package (pkgs) it belongs to: its time is billed there
};

// ---- per-mod cost: every script call's exclusive time (minus nested script calls and game code run by ctx:call) is
// billed to its package; once a frame each package's time is smoothed, and a package that stays over its budget has
// its event handlers paused for a while (its hooks keep running: they may change what the game does). Under vm_lock.
struct Pkg {
    std::string id;
    uint64_t frame_ns = 0;
    unsigned frame_calls = 0, last_calls = 0, hooks = 0, handlers = 0;
    double avg_ms = 0;
    float recent[120] = {};   // ms of the last 120 frames, for the max
    int recent_at = 0, over = 0, warm = 3, pause_len = 0;
    uint64_t paused_until = 0;
    int caps = -1;            // CAP_* the package declared (deploy manifest); -1: not held to a list (loose, console)
    bool skip_noted = false;  // logged once: a hook of this package tried to skip without game.write
};
std::vector<Pkg> pkgs;        // [0] is the console and loose scripts
enum { CAP_READ = 1, CAP_WRITE = 2 };   // game.read; game.write (implies read: changing a value means reading it)
int loading_pkg;              // the package whose script is loading: hooks and handlers it registers are its
double mod_budget_ms = 1.0;   // TOTK_FUSE_MOD_MS
uint64_t frame_now;           // the frame fz_luau_frame last saw

int pkg_index(const std::string &id) {
    for (size_t i = 0; i < pkgs.size(); i++)
        if (pkgs[i].id == id) return (int)i;
    pkgs.emplace_back();
    pkgs.back().id = id;
    return (int)pkgs.size() - 1;
}

// The running script call on this thread: nested calls and ctx:call add their time to `excluded`.
struct CallFrame {
    uint64_t t0, excluded;
    CallFrame *up;
};
thread_local CallFrame *cur_frame;
thread_local int running_pkg;   // the package whose hook or handler is running on this thread (0 outside one)

int owner_pkg() { return loading_pkg ? loading_pkg : running_pkg; }   // who registers a hook or handler now

// The package whose code is running (a hook's, else the loading script's) must have declared cap; a Lua error if not.
// What the player saw on the install card ("Asks for: game.read") is what the scripts can do.
void need(lua_State *l, int cap) {
    int p = running_pkg ? running_pkg : loading_pkg;
    if (p <= 0 || p >= (int)pkgs.size() || pkgs[p].caps < 0 || (pkgs[p].caps & cap)) return;
    luaL_error(l, "%s did not declare %s ([trust] capabilities in its fuse.toml)", pkgs[p].id.c_str(),
               cap == CAP_WRITE ? "game.write" : "game.read");
}

CRITICAL_SECTION vm_lock;   // recursive: a script hook may fire inside another script's call
thread_local int vm_held;   // how many times this thread holds vm_lock (ctx:call lets go of all of them)
void vm_enter() { EnterCriticalSection(&vm_lock), vm_held++; }
void vm_leave() { vm_held--, LeaveCriticalSection(&vm_lock); }
int calls_out;              // threads inside a ctx:call's game function, holding none of vm_lock (changed under it)
lua_State *L;
// Lua threads for hook and event calls: each call takes one from the pool and gives it back when it returns, so a
// call that lets go of the lock (ctx:call) owns its stack outright, whichever OS thread or guest fiber it resumes on.
// Created as needed (as many as calls can be in flight at once), kept until the VM closes. Under vm_lock.
std::vector<lua_State *> free_threads;

lua_State *take_thread() {
    if (!free_threads.empty()) {
        lua_State *t = free_threads.back();
        free_threads.pop_back();
        return t;
    }
    lua_State *t = lua_newthread(L);
    lua_ref(L, -1);
    lua_pop(L, 1);
    return t;
}

void give_thread(lua_State *t) {
    lua_settop(t, 0);
    free_threads.push_back(t);
}
std::vector<ScriptRef *> hooks;    // owned; at reload unhooked and retired, never freed: fz_enter reads a hook's
                                   // user pointer without a lock, so a call may still arrive after its reload
std::vector<ScriptRef *> frame_handlers;   // every event handler (the name is older than the other events)
// Events: frame (every presented frame, with its number), world (a save's world starts running, once per load), menu
// (the port's menu opens or closes, with true or false), and game events built on verified symbols: guard (the player
// or an actor guarded a hit: "guard", "repel", "break" or "perfect") and perfect_guard (the perfect-guard effects
// played). A game event's function is hooked only once a script listens for it.
enum { EV_FRAME, EV_WORLD, EV_MENU, EV_GUARD, EV_PERFECT_GUARD, EV_COUNT };
const char *const EVENTS[] = {"frame", "world", "menu", "guard", "perfect_guard"};

// ---- game events: one runtime hook per event, on the function the symbol file names (1.2.1 offsets as fallback)
struct GameEvent {
    int event;
    const char *symbol;
    uint64_t offset;   // in main, game version 1.2.1
    bool hooked;
};
GameEvent game_events[] = {
    {EV_GUARD, "game::life::GuardCalculator::calcGuardReaction", 0xa07690, false},
    {EV_PERFECT_GUARD, "game::actor::GameActor::calcGuardJustSuccess", 0x1724f10, false},
};

void on_guard_reaction(Cpu *c, void *) {   // after calcGuardReaction: its result, the guard reaction, in w0
    static const char *const names[] = {"guard", "repel", "break", "perfect"};
    unsigned r = (unsigned)c->x[0] & 0xff;
    if (r < 3 || r > 6) return;
    vm_enter();
    fire_text(EV_GUARD, names[r - 3]);
    vm_leave();
}

void on_guard_just(Cpu *c, void *) {   // after calcGuardJustSuccess: 1 when the effects played
    if (((unsigned)c->x[0] & 0xff) != 1) return;
    vm_enter();
    fire(EV_PERFECT_GUARD, 1, true);
    vm_leave();
}

void need_game_event(int event) {   // under vm_lock: hook the event's function the first time a script listens
    for (auto &g : game_events) {
        if (g.event != event || g.hooked) continue;
        uint64_t a = fz_sym(g.symbol);
#ifdef MOD_MAIN_BASE
        if (!a) a = MOD_MAIN_BASE + g.offset;
#endif
        fz_after_fn cb = event == EV_GUARD ? on_guard_reaction : on_guard_just;
        if (a && !fz_hook(a, NULL, cb, &g, 40)) {
            g.hooked = true;
            rt_log("lua: event %s listens at %s", EVENTS[event], g.symbol);
        } else {
            rt_log("lua: event %s is not available (%s not found)", EVENTS[event], g.symbol);
        }
    }
}
bool world_on, menu_on;   // the states the world and menu events last reported
std::vector<ScriptRef *> retired;  // the hooks of earlier VMs (a few bytes each per reload)
std::string *capture;              // fz_luau_eval: where fuse.log also writes
std::vector<std::string> loaded;   // the script files that loaded (the Mods tab)
std::vector<std::string> deployed;          // the deployed packages, in load order (the Mods tab's switches)
std::vector<std::string> disabled_in_game;  // <scripts>/.fuse-disabled: deployed packages the player turned off
std::string scripts_dir;                    // the deployed folder load_deployed read
int ready;
size_t mem_used, mem_cap;
uint64_t call_budget_ns;
thread_local uint64_t call_deadline_ns;   // this thread's running call's deadline; 0 = none
thread_local bool over_budget;            // set when the deadline passed; cleared by the outermost call

const char *const CTX = "fuse.ctx";

struct Ctx {
    Cpu *c;
    lua_State *owner;   // the Lua thread running the hook call it belongs to; used from any other, it refuses
};

// Caps on what scripts register: hooks and handlers live outside the Lua memory cap, and run on hot paths.
enum { MAX_HOOKS = 4096, MAX_HANDLERS = 256 };

// ---- budgets

void *limited_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
    (void)ud;
    if (nsize == 0) {
        free(ptr);
        mem_used -= osize;
        return NULL;
    }
    if (nsize > osize && mem_used + (nsize - osize) > mem_cap) return NULL;   // Luau raises "not enough memory"
    void *p = realloc(ptr, nsize);
    if (p) mem_used = mem_used - osize + nsize;
    return p;
}

void interrupt(lua_State *l, int gc) {
    if (gc >= 0) return;   // a GC step
    if (!over_budget && call_deadline_ns && plat_now_ns() > call_deadline_ns) over_budget = true;
    // Sticky: a script that catches the error with pcall gets it again at its next interrupt, until it unwinds.
    if (over_budget) luaL_error(l, "script ran over its CPU budget");
}

// A Lua number as a 64-bit register value: integers in [-2^63, 2^64), negatives two's complement. Errors otherwise
// (converting an out-of-range double is undefined behaviour in C).
uint64_t to_u64(lua_State *l, int i) {
    double d = luaL_checknumber(l, i);
    if (!(d >= -9223372036854775808.0 && d < 18446744073709551616.0)) luaL_error(l, "number out of range");
    return d < 0 ? (uint64_t)(int64_t)d : (uint64_t)d;
}

uint64_t to_bits(lua_State *l, int i, int bits) {   // a value for a write of that many bits: it must fit
    double d = luaL_checknumber(l, i);
    double hi = bits == 8 ? 256.0 : bits == 16 ? 65536.0 : 4294967296.0;
    if (!(d >= -hi / 2 && d < hi)) luaL_error(l, "%g does not fit %d bits", d, bits);
    return d < 0 ? (uint64_t)(int64_t)d : (uint64_t)d;
}

// ---- guest memory, checked against the guest memory map

int guest_ok(uint64_t a, uint64_t n, int write) { return mem_script_access(a, n, write); }

uint64_t checked(lua_State *l, int i, uint64_t n, int write) {
    double d = luaL_checknumber(l, i);
    if (d < 0 || d >= 18446744073709551616.0) luaL_error(l, "address out of range");
    uint64_t a = (uint64_t)d;
    if (!guest_ok(a, n, write))
        luaL_error(l, "0x%llx is not %s guest memory", (unsigned long long)a, write ? "writable" : "readable");
    return a;
}

// ---- ctx

Cpu *ctx_cpu(lua_State *l) {
    Ctx *x = (Ctx *)luaL_checkudata(l, 1, CTX);
    if (!x->c) luaL_error(l, "ctx used outside its hook call");
    if (x->owner != l) luaL_error(l, "ctx used outside its own hook call");
    return x->c;
}

int reg_index(lua_State *l, int arg, int hi) {
    int i = luaL_checkinteger(l, arg);
    if (i < 0 || i > hi) luaL_error(l, "register %d out of range", i);
    return i;
}

int ctx_x(lua_State *l) {
    Cpu *c = ctx_cpu(l);
    need(l, CAP_READ);
    lua_pushnumber(l, (double)c->x[reg_index(l, 2, 30)]);
    return 1;
}

int ctx_xs(lua_State *l) {
    Cpu *c = ctx_cpu(l);
    need(l, CAP_READ);
    char b[24];
    snprintf(b, sizeof b, "0x%llx", (unsigned long long)c->x[reg_index(l, 2, 30)]);
    lua_pushstring(l, b);
    return 1;
}

int ctx_setx(lua_State *l) {
    Cpu *c = ctx_cpu(l);
    need(l, CAP_WRITE);
    int i = reg_index(l, 2, 30);
    c->x[i] = to_u64(l, 3);
    return 0;
}

int ctx_s(lua_State *l) {
    Cpu *c = ctx_cpu(l);
    need(l, CAP_READ);
    uint32_t w = (uint32_t)c->v[reg_index(l, 2, 31)];
    float f;
    memcpy(&f, &w, 4);
    lua_pushnumber(l, f);
    return 1;
}

int ctx_sets(lua_State *l) {
    Cpu *c = ctx_cpu(l);
    need(l, CAP_WRITE);
    int i = reg_index(l, 2, 31);
    float f = (float)luaL_checknumber(l, 3);
    uint32_t w;
    memcpy(&w, &f, 4);
    c->v[i] = (c->v[i] & ~(__uint128_t)0xFFFFFFFFu) | w;
    return 0;
}

// ctx:call(addr, x0, ..., x7): calls the game function at addr with up to eight integer arguments, on this hook's
// thread and guest stack; returns its x0 and s0. Registers not given keep their current values (so a before hook can
// pass the hooked call's own arguments through). Every register is restored afterwards, so the hooked function sees
// no change. The callee runs as game code: it reaches guest memory only, and any hooks on it run.
int ctx_call(lua_State *l) {
    Cpu *c = ctx_cpu(l);
    need(l, CAP_WRITE);   // a called game function can change anything the game can
    uint64_t a = to_u64(l, 2);
    volatile uint8_t *bits;
    uint64_t k;
    if (!rt_dispatch_fn_bit(a, &bits, &k)) luaL_error(l, "no lifted function starts at 0x%llx", (unsigned long long)a);
    int n = lua_gettop(l) - 2;
    if (n > 8) luaL_error(l, "at most 8 arguments");
    uint64_t args[8];
    for (int i = 0; i < 8; i++) args[i] = i < n ? to_u64(l, 3 + i) : c->x[i];
    Cpu *saved = (Cpu *)_aligned_malloc(sizeof(Cpu), 16);
    if (!saved) luaL_error(l, "out of memory");
    *saved = *c;
    // The VM lock is let go while game code runs: that code may wait on a game lock whose holder is about to run a
    // script hook. Nothing here touches the VM until the lock is back. The call's time does not count toward the
    // script's CPU budget (the budget pauses with it).
    // The per-thread budget state is set aside too: other calls may run on this OS thread meanwhile, and this one may
    // come back on another (a guest fiber can move).
    int held = vm_held;
    uint64_t deadline = call_deadline_ns, t0 = plat_now_ns();
    bool over = over_budget;
    calls_out++;
    call_deadline_ns = 0, over_budget = false;
    CallFrame *fr = cur_frame;   // set aside like the budget: script calls inside the game code bill themselves
    cur_frame = nullptr;
    for (int i = 0; i < held; i++) vm_leave();
    uint64_t x0 = rt_call(c, a, 8, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7]);
    for (int i = 0; i < held; i++) vm_enter();
    calls_out--;
    uint64_t game_ns = plat_now_ns() - t0;
    call_deadline_ns = deadline ? deadline + game_ns : 0, over_budget = over;
    cur_frame = fr;
    if (fr) fr->excluded += game_ns;   // the game code's time (and any script calls inside it) is not this mod's
    uint32_t w = (uint32_t)c->v[0];
    // Registers back, not the context's spare bytes (Fuse's own per-context state, cpu.h _pad).
    memcpy(c, saved, offsetof(Cpu, _pad));
    memcpy(c->v, saved->v, sizeof c->v);
    _aligned_free(saved);
    float f;
    memcpy(&f, &w, 4);
    lua_pushnumber(l, (double)x0);
    lua_pushnumber(l, f);
    return 2;
}

// ---- the fuse table

int api_log(lua_State *l) {
    std::string line;
    for (int i = 1, n = lua_gettop(l); i <= n; i++) {
        size_t len;
        const char *s = luaL_tolstring(l, i, &len);
        if (i > 1) line += ' ';
        line.append(s, len);
        lua_pop(l, 1);
    }
    rt_log("lua: %s", line.c_str());
    if (capture) *capture += line + "\n";
    return 0;
}

int api_notify(lua_State *l) {
    char msg[256];
    snprintf(msg, sizeof msg, "%s", luaL_checkstring(l, 1));
    menu_notice(msg, luaL_optnumber(l, 2, 4));
    return 0;
}

int api_sym(lua_State *l) {
    const char *name = luaL_checkstring(l, 1);
    uint64_t a = fz_sym(name);
    if (!a) luaL_error(l, "unknown symbol '%s' (TOTK_FUSE_SYMBOLS)", name);
    lua_pushnumber(l, (double)a);
    return 1;
}

int api_main(lua_State *l) {
#ifdef MOD_MAIN_BASE
    lua_pushnumber(l, (double)(MOD_MAIN_BASE + to_u64(l, 1)));
#else
    lua_pushnil(l);
#endif
    return 1;
}

int api_read8(lua_State *l) { need(l, CAP_READ); lua_pushnumber(l, LD8(checked(l, 1, 1, 0))); return 1; }
int api_read16(lua_State *l) { need(l, CAP_READ); lua_pushnumber(l, LD16(checked(l, 1, 2, 0))); return 1; }
int api_read32(lua_State *l) { need(l, CAP_READ); lua_pushnumber(l, LD32(checked(l, 1, 4, 0))); return 1; }
int api_read64(lua_State *l) { need(l, CAP_READ); lua_pushnumber(l, (double)LD64(checked(l, 1, 8, 0))); return 1; }
int api_readf(lua_State *l) {
    need(l, CAP_READ);
    uint32_t w = LD32(checked(l, 1, 4, 0));
    float f;
    memcpy(&f, &w, 4);
    lua_pushnumber(l, f);
    return 1;
}
int api_write8(lua_State *l) { need(l, CAP_WRITE); ST8(checked(l, 1, 1, 1), (uint8_t)to_bits(l, 2, 8)); return 0; }
int api_write16(lua_State *l) { need(l, CAP_WRITE); ST16(checked(l, 1, 2, 1), (uint16_t)to_bits(l, 2, 16)); return 0; }
int api_write32(lua_State *l) { need(l, CAP_WRITE); ST32(checked(l, 1, 4, 1), (uint32_t)to_bits(l, 2, 32)); return 0; }
int api_writef(lua_State *l) {
    need(l, CAP_WRITE);
    uint64_t a = checked(l, 1, 4, 1);
    float f = (float)luaL_checknumber(l, 2);
    uint32_t w;
    memcpy(&w, &f, 4);
    ST32(a, w);
    return 0;
}

// ---- typed views: fuse.view(addr, "Type") sees guest memory as a type from the symbol file's [layouts]
// (`"Type.field" = { offset = 0x10, type = "f32" }`). Fields read and write through the same checks as fuse.read* and
// fuse.write* (guest memory only; game.read / game.write); an embedded struct is a view, "*Type" follows a pointer.

const char *const VIEW = "fuse.view";
struct View {
    uint64_t addr;
    char type[96];
};

void push_view(lua_State *l, uint64_t addr, const char *type) {
    View *v = (View *)lua_newuserdata(l, sizeof(View));
    v->addr = addr;
    snprintf(v->type, sizeof v->type, "%s", type);
    luaL_getmetatable(l, VIEW);
    lua_setmetatable(l, -2);
}

int api_view(lua_State *l) {
    uint64_t a = to_u64(l, 1);
    const char *t = luaL_checkstring(l, 2);
    if (!fz_layout(t, NULL, NULL, NULL)) luaL_error(l, "no layout for type %s ([layouts] in TOTK_FUSE_SYMBOLS)", t);
    push_view(l, a, t);
    return 1;
}

int scalar_size(const char *k) {   // bytes of a number or bool field; 0 for a struct or pointer-to-struct
    if (!strcmp(k, "u8") || !strcmp(k, "s8") || !strcmp(k, "bool")) return 1;
    if (!strcmp(k, "u16") || !strcmp(k, "s16")) return 2;
    if (!strcmp(k, "u32") || !strcmp(k, "s32") || !strcmp(k, "f32")) return 4;
    if (!strcmp(k, "u64") || !strcmp(k, "s64") || !strcmp(k, "f64") || !strcmp(k, "ptr")) return 8;
    return 0;
}

View *view_field(lua_State *l, uint64_t *a, const char **kind) {
    View *v = (View *)luaL_checkudata(l, 1, VIEW);
    const char *key = luaL_checkstring(l, 2);
    uint64_t off;
    if (!fz_layout(v->type, key, &off, kind)) luaL_error(l, "%s has no field %s", v->type, key);
    *a = v->addr + off;
    return v;
}

int view_index(lua_State *l) {
    if (lua_isstring(l, 2) && !strcmp(lua_tostring(l, 2), "addr")) {
        lua_pushnumber(l, (double)((View *)luaL_checkudata(l, 1, VIEW))->addr);
        return 1;
    }
    uint64_t a;
    const char *k;
    View *v = view_field(l, &a, &k);
    int n = scalar_size(k);
    if (!n && k[0] != '*') {   // an embedded struct: a view, nothing read yet
        push_view(l, a, k);
        return 1;
    }
    need(l, CAP_READ);
    if (!guest_ok(a, n ? n : 8, 0))
        luaL_error(l, "%s.%s at 0x%llx is not readable guest memory", v->type, lua_tostring(l, 2), (unsigned long long)a);
    if (k[0] == '*') {   // a pointer to a struct: its view, or nil for a null pointer
        uint64_t p = LD64(a);
        if (p) push_view(l, p, k + 1);
        else lua_pushnil(l);
        return 1;
    }
    if (!strcmp(k, "bool")) lua_pushboolean(l, LD8(a) != 0);
    else if (!strcmp(k, "u8")) lua_pushnumber(l, LD8(a));
    else if (!strcmp(k, "s8")) lua_pushnumber(l, (int8_t)LD8(a));
    else if (!strcmp(k, "u16")) lua_pushnumber(l, LD16(a));
    else if (!strcmp(k, "s16")) lua_pushnumber(l, (int16_t)LD16(a));
    else if (!strcmp(k, "u32")) lua_pushnumber(l, LD32(a));
    else if (!strcmp(k, "s32")) lua_pushnumber(l, (int32_t)LD32(a));
    else if (!strcmp(k, "s64")) lua_pushnumber(l, (double)(int64_t)LD64(a));
    else if (!strcmp(k, "f32")) {
        uint32_t w = LD32(a);
        float f;
        memcpy(&f, &w, 4);
        lua_pushnumber(l, f);
    } else if (!strcmp(k, "f64")) {
        uint64_t w = LD64(a);
        double d;
        memcpy(&d, &w, 8);
        lua_pushnumber(l, d);
    } else lua_pushnumber(l, (double)LD64(a));   // u64, ptr
    return 1;
}

int view_newindex(lua_State *l) {
    uint64_t a;
    const char *k;
    View *v = view_field(l, &a, &k);
    int n = scalar_size(k);
    if (!n || !strcmp(k, "ptr")) luaL_error(l, "%s.%s is not a number or bool field", v->type, lua_tostring(l, 2));
    need(l, CAP_WRITE);
    if (!guest_ok(a, n, 1))
        luaL_error(l, "%s.%s at 0x%llx is not writable guest memory", v->type, lua_tostring(l, 2), (unsigned long long)a);
    if (!strcmp(k, "bool")) ST8(a, lua_toboolean(l, 3) ? 1 : 0);
    else if (!strcmp(k, "f32")) {
        float f = (float)luaL_checknumber(l, 3);
        uint32_t w;
        memcpy(&w, &f, 4);
        ST32(a, w);
    } else if (!strcmp(k, "f64")) {
        double d = luaL_checknumber(l, 3);
        uint64_t w;
        memcpy(&w, &d, 8);
        ST64(a, w);
    } else if (n == 1) ST8(a, (uint8_t)to_bits(l, 3, 8));
    else if (n == 2) ST16(a, (uint16_t)to_bits(l, 3, 16));
    else if (n == 4) ST32(a, (uint32_t)to_bits(l, 3, 32));
    else ST64(a, to_u64(l, 3));
    return 0;
}

int view_tostring(lua_State *l) {
    View *v = (View *)luaL_checkudata(l, 1, VIEW);
    char b[128];
    snprintf(b, sizeof b, "%s@0x%llx", v->type, (unsigned long long)v->addr);
    lua_pushstring(l, b);
    return 1;
}

// Calls the function in the registry with one argument pushed by push(); returns its first result as a boolean
// (before hooks: skip). Under vm_lock, with the call's CPU budget.
template <class Push> int call_ref(ScriptRef *h, int ref, Push push) {
    if (ref == LUA_NOREF || h->retired || h->errors >= 10 || !L) return 0;
    Pkg &pk = pkgs[h->pkg];
    if (!h->addr && frame_now < pk.paused_until) return 0;   // an event handler of a mod over its budget
    CallFrame fr = {plat_now_ns(), 0, cur_frame};
    cur_frame = &fr;
    int outer_pkg = running_pkg;
    running_pkg = h->pkg;
    uint64_t outer = call_deadline_ns;   // a hook firing inside another script's call keeps the outer deadline
    if (!outer) call_deadline_ns = plat_now_ns() + call_budget_ns;
    lua_State *T = take_thread();
    lua_getref(T, ref);
    push(T);
    int skip = 0;
    if (lua_pcall(T, 1, 1, 0)) {
        char at[64];
        const char *where = h->addr ? rt_guest_name(h->addr, at, sizeof at) : "event handler";
        rt_log("lua: %s failed (%d): %s", where, ++h->errors, lua_tostring(T, -1));
        if (h->errors == 10) rt_log("lua: %s disabled after 10 errors", where);
    } else {
        skip = lua_toboolean(T, -1);
        Pkg &own = pkgs[h->pkg];   // indexed again: the vector may have grown during the call
        if (skip && own.caps >= 0 && !(own.caps & CAP_WRITE)) {   // skipping the game's own code changes the game
            if (!own.skip_noted) rt_log("lua: %s did not declare game.write; its hook may not skip the game function",
                                        own.id.c_str());
            own.skip_noted = true;
            skip = 0;
        }
    }
    give_thread(T);
    call_deadline_ns = outer;
    if (!outer) over_budget = false;
    uint64_t total = plat_now_ns() - fr.t0;
    cur_frame = fr.up;
    running_pkg = outer_pkg;
    if (fr.up) fr.up->excluded += total;
    pkgs[h->pkg].frame_ns += total > fr.excluded ? total - fr.excluded : 0;
    pkgs[h->pkg].frame_calls++;
    return skip;
}

// A hook call with a fresh ctx, which is dead once the call returns.
int run_hook(ScriptRef *h, bool before, Cpu *c) {
    vm_enter();
    Ctx *x = NULL;
    int skip = call_ref(h, before ? h->before : h->after, [&](lua_State *T) {
        x = (Ctx *)lua_newuserdata(T, sizeof(Ctx));
        x->c = c;
        x->owner = T;
        luaL_getmetatable(T, CTX);
        lua_setmetatable(T, -2);
    });
    if (x) x->c = NULL;   // still referenced until collected; any later use errors
    vm_leave();
    return skip;
}

int hook_before(Cpu *c, void *user) {
    ScriptRef *h = (ScriptRef *)user;
    return run_hook(h, true, c);
}

void hook_after(Cpu *c, void *user) {
    ScriptRef *h = (ScriptRef *)user;
    run_hook(h, false, c);
}

int api_hook(lua_State *l) {
    if (hooks.size() >= MAX_HOOKS) luaL_error(l, "too many hooks (%d)", (int)MAX_HOOKS);
    uint64_t a = to_u64(l, 1);
    ScriptRef *h = new ScriptRef;
    h->addr = a;
    h->pkg = owner_pkg();
    if (lua_isfunction(l, 2)) h->before = lua_ref(l, 2);
    if (lua_isfunction(l, 3)) h->after = lua_ref(l, 3);
    if (fz_hook(a, h->before != LUA_NOREF ? hook_before : NULL, h->after != LUA_NOREF ? hook_after : NULL, h, 50)) {
        delete h;
        luaL_error(l, "no lifted function starts at 0x%llx", (unsigned long long)a);
    }
    hooks.push_back(h);
    pkgs[h->pkg].hooks++;
    lua_pushboolean(l, 1);
    return 1;
}

int api_on(lua_State *l) {
    const char *ev = luaL_checkstring(l, 1);
    luaL_checktype(l, 2, LUA_TFUNCTION);
    int e = -1;
    for (int i = 0; i < (int)(sizeof EVENTS / sizeof *EVENTS); i++)
        if (!strcmp(ev, EVENTS[i])) e = i;
    if (e < 0) luaL_error(l, "unknown event '%s' (events: frame, world, menu, guard, perfect_guard)", ev);
    if (frame_handlers.size() >= MAX_HANDLERS) luaL_error(l, "too many event handlers (%d)", (int)MAX_HANDLERS);
    ScriptRef *h = new ScriptRef;
    h->event = e;
    h->pkg = owner_pkg();
    pkgs[h->pkg].handlers++;
    need_game_event(e);
    h->before = lua_ref(l, 2);
    frame_handlers.push_back(h);
    return 0;
}

void open_vm() {
    mem_used = 0;
    free_threads.clear();   // they belonged to the closed VM
    L = lua_newstate(limited_alloc, NULL);
    lua_callbacks(L)->interrupt = interrupt;
    luaL_openlibs(L);
    static const luaL_Reg ctx_methods[] = {{"x", ctx_x}, {"xs", ctx_xs}, {"setx", ctx_setx}, {"s", ctx_s},
                                           {"sets", ctx_sets}, {"call", ctx_call}, {NULL, NULL}};
    luaL_newmetatable(L, CTX);
    lua_newtable(L);
    luaL_register(L, NULL, ctx_methods);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);
    static const luaL_Reg view_meta[] = {{"__index", view_index}, {"__newindex", view_newindex},
                                         {"__tostring", view_tostring}, {NULL, NULL}};
    luaL_newmetatable(L, VIEW);
    luaL_register(L, NULL, view_meta);
    lua_pop(L, 1);
    static const luaL_Reg api[] = {
        {"log", api_log},         {"notify", api_notify},   {"main", api_main},     {"sym", api_sym},
        {"hook", api_hook},
        {"on", api_on},           {"read8", api_read8},     {"read16", api_read16}, {"read32", api_read32},
        {"read64", api_read64},   {"readf", api_readf},     {"write8", api_write8}, {"write16", api_write16},
        {"write32", api_write32}, {"writef", api_writef},   {"view", api_view},       {NULL, NULL}};
    lua_newtable(L);
    luaL_register(L, NULL, api);
    lua_setglobal(L, "fuse");
    luaL_sandbox(L);   // standard libraries and globals read-only from here; scripts run in their own thread env
}

// Compiles and runs one chunk in a sandboxed thread of the VM with a budget; 0 on success, else the error in *err. With
// nresults != 0 the results are appended to *err, tab-separated.
// The deployed packages' option values ("key=t:value"), by package: their scripts' `config` table.
std::vector<std::pair<std::string, std::string>> pkg_config;

void push_config(lua_State *T, const char *pkg) {   // a read-only table of pkg's option values
    lua_newtable(T);
    for (auto &kv : pkg_config) {
        if (kv.first != pkg) continue;
        const std::string &e = kv.second;
        size_t eq = e.find('=');
        lua_pushlstring(T, e.data(), eq);
        const char *v = e.c_str() + eq + 3;
        if (e[eq + 1] == 'b') lua_pushboolean(T, !strcmp(v, "true"));
        else if (e[eq + 1] == 'n') lua_pushnumber(T, strtod(v, NULL));
        else lua_pushstring(T, v);
        lua_rawset(T, -3);
    }
    lua_setreadonly(T, -1, 1);
}

// pkg: the package whose script this is (its `config` global holds the package's option values), or NULL.
int run_chunk(const char *name, const char *src, size_t len, std::string *err, int nresults, uint64_t budget_ns,
              const char *pkg = NULL) {
    size_t bclen = 0;
    char *bc = luau_compile(src, len, NULL, &bclen);
    lua_State *T = lua_newthread(L);
    luaL_sandboxthread(T);
    if (pkg) {
        push_config(T, pkg);
        lua_setglobal(T, "config");
    }
    int rc = luau_load(T, name, bc, bclen, 0);
    free(bc);
    uint64_t outer = call_deadline_ns;
    if (rc == 0) {
        call_deadline_ns = plat_now_ns() + budget_ns;
        rc = lua_pcall(T, 0, nresults, 0);
        call_deadline_ns = outer;
        if (!outer) over_budget = false;
    }
    if (rc) {
        if (err) *err = lua_tostring(T, -1) ? lua_tostring(T, -1) : "error";
        lua_pop(L, 1);   // the thread
        return rc;
    }
    if (nresults && err) {
        for (int i = 1, n = lua_gettop(T); i <= n; i++) {
            size_t sl;
            const char *s = luaL_tolstring(T, i, &sl);
            if (i > 1) *err += '\t';
            err->append(s, sl);
            lua_pop(T, 1);
        }
    }
    lua_pop(L, 1);
    return 0;
}

bool read_file(const std::string &path, std::string *out) {
    FILE *fp = fopen(path.c_str(), "rb");
    if (!fp) return false;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, fp)) > 0 && out->size() < (8u << 20)) out->append(buf, n);
    fclose(fp);
    return true;
}

// want: the hex SHA-256 the file must have (a deployed package's file), or NULL for a loose script; pkg: its package.
void load_file(const char *path, const char *shown, const char *want = NULL, const char *pkg = NULL) {
    std::string src;
    if (!read_file(path, &src)) {
        rt_log("lua: cannot read %s", shown);
        return;
    }
    if (want && !fz_file_sha256_hex(src.data(), src.size(), want)) {
        rt_log("lua: %s changed since it was deployed (deployed sha256 %s); not loaded", shown, want);
        return;
    }
    std::string err, name = std::string("=") + shown;
    loading_pkg = pkg ? pkg_index(pkg) : 0;
    int rc = run_chunk(name.c_str(), src.data(), src.size(), &err, 0, 2000000000ull, pkg);
    loading_pkg = 0;
    if (rc) {
        rt_log("lua: %s: %s", shown, err.c_str());
    } else {
        rt_log("lua: loaded %s", shown);
        loaded.push_back(shown);
    }
}

// *.luau names in dir (files, or directories with want_dirs), sorted; links are skipped so nothing loads from outside.
std::vector<std::string> list(const std::string &dir, bool want_dirs) {
    std::vector<std::string> out;
    WIN32_FIND_DATAA fd;
    HANDLE f = FindFirstFileA((dir + "\\*").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return out;
    do {
        std::string nm = fd.cFileName;
        if (nm == "." || nm == ".." || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        bool is_dir = fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY;
        if (want_dirs ? is_dir : !is_dir && nm.size() > 5 && nm.compare(nm.size() - 5, 5, ".luau") == 0)
            out.push_back(nm);
    } while (FindNextFileA(f, &fd));
    FindClose(f);
    std::sort(out.begin(), out.end());
    return out;
}

// A folder the fuse CLI deployed (it has .fuse-deploy.json): the listed .luau files in the resolved load order, each
// only if its SHA-256 still matches. true when the manifest was there.
// <dir>/.fuse-disabled: one package id per line (the ids the player turned off in the Mods tab). Not part of what
// `fuse deploy` writes or owns, so a deploy keeps it.
void read_disabled(const std::string &dir) {
    disabled_in_game.clear();
    std::string text;
    if (!read_file(dir + "\\.fuse-disabled", &text)) return;
    size_t at = 0;
    while (at < text.size()) {
        size_t nl = text.find('\n', at);
        std::string id = text.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
        while (!id.empty() && (id.back() == '\r' || id.back() == ' ')) id.pop_back();
        if (!id.empty() && id.size() <= 64) disabled_in_game.push_back(id);
        if (nl == std::string::npos) break;
        at = nl + 1;
    }
}

bool is_disabled(const char *id) {
    return std::find(disabled_in_game.begin(), disabled_in_game.end(), id) != disabled_in_game.end();
}

bool load_deployed(const std::string &dir) {
    struct Ctx {
        const std::string *dir;
    } ctx = {&dir};
    scripts_dir = dir;
    deployed.clear();
    read_disabled(dir);
    pkg_config.clear();
    // Each package's declared capabilities: its scripts are held to them (need()). A manifest from before the lists
    // were written has none; those packages run unrestricted until the next deploy.
    fz_deployed_caps(dir.c_str(), [](const char *id, const char *cap, void *) {
        Pkg &pk = pkgs[pkg_index(id)];
        if (!*cap) pk.caps = 0;
        else if (!strcmp(cap, "game.read")) pk.caps |= CAP_READ;
        else if (!strcmp(cap, "game.write")) pk.caps |= CAP_READ | CAP_WRITE;
    }, NULL);
    fz_deployed_config(dir.c_str(), [](const char *id, const char *entry, void *) { pkg_config.emplace_back(id, entry); },
                       NULL);
    int n = fz_deployed_files(dir.c_str(), ".luau", [](const char *id, const char *path, const char *sha, void *u) {
        if (std::find(deployed.begin(), deployed.end(), id) == deployed.end()) deployed.push_back(id);
        if (is_disabled(id)) return;   // turned off in game
        const std::string &d = *((Ctx *)u)->dir;
        std::string full = d + "\\" + id + "\\" + path, shown = std::string(id) + "/" + path;
        std::replace(full.begin(), full.end(), '/', '\\');
        load_file(full.c_str(), shown.c_str(), sha, id);
    }, &ctx);
    // Packages with only patches (fuse_patches.c applied them at load) are listed too, so the Mods tab can turn them
    // off for the next start.
    fz_deployed_files(dir.c_str(), ".fzpatch", [](const char *id, const char *, const char *, void *) {
        if (std::find(deployed.begin(), deployed.end(), id) == deployed.end()) deployed.push_back(id);
    }, NULL);
    for (auto &id : disabled_in_game)
        if (std::find(deployed.begin(), deployed.end(), id) != deployed.end())
            rt_log("lua: %s is turned off in game (.fuse-disabled); not loaded", id.c_str());
    if (n < 0) return false;
    rt_log("lua: %s: %d deployed scripts (.fuse-deploy.json)", dir.c_str(), n);
    return true;
}

// A folder the fuse CLI deployed: its manifest decides (load_deployed). Any other folder (an author's working copy):
// *.luau in it, then each subfolder's, one level deep, in name order.
void load_folder() {
    const char *d = cfg_get("TOTK_FUSE_SCRIPTS");
    if (!d || !*d) return;
    std::string dir = d;
    if (load_deployed(dir)) return;
    for (auto &nm : list(dir, false)) load_file((dir + "\\" + nm).c_str(), nm.c_str());
    for (auto &sd : list(dir, true))
        for (auto &nm : list(dir + "\\" + sd, false)) load_file((dir + "\\" + sd + "\\" + nm).c_str(), (sd + "/" + nm).c_str());
}

uint64_t cfg_num(const char *key, uint64_t def) {
    const char *s = cfg_get(key);
    return s && *s ? (uint64_t)atoi(s) : def;
}

}   // namespace

extern "C" void fz_luau_init(void) {
    InitializeCriticalSection(&vm_lock);
    call_budget_ns = cfg_num("TOTK_FUSE_SCRIPT_MS", 5) * 1000000ull;
    mem_cap = (size_t)cfg_num("TOTK_FUSE_SCRIPT_MB", 256) << 20;
    const char *mb = cfg_get("TOTK_FUSE_MOD_MS");   // each mod's budget per frame (ms, fractions allowed)
    if (mb && *mb && atof(mb) > 0) mod_budget_ms = atof(mb);
    pkgs.assign(1, Pkg());
    pkgs[0].id = "(console and loose scripts)";
    ready = 1;
    const char *dir = cfg_get("TOTK_FUSE_SCRIPTS");
    if (!dir || !*dir) return;
    vm_enter();
    open_vm();
    load_folder();
    vm_leave();
}

extern "C" int fz_luau_packages(FzPackage *out, int max) {   // out NULL: just the count
    if (!ready) return 0;
    vm_enter();
    int n = out ? 0 : (int)deployed.size();
    for (size_t i = 0; out && i < deployed.size() && n < max; i++, n++) {
        snprintf(out[n].id, sizeof out[n].id, "%s", deployed[i].c_str());
        out[n].enabled = !is_disabled(deployed[i].c_str());
    }
    vm_leave();
    return n;
}

extern "C" int fz_luau_set_enabled(const char *id, int on) {
    if (!ready || !id) return -1;
    vm_enter();
    bool known = std::find(deployed.begin(), deployed.end(), id) != deployed.end();
    int rc = -1;
    if (known && !scripts_dir.empty()) {
        std::vector<std::string> next;
        for (auto &d : disabled_in_game)
            if (d != id) next.push_back(d);
        if (!on) next.push_back(id);
        std::string text;
        for (auto &d : next) text += d + "\n";
        std::string path = scripts_dir + "\\.fuse-disabled", tmp = path + ".tmp";
        FILE *f = fopen(tmp.c_str(), "wb");
        if (f && fwrite(text.data(), 1, text.size(), f) == text.size() && fclose(f) == 0 &&
            MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            rc = 0;
            disabled_in_game = next;   // the choice holds even if the reload below is refused
        } else {
            if (f) fclose(f);
            rt_log("lua: cannot save %s", path.c_str());
        }
    }
    vm_leave();
    if (rc == 0) {
        rt_log("lua: %s turned %s in game", id, on ? "on" : "off");
        if (fz_luau_reload() < 0) rc = -1;
    }
    return rc;
}

extern "C" int fz_luau_list(char *buf, size_t cap) {
    if (!ready || !cap) return 0;
    vm_enter();
    std::string all;
    for (auto &s : loaded) all += (all.empty() ? "" : "\n") + s;
    int n = (int)loaded.size();
    vm_leave();
    snprintf(buf, cap, "%s", all.c_str());
    return n;
}

void fire(int event, double value, bool is_bool) {
    for (size_t i = 0; i < frame_handlers.size(); i++)
        if (frame_handlers[i]->event == event)
            call_ref(frame_handlers[i], frame_handlers[i]->before, [&](lua_State *T) {
                if (is_bool) lua_pushboolean(T, value != 0);
                else lua_pushnumber(T, value);
            });
}

void fire_text(int event, const char *text) {
    for (size_t i = 0; i < frame_handlers.size(); i++)
        if (frame_handlers[i]->event == event)
            call_ref(frame_handlers[i], frame_handlers[i]->before, [&](lua_State *T) { lua_pushstring(T, text); });
}

void end_frame(uint64_t frame) {   // under vm_lock
    frame_now = frame;
    for (Pkg &p : pkgs) {
        double ms = p.frame_ns / 1e6;
        p.last_calls = p.frame_calls;
        p.frame_ns = 0, p.frame_calls = 0;
        p.recent[p.recent_at] = (float)ms;
        p.recent_at = (p.recent_at + 1) % 120;
        if (p.warm > 0) {   // the first frames after loading are not judged
            p.warm--;
            continue;
        }
        p.avg_ms = p.avg_ms * 0.95 + ms * 0.05;
        p.over = p.avg_ms > mod_budget_ms ? p.over + 1 : 0;
        if (p.over >= 30 && frame >= p.paused_until && p.handlers) {
            p.pause_len = p.pause_len ? (p.pause_len * 2 < 1920 ? p.pause_len * 2 : 1920) : 60;
            p.paused_until = frame + (uint64_t)p.pause_len;
            p.over = 0;
            rt_log("fuse: %s uses %.2f ms per frame (budget %.2f); its event handlers pause for %d frames",
                   p.id.c_str(), p.avg_ms, mod_budget_ms, p.pause_len);
        }
    }
}

extern "C" int fz_luau_stats(FzModStat *out, int max, float *budget_ms) {
    if (budget_ms) *budget_ms = (float)mod_budget_ms;
    if (!ready) return 0;
    vm_enter();
    int n = 0;
    for (size_t i = 0; i < pkgs.size() && n < max; i++) {
        const Pkg &p = pkgs[i];
        if (!p.hooks && !p.handlers && !p.last_calls) continue;
        FzModStat &s = out[n++];
        snprintf(s.id, sizeof s.id, "%s", p.id.c_str());
        s.avg_ms = (float)p.avg_ms;
        s.max_ms = 0;
        for (float r : p.recent) s.max_ms = r > s.max_ms ? r : s.max_ms;
        s.calls = p.last_calls, s.hooks = p.hooks, s.handlers = p.handlers;
        s.status = frame_now < p.paused_until ? FZ_MOD_THROTTLED : p.avg_ms > mod_budget_ms / 2 ? FZ_MOD_BUSY : FZ_MOD_OK;
    }
    vm_leave();
    std::sort(out, out + n, [](const FzModStat &a, const FzModStat &b) { return a.avg_ms > b.avg_ms; });
    return n;
}

extern "C" void fz_luau_frame(uint64_t frame) {
    if (!ready) return;
    vm_enter();
    end_frame(frame);
    vm_leave();
    if (frame_handlers.empty()) return;
    bool world = frame_step_world_us() > 0, menu = menu_is_open() != 0;
    vm_enter();
    if (world != world_on) {
        world_on = world;
        if (world) fire(EV_WORLD, 1, true);
    }
    if (menu != menu_on) {
        menu_on = menu;
        fire(EV_MENU, menu, true);
    }
    fire(EV_FRAME, (double)frame, false);
    vm_leave();
}

extern "C" int fz_luau_reload(void) {
    if (!ready) return -1;
    vm_enter();
    if (calls_out) {   // a script is inside a game function and will come back to this VM
        vm_leave();
        rt_log("lua: not reloaded: a script is inside a game call (ctx:call); try again");
        return -1;
    }
    for (ScriptRef *h : hooks) fz_unhook(h);
    if (L) lua_close(L);
    L = NULL;
    for (ScriptRef *h : hooks) h->retired = true, retired.push_back(h);   // a call past fz_enter's check sees this
    for (ScriptRef *h : frame_handlers) delete h;                           // only called under vm_lock
    hooks.clear();
    frame_handlers.clear();
    world_on = menu_on = false;   // the new scripts hear the current state as their first world and menu events
    pkgs.assign(1, Pkg());
    pkgs[0].id = "(console and loose scripts)";
    loaded.clear();
    open_vm();
    load_folder();
    int n = (int)hooks.size();
    vm_leave();
    rt_log("lua: reloaded, %d hooks, %d frame handlers", n, (int)frame_handlers.size());
    return n;
}

// Runs code in the live VM (2 s budget). Returns 0 and the printed lines plus the results (tab-separated) in out, else
// the error.
extern "C" int fz_luau_eval(const char *code, char *out, size_t cap) {
    if (!ready) return -1;
    vm_enter();
    if (!L) open_vm();
    std::string printed, res;
    capture = &printed;
    int rc = run_chunk("=console", code, strlen(code), &res, LUA_MULTRET, 2000000000ull);
    capture = NULL;
    vm_leave();
    std::string all = printed + res;
    snprintf(out, cap, "%s", all.c_str());
    return rc;
}
