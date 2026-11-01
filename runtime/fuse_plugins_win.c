// Fuse native plugins (see fuse_plugin.h): DLLs from TOTK_FUSE_PLUGINS, loaded at start only with TOTK_FUSE_NATIVE=1.
// Each gets the FzApi table; its log lines carry its file name. Plugins are never unloaded (their hooks may be running
// on any guest thread), so changing one means a restart.
#include <windows.h>
#include <psapi.h>

#include <stdio.h>
#include <string.h>

#include "cfg.h"
#include "fuse_hooks.h"
#include "fuse_plugin.h"
#include "host.h"
#include "platform/platform.h"
#include "ui/menu.h"
#if __has_include("layout.h")
#include "layout.h"
#endif

#define FZ_PLUGINS 32

static char names[FZ_PLUGINS][64];   // file names, for log lines
static int nplugins;
static __thread int current = -1;    // the plugin whose init is running (its log prefix)

static void api_log(const char *text) {
    rt_log("plugin %s: %s", current >= 0 ? names[current] : "?", text ? text : "");
}

static uint64_t api_get_x(const FzCpu *c, int i) { return i >= 0 && i <= 30 ? ((const Cpu *)c)->x[i] : 0; }
static void api_set_x(FzCpu *c, int i, uint64_t v) { if (i >= 0 && i <= 30) ((Cpu *)c)->x[i] = v; }

static float api_get_s(const FzCpu *c, int i) {
    if (i < 0 || i > 31) return 0;
    uint32_t w = (uint32_t)((const Cpu *)c)->v[i];
    float f;
    memcpy(&f, &w, 4);
    return f;
}

static void api_set_s(FzCpu *c, int i, float f) {
    if (i < 0 || i > 31) return;
    uint32_t w;
    memcpy(&w, &f, 4);
    Cpu *cpu = (Cpu *)c;
    cpu->v[i] = (cpu->v[i] & ~(__uint128_t)0xFFFFFFFFu) | w;
}

// The plugins' FzBefore/FzAfter take an opaque FzCpu: the same pointer as the runtime's Cpu.
static int api_hook(uint64_t addr, FzBefore before, FzAfter after, void *user, int priority) {
    return fz_hook(addr, (fz_before_fn)(void *)before, (fz_after_fn)(void *)after, user, priority);
}

static const FzApi api = {
    .abi_version = FZ_API_VERSION,
    .size = sizeof(FzApi),
    .game_version = "1.2.1",
#ifdef MOD_MAIN_BASE
    .main_base = MOD_MAIN_BASE,
#endif
    .log = api_log,
    .notify = menu_notice,
    .cfg_get = cfg_get,
    .hook = api_hook,
    .unhook = fz_unhook,
    .get_x = api_get_x,
    .set_x = api_set_x,
    .get_s = api_get_s,
    .set_s = api_set_s,
    .guest_access = mem_guest_access,
    .sym = fz_sym,
};

int fz_plugins_list(char *buf, size_t cap) {
    size_t n = 0;
    if (cap) buf[0] = 0;
    for (int i = 0; i < nplugins && n + 1 < cap; i++) {
        int k = snprintf(buf + n, cap - n, "%s%s", n ? "\n" : "", names[i]);
        if (k > 0) n += (size_t)k < cap - n ? (size_t)k : cap - n - 1;
    }
    return nplugins;
}

const char *fz_owner_name(const void *fn, char *buf, size_t cap) {
    HMODULE m = NULL;
    char path[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)fn, &m) && m)
        GetModuleFileNameA(m, path, sizeof path);
    const char *base = strrchr(path, '\\');
    snprintf(buf, cap, "%s+%#llx", base ? base + 1 : path, (unsigned long long)((uintptr_t)fn - (uintptr_t)m));
    return buf;
}

void fz_plugins_init(void) {
    const char *dir = cfg_get("TOTK_FUSE_PLUGINS");
    if (!dir || !*dir) return;
    const char *native = cfg_get("TOTK_FUSE_NATIVE");
    if (!native || strcmp(native, "1")) {
        rt_log("fuse: TOTK_FUSE_PLUGINS is set but native mods are off (TOTK_FUSE_NATIVE=1 turns them on); no plugin loads");
        return;
    }
    char pat[MAX_PATH];
    snprintf(pat, sizeof pat, "%s\\*.dll", dir);
    WIN32_FIND_DATAA fd;
    HANDLE f = FindFirstFileA(pat, &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        if (nplugins == FZ_PLUGINS) {
            rt_log("fuse: more than %d plugins; the rest are skipped", FZ_PLUGINS);
            break;
        }
        char path[MAX_PATH];
        snprintf(path, sizeof path, "%s\\%s", dir, fd.cFileName);
        HMODULE m = LoadLibraryExA(path, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!m) {
            rt_log("fuse: plugin %s did not load (error %lu)", fd.cFileName, GetLastError());
            continue;
        }
        MODULEINFO mi;
        if (GetModuleInformation(GetCurrentProcess(), m, &mi, sizeof mi)) plat_crash_watch(mi.lpBaseOfDll, mi.SizeOfImage);
        FzPluginInit init = (FzPluginInit)(void *)GetProcAddress(m, "fz_plugin_init");
        if (!init) {
            rt_log("fuse: plugin %s exports no fz_plugin_init; unloaded", fd.cFileName);
            FreeLibrary(m);
            continue;
        }
        int k = nplugins++;
        snprintf(names[k], sizeof names[k], "%s", fd.cFileName);
        current = k;
        int rc = init(&api);
        current = -1;
        if (rc) {
            rt_log("fuse: plugin %s refused to start (%d)", fd.cFileName, rc);
            nplugins--;   // its hooks, if any, stay: a plugin that refuses must not have attached any
        } else {
            rt_log("fuse: plugin %s loaded", fd.cFileName);
        }
    } while (FindNextFileA(f, &fd));
    FindClose(f);
}
