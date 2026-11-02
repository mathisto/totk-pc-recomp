// Fuse runtime hooks (spike): entry and return hooks on any lifted function, attached while the game runs.
//
// Every lifted function starts with FN_ENTER(addr, k) (cpu.h). While no hook is attached anywhere that is one
// predictable branch per call, as it always was. Once one is, FN_ENTER also tests the function's bit in its module's
// hook bitmap (k: the function's index in the module's function table, a compile-time constant) and, at a hooked
// function, calls fz_enter, which runs the hooks around the original body and returns in its place.
//
// - before(c, user): the arguments are in c->x / c->v as the caller left them and may be changed. Returning nonzero
//   skips the original body and the later before hooks (set the return value in c->x[0] / c->v[0] first).
// - after(c, user): runs once the body returned (or was skipped); the return value is in c->x[0] / c->v[0].
// - Hooks on one function run by priority, higher first; equal priorities in the order they were attached.
// - Guest functions a hook calls (rt_call) are hooked as usual; nesting stops at FZ_DEPTH, past which functions run
//   unhooked.
//
// Settings: TOTK_FUSE_TEST attaches the spike's test hooks (1: counts and logs; 2: also zero shield wear);
// TOTK_FUSE_GATE (default 1): 0 keeps the map check off, "ab" switches it with TOTK_AB's B blocks (measurement).
#pragma once
#include <stddef.h>

#include "cpu.h"
#include "fuse_stats.h"

typedef int (*fz_before_fn)(Cpu *c, void *user);
typedef void (*fz_after_fn)(Cpu *c, void *user);

void fz_init(void);                 // after rt_dispatch_init: the hook map, and the test hooks
void fz_present(void);              // every present: the gate's on/off state (TOTK_FUSE_GATE)
// Attaches hooks at the lifted function starting at guest address addr; either callback may be NULL. 0 on success,
// -1 when no lifted function starts there.
int fz_hook(uint64_t addr, fz_before_fn before, fz_after_fn after, void *user, int priority);
// Removes every hook attached with this user pointer; the number removed. The function's map byte stays set, so a
// later call still takes the hook path once and finds nothing (cheap); the gate turns off when no hook is left.
int fz_unhook(void *user);
// The crash reporter's line about Fuse: whether the crashing thread was inside a hook callback (and whose: the module
// and offset of the callback, which names a native plugin's DLL) or inside a hooked function's body.
void fz_crash_note(void);
// The module (DLL or exe) and offset of a host code address, for crash reports (fuse_plugins_win.c).
const char *fz_owner_name(const void *fn, char *buf, size_t cap);
// For the Mods tab: hooks attached now; the loaded scripts and plugins, one name per line (the count is returned).
int fz_hook_count(void);
#ifdef _WIN32
int fz_luau_list(char *buf, size_t cap);
int fz_plugins_list(char *buf, size_t cap);
#else
static inline int fz_luau_list(char *buf, size_t cap) { if (cap) *buf = 0; return 0; }
static inline int fz_plugins_list(char *buf, size_t cap) { if (cap) *buf = 0; return 0; }
#endif
// A folder the fuse CLI deployed into: calls cb(id, path, sha256, user) for each file of its .fuse-deploy.json whose
// path ends with ext, in the manifest's (resolved load) order, after checking the id and path stay inside the folder.
// The number of files, or -1 when the folder has no manifest.
typedef void (*fz_deployed_fn)(const char *id, const char *path, const char *sha256, void *user);
int fz_deployed_files(const char *dir, const char *ext, fz_deployed_fn cb, void *user);
// The packages' option values in the same manifest: cb(id, "key=t:value") for each (t: b, n or s), in order. -1 when
// there is no manifest.
typedef void (*fz_config_fn)(const char *id, const char *entry, void *user);
int fz_deployed_config(const char *dir, fz_config_fn cb, void *user);
// Each package's declared capabilities in the same manifest: cb(id, "") once for a package that has the list (its
// scripts are held to it), then cb(id, capability) for each. The number of packages with a list; -1 without a manifest.
int fz_deployed_caps(const char *dir, fz_config_fn cb, void *user);
// 1 when the SHA-256 of [data, data+n) is the lower-case hex digest want.
int fz_file_sha256_hex(const void *data, size_t n, const char *want);
// ExeFS patches (fuse_patches.c): the deployed .fzpatch files of TOTK_FUSE_SCRIPTS, written into the modules they name
// by build id. The loader calls it once, after loading the images and before relocating them.
typedef struct { const char *name; uint64_t base, text_end, data_end; char bid[65]; } FzPatchModule;   // offsets from base
void fz_patches_apply(const FzPatchModule *mods, int n);
// A layout field from the same file: 1 and its offset and kind ("u32", "f32", "ptr", "*Type" or a type name) when
// type has field (field NULL: when type has any field).
int fz_layout(const char *type, const char *field, uint64_t *offset, const char **kind);
// The guest address of a named function from TOTK_FUSE_SYMBOLS (main base + its offset); 0 when unknown.
uint64_t fz_sym(const char *name);

// Luau scripts (fuse_luau_win.cpp, Windows only for now): TOTK_FUSE_SCRIPTS=<folder of *.luau>, loaded at start; reload
// (unhook all script hooks, fresh VM, load again; the number of hooks) and eval (a chunk in the live VM: 0 and its
// printed lines and results in out, else the error).
#ifdef _WIN32
void fz_plugins_init(void);   // fuse_plugins_win.c: native plugins (TOTK_FUSE_PLUGINS, only with TOTK_FUSE_NATIVE=1)
void fz_luau_init(void);
void fz_luau_frame(uint64_t frame);   // every present: the scripts' "frame" handlers
int fz_luau_reload(void);
int fz_luau_eval(const char *code, char *out, size_t cap);
#else
static inline void fz_plugins_init(void) {}
static inline void fz_luau_init(void) {}
static inline void fz_luau_frame(uint64_t frame) { (void)frame; }
static inline int fz_luau_reload(void) { return -1; }
static inline int fz_luau_eval(const char *code, char *out, size_t cap) { (void)code, (void)out, (void)cap; return -1; }
#endif
