// Commander X16 Emulator
// Copyright (c) 2019 Michael Steil
// All rights reserved. License: 2-clause BSD

#ifndef _PS2_H_
#define _PS2_H_

#include <stdint.h>

#define PS2_DATA_MASK 1
#define PS2_CLK_MASK 2

typedef struct {
	uint8_t out;
	uint8_t in;
} ps2_port_t;

extern ps2_port_t ps2_port[2];

bool ps2_buffer_can_fit(const int i, const int n);
void ps2_buffer_add(const int i, const uint8_t byte);
void ps2_step(const int i);

// fake mouse
void    mouse_button_down(const int num);
void    mouse_button_up(const int num);
void    mouse_move(const int x, const int y);
uint8_t mouse_read(const uint8_t reg);

#endif
