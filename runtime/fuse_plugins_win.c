// Fuse native plugins (see fuse_plugin.h): DLLs from TOTK_FUSE_PLUGINS, loaded at start only with TOTK_FUSE_NATIVE=1.
// A folder the fuse CLI deployed into loads exactly the plugins its .fuse-deploy.json lists, each hash-checked; any
// other folder loads its *.dll by name (an author's own builds). Each gets the FzApi table; its log lines carry its name. Plugins are never unloaded (their hooks may be running
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

// Prefixed with the plugin's name: during its init the one loading, afterwards (a hook calling back) the DLL that the
// call came from.
__attribute__((noinline)) static void api_log(const char *text) {
    char who[160];
    if (current >= 0) {
        snprintf(who, sizeof who, "%s", names[current]);
    } else {
        fz_owner_name(__builtin_return_address(0), who, sizeof who);
        char *plus = strchr(who, '+');
        if (plus) *plus = 0;
    }
    rt_log("plugin %s: %s", who, text ? text : "");
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

// Loads one plugin DLL and runs its init. want: the SHA-256 it must have (a deployed plugin), or NULL. The file is
// opened with write and delete sharing denied while it is hashed and loaded, so it cannot change in between.
static void load_plugin(const char *path, const char *shown, const char *want) {
    if (nplugins == FZ_PLUGINS) {
        rt_log("fuse: more than %d plugins; %s skipped", FZ_PLUGINS, shown);
        return;
    }
    HANDLE pin = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (pin == INVALID_HANDLE_VALUE) {
        rt_log("fuse: plugin %s cannot be opened (error %lu)", shown, GetLastError());
        return;
    }
    if (want) {
        LARGE_INTEGER sz;
        void *buf = NULL;
        DWORD got = 0;
        int ok = GetFileSizeEx(pin, &sz) && sz.QuadPart > 0 && sz.QuadPart < (256ll << 20) &&
                 (buf = malloc((size_t)sz.QuadPart)) && ReadFile(pin, buf, (DWORD)sz.QuadPart, &got, NULL) &&
                 got == (DWORD)sz.QuadPart && fz_file_sha256_hex(buf, got, want);
        free(buf);
        if (!ok) {
            rt_log("fuse: plugin %s changed since it was deployed (or cannot be read); not loaded", shown);
            CloseHandle(pin);
            return;
        }
    }
    HMODULE m = LoadLibraryExA(path, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    CloseHandle(pin);
    if (!m) {
        rt_log("fuse: plugin %s did not load (error %lu)", shown, GetLastError());
        return;
    }
    MODULEINFO mi;
    if (GetModuleInformation(GetCurrentProcess(), m, &mi, sizeof mi)) plat_crash_watch(mi.lpBaseOfDll, mi.SizeOfImage);
    FzPluginInit init = (FzPluginInit)(void *)GetProcAddress(m, "fz_plugin_init");
    if (!init) {
        rt_log("fuse: plugin %s exports no fz_plugin_init; unloaded", shown);
        FreeLibrary(m);
        return;
    }
    int k = nplugins++;
    snprintf(names[k], sizeof names[k], "%s", shown);
    current = k;
    int rc = init(&api);
    current = -1;
    if (rc) {
        rt_log("fuse: plugin %s refused to start (%d)", shown, rc);
        nplugins--;   // its hooks, if any, stay: a plugin that refuses must not have attached any
    } else {
        rt_log("fuse: plugin %s loaded", shown);
    }
}

static void load_deployed_plugin(const char *id, const char *path, const char *sha, void *user) {
    char full[MAX_PATH], shown[200];
    snprintf(full, sizeof full, "%s\\%s\\%s", (const char *)user, id, path);
    for (char *c = full; *c; c++)
        if (*c == '/') *c = '\\';
    snprintf(shown, sizeof shown, "%s/%s", id, path);
    load_plugin(full, shown, sha);
}

void fz_plugins_init(void) {
    const char *dir = cfg_get("TOTK_FUSE_PLUGINS");
    if (!dir || !*dir) return;
    // 1: deployed plugins only (each pinned by hash in .fuse-deploy.json). dev: also a folder of loose DLLs, for an
    // author's own builds.
    const char *native = cfg_get("TOTK_FUSE_NATIVE");
    int dev = native && !strcmp(native, "dev");
    if (!native || (strcmp(native, "1") && !dev)) {
        rt_log("fuse: TOTK_FUSE_PLUGINS is set but native mods are off (TOTK_FUSE_NATIVE=1 turns them on); no plugin loads");
        return;
    }
    // A folder the fuse CLI deployed into: exactly the listed plugins, hash-checked, in load order.
    int n = fz_deployed_files(dir, ".dll", load_deployed_plugin, (void *)dir);
    if (n >= 0) {
        rt_log("fuse: %s: %d deployed plugins (.fuse-deploy.json)", dir, n);
        return;
    }
    if (!dev) {
        rt_log("fuse: %s has no .fuse-deploy.json; loose plugins load only with TOTK_FUSE_NATIVE=dev", dir);
        return;
    }
    // Any other folder (an author's own builds, TOTK_FUSE_NATIVE=dev): every *.dll in it, by name.
    rt_log("fuse: TOTK_FUSE_NATIVE=dev: loading unpinned plugins from %s", dir);
    char pat[MAX_PATH];
    snprintf(pat, sizeof pat, "%s\\*.dll", dir);
    WIN32_FIND_DATAA fd;
    HANDLE f = FindFirstFileA(pat, &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        char path[MAX_PATH];
        snprintf(path, sizeof path, "%s\\%s", dir, fd.cFileName);
        load_plugin(path, fd.cFileName, NULL);
    } while (FindNextFileA(f, &fd));
    FindClose(f);
}
