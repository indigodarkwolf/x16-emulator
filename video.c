// Commander X16 Emulator
// Copyright (c) 2019 Michael Steil
// Copyright (c) 2020 Frank van den Hoef
// All rights reserved. License: 2-clause BSD

#include "video.h"
#include "memory.h"
#include "ps2.h"
#include "glue.h"
#include "debugger.h"
#include "keyboard.h"
#include "gif.h"
#include "vera_spi.h"
#include "vera_psg.h"
#include "vera_pcm.h"
#include "icon.h"

#include <limits.h>

#ifdef __EMSCRIPTEN__
#include "emscripten.h"
#endif

#define ADDR_VRAM_START     0x00000
#define ADDR_VRAM_END       0x20000
#define ADDR_PSG_START      0x1F9C0
#define ADDR_PSG_END        0x1FA00
#define ADDR_PALETTE_START  0x1FA00
#define ADDR_PALETTE_END    0x1FC00
#define ADDR_SPRDATA_START  0x1FC00
#define ADDR_SPRDATA_END    0x20000

#define VIDEO_RAM_SIZE		0x20000

#define NUM_SPRITES 128

// both VGA and NTSC
#define SCAN_WIDTH 800
#define SCAN_HEIGHT 525

// VGA
#define VGA_FRONT_PORCH_X 16
#define VGA_FRONT_PORCH_Y 10
#define VGA_PIXEL_FREQ 25.175

// NTSC: 262.5 lines per frame, lower field first
#define NTSC_FRONT_PORCH_X 80
#define NTSC_FRONT_PORCH_Y 22
#define NTSC_PIXEL_FREQ (15.750 * 800 / 1000)
#define TITLE_SAFE_X 0.067
#define TITLE_SAFE_Y 0.05

// visible area we're drawing
#define SCREEN_WIDTH 640
#define SCREEN_HEIGHT 480

#define SCREEN_RAM_OFFSET 0x00000

#ifdef __APPLE__
#define LSHORTCUT_KEY SDL_SCANCODE_LGUI
#define RSHORTCUT_KEY SDL_SCANCODE_RGUI
#else
#define LSHORTCUT_KEY SDL_SCANCODE_LCTRL
#define RSHORTCUT_KEY SDL_SCANCODE_RCTRL
#endif

// When rendering a layer line, we can amortize some of the cost by calculating multiple pixels at a time.
#define LAYER_PIXELS_PER_ITERATION 8


static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *sdlTexture;
static bool is_fullscreen = false;
static float         step_advance;

static uint8_t video_ram[VIDEO_RAM_SIZE];
static uint8_t video_ram_4bpp[VIDEO_RAM_SIZE * 2];
static uint8_t video_ram_2bpp[VIDEO_RAM_SIZE * 4];
static uint8_t video_ram_1bpp[VIDEO_RAM_SIZE * 8];
static uint8_t palette[256 * 2];
static uint8_t sprite_data[128][8];

// I/O registers
static uint32_t io_addr[2];
static uint8_t io_rddata[2];
static uint8_t io_inc[2];
static uint8_t io_addrsel;
static uint8_t io_dcsel;

static uint8_t ien;
static uint8_t isr;

static uint16_t irq_line;

static uint8_t reg_layer[2][7];
static uint8_t reg_composer[8];

static uint8_t layer_line[2][SCREEN_WIDTH];
static uint8_t sprite_line_col[SCREEN_WIDTH];
static uint8_t sprite_line_z[SCREEN_WIDTH];
static uint8_t sprite_line_mask[SCREEN_WIDTH];
static uint8_t sprite_line_collisions[SCREEN_WIDTH];
static uint8_t sprite_collisions;
static uint8_t layer_line_enable[2];
static uint8_t sprite_line_enable;

struct video_layer_properties {
	uint32_t signature;
	struct video_layer_properties *next;
	struct video_layer_properties *prev;

	uint8_t  color_depth;
	uint32_t map_base;
	uint32_t tile_base;

	bool text_mode;
	bool text_mode_256c;
	bool tile_mode;
	bool bitmap_mode;

	uint16_t hscroll;
	uint16_t vscroll;
	uint8_t  palette_offset;
	uint8_t  *working_palette;

	uint8_t  mapw_log2;
	uint8_t  maph_log2;
	uint16_t tilew;
	uint16_t tileh;
	uint8_t  tilew_log2;
	uint8_t  tileh_log2;

	uint16_t mapw_max;
	uint16_t maph_max;
	uint16_t tilew_max;
	uint16_t tileh_max;
	uint16_t layerw_max;
	uint16_t layerh_max;

	uint32_t tile_size;
	uint8_t tile_size_log2;

	uint8_t bits_per_pixel;
	uint8_t first_color_pos;
	uint8_t color_mask;
	uint8_t color_fields_max;
	uint16_t bitmap_palette_offset;

	uint8_t *layer_backbuffer;
	uint8_t *tile_backbuffer;
};

static struct video_layer_properties layer_properties_pool[16];
static int                    num_layer_properties_allocd = 0;

static struct video_layer_properties *cached_layer_properties;
static struct video_layer_properties *active_layer_properties;

static struct video_layer_properties *layer_properties[2];
static uint8_t                   layer_properties_dirty[2];

static float scan_pos_x;
static uint16_t scan_pos_y;
static int      frame_count = 0;

static uint8_t framebuffer[SCREEN_WIDTH * SCREEN_HEIGHT * 4];

static GifWriter gif_writer;

static const uint16_t default_palette[] = {
0x000,0xfff,0x800,0xafe,0xc4c,0x0c5,0x00a,0xee7,0xd85,0x640,0xf77,0x333,0x777,0xaf6,0x08f,0xbbb,0x000,0x111,0x222,0x333,0x444,0x555,0x666,0x777,0x888,0x999,0xaaa,0xbbb,0xccc,0xddd,0xeee,0xfff,0x211,0x433,0x644,0x866,0xa88,0xc99,0xfbb,0x211,0x422,0x633,0x844,0xa55,0xc66,0xf77,0x200,0x411,0x611,0x822,0xa22,0xc33,0xf33,0x200,0x400,0x600,0x800,0xa00,0xc00,0xf00,0x221,0x443,0x664,0x886,0xaa8,0xcc9,0xfeb,0x211,0x432,0x653,0x874,0xa95,0xcb6,0xfd7,0x210,0x431,0x651,0x862,0xa82,0xca3,0xfc3,0x210,0x430,0x640,0x860,0xa80,0xc90,0xfb0,0x121,0x343,0x564,0x786,0x9a8,0xbc9,0xdfb,0x121,0x342,0x463,0x684,0x8a5,0x9c6,0xbf7,0x120,0x241,0x461,0x582,0x6a2,0x8c3,0x9f3,0x120,0x240,0x360,0x480,0x5a0,0x6c0,0x7f0,0x121,0x343,0x465,0x686,0x8a8,0x9ca,0xbfc,0x121,0x242,0x364,0x485,0x5a6,0x6c8,0x7f9,0x020,0x141,0x162,0x283,0x2a4,0x3c5,0x3f6,0x020,0x041,0x061,0x082,0x0a2,0x0c3,0x0f3,0x122,0x344,0x466,0x688,0x8aa,0x9cc,0xbff,0x122,0x244,0x366,0x488,0x5aa,0x6cc,0x7ff,0x022,0x144,0x166,0x288,0x2aa,0x3cc,0x3ff,0x022,0x044,0x066,0x088,0x0aa,0x0cc,0x0ff,0x112,0x334,0x456,0x668,0x88a,0x9ac,0xbcf,0x112,0x224,0x346,0x458,0x56a,0x68c,0x79f,0x002,0x114,0x126,0x238,0x24a,0x35c,0x36f,0x002,0x014,0x016,0x028,0x02a,0x03c,0x03f,0x112,0x334,0x546,0x768,0x98a,0xb9c,0xdbf,0x112,0x324,0x436,0x648,0x85a,0x96c,0xb7f,0x102,0x214,0x416,0x528,0x62a,0x83c,0x93f,0x102,0x204,0x306,0x408,0x50a,0x60c,0x70f,0x212,0x434,0x646,0x868,0xa8a,0xc9c,0xfbe,0x211,0x423,0x635,0x847,0xa59,0xc6b,0xf7d,0x201,0x413,0x615,0x826,0xa28,0xc3a,0xf3c,0x201,0x403,0x604,0x806,0xa08,0xc09,0xf0b
};

uint8_t video_space_read(uint32_t address);
static void video_space_read_range(uint8_t* dest, uint32_t address, uint32_t size);

static void prerender_layer_line_text(const uint8_t layer, const uint16_t y, uint8_t *const layer_line);
static void prerender_layer_line_tile(const uint8_t layer, const uint16_t y, uint8_t *const prerender_line);
static void prerender_sprite_line(const uint8_t sprite, const uint16_t y, uint8_t *const prerender_line, uint8_t *const cost);

static void refresh_palette();

static void
expand_1bpp_data(uint8_t *dst, const uint8_t *src, int dst_size)
{
	while (dst_size >= 8) {
		*dst = (*src) >> 7;
		++dst;
		*dst = ((*src) >> 6) & 0x1;
		++dst;
		*dst = ((*src) >> 5) & 0x1;
		++dst;
		*dst = ((*src) >> 4) & 0x1;
		++dst;
		*dst = ((*src) >> 3) & 0x1;
		++dst;
		*dst = ((*src) >> 2) & 0x1;
		++dst;
		*dst = ((*src) >> 1) & 0x1;
		++dst;
		*dst = (*src) & 0x1;
		++dst;

		++src;
		dst_size -= 8;
	}
}

static void
expand_2bpp_data(uint8_t *dst, const uint8_t *src, int dst_size)
{
	while (dst_size >= 4) {
		*dst = (*src) >> 6;
		++dst;
		*dst = ((*src) >> 4) & 0x3;
		++dst;
		*dst = ((*src) >> 2) & 0x3;
		++dst;
		*dst = (*src) & 0x3;
		++dst;

		++src;
		dst_size -= 4;
	}
}

static void
expand_4bpp_data(uint8_t *dst, const uint8_t *src, int dst_size)
{
	while (dst_size >= 2) {
		*dst = (*src) >> 4;
		++dst;
		*dst = (*src) & 0xf;
		++dst;

		++src;
		dst_size -= 2;
	}
}

void
video_reset()
{
	// init I/O registers
	memset(io_addr, 0, sizeof(io_addr));
	memset(io_inc, 0, sizeof(io_inc));
	io_addrsel = 0;
	io_dcsel = 0;
	io_rddata[0] = 0;
	io_rddata[1] = 0;

	ien = 0;
	isr = 0;
	irq_line = 0;

	// init Layer registers
	memset(reg_layer, 0, sizeof(reg_layer));

	// init composer registers
	memset(reg_composer, 0, sizeof(reg_composer));
	reg_composer[1] = 128; // hscale = 1.0
	reg_composer[2] = 128; // vscale = 1.0
	reg_composer[5] = 640 >> 2;
	reg_composer[7] = 480 >> 1;

	step_advance = (float)(VGA_PIXEL_FREQ) / (float)(MHZ);

	// init sprite data
	memset(sprite_data, 0, sizeof(sprite_data));

	// copy palette
	memcpy(palette, default_palette, sizeof(palette));
	for (int i = 0; i < 256; i++) {
		palette[i * 2 + 0] = default_palette[i] & 0xff;
		palette[i * 2 + 1] = default_palette[i] >> 8;
	}

	refresh_palette();

	// fill video RAM with random data
	for (int i = 0; i < 128 * 1024; i++) {
		video_ram[i] = rand();
	}
	expand_4bpp_data(video_ram_4bpp, video_ram, VIDEO_RAM_SIZE * 2);
	expand_2bpp_data(video_ram_2bpp, video_ram, VIDEO_RAM_SIZE * 4);
	expand_1bpp_data(video_ram_1bpp, video_ram, VIDEO_RAM_SIZE * 8);

	sprite_collisions = 0;

	scan_pos_x = 0;
	scan_pos_y = 0;

	psg_reset();
	pcm_reset();

	layer_properties[0]       = NULL;
	layer_properties[1]       = NULL;
	layer_properties_dirty[0] = true;
	layer_properties_dirty[1] = true;
}

bool
video_init(int window_scale, char *quality)
{
	video_reset();

	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, quality);
	SDL_CreateWindowAndRenderer(SCREEN_WIDTH * window_scale, SCREEN_HEIGHT * window_scale, SDL_WINDOW_ALLOW_HIGHDPI, &window, &renderer);
#ifndef __MORPHOS__
	SDL_SetWindowResizable(window, true);
#endif
	SDL_RenderSetLogicalSize(renderer, SCREEN_WIDTH, SCREEN_HEIGHT);

	sdlTexture = SDL_CreateTexture(renderer,
									SDL_PIXELFORMAT_RGB888,
									SDL_TEXTUREACCESS_STREAMING,
									SCREEN_WIDTH, SCREEN_HEIGHT);

	SDL_SetWindowTitle(window, "Commander X16");
	SDL_SetWindowIcon(window, CommanderX16Icon());

	SDL_ShowCursor(SDL_DISABLE);

	if (record_gif != RECORD_GIF_DISABLED) {
		if (!strcmp(gif_path+strlen(gif_path)-5, ",wait")) {
			// wait for POKE
			record_gif = RECORD_GIF_PAUSED;
			// move the string terminator to remove the ",wait"
			gif_path[strlen(gif_path)-5] = 0;
		} else {
			// start now
			record_gif = RECORD_GIF_ACTIVE;
		}
		if (!GifBegin(&gif_writer, gif_path, SCREEN_WIDTH, SCREEN_HEIGHT, 1, 8, false)) {
			record_gif = RECORD_GIF_DISABLED;
		}
	}

	if (debugger_enabled) {
		DEBUGInitUI(renderer);
	}

	return true;
}

void
video_end()
{
	if (debugger_enabled) {
		DEBUGFreeUI();
	}

	if (record_gif != RECORD_GIF_DISABLED) {
		GifEnd(&gif_writer);
		record_gif = RECORD_GIF_DISABLED;
	}

	SDL_DestroyRenderer(renderer);
	SDL_DestroyWindow(window);
}

static int
calc_layer_eff_y(const struct video_layer_properties *props, const int y)
{
	return (y + props->vscroll) & (props->layerh_max);
}

static uint32_t
calc_layer_map_addr_base2(const struct video_layer_properties *props, const int eff_x, const int eff_y)
{
	// Slightly faster on some platforms because we know that tilew and tileh are powers of 2.
	return props->map_base + ((((eff_y >> props->tileh_log2) << props->mapw_log2) + (eff_x >> props->tilew_log2)) << 1);
}

static uint8_t palette_8bpp[256] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
    16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
    32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47,
    48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63,
    64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79,
    80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95,
    96, 97, 98, 99, 100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111,
    112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126, 127,
    128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 138, 139, 140, 141, 142, 143,
    144, 145, 146, 147, 148, 149, 150, 151, 152, 153, 154, 155, 156, 157, 158, 159,
    160, 161, 162, 163, 164, 165, 166, 167, 168, 169, 170, 171, 172, 173, 174, 175,
    176, 177, 178, 179, 180, 181, 182, 183, 184, 185, 186, 187, 188, 189, 190, 191,
    192, 193, 194, 195, 196, 197, 198, 199, 200, 201, 202, 203, 204, 205, 206, 207,
    208, 209, 210, 211, 212, 213, 214, 215, 216, 217, 218, 219, 220, 221, 222, 223,
    224, 225, 226, 227, 228, 229, 230, 231, 232, 233, 234, 235, 236, 237, 238, 239,
    240, 241, 242, 243, 244, 245, 246, 247, 248, 249, 250, 251, 252, 253, 254, 255};

static uint8_t palette_4bpp[256] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
    0, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
    0, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47,
    0, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63,
    0, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79,
    0, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95,
    0, 97, 98, 99, 100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111,
    0, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126, 127,
    0, 129, 130, 131, 132, 133, 134, 135, 136, 137, 138, 139, 140, 141, 142, 143,
    0, 145, 146, 147, 148, 149, 150, 151, 152, 153, 154, 155, 156, 157, 158, 159,
    0, 161, 162, 163, 164, 165, 166, 167, 168, 169, 170, 171, 172, 173, 174, 175,
    0, 177, 178, 179, 180, 181, 182, 183, 184, 185, 186, 187, 188, 189, 190, 191,
    0, 193, 194, 195, 196, 197, 198, 199, 200, 201, 202, 203, 204, 205, 206, 207,
    0, 209, 210, 211, 212, 213, 214, 215, 216, 217, 218, 219, 220, 221, 222, 223,
    0, 225, 226, 227, 228, 229, 230, 231, 232, 233, 234, 235, 236, 237, 238, 239,
    0, 241, 242, 243, 244, 245, 246, 247, 248, 249, 250, 251, 252, 253, 254, 255};


// ===========================================
//
// Properties cache management
//
// ===========================================

static void
unlink_layer_properties(struct video_layer_properties **list, struct video_layer_properties *props)
{
	if (props == *list) {
		*list = props->next;
	}

	// If oldest is still us, then there was only us.
	if (props == *list) {
		*list                   = NULL;
		props->next             = NULL;
		props->prev             = NULL;
		return;
	}

	props->prev->next = props->next;
	props->next->prev = props->prev;

	props->next = NULL;
	props->prev = NULL;
}

static void
link_layer_properties(struct video_layer_properties **list, struct video_layer_properties *props)
{
	if (*list == NULL) {
		*list                   = props;
		props->next             = props;
		props->prev             = props;
		return;
	}

	props->next = *list;
	props->prev = (*list)->prev;

	(*list)->prev->next = props;
	(*list)->prev       = props;
}

static void
relink_layer_properties(struct video_layer_properties **from_list, struct video_layer_properties **to_list, struct video_layer_properties *props)
{
	unlink_layer_properties(from_list, props);
	link_layer_properties(to_list, props);
}

static struct video_layer_properties *
find_cached_layer_properties(uint32_t signature)
{
	struct video_layer_properties *props = cached_layer_properties;
	if (props == NULL) {
		return NULL;
	}
	do {
		if (props->signature == signature) {
			// Oh, sweet, we found a cached set of layer properties, let's use them.
			relink_layer_properties(&cached_layer_properties, &active_layer_properties, props);
			return props;
		}
		props = props->next;
	} while (props != cached_layer_properties);

	return NULL;
}

static struct video_layer_properties *
alloc_layer_properties()
{
	struct video_layer_properties *props = cached_layer_properties;
	if (num_layer_properties_allocd < 16) {
		props = &layer_properties_pool[num_layer_properties_allocd];

		props->next = NULL;
		props->prev = NULL;

		props->layer_backbuffer = NULL;
		props->tile_backbuffer  = NULL;

		++num_layer_properties_allocd;
		link_layer_properties(&active_layer_properties, props);
	} else if(props != NULL) {
		relink_layer_properties(&cached_layer_properties, &active_layer_properties, props);
	}
	return props;
}

static void
cache_layer_properties(struct video_layer_properties *props)
{
	relink_layer_properties(&active_layer_properties, &cached_layer_properties, props);
}

// ===========================================
//
// Layer properties
//
// ===========================================

static void
refresh_layer_properties(const uint8_t layer)
{
	struct video_layer_properties* props = layer_properties[layer];

	uint32_t signature = (uint32_t)reg_layer[layer][0] | ((uint32_t)reg_layer[layer][1] << 8) | ((uint32_t)reg_layer[layer][2] << 16);

	if (props == NULL || props->signature != signature) {
		if (props != NULL) {
			cache_layer_properties(props);
		}
		props = find_cached_layer_properties(signature);
		if (props != NULL) {
			layer_properties[layer] = props;

			if (!props->bitmap_mode) {
				props->hscroll = reg_layer[layer][3] | (reg_layer[layer][4] & 0xf) << 8;
				props->vscroll = reg_layer[layer][5] | (reg_layer[layer][6] & 0xf) << 8;
			}
		} else {
			props                   = alloc_layer_properties();
			layer_properties[layer] = props;

			props->signature = signature;

			props->color_depth    = reg_layer[layer][0] & 0x3;
			props->working_palette = (props->color_depth == 3) ? palette_8bpp : palette_4bpp;
			props->map_base       = reg_layer[layer][1] << 9;
			props->tile_base      = (reg_layer[layer][2] & 0xFC) << 9;
			props->bitmap_mode    = (reg_layer[layer][0] & 0x4) != 0;
			props->text_mode      = (props->color_depth == 0) && !props->bitmap_mode;
			props->text_mode_256c = (reg_layer[layer][0] & 8) != 0;
			props->tile_mode      = !props->bitmap_mode && !props->text_mode;

			if (!props->bitmap_mode) {
				props->hscroll = reg_layer[layer][3] | (reg_layer[layer][4] & 0xf) << 8;
				props->vscroll = reg_layer[layer][5] | (reg_layer[layer][6] & 0xf) << 8;
			} else {
				props->hscroll = 0;
				props->vscroll = 0;
			}

			uint16_t mapw = 0;
			uint16_t maph = 0;
			props->tilew  = 0;
			props->tileh  = 0;

			if (props->tile_mode || props->text_mode) {
				props->mapw_log2 = 5 + ((reg_layer[layer][0] >> 4) & 3);
				props->maph_log2 = 5 + ((reg_layer[layer][0] >> 6) & 3);
				mapw             = 1 << props->mapw_log2;
				maph             = 1 << props->maph_log2;

				// Scale the tiles or text characters according to TILEW and TILEH.
				props->tilew_log2 = 3 + (reg_layer[layer][2] & 1);
				props->tileh_log2 = 3 + ((reg_layer[layer][2] >> 1) & 1);
				props->tilew      = 1 << props->tilew_log2;
				props->tileh      = 1 << props->tileh_log2;
			} else if (props->bitmap_mode) {
				// bitmap mode is basically tiled mode with a single huge tile
				props->tilew = (reg_layer[layer][2] & 1) ? 640 : 320;
				props->tileh = SCREEN_HEIGHT;
			}

			// We know mapw, maph, tilew, and tileh are powers of two in all cases except bitmap modes, and any products of that set will be powers of two,
			// so there's no need to modulo against them if we have bitmasks we can bitwise-and against.

			props->mapw_max   = mapw - 1;
			props->maph_max   = maph - 1;
			props->tilew_max  = props->tilew - 1;
			props->tileh_max  = props->tileh - 1;
			props->layerw_max = (mapw * props->tilew) - 1;
			props->layerh_max = (maph * props->tileh) - 1;

			props->bits_per_pixel = 1 << props->color_depth;
			props->tile_size_log2 = props->tilew_log2 + props->tileh_log2 + props->color_depth - 3;
			props->tile_size      = 1 << props->tile_size_log2;

			props->first_color_pos  = 8 - props->bits_per_pixel;
			props->color_mask       = (1 << props->bits_per_pixel) - 1;
			props->color_fields_max = (8 >> props->color_depth) - 1;
		}
	}

	switch (props->color_depth) {
		case 0:
			props->tile_backbuffer = video_ram_1bpp + (props->tile_base << 3);
			break;
		case 1:
			props->tile_backbuffer = video_ram_2bpp + (props->tile_base << 2);
			break;
		case 2:
			props->tile_backbuffer = video_ram_4bpp + (props->tile_base << 1);
			break;
		case 3:
			props->tile_backbuffer = video_ram + props->tile_base;
			break;
	}

	if (props->bitmap_mode) {
		props->bitmap_palette_offset = (reg_layer[layer][4] & 0xf) << 4;

		if (props->layer_backbuffer == NULL) {
			const int     buffer_size    = props->tilew * props->tileh;

			props->layer_backbuffer = malloc(buffer_size);
			for (int i = 0; i < buffer_size; ++i) {
				const uint8_t c = props->tile_backbuffer[i];

				props->layer_backbuffer[i] = c ? (c + props->bitmap_palette_offset) : 0;
			}
		}
	} else {
		if (props->layer_backbuffer == NULL) {
			props->layer_backbuffer = malloc(1 << (props->tilew_log2 + props->tileh_log2 + props->mapw_log2 + props->maph_log2));

			const int buffer_width  = (1 << (props->mapw_log2 + props->tilew_log2));
			const int buffer_height = (1 << (props->maph_log2 + props->tileh_log2));
			if (props->text_mode) {
				for (int y = 0; y < buffer_height; ++y) {
					prerender_layer_line_text(layer, y, props->layer_backbuffer + (buffer_width * y));
				}
			} else if (props->tile_mode) {
				for (int y = 0; y < buffer_height; ++y) {
					prerender_layer_line_tile(layer, y, props->layer_backbuffer + (buffer_width * y));
				}
			}
		}
	}

	layer_properties_dirty[layer] = false;
}

static void
refresh_active_layer_scroll(int layer)
{
	struct video_layer_properties *props = layer_properties[layer];

	if (!props->bitmap_mode) {
		props->hscroll = reg_layer[layer][3] | (reg_layer[layer][4] & 0xf) << 8;
		props->vscroll = reg_layer[layer][5] | (reg_layer[layer][6] & 0xf) << 8;
	} else {
		props->hscroll = 0;
		props->vscroll = 0;
	}
}

static void
clear_layer_layer_backbuffer(int layer)
{
	struct video_layer_properties *props = &layer_properties_pool[layer];

	if (props->layer_backbuffer != NULL) {
		free(props->layer_backbuffer);
		props->layer_backbuffer = NULL;
	}

	if (props == layer_properties[0]) {
		layer_properties_dirty[0] = true;
	} else if (props == layer_properties[1]) {
		layer_properties_dirty[1] = true;
	}
}

static bool
is_layer_map_addr(int l, uint32_t addr)
{
	struct video_layer_properties *props = &layer_properties_pool[l];

	if (props->bitmap_mode) {
		return false;
	}
	if (addr < props->map_base) {
		return false;
	}
	if (addr >= props->map_base + (2 << (props->mapw_log2 + props->maph_log2))) {
		return false;
	}

	return true;
}

static void
poke_layer_map(int l, uint32_t addr, uint8_t value)
{
	struct video_layer_properties *props = &layer_properties_pool[l];

	if (props->layer_backbuffer == NULL) {
		return;
	}

	const uint16_t tile_entry     = *(uint16_t *)(video_ram + (addr & 0xfffffffe));
	const int      tile_offset    = (props->text_mode ? (tile_entry & 0xff) : (tile_entry & 0x3ff)) << (props->tilew_log2 + props->tileh_log2);
	uint8_t        flips          = (tile_entry >> 8) & 0x0C;
	const uint8_t  palette_offset = (tile_entry & 0xf000) >> 8;

	const int map_addr    = (addr - props->map_base) >> 1;
	const int map_x       = map_addr & props->mapw_max;
	const int map_y       = map_addr >> props->mapw_log2;
	const int map_offset  = (1 << (props->tilew_log2 + props->tileh_log2 + props->mapw_log2)) * map_y + (props->tilew * map_x);
	const int line_stride = ((props->mapw_max + 1) << props->tilew_log2) - props->tilew;

	uint8_t *tile_buffer      = props->tile_backbuffer + tile_offset;
	uint8_t *prerender_buffer = props->layer_backbuffer + map_offset;

	if (props->text_mode) {
		uint8_t byte1 = tile_entry >> 8;
		uint8_t fg_color, bg_color;

		if (!props->text_mode_256c) {
			fg_color = byte1 & 15;
			bg_color = byte1 >> 4;
		} else {
			fg_color = byte1;
			bg_color = 0;
		}

		for (int y = 0; y < props->tileh; ++y) {
			for (int x = 0; x < props->tilew; ++x) {
				*prerender_buffer = *tile_buffer ? fg_color : bg_color;
				++prerender_buffer;
				++tile_buffer;
			}
			prerender_buffer += line_stride;
		}
	} else if(props->tile_mode) {
		static uint8_t force_flip_type = 0x4;
		static bool    force_flip      = false;

		if (force_flip) {
			flips = force_flip_type;
		}
		switch (flips) {
			case 0x0: // No flips
				for (int y = 0; y <= props->tileh_max; ++y) {
					for (int x = 0; x <= props->tilew_max; ++x) {
						*prerender_buffer = *tile_buffer ? (*tile_buffer + palette_offset) : 0;
						++prerender_buffer;
						++tile_buffer;
					}
					prerender_buffer += line_stride;
				}
				break;
			case 0x4: // hflip
				tile_buffer += props->tilew_max;
				for (int y = 0; y <= props->tileh_max; ++y) {
					for (int x = 0; x <= props->tilew_max; ++x) {
						*prerender_buffer = *tile_buffer ? (*tile_buffer + palette_offset) : 0;
						++prerender_buffer;
						--tile_buffer;
					}
					prerender_buffer += line_stride;
					tile_buffer += (props->tilew_max << 1) + 2;
				}
				break;
			case 0x8: // vflip
				tile_buffer += (props->tileh_max << props->tilew_log2);
				for (int y = 0; y <= props->tileh_max; ++y) {
					for (int x = 0; x <= props->tilew_max; ++x) {
						*prerender_buffer = *tile_buffer ? (*tile_buffer + palette_offset) : 0;
						++prerender_buffer;
						++tile_buffer;
					}
					prerender_buffer += line_stride;
					tile_buffer -= (props->tileh_max << 1) + 2;
				}
				break;
			case 0xC: // bothflip
				tile_buffer += (props->tileh_max << props->tilew_log2) + props->tilew_max;
				for (int y = 0; y <= props->tileh_max; ++y) {
					for (int x = 0; x <= props->tilew_max; ++x) {
						*prerender_buffer = *tile_buffer ? (*tile_buffer + palette_offset) : 0;
						++prerender_buffer;
						--tile_buffer;
					}
					prerender_buffer += line_stride;
				}
				break;
		}
	}
}

static bool
is_layer_tile_addr(int l, uint32_t addr)
{
	struct video_layer_properties *props = &layer_properties_pool[l];

	if (addr < props->tile_base) {
		return false;
	}
	int tile_size = props->tilew * props->tileh * props->bits_per_pixel / 8;
	if (addr >= props->tile_base + tile_size * (props->bits_per_pixel == 1 ? 256 : 1024)) {
		return false;
	}

	return true;
}

static void
poke_layer_tile(int l, uint32_t addr, uint8_t value)
{
	struct video_layer_properties *props = &layer_properties_pool[l];

	if (props->bitmap_mode) {
		const uint32_t poked_tile_addr  = (addr - props->tile_base) << (3 - props->color_depth);
		uint8_t *      poked_tile_data  = props->tile_backbuffer + poked_tile_addr;
		const uint32_t poked_layer_addr = (addr - props->tile_base) << (3 - props->color_depth);
		uint8_t *      poked_layer_data = props->layer_backbuffer + poked_layer_addr;

		if (props->layer_backbuffer == NULL) {
			return;
		}

		uint8_t pokes = 8 >> props->color_depth;
		do {
			*poked_layer_data = (*poked_tile_data) ? (*poked_tile_data + props->bitmap_palette_offset) : 0;

			++poked_tile_data;
			++poked_layer_data;
			--pokes;
		} while (pokes > 0);
	} else {
		clear_layer_layer_backbuffer(l);

		if (props == layer_properties[0]) {
			layer_properties_dirty[0] = true;
		} else if (props == layer_properties[1]) {
			layer_properties_dirty[1] = true;
		}
	}
}

// ===========================================
//
// Sprite properties
//
// ===========================================

struct video_sprite_properties {
	uint32_t signature;

	int8_t sprite_zdepth;
	uint8_t sprite_collision_mask;

	int16_t sprite_x;
	int16_t sprite_y;
	uint8_t sprite_width_log2;
	uint8_t sprite_height_log2;
	uint8_t sprite_width;
	uint8_t sprite_height;

	bool hflip;
	bool vflip;

	uint8_t color_mode;
	uint32_t sprite_address;

	uint16_t palette_offset;
	uint8_t *working_palette;

	uint8_t *tile_backbuffer;
	uint8_t *sprite_line_cost;
	uint8_t *sprite_backbuffer;
};

struct video_sprite_properties sprite_properties[128];

static void
refresh_sprite_properties(const uint16_t sprite)
{
	struct video_sprite_properties* props = &sprite_properties[sprite];

	uint32_t signature = (sprite_data[sprite][0] << 24) | (sprite_data[sprite][1] << 16) | (sprite_data[sprite][6] << 8) | (sprite_data[sprite][7] << 8);

	if (signature != props->signature) {
		if (props->sprite_backbuffer != NULL) {
			free(props->sprite_backbuffer);
			props->sprite_backbuffer = NULL;
		}
	}

	props->sprite_zdepth = (sprite_data[sprite][6] >> 2) & 3;
	props->sprite_collision_mask = sprite_data[sprite][6] & 0xf0;

	props->sprite_x = sprite_data[sprite][2] | (sprite_data[sprite][3] & 3) << 8;
	props->sprite_y = sprite_data[sprite][4] | (sprite_data[sprite][5] & 3) << 8;
	props->sprite_width_log2  = (((sprite_data[sprite][7] >> 4) & 3) + 3);
	props->sprite_height_log2 = ((sprite_data[sprite][7] >> 6) + 3);
	props->sprite_width       = 1 << props->sprite_width_log2;
	props->sprite_height      = 1 << props->sprite_height_log2;

	// fix up negative coordinates
	if (props->sprite_x >= 0x400 - props->sprite_width) {
		props->sprite_x -= 0x400;
	}
	if (props->sprite_y >= 0x400 - props->sprite_height) {
		props->sprite_y -= 0x400;
	}

	props->hflip = sprite_data[sprite][6] & 1;
	props->vflip = (sprite_data[sprite][6] >> 1) & 1;

	props->color_mode     = (sprite_data[sprite][1] >> 7) & 1;
	props->sprite_address = sprite_data[sprite][0] << 5 | (sprite_data[sprite][1] & 0xf) << 13;

	props->palette_offset = (sprite_data[sprite][7] & 0x0f) << 4;
	props->working_palette = (props->color_mode ? palette_8bpp : palette_4bpp);

	const int sprite_size = 1 << (props->sprite_width_log2 + props->sprite_height_log2);

	switch (props->color_mode) {
		case 0:
			props->tile_backbuffer = video_ram_4bpp + (props->sprite_address << 1);
			break;
		case 1:
			props->tile_backbuffer = video_ram + props->sprite_address;
			break;
	}

	if (props->sprite_backbuffer == NULL) {
		props->sprite_backbuffer = malloc(sprite_size);
		props->sprite_line_cost  = malloc(props->sprite_height);

		for (uint16_t i = 0; i < props->sprite_height; ++i) {
			prerender_sprite_line(sprite, i, props->sprite_backbuffer + (i << props->sprite_width_log2), props->sprite_line_cost + i);
		}
	}
}

static bool
is_sprite_addr(int s, uint32_t addr)
{
	if (!sprite_line_enable) {
		return false;
	}

	struct video_sprite_properties *props = &sprite_properties[s];
	if (addr < props->sprite_address) {
		return false;
	}
	if (addr >= props->sprite_address + (2 << (props->sprite_width_log2 + props->sprite_height_log2))) {
		return false;
	}

	return true;
}

static void
poke_sprite(int sprite, uint32_t addr, uint8_t value)
{
	struct video_sprite_properties *props = &sprite_properties[sprite];

	uint16_t y = (addr - props->sprite_address) >> (props->sprite_width_log2);

	prerender_sprite_line(sprite, y, props->sprite_backbuffer + (y << props->sprite_width_log2), props->sprite_line_cost + y);
}

// ===========================================
//
// Palette
//
// ===========================================

struct video_palette {
	uint32_t entries[256];
	bool     dirty;
};

struct video_palette video_palette;

static void
refresh_palette() {
	const uint8_t out_mode = reg_composer[0] & 3;
	const bool chroma_disable = (reg_composer[0] >> 2) & 1;
	for (int i = 0; i < 256; ++i) {
		uint8_t r;
		uint8_t g;
		uint8_t b;
		if (out_mode == 0) {
			// video generation off
			// -> show blue screen
			r = 0;
			g = 0;
			b = 255;
		} else {
			uint16_t entry = palette[i * 2] | palette[i * 2 + 1] << 8;
			r = ((entry >> 8) & 0xf) << 4 | ((entry >> 8) & 0xf);
			g = ((entry >> 4) & 0xf) << 4 | ((entry >> 4) & 0xf);
			b = (entry & 0xf) << 4 | (entry & 0xf);
			if (chroma_disable) {
				r = g = b = (r + b + g) / 3;
			}
		}

		video_palette.entries[i] = (uint32_t)(r << 16) | ((uint32_t)g << 8) | ((uint32_t)b);
	}
	video_palette.dirty = false;
}

// ===========================================
//
// Rendering to backbuffers
//
// ===========================================

static void
prerender_layer_line_text(const uint8_t layer, const uint16_t y, uint8_t *const prerender_line)
{
	const struct video_layer_properties *props = layer_properties[layer];
	const int                            yy    = y & props->tileh_max;

	// additional bytes to reach the correct line of the tile
	const uint32_t y_add = (yy << props->tilew_log2);

	const int line_width = (1 << (props->tilew_log2 + props->mapw_log2));

	const uint32_t map_addr_begin = calc_layer_map_addr_base2(props, 0, y);
	const uint32_t map_addr_end   = calc_layer_map_addr_base2(props, line_width - 1, y);
	const int      size           = (map_addr_end - map_addr_begin) + 2;

	uint8_t tile_bytes[512]; // max 256 tiles, 2 bytes each.
	video_space_read_range(tile_bytes, map_addr_begin, size);

	uint32_t tile_start;
	uint8_t  palette[2]; // 0 is bg, 1 is fg
	{
		// extract all information from the map
		const uint32_t map_addr = calc_layer_map_addr_base2(props, 0, y) - map_addr_begin;

		const uint8_t tile_index = tile_bytes[map_addr];
		const uint8_t byte1      = tile_bytes[map_addr + 1];

		if (!props->text_mode_256c) {
			palette[1] = byte1 & 15;
			palette[0] = byte1 >> 4;
		} else {
			palette[1] = byte1;
			palette[0] = 0;
		}

		// offset within tilemap of the current tile
		tile_start = tile_index << (props->tile_size_log2 + 3);
	}

	// Render tile line.
	for (int x = 0; x < line_width; x++) {
		// Scrolling
		const int xx = x & props->tilew_max;

		if ((x & props->tilew_max) == 0) {
			// extract all information from the map
			const uint32_t map_addr = calc_layer_map_addr_base2(props, x, y) - map_addr_begin;

			const uint8_t tile_index = tile_bytes[map_addr];
			const uint8_t byte1      = tile_bytes[map_addr + 1];

			if (!props->text_mode_256c) {
				palette[1] = byte1 & 15;
				palette[0] = byte1 >> 4;
			} else {
				palette[1] = byte1;
				palette[0] = 0;
			}

			// offset within tilemap of the current tile
			tile_start = tile_index << (props->tile_size_log2 + 3);
		}

		// convert tile byte to indexed color
		const uint8_t col_index = props->tile_backbuffer[tile_start + xx + y_add];
		prerender_line[x]       = palette[col_index];
	}
}

static void
prerender_layer_line_tile(const uint8_t layer, const uint16_t y, uint8_t *const prerender_line)
{
	struct video_layer_properties *const props = layer_properties[layer];

	const uint8_t  yy         = y & props->tileh_max;
	const uint8_t  yy_flip    = yy ^ props->tileh_max;
	const uint32_t y_add      = (yy << props->tilew_log2);
	const uint32_t y_add_flip = (yy_flip << props->tilew_log2);

	const int line_width = (1 << (props->tilew_log2 + props->mapw_log2));

	const uint32_t map_addr_begin = calc_layer_map_addr_base2(props, 0, y);
	const uint32_t map_addr_end   = calc_layer_map_addr_base2(props, line_width - 1, y);
	const int      size           = (map_addr_end - map_addr_begin) + 2;

	uint8_t tile_bytes[512]; // max 256 tiles, 2 bytes each.
	video_space_read_range(tile_bytes, map_addr_begin, size);

	uint8_t  palette_offset;
	bool     vflip;
	bool     hflip;
	uint32_t tile_start;

	{
		// extract all information from the map
		const uint32_t map_addr = calc_layer_map_addr_base2(props, 0, y) - map_addr_begin;

		const uint8_t byte0 = tile_bytes[map_addr];
		const uint8_t byte1 = tile_bytes[map_addr + 1];

		// Tile Flipping
		vflip = (byte1 & 0x8);
		hflip = (byte1 & 0x4);

		palette_offset = byte1 & 0xf0;

		// offset within tilemap of the current tile
		const uint16_t tile_index = byte0 | ((byte1 & 3) << 8);
		tile_start                = tile_index << (props->tile_size_log2 + 3 - props->color_depth);
	}

	// Render tile line.
	for (int x = 0; x < line_width; x++) {
		if ((x & props->tilew_max) == 0) {
			// extract all information from the map
			const uint32_t map_addr = calc_layer_map_addr_base2(props, x, y) - map_addr_begin;

			const uint8_t byte0 = tile_bytes[map_addr];
			const uint8_t byte1 = tile_bytes[map_addr + 1];

			// Tile Flipping
			vflip = (byte1 & 0x8);
			hflip = (byte1 & 0x4);

			palette_offset = byte1 & 0xf0;

			// offset within tilemap of the current tile
			const uint16_t tile_index = byte0 | ((byte1 & 3) << 8);
			tile_start                = tile_index << (props->tile_size_log2 + 3 - props->color_depth);
		}

		const int xx = x & props->tilew_max;

		// convert tile byte to indexed color
		uint8_t col_index = props->tile_backbuffer[tile_start + (vflip ? y_add_flip : y_add) + (hflip ? (props->tilew_max - xx) : xx)];

		prerender_line[x] = props->working_palette[col_index];
	}
}

static void
prerender_sprite_line(const uint8_t sprite, const uint16_t y, uint8_t *const prerender_line, uint8_t *const cost)
{
	const struct video_sprite_properties *props = &sprite_properties[sprite];

	// one clock per lookup
	*cost = 1;

	const uint16_t eff_sy = props->vflip ? ((props->sprite_height - 1) - y) : y;

	int16_t       eff_sx      = (props->hflip ? (props->sprite_width - 1) : 0);
	const int16_t eff_sx_incr = props->hflip ? -1 : 1;
	const uint16_t penalty_mask = 7 >> props->color_mode;

	const uint8_t *const bitmap_data = props->tile_backbuffer + (eff_sy << props->sprite_width_log2);

	for (uint16_t sx = 0; sx < props->sprite_width; ++sx) {
		const uint16_t line_x = sx;

		// one clock per rendered pixel, plus one for each fetched 32 bits
		*cost += (sx & penalty_mask) ? 1 : 2;

		const uint8_t col_index = bitmap_data[eff_sx];
		eff_sx += eff_sx_incr;

		prerender_line[line_x] = props->working_palette[col_index];
	}
}

// ===========================================
//
// Rendering to presentation buffers
//
// ===========================================

static void
render_sprite_line(const uint16_t y, const uint16_t hsize)
{
	const uint32_t hscale     = reg_composer[1];
	const uint32_t xaccum_max = (uint32_t)(hsize - 1) * hscale;
	const uint16_t x_max      = xaccum_max >> 7;

	memset(sprite_line_col, 0, SCREEN_WIDTH);
	memset(sprite_line_z, 0, SCREEN_WIDTH);
	memset(sprite_line_mask, 0, SCREEN_WIDTH);

	uint16_t sprite_budget = 800 + 1;
	for (int i = 0; i < NUM_SPRITES; i++) {
		// one clock per lookup
		sprite_budget--; if (sprite_budget == 0) break;
		const struct video_sprite_properties *props = &sprite_properties[i];

		if (props->sprite_zdepth == 0) {
			continue;
		}

		// check whether this line falls within the sprite
		if (y < props->sprite_y || y >= props->sprite_y + props->sprite_height) {
			continue;
		}

		const uint16_t eff_sy = props->vflip ? ((props->sprite_height - 1) - (y - props->sprite_y)) : (y - props->sprite_y);

		int16_t       eff_sx      = (props->hflip ? (props->sprite_width - 1) : 0);
		const int16_t eff_sx_incr = props->hflip ? -1 : 1;

		const uint8_t *bitmap_data = video_ram + props->sprite_address + (eff_sy << (props->sprite_width_log2 - (1 - props->color_mode)));

		uint8_t unpacked_sprite_line[64];
		if (props->color_mode == 0) {
			// 4bpp
			expand_4bpp_data(unpacked_sprite_line, bitmap_data, props->sprite_width);
		} else {
			// 8bpp
			memcpy(unpacked_sprite_line, bitmap_data, props->sprite_width);
		}
		
		for (uint16_t sx = 0; sx < props->sprite_width; ++sx) {
			const uint16_t line_x = props->sprite_x + sx;
			if (line_x >= x_max) {
				eff_sx += eff_sx_incr;
				continue;
			}

			// one clock per fetched 32 bits
			if (!(sx & 3)) {
				sprite_budget--; if (sprite_budget == 0) break;
			}

			// one clock per rendered pixel
			sprite_budget--; if (sprite_budget == 0) break;

			const uint8_t col_index = unpacked_sprite_line[eff_sx];
			eff_sx += eff_sx_incr;

			// palette offset
			if (col_index > 0) {
				sprite_collisions |= sprite_line_mask[line_x] & props->sprite_collision_mask;
				sprite_line_mask[line_x] |= props->sprite_collision_mask;

		        if (props->sprite_zdepth > sprite_line_z[line_x]) {
					sprite_line_col[line_x] = col_index + props->palette_offset;
					sprite_line_z[line_x] = props->sprite_zdepth;
				}
			}
		}
	}

	uint32_t xaccum = xaccum_max;
	for (int x = hsize - 1; x >= 0; --x) {
		uint16_t eff_x     = xaccum >> 7;
		sprite_line_col[x] = sprite_line_col[eff_x];
		sprite_line_z[x]   = sprite_line_z[eff_x];
		xaccum -= hscale;
	}
}

static void
render_layer_line_bitmap(uint8_t layer, uint16_t y, const uint16_t hsize)
{
	struct video_layer_properties *props = layer_properties[layer];

	uint32_t line_addr = y * props->tilew;
	uint8_t *line_data = props->layer_backbuffer + line_addr;

	// Render tile line.
	const uint32_t hscale = reg_composer[1];
	uint32_t       xaccum = 0;
	for (int x = 0; x < hsize; x++) {
		layer_line[layer][x] = line_data[xaccum >> 7];
		xaccum += hscale;
	}
}

static void
render_layer_line_fast(const uint8_t layer, const uint16_t y, const uint16_t hsize)
{
	const struct video_layer_properties *const props  = layer_properties[layer];

	const int eff_y = calc_layer_eff_y(props, y);

	const int buffer_width = (1 << (props->mapw_log2 + props->tilew_log2));
	const int max_buffer_x = buffer_width - 1;

	const uint8_t *const prerender_line = props->layer_backbuffer + (buffer_width * eff_y);

	const uint32_t hscale = reg_composer[1];
	uint32_t       xaccum = props->hscroll << 7;
	for (uint16_t x = 0; x < hsize; ++x) {
		const uint16_t eff_x = xaccum >> 7;
		layer_line[layer][x] = prerender_line[eff_x & max_buffer_x];
		xaccum += hscale;
	}
}

static void
render_sprite_line_fast(const uint16_t y, const uint16_t hsize)
{
	const uint32_t hscale     = reg_composer[1];
	const uint32_t xaccum_max = (uint32_t)(hsize - 1) * hscale;
	const uint16_t x_max      = xaccum_max >> 7;

	memset(sprite_line_col, 0, SCREEN_WIDTH);
	memset(sprite_line_z, 0, SCREEN_WIDTH);
	memset(sprite_line_mask, 0, SCREEN_WIDTH);
	memset(sprite_line_collisions, 0, SCREEN_WIDTH);

	int16_t sprite_budget = 800 + 1;
	for (int i = 0; i < NUM_SPRITES; i++) {
		const struct video_sprite_properties *props = &sprite_properties[i];
		if (props->sprite_zdepth == 0) {
			continue;
		}

		// check whether this line falls within the sprite
		if (y < props->sprite_y || y >= props->sprite_y + props->sprite_height) {
			continue;
		}

		const uint16_t eff_sy = (y - props->sprite_y);
		const uint8_t *bitmap_data = props->sprite_backbuffer + (eff_sy << props->sprite_width_log2);

		if (props->sprite_line_cost[eff_sy] > sprite_budget) {
			for (uint16_t x = 0; x < props->sprite_width; ++x) {
				const uint16_t line_x = x + props->sprite_x;
				if (line_x > x_max) {
					continue;
				}

				const uint8_t color = bitmap_data[x];
				if (color > 0) {
					sprite_line_collisions[line_x] |= sprite_line_mask[line_x] & props->sprite_collision_mask;
					sprite_line_mask[line_x] |= props->sprite_collision_mask;

					if (props->sprite_zdepth > sprite_line_z[line_x]) {
						sprite_line_col[line_x] = color + props->palette_offset;
						sprite_line_z[line_x]   = props->sprite_zdepth;
					}
				}
			}
			sprite_budget -= props->sprite_line_cost[eff_sy];
		} else {
			// one clock per lookup
			sprite_budget--;

			for (uint16_t x = 0; x < props->sprite_width; ++x) {
				const uint16_t line_x = x + props->sprite_x;
				if (line_x > x_max) {
					continue;
				}

				// one clock per rendered pixel, plus one for each fetched 32 bits
				const uint16_t penalty_mask = 7 >> props->color_mode;
				sprite_budget -= (x & penalty_mask) ? 1 : 2;

				if (sprite_budget <= 0)
					break;

				const uint8_t color = bitmap_data[x];
				if (color > 0) {
					sprite_line_collisions[line_x] |= sprite_line_mask[line_x] & props->sprite_collision_mask;
					sprite_line_mask[line_x] |= props->sprite_collision_mask;

					if (props->sprite_zdepth > sprite_line_z[line_x]) {
						sprite_line_col[line_x] = color + props->palette_offset;
						sprite_line_z[line_x]   = props->sprite_zdepth;
					}
				}
			}
		}
	}

	uint32_t xaccum = xaccum_max;
	for (int x = hsize - 1; x >= 0; --x) {
		uint16_t eff_x     = xaccum >> 7;
		sprite_line_col[x] = sprite_line_col[eff_x];
		sprite_line_z[x]   = sprite_line_z[eff_x];
		sprite_collisions |= sprite_line_collisions[eff_x];
		xaccum -= hscale;
	}
}



static uint8_t calculate_line_col_index(uint8_t spr_zindex, uint8_t spr_col_index, uint8_t l1_col_index, uint8_t l2_col_index)
{
	uint8_t col_index = 0;
	switch (spr_zindex) {
		case 3:
			col_index = spr_col_index ? spr_col_index : (l2_col_index ? l2_col_index : l1_col_index);
			break;
		case 2:
			col_index = l2_col_index ? l2_col_index : (spr_col_index ? spr_col_index : l1_col_index);
			break;
		case 1:
			col_index = l2_col_index ? l2_col_index : (l1_col_index ? l1_col_index : spr_col_index);
			break;
		case 0:
			col_index = l2_col_index ? l2_col_index : l1_col_index;
			break;
	}
	return col_index;
}

static void
render_line(uint16_t y)
{
	const uint8_t out_mode = reg_composer[0] & 3;

	const uint8_t  border_color = reg_composer[3];
	const uint16_t hstart       = reg_composer[4] << 2;
	const uint16_t hstop_value  = reg_composer[5] << 2;
	const uint16_t hstop        = (hstart < hstop_value) ? hstop_value : SCREEN_WIDTH;
	const uint16_t hsize        = hstop - hstart;
	const uint16_t vstart       = reg_composer[6] << 1;
	const uint16_t vstop        = reg_composer[7] << 1;

	int eff_y = (reg_composer[2] * (y - vstart)) >> 7;

	if (sprite_line_enable) {
		render_sprite_line_fast(eff_y, hsize);
	}

	if (warp_mode && (frame_count & 63)) {
		// sprites were needed for the collision IRQ, but we can skip
		// everything else if we're in warp mode, most of the time
		return;
	}

	// If video output is enabled, calculate color indices for line.
	if (out_mode != 0) {
		if (video_palette.dirty) {
			refresh_palette();
		}

		uint8_t col_line[SCREEN_WIDTH];

		// Add border after if required.
		if (y < vstart || y > vstop) {
			uint32_t border_fill = border_color;
			border_fill     = border_fill | (border_fill << 8);
			border_fill     = border_fill | (border_fill << 16);
			memset(col_line, border_fill, SCREEN_WIDTH);
		} else {
			if (layer_line_enable[0]) {
				if (layer_properties_dirty[0]) {
					refresh_layer_properties(0);
				}
				if (layer_properties[0]->text_mode) {
					render_layer_line_fast(0, eff_y, hsize);
				} else if (layer_properties[0]->bitmap_mode) {
					render_layer_line_bitmap(0, eff_y, hsize);
				} else {
					render_layer_line_fast(0, eff_y, hsize);
				}
			}

			if (layer_line_enable[1]) {
				if (layer_properties_dirty[1]) {
					refresh_layer_properties(1);
				}
				if (layer_properties[1]->text_mode) {
					render_layer_line_fast(1, eff_y, hsize);
				} else if (layer_properties[1]->bitmap_mode) {
					render_layer_line_bitmap(1, eff_y, hsize);
				} else {
					render_layer_line_fast(1, eff_y, hsize);
				}
			}

			for (uint16_t x = 0; x < hstart; ++x) {
				col_line[x] = border_color;
			}
			for (uint16_t x = hstop; x < SCREEN_WIDTH; ++x) {
				col_line[x] = border_color;
			}

			uint8_t *interior = col_line + hstart;

			// Calculate color within.
			switch ((reg_composer[0] >> 4) & 0x7) {
				case 0x1:
					memcpy(interior, layer_line[0], hsize);
					break;
				case 0x2:
					memcpy(interior, layer_line[1], hsize);
					break;
				case 0x3:
					for (uint16_t x = 0; x < hsize; ++x) {
						interior[x] = calculate_line_col_index(0, 0, layer_line[0][x], layer_line[1][x]);
					}
					break;
				case 0x4:
					memcpy(interior, sprite_line_col, hsize);
					break;
				case 0x5:
					for (uint16_t x = 0; x < hsize; ++x) {
						interior[x] = calculate_line_col_index(sprite_line_z[x], sprite_line_col[x], layer_line[0][x], 0);
					}
					break;
				case 0x6:
					for (uint16_t x = 0; x < hsize; ++x) {
						interior[x] = calculate_line_col_index(sprite_line_z[x], sprite_line_col[x], 0, layer_line[1][x]);
					}
					break;
				case 0x7:
					for (uint16_t x = 0; x < hsize; ++x) {
						interior[x] = calculate_line_col_index(sprite_line_z[x], sprite_line_col[x], layer_line[0][x], layer_line[1][x]);
					}
					break;
				default:
					break;
			}
		}

		// Look up all color indices.
		uint32_t* framebuffer4_begin = ((uint32_t*)framebuffer) + (y * SCREEN_WIDTH);
		{
			uint32_t* framebuffer4 = framebuffer4_begin;
			for (uint16_t x = 0; x < SCREEN_WIDTH; x++) {
				*framebuffer4++ = video_palette.entries[col_line[x]];
			}
		}

		// NTSC overscan
		if (out_mode == 2) {
			uint32_t* framebuffer4 = framebuffer4_begin;
			for (uint16_t x = 0; x < SCREEN_WIDTH; x++)
			{
				if (x < SCREEN_WIDTH * TITLE_SAFE_X ||
					x > SCREEN_WIDTH * (1 - TITLE_SAFE_X) ||
					y < SCREEN_HEIGHT * TITLE_SAFE_Y ||
					y > SCREEN_HEIGHT * (1 - TITLE_SAFE_Y)) {

					// Divide RGB elements by 4.
					*framebuffer4 &= 0x00fcfcfc;
					*framebuffer4 >>= 2;
				}
				framebuffer4++;
			}
		}
	}
}

bool
video_step()
{
	uint8_t out_mode = reg_composer[0] & 3;

	bool new_frame = false;
	scan_pos_x += step_advance;
	if (scan_pos_x > SCAN_WIDTH) {
		scan_pos_x -= SCAN_WIDTH;
		uint16_t front_porch = (out_mode & 2) ? NTSC_FRONT_PORCH_Y : VGA_FRONT_PORCH_Y;
		uint16_t y = scan_pos_y - front_porch;
		if (y < SCREEN_HEIGHT) {
			render_line(y);
		}
		scan_pos_y++;
		if (scan_pos_y == SCREEN_HEIGHT) {
			if (ien & 4) {
				if (sprite_collisions != 0) {
					isr |= 4;
				}
				isr = (isr & 0xf) | sprite_collisions;
			}
			sprite_collisions = 0;
		}
		if (scan_pos_y == SCAN_HEIGHT) {
			scan_pos_y = 0;
			new_frame = true;
			frame_count++;
			if (ien & 1) { // VSYNC IRQ
				isr |= 1;
			}
		}
		if (ien & 2) { // LINE IRQ
			y = scan_pos_y - front_porch;
			if (y < SCREEN_HEIGHT && y == irq_line) {
				isr |= 2;
			}
		}
	}

	return new_frame;
}

bool
video_update()
{
	static bool cmd_down = false;

	SDL_UpdateTexture(sdlTexture, NULL, framebuffer, SCREEN_WIDTH * 4);

	if (record_gif > RECORD_GIF_PAUSED) {
		if(!GifWriteFrame(&gif_writer, framebuffer, SCREEN_WIDTH, SCREEN_HEIGHT, 2, 8, false)) {
			// if that failed, stop recording
			GifEnd(&gif_writer);
			record_gif = RECORD_GIF_DISABLED;
			printf("Unexpected end of recording.\n");
		}
		if (record_gif == RECORD_GIF_SINGLE) { // if single-shot stop recording
			record_gif = RECORD_GIF_PAUSED;  // need to close in video_end()
		}
	}

	SDL_RenderClear(renderer);
	SDL_RenderCopy(renderer, sdlTexture, NULL, NULL);

	if (debugger_enabled && showDebugOnRender != 0) {
		DEBUGRenderDisplay(SCREEN_WIDTH, SCREEN_HEIGHT);
		SDL_RenderPresent(renderer);
		return true;
	}

	SDL_RenderPresent(renderer);

	SDL_Event event;
	while (SDL_PollEvent(&event)) {
		if (event.type == SDL_QUIT) {
			return false;
		}
		if (event.type == SDL_KEYDOWN) {
			bool consumed = false;
			if (cmd_down) {
				if (event.key.keysym.sym == SDLK_s) {
					machine_dump();
					consumed = true;
				} else if (event.key.keysym.sym == SDLK_r) {
					machine_reset();
					consumed = true;
				} else if (event.key.keysym.sym == SDLK_v) {
					machine_paste(SDL_GetClipboardText());
					consumed = true;
				} else if (event.key.keysym.sym == SDLK_f || event.key.keysym.sym == SDLK_RETURN) {
					is_fullscreen = !is_fullscreen;
					SDL_SetWindowFullscreen(window, is_fullscreen ? SDL_WINDOW_FULLSCREEN : 0);
					consumed = true;
				} else if (event.key.keysym.sym == SDLK_PLUS || event.key.keysym.sym == SDLK_EQUALS) {
					machine_toggle_warp();
					consumed = true;
				}
			}
			if (!consumed) {
				if (event.key.keysym.scancode == LSHORTCUT_KEY || event.key.keysym.scancode == RSHORTCUT_KEY) {
					cmd_down = true;
				}
				handle_keyboard(true, event.key.keysym.sym, event.key.keysym.scancode);
			}
			return true;
		}
		if (event.type == SDL_KEYUP) {
			if (event.key.keysym.scancode == LSHORTCUT_KEY || event.key.keysym.scancode == RSHORTCUT_KEY) {
				cmd_down = false;
			}
			handle_keyboard(false, event.key.keysym.sym, event.key.keysym.scancode);
			return true;
		}
		if (event.type == SDL_MOUSEBUTTONDOWN) {
			switch (event.button.button) {
				case SDL_BUTTON_LEFT:
					mouse_button_down(0);
					break;
				case SDL_BUTTON_RIGHT:
					mouse_button_down(1);
					break;
			}
		}
		if (event.type == SDL_MOUSEBUTTONUP) {
			switch (event.button.button) {
				case SDL_BUTTON_LEFT:
					mouse_button_up(0);
					break;
				case SDL_BUTTON_RIGHT:
					mouse_button_up(1);
					break;
			}
		}
		if (event.type == SDL_MOUSEMOTION) {
			static int mouse_x;
			static int mouse_y;
			mouse_move(event.motion.x - mouse_x, event.motion.y - mouse_y);
			mouse_x = event.motion.x;
			mouse_y = event.motion.y;
		}
	}
	return true;
}

// ===========================================
//
// I/O
//
// ===========================================

bool
video_get_irq_out()
{
	uint8_t tmp_isr = isr | (pcm_is_fifo_almost_empty() ? 8 : 0);
	return (tmp_isr & ien) != 0;
}

//
// saves the video memory and register content into a file
//

void
video_save(SDL_RWops *f)
{
	SDL_RWwrite(f, &video_ram[0], sizeof(uint8_t), sizeof(video_ram));
	SDL_RWwrite(f, &reg_composer[0], sizeof(uint8_t), sizeof(reg_composer));
	SDL_RWwrite(f, &palette[0], sizeof(uint8_t), sizeof(palette));
	SDL_RWwrite(f, &reg_layer[0][0], sizeof(uint8_t), sizeof(reg_layer));
	SDL_RWwrite(f, &sprite_data[0], sizeof(uint8_t), sizeof(sprite_data));
}

static const int increments[32] = {
	0,   0,
	1,   -1,
	2,   -2,
	4,   -4,
	8,   -8,
	16,  -16,
	32,  -32,
	64,  -64,
	128, -128,
	256, -256,
	512, -512,
	40,  -40,
	80,  -80,
	160, -160,
	320, -320,
	640, -640,
};

uint32_t
get_and_inc_address(uint8_t sel)
{
	uint32_t address = io_addr[sel];
	io_addr[sel] += increments[io_inc[sel]];
	return address;
}

//
// Vera: Internal Video Address Space
//

uint8_t
video_space_read(uint32_t address)
{
	return video_ram[address & 0x1FFFF];
}

static void
video_space_read_range(uint8_t* dest, uint32_t address, uint32_t size)
{
	if (address >= ADDR_VRAM_START && (address+size) <= ADDR_VRAM_END) {
		memcpy(dest, &video_ram[address], size);
	} else {
		for(int i = 0; i < size; ++i) {
			*dest++ = video_space_read(address + i);
		}
	}
}

void
video_space_write(uint32_t address, uint8_t value)
{
	const uint32_t wrap_address = address & 0x1FFFF;
	video_ram[wrap_address]     = value;
	expand_4bpp_data(video_ram_4bpp + (wrap_address << 1), video_ram + wrap_address, 2);
	expand_2bpp_data(video_ram_2bpp + (wrap_address << 2), video_ram + wrap_address, 4);
	expand_1bpp_data(video_ram_1bpp + (wrap_address << 3), video_ram + wrap_address, 8);

	if (address >= ADDR_PSG_START && address < ADDR_PSG_END) {
		psg_writereg(address & 0x3f, value);
	} else if (address >= ADDR_PALETTE_START && address < ADDR_PALETTE_END) {
		palette[address & 0x1ff] = value;
		video_palette.dirty = true;
	} else if (address >= ADDR_SPRDATA_START && address < ADDR_SPRDATA_END) {
		sprite_data[(address >> 3) & 0x7f][address & 0x7] = value;
		refresh_sprite_properties((address >> 3) & 0x7f);
	}

	if (address >= ADDR_PSG_START) {
		return;
	}

	for (int i = 0; i < num_layer_properties_allocd; ++i) {
		if (is_layer_map_addr(i, address)) {
			poke_layer_map(i, address, value);
		}

		if (is_layer_tile_addr(i, address)) {
			poke_layer_tile(i, address, value);
		}
	}
	for (int i = 0; i < 128; ++i) {
		if (is_sprite_addr(i, address)) {
			poke_sprite(i, address, value);
		}
	}
}

//
// Vera: 6502 I/O Interface
//
// if debugOn, read without any side effects (registers & memory unchanged)

uint8_t video_read(uint8_t reg, bool debugOn) {
	switch (reg & 0x1F) {
		case 0x00: return io_addr[io_addrsel] & 0xff;
		case 0x01: return (io_addr[io_addrsel] >> 8) & 0xff;
		case 0x02: return (io_addr[io_addrsel] >> 16) | (io_inc[io_addrsel] << 3);
		case 0x03:
		case 0x04: {
			if (debugOn) {
				return io_rddata[reg - 3];
			}

			uint32_t address = get_and_inc_address(reg - 3);

			uint8_t value = io_rddata[reg - 3];
			io_rddata[reg - 3] = video_space_read(io_addr[reg - 3]);

			if (log_video) {
				printf("READ  video_space[$%X] = $%02X\n", address, value);
			}
			return value;
		}
		case 0x05: return (io_dcsel << 1) | io_addrsel;
		case 0x06: return ((irq_line & 0x100) >> 1) | (ien & 0xF);
		case 0x07: return isr | (pcm_is_fifo_almost_empty() ? 8 : 0);
		case 0x08: return irq_line & 0xFF;

		case 0x09:
		case 0x0A:
		case 0x0B:
		case 0x0C: return reg_composer[reg - 0x09 + (io_dcsel ? 4 : 0)];

		case 0x0D:
		case 0x0E:
		case 0x0F:
		case 0x10:
		case 0x11:
		case 0x12:
		case 0x13: return reg_layer[0][reg - 0x0D];

		case 0x14:
		case 0x15:
		case 0x16:
		case 0x17:
		case 0x18:
		case 0x19:
		case 0x1A: return reg_layer[1][reg - 0x14];

		case 0x1B: return pcm_read_ctrl();
		case 0x1C: return pcm_read_rate();
		case 0x1D: return 0;

		case 0x1E:
		case 0x1F: return vera_spi_read(reg & 1);
	}
	return 0;
}

void video_write(uint8_t reg, uint8_t value) {
	// if (reg > 4) {
	// 	printf("ioregisters[0x%02X] = 0x%02X\n", reg, value);
	// }
	//	printf("ioregisters[%d] = $%02X\n", reg, value);
	switch (reg & 0x1F) {
		case 0x00:
			io_addr[io_addrsel] = (io_addr[io_addrsel] & 0x1ff00) | value;
			io_rddata[io_addrsel] = video_space_read(io_addr[io_addrsel]);
			break;
		case 0x01:
			io_addr[io_addrsel] = (io_addr[io_addrsel] & 0x100ff) | (value << 8);
			io_rddata[io_addrsel] = video_space_read(io_addr[io_addrsel]);
			break;
		case 0x02:
			io_addr[io_addrsel] = (io_addr[io_addrsel] & 0x0ffff) | ((value & 0x1) << 16);
			io_inc[io_addrsel]  = value >> 3;
			io_rddata[io_addrsel] = video_space_read(io_addr[io_addrsel]);
			break;
		case 0x03:
		case 0x04: {
			uint32_t address = get_and_inc_address(reg - 3);
			if (log_video) {
				printf("WRITE video_space[$%X] = $%02X\n", address, value);
			}
			video_space_write(address, value);

			io_rddata[reg - 3] = video_space_read(io_addr[reg - 3]);
			break;
		}
		case 0x05:
			if (value & 0x80) {
				video_reset();
			}
			io_dcsel = (value >> 1) & 1;
			io_addrsel = value & 1;
			break;
		case 0x06:
			irq_line = (irq_line & 0xFF) | ((value >> 7) << 8);
			ien = value & 0xF;
			break;
		case 0x07:
			isr &= value ^ 0xff;
			break;
		case 0x08:
			irq_line = (irq_line & 0x100) | value;
			break;

		case 0x09:
		case 0x0A:
		case 0x0B:
		case 0x0C: {
			int i = reg - 0x09 + (io_dcsel ? 4 : 0);
			reg_composer[i] = value;
			if (i == 0) {
				step_advance = (value & 2) ? ((float)(NTSC_PIXEL_FREQ) / (float)(MHZ)) : ((float)(VGA_PIXEL_FREQ) / (float)(MHZ));

				layer_line_enable[0] = value & 0x10;
				layer_line_enable[1] = value & 0x20;
				sprite_line_enable   = value & 0x40;

				if (layer_line_enable[0]) {
					layer_properties_dirty[0] = true;
				}
				if (layer_line_enable[1]) {
					layer_properties_dirty[1] = true;
				}
				video_palette.dirty = true;
			}
			break;
		}

		case 0x0D:
		case 0x0E:
		case 0x0F:
			if (reg_layer[0][reg - 0x0D] != value) {
				layer_properties_dirty[0] = true;
			}
		case 0x10:
		case 0x11:
		case 0x12:
		case 0x13:
			reg_layer[0][reg - 0x0D] = value;
			if (!layer_properties_dirty[0]) {
				if ((reg - 0x0D) == 4 && layer_properties[0]->bitmap_mode) {
					clear_layer_layer_backbuffer(0);
				}
				refresh_active_layer_scroll(0);
			}
			break;

		case 0x14:
		case 0x15:
		case 0x16:
			if (reg_layer[1][reg - 0x14] != value) {
				if ((reg - 0x0D) == 4 && layer_properties[0]->bitmap_mode) {
					clear_layer_layer_backbuffer(1);
				}
				layer_properties_dirty[1] = true;
			}
		case 0x17:
		case 0x18:
		case 0x19:
		case 0x1A:
			reg_layer[1][reg - 0x14] = value;
			if (!layer_properties_dirty[1]) {
				refresh_active_layer_scroll(1);
			}
			break;

		case 0x1B: pcm_write_ctrl(value); break;
		case 0x1C: pcm_write_rate(value); break;
		case 0x1D: pcm_write_fifo(value); break;

		case 0x1E:
		case 0x1F:
			vera_spi_write(reg & 1, value);
			break;
	}
}

void
video_update_title(const char* window_title)
{
	SDL_SetWindowTitle(window, window_title);
}
