
// *******************************************************************************************
// *******************************************************************************************
//
//		File:		debugger.c
//		Date:		5th September 2019
//		Purpose:	Debugger code
//		Author:		Paul Robson (paul@robson.org.uk)
//
// *******************************************************************************************
// *******************************************************************************************

#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>
#include <SDL.h>
#include "glue.h"
#include "disasm.h"
#include "memory.h"
#include "video.h"
#include "cpu/fake6502.h"
#include "debugger.h"
#include "rendertext.h"

static void DEBUGHandleKeyEvent(SDL_Keycode key,int isShift);

static void DEBUGNumber(int x,int y,int n,int w, SDL_Color colour);
static void DEBUGAddress(int x, int y, int bank, int addr, SDL_Color colour);
static void DEBUGVAddress(int x, int y, int addr, SDL_Color colour);

static void DEBUGRenderData(int y,int data);
static void DEBUGRenderDiffData(int y, int data);
static void DEBUGRenderZeroPageRegisters(int y);
static void DEBUGRenderDiffZeroPageRegisters(int y);
static int  DEBUGRenderRegisters(void);
static void DEBUGRenderVRAM(int y, int data);
static void DEBUGRenderCode(int lines,int initialPC);
static void DEBUGRenderStack(int bytesCount);
static void DEBUGRenderCmdLine();
static bool DEBUGBuildCmdLine(SDL_Keycode key);
static void DEBUGExecCmd();

// *******************************************************************************************
//
//		This is the minimum-interference flag. It's designed so that when
//		its non-zero DEBUGRenderDisplay() is called.
//
//			if (showDebugOnRender != 0) {
//				DEBUGRenderDisplay(SCREEN_WIDTH,SCREEN_HEIGHT,renderer);
//				SDL_RenderPresent(renderer);
//				return true;
//			}
//
//		before the SDL_RenderPresent call in video_update() in video.c
//
//		This controls what is happening. It is at the top of the main loop in main.c
//
//			if (isDebuggerEnabled != 0) {
//				int dbgCmd = DEBUGGetCurrentStatus();
//				if (dbgCmd > 0) continue;
//				if (dbgCmd < 0) break;
//			}
//
//		Both video.c and main.c require debugger.h to be included.
//
//		isDebuggerEnabled should be a flag set as a command line switch - without it
//		it will run unchanged. It should not be necessary to test the render code
//		because showDebugOnRender is statically initialised to zero and will only
//		change if DEBUGGetCurrentStatus() is called.
//
// *******************************************************************************************

//
//				0-9A-F sets the program address, with shift sets the data address.
//
#define DBGKEY_HOME 	SDLK_F1 								// F1 is "Goto PC"
#define DBGKEY_RESET 	SDLK_F2 								// F2 resets the 6502
#define DBGKEY_RUN 		SDLK_F5 								// F5 is run.
#define DBGKEY_SETBRK 	SDLK_F9									// F9 sets breakpoint
#define DBGKEY_STEP 	SDLK_F11 								// F11 is step into.
#define DBGKEY_STEPOVER	SDLK_F10 								// F10 is step over.
#define DBGKEY_PAGE_NEXT	SDLK_KP_PLUS
#define DBGKEY_PAGE_PREV	SDLK_KP_MINUS

#define DBGSCANKEY_BRK 	SDL_SCANCODE_F12 						// F12 is break into running code.
#define DBGSCANKEY_SHOW	SDL_SCANCODE_TAB 						// Show screen key.
																// *** MUST BE SCAN CODES ***

#define DBGMAX_ZERO_PAGE_REGISTERS 20

#define DDUMP_RAM	0
#define DDUMP_VERA	1
#define DDUMP_RAMDIFF 2

enum DBG_CMD { CMD_DUMP_MEM='m', CMD_DUMP_VERA='v', CMD_DISASM='d', CMD_SET_BANK='b', CMD_SET_REGISTER='r', CMD_FILL_MEMORY='f' };

// RGB colours
const SDL_Color col_bkgnd= {0, 0, 0, 255};
const SDL_Color col_label= {0, 255, 0, 255};
const SDL_Color col_data= {0, 255, 255, 255};
const SDL_Color col_highlight= {255, 255, 0, 255};
const SDL_Color col_cmdLine= {255, 255, 255, 255};

const SDL_Color col_vram_tilemap  = {0, 255, 255, 255};
const SDL_Color col_vram_tiledata = {0, 255, 0, 255};
const SDL_Color col_vram_special  = {255, 92, 92, 255};
const SDL_Color col_vram_sprite   = {255, 192, 92, 255};
const SDL_Color col_vram_other    = {128, 128, 128, 255};

const SDL_Color col_data_changed = {255, 92, 92, 255};

int showDebugOnRender = 0;										// Used to trigger rendering in video.c
int showFullDisplay = 0; 										// If non-zero show the whole thing.
int currentPC = -1;												// Current PC value.
int currentData = 0;											// Current data display address.
int currentPCBank = -1;
int currentBank = -1;
int currentMode = DMODE_RUN;									// Start running.
int breakPoint = -1; 											// User Break
int stepBreakPoint = -1;										// Single step break.
int dumpmode          = DDUMP_RAM;

char cmdLine[64]= "";											// command line buffer
int currentPosInLine= 0;										// cursor position in the buffer (NOT USED _YET_)
int currentLineLen= 0;											// command line buffer length

int    oldRegisters[DBGMAX_ZERO_PAGE_REGISTERS];      // Old ZP Register values, for change detection
char * oldRegChange[DBGMAX_ZERO_PAGE_REGISTERS];      // Change notification flags for output
int    oldRegisterTicks = 0;                          // Last PC when change notification was run

//
//		This flag controls
//

SDL_Renderer *dbgRenderer; 										// Renderer passed in.

// *******************************************************************************************
//
//			This is used to determine who is in control. If it returns zero then
//			everything runs normally till the next call.
//			If it returns +ve, then it will update the video, and loop round.
//			If it returns -ve, then exit.
//
// *******************************************************************************************

int  DEBUGGetCurrentStatus(void) {

	SDL_Event event;
	if (currentPC < 0) currentPC = pc;							// Initialise current PC displayed.

	if (currentMode == DMODE_STEP) {							// Single step before
		currentPC = pc;											// Update current PC
		currentMode = DMODE_STOP;								// So now stop, as we've done it.
	}

	if (pc == breakPoint || pc == stepBreakPoint) {				// Hit a breakpoint.
		currentPC = pc;											// Update current PC
		currentMode = DMODE_STOP;								// So now stop, as we've done it.
		stepBreakPoint = -1;									// Clear step breakpoint.
	}

	if (SDL_GetKeyboardState(NULL)[DBGSCANKEY_BRK]) {			// Stop on break pressed.
		currentMode = DMODE_STOP;
		currentPC = pc; 										// Set the PC to what it is.
	}

	if(currentPCBank<0 && currentPC >= 0xA000) {
		currentPCBank= currentPC < 0xC000 ? memory_get_ram_bank() : memory_get_rom_bank();
	}

	if (currentMode != DMODE_RUN) {								// Not running, we own the keyboard.
		showFullDisplay = 										// Check showing screen.
					SDL_GetKeyboardState(NULL)[DBGSCANKEY_SHOW];
		while (SDL_PollEvent(&event)) { 						// We now poll events here.
			switch(event.type) {
				case SDL_QUIT:									// Time for exit
					return -1;

				case SDL_KEYDOWN:								// Handle key presses.
					DEBUGHandleKeyEvent(event.key.keysym.sym,
										SDL_GetModState() & (KMOD_LSHIFT|KMOD_RSHIFT));
					break;

			}
		}
	}

	showDebugOnRender = (currentMode != DMODE_RUN);				// Do we draw it - only in RUN mode.
	if (currentMode == DMODE_STOP) { 							// We're in charge.
		video_update();
		return 1;
	}

	return 0;													// Run wild, run free.
}

// *******************************************************************************************
//
//								Setup fonts and co
//
// *******************************************************************************************
void DEBUGInitUI(SDL_Renderer *pRenderer) {
		DEBUGInitChars(pRenderer);
		dbgRenderer = pRenderer;				// Save renderer.
}

// *******************************************************************************************
//
//								Setup fonts and co
//
// *******************************************************************************************
void DEBUGFreeUI() {
}

// *******************************************************************************************
//
//								Set a new breakpoint address. -1 to disable.
//
// *******************************************************************************************

void DEBUGSetBreakPoint(int newBreakPoint) {
	breakPoint = newBreakPoint;
}

// *******************************************************************************************
//
//								Break into debugger from code.
//
// *******************************************************************************************

void DEBUGBreakToDebugger(void) {
	currentMode = DMODE_STOP;
	currentPC = pc;
}

// *******************************************************************************************
//
//									Handle keyboard state.
//
// *******************************************************************************************

static void DEBUGHandleKeyEvent(SDL_Keycode key,int isShift) {
	int opcode;

	switch(key) {

		case DBGKEY_STEP:									// Single step (F11 by default)
			currentMode = DMODE_STEP; 						// Runs once, then switches back.
			break;

		case DBGKEY_STEPOVER:								// Step over (F10 by default)
			opcode = real_read6502(pc, false, 0);							// What opcode is it ?
			if (opcode == 0x20) { 							// Is it JSR ?
				stepBreakPoint = pc + 3;					// Then break 3 on.
				currentMode = DMODE_RUN;					// And run.
			} else {
				currentMode = DMODE_STEP;					// Otherwise single step.
			}
			break;

		case DBGKEY_RUN:									// F5 Runs until Break.
			currentMode = DMODE_RUN;
			break;

		case DBGKEY_SETBRK:									// F9 Set breakpoint to displayed.
			breakPoint = currentPC;
			break;

		case DBGKEY_HOME:									// F1 sets the display PC to the actual one.
			currentPC = pc;
			currentPCBank= currentPC < 0xC000 ? memory_get_ram_bank() : memory_get_rom_bank();
			break;

		case DBGKEY_RESET:									// F2 reset the 6502
			reset6502();
			currentPC = pc;
			currentPCBank= -1;
			break;

		case DBGKEY_PAGE_NEXT:
			currentBank += 1;
			break;

		case DBGKEY_PAGE_PREV:
			currentBank -= 1;
			break;

		case SDLK_PAGEDOWN:
			if (dumpmode == DDUMP_RAM) {
				currentData = (currentData + 0x128) & 0xFFFF;
			} else if (dumpmode == DDUMP_RAMDIFF) {
				currentData = (currentData + 0x094) & 0xFFFF;
			} else {
				currentData = (currentData + 0x250) & 0x1FFFF;
			}
			break;

		case SDLK_PAGEUP:
			if (dumpmode == DDUMP_RAM) {
				currentData = (currentData - 0x128) & 0xFFFF;
			} else if (dumpmode == DDUMP_RAMDIFF) {
				currentData = (currentData - 0x094) & 0xFFFF;
			} else {
				currentData = (currentData - 0x250) & 0x1FFFF;
			}
			break;

		default:
			if(DEBUGBuildCmdLine(key)) {
				// printf("cmd line: %s\n", cmdLine);
				DEBUGExecCmd();
			}
			break;
	}

}

char kNUM_KEYPAD_CHARS[10] = {'1','2','3','4','5','6','7','8','9','0'};

static bool DEBUGBuildCmdLine(SDL_Keycode key) {
	// right now, let's have a rudimentary input: only backspace to delete last char
	// later, I want a real input line with delete, backspace, left and right cursor
	// devs like their comfort ;)
	if(currentLineLen <= sizeof(cmdLine)) {
		if(
			(key >= SDLK_SPACE && key <= SDLK_AT)
			||
			(key >= SDLK_LEFTBRACKET && key <= SDLK_z)
			||
			(key >= SDLK_KP_1 && key <= SDLK_KP_0)
			) {
			cmdLine[currentPosInLine++]= key>=SDLK_KP_1 ? kNUM_KEYPAD_CHARS[key-SDLK_KP_1] : key;
			if(currentPosInLine > currentLineLen) {
				currentLineLen++;
			}
		} else if(key == SDLK_BACKSPACE) {
			currentPosInLine--;
			if(currentPosInLine<0) {
				currentPosInLine= 0;
			}
			currentLineLen--;
			if(currentLineLen<0) {
				currentLineLen= 0;
			}
		}
		cmdLine[currentLineLen]= 0;
	}
	return (key == SDLK_RETURN) || (key == SDLK_KP_ENTER);
}

static uint8_t *Snapshot_ram = NULL;

static uint8_t
read_diff(uint16_t address, bool debugOn, uint8_t bank)
{
	bank %= num_ram_banks;

	if (address < 0x9f00) { // RAM
		return Snapshot_ram[address];
	} else if (address < 0xa000) { // I/O
		return 0xff;
	} else if (address < 0xc000) { // banked RAM
		return Snapshot_ram[0xa000 + (bank << 13) + address - 0xa000];
	} else { // banked ROM
		return real_read6502(address, debugOn, bank);
	}
}

static void DEBUGExecCmd() {
	int number, addr, size, incr;
	char reg[10];
	char cmd;
	char *line= ltrim(cmdLine);

	cmd= *line;
	if(*line) {
		line++;
	}
	// printf("cmd:%c line: '%s'\n", cmd, line);

	if (strcmp(cmdLine, "snap") == 0) {
		if (Snapshot_ram != NULL) {
			free(Snapshot_ram);
		}
		Snapshot_ram = malloc(RAM_SIZE);

		for (uint16_t i = 0; i < 0x9f00; ++i) {
			Snapshot_ram[i] = real_read6502(i, true, 0);
		}
		for (uint16_t i = 0x9f00; i < 0xa000; ++i) {
			Snapshot_ram[i] = 0;
		}
		for (uint16_t b = 0; b < num_ram_banks; ++b) {
			for (uint16_t i = 0xa000; i < 0xc000; ++i) {
				Snapshot_ram[0xa000 + (b << 13) + i - 0xa000] = real_read6502(i, true, b);
			}
		}
		currentPosInLine = currentLineLen = *cmdLine = 0;
		return;
	}

	if (strcmp(cmdLine, "diff") == 0) {
		if (Snapshot_ram != NULL) {
			dumpmode         = DDUMP_RAMDIFF;
		}
		currentPosInLine = currentLineLen = *cmdLine = 0;
		return;
	}

	switch (cmd) {
		case CMD_DUMP_MEM:
			sscanf(line, "%x", &number);
			addr= number & 0xFFFF;
			// Banked Memory, RAM then ROM
			if(addr >= 0xA000) {
				currentBank= (number & 0xFF0000) >> 16;
			}
			currentData= addr;
			dumpmode    = DDUMP_RAM;
			break;

		case CMD_DUMP_VERA:
			sscanf(line, "%x", &number);
			addr = number & 0x1FFFF;
			currentData = addr;
			dumpmode    = DDUMP_VERA;
			break;

		case CMD_FILL_MEMORY:
			size = 1;
			sscanf(line, "%x %x %d %d", &addr, &number, &size, &incr);

			if (dumpmode == DDUMP_RAM) {
				addr &= 0xFFFF;
				do {
					if (addr >= 0xC000) {
						// Nop.
					} else if (addr >= 0xA000) {
						RAM[0xa000 + (currentBank << 13) + addr - 0xa000] = number;
					} else {
						RAM[addr] = number;
					}
					if (incr) {
						addr += incr;
					} else {
						++addr;
					}
					addr &= 0xFFFF;
					--size;
				} while (size > 0);
			} else {
				addr &= 0x1FFFF;
				do {
					video_space_write(addr, number);
					if (incr) {
						addr += incr;
					} else {
						++addr;
					}
					addr &= 0x1FFFF;
					--size;
				} while (size > 0);
			}
			break;

		case CMD_DISASM:
			sscanf(line, "%x", &number);
			addr= number & 0xFFFF;
			// Banked Memory, RAM then ROM
			if(addr >= 0xA000) {
				currentPCBank= (number & 0xFF0000) >> 16;
			}
			currentPC= addr;
			break;

		case CMD_SET_BANK:
			sscanf(line, "%s %d", reg, &number);

			if(!strcmp(reg, "rom")) {
				memory_set_rom_bank(number & 0x00FF);
			}
			if(!strcmp(reg, "ram")) {
				memory_set_ram_bank(number & 0x00FF);
			}
			break;

		case CMD_SET_REGISTER:
			sscanf(line, "%s %x", reg, &number);

			if(!strcmp(reg, "pc")) {
				pc= number & 0xFFFF;
			}
			if(!strcmp(reg, "a")) {
				a= number & 0x00FF;
			}
			if(!strcmp(reg, "x")) {
				x= number & 0x00FF;
			}
			if(!strcmp(reg, "y")) {
				y= number & 0x00FF;
			}
			if(!strcmp(reg, "sp")) {
				sp= number & 0x00FF;
			}
			break;

		default:
			break;
	}

	currentPosInLine= currentLineLen= *cmdLine= 0;
}

// *******************************************************************************************
//
//							Render the emulator debugger display.
//
// *******************************************************************************************

void DEBUGRenderDisplay(int width, int height) {
	if (showFullDisplay) return;								// Not rendering debug.

	SDL_Rect rc;
	rc.w = DBG_WIDTH * 6 * CHAR_SCALE;							// Erase background, set up rect
	rc.h = height;
	xPos = width-rc.w;yPos = 0; 								// Position screen
	rc.x = xPos;rc.y = yPos; 									// Set rectangle and black out.
	SDL_SetRenderDrawColor(dbgRenderer,0,0,0,SDL_ALPHA_OPAQUE);
	SDL_RenderFillRect(dbgRenderer,&rc);

	DEBUGRenderRegisters();							// Draw register name and values.
	DEBUGRenderCode(20, currentPC);							// Render 6502 disassembly.
	if (dumpmode == DDUMP_RAM) {
		DEBUGRenderData(21, currentData);
		DEBUGRenderZeroPageRegisters(21);
	} else if (dumpmode == DDUMP_RAMDIFF) {
		DEBUGRenderDiffData(21, currentData);
		DEBUGRenderDiffZeroPageRegisters(21);		
	} else {
		DEBUGRenderVRAM(21, currentData);
	}
	DEBUGRenderStack(20);

	DEBUGRenderCmdLine(xPos, rc.w, height);
}

// *******************************************************************************************
//
//									 Render command Line
//
// *******************************************************************************************

static void DEBUGRenderCmdLine(int x, int width, int height) {
	char buffer[sizeof(cmdLine)+1];

	SDL_SetRenderDrawColor(dbgRenderer, 255, 255, 255, SDL_ALPHA_OPAQUE);
	SDL_RenderDrawLine(dbgRenderer, x, height-12, x+width, height-12);

	sprintf(buffer, ">%s", cmdLine);
	DEBUGString(dbgRenderer, 0, DBG_HEIGHT-1, buffer, col_cmdLine);
}

// *******************************************************************************************
//
//									 Render Zero Page Registers
//
// *******************************************************************************************

static void DEBUGRenderZeroPageRegisters(int y) {
#define LAST_R 15
   int reg = 0;
   int y_start = y;
   char lbl[6];
   while (reg < DBGMAX_ZERO_PAGE_REGISTERS) {
      if (((y-y_start) % 5) != 0) {           // Break registers into groups of 5, easier to locate
         if (reg <= LAST_R)
            sprintf(lbl, "R%d", reg);
         else
            sprintf(lbl, "x%d", reg);

         DEBUGString(dbgRenderer, DBG_ZP_REG, y, lbl, col_label);

         int reg_addr = 2 + reg * 2;
         int n = real_read6502(reg_addr+1, true, currentBank)*256+real_read6502(reg_addr, true, currentBank);
         
         DEBUGNumber(DBG_ZP_REG+5, y, n, 4, col_data);

         if (oldRegChange[reg] != NULL)
            DEBUGString(dbgRenderer, DBG_ZP_REG+9, y, oldRegChange[reg], col_data);

         if (oldRegisterTicks != clockticks6502) {   // change detection only when the emulated CPU changes
            oldRegChange[reg] = n != oldRegisters[reg] ? "*" : " ";
            oldRegisters[reg]=n;
         }
         reg++;
      }
      y++;
   }

   if (oldRegisterTicks != clockticks6502) {
      oldRegisterTicks = clockticks6502;
   }
#undef LAST_R
}

static void DEBUGRenderDiffZeroPageRegisters(int y)
{
#define LAST_R 15
	int  reg     = 0;
	int  y_start = y;
	char lbl[6];
	while (reg < DBGMAX_ZERO_PAGE_REGISTERS) {
		if (((y - y_start) % 5) != 0) { // Break registers into groups of 5, easier to locate
			if (reg <= LAST_R)
				sprintf(lbl, "R%d", reg);
			else
				sprintf(lbl, "x%d", reg);

			DEBUGString(dbgRenderer, DBG_ZP_REG, y, lbl, col_label);

			int reg_addr = 2 + reg * 2;
			int n        = real_read6502(reg_addr + 1, true, currentBank) * 256 + real_read6502(reg_addr, true, currentBank);
			int n2       = read_diff(reg_addr + 1, true, currentBank) * 256 + read_diff(reg_addr, true, currentBank);

			SDL_Color c        = (n == n2) ? col_data : col_data_changed;

			DEBUGNumber(DBG_ZP_REG + 5, y, n, 4, c);
			DEBUGString(dbgRenderer, DBG_ZP_REG + 9, y, "->", c);
			DEBUGNumber(DBG_ZP_REG + 11, y, n2, 4, c);
			reg++;
		}
		y++;
	}

	if (oldRegisterTicks != clockticks6502) {
		oldRegisterTicks = clockticks6502;
	}
#undef LAST_R
}

// *******************************************************************************************
//
//									 Render Data Area
//
// *******************************************************************************************

static void DEBUGRenderData(int y,int data) {
	while (y < DBG_HEIGHT-2) {									// To bottom of screen
		DEBUGAddress(DBG_MEMX, y, (uint8_t)currentBank, data & 0xFFFF, col_label);	// Show label.

		for (int i = 0;i < 8;i++) {
			int byte= real_read6502((data+i) & 0xFFFF, true, currentBank);
			DEBUGNumber(DBG_MEMX+8+i*3,y,byte,2, col_data);
			DEBUGWrite(dbgRenderer, DBG_MEMX+33+i,y,byte, col_data);
		}
		y++;
		data += 8;
	}
}

static void
DEBUGRenderDiffData(int y, int data)
{
	while (y < DBG_HEIGHT - 2) {                                                   // To bottom of screen
		DEBUGAddress(DBG_MEMX, y, (uint8_t)currentBank, data & 0xFFFF, col_label); // Show label.

		for (int i = 0; i < 4; i++) {
			int byte = real_read6502((data + i) & 0xFFFF, true, currentBank);
			int byte2 = read_diff((data + i) & 0xFFFF, true, currentBank);

			SDL_Color c = (byte == byte2) ? col_data : col_data_changed;
			// "XX->XX "
			DEBUGNumber(DBG_MEMX + 8 + i * 7, y, byte, 2, c);
			DEBUGString(dbgRenderer, DBG_MEMX + 10 + i * 7, y, "->", c);
			DEBUGNumber(DBG_MEMX + 12 + i * 7, y, byte2, 2, c);
		}
		y++;
		data += 4;
	}
}

static void
DEBUGRenderVRAM(int y, int data)
{
	while (y < DBG_HEIGHT - 2) {                                                   // To bottom of screen
		DEBUGVAddress(DBG_MEMX, y, data & 0x1FFFF, col_label); // Show label.

		for (int i = 0; i < 16; i++) {
			int addr = (data + i) & 0x1FFFF;
			int byte = video_space_read(addr);

			if (video_is_special_address(addr)) {
				DEBUGNumber(DBG_MEMX + 6 + i * 3, y, byte, 2, col_vram_special);
			} else if (video_is_sprite_address(addr)) {
				DEBUGNumber(DBG_MEMX + 6 + i * 3, y, byte, 2, col_vram_sprite);
			} else if (video_is_tilemap_address(addr)) {
				DEBUGNumber(DBG_MEMX + 6 + i * 3, y, byte, 2, col_vram_tilemap);
			} else if (video_is_tiledata_address(addr)) {
				DEBUGNumber(DBG_MEMX + 6 + i * 3, y, byte, 2, col_vram_tiledata);
			} else {
				DEBUGNumber(DBG_MEMX + 6 + i * 3, y, byte, 2, col_vram_other);
			}
		}
		y++;
		data += 16;
	}
}

// *******************************************************************************************
//
//									 Render Disassembly
//
// *******************************************************************************************

static void DEBUGRenderCode(int lines, int initialPC) {
	char buffer[32];
	for (int y = 0; y < lines; y++) { 							// Each line

		DEBUGAddress(DBG_ASMX, y, currentPCBank, initialPC, col_label);

		int size = disasm(initialPC, RAM, buffer, sizeof(buffer), true, currentPCBank);	// Disassemble code
		// Output assembly highlighting PC
		DEBUGString(dbgRenderer, DBG_ASMX+8, y, buffer, initialPC == pc ? col_highlight : col_data);
		initialPC += size;										// Forward to next
	}
}

// *******************************************************************************************
//
//									Render Register Display
//
// *******************************************************************************************

static char *labels[] = { "NV-BDIZC","","","A","X","Y","","BKA","BKO", "PC","SP","","BRK","", "VA","VD0","VD1","VCT", NULL };


static int DEBUGRenderRegisters(void) {
	int n = 0,yc = 0;
	while (labels[n] != NULL) {									// Labels
		DEBUGString(dbgRenderer, DBG_LBLX,n,labels[n], col_label);n++;
	}
	yc++;
	DEBUGNumber(DBG_LBLX, yc, (status >> 7) & 1, 1, col_data);
	DEBUGNumber(DBG_LBLX+1, yc, (status >> 6) & 1, 1, col_data);
	DEBUGNumber(DBG_LBLX+3, yc, (status >> 4) & 1, 1, col_data);
	DEBUGNumber(DBG_LBLX+4, yc, (status >> 3) & 1, 1, col_data);
	DEBUGNumber(DBG_LBLX+5, yc, (status >> 2) & 1, 1, col_data);
	DEBUGNumber(DBG_LBLX+6, yc, (status >> 1) & 1, 1, col_data);
	DEBUGNumber(DBG_LBLX+7, yc, (status >> 0) & 1, 1, col_data);
	yc+= 2;

	DEBUGNumber(DBG_DATX, yc++, a, 2, col_data);
	DEBUGNumber(DBG_DATX, yc++, x, 2, col_data);
	DEBUGNumber(DBG_DATX, yc++, y, 2, col_data);
	yc++;

	DEBUGNumber(DBG_DATX, yc++, memory_get_ram_bank(), 2, col_data);
	DEBUGNumber(DBG_DATX, yc++, memory_get_rom_bank(), 2, col_data);
	DEBUGNumber(DBG_DATX, yc++, pc, 4, col_data);
	DEBUGNumber(DBG_DATX, yc++, sp|0x100, 4, col_data);
	yc++;

	DEBUGNumber(DBG_DATX, yc++, breakPoint & 0xFFFF, 4, col_data);
	yc++;

	DEBUGNumber(DBG_DATX, yc++, video_read(0, true) | (video_read(1, true)<<8) | (video_read(2, true)<<16), 2, col_data);
	DEBUGNumber(DBG_DATX, yc++, video_read(3, true), 2, col_data);
	DEBUGNumber(DBG_DATX, yc++, video_read(4, true), 2, col_data);
	DEBUGNumber(DBG_DATX, yc++, video_read(5, true), 2, col_data);

	return n; 													// Number of code display lines
}

// *******************************************************************************************
//
//									Render Top of Stack
//
// *******************************************************************************************

static void DEBUGRenderStack(int bytesCount) {
	int data= (sp+1) | 0x100;
	int y= 0;
	while (y < bytesCount) {
		DEBUGNumber(DBG_STCK,y,data & 0xFFFF,4, col_label);
		int byte = real_read6502((data++) & 0xFFFF, false, 0);
		DEBUGNumber(DBG_STCK+5,y,byte,2, col_data);
		DEBUGWrite(dbgRenderer, DBG_STCK+9,y,byte, col_data);
		y++;
		data= (data & 0xFF) | 0x100;
	}
}

// *******************************************************************************************
//
//									Write Hex Constant
//
// *******************************************************************************************

static void DEBUGNumber(int x, int y, int n, int w, SDL_Color colour) {
	char fmtString[8],buffer[16];
	snprintf(fmtString, sizeof(fmtString), "%%0%dX", w);
	snprintf(buffer, sizeof(buffer), fmtString, n);
	DEBUGString(dbgRenderer, x, y, buffer, colour);
}

// *******************************************************************************************
//
//									Write Bank:Address
//
// *******************************************************************************************
static void DEBUGAddress(int x, int y, int bank, int addr, SDL_Color colour) {
	char buffer[4];

	if(addr >= 0xA000) {
		snprintf(buffer, sizeof(buffer), "%.2X:", bank);
	} else {
		strcpy(buffer, "--:");
	}

	DEBUGString(dbgRenderer, x, y, buffer, colour);

	DEBUGNumber(x+3, y, addr, 4, colour);

}

static void
DEBUGVAddress(int x, int y, int addr, SDL_Color colour)
{
	DEBUGNumber(x, y, addr, 5, colour);
}
