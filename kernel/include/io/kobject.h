
#pragma once

#include <stdint.h>
#include <stddef.h>

typedef enum kobject_type {
    KOBJECT_TYPE_BLKDEV,
    KOBJECT_TYPE_VNODE,
    KOBJECT_TYPE_OPENFILE,
    KOBJECT_TYPE_PIPE,
    KOBJECT_TYPE_STREAM
} kobject_type_t;

typedef struct kobject kobject_t;

typedef struct kobject_ops  {
    void (*get)(kobject_t* obj);
    void (*put)(kobject_t* obj);
} kobject_ops_t;

// NOTE: All kobject-deriving structures must use kobject_t as their first
// member; must update refcount diligently; and destructors must only run
// when refcount = 0
struct kobject {
    kobject_type_t type;
    uint32_t refcount;
    uint32_t flags;
    void* self; // for use in ops; points to the structure containing this kobject
    kobject_ops_t ops;
    // possible TODO: ops field for "private" operations
};

static inline void kobject_init(kobject_t* obj, kobject_type_t type, void* self) {
    obj->type = type;
    obj->refcount = 1;
    obj->flags = 0;
    obj->ops = (kobject_ops_t){0};
    obj->self = self;
}