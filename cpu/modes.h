/*

						Extracted from original single fake6502.c file

*/

#ifndef MODES_H
#define MODES_H

#include "support.h"

//
//          65C02 changes.
//
//          ind         absolute indirect
//
//                      A 6502 has a bug whereby if you jmp ($12FF) it reads the address from
//                      $12FF and $1200. This has been fixed in the 65C02. 
//

static inline uint8_t tick_penalty(uint16_t addr1, uint16_t addr2)
{
    return ((addr1 ^ addr2) & 0xff00) != 0;
}

static inline uint8_t imp(const uint8_t penalty) { //implied
    return 0;
}

static inline uint16_t acc(const uint8_t penalty) { //accumulator
    return 0;
}

static inline uint16_t imm(const uint8_t penalty) { //immediate
    const uint16_t addr = CPU.pc;
    ++CPU.pc;
    return addr;
}

static inline uint16_t zp(const uint8_t penalty) { //zero-page
    return (uint16_t)read8();
}

static inline uint16_t zpx(const uint8_t penalty) { //zero-page,X
    return (uint8_t)(read8() + CPU.x);
}

static inline uint16_t zpy(const uint8_t penalty) { //zero-page,Y
    return (uint8_t)(read8() + CPU.y);
}

static inline uint16_t abso(const uint8_t penalty) { //absolute
    return read16();
}

static inline uint16_t absx(const uint8_t penalty) { //absolute,X
    const uint16_t start = read16();
    const uint16_t end = start + CPU.x;

    CPU.perf.clock_ticks += penalty & tick_penalty(start, end);

    return end;
}

static inline uint16_t absy(const uint8_t penalty) { //absolute,Y
    const uint16_t start = read16();
    const uint16_t end = start + CPU.y;

    CPU.perf.clock_ticks += penalty & tick_penalty(start, end);

    return end;
}

static inline uint16_t ind(const uint8_t penalty) { //indirect
    const uint16_t addr = read16();
    return (uint16_t)read6502(addr) | ((uint16_t)read6502(addr+1) << 8);
}

static inline uint16_t indx(const uint8_t penalty) { // (indirect,X)
    const uint16_t addr = read8() + CPU.x;
    return (uint16_t)read6502(addr & 0xff) | ((uint16_t)read6502((addr+1) & 0xff) << 8);
}

static inline uint16_t indy(const uint8_t penalty) { // (indirect),Y
    const uint16_t addr = read8();
    const uint16_t addr2 = (uint16_t)read6502(addr & 0xff) | ((uint16_t)read6502((addr+1) & 0xff) << 8);
    const uint16_t addr3 = addr2 + CPU.y;

    CPU.perf.clock_ticks += penalty & tick_penalty(addr2, addr3);

    return addr3;
}

static inline uint16_t ind0(const uint8_t penalty) { // indirect from zero page
    const uint16_t addr = read8();
    const uint16_t addr2 = (uint16_t)read6502(addr) | ((uint16_t)read6502(addr+1) << 8);

    return addr2;
}

static inline uint16_t rel(const uint8_t penalty) {
	const int8_t value = read8();
	return CPU.pc + value;
}

static inline uint16_t ainx(const uint8_t penalty) {
    const uint16_t addr = read16();
    const uint16_t addr2 = addr + CPU.x;

    return (uint16_t)read6502(addr2) | ((uint16_t)read6502(addr2+1) << 8);
}

static inline uint16_t zprel(const uint8_t penalty) { // zero-page plus relative for branch ops (8-bit immediate value, sign-extended)
    uint16_t value = (uint16_t)read8();
    CPU.ea = CPU.pc + (int8_t)read8();

    CPU.perf.clock_ticks += penalty & tick_penalty(CPU.pc, CPU.ea);

    return value;
}

#endif // MODES_H