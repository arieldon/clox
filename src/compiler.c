#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "compiler.h"
#include "object.h"
#include "scanner.h"
#include "value.h"

#ifdef DEBUG_PRINT_CODE
#include "debug.h"
#endif

// Declare as global for ease of use -- no need to pass a pointer around for
// all functions this way.
Parser parser;
Compiler *current = NULL;

static Chunk *
currentChunk(void)
{
    return &current->function->chunk;
}

static void
errorAt(Token *token, const char *message)
{
    // Suppress errors when the parser is in its panic state to mitigate error
    // cascades.
    if (parser.panic_mode) return;
    parser.panic_mode = true;

    fprintf(stderr, "[line %d] error", token->line);

    if (token->type != TOKEN_EOF) {
        fprintf(stderr, " at '%.*s'", token->length, token->start);
    }

    fprintf(stderr, ": %s\n", message);
    parser.had_error = true;
}

static void
error(const char *message)
{
    errorAt(&parser.previous, message);
}

static void
errorAtCurrent(const char *message)
{
    errorAt(&parser.current, message);
}

static void
advance(void)
{
    parser.previous = parser.current;

    for (;;) {
        // Scan source to output next token on demand.
        parser.current = scanToken();
        if (parser.current.type != TOKEN_ERROR) break;

        errorAtCurrent(parser.current.start);
    }
}

static void
consume(TokenType type, const char *message)
{
    if (parser.current.type == type) {
        advance();
        return;
    }
    errorAtCurrent(message);
}

static bool
check(TokenType type)
{
    return parser.current.type == type;
}

static bool
match(TokenType type)
{
    if (!check(type)) return false;
    advance();
    return true;
}

static void
emitByte(uint8_t byte)
{
    writeChunk(currentChunk(), byte, parser.previous.line);
}

static void
emitBytes(uint8_t byte1, uint8_t byte2)
{
    emitByte(byte1);
    emitByte(byte2);
}

static void
emitLoop(int loop_start)
{
    emitByte(OP_LOOP);

    // Calculate instruction offset of loop start; +2 to accont for OP_LOOP
    // instruction.
    int offset = currentChunk()->count - loop_start + 2;
    if (offset > UINT16_MAX) error("loop body too large");

    emitByte((offset >> 8) & 0xff);
    emitByte(offset & 0xff);
}

static int
emitJump(uint8_t instruction)
{
    // Write placeholder operand for jump offset. Once the size of the block is
    // known, backpatch and modify this placeholder value to proper value to
    // skip the block.
    emitByte(instruction);
    emitByte(0xff);
    emitByte(0xff);
    return currentChunk()->count - 2;
}

static void
emitReturn(void)
{
    emitByte(OP_RETURN);
}

static uint8_t
makeConstant(Value value)
{
    int constant = addConstant(currentChunk(), value);
    if (constant > UINT8_MAX) {
        error("too many constants in one chunk");
        return 0;
    }
    return (uint8_t)constant;
}

static void
emitConstant(Value value)
{
    emitBytes(OP_CONSTANT, makeConstant(value));
}

static void
patchJump(int offset)
{
    // Backpatch and replace operand after emitJump() with proper value to skip
    // a block.
    int jump = currentChunk()->count - offset - 2;

    if (jump > UINT16_MAX) {
        error("too much code to jump over");
    }

    currentChunk()->code[offset] = (jump >> 8) & 0xff;
    currentChunk()->code[offset + 1] = jump & 0xff;
}

static void
initCompiler(Compiler *compiler, FunctionType type)
{
    compiler->function = NULL;
    compiler->type = type;
    compiler->local_count = 0;
    compiler->scope_depth = 0;
    compiler->function = newFunction();
    current = compiler;

    // The compiler uses slot zero of the array that tracks local variables and
    // other temporary values.
    Local *local = &current->locals[current->local_count++];
    local->depth = 0;
    local->name.start = "";
    local->name.length = 0;
}

static ObjFunction *
endCompile(void)
{
    emitReturn();
    ObjFunction *function = current->function;

#ifdef DEBUG_PRINT_CODE
    if (!parser.had_error) {
        disassembleChunk(currentChunk(), function->name != NULL
            ? function->name->chars : "<script>");
    }
#endif

    return function;
}

static void
beginScope(void)
{
    ++current->scope_depth;
}

static void
endScope(void)
{
    --current->scope_depth;

    while (current->local_count > 0 &&
           current->locals[current->local_count - 1].depth > current->scope_depth) {
        emitByte(OP_POP);
        --current->local_count;
    }
}

// TODO Reorganize this file to prevent the need to forward declare all of
// these.
static void binary(bool can_assign);
static void literal(bool can_assign);
static void grouping(bool can_assign);
static void number(bool can_assign);
static void string(bool can_assign);
static void variable(bool can_assign);
static void unary(bool can_assign);
static void and(bool can_assign);
static void or(bool can_assign);
static void declaration(void);

ParseRule rules[] = {
    [TOKEN_LEFT_PAREN]    = {grouping, NULL,   PREC_NONE},
    [TOKEN_RIGHT_PAREN]   = {NULL,     NULL,   PREC_NONE},
    [TOKEN_LEFT_BRACE]    = {NULL,     NULL,   PREC_NONE},
    [TOKEN_RIGHT_BRACE]   = {NULL,     NULL,   PREC_NONE},
    [TOKEN_COMMA]         = {NULL,     NULL,   PREC_NONE},
    [TOKEN_DOT]           = {NULL,     NULL,   PREC_NONE},
    [TOKEN_MINUS]         = {unary,    binary, PREC_TERM},
    [TOKEN_PLUS]          = {NULL,     binary, PREC_TERM},
    [TOKEN_SEMICOLON]     = {NULL,     NULL,   PREC_NONE},
    [TOKEN_SLASH]         = {NULL,     binary, PREC_FACTOR},
    [TOKEN_STAR]          = {NULL,     binary, PREC_FACTOR},
    [TOKEN_BANG]          = {unary,    NULL,   PREC_NONE},
    [TOKEN_BANG_EQUAL]    = {NULL,     binary, PREC_EQUALITY},
    [TOKEN_EQUAL]         = {NULL,     NULL,   PREC_NONE},
    [TOKEN_EQUAL_EQUAL]   = {NULL,     binary, PREC_EQUALITY},
    [TOKEN_GREATER]       = {NULL,     binary, PREC_COMPARISON},
    [TOKEN_GREATER_EQUAL] = {NULL,     binary, PREC_COMPARISON},
    [TOKEN_LESSER]        = {NULL,     binary, PREC_COMPARISON},
    [TOKEN_LESSER_EQUAL]  = {NULL,     binary, PREC_COMPARISON},
    [TOKEN_IDENTIFIER]    = {variable, NULL,   PREC_NONE},
    [TOKEN_STRING]        = {string,   NULL,   PREC_NONE},
    [TOKEN_NUMBER]        = {number,   NULL,   PREC_NONE},
    [TOKEN_AND]           = {NULL,     and,    PREC_AND},
    [TOKEN_CLASS]         = {NULL,     NULL,   PREC_NONE},
    [TOKEN_ELSE]          = {NULL,     NULL,   PREC_NONE},
    [TOKEN_FALSE]         = {literal,  NULL,   PREC_NONE},
    [TOKEN_FOR]           = {NULL,     NULL,   PREC_NONE},
    [TOKEN_FUN]           = {NULL,     NULL,   PREC_NONE},
    [TOKEN_IF]            = {NULL,     NULL,   PREC_NONE},
    [TOKEN_NIL]           = {literal,  NULL,   PREC_NONE},
    [TOKEN_OR]            = {NULL,     or,     PREC_OR},
    [TOKEN_PRINT]         = {NULL,     NULL,   PREC_NONE},
    [TOKEN_RETURN]        = {NULL,     NULL,   PREC_NONE},
    [TOKEN_SUPER]         = {NULL,     NULL,   PREC_NONE},
    [TOKEN_THIS]          = {NULL,     NULL,   PREC_NONE},
    [TOKEN_TRUE]          = {literal,  NULL,   PREC_NONE},
    [TOKEN_VAR]           = {NULL,     NULL,   PREC_NONE},
    [TOKEN_WHILE]         = {NULL,     NULL,   PREC_NONE},
    [TOKEN_ERROR]         = {NULL,     NULL,   PREC_NONE},
    [TOKEN_EOF]           = {NULL,     NULL,   PREC_NONE},
};

static ParseRule *
getRule(TokenType type)
{
    return &rules[type];
}

static void
parsePrecedence(Precedence precedence)
{
    advance();
    ParseFn prefix_rule = getRule(parser.previous.type)->prefix;
    if (prefix_rule == NULL) {
        error("expect expression");
        return;
    }

    bool can_assign = precedence <= PREC_ASSIGNMENT;
    prefix_rule(can_assign);

    while (precedence <= getRule(parser.current.type)->precedence) {
        advance();
        ParseFn infix_rule = getRule(parser.previous.type)->infix;
        infix_rule(can_assign);
    }

    if (can_assign && match(TOKEN_EQUAL)) {
        error("invalid assignment target");
    }
}

static uint8_t
identifierConstant(Token *name)
{
    return makeConstant(OBJ_VAL(copyString(name->start, name->length)));
}

static bool
identifiersEqual(Token *a, Token *b)
{
    if (a->length != b->length) return false;
    return memcmp(a->start, b->start, a->length) == 0;
}

static int
resolveLocal(Compiler *compiler, Token *name)
{
    for (int i = compiler->local_count - 1; i >= 0; --i) {
        Local *local = &compiler->locals[i];
        if (identifiersEqual(name, &local->name)) {
            if (local->depth == -1) {
                error("cannot read local variable in its own initializer");
            }
            return i;
        }
    }
    return -1;
}

static void
addLocal(Token name)
{
    if (current->local_count == UINT8_COUNT) {
        error("too many local variables in function");
        return;
    }
    Local *local = &current->locals[current->local_count++];
    local->name = name;
    local->depth = -1;
}

static void
declareVariable(void)
{
    // Only declare local variables like this. Globals are late bound, so it's
    // not necessary to track their declarations.
    if (current->scope_depth == 0) return;

    Token *name = &parser.previous;
    for (int i = current->local_count - 1; i >= 0; --i) {
        Local *local = &current->locals[i];
        if (local->depth != -1 && local->depth < current->scope_depth) {
            break;
        }

        if (identifiersEqual(name, &local->name)) {
            error("a variable with this name already exists within this scope");
        }
    }

    addLocal(*name);
}

static uint8_t
parseVariable(const char *error_message)
{
    consume(TOKEN_IDENTIFIER, error_message);

    declareVariable();
    if (current->scope_depth > 0) return 0;

    return identifierConstant(&parser.previous);
}

static void
markInitialized(void)
{
    current->locals[current->local_count - 1].depth = current->scope_depth;
}

static void
defineVariable(uint8_t global)
{
    if (current->scope_depth > 0) {
        // Do not create local variables at runtime. Instead, they're created
        // during compilation.
        markInitialized();
        return;
    }
    emitBytes(OP_DEFINE_GLOBAL, global);
}

static void
and(bool can_assign)
{
    (void)can_assign;

    int end_jump = emitJump(OP_JUMP_IF_FALSE);

    emitByte(OP_POP);
    parsePrecedence(PREC_AND);

    patchJump(end_jump);
}

static void
or(bool can_assign)
{
    (void)can_assign;

    int else_jump = emitJump(OP_JUMP_IF_FALSE);
    int end_jump = emitJump(OP_JUMP);

    patchJump(else_jump);
    emitByte(OP_POP);

    parsePrecedence(PREC_OR);
    patchJump(end_jump);
}

static void
expression(void)
{
    // Parse lowest precedence level to subsume higher precedence expressions
    // as well.
    parsePrecedence(PREC_ASSIGNMENT);
}

static void
block(void)
{
    while (!check(TOKEN_RIGHT_BRACE) && !check(TOKEN_EOF)) {
        declaration();
    }
    consume(TOKEN_RIGHT_BRACE, "expect '}' after block");
}

static void
varDeclaration(void)
{
    uint8_t global = parseVariable("expect variable name");

    if (match(TOKEN_EQUAL)) {
        expression();
    } else {
        emitByte(OP_NIL);
    }
    consume(TOKEN_SEMICOLON, "expect ';' after variable declaration");

    defineVariable(global);
}

static void
printStatement(void)
{
    expression();
    consume(TOKEN_SEMICOLON, "expect ';' after value");
    emitByte(OP_PRINT);
}

static void
synchronize(void)
{
    parser.panic_mode = false;

    while (parser.current.type != TOKEN_EOF) {
        // A semicolon indiciates the end of a statement.
        if (parser.previous.type == TOKEN_SEMICOLON) return;
        switch (parser.current.type) {
            case TOKEN_CLASS:
            case TOKEN_FUN:
            case TOKEN_VAR:
            case TOKEN_FOR:
            case TOKEN_IF:
            case TOKEN_WHILE:
            case TOKEN_PRINT:
            case TOKEN_RETURN:
                // These tokens signify the beginning of a new statement.
                return;
            default:
                ;
        }
        advance();
    }
}

static void statement(void);

static void
ifStatement(void)
{
    consume(TOKEN_LEFT_PAREN, "expect '(' after if");
    expression();
    consume(TOKEN_RIGHT_PAREN, "expect '(' after if");

    int then_jump = emitJump(OP_JUMP_IF_FALSE);
    emitByte(OP_POP);
    statement();

    int else_jump = emitJump(OP_JUMP);

    patchJump(then_jump);
    emitByte(OP_POP);

    if (match(TOKEN_ELSE)) statement();
    patchJump(else_jump);
}

static void
whileStatement(void)
{
    int loop_start = currentChunk()->count;

    consume(TOKEN_LEFT_PAREN, "expect '(' after 'while'");
    expression();
    consume(TOKEN_RIGHT_PAREN, "expect ')' after condition");

    int exit_jump = emitJump(OP_JUMP_IF_FALSE);
    emitByte(OP_POP);
    statement();
    emitLoop(loop_start);

    patchJump(exit_jump);
    emitByte(OP_POP);
}

static void
forStatement(void)
{
    // Wrap entire for loop in its own scope to ensure any variable declared in
    // its clauses is scoped to loop body.
    beginScope();

    consume(TOKEN_LEFT_PAREN, "expect '(' after 'for'");
    if (match(TOKEN_SEMICOLON)) {
        ; // An immediate semicolon indicates the absence of an initializer.
    } else if (match(TOKEN_VAR)) {
        varDeclaration();
    } else {
        expression();
    }

    int loop_start = currentChunk()->count;
    int exit_jump = -1;
    if (!match(TOKEN_SEMICOLON)) {
        expression();
        consume(TOKEN_SEMICOLON, "expect ';' after loop condition");

        // Jump to end of loop if condition false.
        exit_jump = emitJump(OP_JUMP_IF_FALSE);
        emitByte(OP_POP);
    }

    if (!match(TOKEN_RIGHT_PAREN)) {
        int body_jump = emitJump(OP_JUMP);
        int increment_start = currentChunk()->count;
        expression();
        emitByte(OP_POP);
        consume(TOKEN_RIGHT_PAREN, "expect ')' after for clauses");

        emitLoop(loop_start);
        loop_start = increment_start;
        patchJump(body_jump);
    }

    statement();
    emitLoop(loop_start);

    if (exit_jump != -1) {
        patchJump(exit_jump);
        emitByte(OP_POP);
    }

    endScope();
}

static void
expressionStatement(void)
{
    expression();
    consume(TOKEN_SEMICOLON, "expect ';' after expression");
    emitByte(OP_POP);
}

static void
statement(void)
{
    if (match(TOKEN_PRINT)) {
        printStatement();
    } else if (match(TOKEN_IF)) {
        ifStatement();
    } else if (match(TOKEN_WHILE)) {
        whileStatement();
    } else if (match(TOKEN_FOR)) {
        forStatement();
    } else if (match(TOKEN_LEFT_BRACE)) {
        beginScope();
        block();
        endScope();
    } else {
        expressionStatement();
    }
}

static void
declaration(void)
{
    if (match(TOKEN_VAR)) {
        varDeclaration();
    } else {
        statement();
    }
    if (parser.panic_mode) synchronize();
}

static void
binary(bool can_assign)
{
    (void)can_assign;

    TokenType operator_type = parser.previous.type;
    ParseRule *rule = getRule(operator_type);
    parsePrecedence((Precedence)(rule->precedence + 1));

    switch (operator_type) {
        case TOKEN_BANG_EQUAL:    emitBytes(OP_EQUAL, OP_NOT); break;
        case TOKEN_EQUAL_EQUAL:   emitByte(OP_EQUAL); break;
        case TOKEN_GREATER:       emitByte(OP_GREATER); break;
        case TOKEN_GREATER_EQUAL: emitBytes(OP_LESSER, OP_NOT); break;
        case TOKEN_LESSER:        emitByte(OP_LESSER); break;
        case TOKEN_LESSER_EQUAL:  emitBytes(OP_GREATER, OP_NOT); break;
        case TOKEN_PLUS:    emitByte(OP_ADD); break;
        case TOKEN_MINUS:   emitByte(OP_SUBTRACT); break;
        case TOKEN_STAR:    emitByte(OP_MULTIPLY); break;
        case TOKEN_SLASH:   emitByte(OP_DIVIDE); break;
        default:            return; // Unreachable.
    }
}

static void
literal(bool can_assign)
{
    (void)can_assign;

    switch (parser.previous.type) {
        case TOKEN_FALSE: emitByte(OP_FALSE); break;
        case TOKEN_NIL:   emitByte(OP_NIL); break;
        case TOKEN_TRUE:  emitByte(OP_TRUE); break;
        default:          return;
    }
}

static void
grouping(bool can_assign)
{
    (void)can_assign;

    // Assume opening parenthesis was already consumed.
    expression();
    consume(TOKEN_RIGHT_PAREN, "expect ')' after expression");
}

static void
number(bool can_assign)
{
    (void)can_assign;

    double value = strtod(parser.previous.start, NULL);
    emitConstant(NUMBER_VAL(value));
}

static void
string(bool can_assign)
{
    (void)can_assign;

    // Copy string directly from lexeme, stripping surrounding quotation marks.
    emitConstant(OBJ_VAL(copyString(parser.previous.start + 1, parser.previous.length - 2)));
}

static void
namedVariable(Token name, bool can_assign)
{
    uint8_t get_op, set_op;

    int arg = resolveLocal(current, &name);
    if (arg != -1) {
        get_op = OP_GET_LOCAL;
        set_op = OP_SET_LOCAL;
    } else {
        arg = identifierConstant(&name);
        get_op = OP_GET_GLOBAL;
        set_op = OP_SET_GLOBAL;
    }

    if (can_assign && match(TOKEN_EQUAL)) {
        expression();
        emitBytes(set_op, (uint8_t)arg);
    } else {
        emitBytes(get_op, (uint8_t)arg);
    }
}

static void
variable(bool can_assign)
{
    namedVariable(parser.previous, can_assign);
}

static void
unary(bool can_assign)
{
    (void)can_assign;

    TokenType operator_type = parser.previous.type;

    // Compile operand.
    parsePrecedence(PREC_UNARY);

    // Emit operator instruction.
    switch (operator_type) {
        case TOKEN_BANG:  emitByte(OP_NOT); break;
        case TOKEN_MINUS: emitByte(OP_NEGATE); break;
        default: return; // Unreachable.
    }
}

ObjFunction *
compile(const char *source)
{
    initScanner(source);
    Compiler compiler;
    initCompiler(&compiler, TYPE_SCRIPT);

    parser.had_error = false;
    parser.panic_mode = false;

    advance();
    while (!match(TOKEN_EOF)) {
        declaration();
    }

    ObjFunction *function = endCompile();
    return parser.had_error ? NULL : function;
}
