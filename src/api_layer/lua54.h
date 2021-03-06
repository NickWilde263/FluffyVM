#ifndef header_1653536277_lua54_h
#define header_1653536277_lua54_h

// Compatibility layer for Lua 5.4 C API
// From C API section: https://www.lua.org/manual/5.4/contents.html#index

#ifdef lua_h
# error "Don't include 'lua.h' file; This compat layer serve as replacement for 'lua.h'"
#endif

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "types.h"
#include "config.h"

// Types
typedef fluffyvm_coroutine lua_State;
typedef int (*lua_CFunction)(lua_State*);
typedef fluffyvm_integer lua_Integer;
typedef fluffyvm_number lua_Number;
typedef intptr_t lua_KContext;
typedef int (*lua_KFunction)(lua_State* L, int status, lua_KContext ctx);
typedef fluffyvm_unsigned lua_Unsigned;

// Macros
#define FLUFFYVM_DECLARE(ret, name, ...) ret fluffyvm_compat_lua54_ ## name(__VA_ARGS__)
#define LUA_MULTRET (-1)

// Lua status
enum {
  LUA_OK,
  LUA_ERRRUN,
  LUA_ERRMEM,
  LUA_ERRERR,
  LUA_ERRSYNTAX,
  LUA_YIELD,
  LUA_ERRFILE
};

// Lua types
typedef enum {
  LUA_TNONE,
  LUA_TNIL,
  LUA_TNUMBER,
  LUA_TBOOLEAN,
  LUA_TSTRING,
  LUA_TTABLE,
  LUA_TFUNCTION,
  LUA_TUSERDATA,
  LUA_TTHREAD,
  LUA_TLIGHTUSERDATA
} lua_Type;

// Pseudo indexes
#define FLUFFYVM_COMPAT_LAYER_PSEUDO_INDEX(loc) (0x80000000 | loc)
#define FLUFFYVM_COMPAT_LAYER_IS_PSEUDO_INDEX(idx) ((idx & 0x8000000) != 0)
#define LUA_REGISTRYINDEX FLUFFYVM_COMPAT_LAYER_PSEUDO_INDEX(1)

// Registry indexes
#define LUA_RIDX_MAINTHREAD (0)
#define LUA_RIDX_GLOBALS (1)

// Declarations
FLUFFYVM_DECLARE(void, lua_call, lua_State* L, int nargs, int nresults); 
FLUFFYVM_DECLARE(int, lua_checkstack, lua_State* L, int n); 
FLUFFYVM_DECLARE(int, lua_gettop, lua_State* L); 
FLUFFYVM_DECLARE(void, lua_copy, lua_State* L, int fromidx, int toidx); 
FLUFFYVM_DECLARE(int, lua_absindex, lua_State* L, int idx); 
FLUFFYVM_DECLARE(void, lua_pop, lua_State* L, int n); 
FLUFFYVM_DECLARE(void, lua_remove, lua_State* L, int idx); 
FLUFFYVM_DECLARE(void, lua_pushnil, lua_State* L); 
FLUFFYVM_DECLARE(const char*, lua_pushstring, lua_State* L, const char* s); 
FLUFFYVM_DECLARE(const char*, lua_pushliteral, lua_State* L, const char* s); 
FLUFFYVM_DECLARE(void, lua_error, lua_State* L); 
FLUFFYVM_DECLARE(void, lua_pushvalue, lua_State* L, int idx); 
FLUFFYVM_DECLARE(const char*, lua_tolstring, lua_State* L, int idx, size_t* len); 
FLUFFYVM_DECLARE(const char*, lua_tostring, lua_State* L, int idx); 
FLUFFYVM_DECLARE(const void*, lua_topointer, lua_State* L, int idx); 
FLUFFYVM_DECLARE(lua_Number, lua_tonumberx, lua_State* L, int idx, int* isnum); 
FLUFFYVM_DECLARE(lua_Number, lua_tonumber, lua_State* L, int idx); 
FLUFFYVM_DECLARE(lua_Integer, lua_tointegerx, lua_State* L, int idx, int* isnum); 
FLUFFYVM_DECLARE(lua_Integer, lua_tointeger, lua_State* L, int idx); 
FLUFFYVM_DECLARE(void*, lua_touserdata, lua_State* L, int idx); 
FLUFFYVM_DECLARE(int, lua_type, lua_State* L, int idx); 
FLUFFYVM_DECLARE(const char*, lua_typename, lua_State* L, int idx); 
FLUFFYVM_DECLARE(lua_Number, lua_version, lua_State* L); 
FLUFFYVM_DECLARE(void, lua_replace, lua_State* L, int idx); 
FLUFFYVM_DECLARE(int, lua_isboolean, lua_State* L, int idx); 
FLUFFYVM_DECLARE(int, lua_iscfunction, lua_State* L, int idx); 
FLUFFYVM_DECLARE(int, lua_isfunction, lua_State* L, int idx); 
FLUFFYVM_DECLARE(int, lua_isinteger, lua_State* L, int idx); 
FLUFFYVM_DECLARE(int, lua_islightuserdata, lua_State* L, int idx); 
FLUFFYVM_DECLARE(int, lua_isnil, lua_State* L, int idx); 
FLUFFYVM_DECLARE(int, lua_isnone, lua_State* L, int idx); 
FLUFFYVM_DECLARE(int, lua_isnumber, lua_State* L, int idx); 
FLUFFYVM_DECLARE(int, lua_isstring, lua_State* L, int idx); 
FLUFFYVM_DECLARE(int, lua_istable, lua_State* L, int idx); 
FLUFFYVM_DECLARE(int, lua_isthread, lua_State* L, int idx); 
FLUFFYVM_DECLARE(int, lua_isuserdata, lua_State* L, int idx); 
FLUFFYVM_DECLARE(int, lua_isyieldable, lua_State* L); 
FLUFFYVM_DECLARE(void, lua_len, lua_State* L, int idx); 
FLUFFYVM_DECLARE(void, lua_createtable, lua_State* L, int narr, int nrec); 
FLUFFYVM_DECLARE(void, lua_newtable, lua_State* L); 
FLUFFYVM_DECLARE(lua_State*, lua_newthread, lua_State* L); 
FLUFFYVM_DECLARE(int, lua_resume, lua_State* L, lua_State* from, int nargs, int* nresults); 
FLUFFYVM_DECLARE(void, lua_pushlightuserdata, lua_State* L, void* ptr); 
FLUFFYVM_DECLARE(void, lua_pushinteger, lua_State* L, lua_Integer integer); 
FLUFFYVM_DECLARE(void, lua_pushnumber, lua_State* L, lua_Number integer); 
FLUFFYVM_DECLARE(void, lua_pushcfunction, lua_State* L, lua_CFunction f); 
FLUFFYVM_DECLARE(void, lua_rotate, lua_State* L, int idx, int n); 
FLUFFYVM_DECLARE(const char*, lua_pushlstring, lua_State* L, const char* s, size_t len); 
FLUFFYVM_DECLARE(void, lua_pushglobaltable, lua_State* L); 
FLUFFYVM_DECLARE(int, lua_pushthread, lua_State* L); 
FLUFFYVM_DECLARE(void, lua_xmove, lua_State* L, lua_State* to, int n); 
FLUFFYVM_DECLARE(lua_State*, lua_tothread, lua_State* L, int idx); 
FLUFFYVM_DECLARE(int, lua_toboolean, lua_State* L, int idx); 
FLUFFYVM_DECLARE(lua_CFunction, lua_tocfunction, lua_State* L, int idx); 
FLUFFYVM_DECLARE(void, lua_pushboolean, lua_State* L, int b);
FLUFFYVM_DECLARE(void, lua_setglobal, lua_State* L, const char* name);
FLUFFYVM_DECLARE(void, lua_setfield, lua_State* L, int tableIndex, const char* name);
FLUFFYVM_DECLARE(void, lua_register, lua_State* L, const char* name, lua_CFunction cfunc);

//FLUFFYVM_DECLARE(void, lua_callk, lua_State* L, int nargs, int nresults); 

#ifdef FLUFFYVM_INTERNAL
bool fluffyvm_compat_layer_lua54_init(struct fluffyvm* F);
void fluffyvm_compat_layer_lua54_cleanup(struct fluffyvm* F);
#endif

#ifndef FLUFFYVM_INTERNAL
# ifdef FLUFFYVM_COMPAT_LAYER_REDIRECT_CALL

# endif

# ifdef FLUFFYVM_API_DEBUG_C_FUNCTION
#   ifndef FLUFFYVM_COMPAT_LAYER_INSERT_DEBUG_INFO
#     define FLUFFYVM_COMPAT_LAYER_INSERT_DEBUG_INFO(func, F, ...) do {\
        struct fluffyvm_coroutine* _co = (F) ;\
        struct fluffyvm* _vm = (_co->owner); \
        coroutine_set_debug_info(_vm, __FILE__, __func__, __LINE__); \
        func(_co, __VA_ARGS__); \
        coroutine_set_debug_info(_vm, NULL, NULL, -1); \
      } while(0)
#   endif
#   define fluffyvm_compat_lua54_lua_call(F, ...) FLUFFYVM_COMPAT_LAYER_INSERT_DEBUG_INFO(fluffyvm_compat_lua54_lua_call, F, __VA_ARGS__)
#   define fluffyvm_compat_lua54_lua_checkstack(F, ...) FLUFFYVM_COMPAT_LAYER_INSERT_DEBUG_INFO(fluffyvm_compat_lua54_lua_checkstack, F, __VA_ARGS__)
# endif
#endif

#endif









