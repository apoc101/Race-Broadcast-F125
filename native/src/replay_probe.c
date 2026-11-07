/* Replay probe (test builds): logs what the game does when you open Instant Replay by hand, so the addon can learn
 * to do the same. Logging only: every hook passes the call on unchanged.
 *   - vtable hooks: SessionStateViewInstantReplay onStateEnter / onStateUpdate / onStateExit, and the pause menu's
 *     PauseAvailabilityViewModel::onCommandRaised (vtable slots found statically in the Steam build 1789118616)
 *   - inline hook: Game_ReplayCamera_SetFocusVehicle (its first 14 bytes checked before patching)
 *   - Swofty wraps: Sess_State_OnPauseRequested, Sess_State_Pause, Sess_State_Unpause, Ne_Replay_SetState
 * Every hook first checks the bytes it expects; on any mismatch it stays off and says so. */

#define MEM_COMMIT_RESERVE 0x3000
#define PAGE_RWX 0x40

#define KB_BASE 0x140000000ull
static uintptr_t g_base;
static uintptr_t rva(uint64_t kb_va) { return g_base + (uintptr_t)(kb_va - KB_BASE); }

enum { P_IR_ENTER, P_IR_UPDATE, P_IR_EXIT, P_PAUSE_CMD, P_FOCUS, P_COUNT };
static const char *const P_NAME[P_COUNT] = {"InstantReplay.onStateEnter", "InstantReplay.onStateUpdate", "InstantReplay.onStateExit",
                                            "PauseMenu.onCommandRaised", "ReplayCamera.SetFocusVehicle"};
static void *p_orig[P_COUNT];
static double p_last[P_COUNT];
static void *volatile g_replay_cam;
static volatile int g_replay_state = -1; /* the replay system's state: 2 while a replay plays */
static volatile double g_replay_state_t;
static long p_count[P_COUNT];

static void hex32(char *out, size_t cap, uintptr_t p)
{
    uint8_t b[32];
    if (!mem_read(p, b, 32)) { scopy(out, cap, "(unreadable)"); return; }
    static const char *H = "0123456789abcdef";
    size_t o = 0;
    for (int i = 0; i < 32 && o + 3 < cap; i++) { out[o++] = H[b[i] >> 4]; out[o++] = H[b[i] & 15]; if ((i & 7) == 7) out[o++] = ' '; }
    out[o] = 0;
}
static void str_at(char *out, size_t cap, uintptr_t p)
{
    char s[64];
    out[0] = 0;
    if (!mem_read(p, s, sizeof s)) return;
    s[63] = 0;
    for (int i = 0; s[i]; i++) if ((unsigned char)s[i] < 32 || (unsigned char)s[i] > 126) return;
    if (slen(s) >= 3) scopy(out, cap, s);
}

/* regs: [0]=r9 [1]=r8 [2]=rdx [3]=rcx [4]=return address into the game */
static void probe_log(uint64_t *regs, uint32_t id)
{
    if (id >= P_COUNT) return;
    double t = now();
    p_count[id]++;
    if (id == P_IR_ENTER) g_in_replay = 1;
    if (id == P_FOCUS) g_replay_cam = (void *)(uintptr_t)regs[3];
    if (!S.debug) return; /* the details below are for testing only */
    static uint64_t last_rdx[P_COUNT];
    if (id == P_IR_UPDATE && t - p_last[id] < 1.0) return; /* once a second */
    if ((id == P_FOCUS || id == P_PAUSE_CMD) && regs[2] == last_rdx[id] && t - p_last[id] < 1.0) return; /* repeats */
    last_rdx[id] = regs[2];
    p_last[id] = t;
    uint64_t rcx = regs[3], rdx = regs[2], r8 = regs[1], r9 = regs[0], ret = regs[4];
    uint64_t vt = 0;
    mem_read((uintptr_t)rcx, &vt, 8);
    char a[80] = "", b[80] = "", h[100] = "";
    if (id == P_PAUSE_CMD) {
        /* the command: try arg2 as a string, as a pointer to a string, and dump it */
        uint64_t p2 = 0;
        str_at(a, sizeof a, (uintptr_t)rdx);
        if (mem_read((uintptr_t)rdx, &p2, 8)) str_at(b, sizeof b, (uintptr_t)p2);
        hex32(h, sizeof h, (uintptr_t)rdx);
        /* the command object: arg2 -> { ptr, kind } ; show what ptr points at */
        if (p2) {
            char h2[100] = "", s1[80] = "", s2[80] = "";
            uint64_t q = 0;
            hex32(h2, sizeof h2, (uintptr_t)p2);
            str_at(s1, sizeof s1, (uintptr_t)p2);
            if (mem_read((uintptr_t)p2, &q, 8)) str_at(s2, sizeof s2, (uintptr_t)q);
            uint64_t q8 = 0; char s3[80] = "";
            if (mem_read((uintptr_t)p2 + 8, &q8, 8)) str_at(s3, sizeof s3, (uintptr_t)q8);
            g_api->log(TAG "PROBE   command object %p: %s | str '%s' | *str '%s' | [+8] str '%s'", (void *)(uintptr_t)p2, h2, s1, s2, s3);
        }
    }
    g_api->log(TAG "PROBE %s #%d: rcx %p (vt KB %p) rdx %p r8 %p r9 %p, called from KB %p%s%s%s%s | %s", P_NAME[id], (int)p_count[id],
               (void *)rcx, (void *)(uintptr_t)(vt ? vt - g_base + KB_BASE : 0), (void *)rdx, (void *)r8, (void *)r9,
               (void *)(uintptr_t)(ret - g_base + KB_BASE), a[0] ? " str '" : "", a, b[0] ? "' ptr-str '" : "", b, h);
}

/* a small machine-code stub: save the argument registers (and xmm0-3), call probe_log(regs, id), restore, jump on */
static uint8_t *make_stub(uint32_t id, void *jump_to)
{
    uint8_t *m = (uint8_t *)VirtualAlloc(0, 256, MEM_COMMIT_RESERVE, PAGE_RWX);
    if (!m) return 0;
    uint8_t *p = m;
#define B(...) do { const uint8_t x_[] = {__VA_ARGS__}; memcpy(p, x_, sizeof x_); p += sizeof x_; } while (0)
    B(0x51, 0x52, 0x41, 0x50, 0x41, 0x51);                 /* push rcx, rdx, r8, r9 */
    B(0x48, 0x83, 0xEC, 0x68);                             /* sub rsp, 0x68 */
    B(0xF3, 0x0F, 0x7F, 0x44, 0x24, 0x20);                 /* movdqu [rsp+20], xmm0 */
    B(0xF3, 0x0F, 0x7F, 0x4C, 0x24, 0x30);                 /* xmm1 */
    B(0xF3, 0x0F, 0x7F, 0x54, 0x24, 0x40);                 /* xmm2 */
    B(0xF3, 0x0F, 0x7F, 0x5C, 0x24, 0x50);                 /* xmm3 */
    B(0x48, 0x8D, 0x4C, 0x24, 0x68);                       /* lea rcx, [rsp+68] (saved r9 r8 rdx rcx ret) */
    B(0xBA); memcpy(p, &id, 4); p += 4;                    /* mov edx, id */
    B(0x48, 0xB8); { uint64_t f = (uint64_t)(uintptr_t)probe_log; memcpy(p, &f, 8); p += 8; } /* mov rax, probe_log */
    B(0xFF, 0xD0);                                         /* call rax */
    B(0xF3, 0x0F, 0x6F, 0x44, 0x24, 0x20);
    B(0xF3, 0x0F, 0x6F, 0x4C, 0x24, 0x30);
    B(0xF3, 0x0F, 0x6F, 0x54, 0x24, 0x40);
    B(0xF3, 0x0F, 0x6F, 0x5C, 0x24, 0x50);
    B(0x48, 0x83, 0xC4, 0x68);                             /* add rsp, 0x68 */
    B(0x41, 0x59, 0x41, 0x58, 0x5A, 0x59);                 /* pop r9, r8, rdx, rcx */
    B(0xFF, 0x25, 0x00, 0x00, 0x00, 0x00); { uint64_t j = (uint64_t)(uintptr_t)jump_to; memcpy(p, &j, 8); p += 8; } /* jmp [rip] */
#undef B
    FlushInstructionCache(GetCurrentProcess(), m, (size_t)(p - m));
    return m;
}

/* replace a vtable slot, after checking it holds the function we expect */
static void hook_slot(uint32_t id, uint64_t kb_slot, uint64_t kb_fn)
{
    uintptr_t slot = rva(kb_slot), fn = rva(kb_fn);
    uint64_t cur = 0;
    if (!mem_read(slot, &cur, 8) || cur != fn) {
        LOG("PROBE %s: vtable slot holds %p, expected %p: hook OFF", P_NAME[id], (void *)(uintptr_t)cur, (void *)fn);
        return;
    }
    uint8_t *stub = make_stub(id, (void *)fn);
    DWORD old = 0;
    if (!stub || !VirtualProtect((void *)slot, 8, PAGE_RWX, &old)) { LOG("PROBE %s: can't patch the slot", P_NAME[id]); return; }
    p_orig[id] = (void *)fn;
    *(volatile uint64_t *)slot = (uint64_t)(uintptr_t)stub;
    VirtualProtect((void *)slot, 8, old, &old);
    if (S.debug) LOG("PROBE %s: vtable hook on", P_NAME[id]);
}

/* patch the start of a function whose first n bytes (n >= 14, whole instructions, nothing rip-relative) we know */
static void hook_inline(uint32_t id, uint64_t kb_fn, const uint8_t *expect, int n)
{
    uintptr_t fn = rva(kb_fn);
    uint8_t cur[32];
    if (n < 14 || n > 32 || !mem_read(fn, cur, (size_t)n)) return;
    for (int i = 0; i < n; i++)
        if (cur[i] != expect[i]) { LOG("PROBE %s: unexpected bytes at %p: hook OFF", P_NAME[id], (void *)fn); return; }
    uint8_t *tramp = (uint8_t *)VirtualAlloc(0, 64, MEM_COMMIT_RESERVE, PAGE_RWX);
    if (!tramp) return;
    memcpy(tramp, cur, (size_t)n);
    tramp[n] = 0xFF, tramp[n + 1] = 0x25, tramp[n + 2] = tramp[n + 3] = tramp[n + 4] = tramp[n + 5] = 0;
    uint64_t back = (uint64_t)(fn + (uintptr_t)n);
    memcpy(tramp + n + 6, &back, 8);
    FlushInstructionCache(GetCurrentProcess(), tramp, (size_t)n + 14);
    uint8_t *stub = make_stub(id, tramp);
    if (!stub) return;
    uint8_t patch[32];
    patch[0] = 0xFF, patch[1] = 0x25, patch[2] = patch[3] = patch[4] = patch[5] = 0;
    uint64_t to = (uint64_t)(uintptr_t)stub;
    memcpy(patch + 6, &to, 8);
    for (int i = 14; i < n; i++) patch[i] = 0x90;
    DWORD old = 0;
    if (!VirtualProtect((void *)fn, (size_t)n, PAGE_RWX, &old)) return;
    memcpy((void *)fn, patch, (size_t)n);
    VirtualProtect((void *)fn, (size_t)n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void *)fn, (size_t)n);
    p_orig[id] = tramp;
    if (S.debug) LOG("PROBE %s: inline hook on", P_NAME[id]);
}

/* Swofty wraps for the bound functions */
typedef uint64_t (*Fn4)(uint64_t, uint64_t, uint64_t, uint64_t);
static Fn4 o_pause_req, o_pause, o_unpause, o_set_state;
static void log_state(const char *what, uint64_t st, uint64_t a2, uint64_t a3)
{
    uint64_t vt = 0;
    mem_read((uintptr_t)st, &vt, 8);
    g_api->log(TAG "PROBE %s: state %p (vt KB %p) arg2 %p arg3 %p", what, (void *)(uintptr_t)st,
               (void *)(uintptr_t)(vt ? vt - g_base + KB_BASE : 0), (void *)(uintptr_t)a2, (void *)(uintptr_t)a3);
}
static uint64_t w_pause_req(uint64_t a, uint64_t b, uint64_t c, uint64_t d) { log_state("OnPauseRequested", a, b, c); return o_pause_req(a, b, c, d); }
static uint64_t w_pause(uint64_t a, uint64_t b, uint64_t c, uint64_t d) { log_state("Pause", a, b, c); return o_pause(a, b, c, d); }
static uint64_t w_unpause(uint64_t a, uint64_t b, uint64_t c, uint64_t d) { log_state("Unpause", a, b, c); return o_unpause(a, b, c, d); }
static uint64_t w_set_state(uint64_t a, uint64_t b, uint64_t c, uint64_t d)
{
    g_replay_state = (int)(uint32_t)b;
    g_replay_state_t = now();
    if (S.debug) g_api->log(TAG "PROBE Replay.SetState: replay system %p, state %d, arg3 %d", (void *)(uintptr_t)a, (int)(uint32_t)b, (int)(uint32_t)c);
    return o_set_state(a, b, c, d);
}

static int probe_mixins(SwoftyMixin *m, int at)
{
    struct { const char *t; void *h; Fn4 *o; } w[] = {{"Ne_Replay_SetState", (void *)w_set_state, &o_set_state}};
    (void)w_pause_req, (void)w_pause, (void)w_unpause;
    for (unsigned i = 0; i < sizeof w / sizeof w[0]; i++) {
        if (!g_api->resolve || !g_api->resolve(w[i].t)) { LOG("PROBE %s not bound: no wrap", w[i].t); continue; }
        m[at].target = w[i].t;
        m[at].kind = SWOFTY_MIXIN_WRAP;
        m[at].handler = w[i].h;
        m[at].orig = (void **)w[i].o;
        at++;
    }
    return at;
}

static void probe_install(void)
{
    g_base = g_api->module_base ? g_api->module_base() : (uintptr_t)KB_BASE;
    hook_slot(P_IR_ENTER, 0x1457e3940ull, 0x142da21e0ull);
    if (S.debug) {
        hook_slot(P_IR_UPDATE, 0x1457e3948ull, 0x142da2760ull);
        hook_slot(P_IR_EXIT, 0x1457e3950ull, 0x142da2de0ull);
        hook_slot(P_PAUSE_CMD, 0x1457cba40ull, 0x142bc10f0ull);
    }
    static const uint8_t focus_prologue[14] = {0x41, 0x57, 0x41, 0x56, 0x56, 0x57, 0x53, 0x48, 0x83, 0xEC, 0x70, 0x48, 0x89, 0xCE};
    hook_inline(P_FOCUS, 0x1406ae650ull, focus_prologue, 14);
    /* replays need to know when the game's replay screen is open: on another game version the hook stays off,
       and so do replays */
    if (!p_orig[P_IR_ENTER]) LOG("replays: this game version is not the one they were made for, replays are off");
}
static int replays_supported(void) { return p_orig[P_IR_ENTER] != 0; }
