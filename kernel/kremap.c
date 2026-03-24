
#include "kremap.h"
#include "paging.h"
#include "kmem.h"

void kernel_highhalf_remap() {
    if (!is_paging_ready()) {
        kdbg_puts("Cannot perform kernel high-half remap: paging not ready\r\n", 0x0C);
        return;
    }

    
}
