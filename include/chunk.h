#ifndef CHUNK_H
#define CHUNK_H

#include "common.h"
#include "value.h"

typedef enum {
    OP_CONSTANT,
    OP_NIL,
    OP_TRUE,
    OP_FALSE,
    OP_EQUAL,
    OP_GET_SUPER,
    OP_GET_PROPERTY,
    OP_SET_PROPERTY,
    OP_GET_UPVALUE,
    OP_SET_UPVALUE,
    OP_GET_LOCAL,
    OP_SET_LOCAL,
    OP_GET_GLOBAL,
    OP_SET_GLOBAL,
    OP_DEFINE_GLOBAL,
    OP_POP,
    OP_GREATER,
    OP_LESSER,
    OP_ADD,
    OP_SUBTRACT,
    OP_MULTIPLY,
    OP_DIVIDE,
    OP_NOT,
    OP_NEGATE,
    OP_PRINT,
    OP_JUMP,
    OP_JUMP_IF_FALSE,
    OP_LOOP,
    OP_CALL,
    OP_INVOKE,
    OP_SUPER_INVOKE,
    OP_CLOSURE,
    OP_CLOSE_UPVALUE,
    OP_RETURN,
    OP_CLASS,
    OP_INHERIT,
    OP_METHOD,
} OpCode;

typedef struct {
    int instruction_offset; // Store offset into chunk of first instruction of line.
    int line_number;
} Line;

// A chunk consists of a contiguous series of bytecode instructions and their
// constants. It also maps line numbers to instructions in a run-length encoded
// format for debug and error messages.

typedef struct {
    // Store code and data.
    int count;
    int capacity;
    uint8_t *code;
    ValueArray constants;

    // Track line of each instruction for error and debugging messages.
    int line_count;
    int line_capacity;
    Line *lines;
} Chunk;

void initChunk(Chunk *chunk);
void freeChunk(Chunk *chunk);
void writeChunk(Chunk *chunk, uint8_t byte, int line);
int addConstant(Chunk *chunk, Value value);
int getLine(Chunk *chunk, int instruction);

#endif
