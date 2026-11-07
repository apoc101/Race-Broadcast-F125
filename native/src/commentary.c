/* Commentary: the brain. It lives in the addon, so it knows what the camera shows, the race order, tyres and gaps.
 * Four original characters (invented names, British voices picked in a voice audition):
 *   Ben Archer (lead)       calls the action, leads the broadcast, hands over to the others
 *   Graham Lowe (expert)    ex-driver view: racecraft, replays, the odd strategic point
 *   Tom Kirby (pit lane)    pit stops of the front runners (races of 25% distance or more)
 *   Erin Doyle (strategy)   tyres and strategy, penalties and rules (races of 25% distance or more)
 * Rules of the broadcast:
 *   - silent until the lights go out (a short tyre preview on the formation lap in longer races);
 *   - lines about a car wait until the camera shows it (and a moment longer, so the viewer sees it first); something big
 *     that happens off camera is called as "elsewhere"; small things off camera are dropped;
 *   - an airtime budget per level (quiet / normal / chatty), gaps between speakers, nothing repeated within a race;
 *   - during a replay they talk over what is shown, view by view, then hand back to the live race.
 * The voice side (commentary\race_broadcast_commentary.ps1, started hidden by the addon) only speaks what it is told:
 *   addon -> say.txt   "id|say|voice|text", "id|prep|voice|text" (render now, say later), "0|cut||", "0|warm|voices|"
 *   voice -> said.txt  "id|start|seconds", "id|end", "0|status|text" */

__declspec(dllimport) BOOL __stdcall ReadFile(HANDLE, void *, DWORD, DWORD *, void *);
__declspec(dllimport) BOOL __stdcall SetFilePointerEx(HANDLE, int64_t, int64_t *, DWORD);
__declspec(dllimport) BOOL __stdcall GetFileSizeEx(HANDLE, int64_t *);

enum { R_LEAD, R_EXP, R_PIT, R_STRAT };
static const char *cm_voice[4] = {"lead", "expert", "pit", "strategy"};
static const char *cm_first[4] = {"Ben", "Graham", "Tom", "Erin"};
static const char *cm_tag[4] = {"BEN   ", "GRAHAM", "TOM   ", "ERIN  "};

static int cm_level;       /* 0 off, 1 quiet, 2 normal, 3 chatty */
static int cm_live;        /* the lights have gone out */
static double cm_last_inc; /* the last overtake or contact call */
static char cm_call[300];  /* the lights-out call, rendered in advance */
static char cm_call_rec[120];
static int cm_long;        /* race of 25% distance or more: all four voices */
static int cm_long_known;
static int cm_preview_done, cm_finished, cm_strat_pic_done;
static double cm_shot_t;   /* when the camera last changed car */
static int cm_shot_car = -1;
static int cm_replay_car = -1; /* the car the replay camera shows */
static char cm_replay_kind[16];
static int cm_replay_a = -1, cm_replay_b = -1;

/* ---- random ------------------------------------------------------------------------------------------------- */
static uint64_t cm_seed;
static uint32_t rnd32(void)
{
    if (!cm_seed) { int64_t c; QueryPerformanceCounter(&c); cm_seed = (uint64_t)c * 6364136223846793005ull + 1442695040888963407ull; }
    cm_seed ^= cm_seed << 13, cm_seed ^= cm_seed >> 7, cm_seed ^= cm_seed << 17;
    return (uint32_t)(cm_seed >> 16);
}
static int rnd(int n) { return n > 0 ? (int)(rnd32() % (uint32_t)n) : 0; }
static int chance(int pct) { return rnd(100) < pct; }

/* ---- phrase banks: every line is used once per race before any repeats ------------------------------------- */
typedef struct { const char *const *s; int n; uint32_t used[8]; int last; const char *name; } Bank;
#define BANK(name, ...)                                                                                                \
    static const char *const name##_s[] = {__VA_ARGS__};                                                               \
    static Bank name = {name##_s, (int)(sizeof name##_s / sizeof name##_s[0]), {0}, -1, #name};
#include "cm_phrases.h"
#include "cm_phrases2.h" /* round 2: new banks, conversations, history lookup (generated) */

static const char *bank_pick(Bank *b)
{
    if (!b || b->n <= 0) return "";
    int n = b->n > 256 ? 256 : b->n, free_n = 0;
    for (int i = 0; i < n; i++) if (!(b->used[i >> 5] & 1u << (i & 31))) free_n++;
    if (!free_n) { memset(b->used, 0, sizeof b->used); free_n = n; if (b->last >= 0 && n > 1) b->used[b->last >> 5] |= 1u << (b->last & 31), free_n--; }
    int k = rnd(free_n);
    for (int i = 0; i < n; i++)
        if (!(b->used[i >> 5] & 1u << (i & 31)) && k-- == 0) { b->used[i >> 5] |= 1u << (i & 31); b->last = i; return b->s[i]; }
    return b->s[0];
}

/* ---- words ------------------------------------------------------------------------------------------------------ */
static const char *cm_ord[] = {"", "first", "second", "third", "fourth", "fifth", "sixth", "seventh", "eighth", "ninth", "tenth",
                               "eleventh", "twelfth", "thirteenth", "fourteenth", "fifteenth", "sixteenth", "seventeenth",
                               "eighteenth", "nineteenth", "twentieth", "twenty first", "twenty second"};
static const char *cm_num[] = {"no", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten", "eleven", "twelve",
                               "thirteen", "fourteen", "fifteen", "sixteen", "seventeen", "eighteen", "nineteen", "twenty"};
static const char *team_name(int t)
{
    static const char *n[] = {"Mercedes", "Ferrari", "Red Bull", "Williams", "Aston Martin", "Alpine", "Racing Bulls", "Haas", "McLaren", "Sauber"};
    return t >= 0 && t < 10 ? n[t] : "the team";
}
static const char *track_name(int t)
{
    switch (t) {
    case 0: return "Melbourne"; case 2: return "Shanghai"; case 3: return "Bahrain"; case 4: return "Barcelona"; case 5: return "Monaco";
    case 6: return "Montreal"; case 7: case 39: return "Silverstone"; case 9: return "the Hungaroring"; case 10: return "Spa";
    case 11: return "Monza"; case 12: return "Singapore"; case 13: return "Suzuka"; case 14: return "Abu Dhabi"; case 15: return "Austin";
    case 16: return "Interlagos"; case 17: case 40: return "the Red Bull Ring"; case 19: return "Mexico City"; case 20: return "Baku";
    case 26: case 41: return "Zandvoort"; case 27: return "Imola"; case 29: return "Jeddah"; case 30: return "Miami";
    case 31: return "Las Vegas"; case 32: return "Qatar";
    }
    return "the circuit";
}
static const char *tyre_name(int c, int plural)
{
    switch (c) {
    case 16: return plural ? "softs" : "soft"; case 17: return plural ? "mediums" : "medium"; case 18: return plural ? "hards" : "hard";
    case 7: return plural ? "intermediates" : "intermediate"; case 8: return plural ? "full wets" : "full wet";
    }
    return plural ? "tyres" : "tyre";
}
static const char *weather_words(int w)
{
    static const char *n[] = {"clear skies", "a bit of cloud about", "overcast skies", "light rain falling", "heavy rain", "a storm overhead"};
    return w >= 0 && w <= 5 ? n[w] : "dry conditions";
}
static void surname(int car, char *out, size_t cap)
{
    if (car < 0 || car >= NC || !C[car].name[0]) { scopy(out, cap, "the car"); return; }
    const char *s = C[car].name, *w = s;
    for (const char *p = s; *p; p++) if (*p == ' ' && p[1]) w = p + 1;
    size_t i = 0;
    int first = 1;
    for (; w[i] && i + 1 < cap; i++) {
        char ch = w[i];
        if (!first && i > 0 && (unsigned char)w[i - 1] == 0xC3 && (unsigned char)ch >= 0x80 && (unsigned char)ch <= 0x9E && (unsigned char)ch != 0x97)
            ch = (char)(ch + 0x20); /* accented capital (Perez with an accent) */
        if (!first && ch >= 'A' && ch <= 'Z') ch = (char)(ch + 32);
        if (first && ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
        first = ch == '-' || ch == '\'';
        out[i] = ch;
    }
    out[i] = 0;
    if (!i) scopy(out, cap, "the car");
}
static void gap_words(float g, char *out, size_t cap)
{
    if (g <= 0.0f) scopy(out, cap, "nothing");
    else if (g < 0.15f) scopy(out, cap, "nothing at all");
    else if (g < 0.35f) scopy(out, cap, "a couple of tenths");
    else if (g < 0.65f) scopy(out, cap, "half a second");
    else if (g < 0.95f) scopy(out, cap, "under a second");
    else if (g < 1.5f) scopy(out, cap, "just over a second");
    else if (g < 9.95f) fmt(out, cap, "%.1f seconds", (double)g);
    else if (g < 60.0f) fmt(out, cap, "%d seconds", (int)(g + 0.5f));
    else scopy(out, cap, "over a minute");
}
static int car_at(int pos)
{
    for (int i = 0; i < g_n; i++) if (C[i].valid && C[i].pos == pos) return i;
    return -1;
}
static int laps_to_go(void)
{
    int total = (int)U.total_laps, ll = g_leader >= 0 ? C[g_leader].lap : 0;
    return total > 0 && ll > 0 ? total - ll + 1 : 0;
}
static float gap_behind(int car)
{
    int b = car >= 0 ? car_at(C[car].pos + 1) : -1;
    return b >= 0 ? C[b].gap_ahead : 0;
}
static float gap_leader(int car) { return car >= 0 && car < NC && C[car].pos > 1 ? (float)U.car[car].lead_ms / 1000.0f : 0; }

/* ---- expanding a line: {A} {B} {C} names, {TA} {TB} teams, {P} ordinal, {PN} P-number, {N} number, {NW} number in words,
   {X} {Y} free text, {LAPS} laps to go, {LAP} lap, {TOT} total laps, {TRK} track, {CA} {CB} tyres (plural), {CA1} {CB1}
   (singular), {AGEA} {AGEB} tyre age, {GA} gap ahead of A, {GB} gap behind A, {GL} gap of A to the leader,
   {L} {E} {PT} {ST} first names, {WX} weather */
typedef struct { int a, b, c, p, n; const char *x, *y; } Cx;
static void expand(const char *t, const Cx *x, char *out, size_t cap)
{
    size_t o = 0;
    char v[96], key[8];
    while (*t && o + 1 < cap) {
        if (*t != '{') { out[o++] = *t++; continue; }
        const char *e = t + 1;
        int k = 0;
        while (*e && *e != '}' && k < 7) key[k++] = *e++;
        key[k] = 0;
        if (*e != '}') { out[o++] = *t++; continue; }
        t = e + 1;
        v[0] = 0;
        int a = x ? x->a : -1, b = x ? x->b : -1;
        if (seq(key, "A")) surname(a, v, sizeof v);
        else if (seq(key, "B")) surname(b, v, sizeof v);
        else if (seq(key, "C")) surname(x ? x->c : -1, v, sizeof v);
        else if (seq(key, "TA")) scopy(v, sizeof v, a >= 0 && a < NC ? team_name(U.car[a].team) : "the team");
        else if (seq(key, "TB")) scopy(v, sizeof v, b >= 0 && b < NC ? team_name(U.car[b].team) : "the team");
        else if (seq(key, "P")) scopy(v, sizeof v, x && x->p > 0 && x->p <= 22 ? cm_ord[x->p] : "the points");
        else if (seq(key, "PN")) fmt(v, sizeof v, "P%d", x ? x->p : 0);
        else if (seq(key, "N")) fmt(v, sizeof v, "%d", x ? x->n : 0);
        else if (seq(key, "NW")) { int n = x ? x->n : 0; if (n >= 0 && n <= 20) scopy(v, sizeof v, cm_num[n]); else fmt(v, sizeof v, "%d", n); }
        else if (seq(key, "X")) scopy(v, sizeof v, x && x->x ? x->x : "");
        else if (seq(key, "Y")) scopy(v, sizeof v, x && x->y ? x->y : "");
        else if (seq(key, "LAPS")) { int n = laps_to_go(); if (n >= 0 && n <= 20) scopy(v, sizeof v, cm_num[n]); else fmt(v, sizeof v, "%d", n); }
        else if (seq(key, "LAP")) fmt(v, sizeof v, "%d", g_leader >= 0 ? C[g_leader].lap : 1);
        else if (seq(key, "TOT")) fmt(v, sizeof v, "%d", (int)U.total_laps);
        else if (seq(key, "TRK")) scopy(v, sizeof v, track_name((int)U.track_id));
        else if (seq(key, "CA")) scopy(v, sizeof v, a >= 0 && a < NC ? tyre_name(U.car[a].compound, 1) : "tyres");
        else if (seq(key, "CB")) scopy(v, sizeof v, b >= 0 && b < NC ? tyre_name(U.car[b].compound, 1) : "tyres");
        else if (seq(key, "CA1")) scopy(v, sizeof v, a >= 0 && a < NC ? tyre_name(U.car[a].compound, 0) : "tyre");
        else if (seq(key, "CB1")) scopy(v, sizeof v, b >= 0 && b < NC ? tyre_name(U.car[b].compound, 0) : "tyre");
        else if (seq(key, "AGEA")) fmt(v, sizeof v, "%d", a >= 0 && a < NC ? U.car[a].tyre_age : 0);
        else if (seq(key, "AGEB")) fmt(v, sizeof v, "%d", b >= 0 && b < NC ? U.car[b].tyre_age : 0);
        else if (seq(key, "GA")) gap_words(a >= 0 ? C[a].gap_ahead : 0, v, sizeof v);
        else if (seq(key, "GB")) gap_words(gap_behind(a), v, sizeof v);
        else if (seq(key, "GL")) gap_words(gap_leader(a), v, sizeof v);
        else if (seq(key, "L")) scopy(v, sizeof v, cm_first[R_LEAD]);
        else if (seq(key, "E")) scopy(v, sizeof v, cm_first[R_EXP]);
        else if (seq(key, "PT")) scopy(v, sizeof v, cm_first[R_PIT]);
        else if (seq(key, "ST")) scopy(v, sizeof v, cm_first[R_STRAT]);
        else if (seq(key, "WX")) scopy(v, sizeof v, weather_words((int)U.weather));
        for (int i = 0; v[i] && o + 1 < cap; i++) out[o++] = v[i];
    }
    out[o] = 0;
    /* capital letters at the start of each sentence */
    for (size_t i = 0; out[i]; i++)
        if (out[i] >= 'a' && out[i] <= 'z' && (i == 0 || (i >= 2 && out[i - 1] == ' ' && (out[i - 2] == '.' || out[i - 2] == '!' || out[i - 2] == '?'))))
            out[i] = (char)(out[i] - 32);
}

/* ---- the queue ---------------------------------------------------------------------------------------------- */
typedef struct {
    int used, id, role, prio;
    char text[300], alt[300];
    double t, until, need_by, not_before, delay;
    int need, need2;  /* a car that must be on screen (either), -1 none */
    int after;        /* the line this one answers, 0 none */
    int replay;       /* 1 only in a replay, 0 only live, -1 any */
    int pre;          /* may be said before the lights go out */
    int pair1;        /* 1 + the index of the two-car story it is about, 0 none */
    char rec[120], alt_rec[120]; /* the recorded clip: bank;index;name A;name B;name C (see cm_rec) */
} CmLine;
#define CMQ 32
static CmLine cmq[CMQ];
static int cm_next_id = 1;
static int cm_cur_id, cm_cur_role = -1, cm_cur_prio;
static double cm_cur_sent, cm_cur_until, cm_last_end, cm_air;
static int cm_last_role = -1;
static struct { int id; double t; } cm_ended[16];
static int cm_ended_n;
static double cm_last_any, cm_last_idle, cm_last_exp_cut, cm_last_strat, cm_last_lap_line;
static int cm_mouth_seen;

static HANDLE cm_out = 0, cm_in = 0;
static int64_t cm_in_pos;
static char cm_in_buf[1024];
static int cm_in_len;

static void cm_dir(char *dir, size_t cap)
{
    char base[260];
    DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", base, sizeof base);
    if (!n || n >= sizeof base) { dir[0] = 0; return; }
    fmt(dir, cap, "%s\\Swofty\\addon-storage\\apoc.race-broadcast", base);
}
static void cm_write(const char *line)
{
    if (!cm_out) {
        char dir[300], path[340];
        cm_dir(dir, sizeof dir);
        if (!dir[0]) { cm_out = INVALID_HANDLE; return; }
        fmt(path, sizeof path, "%s\\say.txt", dir);
        CreateDirectoryA(dir, 0);
        /* GENERIC_WRITE: replacing the old file needs write access, not just append */
        cm_out = CreateFileA(path, 0x40000000u, FILE_SHARE_READ | FILE_SHARE_WRITE | 4, 0, 2 /* CREATE_ALWAYS */, FILE_ATTRIBUTE_NORMAL, 0);
        if (cm_out == INVALID_HANDLE) LOG("commentary: can't write %s (error %u)", path, (unsigned)GetLastError());
        else LOG("commentary: lines go to %s", path);
    }
    if (cm_out == INVALID_HANDLE) return;
    DWORD w = 0;
    WriteFile(cm_out, line, (DWORD)slen(line), &w, 0);
}
static void cm_send(const char *cmd, int id, int role, const char *text)
{
    char line[560];
    fmt(line, sizeof line, "%d|%s|%s|%s\r\n", id, cmd, role >= 0 ? cm_voice[role] : "", text ? text : "");
    cm_write(line);
}

static void cm_drop(int i)
{
    int id = cmq[i].id;
    cmq[i].used = 0;
    for (int j = 0; j < CMQ; j++) if (cmq[j].used && cmq[j].after == id) cm_drop(j);
}
static int cm_ended_at(int id, double *t)
{
    for (int i = 0; i < 16; i++) if (cm_ended[i].id == id) { if (t) *t = cm_ended[i].t; return 1; }
    return 0;
}
static int cm_pending(int id)
{
    if (id == cm_cur_id) return 1;
    for (int i = 0; i < CMQ; i++) if (cmq[i].used && cmq[i].id == id) return 1;
    return 0;
}

/* a new line from a bank; 0 when the queue is full or commentary is off */
/* the recorded version of the line just picked from b: "bank;index;A;B;C" with the drivers' surnames, so the voice
   side can play the Gemini recording (with the driver's name before it) instead of reading the text */
static void cm_rec(char *out, size_t cap, const Bank *b, const Cx *x)
{
    char na[48] = "", nb[48] = "", nc[48] = "";
    if (x && x->a >= 0) surname(x->a, na, sizeof na);
    if (x && x->b >= 0) surname(x->b, nb, sizeof nb);
    if (x && x->c >= 0) surname(x->c, nc, sizeof nc);
    fmt(out, cap, "%s;%d;%s;%s;%s", b && b->name ? b->name : "", b ? b->last : -1, na, nb, nc);
}
static CmLine *cm_add(int role, int prio, double ttl, Bank *b, const Cx *x)
{
    if (!cm_level) return 0;
    if (role >= R_PIT && !cm_long) role = role == R_PIT ? R_LEAD : R_EXP; /* short races: two voices */
    int slot = -1;
    for (int i = 0; i < CMQ; i++) if (!cmq[i].used) { slot = i; break; }
    if (slot < 0) { /* full: the lowest priority goes */
        int lo = 0;
        for (int i = 1; i < CMQ; i++) if (cmq[i].prio < cmq[lo].prio) lo = i;
        if (cmq[lo].prio >= prio) return 0;
        cm_drop(lo);
        slot = lo;
    }
    CmLine *l = &cmq[slot];
    memset(l, 0, sizeof *l);
    l->used = 1;
    l->id = cm_next_id++;
    l->role = role;
    l->prio = prio;
    l->t = now();
    l->until = l->t + ttl;
    l->need = l->need2 = -1;
    l->replay = 0;
    expand(bank_pick(b), x, l->text, sizeof l->text);
    if (!l->text[0]) { l->used = 0; return 0; }
    cm_rec(l->rec, sizeof l->rec, b, x);
    return l;
}
static void cm_need(CmLine *l, int a, int b, double wait, Bank *alt, const Cx *x)
{
    if (!l) return;
    l->need = a, l->need2 = b;
    l->need_by = l->t + wait;
    if (alt) { expand(bank_pick(alt), x, l->alt, sizeof l->alt); cm_rec(l->alt_rec, sizeof l->alt_rec, alt, x); }
}
static CmLine *cm_reply(CmLine *to, int role, int prio, Bank *b, const Cx *x, double delay)
{
    if (!to) return 0;
    CmLine *l = cm_add(role, prio, (to->until - now()) + 9.0, b, x);
    if (l) l->after = to->id, l->delay = delay, l->replay = to->replay, l->pre = to->pre;
    return l;
}
static void cm_clear_below(int prio)
{
    for (int i = 0; i < CMQ; i++) if (cmq[i].used && cmq[i].prio < prio) cm_drop(i);
}

static int cm_cam(void)
{
    if (g_in_replay) return cm_replay_car;
    return watch_active ? cur_follow : g_me;
}
static double cm_budget(void) { return cm_level >= 3 ? 0.50 : cm_level == 2 ? 0.36 : 0.22; }
static double cm_est(const char *t)
{
    int words = 1;
    for (const char *p = t; *p; p++) if (*p == ' ') words++;
    return words / 2.7 + 0.5;
}

/* the voice side's answers */
static void cm_read_acks(void)
{
    if (!cm_in) {
        char dir[300], path[340];
        cm_dir(dir, sizeof dir);
        fmt(path, sizeof path, "%s\\said.txt", dir);
        cm_in = CreateFileA(path, 0x80000000u /* GENERIC_READ */, FILE_SHARE_READ | FILE_SHARE_WRITE | 4, 0, 3 /* OPEN_EXISTING */, FILE_ATTRIBUTE_NORMAL, 0);
        if (cm_in == INVALID_HANDLE) { cm_in = 0; return; }
        int64_t sz = 0;
        GetFileSizeEx(cm_in, &sz);
        cm_in_pos = sz; /* only new answers */
        cm_in_len = 0;
    }
    int64_t sz = 0;
    if (!GetFileSizeEx(cm_in, &sz)) return;
    if (sz < cm_in_pos) cm_in_pos = 0, cm_in_len = 0; /* the voice side started again */
    while (sz > cm_in_pos) {
        DWORD got = 0;
        SetFilePointerEx(cm_in, cm_in_pos, 0, 0);
        DWORD want = (DWORD)(sizeof cm_in_buf - 1 - cm_in_len);
        if (!want) cm_in_len = 0, want = sizeof cm_in_buf - 1;
        if (!ReadFile(cm_in, cm_in_buf + cm_in_len, want, &got, 0) || !got) break;
        cm_in_pos += got;
        cm_in_len += (int)got;
        cm_in_buf[cm_in_len] = 0;
        char *s = cm_in_buf, *nl;
        for (;;) {
            nl = 0;
            for (char *p = s; *p; p++) if (*p == '\n') { nl = p; break; }
            if (!nl) break;
            *nl = 0;
            /* id|what|value */
            int id = 0;
            char *p = s;
            while (*p >= '0' && *p <= '9') id = id * 10 + (*p++ - '0');
            if (*p == '|') p++;
            cm_mouth_seen = 1;
            if (p[0] == 's' && p[1] == 't' && p[2] == 'a' && p[3] == 'r' && p[4] == 't') {
                if (id == cm_cur_id) {
                    const char *v = p + 6;
                    double secs = 0, frac = 0.1;
                    while (*v >= '0' && *v <= '9') secs = secs * 10 + (*v++ - '0');
                    if (*v == '.') for (v++; *v >= '0' && *v <= '9'; v++) secs += (*v - '0') * frac, frac /= 10;
                    cm_cur_until = now() + secs + 0.1;
                    cm_air += secs;
                }
            } else if (p[0] == 'e' && p[1] == 'n' && p[2] == 'd') {
                if (id == cm_cur_id) cm_cur_until = now();
            } else if (p[0] == 's' && p[1] == 't' && p[2] == 'a' && p[3] == 't') {
                LOG("commentary voices: %s", p + 7);
                if (S.toasts || (p[7] == 'd' && p[8] == 'o')) toast("COMMENTARY", p + 7, 0); /* "downloading ..." */
            }
            s = nl + 1;
        }
        int rest = (int)slen(s);
        for (int i = 0; i < rest; i++) cm_in_buf[i] = s[i];
        cm_in_len = rest;
        cm_in_buf[rest] = 0;
    }
}

static void pair_said(int ix);
static void cm_start_line(CmLine *l)
{
    if (l->pair1) pair_said(l->pair1 - 1);
    const char *txt = l->text;
    {   /* the text for Piper, then (after a tab) which recording it is, for the recorded voices */
        char both[440];
        fmt(both, sizeof both, "%s\t%s", txt, l->rec);
        cm_send("say", l->id, l->role, both);
    }
    LOG("SAY %s: %s", cm_tag[l->role], txt);
    cm_cur_id = l->id, cm_cur_role = l->role, cm_cur_prio = l->prio;
    cm_cur_sent = now();
    cm_cur_until = cm_cur_sent + cm_est(txt) + 0.6; /* corrected by the voice side's answer */
    if (!cm_mouth_seen) cm_air += cm_est(txt);
    cm_last_any = cm_cur_sent;
    l->used = 0;
}

static void cm_idle(double t);
static void cm2_track(double t);

/* back to the menus: stop talking at once */
static void cm_session_end(void)
{
    if (!cm_level) return;
    memset(cmq, 0, sizeof cmq);
    if (cm_cur_id) cm_send("cut", 0, -1, ""), cm_cur_id = 0;
    cm_live = 0;
    cm_finished = 0;
    cm_preview_done = 0;
    cm_call[0] = 0;
}

static void cm_tick(void)
{
    static double next, last;
    double t = now();
    if (!cm_level || t < next || !g_in_race) return;
    next = t + 0.1;
    double dt = last ? t - last : 0.1;
    last = t;
    cm_air -= cm_air * (dt / 60.0); /* seconds spoken over roughly the last minute */
    cm_read_acks();
    if (cm_live) cm2_track(t);
    /* the line being spoken */
    if (cm_cur_id) {
        if (t >= cm_cur_until && t - cm_cur_sent > 0.5) {
            cm_ended[cm_ended_n % 16].id = cm_cur_id, cm_ended[cm_ended_n % 16].t = t;
            cm_ended_n++;
            cm_last_end = t, cm_last_role = cm_cur_role;
            cm_cur_id = 0;
        } else {
            /* something big while a small line plays: cut it off */
            if (cm_cur_prio <= 3)
                for (int i = 0; i < CMQ; i++)
                    if (cmq[i].used && cmq[i].prio >= 7 && !cmq[i].after && (cmq[i].need < 0 || cmq[i].need == cm_cam() || cmq[i].need2 == cm_cam())) {
                        cm_send("cut", 0, -1, "");
                        cm_cur_until = t;
                        cm_cur_prio = 9;
                        break;
                    }
            return;
        }
    }
    /* pick the next line */
    int cam = cm_cam();
    int best = -1;
    int over = cm_air / 60.0 > cm_budget();
    for (int i = 0; i < CMQ; i++) {
        CmLine *l = &cmq[i];
        if (!l->used) continue;
        if (t > l->until) { cm_drop(i); continue; }
        if (!cm_live && !l->pre) continue;
        if (l->replay == 1 && !g_in_replay) continue;
        if (l->replay == 0 && (g_in_replay || rp_state >= RP_OPENING)) continue;
        if (t < l->not_before) continue;
        if (l->after) {
            double et;
            if (!cm_ended_at(l->after, &et)) { if (!cm_pending(l->after)) cm_drop(i); continue; }
            if (t < et + l->delay) continue;
        }
        if (l->need >= 0) {
            int on = cam >= 0 && (cam == l->need || cam == l->need2);
            if (on && t - cm_shot_t < 0.45 && !g_in_replay) continue; /* let the viewer see it first */
            if (!on) {
                if (t < l->need_by) continue;
                if (l->alt[0]) { scopy(l->text, sizeof l->text, l->alt); scopy(l->rec, sizeof l->rec, l->alt_rec); l->need = l->need2 = -1; }
                else { cm_drop(i); continue; }
            }
        }
        if (over && l->prio <= 3) continue;
        if (best < 0 || l->prio > cmq[best].prio || (l->prio == cmq[best].prio && l->t < cmq[best].t)) best = i;
    }
    if (best >= 0) {
        CmLine *l = &cmq[best];
        double gap = l->prio >= 7 ? 0.15 : l->after ? 0.0 : l->role == cm_last_role ? 0.9 : 0.45;
        if (t - cm_last_end < gap) return;
        cm_start_line(l);
        return;
    }
    if (cm_live && !g_in_replay && !cm_finished) cm_idle(t);
}

/* ---- the race, line by line ------------------------------------------------------------------------------------ */
static int cm_top_team(int car)
{
    /* a car of a team that is running at the front (a car in the top four) */
    if (car < 0 || car >= 22) return 0;
    for (int i = 0; i < g_n; i++) if (C[i].valid && C[i].pos <= 4 && U.car[i].team == U.car[car].team) return 1;
    return 0;
}
static int cm_last_pair_a = -1, cm_last_pair_b = -1;
/* the same two cars again and again (swapping places, banging wheels): how many times within a window */
typedef struct { int a, b, n, kind, blame, said; double t; } CmPair;
static CmPair cm_pairs[24];
static void pair_said(int ix) { if (ix >= 0 && ix < 24) cm_pairs[ix].said = 1; }
static CmPair *pair_note(int kind, int a, int b, double window)
{
    double t = now();
    int lo = a < b ? a : b, hi = a < b ? b : a, old = 0;
    for (int i = 0; i < 24; i++) {
        CmPair *p = &cm_pairs[i];
        if (p->kind == kind && p->a == lo && p->b == hi && t - p->t < window) { p->n++, p->t = t; return p; }
        if (cm_pairs[i].t < cm_pairs[old].t) old = i;
    }
    CmPair *p = &cm_pairs[old];
    p->kind = kind, p->a = lo, p->b = hi, p->n = 1, p->t = t, p->blame = -1, p->said = 0;
    return p;
}
static CmPair *pair_find(int kind, int a, int b, double window)
{
    int lo = a < b ? a : b, hi = a < b ? b : a;
    for (int i = 0; i < 24; i++)
        if (cm_pairs[i].kind == kind && cm_pairs[i].a == lo && cm_pairs[i].b == hi && now() - cm_pairs[i].t < window) return &cm_pairs[i];
    return 0;
}
static double cm_last_pair_t;

#include "cm_round2.c"

static void cm_event(const char *kind, int a, int b, int pos, const char *text)
{
    if (!cm_level) return;
    double t = now();
    Cx x = {a, b, -1, pos, 0, 0, 0};
    CmLine *l = 0, *r = 0;
    int lv = cm_level;
    if (seq(kind, "race_start")) {
        memset(cmq, 0, sizeof cmq);
        cm_live = cm_preview_done = cm_finished = cm_strat_pic_done = 0;
        cm_long_known = 0;
        cm_last_pair_a = cm_last_pair_b = -1;
        memset(cm_pairs, 0, sizeof cm_pairs);
        cm_call[0] = 0;
        cm_last_idle = cm_last_exp_cut = cm_last_strat = cm_last_lap_line = t;
        cm2_reset();
        return;
    }
    if (seq(kind, "session")) {
        int laps = (int)U.total_laps, len = (int)U.track_len;
        /* all four voices in every race (Tom and Erin used to join only from a quarter of a Grand Prix) */
        int lng = 1;
        (void)laps, (void)len;
        if (!cm_long_known || lng != cm_long) {
            cm_long = lng, cm_long_known = 1;
            LOG("commentary: %d laps of %d m: %s", laps, len, lng ? "four voices (pit lane and strategy on)" : "two voices (short race)");
            cm_send("warm", 0, -1, lng ? "lead,expert,pit,strategy" : "lead,expert");
        }
        return;
    }
    if (seq(kind, "shot")) {
        if (a != cm_shot_car) cm_shot_car = a, cm_shot_t = t;
        if (!cm_live || g_in_replay || a < 0) return;
        /* a new picture: now and then the lead says who we are looking at, if nothing else is going on */
        if ((seq(text, "car") || seq(text, "leader") || seq(text, "manual")) && t - cm_last_any > 6 && chance(lv >= 3 ? 55 : lv == 2 ? 35 : 15)) {
            int pa = C[a].pos;
            x.p = pa;
            x.b = car_at(pa - 1), x.c = car_at(pa + 1);
            int close = x.b >= 0 && C[a].gap_ahead < 1.5f && !pair_find(3, a, x.b, 75);
            if (close) pair_note(3, a, x.b, 75);
            Bank *bk = pa == 1 ? &L_shot_leader : close ? &L_shot_close : &L_shot_car;
            if (pa == 1) x.b = x.c;
            l = cm_add(R_LEAD, 2, 5, bk, &x);
            cm_need(l, a, -1, 1.0, 0, 0);
        }
        return;
    }
    if (seq(kind, "formation")) {
        /* the formation lap preview, with all four voices, in every race */
        if (cm_preview_done) return;
        cm_preview_done = 1;
        cm_long = 1;
        int p1 = car_at(1), p2 = car_at(2), p3 = car_at(3);
        Cx y = x;
        y.a = p1, y.b = p2, y.c = p3;
        /* a formation lap takes a minute and a half or more: welcome, conditions, the front row, anyone out of
           position, the tyre picture, the pit lane, then what to watch at the start. Lines are chained, with short
           pauses, and whatever has not been said when the start lights come on is dropped. */
        static char temps[64], ttemp[32], mix[128], oddc[24];
        fmt(temps, sizeof temps, "%d degrees", (int)U.air_temp);
        fmt(ttemp, sizeof ttemp, "%d degrees", (int)U.track_temp);
        /* the tyres */
        int nsoft = 0, nmed = 0, nhard = 0, nwet = 0;
        for (int i = 0; i < g_n; i++) {
            int c = U.car[i].compound;
            nsoft += c == 16, nmed += c == 17, nhard += c == 18, nwet += c == 7 || c == 8;
        }
        int ncars = nsoft + nmed + nhard + nwet;
        int wet = U.weather >= 3 || (ncars && nwet * 2 > ncars);
        int uniform = ncars && (nsoft == ncars || nmed == ncars || nhard == ncars || nwet == ncars);
        l = cm_add(R_LEAD, 6, 60, &L_welcome, &y);
        if (l) l->pre = 1;
        Cx w = y;
        w.x = temps, w.y = ttemp;
        r = cm_reply(l, chance(50) ? R_LEAD : R_EXP, 6, wet ? &X_formation_wet : &X_formation_weather, &w, 1.0);
        /* the front row */
        int same_team = p1 >= 0 && p2 >= 0 && U.car[p1].team == U.car[p2].team;
        r = cm_reply(r, R_LEAD, 6, &L_to_exp_grid, &y, 1.2);
        r = cm_reply(r, R_EXP, 6, same_team ? &E_grid_front_sameteam : &E_grid_front, &y, 0.35);
        /* a quick car starting down the order: a car whose team mate starts in the top four, from eighth or lower */
        int outlier = -1;
        for (int i = 0; i < g_n && outlier < 0; i++)
            if (C[i].valid && C[i].pos >= 8 && C[i].pos <= 22)
                for (int j = 0; j < g_n; j++)
                    if (j != i && C[j].valid && C[j].pos <= 4 && U.car[j].team == U.car[i].team && U.car[i].team < 10) { outlier = i; break; }
        if (outlier >= 0) {
            Cx o = y;
            o.c = outlier, o.p = C[outlier].pos;
            r = cm_reply(r, R_LEAD, 5, &L_grid_outlier, &o, 1.0);
            r = cm_reply(r, R_EXP, 5, &E_grid_outlier, &o, 0.35);
        }
        if (uniform && nwet) fmt(mix, sizeof mix, "every car on %s", tyre_name(U.car[p1 >= 0 ? p1 : 0].compound, 1));
        else if (nwet) fmt(mix, sizeof mix, "%d cars on wet weather tyres", nwet);
        else {
            /* largest group first; groups of none left out */
            int cnt[3] = {nmed, nsoft, nhard};
            const char *nm[3] = {"medium", "soft", "hard"};
            int ord[3] = {0, 1, 2};
            for (int i = 0; i < 3; i++) for (int j = i + 1; j < 3; j++) if (cnt[ord[j]] > cnt[ord[i]]) { int q = ord[i]; ord[i] = ord[j]; ord[j] = q; }
            int parts = (cnt[ord[0]] > 0) + (cnt[ord[1]] > 0) + (cnt[ord[2]] > 0);
            if (parts <= 1) fmt(mix, sizeof mix, "every car on the %s", nm[ord[0]]);
            else if (parts == 2) fmt(mix, sizeof mix, "%d cars on the %s and %d on the %s", cnt[ord[0]], nm[ord[0]], cnt[ord[1]], nm[ord[1]]);
            else fmt(mix, sizeof mix, "%d cars on the %s, %d on the %s and %d on the %s", cnt[ord[0]], nm[ord[0]], cnt[ord[1]], nm[ord[1]], cnt[ord[2]], nm[ord[2]]);
        }
        y.x = mix;
        r = cm_reply(r, R_LEAD, 6, &L_to_strat_preview, &y, 1.2);
        r = cm_reply(r, R_STRAT, 6, uniform ? &S_preview_uniform : &S_preview_mix, &y, 0.4);
        Bank *front = p1 >= 0 && p2 >= 0 && U.car[p1].compound == U.car[p2].compound ? &S_preview_front_same : &S_preview_front_split;
        r = cm_reply(r, R_STRAT, 6, front, &y, 0.3);
        /* the odd one out in the top ten: a different tyre from most of the cars around it */
        {
            int cnt[24] = {0}, top = -1, odd = -1;
            for (int i = 0; i < g_n; i++) if (C[i].valid && C[i].pos <= 10 && U.car[i].compound < 24) cnt[U.car[i].compound]++;
            for (int k = 0; k < 24; k++) if (top < 0 || cnt[k] > cnt[top]) top = k;
            for (int i = 0; i < g_n && odd < 0; i++) if (C[i].valid && C[i].pos <= 10 && U.car[i].compound != top && U.car[i].has_status) odd = i;
            if (odd >= 0 && cnt[top] >= 6 && C[odd].pos > 2) { /* the front row has its own line */
                Cx o = y;
                o.c = odd, o.p = C[odd].pos;
                scopy(oddc, sizeof oddc, tyre_name(U.car[odd].compound, 1));
                o.y = oddc;
                r = cm_reply(r, R_STRAT, 5, &S_preview_oddone, &o, 0.4);
            }
        }
        r = cm_reply(r, R_STRAT, 5, wet ? &S_preview_plan_wet : (int)U.total_laps > 35 ? &S_preview_plan_long : &S_preview_plan_short, &y, 0.3);
        r = cm_reply(r, R_LEAD, 5, &L_preview_thanks, &y, 0.5);
        /* the pit lane */
        r = cm_reply(r, R_LEAD, 5, &L_to_pit_preview, &y, 1.0);
        r = cm_reply(r, R_PIT, 5, &P_preview, &y, 0.4);
        r = cm_reply(r, R_PIT, 4, &P_preview_2, &y, 0.3);
        /* the start */
        r = cm_reply(r, R_LEAD, 5, &L_to_exp_start, &y, 1.2);
        r = cm_reply(r, R_EXP, 5, &E_preview_start, &y, 0.35);
        if (lv >= 2) r = cm_reply(r, R_EXP, 4, &E_preview_watch, &y, 0.4);
        cm_reply(r, R_LEAD, 4, &L_preview_close, &y, 0.6);
        return;
    }
    if (seq(kind, "start_lights")) {
        /* the first light: the call is rendered now, so it comes the instant the lights go out */
        if (cm_call[0]) return;
        Cx y = {car_at(1), car_at(2), -1, 1, 0, 0, 0};
        expand(bank_pick(&L_lights_out), &y, cm_call, sizeof cm_call);
        cm_rec(cm_call_rec, sizeof cm_call_rec, &L_lights_out, &y);
        cm_send("prep", 0, R_LEAD, cm_call);
        cm_clear_below(9); /* the preview stops for the start */
        return;
    }
    if (seq(kind, "lights_out")) {
        cm_live = 1;
        cm_clear_below(10);
        if (cm_cur_id && cm_cur_prio < 9) cm_send("cut", 0, -1, ""), cm_cur_until = t; /* nothing talks over the start */
        Cx y = {car_at(1), car_at(2), -1, 1, 0, 0, 0};
        l = cm_add(R_LEAD, 9, 5, &L_lights_out, &y);
        if (l && cm_call[0]) scopy(l->text, sizeof l->text, cm_call), scopy(l->rec, sizeof l->rec, cm_call_rec);
        cm_call[0] = 0;
        cm_last_idle = cm_last_exp_cut = cm_last_strat = cm_last_lap_line = t;
        return;
    }
    if (cm_finished && !seq(kind, "replay_start") && !seq(kind, "replay_view") && !seq(kind, "replay_end")) return; /* after the flag */
    if (!cm_live && !seq(kind, "restart")) {
        /* a race joined late (no lights seen): the first lap line or any action starts the commentary */
        if ((seq(kind, "lap") || seq(kind, "overtake") || seq(kind, "contact")) && U.sc_status != 3) cm_live = 1; /* not on the formation lap */
        else return;
    }
    if (cm2_event(kind, a, b, pos, text, &x, t, lv)) return; /* pit cycle, pit exit, touches, damage */
    if (seq(kind, "replay_start")) {
        cm_clear_below(8);
        /* text: kind;story */
        int k = 0;
        for (; text[k] && text[k] != ';' && k < 15; k++) cm_replay_kind[k] = text[k];
        cm_replay_kind[k] = 0;
        cm_replay_a = a, cm_replay_b = b, cm_replay_car = -1;
        Bank *bk = seq(cm_replay_kind, "contact") ? &L_replay_contact : seq(cm_replay_kind, "start") ? &L_replay_start
                   : seq(cm_replay_kind, "restart") ? &L_replay_restart : &L_replay_overtake;
        l = cm_add(R_LEAD, 8, 6, bk, &x);
        if (l) l->replay = -1;
        return;
    }
    if (seq(kind, "replay_view")) {
        cm_replay_car = a;
        int v = text[0] - '0';
        int ra = cm_replay_a, rb = cm_replay_b;
        Cx y = {ra, rb, -1, ra >= 0 ? C[ra].pos : 0, 0, 0, 0};
        int con = seq(cm_replay_kind, "contact"), st = seq(cm_replay_kind, "start") || seq(cm_replay_kind, "restart");
        Bank *bk;
        if (st) { y.a = a; bk = v == 0 ? &E_rp_start_1 : v == 1 ? &E_rp_start_2 : &E_rp_start_3; }
        else if (con) {
            CmPair *pr = pair_find(2, ra, rb, 600);
            int blame = pr && pr->blame >= 0 ? pr->blame : chance(55);
            if (pr) pr->blame = blame;
            bk = v == 0 ? &E_rp_contact_1 : v == 1 ? &E_rp_contact_2 : blame ? &E_rp_contact_blame : &E_rp_contact_neutral;
        }
        else bk = v == 0 ? &E_rp_pass_1 : v == 1 ? &E_rp_pass_2 : &E_rp_pass_3;
        if (v == 1 && !st && rb >= 0) y.a = rb, y.b = ra; /* the second view is the other car */
        l = cm_add(R_EXP, 8, v == 2 ? 7 : 5, bk, &y);
        if (l) l->replay = 1;
        return;
    }
    if (seq(kind, "replay_end")) {
        cm_clear_below(9);
        cm_replay_car = -1;
        l = cm_add(R_LEAD, 8, 6, &L_replay_back, &x);
        cm_last_idle = t;
        return;
    }
    if (g_in_replay || rp_state >= RP_OPENING) return; /* live events during a replay are old news when it ends */

    if (seq(kind, "overtake")) {
        int place = pos > 0 ? pos : 0;
        /* a breather after a call: a pass outside the top three within 6 s of the last incident call is let go */
        if (a >= 0 && b >= 0) { int pl = C[a].pos < C[b].pos ? C[a].pos : C[b].pos; if (pl > 3 && t - cm_last_inc < 6) return; }
        cm_last_inc = t;
        /* the game's overtake: a passes b; the place is the higher of the two */
        if (a >= 0 && b >= 0) place = C[a].pos < C[b].pos ? C[a].pos : C[b].pos;
        x.p = place;
        /* the same two cars swapping: the first pass is called, the switchback once, then not again for a while */
        CmPair *pr = pair_note(1, a, b, 25);
        if (pr->n >= 3) return;
        if (pr->n == 2) {
            /* "back in front" only makes sense if the first pass was called, and only near the front */
            if (!pr->said || place > 8) return;
            l = cm_add(R_LEAD, place <= 3 ? 6 : 4, 4, &L_pass_back, &x);
            cm_need(l, a, b, 2.5, 0, 0);
            return;
        }
        if (place == 1) {
            l = cm_add(R_LEAD, 7, 7, &L_pass_lead, &x);
            cm_need(l, a, b, 3.5, &L_pass_lead_else, &x);
            r = cm_reply(l, R_EXP, 5, &E_pass_lead, &x, 0.35);
        } else if (place <= 3) {
            l = cm_add(R_LEAD, 6, 6, &L_pass_podium, &x);
            cm_need(l, a, b, 3.5, &L_pass_else, &x);
            if (chance(lv >= 2 ? 55 : 25)) cm_reply(l, R_EXP, 4, &E_pass_good, &x, 0.35);
        } else if (place == 10) {
            l = cm_add(R_LEAD, 5, 6, &L_pass_points, &x);
            cm_need(l, a, b, 3.5, 0, 0); /* on camera only: fewer "elsewhere" calls bouncing around */
        } else if (place <= 10 || lv >= 3) {
            l = cm_add(R_LEAD, place <= 10 ? 4 : 3, 5, &L_pass_mid, &x);
            cm_need(l, a, b, 3.0, 0, 0);
            if (l && chance(lv >= 3 ? 35 : 15)) cm_reply(l, R_EXP, 3, &E_pass_good, &x, 0.35);
        }
        if (l) l->pair1 = (int)(pr - cm_pairs) + 1; /* when it is heard, a switchback can follow */
        return;
    }
    if (seq(kind, "side")) {
        /* the feed's car is the one behind, car2 the one ahead; the place is the one ahead */
        x.p = pos > 1 ? pos - 1 : pos;
        static double last_side;
        if (t - last_side < (lv >= 3 ? 8 : 14)) return; /* one call per battle, not every time they touch wheels */
        last_side = t;
        l = cm_add(R_LEAD, pos <= 3 ? 5 : 3, 3, &L_side, &x);
        cm_need(l, a, b, 1.5, 0, 0);
        return;
    }
    if (seq(kind, "battle")) {
        /* the director has just cut to a battle: a is behind, b ahead */
        x.p = pos - 1;
        /* this battle has been talked about lately (introduced, or in a lap or picture line): not again yet */
        if (pair_note(3, a, b, pos - 1 == 1 ? 75 : 60)->n > 1) return;
        Bank *bk = pos - 1 == 1 ? &L_battle_lead : pos - 1 <= 3 ? &L_battle_podium : pos - 1 == 10 ? &L_battle_points : &L_battle;
        l = cm_add(R_LEAD, 3, 6, bk, &x);
        cm_need(l, a, b, 1.0, 0, 0);
        if (l && chance(lv >= 3 ? 60 : lv == 2 ? 40 : 15)) {
            int da = a >= 0 && a < NC ? U.car[a].tyre_age : 0, db = b >= 0 && b < NC ? U.car[b].tyre_age : 0;
            Bank *e = &E_battle;
            if (U.car[a].has_status && U.car[a].compound != U.car[b].compound) e = &E_battle_compound;
            else if (U.car[a].has_status && db - da >= 5) e = &E_battle_fresher;
            else if (U.car[a].has_status && da - db >= 5) e = &E_battle_older;
            else if (C[a].closing < -0.05f) e = &E_battle_closing;
            cm_reply(l, R_EXP, 3, e, &x, 0.4);
        }
        return;
    }
    if (seq(kind, "contact")) {
        if (t - cm_last_inc < 5 && !(a >= 0 && C[a].pos <= 3)) return;
        cm_last_inc = t;
        /* the same two cars touching again: one call. Small touches at the back off camera: not worth a call */
        CmPair *pr = pair_note(2, a, b, 40);
        if (pr->n > 1) return;
        /* called when the camera shows it (the director cuts to contact); off camera only in the top ten, and not
           several "elsewhere" calls in a row */
        static double last_else;
        int allow_else = (pos <= 10 || lv >= 3) && t - last_else > 10;
        if (allow_else) last_else = t;
        x.p = pos;
        /* teammates: the team will not be happy */
        int mates = a >= 0 && b >= 0 && a < NC && b < NC && U.car[a].team == U.car[b].team;
        l = cm_add(R_LEAD, 7, 7, mates ? &L_team_crash : &L_contact, &x);
        cm_need(l, a, b, 3.5, allow_else ? &L_contact_else : 0, &x);
        /* the expert's view of who was to blame is decided once, so the replay verdict agrees with it:
           the car behind (a) more to blame, or a racing incident */
        pr->blame = chance(55) ? 1 : 0;
        if (chance(65)) cm_reply(l, R_EXP, 5, pr->blame ? &E_contact_blame : &E_contact_neutral, &x, 0.35);
        return;
    }
    if (seq(kind, "damage")) {
        x.p = pos;
        l = cm_add(chance(50) ? R_EXP : R_LEAD, 5, 12, &X_damage, &x);
        if (l) l->not_before = t + 1.5;
        return;
    }
    if (seq(kind, "spin")) {
        x.p = pos;
        l = cm_add(R_LEAD, 6, 5, &L_spin, &x);
        cm_need(l, a, -1, 3.0, pos <= 10 ? &L_spin_else : 0, &x);
        if (chance(lv >= 2 ? 45 : 20)) cm_reply(l, R_EXP, 3, &E_spin, &x, 0.35);
        return;
    }
    if (seq(kind, "stopped")) {
        x.p = pos;
        l = cm_add(R_LEAD, 5, 8, &L_stopped, &x);
        cm_need(l, a, -1, 3.0, &L_stopped_else, &x);
        if (lv >= 2) cm_reply(l, R_EXP, 3, &E_stopped, &x, 0.4);
        return;
    }
    if (seq(kind, "out")) {
        x.p = pos;
        l = cm_add(R_LEAD, 6, 9, &L_out, &x);
        cm_need(l, a, -1, 3.0, &L_out_else, &x);
        if (lv >= 2) cm_reply(l, R_EXP, 4, &E_out, &x, 0.4);
        return;
    }
    if (seq(kind, "pit")) {
        x.p = pos;
        l = cm_add(R_LEAD, pos <= 3 ? 5 : 4, 6, &L_pit_in, &x);
        cm_need(l, a, -1, 2.5, pos <= 6 ? &L_pit_in_else : 0, &x);
        return;
    }
    if (seq(kind, "pit_out")) {
        /* text: from=;old=;new=;age=;stop=;lane=;stops= */
        int from = 0, old = 0, nw = 0, stop = 0, stops = 0;
        for (const char *p = text; *p; p++) {
            const char *k = p;
            while (*p && *p != '=') p++;
            if (!*p) break;
            int v = 0;
            const char *q = p + 1;
            while (*q >= '0' && *q <= '9') v = v * 10 + (*q++ - '0');
            if (k[0] == 'f') from = v; else if (k[0] == 'o') old = v; else if (k[0] == 'n') nw = v;
            else if (k[0] == 's' && k[4] == '=') stop = v; else if (k[0] == 's' && k[4] == 's') stops = v;
            p = q;
            if (!*p) break;
        }
        (void)stops;
        cm2_event("pit_exit", a, -1, pos, text, &x, t, lv); /* "he's out ahead" after the pit exit moment */
        x.b = b;
        if (!from || from > (cm_long ? 10 : 5)) return;
        if (cm_long && !(from <= 6 || cm_top_team(a))) return;
        char st[48], cmp[24];
        if (stop > 900 && stop < 60000) fmt(st, sizeof st, "%.1f seconds", stop / 1000.0);
        else scopy(st, sizeof st, "");
        scopy(cmp, sizeof cmp, tyre_name(nw, 1));
        x.p = C[a].pos, x.x = st, x.y = cmp;
        x.b = car_at(C[a].pos + 1);
        Bank *bk = !cm_long ? &L_pit_out : !st[0] ? &P_out_nostop : stop > 4500 ? &P_out_slow : stop < 2600 ? &P_out_quick : &P_out;
        l = cm_add(R_PIT, from <= 3 ? 5 : 4, 14, bk, &x);
        if (l && cm_long && lv >= 2 && chance(60)) {
            int left = laps_to_go();
            Bank *sb = nw == 18 && left > 0 ? (left > 30 ? &S_pit_hard_long : &S_pit_hard_end)
                       : nw == 16 ? (left > 20 ? &S_pit_soft_long : &S_pit_soft_end)
                       : nw == 17 ? (left > 28 ? &S_pit_med_long : &S_pit_med_end)
                       : (nw == 7 || nw == 8) ? &S_pit_wet : &S_pit_generic;
            if (old && nw && old != nw && (old == 7 || old == 8) != (nw == 7 || nw == 8)) sb = &S_pit_wet;
            x.n = left;
            cm_reply(l, R_STRAT, 3, sb, &x, 0.5);
        }
        return;
    }
    if (seq(kind, "penalty")) {
        /* text: type=;inf=;time=;lap=;places= */
        int ty = -1, inf = -1, secs = 0;
        for (const char *p = text; *p; p++) {
            if (p[0] == 't' && p[1] == 'y' && p[4] == '=') { ty = 0; for (p += 5; *p >= '0' && *p <= '9'; p++) ty = ty * 10 + (*p - '0'); }
            if (p[0] == 'i' && p[3] == '=') { inf = 0; for (p += 4; *p >= '0' && *p <= '9'; p++) inf = inf * 10 + (*p - '0'); }
            if (p[0] == 't' && p[1] == 'i' && p[4] == '=') { for (p += 5; *p >= '0' && *p <= '9'; p++) secs = secs * 10 + (*p - '0'); }
            if (!*p) break;
        }
        const char *what = ty == 0 ? "a drive through penalty" : ty == 1 ? "a stop and go penalty" : ty == 2 ? "a grid penalty"
                           : ty == 6 ? "a disqualification" : 0;
        static char tb[48];
        if (ty == 4) { if (secs > 0 && secs <= 20) fmt(tb, sizeof tb, "a %s second time penalty", cm_num[secs]); else scopy(tb, sizeof tb, "a time penalty"); what = tb; }
        if (!what) return;
        const char *why = inf == 3 || inf == 4 ? "causing a collision" : inf == 5 || inf == 6 ? "not giving a place back after contact"
                          : inf == 7 || (inf >= 27 && inf <= 29) ? "leaving the track and gaining an advantage"
                          : inf == 8 || inf == 9 ? "overtaking off the track" : inf == 17 ? "speeding in the pit lane"
                          : inf == 11 ? "ignoring blue flags" : inf == 12 ? "ignoring yellow flags" : inf == 34 ? "a jump start"
                          : inf == 36 ? "overtaking under the safety car" : inf == 10 ? "crossing the white line at the pit exit"
                          : inf == 21 ? "too many track limits warnings" : inf == 0 || inf == 1 ? "impeding another car"
                          : inf == 20 ? "too many penalties" : "a breach of the rules";
        Bank *rule = inf == 3 || inf == 4 || inf == 5 || inf == 6 ? &S_rule_collision
                     : inf == 7 || inf == 8 || inf == 9 || inf == 21 || (inf >= 27 && inf <= 29) ? &S_rule_track
                     : inf == 17 ? &S_rule_pitspeed : inf == 34 ? &S_rule_jump : inf == 36 ? &S_rule_sc
                     : inf == 11 || inf == 12 ? &S_rule_flags : &S_rule_generic;
        if (ty == 4) rule = inf == 3 || inf == 4 || inf == 5 || inf == 6 ? &S_rule_collision : rule;
        x.x = what, x.y = why, x.p = pos;
        l = cm_add(R_LEAD, 5, 15, &L_penalty, &x);
        r = cm_reply(l, R_STRAT, 4, rule, &x, 0.4);
        if (r && ty == 4 && cm_long && chance(70)) cm_reply(r, R_STRAT, 3, &S_rule_timepen_how, &x, 0.3);
        return;
    }
    if (seq(kind, "graphic")) {
        /* the director has put one of Project Aeternum's graphics on screen (text says which): talk about what it shows.
           Each kind at most every 25 s, and never over a bigger call. */
        static double g_last[8];
        const char *g = text ? text : "";
        int k = seq(g, "battle_for") ? 0 : seq(g, "battle_on") ? 1 : seq(g, "catching") ? 2 : seq(g, "driver_card") ? 3
              : seq(g, "tyre_life") ? 4 : seq(g, "pit_lane") ? 5 : seq(g, "car_status") ? 6 : seq(g, "weather") ? 7 : -1;
        if (k < 0 || !cm_live || t - g_last[k] < 25) return;
        if (lv < 2 && k != 0 && k != 3) return; /* quiet: only the battle and driver graphics */
        char xb[48], db[96];
        if (k == 0) {
            if (a < 0 || b < 0) return;
            x.p = pos - 1;
            /* the battle has just been called: the expert reads the graphic; otherwise he introduces it himself */
            Bank *e = U.car[a].has_status && U.car[a].compound != U.car[b].compound && chance(50) ? &E_gfx_battle_tyres : &E_gfx_battle;
            l = cm_add(R_EXP, 3, 7, e, &x);
            if (l) l->not_before = t + 1.2;
            cm_need(l, a, b, 2.0, 0, 0);
        } else if (k == 1) {
            if (a < 0 || b < 0) return;
            x.p = pos - 1;
            l = cm_add(R_LEAD, 3, 6, &L_gfx_battle_on, &x);
            cm_need(l, a, b, 1.0, 0, 0);
            if (l && chance(lv >= 3 ? 70 : 45)) cm_reply(l, R_EXP, 3, &E_gfx_battle_on, &x, 0.4);
        } else if (k == 2) {
            if (a < 0 || b < 0) return;
            x.p = pos - 1;
            l = cm_add(R_LEAD, 3, 6, &L_gfx_catching, &x);
            cm_need(l, a, b, 1.5, 0, 0);
            if (l && chance(lv >= 3 ? 60 : 35)) cm_reply(l, R_EXP, 3, &E_gfx_catching, &x, 0.4);
        } else if (k == 3) {
            if (a < 0 || a >= NC) return;
            int grid = U.car[a].grid, now_p = C[a].pos;
            if (grid < 1 || grid > 22 || now_p < 1 || now_p > 22) return;
            scopy(xb, sizeof xb, cm_ord[grid]);
            x.x = xb, x.p = now_p, x.n = grid > now_p ? grid - now_p : now_p - grid;
            Bank *bk = grid > now_p ? &L_gfx_card_up : grid < now_p ? &L_gfx_card_down : &L_gfx_card_same;
            l = cm_add(R_LEAD, 2, 6, bk, &x);
            if (l) l->not_before = t + 0.8;
            cm_need(l, a, -1, 1.5, 0, 0);
        } else if (k == 4) {
            if (a < 0 || a >= NC || !U.car[a].has_status) return;
            int age = U.car[a].tyre_age, wear = U.car[a].wear;
            x.n = wear;
            Bank *bk = age <= 2 ? &S_gfx_tyres_fresh : wear < 30 ? &S_gfx_tyres_ok : wear < 55 ? &S_gfx_tyres_worn : &S_gfx_tyres_gone;
            l = cm_add(R_STRAT, 2, 8, bk, &x);
            if (l) l->not_before = t + 1.0;
            cm_need(l, a, -1, 2.0, 0, 0);
        } else if (k == 5) {
            if (a < 0 || chance(lv >= 3 ? 30 : 55)) return; /* the pit stop has its own calls: only now and then */
            l = cm_add(R_PIT, 2, 6, &P_gfx_pit, &x);
            cm_need(l, a, -1, 1.5, 0, 0);
        } else if (k == 6) {
            if (a < 0 || a >= NC) return;
            int fl = U.car[a].wing_fl, fr = U.car[a].wing_fr, rw = U.car[a].wing_r;
            if (rw > fl && rw > fr) fmt(db, sizeof db, "rear wing damage at %d per cent", rw);
            else fmt(db, sizeof db, "front wing damage at %d per cent", fl > fr ? fl : fr);
            x.x = db;
            l = cm_add(R_EXP, 3, 8, &E_gfx_damage, &x);
            if (l) l->not_before = t + 1.0;
            cm_need(l, a, -1, 2.0, 0, 0);
        } else if (k == 7) {
            l = cm_add(R_STRAT, 3, 10, &S_gfx_weather, &x);
        }
        if (l) g_last[k] = t;
        return;
    }
    if (seq(kind, "fastest_lap")) {
        if (lv < 2 || (g_leader >= 0 && C[g_leader].lap < 3)) return;
        x.x = text;
        l = cm_add(R_LEAD, 2, 6, &L_fastest, &x);
        cm_need(l, a, -1, 6.0, &L_fastest, &x); /* fine off camera too */
        return;
    }
    if (seq(kind, "safety_car")) {
        int dep = 0, vsc = 0, in = 0, resume = 0;
        for (const char *p = text; *p; p++) {
            if (p[0] == 'V' && p[1] == 'I') vsc = 1;
            if (p[0] == 'D' && p[1] == 'E' && p[2] == 'P') dep = 1;
            if (p[0] == 'I' && p[1] == 'N' && p[2] == ' ' && p[3] == 'T') in = 1;
            if (p[0] == 'R' && p[1] == 'E' && p[2] == 'S' && p[3] == 'U') resume = 1;
        }
        /* three phases with their own lines: out (what it means, who gains a cheap stop), the period itself (cm2_idle:
           restart, fresh tyres, who benefits; no battle talk) and in (get ready for the restart) */
        if (dep) {
            cm_clear_below(6);
            M2.topic_t[T_SC] = t + 20; /* the period's own talk starts a little later */
            l = cm_add(R_LEAD, 7, 10, vsc ? (chance(50) ? &L_vsc : &L_vsc_out) : (chance(50) ? &L_sc : &L_sc_out), &x);
            if (chance(50)) r = cm_reply(l, R_LEAD, 5, &L_to_strat, &x, 0.4), cm_reply(r, R_STRAT, 5, vsc ? (chance(50) ? &S_vsc : &S_vsc_out) : (chance(50) ? &S_sc : &S_sc_out), &x, 0.35);
            else if (vsc) cm_reply(l, R_EXP, 4, &E_vsc, &x, 0.4);
            else r = cm_reply(l, R_EXP, 4, chance(50) ? &E_sc : &E_sc_out, &x, 0.4), cm_reply(r, R_STRAT, 4, &S_sc_out, &x, 0.4);
        } else if (in) {
            l = cm_add(R_LEAD, 5, 10, &L_sc_in, &x);
            r = cm_reply(l, R_EXP, 4, &E_sc_in, &x, 0.4);
            if (r && chance(45)) cm_reply(r, R_STRAT, 3, &S_sc_in, &x, 0.4);
        } else if (resume && vsc) {
            M2.sc_end_t = t;
            l = cm_add(R_LEAD, 6, 6, &L_vsc_end, &x);
        } else if (resume) M2.sc_end_t = t;
        return;
    }
    if (seq(kind, "red_flag")) {
        cm_clear_below(8);
        l = cm_add(R_LEAD, 8, 10, &L_red, &x);
        if (cm_long) cm_reply(l, R_STRAT, 5, &S_red, &x, 0.5); else cm_reply(l, R_EXP, 5, &E_red, &x, 0.5);
        return;
    }
    if (seq(kind, "restart")) {
        cm_live = 1;
        int red = text[0] == 'R';
        cm_clear_below(8);
        Cx y = {car_at(1), car_at(2), -1, 1, 0, 0, 0};
        l = cm_add(R_LEAD, 8, 6, red ? &L_restart_red : chance(50) ? &L_restart_sc : &L_sc_restart, &y);
        if (!red) { M2.sc_end_t = t; if (l && lv >= 2 && chance(60)) cm_reply(l, R_EXP, 4, &E_sc_restart, &y, 0.5); }
        return;
    }
    if (seq(kind, "lap")) {
        int left = laps_to_go();
        int p1 = car_at(1), p2 = car_at(2);
        Cx y = {p1, p2, car_at(3), 1, left, 0, 0};
        if (left == 10 || left == 5 || left == 3 || left == 2) {
            l = cm_add(R_LEAD, 4, 12, left == 2 ? &L_two_to_go : &L_laps_to_go, &y);
            if (l && left == 5 && lv >= 2) cm_reply(l, R_EXP, 3, &E_closing_stages, &y, 0.4);
            cm_last_lap_line = t;
        } else if (t - cm_last_lap_line > (lv >= 3 ? 70 : lv == 2 ? 110 : 200) && p1 >= 0 && p2 >= 0) {
            float g = C[p2].gap_ahead;
            Bank *bk = g < 1.0f ? &L_lap_close : g < 4.0f ? &L_lap_gap : &L_lap_clear;
            if (g < 1.0f) { if (pair_find(3, p1, p2, 75)) bk = &L_lap_gap; else pair_note(3, p1, p2, 75); }
            l = cm_add(R_LEAD, 2, 12, bk, &y);
            cm_last_lap_line = t;
        }
        return;
    }
    if (seq(kind, "final_lap")) {
        Cx y = {car_at(1), car_at(2), -1, 1, 0, 0, 0};
        l = cm_add(R_LEAD, 6, 10, &L_final_lap, &y);
        if (lv >= 2 && y.b >= 0 && C[y.b].gap_ahead < 1.5f) cm_reply(l, R_EXP, 4, &E_final_close, &y, 0.4);
        return;
    }
    if (seq(kind, "finish")) {
        cm_finished = 1;
        cm_clear_below(9);
        Cx y = {a, car_at(2), car_at(3), 1, 0, 0, 0};
        l = cm_add(R_LEAD, 9, 15, &L_win, &y);
        r = cm_reply(l, R_LEAD, 7, &L_podium, &y, 0.6);
        r = cm_reply(r, R_EXP, 7, &E_win, &y, 0.5);
        if (cm_long) r = cm_reply(r, R_STRAT, 5, &S_win, &y, 0.5);
        cm_reply(r, R_LEAD, 6, &L_signoff, &y, 0.6);
        return;
    }
}

/* nothing is happening: talk about what is on screen, or the race as a whole */
static void cm_idle(double t)
{
    int lv = cm_level;
    double quiet = lv >= 3 ? 12 : lv == 2 ? 22 : 40;
    if (t - cm_last_any < quiet || t - cm_last_idle < quiet * 0.7) return;
    if (cm_air / 60.0 > cm_budget() * 0.8) return;
    for (int i = 0; i < CMQ; i++) if (cmq[i].used) return;
    cm_last_idle = t;
    int cam = cm_cam();
    if (cam < 0 || cam >= NC || !C[cam].valid) return;
    Cx x = {cam, car_at(C[cam].pos - 1), car_at(C[cam].pos + 1), C[cam].pos, 0, 0, 0};
    int lap = g_leader >= 0 ? C[g_leader].lap : 0;
    CmLine *l = 0;
    /* round 2: the topic engine (safety car talk, net lead, teammates, history, weather, damage, pace, tyres,
       the driver, strategy, the race so far, conversations); the older lines below stay in the mix */
    if (U.sc_status == 1 || U.sc_status == 2 || chance(70)) { if (cm2_idle(t, cam, &x, lv)) return; }
    if (U.sc_status == 1 || U.sc_status == 2) return;
    /* the strategist's picture of the race, now and then in a longer race */
    if (cm_long && lap >= 3 && t - cm_last_strat > (lv >= 3 ? 120 : lv == 2 ? 170 : 300) && U.car[cam].has_status) {
        cm_last_strat = t;
        int stopped = 0, top = 0, oldest = -1;
        for (int i = 0; i < g_n; i++) if (C[i].valid && C[i].pos <= 10) {
            top++;
            if (U.car[i].stops > 0) stopped++;
            if (oldest < 0 || U.car[i].tyre_age > U.car[oldest].tyre_age) oldest = i;
        }
        Cx y = {car_at(1), car_at(2), oldest, 1, stopped, 0, 0};
        Bank *bk = stopped == 0 ? &S_pic_nostops : stopped >= top ? &S_pic_allstopped : &S_pic_some;
        if (!cm_strat_pic_done) bk = &S_pic_first, cm_strat_pic_done = 1;
        CmLine *h = cm_add(R_LEAD, 3, 10, &L_to_strat, &y);
        if (h && chance(50)) { cm_reply(h, R_STRAT, 3, bk, &y, 0.35); }
        else { if (h) h->used = 0; cm_add(R_STRAT, 3, 10, bk, &y); }
        return;
    }
    /* the expert, about the car on screen */
    if (t - cm_last_exp_cut > (lv >= 3 ? 45 : lv == 2 ? 70 : 140) && chance(60)) {
        cm_last_exp_cut = t;
        Bank *bk = &E_idle_general;
        if (U.car[cam].has_status && x.b >= 0 && C[cam].gap_ahead < 3.0f && U.car[x.b].tyre_age - U.car[cam].tyre_age >= 6) bk = &E_idle_fresher;
        else if (U.car[cam].has_status && x.b >= 0 && C[cam].gap_ahead < 3.0f && U.car[cam].tyre_age - U.car[x.b].tyre_age >= 6) bk = &E_idle_older;
        else if (x.b >= 0 && U.car[cam].last_lap_ms > 0 && U.car[x.b].last_lap_ms > 0 && U.car[x.b].last_lap_ms - U.car[cam].last_lap_ms > 300 && C[cam].gap_ahead < 6.0f) {
            static char d[32];
            int tenths = (U.car[x.b].last_lap_ms - U.car[cam].last_lap_ms + 50) / 100;
            if (tenths <= 1) scopy(d, sizeof d, "a tenth");
            else if (tenths < 10) fmt(d, sizeof d, "%s tenths", cm_num[tenths]);
            else fmt(d, sizeof d, "%.1f seconds", tenths / 10.0);
            x.x = d;
            bk = &E_idle_quicker;
        } else if (U.car[cam].has_status && U.car[cam].tyre_age >= 18) bk = &E_idle_old_tyres;
        else if (C[cam].pos == 1) bk = &E_idle_leader;
        l = cm_add(R_EXP, 2, 8, bk, &x);
        cm_need(l, cam, -1, 1.0, 0, 0);
        return;
    }
    /* the lead, about the picture; not the same car again and again while the camera stays on it */
    static int idle_car = -1;
    static double idle_car_t;
    if (cam == idle_car && t - idle_car_t < 60) return;
    idle_car = cam, idle_car_t = t;
    int close = x.b >= 0 && C[cam].gap_ahead < 1.2f;
    if (close && pair_find(3, cam, x.b, 75)) return; /* that battle was talked about a moment ago */
    if (close) pair_note(3, cam, x.b, 75);
    Bank *bk = C[cam].pos == 1 ? &L_idle_leader : close ? &L_idle_close : &L_idle_car;
    if (C[cam].pos == 1) x.b = x.c;
    l = cm_add(R_LEAD, 2, 8, bk, &x);
    cm_need(l, cam, -1, 1.0, 0, 0);
    if (l && lv >= 2 && chance(30)) cm_reply(l, R_EXP, 2, &E_idle_general, &x, 0.4);
}

/* the addon's feed hands every story here as well */
static void cm_feed(const char *kind, int car, int car2, int pos, const char *text)
{
    cm_event(kind, car, car2, pos, text ? text : "");
}
