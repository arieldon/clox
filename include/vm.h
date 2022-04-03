#ifndef VM_H
#define VM_H

#include "chunk.h"
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
} VM;

typedef enum {
    INTERPRET_OK,
    INTERPRET_COMPILE_ERROR,
    INTERPRET_RUNTIME_ERROR,
} InterpretResult;

void initVM(void);
void freeVM(void);
InterpretResult interpret(Chunk *chunk);
void push(Value value);
Value pop(void);

#endif
