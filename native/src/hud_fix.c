/* Keep the game's spectator HUD (timing tower) on screen while the broadcast runs.
 * Ported from Spectator/Broadcast Mode by Kizi & RK (MIT): the game keeps two "reasons to hide the HUD" masks; the
 * in-game spectating state sets reason 0 every frame, which hides both HUDs. While we broadcast, the immediate 1 of each
 * instruction that sets reason 0 is changed to 0 (put back when the broadcast stops), reason 0 is cleared, and the game's
 * own HUD visibility update is called. */

__declspec(dllimport) size_t __stdcall VirtualQuery(const void *, void *, size_t);
typedef struct { void *base, *alloc_base; DWORD alloc_protect; uint16_t part; size_t region; DWORD state, protect, type; } MBI;

static uintptr_t hm_addr;
typedef void (*HudUpdateFn)(void *);
static HudUpdateFn hm_update;
static uintptr_t hb_sites[64];
static int hb_n, hb_on, hm_found;
static double hm_fix_at;

static void hm_find(void)
{
    hm_found = 1;
    uintptr_t at = 0;
    int hits = g_api->scan(".xtls", "8B 05 ?? ?? ?? ?? A8 40 0F 85 ?? ?? ?? ?? 80 3D ?? ?? ?? ?? 01 0F 85 ?? ?? ?? ?? B9 D4 00 00 00", &at);
    if (hits == 1) {
        int32_t rel = 0;
        if (mem_read(at + 2, &rel, 4)) hm_addr = at + 6 + (intptr_t)rel - 4;
    }
    uintptr_t fn = 0;
    int h2 = g_api->scan(".xtls", "56 57 53 48 83 EC 30 48 89 CE 83 39 00 0F 94 44 24 2F 48 8B 79 68 48 85 FF 74 ?? 48 3B 3D ?? ?? ?? ??", &fn);
    if (h2 == 1) hm_update = (HudUpdateFn)fn;
    LOG("HUD hide masks %s (%d), HUD update %s (%d)", hm_addr ? "found" : "NOT found", hits, hm_update ? "found" : "NOT found", h2);
    if (!hm_addr) return;
    /* the instructions that set reason 0: "or eax, 1 ; mov [mask], eax" and "cmove rbx, rdi ; or byte [rbx], 1 ; jmp" */
    uintptr_t mod = g_api->module_base ? g_api->module_base() : 0;
    if (!mod) return;
    uint32_t pe = 0;
    mem_read(mod + 0x3c, &pe, 4);
    uint16_t ns = 0, opt = 0;
    mem_read(mod + pe + 6, &ns, 2);
    mem_read(mod + pe + 20, &opt, 2);
    for (int i = 0; i < ns && i < 40; i++) {
        uint8_t sh[40];
        if (!mem_read(mod + pe + 24 + opt + (uintptr_t)i * 40, sh, 40)) continue;
        uint32_t vsz, va, ch;
        memcpy(&vsz, sh + 8, 4);
        memcpy(&va, sh + 12, 4);
        memcpy(&ch, sh + 36, 4);
        if (!(ch & 0x20000000u)) continue; /* executable */
        const uint8_t *b = (const uint8_t *)(mod + va);
        MBI mbi;
        memset(&mbi, 0, sizeof mbi);
        if (!VirtualQuery(b, &mbi, sizeof mbi) || mbi.state != 0x1000) continue;
        for (size_t k = 0; k + 9 <= vsz && hb_n < 64; k++) {
            if (b[k] == 0x48 && b[k + 1] == 0x0F && b[k + 2] == 0x44 && b[k + 3] == 0xDF && b[k + 4] == 0x80 && b[k + 5] == 0x0B &&
                b[k + 6] == 0x01 && b[k + 7] == 0xEB) {
                hb_sites[hb_n++] = (uintptr_t)(b + k + 6);
                continue;
            }
            if (b[k] != 0x83 || b[k + 1] != 0xC8 || b[k + 2] != 0x01 || b[k + 3] != 0x89 || b[k + 4] != 0x05) continue;
            int32_t rel;
            memcpy(&rel, b + k + 5, 4);
            uintptr_t target = (uintptr_t)(b + k + 9) + (intptr_t)rel;
            if (target == hm_addr || target == hm_addr + 4) hb_sites[hb_n++] = (uintptr_t)(b + k + 2);
        }
    }
    LOG("HUD block: %d place(s) in the game code set hide reason 0", hb_n);
}

static void hb_set(int on)
{
    if (on == hb_on || !hb_n) return;
    int ok = 0;
    for (int i = 0; i < hb_n; i++) {
        DWORD old = 0;
        if (!VirtualProtect((void *)hb_sites[i], 1, 0x40, &old)) continue;
        *(volatile uint8_t *)hb_sites[i] = on ? 0x00 : 0x01;
        VirtualProtect((void *)hb_sites[i], 1, old, &old);
        FlushInstructionCache(GetCurrentProcess(), (void *)hb_sites[i], 1);
        ok++;
    }
    hb_on = on;
    LOG("HUD block %s: %d of %d place(s)", on ? "ON" : "OFF", ok, hb_n);
}

static void hud_keep_up(void)
{
    if (!hm_found) hm_find();
    hb_set(1);
    if (!hm_addr || !hm_update) return;
    uint32_t m[2] = {0, 0};
    if (!mem_read(hm_addr, m, 8) || !((m[0] | m[1]) & 1u)) return;
    if (now() - hm_fix_at < 0.25) return;
    hm_fix_at = now();
    __atomic_fetch_and((volatile uint32_t *)hm_addr, ~1u, __ATOMIC_SEQ_CST);
    __atomic_fetch_and((volatile uint32_t *)(hm_addr + 4), ~1u, __ATOMIC_SEQ_CST);
    hm_update((void *)hm_addr);
}
