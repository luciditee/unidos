
#pragma once

#include "kobject.h"
#include "file.h"
#include "fdpool.h"

typedef struct console {
    // This'll be expanded in the future as we need more metadata attached to TTYs
    file_target_t* target;
    open_file_t* of;
} console_t;

void console_init(void);
open_file_t* console_get_stdio(void);
