#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <Block.h>
#include <math.h>
#include <inttypes.h>

#include "api_layer/types.h"
#include "coroutine.h"
#include "util/functional/functional.h"
#include "value.h"
#include "hashing.h"
#include "fluffyvm.h"
#include "foxgc.h"
#include "fluffyvm_types.h"
#include "config.h"
#include "hashtable.h"
#include "closure.h"
#include "string_cache.h"

#define UNIQUE_KEY(name) static uintptr_t name = (uintptr_t) &name

UNIQUE_KEY(stringDataArrayKey);
UNIQUE_KEY(userdataTypeKey);
UNIQUE_KEY(garbageCollectableUserdataTypeKey);

bool value_init(struct fluffyvm* vm) {
  return true;
}

void value_cleanup(struct fluffyvm* vm) {
}

static atomic_int moduleID = 1;
int value_get_module_id() {
  return atomic_fetch_add(&moduleID, 1);
}

static void commonStringInit(struct value_string* str, foxgc_object_t* strObj) {
  str->hashCode = 0;
  str->str = strObj;
}

struct value value_string_allocator(struct fluffyvm* vm, const char* str, size_t len, foxgc_root_reference_t** rootRef, void* udata, runnable_t finalizer) {
  struct value_string* strStruct = malloc(sizeof(*strStruct));
  if (!strStruct) {
    if (vm->staticStrings.outOfMemoryRootRef)
      fluffyvm_set_errmsg(vm, vm->staticStrings.outOfMemory);
    return value_not_present;
  }

  foxgc_object_t* strObj = foxgc_api_new_data_array(vm->heap, fluffyvm_get_owner_key(), stringDataArrayKey, NULL, fluffyvm_get_root(vm), rootRef, 1, len + 1, Block_copy(^void (foxgc_object_t* obj) {
    if (finalizer) {
      finalizer();
      Block_release(finalizer);
    }
    free(strStruct);
  }));

  if (!strObj) {
    if (vm->staticStrings.outOfMemoryRootRef)
      fluffyvm_set_errmsg(vm, vm->staticStrings.outOfMemory);
    free(strStruct);
    return value_not_present;
  }

  struct value value = {
    .data.str = strStruct,
    .type = FLUFFYVM_TVALUE_STRING
  };

  commonStringInit(strStruct, strObj);
   
  // memcpy because the string could have embedded
  // null to mimic lua for the string
  memcpy(foxgc_api_object_get_data(strObj), str, len);
  
  // Null terminator
  ((char*) foxgc_api_object_get_data(strStruct->str))[len] = '\0';
  return value;
}

struct value value_new_string2(struct fluffyvm* vm, const char* str, size_t len, foxgc_root_reference_t** rootRef) {
  return value_string_allocator(vm, str, len, rootRef, NULL, NULL);
}

struct value value_new_string(struct fluffyvm* vm, const char* cstr, foxgc_root_reference_t** rootRef) {
  return value_new_string2(vm, cstr, strlen(cstr), rootRef);
}

struct value value_new_string2_constant(struct fluffyvm* vm, const char* str, size_t len, foxgc_root_reference_t** rootRef) {
  if (!vm->stringCache)
    return value_string_allocator(vm, str, len, rootRef, NULL, NULL);
  return string_cache_create_string(vm, vm->stringCache, str, len, rootRef);
}

struct value value_new_string_constant(struct fluffyvm* vm, const char* cstr, foxgc_root_reference_t** rootRef) {
  return value_new_string2_constant(vm, cstr, strlen(cstr), rootRef);
}

struct value value_new_long(struct fluffyvm* vm, fluffyvm_integer integer) {
  struct value value = {
   .data.longNum = integer,
   .type = FLUFFYVM_TVALUE_LONG
  };

  return value;
}

struct value value_new_bool(struct fluffyvm* vm, bool boolean) {
  struct value value = {
   .data.boolean = boolean,
   .type = FLUFFYVM_TVALUE_BOOL
  };

  return value;
}

struct value value_new_closure(struct fluffyvm* vm, struct fluffyvm_closure* closure) {
  struct value value = {
   .data.closure = closure,
   .type = FLUFFYVM_TVALUE_CLOSURE
  };

  return value;
}

struct value value_new_table(struct fluffyvm* vm, double loadFactor, int initialCapacity, foxgc_root_reference_t** rootRef) {
  struct hashtable* hashtable = hashtable_new(vm, loadFactor, initialCapacity, fluffyvm_get_root(vm), rootRef);

  if (!hashtable) {
    if (vm->staticStrings.outOfMemoryRootRef)
      fluffyvm_set_errmsg(vm, vm->staticStrings.outOfMemory);
    return value_not_present;
  }

  struct value value = {
    .data.table = hashtable->gc_this,
    .type = FLUFFYVM_TVALUE_TABLE
  };

  return value;
}

struct value value_new_full_userdata(struct fluffyvm* vm, int moduleID, int typeID, size_t size, foxgc_root_reference_t** rootRef, value_userdata_finalizer finalizer) {
  struct value_userdata* userdata = malloc(sizeof(*userdata)); 
  if (!userdata) {
    if (vm->staticStrings.outOfMemoryRootRef)
      fluffyvm_set_errmsg(vm, vm->staticStrings.outOfMemory);
    return value_not_present;
  }
  
  foxgc_object_t* userdataObj = foxgc_api_new_object_opaque(vm->heap, fluffyvm_get_owner_key(), userdataTypeKey, NULL, fluffyvm_get_root(vm), rootRef, size, Block_copy(^void (foxgc_object_t* obj) {
    if (finalizer) {
      finalizer();
      Block_release(finalizer);
    }
    free(userdata);
  }));
  userdata->dataObj = userdataObj;
  userdata->data = foxgc_api_object_get_data(userdataObj);
  userdata->moduleID = moduleID;
  userdata->typeID = typeID;
  userdata->isFull = true;

  if (!userdataObj) {
    if (vm->staticStrings.outOfMemoryRootRef)
      fluffyvm_set_errmsg(vm, vm->staticStrings.outOfMemory);
    free(userdata);
    return value_not_present;
  }

  struct value value = {
    .data.userdata = userdata,
    .type = FLUFFYVM_TVALUE_FULL_USERDATA
  };

  return value; 
}

struct value value_new_light_userdata(struct fluffyvm* vm, int moduleID, int typeID, void* data, foxgc_root_reference_t** rootRef, value_userdata_finalizer finalizer) {
  struct value tmp = value_new_full_userdata(vm, moduleID, typeID, sizeof(void*), rootRef, finalizer);
  tmp.data.userdata->isFull = false;
  *((void**) tmp.data.userdata->data) = data;

  // Im hating this
  *((value_types_t*) &tmp.type) = FLUFFYVM_TVALUE_LIGHT_USERDATA;
  return tmp;
}

struct value value_new_garbage_collectable_userdata(struct fluffyvm* vm, int moduleID, int typeID, foxgc_object_t* object, foxgc_root_reference_t** rootRef) {
  struct value_userdata* userdata = malloc(sizeof(*userdata)); 
  foxgc_object_t* userdataObj = foxgc_api_new_array(vm->heap, fluffyvm_get_owner_key(), garbageCollectableUserdataTypeKey, NULL, fluffyvm_get_root(vm), rootRef, 1, Block_copy(^void (foxgc_object_t* obj) {
    free(userdata);
  }));
  foxgc_api_write_array(userdataObj, 0, object);
  userdata->dataObj = userdataObj;
  userdata->userGarbageCollectableData = object;
  userdata->moduleID = moduleID;
  userdata->typeID = typeID;
  userdata->isFull = false;

  if (!userdataObj) {
    if (vm->staticStrings.outOfMemoryRootRef)
      fluffyvm_set_errmsg(vm, vm->staticStrings.outOfMemory);
    free(userdata);
    return value_not_present;
  }

  struct value value = {
    .data.userdata = userdata,
    .type = FLUFFYVM_TVALUE_GARBAGE_COLLECTABLE_USERDATA
  };

  return value; 
}

foxgc_object_t* value_get_object_ptr(struct value value) {
  switch (value.type) {
    case FLUFFYVM_TVALUE_STRING:
      return value.data.str->str;
    case FLUFFYVM_TVALUE_TABLE:
      return value.data.table;
    case FLUFFYVM_TVALUE_CLOSURE:
      return value.data.closure->gc_this;
    case FLUFFYVM_TVALUE_FULL_USERDATA:
    case FLUFFYVM_TVALUE_LIGHT_USERDATA:
    case FLUFFYVM_TVALUE_GARBAGE_COLLECTABLE_USERDATA:
      return value.data.userdata->dataObj;
    case FLUFFYVM_TVALUE_COROUTINE:
      return value.data.coroutine->gc_this;

    case FLUFFYVM_TVALUE_LONG:
    case FLUFFYVM_TVALUE_DOUBLE:
    case FLUFFYVM_TVALUE_BOOL:
    case FLUFFYVM_TVALUE_NIL:
      return NULL;
    
    case FLUFFYVM_TVALUE_LAST:    
    case FLUFFYVM_TVALUE_NOT_PRESENT:
      abort(); /* Can't happen */
  }
}

bool value_hash_code(struct value value, uint64_t* hashCode) {
  // Compute hash code
  uint64_t hash = 0;
  switch (value.type) {
    case FLUFFYVM_TVALUE_STRING:
      if (value.data.str->hashCode != 0) {
        hash = value.data.str->hashCode;
        break;
      }
      
      void* data = foxgc_api_object_get_data(value.data.str->str);
      size_t len = value_get_len(value);

      hash = hashing_hash_default(data, len);
      value.data.str->hashCode = hash;
      break;
     
    case FLUFFYVM_TVALUE_LONG:
      hash = hashing_hash_default((void*) (&value.data.longNum), sizeof(fluffyvm_integer));
      break;
    case FLUFFYVM_TVALUE_DOUBLE:
      hash = hashing_hash_default((void*) (&value.data.doubleData), sizeof(fluffyvm_number));
      break;
    case FLUFFYVM_TVALUE_TABLE:
      hash = hashing_hash_default((void*) (&value.data.table), sizeof(foxgc_object_t*));
      break;
    case FLUFFYVM_TVALUE_CLOSURE:
      hash = hashing_hash_default((void*) (&value.data.closure->gc_this), sizeof(foxgc_object_t*));
      break;
    case FLUFFYVM_TVALUE_FULL_USERDATA:
    case FLUFFYVM_TVALUE_LIGHT_USERDATA:
    case FLUFFYVM_TVALUE_GARBAGE_COLLECTABLE_USERDATA:
      hash = hashing_hash_default((void*) (&value.data.userdata->data), sizeof(void*));
      break;
    case FLUFFYVM_TVALUE_BOOL:
      hash = hashing_hash_default((void*) (&value.data.boolean), sizeof(bool));
      break;
    case FLUFFYVM_TVALUE_COROUTINE:
      hash = hashing_hash_default((void*) (&value.data.coroutine->gc_this), sizeof(foxgc_object_t*));
      break;
    
    case FLUFFYVM_TVALUE_NIL:
      hash = 0;
      break;

    case FLUFFYVM_TVALUE_LAST:    
    case FLUFFYVM_TVALUE_NOT_PRESENT:
      abort(); /* Can't happen */
  }
  
  *hashCode = hash;
  return true;
}

struct value value_new_double(struct fluffyvm* vm, fluffyvm_number number) {
  struct value value = {
    .data.doubleData = number,
    .type = FLUFFYVM_TVALUE_DOUBLE
  };

  return value;
}

struct value value_new_coroutine(struct fluffyvm* vm, struct fluffyvm_closure* closure, foxgc_root_reference_t** rootRef) {
  struct value value = {
    .data.coroutine = coroutine_new(vm, rootRef, closure),
    .type = FLUFFYVM_TVALUE_COROUTINE
  };

  return value;
}
struct value value_new_coroutine2(struct fluffyvm* vm, struct fluffyvm_coroutine* co) {
  struct value value = {
    .data.coroutine = co,
    .type = FLUFFYVM_TVALUE_COROUTINE
  };

  return value;
}

static void checkPresent(struct value* value) {
  if (value->type == FLUFFYVM_TVALUE_NOT_PRESENT ||
      value->type >= FLUFFYVM_TVALUE_LAST)
    abort();
}

const char* value_get_string(struct value value) {
  checkPresent(&value);
  if (value.type != FLUFFYVM_TVALUE_STRING) {
    printf("%d\n", value.type);
    return NULL;
  }

  return (const char*) foxgc_api_object_get_data(value.data.str->str);
}

size_t value_get_len(struct value value) {
  checkPresent(&value); 

  switch (value.type) {
    case FLUFFYVM_TVALUE_STRING:
      return foxgc_api_get_array_length(value.data.str->str) - 1;
    case FLUFFYVM_TVALUE_TABLE:
      return ((struct hashtable*) foxgc_api_object_get_data(value.data.table))->usage;
    default:
      return -1;
  }
}

struct value value_tostring(struct fluffyvm* vm, struct value value, foxgc_root_reference_t** rootRef) {
  checkPresent(&value);

  size_t bufLen = 0;
  char* buffer = NULL;
  
  // Get the len
  switch (value.type) {
    case FLUFFYVM_TVALUE_STRING:
      foxgc_api_root_add(vm->heap, value.data.str->str, fluffyvm_get_root(vm), rootRef);
      return value;
    
    case FLUFFYVM_TVALUE_LONG:
      bufLen = snprintf(NULL, 0, "%ld", value.data.longNum);
      break;

    case FLUFFYVM_TVALUE_DOUBLE:
      bufLen = snprintf(NULL, 0, "%lf", value.data.doubleData);
      break;

    case FLUFFYVM_TVALUE_BOOL:
      if (value.data.boolean == true) {
        foxgc_api_root_add(vm->heap, value_get_object_ptr(vm->staticStrings.bool_true), fluffyvm_get_root(vm), rootRef);
        return vm->staticStrings.bool_true;
      } else {
        foxgc_api_root_add(vm->heap, value_get_object_ptr(vm->staticStrings.bool_true), fluffyvm_get_root(vm), rootRef);
        return vm->staticStrings.bool_true;
      }
    case FLUFFYVM_TVALUE_NIL:
      foxgc_api_root_add(vm->heap, value_get_object_ptr(vm->staticStrings.typenames_nil), fluffyvm_get_root(vm), rootRef);
      return vm->staticStrings.typenames_nil;
    
    case FLUFFYVM_TVALUE_TABLE:
      bufLen = snprintf(NULL, 0, "table 0x%" PRIXPTR, (uintptr_t) value.data.table);
      break;
    case FLUFFYVM_TVALUE_CLOSURE:
      bufLen = snprintf(NULL, 0, "function 0x%" PRIXPTR, (uintptr_t) value.data.closure->gc_this);
      break;
    case FLUFFYVM_TVALUE_FULL_USERDATA:
    case FLUFFYVM_TVALUE_LIGHT_USERDATA:
      bufLen = snprintf(NULL, 0, "userdata 0x%" PRIXPTR, (uintptr_t) value.data.userdata->data);
      break;
    case FLUFFYVM_TVALUE_GARBAGE_COLLECTABLE_USERDATA:
      bufLen = snprintf(NULL, 0, "userdata 0x%" PRIXPTR, (uintptr_t) value.data.userdata->userGarbageCollectableData);
      break;
    case FLUFFYVM_TVALUE_COROUTINE:
      bufLen = snprintf(NULL, 0, "coroutine 0x%" PRIXPTR, (uintptr_t) value.data.coroutine->gc_this);
      break;

    case FLUFFYVM_TVALUE_LAST:    
    case FLUFFYVM_TVALUE_NOT_PRESENT:
      abort(); /* Can't happen */
  }

  // snprintf return excluding NULL terminator
  bufLen++;
  
  struct value_string* strStruct = malloc(sizeof(*strStruct));
  if (strStruct == NULL)
    goto no_memory;

  foxgc_object_t* obj = foxgc_api_new_data_array(vm->heap, fluffyvm_get_owner_key(), stringDataArrayKey, NULL, fluffyvm_get_root(vm), rootRef, 1, bufLen, Block_copy(^void (foxgc_object_t* obj) {
    free(strStruct); 
  }));

  if (obj == NULL)
    goto no_memory;
  buffer = foxgc_api_object_get_data(obj);
  
  struct value result = {
    .type = FLUFFYVM_TVALUE_STRING,
    .data.str = strStruct
  };
  commonStringInit(strStruct, obj);
   
  switch (value.type) { 
    case FLUFFYVM_TVALUE_LONG:
      snprintf(buffer, bufLen, "%ld", value.data.longNum);
      break;

    case FLUFFYVM_TVALUE_DOUBLE:
      snprintf(buffer, bufLen, "%lf", value.data.doubleData);
      break;
    
    case FLUFFYVM_TVALUE_TABLE:
      snprintf(buffer, bufLen, "table 0x%" PRIXPTR, (uintptr_t) value.data.table);
      break;
    case FLUFFYVM_TVALUE_CLOSURE:
      snprintf(buffer, bufLen, "function 0x%" PRIXPTR, (uintptr_t) value.data.closure->gc_this);
      break;
    
    case FLUFFYVM_TVALUE_FULL_USERDATA:
    case FLUFFYVM_TVALUE_LIGHT_USERDATA:
      snprintf(buffer, bufLen, "userdata 0x%" PRIXPTR, (uintptr_t) value.data.userdata->data);
      break;
    case FLUFFYVM_TVALUE_GARBAGE_COLLECTABLE_USERDATA:
      snprintf(buffer, bufLen, "userdata 0x%" PRIXPTR, (uintptr_t) value.data.userdata->userGarbageCollectableData);
      break;
    case FLUFFYVM_TVALUE_COROUTINE:
      bufLen = snprintf(buffer, bufLen, "coroutine 0x%" PRIXPTR, (uintptr_t) value.data.coroutine->gc_this);
      break;

    case FLUFFYVM_TVALUE_STRING:
    case FLUFFYVM_TVALUE_NIL:  
    case FLUFFYVM_TVALUE_BOOL:  
    case FLUFFYVM_TVALUE_NOT_PRESENT:
    case FLUFFYVM_TVALUE_LAST:    
      abort(); /* Can't happen */
  }
  return result;
  
  no_memory:
  free(strStruct);
  if (vm->staticStrings.outOfMemoryRootRef)
    fluffyvm_set_errmsg(vm, vm->staticStrings.outOfMemory);
  return value_not_present;
}

struct value value_typename(struct fluffyvm* vm, struct value value) {
  return value_typename2(vm, value.type);
}

struct value value_typename2(struct fluffyvm* vm, value_types_t valueType) {
  switch (valueType) { 
    case FLUFFYVM_TVALUE_STRING:
      return vm->staticStrings.typenames_string;
    case FLUFFYVM_TVALUE_LONG:
      return vm->staticStrings.typenames_longNum;
    case FLUFFYVM_TVALUE_DOUBLE:
      return vm->staticStrings.typenames_doubleNum;
    case FLUFFYVM_TVALUE_NIL:
      return vm->staticStrings.typenames_nil;
    case FLUFFYVM_TVALUE_TABLE:
      return vm->staticStrings.typenames_table;
    case FLUFFYVM_TVALUE_CLOSURE:
      return vm->staticStrings.typenames_closure;
    case FLUFFYVM_TVALUE_LIGHT_USERDATA:
      return vm->staticStrings.typenames_light_userdata;
    case FLUFFYVM_TVALUE_FULL_USERDATA:
      return vm->staticStrings.typenames_full_userdata;
    case FLUFFYVM_TVALUE_GARBAGE_COLLECTABLE_USERDATA:
      return vm->staticStrings.typenames_garbage_collectable_userdata;
    case FLUFFYVM_TVALUE_BOOL:
      return vm->staticStrings.typenames_bool;
    case FLUFFYVM_TVALUE_COROUTINE:
      return vm->staticStrings.typenames_coroutine;
    case FLUFFYVM_TVALUE_LAST:    
    case FLUFFYVM_TVALUE_NOT_PRESENT:
      abort();
  };
}

struct value value_todouble(struct fluffyvm* vm, struct value value) {
  checkPresent(&value);
  
  char* lastChar = NULL;
  double number = 0.0f;
  switch (value.type) {
    case FLUFFYVM_TVALUE_STRING:
      errno = 0;
      number = strtod(foxgc_api_object_get_data(value.data.str->str), &lastChar);
      if (*lastChar != '\0') {
        fluffyvm_set_errmsg(vm, vm->staticStrings.strtodDidNotProcessAllTheData);
        return value_not_present;
      }

      if (errno != 0) {
        static const char* format = "strtod errored with %d (%d)";
        
        int currentErrno = errno;
        char errorMessage[256] = {0};
        int err = strerror_r(currentErrno, errorMessage, sizeof(errorMessage));

        size_t len = 0;
        char* errMsg = malloc(len = snprintf(NULL, 0, format, currentErrno, err == 0 ? errorMessage : "error converting errno to string"));
        if (!errMsg) {
          fluffyvm_set_errmsg(vm, vm->staticStrings.outOfMemoryWhileAnErrorOccured);
          return value_not_present;
        }
        snprintf(errMsg, len, format, currentErrno, err == 0 ? errorMessage : "error converting errno to string");
        
        foxgc_root_reference_t* ref = NULL;
        struct value errorString = value_new_string(vm, errMsg, &ref);
        if (errorString.type != FLUFFYVM_TVALUE_NOT_PRESENT) {
          fluffyvm_set_errmsg(vm, errorString);
          foxgc_api_remove_from_root2(vm->heap, fluffyvm_get_root(vm), ref);
        }

        free(errMsg);
        return value_not_present;
      } 
      break;

    case FLUFFYVM_TVALUE_LONG:
      number = (double) value.data.longNum;
      break;

    case FLUFFYVM_TVALUE_DOUBLE:
      return value;

    case FLUFFYVM_TVALUE_CLOSURE:
    case FLUFFYVM_TVALUE_LIGHT_USERDATA:
    case FLUFFYVM_TVALUE_FULL_USERDATA:
    case FLUFFYVM_TVALUE_TABLE:
    case FLUFFYVM_TVALUE_NIL:
    case FLUFFYVM_TVALUE_COROUTINE:
    case FLUFFYVM_TVALUE_GARBAGE_COLLECTABLE_USERDATA:
    case FLUFFYVM_TVALUE_BOOL:
      return value_not_present;
    
    case FLUFFYVM_TVALUE_LAST:    
    case FLUFFYVM_TVALUE_NOT_PRESENT:
      abort(); /* Can't happen */
  }

  return value_new_double(vm, number);
}


void* value_get_unique_ptr(struct value value) {
  checkPresent(&value);

  switch (value.type) {
    case FLUFFYVM_TVALUE_STRING:
      return value.data.str;
    case FLUFFYVM_TVALUE_TABLE:
      return value.data.table;
    case FLUFFYVM_TVALUE_CLOSURE:
      return value.data.closure->gc_this;
    case FLUFFYVM_TVALUE_FULL_USERDATA:
    case FLUFFYVM_TVALUE_LIGHT_USERDATA:
      return value.data.userdata->data;
    case FLUFFYVM_TVALUE_GARBAGE_COLLECTABLE_USERDATA:
      return value.data.userdata->userGarbageCollectableData;
    case FLUFFYVM_TVALUE_COROUTINE:
      return value.data.coroutine->gc_this;
    
    case FLUFFYVM_TVALUE_LONG:
    case FLUFFYVM_TVALUE_BOOL:
    case FLUFFYVM_TVALUE_DOUBLE:
    case FLUFFYVM_TVALUE_NIL:
      return NULL;
    
    case FLUFFYVM_TVALUE_LAST:    
    case FLUFFYVM_TVALUE_NOT_PRESENT:
      abort(); /* Can't happen */
  }
}

void value_copsy(struct value* dest, struct value src) {
  memcpy(dest, &src, sizeof(struct value));
}

bool value_equals_cstring(struct value op1, const char* str, size_t len) {
  uint64_t op1Hash;
  uint64_t op2Hash;
  
  // The order of checking is ordered
  // from least expensive check to most
  // expensive check (which memcmp)

  checkPresent(&op1);
  if (op1.type != FLUFFYVM_TVALUE_STRING)
    return false;

  if (value_get_len(op1) != len)
    return false;

  if (!value_hash_code(op1, &op1Hash))
    return false;
  
  op2Hash = hashing_hash_default(str, len);
  if (op1Hash != op2Hash)
    return false;

  if (memcmp(str, foxgc_api_object_get_data(op1.data.str->str), len) != 0)
    return false;

  return true;
}

bool value_equals(struct value op1, struct value op2) {
  bool result = false;
  uint64_t op1Hash;
  uint64_t op2Hash;
    
  checkPresent(&op1);
  checkPresent(&op2);

  if (op1.type != op2.type)
    return false;
  size_t maxLength = value_get_len(op2);
  if (value_get_len(op1) > maxLength)
    maxLength = value_get_len(op1);

  switch (op1.type) {
    case FLUFFYVM_TVALUE_STRING:
      if (value_get_len(op1) != value_get_len(op2))
        break;
      value_hash_code(op1, &op1Hash);
      value_hash_code(op2, &op2Hash);

      if (op1Hash == op2Hash)
        result = memcmp(value_get_string(op1), value_get_string(op2), maxLength) == 0;
      break;
    case FLUFFYVM_TVALUE_TABLE:
      result = op1.data.table == op2.data.table;
      break;
    case FLUFFYVM_TVALUE_CLOSURE:
      result = op1.data.closure == op2.data.closure;
      break;
    case FLUFFYVM_TVALUE_LONG:
      result = op1.data.longNum == op2.data.longNum;
      break;
    case FLUFFYVM_TVALUE_DOUBLE:
      result = op1.data.doubleData == op2.data.doubleData;
      break;
    case FLUFFYVM_TVALUE_FULL_USERDATA:
    case FLUFFYVM_TVALUE_LIGHT_USERDATA:
      result = op1.data.userdata->data == op2.data.userdata->data;
      break;
    case FLUFFYVM_TVALUE_GARBAGE_COLLECTABLE_USERDATA:
      result = op1.data.userdata->userGarbageCollectableData == op2.data.userdata->userGarbageCollectableData;
      break;
    case FLUFFYVM_TVALUE_BOOL:
      result = op1.data.boolean == op2.data.boolean;
      break;
    case FLUFFYVM_TVALUE_COROUTINE:
      result = op1.data.coroutine == op2.data.coroutine;
      break;
    case FLUFFYVM_TVALUE_NIL:
      return true;
    
    case FLUFFYVM_TVALUE_LAST:    
    case FLUFFYVM_TVALUE_NOT_PRESENT:
      abort(); /* Can't happen */
  }

  return result;
}

bool value_table_set(struct fluffyvm* vm, struct value table, struct value key, struct value value) {
  checkPresent(&table);
  checkPresent(&key);
  checkPresent(&value);
  if (table.type != FLUFFYVM_TVALUE_TABLE) {
    fluffyvm_set_errmsg_printf(vm, "attempt to index '%s'", value_get_string(value_typename(vm, table)));
    return false;
  }
  
  hashtable_set(vm, foxgc_api_object_get_data(table.data.table), key, value);
  return true;
}

/*
bool value_table_remove(struct fluffyvm* vm, struct value table, struct value key) {
  checkPresent(&table);
  checkPresent(&key);
  if (table.type != FLUFFYVM_TVALUE_TABLE) {
    fluffyvm_set_errmsg_printf(vm, "attempt to remove a key/value pair from '%s'", value_get_string(value_typename(vm, table)));
    return false;
  }
  
  hashtable_remove(vm, foxgc_api_object_get_data(table.data.table), key);
  return true;
}
*/

struct value value_table_get(struct fluffyvm* vm, struct value table, struct value key, foxgc_root_reference_t** rootRef) {
  checkPresent(&table);
  checkPresent(&key);
  if (table.type != FLUFFYVM_TVALUE_TABLE) {
    fluffyvm_set_errmsg_printf(vm, "attempt to index '%s'", value_get_string(value_typename(vm, table)));
    return value_not_present;
  }
  
  return hashtable_get(vm, foxgc_api_object_get_data(table.data.table), key, rootRef);
}

bool value_table_is_indexable(struct value val) {
  switch (val.type) {
    case FLUFFYVM_TVALUE_STRING:
    case FLUFFYVM_TVALUE_DOUBLE:
    case FLUFFYVM_TVALUE_LONG:
    case FLUFFYVM_TVALUE_CLOSURE:
    case FLUFFYVM_TVALUE_NIL:
    case FLUFFYVM_TVALUE_FULL_USERDATA:
    case FLUFFYVM_TVALUE_LIGHT_USERDATA:
    case FLUFFYVM_TVALUE_GARBAGE_COLLECTABLE_USERDATA:
    case FLUFFYVM_TVALUE_BOOL:
    case FLUFFYVM_TVALUE_COROUTINE:
      return false;
    
    case FLUFFYVM_TVALUE_TABLE:
      return true;

    case FLUFFYVM_TVALUE_LAST:
    case FLUFFYVM_TVALUE_NOT_PRESENT:
      abort();
  }
  abort();
}

bool value_is_callable(struct value val) {
  switch (val.type) {
    case FLUFFYVM_TVALUE_STRING:
    case FLUFFYVM_TVALUE_DOUBLE:
    case FLUFFYVM_TVALUE_LONG:
    case FLUFFYVM_TVALUE_TABLE:
    case FLUFFYVM_TVALUE_NIL:
    case FLUFFYVM_TVALUE_FULL_USERDATA:
    case FLUFFYVM_TVALUE_LIGHT_USERDATA:
    case FLUFFYVM_TVALUE_BOOL:
    case FLUFFYVM_TVALUE_GARBAGE_COLLECTABLE_USERDATA:
    case FLUFFYVM_TVALUE_COROUTINE:
      return false;
    
    case FLUFFYVM_TVALUE_CLOSURE:
      return true;

    case FLUFFYVM_TVALUE_LAST:
    case FLUFFYVM_TVALUE_NOT_PRESENT:
      abort();
  }
  abort();
}

static bool mathCommon(struct fluffyvm* vm, const char* op, struct value* op1, struct value* op2) {
  if (!value_is_numeric(*op1) || !value_is_numeric(*op2)) { 
    fluffyvm_set_errmsg_printf(vm, "attempt to do '%s' operation with '%s' type and '%s' type", op, value_get_string(value_typename(vm, *op1)), value_get_string(value_typename(vm, *op2)));
    return false;
  }

  if (op1->type == op2->type)
    return true;

  if (op1->type == FLUFFYVM_TVALUE_DOUBLE)
    *op2 = value_todouble(vm, *op2);
  else
    *op1 = value_todouble(vm, *op1);

  return true;
}

#define X(name, op, ...) \
VALUE_DECLARE_MATH_OP(value_math_ ## name) { \
  if (!mathCommon(vm, #op, &op1, &op2)) \
    return value_not_present; \
  switch (op1.type) { \
    case FLUFFYVM_TVALUE_LONG: \
      return value_new_long(vm, op1.data.longNum op op2.data.longNum); \
    case FLUFFYVM_TVALUE_DOUBLE: \
      return value_new_double(vm, op1.data.doubleData op op2.data.doubleData); \
    default: \
        abort(); \
  } \
}

VALUE_MATH_OPS
#undef X

VALUE_DECLARE_MATH_OP(value_math_mod) {
  if (!mathCommon(vm, "%", &op1, &op2))
    return value_not_present;

  switch (op1.type) {
    case FLUFFYVM_TVALUE_LONG:
      return value_new_long(vm, op1.data.longNum % op2.data.longNum);
    case FLUFFYVM_TVALUE_DOUBLE:
      return value_new_double(vm, fmod(op1.data.doubleData, op2.data.doubleData));
    default:
        abort();
  }
}

VALUE_DECLARE_MATH_OP(value_math_pow) {
  if (!mathCommon(vm, "^", &op1, &op2))
    return value_not_present;

  switch (op1.type) {
    case FLUFFYVM_TVALUE_LONG:
      return value_new_double(vm, pow(op1.data.longNum, op2.data.longNum));
    case FLUFFYVM_TVALUE_DOUBLE:
      return value_new_double(vm, pow(op1.data.doubleData, op2.data.doubleData));
    default:
        abort();
  }
}

value_comparison_result_t value_is_equal(struct fluffyvm* vm, struct value op1, struct value op2) {
  if (value_equals(op1, op2))
    return VALUE_CMP_TRUE;
  
  return VALUE_CMP_FALSE;
}

bool value_is_numeric(struct value val) {
  switch (val.type) {
    case FLUFFYVM_TVALUE_DOUBLE:
    case FLUFFYVM_TVALUE_LONG:
      return true;
    case FLUFFYVM_TVALUE_STRING:
    case FLUFFYVM_TVALUE_TABLE:
    case FLUFFYVM_TVALUE_NIL:
    case FLUFFYVM_TVALUE_FULL_USERDATA:
    case FLUFFYVM_TVALUE_LIGHT_USERDATA:
    case FLUFFYVM_TVALUE_BOOL:
    case FLUFFYVM_TVALUE_GARBAGE_COLLECTABLE_USERDATA:
    case FLUFFYVM_TVALUE_COROUTINE:
    case FLUFFYVM_TVALUE_CLOSURE:
      return false;

    case FLUFFYVM_TVALUE_LAST:
    case FLUFFYVM_TVALUE_NOT_PRESENT:
      abort();
  }
  abort();
}

value_comparison_result_t value_is_less(struct fluffyvm* vm, struct value op1, struct value op2) {
  if (!mathCommon(vm, "is less", &op1, &op2))
    return VALUE_CMP_INAPPLICABLE;
  
  switch (op1.type) {
    case FLUFFYVM_TVALUE_LONG:
      return op1.data.longNum < op2.data.longNum ? VALUE_CMP_TRUE : VALUE_CMP_FALSE;
    case FLUFFYVM_TVALUE_DOUBLE:
      return op1.data.longNum < op2.data.longNum ? VALUE_CMP_TRUE : VALUE_CMP_FALSE;
    
    default:
      abort();
  }
}

