/* Fake6502 CPU emulator core v1.1 *******************
 * (c)2011 Mike Chambers (miker00lz@gmail.com)       *
 *****************************************************
 * v1.1 - Small bugfix in BIT opcode, but it was the *
 *        difference between a few games in my NES   *
 *        emulator working and being broken!         *
 *        I went through the rest carefully again    *
 *        after fixing it just to make sure I didn't *
 *        have any other typos! (Dec. 17, 2011)      *
 *                                                   *
 * v1.0 - First release (Nov. 24, 2011)              *
 *****************************************************
 * LICENSE: This source code is released into the    *
 * public domain, but if you use it please do give   *
 * credit. I put a lot of effort into writing this!  *
 *                                                   *
 *****************************************************
 * Fake6502 is a MOS Technology 6502 CPU emulation   *
 * engine in C. It was written as part of a Nintendo *
 * Entertainment System emulator I've been writing.  *
 *                                                   *
 * A couple important things to know about are two   *
 * defines in the code. One is "UNDOCUMENTED" which, *
 * when defined, allows Fake6502 to compile with     *
 * full support for the more predictable             *
 * undocumented instructions of the 6502. If it is   *
 * undefined, undocumented opcodes just act as NOPs. *
 *                                                   *
 * The other define is "NES_CPU", which causes the   *
 * code to compile without support for binary-coded  *
 * decimal (BCD) support for the ADC and SBC         *
 * opcodes. The Ricoh 2A03 CPU in the NES does not   *
 * support BCD, but is otherwise identical to the    *
 * standard MOS 6502. (Note that this define is      *
 * enabled in this file if you haven't changed it    *
 * yourself. If you're not emulating a NES, you      *
 * should comment it out.)                           *
 *                                                   *
 * If you do discover an error in timing accuracy,   *
 * or operation in general please e-mail me at the   *
 * address above so that I can fix it. Thank you!    *
 *                                                   *
 *****************************************************
 * Usage:                                            *
 *                                                   *
 * Fake6502 requires you to provide two external     *
 * functions:                                        *
 *                                                   *
 * uint8_t read6502(uint16_t address)                *
 * void write6502(uint16_t address, uint8_t value)   *
 *                                                   *
 *****************************************************
 * Useful functions in this emulator:                *
 *                                                   *
 * void reset6502()                                  *
 *   - Call this once before you begin execution.    *
 *                                                   *
 * void exec6502(uint32_t tickcount)                 *
 *   - Execute 6502 code up to the next specified    *
 *     count of clock ticks.                         *
 *                                                   *
 * void step6502()                                   *
 *   - Execute a single instrution.                  *
 *                                                   *
 * void irq6502()                                    *
 *   - Trigger a hardware IRQ in the 6502 core.      *
 *                                                   *
 * void nmi6502()                                    *
 *   - Trigger an NMI in the 6502 core.              *
 *                                                   *
 *****************************************************
 * Useful variables in this emulator:                *
 *                                                   *
 * uint32_t clockticks6502                           *
 *   - A running total of the emulated cycle count.  *
 *                                                   *
 * uint32_t instructions                             *
 *   - A running total of the total emulated         *
 *     instruction count. This is not related to     *
 *     clock cycle timing.                           *
 *                                                   *
 *****************************************************/

#include "fake6502.h"

#include <stdio.h>
#include <stdint.h>
#include "../debugger.h"

//6502 defines
#define UNDOCUMENTED //when this is defined, undocumented opcodes are handled.
                     //otherwise, they're simply treated as NOPs.

//#define NES_CPU      //when this is defined, the binary-coded decimal (BCD)
                     //status flag is not honored by ADC and SBC. the 2A03
                     //CPU in the Nintendo Entertainment System does not
                     //support BCD operation.

#define FLAG_CARRY     0x01
#define FLAG_ZERO      0x02
#define FLAG_INTERRUPT 0x04
#define FLAG_DECIMAL   0x08
#define FLAG_BREAK     0x10
#define FLAG_CONSTANT  0x20
#define FLAG_OVERFLOW  0x40
#define FLAG_SIGN      0x80

#define BASE_STACK     0x100

struct cpu_state CPU;

#include "support.h"
#include "modes.h"

// Binary coded decimal adc
static inline void adcd(const uint16_t addr)
{
    const uint8_t value = read6502(addr);

    uint16_t a = (uint16_t)CPU.a + (uint16_t)(value & 0x0f) + (uint16_t)(CPU.status & FLAG_CARRY);
    if((a & 0xf) > 9) a += 6; // -= 10, += 16

    a += value & 0xf0;
    if((a & 0xf0) > 0x90) a += 0x60;

    clearflags(FLAG_ZERO | FLAG_CARRY | FLAG_SIGN | FLAG_OVERFLOW);
	// CPU.status |= select_zero(a) | select_carry(a) | select_sign((uint8_t)a) | select_overflow(a, CPU.a, value);
    CPU.a = (uint8_t)a;
}

// Hexadecimal adc
static inline void adcx(const uint16_t addr)
{
    const uint8_t value = read6502(addr);
    const uint16_t a = (uint16_t)CPU.a + (uint16_t)value + (uint16_t)(CPU.status & FLAG_CARRY);

    clearflags(FLAG_ZERO | FLAG_CARRY | FLAG_SIGN | FLAG_OVERFLOW);
    CPU.status |= select_zero(a) | select_carry(a) | select_sign((uint8_t)a) | select_overflow(a, CPU.a, value);
    CPU.a = (uint8_t)a;
}

static inline void and(const uint16_t addr)
{
    const uint8_t value = read6502(addr);
    CPU.a &= value;
    clearflags(FLAG_ZERO | FLAG_SIGN);
    CPU.status |= select_zero(CPU.a) | select_sign(CPU.a);
}

static inline void asla(const uint16_t addr)
{
    const uint16_t result = ((uint16_t)CPU.a) << 1;
    CPU.a = (uint8_t)result;

    clearflags(FLAG_ZERO | FLAG_CARRY | FLAG_SIGN);
    CPU.status |= select_zero(CPU.a) | select_carry(CPU.a) | select_sign(CPU.a);
}

static inline void aslm(const uint16_t addr)
{
    const uint8_t value = read6502(addr);
    const uint16_t result = ((uint16_t)value) << 1;
    write6502(addr, (uint8_t)result);

    clearflags(FLAG_ZERO | FLAG_CARRY | FLAG_SIGN);
    CPU.status |= select_zero(CPU.a) | select_carry(CPU.a) | select_sign(CPU.a);
}

static inline void bbr0(const uint16_t addr)
{
    const uint8_t value = read6502(addr);
    if(~value & 0x01) CPU.pc = CPU.ea;
}

static inline void bbr1(const uint16_t addr)
{
    const uint8_t value = read6502(addr);
    if(~value & 0x02) CPU.pc = CPU.ea;
}

static inline void bbr2(const uint16_t addr)
{
    const uint8_t value = read6502(addr);
    if(~value & 0x04) CPU.pc = CPU.ea;
}

static inline void bbr3(const uint16_t addr)
{
    const uint8_t value = read6502(addr);
    if(~value & 0x08) CPU.pc = CPU.ea;
}

static inline void bbr4(const uint16_t addr)
{
    const uint8_t value = read6502(addr);
    if(~value & 0x10) CPU.pc = CPU.ea;
}

static inline void bbr5(const uint16_t addr)
{
    const uint8_t value = read6502(addr);
    if(~value & 0x20) CPU.pc = CPU.ea;
}

static inline void bbr6(const uint16_t addr)
{
    const uint8_t value = read6502(addr);
    if(~value & 0x40) CPU.pc = CPU.ea;
}

static inline void bbr7(const uint16_t addr)
{
    const uint8_t value = read6502(addr);
    if(~value & 0x80) CPU.pc = CPU.ea;
}

static inline void bbs0(const uint16_t addr)
{
    const uint8_t value = read6502(addr);
    if(value & 0x01) CPU.pc = CPU.ea;
}

static inline void bbs1(const uint16_t addr)
{
    const uint8_t value = read6502(addr);
    if(value & 0x02) CPU.pc = CPU.ea;
}

static inline void bbs2(const uint16_t addr)
{
    const uint8_t value = read6502(addr);
    if(value & 0x04) CPU.pc = CPU.ea;
}

static inline void bbs3(const uint16_t addr)
{
    const uint8_t value = read6502(addr);
    if(value & 0x08) CPU.pc = CPU.ea;
}

static inline void bbs4(const uint16_t addr)
{
    const uint8_t value = read6502(addr);
    if(value & 0x10) CPU.pc = CPU.ea;
}

static inline void bbs5(const uint16_t addr)
{
    const uint8_t value = read6502(addr);
    if(value & 0x20) CPU.pc = CPU.ea;
}

static inline void bbs6(const uint16_t addr)
{
    const uint8_t value = read6502(addr);
    if(value & 0x40) CPU.pc = CPU.ea;
}

static inline void bbs7(const uint16_t addr)
{
    const uint8_t value = read6502(addr);
    if(value & 0x80) CPU.pc = CPU.ea;
}

static inline void bit(const uint16_t addr)
{
    const uint8_t value = read6502(addr);
    uint8_t a = CPU.a & value;
    clearflags(FLAG_ZERO | FLAG_OVERFLOW | FLAG_SIGN);
    CPU.status |= select_zero(a) | (a & 0xc0);
}

static inline void brk(const uint16_t addr) 
{
    push16(CPU.pc + 1); //push next instruction address onto stack
    push8(CPU.status | FLAG_BREAK); //push CPU status to stack
    setinterrupt(); //set interrupt flag
    cleardecimal(); // clear decimal flag (65C02 change)
    CPU.pc = (uint16_t)read6502(0xFFFE) | ((uint16_t)read6502(0xFFFF) << 8);
}

static inline void clc(const uint16_t addr)
{
    clearcarry();
}

static void substitute_cld(void);
static inline void cld(const uint16_t addr)
{
    substitute_cld();
    cleardecimal();
}

static inline void cli(const uint16_t addr)
{
    clearinterrupt();
}

static inline void clv(const uint16_t addr)
{
    clearoverflow();
}

static inline void cmp(const uint16_t addr)
{
    const uint8_t value = read6502(addr);
    uint16_t a = (int16_t)CPU.a - (int16_t)value;
    clearflags(FLAG_ZERO | FLAG_CARRY | FLAG_SIGN);
	CPU.status |= select_zero(a) | select_carry(~a) | select_sign(a);
}

static inline void cpx(const uint16_t addr)
{
    const uint8_t value = read6502(addr);
    uint16_t x = (int16_t)CPU.x - (int16_t)value;
    clearflags(FLAG_ZERO | FLAG_CARRY | FLAG_SIGN);
    CPU.status |= select_zero(x) | select_carry(~x) | select_sign(x);
}

static inline void cpy(const uint16_t addr)
{
    const uint8_t value = read6502(addr);
    uint16_t y = (int16_t)CPU.y - (int16_t)value;
    clearflags(FLAG_ZERO | FLAG_CARRY | FLAG_SIGN);
    CPU.status |= select_zero(y) | select_carry(~y) | select_sign(y);
}

static inline void deca(const uint16_t addr)
{
    --CPU.a;
    clearflags(FLAG_ZERO | FLAG_SIGN);
    CPU.status |= select_zero(CPU.a) | select_sign(CPU.a);
}

static inline void decm(const uint16_t addr)
{
    const uint8_t value = read6502(addr);
    uint8_t v = value-1;
    write6502(addr, v);
    clearflags(FLAG_ZERO | FLAG_SIGN);
    CPU.status |= select_zero(v) | select_sign(v);
}

static inline void dex(const uint16_t addr)
{
    CPU.x--;
    clearflags(FLAG_ZERO | FLAG_SIGN);
    CPU.status |= select_zero(CPU.x) | select_sign(CPU.x);
}

static inline void dey(const uint16_t addr)
{
    CPU.y--;
    clearflags(FLAG_ZERO | FLAG_SIGN);
    CPU.status |= select_zero(CPU.y) | select_sign(CPU.y);
}

static inline void eor(const uint16_t addr)
{
    const uint8_t value = read6502(addr);
    CPU.a = CPU.a ^ value;
    clearflags(FLAG_ZERO | FLAG_SIGN);
    CPU.status |= select_zero(CPU.a) | select_sign(CPU.a);
}

static inline void inca(const uint16_t addr)
{
    ++CPU.a;
    clearflags(FLAG_ZERO | FLAG_SIGN);
    CPU.status |= select_zero(CPU.a) | select_sign(CPU.a);
}

static inline void incm(const uint16_t addr)
{
    const uint8_t value = read6502(addr);
    uint8_t v = value+1;
    write6502(addr, v);
    clearflags(FLAG_ZERO | FLAG_SIGN);
    CPU.status |= select_zero(v) | select_sign(v);
}

static inline void inx(const uint16_t addr)
{
    CPU.x++;
    clearflags(FLAG_ZERO | FLAG_SIGN);
    CPU.status |= select_zero(CPU.x) | select_sign(CPU.x);
}

static inline void iny(const uint16_t addr)
{
    CPU.y++;
    clearflags(FLAG_ZERO | FLAG_SIGN);
    CPU.status |= select_zero(CPU.y) | select_sign(CPU.y);
}

static inline void jmp(const uint16_t addr)
{
    CPU.pc = addr;
}

static inline void jsr(const uint16_t addr)
{
    push16(CPU.pc - 1);
    CPU.pc = addr;
}

static inline void lda(const uint16_t addr)
{
    const uint8_t value = read6502(addr);
    CPU.a = value;
    clearflags(FLAG_ZERO | FLAG_SIGN);
    CPU.status |= select_zero(value) | select_sign(value);
}

static inline void ldx(const uint16_t addr)
{
    const uint8_t value = read6502(addr);
    CPU.x = value;
    clearflags(FLAG_ZERO | FLAG_SIGN);
    CPU.status |= select_zero(value) | select_sign(value);
}

static inline void ldy(const uint16_t addr)
{
    const uint8_t value = read6502(addr);
    CPU.y = value;
    clearflags(FLAG_ZERO | FLAG_SIGN);
    CPU.status |= select_zero(value) | select_sign(value);
}

static inline void lsra(const uint16_t addr)
{
    clearflags(FLAG_CARRY | FLAG_ZERO | FLAG_SIGN);
	CPU.status |= (CPU.a & 1);
	CPU.a >>= 1;
	CPU.status |= select_zero(CPU.a) | select_sign(CPU.a);
}

static inline void lsrm(const uint16_t addr)
{
    const uint8_t value = read6502(addr);
    const uint8_t result = value >> 1;
    write6502(addr, result);
	clearflags(FLAG_CARRY | FLAG_ZERO | FLAG_SIGN);
	CPU.status |= (value & 1) | select_zero(result) | select_sign(result);
}

static inline void ora(const uint16_t addr)
{
    const uint8_t value = read6502(addr);
    CPU.a |= value;
    clearflags(FLAG_ZERO | FLAG_SIGN);
    CPU.status |= select_zero(CPU.a) | select_sign(CPU.a);
}

static inline void pha(const uint16_t addr)
{
    push8(CPU.a);
}

static inline void php(const uint16_t addr)
{
    push8(CPU.status | FLAG_BREAK);
}

static inline void phx(const uint16_t addr)
{
    push8(CPU.x);
}

static inline void phy(const uint16_t addr)
{
    push8(CPU.y);
}

static inline void pla(const uint16_t addr) 
{
    CPU.a = pull8();

    clearflags(FLAG_ZERO | FLAG_SIGN);
    CPU.status |= select_zero(CPU.a) | select_sign(CPU.a);
}

static inline void plp(const uint16_t addr) 
{
    CPU.status = pull8() | FLAG_CONSTANT;
}

static inline void plx(const uint16_t addr) 
{
    CPU.x = pull8();

    clearflags(FLAG_ZERO | FLAG_SIGN);
    CPU.status |= select_zero(CPU.x) | select_sign(CPU.x);
}

static inline void ply(const uint16_t addr) 
{
    CPU.y = pull8();

    clearflags(FLAG_ZERO | FLAG_SIGN);
    CPU.status |= select_zero(CPU.y) | select_sign(CPU.y);
}

static inline void rola(const uint16_t addr)
{
	const uint8_t result = ((uint16_t)CPU.a << 1) | (CPU.status & FLAG_CARRY);

    clearflags(FLAG_CARRY | FLAG_ZERO | FLAG_SIGN);
    CPU.status |= select_zero(result) | (CPU.a >> 7) | select_sign(result);

    CPU.a = (uint8_t)result;
}

static inline void rolm(const uint16_t addr)
{
    const uint8_t value = read6502(addr);
    const uint8_t result = ((uint16_t)value << 1) | (CPU.status & FLAG_CARRY);
    write6502(addr, (uint8_t)result);

    clearflags(FLAG_CARRY | FLAG_ZERO | FLAG_SIGN);
    CPU.status |= select_zero(result) | select_carry(value >> 7) | select_sign(result);
}

static inline void rora(const uint16_t addr)
{
    const uint16_t result = ((uint16_t)CPU.a >> 1) | (CPU.status << 7);

    clearflags(FLAG_CARRY | FLAG_ZERO | FLAG_SIGN);
	CPU.status |= select_zero(result) | (CPU.a & 1) | select_sign(result);

    CPU.a = (uint8_t)result;
}

static inline void rorm(const uint16_t addr)
{
    const uint8_t value = read6502(addr);
    const uint8_t result = ((uint16_t)value >> 1) | (CPU.status << 7);
    write6502(addr, (uint8_t)result);

    clearflags(FLAG_CARRY | FLAG_ZERO | FLAG_SIGN);
	CPU.status |= select_zero(result) | (value & 1) | select_sign(result);
}

static inline void rti(const uint16_t addr)
{
    CPU.status = pull8();
    CPU.pc = pull16();
}

static inline void rts(const uint16_t addr)
{
    CPU.pc = pull16() + 1;
}

static inline void sbcd(const uint16_t addr)
{
    // See also: http://www.6502.org/tutorials/decimal_mode.html#A
    const uint8_t value = read6502(addr);
    const int16_t al = (int16_t)CPU.a - (value & 0xf) + (CPU.status & FLAG_CARRY) - 1;
    int16_t a = (int16_t)CPU.a - value + (CPU.status & FLAG_CARRY) - 1;
    if(a < 0) { a -= 0x60; }
    if(al < 0) { a -= 0x6; }
    clearflags(FLAG_CARRY | FLAG_ZERO | FLAG_SIGN | FLAG_OVERFLOW);
    CPU.status |= select_zero(a) | select_carry(~a) | select_overflow(a, CPU.a, value) | select_sign(a);
    CPU.a = a;
}

static inline void sbcx(const uint16_t addr)
{
    const uint8_t value = read6502(addr);
    const uint16_t a = (uint16_t)(CPU.a ^ 0xff) + (uint16_t)value + (uint16_t)(CPU.status & FLAG_CARRY);

    clearflags(FLAG_ZERO | FLAG_CARRY | FLAG_SIGN | FLAG_OVERFLOW);
    CPU.status |= select_zero(a) | select_carry(a) | select_sign((uint8_t)a) | select_overflow(a, CPU.a, value);
    CPU.a = (uint8_t)a;
}

static inline void sec(const uint16_t addr)
{
    setcarry();
}

static void substitute_sed(void);
static inline void sed(const uint16_t addr)
{
    substitute_sed();
    setdecimal();
}

static inline void sei(const uint16_t addr)
{
    setinterrupt();
}

static inline void sta(const uint16_t addr)
{
    write6502(addr, CPU.a);
}

static inline void stx(const uint16_t addr)
{
    write6502(addr, CPU.x);
}

static inline void sty(const uint16_t addr)
{
    write6502(addr, CPU.y);
}

static inline void stz(const uint16_t addr)
{
    write6502(addr, 0);
}

static inline void tax(const uint16_t addr)
{
    CPU.x = CPU.a;
    clearflags(FLAG_ZERO | FLAG_SIGN);
    CPU.status |= select_zero(CPU.x) | select_sign(CPU.x);
}

static inline void tay(const uint16_t addr)
{
    CPU.y = CPU.a;
    clearflags(FLAG_ZERO | FLAG_SIGN);
    CPU.status |= select_zero(CPU.x) | select_sign(CPU.x);
}

static inline void trb(const uint16_t addr)
{
    const uint8_t value = read6502(addr);

    clearzero();
    CPU.status |= select_zero(value & CPU.a);

    write6502(addr, value & ~CPU.a);
}

static inline void tsb(const uint16_t addr)
{
    const uint8_t value = read6502(addr);

    clearzero();
    CPU.status |= select_zero(value & CPU.a);

    write6502(addr, value | CPU.a);
}

static inline void tsx(const uint16_t addr)
{
    CPU.x = CPU.sp;
    clearflags(FLAG_ZERO | FLAG_SIGN);
    CPU.status |= select_zero(CPU.x) | select_sign(CPU.x);
}

static inline void txa(const uint16_t addr)
{
    CPU.a = CPU.x;
    clearflags(FLAG_ZERO | FLAG_SIGN);
    CPU.status |= select_zero(CPU.a) | select_sign(CPU.a);
}

static inline void txs(const uint16_t addr)
{
    CPU.sp = CPU.x;
}

static inline void tya(const uint16_t addr)
{
    CPU.a = CPU.y;
    clearflags(FLAG_ZERO | FLAG_SIGN);
    CPU.status |= select_zero(CPU.a) | select_sign(CPU.a);
}

static inline void wai(const uint16_t addr)
{
    CPU.wai = 1;
}

#define CPU_OP(CODE, OP) OP##_##CODE

#define IMPL_CPU_OP(OP, MODE, CODE, TICKS, PENALTY) static void CPU_OP(CODE, OP)() { \
    const uint16_t addr = MODE(PENALTY); \
    OP(addr); \
    CPU.perf.clock_ticks += TICKS; }

#define IMPL_CPU_IMP(OP, CODE, TICKS) static void CPU_OP(CODE, OP)() { \
    OP(0); \
    CPU.perf.clock_ticks += TICKS; }

#define IMPL_CPU_BRA(OP, CODE, COND) static void CPU_OP(CODE, OP)() { \
    const uint16_t addr = rel(0); \
    if(COND) { \
        CPU.perf.clock_ticks += 3 + (tick_penalty(CPU.pc, addr) << 1); \
        CPU.pc = addr; \
    } else { CPU.perf.clock_ticks += 2; } }

IMPL_CPU_OP(adcd, imm, 69, 2, 0);
IMPL_CPU_OP(adcd, zp, 65, 3, 0);
IMPL_CPU_OP(adcd, zpx, 75, 4, 0);
IMPL_CPU_OP(adcd, abso, 6d, 4, 0);
IMPL_CPU_OP(adcd, absx, 7d, 4, 1);
IMPL_CPU_OP(adcd, absy, 79, 4, 1);
IMPL_CPU_OP(adcd, indx, 61, 6, 0);
IMPL_CPU_OP(adcd, indy, 71, 5, 1);
IMPL_CPU_OP(adcd, ind0, 72, 5, 0);

IMPL_CPU_OP(adcx, imm, 69, 2, 0);
IMPL_CPU_OP(adcx, zp, 65, 3, 0);
IMPL_CPU_OP(adcx, zpx, 75, 4, 0);
IMPL_CPU_OP(adcx, abso, 6d, 4, 0);
IMPL_CPU_OP(adcx, absx, 7d, 4, 1);
IMPL_CPU_OP(adcx, absy, 79, 4, 1);
IMPL_CPU_OP(adcx, indx, 61, 6, 0);
IMPL_CPU_OP(adcx, indy, 71, 5, 1);
IMPL_CPU_OP(adcx, ind0, 72, 5, 0);

IMPL_CPU_OP(and, imm, 29, 2, 0);
IMPL_CPU_OP(and, zp, 25, 3, 0);
IMPL_CPU_OP(and, zpx, 35, 4, 0);
IMPL_CPU_OP(and, abso, 2d, 4, 0);
IMPL_CPU_OP(and, absx, 3d, 4, 1);
IMPL_CPU_OP(and, absy, 39, 4, 1);
IMPL_CPU_OP(and, indx, 21, 6, 0);
IMPL_CPU_OP(and, indy, 31, 5, 1);
IMPL_CPU_OP(and, ind0, 32, 5, 0);

IMPL_CPU_OP(asla, acc, 0a, 2, 0);
IMPL_CPU_OP(aslm, zp, 06, 5, 0);
IMPL_CPU_OP(aslm, zpx, 16, 6, 0);
IMPL_CPU_OP(aslm, abso, 0e, 6, 0);
IMPL_CPU_OP(aslm, absx, 1e, 7, 0);

IMPL_CPU_OP(bbr0, zprel, 0f, 2, 1);
IMPL_CPU_OP(bbr1, zprel, 1f, 2, 1);
IMPL_CPU_OP(bbr2, zprel, 2f, 2, 1);
IMPL_CPU_OP(bbr3, zprel, 3f, 2, 1);
IMPL_CPU_OP(bbr4, zprel, 4f, 2, 1);
IMPL_CPU_OP(bbr5, zprel, 5f, 2, 1);
IMPL_CPU_OP(bbr6, zprel, 6f, 2, 1);
IMPL_CPU_OP(bbr7, zprel, 7f, 2, 1);

IMPL_CPU_OP(bbs0, zprel, 8f, 2, 1);
IMPL_CPU_OP(bbs1, zprel, 9f, 2, 1);
IMPL_CPU_OP(bbs2, zprel, af, 2, 1);
IMPL_CPU_OP(bbs3, zprel, bf, 2, 1);
IMPL_CPU_OP(bbs4, zprel, cf, 2, 1);
IMPL_CPU_OP(bbs5, zprel, df, 2, 1);
IMPL_CPU_OP(bbs6, zprel, ef, 2, 1);
IMPL_CPU_OP(bbs7, zprel, ff, 2, 1);

IMPL_CPU_BRA(bcc, 90, ~CPU.status & FLAG_CARRY);
IMPL_CPU_BRA(bcs, b0, CPU.status & FLAG_CARRY);
IMPL_CPU_BRA(beq, f0, CPU.status & FLAG_ZERO);

IMPL_CPU_OP(bit, imm, 89, 3, 0);
IMPL_CPU_OP(bit, zp, 24, 3, 0);
IMPL_CPU_OP(bit, zpx, 34, 3, 0);
IMPL_CPU_OP(bit, abso, 2c, 4, 0);
IMPL_CPU_OP(bit, absx, 3c, 4, 0);

IMPL_CPU_BRA(bmi, 30, CPU.status & FLAG_SIGN);
IMPL_CPU_BRA(bne, d0, ~CPU.status & FLAG_ZERO);
IMPL_CPU_BRA(bpl, 10, ~CPU.status & FLAG_SIGN);
IMPL_CPU_BRA(bra, 80, 1);

IMPL_CPU_IMP(brk, 00, 7);

IMPL_CPU_BRA(bvc, 50, ~CPU.status & FLAG_OVERFLOW);
IMPL_CPU_BRA(bvs, 70, CPU.status & FLAG_OVERFLOW);

IMPL_CPU_IMP(clc, 18, 2);
IMPL_CPU_IMP(cld, d8, 2);
IMPL_CPU_IMP(cli, 58, 2);
IMPL_CPU_IMP(clv, b8, 2);

IMPL_CPU_OP(cmp, imm, c9, 2, 0);
IMPL_CPU_OP(cmp, zp, c5, 3, 0);
IMPL_CPU_OP(cmp, zpx, d5, 4, 0);
IMPL_CPU_OP(cmp, abso, cd, 4, 0);
IMPL_CPU_OP(cmp, absx, dd, 4, 1);
IMPL_CPU_OP(cmp, absy, d9, 4, 1);
IMPL_CPU_OP(cmp, indx, c1, 6, 0);
IMPL_CPU_OP(cmp, indy, d1, 5, 1);
IMPL_CPU_OP(cmp, ind0, d2, 5, 0);

IMPL_CPU_OP(cpx, imm, e0, 2, 0);
IMPL_CPU_OP(cpx, zp, e4, 3, 0);
IMPL_CPU_OP(cpx, abso, ec, 4, 0);

IMPL_CPU_OP(cpy, imm, c0, 2, 0);
IMPL_CPU_OP(cpy, zp, c4, 3, 0);
IMPL_CPU_OP(cpy, abso, cc, 4, 0);

IMPL_CPU_OP(deca, acc, 3a, 2, 0);
IMPL_CPU_OP(decm, zp, c6, 5, 0);
IMPL_CPU_OP(decm, zpx, d6, 6, 0);
IMPL_CPU_OP(decm, abso, ce, 6, 0);
IMPL_CPU_OP(decm, absx, de, 7, 0);

IMPL_CPU_IMP(dex, ca, 2);
IMPL_CPU_IMP(dey, 88, 2);

IMPL_CPU_OP(eor, imm, 49, 2, 0);
IMPL_CPU_OP(eor, zp, 45, 3, 0);
IMPL_CPU_OP(eor, zpx, 55, 4, 0);
IMPL_CPU_OP(eor, abso, 4d, 4, 0);
IMPL_CPU_OP(eor, absx, 5d, 4, 1);
IMPL_CPU_OP(eor, absy, 59, 4, 1);
IMPL_CPU_OP(eor, indx, 41, 6, 0);
IMPL_CPU_OP(eor, indy, 51, 5, 1);
IMPL_CPU_OP(eor, ind0, 52, 5, 0);

IMPL_CPU_OP(inca, acc, 1a, 2, 0);
IMPL_CPU_OP(incm, zp, e6, 5, 0);
IMPL_CPU_OP(incm, zpx, f6, 6, 0);
IMPL_CPU_OP(incm, abso, ee, 6, 0);
IMPL_CPU_OP(incm, absx, fe, 7, 0);

IMPL_CPU_IMP(inx, e8, 2);
IMPL_CPU_IMP(iny, c8, 2);

IMPL_CPU_OP(jmp, abso, 4c, 3, 0);
IMPL_CPU_OP(jmp, ind, 6c, 5, 0);
IMPL_CPU_OP(jmp, ainx, 7c, 6, 0);

IMPL_CPU_OP(jsr, abso, 20, 6, 0);

IMPL_CPU_OP(lda, imm, a9, 2, 0);
IMPL_CPU_OP(lda, zp, a5, 3, 0);
IMPL_CPU_OP(lda, zpx, b5, 4, 0);
IMPL_CPU_OP(lda, abso, ad, 4, 0);
IMPL_CPU_OP(lda, absx, bd, 4, 1);
IMPL_CPU_OP(lda, absy, b9, 4, 1);
IMPL_CPU_OP(lda, indx, a1, 6, 0);
IMPL_CPU_OP(lda, indy, b1, 5, 1);
IMPL_CPU_OP(lda, ind0, b2, 5, 0);

IMPL_CPU_OP(ldx, imm, a2, 2, 0);
IMPL_CPU_OP(ldx, zp, a6, 3, 0);
IMPL_CPU_OP(ldx, zpy, b6, 4, 0);
IMPL_CPU_OP(ldx, abso, ae, 4, 0);
IMPL_CPU_OP(ldx, absy, be, 4, 1);

IMPL_CPU_OP(ldy, imm, a0, 2, 0);
IMPL_CPU_OP(ldy, zp, a4, 3, 0);
IMPL_CPU_OP(ldy, zpx, b4, 4, 0);
IMPL_CPU_OP(ldy, abso, ac, 4, 0);
IMPL_CPU_OP(ldy, absx, bc, 4, 1);

IMPL_CPU_OP(lsra, acc, 4a, 2, 0);
IMPL_CPU_OP(lsrm, zp, 46, 5, 0);
IMPL_CPU_OP(lsrm, zpx, 56, 6, 0);
IMPL_CPU_OP(lsrm, abso, 4e, 6, 0);
IMPL_CPU_OP(lsrm, absx, 5e, 7, 0);

static void nop() { CPU.perf.clock_ticks += 2; }

IMPL_CPU_OP(ora, imm, 09, 2, 0);
IMPL_CPU_OP(ora, zp, 05, 3, 0);
IMPL_CPU_OP(ora, zpx, 15, 4, 0);
IMPL_CPU_OP(ora, abso, 0d, 4, 0);
IMPL_CPU_OP(ora, absx, 1d, 4, 1);
IMPL_CPU_OP(ora, absy, 19, 4, 1);
IMPL_CPU_OP(ora, indx, 01, 6, 0);
IMPL_CPU_OP(ora, indy, 11, 5, 1);
IMPL_CPU_OP(ora, ind0, 12, 5, 0);

IMPL_CPU_IMP(pha, 48, 2);
IMPL_CPU_IMP(php, 08, 2);
IMPL_CPU_IMP(phx, da, 2);
IMPL_CPU_IMP(phy, 5a, 2);
IMPL_CPU_IMP(pla, 68, 2);
IMPL_CPU_IMP(plp, 28, 2);
IMPL_CPU_IMP(plx, fa, 2);
IMPL_CPU_IMP(ply, 7a, 2);

#define IMPL_CPU_OP_RMB(BIT, CODE)            \
	static void rmb##BIT##_##CODE()           \
	{                                         \
		uint8_t addr = zp(0);                 \
		write6502(addr, read6502(addr) & ~(1 << (BIT))); \
		CPU.perf.clock_ticks += 5;            \
	}

IMPL_CPU_OP_RMB(0, 07);
IMPL_CPU_OP_RMB(1, 17);
IMPL_CPU_OP_RMB(2, 27);
IMPL_CPU_OP_RMB(3, 37);
IMPL_CPU_OP_RMB(4, 47);
IMPL_CPU_OP_RMB(5, 57);
IMPL_CPU_OP_RMB(6, 67);
IMPL_CPU_OP_RMB(7, 77);

IMPL_CPU_OP(rola, acc, 2a, 2, 0);
IMPL_CPU_OP(rolm, zp, 26, 5, 0);
IMPL_CPU_OP(rolm, zpx, 36, 6, 0);
IMPL_CPU_OP(rolm, abso, 2e, 6, 0);
IMPL_CPU_OP(rolm, absx, 3e, 7, 0);

IMPL_CPU_OP(rora, acc, 6a, 2, 0);
IMPL_CPU_OP(rorm, zp, 66, 5, 0);
IMPL_CPU_OP(rorm, zpx, 76, 6, 0);
IMPL_CPU_OP(rorm, abso, 6e, 6, 0);
IMPL_CPU_OP(rorm, absx, 7e, 7, 0);

IMPL_CPU_IMP(rti, 40, 6);
IMPL_CPU_IMP(rts, 60, 6);

IMPL_CPU_OP(sbcd, imm, e9, 2, 0);
IMPL_CPU_OP(sbcd, zp, e5, 3, 0);
IMPL_CPU_OP(sbcd, zpx, f5, 4, 0);
IMPL_CPU_OP(sbcd, abso, ed, 4, 0);
IMPL_CPU_OP(sbcd, absx, fd, 4, 1);
IMPL_CPU_OP(sbcd, absy, f9, 4, 1);
IMPL_CPU_OP(sbcd, indx, e1, 6, 0);
IMPL_CPU_OP(sbcd, indy, f1, 5, 1);
IMPL_CPU_OP(sbcd, ind0, f2, 5, 0);

IMPL_CPU_OP(sbcx, imm, e9, 2, 0);
IMPL_CPU_OP(sbcx, zp, e5, 3, 0);
IMPL_CPU_OP(sbcx, zpx, f5, 4, 0);
IMPL_CPU_OP(sbcx, abso, ed, 4, 0);
IMPL_CPU_OP(sbcx, absx, fd, 4, 1);
IMPL_CPU_OP(sbcx, absy, f9, 4, 1);
IMPL_CPU_OP(sbcx, indx, e1, 6, 0);
IMPL_CPU_OP(sbcx, indy, f1, 5, 1);
IMPL_CPU_OP(sbcx, ind0, f2, 5, 0);

IMPL_CPU_IMP(sec, 38, 2);
IMPL_CPU_IMP(sed, f8, 2);
IMPL_CPU_IMP(sei, 78, 2);

#define IMPL_CPU_OP_SMB(BIT, CODE)           \
	static void smb##BIT##_##CODE()          \
	{                                        \
		uint8_t addr = zp(0);                \
		write6502(addr, read6502(addr) | (1 << (BIT))); \
		CPU.perf.clock_ticks += 5;           \
	}

IMPL_CPU_OP_SMB(0, 87);
IMPL_CPU_OP_SMB(1, 97);
IMPL_CPU_OP_SMB(2, a7);
IMPL_CPU_OP_SMB(3, b7);
IMPL_CPU_OP_SMB(4, c7);
IMPL_CPU_OP_SMB(5, d7);
IMPL_CPU_OP_SMB(6, e7);
IMPL_CPU_OP_SMB(7, f7);

IMPL_CPU_OP(sta, zp, 85, 3, 0);
IMPL_CPU_OP(sta, zpx, 95, 4, 0);
IMPL_CPU_OP(sta, abso, 8d, 4, 0);
IMPL_CPU_OP(sta, absx, 9d, 5, 0);
IMPL_CPU_OP(sta, absy, 99, 5, 0);
IMPL_CPU_OP(sta, indx, 81, 6, 0);
IMPL_CPU_OP(sta, indy, 91, 6, 0);
IMPL_CPU_OP(sta, ind0, 92, 5, 0);

static void dbg_db() { DEBUGBreakToDebugger(); }

IMPL_CPU_OP(stx, zp, 86, 3, 0);
IMPL_CPU_OP(stx, zpy, 96, 4, 0);
IMPL_CPU_OP(stx, abso, 8e, 4, 0);

IMPL_CPU_OP(sty, zp, 84, 3, 0);
IMPL_CPU_OP(sty, zpx, 94, 4, 0);
IMPL_CPU_OP(sty, abso, 8c, 4, 0);

IMPL_CPU_OP(stz, zp, 64, 3, 0);
IMPL_CPU_OP(stz, zpx, 74, 4, 0);
IMPL_CPU_OP(stz, abso, 9c, 4, 0);
IMPL_CPU_OP(stz, absx, 9e, 5, 0);

IMPL_CPU_IMP(tax, aa, 2);
IMPL_CPU_IMP(tay, a8, 2);

IMPL_CPU_OP(trb, zp, 14, 5, 0);
IMPL_CPU_OP(trb, abso, 1c, 6, 0);

IMPL_CPU_OP(tsb, zp, 04, 5, 0);
IMPL_CPU_OP(tsb, abso, 0c, 6, 0);

IMPL_CPU_IMP(tsx, ba, 2);
IMPL_CPU_IMP(txa, 8a, 2);
IMPL_CPU_IMP(txs, 9a, 2);
IMPL_CPU_IMP(tya, 98, 2);
IMPL_CPU_IMP(wai, cb, 3);

#include "tables.h"

struct opcode_entry {
    void (*fn)(void);
    uint8_t opcode;
};

#define CPU_OP_ENTRY(OP, CODE) { CPU_OP(CODE, OP), 0x##CODE }

static struct opcode_entry cld_table[] = {
    CPU_OP_ENTRY(adcx, 69),
    CPU_OP_ENTRY(adcx, 65),
    CPU_OP_ENTRY(adcx, 75),
    CPU_OP_ENTRY(adcx, 6d),
    CPU_OP_ENTRY(adcx, 7d),
    CPU_OP_ENTRY(adcx, 79),
    CPU_OP_ENTRY(adcx, 61),
    CPU_OP_ENTRY(adcx, 71),
    CPU_OP_ENTRY(adcx, 72),

    CPU_OP_ENTRY(sbcx, e9),
    CPU_OP_ENTRY(sbcx, e5),
    CPU_OP_ENTRY(sbcx, f5),
    CPU_OP_ENTRY(sbcx, ed),
    CPU_OP_ENTRY(sbcx, fd),
    CPU_OP_ENTRY(sbcx, f9),
    CPU_OP_ENTRY(sbcx, e1),
    CPU_OP_ENTRY(sbcx, f1),
    CPU_OP_ENTRY(sbcx, f2),
};
static const int cld_table_size = sizeof(cld_table) / sizeof(cld_table[0]);

static struct opcode_entry sed_table[] = {
    CPU_OP_ENTRY(adcd, 69),
    CPU_OP_ENTRY(adcd, 65),
    CPU_OP_ENTRY(adcd, 75),
    CPU_OP_ENTRY(adcd, 6d),
    CPU_OP_ENTRY(adcd, 7d),
    CPU_OP_ENTRY(adcd, 79),
    CPU_OP_ENTRY(adcd, 61),
    CPU_OP_ENTRY(adcd, 71),
    CPU_OP_ENTRY(adcd, 72),

    CPU_OP_ENTRY(sbcd, e9),
    CPU_OP_ENTRY(sbcd, e5),
    CPU_OP_ENTRY(sbcd, f5),
    CPU_OP_ENTRY(sbcd, ed),
    CPU_OP_ENTRY(sbcd, fd),
    CPU_OP_ENTRY(sbcd, f9),
    CPU_OP_ENTRY(sbcd, e1),
    CPU_OP_ENTRY(sbcd, f1),
    CPU_OP_ENTRY(sbcd, f2),
};
static const int sed_table_size = sizeof(sed_table) / sizeof(sed_table[0]);

static void substitute_cld(void)
{
    for(int i=0; i<cld_table_size; ++i) {
        optable[cld_table[i].opcode] = cld_table[i].fn;
    }
    cleardecimal();
    CPU.perf.clock_ticks += 2;
}

static void substitute_sed(void)
{
    for(int i=0; i<sed_table_size; ++i) {
        optable[sed_table[i].opcode] = sed_table[i].fn;
    }
    cleardecimal();
    CPU.perf.clock_ticks += 2;
}

void reset6502() {
    CPU.pc = (uint16_t)read6502(0xFFFC) | ((uint16_t)read6502(0xFFFD) << 8);
    CPU.a = 0;
    CPU.x = 0;
    CPU.y = 0;
    CPU.sp = 0xFD;
    CPU.wai = 0;
    CPU.status |= FLAG_CONSTANT;
}

//helper variables
uint32_t clockgoal6502 = 0;

//#include "instructions.h"
//#include "65c02.h"
//#include "tables.h"

void nmi6502() {
    push16(CPU.pc);
	push8(CPU.status);
	CPU.status |= FLAG_INTERRUPT;
	CPU.pc  = (uint16_t)read6502(0xFFFA) | ((uint16_t)read6502(0xFFFB) << 8);
	CPU.wai = 0;
}

void irq6502() {
	push16(CPU.pc);
	push8(CPU.status & ~FLAG_BREAK);
	CPU.status |= FLAG_INTERRUPT;
	CPU.pc  = (uint16_t)read6502(0xFFFE) | ((uint16_t)read6502(0xFFFF) << 8);
	CPU.wai = 0;
}

void exec6502(uint32_t tickcount) {
	if (CPU.wai) {
		CPU.perf.clock_ticks += tickcount;
		clockgoal6502 = CPU.perf.clock_ticks;
		return;
    }

    clockgoal6502 += tickcount;
   
    while (CPU.perf.clock_ticks < clockgoal6502) {
		const uint8_t opcode = read8();
        (*optable[opcode])();
        CPU.perf.instructions++;
    }
}

void step6502() {
	if (CPU.wai) {
		++CPU.perf.clock_ticks;
		clockgoal6502 = CPU.perf.clock_ticks;
		return;
	}

    const uint8_t opcode = read8();
    (*optable[opcode])();
	clockgoal6502 = CPU.perf.clock_ticks;
    CPU.perf.instructions++;
}

//  Fixes from http://6502.org/tutorials/65c02opcodes.html
//
//  65C02 Cycle Count differences.
//        ADC/SBC work differently in decimal mode.
//        The wraparound fixes may not be required.
