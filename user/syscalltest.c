#include <stdint.h>

// Thin syscall wrappers (still raw int $0x80, just not copy-pasted)

static void sys_write(const char* buf, uint32_t len);
static void sys_exit(uint32_t code);
static uint32_t sys_execve(const char* path);

static void test_execve(void);
static void test_fork(void);
static void test_write(void);

// Convenience: write a string literal without manually passing its length.
#define WRITE_LIT(s) sys_write((s), sizeof(s) - 1)

// POISON SENTINEL
// Lives in the flat binary's data region.  Initialized to 0 in the image.
// After exec replaces the address space, a fresh copy of the image is
// loaded from the embedded binary, so this is 0 again -- regardless of
// what the previous iteration wrote into it.
static volatile uint32_t g_sentinel = 0;

void _start(void) {
    //test_execve();
    //test_fork();
    test_write();

    for (;;) {} // unreachable safety net
}

static void test_write(void) {
    // Test that sys_write can write a string to the console without crashing.
    // This validates that the syscall handler, argument passing, and
    // copyin from user to kernel space are all basically working.
    WRITE_LIT("Hello from syscalltest!\r\n");
}

static void test_fork(void) {
    // fork(2) memory isolation test    
    // Validates that mm_clone_user_eager produces truly independent
    // physical backing - the child gets its own copy of every page,
    // so mutations in the child must NOT be visible in the parent.
    //  
    // Sets a static sentinel to a known value, forks, overwrites the sentinel
    // with a different value in the child. Parent should still see old value.
    // waitpid(child) used to know if child has completed its mutation.

    // Use the existing static sentinel.  Set it to a known-good value
    // before forking so both parent and child start from the same state.
    g_sentinel = 0x11111111;

    // fork(): returns child PID in parent, 0 in child
    int32_t pid;
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(pid)
        : "a"(2)
        : "memory", "cc"
    );

    if (pid < 0) {
        // fork failed
        WRITE_LIT("FAIL: fork returned error\r\n");
        sys_exit(1);
    }

    if (pid == 0) {
        // Child process path:
        // Mutate the sentinel.  If isolation works, this write goes to
        // the child's private copy of the page.  The parent's copy
        // must remain 0x11111111.
        g_sentinel = 0x22222222;

        if (g_sentinel == 0x22222222) {
            WRITE_LIT("fork: child sees 22222222\r\n");
        } else {
            WRITE_LIT("FAIL: child write did not stick\r\n");
        }

        sys_exit(42);
    }

    // Parent path:
    // Wait for the child to finish before checking our sentinel, so
    // we know the child's mutation has definitely happened.
    int32_t status = 0;
    int32_t waited;
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(waited)
        : "a"(7), "b"(pid), "c"(&status), "d"(0)
        : "memory", "cc"
    );

    // Check isolation: our sentinel must still be the original value.
    if (g_sentinel == 0x11111111) {
        WRITE_LIT("PASS: parent still sees 11111111 after child mutated\r\n");
    } else {
        WRITE_LIT("FAIL: parent sees mutated value -- fork did not isolate\r\n");
        sys_exit(1);
    }

    // Verify waitpid returned the child's PID (not an error).
    if (waited == pid) {
        WRITE_LIT("PASS: waitpid returned expected child pid\r\n");
    } else {
        WRITE_LIT("FAIL: waitpid did not return expected child pid\r\n");
        sys_exit(1);
    }

    WRITE_LIT("All fork isolation tests passed.\r\n");
    sys_exit(0);
}

static void test_execve(void) {
    // Test case 1: Address space replacement
    // On the very first run g_sentinel is 0 (its initial value in the
    // image).  Before self-exec we poison it to 0xDEADBEEF.  If exec
    // correctly builds a fresh mm, the reloaded image has g_sentinel == 0.
    // If exec cheated (just rewound EIP), g_sentinel will still show
    // 0xDEADBEEF
    if (g_sentinel == 0xDEADBEEF) {
        WRITE_LIT("FAIL: exec did not replace address space\r\n");
        sys_exit(1);
    }
    WRITE_LIT("PASS: image loaded fresh (sentinel == 0)\r\n");

    // Poison the sentinel now.  A correct exec will discard this page.
    g_sentinel = 0xDEADBEEF;

    // Test 2: Bad path safety
    //static void sys_write(const char* buf, uint32_t len)
    // The kernel should return -ENOENT and let us continue.
    sys_execve("/not/a/real/path");
    WRITE_LIT("PASS: bad execve returned without crashing\r\n");

    // Test 3: Self-exec should not return, and we should simply see
    // "image loaded fresh" once again. No new PID is allocated.
    WRITE_LIT("      about to self-exec...\r\n");
    sys_execve("/bin/syscalltest");

    // If we reach here, execve failed to replace the image.
    WRITE_LIT("FAIL: execve returned on valid path\r\n");
    sys_exit(1);
}

static void sys_write(const char* buf, uint32_t len) {
    __asm__ __volatile__(
        "int $0x80"
        :
        : "a"(4), "b"(1), "S"(buf), "d"(len)
        : "memory", "cc"
    );
}

static void sys_exit(uint32_t code) {
    __asm__ __volatile__(
        "int $0x80"
        :
        : "a"(1), "b"(code)
        : "memory", "cc"
    );
    __builtin_unreachable();
}

static uint32_t sys_execve(const char* path) {
    uint32_t ret;
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(11), "b"((uintptr_t)path), "c"(0), "d"(0)
        : "memory", "cc"
    );
    return ret;
}
