
#pragma once

#include "isr.h"

void panic(const char* msg, trap_frame_t* tf);
