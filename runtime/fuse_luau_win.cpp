// Fuse Luau scripts (spike): *.luau files from TOTK_FUSE_SCRIPTS hook game functions through fuse_hooks and react to
// events. A folder the fuse CLI deployed loads exactly what its .fuse-deploy.json lists, in the resolved load order, and
// refuses any file whose SHA-256 no longer matches; any other folder loads its *.luau and its subfolders' (one level).
//
// Trust model (docs/decisions/0002-trust-and-permissions.md in the Fuse repository): a script gets the whole game and
// nothing of the machine.
// - One VM, sandboxed with luaL_sandbox (read-only standard libraries; no io, os.execute, package, loadfile, require of
//   files, debug beyond tracebacks), each script in its own sandboxed thread environment. No network, no files, no
//   processes: the host gives scripts no such functions.
// - Memory access is guest memory only: every read and write is checked with mem_guest_access (the guest heap, alias
//   region, stack, thread-local region or a loaded module's pages with the right permission), so a script cannot
//   reach the runtime's own data or code pointers.
// - Budgets: every call into a script (hook, event, load, console) has a CPU budget (TOTK_FUSE_SCRIPT_MS, default 5 ms;
//   the console and loading 2 s) enforced by Luau's interrupt callback; the VM's memory is capped
//   (TOTK_FUSE_SCRIPT_MB, default 256). A hook or handler that fails 10 times is disabled.
// - The VM sits behind one recursive lock, so hooks from several guest threads take turns (per-thread VMs come later;
//   scripts cannot call guest code yet, so the lock cannot deadlock against guest locks).
//
// The API, table `fuse`:
//   fuse.log(...)                         a log line, "lua: ..."
//   fuse.notify(text [, seconds])         a message at the top of the game's screen
//   fuse.main(offset)                     guest address of main+offset (addresses fit a Luau number exactly)
//   fuse.sym(name)                        guest address of a named function from TOTK_FUSE_SYMBOLS (errors if unknown)
//   fuse.hook(addr, before, after)        before(ctx) may return true to skip the original; after(ctx) sees the return
//                                         value. Either may be nil.
//   fuse.on("frame", fn)                  fn(frame) once per presented frame
//   fuse.read8/read16/read32/read64/readf(addr), fuse.write8/write16/write32/writef(addr, v)   guest memory, checked
//   ctx (valid only during the call): ctx:x(i) / ctx:setx(i, v) general registers (exact up to 2^53; ctx:xs(i) is the
//   hex string), ctx:s(i) / ctx:sets(i, v) the float in s<i>.
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

namespace {

struct ScriptRef {   // a hook or an event handler: one or two functions in the VM's registry
    int before = LUA_NOREF, after = LUA_NOREF;
    int errors = 0;
    uint64_t addr = 0;   // 0 for event handlers
};

CRITICAL_SECTION vm_lock;   // recursive: a script hook may fire inside another script's call
lua_State *L;
std::vector<ScriptRef *> hooks;    // owned; freed only at reload after fz_unhook (no reader holds one past a call)
std::vector<ScriptRef *> frame_handlers;
std::string *capture;              // fz_luau_eval: where fuse.log also writes
std::vector<std::string> loaded;   // the script files that loaded (the Mods tab)
int ready;
size_t mem_used, mem_cap;
uint64_t call_budget_ns, call_deadline_ns;   // the running call's deadline; 0 = none

const char *const CTX = "fuse.ctx";

struct Ctx {
    Cpu *c;
};

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
    if (gc >= 0 || !call_deadline_ns) return;   // a GC step, or no budget running
    if (plat_now_ns() > call_deadline_ns) {
        call_deadline_ns = 0;   // once: the error unwinds the call
        luaL_error(l, "script ran over its CPU budget");
    }
}

// ---- guest memory, checked against the guest memory map

int guest_ok(uint64_t a, uint64_t n, int write) { return mem_guest_access(a, n, write); }

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
    return x->c;
}

int reg_index(lua_State *l, int arg, int hi) {
    int i = luaL_checkinteger(l, arg);
    if (i < 0 || i > hi) luaL_error(l, "register %d out of range", i);
    return i;
}

int ctx_x(lua_State *l) {
    Cpu *c = ctx_cpu(l);
    lua_pushnumber(l, (double)c->x[reg_index(l, 2, 30)]);
    return 1;
}

int ctx_xs(lua_State *l) {
    Cpu *c = ctx_cpu(l);
    char b[24];
    snprintf(b, sizeof b, "0x%llx", (unsigned long long)c->x[reg_index(l, 2, 30)]);
    lua_pushstring(l, b);
    return 1;
}

int ctx_setx(lua_State *l) {
    Cpu *c = ctx_cpu(l);
    int i = reg_index(l, 2, 30);
    c->x[i] = (uint64_t)(int64_t)luaL_checknumber(l, 3);
    return 0;
}

int ctx_s(lua_State *l) {
    Cpu *c = ctx_cpu(l);
    uint32_t w = (uint32_t)c->v[reg_index(l, 2, 31)];
    float f;
    memcpy(&f, &w, 4);
    lua_pushnumber(l, f);
    return 1;
}

int ctx_sets(lua_State *l) {
    Cpu *c = ctx_cpu(l);
    int i = reg_index(l, 2, 31);
    float f = (float)luaL_checknumber(l, 3);
    uint32_t w;
    memcpy(&w, &f, 4);
    c->v[i] = (c->v[i] & ~(__uint128_t)0xFFFFFFFFu) | w;
    return 0;
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
    lua_pushnumber(l, (double)(MOD_MAIN_BASE + (uint64_t)luaL_checknumber(l, 1)));
#else
    lua_pushnil(l);
#endif
    return 1;
}

int api_read8(lua_State *l) { lua_pushnumber(l, LD8(checked(l, 1, 1, 0))); return 1; }
int api_read16(lua_State *l) { lua_pushnumber(l, LD16(checked(l, 1, 2, 0))); return 1; }
int api_read32(lua_State *l) { lua_pushnumber(l, LD32(checked(l, 1, 4, 0))); return 1; }
int api_read64(lua_State *l) { lua_pushnumber(l, (double)LD64(checked(l, 1, 8, 0))); return 1; }
int api_readf(lua_State *l) {
    uint32_t w = LD32(checked(l, 1, 4, 0));
    float f;
    memcpy(&f, &w, 4);
    lua_pushnumber(l, f);
    return 1;
}
int api_write8(lua_State *l) { ST8(checked(l, 1, 1, 1), (uint8_t)luaL_checknumber(l, 2)); return 0; }
int api_write16(lua_State *l) { ST16(checked(l, 1, 2, 1), (uint16_t)luaL_checknumber(l, 2)); return 0; }
int api_write32(lua_State *l) { ST32(checked(l, 1, 4, 1), (uint32_t)luaL_checknumber(l, 2)); return 0; }
int api_writef(lua_State *l) {
    uint64_t a = checked(l, 1, 4, 1);
    float f = (float)luaL_checknumber(l, 2);
    uint32_t w;
    memcpy(&w, &f, 4);
    ST32(a, w);
    return 0;
}

// Calls the function in the registry with one argument pushed by push(); returns its first result as a boolean
// (before hooks: skip). Under vm_lock, with the call's CPU budget.
template <class Push> int call_ref(ScriptRef *h, int ref, Push push) {
    if (ref == LUA_NOREF || h->errors >= 10 || !L) return 0;
    uint64_t outer = call_deadline_ns;   // a hook firing inside another script's call keeps the outer deadline
    if (!outer) call_deadline_ns = plat_now_ns() + call_budget_ns;
    lua_getref(L, ref);
    push();
    int skip = 0;
    if (lua_pcall(L, 1, 1, 0)) {
        char at[64];
        const char *where = h->addr ? rt_guest_name(h->addr, at, sizeof at) : "frame handler";
        rt_log("lua: %s failed (%d): %s", where, ++h->errors, lua_tostring(L, -1));
        if (h->errors == 10) rt_log("lua: %s disabled after 10 errors", where);
    } else {
        skip = lua_toboolean(L, -1);
    }
    lua_pop(L, 1);
    call_deadline_ns = outer;
    return skip;
}

// A hook call with a fresh ctx, which is dead once the call returns.
int run_hook(ScriptRef *h, int ref, Cpu *c) {
    if (ref == LUA_NOREF || h->errors >= 10) return 0;
    EnterCriticalSection(&vm_lock);
    Ctx *x = NULL;
    int skip = call_ref(h, ref, [&] {
        x = (Ctx *)lua_newuserdata(L, sizeof(Ctx));
        x->c = c;
        luaL_getmetatable(L, CTX);
        lua_setmetatable(L, -2);
    });
    if (x) x->c = NULL;   // still referenced until collected; any later use errors
    LeaveCriticalSection(&vm_lock);
    return skip;
}

int hook_before(Cpu *c, void *user) {
    ScriptRef *h = (ScriptRef *)user;
    return run_hook(h, h->before, c);
}

void hook_after(Cpu *c, void *user) {
    ScriptRef *h = (ScriptRef *)user;
    run_hook(h, h->after, c);
}

int api_hook(lua_State *l) {
    uint64_t a = (uint64_t)luaL_checknumber(l, 1);
    ScriptRef *h = new ScriptRef;
    h->addr = a;
    if (lua_isfunction(l, 2)) h->before = lua_ref(l, 2);
    if (lua_isfunction(l, 3)) h->after = lua_ref(l, 3);
    if (fz_hook(a, h->before != LUA_NOREF ? hook_before : NULL, h->after != LUA_NOREF ? hook_after : NULL, h, 50)) {
        delete h;
        luaL_error(l, "no lifted function starts at 0x%llx", (unsigned long long)a);
    }
    hooks.push_back(h);
    lua_pushboolean(l, 1);
    return 1;
}

int api_on(lua_State *l) {
    const char *ev = luaL_checkstring(l, 1);
    luaL_checktype(l, 2, LUA_TFUNCTION);
    if (strcmp(ev, "frame")) luaL_error(l, "unknown event '%s' (events: frame)", ev);
    ScriptRef *h = new ScriptRef;
    h->before = lua_ref(l, 2);
    frame_handlers.push_back(h);
    return 0;
}

void open_vm() {
    mem_used = 0;
    L = lua_newstate(limited_alloc, NULL);
    lua_callbacks(L)->interrupt = interrupt;
    luaL_openlibs(L);
    static const luaL_Reg ctx_methods[] = {{"x", ctx_x}, {"xs", ctx_xs}, {"setx", ctx_setx}, {"s", ctx_s},
                                           {"sets", ctx_sets}, {NULL, NULL}};
    luaL_newmetatable(L, CTX);
    lua_newtable(L);
    luaL_register(L, NULL, ctx_methods);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);
    static const luaL_Reg api[] = {
        {"log", api_log},         {"notify", api_notify},   {"main", api_main},     {"sym", api_sym},
        {"hook", api_hook},
        {"on", api_on},           {"read8", api_read8},     {"read16", api_read16}, {"read32", api_read32},
        {"read64", api_read64},   {"readf", api_readf},     {"write8", api_write8}, {"write16", api_write16},
        {"write32", api_write32}, {"writef", api_writef},   {NULL, NULL}};
    lua_newtable(L);
    luaL_register(L, NULL, api);
    lua_setglobal(L, "fuse");
    luaL_sandbox(L);   // standard libraries and globals read-only from here; scripts run in their own thread env
}

// Compiles and runs one chunk in a sandboxed thread of the VM with a budget; 0 on success, else the error in *err. With
// nresults != 0 the results are appended to *err, tab-separated.
int run_chunk(const char *name, const char *src, size_t len, std::string *err, int nresults, uint64_t budget_ns) {
    size_t bclen = 0;
    char *bc = luau_compile(src, len, NULL, &bclen);
    lua_State *T = lua_newthread(L);
    luaL_sandboxthread(T);
    int rc = luau_load(T, name, bc, bclen, 0);
    free(bc);
    uint64_t outer = call_deadline_ns;
    if (rc == 0) {
        call_deadline_ns = plat_now_ns() + budget_ns;
        rc = lua_pcall(T, 0, nresults, 0);
        call_deadline_ns = outer;
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

// want: the hex SHA-256 the file must have (a deployed package's file), or NULL for a loose script.
void load_file(const char *path, const char *shown, const char *want = NULL) {
    std::string src;
    if (!read_file(path, &src)) {
        rt_log("lua: cannot read %s", shown);
        return;
    }
    if (want) {
        uint8_t h[32];
        char hex[65];
        rt_sha256(src.data(), src.size(), h);
        for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", h[i]);
        if (strcmp(hex, want)) {
            rt_log("lua: %s changed since it was deployed (sha256 %s, deployed %s); not loaded", shown, hex, want);
            return;
        }
    }
    std::string err, name = std::string("=") + shown;
    if (run_chunk(name.c_str(), src.data(), src.size(), &err, 0, 2000000000ull)) {
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

// The JSON string tokens of a .fuse-deploy.json in order (keys and values alike; the format is our own CLI's, written
// with plain ASCII ids and paths). Escapes other than \" and \\ are not expected and end the string.
std::vector<std::string> json_strings(const std::string &s) {
    std::vector<std::string> out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != '"') continue;
        std::string v;
        for (i++; i < s.size() && s[i] != '"'; i++) {
            if (s[i] == '\\' && i + 1 < s.size()) i++;
            v += s[i];
        }
        out.push_back(v);
    }
    return out;
}

// A folder the fuse CLI deployed (it has .fuse-deploy.json): the listed packages in their resolved load order, each
// file of theirs that is a .luau, and only if its SHA-256 still matches. 1 when the manifest was there.
bool load_deployed(const std::string &dir) {
    std::string text;
    if (!read_file(dir + "\\.fuse-deploy.json", &text)) return false;
    std::vector<std::string> t = json_strings(text);
    std::string id, path;
    int n = 0;
    for (size_t i = 0; i + 1 < t.size(); i++) {
        if (t[i] == "id") id = t[++i];
        else if (t[i] == "path") path = t[++i];
        else if (t[i] == "sha256" && !id.empty() && !path.empty()) {
            const std::string &want = t[++i];
            bool safe = path.find("..") == std::string::npos && path.find(':') == std::string::npos && path[0] != '/' &&
                        path[0] != '\\' && id.find_first_of("/\\:") == std::string::npos && id[0] != '.';
            if (safe && path.size() > 5 && path.compare(path.size() - 5, 5, ".luau") == 0) {
                std::string full = dir + "\\" + id + "\\" + path, shown = id + "/" + path;
                std::replace(full.begin(), full.end(), '/', '\\');
                load_file(full.c_str(), shown.c_str(), want.c_str());
                n++;
            }
            path.clear();
        }
    }
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
    ready = 1;
    const char *dir = cfg_get("TOTK_FUSE_SCRIPTS");
    if (!dir || !*dir) return;
    EnterCriticalSection(&vm_lock);
    open_vm();
    load_folder();
    LeaveCriticalSection(&vm_lock);
}

extern "C" int fz_luau_list(char *buf, size_t cap) {
    if (!ready || !cap) return 0;
    EnterCriticalSection(&vm_lock);
    std::string all;
    for (auto &s : loaded) all += (all.empty() ? "" : "\n") + s;
    int n = (int)loaded.size();
    LeaveCriticalSection(&vm_lock);
    snprintf(buf, cap, "%s", all.c_str());
    return n;
}

extern "C" void fz_luau_frame(uint64_t frame) {
    if (!ready || frame_handlers.empty()) return;
    EnterCriticalSection(&vm_lock);
    for (size_t i = 0; i < frame_handlers.size(); i++)
        call_ref(frame_handlers[i], frame_handlers[i]->before, [&] { lua_pushnumber(L, (double)frame); });
    LeaveCriticalSection(&vm_lock);
}

extern "C" int fz_luau_reload(void) {
    if (!ready) return -1;
    for (ScriptRef *h : hooks) fz_unhook(h);
    EnterCriticalSection(&vm_lock);
    if (L) lua_close(L);
    L = NULL;
    for (ScriptRef *h : hooks) delete h;   // unhooked above; a call already inside run_hook holds vm_lock
    for (ScriptRef *h : frame_handlers) delete h;
    hooks.clear();
    frame_handlers.clear();
    loaded.clear();
    open_vm();
    load_folder();
    int n = (int)hooks.size();
    LeaveCriticalSection(&vm_lock);
    rt_log("lua: reloaded, %d hooks, %d frame handlers", n, (int)frame_handlers.size());
    return n;
}

// Runs code in the live VM (2 s budget). Returns 0 and the printed lines plus the results (tab-separated) in out, else
// the error.
extern "C" int fz_luau_eval(const char *code, char *out, size_t cap) {
    if (!ready) return -1;
    EnterCriticalSection(&vm_lock);
    if (!L) open_vm();
    std::string printed, res;
    capture = &printed;
    int rc = run_chunk("=console", code, strlen(code), &res, LUA_MULTRET, 2000000000ull);
    capture = NULL;
    LeaveCriticalSection(&vm_lock);
    std::string all = printed + res;
    snprintf(out, cap, "%s", all.c_str());
    return rc;
}
