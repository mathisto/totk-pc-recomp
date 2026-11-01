// Per-mod script cost (fuse_luau_win.cpp), shared by the runtime and the menu's Mods tab (plain C, no cpu.h).
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char id[64];
    float avg_ms, max_ms;              // smoothed ms per frame; the most in one frame over the last ~2 s
    unsigned calls, hooks, handlers;   // calls in the last frame; hooks and event handlers registered
    int status;                        // FZ_MOD_OK, FZ_MOD_BUSY (over half its budget), FZ_MOD_THROTTLED (paused)
} FzModStat;
enum { FZ_MOD_OK, FZ_MOD_BUSY, FZ_MOD_THROTTLED };

// Each package's exclusive time in its hooks and handlers, most expensive first; the count. *budget_ms: the budget
// per mod per frame.
int fz_luau_stats(FzModStat *out, int max, float *budget_ms);

// The deployed script packages (a folder `fuse deploy` wrote), in load order, with whether each is on in game. A
// package turned off in game stays deployed but does not load; the choice is kept in <scripts>/.fuse-disabled.
typedef struct {
    char id[72];   // package ids are up to 64 characters
    int enabled;
} FzPackage;
int fz_luau_packages(FzPackage *out, int max);   // the count (out NULL: all of them)
// Turns a deployed package on or off and reloads the scripts (about a second): 0, or -1 when it is not deployed or the
// choice could not be saved.
int fz_luau_set_enabled(const char *id, int on);

int fz_luau_reload(void);   // the scripts load again (fuse_luau_win.cpp)

// The fuse manager (fuse_manager_win.c): whether totk.ini says where it is, and running one of its commands.
int fz_manager_ready(void);
int fz_manager_run(const char *const *args, int nargs, char *out, size_t cap, int with_errors);

#ifdef __cplusplus
}
#endif
