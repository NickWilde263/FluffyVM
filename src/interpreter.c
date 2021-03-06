#define FLUFFYVM_INTERNAL

#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <assert.h>
#include <inttypes.h>

#include "bytecode.h"
#include "interpreter.h"
#include "closure.h"
#include "config.h"
#include "coroutine.h"
#include "fluffyvm.h"
#include "util/functional/functional.h"
#include "util/util.h"
#include "value.h"
#include "opcodes.h"
#include "api_layer/types.h"

static int instructionFieldUsed[FLUFFYVM_OPCODE_LAST] = {
# define X(name, op, nameInString, fieldsUsed, ...) \
  [op] = fieldsUsed,
  FLUFFYVM_OPCODES
# undef X
};

static const char* instructionName[FLUFFYVM_OPCODE_LAST] = {
# define X(name, op, nameInString, ...) \
  [op] = nameInString,
  FLUFFYVM_OPCODES
# undef X
};

static inline bool setRegister(struct fluffyvm* vm, struct fluffyvm_call_state* callState, int index, struct value value) {
  assert(value.type != FLUFFYVM_TVALUE_NOT_PRESENT);
  if (index != FLUFFYVM_INTERPRETER_REGISTER_ENV &&
    index >= FLUFFYVM_INTERPRETER_RESERVED_START &&
    index <= FLUFFYVM_INTERPRETER_RESERVED_START) {
    return true;
  }
  
  assert(index >= 0 && index < FLUFFYVM_REGISTERS_NUM);

  callState->registers[index] = value;
  foxgc_api_write_array(callState->gc_registerObjectArray, index, value_get_object_ptr(value));
  return true;
}

static inline struct value getRegister(struct fluffyvm* vm, struct fluffyvm_call_state* callState, int index) { 
  switch (index) {
    case FLUFFYVM_INTERPRETER_REGISTER_ENV:
      return callState->closure->env;
    case FLUFFYVM_INTERPRETER_REGISTER_CURRENT:
      return callState->closure->asValue;
    case FLUFFYVM_INTERPRETER_REGISTER_ALWAYS_NIL:
      return value_nil;
  }
  
  assert(index >= 0 && index < FLUFFYVM_REGISTERS_NUM);
  
  if (callState->registers[index].type == FLUFFYVM_TVALUE_NOT_PRESENT)
    return value_nil;

  return callState->registers[index];
}

bool interpreter_pop(struct fluffyvm* vm, struct fluffyvm_call_state* callState, struct value* result, foxgc_root_reference_t** rootRef) {
  if (callState->sp - 1 < 0) {
    fluffyvm_set_errmsg(vm, vm->staticStrings.stackUnderflow); 
    return false;
  }
  
  if (rootRef)
    *rootRef = NULL;
  
  callState->sp--;
  int index = callState->sp;
  assert(callState->generalStack[index].type != FLUFFYVM_TVALUE_NOT_PRESENT);
  struct value val = callState->generalStack[index];
  
  if (result)
    *result = val;

  foxgc_object_t* ptr;
  if (rootRef && (ptr = value_get_object_ptr(val)))
    foxgc_api_root_add(vm->heap, ptr, fluffyvm_get_root(vm), rootRef);

  callState->generalStack[index] = value_not_present;
  foxgc_api_write_array(callState->gc_generalObjectStack, index, NULL);
  return true;
}

static bool interpreter_pop2(struct fluffyvm* vm, struct fluffyvm_call_state* callState, int destination) {
  struct value val;
  foxgc_root_reference_t* ref = NULL;
  if (!interpreter_pop(vm, callState, &val, &ref))
    return false;

  setRegister(vm, callState, destination, val);
  if (ref)
    foxgc_api_remove_from_root2(vm->heap, fluffyvm_get_root(vm), ref);
  return true;
}

bool interpreter_push(struct fluffyvm* vm, struct fluffyvm_call_state* callState, struct value value) {
  if (callState->sp >= FLUFFYVM_GENERAL_STACK_SIZE) {
    fluffyvm_set_errmsg(vm, vm->staticStrings.stackOverflow);
    return false;
  }
  
  assert(value.type != FLUFFYVM_TVALUE_NOT_PRESENT);
  callState->generalStack[callState->sp] = value;
  foxgc_api_write_array(callState->gc_generalObjectStack, callState->sp, value_get_object_ptr(value));
  callState->sp++;
  return true;
}

void interpreter_function_epilog(struct fluffyvm* vm, struct fluffyvm_coroutine* co) {
}

////////////////////////////////////////////////////////
////////////////////////////////////////////////////////
////////////////////////////////////////////////////////

struct instruction {
  fluffyvm_opcode_t opcode;
  int condFlags;
  uint16_t A, B, C;
   
  // Located at next instruction
  // when needed
  uint16_t D, E, F;
  uint16_t G, H, I;
};

static struct instruction decode(fluffyvm_instruction_t instruction) {
  struct instruction ins = {0};
  ins.opcode = (instruction >> 56) & 0xFF;
  ins.condFlags = (instruction >> 48) & 0xFF;
  ins.A = (instruction >> 32) & 0xFFFF;
  ins.B = (instruction >> 16) & 0xFFFF;
  ins.C = instruction & 0xFFFF; 
  return ins;
}

void interpreter_call(struct fluffyvm* F, struct value func, int nargs, int nret) {
  struct fluffyvm_coroutine* co = fluffyvm_get_executing_coroutine(F);
  assert(co);
  
  struct fluffyvm_closure* closure = func.data.closure;
  struct fluffyvm_call_state* callerState = co->currentCallState;
  
  int argsEnd = 0;
  int argsStart = callerState->sp - nargs;
  if (argsStart < 0 || nargs == -1)
    argsStart = 0;
  
  if (!coroutine_function_prolog(F, closure))
    goto error;

  // Copy args
  argsEnd = argsStart + nargs - 1;
  
  // varargs
  if (nargs == -1)
    argsEnd = co->currentCallState->sp - 1;

  for (int i = argsStart; i <= argsEnd; i++)
    if (!interpreter_push(F, co->currentCallState, callerState->generalStack[i]))
      goto error;

  // Remove from caller stack
  for (int i = argsStart; i <= argsEnd; i++)
    interpreter_pop(F, callerState, NULL, NULL); 

  int actualRetCount = interpreter_exec(F, co);
  
  // Copy return values
  int returnCount = nret;

  // vararg return
  if (nret == -1)
    returnCount = co->currentCallState->sp;

  int startPos = co->currentCallState->sp - actualRetCount;
  if (startPos < 0)
    startPos = 0;
  
  for (int i = 0; i < returnCount; i++) {
    struct value val = value_nil;

    // Copy only if current pos is valid
    if (startPos + i <= co->currentCallState->sp - 1)
      val = co->currentCallState->generalStack[startPos + i];
    
    // Error here
    if (!interpreter_push(F, callerState, val))
      goto error;
  }

  coroutine_function_epilog(F);  
  return;
 
  error:
  interpreter_error(F, fluffyvm_get_errmsg(F));
  abort();
}

bool interpreter_xpcall(struct fluffyvm* vm, runnable_t thingToExecute, runnable_t handler) {
  struct fluffyvm_coroutine* co = fluffyvm_get_executing_coroutine(vm);
  assert(co);

  jmp_buf env;
  jmp_buf* prevErrorHandler = co->errorHandler;
  struct fluffyvm_call_state* callerState = co->currentCallState;

  co->errorHandler = &env; 
  if (setjmp(env)) {
    if (handler)
      handler();
    
    // Fix call stack
    pthread_mutex_lock(&co->callStackLock);
    while (co->currentCallState != callerState)
      coroutine_function_epilog_no_lock(vm);
    pthread_mutex_unlock(&co->callStackLock);
    
    co->errorHandler = prevErrorHandler;
    return false;
  }
   
  thingToExecute();
  co->errorHandler = prevErrorHandler;
  return true;
}

int interpreter_exec(struct fluffyvm* vm, struct fluffyvm_coroutine* co) {
  if (co->currentCallState->closure->prototype == NULL)
    return co->currentCallState->closure->func(vm, co->currentCallState, co->currentCallState->closure->udata);
  int pc = 0;
  struct fluffyvm_call_state* callState = co->currentCallState;

  int instructionsLen = co->currentCallState->closure->prototype->instructions_len;
  assert(instructionsLen >= pc);
  int retCount = 0;

  const fluffyvm_instruction_t* instructionsArray = co->currentCallState->closure->prototype->instructions;
  struct instruction ins;
  while (pc < instructionsLen) {
    ins = decode(instructionsArray[pc]);
    
    int condFlagsMask = ins.condFlags & 0xF0 >> 4;
    int condFlags = ins.condFlags & condFlagsMask;
    int flagRegister = callState->flagRegister & condFlagsMask;
    
    // Skip instruction
    if (((~(flagRegister ^ condFlags)) & condFlagsMask) == 0 && condFlagsMask)
      goto skip_instruction;

    /*
    Value   Mask    Pattern Result
    0b000   0b000   0b000  true
    0b001   0b001   0b000  false
    0b001   0b001   0b001  true
    0b001   0b011   0b000  true
    0b010   0b011   0b011  true
    0b000   0b011   0b011  false
    0b100   0b011   0b011  false
    0b100   0b010   0b001  true
    0b100   0b100   0b101  true
    0b101   0b110   0b101  true
    */

    if (ins.opcode >= FLUFFYVM_OPCODE_LAST || ins.opcode == FLUFFYVM_OPCODE_EXTRA)
      goto illegal_instruction;
 
    // Calculate amount of instructions to increment
    // due some instruction take more than one instruction
    int incrementCount = 0;
    incrementCount += instructionFieldUsed[ins.opcode] / 3;
    incrementCount += instructionFieldUsed[ins.opcode] % 3 > 0 ? 1 : 0;
    
    if (incrementCount == 0)
      incrementCount = 1;

    // Fetch additional fields
    if (incrementCount > 1) {
      struct instruction extraInstructions[incrementCount - 1];
      for (int i = 0; i < incrementCount - 1; i++) {
        extraInstructions[i] = decode(instructionsArray[pc + i + 1]);
        if (extraInstructions[i].opcode != FLUFFYVM_OPCODE_EXTRA)
          goto illegal_instruction;
      }

      switch (incrementCount - 1) {
        case 2:
          ins.G = extraInstructions[1].A;
          ins.H = extraInstructions[1].B;
          ins.I = extraInstructions[1].C;
        case 1:
          ins.D = extraInstructions[0].A;
          ins.E = extraInstructions[0].B;
          ins.F = extraInstructions[0].C;
          break;
        
        default:
          goto illegal_instruction;
      }
    }

    switch (ins.opcode) {
      case FLUFFYVM_OPCODE_MOV:
        //printf("0x%08X: R(%d) = R(%d)\n", pc, ins.A, ins.B);
        setRegister(vm, callState, ins.A, getRegister(vm, callState, ins.B));
        break;
      case FLUFFYVM_OPCODE_ADD:
      {
        struct value tmp = value_math_add(vm, getRegister(vm, callState, ins.B), getRegister(vm, callState, ins.C));
        if (tmp.type == FLUFFYVM_TVALUE_NOT_PRESENT)
          goto error;

        setRegister(vm, callState, ins.A, tmp);
        break;
      }
      case FLUFFYVM_OPCODE_SUB:
      {
        struct value tmp = value_math_sub(vm, getRegister(vm, callState, ins.B), getRegister(vm, callState, ins.C));
        if (tmp.type == FLUFFYVM_TVALUE_NOT_PRESENT)
          goto error;

        setRegister(vm, callState, ins.A, tmp);
        break;
      }
      case FLUFFYVM_OPCODE_MUL:
      {
        struct value tmp = value_math_mul(vm, getRegister(vm, callState, ins.B), getRegister(vm, callState, ins.C));
        if (tmp.type == FLUFFYVM_TVALUE_NOT_PRESENT)
          goto error;

        setRegister(vm, callState, ins.A, tmp);
        break;
      }
      case FLUFFYVM_OPCODE_DIV:
      {
        struct value tmp = value_math_div(vm, getRegister(vm, callState, ins.B), getRegister(vm, callState, ins.C));
        if (tmp.type == FLUFFYVM_TVALUE_NOT_PRESENT)
          goto error;

        setRegister(vm, callState, ins.A, tmp);
        break;
      }
      case FLUFFYVM_OPCODE_MOD:
      {
        struct value tmp = value_math_mod(vm, getRegister(vm, callState, ins.B), getRegister(vm, callState, ins.C));
        if (tmp.type == FLUFFYVM_TVALUE_NOT_PRESENT)
          goto error;

        setRegister(vm, callState, ins.A, tmp);
        break;
      }
      case FLUFFYVM_OPCODE_POW:
      {
        struct value tmp = value_math_pow(vm, getRegister(vm, callState, ins.B), getRegister(vm, callState, ins.C));
        if (tmp.type == FLUFFYVM_TVALUE_NOT_PRESENT)
          goto error;

        setRegister(vm, callState, ins.A, tmp);
        break;
      }
      case FLUFFYVM_OPCODE_JMP_FORWARD:
        if (pc + ins.A >= instructionsLen) {
          fluffyvm_set_errmsg_printf(vm, "Attempting to forward jump to %d out of %d instructions", pc, instructionsLen);
          goto error;
        }
        pc += ins.A;
        break;
      case FLUFFYVM_OPCODE_CMP: 
      {
        struct value op1 = getRegister(vm, callState, ins.A);
        struct value op2 = getRegister(vm, callState, ins.B);
        if (value_is_equal(vm, op1, op2) == VALUE_CMP_TRUE)
          callState->flagRegister |= FLUFFYVM_INTERPRETER_FLAG_EQUAL;
        else
          callState->flagRegister &= ~FLUFFYVM_INTERPRETER_FLAG_EQUAL;
        
        if (value_is_less(vm, op1, op2) == VALUE_CMP_TRUE)
          callState->flagRegister |= FLUFFYVM_INTERPRETER_FLAG_LESS;
        else
          callState->flagRegister &= ~FLUFFYVM_INTERPRETER_FLAG_LESS;

        break;
      }
      case FLUFFYVM_OPCODE_JMP_BACKWARD:
        if (pc - ins.A < 0) {
          fluffyvm_set_errmsg_printf(vm, "Attempting to backward jump to %d", pc);
          goto error;
        }
        pc -= ins.A;
        break;
      case FLUFFYVM_OPCODE_LOAD_PROTOTYPE:
      {
        //printf("0x%08X: R(%d) = Proto[%d]\n", pc, ins.A, ins.B);
        foxgc_root_reference_t* rootRef = NULL;
        struct fluffyvm_closure* closure = closure_new(vm, &rootRef, foxgc_api_object_get_data(callState->closure->prototype->prototypes[ins.B]), getRegister(vm, callState, FLUFFYVM_INTERPRETER_REGISTER_ENV));
        setRegister(vm, callState, ins.A, value_new_closure(vm, closure)); 
        foxgc_api_remove_from_root2(vm->heap, fluffyvm_get_root(vm), rootRef);
        break;
      }
      case FLUFFYVM_OPCODE_GET_CONSTANT: 
        //printf("0x%08X: R(%d) = ConstPool[%d]\n", pc, ins.A, ins.B);
        if (ins.B >= callState->closure->prototype->bytecode->constants_len)
          goto illegal_instruction;
        
        setRegister(vm, callState, ins.A, callState->closure->prototype->bytecode->constants[ins.B]);
        break;
      case FLUFFYVM_OPCODE_STACK_GETTOP:
        setRegister(vm, callState, ins.A, value_new_long(vm, callState->sp - 1));
        break;
      case FLUFFYVM_OPCODE_STACK_POP:
        //printf("0x%08X: S.top--; R(%d) = S(S.top)\n", pc, ins.A);
        if (!interpreter_pop2(vm, callState, ins.A))
          goto error;
        break;
      case FLUFFYVM_OPCODE_TABLE_GET:
        {
          //printf("0x%08X: R(%d) = R(%d)[R(%d)]\n", pc, ins.A, ins.B, ins.C);
          foxgc_root_reference_t* tmpRootRef = NULL;
          struct value table = getRegister(vm, callState, ins.B);
          struct value key = getRegister(vm, callState, ins.C);
          
          fluffyvm_clear_errmsg(vm);
          struct value result = value_table_get(vm, table, key, &tmpRootRef);

          if (!value_table_is_indexable(table) || fluffyvm_is_errmsg_present(vm))
            goto error;
          
          if (result.type == FLUFFYVM_TVALUE_NOT_PRESENT)
            result = value_nil;

          setRegister(vm, callState, ins.A, result);
          if (tmpRootRef)
            foxgc_api_remove_from_root2(vm->heap, fluffyvm_get_root(vm), tmpRootRef);
          break;
        }
      case FLUFFYVM_OPCODE_STACK_PUSH:
        //printf("0x%08X: S(S.top) = R(%d); S.top++\n", pc, ins.A);
        if (!interpreter_push(vm, callState, getRegister(vm, callState, ins.A)))
          goto error;
        break;
      case FLUFFYVM_OPCODE_CALL: 
        {
          int B = ins.B;
          
          int C = 0;
          int D = ins.C;
          if (D > 1)
            C = callState->sp - (D - 1);

          int argsStart = C;
          int argsEnd = C + D - 2;
          int returnCount = B - 1;
          struct fluffyvm_closure* closure;
          
          if (returnCount < 0)
            returnCount = 0;

          if (argsStart < 0) {
            fluffyvm_set_errmsg(vm, vm->staticStrings.stackUnderflow);
            goto error;
          }

          //printf("0x%08X: S(%d)..S(%d) = R(%d)(S(%d)..S(%d))\n", pc, callState->sp, callState->sp + returnCount - 1, ins.A, argsStart, argsEnd);
          struct value val = getRegister(vm, callState, ins.A);

          if (val.type == FLUFFYVM_TVALUE_NIL) {
            fluffyvm_set_errmsg(vm, vm->staticStrings.attemptToCallNilValue);
            goto error;
          }

          if (!value_is_callable(val)) {
            fluffyvm_set_errmsg_printf(vm, "attempt to call non callable value of type '%s'", value_get_string(value_typename(vm, val)));
            goto error;
          }
          
          closure = val.data.closure;

          if (!coroutine_function_prolog(vm, closure))
            goto error;
          
          // Copying the arguments
          if (D == 1)
            argsEnd = callState->sp - 1;

          for (int i = argsStart; i <= argsEnd; i++)
            if (!interpreter_push(vm, co->currentCallState, callState->generalStack[i]))
              goto call_error;

          // Actually executing
          callState->pc = pc;
          int actualRetCount = interpreter_exec(vm, co);
          
          for (int i = argsStart; i <= argsEnd; i++)
            if (!interpreter_pop(vm, callState, NULL, NULL))
              abort();
          
          // Copying the return values
          if (B == 1)
            returnCount = co->currentCallState->sp;
          
          int startPos = co->currentCallState->sp - actualRetCount;
          if (startPos < 0)
            startPos = 0;
          
          for (int i = 0; i < returnCount; i++) {
            struct value val = value_nil;

            // Copy only if current pos is valid
            if (startPos + i <= co->currentCallState->sp - 1)
              val = co->currentCallState->generalStack[startPos + i];

            if (!interpreter_push(vm, callState, val))
              goto call_error;
          }

          coroutine_function_epilog(vm);
          break;

          call_error:
          coroutine_function_epilog(vm);
          goto error;
        }
      case FLUFFYVM_OPCODE_TABLE_SET:
        //printf("0x%08X: R(%d)[R(%d)] = R(%d)\n", pc, ins.A, ins.B, ins.C);
        {
          struct value table = getRegister(vm, callState, ins.A);
          struct value key = getRegister(vm, callState, ins.B);
          struct value value = getRegister(vm, callState, ins.C);

          value_table_set(vm, table, key, value);
          break;
        }
      case FLUFFYVM_OPCODE_RETURN:
        //printf("0x%08X: ret(R(%d)..R(%d))\n", pc, ins.A, ins.A + ins.B - 1);
        for (int i = ins.A; i < ins.A + ins.B; i++)
          interpreter_push(vm, callState, getRegister(vm, callState, i));
        retCount = ins.B;
        goto done_function;
      case FLUFFYVM_OPCODE_NOP:
        //printf("0x%08X: nop\n", pc);
        break;

      case FLUFFYVM_OPCODE_EXTRA:
        goto illegal_instruction;
      case FLUFFYVM_OPCODE_LAST:
        abort(); /* Unreachable */
    }

    skip_instruction:

    pc += incrementCount;
    callState->pc = pc;
  }

  done_function:
  callState->pc = pc;
  return retCount;

  illegal_instruction:
  if (ins.opcode < FLUFFYVM_OPCODE_LAST) 
    fluffyvm_set_errmsg_printf(vm, "illegal instruction 0x%016" PRIX64 " (Op: '%s'  Cond: 0x%02X  A: 0x%04X  B: 0x%04X  C: 0x%04X)", instructionsArray[pc], instructionName[ins.opcode], ins.condFlags, ins.A, ins.B, ins.C);
  else 
    fluffyvm_set_errmsg_printf(vm, "illegal instruction 0x%016" PRIX64 " (Op: 0x%02X  Cond: 0x%02X  A: 0x%04X  B: 0x%04X  C: 0x%04X)", instructionsArray[pc], ins.opcode, ins.condFlags, ins.A, ins.B, ins.C);
  error:
  callState->pc = pc;
  interpreter_error(vm, fluffyvm_get_errmsg(vm));
  
  // Can't be reached
  abort();
}

bool interpreter_peek(struct fluffyvm* vm, struct fluffyvm_call_state* callState, int index, struct value* result) {
  if (index < 0 || index >= callState->sp) {
    if (index < 0) 
      fluffyvm_set_errmsg(vm, vm->staticStrings.stackUnderflow);
    else
      fluffyvm_set_errmsg(vm, vm->staticStrings.stackOverflow);
    return false;
  }

  *result = callState->generalStack[index];
  return true;
}

int interpreter_get_top(struct fluffyvm* vm, struct fluffyvm_call_state* callState) {
  return callState->sp - 1;
}

struct value interpreter_get_env(struct fluffyvm* vm, struct fluffyvm_call_state* callState) {
  return callState->closure->env;
}

static void errorCommon(struct fluffyvm* vm, struct value errmsg) {
  fluffyvm_set_errmsg(vm, errmsg);

  struct fluffyvm_coroutine* co = fluffyvm_get_executing_coroutine(vm);
  assert(co);

  if (co->errorHandler == NULL) {
    foxgc_root_reference_t* tmpRootRef = NULL;
    if (fluffyvm_is_errmsg_present(vm)) {
      struct value errMsg = value_tostring(vm, errmsg, &tmpRootRef);
      if (errMsg.type == FLUFFYVM_TVALUE_NOT_PRESENT)
        fprintf(stderr, "[FATAL] Error thrown without any handler (conversion error!)");
      else
        fprintf(stderr, "[FATAL] Error thrown without any handler: %s\n", value_get_string(errMsg));
    } else {
      fprintf(stderr, "[FATAL] Error thrown without any handler (error message not present)");
    }
    if (tmpRootRef)
      foxgc_api_remove_from_root2(vm->heap, fluffyvm_get_root(vm), tmpRootRef);
    abort(); // Error thrown without any handler
  }
}

static void commonErrorPrintf(struct fluffyvm* vm, const char* fmt, va_list list) {
  struct fluffyvm_coroutine* co = fluffyvm_get_executing_coroutine(vm);
  assert(co);
  
  char* msg;
  util_vasprintf(&msg, fmt, list);
  foxgc_root_reference_t* rootRef = NULL;
  struct value val = value_new_string(vm, msg, &rootRef);
 
  if (val.type == FLUFFYVM_TVALUE_NOT_PRESENT)
    val = vm->staticStrings.outOfMemoryWhileAnErrorOccured;

  if (rootRef)
    foxgc_api_remove_from_root2(vm->heap, fluffyvm_get_root(vm), rootRef);

  free(msg);

  errorCommon(vm, val);
}

void interpreter_error(struct fluffyvm* vm, struct value errmsg) {
  struct fluffyvm_coroutine* co = fluffyvm_get_executing_coroutine(vm);
  assert(co);
  
  errorCommon(vm, errmsg);
  longjmp(*co->errorHandler, 1);
}

void interpreter_error_printf(struct fluffyvm* vm, const char* fmt, ...) {
  struct fluffyvm_coroutine* co = fluffyvm_get_executing_coroutine(vm);
  assert(co);
  
  va_list list;
  va_start(list, fmt);
  commonErrorPrintf(vm, fmt, list);
  va_end(list);
  
  longjmp(*co->errorHandler, 1);
}

void interpreter_error_vprintf(struct fluffyvm* vm, const char* fmt, va_list list) {
  struct fluffyvm_coroutine* co = fluffyvm_get_executing_coroutine(vm);
  assert(co);
  
  commonErrorPrintf(vm, fmt, list);
  longjmp(*co->errorHandler, 1);
}












