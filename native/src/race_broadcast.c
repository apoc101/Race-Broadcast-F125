/* SPDX-License-Identifier: MIT
 * Race Broadcast (apoc.race-broadcast) for F1 25 + Swofty Launcher.
 *
 * One addon for watching AI races like a TV broadcast:
 *   - AI drive: the game's AI drives your car (key or from the start) - same official call AI Drive uses
 *     (session ops set_control), written from scratch.
 *   - Keep the race running after your car is wrecked, then watch the rest (ported from Spectator/Broadcast
 *     Mode by Kizi & RK, MIT: the terminal-damage message filter and garage parking).
 *   - Auto director: scores every car several times a second (contact, spins, cars stopped, retirements,
 *     overtakes, close battles, podium places) and moves the game's spectator camera to the best story,
 *     with hold times so the camera does not flicker.
 *   - Broadcast graphics: lower-third captions saying why the camera is there, a timing tower, AI / LIVE tags.
 *   - Event feed: every director story is appended to a text file the commentary companion reads.
 *
 * Plain C, no C runtime: everything goes through the Swofty API tables and kernel32/user32.
 */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <swofty_api.h>

#ifndef ADDON_VERSION
#define ADDON_VERSION "0.5.1 beta"
#endif
#define TAG "[race-broadcast] "

/* ---- Win32 (declared by hand, no SDK headers in the build) ---------------------------------------------- */
typedef void *HANDLE;
typedef void *HMODULE;
typedef void *HWND;
typedef int BOOL;
typedef unsigned long DWORD;
__declspec(dllimport) HMODULE __stdcall GetModuleHandleW(const wchar_t *);
__declspec(dllimport) void *__stdcall GetProcAddress(HMODULE, const char *);
__declspec(dllimport) BOOL __stdcall QueryPerformanceCounter(int64_t *);
__declspec(dllimport) BOOL __stdcall QueryPerformanceFrequency(int64_t *);
__declspec(dllimport) DWORD __stdcall GetCurrentProcessId(void);
__declspec(dllimport) HANDLE __stdcall GetCurrentProcess(void);
__declspec(dllimport) BOOL __stdcall ReadProcessMemory(HANDLE, const void *, void *, size_t, size_t *);
__declspec(dllimport) HANDLE __stdcall OpenEventA(DWORD, BOOL, const char *);
__declspec(dllimport) DWORD __stdcall WaitForSingleObject(HANDLE, DWORD);
__declspec(dllimport) BOOL __stdcall CloseHandle(HANDLE);
__declspec(dllimport) HANDLE __stdcall CreateFileA(const char *, DWORD, DWORD, void *, DWORD, DWORD, HANDLE);
__declspec(dllimport) BOOL __stdcall WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
__declspec(dllimport) DWORD __stdcall GetEnvironmentVariableA(const char *, char *, DWORD);
__declspec(dllimport) BOOL __stdcall CreateDirectoryA(const char *, void *);
__declspec(dllimport) HANDLE __stdcall CreateThread(void *, size_t, DWORD(__stdcall *)(void *), void *, DWORD, DWORD *);
__declspec(dllimport) void __stdcall Sleep(DWORD);
__declspec(dllimport) DWORD __stdcall GetLastError(void);
__declspec(dllimport) short __stdcall GetAsyncKeyState(int);
__declspec(dllimport) void *__stdcall VirtualAlloc(void *, size_t, DWORD, DWORD);
__declspec(dllimport) BOOL __stdcall VirtualProtect(void *, size_t, DWORD, DWORD *);
__declspec(dllimport) BOOL __stdcall FlushInstructionCache(HANDLE, const void *, size_t);
__declspec(dllimport) HWND __stdcall GetForegroundWindow(void);
__declspec(dllimport) HWND __stdcall FindWindowW(const uint16_t *, const uint16_t *);
__declspec(dllimport) DWORD __stdcall GetWindowThreadProcessId(HWND, DWORD *);
__declspec(dllimport) unsigned __stdcall MapVirtualKeyW(unsigned, unsigned);
typedef struct {
    DWORD type;
    DWORD pad0;
    uint16_t wVk, wScan;
    DWORD dwFlags, time;
    uintptr_t dwExtraInfo;
    uint8_t pad1[8];
} KBINPUT; /* INPUT with the KEYBDINPUT member, 40 bytes on x64 */
__declspec(dllimport) unsigned __stdcall SendInput(unsigned, KBINPUT *, int);
#define SYNCHRONIZE 0x00100000UL
#define WAIT_OBJECT_0 0
#define INVALID_HANDLE ((HANDLE)(intptr_t)-1)
#define FILE_APPEND_DATA 0x0004
#define FILE_SHARE_READ 1
#define FILE_SHARE_WRITE 2
#define OPEN_ALWAYS 4
#define FILE_ATTRIBUTE_NORMAL 0x80
#define VK_F1 0x70

/* ---- no C runtime: what the compiler may still call ------------------------------------------------------- */
void *memset(void *d, int c, size_t n)
{
    volatile unsigned char *p = (volatile unsigned char *)d;
    while (n--) *p++ = (unsigned char)c;
    return d;
}
void *memcpy(void *d, const void *s, size_t n)
{
    volatile unsigned char *p = (volatile unsigned char *)d;
    const unsigned char *q = (const unsigned char *)s;
    while (n--) *p++ = *q++;
    return d;
}
int _fltused = 0;

static size_t slen(const char *s)
{
    size_t n = 0;
    while (s && s[n]) n++;
    return n;
}
static int seq(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *a == *b) a++, b++;
    return *a == *b;
}
static float sqrtf_(float x) { float r; if (x <= 0) return 0; __asm__("sqrtss %1, %0" : "=x"(r) : "x"(x)); return r; }

/* A tiny formatter: %s %d %u %c %% and %.1f / %.2f / %.3f (and %f = %.1f). */
static void fmt_v(char *out, size_t cap, const char *f, va_list ap)
{
    size_t o = 0;
#define PUT(ch) do { if (o + 1 < cap) out[o++] = (ch); } while (0)
    for (; *f; f++) {
        if (*f != '%') { PUT(*f); continue; }
        f++;
        int prec = 1;
        if (*f == '.' && f[1] >= '0' && f[1] <= '9') { prec = f[1] - '0'; f += 2; }
        char tmp[32];
        int n = 0;
        if (*f == 's') {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            while (*s) PUT(*s++);
        } else if (*f == 'd' || *f == 'u') {
            long long v = *f == 'd' ? (long long)va_arg(ap, int) : (long long)va_arg(ap, unsigned);
            if (v < 0) { PUT('-'); v = -v; }
            do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 30);
            while (n) PUT(tmp[--n]);
        } else if (*f == 'c') {
            PUT((char)va_arg(ap, int));
        } else if (*f == 'f') {
            double v = va_arg(ap, double);
            if (v < 0) { PUT('-'); v = -v; }
            long long scale = 1;
            for (int i = 0; i < prec; i++) scale *= 10;
            long long iv = (long long)(v * (double)scale + 0.5);
            long long ip = iv / scale, fp = iv % scale;
            do { tmp[n++] = (char)('0' + ip % 10); ip /= 10; } while (ip && n < 30);
            while (n) PUT(tmp[--n]);
            if (prec) {
                PUT('.');
                for (int i = prec - 1; i >= 0; i--) { tmp[i] = (char)('0' + fp % 10); fp /= 10; }
                for (int i = 0; i < prec; i++) PUT(tmp[i]);
            }
        } else if (*f == '%') {
            PUT('%');
        } else if (!*f) {
            break;
        }
    }
    if (cap) out[o < cap ? o : cap - 1] = 0;
#undef PUT
}
static void fmt(char *out, size_t cap, const char *f, ...)
{
    va_list ap;
    va_start(ap, f);
    fmt_v(out, cap, f, ap);
    va_end(ap);
}
static void scopy(char *d, size_t cap, const char *s)
{
    size_t i = 0;
    while (s && s[i] && i + 1 < cap) { d[i] = s[i]; i++; }
    if (cap) d[i] = 0;
}

/* ---- API tables -------------------------------------------------------------------------------------------- */
static const SwoftyApi *g_api;
static const SwoftySessionOps *g_ops;
static const SwoftyHud *g_hud;
#define LOG(...) g_api->log(TAG __VA_ARGS__)

__declspec(dllexport) const uint32_t SwoftyAddonApi = SWOFTY_API_VERSION;

static const SwoftySessionOps *ops(void)
{
    if (!g_ops) {
        HMODULE core = GetModuleHandleW(L"swofty_core.dll");
        SwoftyCoreSessionOpsFn get = core ? (SwoftyCoreSessionOpsFn)GetProcAddress(core, "SwoftyCoreSessionOps") : 0;
        g_ops = get ? get() : 0;
        if (g_ops && g_ops->version < 1) g_ops = 0;
    }
    return g_ops && g_ops->available && g_ops->available() ? g_ops : 0;
}
static const SwoftyHud *hud(void)
{
    if (!g_hud) {
        HMODULE core = GetModuleHandleW(L"swofty_core.dll");
        const SwoftyHud *(*get)(void) = core ? (const SwoftyHud *(*)(void))GetProcAddress(core, "SwoftyCoreHud") : 0;
        g_hud = get ? get() : 0;
    }
    return g_hud && g_hud->available && g_hud->available() ? g_hud : 0;
}
static const SwoftyOverlay *ovl(void) { return g_api && g_api->version >= 5 ? g_api->overlay : 0; }

static int mem_read(uintptr_t at, void *out, size_t n)
{
    size_t got = 0;
    return at && ReadProcessMemory(GetCurrentProcess(), (const void *)at, out, n, &got) && got == n;
}

static double now(void)
{
    static int64_t f;
    int64_t c;
    if (!f) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c / (double)f;
}

static void toast(const char *text, const char *detail, const char *icon)
{
    const SwoftyHud *h = hud();
    if (h && h->toast) h->toast(text, detail, 3.0f, icon);
}

static int g_udp_port = 20777;

/* ---- settings ------------------------------------------------------------------------------------------------ */
static struct {
    int ai_key, ai_from_start, handback;
    int watch_key, watch_with_ai, watch_after_crash, keep_race, garage_after;
    int director, director_key, hold, cam_mode, own_bias;
    int captions, tower, toasts, feed, camera_key, debug, replays;
    int aeternum;                                   /* hand the graphics to Project Aeternum when it is running */
    int aeternum_gfx;                               /* and let the director decide which of Aeternum's graphics show */
    int aeternum_layout;                            /* and where the car graphics sit */
} S = {0x74, 0, 3, 0x73, 1, 1, 1, 6, 1, 0x72, 9, 0, 1, 1, 1, 0, 0, 'C', 0, 1};

static int key_from(const char *v, int def)
{
    if (!v) return def;
    if (seq(v, "off") || seq(v, "none")) return 0;
    if (v[0] == 'F' && v[1] >= '1' && v[1] <= '9') {
        int n = v[1] - '0';
        if (v[2] >= '0' && v[2] <= '9') n = n * 10 + (v[2] - '0');
        if (n >= 1 && n <= 12) return VK_F1 + n - 1;
    }
    if (v[0] >= 'A' && v[0] <= 'Z' && !v[1]) return v[0];
    return def;
}
static int num_from(const char *v, int def)
{
    if (!v || !*v) return def;
    int n = 0, any = 0;
    for (; *v >= '0' && *v <= '9'; v++) n = n * 10 + (*v - '0'), any = 1;
    return any ? n : def;
}
static int get_toggle(const char *id, int def)
{
    SwoftySettingValue v;
    memset(&v, 0, sizeof v);
    return g_api->setting && g_api->setting(id, SWOFTY_SETTING_TOGGLE, &v) != 0 ? (v.toggle != 0) : def; /* non-zero: found */
}
static const char *get_choice(const char *id)
{
    SwoftySettingValue v;
    memset(&v, 0, sizeof v);
    return g_api->setting && g_api->setting(id, SWOFTY_SETTING_CHOICE, &v) != 0 ? v.choice : 0;
}
static int get_slider(const char *id, int def)
{
    SwoftySettingValue v;
    memset(&v, 0, sizeof v);
    return g_api->setting && g_api->setting(id, SWOFTY_SETTING_SLIDER, &v) != 0 ? (int)(v.slider + 0.5) : def;
}
/* Project Aeternum: while its Control Center window exists (shown or hidden), it draws the broadcast graphics. Our captions,
   tower and toasts stand down, the game's own spectator HUD is no longer held up, and the default telemetry port moves to
   20778, where Aeternum forwards the game's telemetry (it holds 20777 itself). */
static struct { int captions, tower, toasts, port; } W = {1, 0, 0, 20777};
static int g_aet;
static double g_aet_check;
static void aet_apply(void)
{
    S.captions = W.captions && !g_aet;
    S.tower = W.tower && !g_aet;
    S.toasts = W.toasts && !g_aet;
    g_udp_port = (g_aet && W.port == 20777) ? 20778 : W.port;
}
static void aet_tick(double t)
{
    if (t - g_aet_check < 3.0) return;
    g_aet_check = t;
    int on = S.aeternum && FindWindowW(0, (const uint16_t *)L"Project Aeternum Control Center") != 0;
    if (on == g_aet) return;
    g_aet = on;
    aet_apply();
    g_api->log(TAG "Project Aeternum %s: %s, telemetry on port %d", on ? "is running" : "is not running",
               on ? "it draws the graphics" : "our own graphics", g_udp_port);
}
static void read_settings(void)
{
    const char *c;
    S.ai_key = key_from(get_choice("ai_key"), 0x74);
    S.ai_from_start = get_toggle("ai_from_start", 0);
    S.handback = num_from(get_choice("handback"), 3);
    S.watch_key = key_from(get_choice("watch_key"), 0x73);
    S.watch_with_ai = get_toggle("watch_with_ai", 1);
    S.watch_after_crash = get_toggle("watch_after_crash", 1);
    S.keep_race = get_toggle("keep_race", 1);
    S.garage_after = num_from(get_choice("garage_after"), 6);
    S.director = get_toggle("director", 1);
    S.director_key = key_from(get_choice("director_key"), 0x72);
    S.hold = get_slider("hold", 12);
    if (S.hold < 4) S.hold = 4;
    c = get_choice("camera");
    S.cam_mode = seq(c, "tv_pod") ? 1 : seq(c, "mix") ? 2 : 0;
    c = get_choice("own_bias");
    S.own_bias = seq(c, "light") ? 15 : seq(c, "strong") ? 40 : 0;
    W.captions = get_toggle("captions", 1);
    W.tower = get_toggle("tower", 0);
    W.toasts = get_toggle("toasts", 0);
    S.aeternum = get_toggle("aeternum", 1);
    S.aeternum_gfx = get_toggle("aeternum_gfx", 1);
    S.aeternum_layout = get_toggle("aeternum_layout", 1);
    S.feed = get_toggle("feed", 0);
    S.camera_key = key_from(get_choice("camera_key"), 'C');
    S.debug = get_toggle("debug", 0);
    { const char *c = get_choice("replays"); S.replays = seq(c, "offer") ? 1 : seq(c, "auto") ? 2 : 0; } /* off unless chosen */
    W.port = num_from(get_choice("udp_port"), 20777);
    aet_apply();
}
static void on_setting(const char *id, const SwoftySettingValue *v, void *user)
{
    (void)v, (void)user;
    read_settings();
    LOG("setting %s changed", id);
}

/* ---- event feed for the commentary companion ------------------------------------------------------------- */
static HANDLE g_feed = 0;
static void feed_open(void)
{
    if (g_feed) return;
    char base[260], dir[300], path[340];
    DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", base, sizeof base);
    if (!n || n >= sizeof base) { g_feed = INVALID_HANDLE; return; }
    fmt(dir, sizeof dir, "%s\\Swofty\\addon-storage", base);
    CreateDirectoryA(dir, 0);
    fmt(dir, sizeof dir, "%s\\Swofty\\addon-storage\\apoc.race-broadcast", base);
    CreateDirectoryA(dir, 0);
    fmt(path, sizeof path, "%s\\events.txt", dir);
    g_feed = CreateFileA(path, 0x40000000u /* GENERIC_WRITE */, FILE_SHARE_READ | FILE_SHARE_WRITE | 4 /* delete */, 0, 2 /* CREATE_ALWAYS: a fresh feed each game */, FILE_ATTRIBUTE_NORMAL, 0);
    LOG("event feed %s: %s", g_feed != INVALID_HANDLE ? "open" : "NOT open", path);
}
/* one line per story, for the commentary app:
   time|kind|car|name|car2|name2|position|leader lap|total laps|text
   kinds: race_start, battle, overtake, contact, side, spin, stopped, out, pit, fastest_lap, safety_car, red_flag, restart,
   lap, final_lap, finish, replay_offer, replay_start, replay_end. Set fx_* just before a call to add the second car etc. */
static int fx_car2 = -1, fx_pos, g_lead_lap_fx, g_total_fx;
static const char *fx_name2 = "";
static void cm_feed(const char *kind, int car, int car2, int pos, const char *text);
static void cm_session_end(void);
static void feed(const char *kind, int car, const char *name, const char *text)
{
    cm_feed(kind, car, fx_car2, fx_pos, text);
    if (!S.feed) { fx_car2 = -1, fx_name2 = "", fx_pos = 0; return; }
    feed_open();
    if (g_feed == INVALID_HANDLE) return;
    char line[400];
    fmt(line, sizeof line, "%.2f|%s|%d|%s|%d|%s|%d|%d|%d|%s\r\n", now(), kind, car, name ? name : "", fx_car2, fx_name2 ? fx_name2 : "", fx_pos,
        g_lead_lap_fx, g_total_fx, text ? text : "");
    fx_car2 = -1, fx_name2 = "", fx_pos = 0;
    DWORD w = 0;
    WriteFile(g_feed, line, (DWORD)slen(line), &w, 0);
}
/* a long line (race state, drivers): kind and a payload, the other fields empty */
static void feed_long(const char *kind, const char *payload)
{
    cm_feed(kind, -1, -1, 0, payload);
    if (!S.feed) return;
    feed_open();
    if (g_feed == INVALID_HANDLE) return;
    static char line[3200];
    fmt(line, sizeof line, "%.2f|%s|-1||-1||0|%d|%d|%s\r\n", now(), kind, g_lead_lap_fx, g_total_fx, payload);
    DWORD w = 0;
    WriteFile(g_feed, line, (DWORD)slen(line), &w, 0);
}

/* ---- keyboard ------------------------------------------------------------------------------------------------ */
static int game_in_front(void)
{
    DWORD pid = 0;
    HWND w = GetForegroundWindow();
    if (!w) return 0;
    GetWindowThreadProcessId(w, &pid);
    return pid == GetCurrentProcessId();
}
static int pressed(int vk, int *was)
{
    if (!vk) return 0;
    int down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    int hit = down && !*was;
    *was = down;
    return hit && game_in_front();
}
static DWORD __stdcall key_thread(void *p)
{
    unsigned code = MapVirtualKeyW((unsigned)(uintptr_t)p, 0);
    KBINPUT in;
    memset(&in, 0, sizeof in);
    in.type = 1;
    in.wScan = (uint16_t)code;
    in.dwFlags = 0x0008; /* KEYEVENTF_SCANCODE */
    SendInput(1, &in, sizeof in);
    Sleep(60);
    in.dwFlags = 0x0008 | 0x0002;
    SendInput(1, &in, sizeof in);
    return 0;
}
static void press_key_once(int vk)
{
    if (vk && game_in_front()) {
        HANDLE t = CreateThread(0, 0, key_thread, (void *)(uintptr_t)vk, 0, 0);
        if (t) CloseHandle(t);
    }
}

/* ---- race state ---------------------------------------------------------------------------------------------- */
#define NC 24
#define F_PIT (1u << 0)
#define F_RETIRED (1u << 4)
#define F_TERMINAL (1u << 9)

typedef struct {
    int valid, racing, pos, prev_pos, lap, in_pit;
    float ld, speed, prev_speed, brake, total, prev_total, gap_ahead, prev_gap, closing;
    float x, y, z;
    double stopped_since, pos_t;
    float avg_spd; double slow_since; int slow_fed, stop_fed; /* a car losing pace (a failure on the way); stopped (once per stop) */
    float inc_score;
    double inc_until;
    char inc_text[96], inc_kind[16];
    float score;
    char why_t[48], why_d[96], why_kind[16];
    char name[48], abbr[4];
    uint32_t flags;
    int was_out, was_pit;
    int pit_raw, pit_comp_in, pit_stop_max, pit_lane_max, pit_from; /* pit stop being followed */
    double pit_out_at;
    double pit_left;   /* when the car last left the pit lane (a place change around then is the pit cycle) */
    double pit_still_t; int pit_leave_fed; /* stopped in the box; "heading for the exit" said */
    int dmg_prev;
    float screen; /* seconds on screen lately, decays */
    int ahead_id; /* the car this one is battling (ahead), -1 none */
    int inc_mate; /* the other car in this car's story (side by side, contact, overtake), -1 none */
    double calm_since; /* the followed fight has been calm (no car within a second) since then */
} Car;

static Car C[NC];
static int g_n;              /* participants */
static int g_me = -1;        /* your car */
static float g_len;          /* longest lap distance seen (track length estimate) */
static int g_session_type = -1;
static int g_in_race;
static double g_race_t0;

/* control state */
static int ai_want, ai_on, ai_auto_done;
static double ai_lost_at, ai_next_try, hb_at;
static int hb_left;
static int watch_manual, watch_user_off, watch_active, race_over;
static double watch_next_try, watch_t0, lost_since;
static int dir_on = 1;
static double last_switch, next_eval;
static int cur_follow = -1, last_cam = -1;
static volatile long g_reset_req;
static int k_ai, k_watch, k_dir;

/* keep race (Spectator/Broadcast Mode by Kizi & RK, MIT) */
static uintptr_t kc_term_vt;
static volatile long kc_broken, kc_vehicle = -1, kc_garaged;
static void *volatile kc_pit_system;
static volatile long kc_own_call;
static double kc_since;
typedef uint64_t (*PitSet_t)(void *, uint64_t, uint64_t);
static PitSet_t orig_pit_set;
typedef uint64_t (*BusPost_t)(void *, void *);
static BusPost_t orig_bus_post;

/* a race; with no UDP telemetry yet the session type is unknown (-1) and Swofty's race.start event decides */
static uint32_t me_mode = 0xffffffffu, me_flags = 0xffffffffu;
static double me_next;
static int g_leader = -1;
static int g_paused;
static int g_clock_stopped; /* only the race clock (pause menu, replay, loading), not our replay flag */           /* the race clock is not moving: pause menu, instant replay, loading */
static int g_phase_lap = -1;    /* leader's lap of a start / restart: the camera stays on the leader for it */
static const char *g_phase_name = "THE START";
static int g_redflag;           /* red flag shown: the next getaway is a restart */
static volatile long g_in_replay; /* the game's instant replay state is on screen */
static double g_leader_seen;
static double g_lights_out, g_reset_t, g_resume_t;
static long g_ev_r;
static int g_skip_old_events;
static long g_ev_mark;
static long g_ev_w_now(void);
static int g_lights_seen;
static void me_refresh(void);
static int race_session(void) { return g_session_type < 0 || (g_session_type >= 15 && g_session_type <= 17); }

static uint32_t car_flags(int c, uint32_t *mode)
{
    const SwoftySessionOps *o = ops();
    uint32_t m = 0, f = 0;
    if (!o || !o->control || o->control((uint32_t)c, &m, &f) != SWOFTY_ASSET_OK) {
        if (mode) *mode = 0xffffffffu;
        return 0xffffffffu;
    }
    if (mode) *mode = m;
    return f;
}

static void make_abbr(Car *c)
{
    const char *s = c->name;
    /* use the last word (surname), 3 letters, upper case */
    const char *w = s;
    for (const char *p = s; *p; p++)
        if (*p == ' ' && p[1]) w = p + 1;
    int i = 0;
    for (; w[i] && i < 3; i++) {
        char ch = w[i];
        if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
        c->abbr[i] = ch;
    }
    c->abbr[i] = 0;
    if (!i) scopy(c->abbr, 4, "???");
}

static void gfx_reset(void);
static void gfx_fastest_lap(void);
static void gfx_tick(double t);
static void reset_race(const char *why)
{
    gfx_reset();
    memset(C, 0, sizeof C);
    for (int i = 0; i < NC; i++) C[i].pos = 99;
    g_len = 0;
    /* your car and AI drive carry on through a restart of the same session (the formation lap ends with a race.start
       too, and skipping it as well): only the end of the session forgets them */
    me_mode = me_flags = 0xffffffffu;
    me_next = 0;
    g_leader = -1;
    g_leader_seen = 0;
    g_phase_lap = 1;
    g_phase_name = "THE START";
    g_redflag = 0;
    ai_on = 0;
    ai_lost_at = ai_next_try = hb_at = 0;
    if (ai_want) LOG("AI drive stays on through the race start: it takes the car again");
    hb_left = 0;
    watch_manual = watch_user_off = race_over = 0;
    lost_since = 0;
    cur_follow = -1;
    last_cam = -1;
    last_switch = 0;
    kc_broken = 0;
    kc_vehicle = -1;
    kc_garaged = 0;
    kc_since = 0;
    g_skip_old_events = 1; /* old events (an earlier session) are not news: from the ring position at this moment */
    g_ev_mark = g_ev_w_now();
    g_lights_seen = 0;
    g_reset_t = now();
    LOG("reset (%s)", why);
}

#include "hud_fix.c"

/* ---- spectator ------------------------------------------------------------------------------------------- */
static void spectate_off(const char *why)
{
    const SwoftySessionOps *o = ops();
    int was = watch_active;
    watch_active = 0;
    cur_follow = -1;
    hb_set(0);
    if (o && o->spectator_set_active && was) {
        int r = o->spectator_set_active(0);
        LOG("spectate OFF (%s) -> %d", why, r);
        if (S.camera_key) press_key_once(S.camera_key);
    }
}
static void spectate_on(const char *why)
{
    const SwoftySessionOps *o = ops();
    double t = now();
    if (watch_active || t < watch_next_try || !o || !o->spectator_set_active) return;
    watch_next_try = t + 1.0;
    int r = o->spectator_set_active(1);
    if (r != SWOFTY_ASSET_OK) {
        LOG("spectate ON (%s) -> %d (will try again)", why, r);
        return;
    }
    watch_active = 1;
    watch_t0 = t;
    last_switch = 0;
    last_cam = -1;
    lost_since = 0;
    int cam = S.cam_mode == 1 ? 1 : 2;
    if (o->spectator_set_camera) o->spectator_set_camera(cam);
    last_cam = cam;
    LOG("spectate ON (%s), camera %s", why, cam == 1 ? "TV pod" : "broadcast");
    if (S.toasts) toast("BROADCAST", dir_on && S.director ? "AUTO DIRECTOR ON" : "MANUAL CAMERA", 0);
}

/* ---- the snapshot the overlay draws (written on the flow thread, read on the render thread) ------------ */
typedef struct {
    int show_any, watching, ai, director, me;
    int n;
    struct { int car, pos; char abbr[4]; float gap; int follow, out, mine, pit; } row[NC];
    char cap_title[48], cap_detail[96];
    double cap_until;
    char countdown[24];
} Snap;
static Snap g_snap[2];
static volatile long g_snap_idx;
static Snap g_draft;
static char g_cap_t[48], g_cap_d[96];
static double g_cap_until;

static void publish(void)
{
    Snap *s = &g_draft;
    memset(s, 0, sizeof *s);
    s->watching = watch_active;
    s->ai = ai_on;
    s->director = dir_on && S.director;
    s->me = g_me;
    s->show_any = g_in_race && (watch_active || ai_on || hb_left > 0);
    if (hb_left > 0) fmt(s->countdown, sizeof s->countdown, "YOUR CAR IN %d", hb_left);
    /* tower: cars sorted by position */
    int n = 0;
    for (int p = 1; p <= NC && n < NC; p++)
        for (int i = 0; i < g_n && i < NC; i++)
            if (C[i].valid && C[i].pos == p) {
                s->row[n].car = i;
                s->row[n].pos = p;
                scopy(s->row[n].abbr, 4, C[i].abbr);
                s->row[n].gap = C[i].gap_ahead;
                s->row[n].follow = i == cur_follow;
                s->row[n].out = (C[i].flags & (F_RETIRED | F_TERMINAL)) != 0;
                s->row[n].pit = C[i].in_pit;
                s->row[n].mine = i == g_me;
                n++;
            }
    s->n = n;
    scopy(s->cap_title, sizeof s->cap_title, g_cap_t);
    scopy(s->cap_detail, sizeof s->cap_detail, g_cap_d);
    s->cap_until = g_cap_until;
    long next = 1 - g_snap_idx;
    memcpy(&g_snap[next], s, sizeof *s);
    __atomic_store_n(&g_snap_idx, next, __ATOMIC_RELEASE);
}

static void caption(const char *t, const char *d, double secs)
{
    scopy(g_cap_t, sizeof g_cap_t, t);
    scopy(g_cap_d, sizeof g_cap_d, d);
    g_cap_until = now() + secs;
}

/* ---- telemetry update and story detection ------------------------------------------------------------------ */
static void incident_m(int i, float score, double secs, const char *kind, const char *text, int mate)
{
    Car *c = &C[i];
    double t = now();
    if (t < c->inc_until && c->inc_score >= score) return;
    c->inc_score = score;
    c->inc_mate = -1;
    c->inc_until = t + secs;
    scopy(c->inc_kind, sizeof c->inc_kind, kind);
    scopy(c->inc_text, sizeof c->inc_text, text);
    if (mate >= 0 && mate < NC) fx_car2 = mate, fx_name2 = C[mate].name;
    fx_pos = c->pos;
    feed(kind, i, c->name, text);
    c->inc_mate = mate;
    if (S.debug) LOG("story %s car %d (%s): %s", kind, i, c->abbr, text);
}

static void incident(int i, float score, double secs, const char *kind, const char *text) { incident_m(i, score, secs, kind, text, -1); }

/* ---- replay candidates: what a TV director would offer to show again ----------------------------------------------
   Every crash and contact; the start and every restart (offered at the end of that lap); overtakes only when they matter
   (the lead, the podium, the last points place, or the last three laps). Kept with the race time so the replay can rewind to
   a few seconds before. For now they are logged and written to the event feed; the replay itself comes next. */
typedef struct { double t; float race_time; int car, car2, importance; char kind[16], text[96]; } ReplayCand;
static ReplayCand g_rc[16];
static int g_rc_n;
static void replay_candidate(const char *kind, int car, int importance, const char *text)
{
    ReplayCand *r = &g_rc[g_rc_n % 16];
    g_rc_n++;
    r->t = now();
    r->race_time = 0;
    r->car = car;
    r->car2 = -1;
    r->importance = importance;
    scopy(r->kind, sizeof r->kind, kind);
    scopy(r->text, sizeof r->text, text);
    LOG("REPLAY CANDIDATE %s (importance %d): %s", kind, importance, text);
    feed("replay_offer", car, "", text);
}

/* ---- the game's UDP telemetry (Settings > Telemetry: UDP on, 127.0.0.1, port 20777, format 2026 or 2025) -------------
   Read on its own thread into U; the flow thread copies what it needs. Word-sized fields, benign races. */
typedef struct {
    volatile long packets, session_type, player, active_cars, total_laps, track_len;
    volatile long format;
    volatile long weather, track_temp, air_temp, track_id, sc_status, game_paused, has_session;
    volatile float session_time;
    struct {
        float x, y, z;              /* motion */
        float speed, brake;         /* car telemetry (speed in m/s) */
        float lap_dist, total_dist; /* lap data */
        int32_t gap_ms, lead_ms, pos, lap, pit, result, driver_status, stops, pen_s, grid, last_lap_ms, lane_ms, stop_ms;
        int32_t compound, tyre_age, has_status, team, number; /* car status, participants */
        int32_t wing_fl, wing_fr, wing_r, wear;                /* car damage (percent) */
        float fuel_laps;
        char name[33];
        int has_lap;
    } car[24];
    /* events, a small ring */
    volatile long ev_w;
    struct { char code[5]; int a, b; float f; uint8_t raw[8]; } ev[64];
} UdpState;
static UdpState U;
static volatile long g_udp_bound;

typedef struct { uint16_t family, port; uint32_t addr; uint8_t zero[8]; } SOCKADDR_IN4;
typedef uintptr_t SOCKET;
typedef struct { uint16_t v, hv; char d[257], s[129]; unsigned short mx, mu; char *vi; } WSADATA_;
__declspec(dllimport) int __stdcall WSAStartup(uint16_t, WSADATA_ *);
__declspec(dllimport) SOCKET __stdcall socket(int, int, int);
__declspec(dllimport) int __stdcall bind(SOCKET, const void *, int);
__declspec(dllimport) int __stdcall setsockopt(SOCKET, int, int, const char *, int);
__declspec(dllimport) int __stdcall recvfrom(SOCKET, char *, int, int, void *, int *);
__declspec(dllimport) int __stdcall closesocket(SOCKET);
__declspec(dllimport) int __stdcall WSAGetLastError(void);

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static float rdf(const uint8_t *p) { uint32_t u = rd32(p); float f; memcpy(&f, &u, 4); return f; }

static void udp_packet(const uint8_t *d, int n)
{
    const int H = 29;
    if (n < H) return;
    int fmt_ = rd16(d);
    if (fmt_ < 2023) return; /* packet format */
    /* 2026 (the 2026 Season Pack's format): 24 car slots, 16-bit ids in participants, a longer car status and
       shorter motion and telemetry entries. Entry sizes are worked out from the packet size, so only the slot count
       and the participant offsets differ here. */
    const int NS = fmt_ >= 2026 ? 24 : 22;
    U.format = fmt_;
    int pid = d[6];
    U.player = d[27];
    U.session_time = rdf(d + 15);
    U.packets++;
    const uint8_t *b = d + H;
    if (pid == 0) { /* motion: world position first (60 bytes per car in 2025, 54 in 2026) */
        int sz = (n - H) / NS;
        if (sz < 12) return;
        for (int i = 0; i < NS && H + (i + 1) * sz <= n; i++) {
            U.car[i].x = rdf(b + i * sz), U.car[i].y = rdf(b + i * sz + 4), U.car[i].z = rdf(b + i * sz + 8);
        }
    } else if (pid == 1) { /* session */
        if (n > H + 7) U.session_type = b[6], U.total_laps = b[3], U.track_len = rd16(b + 4);
        if (n > H + 8) U.weather = b[0], U.track_temp = (int8_t)b[1], U.air_temp = (int8_t)b[2], U.track_id = (int8_t)b[7];
        if (n > H + 15) U.game_paused = b[14];
        if (n > H + 125) U.sc_status = b[124]; /* 0 none, 1 safety car, 2 virtual, 3 formation lap */
        U.has_session = 1;
    } else if (pid == 2) { /* lap data, 57 bytes per car in 2025 */
        int sz = (n - H - 2) / NS;
        if (sz < 46) return;
        for (int i = 0; i < NS; i++) {
            const uint8_t *o = b + i * sz;
            if (o + 46 > d + n) break;
            U.car[i].gap_ms = (int32_t)rd16(o + 14) + (int32_t)o[16] * 60000;
            U.car[i].lead_ms = (int32_t)rd16(o + 17) + (int32_t)o[19] * 60000;
            U.car[i].last_lap_ms = (int32_t)rd32(o);
            U.car[i].stops = o[35];
            U.car[i].pen_s = o[38];
            U.car[i].grid = o[43];
            if (sz >= 51) U.car[i].lane_ms = rd16(o + 47), U.car[i].stop_ms = rd16(o + 49);
            U.car[i].lap_dist = rdf(o + 20);
            U.car[i].total_dist = rdf(o + 24);
            U.car[i].pos = o[32];
            U.car[i].lap = o[33];
            U.car[i].pit = o[34];
            U.car[i].driver_status = o[44];
            U.car[i].result = o[45];
            U.car[i].has_lap = 1;
        }
    } else if (pid == 3) { /* event: code + details */
        if (n < H + 4) return;
        long w = U.ev_w;
        int k = (int)(w & 63);
        memcpy(U.ev[k].code, b, 4);
        U.ev[k].code[4] = 0;
        U.ev[k].a = n > H + 4 ? b[4] : -1;
        U.ev[k].b = n > H + 5 ? b[5] : -1;
        U.ev[k].f = n > H + 9 ? rdf(b + 5) : 0;
        for (int j = 0; j < 8; j++) U.ev[k].raw[j] = H + 4 + j < n ? b[4 + j] : 0;
        __atomic_store_n(&U.ev_w, w + 1, __ATOMIC_RELEASE);
    } else if (pid == 4) { /* participants */
        int sz = (n - H - 1) / NS;
        if (sz < 39) return;
        U.active_cars = b[0];
        for (int i = 0; i < NS; i++) {
            const uint8_t *o = b + 1 + i * sz;
            if (o + 39 > d + n) break;
            int big = fmt_ >= 2026;   /* 2026: driver, network and team ids are 16-bit */
            if (o + (big ? 42 : 39) > d + n) break;
            U.car[i].team = big ? rd16(o + 5) : o[3];
            U.car[i].number = o[big ? 8 : 5];
            memcpy(U.car[i].name, o + (big ? 10 : 7), 32);
            U.car[i].name[32] = 0;
        }
    } else if (pid == 6) { /* car telemetry */
        int sz = (n - H - 3) / NS;
        if (sz < 14) return;
        for (int i = 0; i < NS; i++) {
            const uint8_t *o = b + i * sz;
            if (o + 14 > d + n) break;
            U.car[i].speed = (float)rd16(o) / 3.6f;
            U.car[i].brake = rdf(o + 10);
        }
    } else if (pid == 7) { /* car status: tyres (visual compound 16 soft, 17 medium, 18 hard, 7 inter, 8 wet), fuel */
        int sz = (n - H) / NS;
        if (sz < 28) return;
        for (int i = 0; i < NS; i++) {
            const uint8_t *o = b + i * sz;
            if (o + 28 > d + n) break;
            U.car[i].fuel_laps = rdf(o + 13);
            U.car[i].compound = o[26];
            U.car[i].tyre_age = o[27];
            U.car[i].has_status = 1;
        }
    } else if (pid == 10) { /* car damage: tyre wear, wings (2025 has tyre blisters before the wings) */
        int sz = (n - H) / NS;
        int wo = sz >= 46 ? 28 : 24;
        if (sz < 27) return;
        for (int i = 0; i < NS; i++) {
            const uint8_t *o = b + i * sz;
            if (o + wo + 3 > d + n) break;
            float w = (rdf(o) + rdf(o + 4) + rdf(o + 8) + rdf(o + 12)) / 4.0f;
            U.car[i].wear = (int32_t)(w + 0.5f);
            U.car[i].wing_fl = o[wo], U.car[i].wing_fr = o[wo + 1], U.car[i].wing_r = o[wo + 2];
        }
    }
}

static DWORD __stdcall udp_main(void *unused)
{
    (void)unused;
    static uint8_t buf[2048];
    WSADATA_ wd;
    if (WSAStartup(0x0202, &wd) != 0) { g_api->log(TAG "UDP: WSAStartup failed"); return 0; }
    for (;;) {
        SOCKET sk = socket(2, 2, 17);
        if (sk == (SOCKET)~0ull) { Sleep(2000); continue; }
        int yes = 1;
        setsockopt(sk, 0xffff, 0x0004, (const char *)&yes, 4); /* SO_REUSEADDR */
        DWORD to = 500;
        setsockopt(sk, 0xffff, 0x1006, (const char *)&to, 4);  /* SO_RCVTIMEO */
        SOCKADDR_IN4 a;
        memset(&a, 0, sizeof a);
        a.family = 2;
        a.port = (uint16_t)((g_udp_port >> 8 & 0xff) | (g_udp_port & 0xff) << 8);
        a.addr = 0;
        if (bind(sk, &a, sizeof a) != 0) {
            g_api->log(TAG "UDP: can't listen on port %d (error %d): is another program using it? Retrying in 10 s", g_udp_port, WSAGetLastError());
            closesocket(sk);
            Sleep(10000);
            continue;
        }
        g_udp_bound = 1;
        g_api->log(TAG "UDP: listening on port %d (turn the game's UDP telemetry on: 127.0.0.1, port %d, format 2026 or 2025)", g_udp_port, g_udp_port);
        int port_now = g_udp_port;
        while (port_now == g_udp_port) {
            int r = recvfrom(sk, (char *)buf, sizeof buf, 0, 0, 0);
            if (r > 0) udp_packet(buf, r);
        }
        closesocket(sk);
        g_udp_bound = 0;
    }
    return 0;
}

static int g_udp_warned;
static long g_ev_w_now(void) { return __atomic_load_n(&U.ev_w, __ATOMIC_ACQUIRE); }
/* ---- contact, judged a moment later --------------------------------------------------------------------------------- */
static struct { int a, b; double t; int dmg_a, dmg_b, pos_a, pos_b; } g_ct[8];
static int g_minor_contacts; /* clean touches off camera since the last summary */
static int car_dmg(int i) { int d = U.car[i].wing_fl > U.car[i].wing_fr ? U.car[i].wing_fl : U.car[i].wing_fr; return U.car[i].wing_r > d ? U.car[i].wing_r : d; }
static void contact_note(int a, int b)
{
    double t = now();
    for (int k = 0; k < 8; k++) /* one contact gives several COLL events */
        if (g_ct[k].t > 0 && ((g_ct[k].a == a && g_ct[k].b == b) || (g_ct[k].a == b && g_ct[k].b == a)) && t - g_ct[k].t < 4) return;
    for (int k = 0; k < 8; k++)
        if (g_ct[k].t == 0) { g_ct[k].a = a, g_ct[k].b = b, g_ct[k].t = t, g_ct[k].dmg_a = car_dmg(a), g_ct[k].dmg_b = car_dmg(b), g_ct[k].pos_a = C[a].pos, g_ct[k].pos_b = C[b].pos; return; }
}
static void contact_tick(void)
{
    double t = now();
    for (int k = 0; k < 8; k++) {
        if (g_ct[k].t == 0 || t - g_ct[k].t < 2.0) continue;
        int a = g_ct[k].a, b = g_ct[k].b;
        g_ct[k].t = 0;
        if (a < 0 || b < 0 || a >= g_n || b >= g_n) continue;
        int dmg = car_dmg(a) - g_ct[k].dmg_a >= 8 || car_dmg(b) - g_ct[k].dmg_b >= 8;
        int spun = C[a].speed < 8.0f || C[b].speed < 8.0f || seq(C[a].why_kind, "spin") || seq(C[b].why_kind, "spin");
        int lost = C[a].pos - g_ct[k].pos_a >= 2 || C[b].pos - g_ct[k].pos_b >= 2;
        char txt[96];
        fmt(txt, sizeof txt, "CONTACT BETWEEN %s AND %s", C[a].abbr, C[b].abbr);
        int behind = C[a].pos > C[b].pos ? a : b, mate = behind == a ? b : a;
        if (dmg || spun || lost) {
            LOG("contact %s / %s: %s", C[a].abbr, C[b].abbr, dmg ? "damage" : spun ? "spin" : "places lost");
            incident_m(behind, 150, 7.0, "contact", txt, mate);
            replay_candidate("contact", behind, 300, txt);
            g_rc[(g_rc_n - 1) % 16].car2 = mate;
        } else if (watch_active && (cur_follow == a || cur_follow == b)) {
            fx_car2 = mate, fx_name2 = C[mate].name, fx_pos = C[behind].pos;
            feed("touch", behind, C[behind].name, txt); /* a word from the commentary, no cut, no replay */
        } else {
            g_minor_contacts++;
            fx_car2 = mate, fx_name2 = C[mate].name, fx_pos = C[behind].pos;
            feed("touch_off", behind, C[behind].name, txt); /* counted: a scrappy midfield gets a word now and then */
        }
    }
}
static void udp_events(void)
{
    long w = __atomic_load_n(&U.ev_w, __ATOMIC_ACQUIRE);
    if (g_skip_old_events) g_skip_old_events = 0, g_ev_r = g_ev_mark;
    if (w - g_ev_r > 64) g_ev_r = w - 64;
    for (; g_ev_r < w; g_ev_r++) {
        int k = (int)(g_ev_r & 63);
        const char *code = U.ev[k].code;
        int a = U.ev[k].a, b = U.ev[k].b;
        char txt[96];
        if (S.debug) LOG("UDP event %s %d %d", code, a, b);
        if (!g_in_race || !race_session()) continue;
        if (seq(code, "COLL") && a >= 0 && a < g_n && b >= 0 && b < g_n) {
            /* contact is judged 2 s later (contact_tick): damage, a spin or lost places make it an incident; a clean
               touch on the car we are watching gets a quick word; a clean touch off camera is let go */
            contact_note(a, b);
        } else if (seq(code, "OVTK") && a >= 0 && a < g_n && b >= 0 && b < g_n &&
                   (C[a].in_pit || C[b].in_pit || now() - C[a].pit_left < 20 || now() - C[b].pit_left < 20)) {
            /* a place changing hands in the pit cycle: not an overtake. The car that pitted lost it (or gained it) */
            int pitted = (C[b].in_pit || now() - C[b].pit_left < 20) ? b : a, other = pitted == a ? b : a;
            fmt(txt, sizeof txt, "%s %s %s IN THE PIT CYCLE", C[pitted].abbr, pitted == b ? "DROPS BEHIND" : "COMES OUT AHEAD OF", C[other].abbr);
            fx_car2 = other, fx_name2 = C[other].name, fx_pos = C[pitted].pos;
            feed("pit_cycle", pitted, C[pitted].name, pitted == b ? "lost" : "gained");
            if (S.debug) LOG("%s", txt);
        } else if (seq(code, "OVTK") && a >= 0 && a < g_n && b >= 0 && b < g_n) {
            int place = C[a].pos < C[b].pos ? C[a].pos : C[b].pos;
            if (place == 1) fmt(txt, sizeof txt, "%s PASSES %s FOR THE LEAD", C[a].abbr, C[b].abbr);
            else fmt(txt, sizeof txt, "%s PASSES %s FOR P%d", C[a].abbr, C[b].abbr, place);
            /* higher places matter more: two passes at once, the camera picks the one nearer the front */
            incident_m(a, 150.0f - (float)place * 3.0f, 5.0, "overtake", txt, b);
            int total = (int)U.total_laps, lead_lap = g_leader >= 0 ? C[g_leader].lap : 0;
            int imp = place == 1 ? 260 : place <= 3 ? 220 : place == 10 ? 180 : (total > 0 && lead_lap > total - 3) ? 160 : 0;
            if (imp) { replay_candidate("overtake", a, imp, txt); g_rc[(g_rc_n - 1) % 16].car2 = b; }
        } else if (seq(code, "RTMT") && a >= 0 && a < g_n) {
            fmt(txt, sizeof txt, "%s IS OUT OF THE RACE", C[a].abbr);
            incident(a, 175, 12.0, "out", txt); /* a big story: the camera goes there, even from a fight */
            C[a].was_out = 1;
        } else if (seq(code, "FTLP") && a >= 0 && a < g_n) {
            float tl = U.ev[k].f;
            int mins = (int)(tl / 60);
            fmt(txt, sizeof txt, "FASTEST LAP %s %d:%.3f", C[a].abbr, mins, (double)(tl - mins * 60));
            if (S.captions && !watch_active) {}
            feed("fastest_lap", a, C[a].name, txt);
            gfx_fastest_lap();
            if (watch_active && S.captions) caption("FASTEST LAP", txt, 5.0);
        } else if (seq(code, "RDFL")) {
            g_redflag = 1;
            for (int k2 = 0; k2 < 16; k2++) if (g_rc[k2].importance > 0) g_rc[k2].importance = -g_rc[k2].importance; /* nothing from before the red flag is offered */
            feed("red_flag", -1, "", "RED FLAG");
            if (watch_active && S.captions) caption("RACE CONTROL", "RED FLAG", 6.0);
        } else if (seq(code, "STLG")) {
            g_lights_seen = 1;
            fmt(txt, sizeof txt, "%d", a);
            feed("start_lights", -1, "", txt);
        } else if (seq(code, "LGOT")) {
            /* the game also sends this when the formation lap gets going: only the one after the start lights counts */
            if (U.sc_status == 3 || (!g_lights_seen && now() - g_reset_t < 30)) { LOG("lights out signal ignored (formation lap)"); continue; }
            g_lights_seen = 0;
            g_lights_out = now();
            feed("lights_out", -1, "", "LIGHTS OUT");
        } else if (seq(code, "SCAR") && a == 3) {
            /* the formation lap (from the session packet as well) */
        } else if (seq(code, "SCAR") && b == 3 && a == 2) {
            /* the virtual safety car ends: racing resumes where they are (no restart lap) */
            feed("safety_car", -1, "", "VIRTUAL SAFETY CAR - RACING RESUMES");
            if (watch_active && S.captions) caption("RACE CONTROL", "VIRTUAL SAFETY CAR ENDING", 6.0);
        } else if (seq(code, "SCAR") && b == 3 && g_leader >= 0) {
            /* safety car in, racing resumes: the restart lap follows the leader */
            g_phase_lap = C[g_leader].lap;
            g_phase_name = "THE RESTART";
            feed("restart", -1, "", "SAFETY CAR RESTART");
            replay_candidate("restart", g_leader, 200, "THE RESTART");
            fmt(txt, sizeof txt, "%s", "SAFETY CAR IN - RACING RESUMES");
            feed("safety_car", -1, "", txt);
            if (watch_active && S.captions) caption("RACE CONTROL", txt, 6.0);
        } else if (seq(code, "SCAR")) {
            /* a = safety car type (1 full, 2 virtual, 3 formation), b = event (0 deployed, 1 returning, 2 returned, 3 resume) */
            const char *what = a == 2 ? "VIRTUAL SAFETY CAR" : a == 1 ? "SAFETY CAR" : 0;
            if (what) {
                fmt(txt, sizeof txt, "%s %s", what, b == 0 ? "DEPLOYED" : b == 1 ? "IN THIS LAP" : b == 3 ? "- RACING RESUMES" : "ENDING");
                feed("safety_car", -1, "", txt);
                if (watch_active && S.captions) caption("RACE CONTROL", txt, 6.0);
            }
        } else if (seq(code, "PENA")) {
            /* penalty type, infringement, car, other car, time (s), lap, places gained */
            const uint8_t *r = U.ev[k].raw;
            int car = r[2], other = r[3];
            if (car < g_n && r[0] != 3 && !(r[0] >= 10 && r[0] <= 15)) { /* not reminders or lap invalidations */
                fmt(txt, sizeof txt, "type=%d;inf=%d;time=%d;lap=%d;places=%d", r[0], r[1], r[4], r[5], r[6]);
                if (other < g_n && other != car) fx_car2 = other, fx_name2 = C[other].name;
                fx_pos = C[car].pos;
                feed("penalty", car, C[car].name, txt);
            }
        } else if (seq(code, "DRSE") || seq(code, "DRSD")) {
            feed(seq(code, "DRSE") ? "drs_on" : "drs_off", -1, "", "");
        } else if (seq(code, "CHQF")) {
            feed("chequered", -1, "", "");
        }
    }
}

/* the race as a whole for the commentary: the session, the drivers, the formation lap and a state line every 4 s */
static void race_info(double tn)
{
    static long last_sc = -1;
    static double next_state, next_session;
    static int drivers_fed;
    if (!g_in_race) { last_sc = -1, drivers_fed = 0, next_session = 0; return; }
    if (U.has_session && tn >= next_session) {
        char t[200];
        next_session = tn + 60;
        fmt(t, sizeof t, "type=%d;track=%d;laps=%d;len=%d;weather=%d;air=%d;tracktemp=%d;cars=%d", (int)U.session_type, (int)U.track_id,
            (int)U.total_laps, (int)U.track_len, (int)U.weather, (int)U.air_temp, (int)U.track_temp, g_n);
        feed_long("session", t);
    }
    if (U.sc_status != last_sc) {
        if (U.sc_status == 3 && last_sc != 3) feed("formation", -1, "", "FORMATION LAP");
        last_sc = U.sc_status;
    }
    if (!drivers_fed || tn >= next_state) {
        /* drivers: index,team,number,grid,name ; state: index,pos,lap,gap to leader ms,interval ms,compound,tyre age,stops,
           pit,out,last lap ms,wear %,penalty s,fuel laps x10 */
        static char p[3000];
        int len = 0, named = 0;
        p[0] = 0;
        if (!drivers_fed) {
            for (int i = 0; i < g_n && len < 2800; i++)
                if (C[i].valid && C[i].name[0]) {
                    named++;
                    fmt(p + len, sizeof p - len, "%s%d,%d,%d,%d,%s", len ? ";" : "", i, U.car[i].team, U.car[i].number,
                               U.car[i].grid, C[i].name);
                    len = (int)slen(p);
                }
            if (named >= 2) feed_long("drivers", p), drivers_fed = 1;
            len = 0, p[0] = 0;
        }
        if (tn >= next_state) {
            next_state = tn + 4.0;
            for (int i = 0; i < g_n && len < 2800; i++) {
                if (!C[i].valid) continue;
                int out = (C[i].flags & (F_RETIRED | F_TERMINAL)) != 0 || U.car[i].result >= 4;
                fmt(p + len, sizeof p - len, "%s%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d", len ? ";" : "", i, C[i].pos, C[i].lap,
                           C[i].pos > 1 ? U.car[i].lead_ms : 0, C[i].pos > 1 ? U.car[i].gap_ms : 0, U.car[i].compound, U.car[i].tyre_age,
                           U.car[i].stops, U.car[i].pit, out, U.car[i].last_lap_ms, U.car[i].wear, U.car[i].pen_s,
                           (int)(U.car[i].fuel_laps * 10));
                len = (int)slen(p);
            }
            if (len) feed_long("state", p);
        }
    }
}

static void update_cars(double dt)
{
    if (g_me < 0 && g_in_race) { /* no UDP yet: your car is the one with the human controller */
        static double next_scan;
        if (now() >= next_scan) {
            next_scan = now() + 1.0;
            for (int i = 21; i >= 0; i--) { /* the player's car is usually the last one */
                uint32_t m = 9;
                if (car_flags(i, &m) != 0xffffffffu && m == 0) { g_me = i; LOG("your car is %d (human controller)", i); break; }
            }
        }
    }
    if (!U.packets) {
        if (g_in_race && race_session() && !g_udp_warned && now() - g_race_t0 > 15) {
            g_udp_warned = 1;
            LOG("no UDP telemetry received: turn it on in the game (Settings > Telemetry: UDP On, 127.0.0.1, port %d, format 2026 or 2025)", g_udp_port);
            toast("RACE BROADCAST", "TURN UDP TELEMETRY ON", 0);
        }
        /* without telemetry we still know the race type from Swofty's events: treat every session as a race */
        return;
    }
    g_session_type = (int)U.session_type;
    {
        /* paused when the race clock stops (pause menu, instant replay) or packets stop */
        static float last_st;
        static double st_moved, last_pk_t;
        static long last_pk;
        double tn0 = now();
        if (U.packets != last_pk) last_pk = U.packets, last_pk_t = tn0;
        static double st_run_since;
        static int was_stopped;
        if (U.session_time != last_st) {
            if (tn0 - st_moved > 0.3) st_run_since = tn0; /* the clock starts moving again */
            last_st = U.session_time, st_moved = tn0;
        }
        /* left the instant replay (by hand or by us): the race clock runs for 1 s */
        if (g_in_replay && st_run_since && tn0 - st_run_since > 1.0 && tn0 - st_moved < 0.3) { g_in_replay = 0; LOG("instant replay closed"); }
        g_clock_stopped = tn0 - st_moved > 0.4 || tn0 - last_pk_t > 0.5;
        int p = g_in_replay || g_clock_stopped;
        if (p != g_paused) { g_paused = p; LOG("race clock %s", p ? "stopped (pause / replay): director waits" : "running again"); }
        if (!g_clock_stopped && was_stopped) g_resume_t = tn0; /* positions and speeds jump as the race resumes */
        was_stopped = g_clock_stopped;
        if (g_paused) {
            /* the race clock also stands still on the grid until the lights go out: the game's events (start lights,
               lights out) still count, except during our own replay */
            if (!g_in_replay) udp_events();
            return;
        }
    }
    if (U.track_len > 1000) g_len = (float)U.track_len;
    g_n = (int)(U.active_cars > 0 && U.active_cars <= 24 ? U.active_cars : (U.format >= 2026 ? 24 : 22));
    if (g_me < 0 && U.player < 24) {
        /* only if that car really has a human controller: the index can be wrong around a start or while spectating */
        uint32_t m = 9;
        if (car_flags((int)U.player, &m) != 0xffffffffu && m == 0) {
            g_me = (int)U.player;
            LOG("your car is %d (UDP player index)", g_me);
        }
    }
    double tn = now();
    for (int i = 0; i < g_n; i++) {
        Car *c = &C[i];
        if (!U.car[i].has_lap) { c->valid = 0; continue; }
        if (!c->name[0] && U.car[i].name[0]) {
            scopy(c->name, sizeof c->name, U.car[i].name);
            make_abbr(c);
        }
        if (!c->abbr[0]) fmt(c->abbr, 4, "C%d", i);
        c->flags = i == g_me && me_flags != 0xffffffffu ? me_flags : 0;
        int res = U.car[i].result;
        {
            int was_in = c->in_pit;
            c->in_pit = (c->flags & F_PIT) != 0 || U.car[i].pit != 0 || U.car[i].driver_status == 0;
            if (was_in && !c->in_pit) c->pit_left = now();
        }
        int out = (c->flags & (F_RETIRED | F_TERMINAL)) != 0 || res >= 4;
        c->racing = !out && !c->in_pit && res == 2;
        c->prev_speed = c->valid ? c->speed : U.car[i].speed;
        c->speed = U.car[i].speed;
        c->brake = U.car[i].brake;
        c->prev_pos = c->valid ? c->pos : U.car[i].pos;
        c->pos = U.car[i].pos > 0 ? U.car[i].pos : 99;
        c->lap = U.car[i].lap;
        c->ld = U.car[i].lap_dist;
        if (c->ld > g_len && U.track_len <= 1000) g_len = c->ld;
        c->x = U.car[i].x, c->y = U.car[i].y, c->z = U.car[i].z;
        c->prev_gap = c->gap_ahead;
        c->gap_ahead = c->pos > 1 ? (float)U.car[i].gap_ms / 1000.0f : 0;
        if (dt > 0 && c->prev_gap > 0 && c->gap_ahead > 0 && c->valid) {
            float rate = (c->gap_ahead - c->prev_gap) / (float)dt;
            c->closing = c->closing * 0.85f + rate * 0.15f;
        }
        c->valid = 1;
        if (out && !c->was_out) {
            char txt[96];
            fmt(txt, sizeof txt, "%s IS OUT OF THE RACE", c->abbr);
            incident(i, 175, 12.0, "out", txt);
        }
        c->was_out = out;
        /* pit stops of cars in the points */
        int pitting = U.car[i].pit == 1;
        if (pitting && !c->was_pit && g_in_race && c->pos <= 10 && c->lap >= 2) {
            char txt[96];
            fmt(txt, sizeof txt, "%s PITS FROM P%d", c->abbr, c->pos);
            incident(i, 110.0f - (float)c->pos * 4.0f, 6.0, "pit", txt); /* the leader's stop beats P10's */
        }
        c->was_pit = pitting;
        /* follow each stop through the pit lane: stop time, lane time, tyres before and after */
        {
            int pr = U.car[i].pit;
            if (pr && !c->pit_raw) c->pit_comp_in = U.car[i].compound, c->pit_stop_max = c->pit_lane_max = 0, c->pit_from = c->pos, c->pit_still_t = 0, c->pit_leave_fed = 0;
            /* stopped in the box, then moving again: the "will he come out in front?" moment (front runners) */
            if (pr && c->speed < 0.5f) c->pit_still_t = tn;
            if (pr && c->pit_still_t > 0 && tn - c->pit_still_t < 3.0 && c->speed > 4.0f && !c->pit_leave_fed && g_in_race && c->pit_from <= 10) {
                c->pit_leave_fed = 1;
                fx_pos = c->pos;
                feed("pit_leaving", i, c->name, "LEAVING THE PIT BOX");
            }
            if (pr) {
                if (U.car[i].stop_ms > c->pit_stop_max) c->pit_stop_max = U.car[i].stop_ms;
                if (U.car[i].lane_ms > c->pit_lane_max) c->pit_lane_max = U.car[i].lane_ms;
            }
            if (!pr && c->pit_raw && g_in_race && c->lap >= 1 && !out) c->pit_out_at = tn + 2.5; /* tyres update a moment later */
            c->pit_raw = pr;
            if (c->pit_out_at && tn >= c->pit_out_at) {
                char txt[160];
                c->pit_out_at = 0;
                fmt(txt, sizeof txt, "from=%d;old=%d;new=%d;age=%d;stop=%d;lane=%d;stops=%d", c->pit_from, c->pit_comp_in,
                    U.car[i].compound, U.car[i].tyre_age, c->pit_stop_max, c->pit_lane_max, U.car[i].stops);
                fx_pos = c->pos;
                feed("pit_out", i, c->name, txt);
            }
        }
        /* wing damage after contact */
        {
            int dmg = U.car[i].wing_fl > U.car[i].wing_fr ? U.car[i].wing_fl : U.car[i].wing_fr;
            if (dmg >= 25 && c->dmg_prev < 25 && g_in_race && race_session()) {
                char txt[64];
                fmt(txt, sizeof txt, "wing=%d;fl=%d;fr=%d", dmg, U.car[i].wing_fl, U.car[i].wing_fr);
                fx_pos = c->pos;
                feed("damage", i, c->name, txt);
            }
            c->dmg_prev = dmg;
        }
        if (!g_in_race || !race_session()) continue;
        /* hard deceleration without braking: a spin or a hit */
        if (c->racing && tn - g_resume_t > 5.0 && dt > 0.05 && c->prev_speed - c->speed > 18.0f * (float)(dt / 0.25) && c->brake < 0.35f &&
            c->prev_speed > 25.0f) {
            char txt[96];
            fmt(txt, sizeof txt, "%s IN TROUBLE", c->abbr);
            incident(i, 160, 7.0, "spin", txt);
        }
        /* stopped on track */
        if (c->racing && c->lap >= 1 && c->speed < 3.0f && tn - g_race_t0 > 20 && tn - g_resume_t > 5.0) {
            if (!c->stopped_since) c->stopped_since = tn;
            else if (tn - c->stopped_since > 1.5 && !c->stop_fed) {
                char txt[96];
                c->stop_fed = 1;
                fmt(txt, sizeof txt, "%s HAS STOPPED ON TRACK", c->abbr);
                incident(i, 165, 12.0, "stopped", txt); /* a crash or a failure: the camera goes there */
            }
        } else if (c->speed > 15.0f) {
            c->stopped_since = 0, c->stop_fed = 0;
        }
        /* losing pace without a reason we can see (a failure, a puncture, damage): well under the car's usual speed
           for 3 s, not braking, not in the pits, not behind the safety car */
        if (c->racing && c->speed > 30.0f && !U.sc_status) c->avg_spd = c->avg_spd > 0 ? c->avg_spd * 0.995f + c->speed * 0.005f : c->speed;
        if (c->racing && c->lap >= 1 && c->avg_spd > 40.0f && !U.sc_status && tn - g_race_t0 > 30 && tn - g_resume_t > 8.0 &&
            c->speed > 3.0f && c->speed < c->avg_spd * 0.45f && c->brake < 0.2f) {
            if (!c->slow_since) c->slow_since = tn;
            else if (tn - c->slow_since > 3.0 && !c->slow_fed) {
                char txt[96];
                c->slow_fed = 1;
                fmt(txt, sizeof txt, "%s IS SLOWING, A PROBLEM?", c->abbr);
                incident(i, 155, 12.0, "slow", txt);
            }
        } else if (c->speed > c->avg_spd * 0.7f) {
            c->slow_since = 0, c->slow_fed = 0;
        }
    }
    udp_events();
    contact_tick();
    race_info(tn);
    /* laps and the flag, for the commentary, whether the broadcast is on or not */
    {
        static int last_lap_fed, finish_fed;
        int ld = -1;
        for (int i = 0; i < g_n; i++) if (C[i].valid && C[i].pos == 1) ld = i;
        int total = (int)U.total_laps, lead_lap = ld >= 0 ? C[ld].lap : 0;
        if (ld >= 0) g_leader = ld;
        g_lead_lap_fx = lead_lap;
        g_total_fx = total;
        if (g_in_race && race_session()) {
            if (ld >= 0 && lead_lap > last_lap_fed) {
                last_lap_fed = lead_lap;
                if (lead_lap >= 2 && !finish_fed) { fx_pos = 1; feed(total > 0 && lead_lap == total ? "final_lap" : "lap", ld, C[ld].name, ""); }
            }
            if (ld >= 0 && U.car[ld].result == 3 && !finish_fed) { finish_fed = 1; fx_pos = 1; feed("finish", ld, C[ld].name, "CHEQUERED FLAG"); }
            if (lead_lap <= 1) last_lap_fed = lead_lap, finish_fed = 0;
        }
    }
    if (!g_in_race || !race_session()) return;
    /* side by side: two racing cars with centres under 2.6 m apart at speed */
    for (int i = 0; i < g_n; i++)
        for (int j = i + 1; j < g_n; j++) {
            Car *a = &C[i], *b = &C[j];
            if (!a->racing || !b->racing || a->speed < 15 || b->speed < 15) continue;
            float dx = a->x - b->x, dy = a->y - b->y, dz = a->z - b->z;
            float dist = sqrtf_(dx * dx + dy * dy + dz * dz);
            if (dist > 0.01f && dist < 2.6f) {
                Car *behind = a->pos > b->pos ? a : b, *ahead = a->pos > b->pos ? b : a;
                char txt[96];
                fmt(txt, sizeof txt, "%s AND %s SIDE BY SIDE FOR P%d", ahead->abbr, behind->abbr, ahead->pos);
                incident_m((int)(behind - C), ahead->pos <= 3 ? 115 : 100, 4.0, "side", txt, (int)(ahead - C));
            }
        }
}

/* ---- the director -------------------------------------------------------------------------------------------- */
/* How a TV director thinks, as numbers. Every car gets a score; the camera goes to the highest, with hold times.
   Incidents (contact, spins, stopped cars, retirements, overtakes, pit stops) score 75-160 for a few seconds.
   A battle scores by gap (under 1.2 s), how fast it is closing, and what is at stake (the lead, the podium, the
   last points place). The race phase changes the weights: the start follows the front, the last lap follows the
   leader to the flag. Cars that were on screen a lot lately score less, so every story gets its turn. */
static void score_cars(void)
{
    double t = now();
    int total = (int)U.total_laps;
    g_leader = -1;
    for (int i = 0; i < g_n; i++)
        if (C[i].valid && C[i].pos == 1) g_leader = i;
    int lead_lap = g_leader >= 0 ? C[g_leader].lap : 0;
    /* a red flag: the cars first drive back to the pit lane (1), stop there (2), and only then does the restart come */
    if (g_redflag == 1 && g_leader >= 0 && (C[g_leader].speed < 3.0f || C[g_leader].in_pit)) {
        static double stopped_since;
        if (!stopped_since) stopped_since = now();
        if (now() - stopped_since > 3.0) { g_redflag = 2; stopped_since = 0; LOG("red flag: the field has stopped"); }
    }
    if (g_redflag == 2 && g_leader >= 0 && C[g_leader].speed > 30 && !C[g_leader].in_pit) {
        g_redflag = 0;
        LOG("red flag: racing again");
        g_phase_lap = lead_lap;
        g_phase_name = "THE RESTART";
        feed("restart", -1, "", "RED FLAG RESTART");
    }
    /* the start / restart lap is over when the leader starts the next lap: offer its replay */
    if (g_phase_lap >= 0 && g_leader >= 0 && lead_lap > g_phase_lap) {
        replay_candidate(seq(g_phase_name, "THE START") ? "start" : "restart", g_leader, 240,
                         seq(g_phase_name, "THE START") ? "THE START" : "THE RESTART");
        g_phase_lap = -1;
    }
    int start_phase = g_phase_lap >= 0 && g_leader >= 0 && lead_lap == g_phase_lap;
    int last_lap = total > 0 && lead_lap >= total;
    for (int i = 0; i < g_n; i++) {
        Car *c = &C[i];
        c->score = -1;
        c->why_t[0] = c->why_d[0] = 0;
        scopy(c->why_kind, sizeof c->why_kind, "");
        c->ahead_id = -1;
        if (!c->valid) continue;
        if (t < c->inc_until) {
            c->score = c->inc_score;
            const char *k = c->inc_kind;
            scopy(c->why_kind, sizeof c->why_kind, k);
            scopy(c->why_t, sizeof c->why_t,
                  seq(k, "overtake") ? "OVERTAKE" : seq(k, "side") ? "WHEEL TO WHEEL" : seq(k, "contact") ? "CONTACT"
                  : seq(k, "spin") ? "INCIDENT" : seq(k, "stopped") ? "CAR STOPPED" : seq(k, "out") ? "RETIREMENT"
                  : seq(k, "pit") ? "PIT STOP" : seq(k, "slow") ? "TROUBLE" : "INCIDENT");
            scopy(c->why_d, sizeof c->why_d, c->inc_text);
            if (c->racing || seq(k, "out") || seq(k, "stopped") || seq(k, "slow") || seq(k, "pit")) continue;
        }
        /* a stop in progress (lap 2 on, in the points): it stays a story until the car leaves the pit lane */
        if (!c->racing && U.car[i].pit != 0 && c->pos <= 10 && c->lap >= 2 && !c->was_out) {
            c->score = 70.0f - (float)c->pos * 2.0f;
            scopy(c->why_kind, sizeof c->why_kind, "pit");
            scopy(c->why_t, sizeof c->why_t, "PIT STOP");
            fmt(c->why_d, sizeof c->why_d, "%s IN THE PIT LANE", c->abbr);
            continue;
        }
        if (!c->racing) continue;
        float s = 4;
        if (c->pos == 1) {
            s += 10;
            scopy(c->why_t, sizeof c->why_t, last_lap ? "FINAL LAP" : start_phase ? g_phase_name : "RACE LEADER");
            fmt(c->why_d, sizeof c->why_d, last_lap ? "%s LEADS INTO THE LAST LAP" : "%s LEADS", c->abbr);
            if (start_phase) s += 130; /* the whole lap on the leader; only crashes and spins (150+) cut away */
            if (last_lap) s += 90;
            if (t - g_leader_seen > 120) s += 35; /* a leader check every couple of minutes */
        } else if (c->pos <= 3) {
            s += 5;
            if (start_phase) s += 0;
        }
        if (i == g_me) s += (float)S.own_bias;
        float g = c->gap_ahead;
        if (c->pos > 1 && g > 0 && g < 1.2f) {
            float b = (1.2f - g) / 1.2f * 60.0f;
            if (c->closing < 0) b += (-c->closing * 40.0f > 25.0f ? 25.0f : -c->closing * 40.0f);
            int place = c->pos - 1;
            b *= place == 1 ? 2.0f : place <= 3 ? 1.6f : place == 10 ? 1.4f : place <= 10 ? 1.2f : 0.9f;
            if (g < 0.5f) b += 10;
            if (last_lap) b *= 1.3f;
            s += b;
            int ahead = -1;
            for (int j = 0; j < g_n; j++)
                if (C[j].valid && C[j].pos == c->pos - 1) ahead = j;
            if (place == 1) scopy(c->why_t, sizeof c->why_t, "BATTLE FOR THE LEAD");
            else if (place == 10) scopy(c->why_t, sizeof c->why_t, "FIGHT FOR THE LAST POINT");
            else fmt(c->why_t, sizeof c->why_t, "BATTLE FOR P%d", place);
            fmt(c->why_d, sizeof c->why_d, "%s %.1fs BEHIND %s%s", c->abbr, (double)g, ahead >= 0 ? C[ahead].abbr : "?",
                c->closing < -0.05f ? " AND CLOSING" : "");
            scopy(c->why_kind, sizeof c->why_kind, "battle");
            c->ahead_id = ahead;
        }
        if (start_phase && c->pos != 1 && s > 120) s = 120;
        /* variety: what was on screen a lot lately scores less (not for incidents) */
        float tired = start_phase && c->pos == 1 ? 0 : c->screen * 0.5f;
        s -= tired > 30 ? 30 : tired;
        if (s < 1) s = 1;
        if (!c->why_t[0]) {
            fmt(c->why_t, sizeof c->why_t, "P%d", c->pos);
            fmt(c->why_d, sizeof c->why_d, "%s%s", c->abbr, i == g_me ? " (YOUR CAR)" : "");
        }
        c->score = s;
    }
}

static double g_cur_next;   /* spectator_current is polled twice a second; a cut refreshes the cached answer */
static int32_t g_cur_last = -1;
static int g_shot_fought;    /* the shot on screen has shown a fight (for the settle time after it) */
static void follow(int car, const char *why_kind)
{
    const SwoftySessionOps *o = ops();
    if (!o || !o->spectator_follow) return;
    int r = o->spectator_follow((uint32_t)car);
    Car *c = &C[car];
    LOG("director: follow car %d (%s) score %.1f: %s | %s -> %d", car, c->abbr, (double)c->score, c->why_t, c->why_d, r);
    if (r != SWOFTY_ASSET_OK) return;
    cur_follow = car;
    last_switch = now();
    g_shot_fought = 0;
    C[car].calm_since = 0;
    g_cur_last = car, g_cur_next = last_switch + 0.6;
    if (S.cam_mode == 2 && o->spectator_set_camera) {
        int want = seq(why_kind, "battle") ? 1 : 2;
        if (want != last_cam) o->spectator_set_camera(want), last_cam = want;
    }
    if (S.captions) caption(c->why_t, c->why_d, 6.0);
    if (S.toasts) toast(c->why_t, c->why_d, 0);
    /* what is on screen now, for the commentary: the car, why, and the other car of the story */
    {
        int mate = c->inc_mate >= 0 && now() < c->inc_until ? c->inc_mate : c->ahead_id;
        if (mate >= 0 && mate < NC) fx_car2 = mate, fx_name2 = C[mate].name;
        fx_pos = c->pos;
        feed("shot", car, c->name, why_kind[0] ? why_kind : seq(c->why_t, "RACE LEADER") || c->pos == 1 ? "leader" : "car");
    }
    if (!seq(why_kind, "") && !seq(why_kind, "battle")) return; /* incidents were already fed */
    if (seq(why_kind, "battle")) {
        if (c->ahead_id >= 0) fx_car2 = c->ahead_id, fx_name2 = C[c->ahead_id].name;
        fx_pos = c->pos;
        feed("battle", car, c->name, c->why_d);
    } else if (!why_kind[0] && seq(c->why_t, "RACE LEADER")) {
        fx_pos = 1;
        feed("leader", car, c->name, c->why_d);
    }
}

static void direct(void)
{
    const SwoftySessionOps *o = ops();
    int32_t car = -1, cam = -1;
    double t = now();
    if (t >= g_cur_next) {
        g_cur_next = t + 0.5;
        if (!o || !o->spectator_current || o->spectator_current(&car, &cam) != SWOFTY_ASSET_OK) car = -1;
        g_cur_last = car;
    } else {
        car = g_cur_last;
    }
    /* screen time: the followed car gains, everyone decays */
    for (int i = 0; i < g_n; i++) C[i].screen *= 0.995f;
    if (car >= 0 && car < NC) C[car].screen += 0.1f;
    if (car >= 0 && car == g_leader) g_leader_seen = t;
    if (car < 0) {
        if (t - watch_t0 > 3.0) {
            if (!lost_since) lost_since = t;
            else if (t - lost_since > 3.0) {
                race_over = 1;
                spectate_off("director follows nobody: race over or cutscene");
                return;
            }
        }
    } else {
        lost_since = 0;
    }
    if (car >= 0 && car != cur_follow) {
        /* the camera moved by itself or by the game's keys */
        cur_follow = car;
        last_switch = t;
        fx_pos = C[car].pos;
        feed("shot", car, C[car].name, "manual");
    }
    if (!(dir_on && S.director) || t < next_eval) {
        /* director off: only leave a car that is out or in the pits */
        if (car >= 0 && car < NC && !C[car].racing && t - last_switch > 2.0) {
            int best = -1;
            for (int i = 0; i < g_n; i++)
                if (C[i].racing && i != car && (best < 0 || C[i].pos < C[best].pos)) best = i;
            if (best >= 0 && !(dir_on && S.director)) score_cars(), follow(best, "");
        }
        return;
    }
    next_eval = t + 0.25;
    score_cars();
    int best = -1;
    for (int i = 0; i < g_n; i++)
        if (C[i].score >= 0 && (best < 0 || C[i].score > C[best].score)) best = i;
    if (best < 0) return;
    int cur = cur_follow;
    float cs = cur >= 0 && cur < NC ? C[cur].score : -1;
    double held = t - last_switch;
    float bored = held > 25 ? (float)(held - 25) : 0;
    int switch_now = 0;
    /* an incident is a story from the game's events or our checks, not a battle (battles can score 100+ too) */
    int best_inc = C[best].why_kind[0] && !seq(C[best].why_kind, "battle");
    int cur_inc = cur >= 0 && cur < NC && C[cur].why_kind[0] && !seq(C[cur].why_kind, "battle");
    /* the two cars of one battle are one story: no ping-pong between them */
    int same_battle = cur >= 0 && cur < NC &&
                      (C[best].ahead_id == cur || C[cur].ahead_id == best || C[best].inc_mate == cur || C[cur].inc_mate == best);
    /* the start / restart lap: go to the leader at once */
    if (best == g_leader && seq(C[best].why_t, g_phase_name) && cur != best && held >= 2.5 && !cur_inc) { follow(best, ""); return; }
    /* a story still running on screen is not cut short: a fight stays until it is settled (no car within a second for
       2 s, so a place taken back is not missed), a pass until the cars have separated, a pit stop until the car leaves
       the pit lane. Only a big incident (contact, spin: 150+) or a fight much nearer the front can take the camera. */
    int live = 0, cur_place = 99;
    if (cur >= 0 && cur < NC && C[cur].valid) {
        Car *k = &C[cur];
        int fight = k->pos > 1 && k->gap_ahead > 0 && k->gap_ahead < 1.0f;
        for (int j = 0; j < g_n && !fight; j++)
            if (C[j].valid && C[j].pos == k->pos + 1 && C[j].gap_ahead > 0 && C[j].gap_ahead < 1.0f) fight = 1; /* defending */
        int pass = (seq(k->inc_kind, "overtake") || seq(k->inc_kind, "side")) && t < k->inc_until + 6 && k->inc_mate >= 0;
        if (fight || pass) { k->calm_since = 0; g_shot_fought = 1; cur_place = fight && k->gap_ahead > 0 && k->gap_ahead < 1.0f ? k->pos - 1 : k->pos; }
        else if (!k->calm_since) k->calm_since = t;
        if (fight || pass || (g_shot_fought && k->calm_since && t - k->calm_since < 2.0)) live = held < 75;
        if (U.car[cur].pit != 0 && k->lap >= 2 && !k->was_out && held < 45) live = 2;
        /* a crash, a failure, a retirement: stay with it while the story runs (only another big one takes over) */
        if ((seq(k->inc_kind, "out") || seq(k->inc_kind, "stopped") || seq(k->inc_kind, "slow") || seq(k->inc_kind, "spin")) &&
            t < k->inc_until && held < 20) live = 3;
    }
    int best_place = seq(C[best].why_kind, "battle") ? C[best].pos - 1 : seq(C[best].why_kind, "overtake") ? C[best].pos : 99;
    if (live && best != cur && !same_battle) {
        int big = best_inc && C[best].inc_score >= 150 && held >= 1.5 && !(live == 3 && C[best].inc_score <= C[cur].inc_score && held < 8);
        int front = live == 1 && best_place <= 3 && cur_place > best_place + 3 && held >= 4;
        if (big || front) follow(best, C[best].why_kind);
        return;
    }
    if (cur < 0 || cs < 0) switch_now = held > 1.0; /* followed car is gone, out (after its story) or in the pits */
    else if (best != cur && best_inc && !cur_inc && !same_battle && held >= (C[best].inc_score >= 150 ? 1.5 : 4.0) && C[best].inc_score >= 130) switch_now = 1; /* a real incident cuts in */
    else if (best != cur && !same_battle && held >= S.hold && C[best].score > (cs - bored) * 1.2f + 8) switch_now = 1;
    else if (best != cur && same_battle && held >= S.hold * 3 && C[best].score > cs + 30) switch_now = 1; /* swap sides now and then */
    if (switch_now && best != cur) follow(best, C[best].why_kind);
}

/* your car's controller: one session-op call three times a second (each call costs a few ms) */
static void me_refresh(void)
{
    double t = now();
    if (g_me < 0 || t < me_next) return;
    me_next = t + 0.33;
    me_flags = car_flags(g_me, &me_mode);
}

/* ---- AI drive ------------------------------------------------------------------------------------------------ */
static void ai_tick(void)
{
    const SwoftySessionOps *o = ops();
    if (!o || g_me < 0) return;
    double t = now();
    if (me_flags == 0xffffffffu) return;
    int ai_now = me_mode == 1;
    if (ai_want && !ai_now) {
        if (ai_on) { /* the game took the car back (flashback, restart, garage) */
            ai_on = 0;
            ai_lost_at = t;
            LOG("AI drive: the game took the car back, taking it again in 2.5 s");
        }
        if (t - ai_lost_at < 2.5 || t < ai_next_try) return;
        ai_next_try = t + 1.0;
        int r = o->set_control((uint32_t)g_me, 1);
        LOG("AI drive: set_control(car %d, AI) -> %d", g_me, r);
        if (r == SWOFTY_ASSET_OK) {
            ai_on = 1;
            me_mode = 1;
            toast("AI DRIVING", "THE AI HAS YOUR CAR", 0);
        }
    } else if (!ai_want && ai_now && ai_on) {
        /* the game refuses while your car is held (pit lane, red flag): try once a second, say so once */
        static double next_hb;
        static int refused;
        if (t < next_hb) return;
        next_hb = t + 1.0;
        int r = o->set_control((uint32_t)g_me, 0);
        if (r != SWOFTY_ASSET_OK) {
            if (!refused++) { LOG("AI drive: the game won't hand your car back yet (-> %d), trying once a second", r); toast("AI DRIVING", "YOUR CAR COMES BACK WHEN THE GAME ALLOWS", 0); }
            return;
        }
        LOG("AI drive: set_control(car %d, human) -> %d%s", g_me, r, refused ? " (after waiting)" : "");
        refused = 0;
        if (r == SWOFTY_ASSET_OK) {
            ai_on = 0;
            me_mode = 0;
            toast("YOU HAVE CONTROL", "", 0);
        }
    } else {
        ai_on = ai_now && ai_want;
    }
}

/* ---- keep race (from Spectator/Broadcast Mode, Kizi & RK, MIT) -------------------------------------------- */
static void kc_find(void)
{
    uintptr_t at = 0;
    int hits = g_api->scan(".xtls",
                           "48 C7 44 24 28 00 00 00 00 48 8D 0D ?? ?? ?? ?? 48 89 4C 24 20 48 8B 4C 24 48 48 89 4C 24 30 "
                           "48 8B 48 08 48 8D 54 24 20 E8 ?? ?? ?? ??",
                           &at);
    if (hits == 1) {
        int32_t rel = 0;
        if (mem_read(at + 12, &rel, 4)) kc_term_vt = at + 16 + (intptr_t)rel;
    }
    LOG("keep race: terminal damage message %s (matches %d)", kc_term_vt ? "found" : "NOT found", hits);
}
static uint64_t wrap_bus_post(void *bus, void *msg)
{
    uint64_t vt = 0;
    if (S.keep_race && kc_term_vt && msg && mem_read((uintptr_t)msg, &vt, 8) && vt == kc_term_vt) {
        uint8_t veh = 0xff;
        mem_read((uintptr_t)msg + 0x10, &veh, 1);
        int mine = 0;
        if (veh < 32) {
            uint32_t mode = 0;
            uint32_t f = car_flags(veh, &mode);
            mine = (f != 0xffffffffu && mode == 0) || (int)veh == g_me;
        }
        if (mine) {
            kc_vehicle = veh;
            if (!__atomic_exchange_n(&kc_broken, 1, __ATOMIC_SEQ_CST)) g_api->log(TAG "keep race: terminal damage for your car %d dropped, the race goes on", (int)veh);
            return 0;
        }
    }
    return orig_bus_post(bus, msg);
}
static uint64_t wrap_pit_set(void *sys, uint64_t vehicle, uint64_t status)
{
    kc_pit_system = sys;
    if (!kc_own_call && status != 9 && kc_garaged && (long)vehicle == kc_vehicle && !g_reset_req) return 0;
    return orig_pit_set(sys, vehicle, status);
}
static void kc_garage(void)
{
    void *sys = kc_pit_system;
    long v = kc_vehicle;
    if (!sys || v < 0 || !orig_pit_set) { LOG("keep race: can't send the wreck to the garage yet (pit system %s)", sys ? "seen" : "not seen"); return; }
    kc_own_call = 1;
    uint64_t r = orig_pit_set(sys, (uint64_t)v, 9);
    kc_own_call = 0;
    kc_garaged = 1;
    LOG("keep race: wreck (car %d) sent to the garage -> %d", (int)v, (int)r);
}

/* ---- events ---------------------------------------------------------------------------------------------- */
static void on_race_start(const SwoftyEvent *ev, void *u)
{
    (void)ev, (void)u;
    g_reset_req = 1;
}
static void on_session_end(const SwoftyEvent *ev, void *u)
{
    (void)u;
    /* the director must be off before the session is torn down (the game crashed on exit with it on) */
    spectate_off(ev && ev->name ? ev->name : "session end");
    g_in_race = 0;
    /* back to the menus: the AI drive state and its tag go too (the game gives the car back with the session) */
    ai_want = ai_on = ai_auto_done = 0, hb_left = 0;
    g_me = -1;
    cm_session_end();
}

/* the game hides its spectator HUD while the director follows a car that is not yours: keep it up
   (payload hud.spectator.visible v1: size, version, car index + handle, bool visible) */
typedef struct { uint32_t size, payload_version; uint32_t car; uint32_t pad; void *handle; uint8_t visible; } HudVisible;
static int on_hud_visible(void *user, SwoftyHookCall *call)
{
    (void)user;
    HudVisible *e = call ? (HudVisible *)call->payload : 0;
    if (e && watch_active && !g_aet && !e->visible) e->visible = 1;
    return 0;
}

static void replay_try_tick(void);
static void replays_tick(void);
static void cm_tick(void);
static int cm_level;
static void cm_write(const char *line);
static void replays_paint(SwoftyPaint *p, const SwoftyOverlay *o, float W, float H, float k);

/* ---- the frame tick (flow thread) -------------------------------------------------------------------------- */
static void frame(double dt, void *user)
{
    (void)dt, (void)user;
    static double last;
    double t = now();
    if (__atomic_exchange_n(&g_reset_req, 0, __ATOMIC_SEQ_CST)) {
        int was = watch_active;
        spectate_off("race (re)start");
        reset_race(was ? "race start, broadcast was on" : "race start");
        g_in_race = 1;
        g_race_t0 = t;
        feed("race_start", -1, "", "");
    }
    /* keys */
    if (pressed(S.ai_key, &k_ai)) {
        if (hb_left > 0) { hb_left = 0; LOG("key: hand-back cancelled"); }
        else if (ai_want) {
            if (S.handback <= 0) { ai_want = 0; LOG("key: AI drive off"); }
            else { hb_left = S.handback; hb_at = t + 1.0; LOG("key: AI drive off after a %d s countdown", S.handback);
                   char d[32]; fmt(d, sizeof d, "TAKE OVER IN %d", hb_left); toast("GET READY", d, 0); }
        } else {
            ai_want = 1;
            ai_lost_at = 0;
            LOG("key: AI drive on");
        }
    }
    if (pressed(S.watch_key, &k_watch)) {
        if (watch_active) { watch_user_off = 1; watch_manual = 0; spectate_off("key"); toast("BROADCAST OFF", "", 0); }
        else { watch_user_off = 0; watch_manual = 1; race_over = 0; LOG("key: broadcast on"); }
    }
    replay_try_tick();
    replays_tick();
    cm_tick();
    if (pressed(S.director_key, &k_dir)) {
        dir_on = !dir_on;
        LOG("key: auto director %s", dir_on ? "ON" : "OFF");
        toast(dir_on ? "AUTO DIRECTOR ON" : "AUTO DIRECTOR OFF", dir_on ? "THE CAMERA FOLLOWS THE ACTION" : "USE THE GAME'S KEYS", 0);
    }
    if (t - last < 0.1) return;
    double step = last ? t - last : 0.1;
    last = t;
    me_refresh();
    update_cars(step);
    /* hand-back countdown: the AI keeps driving until it reaches 0 */
    if (hb_left > 0 && t >= hb_at) {
        hb_left--;
        hb_at = t + 1.0;
        if (hb_left > 0) { char d[32]; fmt(d, sizeof d, "TAKE OVER IN %d", hb_left); toast("GET READY", d, 0); }
        else ai_want = 0;
    }
    /* AI from the start */
    if (S.ai_from_start && g_in_race && race_session() && !ai_auto_done && g_me >= 0 && t - g_race_t0 > 5.0 &&
        C[g_me].valid && C[g_me].speed > 20.0f) {
        ai_auto_done = 1;
        ai_want = 1;
        LOG("AI drive: from the start");
    }
    if (race_session()) ai_tick();
    /* your car is out */
    int me_out = g_me >= 0 && (kc_broken || (C[g_me].valid && (C[g_me].flags & (F_RETIRED | F_TERMINAL))));
    if (kc_broken && !kc_since) kc_since = t;
    if (kc_broken && !kc_garaged && S.garage_after > 0 && kc_since && t - kc_since >= S.garage_after) kc_garage();
    /* broadcast on or off */
    /* under a red flag the game runs its own screens and pit lane sequence: no broadcast camera, no director, no replays,
       no key presses until the cars are racing again */
    int want = g_in_race && !watch_user_off && !race_over && !g_redflag &&
               (watch_manual || (S.watch_with_ai && ai_on) || (S.watch_after_crash && me_out));
    if (want && !watch_active) spectate_on(watch_manual ? "key" : me_out ? "your car is out" : "AI is driving");
    else if (!want && watch_active) spectate_off(g_redflag ? "red flag" : ai_want ? "broadcast off" : "AI stopped / you race again");
    /* the session is gone when your car has no controller for 3 s: switch the director off before teardown */
    {
        static double gone_since;
        if (watch_active && g_me >= 0 && me_flags == 0xffffffffu) {
            if (!gone_since) gone_since = t;
            else if (t - gone_since > 3.0) { spectate_off("session over (your car has no controller)"); g_in_race = 0; gone_since = 0; }
        } else gone_since = 0;
    }
    if (watch_active && !g_paused) direct();
    aet_tick(t);
    gfx_tick(t);
    if (watch_active && !g_aet) hud_keep_up();
    else if (g_aet) hb_set(0);                      /* let the game hide its spectator HUD: Aeternum draws the graphics */
    publish();
}

/* ---- overlay (render thread) ----------------------------------------------------------------------------- */
static void paint(SwoftyPaint *p, void *user)
{
    (void)user;
    const SwoftyOverlay *o = ovl();
    if (!o) return;
    const Snap *s = &g_snap[__atomic_load_n(&g_snap_idx, __ATOMIC_ACQUIRE)];
    if (!s->show_any) return;
    float W = 1920, H = 1080;
    o->size(p, &W, &H);
    float k = H / 1080.0f;
    double t = now();
    replays_paint(p, o, W, H, k);
    /* tags, top right */
    float tx = W - 40 * k, ty = 36 * k;
    if (s->watching && !g_aet) {
        const char *tag = s->director ? "LIVE  |  AUTO DIRECTOR" : "LIVE";
        float tw = 0, th = 0;
        o->measure(p, 20 * k, tag, &tw, &th);
        o->rect(p, tx - tw - 28 * k, ty, tw + 28 * k, 34 * k, 0x101418d8);
        o->rect(p, tx - tw - 28 * k, ty, 6 * k, 34 * k, 0xe10600ff);
        o->text(p, tx - tw - 14 * k, ty + 7 * k, 20 * k, 0xffffffff, tag);
        ty += 42 * k;
    }
    if (s->countdown[0] || (s->ai && !g_aet)) {
        const char *tag = s->countdown[0] ? s->countdown : "AI DRIVING YOUR CAR";
        float tw = 0, th = 0;
        o->measure(p, 20 * k, tag, &tw, &th);
        o->rect(p, tx - tw - 28 * k, ty, tw + 28 * k, 34 * k, 0x101418d8);
        o->rect(p, tx - tw - 28 * k, ty, 6 * k, 34 * k, 0x00d2beff);
        o->text(p, tx - tw - 14 * k, ty + 7 * k, 20 * k, 0xffffffff, tag);
    }
    if (!s->watching) return;
    /* timing tower, top left */
    if (S.tower && s->n > 0) {
        float x = 40 * k, y = 120 * k, rh = 30 * k, w = 210 * k;
        o->rect(p, x, y - 40 * k, w, 40 * k, 0xe10600f0);
        o->text(p, x + 12 * k, y - 32 * k, 22 * k, 0xffffffff, "RACE");
        for (int i = 0; i < s->n; i++) {
            uint32_t bg = s->row[i].follow ? 0xffffffe8 : (i & 1) ? 0x15191ed8 : 0x101418d8;
            uint32_t fg = s->row[i].follow ? 0x101418ff : s->row[i].out ? 0x8a8f98ff : 0xffffffff;
            o->rect(p, x, y, w, rh, bg);
            if (s->row[i].mine) o->rect(p, x, y, 5 * k, rh, 0x00d2beff);
            char num[8], gap[16];
            fmt(num, sizeof num, "%d", s->row[i].pos);
            o->text(p, x + 12 * k, y + 5 * k, 19 * k, fg, num);
            o->text(p, x + 50 * k, y + 5 * k, 19 * k, fg, s->row[i].abbr);
            if (s->row[i].out) scopy(gap, sizeof gap, "OUT");
            else if (s->row[i].pit) scopy(gap, sizeof gap, "PIT");
            else if (s->row[i].pos == 1) scopy(gap, sizeof gap, "LEADER");
            else if (s->row[i].gap > 0 && s->row[i].gap < 60) fmt(gap, sizeof gap, "+%.1f", (double)s->row[i].gap);
            else gap[0] = 0;
            float gw = 0, gh = 0;
            o->measure(p, 17 * k, gap, &gw, &gh);
            o->text(p, x + w - gw - 12 * k, y + 6 * k, 17 * k, fg, gap);
            y += rh;
        }
    }
    /* lower third: why the camera is here */
    if (S.captions && s->cap_title[0] && t < s->cap_until) {
        float a = (float)(s->cap_until - t);
        float op = a > 0.5f ? 1.0f : a / 0.5f;
        uint32_t al = (uint32_t)(op * 230);
        float w1 = 0, h1 = 0, w2 = 0, h2 = 0;
        o->measure(p, 34 * k, s->cap_title, &w1, &h1);
        o->measure(p, 24 * k, s->cap_detail, &w2, &h2);
        float w = (w1 > w2 ? w1 : w2) + 60 * k;
        /* bottom right, above the game's button hints, clear of the timing tower */
        float x = W - w - 40 * k, y = H - 250 * k;
        o->rect(p, x, y, w, 52 * k, 0xe1060000 | al);
        o->text(p, x + 24 * k, y + 8 * k, 34 * k, 0xffffff00 | (uint32_t)(op * 255), s->cap_title);
        o->rect(p, x, y + 52 * k, w, 42 * k, 0x10141800 | al);
        o->text(p, x + 24 * k, y + 60 * k, 24 * k, 0xffffff00 | (uint32_t)(op * 255), s->cap_detail);
    }
}

#include "replay_probe.c"
#include "replay_try.c"
#include "replays.c"
#include "gfx.c"
#include "commentary.c"
#include "commentary_launch.c"

/* ---- load ---------------------------------------------------------------------------------------------------- */
static SwoftyMixin g_mixins[8];

__declspec(dllexport) int SwoftyAddonInit(const SwoftyApi *a)
{
    if (!a || a->version < 5 || !a->log) return 0;
    g_api = a;
    read_settings();
    LOG("v" ADDON_VERSION " loading: AI key %d, broadcast key %d, director key %d, hold %d s, camera %d", S.ai_key, S.watch_key,
        S.director_key, S.hold, S.cam_mode);
    if (OpenEventA(SYNCHRONIZE, 0, "kizi.ai-drive.active"))
        LOG("WARNING: AI Drive (Kizi) is loaded too: turn it off, both drive your car");
    static const char *ids[] = {"ai_key", "ai_from_start", "handback", "watch_key", "watch_with_ai", "watch_after_crash", "keep_race",
                                "garage_after", "director", "director_key", "hold", "camera", "own_bias", "captions", "tower", "toasts", "aeternum", "aeternum_gfx", "aeternum_layout",
                                "feed", "camera_key", "debug", "udp_port", "replays"};
    if (a->watch_setting)
        for (unsigned i = 0; i < sizeof ids / sizeof ids[0]; i++) a->watch_setting(ids[i], on_setting, 0);
    /* keep race: the same two wraps Spectator/Broadcast Mode uses */
    int nm = 0;
    if (a->resolve && a->resolve("Ne_Post_BusMessage")) {
        g_mixins[nm].target = "Ne_Post_BusMessage";
        g_mixins[nm].kind = SWOFTY_MIXIN_WRAP;
        g_mixins[nm].handler = (void *)wrap_bus_post;
        g_mixins[nm].orig = (void **)&orig_bus_post;
        nm++;
    } else LOG("keep race: Ne_Post_BusMessage not bound on this build, keep race is off");
    if (a->resolve && a->resolve("Game_Pitlane_SetVehicleStatus")) {
        g_mixins[nm].target = "Game_Pitlane_SetVehicleStatus";
        g_mixins[nm].kind = SWOFTY_MIXIN_WRAP;
        g_mixins[nm].handler = (void *)wrap_pit_set;
        g_mixins[nm].orig = (void **)&orig_pit_set;
        nm++;
    } else LOG("keep race: Game_Pitlane_SetVehicleStatus not bound, the wreck stays on track");
    nm = probe_mixins(g_mixins, nm);
    if (nm && a->register_mixins && !a->register_mixins(g_mixins, (size_t)nm)) LOG("mixins: register failed");
    probe_install();
    kc_find();
    reset_race("load");
    if (a->events && a->events->available && a->events->available()) {
        uint32_t tok = 0;
        a->events->subscribe(SWOFTY_EVENT_RACE_START, on_race_start, 0, &tok);
        a->events->subscribe(SWOFTY_EVENT_SESSION_START, on_race_start, 0, &tok);
        a->events->subscribe(SWOFTY_EVENT_SESSION_END, on_session_end, 0, &tok);
        a->events->subscribe(SWOFTY_EVENT_SESSION_UNLOAD, on_session_end, 0, &tok);
        int r = a->events->on_frame(frame, 0, &tok);
        LOG("frame tick %s", r == 0 ? "on" : "NOT on: the addon can't run");
    } else {
        LOG("events library not available: the addon can't run");
    }
    if (a->hooks && a->hooks->available && a->hooks->available() && a->hooks->attach) {
        uint32_t tok = 0;
        int r = a->hooks->attach("hud.spectator.visible", SWOFTY_PHASE_BEFORE, 0, on_hud_visible, 0, &tok);
        LOG("keep the spectator HUD up: %s", r == 0 ? "on" : "NOT on");
    }
    const SwoftyOverlay *o = ovl();
    if (o && o->available && o->available()) {
        uint32_t tok = 0;
        LOG("overlay %s", o->on_paint(paint, 0, &tok) == 0 ? "on" : "NOT on");
    } else {
        LOG("overlay library not available: no broadcast graphics");
    }
    {
        HANDLE th = CreateThread(0, 0, udp_main, 0, 0, 0);
        if (th) CloseHandle(th);
    }
    commentary_start();
    LOG("UDP port %d, session ops %s, HUD %s", g_udp_port, ops() ? "live" : "not live yet", hud() ? "ok" : "not yet");
    return 1;
}
