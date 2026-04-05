#include <stdint.h>

void _start(void) {
    static const char msg[] = "user: write message test\r\n";
    static const char unreachable[] = "UNREACHABLE after exec\r\n";
    uint32_t ret;

    /* write(1, msg, sizeof(msg) - 1) */
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret) // gets replaced later, unused for now
        : "a"(4), "b"(1), "S"(msg), "d"((uint32_t)(sizeof(msg) - 1))
        : "memory", "cc"
    );

    for(volatile int i = 0; i < 1000000; i++); // burn cycles temporarily

    /* execve("/not/a/real/path", NULL, NULL) */
    // confirmed that this returns ENOENT
    /*__asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(11), "b"((uintptr_t)"/not/a/real/path"), "c"(0), "d"(0) // deliberately -ENOENT
        : "memory", "cc"
    );*/ 

    /* execve("/bin/syscalltest", NULL, NULL) */
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(11), "b"((uintptr_t)"/bin/syscalltest"), "c"(0), "d"(0) // should succeed and not return
        : "memory", "cc"
    );

    /* exit(ret) */
    __asm__ __volatile__(
        "int $0x80"
        :
        : "a"(1), "b"(-ret)
        : "memory", "cc"
    );

    /* write(1, unreachable, sizeof(unreachable) - 1) */
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(4), "b"(1), "S"(unreachable), "d"((uint32_t)(sizeof(unreachable) - 1)) // should not be printed
        : "memory", "cc"
    );

    
    /* write(1, NULL, 1) */
    /*__asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(4), "b"(1), "S"((const char*)0), "d"(1) // deliberately -EFAULT 
        : "memory", "cc"
    );*/

    /* exit(ret) */
    __asm__ __volatile__(
        "int $0x80"
        :
        : "a"(1), "b"(-ret)
        : "memory", "cc"
    );

    for (;;) {} // jmp $
}