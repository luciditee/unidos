
#pragma once

#include "isr.h"

void exception_init();

// General protection fault handler
void exception_gpf(trap_frame_t* tf);

// Page fault handler
void exception_pf(trap_frame_t* tf);

// Invalid opcode handler
void exception_ud(trap_frame_t* tf);

// Stack segment fault handler
void exception_ss(trap_frame_t* tf);

// Double fault handler
void exception_np(trap_frame_t* tf);

