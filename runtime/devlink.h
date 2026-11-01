// DevLink: a live channel into the running game for development tools and agents (devlink_win.c). Newline-delimited
// JSON-RPC 2.0 over loopback TCP, a per-launch token in <profile>/devlink.json, off unless TOTK_DEVLINK=1. Modelled on
// the author's DevLink for Fallout: New Vegas, so the same client habits work. Windows only for now.
#pragma once
#include <stddef.h>

#ifdef _WIN32
void devlink_init(void);                         // hostfn_seal, after the dispatch table and the hook map
void devlink_log_tap(const char *p, size_t n);   // rt_log_raw: every log byte, kept in a ring for the "log" method
#else
static inline void devlink_init(void) {}
static inline void devlink_log_tap(const char *p, size_t n) { (void)p, (void)n; }
#endif
