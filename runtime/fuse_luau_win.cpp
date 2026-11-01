// Fuse Luau scripts (spike): *.luau files from TOTK_FUSE_SCRIPTS hook game functions through fuse_hooks.
//
// - One sandboxed VM (luaL_sandbox: read-only standard libraries, no io/os/package), behind a recursive lock, so hooks
//   from several guest threads take turns. Per-thread VMs come later; scripts do not call guest code yet, so the lock
//   cannot deadlock against guest locks.
// - The API, table `fuse`:
//     fuse.log(...)                         a log line, "lua: ..."
//     fuse.main(offset)                     guest address of main+offset (addresses fit a Luau number exactly)
//     fuse.hook(addr, before, after)        before(ctx) may return true to skip the original; after(ctx) sees the
//                                           return value. Either may be nil. Errors disable that hook after 10.
//     fuse.read32/read64/readf(addr), fuse.write32/writef(addr, v)   guest memory, unchecked (spike)
//   ctx (valid only during the call): ctx:x(i) / ctx:setx(i, v) general registers (exact up to 2^53; ctx:xs(i) is the
//   hex string), ctx:s(i) / ctx:sets(i, v) the float in s<i>.
// - fz_luau_reload: unhooks every script hook, closes the VM and loads the folder again (DevLink "script_reload").
//   fz_luau_eval: runs a chunk in the live VM and returns what it printed and returned (DevLink "lua", the console).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

#include <windows.h>

extern "C" {
#include "cfg.h"
#include "dispatch.h"
#include "fuse_hooks.h"
#include "host.h"
#include "platform/platform.h"
#if __has_include("layout.h")
#include "layout.h"
#endif
}

#include "lua.h"
#include "luacode.h"
#include "lualib.h"

namespace {

struct ScriptHook {
    int before = LUA_NOREF, after = LUA_NOREF;
    int errors = 0;
    uint64_t addr = 0;
};

CRITICAL_SECTION vm_lock;   // recursive: a script hook may fire inside another script's call
lua_State *L;
std::vector<ScriptHook *> hooks;   // owned; freed only at reload after fz_unhook (readers hold no pointers past a call)
std::string *capture;              // fz_luau_eval: where fuse.log also writes
int ready;

const char *const CTX = "fuse.ctx";

struct Ctx {
    Cpu *c;
};

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

int api_main(lua_State *l) {
#ifdef MOD_MAIN_BASE
    lua_pushnumber(l, (double)(MOD_MAIN_BASE + (uint64_t)luaL_checknumber(l, 1)));
#else
    lua_pushnil(l);
#endif
    return 1;
}

uint64_t addr_arg(lua_State *l, int i) { return (uint64_t)luaL_checknumber(l, i); }

int api_read32(lua_State *l) { lua_pushnumber(l, LD32(addr_arg(l, 1))); return 1; }
int api_read64(lua_State *l) { lua_pushnumber(l, (double)LD64(addr_arg(l, 1))); return 1; }
int api_readf(lua_State *l) {
    uint32_t w = LD32(addr_arg(l, 1));
    float f;
    memcpy(&f, &w, 4);
    lua_pushnumber(l, f);
    return 1;
}
int api_write32(lua_State *l) { ST32(addr_arg(l, 1), (uint32_t)luaL_checknumber(l, 2)); return 0; }
int api_writef(lua_State *l) {
    float f = (float)luaL_checknumber(l, 2);
    uint32_t w;
    memcpy(&w, &f, 4);
    ST32(addr_arg(l, 1), w);
    return 0;
}

// Runs the hook's function with a fresh ctx; returns its first result as a boolean (before hooks: skip).
int run(ScriptHook *h, int ref, Cpu *c) {
    if (ref == LUA_NOREF || h->errors >= 10) return 0;
    EnterCriticalSection(&vm_lock);
    int skip = 0;
    if (L) {
        lua_getref(L, ref);
        Ctx *x = (Ctx *)lua_newuserdata(L, sizeof(Ctx));
        x->c = c;
        luaL_getmetatable(L, CTX);
        lua_setmetatable(L, -2);
        lua_pushvalue(L, -1);
        int ctx_ref = lua_ref(L, -1);
        lua_pop(L, 1);
        if (lua_pcall(L, 1, 1, 0)) {
            char at[64];
            rt_log("lua: hook at %s failed (%d): %s", rt_guest_name(h->addr, at, sizeof at), ++h->errors,
                   lua_tostring(L, -1));
            if (h->errors == 10) rt_log("lua: hook disabled after 10 errors");
        } else {
            skip = lua_toboolean(L, -1);
        }
        lua_pop(L, 1);
        lua_getref(L, ctx_ref);
        ((Ctx *)lua_touserdata(L, -1))->c = NULL;   // a ctx kept by the script is dead from here
        lua_pop(L, 1);
        lua_unref(L, ctx_ref);
    }
    LeaveCriticalSection(&vm_lock);
    return skip;
}

int hook_before(Cpu *c, void *user) {
    ScriptHook *h = (ScriptHook *)user;
    return run(h, h->before, c);
}

void hook_after(Cpu *c, void *user) {
    ScriptHook *h = (ScriptHook *)user;
    run(h, h->after, c);
}

int api_hook(lua_State *l) {
    uint64_t a = addr_arg(l, 1);
    ScriptHook *h = new ScriptHook;
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

void open_vm() {
    L = luaL_newstate();
    luaL_openlibs(L);
    static const luaL_Reg ctx_methods[] = {{"x", ctx_x}, {"xs", ctx_xs}, {"setx", ctx_setx}, {"s", ctx_s},
                                           {"sets", ctx_sets}, {NULL, NULL}};
    luaL_newmetatable(L, CTX);
    lua_newtable(L);
    luaL_register(L, NULL, ctx_methods);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);
    static const luaL_Reg api[] = {{"log", api_log}, {"main", api_main}, {"hook", api_hook}, {"read32", api_read32},
                                   {"read64", api_read64}, {"readf", api_readf}, {"write32", api_write32},
                                   {"writef", api_writef}, {NULL, NULL}};
    lua_newtable(L);
    luaL_register(L, NULL, api);
    lua_setglobal(L, "fuse");
    luaL_sandbox(L);   // standard libraries and globals read-only from here; scripts run in their own thread env
}

// Compiles and runs one chunk in a sandboxed thread of the VM; 0 on success, else the error in *err.
int run_chunk(const char *name, const char *src, size_t len, std::string *err, int nresults) {
    size_t bclen = 0;
    char *bc = luau_compile(src, len, NULL, &bclen);
    lua_State *T = lua_newthread(L);
    luaL_sandboxthread(T);
    int rc = luau_load(T, name, bc, bclen, 0);
    free(bc);
    if (rc == 0) rc = lua_pcall(T, 0, nresults, 0);
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

void load_folder() {
    const char *dir = cfg_get("TOTK_FUSE_SCRIPTS");
    if (!dir || !*dir) return;
    char pat[MAX_PATH];
    snprintf(pat, sizeof pat, "%s\\*.luau", dir);
    WIN32_FIND_DATAA fd;
    HANDLE f = FindFirstFileA(pat, &fd);
    if (f == INVALID_HANDLE_VALUE) {
        rt_log("lua: no *.luau in %s", dir);
        return;
    }
    do {
        char path[MAX_PATH];
        snprintf(path, sizeof path, "%s\\%s", dir, fd.cFileName);
        FILE *fp = fopen(path, "rb");
        if (!fp) continue;
        std::string src;
        char buf[65536];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, fp)) > 0) src.append(buf, n);
        fclose(fp);
        std::string err;
        std::string name = std::string("=") + fd.cFileName;
        if (run_chunk(name.c_str(), src.data(), src.size(), &err, 0)) rt_log("lua: %s: %s", fd.cFileName, err.c_str());
        else rt_log("lua: loaded %s", fd.cFileName);
    } while (FindNextFileA(f, &fd));
    FindClose(f);
}

}   // namespace

extern "C" void fz_luau_init(void) {
    InitializeCriticalSection(&vm_lock);
    ready = 1;
    const char *dir = cfg_get("TOTK_FUSE_SCRIPTS");
    if (!dir || !*dir) return;
    EnterCriticalSection(&vm_lock);
    open_vm();
    load_folder();
    LeaveCriticalSection(&vm_lock);
}

extern "C" int fz_luau_reload(void) {
    if (!ready) return -1;
    for (ScriptHook *h : hooks) fz_unhook(h);
    EnterCriticalSection(&vm_lock);
    if (L) lua_close(L);
    L = NULL;
    for (ScriptHook *h : hooks) delete h;   // unhooked above; a call already inside run() holds vm_lock, so none is live
    hooks.clear();
    open_vm();
    load_folder();
    int n = (int)hooks.size();
    LeaveCriticalSection(&vm_lock);
    rt_log("lua: reloaded, %d hooks", n);
    return n;
}

// Runs code in the live VM. Returns 0 and the printed lines plus the results (tab-separated) in out, else the error.
extern "C" int fz_luau_eval(const char *code, char *out, size_t cap) {
    if (!ready) return -1;
    EnterCriticalSection(&vm_lock);
    if (!L) open_vm();
    std::string printed, res;
    capture = &printed;
    int rc = run_chunk("=console", code, strlen(code), &res, LUA_MULTRET);
    capture = NULL;
    LeaveCriticalSection(&vm_lock);
    std::string all = printed + res;
    snprintf(out, cap, "%s", all.c_str());
    return rc;
}
