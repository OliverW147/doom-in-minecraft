/* The bridge between Doom's sound calls and the server (see tools/SOUND_README.md).
 * Doom runs with -nosound, so S_StartSound and S_ChangeMusic only collect what Doom wants to play into small queues
 * (soundglue.c). The server empties the queues once per loop and turns the entries into Minecraft sound packets (sound.h).
 * This header is shared by C (soundglue.c) and C++ (sound.h). */
#ifndef SOUNDGLUE_H
#define SOUNDGLUE_H

#ifdef __cplusplus
extern "C" {
#endif

/* One sound Doom wants to play. Positions are Doom map units, z is the height. */
typedef struct DgsEvent {
    int sfx;            /* index into S_sfx (sounds.h): 1 = pistol ... */
    int hasOrigin;      /* 0 = no place (menu, intermission): play it centred */
    int selfOrigin;     /* 1 = the listener's own mobj or no origin: the player made it */
    double x, y, z;     /* where the sound starts (the listener's own position for selfOrigin) */
    double lx, ly, lz;  /* the listener: player position and eye height */
    double angle;       /* the listener's facing in degrees (Doom: 0 = east, counter clockwise) */
    int level;          /* Episode 1 map number 1..9 */
    int map8;           /* Doom never clips sounds on E1M8 */
} DgsEvent;

/* A music request: start (name "e1m1", "inter", "intro", "victor"...) or stop. */
typedef struct DgsMusic {
    int stop;
    int looping;
    char name[16];
} DgsMusic;

/* Called by the patched Doom code (s_sound.c). */
void dgs_hook_sound(void *origin, int sfx_id);
void dgs_hook_music(const char *name, int looping);
void dgs_hook_music_stop(void);
void dgs_hook_volume(int which /* 0 = effects, 1 = music */, int volume /* 0..127, the menu sliders times 8 */);

/* Called by the server: 1 and fills *out while entries are waiting. */
int dgs_pop_sound(DgsEvent *out);
int dgs_pop_music(DgsMusic *out);
int dgs_pop_volume(int *which, int *volume);

#ifdef __cplusplus
}
#endif
#endif
