# How 720 Degrees works: programming tricks, secrets and bugs

Atari's 720 Degrees (1986) looks like a 3D skateboarding world. It runs on a 10 MHz CPU with no multiply instruction, 64 KB of address space and a tile-and-sprite video chip. This document collects the clever tricks its programmers used, found while taking the game apart for this port. It also covers the hidden leftovers and bugs you can find in it.

Each item was checked against the original program: the disassembly, the ROM data, and test runs of the original code. Where something is our interpretation rather than something we confirmed, the text says so ("probably", "we think").

> Reading the references: F:9B82 is an address in the fixed program ROM (CPU addresses 0x8000-0xFFFF). A5:4A2A or A7:4098 are code banks seen through window A (0x4000-0x5FFF), and B1/B3/B6/B8:xxxx are code banks seen through window B (0x6000-0x7FFF). "Table at bank 0x2C00:0x6048" means ROM data that the game pages in with bank value 0x2C00 and reads at CPU address 0x6048. RAM addresses are plain, for example 0x01B4.

## The machine in one minute

- Main CPU: a DEC T-11 at 10 MHz. The T-11 is a PDP-11 minicomputer squeezed onto one chip, but without the PDP-11's multiply, divide and multi-bit shift instructions. It sees 64 KB: RAM, video RAM, I/O, 32 KB of fixed ROM and two 8 KB windows into 384 KB of banked ROM.
- Sound and I/O CPU: a 6502 driving a YM2151 FM synthesizer, two POKEY chips and a TMS5220 speech chip. It also reads the coins, the controller and the high-score EEPROM (more on that below).
- Video (Atari System 2): a scrolling playfield of 8x8 tiles, 128 x 64 tiles (1024 x 512 pixels, wrapping around), a text layer, and hardware sprites ("motion objects") drawn from a linked list. The visible screen is 512 x 384.
- A frame is 416 scanlines of 40 microseconds: 16.64 ms, about 60.1 frames per second, or 166,400 CPU cycles.
- A "slapstic" protection chip sits between the CPU and part of the address space.

## World: it is not really 3D

The game has no 3D math at all. Every object has an x, a y and a height z, but x and y are not the isometric ground axes: they are the screen's own axes (x to the right, y upward). The screen position is just:

```
screen x = x - scroll x
screen y = y + scroll y + z        (sprite y counts upward)
```

Height is added straight to the screen y, one to one. There is no x+y or x-y anywhere. The isometric look lives entirely in the artwork and in a few lookup tables (next sections).

Even better, the units are the hardware's units. The scroll and sprite registers keep pixel positions in bits 6-15, so 64 units are one pixel. The game uses that same format for world coordinates, so a 16-bit coordinate covers exactly 1024 pixels: the width of the hardware playfield. A world position goes into a hardware register after one subtraction, with no shifting or scaling.

Where: F:B47A (screen position), board video registers.

## World: 16 directions on a squashed circle

The skater faces one of 16 directions (0 = right, 4 = up, counting counter-clockwise; adding 8 gives the opposite direction). The movement vector for each direction comes from a table, and the table is the isometric trick: all 16 vectors lie on an ellipse that is exactly twice as wide as it is tall.

| Direction | x | y |
|---|---|---|
| 0 (right) | 20724 | 0 |
| 2 (diagonal) | 14654 | 7327 |
| 4 (up) | 0 | 10362 |

So moving "up" the screen is half as fast as moving sideways, and the diagonal has the classic 2:1 pixel slope (26.57 degrees) of isometric pixel art. Velocity is speed x table value / 65536, done with a software multiply.

The in-between directions are not evenly spaced (they sit at about 25 and 70 degrees instead of 22.5 and 67.5). Direction 3 also has slightly different values (7088, 9737) from its three mirror images (+/-7292, +/-9700). It looks like a typo in a hand-made table.

Where: skater_motion_params F:9B82, tables at bank 0x2C00:0x6048 (x) and 0x6588 (y); velocity in skater_velocity F:91A6.

## Controls: a spinning joystick read by the sound CPU

The 720 cabinet has a joystick that spins all the way round. Inside are two optical discs: one with 72 teeth (144 counts per turn, 2.5 degrees per count) and a "center" disc with 2 teeth that marks straight up.

- The counter chip is wired to the sound CPU, not the main CPU. Every frame the main CPU sends sound command 0x17 and gets back four bytes: how far the stick turned and whether the center disc ticked. It adds the change to an angle from 0 to 143.
- The angle calibrates itself: the first time the center disc fires while the stick turns slowly, the angle snaps to 36 (straight up). After that, each center pulse nudges it one count toward 36, so any drift is corrected as you play.
- A 72-entry table turns the angle into one of the 16 directions. Its sectors are not equal: left and right get 25 degrees each, the diagonals 20, but straight up and down only 15.

The same dial is also used in menus: rotary_step gives one step per 16 counts (40 degrees) with hysteresis, which is how you pick letters for the high-score initials.

Where: F:8D86 (main CPU side), the 6502 code at F53B and F764, direction table at bank 0x2C00:0x6000.

## Movement: where the board points vs where you roll

The game keeps two directions: where the board points (it follows the stick at once) and where the skater is actually rolling.

- If the board points more than 90 degrees away from the rolling direction, the speed's sign flips and you roll backwards. No U-turn needed: you can switch to "fakie" at any time.
- Otherwise the rolling direction turns one notch toward the board whenever a counter overflows. The counter grows by a speed-dependent amount: one notch per frame when slow, one per 4 frames when fast. So fast skating carves wide arcs.
- A sharp 67-90 degree turn, unless you are spinning the stick fast, triggers a skid that sheds 16 speed per frame. A 16 x 16 bitmap of direction pairs decides which turns skid.

Where: A5:4CD8-4D7E, skid bitmap at bank 0x2C00:0x7988.

## Movement: kicking, top speed and friction every 16th frame

- Every change of the kick button (pressing it and letting go both count) adds kick power to a "push" value. Kick power depends on your shoes: 400, 450, 500 or 550, plus a bonus of up to 120 when you are slow. The push value halves every 16 frames.
- Every 16 frames the push target moves halfway toward that value, kept between your current speed and top speed + 128. Top speed: 900, 960, 1030 or 1100.
- While pushing, speed rises toward the target by at most 80 per frame.
- Friction is applied only once every 16 frames: 10, 8, 7 or 5 depending on your board. That gives a friction of less than one unit per frame without any fractions. Below 128 on flat ground, speed snaps to zero.
- The buttons are debounced with two XOR passes: a bit only changes once two reads in a row agree.

Where: update_push F:9236, A5:4EF4-4F50; tables at bank 0x2C00:0x7C64 (friction), 0x7C80 (kick power), 0x7C88 (top speed), 0x7CB0 (slow-speed bonus); debounce F:80A4.

## Terrain: the collision map is the screen

There is no separate collision map. The physics reads the playfield's video RAM, the same tiles you see, and looks up what each tile means.

- Once per frame, probe_ground converts the skater's position into a video RAM address and reads three tile words: one behind the feet, one under them and one ahead. Because height is added to screen y, the probe lands on the tile drawn where the feet appear on screen.
- Every tile graphic has a terrain byte: one byte per tile in an 8 KB table, for all 8192 tiles. Bits 0-2 give the type (0 flat, 1-6 slopes of increasing steepness, 7 wall), the other bits give the height level or which way the slope faces. So the terrain belongs to the tile art, not to the map. Draw a ramp tile and you get a ramp.
- A second 8 KB table gives each tile an event number (water, park entrances and so on) or a replacement terrain code (see "One tile, two places" below).
- Height levels come from a 16-entry table: -1536, -1024, -512, 0 (street level), 512, ... in steps of 512, then 6144 and 7680. 512 units is exactly one 8-pixel tile row on screen. So "one level up" looks exactly one tile row higher, and the game can measure how tall a wall is by counting tile rows.

Where: probe_ground F:934C, cell_terrain_codes F:944C, terrain table at bank 0x0800:0x4000, second table at bank 0x0C00:0x6000, heights at 0xFCB0.

## Ramps: physics without trigonometry

All slope physics is precomputed. For every surface (flat, six steepness levels in eight facings, and walls) and every one of the 16 directions, six packed tables in ROM give:

- a 3D direction vector (dx, dy, dz) for travelling that way on that surface,
- the gravity along the slope (a signed value added to speed every frame),
- two take-off factors used when leaving the surface.

The steepness levels are 8.4, 24.1, 36.7, 53.3, 72.3 and 81.6 degrees, and walls are 90: a quarter-pipe curve cut into tile-sized steps. Along the line of fall each vector has the same length (16384), so speed is preserved when the surface tilts. The tables are packed so tightly that some rows overlap the next row's data.

- On a slope, the height change per frame is speed x dz / 65536. On flat tiles the height eases toward the tile's level. Height is integrated, never looked up.
- Gravity on slopes is about -5/+6 per frame on the gentlest slope and -34/+38 on the steepest: going downhill gains about 10% more than going uphill loses. We do not know if that is deliberate.
- Above a speed limit that depends on the slope and your board, downhill gravity stops adding speed.

Where: F:9B82, tables at bank 0x2C00:0x6048, 0x6588, 0x6AC8 (vectors), 0x7668 (gravity), 0x7008 and 0x7228 (take-off), row bases at 0x79E8.

## Air: ramp launches, jumps and gravity

- Leaving a ramp launches you automatically. When the tile under you changes, the game multiplies how sharply the surface bends (the change in slope) by your speed / 4. If the result is above a threshold, you fly. A 24-degree ramp onto flat ground launches you at a speed above about 0x2C4.
- The jump button first crouches (the speed is parked), then launches upward with 170, 208, 220 or 242 depending on your shoes.
- Gravity in the air is 12 per frame (the instruction MOV #12 at F:A068), with a maximum falling speed of 512. With basic shoes a flat-ground jump lasts about 28 frames and rises about 1120 units, a bit over two height levels.
- In the air the skater is two objects: object 0 stays on the ground below him (the shadow point) and object 1 is the flying body. He lands when the body comes within 0x40 of the ground point. Landing on a very steep surface turns him to face straight downhill.

Where: launch test F:9A32, take-off F:9FB4, landing F:9AA6 and skater_touch_down F:A090.

## Spins: the 720 in the name

- In the air, if you turn the stick fast enough in one frame, an automatic spin starts. The threshold depends on your helmet: 12, 13, 16 or 19 counts.
- The spin then turns at a fixed rate (that same threshold, in half counts per frame: 15 to almost 24 degrees per frame) until the skater's total rotation catches up with how far you actually turned the stick. So a spin keeps going briefly after you stop turning, like momentum.
- If you are still spinning when you touch down, you crash. The hint "YOU'RE TRYING TO SPIN TOO FAR" is about exactly this.
- Otherwise only the final angle matters: the board must point within 45 degrees of the direction you are travelling, forwards or backwards. Landing sideways at speed while falling hard is a crash; slower, it is a skid.
- By our arithmetic, with starting equipment a flat-ground jump allows about 425 degrees of spin. A full 720 needs a ramp or better gear (about 950 degrees is possible with the best helmet and shoes).

Where: A5:4B04-4B90 (spin), landing judgement around F:A090, tables at bank 0x2C00:0x7CA0 (spin rate), 0x7C90 and 0x7C98 (sideways landing limits).

## Air: crashing and getting up

- A crash sets a get-up timer that depends on your pads: 120, 80, 50 or 40 frames. The hint "YOU'D GET UP FASTER WITH BETTER PADS" says so.
- While down you slide, losing 20 speed per frame on flat ground.
- If the push value is high enough, the get-up timer runs twice as fast. So hammering the kick button while down should get you up sooner (from reading the code; not tested on a cabinet).
- The equipment slots are RAM 0x025C board, 0x025D helmet, 0x025E pads and 0x025F shoes, each indexing its own physics tables.

Where: crash F:A2F0, recovery A7:50F8, get-up times at bank 0x2C00:0x7CAC.

## Walls: bouncing by table, measuring height by counting rows

- When a wall tile (type 7) is under or in front of you, a table of 8 wall facings x 16 directions gives the result: no hit (moving away), straight back (head-on: direction + 8), or a mirrored direction. Glancing directions are pushed one extra step away from the wall.
- To decide whether you bounce off or ride on top, trace_cells climbs the screen one tile row at a time (+512 height per row). If it reaches your height before the top of the wall, the wall is too tall and you bounce (at speed 0x160). Otherwise you end up on top.
- After a hit, the same wall is ignored for a short time, so you do not bounce twice in a row.

Where: check_wall F:979A, bounce table at bank 0x2C00:0x7908 (slope_table_value F:9E52), trace_cells F:9C5E.

## Graphics: masking with a stencil hidden in the colours

How does the skater go behind a fence or a building when there is no depth buffer? Each playfield tile has two priority bits, each sprite has two priority bits, and the video hardware combines them per pixel:

```
if (sprite priority + tile priority) has bit 1 set,
    the sprite only shows where the tile's pixel colour is below 8
```

So the upper half of each tile palette (colours 8-15) doubles as a 1-bit stencil. The artists drew the parts of buildings, fences and ramps that should cover the skater in colours 8-15. Each sprite priority is hidden by exactly two of the four tile classes:

| Sprite priority | Hidden by tiles with bits |
|---|---|
| 0 | 00 and 01 |
| 1 | 01 and 10 |
| 2 | 10 and 11 |
| 3 | 11 and 00 |

The skater normally uses priority 3, so almost nothing covers him (in Skate City only 0.1% of the pixels in the common 00 tiles use colours 8-15). When he is behind a structure (see the next section), the game switches him to priority 1, and now the flagged structure tiles cover him with their colour 8-15 pixels. Map objects use priority 0, or 2 if their placement says so.

This is how MAME and this port model the hardware's priority logic; how the real board's chips implement it was not checked.

Where: skater priority A5:50FC-515E, map objects F:C050, the mixing rule in the board video code.

## World: one tile, two places

In a 2.5D picture one screen tile can show two different places: the top of a ramp, or the floor hidden behind it. 720 solves this with the tile priority bits again. The physics reads them too, and runs a small state machine (edge_mode):

- Mode 0, open ground: no flagged tiles under the probes.
- Mode 2, front layer: a tile with bit 14 is under a probe. Here the second terrain table can replace a tile's terrain code, so the same tile becomes, say, a slope.
- Mode 1, behind layer: only bit 15 is present. The normal table is used, and bit-14 tiles act as walls or edges. This is also when the skater's sprite priority switches so that the structure masks him.
- The game only returns to mode 0 when no flagged tiles remain under the probes.

Example: the kicker ramp in Skate City next to the JUMP PARK and SLALOM PARK signs. Its tiles say "flat floor with walls" in the normal table and "slopes rising three levels" in the second table. Ride at it from the front and you go up the ramp; come from behind and you roll on the floor hidden behind it.

The parks set bit 14 on nearly every tile, so there the skater stays on the front layer and is never masked.

Where: edge_mode F:94DC, cell_terrain_codes F:944C.

## Graphics: depth sorting on the hardware's linked list

The sprite hardware draws a linked list: each sprite entry holds the index of the next one, and later entries are drawn on top. The game keeps its objects in doubly linked lists in the same order.

- Sorting by depth is lazy: one bubble-sort pass over each list every 4 frames. Swapping two objects only rewrites three link words, no matter how many sprites each object uses.
- The sort key is the screen y of the object's ground point plus a per-shape offset. For the skater that is object 0, the point on the ground, so in the air he sorts by his shadow, not by his height.
- If an object's animation frame did not change, its sprites are not rebuilt: the game adds the scroll change to each sprite's y word. It rounds the change to a multiple of 8 because bits 0-2 of that word hold other data (the picture bank).

Where: link_sprites F:AE02, sort_sprite_list F:AF32, compare F:AD70, y shift F:B344.

## Graphics: two sprite lists swapped mid-screen

The sprite hardware starts its list at sprite entry 0, so the game keeps two lists, one for the top half of the screen and one for the bottom half:

- At scanline 0 the game points entry 0 at the top list and rebuilds the bottom list.
- At scanline 192 it points entry 0 at the bottom list and rebuilds the top list.

The CPU always rebuilds the half the beam is not drawing. That is double buffering, but split by screen half instead of by frame. Each object owns two chains of sprites (one per list); an object near the middle is in both, with a small overlap.

Where: sprite_link F:B1C8, the 32V interrupt handlers.

## Graphics: sprites made of columns, motion stored in the animation

- A sprite frame is a row of columns, each 16 pixels wide and 1-8 tiles tall. Each column is one hardware sprite placed "next to the previous one". A frame is a few bytes: an x offset, a mode byte, then per column an optional y offset and a picture word.
- Animations are lists of 16-bit words: a frame, a pointer to another list, or a link that loops back.
- Objects turn by stepping around a "ring" of 16 frames or lists, one per direction, taking the short way round.
- For some object types, the 3 bytes in front of each frame hold a velocity for that animation step. Walkers and cars move as part of their animation data.

Where: build_sprite_pieces F:B1EA, animation stepping F:B378, turning F:C114.

## World: the camera follows the isometric diagonals

- The camera aims at the skater's screen position plus one frame of velocity.
- Its "dead zone" is not a rectangle. Its edges are diagonal lines (X +/- Y/2 = constant), and when you cross one, the camera moves along the 2:1 isometric diagonals at overshoot / 5 per frame.
- Outside a hard box it catches up at once.
- In Skate City the scroll speed eases by 8 per frame in x and 4 in y (2:1 again); in the parks it snaps.
- The camera also follows your height, so it moves up with you when you go high on a ramp. In the town it only starts once you are more than 64 pixels up.
- The hardware scroll registers are only loaded during vertical blank, so the screen never tears.

Where: follow_skater_scroll B6:7942.

## World: an endless town in a small playfield

The playfield is 1024 x 512 pixels and wraps around, while the screen is 512 x 384. Vertically that leaves only 128 spare pixels, so the map has to be drawn just ahead of the scroll:

- Each time the scroll crosses a 64-pixel row line, a strip of half-blocks is drawn along the edge; each time it crosses a 128-pixel column line, a column strip.
- This drawing is the main loop's idle job. Game logic runs in interrupts, and the map decoder runs in whatever time is left between them.
- Wrapping is free: the video RAM address is simply masked, and the tile column cursor is a byte that wraps by itself.

Where: draw_map_strip A5:41CE, called from the main loop at F:8714.

## Levels: squeezing the maps

- A map is a grid of pointers to blocks of 128 x 128 pixels (16 x 16 tiles). Each block is stored as two halves, each run-length coded with four modes: new low byte only, repeat, full word, and incrementing tile codes.
- Skate City is 48 x 24 blocks (6144 x 3072 pixels). Its 461 unique blocks pack into about 96 KB, against 576 KB of raw tile words. When we re-encoded the town optimally for the level editor, we only beat the original encoder by about 2%.
- Ground tiles are randomized as they are drawn: tile codes 0-15 are placeholders, replaced from a per-level table plus a variant picked from a 32-byte sequence that is re-seeded with a random number for every block. The same pavement can look different when you scroll back to it.
- Some blocks have a list of tile "stamps" stored backwards in front of the block, chosen by game state such as the park level or ticket count.
- Six layouts per park: each park has six maps, chosen by your progress, and they grow as you get better (park 2 goes from 16 x 33 to 16 x 73 blocks). Park 3 uses the same map for all six.

Where: unpack_map_block F:E82E, stamps F:EC20, layouts chosen in init_map_view A5:4000 and park_level F:90BA.

## Levels: spare bits as address bits

The town's blocks do not fit in the 64 KB that one block pointer can reach, so the game finds a 17th address bit in two different places:

- Skate City: a ROM bitmap with one bit per map cell says "take this block from the second bank". 250 of the 461 town blocks live there, mostly the four park plazas.
- Parks: blocks normally start at even addresses, so an odd pointer means "the other bank". The pointer's lowest bit is the extra address bit.

Where: town_alt_cell_bit A5:4894 (bitmap at bank 0x2C00:0x7F16), F:E82E.

## Levels: five tile sets through two

Each tile word picks one of two banks of 1024 tiles, and the bank numbers live in the low bits of the scroll registers. Skate City uses five banks: one common bank plus one of four others, one for each park plaza.

Every half-block carries a zone byte saying which tile bank it needs. As the town scrolls, the game checks the zone bytes along the incoming edge. When a different plaza comes into view, it switches the second bank, along a diagonal boundary that matches the isometric streets. That gives 5 x 1024 tiles of art on hardware that shows 2048 at a time.

Where: scan_zone_switches A5:454A, zone_switch_a-d A5:43F8-44EC, set_tile_bank F:EEE6.

## Levels: Skate City wraps with a twist

Skate down off the bottom of the town and you come back in at the top, but 10 blocks (1280 pixels) further to the right. The town is drawn so that this shifted seam is invisible: the diagonal roads continue across it. So the town is not a simple loop but a loop with a sideways shift, and the map objects follow the same shifted wrap.

Where: step_map_row A5:42BC, track_scroll_blocks B6:7B82, map_object_position F:BF5E.

## Gameplay: scoring wears tricks out

When you land, a score is summed from air time, spin (in quarter turns), height, a bonus for taking off backwards (in Skate City) and your momentum. That sum is turned into a result from 0 to 9, worth 100 to 1000 points.

Each kind of trick has a freshness counter. Repeat the same spin or the same kind of air and the game reads its table 40 or 80 entries further on, which pays less. A long jump (over 56 frames in the air) refreshes all of them. Landing in a hazard scores nothing.

Where: score_landing B3:7ADA.

## Gameplay: the breadcrumb trail that rescues you

- Every frame, if you have moved far enough, the game writes your position into an 8-entry ring buffer, but only on safe ground: never on walls or event tiles.
- Roll into a hazard (certain event tiles; we think water and the empty background of the parks) and a rescue starts. The skater lies there for 30 frames, rises, flies back through the air (at the 2:1 isometric speed ratio again) and is dropped onto the position two entries back, safely away from the hazard.

Where: record_position_trail B1:7854, start_trail_return A7:548E, trail_return_step B1:792A.

## Gameplay: Skate or Die, and why you cannot outrun the bees

When the Skate City timer runs out, a countdown starts and then the chaser appears: object type 4, the killer bees. It is the only object type never removed when it goes off screen.

- It moves in 8 directions with a dead zone. A sawtooth from the frame counter is added to its aim, so it wavers.
- It gets one speed tier faster about every 250 frames.
- The trick: while it is on screen it re-aims every frame and adds your own velocity to its pursuit speed. Its closing speed is the same however fast you skate, so you cannot outrun it while you can see it. Off screen it only re-aims every 16-31 frames, without your velocity.
- When it touches you, you crash and the game is over a second later.

Where: chaser spawn F:E036, pursuit A7:5018 and A7:4F98, speed tables at bank 0x2400:0x4138.

## Gameplay: pixel-perfect collision with 1-bit masks

Collision between objects is a two-step test. First the bounding boxes (one per animation frame) must overlap. Then shapes_touch compares 1-bit masks, 16 pixels wide, line by line.

Lining the masks up needs a shift by any number of bits, and the T-11 can only shift one bit per instruction. So the game jumps into one of 32 hand-written instruction sequences, one per shift count. For example, "shift right 8 with rounding" is ADD #0x80, ROLB, SWAB, BIC: four instructions instead of eight shifts.

Where: find_collision A7:5684, shapes_touch A7:572A, shift_rounded B8:7DB8.

## Engine: the game lives inside interrupts

The video hardware raises an interrupt every 64 scanlines (the "32V" interrupt) and one at vertical blank. Almost all of the game runs inside these interrupts:

- Scanline 0: rebuild the bottom sprite list, then the per-frame work: camera, skater state, moving every object.
- Scanline 192: rebuild the top sprite list, then the skater physics: ground probes, terrain, walls.
- Vertical blank: palette fades and animation, scroll registers, timers, map objects coming into view, input and sound.

The main loop (F:8714) is a list of seven jobs run one per pass from a jump table: the game-flow state machine, the status line, messages, spawning and so on. Between jobs it draws newly scrolled-in map strips. If vertical blank arrives first, the list starts over, so jobs that did not fit are skipped for that frame.

The interrupt handlers lower the CPU priority so they can interrupt each other, and protect themselves with "already running" flags. They also save the bank registers, so an interrupt can arrive in the middle of any banked code and still put the right ROM bank back.

The T-11 has no separate vector per interrupt line: it combines its 4 interrupt inputs into one priority code. So the game writes its vertical-blank handler into all 8 vectors whose code includes the vertical-blank line, and the 32V handler into the other 4.

Where: interrupt handlers at F:84D4 (vertical blank) and F:B66E, F:B8FC, F:B9D4 (32V), vector tables at 0xFC4E and 0xFC5E, main loop F:8714.

## Engine: compiled code with a coroutine register-save trick

According to Wikipedia, the arcade program was written in BLISS, DEC's systems programming language, rather than in assembly or C. The ROM itself does not name a language or compiler, but much of the code is clearly compiler output.

The compiled functions start with JSR R1,@#F138 (or F144, F152, F162). That helper pushes the registers the function needs (R2, R2-R3, R2-R4 or R2-R5), then runs JSR PC,@(SP)+. This classic PDP-11 coroutine call pops the function's own address and calls it, leaving the helper's restore code as the return address. When the function ends with a plain RTS, it lands in a shared chain that pops the registers and returns to the caller.

The cost is a 4-byte prologue and no frame pointer: arguments and locals are addressed relative to the stack pointer, and the result comes back in R0. You can also spot typical compiler habits: TST (SP)+ and CMP (SP)+,(SP)+ to drop arguments, CLR R0 then BISB to load an unsigned byte, and branches that jump to other branches.

116 of the 311 functions use the save helpers, about 41% of the code, and many small functions without them are compiled too. The interrupt handlers, the sprite code and the palette code are hand-written assembly.

Where: F:F138-F178.

## Engine: no multiply, no divide

The T-11 lacks the PDP-11's MUL, DIV and ASH instructions, and the program contains none. Multiplication is shift-and-add (16-bit at F:80D8, 32-bit at F:80FA) and division is shift-and-subtract (F:90A6). Many places avoid both with tables: direction vectors, slope physics, take-off factors and the score result are all precomputed.

## Engine: 416 KB of ROM in a 64 KB address space

The program and data are 416 KB: 32 KB fixed and 48 banks of 8 KB, paged into two windows (0x4000 and 0x6000) by two bank registers. Only six banks contain code; the rest is data.

There are no helper routines for bank switching. Each of the 424 places that switch banks first writes the bank value to a RAM copy, then to the register. Because the RAM copy is always written first, an interrupt that saves and restores banks can arrive at any instruction and still restore the right one.

The bank register values are scrambled on the board: the bank number is a bit-swapped and XORed version of the value written.

## Engine: the protection chip is the video memory map

Atari's "slapstic" chip guards part of the address space and changes what it shows when the CPU reads certain address sequences. On this board it selects which video RAM appears at 0x2000-0x3FFF: the text layer and sprites, or one of the two halves of the playfield. Without the chip (or a copy that knows its sequences) the game cannot draw anything.

720 uses only the simplest sequence (one arming read, then one selecting read), in just three routines. The interrupt handlers end by re-selecting the bank from a RAM copy, so code that was interrupted while drawing the map gets its video RAM back.

Where: select_video_bank0 F:806E, select_video_bank F:808C, reselect_video_bank F:809A.

## Engine: the sound CPU is also the I/O computer

The 6502 on the sound board does more than sound. The coin inputs, the spinning joystick's counters, the high-score EEPROM and a POKEY with a hardware random-number generator are all on its bus. The main CPU asks for them through the same one-byte command latch it uses for sounds.

- Each command raises an interrupt (NMI) on the 6502. Twenty "system" commands are answered right inside it: status, credits, high scores, statistics, the controller (0x17), random bytes (0x18). All other commands are queued for the sound engine.
- Replies come back byte by byte through a 16-entry queue, often framed by 0xAA and 0x55 markers. If a reply is late or a marker is wrong, the main CPU records an error code and treats the sound board as dead.
- On the main CPU side, sounds go into an 8-entry queue, and vertical blank sends at most one command per frame, only when the previous one has been taken. During the attract mode every sound request is dropped, so the demo is silent.

Where: send_sound_command, queue_sound, the 6502 NMI handler.

## Engine: random numbers from the POKEY

next_random just walks a 32-byte pool in RAM. The pool is refilled by asking the sound CPU for bytes from the POKEY's hardware random register: 2 new bytes every 32 frames, and 64 at the start of each game.

The POKEYs are otherwise silent: in two minutes of play their frequency and volume registers were written over 14,000 times each, always with 0. All music and effects come from the YM2151, and the voice from the TMS5220.

Where: next_random F:9152, command 0x18, the pool at 0x02C0.

## Engine: high scores in an EEPROM on the sound board

- The high-score table and the operator statistics live in a small EEPROM on the sound board. At power-on the 6502 copies 161 bytes of it into its RAM.
- Changes go into a 20-entry queue and are written one byte every 3 timer ticks (about 12 ms), because an EEPROM needs time to store each byte.
- The main CPU reads the high scores with command 0x11, straight into the RAM area normally used by the object tables (free during the high-score screens). It writes an entry by sending the data bytes as further commands.

Where: insert_high_score, A7:447A, A7:49B0; the 6502 EEPROM code at F208 and F568.

## Engine: palette tricks

- Fades: colours are stored as RRRRGGGGBBBBIIII, with a 4-bit intensity in the low bits. So a fade is one SUB instruction per colour: the ROM colour word minus the fade level, with no masking or clamping. The sprite and playfield palettes step on alternate frames, halving the work per frame.
- A palette "mailbox": code that wants new colours stores a routine address in RAM, and vertical blank calls it once and clears it. Colours therefore only change between frames, never mid-screen.
- Round-robin animation: one palette job per frame in an 8-frame cycle, including the equipment colours, the time-warning blink, the high-score flash and the water.
- Skate City's water ripples by colour cycling: two colours of all five playfield palettes step through a 5-entry table, one step apart, every 8 frames.

Where: fade loop F:DB30, run_palette_job F:DDEC (mailbox at 0x0EB0), animate_palette B1:7BBC (water table at 0x7A8C).

## Engine: a tiny language for screens and messages

The text screens and message boxes are written in a small bytecode. Each 16-bit word either prints a string (low byte = string number; the high byte picks the font, or a variant chosen by your rating, the park, a random choice, or alternating by game count) or prints a number (park levels, ratings, equipment) at a relative or absolute position in one of four formats. 0xFFFF ends the script.

Up to 8 message boxes can be on screen at once, each with an expiry time.

Where: run_text_script F:A9FC, message_box, queue_message.

## Engine: the attract demo is a scripted track

The demo is not a recording of someone playing. The demo skater is moved by "object scripts": four byte streams of (number of frames, value) pairs. Two channels add to x and y every frame, two step the animations of the skater's body parts, and a special code loops back. The skater physics is switched off during the demo.

The same little script system moves the scripted skaters you meet in town. One track can be replayed on any of 4 lanes, because channel 0 adds a lane offset.

Where: run_object_scripts F:DE28, start_object_scripts F:DF40.

## Engine: self-test tricks

- The power-on RAM test starts with a red screen, fills RAM with 0x0101 and shifts every word left in place 7 times, checking the walking bit on each pass. The screen turns green when it passes.
- ROM checksums are summed separately for even and odd bytes, because each 8-bit ROM chip holds one of them; the low byte of each sum must be 0.
- Then the self-test screens: character sets, a scrolling test you drive with the controller, colours, and the operator statistics.
- The ROM test screen has labels for two ROM sockets (6L and 6S) that 720 does not use. The checksum loop stops before them, so those labels are never shown.

Where: the self-test loop F:F43C, ROM checks F:F52E-F5D6.

## Secrets: the bees turn into weapons

Survive the "SKATE OR DIE!" swarm long enough and it changes shape. The bees speed up in steps (about every 250 frames). From the fourth speed level on, the swarm rearranges itself into a picture at its next change of direction:

1. a hammer
2. a set of false teeth
3. scissors
4. a syringe
5. a skull and crossbones
6. an arrow

The picture changes each time the bees are released, in that order, then repeats. In practice you have to dodge the swarm for about 12 seconds to see one. Few players ever do, because the bees are hard to escape on screen (see "Skate or Die" above). We confirmed all six shapes by running the original code.

Where: set_object7_motion F:E13A (the shape branch F:E1A2-E1CA), order table at F:FE18, speed-up in chaser_step A7:5018.

## Secrets: a hidden pause, frame-step and no-time-limit mode

The per-frame interrupt still contains a developer tool. It reads two input bits that are not connected to any control on a standard cabinet:

- Holding the first bit freezes the game.
- Adding the second bit advances it one frame at a time. Hold it for about a second and the game runs in slow motion at half speed.
- Pressing the first bit also sets a flag that nothing in the game ever clears again. With it set, the town timer running out no longer releases the bees, and their speed-up clock stops: no more "SKATE OR DIE".

On a real machine these inputs would have to be wired up first (MAME lists them as unused). We confirmed the behaviour by feeding the bits to the original code.

Where: F:B6BC-B71E, input decoding F:80A4, time-up check B3:7982.

## Secrets: messages hidden in the program ROMs

Two of the program ROM chips hold a message each, at the same spot (offset 0x8FD6):

- one says "STEALTHISCODEANDYOUDIE." (steal this code and you die)
- its partner says "JOHNSALWITZWASHEREATARI" (John Salwitz, the game's lead programmer, was here)

The two chips hold the even and odd bytes of the same 16-bit words, so in the CPU's view the messages are interleaved into gibberish. You can only read them in a dump of a single chip. The sound ROM says "COPYRIGHT (C) 1984 ATARI GAMES, INC." although the game is from 1986. Its sound driver was probably reused from an earlier game.

## Secrets: the cut cast (unused sprites)

236 of the 1,864 sprite frames are in the ROM but no animation or code ever uses them. They include:

- a whole extra pedestrian: a blond man in a white shirt, red suspenders and green trousers, with 26 walking and running frames and a frame of him knocked flat
- a bald businessman in a suit carrying a briefcase, 12 walking frames
- the digits 0-F as sprites, a hexadecimal font that probably drove a removed debug display
- 15 solid colour swatches, like a palette chart
- blue flags on stands marked 100 to 1000. We think these were visible point flags; the finished game uses invisible "hidden points" objects instead
- "CLOSED" banners
- 17 skater poses without a board
- extra frames for the convertible, the pickup, the hot rod, the mower rider, the unicyclist, the tricycle kid, the dancer and the pennant

Three unused object types (the points flag and two "CLOSED" signs) store a bare frame number where the engine expects an animation list, so they would show garbage if spawned. Types 1 and 2 hold the hex "0" digit in the same way. All of this looks like leftovers from an older data format.

The level editor in this port can show all of them: Library, Sprites, tick "unused frames".

## Secrets: smaller oddities

- The ramp park has only one map. Every other park has six layouts that grow as you progress, but all six of park 3's layout slots point to the same map.
- The text table has one hole: entry 0x79, between "SEE MORE PARKS." and "PRESS KICK BUTTON TO CONTINUE", is empty, and nothing asks for it.
- The game's credits screen, "TEAM 720" (the degree sign is drawn as a small O), is part of the attract mode and comes round every fourth cycle.
- The text includes the operator statistics screens, with histograms of game times up to "OVER 12:00", and the controller calibration prompts.

## Bugs: sub-pixel movement that never carries

Object velocities have an extra fraction byte, so positions are meant to move in 1/256-unit steps. The position update adds the fractions and tries to carry into the position:

```
ADD  R1,R0          ; add the fraction bytes (sets the carry)
SWAB R0             ; swap bytes - this CLEARS the carry flag
MOVB R0,0x07FC(R2)  ; store the new fraction
ADC  0x06F0(R3)     ; add the carry to the position: always adds 0
```

On the PDP-11, SWAB always clears the carry, so the ADC never adds anything. The fraction bytes go round and round, but they never reach the position: everything moves by the whole part of its velocity only. Nobody would notice while playing, and this port keeps the behaviour so the game moves exactly like the original.

Where: A7:40A0-40BC.

## Bugs: the roller skater who never shows up

The town object lists for stages 2, 5, 6 and 7 each include a roller skater in white (type 51). Its placement points at map cell column 14, row 1, which has no places for objects defined at all, so the game skips it. The neighbouring cells do have places, so it is probably a typo in the data. As a result this skater never appears in those stages.

Where: the list entries with descriptor 0x02E1; the check at F:CE24.

## Bugs: a bonus computed and thrown away

When you land, the scoring routine looks up a bonus based on a counter of special jump tiles you crossed in the air, then overwrites the register before adding it. It looks like a missing ADD instruction. The bonus simply never happens, so a player cannot see it, but the designers evidently planned one.

Where: score_landing B3:7C98-7CB4.

## Bugs: limits you can bump into, and what we did not find

- Cash is clamped to $0-$9,999.
- The bonus-ticket counter stops at 4.
- The town timer is only ever set to 127 or counted down by one, so it cannot overflow.
- Direction 3 of the movement table is slightly off from its mirror images (see "16 directions" above). It is only about half a degree, too small to feel.

We looked for exploits a player could trigger from the cabinet's controls (infinite money or time, counter overflows, clipping through walls at speed) and found none in the code we read. The code paths that never ran in our test sessions turned out to be rare but normal features: entering high-score initials, the "Hall of Fame" box, the sound test, the controller calibration, a corner take-off from ramp lips, and the bee shapes above.

## Reported by others, not checked here

- A "DR" graffiti tag (for Dave Ralston, the game's designer and artist) is said to be hidden in the Skate City artwork.
- Holding kick and jump at the lip of the ramp park at levels 8-10 is said to give extra air.
- Money is said to turn into points after the 16th park.

## How this was found

These notes come from rebuilding the game for this port. The original program was translated instruction by instruction into C++ and checked against an emulator frame by frame. Its routines are now being rewritten by hand and compared call by call with the original. Reading and testing every routine that way turns up how they work.

Addresses in this document point into the original ROMs, which are not included with this port.
