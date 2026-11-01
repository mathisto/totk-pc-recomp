// The fuse manager, run from the game for the Mods tab's install card: a mod package dropped on the window is shown
// with what `fuse card` says about it, and installed with `fuse install` and `fuse apply --again --no-merge`. The game
// itself never unpacks, verifies or downloads anything: the manager does, as a separate process. Where the manager is
// comes from totk.ini, written by `fuse apply`: fuse_manager (its Python), fuse_manager_dir (the CLI's folder),
// fuse_profile (the profile it manages).
#include "fuse_stats.h"

#include <stdio.h>
#include <string.h>
#include <windows.h>

#include "cfg.h"
#include "host.h"

int fz_manager_ready(void) {
    const char *py = cfg_get("TOTK_FUSE_MANAGER"), *dir = cfg_get("TOTK_FUSE_MANAGER_DIR");
    return py && *py && dir && *dir;
}

// Appends arg to cmd as one argument, quoted by the rules the C runtime splits command lines with (backslashes are
// literal except before a quote, so a run of them before a quote, or before the closing quote, is doubled). 0 when it
// does not fit or holds a quote (no path does).
static int add_arg(char *cmd, size_t cap, size_t *n, const char *arg) {
    if (strchr(arg, '"')) return 0;
    size_t len = strlen(arg), trailing = 0;
    while (trailing < len && arg[len - 1 - trailing] == '\\') trailing++;
    if (*n + len + trailing + 4 >= cap) return 0;
    cmd[(*n)++] = ' ';
    cmd[(*n)++] = '"';
    memcpy(cmd + *n, arg, len);
    *n += len;
    for (size_t i = 0; i < trailing; i++) cmd[(*n)++] = '\\';
    cmd[(*n)++] = '"';
    cmd[*n] = 0;
    return 1;
}

// Runs `<python> -m fuse --profile <profile> <args...>` in the manager's folder, with no window, and collects its
// standard output (and its errors too when with_errors) into out. The child gets only the pipe and NUL, none of the
// game's other handles (DevLink's sockets, files). The exit code, or -1 when it could not run.
int fz_manager_run(const char *const *args, int nargs, char *out, size_t cap, int with_errors) {
    if (cap) *out = 0;
    const char *py = cfg_get("TOTK_FUSE_MANAGER"), *dir = cfg_get("TOTK_FUSE_MANAGER_DIR");
    const char *profile = cfg_get("TOTK_FUSE_PROFILE");
    if (!py || !*py || !dir || !*dir) return -1;
    char cmd[8192];
    size_t n = 0;
    cmd[0] = 0;
    if (!add_arg(cmd, sizeof cmd, &n, py)) return -1;
    memmove(cmd, cmd + 1, n);   // no leading space before the program
    n--;
    static const char *const fixed[] = {"-m", "fuse"};
    for (int i = 0; i < 2; i++)
        if (!add_arg(cmd, sizeof cmd, &n, fixed[i])) return -1;
    if (profile && *profile && (!add_arg(cmd, sizeof cmd, &n, "--profile") || !add_arg(cmd, sizeof cmd, &n, profile)))
        return -1;
    for (int i = 0; i < nargs; i++)
        if (!add_arg(cmd, sizeof cmd, &n, args[i])) return -1;
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE rd, wr, nul = CreateFileA("NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                                     OPEN_EXISTING, 0, NULL);
    if (nul == INVALID_HANDLE_VALUE) return -1;
    if (!CreatePipe(&rd, &wr, &sa, 0)) {
        CloseHandle(nul);
        return -1;
    }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    // Exactly these two handles are inherited (PROC_THREAD_ATTRIBUTE_HANDLE_LIST).
    HANDLE inherit[2] = {wr, nul};
    SIZE_T asize = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &asize);
    LPPROC_THREAD_ATTRIBUTE_LIST attrs = (LPPROC_THREAD_ATTRIBUTE_LIST)HeapAlloc(GetProcessHeap(), 0, asize);
    BOOL ok = attrs && InitializeProcThreadAttributeList(attrs, 1, 0, &asize) &&
              UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof inherit, NULL, NULL);
    STARTUPINFOEXA si = {.StartupInfo = {.cb = sizeof si, .dwFlags = STARTF_USESTDHANDLES, .hStdInput = nul,
                                         .hStdOutput = wr, .hStdError = with_errors ? wr : nul},
                         .lpAttributeList = attrs};
    PROCESS_INFORMATION pi;
    if (ok)
        ok = CreateProcessA(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, NULL, dir,
                            &si.StartupInfo, &pi);
    DWORD err = GetLastError();
    if (attrs) {
        DeleteProcThreadAttributeList(attrs);
        HeapFree(GetProcessHeap(), 0, attrs);
    }
    CloseHandle(wr);
    CloseHandle(nul);
    if (!ok) {
        CloseHandle(rd);
        rt_log("fuse: cannot run the manager (%lu): %s", err, cmd);
        return -1;
    }
    size_t used = 0;
    char buf[4096];
    DWORD got;
    while (ReadFile(rd, buf, sizeof buf, &got, NULL) && got) {
        size_t k = used + got < cap ? got : (cap > used + 1 ? cap - used - 1 : 0);
        memcpy(out + used, buf, k);
        used += k;
    }
    if (cap) out[used < cap ? used : cap - 1] = 0;
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, 10 * 60 * 1000);
    DWORD code = (DWORD)-1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}
