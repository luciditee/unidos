#include <stdint.h>

void _start(void) {
    static const char msg[] = "user: write/exit test\r\n";
    uint32_t ret;

    /* write(1, msg, sizeof(msg) - 1) */
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(4), "b"(1), "S"(msg), "d"((uint32_t)(sizeof(msg) - 1))
        : "memory", "cc"
    );

    /* write(1, NULL, 1) */
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(4), "b"(1), "S"((const char*)0), "d"(1) // deliberately -EFAULT 
        : "memory", "cc"
    );

    /* exit(ret) */
    __asm__ __volatile__(
        "int $0x80"
        :
        : "a"(1), "b"(ret)
        : "memory", "cc"
    );

    for (;;) {}
}