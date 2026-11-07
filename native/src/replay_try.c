/* Replay trigger experiment (test builds). What the probe showed: choosing Instant Replay in the pause menu makes the
 * pause UI flow move to its "replay" route; the session's pause handler (SessionStatePause::onPauseFlowStateChange) reads
 * the UI value ui_menu_data.frontend_transition_state and keeps the game paused for "replay", and the session then enters
 * SessionStateViewInstantReplay. This experiment writes that value the way the UI does (Vm_Set_String, as Swofty's own
 * core writes UI strings; node lookup as in Spectator/Broadcast Mode by Kizi & RK):
 *   Ctrl+F9  : write "replay" only (no pause)
 *   Ctrl+F10 : open the pause menu (Esc), wait 1 s, then write "replay"
 *   Ctrl+F11 : drive the pause menu like a player: Esc, Down x N, Enter (N = setting "Test: Instant Replay position")
 * Keys are only sent once Ctrl is released, so Windows doesn't see Ctrl+Esc.
 * The probe logs whether the instant replay state was entered. */

/* a key sequence played from the frame tick: vk, extended flag, delay before it (s) */
typedef struct { uint16_t vk, ext; float wait; } KeyStep;
static KeyStep ks[16];
static int ks_n, ks_i;
static double ks_next;
static DWORD __stdcall key_thread2(void *p)
{
    uintptr_t v = (uintptr_t)p;
    unsigned vk = (unsigned)(v & 0xffff), ext = (unsigned)(v >> 16);
    KBINPUT in;
    memset(&in, 0, sizeof in);
    in.type = 1;
    in.wScan = (uint16_t)MapVirtualKeyW(vk, 0);
    in.dwFlags = 0x0008 | (ext ? 0x0001 : 0);
    SendInput(1, &in, sizeof in);
    Sleep(50);
    in.dwFlags |= 0x0002;
    SendInput(1, &in, sizeof in);
    return 0;
}
static void ks_play(const KeyStep *steps, int n)
{
    for (int i = 0; i < n && i < 16; i++) ks[i] = steps[i];
    ks_n = n;
    ks_i = 0;
    ks_next = now() + steps[0].wait;
}
static void ks_tick(void)
{
    if (ks_i >= ks_n) return;
    if ((GetAsyncKeyState(0x11) & 0x8000) || (GetAsyncKeyState(0x10) & 0x8000) || (GetAsyncKeyState(0x12) & 0x8000)) { ks_next = now() + 0.2; return; }
    if (now() < ks_next || !game_in_front()) return;
    HANDLE t = CreateThread(0, 0, key_thread2, (void *)(uintptr_t)(ks[ks_i].vk | (uintptr_t)ks[ks_i].ext << 16), 0, 0);
    if (t) CloseHandle(t);
    if (S.debug) LOG("replay keys: key %d sent (%d of %d)", ks[ks_i].vk, ks_i + 1, ks_n);
    ks_i++;
    if (ks_i < ks_n) ks_next = now() + ks[ks_i].wait;
}
static int k_try3;

typedef void **(*VmPath_t)(void *registry, void *root, const char *path);
typedef void *(*VmSetString_t)(void *registry, void *parent, const char *name, void **value, int overwrite);
typedef void (*MakeStr_t)(void **out, const char *text, uint64_t a, uint64_t b);
static uintptr_t vm_registry;
static VmPath_t vm_path;
static VmSetString_t vm_set_string;
static MakeStr_t make_str;
static int vm_ready, k_try1, k_try2;
static double try_at;
static const char *try_value;

static void vm_find(void)
{
    vm_registry = g_api->resolve ? g_api->resolve("g_Vm_Registry") : 0;
    vm_set_string = (VmSetString_t)(g_api->resolve ? g_api->resolve("Vm_Set_String") : 0);
    make_str = (MakeStr_t)(g_api->resolve ? g_api->resolve("Ne_Make_StrFromCStr") : 0);
    uintptr_t at = 0;
    int hits = g_api->scan(".xtls", "E9 ?? ?? ?? ?? 18 36 B8 0F 88 D1 41", &at);
    if (hits != 1) {
        uintptr_t reg = 0;
        hits = g_api->scan(".xtls", "41 57 41 56 41 55 41 54 56 57 55 53 48 81 EC 88 02 00 00 4C 89 C3", &reg);
        const uint8_t *t = hits == 1 ? (const uint8_t *)(reg - 0xCCE0) : 0;
        uint8_t b0 = 0, b1 = 0;
        if (t) { mem_read((uintptr_t)t - 1, &b0, 1); mem_read((uintptr_t)t, &b1, 1); }
        at = (t && b0 == 0xcc && b1 == 0xe9) ? (uintptr_t)t : 0;
    }
    vm_path = (VmPath_t)at;
    vm_ready = vm_registry && vm_set_string && make_str && vm_path;
    LOG("REPLAY TRY: UI registry %s (registry %d, Vm_Set_String %d, Ne_Make_StrFromCStr %d, node path %d)", vm_ready ? "ready" : "NOT ready",
        vm_registry != 0, vm_set_string != 0, make_str != 0, vm_path != 0);
}

static int vm_string(const char *path, const char *leaf, const char *text)
{
    if (!vm_ready) return 0;
    void *reg = *(void **)vm_registry;
    uint8_t *root = reg ? *(uint8_t **)((uint8_t *)reg + 8) : 0;
    void **node = root ? vm_path(reg, root + 8, path) : 0;
    if (!node || !*node) { LOG("REPLAY TRY: node %s not found", path); return 0; }
    void *s = 0;
    make_str(&s, text, 0, 0);
    vm_set_string(reg, (uint8_t *)*node + 8, leaf, &s, 1);
    make_str(&s, 0, 0, 0);
    LOG("REPLAY TRY: %s.%s = \"%s\" written", path, leaf, text);
    return 1;
}

static void replay_try_tick(void)
{
    /* the test keys (Ctrl+F9/F10/F11) from the replay experiments are gone; only the key player the replays use runs */
    ks_tick();
}
