#ifndef VM_H
#define VM_H

#include "chunk.h"
#include "table.h"
#include "value.h"

#define STACK_MAX 256

typedef struct {
    Chunk *chunk;

    // Use a pointer instead of an index since it's faster to dereference a
    // pointer than to access a value in an array by index.
    // The instruction pointer (IP) points to the next instruction to execute.
    uint8_t *ip;

    Value stack[STACK_MAX];

    // Like the instruction pointer, this pointer points to the address where
    // the next item is to be pushed onto the stack.
    Value *stack_top;

    // Maintain a table of distinct strings to deduplicate strings and enable
    // their comparison within the VM using the equality operator `==` rather
    // than memcmp(). Although the type is technically a hash table, it's used
    // as a set.
    Table strings;

    // Maintain a linked list of objects, which may be dynamically allocated,
    // to free and thus prevent memory leaks.
    Obj *objects;
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
