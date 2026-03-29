
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "errno.h"

errno_t copyout(const void* ksrc, void* udest, size_t n);
errno_t copyin(const void* usrc, void* kdest, size_t n);
