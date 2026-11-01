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

// Luau scripts (fuse_luau_win.cpp, Windows only for now): TOTK_FUSE_SCRIPTS=<folder of *.luau>, loaded at start; reload
// (unhook all script hooks, fresh VM, load again; the number of hooks) and eval (a chunk in the live VM: 0 and its
// printed lines and results in out, else the error).
#ifdef _WIN32
void fz_luau_init(void);
int fz_luau_reload(void);
int fz_luau_eval(const char *code, char *out, size_t cap);
#else
static inline void fz_luau_init(void) {}
static inline int fz_luau_reload(void) { return -1; }
static inline int fz_luau_eval(const char *code, char *out, size_t cap) { (void)code, (void)out, (void)cap; return -1; }
#endif
