// Commander X16 Emulator
// Copyright (c) 2019 Michael Steil
// All rights reserved. License: 2-clause BSD

#include <sys/types.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include "glue.h"
#include "via.h"
#include "memory.h"
#include "video.h"
#include "ym2151.h"
#include "ps2.h"
#include "cpu/fake6502.h"

uint8_t ram_bank;
uint8_t rom_bank;

uint8_t *RAM;
uint8_t ROM[ROM_SIZE];

bool led_status;

#define DEVICE_EMULATOR (0x9fb0)

#define MEMMAP_NULL (0)
#define MEMMAP_DIRECT (1)
#define MEMMAP_IO (2)
#define MEMMAP_IO_SOUND (2)
#define MEMMAP_IO_VIDEO (3)
#define MEMMAP_IO_LCD (4)
#define MEMMAP_IO_VIA1 (5)
#define MEMMAP_IO_VIA2 (6)
#define MEMMAP_IO_RTC (7)
#define MEMMAP_IO_MOUSE (8)
#define MEMMAP_IO_EMU (9)
#define MEMMAP_RAMBANK (10)
#define MEMMAP_ROMBANK (11)

struct memmap_table_entry
{
	uint8_t entry_start;
	uint8_t final_entry;
	uint8_t memory_type;
};

// High byte mapping of memory
struct memmap_table_entry memmap_table_hi[] = {
    {0x00, 0x9f - 1, MEMMAP_DIRECT},
    {0x9f, 0xa0 - 1, MEMMAP_IO},
    {0xa0, 0xc0 - 1, MEMMAP_RAMBANK},
    {0xc0, 0xff, MEMMAP_ROMBANK},
};

struct memmap_table_entry memmap_table_io[] = {
    {0x00, 0x20 - 1, MEMMAP_IO_SOUND},
    {0x20, 0x40 - 1, MEMMAP_IO_VIDEO},
    {0x40, 0x60 - 1, MEMMAP_IO_LCD},
    {0x60, 0x70 - 1, MEMMAP_IO_VIA1},
    {0x70, 0x80 - 1, MEMMAP_IO_VIA2},
    {0x80, 0xa0 - 1, MEMMAP_IO_RTC},
    {0xa0, 0xb0 - 1, MEMMAP_IO_MOUSE},
    {0xb0, 0xc0 - 1, MEMMAP_IO_EMU},
    {0xc0, 0xe0 - 1, MEMMAP_NULL},
    {0xe0, 0xff, MEMMAP_IO_SOUND},
};

uint8_t memory_map_hi[0x100];
uint8_t memory_map_io[0x100];

static void
build_memory_map(struct memmap_table_entry *table_entries, uint8_t *map)
{
	int e = 0;
	while (table_entries[e].final_entry < 0xff) {
		for (int i = table_entries[e].entry_start; i <= table_entries[e].final_entry; ++i) {
			map[i] = table_entries[e].memory_type;
		}
		++e;
	}
	for (int i = table_entries[e].entry_start; i <= table_entries[e].final_entry; ++i) {
		map[i] = table_entries[e].memory_type;
	}
}

void
memory_init()
{
	RAM = calloc(RAM_SIZE, sizeof(uint8_t));

	build_memory_map(memmap_table_hi, memory_map_hi);
	build_memory_map(memmap_table_io, memory_map_io);
}

static uint8_t
effective_ram_bank()
{
	return ram_bank % num_ram_banks;
}

static uint8_t
debug_ram_read(uint16_t address, uint8_t bank)
{
	const int ramBank = bank % num_ram_banks;
	return RAM[0xa000 + (ramBank << 13) + address - 0xa000];
}

static uint8_t
real_ram_read(uint16_t address)
{
	const int ramBank = effective_ram_bank();
	return RAM[0xa000 + (ramBank << 13) + address - 0xa000];
}

static uint8_t
debug_rom_read(uint16_t address, uint8_t bank)
{
	const int romBank = bank % NUM_ROM_BANKS;
	return ROM[(romBank << 14) + address - 0xc000];
}

static uint8_t
real_rom_read(uint16_t address)
{
	return ROM[(rom_bank << 14) + address - 0xc000];
}

static uint8_t
debug_io_read(uint16_t address)
{
	switch (memory_map_io[address & 0xff]) {
		case MEMMAP_IO_SOUND: return 0; // TODO: Sound
		case MEMMAP_IO_VIDEO: return debug_video_read(address & 0x1f);
		case MEMMAP_IO_LCD: return 0; // TODO: Character LCD
		case MEMMAP_IO_VIA1: return via1_read(address & 0xf);
		case MEMMAP_IO_VIA2: return via2_read(address & 0xf);
		case MEMMAP_IO_RTC: return 0; // TODO: RTC
		case MEMMAP_IO_MOUSE: return mouse_read(address & 0x1f);
		case MEMMAP_IO_EMU: return debug_emu_read(address & 0xf);
		case MEMMAP_NULL: return 0;
		default: return 0;
	}
	return 0;
}

static uint8_t
real_io_read(uint16_t address)
{
	switch (memory_map_io[address & 0xff]) {
		case MEMMAP_IO_SOUND: return 0; // TODO: Sound
		case MEMMAP_IO_VIDEO: return video_read(address & 0x1f);
		case MEMMAP_IO_LCD: return 0; // TODO: Character LCD
		case MEMMAP_IO_VIA1: return via1_read(address & 0xf);
		case MEMMAP_IO_VIA2: return via2_read(address & 0xf);
		case MEMMAP_IO_RTC: return 0; // TODO: RTC
		case MEMMAP_IO_MOUSE: return mouse_read(address & 0x1f);
		case MEMMAP_IO_EMU: return emu_read(address & 0xf);
		case MEMMAP_NULL: return 0;
		default: return 0;
	}
	return 0;
}

//
// interface for fake6502
//
// if debugOn then reads memory only for debugger; no I/O, no side effects whatsoever

uint8_t
read6502(uint16_t address) 
{
	uint8_t const value = real_read6502(address);
	#ifdef TRACE
	printf("$%04x >> $%02x\n", address, value);
	#endif
	return value;
}

uint8_t
debug_read6502(uint16_t address, uint8_t bank)
{
	switch (memory_map_hi[address >> 8]) {
		case MEMMAP_NULL: return 0;
		case MEMMAP_DIRECT: return RAM[address];
		case MEMMAP_IO: return debug_io_read(address);
		case MEMMAP_RAMBANK: return debug_ram_read(address, bank);
		case MEMMAP_ROMBANK: return debug_rom_read(address, bank);
		default: return 0;
	}
	return 0;
}

uint8_t
real_read6502(uint16_t address)
{
	switch(memory_map_hi[address >> 8]) {
		case MEMMAP_NULL: return 0;
		case MEMMAP_DIRECT: return RAM[address];
		case MEMMAP_IO: return real_io_read(address);
		case MEMMAP_RAMBANK: return real_ram_read(address);
		case MEMMAP_ROMBANK: return real_rom_read(address);
		default: return 0;
	}
	return 0;
}

static void
ram_write(uint16_t address, uint8_t value)
{
	RAM[0xa000 + (effective_ram_bank() << 13) + address - 0xa000] = value;
}

static void
sound_write(uint16_t address, uint8_t value)
{
	static uint8_t lastAudioAdr = 0;
	if (address == 0) {
		lastAudioAdr = value;
	} else if (address == 1) {
		YM_write_reg(lastAudioAdr, value);
	}
}

static void
io_write(uint16_t address, uint8_t value)
{
	switch (memory_map_io[address & 0xff]) {
		case MEMMAP_NULL: break;
		case MEMMAP_IO_SOUND: sound_write(address & 0x1f, value); break; // TODO: Sound
		case MEMMAP_IO_VIDEO: video_write(address & 0x1f, value); break;
		case MEMMAP_IO_LCD: break; // TODO: Character LCD
		case MEMMAP_IO_VIA1: via1_write(address & 0xf, value); break;
		case MEMMAP_IO_VIA2: via2_write(address & 0xf, value); break;
		case MEMMAP_IO_RTC: break; // TODO: RTC
		case MEMMAP_IO_MOUSE: break;
		case MEMMAP_IO_EMU: emu_write(address & 0xf, value); break;
		default: break;
	}
}

void
write6502(uint16_t address, uint8_t value)
{
#ifdef TRACE
	printf("$%04x << $%02x\n", address, value);
#endif

	switch (memory_map_hi[address >> 8]) {
		case MEMMAP_NULL: break;
		case MEMMAP_DIRECT: RAM[address] = value; break;
		case MEMMAP_IO: io_write(address, value); break;
		case MEMMAP_RAMBANK: ram_write(address, value); break;
		case MEMMAP_ROMBANK: break;
		default: break;
	}
	//if (address < 0x9f00) { // RAM
	//	RAM[address] = value;
	//} else if (address < 0xa000) { // I/O
	//	if (address >= 0x9f00 && address < 0x9f20) {
	//		// TODO: sound
	//	} else if (address >= 0x9f20 && address < 0x9f40) {
	//		video_write(address & 0x1f, value);
	//	} else if (address >= 0x9f40 && address < 0x9f60) {
	//		// TODO: character LCD
	//	} else if (address >= 0x9f60 && address < 0x9f70) {
	//		via1_write(address & 0xf, value);
	//	} else if (address >= 0x9f70 && address < 0x9f80) {
	//		via2_write(address & 0xf, value);
	//	} else if (address >= 0x9f80 && address < 0x9fa0) {
	//		// TODO: RTC
	//	} else if (address >= 0x9fb0 && address < 0x9fc0) {
	//		// emulator state
	//		emu_write(address & 0xf, value);
	//	} else if (address == 0x9fe0) {
	//		lastAudioAdr = value;
	//	} else if (address == 0x9fe1) {
	//		YM_write_reg(lastAudioAdr, value);
	//	} else {
	//		// future expansion
	//	}
	//} else if (address < 0xc000) { // banked RAM
	//	RAM[0xa000 + (effective_ram_bank() << 13) + address - 0xa000] = value;
	//} else { // ROM
	//	// ignore
	//}
}

//
// saves the memory content into a file
//

void
memory_save(SDL_RWops *f, bool dump_ram, bool dump_bank)
{
	if (dump_ram) {
		SDL_RWwrite(f, &RAM[0], sizeof(uint8_t), 0xa000);
	}
	if (dump_bank) {
		SDL_RWwrite(f, &RAM[0xa000], sizeof(uint8_t), (num_ram_banks * 8192));
	}
}


///
///
///

void
memory_set_ram_bank(uint8_t bank)
{
	ram_bank = bank & (NUM_MAX_RAM_BANKS - 1);
}

uint8_t
memory_get_ram_bank()
{
	return ram_bank;
}

void
memory_set_rom_bank(uint8_t bank)
{
	rom_bank = bank & (NUM_ROM_BANKS - 1);;
}

uint8_t
memory_get_rom_bank()
{
	return rom_bank;
}

// Control the GIF recorder
void
emu_recorder_set(gif_recorder_command_t command)
{
	// turning off while recording is enabled
	if (command == RECORD_GIF_PAUSE && record_gif != RECORD_GIF_DISABLED) {
		record_gif = RECORD_GIF_PAUSED; // need to save
	}
	// turning on continuous recording
	if (command == RECORD_GIF_RESUME && record_gif != RECORD_GIF_DISABLED) {
		record_gif = RECORD_GIF_ACTIVE;		// activate recording
	}
	// capture one frame
	if (command == RECORD_GIF_SNAP && record_gif != RECORD_GIF_DISABLED) {
		record_gif = RECORD_GIF_SINGLE;		// single-shot
	}
}

//
// read/write emulator state (feature flags)
//
// 0: debugger_enabled
// 1: log_video
// 2: log_keyboard
// 3: echo_mode
// 4: save_on_exit
// 5: record_gif
// POKE $9FB3,1:PRINT"ECHO MODE IS ON":POKE $9FB3,0
void
emu_write(uint8_t reg, uint8_t value)
{
	bool v = value != 0;
	switch (reg) {
		case 0: debugger_enabled = v; break;
		case 1: log_video = v; break;
		case 2: log_keyboard = v; break;
		case 3: echo_mode = value; break;
		case 4: save_on_exit = v; break;
		case 5: emu_recorder_set((gif_recorder_command_t) value); break;
		case 15: led_status = v; break;
		default: printf("WARN: Invalid register %x\n", DEVICE_EMULATOR + reg);
	}
}

uint8_t
debug_emu_read(uint8_t reg)
{
	if (reg == 0) {
		return debugger_enabled ? 1 : 0;
	} else if (reg == 1) {
		return log_video ? 1 : 0;
	} else if (reg == 2) {
		return log_keyboard ? 1 : 0;
	} else if (reg == 3) {
		return echo_mode;
	} else if (reg == 4) {
		return save_on_exit ? 1 : 0;
	} else if (reg == 5) {
		return record_gif;

	} else if (reg == 8) {
		return (clockticks6502 >> 0) & 0xff;
	} else if (reg == 9) {
		return (clockticks6502 >> 8) & 0xff;
	} else if (reg == 10) {
		return (clockticks6502 >> 16) & 0xff;
	} else if (reg == 11) {
		return (clockticks6502 >> 24) & 0xff;

	} else if (reg == 13) {
		return keymap;
	} else if (reg == 14) {
		return '1'; // emulator detection
	} else if (reg == 15) {
		return '6'; // emulator detection
	}
	return -1;
}

uint8_t
emu_read(uint8_t reg)
{
	if (reg == 0) {
		return debugger_enabled ? 1 : 0;
	} else if (reg == 1) {
		return log_video ? 1 : 0;
	} else if (reg == 2) {
		return log_keyboard ? 1 : 0;
	} else if (reg == 3) {
		return echo_mode;
	} else if (reg == 4) {
		return save_on_exit ? 1 : 0;
	} else if (reg == 5) {
		return record_gif;

	} else if (reg == 8) {
		return (clockticks6502 >> 0) & 0xff;
	} else if (reg == 9) {
		return (clockticks6502 >> 8) & 0xff;
	} else if (reg == 10) {
		return (clockticks6502 >> 16) & 0xff;
	} else if (reg == 11) {
		return (clockticks6502 >> 24) & 0xff;

	} else if (reg == 13) {
		return keymap;
	} else if (reg == 14) {
		return '1'; // emulator detection
	} else if (reg == 15) {
		return '6'; // emulator detection
	}
	printf("WARN: Invalid register %x\n", DEVICE_EMULATOR + reg);
	return -1;
}
