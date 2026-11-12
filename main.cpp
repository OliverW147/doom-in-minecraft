// doom-mc-server
//
// A deliberately tiny fake Minecraft 26.3 server (offline mode, one player) that runs the original
// Doom through doomgeneric and shows the picture as a grid of text_display entities, one per pixel.
// Mouse movement (the player's yaw changes) turns the Doom player, WASD walks, clicks fire and use.
//
// Everything the client needs is hand-encoded here: handshake, login, configuration (registries are
// sent as "known pack" references so no data is needed), one chunk with a few barrier blocks, and
// the entity packets. Packet ids and layouts were read from the running 26.3 client.

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mmsystem.h>
static void sleepMs(unsigned ms) { Sleep(ms); }
static int pollReadable(SOCKET s) { WSAPOLLFD p{s, POLLRDNORM, 0}; return WSAPoll(&p, 1, 0); }
#define SEND_FLAGS 0
#else
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <dirent.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
typedef int SOCKET;
#define INVALID_SOCKET (-1)
static int closesocket(SOCKET s) { return ::close(s); }
static void sleepMs(unsigned ms) { usleep(ms * 1000u); }
static int pollReadable(SOCKET s) { pollfd p{s, POLLIN, 0}; return poll(&p, 1, 0); }
#define SEND_FLAGS MSG_NOSIGNAL
#endif

#include <zlib.h>

#include <algorithm>
#include <condition_variable>
#include <map>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <deque>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include "doomgeneric.h"
#include "doomkeys.h"
void dg_key(int down, int key, int ascii);
void dg_mouse(int units);
int dg_player_state(double *x, double *y, double *viewz, double *angleDeg);
void dg_set_angle(double deg);
void dg_teleport(double x, double y, double deg);
void dg_fullview(void);
int dg_screen_mode(void);
int dg_wipe_active(void);      // Doom's screen melt is running
int dg_on_title(void);         // the title picture is up and no menu is open
int dg_has_autosave(void);
void dg_load_autosave(void);
int dg_map(void);
int dg_num_sectors(void);
int dg_switch_on(int line);
int dg_skill(void);

}   // end of extern "C" for a moment: sound.h has templates
#include "sound.h"
extern "C" {

// The player list: every player process publishes its name, level and skill in shared memory; every process shows all of them in the tab list.
struct RosterEntry { bool active = false; std::string name; int level = 1, skill = 3; };
static void rosterPublish(const std::string &name, int level, int skill);   // this process's own entry
static void rosterClear();
static int rosterSelf();
static int rosterOnline();   // players in the game right now
static std::string logTime() {
    char b[32];
    time_t t = time(nullptr);
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S", &tmv);
    return b;
}                                                    // this process's slot
static void rosterRead(RosterEntry *out);                                   // 64 entries
void dg_cross_line(int n);
const char *dg_floor_flat_name(int sec);
int dg_weapon_slot(void);
void dg_warp(int map);
int dg_restart_if_idle(void);
void dg_set_single_level(int on);
int dg_autosave(void);
int dg_has_autosave(void);
void dg_load_autosave(void);
int dg_gametic(void);
int dg_player_health(void);
void dg_sector_state(int sec, int *floor, int *ceil, int *light);
void dg_set_sector(int sec, int floor, int ceil);
void dg_zero_momentum(void);
int dg_things(int *out, int maxn, double maxdist);
void dg_hud(int *o);
int dg_sector_at(double x, double y);
int dg_player_sector(void);
int dg_mouse_free(void);
const uint32_t *dg_frame(void);
}

#include "gen_registries.h"
#include "gen_maps.h"
#include "gen_level.h"

static std::vector<uint8_t> g_tags;                  // payload of the update_tags packet, loaded from tags.bin
static uint8_t g_lut[262144];                        // RGB 6 bits per channel -> packed map colour id, nearest in CIELAB, built at startup
static std::vector<std::vector<uint8_t>> g_regs;     // one registry_data payload per registry, from registries.bin


// ---------------------------------------------------------------- platform hooks for doomgeneric

static double nowMs() {
#ifdef _WIN32
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)freq.QuadPart;
#else
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
#endif
}

// ---- save games: Doom's menu slots 0..5 and the autosave (slot 6) live in memory shared by all player processes, per player name.
// (The service may not write files.) glue.c routes Doom's save and load code here.
constexpr int SAVE_SLOTS = 7, SAVE_MAX = 98304, SAVE_NAMES = 16;
struct SaveEntry {
    char name[17];
    uint32_t stamp;
    int32_t len[SAVE_SLOTS];
    unsigned char data[SAVE_SLOTS][SAVE_MAX];
};
struct SaveStore {
    std::atomic<int> lock;
    std::atomic<uint32_t> dirty;   // counts changes; the supervisor writes the saves to disk when it moves (children may not write files)
    uint32_t clock;
    SaveEntry e[SAVE_NAMES];
};
static SaveStore *g_saves = nullptr;
static char g_saveName[17] = "DoomGuy";

struct SaveLock {   // a few microseconds of memcpy: a spin lock, with a way out if a killed process died holding it
    SaveLock() {
        for (int i = 0; i < 50000 && g_saves->lock.exchange(1); i++) sleepMs(0);
        g_saves->lock = 1;
    }
    ~SaveLock() { g_saves->lock = 0; }
};

static SaveEntry *saveEntry(bool create) {   // lock held
    if (!g_saves) return nullptr;
    SaveEntry *oldest = &g_saves->e[0];
    for (auto &e : g_saves->e) {
        if (e.name[0] && strcmp(e.name, g_saveName) == 0) { e.stamp = ++g_saves->clock; return &e; }
        if (e.stamp < oldest->stamp || !e.name[0]) oldest = &e;
        if (!e.name[0]) break;
    }
    if (!create) return nullptr;
    memset(oldest, 0, sizeof *oldest);   // a free entry, or the least recently used name
    strncpy(oldest->name, g_saveName, 16);
    oldest->stamp = ++g_saves->clock;
    return oldest;
}

extern "C" {
void dgs_set_player(const char *name) { snprintf(g_saveName, sizeof g_saveName, "%s", name); }
int dgs_exists(int slot) {
    if (slot < 0 || slot >= SAVE_SLOTS) return 0;
    SaveLock l;
    SaveEntry *e = saveEntry(false);
    return e && e->len[slot] > 0;
}
int dgs_read(int slot, unsigned char *buf, int cap) {
    if (slot < 0 || slot >= SAVE_SLOTS) return -1;
    SaveLock l;
    SaveEntry *e = saveEntry(false);
    if (!e || e->len[slot] <= 0 || e->len[slot] > cap) return -1;
    memcpy(buf, e->data[slot], (size_t)e->len[slot]);
    return e->len[slot];
}
void dgs_write(int slot, const unsigned char *buf, int len) {
    if (slot < 0 || slot >= SAVE_SLOTS || len <= 0 || len > SAVE_MAX) return;
    SaveLock l;
    SaveEntry *e = saveEntry(true);
    if (!e) return;
    memcpy(e->data[slot], buf, (size_t)len);
    e->len[slot] = len;
    g_saves->dirty++;
}
void dgs_delete(int slot) {
    if (slot < 0 || slot >= SAVE_SLOTS) return;
    SaveLock l;
    SaveEntry *e = saveEntry(false);
    if (e) { e->len[slot] = 0; g_saves->dirty++; }
}
}

extern "C" {
void DG_Init() {}
void DG_DrawFrame() {}
void DG_SleepMs(uint32_t ms) { sleepMs(ms); }
uint32_t DG_GetTicksMs() { return (uint32_t)nowMs(); }
int DG_GetKey(int *, unsigned char *) { return 0; }  // input is injected through dg_key / dg_mouse
void DG_SetWindowTitle(const char *) {}
}

// ---------------------------------------------------------------- configuration

struct Config {
    int port = 25565;
    std::string iwad = "DOOM1.WAD";
    float sens = 2.0f;       // Doom turn per degree of Minecraft yaw
    int res = 128;           // pixels across (height is res * 5 / 8, so 128 gives 128x80 and 160 gives 160x100)
    float width = 18.0f;     // screen width in blocks (height is 3/4 of that, like a 4:3 monitor)
    float dist = 6.0f;       // distance from the player to the screen
    int fps = 20;            // frames sent to the client per second
    float smooth = 0.0f;     // resampling sharpness: 0 = nearest (blocky), 1 = plain bilinear (soft), in between = sharp bilinear
    float dither = 0.0f;    // ordered dither strength in 0..255 levels, 0 = off
    int ping = 0;            // artificial round trip latency in ms (half added each way), 0 = off
    int maxPlayers = 10;     // simultaneous players (every player gets their own Doom process)
    int perIp = 3;           // simultaneous connections per address
    int compress = 256;      // packets of at least this many bytes are zlib compressed, -1 = off
    int idleMin = 10;        // kick players that did nothing for this many minutes, 0 = never
    int warpMap = 1;         // --map N: start on E1MN (testing)
    bool warp = false;       // --warp: skip the title screen and start E1M1 (testing)
    bool debug = false;      // log input events
    double eyeBoost = 8.0;       // client render: camera this many Doom units above the Doom eye (41), purely visual
    bool clientRender = false;   // the client draws Doom itself (resource pack shader), the server only sends the player position
    std::string icon;   // --icon: 64x64 PNG shown in the server list
    std::string saveDir;   // --save-dir: where the supervisor keeps the players' saved games (one file per name), so they survive restarts
    std::string packUrl, packSha1, packFile;   // packFile: the zip this server hands out itself over HTTP on its game port (plain GET /doomgl.zip)
      // resource pack to push to the client in --client mode
    bool freecam = false;    // dev mode for the client side renderer: free camera, no screen, flying player
    bool lab = false;        // match map colours in CIELAB instead of plain RGB distance
    int maps = 8;           // map mode (default): screen is a wall of this many item frames across (rows = cols * 3 / 4), 0 = entity pixels
    std::vector<std::string> doomArgs;
} cfg;

static int GW = 128, GH = 80;           // entity grid, set from --res
static float CW = 0.0625f, CH = 0.075f;  // pixel size in blocks, derived from --width
constexpr int DW = 320, DH = 200;     // Doom framebuffer
constexpr uint32_t PROTOCOL = 777;    // Minecraft 26.3
constexpr size_t MAX_IN_PKT = 32768;  // largest client packet we accept (decompressed)
constexpr int BASE_ID = 1000;         // first pixel entity id
constexpr int PLAYER_ID = 1;
constexpr int VEHICLE_ID = 2;       // invisible seat: a passenger cannot walk or jump
constexpr double BG_PARK_Y = 3000.0;
constexpr int BG_ID = 4;            // huge flat background entity behind the screen
constexpr int CONTROLS_SLOT = 35;   // bottom right slot of the main inventory (not the hotbar: hotbar item icons are drawn even though the hotbar is hidden)
constexpr int ITEM_BOOK = 1146, COMP_ITEM_NAME_ID = 9, COMP_LORE_ID = 11;   // ids of this game build
constexpr int TOGGLE_SLOT = 13;     // container slot holding the background toggle item
static int g_bg = 1;                // 0 = none (sky), 1 = black, 2 = white
constexpr int CAMERA_ID = 3;        // invisible entity the view is locked to, so turning the mouse never moves the picture

constexpr double PX = 8.5, PY = 64.0, PZ = 8.5;  // seat position
static double EYE_Y = 65.0;                       // height of the fixed camera
constexpr double CAM_Z = PZ - 1.5;                // the camera sits in front of the player so the hidden player body is behind it
constexpr double CAM_EYE = 0.0;                   // an interaction entity has no eye height: the camera sits at its origin (measured)
constexpr float HOME_YAW = 180.0f;               // facing north (-z)

// ---------------------------------------------------------------- byte writer / reader

struct W {
    std::vector<uint8_t> b;
    W &u8(int v) { b.push_back((uint8_t)v); return *this; }
    W &i16(int v) { u8(v >> 8); u8(v); return *this; }
    W &i32(int32_t v) { for (int s = 24; s >= 0; s -= 8) u8(v >> s); return *this; }
    W &i64(int64_t v) { for (int s = 56; s >= 0; s -= 8) u8((int)(v >> s)); return *this; }
    W &f32(float f) { uint32_t u; memcpy(&u, &f, 4); return i32((int32_t)u); }
    W &f64(double d) { uint64_t u; memcpy(&u, &d, 8); return i64((int64_t)u); }
    W &vi(uint32_t v) {
        while (v & ~0x7Fu) { u8((v & 0x7F) | 0x80); v >>= 7; }
        return u8(v);
    }
    W &str(const std::string &s) { vi((uint32_t)s.size()); b.insert(b.end(), s.begin(), s.end()); return *this; }
    W &uuid(uint64_t hi, uint64_t lo) { return i64((int64_t)hi).i64((int64_t)lo); }
    W &raw(const std::vector<uint8_t> &v) { b.insert(b.end(), v.begin(), v.end()); return *this; }
};

struct R {
    const uint8_t *p, *e;
    bool ok = true;
    R(const std::vector<uint8_t> &v) : p(v.data()), e(v.data() + v.size()) {}
    int u8() { if (p >= e) { ok = false; return 0; } return *p++; }
    int i16() { int a = u8(); return (int16_t)((a << 8) | u8()); }
    int32_t i32() { uint32_t v = 0; for (int i = 0; i < 4; i++) v = (v << 8) | (uint32_t)u8(); return (int32_t)v; }
    int64_t i64() { uint64_t v = 0; for (int i = 0; i < 8; i++) v = (v << 8) | (uint64_t)u8(); return (int64_t)v; }
    float f32() { uint32_t u = (uint32_t)i32(); float f; memcpy(&f, &u, 4); return f; }
    double f64() { uint64_t u = (uint64_t)i64(); double d; memcpy(&d, &u, 8); return d; }
    uint32_t vi() {
        uint32_t v = 0;
        for (int s = 0; s < 35; s += 7) {
            int c = u8();
            v |= (uint32_t)(c & 0x7F) << s;
            if (!(c & 0x80)) break;
        }
        return v;
    }
    std::string str() {
        uint32_t n = vi();
        if ((size_t)(e - p) < n) { ok = false; return ""; }
        std::string s((const char *)p, n);
        p += n;
        return s;
    }
};

static bool readFile(const char *path, std::vector<uint8_t> &out) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.insert(out.end(), buf, buf + n);
    fclose(f);
    return !out.empty();
}

// Skips one NBT payload of the given tag type (used to find where an inline registry entry ends).
static bool skipNbt(const std::vector<uint8_t> &d, size_t &p, int type) {
    auto need = [&](size_t n) { return p + n <= d.size(); };
    auto be = [&](size_t n) { uint32_t v = 0; for (size_t i = 0; i < n; i++) v = (v << 8) | d[p + i]; return v; };
    switch (type) {
    case 1: if (!need(1)) return false; p += 1; return true;
    case 2: if (!need(2)) return false; p += 2; return true;
    case 3: case 5: if (!need(4)) return false; p += 4; return true;
    case 4: case 6: if (!need(8)) return false; p += 8; return true;
    case 7: { if (!need(4)) return false; uint32_t n = be(4); p += 4; if (!need(n)) return false; p += n; return true; }
    case 8: { if (!need(2)) return false; uint32_t n = be(2); p += 2; if (!need(n)) return false; p += n; return true; }
    case 9: {
        if (!need(5)) return false;
        int et = d[p]; uint32_t n = (p++, be(4)); p += 4;
        for (uint32_t i = 0; i < n; i++) if (!skipNbt(d, p, et)) return false;
        return true;
    }
    case 10:
        for (;;) {
            if (!need(1)) return false;
            int t = d[p++];
            if (t == 0) return true;
            if (!need(2)) return false;
            uint32_t n = be(2); p += 2;
            if (!need(n)) return false;
            p += n;
            if (!skipNbt(d, p, t)) return false;
        }
    case 11: { if (!need(4)) return false; uint32_t n = be(4); p += 4; if (!need((size_t)n * 4)) return false; p += (size_t)n * 4; return true; }
    case 12: { if (!need(4)) return false; uint32_t n = be(4); p += 4; if (!need((size_t)n * 8)) return false; p += (size_t)n * 8; return true; }
    }
    return false;
}

// registries.bin: varint count, then per registry: key, entry count, entries (id, flag, optional NBT).
static bool loadRegistries(const char *path) {
    std::vector<uint8_t> d;
    if (!readFile(path, d)) return false;
    R r(d);
    uint32_t nreg = r.vi();
    for (uint32_t i = 0; i < nreg; i++) {
        const uint8_t *start = r.p;
        r.str();
        uint32_t n = r.vi();
        for (uint32_t j = 0; j < n; j++) {
            r.str();
            int flag = r.u8();
            if (flag) {
                size_t p = (size_t)(r.p - d.data());
                int type = d[p++];
                if (!skipNbt(d, p, type)) return false;
                r.p = d.data() + p;
            }
            if (!r.ok) return false;
        }
        g_regs.emplace_back(start, r.p);
    }
    return r.ok;
}

// ---------------------------------------------------------------- connection

static int g_compress = -1;   // zlib threshold once the login compression packet was sent, -1 = not yet

static void appendVarint(std::vector<uint8_t> &o, uint32_t v) {
    while (v & ~0x7Fu) { o.push_back((uint8_t)((v & 0x7F) | 0x80)); v >>= 7; }
    o.push_back((uint8_t)v);
}

struct Conn {
    SOCKET s = INVALID_SOCKET;
    std::vector<uint8_t> in;
    std::atomic<bool> dead{false};
    std::mutex sm;   // one writer at a time: the main loop and the encoder thread both send

    // Flood protection: a client that sends more than this is not a Minecraft client.
    double winStart = 0;
    int winPackets = 0;
    size_t winBytes = 0;

    // Artificial latency: outgoing data waits in outQ for delayOut ms and is sent by a helper thread,
    // incoming packets wait in inQ for delayIn ms before poll() hands them out.
    double delayOut = 0, delayIn = 0;
    std::mutex qm;
    std::deque<std::pair<double, std::vector<uint8_t>>> outQ;
    std::deque<std::pair<double, std::vector<uint8_t>>> inQ;
    std::thread sender;

    void enableDelay(double oneWayMs) {
        if (oneWayMs <= 0 || sender.joinable()) return;
        delayOut = delayIn = oneWayMs;
        sender = std::thread([this] {
            std::vector<std::vector<uint8_t>> due;
            while (!dead) {
                due.clear();
                {
                    std::lock_guard<std::mutex> lk(qm);
                    double t = nowMs();
                    while (!outQ.empty() && outQ.front().first <= t) { due.push_back(std::move(outQ.front().second)); outQ.pop_front(); }
                }
                for (auto &d : due) sendNow(d);
                sleepMs(1);
            }
        });
    }

    void close() {
        dead = true;
        if (sender.joinable()) sender.join();
        if (s != INVALID_SOCKET) { closesocket(s); s = INVALID_SOCKET; }
    }

    void sendRaw(const std::vector<uint8_t> &d) {
        if (delayOut > 0) {
            std::lock_guard<std::mutex> lk(qm);
            if (outQ.size() < 4096) outQ.emplace_back(nowMs() + delayOut, d);
        } else {
            sendNow(d);
        }
    }

    void sendNow(const std::vector<uint8_t> &d) {
        std::lock_guard<std::mutex> lk(sm);
        size_t off = 0;
        while (off < d.size() && !dead) {
            int n = (int)::send(s, (const char *)d.data() + off, (int)std::min<size_t>(d.size() - off, 1 << 16), SEND_FLAGS);
            if (n <= 0) { dead = true; return; }   // includes the send timeout: a client that stops reading is dropped
            off += (size_t)n;
        }
    }

    // Adds length framing (and zlib compression once enabled) around one packet.
    static void frame(std::vector<uint8_t> &out, const W &p) {
        const std::vector<uint8_t> &b = p.b;
        if (g_compress < 0) {
            appendVarint(out, (uint32_t)b.size());
            out.insert(out.end(), b.begin(), b.end());
        } else if ((int)b.size() < g_compress) {
            appendVarint(out, (uint32_t)b.size() + 1);
            out.push_back(0);
            out.insert(out.end(), b.begin(), b.end());
        } else {
            thread_local std::vector<uint8_t> z;
            uLongf zl = compressBound((uLong)b.size());
            z.resize(zl);
            if (compress2(z.data(), &zl, b.data(), (uLong)b.size(), 1) != Z_OK) return;
            std::vector<uint8_t> dl;
            appendVarint(dl, (uint32_t)b.size());
            appendVarint(out, (uint32_t)(dl.size() + zl));
            out.insert(out.end(), dl.begin(), dl.end());
            out.insert(out.end(), z.begin(), z.begin() + (long)zl);
        }
    }

    void send(const W &p) {
        std::vector<uint8_t> o;
        frame(o, p);
        sendRaw(o);
    }

    // Non-blocking: returns true and fills pkt when a packet is available (and its artificial delay is over).
    bool poll(std::vector<uint8_t> &pkt) {
        if (delayIn <= 0 && inQ.empty()) return rawPoll(pkt);
        std::vector<uint8_t> p;
        while (inQ.size() < 1024 && rawPoll(p)) inQ.emplace_back(nowMs(), std::move(p));
        if (!inQ.empty() && nowMs() >= inQ.front().first + delayIn) {
            pkt = std::move(inQ.front().second);
            inQ.pop_front();
            return true;
        }
        return false;
    }

    bool rawPoll(std::vector<uint8_t> &pkt) {
        if (dead) return false;
        for (;;) {
            // try to parse from buffer first
            size_t i = 0;
            uint32_t len = 0;
            int shift = 0;
            bool haveLen = false;
            while (i < in.size() && i < 3) {   // 3 bytes of varint = 2 MB, far more than we accept
                uint8_t c = in[i++];
                len |= (uint32_t)(c & 0x7F) << shift;
                shift += 7;
                if (!(c & 0x80)) { haveLen = true; break; }
            }
            if (!haveLen && i >= 3) { dead = true; return false; }
            if (haveLen && len > MAX_IN_PKT) { dead = true; return false; }
            if (haveLen && in.size() >= i + len) {
                std::vector<uint8_t> body(in.begin() + (long)i, in.begin() + (long)(i + len));
                in.erase(in.begin(), in.begin() + (long)(i + len));
                double now = nowMs();
                if (now - winStart >= 1000) { winStart = now; winPackets = 0; winBytes = 0; }
                winPackets++; winBytes += len + i;
                if (winPackets > 600 || winBytes > (1u << 20)) { dead = true; return false; }
                if (g_compress < 0) { pkt = std::move(body); return true; }
                R r(body);
                uint32_t dataLen = r.vi();
                if (!r.ok) { dead = true; return false; }
                if (dataLen == 0) { pkt.assign(r.p, r.e); return true; }
                if (dataLen > MAX_IN_PKT || (int)dataLen < g_compress) { dead = true; return false; }   // zip bomb guard
                pkt.resize(dataLen);
                uLongf outLen = dataLen;
                if (uncompress(pkt.data(), &outLen, r.p, (uLong)(r.e - r.p)) != Z_OK || outLen != dataLen) { dead = true; return false; }
                return true;
            }
            if (pollReadable(s) <= 0) return false;
            char buf[16384];
            int n = (int)recv(s, buf, sizeof buf, 0);
            if (n <= 0) { dead = true; return false; }
            in.insert(in.end(), buf, buf + n);
        }
    }

    bool wait(std::vector<uint8_t> &pkt, int timeoutMs) {
        double end = nowMs() + timeoutMs;
        while (nowMs() < end && !dead) {
            if (poll(pkt)) return true;
            sleepMs(1);
        }
        return false;
    }
};

// ---------------------------------------------------------------- registry lookups

static int indexOf(const char *reg, const char *id) {
    for (auto &r : REGISTRIES)
        if (!strcmp(r.key, reg)) {
            for (size_t i = 0; i < r.ids.size(); i++)
                if (!strcmp(r.ids[i], id)) return (int)i;
        }
    fprintf(stderr, "registry entry not found: %s %s\n", reg, id);
    exit(1);
}

// ---------------------------------------------------------------- packet ids (26.3, protocol 777)

namespace out {   // clientbound
enum { BundleDelim = 0, AddEntity = 1, ChunkBatchFinished = 11, ChunkBatchStart = 12, Disconnect = 32,
       GameEvent = 39, KeepAlive = 45, LevelChunk = 46, Login = 50, PongResponse = 63, PlayerAbilities = 65,
       PlayerInfoUpdate = 71, PlayerPosition = 73, PlayerRotation = 74, SetChunkCacheCenter = 96,
       SetEntityData = 101, ContainerSetContent = 18, SetPassengers = 109, SetCamera = 95, MapItemData = 52, SetHealth = 106, SetHeldSlot = 107, SystemChat = 124, EntityPositionSync = 35, SetTime = 115, SetEntityDataPkt = 101, ResourcePackPush = 82, PostEffects = 83, UpdateAttributes = 134 };
}
namespace in {    // serverbound
enum { AcceptTeleport = 0, Attack = 1, KeepAlive = 28, MovePos = 30, MovePosRot = 31, MoveRot = 32,
       Interact = 26, ContainerClick = 18, PingRequest = 38, PlayerAction = 41, PlayerInput = 43, Punch = 46, SetCarried = 54,
       UseItemOn = 66, UseItem = 67, PlayerLoaded = 44, ResourcePack = 50 };
}

// ---------------------------------------------------------------- world: one chunk with a barrier cell

struct Pos3 { int x, y, z; };

static std::vector<Pos3> barrierBlocks() {
    // Just a floor. Nothing at eye level, so there is no selection outline to look at.
    return {{(int)PX, (int)PY - 1, (int)PZ}};
}

static void writeSection(W &w, const std::vector<std::array<int, 3>> &bl, int biomeId) {
    w.i16((int)bl.size());  // non-empty block count
    w.i16(0);               // fluid count
    if (bl.empty()) {
        w.u8(0).vi(BLOCK_AIR);                   // single-valued palette, no data
    } else {
        w.u8(4).vi(2).vi(BLOCK_AIR).vi(BLOCK_BARRIER);  // linear palette, 4 bits per entry
        uint64_t longs[256] = {0};
        for (auto &p : bl) {
            int idx = (p[1] << 8) | (p[2] << 4) | p[0];
            longs[idx / 16] |= 1ull << ((idx % 16) * 4);  // palette index 1 = barrier
        }
        for (auto l : longs) w.i64((int64_t)l);
    }
    w.u8(0).vi((uint32_t)biomeId);               // biomes: single value
}

static W buildChunkPacket(int biomeId, int chunkX, int chunkZ) {
    const int minY = -64, sections = 24;
    W buf;
    std::vector<Pos3> blocks;
    if (chunkX == 0 && chunkZ == 0) blocks = barrierBlocks();   // every other chunk is plain air
    for (int s = 0; s < sections; s++) {
        std::vector<std::array<int, 3>> local;
        for (auto &b : blocks) {
            if (((b.y - minY) >> 4) == s) local.push_back({b.x & 15, (b.y - minY) & 15, b.z & 15});
        }
        writeSection(buf, local, biomeId);
    }
    W p;
    p.vi(out::LevelChunk).i32(chunkX).i32(chunkZ);
    p.vi(0);                                     // heightmaps: none
    p.vi((uint32_t)buf.b.size()).raw(buf.b);     // chunk data
    p.vi(0);                                     // block entities
    for (int i = 0; i < 4; i++) p.vi(0);         // light masks (all empty)
    p.vi(0).vi(0);                               // sky / block light arrays
    return p;
}

// ---------------------------------------------------------------- pixel entities

static uint32_t quant(uint32_t c) { return c & 0xF8F8F8u; }

static W entityDataPacket(int idx, uint32_t rgb, bool full, float sx, float sy, float ty) {
    W p;
    p.vi(out::SetEntityData).vi((uint32_t)(BASE_ID + idx));
    if (full) {
        p.u8(11).vi(39).f32(0).f32(ty).f32(0);                    // translation
        p.u8(12).vi(39).f32(sx).f32(sy).f32(1);                   // scale
        p.u8(15).vi(0).u8(3);                                     // billboard: center
        p.u8(16).vi(1).vi(0xF000F0u);                             // brightness override 15/15
        p.u8(17).vi(3).f32(1.0f);                                 // view range
        p.u8(23).vi(5).u8(8).i16(1).u8(' ');                      // text: " " (NBT string tag)
    }
    p.u8(25).vi(1).vi(0xFF000000u | rgb);                         // background colour
    p.u8(255);
    return p;
}

// ---------------------------------------------------------------- game session

// Greedy rectangle cover of a colour grid: how many same-colour rectangles would draw this picture.
static int countRects(const std::vector<uint32_t> &col, int w, int h) {
    std::vector<char> used((size_t)w * h, 0);
    int n = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            if (used[(size_t)y * w + x]) continue;
            uint32_t c = col[(size_t)y * w + x];
            int x1 = x;
            while (x1 + 1 < w && !used[(size_t)y * w + x1 + 1] && col[(size_t)y * w + x1 + 1] == c) x1++;
            int y1 = y;
            for (;;) {
                if (y1 + 1 >= h) break;
                bool ok = true;
                for (int xx = x; xx <= x1 && ok; xx++) ok = !used[(size_t)(y1 + 1) * w + xx] && col[(size_t)(y1 + 1) * w + xx] == c;
                if (!ok) break;
                y1++;
            }
            for (int yy = y; yy <= y1; yy++) for (int xx = x; xx <= x1; xx++) used[(size_t)yy * w + xx] = 1;
            n++;
        }
    return n;
}

static bool g_freeze = false;   // test harness: ignore the player's movement keys

struct Input { bool fwd = false, back = false, left = false, right = false, use = false, run = false; };

struct Session {
    Conn &c;
    std::string name = "DoomGuy";
    std::string peer = "?";   // the player's address, for the log
    explicit Session(Conn &conn) : c(conn) {}

    // keys (merged from held state and timed pulses)
    Input held;
    bool fireHeld = false;
    double firePulseUntil = 0, usePulseUntil = 0;
    bool kFwd = false, kBack = false, kL = false, kR = false, kUse = false, kRun = false, kFire = false;
    bool arrowL = false, arrowR = false;
    double nextArrowL = 0, nextArrowR = 0;
    struct Pulse { int key, ascii; double until; };
    std::vector<Pulse> pulses;

    // look
    float lastYaw = HOME_YAW;
    double turnUnits = 0;     // accumulated Doom mouse units waiting to be posted

    std::vector<int> prev;
    int invState = 0;
    // camera / screen placement (depends on the mode)
    double camX = PX, camY = EYE_Y, camZ = CAM_Z;
    // map mode
    int mN = 0, mM = 0, mBx0 = 0, mY0 = 0, mBz = 0;
    std::vector<std::vector<uint8_t>> mapPrev;
    std::vector<uint16_t> fx0, fx1, fy0, fy1, fxw, fyw;   // resampling tables (weights 0..256)
    std::vector<uint32_t> lastFb;
    std::vector<uint32_t> prevSrc;                         // encoder thread: the Doom frame converted last
    std::vector<uint8_t> full, rowbuf;                     // whole screen as map colour ids, and the horizontally stretched rows
    int ditherTab[64] = {0};
    long statChanged = 0, statFrames = 0, statBytes = 0;   // for --debug
    double tConvSum = 0, tConvMax = 0, tSendSum = 0, tSendMax = 0, tLoopMax = 0;
    double nextKeepAlive = 0, nextFrame = 0;
    double lastAck = 0, lastActive = 0;
    float rawPitch = 0;                   // the player's own view pitch, for the shader (third person front view)
    float rawYaw = HOME_YAW;              // client side render: the player's own view yaw drives the Doom player angle
    double nextPosSync = 0, nextPostFx = 0;
    int postFxSent = 0;   // last keep alive answer, last real player input

    // Encoder thread: converts the newest Doom frame to map colours and sends it, so neither the colour
    // conversion nor a slow socket can stall Doom's game loop and the input handling.
    std::thread enc;
    std::mutex em;
    std::condition_variable ecv;
    std::vector<uint32_t> snap;
    bool snapReady = false, encStop = false;

    void startEncoder() { enc = std::thread([this] { encoderLoop(); }); }
    void stopEncoder() {
        { std::lock_guard<std::mutex> lk(em); encStop = true; }
        ecv.notify_all();
        if (enc.joinable()) enc.join();
    }
    void encoderLoop() {
        std::vector<uint32_t> local;
        for (;;) {
            {
                std::unique_lock<std::mutex> lk(em);
                ecv.wait(lk, [&] { return snapReady || encStop; });
                if (encStop) return;
                local.swap(snap);
                snapReady = false;
            }
            encodeFrame(local.data());
        }
    }

    void kick(const char *why) {   // play phase disconnect, reason as an NBT string component
        W d;
        d.vi(out::Disconnect).u8(8).i16((int)strlen(why));
        d.b.insert(d.b.end(), why, why + strlen(why));
        c.send(d);
        printf("kicked: %s\n", why);
        c.dead = true;
    }

    void key(bool down, int k, int ascii = 0) { dg_key(down ? 1 : 0, k, ascii); }

    void tap(int k, int ascii, double ms = 120) {
        key(true, k, ascii);
        pulses.push_back({k, ascii, nowMs() + ms});
    }

    void syncKeys() {
        double t = nowMs();
        auto apply = [&](bool &state, bool want, int k) { if (state != want) { state = want; key(want, k); } };
        apply(kFwd, held.fwd, KEY_UPARROW);
        apply(kBack, held.back, KEY_DOWNARROW);
        // On a Doom screen (menu sliders, automap) sideways keys press the left and right arrows, repeating while held; in the game they strafe.
        const bool screen = dg_screen_mode() != 0;
        auto arrow = [&](bool down, bool &was, double &next, int k) {
            if (!down) { was = false; return; }
            if (!was) { was = true; tap(k, 0, 60); next = t + 350.0; }
            else if (t >= next) { tap(k, 0, 60); next = t + 110.0; }
        };
        if (screen) { arrow(held.left, arrowL, nextArrowL, KEY_LEFTARROW); arrow(held.right, arrowR, nextArrowR, KEY_RIGHTARROW); }
        else { arrowL = arrowR = false; }
        apply(kL, held.left && !screen, KEY_STRAFE_L);
        apply(kR, held.right && !screen, KEY_STRAFE_R);
        const bool useWant = held.use || t < usePulseUntil;
        if (useWant && !kUse && dg_on_title() && dg_has_autosave()) { dg_load_autosave(); kUse = true; }   // title picture: use continues the saved game
        else apply(kUse, useWant, KEY_USE);
        apply(kRun, held.run, KEY_RSHIFT);
        apply(kFire, fireHeld || t < firePulseUntil, KEY_FIRE);
        for (size_t i = 0; i < pulses.size();) {
            if (t >= pulses[i].until) { key(false, pulses[i].key, pulses[i].ascii); pulses.erase(pulses.begin() + (long)i); }
            else i++;
        }
    }

    static float wrap180(float a) {
        while (a > 180) a -= 360;
        while (a < -180) a += 360;
        return a;
    }

    // The client reports the (hidden) player's rotation every tick while seated. The camera is locked to a
    // separate entity, so the picture never moves and nothing has to be snapped back. Only the change in yaw
    // matters; pitch is ignored because Doom has no vertical look.
    void onLook(float yaw, float pitch) {
        float dy = wrap180(yaw - lastYaw);
        lastYaw = yaw;
        rawYaw = yaw;
        rawPitch = pitch;
        if (dy != 0) lastActive = nowMs();
        if (cfg.debug && dy != 0) printf("look yaw=%.2f dy=%.2f\n", yaw, dy);
        if (!cfg.clientRender) turnUnits += (double)dy * cfg.sens * (65536.0 / 360.0 / 8.0);   // right turn = positive mouse x
    }

    // Doom keeps one mouse event per game tic. Releasing about half of the pending turn each tic spreads the
    // 20 Hz yaw reports over several tics, so the turning looks smooth instead of lurching.
    void postMouse() {
        if (!dg_mouse_free()) return;
        int n = (std::fabs(turnUnits) < 4.0) ? (int)std::lround(turnUnits) : (int)std::lround(turnUnits * 0.5);
        if (n == 0) return;
        n = std::max(-4096, std::min(4096, n));
        turnUnits -= n;
        if (cfg.debug) printf("mouse units %d\n", n);
        dg_mouse(n);
    }

    void onPacket(const std::vector<uint8_t> &pkt) {
        R r(pkt);
        uint32_t id = r.vi();
        switch (id) {
        case in::MovePosRot: { r.f64(); r.f64(); r.f64(); float y = r.f32(), p = r.f32(); if (r.ok) onLook(y, p); break; }
        case in::MoveRot: { float y = r.f32(), p = r.f32(); if (r.ok) onLook(y, p); break; }
        case in::PlayerInput: {
            int f = g_freeze ? 0 : r.u8();
            held.fwd = f & 1; held.back = f & 2; held.left = f & 4; held.right = f & 8;
            held.use = f & 16; held.run = (f & 32) || (f & 64);
            if (f) lastActive = nowMs();
            if (cfg.debug) printf("input flags=0x%02x\n", f);
            break;
        }
        case in::Punch: case in::Attack: if (cfg.debug) printf("punch/attack\n"); firePulseUntil = nowMs() + 200; lastActive = nowMs(); break;
        case in::UseItem: case in::UseItemOn: case in::Interact: usePulseUntil = nowMs() + 150; lastActive = nowMs(); break;
        case in::SetCarried: { int s = r.i16(); if (r.ok && s >= 0 && s <= 8) { clientSlot = s; lastCarriedAt = nowMs(); if (pulses.size() < 32) tap('1' + s, '1' + s); } lastActive = nowMs(); break; }
        case in::PlayerAction: {
            uint32_t a = r.vi();
            if (!r.ok) break;
            lastActive = nowMs();
            if (pulses.size() >= 32 && a >= 4) break;
            if (a == 0) fireHeld = true;                       // start destroy block = left button down
            else if (a == 2 || a == 3) fireHeld = false;       // abort / stop destroy = button up
            else if (a == 4) tap('y', 'y');                    // ctrl+Q : yes
            else if (a == 5) tap(KEY_ENTER, 0);                // Q : enter (menu select)
            else if (a == 7) tap(KEY_ESCAPE, 0);               // F : escape (Doom menu)
            break;
        }
        case in::KeepAlive: {   // measured round trip shows up as the ping in the player list
            int64_t sentAt = r.i64();
            if (!r.ok) break;
            lastAck = nowMs();
            int rtt = (int)std::max<double>(0.0, nowMs() - (double)sentAt);
            W p;
            p.vi(out::PlayerInfoUpdate).u8(0x10).vi(1).uuid(0x4D43444F4F4D0001ull, 1).vi((uint32_t)rtt);   // action: update latency
            c.send(p);
            if (cfg.debug) printf("round trip %d ms\n", rtt);
            break;
        }
        case in::ContainerClick: {
            int cid = (int)r.vi();
            r.vi();                       // state id
            int slot = r.i16();
            lastActive = nowMs();
            if (r.ok && cid == 0) {               // player inventory
                (void)slot;
                sendInventory();          // always resync, so nothing can be moved or picked up
            }
            break;
        }
        case in::PingRequest: { int64_t v = r.i64(); if (!r.ok) break; W p; p.vi(out::PongResponse).i64(v); c.send(p); break; }
        case in::PlayerLoaded: hintLoaded = true; break;                  // the client has finished loading the world
        case in::ResourcePack: {                                           // uuid, then the status; 0 = the pack is loaded and its shader runs
            for (int i = 0; i < 16; i++) r.u8();
            uint32_t status = r.vi();
            if (r.ok && status == 0) hintPack = true;
            break;
        }
        default: break;
        }
    }

    // ---- join sequence
    void joinPlay() {
        if (cfg.maps) {   // wall of N x M item frames facing south, camera straight in front of its centre
            mN = cfg.maps;
            mM = mN * 3 / 4;
            mBx0 = (cfg.clientRender ? 33 : (int)PX) - mN / 2;   // client render: far south of the Doom player, inside the loaded chunks
            mY0 = (int)std::lround(EYE_Y) - mM / 2;
            mBz = cfg.clientRender ? 200 : 4;
            camX = mBx0 + mN / 2.0;
            camY = mY0 + mM / 2.0;
            camZ = mBz + 0.03125 + 0.34 * mN;
        }
        int dimType = indexOf("minecraft:dimension_type", "minecraft:overworld");
        int biome = indexOf("minecraft:worldgen/biome", "minecraft:plains");
        std::vector<uint8_t> o;

        W login;
        login.vi(out::Login).i32(PLAYER_ID).u8(0);
        login.vi(1).str("minecraft:overworld");
        login.vi(1).vi(4).vi(4);                 // max players, view distance, simulation distance
        login.u8(0).u8(1).u8(0);                 // reduced debug, death screen, limited crafting
        login.vi((uint32_t)dimType).str("minecraft:overworld").i64(0);
        login.vi(0).vi(0);                       // game mode survival, previous game mode none
        login.u8(0).u8(0).u8(0);                 // debug, flat, no last death location
        login.vi(0).vi(63);                      // portal cooldown, sea level
        login.u8(0).u8(0);                       // online mode, enforces secure chat
        Conn::frame(o, login);

        W vis;   // make the player invisible: the client then skips drawing the first-person arm
        vis.vi(out::SetEntityData).vi(PLAYER_ID).u8(0).vi(0).u8(0x20).u8(255);
        if (!cfg.freecam || cfg.clientRender) Conn::frame(o, vis);   // no first person arm over the menu

        W info;   // add ourselves to the player list (actions: add player, game mode, listed, latency)
        info.vi(out::PlayerInfoUpdate).u8(0x1D).vi(1).uuid(0x4D43444F4F4D0001ull, 1).str(name).vi(0).vi(0).u8(1).vi(0);
        Conn::frame(o, info);

        W abil;
        abil.vi(out::PlayerAbilities).u8(cfg.clientRender ? 0x01 : (cfg.freecam ? 0x0F : 0)).f32(0.05f).f32(0.1f);
        Conn::frame(o, abil);

        W held0; held0.vi(out::SetHeldSlot).vi(0);
        Conn::frame(o, held0);

        W ge; ge.vi(out::GameEvent).u8(13).f32(0);   // level chunks load start
        Conn::frame(o, ge);
        const int CCX = cfg.freecam ? 2 : 0, CCZ = cfg.freecam ? 7 : 0;   // freecam starts at the Doom player start (1056, -3616)
        W cc; cc.vi(out::SetChunkCacheCenter).vi((uint32_t)CCX).vi((uint32_t)CCZ);
        Conn::frame(o, cc);

        W bs; bs.vi(out::ChunkBatchStart);
        Conn::frame(o, bs);
        const int RAD = cfg.clientRender ? 8 : 6;   // client wants the whole view distance square before it leaves the loading screen; client render needs every level (up to 112 blocks from its centre) inside
        for (int dx = -RAD; dx <= RAD; dx++)
            for (int dz = -RAD; dz <= RAD; dz++) Conn::frame(o, buildChunkPacket(biome, CCX + dx, CCZ + dz));
        W bf; bf.vi(out::ChunkBatchFinished).vi((2 * RAD + 1) * (2 * RAD + 1));
        Conn::frame(o, bf);

        W pos;
        pos.vi(out::PlayerPosition).vi(1).f64(cfg.freecam ? 33.0 : PX).f64(cfg.freecam ? 64.0 + 41.0 / 32.0 - 1.62 : PY).f64(cfg.freecam ? 113.0 : PZ).f64(0).f64(0).f64(0).f32(HOME_YAW).f32(0).i32(0);
        Conn::frame(o, pos);

        if (!cfg.freecam) {
        W veh;   // invisible seat for the player
        veh.vi(out::AddEntity).vi(VEHICLE_ID).uuid(0x444F4F4D00000001ull, 1).vi(ET_INTERACTION);
        veh.f64(PX).f64(PY).f64(PZ).u8(0).u8(0).u8(0).u8(0).vi(0);
        Conn::frame(o, veh);
        W ride; ride.vi(out::SetPassengers).vi(VEHICLE_ID).vi(1).vi(PLAYER_ID);
        Conn::frame(o, ride);

        W cam;   // fixed camera, looking north at the screen (yaw byte 128 = 180 degrees)
        cam.vi(out::AddEntity).vi(CAMERA_ID).uuid(0x444F4F4D00000001ull, 2).vi(ET_INTERACTION);
        cam.f64(camX).f64(camY - CAM_EYE).f64(camZ).u8(0).u8(0).u8(128).u8(128).vi(0);
        Conn::frame(o, cam);
        W setcam; setcam.vi(out::SetCamera).vi(CAMERA_ID);
        Conn::frame(o, setcam);

        const double bgX = cfg.maps ? camX : PX, bgY = cfg.maps ? camY : EYE_Y, bgZ = cfg.maps ? mBz - 0.3 : CAM_Z - cfg.dist - 0.3;
        W bg;   // background plane, far behind the picture in the camera direction
        bg.vi(out::AddEntity).vi(BG_ID).uuid(0x444F4F4D00000003ull, 1).vi(ET_TEXT_DISPLAY);
        bg.f64(bgX).f64(bgY).f64(bgZ).u8(0).u8(0).u8(0).u8(0).vi(0);
        Conn::frame(o, bg);
        Conn::frame(o, bgData(true));

        }

        W hp; hp.vi(out::SetHealth).f32(20).vi(20).f32(5);
        Conn::frame(o, hp);

        c.sendRaw(o);
        if (cfg.clientRender) { startClientRender(); startMenuScreen(); sendInventory(); return; }
        if (cfg.freecam) return;
        sendInventory();
        if (cfg.maps) { spawnMaps(); sendMapFrame(); }
        else spawnPixels();
    }

    // ---- state channel: item_display entities whose item tints carry 24 bit numbers (see tools/bake_data_items.py)
    static constexpr int DATA_GROUPS = 24, DATA_CELLS = 16, DATA_ID = 6000;   // keep in step with tools/bake_data_items.py
    static constexpr int COMP_ITEM_MODEL = 10, COMP_CUSTOM_MODEL_DATA = 17, ET_ITEM_DISPLAY = 73, ITEM_STICK = 1060;   // ids read from the 26.3 client
    uint16_t dataWords[DATA_GROUPS][DATA_CELLS] = {};      // 16 bit words; the client checks them with a check byte
    uint16_t dataSent[DATA_GROUPS][DATA_CELLS] = {};
    bool dataFirst = true;

    void dataSetWord(int d, uint32_t v) {   // d = word number over all groups
        int g = d / DATA_CELLS, i = d % DATA_CELLS;
        if (g < DATA_GROUPS) dataWords[g][i] = (uint16_t)v;
    }

    W dataItemStack(int g) {
        W w;
        w.vi(1).vi((uint32_t)ITEM_STICK).vi(2).vi(0);   // count, item, 2 components added, none removed
        std::string model = "minecraft:doomdata" + std::to_string(g);
        w.vi(COMP_ITEM_MODEL).str(model);
        w.vi(COMP_CUSTOM_MODEL_DATA).vi(0).vi(0).vi(0).vi(DATA_CELLS);          // floats, flags, strings: none; colours: 64
        for (int i = 0; i < DATA_CELLS; i++) {   // tint colour = high byte, low byte, check byte
            uint32_t hi = dataWords[g][i] >> 8, lo = dataWords[g][i] & 255;
            w.i32((int32_t)((hi << 16) | (lo << 8) | (hi ^ lo ^ 0x5Cu)));
        }
        return w;
    }

    void spawnDataEntities(std::vector<uint8_t> &o, double mx, double my, double mz) {
        const int et = ET_ITEM_DISPLAY;
        for (int g = 0; g < DATA_GROUPS; g++) {
            W a;
            a.vi(out::AddEntity).vi((uint32_t)(DATA_ID + g)).uuid(0x444F4F4D44415441ull, (uint64_t)g + 1).vi((uint32_t)et);
            a.f64(mx).f64(my).f64(mz).u8(0).u8(0).u8(0).u8(0).vi(0);
            Conn::frame(o, a);
        }
    }

    void sendDataEntities(bool full) {
        std::vector<uint8_t> o;
        for (int g = 0; g < DATA_GROUPS; g++) {
            bool changed = full || dataFirst;
            for (int i = 0; i < DATA_CELLS && !changed; i++) changed = dataWords[g][i] != dataSent[g][i];
            if (!changed) continue;
            W d;
            d.vi(out::SetEntityDataPkt).vi((uint32_t)(DATA_ID + g));
            if (full || dataFirst) {
                d.u8(17).vi(3).f32(10.0f);       // view range
                d.u8(20).vi(3).f32(300.0f);      // culling box: never leave the frustum
                d.u8(21).vi(3).f32(300.0f);
            }
            d.u8(23).vi(7).raw(dataItemStack(g).b);    // item stack
            d.u8(255);
            Conn::frame(o, d);
            memcpy(dataSent[g], dataWords[g], sizeof dataSent[g]);
        }
        dataFirst = false;
        if (!o.empty()) c.sendRaw(o);
    }

    // ---- client side rendering: the player rides an invisible entity that follows the Doom player
    static constexpr double SEAT_DY = 1.02;  // eye height of the rider above the vehicle origin, measured on the client (same for interaction and display entities)
    static constexpr int POS_INTERP_TICKS = 2;

    void mcPosition(double &mx, double &my, double &mz) {
        double x = 0, y = 0, vz = 0, a = 0;
        dg_player_state(&x, &y, &vz, &a);
        mcFrom(x, y, vz, mx, my, mz);
    }

    void mcFrom(double x, double y, double vz, double &mx, double &my, double &mz) {
        const int lv = std::max(0, std::min(N_LEVELS - 1, dg_map() - 1));   // each level is shifted so it lies inside the loaded chunks
        mx = x / 32.0 + LEVEL_OX[lv];
        my = 64.0 + (vz + cfg.eyeBoost) / 32.0 - SEAT_DY;
        mz = -y / 32.0 + LEVEL_OZ[lv];
    }

    // Doom's own screens (menu, automap, intermission) are not drawn by the shader. While one is up the server turns the
    // post effect off, moves the camera to a fixed spot in front of a small wall of map frames and streams Doom's frame there.
    bool menuView = false;
    double menuSwitchAt = 0;

    void startMenuScreen() {
        std::vector<uint8_t> o;
        W cam;
        cam.vi(out::AddEntity).vi(CAMERA_ID).uuid(0x444F4F4D00000001ull, 2).vi(ET_INTERACTION);
        cam.f64(camX).f64(camY - CAM_EYE).f64(camZ).u8(0).u8(0).u8(128).u8(128).vi(0);
        Conn::frame(o, cam);
        W bg;   // black background behind the menu wall; parked far above while the shader view is shown (it would cover the beacon)
        bg.vi(out::AddEntity).vi(BG_ID).uuid(0x444F4F4D00000003ull, 1).vi(ET_TEXT_DISPLAY);
        bg.f64(camX).f64(BG_PARK_Y).f64(mBz - 0.3).u8(0).u8(0).u8(0).u8(0).vi(0);
        Conn::frame(o, bg);
        Conn::frame(o, bgData(true));
        c.sendRaw(o);
        spawnMaps();
    }

    void placeMenuBackground(std::vector<uint8_t> &o, bool shown) {
        W q;
        q.vi(out::EntityPositionSync).vi(BG_ID).vi(0).f64(camX).f64(shown ? camY : BG_PARK_Y).f64(mBz - 0.3).f32(0).f32(0).u8(0);
        Conn::frame(o, q);
    }

    // Switching between the shader view and the Doom screens happens in two steps so nothing stale or raw shows:
    //  to a screen: the newest Doom frame is streamed to the maps while the shader view (frozen) is still up; then the camera moves.
    //  back to the game: the shader is switched on and fed while the camera still looks at the (static) maps; then the camera moves.
    enum ViewStep { VS_STABLE, VS_ENTER, VS_LEAVE };
    ViewStep vs = VS_STABLE;
    double vsUntil = 0, vsFrom = 0, fadeInUntil = 0;
    bool enterFade = true;   // fade the game view out before a Doom screen (not for the melt, which has to start at once)

    bool shaderFrozen() const { return dg_wipe_active() || vs == VS_ENTER; }   // the screen melt or a pending switch: keep showing the old picture

    void switchToMenu() {
        menuView = true;
        std::vector<uint8_t> o;
        W fx; fx.vi(out::PostEffects).vi(0);   // no post effect
        Conn::frame(o, fx);
        W sc; sc.vi(out::SetCamera).vi(CAMERA_ID);
        Conn::frame(o, sc);
        placeMenuBackground(o, true);
        for (int g = 0; g < DATA_GROUPS; g++) {   // the data carriers draw into screen cells wherever they are, so park them high above
            W q;
            q.vi(out::EntityPositionSync).vi((uint32_t)(DATA_ID + g)).vi(0).f64(33.0).f64(3000.0).f64(113.0).f32(0).f32(0).u8(0);
            Conn::frame(o, q);
        }
        c.sendRaw(o);
    }

    void updateMenuView(double t) {
        const bool want = dg_screen_mode() != 0;
        if (vs == VS_STABLE) {
            if (want == menuView) return;
            if (want) {   // a Doom screen is coming up: get its first frames to the maps before the camera moves
                vs = VS_ENTER;
                enterFade = !dg_wipe_active();
                vsFrom = t;
                vsUntil = t + (enterFade ? 220.0 : 110.0);
                std::fill(lastFb.begin(), lastFb.end(), 0xFFFFFFFFu);   // force the frame out
            } else {      // back to the game: shader on, data fed, camera stays on the maps for now
                vs = VS_LEAVE;
                vsUntil = t + 400.0;
                nextPostFx = t;        // shader on at once
                nextCarrierSync = 0;   // and the data carriers back to the camera
            }
            return;
        }
        if (t < vsUntil) return;
        if (vs == VS_ENTER) {
            vs = VS_STABLE;
            if (dg_screen_mode()) switchToMenu();
        } else {
            vs = VS_STABLE;
            menuView = false;
            fadeInUntil = t + 200.0 + 250.0;   // black for 200 ms after the camera is back, then the game view fades in
            std::vector<uint8_t> o;
            W sc; sc.vi(out::SetCamera).vi(PLAYER_ID);
            Conn::frame(o, sc);
            placeMenuBackground(o, false);
            c.sendRaw(o);
        }
    }

    void startClientRender() {
        joinMs = nowMs();
        hintPack = cfg.packUrl.empty();
        dg_fullview();
        {   // freeze the world clock at noon: no sunrise glow over the beacon pixels (rate 0)
            int clk = indexOf("minecraft:world_clock", "minecraft:overworld");
            W t;
            t.vi(out::SetTime).i64(0).vi(1).vi((uint32_t)clk).vi(6000).f32(0.0f).f32(0.0f);
            std::vector<uint8_t> tb;
            Conn::frame(tb, t);
            c.sendRaw(tb);
        }
        double mx, my, mz;
        mcPosition(mx, my, mz);
        std::vector<uint8_t> o;
        W veh;
        veh.vi(out::AddEntity).vi(VEHICLE_ID).uuid(0x444F4F4D00000001ull, 1).vi(ET_ITEM_DISPLAY);   // a display entity: its position is interpolated inside the client tick
        veh.f64(mx).f64(my).f64(mz).u8(0).u8(0).u8(0).u8(0).vi(0);
        Conn::frame(o, veh);
        W vd; vd.vi(out::SetEntityDataPkt).vi(VEHICLE_ID).u8(10).vi(1).vi(POS_INTERP_TICKS).u8(255);   // teleport_duration
        Conn::frame(o, vd);
        W ride; ride.vi(out::SetPassengers).vi(VEHICLE_ID).vi(1).vi(PLAYER_ID);
        Conn::frame(o, ride);
        spawnDataEntities(o, mx, my, mz);
        if (!cfg.packUrl.empty()) {
            W p;   // the pack with the shaders; required, so the client asks the player once
            p.vi(out::ResourcePackPush).uuid(0x444F4F4D47500001ull, 1).str(cfg.packUrl).str(cfg.packSha1).u8(1).u8(0);
            Conn::frame(o, p);
        }
        c.sendRaw(o);
    }

    // Test harness (--debug): "doomcmd.txt" holds "x y angle" to teleport the Doom player; the server then writes
    // "doomframe.ppm" (what Doom itself draws) and "doomstate.txt" so the client picture can be compared.
    double nextDump = 0;
    void debugHarness(double t) {
        if (!cfg.debug || t < nextDump) return;
        nextDump = t + 400.0;
        if (FILE *f = fopen("doomcmd.txt", "r")) {
            double x, y, a;
            int fr = 0;
            int nread = fscanf(f, "%lf %lf %lf %d", &x, &y, &a, &fr);
            if (nread >= 3) dg_teleport(x, y, a);
            if (nread >= 3) g_freeze = fr != 0;
            fclose(f);
            remove("doomcmd.txt");
        }
        if (FILE *f = fopen("doomwarp.txt", "r")) {   // a map number 1..9: start that level now
            int m = 0;
            if (fscanf(f, "%d", &m) == 1 && m >= 1 && m <= 9) dg_warp(m);
            fclose(f);
            remove("doomwarp.txt");
        }
        if (FILE *f = fopen("doomline.txt", "r")) {   // a line number: cross it now (walk triggers)
            int ln = -1;
            while (fscanf(f, "%d", &ln) == 1) if (ln >= 0) dg_cross_line(ln);
            fclose(f);
            remove("doomline.txt");
        }
        if (FILE *f = fopen("doomdump.txt", "r")) {   // any content: write the heights of every sector to doomsectors.txt (for checking which sectors move)
            fclose(f);
            remove("doomdump.txt");
            if (FILE *o = fopen("doomsectors.txt", "w")) {
                for (int k = 0, n = dg_num_sectors(); k < n; k++) { int fl, cl, li; dg_sector_state(k, &fl, &cl, &li); fprintf(o, "%d %d %d\n", k, fl, cl); }
                fclose(o);
            }
        }
        if (FILE *f = fopen("doomsec.txt", "r")) {   // "sector floor ceiling" lines: force sector heights
            int sec, fl, cl;
            while (fscanf(f, "%d %d %d", &sec, &fl, &cl) == 3) dg_set_sector(sec, fl, cl);
            fclose(f);
            remove("doomsec.txt");
        }
        double x = 0, y = 0, vz = 0, a = 0;
        const bool havePlayer = dg_player_state(&x, &y, &vz, &a) != 0;
        if (havePlayer) if (FILE *f = fopen("doomstate.txt", "w")) {
            fprintf(f, "%.3f %.3f %.3f %.3f %d dyn", x, y, vz, a, dg_player_sector());
            { const int lv = std::max(0, std::min(N_LEVELS - 1, dg_map() - 1)); fprintf(f, " map %d", lv + 1); for (int k = 0; k < N_DYN_LV[lv]; k++) { int fl, cl, li; dg_sector_state(DYN_SECTORS_LV[lv][k], &fl, &cl, &li); fprintf(f, " %d:%d/%d/%d", DYN_SECTORS_LV[lv][k], fl, cl, li); } }
            fprintf(f, "\n");
            fclose(f);
        }
        const uint32_t *fb = dg_frame();
        if (fb) if (FILE *f = fopen("doomframe.ppm", "wb")) {
            fprintf(f, "P6\n%d %d\n255\n", DW, DH);
            for (int i = 0; i < DW * DH; i++) { uint8_t px[3] = {(uint8_t)(fb[i] >> 16), (uint8_t)(fb[i] >> 8), (uint8_t)fb[i]}; fwrite(px, 1, 3, f); }
            fclose(f);
        }
    }

    int lastSyncTic = -1;
    double lastSyncT = 0;

    void clientRenderPre(double t) {
        debugHarness(t);
        syncRoster(t);
        if (g_freeze) { held = Input(); fireHeld = false; dg_zero_momentum(); }
        double x = 0, y = 0, vz = 0, a = 0;
        if (!menuView && dg_player_state(&x, &y, &vz, &a)) dg_set_angle(-(rawYaw + 90.0));   // Doom angle 0 = east counter clockwise, Minecraft yaw 0 = south clockwise
        if (t >= nextPostFx && (!menuView || vs == VS_LEAVE)) {   // switch the full screen Doom shader on (repeated until the pack has loaded)
            nextPostFx = t + (++postFxSent < 15 ? 2000.0 : 10000.0);
            W p;
            p.vi(out::PostEffects).vi(1).str("minecraft:doomgl");
            c.send(p);
            W at;   // third person camera distance 32 blocks (the maximum): the shader recognises it from the position and renders the Doom view itself (attribute minecraft:camera_distance = id 11 in this build)
            at.vi(out::UpdateAttributes).vi(PLAYER_ID).vi(1).vi(11).f64(32.0).vi(0);
            c.send(at);
        }
    }

    // Right after every Doom tic (35 per second, more often than the client's 20 ticks) the new position goes out,
    // so the client always has something to interpolate to and the camera never stalls and then jumps.
    // State words (16 bit) the client shader reads, see the W_ constants in shaders/post/doomgl.fsh
    static constexpr int W_TIC = 0, W_MAP = 1, W_NSPR = 3, W_DYN = 32, W_SPR = 32 + 3 * DYN_MAX_ALL, MAX_SPR = 48;   // W_SPR matches the baker (gen_level.h)
    static_assert(W_SPR + 4 * MAX_SPR <= DATA_GROUPS * DATA_CELLS, "state words do not fit the data entities");
    void fillState(double) {
        dataSetWord(W_TIC, 1 + (uint32_t)(dg_gametic() % 60000));
        const int lv = std::max(0, std::min(N_LEVELS - 1, dg_map() - 1));
        dataSetWord(W_MAP, (uint32_t)(lv + 1));                             // which baked level the shader walks
        {   // the first ten seconds after joining: the shader shows a hint about the controls (1), the ten after that one about turning sound on (2)
            const double now = nowMs();
            if (hintFrom == 0 && ((hintPack && hintLoaded) || now - joinMs > 90000.0)) {
                hintFrom = now;
                printf("[%s] READY %s (pack loaded and in the game after %.1f s)\n", logTime().c_str(), name.c_str(), (now - joinMs) / 1000.0);
            }
            const double since = now - hintFrom;
            dataSetWord(383, hintFrom == 0 ? 0 : since < 10000.0 ? 1 : since < 20000.0 ? 2 : 0);
        }
        {   // which switches show their pressed picture: bit k of word 31 (k < 16) or word 2 (k >= 16) for the level's k-th switch line
            uint32_t lo = 0, hi = 0;
            for (int k = 0; k < N_SW_LV[lv] && k < 32; k++)
                if ((dg_switch_on(SW_LINES_LV[lv][k]) != 0) != (SW_INIT_LV[lv][k] != 0)) { if (k < 16) lo |= 1u << k; else hi |= 1u << (k - 16); }   // switch lines that start pressed (SW2) show the other picture once used
            dataSetWord(31, lo); dataSetWord(2, hi);
        }
        dataSetWord(26, (uint32_t)(int)std::lround((std::fmod(std::fmod((double)rawYaw, 360.0) + 360.0, 360.0)) / 360.0 * 65536.0) & 0xFFFF);   // the player's own yaw and pitch: the shader undoes the camera flip of the third person front view
        dataSetWord(27, (uint32_t)(int)(std::lround((double)rawPitch * 128.0) + 32768));
        {   // the Doom player's eye in Doom units: the shader compares it with the camera to tell the third person camera from the real view
            double ex = 0, ey = 0, ez = 0, ea = 0;
            if (dg_player_state(&ex, &ey, &ez, &ea)) {
                dataSetWord(28, (uint32_t)std::max(0L, std::min(65535L, std::lround(ex) + 32768)));
                dataSetWord(29, (uint32_t)std::max(0L, std::min(65535L, std::lround(ey) + 32768)));
                dataSetWord(30, (uint32_t)std::max(0L, std::min(65535L, std::lround(ez + cfg.eyeBoost) + 32768)));
            }
        }
        for (int k = 0; k < N_DYN_LV[lv]; k++) {                            // doors, lifts and flickering lights
            int fl, cl, li;
            dg_sector_state(DYN_SECTORS_LV[lv][k], &fl, &cl, &li);
            dataSetWord(W_DYN + 3 * k, (uint32_t)(fl + 32768));
            dataSetWord(W_DYN + 3 * k + 1, (uint32_t)(cl + 32768));
            int flatId = 0;                                                  // floor flat (baker id) in the high byte of the light word: donuts and some lifts change it
            const char *fn = dg_floor_flat_name(DYN_SECTORS_LV[lv][k]);
            for (int f = 1; f < N_FLAT_NAMES; f++) if (strncmp(fn, FLAT_NAMES[f], 8) == 0) { flatId = f; break; }
            dataSetWord(W_DYN + 3 * k + 2, (uint32_t)((li & 255) | (flatId << 8)));
        }
        int h[24];                                                          // status bar and weapon, see dg_hud
        dg_hud(h);
        dataSetWord(4, (uint32_t)std::max(0, h[0]));                         // health
        dataSetWord(5, (uint32_t)std::max(0, h[1]));                         // armor
        dataSetWord(6, (uint32_t)(h[2] < 0 ? 0xFFFF : h[2]));                // ammo of the ready weapon
        for (int i = 0; i < 4; i++) { dataSetWord(7 + i, (uint32_t)h[3 + i]); dataSetWord(11 + i, (uint32_t)h[7 + i]); }
        dataSetWord(15, (uint32_t)h[11]);                                    // keys
        dataSetWord(16, (uint32_t)h[12]);                                    // weapons owned
        dataSetWord(17, (uint32_t)h[13]);                                    // face
        dataSetWord(18, (uint32_t)h[14]);                                    // palette flash
        dataSetWord(19, (uint32_t)h[15]);                                    // ready weapon
        for (int k = 0; k < 2; k++) {                                        // weapon sprite, then muzzle flash
            const int *q = h + 16 + 4 * k;
            dataSetWord(20 + 3 * k, q[0] < 0 ? 0 : (uint32_t)((q[0] & 511) | (q[1] << 9) | (1 << 10)));
            dataSetWord(21 + 3 * k, (uint32_t)(q[2] + 32768));
            dataSetWord(22 + 3 * k, (uint32_t)(q[3] + 32768));
        }
        int th[6 * MAX_SPR];                                                // monsters, items, projectiles: 4 words each
        int n = dg_things(th, MAX_SPR, 2600.0);
        dataSetWord(W_NSPR, (uint32_t)n);
        for (int k = 0; k < n; k++) {
            const int *r = th + 6 * k;
            dataSetWord(W_SPR + 4 * k, (uint32_t)(r[0] + 32768));
            dataSetWord(W_SPR + 4 * k + 1, (uint32_t)(r[1] + 32768));
            dataSetWord(W_SPR + 4 * k + 2, (uint32_t)(r[2] + 32768));
            dataSetWord(W_SPR + 4 * k + 3, (uint32_t)((r[3] & 511) | ((r[4] & 1) << 9) | ((r[4] & 2) << 9) | ((r[4] & 4) << 9) | (((r[5] >> 4) & 15) << 12)));
        }
    }

    // The client moves entities once per client tick (20 per second) while Doom runs at 35 tics per second, so sending
    // after every Doom tic made some client ticks receive two steps (a judder every 200 ms). Instead one update goes
    // out every 25 ms (the display entity interpolates over two client ticks), showing the position one Doom tic ago blended towards the newest, like Doom's own frame
    // interpolation: constant speed, at the price of about 30 ms of delay.
    double posPrev[3] = {0, 0, 0}, posCur[3] = {0, 0, 0}, ticSpan = 28.571, ticSeenAt = 0;
    bool posInit = false;
    double nextCarrierSync = 0;

    void clientRenderPost(double t) {
        int tic = dg_gametic();
        if (tic != lastSyncTic) {
            double x, y, vz, a;
            if (dg_player_state(&x, &y, &vz, &a)) {
                bool jump = !posInit || fabs(x - posCur[0]) + fabs(y - posCur[1]) > 160.0 || tic - lastSyncTic > 4;   // teleport or long stall: no blending
                if (jump) { posPrev[0] = x; posPrev[1] = y; posPrev[2] = vz; } else memcpy(posPrev, posCur, sizeof posPrev);
                posCur[0] = x; posCur[1] = y; posCur[2] = vz;
                posInit = true;
                ticSpan = std::max(1, std::min(4, tic - lastSyncTic)) * (1000.0 / 35.0);
                ticSeenAt = t;
            }
            lastSyncTic = tic;
        }
        if (!posInit || t - lastSyncT < 25.0) return;
        lastSyncT = t;
        {   // fade through black around a switch between the game and a Doom screen (state word 382, 0 = clear, 16 = black)
            int dark = 0;
            if (vs == VS_LEAVE) dark = 16;                                                           // game view being prepared: black until the camera is back
            else if (vs == VS_ENTER && enterFade) dark = (int)std::lround(16.0 * std::min(1.0, std::max(0.0, (t - vsFrom) / std::max(1.0, vsUntil - vsFrom))));
            else if (t < fadeInUntil) dark = (int)std::lround(16.0 * std::min(250.0, fadeInUntil - t) / 250.0);   // full black first (the first frames after the camera switch still hold the old camera's rotation), then the fade
            dataSetWord(382, (uint32_t)std::max(0, std::min(16, dark)));
        }
        if (shaderFrozen()) { sendDataEntities(false); return; }   // the screen melt runs on the maps: the shader keeps its old picture
        double f = std::max(0.0, std::min(1.0, (t - ticSeenAt) / ticSpan));
        double mx, my, mz;
        mcFrom(posPrev[0] + (posCur[0] - posPrev[0]) * f, posPrev[1] + (posCur[1] - posPrev[1]) * f, posPrev[2] + (posCur[2] - posPrev[2]) * f, mx, my, mz);
        W p;
        p.vi(out::EntityPositionSync).vi(VEHICLE_ID).vi(0).f64(mx).f64(my).f64(mz).f32(0).f32(0).u8(0);   // id, linear path (type 0, end position), yaw, pitch, on ground
        c.send(p);
        if (t >= nextCarrierSync && (!menuView || vs == VS_LEAVE)) {   // the data carriers stay near the camera so they are always in view (their culling box is huge)
            nextCarrierSync = t + 500.0;
            for (int g = 0; g < DATA_GROUPS; g++) {
                W q;
                q.vi(out::EntityPositionSync).vi((uint32_t)(DATA_ID + g)).vi(0).f64(mx).f64(my).f64(mz).f32(0).f32(0).u8(0);
                c.send(q);
            }
        }
        fillState(t);
        sendDataEntities(false);
    }

    // ---- background toggle: an item in the inventory, and a huge text_display behind the screen
    W toggleStack() const {
        static const char *names[3] = {"Background: none (click to change)", "Background: black (click to change)", "Background: white (click to change)"};
        const int ids[3] = {ITEM_GLASS, ITEM_BLACK_CONCRETE, ITEM_WHITE_CONCRETE};
        std::string n = names[g_bg];
        W w;
        w.vi(1).vi((uint32_t)ids[g_bg]).vi(1).vi(0).vi(COMP_ITEM_NAME).u8(8).i16((int)n.size());
        w.b.insert(w.b.end(), n.begin(), n.end());
        return w;
    }

    // The tab list name: the player's name, then level and skill as coloured extra parts.
    static void nbtTab(W &w, const RosterEntry &e) {
        static const char *skills[5] = {"Too Young To Die", "Not Too Rough", "Hurt Me Plenty", "Ultra-Violence", "Nightmare"};
        auto field = [&](uint8_t type, const char *name) { w.u8(type); w.i16((int)strlen(name)); w.b.insert(w.b.end(), name, name + strlen(name)); };
        auto str = [&](const std::string &v) { w.i16((int)v.size()); w.b.insert(w.b.end(), v.begin(), v.end()); };
        auto part = [&](const std::string &t, const char *color) { field(8, "text"); str(t); field(8, "color"); str(color); w.u8(0); };
        w.u8(10);
        field(8, "text"); str(e.name);
        field(8, "color"); str("white");
        field(9, "extra"); w.u8(10); w.i32(2);
        part("  E1M" + std::to_string(std::max(1, std::min(9, e.level))), "gold");
        part("  " + std::string(skills[std::max(0, std::min(4, e.skill))]), "red");
        w.u8(0);
    }

    // ---- sound: Doom's sounds and music become Minecraft sound packets (sound.h, tools/SOUND_README.md)
    snd::Engine sound;

    void soundInit() {
        FILE *f = fopen("sound_map.json", "rb");   // next to the server in production, in tools/ when run from the repo
        if (f) fclose(f);
        sound.init(f ? "sound_map.json" : "tools/sound_map.json");
    }

    void soundPump(double t) {
        snd::Listener L;
        double x = 0, y = 0, vz = 0, a = 0;
        if (dg_player_state(&x, &y, &vz, &a)) {
            double mx, my, mz;
            mcFrom(x, y, vz, mx, my, mz);   // vehicle position; the eye is SEAT_DY higher
            L.eyeX = mx; L.eyeY = my + SEAT_DY; L.eyeZ = mz;
            L.doomX = x; L.doomY = y; L.doomZ = vz;
        }
        L.level = dg_map();
        L.yaw = rawYaw; L.pitch = rawPitch;
        L.menu = menuView || vs != VS_STABLE;   // on a Doom screen the camera is the map wall
        L.menuX = camX; L.menuY = camY; L.menuZ = camZ;
        sound.setListener(L);
        DgsEvent e;
        while (dgs_pop_sound(&e)) sound.onDoom(e, t);
        DgsMusic m;
        while (dgs_pop_music(&m)) { if (m.stop) sound.musicStop(); else sound.musicRequest(m.name, m.looping != 0); }
        int which, vol;
        while (dgs_pop_volume(&which, &vol)) sound.setDoomVolume(which, vol);
        std::vector<uint8_t> o;
        sound.pump<W>(t, [&](const W &p) { Conn::frame(o, p); });   // one write for the whole bundle
        if (!o.empty()) c.sendRaw(o);
    }

    // Keep the tab list up to date: add players that came in, update level and skill, remove players that left (once a second).
    RosterEntry rosterSent[64];
    double nextRoster = 0;
    double joinMs = 0;
    // The join hints run on a timer that only starts once the player is really in the game: the pack has loaded (its shader runs) and the
    // client has finished loading the world. A slow pack download must not use up the hint time. After 90 s the timer starts anyway.
    bool hintPack = false, hintLoaded = false;
    double hintFrom = 0;
    void syncRoster(double t) {
        if (t < nextRoster) return;
        nextRoster = t + 1000.0;
        rosterPublish(name, dg_map(), dg_skill());
        RosterEntry now[64];
        rosterRead(now);
        const int self = rosterSelf();
        for (int i = 0; i < 64; i++) {
            const bool mine = i == self;
            const uint64_t hi = mine ? 0x4D43444F4F4D0001ull : 0x4D43444F4F4D1000ull + (uint64_t)i, lo = mine ? 1 : 2;
            RosterEntry &s = rosterSent[i];
            if (!now[i].active) {
                if (s.active && !mine) { W p; p.vi(70).vi(1).uuid(hi, lo); c.send(p); }   // PlayerInfoRemove
                s.active = false;
                continue;
            }
            const bool isNew = !s.active;
            if (!isNew && s.name == now[i].name && s.level == now[i].level && s.skill == now[i].skill) continue;
            W p;
            if (isNew && !mine) {
                p.vi(out::PlayerInfoUpdate).u8(0x3D).vi(1).uuid(hi, lo).str(now[i].name).vi(0).vi(0).u8(1).vi(0).u8(1);   // add, game mode, listed, latency, display name
            } else {
                p.vi(out::PlayerInfoUpdate).u8(0x20).vi(1).uuid(hi, lo).u8(1);                                          // display name only
            }
            nbtTab(p, now[i]);
            c.send(p);
            s = now[i];
            s.active = true;
        }
    }

    // A text component as network NBT: a compound with text, colour and italic off (item lore is italic purple otherwise).
    static void nbtText(W &w, const std::string &t, const char *color, bool bold) {
        auto field = [&](uint8_t type, const char *name) { w.u8(type); w.i16((int)strlen(name)); w.b.insert(w.b.end(), name, name + strlen(name)); };
        auto str = [&](const std::string &v) { w.i16((int)v.size()); w.b.insert(w.b.end(), v.begin(), v.end()); };
        w.u8(10);
        field(8, "text"); str(t);
        field(8, "color"); str(color);
        field(1, "italic"); w.u8(0);
        if (bold) { field(1, "bold"); w.u8(1); }
        w.u8(0);
    }

    // The last inventory slot: a book that lists the controls as Minecraft actions (what the action is called, not which key it is bound to).
    W controlsStack() const {
        static const char *lines[] = {
            "Walk: move forward, back, sideways", "Turn: look around", "Shoot: attack (left click)",
            "Open doors, press switches: use item or jump", "Run: sprint or sneak", "Weapon: hotbar slots 1 to 7 or scroll",
            "Doom menu: swap hands opens and closes it", "In the menu: walk forward and back moves,", "left and right change sliders, drop item selects",
            "Progress autosaves. New Game wipes it."};
        const int n = (int)(sizeof lines / sizeof lines[0]);
        W w;
        w.vi(1).vi(ITEM_BOOK).vi(2).vi(0);   // count, item, two components added, none removed
        w.vi(COMP_ITEM_NAME_ID); nbtText(w, "Doom controls", "gold", true);
        w.vi(COMP_LORE_ID).vi((uint32_t)n);
        for (int i = 0; i < n; i++) nbtText(w, lines[i], "gray", false);
        return w;
    }

    void sendInventory() {   // whole player inventory container: empty except for the toggle and the controls book, cursor empty
        W p;
        p.vi(out::ContainerSetContent).vi(0).vi((uint32_t)++invState).vi(46);
        for (int i = 0; i < 46; i++) {
            if (i == CONTROLS_SLOT) p.raw(controlsStack().b);
            else p.vi(0);
        }
        p.vi(0);
        c.send(p);
    }

    // data of the background entity: colour, and the scale (tiny when switched off so nothing is drawn)
    W bgData(bool full) const {
        const float sx = (g_bg ? 600.0f : 0.001f), sy = (g_bg ? 200.0f : 0.001f), ty = -0.1375f * sy;
        const uint32_t argb = g_bg == 0 ? 0u : (g_bg == 1 ? 0xFF000000u : 0xFFFFFFFFu);
        W p;
        p.vi(out::SetEntityData).vi(BG_ID);
        p.u8(11).vi(39).f32(0).f32(ty).f32(0);                    // translation
        p.u8(12).vi(39).f32(sx).f32(sy).f32(1);                   // scale
        if (full) {
            p.u8(15).vi(0).u8(3);                                 // billboard: center
            p.u8(16).vi(1).vi(0xF000F0u);                         // full brightness
            p.u8(23).vi(5).u8(8).i16(1).u8(' ');                  // text " "
        }
        p.u8(25).vi(1).vi(argb);                                  // background colour
        p.u8(255);
        return p;
    }

    void applyBackground() {
        W p = bgData(false);
        c.send(p);
    }

    // ---- map mode: item frames holding maps. One map is 128x128 pixels and costs the client one entity.
    static constexpr int FRAME_BASE = 5000;

    void spawnMaps() {
        std::vector<uint8_t> o;
        for (int r = 0; r < mM; r++)
            for (int cI = 0; cI < mN; cI++) {
                int k = r * mN + cI;
                W a;
                a.vi(out::AddEntity).vi((uint32_t)(FRAME_BASE + k)).uuid(0x444F4F4D00000002ull, (uint64_t)k + 1).vi(ET_GLOW_ITEM_FRAME);
                a.f64(mBx0 + cI).f64(mY0 + (mM - 1 - r)).f64(mBz);   // block position of the frame
                a.u8(0).u8(0).u8(0).u8(0).vi(3);                      // facing south (3D data value 3)
                Conn::frame(o, a);
                W d;
                d.vi(out::SetEntityData).vi((uint32_t)(FRAME_BASE + k));
                d.u8(0).vi(0).u8(0x20);                               // invisible frame: only the map shows
                d.u8(FRAME_DATA_ITEM).vi(7).vi(1).vi(ITEM_FILLED_MAP).vi(1).vi(0).vi(COMP_MAP_ID).vi((uint32_t)(1 + k));
                d.u8(255);
                Conn::frame(o, d);
            }
        c.sendRaw(o);
        mapPrev.assign((size_t)mN * mM, std::vector<uint8_t>(128 * 128, 0));
        buildTables();
        printf("spawned %dx%d map frames = %dx%d pixels\n", mN, mM, mN * 128, mM * 128);
        startEncoder();
    }

    static float sharpen(float f, float width) {   // 0 = nearest, 1 = bilinear, in between keeps edges crisper
        if (width >= 1.0f) return f;
        if (width <= 0.0f) return f < 0.5f ? 0.0f : 1.0f;
        return std::max(0.0f, std::min(1.0f, (f - 0.5f) / width + 0.5f));
    }

    void buildTables() {
        const int SW = mN * 128, SH = mM * 128;
        auto build = [&](int outN, int inN, std::vector<uint16_t> &a, std::vector<uint16_t> &b, std::vector<uint16_t> &w) {
            a.resize((size_t)outN); b.resize((size_t)outN); w.resize((size_t)outN);
            for (int i = 0; i < outN; i++) {
                float u = (i + 0.5f) * inN / outN - 0.5f;
                int i0 = (int)std::floor(u);
                float f = sharpen(u - i0, cfg.smooth);
                a[(size_t)i] = (uint16_t)std::max(0, std::min(inN - 1, i0));
                b[(size_t)i] = (uint16_t)std::max(0, std::min(inN - 1, i0 + 1));
                w[(size_t)i] = (uint16_t)std::lround(f * 256.0f);
            }
        };
        build(SW, DW, fx0, fx1, fxw);
        build(SH, DH, fy0, fy1, fyw);
        lastFb.assign((size_t)DW * DH, 0xFFFFFFFFu);
        full.assign((size_t)SW * SH, 0);
        rowbuf.assign((size_t)DH * SW * 3, 0);
        int m[8][8] = {{0}};   // 8x8 Bayer matrix for a fixed (flicker free) ordered dither
        for (int n = 1; n < 8; n *= 2)
            for (int y = 0; y < n; y++)
                for (int x = 0; x < n; x++) {
                    int v = m[y][x];
                    m[y][x] = 4 * v; m[y][x + n] = 4 * v + 2; m[y + n][x] = 4 * v + 3; m[y + n][x + n] = 4 * v + 1;
                }
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) ditherTab[y * 8 + x] = (int)std::lround(((m[y][x] + 0.5) / 64.0 - 0.5) * 2.0 * cfg.dither);
    }

    void sendMapFrame() {   // main thread: hand the newest Doom frame to the encoder (older unsent ones are dropped)
        const uint32_t *fb = dg_frame();
        if (!fb || mapPrev.empty()) return;
        if (memcmp(fb, lastFb.data(), (size_t)DW * DH * 4) == 0) return;   // Doom drew nothing new
        memcpy(lastFb.data(), fb, (size_t)DW * DH * 4);
        {
            std::lock_guard<std::mutex> lk(em);
            snap.assign(fb, fb + (size_t)DW * DH);
            snapReady = true;
        }
        ecv.notify_one();
    }

    void encodeFrame(const uint32_t *fb) {   // encoder thread
        const double tStart = nowMs();
        const int SW = mN * 128, SH = mM * 128;

        // 0) which Doom rows changed since the last frame (the status bar and still scenes cost nothing)
        bool dirty[DH];
        if (prevSrc.empty()) { prevSrc.assign(fb, fb + (size_t)DW * DH); std::fill(dirty, dirty + DH, true); }
        else for (int y = 0; y < DH; y++) {
            dirty[y] = memcmp(fb + (size_t)y * DW, prevSrc.data() + (size_t)y * DW, DW * 4) != 0;
            if (dirty[y]) memcpy(prevSrc.data() + (size_t)y * DW, fb + (size_t)y * DW, DW * 4);
        }
        // 1) stretch every Doom row to the full map width
        for (int y = 0; y < DH; y++) {
            if (!dirty[y]) continue;
            const uint32_t *src = fb + (size_t)y * DW;
            uint8_t *dst = rowbuf.data() + (size_t)y * SW * 3;
            for (int X = 0; X < SW; X++) {
                uint32_t p0 = src[fx0[(size_t)X]], p1 = src[fx1[(size_t)X]];
                int w = fxw[(size_t)X], iw = 256 - w;
                dst[X * 3 + 0] = (uint8_t)((((p0 >> 16) & 255) * iw + ((p1 >> 16) & 255) * w) >> 8);
                dst[X * 3 + 1] = (uint8_t)((((p0 >> 8) & 255) * iw + ((p1 >> 8) & 255) * w) >> 8);
                dst[X * 3 + 2] = (uint8_t)(((p0 & 255) * iw + (p1 & 255) * w) >> 8);
            }
        }
        // 2) blend rows vertically, dither, and convert to the nearest map colour (in parallel bands)
        auto band = [&](int ya, int yb) {
            for (int Y = ya; Y < yb; Y++) {
                if (!dirty[fy0[(size_t)Y]] && !dirty[fy1[(size_t)Y]]) continue;
                const uint8_t *r0 = rowbuf.data() + (size_t)fy0[(size_t)Y] * SW * 3, *r1 = rowbuf.data() + (size_t)fy1[(size_t)Y] * SW * 3;
                int w = fyw[(size_t)Y], iw = 256 - w;
                uint8_t *dst = full.data() + (size_t)Y * SW;
                const int *dt = ditherTab + (Y & 7) * 8;
                for (int X = 0; X < SW; X++) {
                    int d = dt[X & 7];
                    int r = ((r0[X * 3] * iw + r1[X * 3] * w) >> 8) + d;
                    int g = ((r0[X * 3 + 1] * iw + r1[X * 3 + 1] * w) >> 8) + d;
                    int b = ((r0[X * 3 + 2] * iw + r1[X * 3 + 2] * w) >> 8) + d;
                    r = r < 0 ? 0 : (r > 255 ? 255 : r);
                    g = g < 0 ? 0 : (g > 255 ? 255 : g);
                    b = b < 0 ? 0 : (b > 255 ? 255 : b);
                    dst[X] = g_lut[(r >> 2) << 12 | (g >> 2) << 6 | (b >> 2)];
                }
            }
        };
        band(0, SH);

        const double tConv = nowMs();
        // 3) cut into maps, send only what changed
        std::vector<uint8_t> o;
        W delim; delim.vi(out::BundleDelim);
        Conn::frame(o, delim);
        int sent = 0;
        for (int r = 0; r < mM; r++)
            for (int cI = 0; cI < mN; cI++) {
                int k = r * mN + cI;
                const uint8_t *tile = full.data() + (size_t)(r * 128) * SW + cI * 128;   // row y is tile + y * SW
                auto &pv = mapPrev[(size_t)k];
                int y0 = 128, y1 = -1, x0 = 128, x1 = -1;   // bounding box of what changed
                for (int y = 0; y < 128; y++) {
                    const uint8_t *cr = tile + (size_t)y * SW, *pr = pv.data() + (size_t)y * 128;
                    if (!memcmp(cr, pr, 128)) continue;
                    int a = 0, b = 127;
                    while (cr[a] == pr[a]) a++;
                    while (cr[b] == pr[b]) b--;
                    y0 = std::min(y0, y); y1 = y; x0 = std::min(x0, a); x1 = std::max(x1, b);
                }
                if (y1 < 0) continue;
                int w = x1 - x0 + 1, h = y1 - y0 + 1;
                W p;
                p.b.reserve((size_t)w * h + 16);
                p.vi(out::MapItemData).vi((uint32_t)(1 + k)).u8(0).u8(0).u8(0);   // scale 0, not locked, no decorations
                p.u8(w).u8(h).u8(x0).u8(y0).vi((uint32_t)(w * h));
                for (int y = y0; y <= y1; y++) {
                    const uint8_t *cr = tile + (size_t)y * SW;
                    p.b.insert(p.b.end(), cr + x0, cr + x1 + 1);
                    memcpy(pv.data() + (size_t)y * 128, cr, 128);
                }
                Conn::frame(o, p);
                sent++;
            }
        Conn::frame(o, delim);
        statChanged += sent; statFrames++; statBytes += (long)o.size();
        const double tBuilt = nowMs();
        if (sent) c.sendRaw(o);
        const double tEnd = nowMs();
        tConvSum += tConv - tStart; tConvMax = std::max(tConvMax, tConv - tStart);
        tSendSum += tEnd - tConv; tSendMax = std::max(tSendMax, tEnd - tConv);
        (void)tBuilt;
    }

    void spawnPixels() {
        const float grow = 1.12f;
        float sx = CW * grow / 0.1f;          // quad is 0.1 x 0.225 blocks at scale 1 (space glyph 4 x 9 px)
        float sy = CH * grow / 0.225f;
        float ty = -0.1375f * sy;                    // quad centre sits 0.1375 above the entity origin
        double x0 = PX - (GW - 1) * 0.5 * CW;
        double yTop = EYE_Y + GH * 0.5 * CH;
        double z = CAM_Z - cfg.dist;

        std::vector<uint8_t> o;
        prev.assign((size_t)GW * GH, 0);
        for (int i = 0; i < GW * GH; i++) {
            int cx = i % GW, cy = i / GW;
            W a;
            a.vi(out::AddEntity).vi((uint32_t)(BASE_ID + i)).uuid(0x444F4F4D00000000ull, (uint64_t)i + 1).vi(ET_TEXT_DISPLAY);
            a.f64(x0 + cx * CW).f64(yTop - (cy + 0.5) * CH).f64(z);
            a.u8(0).u8(0).u8(0).u8(0).vi(0);         // zero movement, rotations, data
            Conn::frame(o, a);
            Conn::frame(o, entityDataPacket(i, 0, true, sx, sy, ty));
            if (o.size() > (1 << 16)) { c.sendRaw(o); o.clear(); }
        }
        c.sendRaw(o);
        printf("spawned %dx%d = %d pixel entities\n", GW, GH, GW * GH);
    }

    void sendFrame() {
        const uint32_t *fb = dg_frame();
        if (!fb) return;
        std::vector<uint8_t> o;
        W delim; delim.vi(out::BundleDelim);
        Conn::frame(o, delim);
        int changed = 0;
        std::vector<uint32_t> raw;
        if (cfg.debug) raw.assign((size_t)GW * GH, 0);
        for (int cy = 0; cy < GH; cy++) {
            int y0 = cy * DH / GH, y1 = std::max(y0 + 1, (cy + 1) * DH / GH);
            for (int cx = 0; cx < GW; cx++) {
                int x0 = cx * DW / GW, x1 = std::max(x0 + 1, (cx + 1) * DW / GW);
                uint32_t r = 0, g = 0, b = 0;
                for (int y = y0; y < y1; y++)
                    for (int x = x0; x < x1; x++) {
                        uint32_t px = fb[y * DW + x];
                        r += (px >> 16) & 255; g += (px >> 8) & 255; b += px & 255;
                    }
                uint32_t n = (uint32_t)((y1 - y0) * (x1 - x0));
                uint32_t col = quant(((r / n) << 16) | ((g / n) << 8) | (b / n));
                int i = cy * GW + cx;
                if (cfg.debug) raw[i] = ((r / n) << 16) | ((g / n) << 8) | (b / n);
                if ((int)col == prev[i]) continue;
                prev[i] = (int)col;
                Conn::frame(o, entityDataPacket(i, col, false, 0, 0, 0));
                changed++;
                if (changed % 3000 == 0) {   // the client allows at most 4096 packets per bundle
                    Conn::frame(o, delim);
                    Conn::frame(o, delim);
                }
            }
        }
        if (cfg.debug && (statFrames % 20) == 0) {   // how many rectangles would merging same-colour pixels leave?
            static const uint32_t masks[4] = {0xF8F8F8u, 0xF0F0F0u, 0xE0E0E0u, 0xC0C0C0u};
            printf("rects of %d pixels:", GW * GH);
            for (uint32_t m : masks) {
                std::vector<uint32_t> q(raw.size());
                for (size_t k = 0; k < raw.size(); k++) q[k] = raw[k] & m;
                printf("  mask %06X: %d", m, countRects(q, GW, GH));
            }
            printf("\n");
        }
        Conn::frame(o, delim);
        statChanged += changed; statFrames++; statBytes += (long)o.size();
        if (changed) c.sendRaw(o);
    }

    double nextAutosave = 0, nextRestartCheck = 0, lastCarriedAt = 0, nextSlotSync = 0;
    int clientSlot = 0;

    // Doom changes weapons by itself (pickups, empty ammo) and refuses keys for weapons you do not own: keep the client's selected
    // hotbar slot equal to the weapon Doom holds, so pressing the number of the weapon you want always sends a change.
    void syncHeldSlot(double t) {
        if (t < nextSlotSync || t - lastCarriedAt < 600.0) return;
        nextSlotSync = t + 200.0;
        int sl = dg_weapon_slot();
        if (sl == clientSlot) return;
        clientSlot = sl;
        W h; h.vi(out::SetHeldSlot).vi((uint32_t)sl);
        c.send(h);
    }


    void run() {
        joinPlay();
        dgs_set_player(name.c_str());
        soundInit();
        if (cfg.clientRender) { menuView = false; switchToMenu(); }   // start on the maps: the title picture (or the game, after the leave step)
        c.enableDelay(cfg.ping / 2.0);   // only after the join, so loading is not slowed down
        double t0 = nowMs();
        lastAck = lastActive = t0;
        nextKeepAlive = t0 + 1000;
        nextFrame = t0 + 500;
        nextAutosave = t0 + 15000;
        double nextStat = t0 + 3000;
        std::vector<uint8_t> pkt;
        const double joinedAt = nowMs();
        printf("[%s] JOIN  %s (%s) slot %d, %d online\n", logTime().c_str(), name.c_str(), peer.c_str(), rosterSelf(), rosterOnline());
        double loopStart = nowMs();
        while (!c.dead) {
            tLoopMax = std::max(tLoopMax, nowMs() - loopStart);
            loopStart = nowMs();
            for (int n = 0; n < 256 && c.poll(pkt); n++) onPacket(pkt);
            double t = nowMs();
            syncKeys();
            if (!cfg.clientRender) postMouse();
            if (cfg.clientRender) clientRenderPre(t);
            doomgeneric_Tick();
            t = nowMs();
            soundPump(t);
            if (cfg.clientRender) { clientRenderPost(t); updateMenuView(t); }
            syncHeldSlot(t);
            if (t >= nextAutosave) { nextAutosave = t + 15000; dg_autosave(); }   // progress is kept for the next visit
            if (t - lastAck > 20000) { kick("Timed out"); break; }
            if (cfg.idleMin > 0 && t - lastActive > cfg.idleMin * 60000.0) { kick("Kicked for being idle"); break; }
            if (t >= nextKeepAlive) {
                W k; k.vi(out::KeepAlive).i64((int64_t)t);
                c.send(k);
                nextKeepAlive = t + 3000;
            }
            if (cfg.debug && t >= nextStat) {
                if (statFrames) printf("stats: %ld frames, %.0f updates/frame, %.0f KB/s, convert %.1f/%.1f ms, build+send %.1f/%.1f ms (avg/max), worst loop %.1f ms\n", statFrames, (double)statChanged / statFrames, statBytes / 1024.0 / 3.0, tConvSum / statFrames, tConvMax, tSendSum / statFrames, tSendMax, tLoopMax);
                tConvSum = tConvMax = tSendSum = tSendMax = tLoopMax = 0;
                statChanged = statFrames = statBytes = 0;
                nextStat = t + 3000;
            }
            if (t >= nextFrame) {
                const bool streamNow = cfg.clientRender ? (menuView || vs != VS_STABLE) : !cfg.freecam;   // client render streams only while a Doom screen is up
                if (!streamNow) {} else if (cfg.maps) sendMapFrame(); else sendFrame();
                nextFrame += 1000.0 / cfg.fps;
                if (nextFrame < t) nextFrame = t + 1000.0 / cfg.fps;
            }
        }
        dg_autosave();
        stopEncoder();
        {
            const int secs = (int)((nowMs() - joinedAt) / 1000.0);
            printf("[%s] LEAVE %s (%s) after %dm %02ds, on E1M%d, %d online\n", logTime().c_str(), name.c_str(), peer.c_str(), secs / 60, secs % 60, dg_map(), std::max(0, rosterOnline() - 1));
        }
    }
};

// ---------------------------------------------------------------- shared state, admission

// The supervisor process forks one process per connection (Doom keeps its state in globals, so every player
// needs their own process). This small block of shared memory tells them how many players are in.
struct Shared {
    std::atomic<int> playing;        // players that passed admission
    std::atomic<int> state[64];      // per child slot: 0 free, 1 connecting, 2 admitted
    struct Rec { std::atomic<int> active, level, skill; char name[20]; } rec[64];   // the player list (tab list)
};
static Shared *g_shared = nullptr;
static int g_slot = 0;
static void rosterPublish(const std::string &name, int level, int skill) {
    Shared::Rec &r = g_shared->rec[g_slot];
    if (!r.active.load()) { memset(r.name, 0, sizeof r.name); memcpy(r.name, name.c_str(), std::min<size_t>(name.size(), 16)); }
    r.level = level; r.skill = skill; r.active = 1;
}
static void rosterClear() { g_shared->rec[g_slot].active = 0; }
static int rosterOnline() { return g_shared ? g_shared->playing.load() : 1; }
static int rosterSelf() { return g_slot; }
static void rosterRead(RosterEntry *out) {
    for (int i = 0; i < 64; i++) {
        const Shared::Rec &r = g_shared->rec[i];
        out[i].active = r.active.load() != 0;
        if (!out[i].active) continue;
        char nm[20]; memcpy(nm, r.name, sizeof nm); nm[19] = 0;
        out[i].name = nm; out[i].level = r.level.load(); out[i].skill = r.skill.load();
    }
}
#ifdef _WIN32
static bool g_admitted = false;
#endif

static bool admit() {
    if (g_shared->playing.fetch_add(1) >= cfg.maxPlayers) { g_shared->playing.fetch_sub(1); return false; }
    g_shared->state[g_slot] = 2;
#ifdef _WIN32
    g_admitted = true;
#endif
    return true;
}

static void initDoom() {
    static bool done = false;
    if (done) return;
    done = true;
    static std::vector<std::string> args;
    static std::vector<char *> av;
    args = {"doom", "-iwad", cfg.iwad, "-nosound", "-nomusic"};
    if (cfg.warp) { args.push_back("-warp"); args.push_back("1"); args.push_back(std::to_string(cfg.warpMap)); args.push_back("-skill"); args.push_back("3"); }
    for (auto &a : cfg.doomArgs) args.push_back(a);
    for (auto &a : args) av.push_back((char *)a.c_str());
    av.push_back(nullptr);
    doomgeneric_Create((int)args.size(), av.data());
}

static void loginDisconnect(Conn &c, const char *msg) {
    std::string js = std::string("{\"text\":\"") + msg + "\"}";
    W p;
    p.vi(0).str(js);
    c.send(p);
}

// ---------------------------------------------------------------- handshake, status, login, configuration

static std::string g_favicon;   // base64 of the 64x64 server list icon (--icon)

static std::string base64(const std::vector<uint8_t> &in) {
    static const char *T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    for (size_t i = 0; i < in.size(); i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < in.size() ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < in.size() ? in[i + 2] : 0);
        o += T[v >> 18 & 63]; o += T[v >> 12 & 63];
        o += i + 1 < in.size() ? T[v >> 6 & 63] : '='; o += i + 2 < in.size() ? T[v & 63] : '=';
    }
    return o;
}

static bool handleConnection(Conn &c, const char *peer) {
    std::vector<uint8_t> pkt;
    if (!c.wait(pkt, 5000)) return false;
    R r(pkt);
    if (r.vi() != 0) return false;
    uint32_t proto = r.vi();
    std::string host = r.str();
    if (host.size() > 255) return false;
    r.u8(); r.u8();  // port
    uint32_t intent = r.vi();
    if (!r.ok) return false;

    if (intent == 1) {
        for (int i = 0; i < 2; i++) {
            if (!c.wait(pkt, 3000)) return false;
            R q(pkt);
            uint32_t id = q.vi();
            if (id == 0) {
                char js[400];
                snprintf(js, sizeof js,
                         "{\"version\":{\"name\":\"%s\",\"protocol\":%u},\"players\":{\"max\":%d,\"online\":%d},"
                         "\"description\":{\"text\":\"Doom\",\"color\":\"#ff3a00\",\"bold\":true}",
                         MC_VERSION_ID, PROTOCOL, cfg.maxPlayers, g_shared->playing.load());
                std::string status = js;
                if (!g_favicon.empty()) status += ",\"favicon\":\"data:image/png;base64," + g_favicon + "\"";
                status += "}";
                W p; p.vi(0).str(status);
                c.send(p);
            } else if (id == 1) {
                int64_t v = q.i64();
                W p; p.vi(1).i64(v);
                c.send(p);
                return false;
            }
        }
        return false;
    }
    if (intent != 2 && intent != 3) return false;
    if (proto != PROTOCOL) {
        printf("%s: refused, protocol %u (need %u)\n", peer, proto, PROTOCOL);
        loginDisconnect(c, "This server needs Minecraft 26.3");
        return false;
    }

    // login
    if (!c.wait(pkt, 5000)) return false;
    R h(pkt);
    if (h.vi() != 0) return false;
    std::string rawName = h.str();
    if (!h.ok) return false;
    std::string name;   // only what a real name can contain: this ends up in logs and in packets
    for (char ch : rawName)
        if (name.size() < 16 && (isalnum((unsigned char)ch) || ch == '_')) name += ch;
    if (name.empty()) name = "DoomGuy";
    if (!admit()) {
        printf("%s: refused, server full\n", peer);
        loginDisconnect(c, "Server full, try again in a minute");
        return false;
    }
    printf("login from %s (%s), %d player(s) in\n", name.c_str(), peer, g_shared->playing.load());
    if (cfg.compress >= 0) {
        W sc; sc.vi(3).vi((uint32_t)cfg.compress);   // login compression: everything after this packet is framed with a data length
        c.send(sc);
        g_compress = cfg.compress;
    }
    {
        W p;
        p.vi(2).uuid(0x4D43444F4F4D0001ull, 1).str(name).vi(0).uuid(0x4D43444F4F4D0002ull, 2);
        c.send(p);
    }
    for (int n = 0;; n++) {
        if (n > 32 || !c.wait(pkt, 5000)) { printf("%s: dropped waiting for login acknowledged\n", peer); return false; }
        R q(pkt);
        if (q.vi() == 3) break;   // login acknowledged
    }

    // configuration
    {
        W p;
        p.vi(15).vi(1).str("minecraft").str("core").str(MC_VERSION_ID);
        c.send(p);
    }
    for (int n = 0;; n++) {
        if (n > 64 || !c.wait(pkt, 10000)) { printf("%s: dropped waiting for known packs\n", peer); return false; }
        R q(pkt);
        if (q.vi() == 7) {
            uint32_t k = q.vi();
            printf("client knows %u pack(s)\n", k);
            if (k == 0) fprintf(stderr, "warning: client does not know minecraft:core %s, join will probably fail\n", MC_VERSION_ID);
            break;
        }
    }
    std::vector<uint8_t> o;
    {
        W f; f.vi(13).vi(1).str("minecraft:vanilla");
        Conn::frame(o, f);
        for (auto &reg : g_regs) {   // vanilla entries carry no data (client uses its own copy), mod entries carry NBT
            W p;
            p.vi(7).raw(reg);
            Conn::frame(o, p);
        }
        // tags.bin holds every vanilla tag (dumped from the client jar); the client refuses to finish
        // configuration if tags that item components reference are missing.
        W tags;
        tags.vi(14).raw(g_tags);
        Conn::frame(o, tags);
        W fin; fin.vi(3);
        Conn::frame(o, fin);
        c.sendRaw(o);
    }
    for (int n = 0;; n++) {
        if (n > 64 || !c.wait(pkt, 10000)) { printf("%s: dropped waiting for the client to finish configuration (n=%d)\n", peer, n); return false; }
        R q(pkt);
        if (q.vi() == 3) break;   // client finished configuration
    }

    initDoom();   // only now, so nobody who has not logged in properly makes the server load a game
#ifndef _WIN32
    alarm(0);     // the connect time limit no longer applies
#endif
    Session s(c);
    s.name = name;
    s.peer = peer;
    s.run();
    rosterClear();
#ifdef _WIN32
    if (g_admitted) { g_shared->playing.fetch_sub(1); g_admitted = false; }
#endif
    return true;
}

// ---------------------------------------------------------------- map colour matching

struct Lab { float L, a, b; };

static Lab toLab(int r8, int g8, int b8) {
    auto lin = [](int v) { float c = v / 255.0f; return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); };
    float r = lin(r8), g = lin(g8), b = lin(b8);
    float x = (0.4124f * r + 0.3576f * g + 0.1805f * b) / 0.95047f;
    float y = (0.2126f * r + 0.7152f * g + 0.0722f * b);
    float z = (0.0193f * r + 0.1192f * g + 0.9505f * b) / 1.08883f;
    auto f = [](float t) { return t > 0.008856f ? std::cbrt(t) : 7.787f * t + 16.0f / 116.0f; };
    float fx = f(x), fy = f(y), fz = f(z);
    return {116.0f * fy - 16.0f, 500.0f * (fx - fy), 200.0f * (fy - fz)};
}

// For every RGB value (6 bits per channel) the nearest map palette colour as the eye sees it (CIELAB distance).
static void buildLut() {
    std::vector<int> cand;
    std::vector<Lab> lab;
    for (int i = 4; i < 256; i++)
        if (MAP_PALETTE[i]) {
            cand.push_back(i);
            lab.push_back(toLab((MAP_PALETTE[i] >> 16) & 255, (MAP_PALETTE[i] >> 8) & 255, MAP_PALETTE[i] & 255));
        }
    auto work = [&](int from, int to) {
        for (int v = from; v < to; v++) {
            int r = ((v >> 12) & 63) * 255 / 63, g = ((v >> 6) & 63) * 255 / 63, b = (v & 63) * 255 / 63;
            Lab q = toLab(r, g, b);
            float best = 1e30f;
            int bi = cand[0];
            for (size_t i = 0; i < cand.size(); i++) {
                float dl = q.L - lab[i].L, da = q.a - lab[i].a, db = q.b - lab[i].b;
                float d = dl * dl + da * da + db * db;
                if (!cfg.lab) {
                    const int pr = (int)((MAP_PALETTE[cand[i]] >> 16) & 255), pg = (int)((MAP_PALETTE[cand[i]] >> 8) & 255), pb = (int)(MAP_PALETTE[cand[i]] & 255);
                    float dr = (float)(r - pr), dg = (float)(g - pg), dbb = (float)(b - pb);
                    d = dr * dr + dg * dg + dbb * dbb;
                }
                if (d < best) { best = d; bi = cand[i]; }
            }
            g_lut[v] = (uint8_t)bi;
        }
    };
    std::vector<std::thread> th;
    const int T = 4, N = 262144;
    for (int t = 1; t < T; t++) th.emplace_back(work, N * t / T, N * (t + 1) / T);
    work(0, N / T);
    for (auto &x : th) x.join();
}

// ---------------------------------------------------------------- main

typedef std::array<uint8_t, 16> IpKey;

static void tuneSocket(SOCKET cs) {
    int nd = 1;
    setsockopt(cs, IPPROTO_TCP, TCP_NODELAY, (const char *)&nd, sizeof nd);
#ifdef _WIN32
    DWORD to = 10000;
    setsockopt(cs, SOL_SOCKET, SO_SNDTIMEO, (const char *)&to, sizeof to);
#else
    timeval to{10, 0};   // a client that stops reading is dropped after 10 seconds
    setsockopt(cs, SOL_SOCKET, SO_SNDTIMEO, (const char *)&to, sizeof to);
#endif
}

#ifndef _WIN32
static SOCKET makeListener(int port) {
    SOCKET ls = socket(AF_INET6, SOCK_STREAM, 0);   // dual stack: IPv6 and IPv4
    int yes = 1;
    if (ls != INVALID_SOCKET) {
        setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
        int off = 0;
        setsockopt(ls, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof off);
        sockaddr_in6 a6{};
        a6.sin6_family = AF_INET6;
        a6.sin6_addr = in6addr_any;
        a6.sin6_port = htons((uint16_t)port);
        if (bind(ls, (sockaddr *)&a6, sizeof a6) == 0 && listen(ls, 32) == 0) return ls;
        closesocket(ls);
    }
    ls = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    sockaddr_in a4{};
    a4.sin_family = AF_INET;
    a4.sin_addr.s_addr = htonl(INADDR_ANY);
    a4.sin_port = htons((uint16_t)port);
    if (bind(ls, (sockaddr *)&a4, sizeof a4) == 0 && listen(ls, 32) == 0) return ls;
    return INVALID_SOCKET;
}

static IpKey keyOf(const sockaddr_storage &ss) {
    IpKey k{};
    if (ss.ss_family == AF_INET6) memcpy(k.data(), &((const sockaddr_in6 &)ss).sin6_addr, 16);
    else {
        k[10] = k[11] = 0xFF;   // v4 mapped
        memcpy(k.data() + 12, &((const sockaddr_in &)ss).sin_addr, 4);
    }
    return k;
}

// The resource pack is small (under 1 MB), so the game port also answers one HTTP request: GET /doomgl.zip. Anything else gets
// a 404. This saves a web server and an extra open port; the child process limits (alarm, address space, no files) still apply.
static std::vector<uint8_t> g_packData;

static bool sendAllTimed(SOCKET s, const char *p, size_t n) {
    while (n > 0) {
        pollfd pf{s, POLLOUT, 0};
        if (poll(&pf, 1, 10000) <= 0) return false;
        ssize_t k = send(s, p, n, MSG_NOSIGNAL);
        if (k <= 0) return false;
        p += k; n -= (size_t)k;
    }
    return true;
}

static void serveHttp(SOCKET cs) {
    char buf[2048];
    size_t n = 0;
    const double t0 = nowMs();
    buf[0] = 0;
    while (n < sizeof buf - 1 && nowMs() - t0 < 5000) {
        pollfd pf{cs, POLLIN, 0};
        if (poll(&pf, 1, 500) <= 0) continue;
        ssize_t k = recv(cs, buf + n, sizeof buf - 1 - n, 0);
        if (k <= 0) return;
        n += (size_t)k;
        buf[n] = 0;
        if (strstr(buf, "\r\n\r\n")) break;
    }
    const bool ok = !g_packData.empty() && strncmp(buf, "GET /doomgl.zip HTTP/1.", 23) == 0;
    char head[200];
    int hl = ok ? snprintf(head, sizeof head, "HTTP/1.1 200 OK\r\nContent-Type: application/zip\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", g_packData.size())
                : snprintf(head, sizeof head, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
    if (!sendAllTimed(cs, head, (size_t)hl)) return;
    if (ok) sendAllTimed(cs, (const char *)g_packData.data(), g_packData.size());
}

static void childMain(SOCKET cs, int slot, const char *peer) {
    g_slot = slot;
    rlimit z{0, 0};
    setrlimit(RLIMIT_CORE, &z);
    setrlimit(RLIMIT_FSIZE, &z);                     // nothing may be written, not even by Doom's save game menu
    signal(SIGXFSZ, SIG_IGN);
    rlimit nf{64, 64};
    setrlimit(RLIMIT_NOFILE, &nf);
    rlimit as{(rlim_t)1 << 30, (rlim_t)1 << 30};
    setrlimit(RLIMIT_AS, &as);
    alarm(60);                                       // login must finish in a minute (cancelled once in game)
    tuneSocket(cs);
    {
        char pk[4];
        pollfd pf{cs, POLLIN, 0};
        if (poll(&pf, 1, 5000) > 0 && recv(cs, pk, 4, MSG_PEEK) == 4 && memcmp(pk, "GET ", 4) == 0) {   // a web request, not a Minecraft handshake
            serveHttp(cs);
            closesocket(cs);
            _exit(0);
        }
    }
    Conn c;
    c.s = cs;
    handleConnection(c, peer);
    c.close();
    _exit(0);
}

// Saved games on disk (Linux): the supervisor process is the only one that may write files. One file per player name:
// "DSV1", name (17 bytes), the seven slot lengths (int32), then the slots' bytes. Written (atomically) a second after something changed.
static void savesLoad() {
    if (cfg.saveDir.empty()) return;
    DIR *d = opendir(cfg.saveDir.c_str());
    if (!d) { fprintf(stderr, "cannot read --save-dir %s\n", cfg.saveDir.c_str()); return; }
    int n = 0;
    while (dirent *de = readdir(d)) {
        std::string f = de->d_name;
        if (f.size() < 5 || f.compare(f.size() - 4, 4, ".sav") != 0) continue;
        FILE *fp = fopen((cfg.saveDir + "/" + f).c_str(), "rb");
        if (!fp) continue;
        char magic[4], name[17];
        int32_t len[SAVE_SLOTS];
        bool ok = fread(magic, 1, 4, fp) == 4 && memcmp(magic, "DSV1", 4) == 0 && fread(name, 1, 17, fp) == 17 && fread(len, 4, SAVE_SLOTS, fp) == (size_t)SAVE_SLOTS;
        name[16] = 0;
        for (int i = 0; ok && i < SAVE_SLOTS; i++) ok = len[i] >= 0 && len[i] <= SAVE_MAX;
        if (ok && n < SAVE_NAMES) {
            SaveEntry &e = g_saves->e[n];
            memset(&e, 0, sizeof e);
            strncpy(e.name, name, 16);
            e.stamp = ++g_saves->clock;
            for (int i = 0; ok && i < SAVE_SLOTS; i++) {
                if (len[i] > 0 && fread(e.data[i], 1, (size_t)len[i], fp) != (size_t)len[i]) ok = false;
                e.len[i] = len[i];
            }
            if (ok) n++; else memset(&e, 0, sizeof e);
        }
        fclose(fp);
    }
    closedir(d);
    printf("loaded the saved games of %d player(s) from %s\n", n, cfg.saveDir.c_str());
}

static void savesFlush() {
    if (cfg.saveDir.empty()) return;
    static SaveEntry copy;   // one entry at a time, copied under the lock, written without it
    for (int i = 0; i < SAVE_NAMES; i++) {
        {
            SaveLock l;
            if (!g_saves->e[i].name[0]) continue;
            memcpy(&copy, &g_saves->e[i], sizeof copy);
        }
        std::string path = cfg.saveDir + "/" + copy.name + ".sav", tmp = path + ".tmp";
        FILE *fp = fopen(tmp.c_str(), "wb");
        if (!fp) { fprintf(stderr, "cannot write %s\n", tmp.c_str()); return; }
        bool ok = fwrite("DSV1", 1, 4, fp) == 4 && fwrite(copy.name, 1, 17, fp) == 17 && fwrite(copy.len, 4, SAVE_SLOTS, fp) == (size_t)SAVE_SLOTS;
        for (int k = 0; ok && k < SAVE_SLOTS; k++) if (copy.len[k] > 0) ok = fwrite(copy.data[k], 1, (size_t)copy.len[k], fp) == (size_t)copy.len[k];
        ok = (fclose(fp) == 0) && ok;
        if (ok) rename(tmp.c_str(), path.c_str()); else remove(tmp.c_str());
    }
}

// The supervisor: accepts connections, applies the limits and forks one process per connection.
static void supervise(SOCKET ls) {
    struct Kid { pid_t pid; int slot; IpKey ip; };
    std::vector<Kid> kids;
    std::map<IpKey, std::deque<double>> hist;
    const size_t maxKids = (size_t)cfg.maxPlayers + 24;   // players plus a few still logging in or pinging
    double lastPrune = 0, lastFlush = 0;
    uint32_t flushedDirty = g_saves->dirty.load();
    for (;;) {
        pollfd pf{ls, POLLIN, 0};
        int pr = poll(&pf, 1, 500);
        if (nowMs() - lastFlush > 1000.0) {   // write changed saved games to disk
            lastFlush = nowMs();
            const uint32_t dv = g_saves->dirty.load();
            if (dv != flushedDirty) { flushedDirty = dv; savesFlush(); }
        }
        int st;
        pid_t pid;
        while ((pid = waitpid(-1, &st, WNOHANG)) > 0) {
            for (size_t i = 0; i < kids.size(); i++)
                if (kids[i].pid == pid) {
                    int sl = kids[i].slot;
                    if (g_shared->state[sl] == 2) g_shared->playing.fetch_sub(1);
                    g_shared->state[sl] = 0;
                    g_shared->rec[sl].active = 0;
                    if (WIFSIGNALED(st)) printf("player process %d died with signal %d\n", (int)pid, WTERMSIG(st));
                    kids.erase(kids.begin() + (long)i);
                    break;
                }
        }
        double now = nowMs();
        if (now - lastPrune > 5000) {
            lastPrune = now;
            for (auto it = hist.begin(); it != hist.end();) {
                while (!it->second.empty() && now - it->second.front() > 10000) it->second.pop_front();
                it = it->second.empty() ? hist.erase(it) : std::next(it);
            }
        }
        if (pr <= 0) continue;
        sockaddr_storage ss{};
        socklen_t sl = sizeof ss;
        SOCKET cs = accept(ls, (sockaddr *)&ss, &sl);
        if (cs == INVALID_SOCKET) { if (errno == EMFILE || errno == ENFILE) sleepMs(100); continue; }
        IpKey ip = keyOf(ss);
        auto &h = hist[ip];
        while (!h.empty() && now - h.front() > 10000) h.pop_front();
        h.push_back(now);
        int same = 0;
        for (auto &k : kids) if (k.ip == ip) same++;
        if (kids.size() >= maxKids || same >= cfg.perIp || h.size() > 12) { closesocket(cs); continue; }
        int slot = -1;
        for (int i = 0; i < 64 && slot < 0; i++)
            if (g_shared->state[i] == 0) slot = i;
        if (slot < 0) { closesocket(cs); continue; }
        char peer[INET6_ADDRSTRLEN + 8] = "?";
        if (ss.ss_family == AF_INET6) {
            const in6_addr &a = ((sockaddr_in6 &)ss).sin6_addr;
            if (IN6_IS_ADDR_V4MAPPED(&a)) inet_ntop(AF_INET, a.s6_addr + 12, peer, sizeof peer);
            else inet_ntop(AF_INET6, &a, peer, sizeof peer);
        } else inet_ntop(AF_INET, &((sockaddr_in &)ss).sin_addr, peer, sizeof peer);
        g_shared->state[slot] = 1;
        pid = fork();
        if (pid == 0) {
            closesocket(ls);
            childMain(cs, slot, peer);
        }
        closesocket(cs);
        if (pid < 0) { g_shared->state[slot] = 0; continue; }
        kids.push_back({pid, slot, ip});
    }
}
#endif

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> const char * { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--port") cfg.port = atoi(next());
        else if (a == "--iwad") cfg.iwad = next();
        else if (a == "--sens") cfg.sens = (float)atof(next());
        else if (a == "--res") { cfg.res = std::max(16, std::min(320, atoi(next()))); cfg.maps = 0; }
        else if (a == "--pixels") cfg.maps = 0;
        else if (a == "--ping") cfg.ping = std::max(0, atoi(next()));
        else if (a == "--smooth") cfg.smooth = (float)atof(next());
        else if (a == "--dither") cfg.dither = (float)atof(next());
        else if (a == "--lab") cfg.lab = true;
        else if (a == "--freecam") { cfg.freecam = true; cfg.maps = 0; }
        else if (a == "--client") { cfg.freecam = true; cfg.clientRender = true; cfg.maps = 4; }   // 4x3 maps (512x384) show the menu, automap and intermission
        else if (a == "--eye-boost") cfg.eyeBoost = atof(next());
        else if (a == "--pack-url") cfg.packUrl = next();
        else if (a == "--pack-sha1") cfg.packSha1 = next();
        else if (a == "--pack-file") cfg.packFile = next();
        else if (a == "--save-dir") cfg.saveDir = next();
        else if (a == "--icon") cfg.icon = next();
        else if (a == "--width") cfg.width = (float)atof(next());
        else if (a == "--eye") EYE_Y = atof(next());
        else if (a == "--maps") cfg.maps = std::max(4, std::min(16, atoi(next()))) / 4 * 4;
        else if (a == "--dist") cfg.dist = (float)atof(next());
        else if (a == "--fps") cfg.fps = std::max(1, std::min(60, atoi(next())));
        else if (a == "--max-players") cfg.maxPlayers = std::max(1, std::min(60, atoi(next())));
        else if (a == "--per-ip") cfg.perIp = std::max(1, atoi(next()));
        else if (a == "--compress") cfg.compress = atoi(next());
        else if (a == "--idle") cfg.idleMin = std::max(0, atoi(next()));
        else if (a == "--menu") cfg.warp = false;
        else if (a == "--warp") cfg.warp = true;
        else if (a == "--map") cfg.warpMap = std::max(1, std::min(9, atoi(next())));
        else if (a == "--debug") cfg.debug = true;
        else if (a == "--") { for (i++; i < argc; i++) cfg.doomArgs.push_back(argv[i]); }
        else { fprintf(stderr, "unknown option %s\n", a.c_str()); return 1; }
    }

    GW = cfg.res;
    GH = cfg.res * 5 / 8;
    CW = cfg.width / (float)GW;
    CH = CW * 1.2f;
    buildLut();
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (!readFile("tags.bin", g_tags) || !loadRegistries("registries.bin")) {
        fprintf(stderr, "tags.bin / registries.bin missing or corrupt (they are dumped from the running game)\n");
        return 1;
    }
    printf("loaded %zu registries and %zu bytes of tags\n", g_regs.size(), g_tags.size());
    if (!cfg.icon.empty()) {
        std::vector<uint8_t> ic;
        if (!readFile(cfg.icon.c_str(), ic) || ic.empty() || ic.size() > 16384) { fprintf(stderr, "cannot read --icon %s (max 16 KB)\n", cfg.icon.c_str()); return 1; }
        g_favicon = base64(ic);
    }
#ifndef _WIN32
    if (!cfg.packFile.empty()) {
        if (!readFile(cfg.packFile.c_str(), g_packData) || g_packData.empty() || g_packData.size() > (32u << 20)) { fprintf(stderr, "cannot read --pack-file %s\n", cfg.packFile.c_str()); return 1; }
        printf("serving %s (%zu bytes) over HTTP on the game port\n", cfg.packFile.c_str(), g_packData.size());
    }
#endif

#ifdef _WIN32
    timeBeginPeriod(1);
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    g_shared = new Shared();
    g_saves = (SaveStore *)calloc(1, sizeof(SaveStore));
    SOCKET ls = socket(AF_INET, SOCK_STREAM, 0);
    int yes = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof yes);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((u_short)cfg.port);
    if (bind(ls, (sockaddr *)&addr, sizeof addr) != 0 || listen(ls, 4) != 0) {
        fprintf(stderr, "cannot listen on port %d\n", cfg.port);
        return 1;
    }
    printf("Doom server for Minecraft %s listening on port %d (Windows: one player at a time)\n", MC_VERSION_ID, cfg.port);
    for (;;) {   // Windows has no fork: one player at a time, in this process
        SOCKET cs = accept(ls, nullptr, nullptr);
        if (cs == INVALID_SOCKET) continue;
        tuneSocket(cs);
        Conn c;
        c.s = cs;
        handleConnection(c, "local");
        c.close();
    }
#else
    signal(SIGPIPE, SIG_IGN);
    void *mem = mmap(nullptr, sizeof(Shared), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) { perror("mmap"); return 1; }
    g_shared = new (mem) Shared();
    void *sm = mmap(nullptr, sizeof(SaveStore), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);   // zero filled
    if (sm == MAP_FAILED) { perror("mmap saves"); return 1; }
    g_saves = (SaveStore *)sm;
    savesLoad();
    for (auto &s : g_shared->state) s = 0;
    g_shared->playing = 0;
    SOCKET ls = makeListener(cfg.port);
    if (ls == INVALID_SOCKET) { fprintf(stderr, "cannot listen on port %d\n", cfg.port); return 1; }
    printf("Doom server for Minecraft %s listening on port %d, up to %d players, one Doom process each\n", MC_VERSION_ID, cfg.port, cfg.maxPlayers);
    supervise(ls);
    return 0;
#endif
}
