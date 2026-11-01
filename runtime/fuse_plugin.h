// Fuse native plugin API (fuse_plugins_win.c). This header is also what plugin authors compile against (the Fuse SDK
// ships a copy), so it uses only C99 and fixed-width types and never exposes the runtime's internal structures: the
// guest CPU state is an opaque FzCpu, read and written through the accessors below.
//
// A plugin is a DLL that exports
//     int fz_plugin_init(const FzApi *api);
// called once at start; nonzero refuses the load. Check api->abi_version >= the version you were built for and
// api->size before using a field: the table only ever grows (fields are appended, never removed or reordered).
//
// Native plugins run with the game's full privileges: Fuse loads them only when the player turned native mods on
// (TOTK_FUSE_NATIVE=1) and points TOTK_FUSE_PLUGINS at a folder of them; the manager adds its trust checks (attested
// builds, pinned hashes, per-mod opt-in) before a DLL lands there.
#pragma once
#include <stdint.h>

#define FZ_API_VERSION 1

typedef struct FzCpu FzCpu;   // opaque: the guest CPU state of the thread running the hooked function

// before: arguments are in the registers as the caller left them; return nonzero to skip the original (set the return
// value first). after: runs once the body returned or was skipped; the return value is in x0 / s0.
typedef int (*FzBefore)(FzCpu *c, void *user);
typedef void (*FzAfter)(FzCpu *c, void *user);

typedef struct FzApi {
    uint32_t abi_version;   // FZ_API_VERSION of the runtime
    uint32_t size;          // sizeof(FzApi) of the runtime
    const char *game_version;   // "1.2.1"
    uint64_t main_base;     // guest address of the main module (offsets in symbol maps are relative to it)

    void (*log)(const char *text);                      // one log line, prefixed with the plugin's name
    void (*notify)(const char *text, double seconds);   // a message at the top of the game's screen
    const char *(*cfg_get)(const char *key);            // a setting (TOTK_<KEY> or the totk.ini key), NULL if unset

    // Hooks on the lifted function starting at a guest address; 0 on success, -1 when no function starts there.
    // Hooks of one function run by priority (higher first). unhook removes every hook attached with that user pointer.
    int (*hook)(uint64_t addr, FzBefore before, FzAfter after, void *user, int priority);
    int (*unhook)(void *user);

    // Registers of the hooked call: x0-x30, and the low 32 bits of v0-v31 as a float (s0-s31).
    uint64_t (*get_x)(const FzCpu *c, int i);
    void (*set_x)(FzCpu *c, int i, uint64_t v);
    float (*get_s)(const FzCpu *c, int i);
    void (*set_s)(FzCpu *c, int i, float v);

    // Guest memory: 1 when [addr, addr+n) is the guest's own memory with that access (write: 1 for writing). Guest
    // addresses are host addresses, so a plugin reads and writes them directly once checked.
    int (*guest_access)(uint64_t addr, uint64_t n, int write);

    // The guest address of a named function from the runtime's symbol file (TOTK_FUSE_SYMBOLS); 0 when unknown.
    uint64_t (*sym)(const char *name);
} FzApi;

typedef int (*FzPluginInit)(const FzApi *api);
