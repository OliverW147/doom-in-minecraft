/* Collects Doom's sound and music calls for the server (see soundglue.h and tools/SOUND_README.md).
 * Kept in C because it reads Doom's own structures; no windows.h on purpose (Doom's "boolean" clashes with it). */
#include <string.h>
#include <stdlib.h>
#include "doomstat.h"
#include "d_player.h"
#include "p_mobj.h"
#include "r_defs.h"
#include "sounds.h"
#include "soundglue.h"

#define SQ 256
static DgsEvent sq[SQ];
static unsigned sq_head, sq_tail;
static DgsMusic mq[16];
static unsigned mq_head, mq_tail;

void dgs_hook_sound(void *origin_p, int sfx_id)
{
    player_t *pl = &players[consoleplayer];
    mobj_t *lis = pl->mo;
    DgsEvent e;
    if (!lis || sfx_id < 1 || sfx_id >= NUMSFX) return;
    memset(&e, 0, sizeof e);
    e.sfx = sfx_id;
    e.hasOrigin = origin_p != NULL;
    e.selfOrigin = origin_p == NULL || origin_p == (void *)lis;
    if (origin_p && !e.selfOrigin) {
        /* mobj_t and degenmobj_t (sector sound origins) start with the same thinker, x, y, z */
        const degenmobj_t *o = (const degenmobj_t *)origin_p;
        e.x = o->x / 65536.0; e.y = o->y / 65536.0; e.z = o->z / 65536.0;
    } else {
        e.x = lis->x / 65536.0; e.y = lis->y / 65536.0; e.z = lis->z / 65536.0;
    }
    e.lx = lis->x / 65536.0; e.ly = lis->y / 65536.0; e.lz = pl->viewz / 65536.0;
    e.angle = (double)lis->angle * 360.0 / 4294967296.0;
    e.level = (gameepisode == 1 && gamemap >= 1 && gamemap <= 9) ? gamemap : 1;
    e.map8 = gamemap == 8;
    if (sq_head - sq_tail >= SQ) sq_tail++;        /* queue full: drop the oldest */
    sq[sq_head % SQ] = e;
    sq_head++;
}

static void push_music(const DgsMusic *m)
{
    if (mq_head - mq_tail >= 16) mq_tail++;
    mq[mq_head % 16] = *m;
    mq_head++;
}

void dgs_hook_music(const char *name, int looping)
{
    DgsMusic m;
    memset(&m, 0, sizeof m);
    m.looping = looping;
    strncpy(m.name, name, sizeof m.name - 1);
    push_music(&m);
}

void dgs_hook_music_stop(void)
{
    DgsMusic m;
    memset(&m, 0, sizeof m);
    m.stop = 1;
    push_music(&m);
}

int dgs_pop_sound(DgsEvent *out)
{
    if (sq_tail == sq_head) return 0;
    *out = sq[sq_tail % SQ];
    sq_tail++;
    return 1;
}

int dgs_pop_music(DgsMusic *out)
{
    if (mq_tail == mq_head) return 0;
    *out = mq[mq_tail % 16];
    mq_tail++;
    return 1;
}

/* The sliders of Doom's Options > Sound Volume menu: the server keeps the player's own sound and music level from them. */
static int vq_which[16], vq_vol[16];
static unsigned vq_head, vq_tail;

void dgs_hook_volume(int which, int volume)
{
    if (vq_head - vq_tail >= 16) vq_tail++;
    vq_which[vq_head % 16] = which;
    vq_vol[vq_head % 16] = volume;
    vq_head++;
}

int dgs_pop_volume(int *which, int *volume)
{
    if (vq_tail == vq_head) return 0;
    *which = vq_which[vq_tail % 16];
    *volume = vq_vol[vq_tail % 16];
    vq_tail++;
    return 1;
}
