// Commander X16 Emulator
// Copyright (c) 2019 Michael Steil
// All rights reserved. License: 2-clause BSD

#ifndef _FAKE6502_H_
#define _FAKE6502_H_

#include <stdint.h>

struct cpu_performance
{
    uint32_t instructions;
    uint32_t clock_ticks;
};

struct cpu_state 
{
    uint16_t pc;
    uint8_t sp, a, x, y, status;

    uint8_t wai;
    uint8_t ea;

    struct cpu_performance perf;
};

extern struct cpu_state CPU;

extern void reset6502();
extern void step6502();
extern void exec6502(uint32_t tickcount);
extern void irq6502();

// These must be externally implemented for the CPU
extern uint8_t read6502(uint16_t address);
extern void write6502(uint16_t address, uint8_t value);

#endif
