/* Race Broadcast - commentary, round 2: safety car phases, the pit cycle and the pit exit, the net race lead, touches
 * and damage, and the topic engine for quieter moments (weather, tyres, pace, strategy, the race so far, predictions,
 * teammates, drivers and circuit history, and short conversations between the team).
 * Included from commentary.c after the phrase banks (cm_phrases.h and the generated cm_phrases2.h). */

/* ---- lookups ------------------------------------------------------------------------------------------------------ */
/* a driver's surname as a key: "Pérez" -> "perez" (letters only, accents dropped) */
static void name_key(int car, char *out, size_t cap)
{
    char s[48];
    surname(car, s, sizeof s);
    size_t k = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p && k + 1 < cap; p++) {
        unsigned char c = *p;
        if (c == 0xC3 && p[1]) {
            unsigned char d = *++p, b = 0;
            if (d >= 0x80 && d <= 0x85) b = 'a'; else if (d >= 0xA0 && d <= 0xA5) b = 'a';
            else if (d == 0x87 || d == 0xA7) b = 'c';
            else if ((d >= 0x88 && d <= 0x8B) || (d >= 0xA8 && d <= 0xAB)) b = 'e';
            else if ((d >= 0x8C && d <= 0x8F) || (d >= 0xAC && d <= 0xAF)) b = 'i';
            else if (d == 0x91 || d == 0xB1) b = 'n';
            else if ((d >= 0x92 && d <= 0x96) || (d >= 0xB2 && d <= 0xB6) || d == 0x98 || d == 0xB8) b = 'o';
            else if ((d >= 0x99 && d <= 0x9C) || (d >= 0xB9 && d <= 0xBC)) b = 'u';
            if (b) out[k++] = (char)b;
            continue;
        }
        if (c >= 'A' && c <= 'Z') out[k++] = (char)(c + 32);
        else if (c >= 'a' && c <= 'z') out[k++] = (char)c;
    }
    out[k] = 0;
}
/* the circuit as a key, from the game's track id ("" if it has no history lines) */
static const char *track_key(void)
{
    switch ((int)U.track_id) {
    case 0: return "melbourne"; case 2: return "shanghai"; case 3: return "sakhir"; case 4: return "catalunya";
    case 5: return "monaco"; case 6: return "montreal"; case 7: return "silverstone"; case 9: return "hungaroring";
    case 10: return "spa"; case 11: return "monza"; case 12: return "singapore"; case 13: return "suzuka";
    case 14: return "abudhabi"; case 15: return "texas"; case 16: return "brazil"; case 17: return "austria";
    case 19: return "mexico"; case 20: return "baku"; case 26: return "zandvoort"; case 27: return "imola";
    case 29: return "jeddah"; case 30: return "miami"; case 31: return "lasvegas"; case 32: return "losail";
    default: return "";
    }
}
static Bank *bank_named(const char *name)
{
    for (size_t i = 0; i < sizeof cm_bank_index / sizeof cm_bank_index[0]; i++)
        if (seq(cm_bank_index[i].name, name)) return cm_bank_index[i].b;
    return 0;
}
static int is_car(int i) { return i >= 0 && i < g_n && C[i].valid; }

/* ---- per race memory -------------------------------------------------------------------------------------------- */
enum { T_TOUCH, T_WEATHER, T_TYRES, T_PACE, T_RECAP, T_PREDICT, T_STRAT, T_NET, T_TEAM, T_HISTORY, T_DRIVER, T_CONV, T_SC, T_DAMAGE, T_DAMAGE_EV, T_FINISH, T_N };
static struct {
    int laps_seen[NC], lap_ms[NC][4];    /* the last four lap times of each car */
    double touch_t[24]; int touch_n;      /* clean touches off camera (for the "scrappy midfield" summary) */
    long weather0, weather_last, weather_prev; double weather_change_t;
    int last_leader, lead_changes;
    int net_leader; double net_t;
    double topic_t[T_N];                   /* when each topic was last used */
    uint32_t used_hist;                   /* circuit / driver-at-circuit lines already used */
    uint8_t driver_done[NC];
    int pit_watch_car; double pit_watch_t;
    int exit_car; double exit_t;          /* the car that has just rejoined, for "ahead" / "behind" */
    double last_conv, sc_end_t;
} M2;
static void cm2_reset(void)
{
    memset(&M2, 0, sizeof M2);
    M2.weather0 = M2.weather_last = -1;
    M2.net_leader = -1, M2.pit_watch_car = -1, M2.exit_car = -1, M2.last_leader = -1;
}
/* lap times, weather changes: called from cm_tick */
static void cm2_track(double t)
{
    for (int i = 0; i < g_n; i++) {
        if (!C[i].valid) continue;
        int lm = U.car[i].last_lap_ms;
        if (lm > 0 && (M2.laps_seen[i] == 0 || M2.lap_ms[i][0] != lm)) {
            for (int k = 3; k > 0; k--) M2.lap_ms[i][k] = M2.lap_ms[i][k - 1];
            M2.lap_ms[i][0] = lm;
            M2.laps_seen[i]++;
        }
    }
    long w = U.weather;
    if (M2.weather0 < 0) M2.weather0 = M2.weather_last = w;
    if (w != M2.weather_last) M2.weather_prev = M2.weather_last, M2.weather_last = w, M2.weather_change_t = t;
    if (g_leader >= 0 && g_leader != M2.last_leader) { if (M2.last_leader >= 0 && cm_live) M2.lead_changes++; M2.last_leader = g_leader; }
}
/* the car's pace over the last laps: +1 quicker, -1 slower, 0 steady/unknown (ignores in- and out-laps) */
static int pace_trend(int i)
{
    if (M2.laps_seen[i] < 4) return 0;
    int a = M2.lap_ms[i][0], b = M2.lap_ms[i][1], c = M2.lap_ms[i][2], d = M2.lap_ms[i][3];
    if (C[i].pit_left > 0 && now() - C[i].pit_left < 200) return 0;   /* the in- and out-laps are in the last four */
    if (U.sc_status == 1 || U.sc_status == 2 || now() - M2.sc_end_t < 200) return 0;
    if (a < b - 250 && b < c + 150 && (a + b) < (c + d) - 500) return 1;
    if (a > b + 250 && b > c - 150 && (a + b) > (c + d) + 500) return -1;
    return 0;
}
/* rain falling, or the weather has changed in the last ten minutes */
static int changeable(double t) { return U.weather >= 3 || (M2.weather_change_t > 0 && t - M2.weather_change_t < 600); }

/* ---- conversations ---------------------------------------------------------------------------------------------- */
/* plays a set: each part answers the one before; the first part may name the car on camera */
static int conv_play(const CmConv *sets, int nsets, const Cx *x, int need_car)
{
    if (nsets <= 0) return 0;
    static uint32_t used[16];
    int pick = -1;
    for (int tries = 0; tries < 12 && pick < 0; tries++) {
        int k = rnd(nsets);
        uint32_t key = (uint32_t)(((uintptr_t)sets >> 4) + (uint32_t)k * 2654435761u);
        int seen = 0;
        for (int j = 0; j < 16; j++) if (used[j] == key) seen = 1;
        if (!seen || tries == 11) { pick = k; for (int j = 15; j > 0; j--) used[j] = used[j - 1]; used[0] = key; }
    }
    const CmConv *cv = &sets[pick];
    CmLine *l = 0;
    for (int p = 0; p < cv->n; p++) {
        int role = cv->role[p];   /* the voice that recorded it */
        l = p == 0 ? cm_add(role, 3, 12, cv->part[p], x) : cm_reply(l, role, 3, cv->part[p], x, p == 1 ? 0.35 : 0.4);
        if (!l) return p > 0;
        if (p == 0 && need_car >= 0) cm_need(l, need_car, -1, 1.5, 0, 0);
    }
    M2.last_conv = now();
    return 1;
}

/* ---- events ------------------------------------------------------------------------------------------------------- */
/* returns 1 if it handled the event */
static int cm2_event(const char *kind, int a, int b, int pos, const char *text, Cx *x, double t, int lv)
{
    CmLine *l = 0, *r = 0;
    if (seq(kind, "pit_cycle")) {
        /* a place changing hands in the stops: no overtake call; the strategy behind it */
        if (!is_car(a) || C[a].pos > (cm_long ? 10 : 6)) return 1;
        int lost = text && text[0] == 'l';
        if (lost && M2.exit_car == a && t - M2.exit_t < 30) {
            l = cm_add(R_LEAD, 4, 5, &L_pit_exit_behind, x);   /* "he's come out just behind" */
            cm_need(l, a, -1, 2.0, 0, 0);
            M2.exit_car = -1;
            return 1;
        }
        l = cm_add(R_LEAD, 4, 6, lost ? &L_pitcycle_lost : &L_pitcycle_gained, x);
        cm_need(l, a, -1, 2.5, 0, 0);
        if (l && chance(lv >= 2 ? 60 : 35)) {
            /* the strategist: the stop cost him the place (the undercut did not work), or it won it (undercut, or
               the overcut when the other car stopped first) */
            Bank *s = lost ? (chance(50) ? &S_pitcycle_lost : &S_undercut_fails)
                           : (is_car(b) && U.car[b].stops > U.car[a].stops - 1 && chance(50) ? &S_overcut : &S_undercut_works);
            cm_reply(l, R_STRAT, 3, s, x, 0.4);
        }
        return 1;
    }
    if (seq(kind, "touch")) {
        /* a clean touch on the car we are watching */
        if (t - cm_last_inc < 5) return 1;
        cm_last_inc = t;
        l = cm_add(R_LEAD, 4, 4, &L_touch_minor, x);
        cm_need(l, a, b, 1.5, 0, 0);
        if (l && lv >= 2 && chance(30)) cm_reply(l, R_EXP, 3, &E_touch_minor, x, 0.35);
        return 1;
    }
    if (seq(kind, "touch_off")) {
        /* a clean touch off camera: counted, and now and then a word about the scrappy midfield */
        if (M2.touch_n < 24) M2.touch_t[M2.touch_n++] = t;
        int recent = 0;
        for (int i = 0; i < M2.touch_n; i++) if (t - M2.touch_t[i] < 180) recent++;
        if (recent >= 4 && t - M2.topic_t[0] > 300 && cm_live) {
            M2.topic_t[0] = t, M2.touch_n = 0;
            cm_add(R_LEAD, 2, 10, &L_scrappy_midfield, x);
        }
        return 1;
    }
    if (seq(kind, "damage") && is_car(a)) {
        x->p = pos;
        l = cm_add(R_LEAD, 5, 8, &L_damage_spot, x);
        cm_need(l, a, -1, 3.0, &X_damage, x);   /* off camera: the short damage line */
        if (l && chance(70)) r = cm_reply(l, R_EXP, 4, &E_damage_impact, x, 0.35);
        if (r && chance(45)) cm_reply(r, R_STRAT, 3, &S_damage_pit, x, 0.4);
        if (!l) { l = cm_add(chance(50) ? R_EXP : R_LEAD, 5, 12, &X_damage, x); if (l) l->not_before = t + 1.5; }
        M2.topic_t[T_DAMAGE_EV] = t;
        return 1;
    }
    if (seq(kind, "pit_exit") && is_car(a)) {
        /* the car has just rejoined after the "will he come out in front?" moment: a rival close behind on the road
           means "he's out ahead" (coming out behind is the pit cycle's "lost" call) */
        if (M2.exit_car != a || t - M2.exit_t > 40) return 1;
        int behind = car_at(C[a].pos + 1);
        if (is_car(behind) && C[behind].gap_ahead > 0 && C[behind].gap_ahead < 1.5f && C[a].pos <= 10) {
            x->b = behind;
            l = cm_add(R_LEAD, 5, 4, &L_pit_exit_ahead, x);
            cm_need(l, a, -1, 1.5, 0, 0);
        }
        return 1;
    }
    if (seq(kind, "pit_leaving") && is_car(a)) {
        /* the stop is done and he's heading for the exit, on camera: the "will he come out in front?" moment */
        if (M2.pit_watch_car == a && t - M2.pit_watch_t < 30) return 1;
        M2.pit_watch_car = a, M2.pit_watch_t = t;
        M2.exit_car = a, M2.exit_t = t;
        l = cm_add(R_LEAD, 5, 4, &L_pit_exit_watch, x);
        cm_need(l, a, -1, 0.5, 0, 0);
        return 1;
    }
    return 0;
}

/* ---- the topic engine for quieter moments ------------------------------------------------------------------------ */

static int topic_ok(int k, double t, double gap) { return t - M2.topic_t[k] >= gap; }
static void topic_used(int k, double t) { M2.topic_t[k] = t; }

/* the net race lead: who leads once everyone has made the stops still to come (one stop assumed for a car that has
   not stopped in a dry race); the time lost in the pits is measured from the stops so far (22 s until then) */
static int net_leader(int *road)
{
    int lead = car_at(1);
    *road = lead;
    if (!is_car(lead)) return -1;
    double loss = 22.0, sum = 0; int n = 0;
    for (int i = 0; i < g_n; i++) if (C[i].valid && U.car[i].stops > 0 && C[i].pit_lane_max > 15000 && C[i].pit_lane_max < 45000) sum += C[i].pit_lane_max / 1000.0 - 4.0, n++;
    if (n) loss = sum / n;   /* time in the lane, less the time that part of the lap takes at racing speed */
    int wet = U.weather >= 3, best = -1; double bt = 1e9;
    for (int i = 0; i < g_n; i++) {
        if (!C[i].valid || C[i].pos > 8 || C[i].in_pit) continue;
        double gap = U.car[i].lead_ms / 1000.0;
        if (i == lead) gap = 0;
        int owes = !wet && U.car[i].stops == 0;
        double nt = gap + (owes ? loss : 0);
        if (nt < bt) bt = nt, best = i;
    }
    if (best >= 0 && best != lead && !(bt + 2.0 < loss)) return lead;   /* too close to call */
    return best;
}

/* returns 1 if it queued something */
static int cm2_idle(double t, int cam, Cx *x, int lv)
{
    int lap = g_leader >= 0 ? C[g_leader].lap : 0, total = (int)U.total_laps;
    double scale = lv >= 3 ? 0.7 : lv == 2 ? 1.0 : 1.8;   /* how often topics come round */
    CmLine *l = 0, *r = 0;
    char key[48], nm[64];

    /* under the safety car: its own talk, nothing about battles */
    if (U.sc_status == 1 || U.sc_status == 2) {
        if (!topic_ok(T_SC, t, 25 * scale)) return 1;
        topic_used(T_SC, t);
        int k = rnd(4);
        if (k == 0 && conv_play(CV_sc, (int)(sizeof CV_sc / sizeof CV_sc[0]), x, -1)) return 1;
        Bank *b = k == 1 ? &E_sc_period : k == 2 && cm_long ? &S_sc_period : k == 3 && cm_long && chance(40) ? &P_sc_pits : &L_sc_period;
        int role = b == &E_sc_period ? R_EXP : b == &S_sc_period ? R_STRAT : b == &P_sc_pits ? R_PIT : R_LEAD;
        cm_add(role, 3, 10, b, x);
        return 1;
    }
    if (!is_car(cam) || lap < 2) return 0;

    /* the net race lead, when it really differs from the road order */
    if (cm_long && total >= 15 && lap >= 6 && topic_ok(T_NET, t, 240 * scale)) {
        int road, net = net_leader(&road);
        if (is_car(net) && net != road && U.car[road].stops == 0 && U.car[net].stops > 0) {
            topic_used(T_NET, t);
            Cx y = *x; y.a = net; y.b = road;
            l = cm_add(R_LEAD, 3, 10, &L_net_lead, &y);
            r = l ? cm_reply(l, R_STRAT, 3, chance(70) ? &S_net_lead : &S_stops_offset, &y, 0.4) : cm_add(R_STRAT, 3, 10, &S_net_lead, &y);
            if (r && chance(35)) cm_reply(r, R_EXP, 2, &E_net_lead, &y, 0.4);
            return 1;
        }
    }
    /* teammates close together on the car on camera */
    int ahead = car_at(C[cam].pos - 1), behind = car_at(C[cam].pos + 1);
    int mate = -1;
    if (is_car(ahead) && U.car[ahead].team == U.car[cam].team && C[cam].gap_ahead < 1.5f) mate = ahead;
    else if (is_car(behind) && U.car[behind].team == U.car[cam].team && C[behind].gap_ahead < 1.5f) mate = behind;
    if (mate >= 0 && topic_ok(T_TEAM, t, 150 * scale)) {
        topic_used(T_TEAM, t);
        Cx y = *x; y.a = cam; y.b = mate;
        char k1[24], k2[24]; name_key(cam, k1, sizeof k1); name_key(mate, k2, sizeof k2);
        Bank *spec = 0;
        #define PAIR(p, q) ((seq(k1, p) && seq(k2, q)) || (seq(k1, q) && seq(k2, p)))
        if (PAIR("norris", "piastri")) spec = &E_team_mclaren;
        else if (PAIR("leclerc", "hamilton")) spec = &E_team_ferrari;
        else if (seq(k1, "alonso") || seq(k2, "alonso")) spec = &E_team_astonmartin_alonso;
        else if (PAIR("verstappen", "hadjar")) spec = &E_team_redbull;
        else if (PAIR("russell", "antonelli")) spec = &E_team_mercedes;
        #undef PAIR
        if (chance(40) && conv_play(CV_teammates, (int)(sizeof CV_teammates / sizeof CV_teammates[0]), &y, cam)) return 1;
        l = cm_add(R_LEAD, 3, 8, &L_team_battle, &y);
        cm_need(l, cam, -1, 1.5, 0, 0);
        if (l) r = cm_reply(l, R_EXP, 3, spec && chance(70) ? spec : chance(50) ? &E_team_orders : chance(60) ? &E_team_risk : &E_team_generic, &y, 0.4);
        if (r && U.car[cam].compound != U.car[mate].compound && chance(50)) cm_reply(r, R_STRAT, 2, &S_team_strategy, &y, 0.4);
        return 1;
    }
    /* history: this driver at this circuit, once per race each; then the circuit itself */
    const char *tk = track_key();
    if (tk[0] && topic_ok(T_HISTORY, t, 300 * scale) && lap >= 3) {
        name_key(cam, key, sizeof key);
        fmt(nm, sizeof nm, "XL_%s_%s", key, tk);
        Bank *xl = bank_named(nm);
        fmt(nm, sizeof nm, "XE_%s_%s", key, tk);
        Bank *xe = bank_named(nm);
        uint32_t bit = 1u << ((cam * 7 + (int)U.track_id) % 31);
        if ((xl || xe) && !(M2.used_hist & bit)) {
            M2.used_hist |= bit;
            topic_used(T_HISTORY, t);
            l = xl ? cm_add(R_LEAD, 3, 10, xl, x) : 0;
            if (l) cm_need(l, cam, -1, 1.5, 0, 0);
            if (xe) { if (l) cm_reply(l, R_EXP, 3, xe, x, 0.4); else cm_add(R_EXP, 3, 10, xe, x); }
            return 1;
        }
        if (!(M2.used_hist & 0x80000000u) || chance(25)) {
            fmt(nm, sizeof nm, "H_%s", tk);
            Bank *h = bank_named(nm);
            fmt(nm, sizeof nm, "HE_%s", tk);
            Bank *he = bank_named(nm);
            if (h) {
                M2.used_hist |= 0x80000000u;
                topic_used(T_HISTORY, t);
                l = cm_add(R_LEAD, 2, 10, h, x);
                if (l && he && chance(70)) cm_reply(l, R_EXP, 2, he, x, 0.4);
                return 1;
            }
        }
    }
    /* weather: much more often in changeable conditions (rain, or a change lately); an overcast sky now and then */
    int chg = changeable(t);
    if (topic_ok(T_WEATHER, t, (chg ? 90 : U.weather == 2 ? 240 : 420) * scale)) {
        topic_used(T_WEATHER, t);
        if (chance(45) && conv_play(chg ? CV_wx_changeable : CV_wx_stable,
                                    chg ? (int)(sizeof CV_wx_changeable / sizeof CV_wx_changeable[0]) : (int)(sizeof CV_wx_stable / sizeof CV_wx_stable[0]), x, -1)) return 1;
        Bank *b; int role;
        int drying = M2.weather_prev >= 3 && U.weather < M2.weather_prev && t - M2.weather_change_t < 900;
        if (drying) b = chance(50) ? &L_wx_drying : &S_wx_crossover;
        else if (U.weather >= 3) { int k = rnd(3); b = k == 0 ? &S_wx_crossover : k == 1 ? &S_wx_change_coming : &E_wx_rain_threat; }
        else if (chg || U.weather == 2) { int k = rnd(3); b = k == 0 ? &S_wx_change_coming : k == 1 ? &L_wx_rain_coming : &E_wx_rain_threat; }
        else { int k = rnd(4); b = k == 0 ? &E_wx_stable : k == 1 && U.track_temp >= 40 ? &L_wx_hot : k == 1 && U.track_temp > 0 && U.track_temp < 25 ? &L_wx_cool : &L_wx_stable; }
        role = b->name[0] == 'S' ? R_STRAT : b->name[0] == 'E' ? R_EXP : R_LEAD;
        cm_add(role, 2, 10, b, x);
        return 1;
    }
    /* damage on the car on camera: follow it up */
    {
        int d = U.car[cam].wing_fl > U.car[cam].wing_fr ? U.car[cam].wing_fl : U.car[cam].wing_fr;
        if (d >= 10 && topic_ok(T_DAMAGE, t, 120 * scale) && t - M2.topic_t[T_DAMAGE_EV] > 25) {
            topic_used(T_DAMAGE, t);
            if (chance(40) && conv_play(CV_damage_car, (int)(sizeof CV_damage_car / sizeof CV_damage_car[0]), x, cam)) return 1;
            int tr = pace_trend(cam);
            Bank *b = tr < 0 ? &L_damage_losing : tr > 0 || chance(50) ? &L_damage_holding : &E_damage_watch;
            l = cm_add(b == &E_damage_watch ? R_EXP : R_LEAD, 3, 8, b, x);
            cm_need(l, cam, -1, 1.5, 0, 0);
            return 1;
        }
    }
    /* pace: someone on the charge or fading */
    int tr = pace_trend(cam);
    if (tr != 0 && topic_ok(T_PACE, t, 120 * scale)) {
        topic_used(T_PACE, t);
        if (tr > 0 && chance(35) && conv_play(CV_pace_car, (int)(sizeof CV_pace_car / sizeof CV_pace_car[0]), x, cam)) return 1;
        l = cm_add(R_LEAD, 3, 8, tr > 0 ? &L_pace_up : &L_pace_down, x);
        cm_need(l, cam, -1, 1.5, 0, 0);
        if (l) cm_reply(l, tr < 0 && cm_long && chance(40) ? R_STRAT : R_EXP, 3, tr > 0 ? &E_pace_up : cm_long && chance(40) ? &S_pace_window : &E_pace_down, x, 0.4);
        return 1;
    }
    /* catching the car ahead: a prediction */
    if (is_car(ahead) && C[cam].gap_ahead > 1.2f && C[cam].gap_ahead < 6.0f && C[cam].closing < -0.08f && topic_ok(T_PREDICT, t, 150 * scale)) {
        topic_used(T_PREDICT, t);
        Cx y = *x; y.b = ahead;
        l = cm_add(R_LEAD, 3, 8, &L_predict_catch, &y);
        cm_need(l, cam, -1, 1.5, 0, 0);
        if (l && chance(55)) cm_reply(l, R_EXP, 3, &E_predict_catch, &y, 0.4);
        return 1;
    }
    /* tyres on the car on camera */
    if (U.car[cam].has_status && U.car[cam].tyre_age >= 6 && topic_ok(T_TYRES, t, 200 * scale)) {
        topic_used(T_TYRES, t);
        if (chance(40) && conv_play(CV_tyres_car, (int)(sizeof CV_tyres_car / sizeof CV_tyres_car[0]), x, cam)) return 1;
        int age = U.car[cam].tyre_age, wear = U.car[cam].wear;
        Bank *b; int role = R_STRAT;
        if (wear > 0 && wear < age) b = &S_tyre_conserving;                /* little wear for the laps done */
        else if (wear > age * 3 + 10) b = chance(60) ? &S_tyre_struggling : &E_tyre_deg_high, role = b == &E_tyre_deg_high ? R_EXP : R_STRAT;
        else if (age >= 20) b = &L_tyre_old, role = R_LEAD;
        else b = &E_tyre_managing, role = R_EXP;
        l = cm_add(role, 2, 8, b, x);
        cm_need(l, cam, -1, 1.5, 0, 0);
        return 1;
    }
    /* the driver on camera, once per race each */
    name_key(cam, key, sizeof key);
    if (!M2.driver_done[cam] && topic_ok(T_DRIVER, t, 150 * scale)) {
        fmt(nm, sizeof nm, "D_%s", key);
        Bank *d = bank_named(nm);
        fmt(nm, sizeof nm, "DE_%s", key);
        Bank *de = bank_named(nm);
        if (d || de) {
            M2.driver_done[cam] = 1;
            topic_used(T_DRIVER, t);
            if (chance(30) && conv_play(CV_driver_car, (int)(sizeof CV_driver_car / sizeof CV_driver_car[0]), x, cam)) return 1;
            l = d ? cm_add(R_LEAD, 2, 8, d, x) : 0;
            if (l) cm_need(l, cam, -1, 1.5, 0, 0);
            if (de) { if (l) cm_reply(l, R_EXP, 2, de, x, 0.4); else cm_add(R_EXP, 2, 8, de, x); }
            return 1;
        }
    }
    /* strategy talk, mid-race */
    if (cm_long && total > 0 && lap > total / 5 && lap < total * 3 / 4 && topic_ok(T_STRAT, t, 220 * scale)) {
        topic_used(T_STRAT, t);
        if (chance(50) && conv_play(CV_strategy, (int)(sizeof CV_strategy / sizeof CV_strategy[0]), x, -1)) return 1;
        int k = rnd(6);
        Bank *b = k == 0 ? &S_strat_onestop : k == 1 ? &S_strat_twostop : k == 2 ? &S_strat_split : k == 3 ? &S_tyre_offset : k == 4 ? &S_predict_stop : &S_stops_offset;
        l = cm_add(R_STRAT, 2, 10, b, x);
        if (l && chance(30)) cm_reply(l, R_EXP, 2, &E_strat_risky, x, 0.4);
        return 1;
    }
    /* the race so far, and how it might end */
    if (lap >= 5 && topic_ok(T_RECAP, t, 420 * scale)) {
        topic_used(T_RECAP, t);
        if (chance(40) && conv_play(CV_race_so_far, (int)(sizeof CV_race_so_far / sizeof CV_race_so_far[0]), x, -1)) return 1;
        int mover = -1, gain = 0;
        for (int i = 0; i < g_n; i++) if (C[i].valid && U.car[i].grid > 0 && U.car[i].grid - C[i].pos > gain) gain = U.car[i].grid - C[i].pos, mover = i;
        if (M2.lead_changes >= 1 && chance(40)) { Cx y = *x; y.a = car_at(1); l = cm_add(R_LEAD, 2, 10, &L_recap_leader_change, &y); if (l && chance(40)) cm_reply(l, R_EXP, 2, &E_recap_incident, &y, 0.4); }
        else if (mover >= 0 && gain >= 4 && chance(60)) { Cx y = *x; y.a = mover; cm_add(R_LEAD, 2, 10, &L_recap_mover, &y); }
        else cm_add(R_LEAD, 2, 10, chance(50) ? &L_recap_start : &L_recap_quiet, x);
        return 1;
    }
    if (total > 0 && lap >= total * 2 / 3 && topic_ok(T_FINISH, t, 400 * scale) && chance(30)) {
        topic_used(T_FINISH, t);
        if (chance(50) && conv_play(CV_predict, (int)(sizeof CV_predict / sizeof CV_predict[0]), x, -1)) return 1;
        cm_add(R_EXP, 2, 10, &E_predict_finish, x);
        return 1;
    }
    /* otherwise now and then a general conversation */
    if (topic_ok(T_CONV, t, 150 * scale) && t - M2.last_conv > 90 * scale && chance(lv >= 3 ? 60 : 40)) {
        topic_used(T_CONV, t);
        int k = rnd(6);
        int ok = 0;
        if (k == 0) ok = conv_play(CV_racecraft, (int)(sizeof CV_racecraft / sizeof CV_racecraft[0]), x, -1);
        else if (k == 1 && C[cam].pos == 1) ok = conv_play(CV_leader, (int)(sizeof CV_leader / sizeof CV_leader[0]), x, cam);
        else if (k == 2 && C[cam].pos > 6) ok = conv_play(CV_midfield, (int)(sizeof CV_midfield / sizeof CV_midfield[0]), x, -1);
        else if (k == 3) ok = conv_play(CV_tyres, (int)(sizeof CV_tyres / sizeof CV_tyres[0]), x, -1);
        else if (k == 4 && cm_long) ok = conv_play(CV_pit_lane, (int)(sizeof CV_pit_lane / sizeof CV_pit_lane[0]), x, -1);
        else ok = conv_play(CV_banter, (int)(sizeof CV_banter / sizeof CV_banter[0]), x, -1);
        if (ok) return 1;
    }
    return 0;
}
