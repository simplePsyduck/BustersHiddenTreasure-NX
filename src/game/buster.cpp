// Tiny Toon Adventures: Buster's Hidden Treasure (Sega Mega Drive / Genesis, Konami 1993), USA release.
//
// ROM identification for the launcher and the ROM search, and the 16:9 widescreen view.
//
// How the widescreen works (the same recipe as the Mickey & Donald modules; the notes are in docs/WIDESCREEN.md):
//  * The renderer draws `ext` extra columns on each side of the 320-px picture (md.vdp.ext). The game keeps the
//    whole 512-px plane filled with the map (measured to +-96 px), so the extra columns hold real scenery.
//  * Objects (enemies, items) are created when the camera reaches their 16-px map column, 32 px past the edge of
//    the 4:3 view, and deleted 64 px past it; sprites are only drawn within 64 / 48 px of it. In the wider view
//    that would show them popping in and out, so those windows are widened (ROM patches below, each checked
//    against the original words).
//  * The camera is locked so the wide view never leaves the map: it stops `ext` px from the first and last map
//    column (PC hooks in the two camera routines), and the player's screen limits grow by the same amount, so
//    the player still reaches exactly the same places. A one-screen arena, or a section whose camera range is
//    too small for that, keeps the 4:3 camera and is shown with its extension blacked out (the VDP's edge masks).
//  * The HUD sprites are moved out to the screen edges (and are exempt from the masks).
//  * Shown wide: levels (playing, the goal animation, the scenes inside them) and the island map, which is a plane
//    that wraps all round. Logos, title, password, options and the level cards stay 4:3 (pillarboxed).
//
// The ROM is never kept or shipped: the patches are applied in memory to the player's own dump.
#include <algorithm>
#include "game.h"
#include "common.h"
#include <iterator>
#include "../core/md.h"

namespace {

// 68000 RAM ($FFxxxx) the module reads.
constexpr uint32_t LEVEL = 0xFFF940;   // section number 0..$28 ($28 = the stage map)
constexpr uint32_t MODE  = 0xFFF960;   // game state: 2 = playing a level
constexpr uint32_t CAMX  = 0xFFF700;   // camera X, 16.16 (the word at +0 is the pixel position)
constexpr uint32_t MAXX  = 0xFFF8D4;   // the camera's right limit, 16.16
constexpr uint32_t FLAGS = 0xFFF9F4;   // bit 0: fixed-screen arena (camera pinned at 0)
constexpr uint32_t PLAYER = 0xFFA000;  // object slot 0; +2 = screen x (16.16, sprite coordinates)
constexpr int M_PLAY = 2;
constexpr int SECTION_MAP = 0x28;      // sections from here on are the stage map (the game tests `cmpi.w #$28,$FFF940`)

// ---- when the wide view is on ---------------------------------------------------------------------------------
// Game states ($FFF960): 0 title / password / options / the stage map's first visit, 1 loading, 2 playing, 3 a life
// was lost (one frame), 4 the goal animation, 5 game over, 6 scenes inside a level (boss intros, the chest).
// Shown wide: playing a level, its goal animation and its scenes, the island map and the options screen. Not the
// logos, title, password, level cards or game over.
constexpr int M_CLEAR = 4, M_OVER = 5, M_SCENE = 6;
// The island map (sections $28 and up: the game tests `cmpi.w #$28,$FFF940`) is one 512 x 512 plane that wraps
// all round: a view wider than the screen just shows more of it, with nothing to mask and no camera to lock.
bool on_map(const MegaDrive& md) { return md.ram16(LEVEL) >= SECTION_MAP; }
// The options screen: the routine at $6DDE runs once a frame on it (it moves the two running characters), and a hook
// there notes the frame in module_state[4]. Shown wide: the characters run across the whole wide picture.
constexpr uint32_t PC_OPTIONS = 0x006DDE;
bool on_options(const MegaDrive& md) {
    return md.module_state[4] != 0 && md.frame_count - (md.module_state[4] - 1) <= 2;
}
bool wide_view(const MegaDrive& md) {
    if (on_map(md) || on_options(md)) return true;
    const int mode = md.ram16(MODE);
    return mode == M_PLAY || mode == M_CLEAR || mode == M_SCENE;
}
// The wide / 4:3 switch is made while the screen is black (the game fades between its screens), so a fade-out is
// not shown as a pillarbox for a moment. Game over / password (state 5) is cut to 4:3 at once. module_state[6] = what is being shown, [5] = frames waiting for black.
bool screen_black(const MegaDrive& md) {
    for (int i = 0; i < 64; i++) if (md.vdp.cram[i] & 0x0EEE) return false;
    return true;
}
void update_shown(MegaDrive& md) {
    if (md.ram16(MODE) == M_OVER && !on_map(md)) { md.module_state[6] = 0; md.module_state[5] = 0; return; }   // 4:3 at once
    const uint32_t want = wide_view(md) ? 1 : 0;
    if (want == md.module_state[6]) { md.module_state[5] = 0; return; }
    if (screen_black(md) || ++md.module_state[5] > 90) { md.module_state[6] = want; md.module_state[5] = 0; }
}
bool shown_wide(const MegaDrive& md) { return md.module_state[6] != 0; }

// A one-screen arena: the game pins the camera at 0 ($FFF9F4 bit 0) and moves the player instead.
bool fixed_screen(const MegaDrive& md) { return (md.ram8(FLAGS) & 1) != 0; }
// The camera lock follows the whole of a level, scenes included, so the camera does not jump at their edges. It
// needs a camera range of at least 2 x ext, and a scrolling camera.
bool camera_lock(const MegaDrive& md) {
    return md.vdp.ext > 0 && md.ram16(MODE) != 0 && md.ram16(LEVEL) < SECTION_MAP && !fixed_screen(md) &&
           (int16_t)md.ram16(MAXX) >= 2 * md.vdp.ext;
}

// ---- ROM patches --------------------------------------------------------------------------------------------
// L = how much earlier objects are created / how much later they are deleted: the widened view's extension
// rounded up to the 16-px map grid (16, 32 or 48).
int lead_for(int ext) { return (ext + 15) & ~15; }

std::vector<RomPatch> wide_patches(int ext) {
    const uint16_t L = (uint16_t)lead_for(ext);
    const uint16_t C = (uint16_t)(L / 2);        // sprites are drawn this much further out (24 px at 16:9)
    const uint16_t R = (uint16_t)(ext + 10), S = (uint16_t)(ext + 48);   // the options screen's runners (below)
    return {
        // Object spawner ($14390..): column that gets created when the camera crosses a 16-px step. Moving right:
        // camX + 352 (+L). (The left side and the vertical scan are in the hooks below: that code is shared.)
        {0x0143C0, {0x383C, 0x0160}, {0x383C, (uint16_t)(0x0160 + L)}},
        // Window an object lives in, set when it is created: x limit $200 (+ the entry's own extra); its left
        // limit is $240 minus that, so one constant widens both sides.
        {0x0144F0, {0x0640, 0x0200}, {0x0640, (uint16_t)(0x0200 + L)}},
        // The two copies of the default window test (objects that don't carry their own): left, right.
        {0x00DF20, {0x0C40, 0x0040}, {0x0C40, (uint16_t)(0x0040 - L)}},
        {0x00DF2C, {0x0C40, 0x0200}, {0x0C40, (uint16_t)(0x0200 + L)}},
        {0x00E284, {0x0C40, 0x0040}, {0x0C40, (uint16_t)(0x0040 - L)}},
        {0x00E290, {0x0C40, 0x0200}, {0x0C40, (uint16_t)(0x0200 + L)}},
        // The sprite builder ($146A) skips objects whose x is outside $40..$1F0: widen it, so an object whose
        // sprites reach back into the picture is drawn before its origin is.
        {0x001498, {0x0C42, 0x01F0}, {0x0C42, (uint16_t)(0x01F0 + C)}},
        {0x00149E, {0x0C42, 0x0040}, {0x0C42, (uint16_t)(0x0040 - C)}},
        // An enemy that comes in from the left ($EEEE..): created at the right edge's x ($23C) and then moved back by
        // $1D0 to $6C, which is 20 px left of the 4:3 picture: in the wide view it would appear in the picture.
        // Start it L further out.
        {0x00EF00, {0x01D0}, {(uint16_t)(0x01D0 + L)}},
        // The options screen's two running characters ($6DDE..): a table at $6E48 of four runs (x of the first
        // character, then of the second, in sprite coordinates; 0x200 = 384 px, just beyond the 4:3 screen), each
        // for $100 frames at 2 px a frame. Runs 0 and 2 come in from the right and 1 and 3 from the left. A
        // character's picture reaches about 10 px left and 40 px right of its x, and it is only drawn once its x is
        // inside the sprite window, so start the runs far enough out that none of it shows in the wide view: R
        // further out on the right, S on the left (40 px more for the picture's reach), and run S / 2 + 8 frames longer so that they get just as far off the
        // other side.
        {0x006E4A, {0x0200}, {(uint16_t)(0x0200 + R)}}, {0x006E52, {0x01C8}, {(uint16_t)(0x01C8 + R)}},
        {0x006E5A, {0x0040}, {(uint16_t)(0x0040 - S)}}, {0x006E62, {0x0078}, {(uint16_t)(0x0078 - S)}},
        {0x006E6A, {0x01C0}, {(uint16_t)(0x01C0 + R)}}, {0x006E72, {0x0200}, {(uint16_t)(0x0200 + R)}},
        {0x006E7A, {0x0080}, {(uint16_t)(0x0080 - S)}}, {0x006E82, {0x0040}, {(uint16_t)(0x0040 - S)}},
        {0x006E3E, {0x0100}, {(uint16_t)(0x0100 + S / 2 + 8)}},
    };
}

// ---- the camera as drawn ------------------------------------------------------------------------------------
// The picture is drawn from the scroll values uploaded at the last VBlank, which can be a frame older than the
// camera in RAM: take the pixel position from VRAM (modulo the 1024-px scroll range), unwrapped around RAM's.
int drawn_camera(const MegaDrive& md) {
    const VDP& v = md.vdp;
    const int ram = (int16_t)md.ram16(CAMX);
    const uint32_t hsb = (v.reg[13] & 0x3F) << 10;
    const int line = 100;
    int hoff;
    switch (v.reg[11] & 3) {
    case 0: hoff = 0; break;
    case 1: hoff = (line & 7) * 4; break;
    case 2: hoff = (line & ~7) * 4; break;
    default: hoff = line * 4; break;
    }
    const int mod = (-(int)v.vram16(hsb + hoff)) & 0x3FF;
    return ram + ((((mod - ram) + 512) & 0x3FF) - 512);
}

// ---- the HUD ------------------------------------------------------------------------------------------------
// The status bar is sprites: SCORE and the carrot / helper counts at the top left, hearts at the bottom left,
// lives at the bottom right. The game's sprite builder ($146A) draws its objects in layer order, and the HUD's
// own objects (an array at $FFB000..$FFB7FF, layer 0) come before every game object. The bar itself is the objects
// $FFB000..$FFB6FF; further objects of that array are texts (the PAUSE letters are $FFB7C0, in the middle of the
// screen), which must stay where they are. So the bar is the first N entries of the sprite table. Three PC hooks
// in the builder find N: the builder starts, an object is drawn (the first one outside the bar ends it), the
// builder ends (all of it was the bar).
// module_state[0] = N known for this build, [7] = N, [1..3] = table-index bits of the HUD sprites being shown
// (set from [7] in pre_frame, so the picture uses the sprite table that was uploaded, not the one being built).
constexpr uint32_t PC_BUILD = 0x00146A, PC_VISIT = 0x0014A4, PC_DONE = 0x001512;

int sat_index(const MegaDrive& md) { return (int)(((md.cpu.r[13] & 0xFFFF) - 0xF000) >> 3); }   // A5 = next entry

// Camera lock. Both camera routines run: camera += d6; if it went below 0 give the overshoot to the player, put the
// camera at 0, else if it passed the limit D1 (read from $FFF8D4) put it there. Left: before that sign test, do the
// same against `ext` instead of 0 (the player may then go as far left as $90 - ext). Right: after D1 is loaded,
// lower it by `ext`.
constexpr uint32_t PC_CAM_LEFT[2]  = {0x002D84, 0x005F94};   // the `bpl` after `add.l d6,($FFF700)`
constexpr uint32_t PC_CAM_RIGHT[2] = {0x002DAC, 0x005FBC};   // `move.l d1,d2`, right after D1 = the limit

void camera_left(MegaDrive& m) {
    if (!camera_lock(m)) return;
    const int32_t lo = (int32_t)m.vdp.ext << 16;
    const int32_t cam = (int32_t)m.ram32(CAMX);
    if (cam >= lo) return;
    int32_t px = (int32_t)m.ram32(PLAYER + 2) + (cam - lo);       // the overshoot goes to the player
    const int32_t min_px = (int32_t)(0x90 - m.vdp.ext) << 16;
    if (px < min_px) px = min_px;
    m.set_ram32(PLAYER + 2, (uint32_t)px);
    m.set_ram32(CAMX, (uint32_t)lo);
    m.cpu.r[6] = 0;     // d6: no scroll left over (the background stops with it)
    m.cpu.fn = 0;       // N: the `bpl` goes on to the right-hand test
}

void camera_right(MegaDrive& m) {
    if (camera_lock(m)) m.cpu.r[1] -= (uint32_t)m.vdp.ext << 16;
}

// With the limit lowered by `ext`, the player's reach is kept by moving the two numbers that tie it to the limit
// out by `ext`: the "is he past the right end" test (`addi.l #$1400000,d2`) and the screen position he is put at
// when he is ($1B0, in `move.l #$1B00000,$2(a6)`, which the hook performs itself and steps over).
constexpr uint32_t PC_CAM_D2[2]  = {0x002DAE, 0x005FBE};
constexpr uint32_t PC_CAM_MAXX[2] = {0x002DD4, 0x005FE4};
void camera_d2(MegaDrive& m) {
    if (camera_lock(m)) m.cpu.r[2] += (uint32_t)m.vdp.ext << 16;
}
void camera_player_max(MegaDrive& m) {
    if (!camera_lock(m)) return;
    m.set_ram32(PLAYER + 2, (uint32_t)(0x1B0 + m.vdp.ext) << 16);
    m.cpu.pc += 8;   // over the original `move.l #$1B00000,$2(a6)`
}

// The test at $45EC: "the player is not at the left / right limit of the screen ($90 / $1B0)" (then something
// happens). The limits are $90 - ext / $1B0 + ext now.
void edge_test(MegaDrive& m) {
    if (!camera_lock(m)) return;
    const uint32_t x = m.ram32(PLAYER + 2);
    const bool at_limit = x == (uint32_t)(0x90 - m.vdp.ext) << 16 || x == (uint32_t)(0x1B0 + m.vdp.ext) << 16;
    m.cpu.pc = at_limit ? 0x004614 : 0x004600;
}

// ---- terrain outside the streamed plane ---------------------------------------------------------------------------
// The game has no collision map: the terrain probe ($87CE) reads the name-table word of plane A back from VRAM and
// looks the tile up in a table at $FFC000. Plane A is 512 x 512 px and is streamed a column / row at a time around
// two trackers ($FFF704 for X, $FFF70C for Y, each within ~12 px behind the camera): it holds the map from
// tracker - 128 to tracker + 384, and what is further out is still the map from 512 px away. At 4:3 nothing ever
// probes out there: objects are created at most at camX + 367 and deleted at camX + 384. With the windows widened
// they are created at up to camX + 415 (and live to camX + 432), so an object that looks for the ground when it is
// set up (stones, most enemies) found the ground of the place 512 px to the left: it sat too low or too high until
// it left and was created again from another side. So: a probe to the left or right of the streamed area gets the
// word the game itself would put there, composed from the map the same way as the column routine at $86F8..$8776 does:
//   map (ROM, per section: size in 256-px screens at $17BFE / $17C28, layout at the pointer in $17826) -> screen byte
//   (bits 0-5 id, 6 flip Y, 7 flip X) -> 8 x 8 blocks of 32 px ($FF4000: bits 0-6 block, 7 flip X) -> 4 x 4 cells
//   ($FF6000, name-table words). The map wraps at its size, as the game's own routine does ($866C).
// Everywhere else VRAM is still read, as on the cartridge.
constexpr uint32_t MAP_H = 0x017BFE, MAP_W = 0x017C28, MAP_LAYOUT = 0x017826;
constexpr uint32_t TRACK_X = 0xFFF704, TRACK_Y = 0xFFF70C, STREAM_OFF = 0xFFF9F5;   // bit 0: no X streaming, 1: no Y
constexpr uint32_t PC_PROBE = 0x0087E6;   // d0.w = world y, d1.w = world x; then the VRAM read, d2 = the word at $8808
}  // namespace

uint16_t bht_map_cell(const MegaDrive& md, int x, int y) {
    const int sec = md.ram16(LEVEL);
    const int H = md.rom[(MAP_H + sec) % md.rom.size()], W = md.rom[(MAP_W + sec) % md.rom.size()];
    if (!W || !H) return 0;
    x %= W * 256; if (x < 0) x += W * 256;
    y %= H * 256; if (y < 0) y += H * 256;
    const uint32_t layout = md.rom32(MAP_LAYOUT + (uint32_t)sec * 4);
    const uint8_t scr = md.rom[(layout + (uint32_t)((y >> 8) * W + (x >> 8))) % md.rom.size()];
    int bi = (((y >> 5) & 7) << 3) | ((x >> 5) & 7);
    if (scr & 0x80) bi ^= 7;
    if (scr & 0x40) bi ^= 0x38;
    const uint8_t blk = md.ram8(0xFF4000 + (((scr & 0x3F) << 6) | bi));
    const int xf = ((scr ^ blk) >> 7) & 1, yf = (scr >> 6) & 1;
    int ci = (((y >> 3) & 3) << 3) | (((x >> 3) & 3) << 1);
    if (xf) ci ^= 6;
    if (yf) ci ^= 0x18;
    return (uint16_t)(md.ram16(0xFF6000 + (((blk & 0x7F) << 5) | ci)) ^ (xf << 11) ^ (yf << 12));
}

// Is the cell at world (x, y) in the part of plane A that is sure to hold the map around the camera? The plane
// has the map from tracker - 128 to tracker + 384; 32 px are left off each side, because the newest columns / rows
// are still in the transfer queue until the next VBlank (two of them after a slow frame), and a fast camera gets
// ahead of the streaming. That still covers all of the 4:3 screen and all of the wide view.
bool bht_cell_streamed(const MegaDrive& md, int x, int y) {
    const uint8_t off = md.ram8(STREAM_OFF);
    const int tx = (int16_t)md.ram16(TRACK_X) & ~7, ty = (int16_t)md.ram16(TRACK_Y) & ~7;
    const bool in_x = (off & 1) || (x >= tx - 96 && x < tx + 352);
    const bool in_y = (off & 2) || (y >= ty - 96 && y < ty + 352);
    return in_x && in_y;
}

// Does a probe at world x get the composed word? Only when it is outside the plane sideways, which is what the
// widened windows added. Vertically nothing was widened, so a probe that is only above / below the plane (the
// player far above the screen, say) reads what it reads on the cartridge.
bool bht_probe_from_map(const MegaDrive& md, int x) {
    if (md.ram8(STREAM_OFF) & 1) return false;
    const int tx = (int16_t)md.ram16(TRACK_X) & ~7;
    return x < tx - 96 || x >= tx + 352;
}

namespace {
void terrain_probe(MegaDrive& m) {
    if (m.ram16(LEVEL) >= SECTION_MAP) return;
    const int y = (int16_t)(m.cpu.r[0] & 0xFFFF), x = (int16_t)(m.cpu.r[1] & 0xFFFF);
    if (!bht_probe_from_map(m, x)) return;
    const uint16_t w = bht_map_cell(m, x, y);
    m.cpu.r[2] = (m.cpu.r[2] & 0xFFFF0000u) | w;
    m.cpu.r[8] = 0x00C00000;                      // a0, as the skipped `lea ($C00000).l,a0` leaves it
    m.cpu.pc = (w & 0x8000) ? 0x008828 : 0x00880C;   // the two ways on from `move.w (a0),d2 / bmi`
}

// Object spawner, the parts a plain patch can't do. The routine at $1440C is called twice, for the horizontal scan
// (tracker $FFBC96, A2) and for the vertical scan (tracker $FFBC9A); it creates the objects of the column / row
// the camera has just reached, and the routine at $14446 then matches the map entries of that column / row whose
// other coordinate is within a window (horizontal scan: camY - $30 .. camY + $110; vertical scan: camX - $30 ..
// camX + $170, D3 = $170). With the view wider by `ext`:
//  * the left column ($14420: `subi.w #$20,d5`) is moved out by L, for the horizontal scan only. Moved for the
//    vertical scan too it made objects entering at the top appear L px above the screen, outside the delete
//    window, so they were deleted at once and never came back;
//  * the vertical scan's window must grow by L on both sides: its objects are created inside it, and the
//    horizontal scan has moved out by L, so otherwise a strip of objects is never created by either.
constexpr uint32_t PC_SPAWN_LEFT = 0x014420, PC_SPAWN_WINDOW = 0x01444E;
void spawn_left(MegaDrive& m) {
    if ((m.cpu.r[10] & 0xFFFF) != 0xBC96) return;       // A2 = the horizontal tracker
    const uint32_t d5 = m.cpu.r[5];
    m.cpu.r[5] = (d5 & 0xFFFF0000u) | (uint16_t)((d5 & 0xFFFF) - (uint32_t)lead_for(m.vdp.ext));
}
void spawn_window(MegaDrive& m) {
    if ((m.cpu.r[3] & 0xFFFF) != 0x170) return;         // D3 = the vertical scan's upper window
    const uint32_t L = (uint32_t)lead_for(m.vdp.ext);
    m.cpu.r[3] += L;                                    // (the instruction adds D3 to A0 next)
    m.cpu.r[9] -= L;                                    // A1 = the lower end, camX - $30
}

void install_hooks(MegaDrive& md) {
    md.clear_pc_hooks();
    for (uint32_t pc : PC_CAM_LEFT) md.add_pc_hook(pc, camera_left);
    for (uint32_t pc : PC_CAM_RIGHT) md.add_pc_hook(pc, camera_right);
    for (uint32_t pc : PC_CAM_D2) md.add_pc_hook(pc, camera_d2);
    for (uint32_t pc : PC_CAM_MAXX) md.add_pc_hook(pc, camera_player_max);
    md.add_pc_hook(0x0045EC, edge_test);
    md.add_pc_hook(PC_OPTIONS, [](MegaDrive& m) { m.module_state[4] = (uint32_t)m.frame_count + 1; });
    md.add_pc_hook(PC_SPAWN_LEFT, spawn_left);
    md.add_pc_hook(PC_SPAWN_WINDOW, spawn_window);
    md.add_pc_hook(PC_PROBE, terrain_probe);
    md.add_pc_hook(PC_BUILD, [](MegaDrive& m) { m.module_state[0] = 0; });
    md.add_pc_hook(PC_VISIT, [](MegaDrive& m) {
        const uint32_t obj = m.cpu.r[14] & 0xFFFF;   // A6
        if (!m.module_state[0] && !(obj >= 0xB000 && obj < 0xB700)) { m.module_state[7] = (uint32_t)sat_index(m); m.module_state[0] = 1; }
    });
    md.add_pc_hook(PC_DONE, [](MegaDrive& m) {
        if (!m.module_state[0]) { m.module_state[7] = (uint32_t)sat_index(m); m.module_state[0] = 1; }
    });
}

// Is this the status bar as it always is: every sprite in a corner (top: y < 72, bottom: y >= 184; x within 140 px
// of an edge)? The game also draws other things with these objects (score tallies and the like); then nothing
// moves, and they stay where the game put them, in the 4:3 middle of the screen.
bool hud_is_the_bar(const MegaDrive& md, int n) {
    const VDP& v = md.vdp;
    const uint32_t sat = (v.reg[5] & 0x7E) << 9;
    int idx = 0;
    for (int k = 0; k < n && k < 80; k++) {
        const int x = (int)(v.vram16(sat + (uint32_t)idx * 8 + 6) & 0x1FF) - 128, y = (int)(v.vram16(sat + (uint32_t)idx * 8) & 0x3FF) - 128;
        const bool row = y < 72 || y >= 184, col = x < 140 || (x >= 180 && x < 320);
        if (!(row && col)) return false;
        idx = v.vram[(sat + (uint32_t)idx * 8 + 3) & 0xFFFF] & 0x7F;
        if (!idx) break;
    }
    return true;
}

void show_hud(MegaDrive& md, int n) {
    uint32_t bits[3] = {0, 0, 0};
    if (hud_is_the_bar(md, n))
        for (int i = 0; i < n && i < 80; i++) bits[i >> 5] |= 1u << (i & 31);
    for (int i = 0; i < 3; i++) md.module_state[1 + i] = bits[i];
}

bool is_hud(const MegaDrive& md, int index) {
    return index >= 0 && index < 96 && ((md.module_state[1 + (index >> 5)] >> (index & 31)) & 1);
}

bool sprite_is_hud(void* host, int index) { return is_hud(*(const MegaDrive*)host, index); }

// HUD sprites to the edges of the widened screen: those on the left half move out by E, those on the right by E.
// Only sprites in the corners where the bar sits (top: y < 72, bottom: y >= 184; x within 140 px of an edge) move:
// anything else of the HUD array stays where the game put it.
int sprite_xoff(void* host, int index, uint16_t attr, int x, int y) {
    (void)attr;
    const MegaDrive& md = *(const MegaDrive*)host;
    if (!(md.vdp.hook_flags & 1) || !is_hud(md, index)) return 0;
    const int sx = x - 128, sy = y - 128;
    if (!(sy < 72 || sy >= 184)) return 0;
    if (sx >= 0 && sx < 140) return -md.vdp.ext;
    if (sx >= 180 && sx < 320) return md.vdp.ext;
    return 0;
}

// Cheats: ROM patches from Game Genie codes (each code is an address and a replacement word; the original word is
// checked, so a patch that does not fit the ROM is skipped). Decoded and read against the USA ROM (docs/CHEATS.md).
//   BJLA-DA52 carrots: the pick-up ($11470) adds d0 = 1 to the carrot counter ($FFFB0E) through $AB70; 50 carrots
//             give a special helper ($AC0E). The code as published replaces the `bsr.w $AB70` at $11478, which would
//             drop the add and run its displacement word as an instruction, so the same change is made where d0 is
//             loaded instead: moveq #1 -> moveq #10.
//   AJWA-AA8T `subq.w #1,($FFFB10).w` (a helper is used up) -> `bra.s +2`, skipping it.
//   AJYT-AA5G `subq.w #1,($FFFB08).w` (a life is lost) -> `bra.s +2`.
//   ACZT-BA70 the hit routine ($ABB6) adds d0 = -1 to the hearts ($FFFB0A): now 0.
//   97MA-AAGN `move.w #$40,$38(a6)` (the timer set after a hit) -> #$FF.
//   AKAA-AA7T `bsr.w $4E6A` (the gravity routine: adds to the vertical speed $E(a6) each frame) -> `bra.s +2`.
//   AYZA-AADC + 9AZA-B95Y + STZA-B4MW + B2ZA-AAD0 + 6TZA-BX5E: moon jump. The jump test at $2A60 reads the held-button
//             byte ($FFF9E4, bit 5) instead of the pressed one ($FFF9E1, bit 7), and what it does becomes
//             `move.w #$FFF8,$E(a6)`: a steady upward speed of 8 px a frame while the button is held.
const std::vector<RomPatch> kCarrots10 = {{0x011476, {0x7001}, {0x700A}}};
const std::vector<RomPatch> kInfHelpers = {{0x0024D0, {0x5378}, {0x6002}}};
const std::vector<RomPatch> kInfLives = {{0x002966, {0x5378}, {0x6002}}};
const std::vector<RomPatch> kHitProtection = {{0x00ABB6, {0x70FF}, {0x7000}}};
const std::vector<RomPatch> kLongInvincible = {{0x0056CC, {0x0040}, {0x00FF}}};
const std::vector<RomPatch> kHighJump = {{0x0040B0, {0x6100}, {0x6002}}};
// Boss fights again: the map refuses a boss node whose flag is set (the flag is set once it has been beaten):
// `tst.b 1(a3,d2.w) ; beq.s $14B94` at $14B7A ($14B94 = "this node may be chosen"). Always take the branch.
const std::vector<RomPatch> kBossReplay = {{0x014B7E, {0x6714}, {0x6014}}};
const std::vector<RomPatch> kMoonJump = {
    {0x002A62, {0x0007}, {0x0005}}, {0x002A64, {0xF9E1}, {0xF9E4}}, {0x002A72, {0x5278}, {0x3D7C}},
    {0x002A74, {0xF912}, {0xFFF8}}, {0x002A76, {0x70F0}, {0x000E}},
};

std::vector<RomPatch> patch_list(const GameConfig& cfg) {
    std::vector<RomPatch> p;
    auto add = [&](const std::vector<RomPatch>& v) { p.insert(p.end(), v.begin(), v.end()); };
    if (cfg.cheat("carrots10")) add(kCarrots10);
    if (cfg.cheat("inf_helpers")) add(kInfHelpers);
    if (cfg.cheat("inf_lives")) add(kInfLives);
    if (cfg.cheat("hit_protection")) add(kHitProtection);
    if (cfg.cheat("long_invincible")) add(kLongInvincible);
    if (cfg.cheat("high_jump")) add(kHighJump);
    if (cfg.cheat("moon_jump")) add(kMoonJump);
    if (cfg.cheat("boss_replay")) add(kBossReplay);
    if (cfg.ext > 0) add(wide_patches(cfg.ext));
    return p;
}

// ---- sound test / music player ---------------------------------------------------------------------------------
// The 68000 asks the Z80 sound driver for a sound through the routine at $4A0 (d0 = the id): ids below $80 are effects
// (queued at $FFF9C0), $80 and up are music and commands (queued at $FFF9CC). Each frame the 68000 copies the queues
// into the driver's mailbox: Z80 RAM $1A00..$1A02 take effects, $1A03 takes music and commands, and the driver
// clears a byte once it has read it. The player writes the mailbox itself on a parked 68000 (as the Mickey & Donald
// player does). Commands: $FF / $FE stop the music, $E0 / $E1 fade it out (about 1.5 s / 4 s), $F0 cuts it.
// Every id below was tried headless on the real driver: music $80..$99 and the effects listed make sound; the ids
// left out (effects $31 $37 $48 $4D $63..$6F $7F, and music $9A and up) are silent. Music is named for where the game
// asks for it (levels in the order of their first section; `docs/SOUND.md` has the sections), not by ear.
constexpr int Z_SFX = 0x1A00, Z_MUSIC = 0x1A03;
const SoundEntry kSounds[] = {
    {0x81, "Logo Jingle", true, 2}, {0x82, "Title Screen", true, 6}, {0x83, "Island Map", true},
    {0x84, "Level Theme 1", true}, {0x88, "Level Theme 2", true}, {0x85, "Level Theme 3", true},
    {0x86, "Level Theme 4", true}, {0x87, "Level Theme 5", true}, {0x8A, "Level Theme 6", true},
    {0x8B, "Level Theme 7", true}, {0x8C, "Level Theme 8", true}, {0x89, "Level Theme 9", true},
    {0x94, "Level Theme 10", true},
    {0x8D, "Boss Theme 1", true}, {0x8E, "Boss Theme 2", true}, {0x8F, "Boss Theme 3", true},
    {0x80, "Crystal Invincibility", true},
    {0x92, "Goal", true, 5}, {0x93, "Goal (alternate)", true}, {0x91, "Level Complete", true, 9},
    {0x90, "Game Over", true},
    {0x95, "Special Helper Appears", true, 7}, {0x96, "Special Helper Theme", true},
    {0x97, "Password Screen", true}, {0x98, "Menu Music A", true}, {0x99, "Menu Music B", true},
    {0x01, "Jump", false}, {0x02, "Effect 02", false}, {0x03, "Effect 03", false}, {0x04, "Effect 04", false},
    {0x05, "Effect 05", false}, {0x06, "Effect 06", false}, {0x07, "Effect 07", false}, {0x08, "Effect 08", false},
    {0x09, "Effect 09", false}, {0x0A, "Effect 0A", false}, {0x0B, "Effect 0B", false}, {0x0C, "Effect 0C", false},
    {0x0D, "Effect 0D", false}, {0x0E, "Effect 0E", false}, {0x0F, "Effect 0F", false}, {0x10, "Effect 10", false},
    {0x11, "Effect 11", false}, {0x12, "Effect 12", false}, {0x13, "Effect 13", false}, {0x14, "Effect 14", false},
    {0x15, "Effect 15", false}, {0x16, "Effect 16", false}, {0x17, "Effect 17", false}, {0x18, "Effect 18", false},
    {0x19, "Effect 19", false}, {0x1A, "Effect 1A", false}, {0x1B, "Effect 1B", false}, {0x1C, "Effect 1C", false},
    {0x1D, "Effect 1D", false}, {0x1E, "Effect 1E", false}, {0x1F, "Effect 1F", false}, {0x20, "Effect 20", false},
    {0x21, "Effect 21", false}, {0x22, "Effect 22", false}, {0x23, "Effect 23", false}, {0x24, "Effect 24", false},
    {0x25, "Effect 25", false}, {0x26, "Effect 26", false}, {0x27, "Effect 27", false}, {0x28, "Effect 28", false},
    {0x29, "Effect 29", false}, {0x2A, "Effect 2A", false}, {0x2B, "Effect 2B", false}, {0x2C, "Effect 2C", false},
    {0x2D, "Effect 2D", false}, {0x2E, "Effect 2E", false}, {0x2F, "Effect 2F", false}, {0x30, "Effect 30", false},
    {0x32, "Effect 32", false}, {0x33, "Effect 33", false}, {0x34, "Effect 34", false}, {0x35, "Effect 35", false},
    {0x36, "Effect 36", false}, {0x38, "Effect 38", false}, {0x39, "Effect 39", false}, {0x3A, "Effect 3A", false},
    {0x3B, "Effect 3B", false}, {0x3C, "Effect 3C", false}, {0x3D, "Effect 3D", false}, {0x3E, "Effect 3E", false},
    {0x3F, "Effect 3F", false}, {0x40, "Effect 40", false}, {0x41, "Effect 41", false}, {0x42, "Effect 42", false},
    {0x43, "Effect 43", false}, {0x44, "Effect 44", false}, {0x45, "Effect 45", false}, {0x46, "Effect 46", false},
    {0x47, "Effect 47", false}, {0x49, "Effect 49", false}, {0x4A, "Effect 4A", false}, {0x4B, "Effect 4B", false},
    {0x4C, "Effect 4C", false}, {0x4E, "Effect 4E", false}, {0x4F, "Effect 4F", false}, {0x50, "Effect 50", false},
    {0x51, "Effect 51", false}, {0x52, "Effect 52", false}, {0x53, "Effect 53", false}, {0x54, "Effect 54", false},
    {0x55, "Effect 55", false}, {0x56, "Effect 56", false}, {0x57, "Effect 57", false}, {0x58, "Effect 58", false},
    {0x59, "Effect 59", false}, {0x5A, "Effect 5A", false}, {0x5B, "Effect 5B", false}, {0x5C, "Effect 5C", false},
    {0x5D, "Effect 5D", false}, {0x5E, "Effect 5E", false}, {0x5F, "Effect 5F", false}, {0x60, "Effect 60", false},
    {0x61, "Effect 61", false}, {0x62, "Effect 62", false}, {0x70, "Effect 70", false}, {0x71, "Effect 71", false},
    {0x72, "Effect 72", false}, {0x73, "Effect 73", false}, {0x74, "Effect 74", false}, {0x75, "Effect 75", false},
    {0x76, "Effect 76", false}, {0x77, "Effect 77", false}, {0x78, "Effect 78", false}, {0x79, "Effect 79", false},
    {0x7A, "Effect 7A", false}, {0x7B, "Effect 7B", false}, {0x7C, "Effect 7C", false}, {0x7D, "Effect 7D", false},
    {0x7E, "Effect 7E", false},
};

// ---- level select ------------------------------------------------------------------------------------------------
// The level is the section number in $FFF940: 0..39 are playable, 40 and up is the island map. Setting it and the
// state at $FFF960 to 1 (loading) starts that section; the game then sets up the hearts and plays it. The lives are
// set when the map is first reached, so the boot goes through the title to the map first. The sections are grouped by
// their music (the Level Theme numbers of `docs/SOUND.md`); the names are not the game's own (they are graphics).
struct Level { int section; const char* group; const char* name; };
const Level kLevels[] = {
    {0, "Level Theme 1", "Section 0"},
    {1, "Level Theme 1", "Section 1"},
    {2, "Level Theme 2", "Section 2"},
    {3, "Level Theme 2", "Section 3"},
    {8, "Level Theme 2", "Section 8"},
    {10, "Level Theme 2", "Section 10"},
    {24, "Level Theme 2", "Section 24"},
    {25, "Level Theme 2", "Section 25"},
    {26, "Level Theme 2", "Section 26"},
    {27, "Level Theme 2", "Section 27"},
    {5, "Level Theme 3", "Section 5"},
    {6, "Level Theme 3", "Section 6"},
    {36, "Level Theme 3", "Section 36"},
    {7, "Level Theme 4", "Section 7"},
    {9, "Level Theme 4", "Section 9"},
    {35, "Level Theme 4", "Section 35"},
    {12, "Level Theme 5", "Section 12"},
    {13, "Level Theme 5", "Section 13"},
    {14, "Level Theme 5", "Section 14"},
    {15, "Level Theme 6", "Section 15"},
    {21, "Level Theme 6", "Section 21"},
    {23, "Level Theme 6", "Section 23"},
    {37, "Level Theme 6", "Section 37"},
    {17, "Level Theme 7", "Section 17"},
    {18, "Level Theme 7", "Section 18"},
    {19, "Level Theme 7", "Section 19"},
    {22, "Level Theme 8", "Section 22"},
    {29, "Level Theme 9", "Section 29"},
    {30, "Level Theme 9", "Section 30"},
    {31, "Level Theme 9", "Section 31"},
    {38, "Level Theme 9", "Section 38 (follows section 31)"},
    {39, "Level Theme 9", "Section 39 (follows the other levels)"},
    {33, "Level Theme 10", "Section 33"},
    {4, "Boss / Scene Sections", "Section 4"},
    {11, "Boss / Scene Sections", "Section 11"},
    {16, "Boss / Scene Sections", "Section 16"},
    {20, "Boss / Scene Sections", "Section 20"},
    {28, "Boss / Scene Sections", "Section 28"},
    {32, "Boss / Scene Sections", "Section 32"},
    {34, "Boss / Scene Sections", "Section 34 (follows section 32)"},
};

class BustersHiddenTreasure : public GameModule {
public:
    const char* id() const override { return "bht"; }
    const char* title() const override { return "Buster's Hidden Treasure"; }
    const char* subtitle() const override { return "Tiny Toon Adventures"; }
    const char* year() const override { return "1993"; }

    // The verified good USA dump: 512 KB, header "TINY TOON ADVENTURES", serial GM T-95036-00, region U.
    uint32_t rom_crc32() const override { return 0xA26D3AE0; }
    uint32_t rom_size() const override { return 0x80000; }
    const char* rom_hint() const override { return "the Buster's Hidden Treasure (USA) ROM"; }
    // Whole-name match ('='): other Tiny Toon cartridges (Acme All-Stars) carry longer names.
    const char* rom_names() const override { return "=TINYTOONADVENTURES"; }
    uint32_t accent_color() const override { return 0xFF4C9BE8; }   // Buster's blue

    int max_ext() const override { return 40; }   // 16:9 = 400 px

    std::vector<CheatEntry> cheats() const override {
        return {
            {"inf_lives", "Infinite Lives", "Losing a life never reduces the lives counter (Game Genie AJYT-AA5G)."},
            {"inf_helpers", "Infinite Special Helpers", "Calling a special helper doesn't use it up, as long as you have at least one (Game Genie AJWA-AA8T)."},
            {"hit_protection", "Protection From Most Hits", "Hits no longer take hearts away (Game Genie ACZT-BA70)."},
            {"long_invincible", "Longer Invincibility After a Hit", "Invincibility lasts longer after getting hit (Game Genie 97MA-AAGN)."},
            {"carrots10", "Carrots Worth 10", "Each carrot counts as 10, so a special helper comes after every 5 carrots instead of 50 (based on Game Genie BJLA-DA52)."},
            {"high_jump", "Jump As High As You Want", "Takes the gravity pull out of the jump (Game Genie AKAA-AA7T)."},
            {"boss_replay", "Replay Boss Fights", "A boss you have beaten can be chosen again on the island map."},
            {"moon_jump", "Moon Jump", "Hold jump to keep rising (Game Genie AYZA-AADC + STZA-B4MW + 9AZA-B95Y + B2ZA-AAD0 + 6TZA-BX5E)."},
        };
    }

    // ---- Achievements: the list is the Retro Achievements set of the game (retroachievements.org/game/42) as far as it
    // can be told from RAM. The rules and how each was checked are in docs/ACHIEVEMENTS.md. Indices follow
    // achievements().
    std::vector<AchievementDef> achievements() const override {
        return {
            {"bht_carrots", "Carrotus Maximus", "Collect 50 carrots", false, 0},
            {"bht_crystal", "We Earn Our Toon Degree", "Collect a Crystal", false, 0},
            {"bht_helper", "We're All a Little Loony", "Use a special helper", false, 0},
            {"bht_hearts", "Honey Bunny", "Max out your energy meter at 5 hearts", false, 0},
            {"bht_lives", "Where's Furball?", "Reach 9 lives", false, 0},
            {"bht_dizzy", "Plains and Simple", "Free Dizzy Devil and clear the Grasslands", false, 0},
            {"bht_calamity", "Be-Wilderness", "Free Calamity Coyote and clear the Forest", false, 0},
            {"bht_plucky", "Lucky Spelunking", "Free Plucky Duck and clear the Cave", false, 0},
            {"bht_toxic", "All's Fair in Lava and War", "Free the Toxic Revenger and clear the Volcano", false, 0},
            {"bht_monty", "Endurance Match", "Defeat Montana Max and clear the Lake", false, 0},
            {"bht_hamton", "Frozen Frontier", "Free Hamton and clear the Glacier", false, 0},
            {"bht_gogo_grass", "Gogo's Grassy Getaway", "Enter the bonus round in the Grasslands", false, 0},
            {"bht_gogo_forest", "Gogo's Forested Getaway", "Enter the bonus round in the Forest", false, 0},
            {"bht_gogo_cave", "Gogo's Cavernous Getaway", "Enter the bonus round in the Cave", false, 0},
        };
    }
    enum { A_CARROTS, A_CRYSTAL, A_HELPER, A_HEARTS, A_LIVES, A_DIZZY, A_CALAMITY, A_PLUCKY, A_TOXIC, A_MONTY, A_HAMTON,
           A_GOGO_GRASS, A_GOGO_FOREST, A_GOGO_CAVE };

    void achievement_frame(const MegaDrive& md, AchievementCtx& c) const override {
        if (c.prev.size() != 0x10000) return;
        const uint8_t* pv = c.prev.data();
        auto prev16 = [&](uint32_t a) { return (uint16_t)((pv[a & 0xFFFF] << 8) | pv[(a + 1) & 0xFFFF]); };
        auto fire = [&](int i) {
            if (c.scratch[0] & (1 << i)) return;
            c.scratch[0] |= 1 << i;
            c.unlocked.push_back(i);
        };
        const int state = md.ram16(MODE), pstate = prev16(MODE);
        const bool playing = state == 2 && pstate == 2;   // a level being played (not a scene, the goal, the menus)
        if (playing) {
            // 50 carrots: the counter ($FFFB0E) counts up one by one and is cleared (with a special helper as the prize)
            // when it gets to 50
            if (prev16(0xFB0E) == 49 && md.ram16(0xFB0E) == 0) fire(A_CARROTS);
            // A special helper is called: the helper counter goes down by one
            if (md.ram16(0xFB10) + 1 == prev16(0xFB10)) fire(A_HELPER);
            // 5 hearts: the energy meter's size ($FFFB0C) is the hearts less one
            if (md.ram16(0xFB0C) >= 4) fire(A_HEARTS);
            if (md.ram16(0xFB08) >= 9) fire(A_LIVES);
        }
        // The Crystal: picking it up sets the invincibility timer ($FFF78E) to $4EC ($64F0)
        if (state != 0 && prev16(0xF78E) == 0 && md.ram16(0xF78E) > 0x400) fire(A_CRYSTAL);
        // A boss, and with it the world, is beaten: its bit in the beaten-nodes mask ($FFFBE8, bit n = node n) gets set
        // (the routine at $832C, when a level is cleared). Not by a password, which sets the mask on the menu screen.
        if (state != 0) {
            uint64_t now = 0, was = 0;
            for (int i = 0; i < 4; i++) { now |= (uint64_t)md.ram8(0xFFFBE8 + i) << (8 * i); was |= (uint64_t)pv[(0xFBE8 + i) & 0xFFFF] << (8 * i); }
            const uint64_t added = now & ~was;
            if (added && (added & (added - 1)) == 0) {   // one node
                static const struct { int node, ach; } kBoss[] = {{4, A_DIZZY}, {11, A_CALAMITY}, {16, A_PLUCKY}, {20, A_TOXIC}, {22, A_MONTY}, {28, A_HAMTON}};
                for (const auto& b : kBoss) if (added == (1ull << b.node)) fire(b.ach);
            }
        }
        // The bonus rounds (Wackyland), entered from the Grasslands, the Forest and the Cave
        const int sec = md.ram16(LEVEL);
        if ((state == 1 || state == 2) && sec != (int)prev16(LEVEL)) {
            if (sec == 36) fire(A_GOGO_GRASS);
            if (sec == 35) fire(A_GOGO_FOREST);
            if (sec == 37) fire(A_GOGO_CAVE);
        }
    }

    std::vector<LevelEntry> levels() const override {
        std::vector<LevelEntry> v;
        for (const Level& l : kLevels) v.push_back({l.group, l.name});
        return v;
    }

    bool start_level(MegaDrive& md, const GameConfig& cfg, int index, int) override {
        if (index < 0 || index >= (int)(sizeof(kLevels) / sizeof(kLevels[0]))) return false;
        const int section = kLevels[index].section;
        md.power_on();
        configure(md, cfg);
        // power-on -> logos -> title -> START -> the island map (section $28, lives set, ready for a choice). START is
        // tapped for two frames in forty from frame 400 (the title is up from about frame 800).
        bool on_map = false;
        for (int f = 1; f <= 2400 && !on_map; f++) {
            md.set_pad(0, f >= 400 && (f / 4) % 10 < 1 ? MD_START : 0);
            md.run_frame();
            on_map = md.ram16(0xF940) >= 0x28 && md.ram16(0xF962) >= 1;
        }
        md.set_pad(0, 0);
        if (!on_map) return false;
        for (int i = 0; i < 60; i++) md.run_frame();   // (the map fades in first)
        // The game fades the map out to black before it loads a level, and fades the level in by only raising the
        // colour channels of what is on screen: from the map's colours that leaves them too bright (the grass turns
        // cyan). Choosing a level on the map starts that fade-out; the jump does not, so black the palette here: the
        // current palette ($FFEF00, 64 words) and the VDP's own.
        for (int a = 0xEF00; a < 0xEF80; a++) md.set_ram8(a, 0);
        for (int i = 0; i < 64; i++) md.vdp.cram[i] = 0;
        // The map's progress is kept as bit masks (bit n = node n), each stored little-endian: $FFFBE0 = the nodes that
        // are open, $FFFBE8 = the nodes that are beaten, and copies of both that the password is made from ($FFFBF0 for
        // the open nodes below 30, $FFFBF4 for the beaten ones below 29). Beating node n sets its bit in the beaten masks
        // and opens node n+1 (the routines at $8304 and $832C). Set them as if the player had come the whole way:
        // everything before the chosen node beaten, and the chosen node open, so that beating it leaves the map with
        // the next node just opened. Sections 34 and up are not map nodes: they follow a level (34 follows 32, 38
        // follows 31), so they get the progress of that node, and the rest that of the last node.
        const int last_node = section <= 33 ? section : section == 34 ? 33 : section == 38 ? 32 : 33;
        const uint64_t open_nodes = (1ull << (last_node + 1)) - 1;
        const uint64_t beaten_nodes = (1ull << last_node) - 1;
        auto put = [&](uint32_t at, uint64_t bits, int bytes) { for (int i = 0; i < bytes; i++) md.set_ram8(at + i, (uint8_t)(bits >> (8 * i))); };
        put(0xFFFBE0, open_nodes, 5);
        put(0xFFFBE8, beaten_nodes, 5);
        put(0xFFFBF0, open_nodes & 0x3FFFFFFFull, 4);
        put(0xFFFBF4, beaten_nodes & 0x1FFFFFFFull, 4);
        md.set_ram16(0xF940, (uint16_t)section);
        md.set_ram16(0xF960, 1);
        md.set_ram16(0xF962, 0);
        if (!run_until(md, [&]() { return md.ram16(0xF940) == section && (md.ram16(0xF960) == 2 || md.ram16(0xF960) == 6); }, 1500)) return false;
        // The hearts are filled when a level is chosen on the map; the later sections (33 and up) are entered from the
        // one before and arrive with whatever is left, which here is none. Fill them, and ask for the HUD to redraw.
        if (md.ram16(0xFB0A) == 0) {
            md.set_ram16(0xFB0A, md.ram16(0xFB0C));
            md.set_ram8(0xFFF9F6, (uint8_t)(md.ram8(0xFFF9F6) | 0x04));
        }
        // Wide at once: the wide / 4:3 switch otherwise waits for a fade through black, which a jump does not have.
        md.module_state[6] = wide_view(md) ? 1 : 0;
        md.module_state[5] = 0;
        return true;
    }

    std::vector<SoundEntry> sounds() const override {
        return std::vector<SoundEntry>(std::begin(kSounds), std::end(kSounds));
    }

    void jukebox_boot(MegaDrive& md) override {
        md.power_on();
        // The driver is uploaded to the Z80 at boot ($3B8); give it a moment, then park the 68000 so the game does
        // not ask for anything, and leave the Z80 running on its own.
        for (int i = 0; i < 120; i++) md.run_frame();
        freeze_68k(md);
        stop_sound(md);
        for (int i = 0; i < 4; i++) md.run_frame();   // let the driver take the stop request
    }

    void play_sound(MegaDrive& md, int id) override {
        if (id < 0x80) md.zram[Z_SFX] = (uint8_t)id;
        else md.zram[Z_MUSIC] = (uint8_t)id;
    }

    void stop_sound(MegaDrive& md) override { md.zram[Z_MUSIC] = 0xFF; }

    bool can_configure(const MegaDrive& md, const GameConfig& cfg) const override {
        return patches_safe(md, patch_list(cfg));
    }

    void configure(MegaDrive& md, const GameConfig& cfg) override {
        apply_patches(md, patch_list(cfg));
        md.vdp.sprite_xoff = cfg.ext > 0 ? sprite_xoff : nullptr;
        md.vdp.sprite_is_hud = cfg.ext > 0 ? sprite_is_hud : nullptr;
        md.vdp.hook_host = &md;
        md.vdp.hook_flags = cfg.hud_anchor ? 1 : 0;
        md.vdp.edge_mask_l = md.vdp.edge_mask_r = 0;
        for (int i = 1; i < 4; i++) md.module_state[i] = 0;
        if (cfg.ext > 0) install_hooks(md);
        else md.clear_pc_hooks();
    }

    void pre_frame(MegaDrive& md, const GameConfig& cfg) override {
        (void)cfg;
        VDP& v = md.vdp;
        const int E = v.ext;
        update_shown(md);
        if (E == 0 || !shown_wide(md) || !wide_view(md) || on_map(md) || on_options(md)) {
            v.edge_mask_l = v.edge_mask_r = 0;       // (on the map and the options screen nothing is masked and there is no HUD)
            for (int i = 1; i < 4; i++) md.module_state[i] = 0;
            return;
        }
        if (camera_lock(md)) {
            // The locked camera keeps the wide view inside the map: nothing to black out.
            v.edge_mask_l = v.edge_mask_r = 0;
        } else {
            // A section too small to lock (a one-screen arena): what the wide view shows outside the camera's
            // range 0..MAXX is blacked out.
            // (A fixed-screen arena is one 320-px screen: all of the extension is blacked out.)
            const int cam = drawn_camera(md), hi = (int16_t)md.ram16(MAXX);
            v.edge_mask_l = std::clamp(E - cam, 0, E);
            v.edge_mask_r = std::clamp(cam + E - hi, 0, E);
            if (fixed_screen(md)) v.edge_mask_l = v.edge_mask_r = E;
        }
        show_hud(md, (int)md.module_state[7]);
    }

    void post_frame(MegaDrive& md, const GameConfig& cfg, FrameInfo& info) override {
        info.gameplay = cfg.ext > 0 && shown_wide(md);
        info.mask_left = info.mask_right = 0;   // (the edge masks above do the level ends)
    }
};

}  // namespace

GameModule* create_bht_module() { return new BustersHiddenTreasure(); }
