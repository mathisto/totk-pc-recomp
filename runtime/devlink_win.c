// DevLink server (see devlink.h).
//
// - One thread accepts loopback connections; the first request on a connection must be {"method":"auth","params":
//   {"token":...}} with the token from <profile>/devlink.json ({host, port, token, pid, version}, rewritten each launch).
// - Requests run on the DevLink thread. Guest memory reads and writes are checked with VirtualQuery first; the game
//   keeps running meanwhile, so a read is a snapshot. A screenshot waits for the next present.
// - Methods: auth, ping, log, cfg_get, cfg_set, patch, read, write, name, hook, hooks, shot, input, lua, script_reload,
//   quit.
// The "write" and "hook" methods change the running game: DevLink is for development, keep it off otherwise.
#define _CRT_RAND_S
#include "devlink.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "dispatch.h"
#include "fuse_hooks.h"
#include "gfx.h"
#include "host.h"
#include "input.h"
#include "platform/platform.h"
#include "ui/menu.h"

#define DL_VERSION "0.1"
#define DL_LOG_RING (1u << 20)   // bytes of log kept for the "log" method
#define DL_LINE_MAX (1u << 16)   // longest request line
#define DL_HOOKS 64

static char token[33];
static SOCKET listener = INVALID_SOCKET;

// ---- the log ring: byte offsets count from the start of the run, like a file that never truncates

static char *ring;
static uint64_t ring_total;
static CRITICAL_SECTION ring_lock;
static volatile LONG ring_on;

void devlink_log_tap(const char *p, size_t n) {
    if (!ring_on) return;
    EnterCriticalSection(&ring_lock);
    for (size_t i = 0; i < n; i++) ring[(ring_total + i) % DL_LOG_RING] = p[i];
    ring_total += n;
    LeaveCriticalSection(&ring_lock);
}

// ---- a growable reply buffer and JSON helpers

typedef struct { char *p; size_t n, cap; } Buf;

static void put(Buf *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 4096;
        while (cap < b->n + n + 1) cap *= 2;
        char *q = realloc(b->p, cap);
        if (!q) return;
        b->p = q, b->cap = cap;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}

static void putf(Buf *b, const char *fmt, ...) {
    char tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n > 0) put(b, tmp, (size_t)n < sizeof tmp ? (size_t)n : sizeof tmp - 1);
}

static void put_str(Buf *b, const char *s, size_t n) {   // a JSON string literal
    put(b, "\"", 1);
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') { char e[2] = {'\\', (char)c}; put(b, e, 2); }
        else if (c == '\n') put(b, "\\n", 2);
        else if (c == '\r') put(b, "\\r", 2);
        else if (c == '\t') put(b, "\\t", 2);
        else if (c < 0x20) putf(b, "\\u%04x", c);
        else put(b, (const char *)&c, 1);
    }
    put(b, "\"", 1);
}

// The value of "key" anywhere in the request line (keys are unique in DevLink requests): a string (unescaped into out)
// or a bare token (number, true, false, null). 1 when found.
static int json_get(const char *line, const char *key, char *out, size_t cap) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(line, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t') p++;
    if (*p++ != ':') return 0;
    while (*p == ' ' || *p == '\t') p++;
    size_t n = 0;
    if (*p == '"') {
        for (p++; *p && *p != '"' && n + 1 < cap; p++) {
            char c = *p;
            if (c == '\\' && p[1]) {
                c = *++p;
                c = c == 'n' ? '\n' : c == 't' ? '\t' : c == 'r' ? '\r' : c;
            }
            out[n++] = c;
        }
    } else {
        while (*p && *p != ',' && *p != '}' && *p != ' ' && n + 1 < cap) out[n++] = *p++;
    }
    out[n] = 0;
    return 1;
}

static uint64_t json_u64(const char *line, const char *key, uint64_t def) {
    char v[64];
    return json_get(line, key, v, sizeof v) ? strtoull(v, NULL, 0) : def;
}

// ---- guest memory, checked

static int readable(uint64_t a, size_t n, int write) {
    while (n) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery((void *)(uintptr_t)a, &mi, sizeof mi) || mi.State != MEM_COMMIT) return 0;
        DWORD ok = write ? (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)
                         : (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                            PAGE_EXECUTE_WRITECOPY);
        if (!(mi.Protect & ok) || (mi.Protect & PAGE_GUARD)) return 0;
        uint64_t end = (uint64_t)(uintptr_t)mi.BaseAddress + mi.RegionSize;
        size_t k = end - a < n ? (size_t)(end - a) : n;
        a += k, n -= k;
    }
    return 1;
}

// ---- built-in hooks for exploring: count calls, or also log the first arguments (rate-limited)

typedef struct { uint64_t addr; int log; volatile LONG64 calls; } DlHook;
static DlHook hooks[DL_HOOKS];
static volatile LONG nhooks;

static int hook_before(Cpu *c, void *user) {
    DlHook *h = user;
    LONG64 n = InterlockedIncrement64(&h->calls);
    if (h->log && (n <= 20 || n % 1000 == 0)) {
        char at[64];
        rt_log("devlink: %s call %lld: x0 %#llx x1 %#llx x2 %#llx x3 %#llx lr %#llx", rt_guest_name(h->addr, at, sizeof at),
               (long long)n, (unsigned long long)c->x[0], (unsigned long long)c->x[1], (unsigned long long)c->x[2],
               (unsigned long long)c->x[3], (unsigned long long)c->x[30]);
    }
    return 0;
}

// ---- methods

static int method(const char *line, const char *name, Buf *r) {
    char v[512], at[64];
    if (!strcmp(name, "ping")) {
        putf(r, "{\"version\":\"" DL_VERSION "\",\"pid\":%lu,\"world_s\":%.3f,\"ticks\":%llu,\"hooks\":%ld}",
             (unsigned long)GetCurrentProcessId(), frame_step_world_us() / 1e6, (unsigned long long)frame_step_ticks(),
             (long)nhooks);
    } else if (!strcmp(name, "log")) {   // {since}: text logged since that byte offset, and the offset to pass next
        uint64_t since = json_u64(line, "since", 0);
        EnterCriticalSection(&ring_lock);
        uint64_t total = ring_total, lo = total > DL_LOG_RING ? total - DL_LOG_RING : 0;
        if (since < lo) since = lo;
        if (since > total) since = total;
        size_t n = (size_t)(total - since);
        if (n > (1u << 18)) since = total - (n = 1u << 18);   // at most 256 KiB a call
        char *tmp = malloc(n + 1);
        for (size_t i = 0; tmp && i < n; i++) tmp[i] = ring[(since + i) % DL_LOG_RING];
        LeaveCriticalSection(&ring_lock);
        putf(r, "{\"next\":%llu,\"from\":%llu,\"text\":", (unsigned long long)total, (unsigned long long)since);
        put_str(r, tmp ? tmp : "", tmp ? n : 0);
        put(r, "}", 1);
        free(tmp);
    } else if (!strcmp(name, "cfg_get")) {   // {key}: TOTK_ name, or the ini key
        if (!json_get(line, "key", v, sizeof v)) return -32602;
        char key[128];
        snprintf(key, sizeof key, "%s%s", _strnicmp(v, "TOTK_", 5) ? "TOTK_" : "", v);
        for (char *k = key; *k; k++) *k = (char)toupper((unsigned char)*k);
        const char *s = cfg_get(key);
        put(r, "{\"value\":", 9);
        if (s) put_str(r, s, strlen(s)); else put(r, "null", 4);
        put(r, "}", 1);
    } else if (!strcmp(name, "cfg_set")) {   // {key, value}: a live setting, as the menu sets it (not saved)
        char val[512], key[128];
        if (!json_get(line, "key", v, sizeof v) || !json_get(line, "value", val, sizeof val)) return -32602;
        snprintf(key, sizeof key, "%s%s", _strnicmp(v, "TOTK_", 5) ? "TOTK_" : "", v);
        for (char *k = key; *k; k++) *k = (char)toupper((unsigned char)*k);
        cfg_set(key, val);
        put(r, "{\"ok\":true}", 11);
    } else if (!strcmp(name, "patch")) {   // {set, on}: switch a patch set (deflect, lockon, tsq, ...) live
        if (!json_get(line, "set", v, sizeof v)) return -32602;
        int set = rt_patch_set(v);
        if (set < 0) return -32602;
        rt_patch_switch(set, (int)json_u64(line, "on", 1));
        putf(r, "{\"set\":%d,\"on\":%d}", set, rt_patch_enabled(set));
    } else if (!strcmp(name, "read")) {   // {addr, n}: hex bytes
        uint64_t a = json_u64(line, "addr", 0), n = json_u64(line, "n", 16);
        if (!a || n == 0 || n > 65536) return -32602;
        if (!readable(a, (size_t)n, 0)) return -32001;
        put(r, "{\"hex\":\"", 8);
        for (uint64_t i = 0; i < n; i++) putf(r, "%02x", ((const uint8_t *)(uintptr_t)a)[i]);
        put(r, "\"}", 2);
    } else if (!strcmp(name, "write")) {   // {addr, hex}
        static char hex[2 * 4096 + 1];
        uint64_t a = json_u64(line, "addr", 0);
        if (!a || !json_get(line, "hex", hex, sizeof hex) || strlen(hex) % 2) return -32602;
        size_t n = strlen(hex) / 2;
        if (!readable(a, n, 1)) return -32001;
        for (size_t i = 0; i < n; i++) {
            unsigned x;
            sscanf(hex + 2 * i, "%2x", &x);
            ((uint8_t *)(uintptr_t)a)[i] = (uint8_t)x;
        }
        putf(r, "{\"written\":%zu}", n);
    } else if (!strcmp(name, "name")) {   // {addr}: module+offset
        uint64_t a = json_u64(line, "addr", 0);
        put(r, "{\"name\":", 8);
        const char *s = rt_guest_name(a, at, sizeof at);
        put_str(r, s, strlen(s));
        putf(r, ",\"function\":%s}", rt_dispatch_lookup(a) ? "true" : "false");
    } else if (!strcmp(name, "hook")) {   // {addr, log}: count calls of a function (and log arguments)
        uint64_t a = json_u64(line, "addr", 0);
        LONG i = nhooks;
        if (i >= DL_HOOKS) return -32002;
        hooks[i] = (DlHook){a, (int)json_u64(line, "log", 0), 0};
        if (fz_hook(a, hook_before, NULL, &hooks[i], 0)) return -32003;
        InterlockedIncrement(&nhooks);
        putf(r, "{\"id\":%ld,\"name\":", (long)i);
        const char *s = rt_guest_name(a, at, sizeof at);
        put_str(r, s, strlen(s));
        put(r, "}", 1);
    } else if (!strcmp(name, "hooks")) {
        put(r, "{\"hooks\":[", 10);
        for (LONG i = 0; i < nhooks; i++) {
            if (i) put(r, ",", 1);
            putf(r, "{\"id\":%ld,\"addr\":\"%#llx\",\"calls\":%lld,\"name\":", (long)i, (unsigned long long)hooks[i].addr,
                 (long long)hooks[i].calls);
            const char *s = rt_guest_name(hooks[i].addr, at, sizeof at);
            put_str(r, s, strlen(s));
            put(r, "}", 1);
        }
        put(r, "]}", 2);
    } else if (!strcmp(name, "shot")) {   // {path}: PNG of the next presented frame (waits up to 5 s)
        if (!json_get(line, "path", v, sizeof v)) return -32602;
        volatile int done = 0;
        rt_shot_request(v, &done);
        for (int i = 0; i < 500 && !done; i++) Sleep(10);
        if (!done) rt_shot_request("", NULL);
        if (done != 1) return done < 0 ? -32004 : -32005;
        put(r, "{\"path\":", 8);
        put_str(r, v, strlen(v));
        put(r, "}", 1);
    } else if (!strcmp(name, "input")) {   // {button, frames} or {stick: "L"|"R", x, y, frames}
        uint64_t frames = json_u64(line, "frames", 6);
        int ok;
        if (json_get(line, "stick", v, sizeof v)) {
            char x[32] = "0", y[32] = "0";
            json_get(line, "x", x, sizeof x), json_get(line, "y", y, sizeof y);
            ok = input_inject(-1, v[0] == 'R' || v[0] == 'r' ? 2 : 1, (int32_t)atol(x), (int32_t)atol(y), frames);
        } else {
            if (!json_get(line, "button", v, sizeof v)) return -32602;
            for (char *k = v; *k; k++) *k = (char)toupper((unsigned char)*k);
            int bit = input_button_bit(v);
            if (bit < 0) return -32602;
            ok = input_inject(bit, 0, 0, 0, frames);
        }
        if (!ok) return -32006;
        put(r, "{\"ok\":true}", 11);
    } else if (!strcmp(name, "lua")) {   // {code}: run Luau in the live script VM (the console)
        static char code[DL_LINE_MAX], out[1 << 16];
        if (!json_get(line, "code", code, sizeof code)) return -32602;
        int rc = fz_luau_eval(code, out, sizeof out);
        putf(r, "{\"ok\":%s,\"output\":", rc ? "false" : "true");
        put_str(r, out, strlen(out));
        put(r, "}", 1);
    } else if (!strcmp(name, "script_reload")) {   // unhook every script hook and load TOTK_FUSE_SCRIPTS again
        int n = fz_luau_reload();
        if (n < 0) return -32007;
        putf(r, "{\"hooks\":%d}", n);
    } else if (!strcmp(name, "quit")) {
        gfx_window_request_close();
        put(r, "{\"ok\":true}", 11);
    } else {
        return -32601;
    }
    return 0;
}

static const char *error_text(int code) {
    switch (code) {
    case -32601: return "method not found";
    case -32602: return "invalid params";
    case -32001: return "address not readable/writable";
    case -32002: return "hook table full";
    case -32003: return "no lifted function starts at that address";
    case -32004: return "the presented frame is not 8-bit RGBA";
    case -32005: return "no frame presented within 5 s";
    case -32006: return "too many pending inputs";
    case -32007: return "scripts not initialised";
    case -32010: return "auth required";
    default: return "error";
    }
}

static void reply(SOCKET s, const char *line, Buf *r, int code) {
    char id[64] = "null";
    json_get(line, "id", id, sizeof id);
    Buf out = {0};
    int str = strstr(line, "\"id\":\"") || strstr(line, "\"id\": \"");
    put(&out, "{\"jsonrpc\":\"2.0\",\"id\":", 22);
    if (str) put_str(&out, id, strlen(id)); else put(&out, id, strlen(id));
    if (code) putf(&out, ",\"error\":{\"code\":%d,\"message\":\"%s\"}}\n", code, error_text(code));
    else { put(&out, ",\"result\":", 10); put(&out, r->p ? r->p : "null", r->p ? r->n : 4); put(&out, "}\n", 2); }
    for (size_t sent = 0; sent < out.n;) {
        int k = send(s, out.p + sent, (int)(out.n - sent), 0);
        if (k <= 0) break;
        sent += (size_t)k;
    }
    free(out.p);
}

static void serve(SOCKET s) {
    char *line = malloc(DL_LINE_MAX);
    size_t n = 0;
    int authed = 0;
    for (;;) {
        int k = recv(s, line + n, (int)(DL_LINE_MAX - 1 - n), 0);
        if (k <= 0) break;
        n += (size_t)k;
        line[n] = 0;
        char *nl;
        while ((nl = memchr(line, '\n', n))) {
            *nl = 0;
            char name[64] = "";
            json_get(line, "method", name, sizeof name);
            Buf r = {0};
            int code = 0;
            if (!strcmp(name, "auth")) {
                char t[64] = "";
                json_get(line, "token", t, sizeof t);
                authed = !strcmp(t, token);
                if (authed) put(&r, "{\"ok\":true}", 11); else code = -32010;
            } else if (!authed) {
                code = -32010;
            } else {
                code = method(line, name, &r);
            }
            reply(s, line, &r, code);
            free(r.p);
            size_t used = (size_t)(nl + 1 - line);
            memmove(line, nl + 1, n - used);
            n -= used;
            line[n] = 0;
        }
        if (n == DL_LINE_MAX - 1) break;   // a line too long
    }
    free(line);
    closesocket(s);
}

static DWORD WINAPI accept_loop(void *arg) {
    (void)arg;
    for (;;) {
        SOCKET s = accept(listener, NULL, NULL);
        if (s == INVALID_SOCKET) { Sleep(100); continue; }
        serve(s);   // one client at a time: tools open a connection per call
    }
    return 0;
}

void devlink_init(void) {
    const char *on = cfg_get("TOTK_DEVLINK");
    if (!on || !*on || !strcmp(on, "0")) return;
    ring = calloc(1, DL_LOG_RING);
    if (!ring) return;
    InitializeCriticalSection(&ring_lock);
    ring_on = 1;
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa)) { rt_log("devlink: WSAStartup failed"); return; }
    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK), .sin_port = 0};
    int alen = sizeof a;
    if (listener == INVALID_SOCKET || bind(listener, (struct sockaddr *)&a, sizeof a) || listen(listener, 4) ||
        getsockname(listener, (struct sockaddr *)&a, &alen)) {
        rt_log("devlink: cannot listen on loopback (%d)", WSAGetLastError());
        return;
    }
    for (int i = 0; i < 4; i++) {
        unsigned int x = 0;
        rand_s(&x);
        snprintf(token + 8 * i, 9, "%08x", x);
    }
    char path[PLAT_PATH_MAX];
    const RtConfig *cfg = rt_config();
    snprintf(path, sizeof path, "%s/devlink.json", cfg && cfg->profile ? cfg->profile : ".");
    FILE *f = fopen(path, "w");
    if (!f) { rt_log("devlink: cannot write %s", path); return; }
    fprintf(f, "{\"host\":\"127.0.0.1\",\"port\":%u,\"token\":\"%s\",\"pid\":%lu,\"version\":\"" DL_VERSION "\"}\n",
            (unsigned)ntohs(a.sin_port), token, (unsigned long)GetCurrentProcessId());
    fclose(f);
    CreateThread(NULL, 0, accept_loop, NULL, 0, NULL);
    rt_log("devlink: listening on 127.0.0.1:%u (handshake %s)", (unsigned)ntohs(a.sin_port), path);
}
