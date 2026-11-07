/* Race Broadcast - the graphics director for Project Aeternum.
 *
 * While Aeternum is running (and "Graphics: director runs Aeternum" is on), Race Broadcast decides which of Aeternum's
 * graphics are on screen. The Aeternum panel addon gives each graphic a hotkey that no keyboard has (Ctrl/Alt/Shift with
 * F13-F24, table below, same order as $rbActions in aeternum_panel.ps1); we press them with SendInput and Aeternum's
 * global hotkeys do the rest.
 *
 * - Overlays (0-23) only have a toggle key. A worker thread looks at Aeternum's overlay windows (title, visible) and
 *   presses a toggle only when an overlay's real state differs from what the director wants: it cannot drift.
 * - Broadcast cards (24-47) have separate show and hide keys, so pressing one twice does no harm.
 *
 * The leaderboard stays up, except while a focus card (driver card) is on. Everything else comes and goes with the story:
 * battle for, catching, driver card, pit lane, tyre life, car status, track map, fastest lap, weather, live speed. */

enum {
    G_LB_INITIALS, G_LB_FULL, G_SECTOR, G_MULTI_SECTOR, G_FASTEST_LAP, G_LAPTIME_GRAPH, G_FASTEST_SECTORS, G_DRIVER_INCIDENT,
    G_FASTEST_SECTOR_NOTE, G_LIVE_SPEED, G_DRIVER_INFO, G_GAP, G_TEAMMATE, G_TYRE_LIFE, G_CAR_STATUS, G_WEATHER, G_VWEATHER,
    G_PIT_LANE, G_ERS, G_RACE_DIRECTOR, G_DRIVER_INPUTS, G_TRACK_MAP, G_HALO, G_TRACK_MARSHAL, G_NTOGGLE,
    B_SELECT_CUR = 24, B_SELECT_AHEAD, B_SELECT_BEHIND, B_CLEAR_SEL, B_SHOW_BATTLE, B_CLEAR_BATTLE, B_SHOW_CARD, B_HIDE_CARD,
    B_SHOW_CATCHING, B_HIDE_CATCHING, B_SHOW_SECTOR_CMP, B_HIDE_SECTOR_CMP, B_SHOW_LAP_CMP, B_HIDE_LAP_CMP, B_SHOW_INPUT_CMP,
    B_HIDE_INPUT_CMP, B_SHOW_ERS_CMP, B_HIDE_ERS_CMP, B_SHOW_TRACK_DOM, B_HIDE_TRACK_DOM, B_SHOW_MULTI_CARD, B_CLEAR_MULTI_CARD,
    B_SHOW_LAPTIME, B_HIDE_LAPTIME, G_NKEYS
};
/* each overlay's window title, letters only, lower case ("Leaderboard · Initials" -> "leaderboardinitials") */
static const char *const g_titles[G_NTOGGLE] = {
    "leaderboardinitials", "leaderboardfullname", "sectorcurrentcar", "sectormulticars", "fastestlap", "laptimegraph",
    "fastestsectors", "driverincident", "fastestsectornotification", "livespeed", "driverinformation", "gapaheadbehind",
    "teammate", "tyrelife", "carstatus", "weather", "verticalweather", "pitlane", "ersbatterylevel", "racedirector",
    "driverinputs", "trackmapsectors", "halo", "trackmapmarshalzones"};

__declspec(dllimport) BOOL __stdcall EnumWindows(BOOL(__stdcall *)(HWND, long long), long long);
__declspec(dllimport) int __stdcall GetWindowTextW(HWND, uint16_t *, int);
__declspec(dllimport) BOOL __stdcall IsWindowVisible(HWND);
typedef struct { long left, top, right, bottom; } GxRect;
__declspec(dllimport) BOOL __stdcall GetWindowRect(HWND, GxRect *);
__declspec(dllimport) BOOL __stdcall SetWindowPos(HWND, HWND, int, int, int, int, unsigned);
__declspec(dllimport) int __stdcall GetSystemMetrics(int);

/* ---- the worker: presses keys, watches the overlay windows ---------------------------------------------- */
static volatile int gx_want[G_NTOGGLE];     /* -1 leave alone, 0 off, 1 on (written by the flow thread) */
static volatile int gx_vis[G_NTOGGLE];      /* what is on screen (written by the worker) */
static volatile int gx_seen;                /* the worker has found Aeternum's overlay windows at least once */
static volatile long gx_q[64], gx_qw, gx_qr; /* broadcast keys to press */
static double gx_last_press[G_NTOGGLE], gx_backoff[G_NTOGGLE];
static int gx_tries[G_NTOGGLE], gx_miss[G_NTOGGLE];
static DWORD gx_pid;
static int gx_tmp_vis[G_NTOGGLE];
static HWND gx_tmp_hwnd[G_NTOGGLE];
static int gx_thread_on;

static void gx_send(int i)
{
    static const uint8_t mods[4][3] = {{0x11, 0x12, 0x10}, {0x11, 0x10, 0}, {0x11, 0x12, 0}, {0x12, 0x10, 0}};
    int g = i / 12;
    uint16_t vk = (uint16_t)(0x7C + i % 12); /* VK_F13.. */
    KBINPUT in[8];
    int n = 0;
    memset(in, 0, sizeof in);
    for (int m = 0; m < 3 && mods[g][m]; m++) in[n].type = 1, in[n].wVk = mods[g][m], n++;
    in[n].type = 1, in[n].wVk = vk, n++;
    in[n].type = 1, in[n].wVk = vk, in[n].dwFlags = 0x0002, n++;
    for (int m = 2; m >= 0; m--) if (mods[g][m]) in[n].type = 1, in[n].wVk = mods[g][m], in[n].dwFlags = 0x0002, n++;
    SendInput((unsigned)n, in, sizeof in[0]);
}
static BOOL __stdcall gx_each(HWND h, long long l)
{
    (void)l;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != gx_pid || !IsWindowVisible(h)) return 1;
    uint16_t t[96];
    int n = GetWindowTextW(h, t, 96);
    char a[96];
    int k = 0;
    for (int i = 0; i < n && k < 95; i++) {
        uint16_t c = t[i];
        if (c >= 'A' && c <= 'Z') a[k++] = (char)(c + 32);
        else if (c >= 'a' && c <= 'z') a[k++] = (char)c;
    }
    a[k] = 0;
    for (int i = 0; i < G_NTOGGLE; i++) {
        const char *p = g_titles[i];
        int j = 0;
        while (p[j] && a[j] == p[j]) j++;
        if (!p[j]) { gx_tmp_vis[i] = 1; gx_tmp_hwnd[i] = h; break; } /* the title starts with the pattern */
    }
    return 1;
}
/* The director's layout: the car graphics that come and go stack down the right-hand side, top first in this order;
   Live Speed sits bottom centre. The leaderboard, track map and race control stay where the profile puts them. */
/* The director's layout: the car graphics that come and go stack up from the bottom right corner, the first one in the
   corner and the next above it, in this order. If the track map sits in that corner, the stack starts above it.
   The leaderboard, track map, race control and fastest-lap banner stay where the profile and Aeternum put them. */
static const int g_stack[] = {G_LIVE_SPEED, G_TYRE_LIFE, G_CAR_STATUS, G_PIT_LANE, G_WEATHER, G_VWEATHER, G_GAP, G_DRIVER_INFO,
                              G_TEAMMATE, G_ERS, G_SECTOR, G_MULTI_SECTOR, G_LAPTIME_GRAPH, G_FASTEST_SECTORS};
static void gx_place(HWND h, int x, int y)
{
    GxRect r;
    if (!GetWindowRect(h, &r)) return;
    int dx = x - (int)r.left, dy = y - (int)r.top;
    if (dx > 3 || dx < -3 || dy > 3 || dy < -3) SetWindowPos(h, 0, x, y, 0, 0, 0x0001 | 0x0004 | 0x0010); /* NOSIZE NOZORDER NOACTIVATE */
}
static void gx_layout(void)
{
    int W = GetSystemMetrics(0), H = GetSystemMetrics(1);
    if (W < 640 || H < 480) return;
    float k = (float)H / 1080.0f;
    int m = (int)(24 * k), top = (int)(110 * k), bottom = H - m, gap = (int)(10 * k);
    /* the track map in the bottom right corner: the stack starts above it */
    for (int i = G_TRACK_MAP; i <= G_TRACK_MARSHAL; i += G_TRACK_MARSHAL - G_TRACK_MAP) {
        GxRect r;
        if (gx_tmp_vis[i] && gx_tmp_hwnd[i] && GetWindowRect(gx_tmp_hwnd[i], &r) && r.right > W / 2 && r.bottom > H / 2 && (int)r.top - gap < bottom)
            bottom = (int)r.top - gap;
    }
    for (unsigned n = 0; n < sizeof g_stack / sizeof g_stack[0]; n++) {
        int i = g_stack[n];
        if (!gx_tmp_vis[i] || !gx_tmp_hwnd[i]) continue;
        GxRect r;
        if (!GetWindowRect(gx_tmp_hwnd[i], &r)) continue;
        int w = (int)(r.right - r.left), h = (int)(r.bottom - r.top);
        if (bottom - h < top) break;
        gx_place(gx_tmp_hwnd[i], W - w - m, bottom - h);
        bottom -= h + gap;
    }
    /* the leaderboard is not moved: it is made of several windows that Aeternum keeps lined up, so it is placed with
       Aeternum's own Edit layout */
}
static volatile int gx_layout_on;
static DWORD __stdcall gx_main(void *unused)
{
    (void)unused;
    for (;;) {
        /* broadcast keys first, one at a time */
        while (gx_qr != gx_qw) {
            gx_send((int)gx_q[gx_qr & 63]);
            gx_qr++;
            Sleep(140);
        }
        /* the overlays: compare with what the director wants */
        HWND cc = FindWindowW(0, (const uint16_t *)L"Project Aeternum Control Center");
        gx_pid = 0;
        if (cc) GetWindowThreadProcessId(cc, &gx_pid);
        if (gx_pid) {
            memset(gx_tmp_vis, 0, sizeof gx_tmp_vis);
            memset(gx_tmp_hwnd, 0, sizeof gx_tmp_hwnd);
            EnumWindows(gx_each, 0);
            if (gx_layout_on) gx_layout();
            int any = 0;
            for (int i = 0; i < G_NTOGGLE; i++) gx_vis[i] = gx_tmp_vis[i], any |= gx_tmp_vis[i];
            if (any) gx_seen = 1;
            double t = now();
            for (int i = 0; i < G_NTOGGLE; i++) {
                int w = gx_want[i];
                if (w < 0 || w == gx_vis[i]) { gx_tries[i] = 0; gx_miss[i] = 0; continue; }
                if (++gx_miss[i] < 3) continue; /* a window can blink while it redraws: be sure first */
                if (t < gx_backoff[i] || t - gx_last_press[i] < 1.5) continue;
                if (++gx_tries[i] > 3) { gx_backoff[i] = t + 30; gx_tries[i] = 0; g_api->log(TAG "graphics: %s does not respond, trying again in 30 s", g_titles[i]); continue; }
                gx_send(i);
                gx_last_press[i] = t;
                Sleep(140);
            }
        }
        Sleep(250);
    }
    return 0;
}
static void gx_key(int i)
{
    if (i < G_NTOGGLE || i >= G_NKEYS) return;
    if (gx_qw - gx_qr >= 60) return;
    gx_q[gx_qw & 63] = i;
    __atomic_store_n(&gx_qw, gx_qw + 1, __ATOMIC_RELEASE);
}

/* ---- the director's choices (flow thread) ------------------------------------------------------------------ */
static int gx_active;               /* we are running the graphics */
static int gx_lb = G_LB_INITIALS;   /* the leaderboard style: the one the profile showed */
static int gx_map = G_TRACK_MARSHAL;
static int gx_styles_set;
static int gx_card;                 /* broadcast card on screen: 0 none, B_SHOW_BATTLE, B_SHOW_CATCHING, B_SHOW_CARD */
static double gx_card_t, gx_card_until, gx_last_card, gx_calm_since;
static int gx_on_n;
static int gx_card_car = -1, gx_cut_car = -1;
static double gx_cut_t, gx_next;
static double gx_card_shown[NC];    /* when each driver last had the driver card */
static double gx_ftl_until, gx_weather_until, gx_map_until, gx_map_next, gx_tyre_next, gx_tyre_until, gx_dmg_until;
static long gx_weather_last = -1;
static int gx_sc_last;

static void gx_feed(const char *what, int car)
{
    if (car >= 0 && car < NC) { if (C[car].ahead_id >= 0) fx_car2 = C[car].ahead_id, fx_name2 = C[C[car].ahead_id].name; fx_pos = C[car].pos; }
    feed("graphic", car, car >= 0 && car < NC ? C[car].name : "", what);
}
static void gx_card_off(void)
{
    if (gx_card == B_SHOW_BATTLE) gx_key(B_CLEAR_BATTLE);
    else if (gx_card == B_SHOW_CATCHING) gx_key(B_HIDE_CATCHING);
    else if (gx_card == B_SHOW_CARD) gx_key(B_HIDE_CARD);
    if (gx_card) gx_key(B_CLEAR_SEL), gx_last_card = now();
    gx_card = 0, gx_card_car = -1;
}
static void gx_card_on(int card, int car, double secs, const char *what)
{
    gx_card_off();
    gx_key(B_CLEAR_SEL);
    gx_key(B_SELECT_CUR);
    if (card == B_SHOW_BATTLE) gx_key(B_SELECT_AHEAD);
    gx_key(card);
    gx_card = card, gx_card_car = car, gx_card_t = now(), gx_card_until = gx_card_t + secs;
    gx_on_n = 0;
    LOG("graphics: %s for %s", what, C[car].abbr);
    gx_feed(what, car);
}
static void gx_release(void)
{
    gx_card_off();
    for (int i = 0; i < G_NTOGGLE; i++) gx_want[i] = -1;
    gx_active = 0;
}
/* what is on screen while nothing special happens, before the director adds the story */
static void gx_base(int broadcast)
{
    for (int i = 0; i < G_NTOGGLE; i++) gx_want[i] = broadcast ? 0 : -1;
    gx_want[G_LB_INITIALS] = gx_want[G_LB_FULL] = -1; /* the leaderboard stays exactly as the profile has it */
    gx_want[G_TRACK_MAP] = gx_want[G_TRACK_MARSHAL] = 0; /* no track map while the director runs the graphics */
    if (broadcast) gx_want[G_DRIVER_INPUTS] = 0; /* the inputs HUD is for driving, not for the broadcast */
    gx_want[G_RACE_DIRECTOR] = 1;   /* race control messages: Aeternum shows them only when there is one */
    gx_want[G_DRIVER_INCIDENT] = 0;   /* Aeternum's incident pop-up: off (it showed error text under the names) */
}

static void gfx_tick(double t)
{
    int want_on = g_aet && S.aeternum_gfx && g_in_race;
    if (want_on && !gx_thread_on) {
        gx_thread_on = 1;
        for (int i = 0; i < G_NTOGGLE; i++) gx_want[i] = -1;
        CreateThread(0, 0, gx_main, 0, 0, 0);
    }
    gx_layout_on = want_on && S.aeternum_layout;
    if (!want_on) { if (gx_active) { gx_release(); LOG("graphics: handed back to Aeternum"); } return; }
    if (t < gx_next) return;
    gx_next = t + 0.25;
    if (!gx_seen) return; /* Aeternum's overlay windows not found yet */
    if (!gx_styles_set) {
        /* keep the leaderboard and track map styles the profile uses */
        gx_styles_set = 1;
        if (gx_vis[G_LB_FULL] && !gx_vis[G_LB_INITIALS]) gx_lb = G_LB_FULL;
        if (gx_vis[G_TRACK_MAP] && !gx_vis[G_TRACK_MARSHAL]) gx_map = G_TRACK_MAP;
        LOG("graphics: the director runs Aeternum's graphics (leaderboard %s, track map %s)", g_titles[gx_lb], g_titles[gx_map]);
    }
    if (!gx_active) { gx_active = 1; gx_map_next = t + 240; gx_tyre_next = t + 360; }

    /* replays: a clean picture */
    if (rp_state >= RP_OPENING) {
        gx_card_off();
        for (int i = 0; i < G_NTOGGLE; i++) gx_want[i] = 0;
        gx_want[G_LB_INITIALS] = gx_want[G_LB_FULL] = -1;
        return;
    }
    gx_base(watch_active);
    if (!watch_active) { gx_card_off(); return; } /* you drive: your own overlays stay as they are */

    int cur = cur_follow;
    if (cur != gx_cut_car) {                            /* a cut: car graphics go with the old car */
        if (gx_card && gx_card_car != cur) gx_card_off();
        gx_cut_car = cur, gx_cut_t = t;
    }
    double held = t - gx_cut_t;
    Car *c = cur >= 0 && cur < g_n ? &C[cur] : 0;

    /* events that bring something up for a while */
    long sc = U.sc_status;
    if (sc != gx_sc_last) { if (sc == 1 || sc == 2) gx_map_until = t + 20; if (sc == 3) gx_weather_until = t + 14; gx_sc_last = (int)sc; }
    if (U.weather != gx_weather_last) { if (gx_weather_last >= 0) { gx_weather_until = t + 12; gx_feed("weather", -1); } gx_weather_last = U.weather; }

    /* the race picture */
    int start_lap = g_phase_lap >= 0;
    (void)start_lap; /* the track map is not the director's: it stays as the profile has it */
    if (t < gx_weather_until) gx_want[G_WEATHER] = 1;
    if (t < gx_ftl_until) gx_want[G_FASTEST_LAP] = 1;

    if (c) {
        const char *k = c->why_kind;
        int incident = seq(k, "spin") || seq(k, "stopped") || seq(k, "contact") || seq(k, "out");
        if (cur == g_me && g_me >= 0) gx_want[G_LIVE_SPEED] = 1;
        /* pit stops */
        if (c->in_pit && c->racing == 0 && !c->was_out) { gx_want[G_PIT_LANE] = 1; if (held > 1 && held < 1.4) gx_feed("pit_lane", cur); }
        if (c->pit_out_at > 0 && t > c->pit_out_at && t < c->pit_out_at + 14) gx_want[G_TYRE_LIFE] = 1;
        /* a tyre check on the leader now and then, later in the race */
        if (t > gx_tyre_next && c->pos <= 3 && held > 4 && !gx_card) { gx_tyre_until = t + 12; gx_tyre_next = t + 420; gx_feed("tyre_life", cur); }
        if (t < gx_tyre_until) gx_want[G_TYRE_LIFE] = 1;
        /* damage on the car we are looking at */
        int dmg = U.car[cur].wing_fl > U.car[cur].wing_fr ? U.car[cur].wing_fl : U.car[cur].wing_fr;
        if (U.car[cur].wing_r > dmg) dmg = U.car[cur].wing_r;
        if (dmg >= 10 && held > 1.5 && held < 2.0 && t > gx_dmg_until) { gx_dmg_until = t + 12; gx_feed("car_status", cur); }
        if (dmg >= 10 && t < gx_dmg_until) gx_want[G_CAR_STATUS] = 1;

        /* broadcast cards: one at a time, a breather between them */
        /* the battle card stays at least 8 s, and goes only once the fight has been over for 3 s */
        if (gx_card == B_SHOW_BATTLE) {
            int fight = c->ahead_id >= 0 && c->gap_ahead > 0 && c->gap_ahead < 2.0f;
            /* a long fight: tell the commentary it is still going (after 20 s, then 45 s) */
            if (fight && gx_card_car == cur && ((gx_on_n == 0 && t - gx_card_t > 20) || (gx_on_n == 1 && t - gx_card_t > 45))) gx_on_n++, gx_feed("battle_on", cur);
            if (fight) gx_calm_since = 0; else if (!gx_calm_since) gx_calm_since = t;
            if (t - gx_card_t > 8 && ((gx_calm_since && t - gx_calm_since > 3) || t > gx_card_until)) gx_card_off();
        } else if (gx_card && t > gx_card_until) gx_card_off();
        if (!gx_card && t - gx_last_card > 6 && !incident) {
            if (seq(k, "battle") && c->ahead_id >= 0 && held > 1.0 && held < 6)
                gx_card_on(B_SHOW_BATTLE, cur, 60, "battle_for"), gx_calm_since = 0;
            else if (!seq(k, "battle") && c->pos > 1 && c->gap_ahead > 1.2f && c->gap_ahead < 3.5f && c->closing < -0.05f && held > 2 && held < 8)
                gx_card_on(B_SHOW_CATCHING, cur, 10, "catching");
            else if (held > 2.5 && held < 6 && t - gx_card_shown[cur] > 300 && t - gx_last_card > 20) {
                gx_card_shown[cur] = t;
                gx_card_on(B_SHOW_CARD, cur, 7, "driver_card");
            }
        }
    }
}
static void gfx_fastest_lap(void) { gx_ftl_until = now() + 10; }
static void gfx_reset(void)
{
    gx_card_off();
    gx_cut_car = -1, gx_map_until = gx_weather_until = gx_ftl_until = gx_tyre_until = gx_dmg_until = 0;
    for (int i = 0; i < NC; i++) gx_card_shown[i] = 0;
}
