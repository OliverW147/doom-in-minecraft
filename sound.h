// Sound engine for the Doom Minecraft server: header only, C++17. See tools/SOUND_README.md for how to wire it in.
//
//  * Packet builders for the Minecraft 26.3 (protocol 777) packets Sound Effect (119), Entity Sound Effect (118) and Stop Sound (121).
//    The sound event is sent inline (holder id 0 + identifier), so no registry ids are needed.
//  * Positional logic: a Doom sound at some map position is placed in the listener's Minecraft space so that it comes from the right
//    direction and with the loudness Doom would use.
//  * A mapping from Doom sfx names to Minecraft sounds (tools/sound_map.json, made by tools/sound_chooser.html), with built in defaults.
//  * Music, selected by music.mode in the map: "real" plays the songs rendered to ogg into the resource pack (gen_music_real.h, made by
//    tools/gen_doom_music.py) through the Entity Sound Effect packet attached to the player, so they follow the player and the
//    stereo file is neither attenuated nor directional; "notes" is a note block player for the Doom songs (gen_music.h, made by
//    tools/mus_to_notes.py); "off" is silence.
//  * SoundRing: a small ring buffer for shared memory so players on the same level hear each other (multiplayer).
//
// The writer type W is the server's byte writer (vi, u8, i32, i64, f32, str); the engine calls emit(const W&) for every packet.
#pragma once
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "soundglue.h"
#include "gen_sound_defaults.h"   // DOOM_SFX_NAMES, SOUND_MAP_DEFAULT
#if __has_include("gen_sound_variants.h")
#include "gen_sound_variants.h"   // SND_EVENTS, SND_ENTRIES, SND_SEEDS: a seed for every variant of every embedded sound event
#define SOUND_HAVE_VARIANTS 1
#else
#define SOUND_HAVE_VARIANTS 0
#endif
#if __has_include("gen_music.h")
#include "gen_music.h"            // MUS_TRACKS
#define SOUND_HAVE_MUSIC 1
#else
#define SOUND_HAVE_MUSIC 0
#endif
#if __has_include("gen_music_real.h")
#include "gen_music_real.h"       // REAL_TRACKS: the songs rendered to ogg in the resource pack
#define SOUND_HAVE_REAL 1
#else
#define SOUND_HAVE_REAL 0
#endif

namespace snd {

// protocol 777 constants (read from the 26.3 client: GameProtocols)
constexpr uint32_t PKT_BUNDLE_DELIMITER = 0;
constexpr uint32_t PKT_SOUND_ENTITY = 118;
constexpr uint32_t PKT_SOUND = 119;
constexpr uint32_t PKT_STOP_SOUND = 121;

// net.minecraft.sounds.SoundSource ordinals
enum Source { MASTER = 0, MUSIC = 1, RECORDS = 2, WEATHER = 3, BLOCKS = 4, HOSTILE = 5, NEUTRAL = 6, PLAYERS = 7, AMBIENT = 8, VOICE = 9, UI = 10 };

inline int sourceFromName(const std::string &s, int fallback) {
    static const char *n[] = {"master", "music", "record", "weather", "block", "hostile", "neutral", "player", "ambient", "voice", "ui"};
    for (int i = 0; i < 11; i++) if (s == n[i]) return i;
    return fallback;
}

// SoundEvent holder written inline: varint 0, identifier string, optional fixed range (none)
template <class W> void writeInlineSound(W &w, const std::string &id) {
    w.vi(0).str(id).u8(0);
}

// Sound Effect: sound, source, x, y, z (fixed point, 1/8 block), volume, pitch, seed
template <class W> W soundAt(const std::string &id, int source, double x, double y, double z, float vol, float pitch, int64_t seed) {
    W w;
    w.vi(PKT_SOUND);
    writeInlineSound(w, id);
    w.vi((uint32_t)source).i32((int32_t)(x * 8.0)).i32((int32_t)(y * 8.0)).i32((int32_t)(z * 8.0)).f32(vol).f32(pitch).i64(seed);
    return w;
}

// Entity Sound Effect: the sound follows an entity (id from the server's own entities)
template <class W> W soundOnEntity(const std::string &id, int source, int entityId, float vol, float pitch, int64_t seed) {
    W w;
    w.vi(PKT_SOUND_ENTITY);
    writeInlineSound(w, id);
    w.vi((uint32_t)source).vi((uint32_t)entityId).f32(vol).f32(pitch).i64(seed);
    return w;
}

// Stop Sound: flags bit 0 = a source follows, bit 1 = a sound name follows. Both absent stops everything.
template <class W> W stopSound(int source /* -1 = any */, const std::string &id /* empty = any */) {
    W w;
    w.vi(PKT_STOP_SOUND);
    int flags = (source >= 0 ? 1 : 0) | (!id.empty() ? 2 : 0);
    w.u8(flags);
    if (source >= 0) w.vi((uint32_t)source);
    if (!id.empty()) w.str(id);
    return w;
}

// which variant of a sound event the client plays
// The client creates RandomSource.create(seed) = LegacyRandomSource (read from the 26.3 client): the Java LCG with the 0x5DEECE66D scramble.
// WeighedSoundEvents.getSound(random) draws nextInt(total weight) once, walks the weights of the event's list (files by their weight, a nested
// "event" entry by the total weight of that event) and for a nested event draws again from the same random. Volume and pitch of a variant are
// constants (ConstantFloat / MultipliedFloats), so the seed decides nothing else. gen_sound_variants.h holds a seed per variant.
struct JavaRandom {
    int64_t s;
    explicit JavaRandom(int64_t seed) : s((seed ^ 0x5DEECE66DLL) & ((1LL << 48) - 1)) {}
    int32_t next(int bits) {
        s = (s * 0x5DEECE66DLL + 0xBLL) & ((1LL << 48) - 1);
        return (int32_t)(s >> (48 - bits));
    }
    int32_t nextInt(int32_t bound) {   // BitRandomSource.nextInt(bound)
        if ((bound & (bound - 1)) == 0) return (int32_t)(((int64_t)bound * next(31)) >> 31);
        int32_t sample, modulo;
        do {
            sample = next(31);
            modulo = sample % bound;
        } while ((int32_t)((uint32_t)sample - (uint32_t)modulo + (uint32_t)(bound - 1)) < 0);   // int overflow wraps like in Java
        return modulo;
    }
};

#if SOUND_HAVE_VARIANTS
inline int findEventIndex(const std::string &name) {
    std::string n = name.compare(0, 10, "minecraft:") == 0 ? name.substr(10) : name;
    int lo = 0, hi = SND_NUM_EVENTS - 1;   // SND_EVENTS is sorted by name
    while (lo <= hi) {
        int mid = (lo + hi) / 2, c = strcmp(SND_EVENTS[mid].name, n.c_str());
        if (c == 0) return mid;
        if (c < 0) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}
inline int variantCount(const std::string &event) { int i = findEventIndex(event); return i < 0 ? 0 : SND_EVENTS[i].leafCount; }
// the seed that makes the client play variant `variant` (0 based, tree order) of the event; false if unknown
inline bool variantSeed(const std::string &event, int variant, int64_t &seed) {
    int i = findEventIndex(event);
    if (i < 0 || variant < 0 || variant >= SND_EVENTS[i].leafCount) return false;
    seed = SND_SEEDS[SND_EVENTS[i].seedStart + variant];
    return true;
}
inline int totalWeight(int ev, int depth = 0) {
    if (ev < 0 || depth > 6) return 0;
    int w = 0;
    for (int k = 0; k < SND_EVENTS[ev].entryCount; k++) {
        const SndEntry &e = SND_ENTRIES[SND_EVENTS[ev].entryStart + k];
        w += e.ref < 0 ? e.weight : totalWeight(e.ref, depth + 1);
    }
    return w;
}
// WeighedSoundEvents.getSound emulated: the leaf index (within the event) a client plays for this random, -1 for the empty sound
inline int pickLeaf(int ev, JavaRandom &rnd, int depth = 0) {
    int w = totalWeight(ev);
    if (ev < 0 || w == 0 || depth > 6) return -1;
    int index = rnd.nextInt(w);
    for (int k = 0; k < SND_EVENTS[ev].entryCount; k++) {
        const SndEntry &e = SND_ENTRIES[SND_EVENTS[ev].entryStart + k];
        index -= e.ref < 0 ? e.weight : totalWeight(e.ref, depth + 1);
        if (index < 0) {
            if (e.ref < 0) return e.leaf;
            int sub = pickLeaf(e.ref, rnd, depth + 1);
            return sub < 0 ? -1 : e.leaf + sub;
        }
    }
    return -1;
}
#else
inline int variantCount(const std::string &) { return 0; }
inline bool variantSeed(const std::string &, int, int64_t &) { return false; }
#endif

// a small JSON reader (for sound_map.json)
struct J {
    enum T { Null, Bool, Num, Str, Arr, Obj } t = Null;
    double n = 0;
    bool b = false;
    std::string s;
    std::vector<J> a;
    std::vector<std::pair<std::string, J>> o;
    const J *get(const char *k) const {
        for (auto &p : o) if (p.first == k) return &p.second;
        return nullptr;
    }
    double num(double d) const { return t == Num ? n : (t == Bool ? (b ? 1 : 0) : d); }
    std::string str(const std::string &d) const { return t == Str ? s : d; }
};

struct JParser {
    const char *p, *e;
    bool ok = true;
    JParser(const std::string &s) : p(s.data()), e(s.data() + s.size()) {}
    void ws() { while (p < e && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++; }
    bool lit(const char *w) { size_t n = strlen(w); if ((size_t)(e - p) >= n && !strncmp(p, w, n)) { p += n; return true; } return false; }
    std::string rawString() {
        std::string r;
        p++;   // opening quote
        while (p < e && *p != '"') {
            if (*p == '\\' && p + 1 < e) {
                p++;
                switch (*p) {
                case 'n': r += '\n'; break; case 't': r += '\t'; break; case 'r': r += '\r'; break;
                case 'u': { unsigned v = 0; for (int i = 1; i <= 4 && p + i < e; i++) v = v * 16 + (unsigned)(isdigit((unsigned char)p[i]) ? p[i] - '0' : (tolower(p[i]) - 'a' + 10)); p += 4; if (v < 128) r += (char)v; else r += '?'; break; }
                default: r += *p;
                }
                p++;
            } else r += *p++;
        }
        if (p < e) p++;
        return r;
    }
    J value(int depth = 0) {
        J j;
        ws();
        if (p >= e || depth > 32) { ok = false; return j; }
        if (*p == '{') {
            j.t = J::Obj; p++; ws();
            if (p < e && *p == '}') { p++; return j; }
            while (p < e) {
                ws();
                if (*p != '"') { ok = false; return j; }
                std::string k = rawString();
                ws();
                if (p >= e || *p != ':') { ok = false; return j; }
                p++;
                j.o.emplace_back(k, value(depth + 1));
                ws();
                if (p < e && *p == ',') { p++; continue; }
                if (p < e && *p == '}') { p++; break; }
                ok = false; return j;
            }
        } else if (*p == '[') {
            j.t = J::Arr; p++; ws();
            if (p < e && *p == ']') { p++; return j; }
            while (p < e) {
                j.a.push_back(value(depth + 1));
                ws();
                if (p < e && *p == ',') { p++; continue; }
                if (p < e && *p == ']') { p++; break; }
                ok = false; return j;
            }
        } else if (*p == '"') { j.t = J::Str; j.s = rawString(); }
        else if (lit("true")) { j.t = J::Bool; j.b = true; }
        else if (lit("false")) { j.t = J::Bool; j.b = false; }
        else if (lit("null")) { j.t = J::Null; }
        else {
            char *end = nullptr;
            j.n = strtod(p, &end);
            if (end == p) { ok = false; return j; }
            j.t = J::Num; p = end;
        }
        return j;
    }
};

// configuration
// variant: which random variation of the event plays (index in the event's variant list, tree order); -1 = random (a fresh seed every time, like the game)
struct Layer { std::string event; float volume = 1, pitch = 1; int delay = 0; int variant = -1; };
struct SfxDef { std::vector<Layer> layers; int source = HOSTILE; float pitchVar = 0; };
struct RoleDef { std::string instrument = "harp"; int octave = 0; float volume = 1; };
struct DoomCfg { double clip = 1200, close = 200, range = 16, map8floor = 0.25; bool useZ = false; int maxPerTick = 6; };

struct SoundMap {
    std::map<std::string, SfxDef> sfx;
    std::map<std::string, RoleDef> roles;
    std::map<std::string, bool> trackOn;
    DoomCfg doom;
    bool musicEnabled = true;
    float musicVolume = 0.8f;
    int musicSource = MUSIC;
    std::string musicMode = "real";   // "real" (ogg songs of the resource pack), "notes" (note blocks) or "off"
    float realVolume = -1.f;          // volume of the real songs; negative: the same as musicVolume

    bool parse(const std::string &text) {
        JParser jp(text);
        J root = jp.value();
        if (!jp.ok || root.t != J::Obj) return false;
        if (const J *d = root.get("doom")) {
            doom.clip = d->get("clip_dist") ? d->get("clip_dist")->num(doom.clip) : doom.clip;
            doom.close = d->get("close_dist") ? d->get("close_dist")->num(doom.close) : doom.close;
            doom.range = d->get("range_blocks") ? d->get("range_blocks")->num(doom.range) : doom.range;
            doom.map8floor = d->get("map8_floor") ? d->get("map8_floor")->num(doom.map8floor) : doom.map8floor;
            doom.useZ = d->get("use_z") ? d->get("use_z")->num(0) != 0 : doom.useZ;
            doom.maxPerTick = d->get("max_per_tick") ? (int)d->get("max_per_tick")->num(doom.maxPerTick) : doom.maxPerTick;
        }
        if (const J *s = root.get("sfx")) {
            for (auto &kv : s->o) {
                SfxDef def;
                def.source = sourceFromName(kv.second.get("source") ? kv.second.get("source")->str("") : "", HOSTILE);
                def.pitchVar = kv.second.get("pitch_var") ? (float)kv.second.get("pitch_var")->num(0) : 0.f;
                if (const J *ls = kv.second.get("layers")) {
                    for (auto &l : ls->a) {
                        Layer ly;
                        ly.event = l.get("event") ? l.get("event")->str("") : "";
                        ly.volume = l.get("volume") ? (float)l.get("volume")->num(1) : 1.f;
                        ly.pitch = l.get("pitch") ? (float)l.get("pitch")->num(1) : 1.f;
                        ly.delay = l.get("delay") ? (int)l.get("delay")->num(0) : 0;
                        if (const J *vj = l.get("variant")) ly.variant = vj->t == J::Num ? (int)vj->n : -1;   // a number, or "random" (or missing): random
                        if (!ly.event.empty()) def.layers.push_back(ly);
                    }
                }
                sfx[kv.first] = def;
            }
        }
        if (const J *m = root.get("music")) {
            musicEnabled = m->get("enabled") ? m->get("enabled")->num(1) != 0 : true;
            if (const J *md = m->get("mode")) { musicMode = md->str("real"); if (musicMode != "notes" && musicMode != "off") musicMode = "real"; }
            realVolume = m->get("real_volume") ? (float)m->get("real_volume")->num(-1) : -1.f;
            musicVolume = m->get("volume") ? (float)m->get("volume")->num(0.8) : 0.8f;
            musicSource = sourceFromName(m->get("source") ? m->get("source")->str("") : "", MUSIC);
            if (const J *r = m->get("roles"))
                for (auto &kv : r->o) {
                    RoleDef rd;
                    rd.instrument = kv.second.get("instrument") ? kv.second.get("instrument")->str("harp") : "harp";
                    rd.octave = kv.second.get("octave") ? (int)kv.second.get("octave")->num(0) : 0;
                    rd.volume = kv.second.get("volume") ? (float)kv.second.get("volume")->num(1) : 1.f;
                    roles[kv.first] = rd;
                }
            if (const J *t = m->get("tracks"))
                for (auto &kv : t->o) trackOn[kv.first] = kv.second.num(1) != 0;
        }
        return true;
    }

    // sound_map.json if it exists, otherwise the defaults compiled in
    bool load(const std::string &path) {
        *this = SoundMap();
        parse(SOUND_MAP_DEFAULT);
        std::string text;
        if (FILE *f = fopen(path.c_str(), "rb")) {
            char buf[4096]; size_t n;
            while ((n = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
            fclose(f);
        } else return false;
        SoundMap m = *this;
        if (!m.parse(text)) return false;   // a broken file leaves the defaults
        *this = m;
        return true;
    }
};

// the listener
struct Listener {
    // Minecraft space: where the player's eye is (vehicle position plus the seat height)
    double eyeX = 0, eyeY = 64, eyeZ = 0;
    // Doom space: the listener's position and eye height (for the distance to a sound)
    double doomX = 0, doomY = 0, doomZ = 0;
    int level = 1;               // Episode 1 map number; sounds from other levels are not heard
    double yaw = 0, pitch = 0;   // the player's own look direction in Minecraft degrees (rawYaw, rawPitch)
    // Third person front view (F5 twice): the real camera is camDist blocks in front of the eye, looking back at it. The sound is then
    // placed in the camera's own frame so left and right stay true. Leave false for the normal first person view.
    bool thirdFront = false;
    double camDist = 32;
    // On a Doom screen (menu, intermission) the camera is fixed at the map wall: everything is centred there.
    bool menu = false;
    double menuX = 0, menuY = 0, menuZ = 0;
};

struct Vec { double x = 0, y = 0, z = 0; };
inline Vec operator+(Vec a, Vec b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec operator*(Vec a, double k) { return {a.x * k, a.y * k, a.z * k}; }
inline double dot(Vec a, Vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec cross(Vec a, Vec b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }

// Minecraft look frame (forward, up, right) for yaw and pitch in degrees: yaw 0 looks south (+z), pitch positive looks down.
struct Frame { Vec f, u, r; };
inline Frame frameOf(double yawDeg, double pitchDeg) {
    const double k = 3.14159265358979323846 / 180.0;
    double y = yawDeg * k, p = pitchDeg * k;
    Frame fr;
    fr.f = {-std::sin(y) * std::cos(p), -std::sin(p), std::cos(y) * std::cos(p)};
    fr.u = {-std::sin(y) * std::sin(p), std::cos(p), std::cos(y) * std::sin(p)};
    fr.r = cross(fr.f, fr.u);
    return fr;
}

// Where to put a sound so the client plays it right. v is the vector from the listener's eye to the sound in Minecraft axes,
// already scaled to the distance that gives the wanted loudness.
inline Vec placeFor(const Listener &L, Vec v) {
    if (L.menu) return {L.menuX, L.menuY, L.menuZ};
    Vec eye = {L.eyeX, L.eyeY, L.eyeZ};
    if (!L.thirdFront) return eye + v;
    Frame p = frameOf(L.yaw, L.pitch);            // the player
    Frame c = frameOf(L.yaw + 180.0, -L.pitch);   // the camera that looks back at the player
    Vec cam = eye + p.f * L.camDist;
    return cam + c.f * dot(v, p.f) + c.r * dot(v, p.r) + c.u * dot(v, p.u);
}

// note block instruments
struct InstDef { const char *name; int lo; };   // lowest MIDI note; -1 = percussion
static const InstDef INSTRUMENTS[] = {
    {"harp", 54}, {"bass", 30}, {"guitar", 42}, {"banjo", 54}, {"bit", 54}, {"pling", 54}, {"iron_xylophone", 54}, {"cow_bell", 66},
    {"flute", 66}, {"bell", 78}, {"chime", 78}, {"xylophone", 78}, {"didgeridoo", 30}, {"basedrum", -1}, {"snare", -1}, {"hat", -1},
};
inline const InstDef *findInstrument(const std::string &n) {
    for (auto &i : INSTRUMENTS) if (n == i.name) return &i;
    return nullptr;
}

// A note block plays two octaves: pitch 0.5 .. 2.0. Move the note by whole octaves into the instrument's range.
inline bool noteToPitch(const InstDef &inst, int midi, int octaveShift, float &pitch) {
    if (inst.lo < 0) return false;
    int m = midi + 12 * octaveShift;
    while (m < inst.lo) m += 12;
    while (m > inst.lo + 24) m -= 12;
    pitch = (float)std::pow(2.0, (m - inst.lo - 12) / 12.0);
    return true;
}

// shared ring for multiplayer
// Put one SoundRing in the shared memory block. Every player process publishes the sounds Doom plays for it (with its absolute position)
// and polls the others' sounds; the engine ignores sounds from another level.
struct SoundRing {
    static constexpr int N = 256;
    struct Slot { std::atomic<uint32_t> stamp; int32_t x, y, z; int16_t sfx; uint8_t level, owner; };
    std::atomic<uint32_t> seq;
    Slot slots[N];

    void publish(int owner, int level, int sfx, double x, double y, double z) {
        uint32_t i = seq.fetch_add(1);
        Slot &s = slots[i % N];
        s.stamp.store(0);
        s.x = (int32_t)x; s.y = (int32_t)y; s.z = (int32_t)z; s.sfx = (int16_t)sfx; s.level = (uint8_t)level; s.owner = (uint8_t)owner;
        s.stamp.store(i + 1);
    }
    // calls f(owner, level, sfx, x, y, z) for every new entry since *last (a reader keeps its own counter, start it at seq at join)
    template <class F> void poll(uint32_t &last, F &&f) {
        uint32_t cur = seq.load();
        if (cur - last > N) last = cur - N;
        for (; last != cur; last++) {
            const Slot &s = slots[last % N];
            if (s.stamp.load() != last + 1) continue;
            f((int)s.owner, (int)s.level, (int)s.sfx, (double)s.x, (double)s.y, (double)s.z);
        }
    }
};

// the engine
class Engine {
public:
    SoundMap map;
    Listener listener;
    bool sfxEnabled = true;
    bool musicEnabled = true;       // the player's own switch; the map's "enabled" only sets the starting value
    float sfxVolume = 1.0f;         // 0..1, the player's own slider
    float musicGain = 1.0f;         // 0..1, the player's own music slider
    int owner = 0;                  // this player's slot (for the ring)
    int playerEntityId = 1;         // the player's own entity (PLAYER_ID in main.cpp): the real songs follow it

    void init(const std::string &mapPath) {
        map.load(mapPath);
        musicEnabled = map.musicEnabled;
        rng = 0x9E3779B97F4A7C15ull ^ (uint64_t)(uintptr_t)this;
    }
    void setListener(const Listener &l) { listener = l; }

    // The sliders of Doom's Options > Sound Volume menu arrive as 0..120 (slider 0..15 times 8). Slider 0 is off; the default
    // slider (8) is a bit under full so the player can still turn it up.
    void setDoomVolume(int which, int volume) {
        int s = std::max(0, std::min(15, volume / 8));
        float g = s == 0 ? 0.f : 0.5f + 0.5f * (float)s / 15.0f;
        if (which == 0) sfxVolume = g;
        else { musicGain = g; if (s == 0 && musicEnabled) musicStopPending = true; }
    }

    // A sound Doom started for this player (from dgs_pop_sound). Returns true if it was queued.
    bool onDoom(const DgsEvent &e, double nowMs, SoundRing *ring = nullptr) {
        if (ring && e.hasOrigin) ring->publish(owner, e.level, e.sfx, e.x, e.y, e.z);   // other players on this level hear it too
        return place(e.sfx, e.selfOrigin != 0, e.x, e.y, e.z, e.lx, e.ly, e.lz, e.map8 != 0, nowMs);
    }
    // A sound another player's Doom started (from SoundRing::poll). Own sounds in the ring are skipped.
    bool onRemote(int fromOwner, int level, int sfx, double x, double y, double z, double nowMs) {
        if (fromOwner == owner || level != listener.level) return false;
        return place(sfx, false, x, y, z, listener.doomX, listener.doomY, listener.doomZ, level == 8, nowMs);
    }

    void musicRequest(const char *name, bool looping) {   // "e1m1" or "d_e1m1"
        musicStop();
        std::string n = name;
        for (auto &c : n) c = (char)toupper((unsigned char)c);
        if (n.compare(0, 2, "D_") != 0) n = "D_" + n;
        {
            auto it = map.trackOn.find(n);
            if (it != map.trackOn.end() && !it->second) return;
        }
        if (map.musicMode == "off") return;
        if (map.musicMode == "real") {
#if SOUND_HAVE_REAL
            for (int i = 0; i < REAL_NUM_TRACKS; i++)
                if (n == REAL_TRACKS[i].name) { realIdx = i; realLoop = looping; musicName = n; return; }
#endif
            return;   // a song without a rendered file stays silent in this mode
        }
#if SOUND_HAVE_MUSIC
        for (int i = 0; i < MUS_NUM_TRACKS; i++)
            if (n == MUS_TRACKS[i].name) {
                auto it = map.trackOn.find(n);
                if (it != map.trackOn.end() && !it->second) return;
                track = &MUS_TRACKS[i]; musicLoop = looping; musicIdx = 0; musicStartMs = -1; musicName = n;
                return;
            }
#else
        (void)name; (void)looping;
#endif
    }
    void musicStop() { track = nullptr; realIdx = -1; musicStopPending = true; }
    // The player toggles the music (chat command, hotbar item ...): off also cuts the notes still ringing and the real song.
    void setMusicEnabled(bool on) { musicEnabled = on; if (!on) musicStopPending = true; }
    const std::string &currentTrack() const { return musicName; }
    bool musicPlaying() const { return (track != nullptr || realIdx >= 0) && musicEnabled; }

    // Call once per server loop. emit(const W&) gets every packet; sounds that fall into the same instant are bundled
    // (BundleDelimiter before and after) so the client plays them in the same tick.
    template <class W, class Emit> void pump(double nowMs, Emit &&emit) {
        if (musicStopPending) {
            musicStopPending = false;
            if (!realPlaying.empty()) {   // the real song: stop just that sound (all copies of it), source music
                emit(stopSound<W>(map.musicSource, realPlaying));
                realPlaying.clear();
            }
            emit(stopSound<W>(map.musicSource, std::string()));   // and whatever else the music source still plays (note block notes)
            realNextMs = -1; realVolDiffSince = -1;
        }
        realMusicTick<W>(nowMs, emit);
        musicTick(nowMs);
        if (pending.empty()) return;
        // count what is due; thin out a crowded window, keeping the loudest
        if (nowMs - windowStart >= 50.0) { windowStart = nowMs; sentInWindow = 0; }
        std::vector<Pending> due;
        for (size_t i = 0; i < pending.size();) {
            if (pending[i].dueMs <= nowMs) { due.push_back(pending[i]); pending.erase(pending.begin() + (long)i); }
            else i++;
        }
        if (due.empty()) return;
        std::stable_sort(due.begin(), due.end(), [](const Pending &a, const Pending &b) { return a.volume > b.volume; });
        size_t room = (size_t)std::max(0, map.doom.maxPerTick * 4 - sentInWindow);   // layers and music notes count too
        if (due.size() > room) due.resize(room);
        if (due.empty()) return;
        { W d; d.vi(PKT_BUNDLE_DELIMITER); emit(d); }
        for (auto &p : due) emit(soundAt<W>(p.event, p.source, p.x, p.y, p.z, p.volume, p.pitch, p.seed));
        { W d; d.vi(PKT_BUNDLE_DELIMITER); emit(d); }
        sentInWindow += (int)due.size();
    }

    // exposed for tests
    size_t queued() const { return pending.size(); }

private:
    struct Pending { double dueMs; std::string event; int source; double x, y, z; float volume, pitch; int64_t seed; };
    std::vector<Pending> pending;
    uint64_t rng = 1;
    double windowStart = 0;
    int sentInWindow = 0;
    bool musicStopPending = false;
#if SOUND_HAVE_MUSIC
    const MusTrack *track = nullptr;
#else
    const void *track = nullptr;
#endif
    // real songs
    int realIdx = -1;               // the requested song in REAL_TRACKS, -1 none
    bool realLoop = true;
    std::string realPlaying;        // the event that was started and is playing (empty when stopped)
    double realNextMs = -1;         // when to send the next pass of a looping song
    float realVol = 0;              // the volume the song was started with
    double realVolDiffSince = -1;   // the slider moved: restart with the new volume once it has settled
    bool musicLoop = true;
    int musicIdx = 0;
    double musicStartMs = -1;
    std::string musicName;

    uint64_t next() { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }
    double frand() { return (double)(next() >> 11) / 9007199254740992.0; }

    static double approxDist(double dx, double dy) {   // Doom's own fast distance
        dx = std::fabs(dx); dy = std::fabs(dy);
        return dx + dy - std::min(dx, dy) / 2.0;
    }

    bool place(int sfx, bool self, double sx, double sy, double sz, double lx, double ly, double lz, bool map8, double nowMs) {
        if (!sfxEnabled || sfx < 1 || sfx >= DOOM_NUM_SFX) return false;
        auto it = map.sfx.find(DOOM_SFX_NAMES[sfx]);
        if (it == map.sfx.end() || it->second.layers.empty()) return false;
        const SfxDef &def = it->second;
        // loudness as Doom computes it (S_AdjustSoundParams)
        double gain = 1.0;
        Vec v;   // from the eye to the sound, Minecraft axes
        if (!self) {
            double dx = sx - lx, dy = sy - ly;
            double d = approxDist(dx, dy);
            const DoomCfg &c = map.doom;
            if (!map8 && d > c.clip) return false;
            if (d >= c.close) {
                double t = std::max(0.0, std::min(1.0, (c.clip - d) / (c.clip - c.close)));
                gain = map8 ? c.map8floor + (1.0 - c.map8floor) * t : t;
            }
            if (gain < 0.02) return false;
            v = {dx / 32.0, map.doom.useZ ? std::max(-4.0, std::min(4.0, (sz - lz) / 32.0)) : 0.0, -dy / 32.0};
            double len = std::sqrt(dot(v, v));
            if (len < 0.05) v = {0, 0, 0};
            else {
                // Minecraft fades a sound linearly from 1 block (full) to range blocks (silent): pick the distance with the wanted gain
                double dist = 1.0 + (1.0 - gain) * (c.range - 1.0);
                v = v * (dist / len);
            }
        }
        Vec pos = placeFor(listener, v);
        for (auto &ly2 : def.layers) {
            Pending p;
            p.dueMs = nowMs + 50.0 * ly2.delay;
            p.event = ly2.event;
            p.source = def.source;
            p.x = pos.x; p.y = pos.y; p.z = pos.z;
            p.volume = std::max(0.f, std::min(1.f, ly2.volume * sfxVolume));
            float pitch = ly2.pitch * (float)(1.0 + (frand() * 2.0 - 1.0) * def.pitchVar);
            p.pitch = std::max(0.5f, std::min(2.0f, pitch));
            int64_t fixedSeed = 0;
            p.seed = (ly2.variant >= 0 && variantSeed(ly2.event, ly2.variant, fixedSeed)) ? fixedSeed : (int64_t)next();   // the seed picks the variant
            if (p.volume > 0.01f) pending.push_back(p);
        }
        if (pending.size() > 512) pending.erase(pending.begin(), pending.begin() + 256);
        return true;
    }

    float realWantedVolume() const {
        float base = map.realVolume >= 0.f ? map.realVolume : map.musicVolume;
        return std::max(0.f, std::min(1.f, base * musicGain));
    }

    // The real song: one Entity Sound Effect packet per pass. A sound cannot be changed once it plays, so a new volume (the Doom slider) restarts the song
    // after the slider has rested for 600 ms, and a looping song is sent again shortly before its end (the file is rendered as a seamless loop).
    template <class W, class Emit> void realMusicTick(double nowMs, Emit &&emit) {
#if SOUND_HAVE_REAL
        if (realIdx < 0 || !musicEnabled || map.musicMode != "real") return;
        const float want = realWantedVolume();
        if (want <= 0.f) return;   // slider at zero: silent until it comes up (musicStop already cut the song)
        const RealTrack &t = REAL_TRACKS[realIdx];
        if (!realPlaying.empty()) {
            if (std::fabs(want - realVol) > 0.03f) {
                if (realVolDiffSince < 0) realVolDiffSince = nowMs;
                if (nowMs - realVolDiffSince > 600.0) {
                    emit(stopSound<W>(map.musicSource, realPlaying));
                    realPlaying.clear();
                    realVolDiffSince = -1;   // falls through: starts again below with the new volume
                } else return;
            } else realVolDiffSince = -1;
        }
        if (realPlaying.empty()) {
            emit(soundOnEntity<W>(t.event, map.musicSource, playerEntityId, want, 1.0f, (int64_t)next()));
            realPlaying = t.event; realVol = want;
            realNextMs = nowMs + t.lengthMs - 40.0;   // the client needs a few tens of ms to open the stream
            return;
        }
        if (nowMs >= realNextMs) {
            if (!realLoop) { realIdx = -1; realPlaying.clear(); return; }   // played once (the intro): done
            emit(soundOnEntity<W>(t.event, map.musicSource, playerEntityId, want, 1.0f, (int64_t)next()));
            realNextMs += t.lengthMs;
        }
#else
        (void)nowMs; (void)emit;
#endif
    }

    void musicTick(double nowMs) {
#if SOUND_HAVE_MUSIC
        if (map.musicMode != "notes") return;
        if (!track || !musicEnabled || musicGain <= 0.f || track->count == 0) return;
        if (musicStartMs < 0) musicStartMs = nowMs + 150.0;   // a short lead so the first notes are not late
        for (;;) {
            if (musicIdx >= track->count) {
                double end = musicStartMs + track->lengthTicks * 50.0;
                if (nowMs < end) return;
                if (!musicLoop) { track = nullptr; return; }
                musicStartMs = end; musicIdx = 0;
            }
            uint32_t w = track->events[musicIdx];
            double due = musicStartMs + (w & 32767) * 50.0;
            if (due > nowMs) return;
            musicIdx++;
            if (nowMs - due > 300.0) continue;   // fell behind (a stall): skip instead of a burst of old notes
            queueNote(w, nowMs);
        }
#else
        (void)nowMs;
#endif
    }

    void queueNote(uint32_t w, double nowMs) {
#if SOUND_HAVE_MUSIC
        int role = (int)((w >> 15) & 31), midi = (int)((w >> 20) & 127), vel = (int)((w >> 27) & 31);
        if (role >= MUS_NUM_ROLES) return;
        auto it = map.roles.find(MUS_ROLES[role]);
        if (it == map.roles.end() || it->second.instrument == "off") return;
        const RoleDef &r = it->second;
        const InstDef *inst = findInstrument(r.instrument);
        if (!inst) return;
        float pitch = 1.0f;
        if (!noteToPitch(*inst, midi, r.octave, pitch)) {
            pitch = 1.0f;   // percussion: the toms follow the key, everything else plays the plain sample
            if (std::string(MUS_ROLES[role]) == "drum_tom") pitch = (float)std::pow(2.0, (midi - 45) / 24.0);
        }
        float vol = (0.35f + 0.65f * (float)vel / 31.0f) * r.volume * map.musicVolume * musicGain;
        if (vol < 0.03f) return;
        Pending p;
        p.dueMs = nowMs;
        p.event = std::string("minecraft:block.note_block.") + inst->name;
        p.source = map.musicSource;
        Vec pos = listener.menu ? Vec{listener.menuX, listener.menuY, listener.menuZ} : Vec{listener.eyeX, listener.eyeY, listener.eyeZ};
        p.x = pos.x; p.y = pos.y; p.z = pos.z;
        p.volume = std::min(1.0f, vol);
        p.pitch = std::max(0.5f, std::min(2.0f, pitch));
        p.seed = (int64_t)next();
        pending.push_back(p);
#else
        (void)w; (void)nowMs;
#endif
    }
};

}  // namespace snd
