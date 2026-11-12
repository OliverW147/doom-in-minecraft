#version 330
#extension GL_ARB_separate_shader_objects : require
#include <minecraft:doomlayout.glsl>

// Doom's renderer as a fragment shader: every pixel casts its own ray through the level and walks from sector
// to sector through the portals (two sided lines), exactly like the data of the WAD describes them.
//   InSampler    the main framebuffer: holds the view rotation written by the sky shader in its bottom left pixels
//   GeoSampler   level data, one 24 bit integer per texel (layout in doomlayout.glsl, made by tools/bake_wad.py)
//   AtlasSampler wall textures and flats as palette indices
//   CmapSampler  Doom's COLORMAP applied to the palette: lit colour of every palette index

uniform sampler2D RotSampler;
uniform sampler2D StateSampler;
uniform sampler2D GeoSampler;
uniform sampler2D AtlasSampler;
uniform sampler2D CmapSampler;
uniform sampler2D SprSampler;

layout(std140) uniform SamplerInfo {
    vec2 OutSize;
    vec2 InSize;
};

layout(std140) uniform Globals {
    ivec3 CameraBlockPos;
    float GlintAlpha;
    vec3 CameraOffset;
    float GameTime;
    vec2 ScreenSize;
    int MenuBlurRadius;
    int UseRgss;
};

layout(location = 0) in vec2 texCoord;
layout(location = 0) out vec4 fragColor;

const float S = 32.0;       // Doom units per block
const float YOFF = 64.0;    // block height of Doom height 0
const int NONE = 65535;

// the baked levels share one data texture; the server says which one is being played (state word 1), its bases are read once
int gListBase, gSecBase, gLdBase, gNodeBase, gSsBase, gNNodes;
#define LIST_BASE gListBase
#define SEC_BASE gSecBase
#define LD_BASE gLdBase
#define NODE_BASE gNodeBase
#define SS_BASE gSsBase
#define NNODES gNNodes

int geo(int i) {
    ivec3 c = ivec3(texelFetch(GeoSampler, ivec2(i & (DATA_W - 1), i / DATA_W), 0).rgb * 255.0 + 0.5);
    return (c.r << 16) | (c.g << 8) | c.b;
}
float geoS(int i) { return float(geo(i) - BIAS); }

// 16 bit state words from the server (see doomdata.glsl and main.cpp): word 0 is non zero once any state has arrived
int sw(int w) {
    ivec3 c = ivec3(texelFetch(StateSampler, ivec2(w, 0), 0).rgb * 255.0 + 0.5);
    return (c.r << 8) | c.g;
}
const int W_NSPR = 3;                                   // W_DYN and W_SPR come from the baker: 3 words per dynamic sector (floor + 32768, ceiling + 32768, light)
bool stateLive() { return sw(0) != 0; }
float secFloor(int sb) { int d = geo(sb + 7); return (d > 0 && stateLive()) ? float(sw(W_DYN + 3 * (d - 1)) - 32768) : geoS(sb); }
float secCeil(int sb) { int d = geo(sb + 7); return (d > 0 && stateLive()) ? float(sw(W_DYN + 3 * (d - 1) + 1) - 32768) : geoS(sb + 1); }
int secFFlat(int sb) { int d = geo(sb + 7); int f = (d > 0 && stateLive()) ? (sw(W_DYN + 3 * (d - 1) + 2) >> 8) : 0; return f > 0 ? f : geo(sb + 2); }   // floor flat: the server sends the current one in the high byte of the light word
int secLight(int sb) { int d = geo(sb + 7); return (d > 0 && stateLive()) ? (sw(W_DYN + 3 * (d - 1) + 2) & 255) : geo(sb + 4); }

float beacon(int i) {
    uvec3 b = uvec3(texelFetch(RotSampler, ivec2(i, 0), 0).rgb * 255.0 + 0.5) >> 4u;
    return float((b.r << 8) | (b.g << 4) | b.b) / 4095.0 * 2.0 - 1.0;
}

int imod(int a, int m) { return a - m * int(floor(float(a) / float(m))); }

// palette index of a wall texture at (u, v) in texels, -1 where the texture is transparent
int wallTexel(int tex, float u, float v) {
    int tb = TEX_BASE + 4 * tex;
    int ay = geo(tb + 1);
    int an = ((ay >> 12) & 7) + 1;                                   // animated texture: the baker stored the frame count and this frame's number above the atlas y
    if (an > 1) { int ai = (ay >> 16) & 7; tex = tex - ai + (ai + sw(0) / 8) % an; tb = TEX_BASE + 4 * tex; ay = geo(tb + 1); }
    int ax = geo(tb), w = geo(tb + 2), h = geo(tb + 3);
    ay &= 4095;
    int iu = imod(int(floor(u)), w), iv = imod(int(floor(v)), h);
    vec4 c = texelFetch(AtlasSampler, ivec2(ax + iu, ay + iv), 0);
    if (c.g < 0.5) return -1;
    return int(c.r * 255.0 + 0.5);
}

int texHeight(int tex) { return geo(TEX_BASE + 4 * tex + 3); }

// Doom's light diminishing: sector light level, wall contrast bias, distance along the view axis
vec3 lit(int idx, int light, int bias, float depth) {
    int ln = clamp((light >> 4) + bias, 0, 15);
    int row = clamp((15 - ln) * 4 - min(47, int(2560.0 / max(depth, 1.0))) / 2, 0, NCMAP - 3);
    return texelFetch(CmapSampler, ivec2(idx, row), 0).rgb;
}

// palette index of a patch (HUD graphic or sprite) at pixel (px, py) from its top left corner, -1 where transparent
int patchAt(int pid, float px, float py) {
    int sb = SPR_BASE + 6 * pid;
    int w = geo(sb + 2), h = geo(sb + 3);
    if (px < 0.0 || py < 0.0 || px >= float(w) || py >= float(h)) return -1;
    vec4 c = texelFetch(SprSampler, ivec2(geo(sb) + int(px), geo(sb + 1) + int(py)), 0);
    return c.g < 0.5 ? -1 : int(c.r * 255.0 + 0.5);
}

// right aligned number made of digit patches (Doom's STlib_drawNum)
int drawNumber(int val, float rightX, float topY, float bx, float by, int base0, float digitW, int maxDigits) {
    int div = 1;
    for (int j = 0; j < maxDigits; j++) {
        if (j > 0 && val < div) break;
        int idx = patchAt(base0 + (val / div) % 10, bx - (rightX - digitW * float(j + 1)), by - topY);
        if (idx >= 0) return idx;
        div *= 10;
    }
    return -1;
}

int statusBar(float bx, float by) {
    int idx = patchAt(P_STBAR, bx, by);
    if (idx < 0) idx = 0;
    int t = patchAt(P_ARMS, bx - 104.0, by);
    if (t >= 0) idx = t;
    int ammo = sw(6);
    if (ammo != 0xFFFF) { t = drawNumber(ammo, 44.0, 3.0, bx, by, P_TNUM0, 14.0, 3); if (t >= 0) idx = t; }
    t = drawNumber(sw(4), 90.0, 3.0, bx, by, P_TNUM0, 14.0, 3); if (t >= 0) idx = t;
    t = patchAt(P_TPRCNT, bx - 90.0, by - 3.0); if (t >= 0) idx = t;
    t = drawNumber(sw(5), 221.0, 3.0, bx, by, P_TNUM0, 14.0, 3); if (t >= 0) idx = t;
    t = patchAt(P_TPRCNT, bx - 221.0, by - 3.0); if (t >= 0) idx = t;
    int owned = sw(16);
    for (int i = 0; i < 6; i++) {
        int pid = ((owned >> i) & 1) != 0 ? P_YNUM0 + 2 + i : P_GNUM2 + i;
        t = patchAt(pid, bx - (111.0 + float(i % 3) * 12.0), by - (4.0 + float(i / 3) * 10.0)); if (t >= 0) idx = t;
    }
    int face = sw(17);
    int facePid = face == 15 ? P_FACEDEAD : (face == 16 ? P_FACEGOD : P_FACE0 + face);
    // Doom draws a patch at (x - leftoffset, y - topoffset); the face patches have offsets (-5, -2), which centre them in the black box
    t = patchAt(facePid, bx - (143.0 - geoS(SPR_BASE + 6 * facePid + 4)), by + geoS(SPR_BASE + 6 * facePid + 5)); if (t >= 0) idx = t;
    int keys = sw(15);
    for (int i = 0; i < 3; i++) {
        int pid = ((keys >> (i + 3)) & 1) != 0 ? P_KEYS0 + 3 + i : (((keys >> i) & 1) != 0 ? P_KEYS0 + i : -1);
        if (pid >= 0) { t = patchAt(pid, bx - 239.0, by - (3.0 + 10.0 * float(i))); if (t >= 0) idx = t; }
    }
    for (int i = 0; i < 4; i++) {
        t = drawNumber(sw(7 + i), 288.0, 5.0 + 6.0 * float(i), bx, by, P_YNUM0, 4.0, 3); if (t >= 0) idx = t;
        t = drawNumber(sw(11 + i), 314.0, 5.0 + 6.0 * float(i), bx, by, P_YNUM0, 4.0, 3); if (t >= 0) idx = t;
    }
    return idx;
}

vec3 skyColor(vec3 d) {
    float ang = degrees(atan(d.y, d.x));
    int sx = imod(int(floor(ang / 360.0 * 1024.0)), 256);
    float hor = max(length(d.xy), 1e-4);
    float row = 100.0 - d.z / hor * 133.3;
    int idx = wallTexel(SKY_TEX, float(sx), clamp(row, 0.0, 127.0));
    if (idx < 0) idx = 0;
    return texelFetch(CmapSampler, ivec2(idx, 0), 0).rgb;
}

// "SWITCH TO THE FIRST PERSON CAMERA" in Doom's small font (ASCII codes), shown while the camera is in third person
const int MSG_LEN = 33;
const int MSG[33] = int[33](83, 87, 73, 84, 67, 72, 32, 84, 79, 32, 84, 72, 69, 32, 70, 73, 82, 83, 84, 32, 80, 69, 82, 83, 79, 78, 32, 67, 65, 77, 69, 82, 65);
// "USE E, F AND Q TO READ CONTROLS AND OPTIONS", shown in the first ten seconds after joining (word 383 of the state)
const int HINT_LEN = 43;
const int HINT[43] = int[43](85, 83, 69, 32, 69, 44, 32, 70, 32, 65, 78, 68, 32, 81, 32, 84, 79, 32, 82, 69, 65, 68, 32, 67, 79, 78, 84, 82, 79, 76, 83, 32, 65, 78, 68, 32, 79, 80, 84, 73, 79, 78, 83);
const int HINT2_LEN = 29; const int HINT2[29] = int[29](84, 85, 82, 78, 32, 71, 65, 77, 69, 32, 83, 79, 85, 78, 68, 83, 32, 65, 78, 68, 32, 77, 85, 83, 73, 67, 32, 79, 78);   // "TURN GAME SOUNDS AND MUSIC ON": word 383 = 2, the ten seconds after the controls hint
int msgLen(int which) { return which == 0 ? MSG_LEN : which == 1 ? HINT_LEN : HINT2_LEN; }
int msgCode(int which, int i) { return which == 0 ? MSG[i] : which == 1 ? HINT[i] : HINT2[i]; }

float msgWidth(int which) {
    float x = 0.0;
    int n = msgLen(which);
    for (int i = 0; i < n; i++) {
        int c = msgCode(which, i);
        x += (c == 32) ? 4.0 : float(geo(SPR_BASE + 6 * (P_FONT + c - 33) + 2));
    }
    return x;
}

// palette index of the message at (px, py) in font pixels from its top left corner, -1 where there is no letter
int msgAt(int which, float px, float py) {
    float x = 0.0;
    int n = msgLen(which);
    for (int i = 0; i < n; i++) {
        int c = msgCode(which, i);
        if (c == 32) { x += 4.0; continue; }
        int pid = P_FONT + c - 33;
        float w = float(geo(SPR_BASE + 6 * pid + 2));
        if (px >= x && px < x + w) return patchAt(pid, px - x, py);
        x += w;
    }
    return -1;
}

vec3 rotX(vec3 v, float a) { float c = cos(a), s = sin(a); return vec3(v.x, c * v.y - s * v.z, s * v.y + c * v.z); }
vec3 rotY(vec3 v, float a) { float c = cos(a), s = sin(a); return vec3(c * v.x + s * v.z, v.y, -s * v.x + c * v.z); }

// the sector that contains a point, by walking the BSP tree. Used when a ray passes through (or within a hair of) the end of a
// linedef: the linedef that was picked may not be the one the ray really crosses, so the sector on the far side is looked up
// from the position just past the crossing instead of being trusted from the linedef.
int sectorAt(vec2 p) {
    int node = NNODES - 1;
    for (int i = 0; i < 32; i++) {
        if ((node & 0x8000) != 0) break;
        int nb = NODE_BASE + 6 * node;
        float nx = geoS(nb), ny = geoS(nb + 1), ndx = geoS(nb + 2), ndy = geoS(nb + 3);
        float dx = p.x - nx, dy = p.y - ny;
        int side = (dy * ndx < ndy * dx) ? 0 : 1;
        node = geo(nb + 4 + side);
    }
    return geo(SS_BASE + (node & 0x7FFF));
}

void main() {
    mat3 R = mat3(vec3(beacon(0), beacon(1), beacon(2)), vec3(beacon(3), beacon(4), beacon(5)), vec3(beacon(6), beacon(7), beacon(8)));

    // Doom's default framing: the 3D view is the top 168 of 200 rows, the status bar is below it
    float f = 1.0 - texCoord.y;                      // 0 at the top of the picture
    bool hud = f > 0.84;                              // status bar area (drawn at the end)
    vec2 ndc = vec2(texCoord.x * 2.0 - 1.0, 1.0 - min(f, 0.84) / 0.84 * 2.0);
    vec3 dv = normalize(vec3(ndc.x, ndc.y * 0.63, -1.0));
    vec3 dw = transpose(R) * dv;
    vec3 fwm = transpose(R) * vec3(0.0, 0.0, -1.0);

    int lvh = LVL_BASE + 10 * (clamp(sw(1), 1, NLEVELS) - 1);
    gListBase = geo(lvh); gSecBase = geo(lvh + 1); gLdBase = geo(lvh + 2); gNodeBase = geo(lvh + 3); gSsBase = geo(lvh + 4); gNNodes = geo(lvh + 5);
    vec2 lvOff = vec2(geoS(lvh + 8), geoS(lvh + 9));   // every level is shifted by whole blocks so it lies inside the chunks the server sends

    vec3 cam = vec3(CameraBlockPos) - CameraOffset;   // Globals.CameraOffset is (block - camera), the negative of the fraction
    vec3 P = vec3((cam.x - lvOff.x) * S, -(cam.z - lvOff.y) * S, (cam.y - YOFF) * S);
    bool thirdPerson = false;

    // Third person (F5): the server sets the camera distance to 32 blocks (1024 Doom units). Then the camera is far from the Doom player's eye,
    // which the server sends (words 28 to 30), and looks at it. That cannot be mistaken for the little lag of the real first person view, so
    // no guessing from angles is needed: the eye is the camera plus the distance along its view direction (smooth, from the client), and the
    // view direction is the player's own yaw and pitch from the server, which is never mirrored. Whatever the player does with the mouse, it
    // is always a valid view; only the turning is a few ticks late.
    if (sw(0) != 0) {
        vec3 camFw = vec3(fwm.x, -fwm.z, fwm.y);
        vec3 Ps = vec3(float(sw(28) - 32768), float(sw(29) - 32768), float(sw(30) - 32768));
        vec3 off = Ps - P;
        float dist = max(length(off), 1.0);
        if (abs(dist - 1024.0) < 480.0 && dot(off / dist, camFw) > 0.7) {
            float syaw = float(sw(26)) * (6.28318530718 / 65536.0);
            float spit = float(sw(27) - 32768) / 128.0 * 0.0174532925;
            thirdPerson = true;
            P = P + camFw * 1024.0;
            dw = rotY(rotX(dv, -spit), 3.14159265 - syaw);
            fwm = rotY(rotX(vec3(0.0, 0.0, -1.0), -spit), 3.14159265 - syaw);
        }
    }
    vec3 d = vec3(dw.x, -dw.z, dw.y);      // Minecraft (x east, y up, z south) to Doom (x east, y north, z up)
    vec3 fw = vec3(fwm.x, -fwm.z, fwm.y);

    // which sector is the camera in: walk the BSP tree
    int node = NNODES - 1;
    for (int i = 0; i < 32; i++) {
        if ((node & 0x8000) != 0) break;
        int nb = NODE_BASE + 6 * node;
        float nx = geoS(nb), ny = geoS(nb + 1), ndx = geoS(nb + 2), ndy = geoS(nb + 3);
        float dx = P.x - nx, dy = P.y - ny;
        int side = (dy * ndx < ndy * dx) ? 0 : 1;
        node = geo(nb + 4 + side);
    }
    int sec = geo(SS_BASE + (node & 0x7FFF));
    int sec0 = sec;

    vec3 col = vec3(0.0);
    float tCur = 0.0;
    int lastLd = -1;        // the linedef the ray just crossed: never the next exit
    float tSurf = 1e9;     // distance along the ray of the first opaque wall or flat: sprites in front of it are drawn
    float axisScale = max(dot(d, fw), 0.05);
    for (int it = 0; it < 64; it++) {
        int sb = SEC_BASE + 8 * sec;
        float fh = secFloor(sb), ch = secCeil(sb);
        int fflat = secFFlat(sb), cflat = geo(sb + 3), light = secLight(sb);
        int lstart = geo(sb + 5), lcount = geo(sb + 6) & 0x7FFF;
        bool hasSelf = (geo(sb + 6) & 0x8000) != 0;       // lines with this sector on both sides and a mid texture (grates)

        float tExit = 1e9, exitU = 0.0;
        int exitLd = -1;
        for (int k = 0; k < lcount; k++) {
            int ld = geo(LIST_BASE + lstart + k);
            int lb = LD_BASE + 24 * ld;
            int fs = geo(lb + 5), bs = geo(lb + 6);
            if (fs == bs || ld == lastLd) continue;
            vec2 A = vec2(geoS(lb), geoS(lb + 1));
            vec2 E = vec2(geoS(lb + 2), geoS(lb + 3)) - A;
            float cr = E.x * d.y - E.y * d.x;                  // > 0: the ray leaves the front (right) side
            if (!((fs == sec && cr > 0.0) || (bs == sec && cr < 0.0))) continue;
            vec2 AO = A - P.xy;
            float den = d.x * E.y - d.y * E.x;
            float t = (AO.x * E.y - AO.y * E.x) / den;
            float u = (AO.x * d.y - AO.y * d.x) / den;
            if (t >= tCur - 0.01 && t < tExit && u >= -0.00002 && u <= 1.00002) { tExit = t; exitU = u; exitLd = ld; }
        }

        // floor or ceiling before the ray reaches the wall?
        float tHit = 1e9;
        bool isFloor = false;
        if (d.z < -1e-5) { float tf = (fh - P.z) / d.z; if (tf > tCur && tf < tExit) { tHit = tf; isFloor = true; } }
        else if (d.z > 1e-5) { float tc = (ch - P.z) / d.z; if (tc > tCur && tc < tExit) { tHit = tc; } }
        if (hasSelf) {
            // Masked lines inside one sector (grates, railings): nearest first; a transparent texel lets the ray go on to the next one.
            float tLim = min(tExit, tHit), tm = tCur;
            bool hitSelf = false;
            for (int j = 0; j < 4 && !hitSelf; j++) {
                float tb = 1e9, bu = 0.0, bcr = 0.0;
                int bl = -1;
                for (int k = 0; k < lcount; k++) {
                    int ld = geo(LIST_BASE + lstart + k);
                    int lb = LD_BASE + 24 * ld;
                    if (geo(lb + 5) != geo(lb + 6)) continue;
                    vec2 A = vec2(geoS(lb), geoS(lb + 1));
                    vec2 E = vec2(geoS(lb + 2), geoS(lb + 3)) - A;
                    float cr = E.x * d.y - E.y * d.x;
                    vec2 AO = A - P.xy;
                    float den = d.x * E.y - d.y * E.x;
                    if (abs(den) < 1e-9) continue;
                    float t = (AO.x * E.y - AO.y * E.x) / den;
                    float u = (AO.x * d.y - AO.y * d.x) / den;
                    if (t > tm + 0.01 && t < tb && t < tLim && u >= 0.0 && u <= 1.0) { tb = t; bl = ld; bu = u; bcr = cr; }
                }
                if (bl < 0) break;
                int lb = LD_BASE + 24 * bl;
                bool front = bcr > 0.0;
                int sdb = lb + (front ? 7 : 12);
                int midTex = geo(sdb + 2);
                tm = tb;
                if (midTex <= 0) continue;
                float mth = float(texHeight(midTex));
                float mtop = ((geo(lb + 4) & 0x10) != 0) ? fh + mth : ch;
                float hz = P.z + tb * d.z;
                float mv = mtop - hz + geoS(sdb + 4);
                if (mv < 0.0 || mv >= mth) continue;
                vec2 A = vec2(geoS(lb), geoS(lb + 1)), B = vec2(geoS(lb + 2), geoS(lb + 3));
                float len = length(B - A);
                float along = bu * len;
                int midx = wallTexel(midTex, (front ? along : len - along) + geoS(sdb + 3), mv);
                if (midx < 0) continue;
                col = lit(midx, light, (A.y == B.y) ? -1 : ((A.x == B.x) ? 1 : 0), tb * axisScale);
                tSurf = tb;
                hitSelf = true;
            }
            if (hitSelf) break;
        }
        if (tHit < 1e8) {
            vec3 pos = P + tHit * d;
            int flatId = isFloor ? fflat : cflat;
            if (!isFloor && flatId == FLAT_SKY) { col = skyColor(d); break; }
            int fb = FLAT_BASE + 2 * flatId;
            int fy = geo(fb + 1);
            int fn = ((fy >> 12) & 7) + 1;                           // animated flat (nukage, water, lava): step to the current frame
            if (fn > 1) { int fi = (fy >> 16) & 7; flatId = flatId - fi + (fi + sw(0) / 8) % fn; fb = FLAT_BASE + 2 * flatId; fy = geo(fb + 1); }
            vec4 c = texelFetch(AtlasSampler, ivec2(geo(fb) + imod(int(floor(pos.x)), 64), (fy & 4095) + imod(int(floor(-pos.y)), 64)), 0);
            col = lit(int(c.r * 255.0 + 0.5), light, 0, tHit * axisScale);
            tSurf = tHit;
            break;
        }
        if (exitLd < 0) {        // no exit line: the ray slipped through a seam, so find the sector it is really in; if that is the same one, leave the ray black
            int seam = sectorAt(P.xy + (tCur + 0.25) * d.xy);
            if (seam == sec) break;
            sec = seam;
            continue;
        }

        int lb = LD_BASE + 24 * exitLd;
        int flags = geo(lb + 4);
        int fs = geo(lb + 5), bs = geo(lb + 6);
        bool frontSide = (fs == sec);
        int other = frontSide ? bs : fs;
        int sdb = lb + (frontSide ? 7 : 12);          // upper, lower, mid, xoffset, yoffset of the side we look at
        int upperTex = geo(sdb), lowerTex = geo(sdb + 1), midTex = geo(sdb + 2);
        float xoff = geoS(sdb + 3), yoff = geoS(sdb + 4);
        vec2 A = vec2(geoS(lb), geoS(lb + 1));
        vec2 B = vec2(geoS(lb + 2), geoS(lb + 3));
        float len = length(B - A);
        float along = exitU * len;
        float u = (frontSide ? along : len - along) + xoff;
        float hz = P.z + tExit * d.z;
        if (hz > ch + 0.01 && cflat == FLAT_SKY) { col = skyColor(d); break; }   // above a sky ceiling (reached through a skipped sky upper wall): only sky
        int bias = (A.y == B.y) ? -1 : ((A.x == B.x) ? 1 : 0);
        float depth = tExit * axisScale;

        int tex = 0;
        float row0 = 0.0;
        if (other == NONE) {
            tex = midTex;
            row0 = ((flags & 0x10) != 0) ? fh + float(texHeight(max(tex, 1))) : ch;
        } else {
            int ob = SEC_BASE + 8 * other;
            float ofl = secFloor(ob), och = secCeil(ob);
            int ocflat = geo(ob + 3);
            if (hz < ofl) {
                tex = lowerTex;
                row0 = ((flags & 0x10) != 0) ? ch : ofl;
            } else if (hz > och) {
                if (cflat == FLAT_SKY && ocflat == FLAT_SKY) { tCur = tExit; lastLd = exitLd; sec = other; continue; }
                tex = upperTex;
                row0 = ((flags & 0x08) != 0) ? ch : och + float(texHeight(max(tex, 1)));
            } else {
                // Open portal. A masked mid texture (railing, grating, window frame) hangs in the opening: Doom draws it once, without
                // repeating it vertically, between its own top and bottom. Transparent texels let the ray go on.
                if (midTex > 0) {
                    float mth = float(texHeight(midTex));
                    float mtop = ((flags & 0x10) != 0) ? max(fh, ofl) + mth : min(ch, och);
                    float mv = mtop - hz + yoff;
                    if (mv >= 0.0 && mv < mth) {
                        int midx = wallTexel(midTex, u, mv);
                        if (midx >= 0) { col = lit(midx, light, bias, depth); tSurf = tExit; break; }
                    }
                }
                tCur = tExit; lastLd = exitLd; sec = other; continue;
            }
        }
        if (geo(lb + 17) == 48) u += float(sw(0));                    // scrolling wall: one unit per tic
        int swk = geo(lb + 20);                                      // switch: show the pressed picture while the server says so
        if (swk > 0 && tex == geo(lb + 21) && ((sw(swk <= 16 ? 31 : 2) >> ((swk - 1) & 15)) & 1) != 0) tex = geo(lb + 22);
        if (tex > 0) {
            int idx = wallTexel(tex, u, row0 - hz + yoff);
            if (idx >= 0) col = lit(idx, light, bias, depth);
        }
        tSurf = tExit;
        break;
    }
    // sprites: vertical billboards facing the view plane, like Doom draws them
    if (sw(0) != 0) {
        int nspr = min(sw(W_NSPR), 48);
        vec2 fh2 = normalize(fw.xy);
        vec2 rh2 = vec2(fh2.y, -fh2.x);
        float dh = dot(d.xy, fh2);
        float bestT = tSurf;
        int sIdx = -1, sInfo = 0;
        for (int k = 0; k < nspr; k++) {
            int wb = W_SPR + 4 * k;
            vec2 sp = vec2(float(sw(wb) - 32768), float(sw(wb + 1) - 32768));
            float depthS = dot(sp - P.xy, fh2);
            if (depthS <= 1.0 || dh <= 1e-4) continue;
            float t = depthS / dh;
            if (t >= bestT) continue;
            vec3 H = P + t * d;
            int info = sw(wb + 3);
            int sb2 = SPR_BASE + 6 * (info & 511);
            int w = geo(sb2 + 2), h = geo(sb2 + 3);
            float lo = geoS(sb2 + 4), to = geoS(sb2 + 5);
            float xo = dot(H.xy - sp, rh2);
            float colf = ((info & 512) != 0) ? float(w) - 1.0 - floor(xo + float(w) - lo) : floor(xo + lo);
            float rowf = floor(float(sw(wb + 2) - 32768) + to - H.z);
            if (colf < 0.0 || colf >= float(w) || rowf < 0.0 || rowf >= float(h)) continue;
            vec4 c = texelFetch(SprSampler, ivec2(geo(sb2) + int(colf), geo(sb2 + 1) + int(rowf)), 0);
            if (c.g < 0.5) continue;
            bestT = t; sIdx = int(c.r * 255.0 + 0.5); sInfo = info;
        }
        if (sIdx >= 0) {
            if ((sInfo & 2048) != 0) col *= 0.55;                                   // fuzzy (spectre): darken what is behind
            else col = lit(sIdx, (sInfo & 1024) != 0 ? 255 : (((sInfo >> 12) & 15) << 4) + 8, 0, bestT * axisScale);
        }
    }
    if (sw(0) != 0) {
        // weapon and muzzle flash are drawn on the screen, in the 320x168 view window like Doom does
        float X = texCoord.x * 320.0, Yw = f / 0.84 * 168.0;
        for (int k = 0; k < 2; k++) {
            int info = sw(20 + 3 * k);
            if ((info & 1024) == 0 || hud) continue;
            int sb2 = SPR_BASE + 6 * (info & 511);
            float lo = geoS(sb2 + 4), to = geoS(sb2 + 5);
            float sx = float(sw(21 + 3 * k) - 32768), sy = float(sw(22 + 3 * k) - 32768);
            int idx = patchAt(info & 511, X - (sx - lo), Yw - (sy - to - 16.0));
            if (idx >= 0) col = lit(idx, (info & 512) != 0 ? 255 : secLight(SEC_BASE + 8 * sec0), 0, 1.0);
        }
        if (hud) col = texelFetch(CmapSampler, ivec2(statusBar(texCoord.x * 320.0, (f - 0.84) / 0.16 * 32.0), 0), 0).rgb;
        int pal = sw(18);                                              // damage, pickup and radiation suit tints
        if (pal >= 1 && pal <= 8) col = mix(col, vec3(0.9, 0.0, 0.0), 0.10 * float(pal));
        else if (pal >= 9 && pal <= 12) col = mix(col, vec3(1.0, 0.85, 0.35), 0.10 * float(pal - 8));
        else if (pal == 13) col = mix(col, vec3(0.0, 0.7, 0.0), 0.2);
    }
    if (hud && sw(0) == 0) col = vec3(0.12, 0.10, 0.08);
    if (thirdPerson && !hud) {
        // tell the player: a dark band across the top of the view with the message in it
        float X = texCoord.x * 320.0, Yw = f / 0.84 * 168.0;
        float raw = msgWidth(0);
        float sc = min(2.0, 292.0 / raw);                  // as large as fits the 320 pixel wide picture
        float tw = raw * sc, x0 = (320.0 - tw) * 0.5, y0 = 14.0;
        if (Yw > y0 - 4.0 && Yw < y0 + 12.0 * sc + 4.0) {
            col *= 0.25;
            int idx = msgAt(0, floor((X - x0) / sc), floor((Yw - y0) / sc));
            if (idx >= 0 && X >= x0 && X < x0 + tw) col = texelFetch(CmapSampler, ivec2(idx, 0), 0).rgb;
        }
    }
    if (sw(0) != 0 && sw(383) != 0 && !hud) {
        // the first seconds after joining: where to read the controls, in a dark band near the bottom of the view
        float X = texCoord.x * 320.0, Yw = f / 0.84 * 168.0;
        int hintId = sw(383);                               // 1: controls hint, 2: sound hint
        float raw = msgWidth(hintId);
        float sc = min(2.0, 300.0 / raw);
        float tw = raw * sc, x0 = (320.0 - tw) * 0.5, y0 = 140.0;
        if (Yw > y0 - 4.0 && Yw < y0 + 12.0 * sc + 4.0) {
            col *= 0.25;
            int idx = msgAt(hintId, floor((X - x0) / sc), floor((Yw - y0) / sc));
            if (idx >= 0 && X >= x0 && X < x0 + tw) col = texelFetch(CmapSampler, ivec2(idx, 0), 0).rgb;
        }
    }
    col *= 1.0 - clamp(float(sw(382)) / 16.0, 0.0, 1.0);   // the server fades the view through black when it switches to or from a Doom screen
    if (sw(0) == 0) col = vec3(0.0);                      // no state yet (just joined, or the carriers are being moved): black, never a wrong view
    fragColor = vec4(col, 1.0);
}
