/* Commentary: the addon starts the voice side (commentary\race_broadcast_commentary.ps1 in the addon's folder) by itself,
 * hidden, with Windows PowerShell. It speaks what commentary.c decides (say.txt) with Piper voices, and stops by itself
 * when the game closes (it watches the game's process id). Setting "Commentary": off / quiet / normal / chatty. */
typedef struct {
    DWORD cb; char *reserved, *desktop, *title; DWORD x, y, xs, ys, xc, yc, fill, flags; uint16_t show, r2; uint8_t *r3;
    HANDLE in, out, err;
} STARTUPINFOA_;
typedef struct { HANDLE process, thread; DWORD pid, tid; } PROCINFO_;
__declspec(dllimport) BOOL __stdcall CreateProcessA(const char *, char *, void *, void *, BOOL, DWORD, void *, const char *, STARTUPINFOA_ *, PROCINFO_ *);
__declspec(dllimport) DWORD __stdcall GetFileAttributesA(const char *);

static void commentary_start(void)
{
    const char *c = get_choice("commentary");
    int level = c && seq(c, "off") ? 0 : c && seq(c, "quiet") ? 1 : c && seq(c, "chatty") ? 3 : 2; /* normal unless chosen otherwise */
    cm_level = level;
    if (!level) { LOG("commentary: off in the settings"); return; }
    cm_write(""); /* a fresh say.txt before the voice side starts reading it */
    char base[260], script[400], cmd[900];
    DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", base, sizeof base);
    if (!n || n >= sizeof base) return;
    fmt(script, sizeof script, "%s\\Swofty\\addons\\apoc.race-broadcast\\commentary\\race_broadcast_commentary.ps1", base);
    if (GetFileAttributesA(script) == 0xFFFFFFFFu) { LOG("commentary: %s not found", script); return; }
    const char *vc = get_choice("commentary_voices");
    int piper = vc && seq(vc, "piper");
    fmt(cmd, sizeof cmd, "powershell.exe -NoProfile -STA -ExecutionPolicy Bypass -WindowStyle Hidden -File \"%s\" -GamePid %u -Chatty %d -Voices %s", script,
        (unsigned)GetCurrentProcessId(), level, piper ? "piper" : "recorded");
    STARTUPINFOA_ si;
    PROCINFO_ pi;
    memset(&si, 0, sizeof si);
    memset(&pi, 0, sizeof pi);
    si.cb = sizeof si;
    si.flags = 1; /* STARTF_USESHOWWINDOW */
    si.show = 0;  /* SW_HIDE */
    if (CreateProcessA(0, cmd, 0, 0, 0, 0x08000000 /* CREATE_NO_WINDOW */, 0, 0, &si, &pi)) {
        LOG("commentary: started (pid %u, level %d)", (unsigned)pi.pid, level);
        CloseHandle(pi.thread);
        CloseHandle(pi.process);
    } else {
        LOG("commentary: could not start PowerShell");
    }
}
