#ifndef OBJECT_H
#define OBJECT_H

#include "common.h"
#include "chunk.h"
#include "value.h"

#define OBJ_TYPE(value)  (AS_OBJ(value)->type)

#define IS_CLOSURE(value)   isObjType(value, OBJ_CLOSURE)
#define IS_FUNCTION(value)  isObjType(value, OBJ_FUNCTION)
#define IS_NATIVE(value)    isObjType(value, OBJ_NATIVE)
#define IS_STRING(value)    isObjType(value, OBJ_STRING)

#define AS_CLOSURE(value)   ((ObjClosure *)AS_OBJ(value))
#define AS_FUNCTION(value)  ((ObjFunction *)AS_OBJ(value))
#define AS_NATIVE(value)    (((ObjNative *)AS_OBJ(value))->function)
#define AS_STRING(value)    ((ObjString *)AS_OBJ(value))
#define AS_CSTRING(value)   (((ObjString *)AS_OBJ(value))->chars)

typedef enum {
    OBJ_CLOSURE,
    OBJ_FUNCTION,
    OBJ_NATIVE,
    OBJ_STRING,
    OBJ_UPVALUE,
} ObjType;

struct Obj {
    ObjType type;
    struct Obj *next;
};

typedef struct {
    Obj obj;
    int arity;
    int upvalue_count;
    Chunk chunk;
    ObjString *name;
} ObjFunction;

typedef Value (*NativeFn)(int arg_count, Value *args);

// This implementation treats native functions differently from regular
// functions because native functions do not have bytecode for the VM to
// execute. Instead, they reference native C code, and they differ in structure
// as a result.
typedef struct {
    Obj obj;
    NativeFn function;
} ObjNative;

struct ObjString {
    Obj obj;
    int length;
    char *chars;
    uint32_t hash;
};

typedef struct ObjUpvalue {
    Obj obj;
    Value *location;
    Value closed;
    struct ObjUpvalue *next;
} ObjUpvalue;

typedef struct {
    Obj obj;
    ObjFunction *function;
    ObjUpvalue **upvalues;
    int upvalue_count;
} ObjClosure;

ObjClosure *newClosure(ObjFunction *function);
ObjFunction *newFunction(void);
ObjNative *newNative(NativeFn function);

// Define as a function rather than a macro because it uses `value` twice. If
// the argument passed as `value` produces side effects, it doesn't execute
// them more than once this way.
static inline bool
isObjType(Value value, ObjType type)
{
    return IS_OBJ(value) && AS_OBJ(value)->type == type;
}

ObjString *takeString(char *chars, int length);
ObjString *copyString(const char *chars, int length);
ObjUpvalue *newUpvalue(Value *slot);
void printObject(Value value);

#endif
