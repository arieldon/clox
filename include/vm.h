#ifndef VM_H
#define VM_H

#include "chunk.h"
#include "object.h"
#include "table.h"
#include "value.h"

#define FRAMES_MAX 64
#define STACK_MAX  (FRAMES_MAX * UINT8_COUNT)

typedef struct {
    ObjClosure *closure;

    // Because callers store their own instruction pointers, upon return from a
    // function the VM jumps to the address to which the caller's CallFrame
    // instruction points.
    uint8_t *ip;

    // Point to stack of values maintained by VM.
    Value *slots;
} CallFrame;

typedef struct {
    // Each CallFrame maintains its own instruction pointer.
    CallFrame frames[FRAMES_MAX];

    // Track height of CallFrame stack.
    int frame_count;

    Value stack[STACK_MAX];

    // Like the instruction pointer, this pointer points to the address where
    // the next item is to be pushed onto the stack.
    Value *stack_top;

    // Store global variables and their corresponding values.
    Table globals;

    // Maintain a table of distinct strings to deduplicate strings and enable
    // their comparison within the VM using the equality operator `==` rather
    // than memcmp(). Although the type is technically a hash table, it's used
    // as a set.
    Table strings;

    ObjUpvalue *open_upvalues;

    // Track the total number of bytes currently allocated by the VM and the
    // threshold to trigger the next collection.
    size_t bytes_allocated;
    size_t next_gc;

    // Maintain a linked list of objects, which may be dynamically allocated,
    // to free and thus prevent memory leaks.
    Obj *objects;

    // The stack of gray objects represents the working set of objects marked
    // to be kept during garbage collection because they're reachable. The
    // references within these gray objects are yet to be traversed though, so
    // they're not marked black.
    int gray_count;
    int gray_capacity;
    Obj **gray_stack;
} VM;

typedef enum {
    INTERPRET_OK,
    INTERPRET_COMPILE_ERROR,
    INTERPRET_RUNTIME_ERROR,
} InterpretResult;

extern VM vm;

void initVM(void);
void freeVM(void);
InterpretResult interpret(const char *source);
void push(Value value);
Value pop(void);

#endif
