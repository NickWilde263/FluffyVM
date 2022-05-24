#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <assert.h>

#include "bytecode.h"
#include "interpreter.h"
#include "closure.h"
#include "config.h"
#include "coroutine.h"
#include "fluffyvm.h"
#include "util/functional/functional.h"
#include "value.h"
#include "opcodes.h"

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

typedef enum {
  INTERPRETER_OK,
  INTERPRETER_ERROR
} call_status_t;

static bool setRegister(struct fluffyvm* vm, struct fluffyvm_call_state* callState, int index, struct value value) {
  assert(index >= 0 && index < FLUFFYVM_REGISTERS_NUM);
  assert(value.type != FLUFFYVM_TVALUE_NOT_PRESENT);
  value_copy(&callState->registers[index], &value);
  foxgc_api_write_array(callState->gc_registerObjectArray, index, value_get_object_ptr(value));
  return true;
}

static struct value getRegister(struct fluffyvm* vm, struct fluffyvm_call_state* callState, int index) {
  assert(index >= 0 && index < FLUFFYVM_REGISTERS_NUM);
  assert(callState->registers[index].type != FLUFFYVM_TVALUE_NOT_PRESENT);
  return callState->registers[index];
}

bool interpreter_pop(struct fluffyvm* vm, struct fluffyvm_call_state* callState, struct value* result, foxgc_root_reference_t** rootRef) {
  if (callState->sp - 1 < 0) {
    fluffyvm_set_errmsg(vm, vm->staticStrings.stackUnderflow); 
    return false;
  }
  
  callState->sp--;
  int index = callState->sp;
  assert(callState->generalStack[index].type != FLUFFYVM_TVALUE_NOT_PRESENT);
  struct value val = callState->generalStack[index];
  value_copy(result, &val);

  foxgc_object_t* ptr;
  if ((ptr = value_get_object_ptr(val)))
    foxgc_api_root_add(vm->heap, ptr, fluffyvm_get_root(vm), rootRef);
  else
    *rootRef = NULL;

  foxgc_api_write_array(callState->gc_generalObjectStack, callState->sp, NULL);
  
  return true;
}

static bool interpreter_pop2(struct fluffyvm* vm, struct fluffyvm_call_state* callState, int destination) {
  struct value val;
  foxgc_root_reference_t* ref = NULL;
  if (!interpreter_pop(vm, callState, &val, &ref))
    return false;

  setRegister(vm, callState, destination, val);
  foxgc_api_remove_from_root2(vm->heap, fluffyvm_get_root(vm), ref);
  return true;
}

bool interpreter_push(struct fluffyvm* vm, struct fluffyvm_call_state* callState, struct value value) {
  if (callState->sp >= FLUFFYVM_GENERAL_STACK_SIZE) {
    fluffyvm_set_errmsg(vm, vm->staticStrings.stackOverflow);
    return false;
  }
  assert(value.type != FLUFFYVM_TVALUE_NOT_PRESENT);
  value_copy(&callState->generalStack[callState->sp], &value);
  foxgc_api_write_array(callState->gc_generalObjectStack, callState->sp, value_get_object_ptr(value));
  callState->sp++;
  return true;
}

bool interpreter_function_prolog(struct fluffyvm* vm, struct fluffyvm_coroutine* co, struct fluffyvm_closure* func) {
  if (!co->currentCallState->closure->func) {
    setRegister(vm, co->currentCallState, FLUFFYVM_INTERPRETER_REGISTER_ENV, func->env);
    setRegister(vm, co->currentCallState, FLUFFYVM_INTERPRETER_REGISTER_ALWAYS_NIL, value_nil());
  }

  return true;
  /*
  error:
  coroutine_function_epilog(vm, co);
  */
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

bool interpreter_pcall(struct fluffyvm* vm, struct fluffyvm_call_state* callState, runnable_t thingToExecute) {
  struct fluffyvm_coroutine* co = fluffyvm_get_executing_coroutine(vm);
  assert(co);

  jmp_buf env;
  jmp_buf* prevErrorHandler = co->errorHandler;

  if (co->currentCallState->closure->func) {
    co->errorHandler = &env;
    
    if (setjmp(env))
      return false;
  }
  
  thingToExecute();
  co->errorHandler = prevErrorHandler;
  return true;
}

// I know i could just not recurse on calling
// each function. Until the interpreter fully
// working it stays like this
static call_status_t exec(struct fluffyvm* vm, struct fluffyvm_coroutine* co) {
  int pc = 0;
  struct fluffyvm_call_state* callState = co->currentCallState;
  
  if (co->currentCallState->closure->prototype == NULL) {
    co->currentCallState->closure->func(vm, co->currentCallState, co->currentCallState->closure->udata);
    goto done_function;
  }

  int instructionsLen = co->currentCallState->closure->prototype->instructions_len;
  assert(instructionsLen >= pc);

  const fluffyvm_instruction_t* instructionsArray = co->currentCallState->closure->prototype->instructions;
  while (pc < instructionsLen) {
    struct instruction ins = decode(instructionsArray[pc]);
    int incrementCount = 0;
    
    // Calculate amount of instructions to skip
    // due some instruction take more than one
    // instruction
    if (instructionFieldUsed > 0) {
      incrementCount += instructionFieldUsed[ins.opcode] / 3;
      incrementCount += instructionFieldUsed[ins.opcode] % 3 > 0 ? 1 : 0;
    } else {
      incrementCount = 1;
    }

    // Fetch additional fields
    if (incrementCount > 1) {
      struct instruction extraInstructions[incrementCount - 1];
      for (int i = 0; i < incrementCount - 1; i++) {
        extraInstructions[i] = decode(instructionsArray[pc + i + 1]);
        if (extraInstructions[i].opcode != FLUFFYVM_OPCODE_EXTRA) {
          fluffyvm_set_errmsg(vm, vm->staticStrings.illegalInstruction);
          goto error;
        }
      }

      switch (incrementCount - 1) {
        default:
          fluffyvm_set_errmsg(vm, vm->staticStrings.illegalInstruction);
          goto error;

        case 2:
          ins.G = extraInstructions[1].A;
          ins.H = extraInstructions[1].B;
          ins.I = extraInstructions[1].C;
        case 1:
          ins.D = extraInstructions[0].A;
          ins.E = extraInstructions[0].B;
          ins.F = extraInstructions[0].C;
          break;
      }
    }

    if (ins.opcode >= FLUFFYVM_OPCODE_LAST || ins.opcode == FLUFFYVM_OPCODE_EXTRA)
      goto illegal_instruction;
 
    setRegister(vm, callState, FLUFFYVM_INTERPRETER_REGISTER_ALWAYS_NIL, value_nil());
    switch (ins.opcode) {
      case FLUFFYVM_OPCODE_MOV:
        printf("0x%08X: R(%d) = R(%d)\n", pc, ins.A, ins.B);
        setRegister(vm, callState, ins.A, getRegister(vm, callState, ins.B));
        break;
      case FLUFFYVM_OPCODE_LOAD_PROTOTYPE:
      {
        printf("0x%08X: R(%d) = Proto[%d]\n", pc, ins.A, ins.B);
        foxgc_root_reference_t* rootRef = NULL;
        struct fluffyvm_closure* closure = closure_new(vm, &rootRef, foxgc_api_object_get_data(callState->closure->prototype->prototypes[ins.B]), getRegister(vm, callState, FLUFFYVM_INTERPRETER_REGISTER_ENV));
        setRegister(vm, callState, ins.A, value_new_closure(vm, closure)); 
        foxgc_api_remove_from_root2(vm->heap, fluffyvm_get_root(vm), rootRef);
        break;
      }
      case FLUFFYVM_OPCODE_GET_CONSTANT: 
        printf("0x%08X: R(%d) = ConstPool[%d]\n", pc, ins.A, ins.B);
        if (ins.B >= callState->closure->prototype->bytecode->constants_len)
          goto illegal_instruction;
        
        setRegister(vm, callState, ins.A, callState->closure->prototype->bytecode->constants[ins.B]);
        break;
      case FLUFFYVM_OPCODE_STACK_GETTOP:
        setRegister(vm, callState, ins.A, value_new_long(vm, callState->sp - 1));
        break;
      case FLUFFYVM_OPCODE_STACK_POP:
        printf("0x%08X: S.top--; R(%d) = S(S.top)\n", pc, ins.A);
        if (!interpreter_pop2(vm, callState, ins.A))
          goto error;
        break;
      case FLUFFYVM_OPCODE_TABLE_GET:
        printf("0x%08X: R(%d) = R(%d)[R(%d)]\n", pc, ins.A, ins.B, ins.C);
        {
          foxgc_root_reference_t* tmpRootRef = NULL;
          struct value table = getRegister(vm, callState, ins.B);
          struct value key = getRegister(vm, callState, ins.C);
          
          fluffyvm_clear_errmsg(vm);
          struct value result = value_table_get(vm, table, key, &tmpRootRef);

          if (!value_table_is_indexable(table) || fluffyvm_is_errmsg_present(vm))
            goto error;
          
          if (result.type == FLUFFYVM_TVALUE_NOT_PRESENT) {
            struct value nil = value_nil();
            value_copy(&result, &nil);
          }

          setRegister(vm, callState, ins.A, result);
          if (tmpRootRef)
            foxgc_api_remove_from_root2(vm->heap, fluffyvm_get_root(vm), tmpRootRef);
          break;
        }
      
        
      case FLUFFYVM_OPCODE_STACK_PUSH:
        printf("0x%08X: S(S.top) = R(%d); S.top++\n", pc, ins.A);
        if (!interpreter_push(vm, callState, getRegister(vm, callState, ins.A)))
          goto error;
        break;
      case FLUFFYVM_OPCODE_CALL: 
        {
          int argsStart = ins.C;
          int argsEnd = ins.C + ins.D - 2;
          int returnCount = ins.B - 1;
          struct fluffyvm_closure* closure;
          
          printf("0x%08X: S(%d)..S(%d) = R(%d)(S(%d)..S(%d))\n", pc, callState->sp, callState->sp + returnCount - 1, ins.A, argsStart, argsEnd);
          struct value val = getRegister(vm, callState, ins.A);

          if (val.type == FLUFFYVM_TVALUE_NIL) {
            fluffyvm_set_errmsg(vm, vm->staticStrings.attemptToCallNilValue);
            goto error;
          }

          if (!value_is_callable(val)) {
            fluffyvm_set_errmsg(vm, vm->staticStrings.attemptToCallNonCallableValue);
            goto error;
          }
          closure = val.data.closure;

          if (!coroutine_function_prolog(vm, closure))
            goto error;
          fluffyvm_clear_errmsg(vm);

          if (ins.D == 1)
            argsEnd = callState->sp-1;

          // Copying the arguments
          for (int i = argsStart; i <= argsEnd; i++)
            if (!interpreter_push(vm, co->currentCallState, callState->generalStack[i]))
              goto call_error;

          callState->pc = pc;
          call_status_t result = exec(vm, co);
          if (result == INTERPRETER_ERROR)
            goto error;

          if (ins.B == 0) {
            returnCount = 0;
          } else if (ins.B == 1) {
            returnCount = co->currentCallState->sp;
          }

          int startPos = co->currentCallState->sp - returnCount;
          if (startPos < 0)
            startPos = 0;
         
          // Copying the return values
          for (int i = 0; i < returnCount; i++) {
            struct value val = value_nil();

            // Copy only if current pos is valid
            if (startPos + i <= co->currentCallState->sp - 1)
              value_copy(&val, &co->currentCallState->generalStack[startPos + i]);

            if (!interpreter_push(vm, callState, val)) {
              goto call_error;
            }
          }


          coroutine_function_epilog(vm);
          break;

          call_error:
          coroutine_function_epilog(vm);
          goto error;
        }
      case FLUFFYVM_OPCODE_TABLE_SET:
        printf("0x%08X: R(%d)[R(%d)] = R(%d)\n", pc, ins.A, ins.B, ins.C);
        {
          struct value table = getRegister(vm, callState, ins.A);
          struct value key = getRegister(vm, callState, ins.B);
          struct value value = getRegister(vm, callState, ins.C);

          value_table_set(vm, table, key, value);
          break;
        }
      case FLUFFYVM_OPCODE_RETURN:
        printf("0x%08X: ret(R(%d)..R(%d))\n", pc, ins.A, ins.A + ins.B - 1);
        goto done_function; 
      case FLUFFYVM_OPCODE_NOP:
        printf("0x%08X: nop\n", pc);
        break;

      case FLUFFYVM_OPCODE_EXTRA:
        goto illegal_instruction;
      case FLUFFYVM_OPCODE_LAST:
        abort(); /* Unreachable */
    }
    setRegister(vm, callState, FLUFFYVM_INTERPRETER_REGISTER_ALWAYS_NIL, value_nil());

    pc += incrementCount;
    callState->pc = pc;
  }

  done_function:
  callState->pc = pc;
  return INTERPRETER_OK;

  illegal_instruction:
  fluffyvm_set_errmsg(vm, vm->staticStrings.illegalInstruction);
  error:
  callState->pc = pc;
  interpreter_error(vm, callState, fluffyvm_get_errmsg(vm));
  
  // Can't be reached
  abort();
  return 0;
}

bool interpreter_exec(struct fluffyvm* vm, struct fluffyvm_coroutine* co) {
  call_status_t result = exec(vm, co);
   
  coroutine_function_epilog(vm); 
  return result == INTERPRETER_OK;
}

bool interpreter_peek(struct fluffyvm* vm, struct fluffyvm_call_state* callState, int index, struct value* result) {
  if (index < 0 || index >= callState->sp)
    return false;
  value_copy(result, &callState->generalStack[index]);
  return true;
}

int interpreter_get_top(struct fluffyvm* vm, struct fluffyvm_call_state* callState) {
  return callState->sp - 1;
}

struct value interpreter_get_env(struct fluffyvm* vm, struct fluffyvm_call_state* callState) {
  return callState->closure->env;
}

void interpreter_error(struct fluffyvm* vm, struct fluffyvm_call_state* callState, struct value errmsg) {
  fluffyvm_set_errmsg(vm, errmsg);
  struct fluffyvm_coroutine* co = fluffyvm_get_executing_coroutine(vm);
  assert(co);
  
  longjmp(*co->errorHandler, 1);
}


