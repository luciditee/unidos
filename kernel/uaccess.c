
#include "include/uaccess.h"
#include "include/kmem.h"
#include "include/paging.h"
#include "include/errno.h"

static int range_check(uint32_t start, size_t n, int want_user) {
    if (n == 0) return 1;

    uint32_t end = start + (uint32_t)(n - 1);
    if (end < start) return 0; // overflow

    if (want_user) {
        if (!is_user_vaddr(start) || !is_user_vaddr(end)) return 0;
    } else {
        if (!is_kernel_vaddr(start) || !is_kernel_vaddr(end)) return 0;
    }
    return 1;
}

errno_t copyout(const void* ksrc, void* udest, size_t n) {
    // Copy a buffer of size n from kernel space (ksrc) to user space (udest).
    // Returns 0 on success, or an error code on failure.
    if (get_current_cpl() != CPL_KERNEL)
        return -EPERM; // copyout should only be called from kernel context

    if (!ksrc || !udest)
        return -EFAULT;

    if (!n) return ESUCCESS; // nothing to copy, trivially succeed

    // Validate pointer ranges
    if (!range_check((uint32_t)ksrc, n, 0)) return -EFAULT;
    if (!range_check((uint32_t)udest, n, 1)) return -EFAULT;
        
    // Validate that all destination user pages are mapped, user, and writable.
    uint32_t d = (uint32_t)udest;
    uint32_t first_page = d & ~0xFFFu;
    uint32_t last_page  = (d + (uint32_t)(n - 1)) & ~0xFFFu;

    // Page-wise check that all destination pages are valid for userspace writing
    // (bytewise would be much too slow)
    for (uint32_t va = first_page;; va += 0x1000u) {
        paging_query_result_t res;
        paging_status_t status = paging_query_page(va, &res);
        if (status != PAGING_OK || !res.mapped) return -EFAULT; // not mapped
        if ((res.flags & PG_USER) == 0) return -EFAULT; // not user-accessible
        if ((res.flags & PG_RW) == 0) return -EFAULT; // not writable

        if (va == last_page) break;
    }

    // Perform copy
    // TODO: Not fault safe if mappings can change between validation and copy
    // To be truly safe, we would need to implement page pinning or a preemption
    // mechanism to prevent mappings from changing (could use PT OS-defined attrib
    // bits for this). For now, kmemcpy will suffice.
    kmemcpy(udest, ksrc, n);
    
    return ESUCCESS;
}

errno_t copyin(const void* usrc, void* kdest, size_t n) {
    // Copy a buffer of size n from user space (usrc) to kernel space (kdest).
    // Returns 0 on success, or an error code on failure.
    if (get_current_cpl() != CPL_KERNEL)
        return -EPERM; // copyin should only be called from kernel context

    if (!usrc || !kdest)
        return -EFAULT;

    if (!n) return ESUCCESS; // nothing to copy, trivially succeed

    // Validate pointer ranges
    if (!range_check((uint32_t)usrc, n, 1)) return -EFAULT;
    if (!range_check((uint32_t)kdest, n, 0)) return -EFAULT;
        
    // Validate that all source user pages are mapped and user-accessible.
    uint32_t s = (uint32_t)usrc;
    uint32_t first_page = s & ~0xFFFu;
    uint32_t last_page  = (s + (uint32_t)(n - 1)) & ~0xFFFu;

    // Page-wise check that all source pages are valid for userspace reading
    // (bytewise would be much too slow)
    for (uint32_t va = first_page;; va += 0x1000u) {
        paging_query_result_t res;
        paging_status_t status = paging_query_page(va, &res);
        if (status != PAGING_OK || !res.mapped) return -EFAULT; // not mapped
        if ((res.flags & PG_USER) == 0) return -EFAULT; // not user-accessible

        if (va == last_page) break;
    }

    // Perform copy
    // TODO: See above TODO in copyout
    kmemcpy(kdest, usrc, n);
    
    return ESUCCESS;
}