
#include "io/file.h"
#include "io/fdpool.h"
#include "panic.h"
#include "../include/errno.h"

void file_target_get(file_target_t* target) {
	if (!target) return;

	target->kobj.refcount++;
	if (target->kobj.ops.get)
		target->kobj.ops.get(&target->kobj);
}

void file_target_put(file_target_t* target) {
	if (!target) return;

	if (target->kobj.refcount == 0)
		panic("file_target_put: refcount underflow", NULL);

	if (target->kobj.ops.put)
		target->kobj.ops.put(&target->kobj);

	target->kobj.refcount--;
	if (target->kobj.refcount == 0)
		fdpool_free_ftarget(target);
}

void open_file_get(open_file_t* of) {
	if (!of) return;

	of->kobj.refcount++;
	if (of->kobj.ops.get)
		of->kobj.ops.get(&of->kobj);
}

void open_file_put(open_file_t* of) {
	if (!of) return;

	if (of->kobj.refcount == 0)
		panic("open_file_put: refcount underflow", NULL);

	if (of->kobj.ops.put)
		of->kobj.ops.put(&of->kobj);

	of->kobj.refcount--;
	if (of->kobj.refcount == 0) {
		file_target_put(of->target);
		fdpool_free_openfile(of);
	}
}

ssize_t open_file_read(open_file_t* of, void* buffer, size_t length, uint32_t offset, void* context) {
	if (!of || !of->target)
		return -EBADFD;

	if (!of->target->io_ops.read)
		return -ENOSYS;

	return of->target->io_ops.read(of->target, buffer, length, offset, context);
}

ssize_t open_file_write(open_file_t* of, const void* buffer, size_t length, uint32_t offset, void* context, errno_t* err_out) {
	if (!of || !of->target) {
		if (err_out) *err_out = EBADFD;
		return 0;
    }

	if (!of->target->io_ops.write) {
		if (err_out) *err_out = ENOSYS;
		return 0;
    }

    ssize_t res = of->target->io_ops.write(of->target, buffer, length, offset, context);

	if (res < 0) {
		if (err_out) *err_out = (errno_t)(-res);
		return 0;
	}

	if (err_out) *err_out = ESUCCESS;
	return res;
}

