
#pragma once

#include <stdint.h>
#include <stddef.h>

void pool_init(uint32_t* thread_base, size_t* thread_count, uint32_t* proc_base, size_t* proc_count);
void pool_get_kstack(size_t slot, uint32_t* bottom, uint32_t* top);


