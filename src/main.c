#include <assert.h>
#include <stdio.h>

#include "chunk.h"
#include "common.h"
#include "debug.h"

int
main(int argc, char *argv[])
{
    Chunk chunk;
    initChunk(&chunk);
    writeChunk(&chunk, OP_RETURN);

    // XXX The size of `constant` is larger than 1 byte.
    int constant = addConstant(&chunk, 1.2);
    assert(constant < 256);
    writeChunk(&chunk, OP_CONSTANT);
    writeChunk(&chunk, constant);

    disassembleChunk(&chunk, "test chunk");
    freeChunk(&chunk);
    return 0;
}
