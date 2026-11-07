/* Replays, the TV way (only while the auto director runs the broadcast).
 * The director keeps a list of replay-worthy moments (replay_candidate). The best recent one is offered on screen:
 * "REPLAY - <story> - press F9" (or played at once with the setting "Replays: automatic"). Playing it opens the game's own
 * Instant Replay through the pause menu (Esc, Enter: the menu opens on Instant Replay / Rewind), then the replay camera
 * is pointed at the cars of the story in turn (the game's Game_ReplayCamera_SetFocusVehicle, the call the replay screen
 * makes itself), and after the replay time the addon leaves with Esc and the race goes on. */

typedef void (*SetFocus_t)(void *cam, uint64_t car, uint64_t flag);

enum { RP_IDLE, RP_OFFER, RP_OPENING, RP_STARTING, RP_PLAYING, RP_LEAVING };
static int rp_state;
static int rp_cand = -1;                    /* index into g_rc */
static double rp_until, rp_t0, rp_next_focus, rp_last_done;
static int rp_focus_i, k_replay, rp_tries, rp_escs;
static double rp_step_t;
static char rp_title[48], rp_text[96];

static int rp_mode(void) { return replays_supported() ? S.replays : 0; }

/* the best moment of the last 25 s that has not been offered yet */
static int rp_pick(void)
{
    double t = now();
    int best = -1;
    int n = g_rc_n < 16 ? g_rc_n : 16;
    for (int i = 0; i < n; i++) {
        ReplayCand *r = &g_rc[i];
        if (r->importance <= 0 || t - r->t > 25 || r->t <= rp_last_done) continue;
        if (best < 0 || r->importance > g_rc[best].importance) best = i;
    }
    return best;
}

static void rp_focus(int car)
{
    void *cam = g_replay_cam;
    SetFocus_t f = (SetFocus_t)p_orig[P_FOCUS];
    if (!cam || !f || car < 0 || car > 21) { LOG("REPLAY: can't point the camera at car %d (camera %s)", car, cam ? "seen" : "not seen yet"); return; }
    /* flag 1 also switches the replay camera to the spectator preset (spectator_default_1, the broadcast-style camera);
       flag 0 keeps the camera the replay has, for the TV pod setting */
    f(cam, (uint64_t)car, S.cam_mode == 1 ? 0 : 1);
    LOG("REPLAY: camera on car %d (%s)", car, C[car].abbr);
}

static void replays_tick(void)
{
    double t = now();
    int mode = rp_mode();
    int key = pressed(0x78, &k_replay); /* F9 */
    switch (rp_state) {
    case RP_IDLE: {
        if (!mode || !watch_active || !(dir_on && S.director) || g_paused || g_redflag || t - rp_last_done < 20) return;
        int i = rp_pick();
        if (i < 0) return;
        ReplayCand *r = &g_rc[i];
        /* a moment needs a few seconds to happen before it is worth showing again */
        if (t - r->t < 4.0) return;
        rp_cand = i;
        scopy(rp_title, sizeof rp_title, seq(r->kind, "contact") ? "REPLAY: CONTACT" : seq(r->kind, "start") ? "REPLAY: THE START"
                                         : seq(r->kind, "restart") ? "REPLAY: THE RESTART" : "REPLAY: OVERTAKE");
        scopy(rp_text, sizeof rp_text, r->text);
        r->importance = -r->importance; /* offered once */
        if (mode == 2) {
            LOG("REPLAY: playing %s (%s)", rp_title, rp_text);
            rp_state = RP_OPENING;
            rp_t0 = t;
            rp_tries = 0;
            KeyStep q[2] = {{0x1B, 0, 0.05f}, {0x0D, 0, 1.4f}};
            ks_play(q, 2);
        } else {
            LOG("REPLAY: offered %s (%s), F9 to watch", rp_title, rp_text);
            feed("replay_offered", r->car, r->car >= 0 ? C[r->car].name : "", r->kind);
            rp_state = RP_OFFER;
            rp_until = t + 12.0;
        }
        return;
    }
    case RP_OFFER:
        if (g_redflag) { rp_state = RP_IDLE; rp_last_done = t; return; } /* never press keys into the red flag screens */
        if (key && !g_paused) {
            LOG("REPLAY: F9 pressed, playing %s", rp_title);
            rp_state = RP_OPENING;
            rp_t0 = t;
            rp_tries = 0;
            KeyStep q[2] = {{0x1B, 0, 0.05f}, {0x0D, 0, 1.4f}};
            ks_play(q, 2);
        } else if (t > rp_until || !watch_active) {
            rp_state = RP_IDLE;
            rp_last_done = t - 15; /* another offer may follow soon */
        }
        return;
    case RP_OPENING:
        if (g_in_replay) {
            /* the replay screen opens paused and ignores keys while it slides in: Enter (play) after 1.8 s,
               again if the replay system does not start playing (state 2) */
            rp_state = RP_STARTING;
            rp_t0 = t;
            rp_tries = 0;
            rp_step_t = t + 2.2;
            rp_focus_i = 0;
            LOG("REPLAY: the game's instant replay is open, pressing play in 2.2 s");
            fx_pos = 0;
            if (rp_cand >= 0 && g_rc[rp_cand].car2 >= 0) fx_car2 = g_rc[rp_cand].car2, fx_name2 = C[g_rc[rp_cand].car2].name;
            {
                char t2[128];
                fmt(t2, sizeof t2, "%s;%s", rp_cand >= 0 ? g_rc[rp_cand].kind : "", rp_text);
                feed("replay_start", rp_cand >= 0 ? g_rc[rp_cand].car : -1, rp_cand >= 0 && g_rc[rp_cand].car >= 0 ? C[g_rc[rp_cand].car].name : "", t2);
            }
        } else if (ks_i >= ks_n && t - rp_t0 > 2.6 + rp_tries * 1.4) {
            if (rp_tries < 3) {
                /* the menu sometimes ignores the first Enter while it is still sliding in */
                rp_tries++;
                LOG("REPLAY: replay not open yet, Enter again (%d)", rp_tries);
                KeyStep q[1] = {{0x0D, 0, 0.05f}};
                ks_play(q, 1);
            } else {
                LOG("REPLAY: instant replay did not open, closing the menu");
                KeyStep q[1] = {{0x1B, 0, 0.05f}};
                ks_play(q, 1);
                rp_state = RP_IDLE;
                rp_last_done = t;
            }
        }
        return;
    case RP_STARTING:
        /* one Enter only: a second one would pause it again if the first one worked */
        if (!rp_tries && t >= rp_step_t && ks_i >= ks_n) {
            rp_tries = 1;
            LOG("REPLAY: Enter to play (replay state before: %d)", g_replay_state);
            KeyStep q[2] = {{0x0D, 0, 0.05f}, {0x71, 0, 0.8f}}; /* Enter = play, F2 = hide the replay HUD */
            ks_play(q, 2);
            rp_step_t = t + 1.0;
        } else if (rp_tries && t >= rp_step_t) {
            LOG("REPLAY: replay state after play: %d", g_replay_state);
            rp_state = RP_PLAYING;
            rp_t0 = t;
            rp_next_focus = t + 0.2;
            feed("replay_playing", -1, "", "16");
        }
        if (!g_in_replay) rp_state = RP_IDLE, rp_last_done = t;
        return;
    case RP_PLAYING: {
        ReplayCand *r = rp_cand >= 0 ? &g_rc[rp_cand] : 0;
        if (r && t >= rp_next_focus) {
            /* contact: car A, car B, then A again; overtake: the attacker then the car passed; start: the leader */
            int cars[3] = {r->car, r->car2 >= 0 ? r->car2 : r->car, r->car};
            if (rp_focus_i < 3) {
                rp_focus(cars[rp_focus_i]);
                char vi[8];
                fmt(vi, sizeof vi, "%d", rp_focus_i);
                if (cars[rp_focus_i] >= 0) feed("replay_view", cars[rp_focus_i], C[cars[rp_focus_i]].name, vi);
            }
            rp_focus_i++;
            rp_next_focus = t + 5.0; /* long enough for a line from the expert on each view */
        }
        if (t - rp_t0 > 16.0 || !g_in_replay) {
            LOG("REPLAY: done, leaving with Esc");
            rp_state = RP_LEAVING;
            rp_t0 = t;
            rp_escs = 1;
            rp_step_t = t + 3.5; /* the replay closes with a short animation; the pause menu may come up after it */
            KeyStep q[1] = {{0x1B, 0, 0.05f}};
            ks_play(q, 1);
        }
        return;
    }
    case RP_LEAVING:
        /* after the animation: if the race clock is still stopped we are on the pause menu, Esc resumes; never press
           Esc while the race runs (that would open the pause menu) */
        if (t >= rp_step_t && ks_i >= ks_n) {
            if (g_clock_stopped && rp_escs < 3) {
                rp_escs++;
                LOG("REPLAY: still paused after the replay, Esc (%d)", rp_escs);
                KeyStep q[1] = {{0x1B, 0, 0.05f}};
                ks_play(q, 1);
                rp_step_t = t + 2.5;
            } else {
                g_in_replay = 0;
                rp_state = RP_IDLE;
                rp_last_done = t;
                LOG("REPLAY: back to the race%s", g_clock_stopped ? " (still paused, giving up)" : "");
                feed("replay_end", -1, "", "");
            }
        }
        return;
    }
}

/* drawn by the overlay: the offer */
static void replays_paint(SwoftyPaint *p, const SwoftyOverlay *o, float W, float H, float k)
{
    if (rp_state != RP_OFFER) return;
    double left = rp_until - now();
    if (left <= 0) return;
    char hint[48];
    fmt(hint, sizeof hint, "PRESS F9 TO WATCH  (%d)", (int)left + 1);
    float w1 = 0, h1 = 0, w2 = 0, h2 = 0, w3 = 0, h3 = 0;
    o->measure(p, 30 * k, rp_title, &w1, &h1);
    o->measure(p, 22 * k, rp_text, &w2, &h2);
    o->measure(p, 20 * k, hint, &w3, &h3);
    float w = w1;
    if (w2 > w) w = w2;
    if (w3 > w) w = w3;
    w += 56 * k;
    float x = W - w - 40 * k, y = 150 * k;
    o->rect(p, x, y, w, 46 * k, 0xffd400f0);
    o->text(p, x + 22 * k, y + 8 * k, 30 * k, 0x101418ff, rp_title);
    o->rect(p, x, y + 46 * k, w, 72 * k, 0x101418e6);
    o->text(p, x + 22 * k, y + 54 * k, 22 * k, 0xffffffff, rp_text);
    o->text(p, x + 22 * k, y + 86 * k, 20 * k, 0xffd400ff, hint);
}
