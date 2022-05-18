#include <stdlib.h>

#include "chunk.h"
#include "memory.h"
#include "vm.h"

void
initChunk(Chunk *chunk)
{
    chunk->count = 0;
    chunk->capacity = 0;
    chunk->code = NULL;
    chunk->line_count = 0;
    chunk->line_capacity = 0;
    chunk->lines = NULL;
    initValueArray(&chunk->constants);
}

void
freeChunk(Chunk *chunk)
{
    FREE_ARRAY(uint8_t, chunk->code, chunk->capacity);
    FREE_ARRAY(Line, chunk->lines, chunk->line_capacity);
    freeValueArray(&chunk->constants);
    initChunk(chunk);
}

void
writeChunk(Chunk *chunk, uint8_t byte, int line)
{
    if (chunk->capacity < chunk->count + 1) {
        int old_capacity = chunk->capacity;
        chunk->capacity = GROW_CAPACITY(old_capacity);
        chunk->code = GROW_ARRAY(uint8_t, chunk->code, old_capacity, chunk->capacity);
    }
    chunk->code[chunk->count++] = byte;

    if (chunk->line_count > 0 && chunk->lines[chunk->line_count - 1].line_number == line) {
        // Current instruction sits on same line as previous instruction -- no
        // need to append another line.
        return;
    }

    if (chunk->line_capacity < chunk->line_count + 1) {
        int old_line_capacity = chunk->line_capacity;
        chunk->line_capacity = GROW_CAPACITY(old_line_capacity);
        chunk->lines = GROW_ARRAY(Line, chunk->lines, old_line_capacity, chunk->line_capacity);
    }
    chunk->lines[chunk->line_count++] = (Line){
        .instruction_offset = chunk->count - 1,
        .line_number = line,
    };
}

int
addConstant(Chunk *chunk, Value value)
{
    // Push and pop to prevent the garbage collector from sweeping the value in
    // case of reallocate().
    push(value);
    writeValueArray(&chunk->constants, value);
    pop();
    return chunk->constants.count - 1;
}

int
getLine(Chunk *chunk, int instruction_offset)
{
    for (int i = 0; i < chunk->line_count - 1; ++i) {
        Line *current_line = &chunk->lines[i];
        Line *next_line = &chunk->lines[i + 1];
        if (instruction_offset >= current_line->instruction_offset
                && instruction_offset < next_line->instruction_offset) {
            return current_line->line_number;
        }
    }
    return chunk->lines[chunk->line_count - 1].line_number;
}
