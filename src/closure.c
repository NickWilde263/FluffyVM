#include "fluffyvm_types.h"
#include <stddef.h>

#include <Block.h>
#include "closure.h"
#include "value.h"

#define UNIQUE_KEY(name) static uintptr_t name = (uintptr_t) &name

UNIQUE_KEY(closureTypeKey);

#define create_descriptor(name2, key, name, structure, ...) do { \
  foxgc_descriptor_pointer_t offsets[] = __VA_ARGS__; \
  vm->closureStaticData->name = foxgc_api_descriptor_new(vm->heap, fluffyvm_get_owner_key(), key, name2, sizeof(offsets) / sizeof(offsets[0]), offsets, sizeof(structure)); \
  if (vm->closureStaticData->name == NULL) \
    return false; \
} while (0)

#define free_descriptor(name) do { \
  if (vm->closureStaticData->name) \
    foxgc_api_descriptor_remove(vm->closureStaticData->name); \
} while(0)

bool closure_init(struct fluffyvm* vm) {
  vm->closureStaticData = malloc(sizeof(*vm->closureStaticData));
  if (!vm->closureStaticData)
    return false;

  create_descriptor("net.fluffyfox.fluffyvm.closure.Closure", closureTypeKey, desc_closure, struct fluffyvm_closure, {
    {"this", offsetof(struct fluffyvm_closure, gc_this)}, 
    {"prototype", offsetof(struct fluffyvm_closure, gc_prototype)},
    {"env", offsetof(struct fluffyvm_closure, gc_env)}
  });

  return true;
}

#define CLOSURE_OFFSET_THIS (0)
#define CLOSURE_OFFSET_PROTOTYPE (1)
#define CLOSURE_OFFSET_ENV (2)

void closure_cleanup(struct fluffyvm* vm) {
  if (vm->closureStaticData) {
    free_descriptor(desc_closure);
    free(vm->closureStaticData);
  }
}

static struct fluffyvm_closure* closure_new_common(struct fluffyvm* vm, foxgc_root_reference_t** rootRef, struct value env) {
  foxgc_object_t* obj = foxgc_api_new_object(vm->heap, NULL, fluffyvm_get_root(vm), rootRef, vm->closureStaticData->desc_closure, ^void (foxgc_object_t* obj) {
    struct fluffyvm_closure* this = foxgc_api_object_get_data(obj);
    if (this->finalizer)
      this->finalizer(this->udata);
  });
  if (obj == NULL) {
    fluffyvm_set_errmsg(vm, vm->staticStrings.outOfMemory);
    return NULL;
  }
  struct fluffyvm_closure* this = foxgc_api_object_get_data(obj);
  // Write to this->gc_this
  foxgc_api_write_field(obj, CLOSURE_OFFSET_THIS, obj);
  this->prototype = NULL;
  this->func = NULL;
  this->finalizer = NULL;
  this->luaCFunction = NULL;
  this->asValue = value_new_closure(vm, this);

  this->env = env;
  foxgc_api_write_field(obj, CLOSURE_OFFSET_ENV, value_get_object_ptr(env));

  return this;
  
  /* error:
  foxgc_api_remove_from_root2(vm->heap, fluffyvm_get_root(vm), *rootRef);
  *rootRef = NULL;
  return NULL; */
}

struct fluffyvm_closure* closure_new(struct fluffyvm* vm, foxgc_root_reference_t** rootRef, struct fluffyvm_prototype* prototype, struct value env) {
  struct fluffyvm_closure* this = closure_new_common(vm, rootRef, env);
  if (!this)
    return NULL;

  this->prototype = prototype;
  this->isNative = false;
  foxgc_api_write_field(this->gc_this, CLOSURE_OFFSET_PROTOTYPE, prototype->gc_this);
  return this;
}

struct fluffyvm_closure* closure_from_cfunction(struct fluffyvm* vm, foxgc_root_reference_t** rootRef, closure_cfunction_t func, void* udata, closure_udata_finalizer_t finalizer, struct value env) {
  struct fluffyvm_closure* this = closure_new_common(vm, rootRef, env);
  if (!this)
    return NULL;
  
  this->func = func;
  this->udata= udata;
  this->finalizer = finalizer;
  this->isNative = true;
  foxgc_api_write_field(this->gc_this, CLOSURE_OFFSET_PROTOTYPE, NULL);
  return this;
}






