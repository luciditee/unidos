
#include "io/blkdev.h"

static int blkdev_prepare_aligned_io(
    file_target_t* target,
    size_t bytesize,
    offset_t offset,
    kblkdev_t** out_blk,
    size_t* out_block_count,
    offset_t* out_effective_lba)
{
    if (!target || !out_blk || !out_block_count || !out_effective_lba)
        return -EINVAL;

    kblkdev_t* blk = &(target->data.blkdev);

    if (blk->block_size == 0)
        return -EINVAL;

    // Avoid 64-bit divide/mod runtime helpers in freestanding i386 builds.
    // Require power-of-two block sizes so offset translation can use mask/shift.
    if ((blk->block_size & (blk->block_size - 1)) != 0)
        return -EINVAL;

    uint32_t block_shift = 0;
    uint32_t bs = blk->block_size;
    while (bs > 1) {
        bs >>= 1;
        block_shift++;
    }

    offset_t block_mask = (offset_t)blk->block_size - 1;

    // v1: byte adapters only accept block-aligned offsets and lengths.
    if ((offset & block_mask) != 0)
        return -EINVAL;

    if ((bytesize % blk->block_size) != 0) {
        // TODO: Add bounce-buffer handling for partial tail reads/writes.
        return -EINVAL;
    }

    offset_t rel_lba = offset >> block_shift;
    size_t block_count = bytesize / blk->block_size;
    offset_t block_count_off = (offset_t)block_count;

    if (rel_lba > blk->span_blocks)
        return -EINVAL;

    if (block_count_off > (blk->span_blocks - rel_lba))
        return -EINVAL;

    if (blk->base_lba > ((offset_t)-1) - rel_lba)
        return -EOVERFLOW;

    *out_blk = blk;
    *out_block_count = block_count;
    *out_effective_lba = blk->base_lba + rel_lba;
    return 0;
}

// Block device wrapper function which calls the underlying block device's write_blocks implementation,
// but accepts a byte buffer and size rather than a block buffer and block count, used mainly as a
// passthrough for file targets referencing block devices, since the write syscall has no idea about devices,
// but it understands file descriptors.
//
// v1: no internal buffering, caller must ensure byte buffer is appropriately sized to the device's block size.
// If it is not, returns -EINVAL.
// v2 (tbd): internal buffering to allow arbitrary buffer size writes.
ssize_t kblkdev_write_bytes(file_target_t* target, const void* bytebuffer, size_t bytesize, offset_t offset, void* _context) {
    (void)_context; // Unused for this function.

    if (!target)
        return -EBADFD;

    if (!bytebuffer && bytesize != 0)
        return -EFAULT;

    kblkdev_t* blk = NULL;
    size_t block_count = 0;
    offset_t effective_lba = 0;
    int prep = blkdev_prepare_aligned_io(target, bytesize, offset, &blk, &block_count, &effective_lba);
    if (prep != 0)
        return prep;

    if (block_count == 0)
        return 0;

    if (!blk->ops.write)
        return -ENOSYS;

    return blk->ops.write(blk, bytebuffer, block_count, effective_lba);
}

// Block device function which calls the underlying block device's read_blocks implementation,
// but accepts a byte buffer and size rather than a block buffer and block count, used mainly as a
// passthrough for file targets referencing block devices, since the read syscall has no idea about
// devices, but it understands file descriptors.
//
// Caller is responsible for ensuring target buffer is at least as large as the requested size. 
//
// In the event of a partial read of the final block, it is possible that the first N whole blocks
// were successfully read, and only the final partial block read failed. In this case, the target
// buffer will contain the successfully read whole blocks, and the return code will contain the error
// of the final partial block read.
// 
// If a partial read occurs with no specific error code, the return value will be the number of bytes
// successfully read up to the point of failure, which will be equal to or greater than zero.
//
// v1: Unlike the write function, this function reads whole blocks until the final (partial)
// block, which will be read up to the requested byte size.
ssize_t kblkdev_read_bytes(file_target_t* target, void* bytebuffer, size_t bytesize, offset_t offset, void* _context) {
    (void)_context; // Unused for this function.

    if (!target)
        return -EBADFD;

    if (!bytebuffer && bytesize != 0)
        return -EFAULT;

    kblkdev_t* blk = NULL;
    size_t block_count = 0;
    offset_t effective_lba = 0;
    int prep = blkdev_prepare_aligned_io(target, bytesize, offset, &blk, &block_count, &effective_lba);
    if (prep != 0)
        return prep;

    if (block_count == 0)
        return 0;

    if (!blk->ops.read)
        return -ENOSYS;

    return blk->ops.read(blk, bytebuffer, block_count, effective_lba);
}

void blkdev_populate(kblkdev_t* blk, uint32_t block_size, blkdev_target_io_ops_t ops, offset_t base_lba, offset_t span_blocks, void* driver_context) {
    if (!blk)
        return;

    *blk = (kblkdev_t){0};
    blk->block_size = block_size;
    blk->ops = ops;
    blk->base_lba = base_lba;
    blk->span_blocks = span_blocks;
    blk->driver_context = driver_context;
}

kblkdev_t blkdev_create(uint32_t block_size, blkdev_target_io_ops_t ops, offset_t base_lba, offset_t span_blocks, void* driver_context) {
    kblkdev_t blk = (kblkdev_t){0};
    blkdev_populate(&blk, block_size, ops, base_lba, span_blocks, driver_context);
    return blk;
}

void ft_blkdev_populate(file_target_t* target, kblkdev_t blk) {
    if (!target)
        return;

    *target = (file_target_t){0};
    target->kobj.type = KOBJECT_TYPE_BLKDEV;
    target->io_ops.read = kblkdev_read_bytes;
    target->io_ops.write = kblkdev_write_bytes;
    target->data.blkdev = blk;
}
