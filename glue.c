/* Tiny bridge between doomgeneric and the server. Kept in C because it pokes at Doom internals.
 * No windows.h here on purpose: Doom's "boolean" typedef clashes with the Windows headers.
 * The platform hooks (sleep, clock) live in main.cpp. */
#include <stdint.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "doomgeneric.h"
#include "d_event.h"
#include "doomstat.h"
#include "d_player.h"
#include "p_mobj.h"
#include "r_defs.h"
#include "r_state.h"
#include "p_local.h"
#include "p_pspr.h"
#include "d_items.h"
#include "w_wad.h"
extern int gametic;
extern spritedef_t *sprites;
extern int numsprites;
extern boolean P_TeleportMove(mobj_t *thing, fixed_t x, fixed_t y);
extern subsector_t *R_PointInSubsector(fixed_t x, fixed_t y);

extern int mousex; /* g_game.c: consumed once per game tic by G_BuildTiccmd */

void dg_key(int down, int key, int ascii)
{
    event_t ev;
    ev.type = down ? ev_keydown : ev_keyup;
    ev.data1 = key;
    ev.data2 = ascii;
    ev.data3 = 0;
    ev.data4 = 0;
    D_PostEvent(&ev);
}

/* Doom keeps only the LAST mouse event per tic, so only post when the previous one was consumed. */
int dg_mouse_free(void)
{
    return mousex == 0;
}

void dg_mouse(int units)
{
    event_t ev;
    ev.type = ev_mouse;
    ev.data1 = 0;
    ev.data2 = units;
    ev.data3 = 0;
    ev.data4 = 0;
    D_PostEvent(&ev);
}

const uint32_t *dg_frame(void)
{
    return (const uint32_t *)DG_ScreenBuffer;
}

/* Doom player position (map units), eye height and facing in degrees. Returns 0 while there is no player yet. */
int dg_player_state(double *x, double *y, double *viewz, double *angleDeg)
{
    mobj_t *mo = players[consoleplayer].mo;
    if (!mo) return 0;
    *x = (double)mo->x / 65536.0;
    *y = (double)mo->y / 65536.0;
    *viewz = (double)players[consoleplayer].viewz / 65536.0;
    *angleDeg = (double)mo->angle * 360.0 / 4294967296.0;
    return 1;
}

/* The client renders the view and owns where it looks: set the Doom player's facing from it. */
void dg_set_angle(double deg)
{
    mobj_t *mo = players[consoleplayer].mo;
    if (!mo) return;
    while (deg < 0) deg += 360.0;
    while (deg >= 360.0) deg -= 360.0;
    angle_t na = (angle_t)(deg / 360.0 * 4294967296.0);
    /* Turning with the mouse is instant, Doom's momentum is not: carry the velocity round with the view,
     * so the player does not keep sliding in the old direction (the feel of a modern port, not 1993 ice skating). */
    int32_t delta = (int32_t)(na - mo->angle);
    if (delta != 0 && (mo->momx || mo->momy)) {
        double rad = (double)delta * 3.14159265358979323846 / 2147483648.0;
        double c = cos(rad), s = sin(rad);
        double mx = (double)mo->momx, my = (double)mo->momy;
        mo->momx = (fixed_t)(mx * c - my * s);
        mo->momy = (fixed_t)(mx * s + my * c);
    }
    mo->angle = na;
}

/* Test harness: put the player somewhere (map units, degrees) without any sound or fog, like a teleporter. */
void dg_teleport(double x, double y, double deg)
{
    player_t *pl = &players[consoleplayer];
    mobj_t *mo = pl->mo;
    if (!mo) return;
    pl->cheats |= CF_GODMODE;                   /* test harness: stand still in nukage without dying */
    if (pl->playerstate == PST_DEAD) pl->playerstate = PST_REBORN;
    if (pl->health < 100) { pl->health = 100; mo->health = 100; }
    P_TeleportMove(mo, (fixed_t)(x * 65536.0), (fixed_t)(y * 65536.0));
    mo->z = mo->floorz;                 /* a teleporter puts you on the floor of the new sector */
    mo->momx = mo->momy = mo->momz = 0;
    players[consoleplayer].viewz = mo->z + players[consoleplayer].viewheight;
    dg_set_angle(deg);
}

/* Which sector Doom's own BSP walk says (x, y) is in. */
int dg_sector_at(double x, double y)
{
    subsector_t *ss = R_PointInSubsector((fixed_t)(x * 65536.0), (fixed_t)(y * 65536.0));
    return (int)(ss->sector - sectors);
}

int dg_player_sector(void)
{
    mobj_t *mo = players[consoleplayer].mo;
    if (!mo) return -1;
    return (int)(mo->subsector->sector - sectors);
}

extern void R_SetViewSize(int blocks, int detail);

/* Doom's 3D view over the whole screen (no status bar): then its geometry is a plain 4:3 pinhole, like the client renderer. */
void dg_fullview(void)
{
    R_SetViewSize(11, 0);
}

/* Doom game tic counter: advances 35 times a second. */
int dg_gametic(void)
{
    extern int gametic;
    return gametic;
}

int dg_player_health(void)
{
    return players[consoleplayer].health;
}

/* Current floor and ceiling height (map units) and light level of a sector, for the client renderer. */
void dg_sector_state(int sec, int *floor, int *ceil, int *light)
{
    sector_t *s = &sectors[sec];
    *floor = s->floorheight >> 16;
    *ceil = s->ceilingheight >> 16;
    *light = s->lightlevel;
}

/* Name of the sector's current floor flat (8 characters, not terminated): donuts and some lifts change it while they move. */
extern int firstflat;
extern lumpinfo_t *lumpinfo;
const char *dg_floor_flat_name(int sec)
{
    return lumpinfo[firstflat + sectors[sec].floorpic].name;
}

/* Test harness: force a sector's heights (map units). */
void dg_set_sector(int sec, int floor, int ceil)
{
    sectors[sec].floorheight = floor << 16;
    sectors[sec].ceilingheight = ceil << 16;
}

/* Test harness: keep the player exactly where it is. */
void dg_zero_momentum(void)
{
    mobj_t *mo = players[consoleplayer].mo;
    if (mo) mo->momx = mo->momy = mo->momz = 0;
}

/* The visible things around the player for the client renderer: x, y, z (map units), sprite lump (relative to the first
 * sprite lump), flags (1 = mirrored, 2 = full bright, 4 = fuzzy) and the sector light level, six ints each, nearest
 * first. The sprite frame and rotation are picked exactly like Doom's R_ProjectSprite does it, as seen from the player. */
int dg_things(int *out, int maxn, double maxdist)
{
    player_t *pl = &players[consoleplayer];
    if (!pl->mo) return 0;
    static struct { double d; int rec[6]; } best[64];
    int n = 0;
    if (maxn > 64) maxn = 64;
    for (thinker_t *th = thinkercap.next; th != &thinkercap; th = th->next) {
        if (th->function.acp1 != (actionf_p1)P_MobjThinker) continue;
        mobj_t *mo = (mobj_t *)th;
        if (mo == pl->mo || (mo->flags & MF_NOSECTOR)) continue;
        if (mo->sprite < 0 || mo->sprite >= numsprites) continue;
        spritedef_t *sd = &sprites[mo->sprite];
        int fr = mo->frame & FF_FRAMEMASK;
        if (fr >= sd->numframes) continue;
        spriteframe_t *sf = &sd->spriteframes[fr];
        int rot = 0;
        if (sf->rotate) {
            angle_t ang = R_PointToAngle2(pl->mo->x, pl->mo->y, mo->x, mo->y);
            rot = (int)((ang - mo->angle + (unsigned)(ANG45 / 2) * 9) >> 29);
        }
        double dx = (double)(mo->x - pl->mo->x) / 65536.0, dy = (double)(mo->y - pl->mo->y) / 65536.0;
        double d2 = dx * dx + dy * dy;
        if (d2 > maxdist * maxdist) continue;
        int rec[6] = {mo->x >> 16, mo->y >> 16, mo->z >> 16, sf->lump[rot],
                      (sf->flip[rot] ? 1 : 0) | ((mo->frame & FF_FULLBRIGHT) ? 2 : 0) | ((mo->flags & MF_SHADOW) ? 4 : 0),
                      mo->subsector->sector->lightlevel};
        int pos = n < maxn ? n : maxn - 1;
        if (n >= maxn && d2 >= best[maxn - 1].d) continue;
        while (pos > 0 && best[pos - 1].d > d2) { best[pos] = best[pos - 1]; pos--; }
        best[pos].d = d2;
        memcpy(best[pos].rec, rec, sizeof rec);
        if (n < maxn) n++;
    }
    for (int i = 0; i < n; i++) memcpy(out + 6 * i, best[i].rec, 6 * sizeof(int));
    return n;
}

/* Everything the status bar and the weapon need: health, armor, ammo, keys, weapons, face, palette flash, weapon sprites. */
void dg_hud(int *o)
{
    player_t *pl = &players[consoleplayer];
    o[0] = pl->health;
    o[1] = pl->armorpoints;
    ammotype_t at = weaponinfo[pl->readyweapon].ammo;
    o[2] = (at == am_noammo) ? -1 : pl->ammo[at];
    for (int i = 0; i < 4; i++) { o[3 + i] = pl->ammo[i]; o[7 + i] = pl->maxammo[i]; }
    o[11] = 0;
    for (int i = 0; i < 6; i++) if (pl->cards[i]) o[11] |= 1 << i;
    o[12] = 0;
    for (int i = 0; i < 6; i++) if (pl->weaponowned[i + 1]) o[12] |= 1 << i;
    int pain = pl->health >= 80 ? 0 : pl->health >= 60 ? 1 : pl->health >= 40 ? 2 : pl->health >= 20 ? 3 : 4;
    o[13] = pl->health <= 0 ? 15 : (pl->cheats & CF_GODMODE) ? 16 : pain * 3 + (gametic / 17) % 3;
    int cnt = pl->damagecount;
    if (pl->powers[pw_strength]) { int bzc = 12 - (pl->powers[pw_strength] >> 6); if (bzc > cnt) cnt = bzc; }
    int pal = 0;
    if (cnt) { pal = (cnt + 7) >> 3; if (pal >= 8) pal = 7; pal += 1; }
    else if (pl->bonuscount) { pal = (pl->bonuscount + 7) >> 3; if (pal >= 4) pal = 3; pal += 9; }
    else if (pl->powers[pw_ironfeet] > 4 * 32 || (pl->powers[pw_ironfeet] & 8)) pal = 13;
    o[14] = pal;
    o[15] = pl->readyweapon;
    for (int k = 0; k < 2; k++) {                       /* weapon, then muzzle flash */
        pspdef_t *psp = &pl->psprites[k];
        int *q = o + 16 + 4 * k;
        q[0] = -1; q[1] = q[2] = q[3] = 0;
        if (!psp->state) continue;
        spritedef_t *sd = &sprites[psp->state->sprite];
        int fr = psp->state->frame & FF_FRAMEMASK;
        if (fr >= sd->numframes) continue;
        q[0] = sd->spriteframes[fr].lump[0];
        q[1] = (psp->state->frame & FF_FULLBRIGHT) ? 1 : 0;
        q[2] = psp->sx >> 16;
        q[3] = psp->sy >> 16;
    }
}

/* 1 while Doom shows something other than the 3D game view: menu, automap, intermission, finale, title. */
extern int dg_wipe_active_flag;   /* d_main.c: the screen melt is running */
int dg_screen_mode(void)
{
    return menuactive || automapactive || gamestate != GS_LEVEL || dg_wipe_active_flag;
}

int dg_wipe_active(void)
{
    return dg_wipe_active_flag;
}

/* The title picture without a menu on top of it. */
int dg_on_title(void)
{
    return gamestate == GS_DEMOSCREEN && !menuactive && !dg_wipe_active_flag;
}

/* Text on the title picture (called by D_Display): how to get going. */
extern void M_WriteText(int x, int y, char *string);
extern int M_StringWidth(char *string);
extern int dgs_exists(int slot);
void dg_title_overlay(void)
{
    static char l1[] = "PRESS F FOR THE MENU";
    static char l2[] = "JUMP TO CONTINUE YOUR GAME";
    if (menuactive) return;
    M_WriteText((320 - M_StringWidth(l1)) / 2, 172, l1);
    if (dgs_exists(6)) M_WriteText((320 - M_StringWidth(l2)) / 2, 186, l2);
}

/* ---------------------------------------------------------------------------------------------------------------
 * Save games. Doom writes them as files; here they are memory buffers kept by the server (main.cpp, shared between the
 * per player processes, keyed by the player name). Slots 0..5 are the menu slots, 6 is the autosave.
 * The three files that touch save games (g_game.c, m_menu.c) call these instead of fopen/remove/rename. */
#define DGS_MAX 98304
#define SLOT_TEMP (-2)
extern int dgs_read(int slot, unsigned char *buf, int cap);
extern void dgs_write(int slot, const unsigned char *buf, int len);
extern void dgs_delete(int slot);
extern int dgs_exists(int slot);
extern int dgv_autosave(void);
extern void G_LoadGame(char *name);

static unsigned char *g_tmp;
static int g_tmplen;
static FILE *g_wfile, *g_rfile;
static int g_wslot;
static unsigned char *g_rbuf;
#ifndef _WIN32
static char *g_wbuf;
static size_t g_wlen;
#endif

static int vfs_slot(const char *name)
{
    const char *p = strstr(name, "doomsav");
    if (p) { int n = atoi(p + 7); return (n >= 0 && n <= 6) ? n : -1; }
    return strstr(name, "temp.dsg") ? SLOT_TEMP : -1;
}

FILE *dgv_fopen(const char *name, const char *mode)
{
    int slot = vfs_slot(name);
    if (slot == -1) return NULL;
    if (mode[0] == 'w') {
        if (g_wfile) return NULL;
        g_wslot = slot;
#ifdef _WIN32
        g_wfile = fopen("dgv_tmp.bin", "w+b");
#else
        g_wbuf = NULL; g_wlen = 0;
        g_wfile = open_memstream(&g_wbuf, &g_wlen);
#endif
        return g_wfile;
    }
    if (g_rfile) return NULL;
    unsigned char *data = (unsigned char *)malloc(DGS_MAX);
    int len;
    if (!data) return NULL;
    if (slot == SLOT_TEMP) { len = g_tmp ? g_tmplen : -1; if (len > 0) memcpy(data, g_tmp, (size_t)len); }
    else len = dgs_read(slot, data, DGS_MAX);
    if (len <= 0) { free(data); return NULL; }
    g_rbuf = data;
#ifdef _WIN32
    g_rfile = fopen("dgv_tmp.bin", "w+b");
    if (g_rfile) { fwrite(data, 1, (size_t)len, g_rfile); rewind(g_rfile); }
#else
    g_rfile = fmemopen(data, (size_t)len, "rb");
#endif
    if (!g_rfile) { free(data); g_rbuf = NULL; }
    return g_rfile;
}

int dgv_fclose(FILE *f)
{
    if (f && f == g_wfile) {
        unsigned char *buf = NULL;
        long n = 0;
#ifdef _WIN32
        fflush(f); fseek(f, 0, SEEK_END); n = ftell(f); rewind(f);
        buf = (unsigned char *)malloc((size_t)(n > 0 ? n : 1));
        if (buf && n > 0) fread(buf, 1, (size_t)n, f);
        fclose(f); remove("dgv_tmp.bin");
#else
        fclose(f);
        buf = (unsigned char *)g_wbuf; n = (long)g_wlen;
#endif
        g_wfile = NULL;
        if (buf && n > 0 && n <= DGS_MAX) {
            if (g_wslot == SLOT_TEMP) {
                free(g_tmp);
                g_tmp = (unsigned char *)malloc((size_t)n);
                if (g_tmp) { memcpy(g_tmp, buf, (size_t)n); g_tmplen = (int)n; } else g_tmplen = 0;
            } else dgs_write(g_wslot, buf, (int)n);
        }
        free(buf);   /* malloc'ed by us on Windows, by open_memstream on Linux */
        return 0;
    }
    if (f && f == g_rfile) {
        fclose(f);
#ifdef _WIN32
        remove("dgv_tmp.bin");
#endif
        free(g_rbuf); g_rbuf = NULL; g_rfile = NULL;
        return 0;
    }
    return f ? fclose(f) : 0;
}

int dgv_remove(const char *name)
{
    int slot = vfs_slot(name);
    if (slot >= 0) dgs_delete(slot);
    return 0;
}

int dgv_rename(const char *from, const char *to)
{
    int slot = vfs_slot(to);
    if (vfs_slot(from) == SLOT_TEMP && slot >= 0 && g_tmp && g_tmplen > 0) dgs_write(slot, g_tmp, g_tmplen);
    return 0;
}

/* "New game" in the menu: forget the autosave. */
void dgv_new_game(void)
{
    dgs_delete(6);
}

/* Server side hooks. */
int dg_autosave(void)
{
    return dgv_autosave();
}

int dg_has_autosave(void)
{
    return dgs_exists(6);
}

void dg_load_autosave(void)
{
    static char name[] = "doomsav6.dsg";
    extern void M_ClearMenus(void);
    G_LoadGame(name);   /* takes effect on the next tic */
    M_ClearMenus();
}

extern int dg_single_level;
void dg_set_single_level(int on)
{
    dg_single_level = on;
}

/* Which Episode 1 map is being played (1..9); the shader picks the level data with it. */
int dg_map(void)
{
    return (gameepisode == 1 && gamemap >= 1 && gamemap <= 9) ? gamemap : 1;
}

/* After the last level Doom falls back to its demo loop: start a fresh game instead. Returns 1 when it did. */
extern void G_DeferedInitNew(skill_t skill, int episode, int map);
extern void dgv_new_game(void);
int dg_restart_if_idle(void)
{
    if (gamestate != GS_DEMOSCREEN) return 0;
    dgv_new_game();
    G_DeferedInitNew(sk_medium, 1, 1);
    return 1;
}

/* test harness: jump to E1Mn */
void dg_warp(int map)
{
    G_DeferedInitNew(sk_medium, 1, map);
}

/* Hotbar slot (0..6) of the weapon Doom is holding or switching to: keys 1..7 pick the weapons, the chainsaw shares 1 with the fist. */
int dg_weapon_slot(void)
{
    player_t *pl = &players[consoleplayer];
    int w = pl->pendingweapon != wp_nochange ? (int)pl->pendingweapon : (int)pl->readyweapon;
    return w <= 6 ? w : (w == 7 ? 0 : 2);
}

int dg_num_sectors(void)
{
    return numsectors;
}

int dg_skill(void)
{
    extern skill_t gameskill;
    return (int)gameskill;
}

/* 1 when the switch line shows its pressed picture (a texture called SW2...): the first member of a texture is its name */
int dg_switch_on(int n)
{
    extern int numlines, numtextures;
    extern line_t *lines;
    extern side_t *sides;
    extern void **textures;
    if (n < 0 || n >= numlines || lines[n].sidenum[0] < 0) return 0;
    const side_t *s = &sides[lines[n].sidenum[0]];
    const int t[3] = {s->toptexture, s->midtexture, s->bottomtexture};
    for (int i = 0; i < 3; i++) {
        if (t[i] <= 0 || t[i] >= numtextures) continue;
        const char *nm = (const char *)textures[t[i]];
        if (nm[0] == 'S' && nm[1] == 'W' && nm[2] == '2') return 1;
    }
    return 0;
}

/* test harness: walk over a line (as a monster or the player would) */
extern void P_CrossSpecialLine(int linenum, int side, mobj_t *thing);
void dg_cross_line(int n)
{
    extern int numlines;
    extern line_t *lines;
    extern boolean P_UseSpecialLine(mobj_t *thing, line_t *line, int side);
    if (!players[consoleplayer].mo || n >= numlines) return;
    P_CrossSpecialLine(n, 0, players[consoleplayer].mo);              /* walk triggers */
    P_UseSpecialLine(players[consoleplayer].mo, &lines[n], 0);        /* switches and doors */
}
