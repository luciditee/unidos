
#pragma once

#include "file.h"

void blkdev_populate(kblkdev_t* blk, uint32_t block_size, blkdev_target_io_ops_t ops, offset_t base_lba, offset_t span_blocks, void* driver_context);
void ft_blkdev_populate(file_target_t* target, kblkdev_t blk);
kblkdev_t blkdev_create(uint32_t block_size, blkdev_target_io_ops_t ops, offset_t base_lba, offset_t span_blocks, void* driver_context);
ssize_t kblkdev_read_bytes(file_target_t* target, void* bytebuffer, size_t bytesize, offset_t offset, void* _context);
ssize_t kblkdev_write_bytes(file_target_t* target, const void* bytebuffer, size_t bytesize, offset_t offset, void* _context);