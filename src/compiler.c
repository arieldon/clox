#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "compiler.h"
#include "memory.h"
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
ClassCompiler *current_class = NULL;

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
    if (current->type == TYPE_INITIALIZER) {
        emitBytes(OP_GET_LOCAL, 0);
    } else {
        emitByte(OP_NIL);
    }
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
    compiler->enclosing = current;
    compiler->function = NULL;
    compiler->type = type;
    compiler->local_count = 0;
    compiler->scope_depth = 0;
    compiler->function = newFunction();

    current = compiler;
    if (type != TYPE_SCRIPT) {
        current->function->name = copyString(parser.previous.start,
                parser.previous.length);
    }

    // The compiler uses slot zero of the array that tracks local variables and
    // other temporary values.
    Local *local = &current->locals[current->local_count++];
    local->depth = 0;
    local->is_captured = false;
    if (type != TYPE_FUNCTION) {
        local->name.start = "this";
        local->name.length = 4;
    } else {
        local->name.start = "";
        local->name.length = 0;
    }
}

static ObjFunction *
endCompiler(void)
{
    emitReturn();
    ObjFunction *function = current->function;

#ifdef DEBUG_PRINT_CODE
    if (!parser.had_error) {
        disassembleChunk(currentChunk(), function->name != NULL
            ? function->name->chars : "<script>");
    }
#endif

    // Each function introduces a new compiler. The end of one compiler's
    // compilation signifies the need to pop the enclosing compiler from the
    // stack and use it instead.
    current = current->enclosing;
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
        if (current->locals[current->local_count - 1].is_captured) {
            emitByte(OP_CLOSE_UPVALUE);
        } else {
            emitByte(OP_POP);
        }
        --current->local_count;
    }
}

// TODO Reorganize this file to prevent the need to forward declare all of
// these.
static void binary(bool can_assign);
static void call(bool can_assign);
static void dot(bool can_assign);
static void literal(bool can_assign);
static void grouping(bool can_assign);
static void number(bool can_assign);
static void string(bool can_assign);
static void variable(bool can_assign);
static void namedVariable(Token name, bool can_assign);
static void this(bool can_assign);
static void unary(bool can_assign);
static void and(bool can_assign);
static void or(bool can_assign);
static void declaration(void);

ParseRule rules[] = {
    [TOKEN_LEFT_PAREN]    = {grouping, call,   PREC_CALL},
    [TOKEN_RIGHT_PAREN]   = {NULL,     NULL,   PREC_NONE},
    [TOKEN_LEFT_BRACE]    = {NULL,     NULL,   PREC_NONE},
    [TOKEN_RIGHT_BRACE]   = {NULL,     NULL,   PREC_NONE},
    [TOKEN_COMMA]         = {NULL,     NULL,   PREC_NONE},
    [TOKEN_DOT]           = {NULL,     dot,    PREC_CALL},
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
    [TOKEN_THIS]          = {this,     NULL,   PREC_NONE},
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

static int
addUpvalue(Compiler *compiler, uint8_t index, bool is_local)
{
    int upvalue_count = compiler->function->upvalue_count;

    // If previously stored, return existing index of an upvalue.
    for (int i = 0; i < upvalue_count; ++i) {
        Upvalue *upvalue = &compiler->upvalues[i];
        if (upvalue->index == index && upvalue->is_local == is_local) {
            return i;
        }
    }

    if (upvalue_count == UINT8_COUNT) {
        error("Too many closure variables in function");
        return 0;
    }

    // If not previously stored, append the upvalue and return its index.
    compiler->upvalues[upvalue_count].is_local = is_local;
    compiler->upvalues[upvalue_count].index = index;
    return compiler->function->upvalue_count++;
}

static int
resolveUpvalue(Compiler *compiler, Token *name)
{
    if (compiler->enclosing == NULL) return -1;

    int local = resolveLocal(compiler->enclosing, name);
    if (local != -1) {
        compiler->enclosing->locals[local].is_captured = true;
        return addUpvalue(compiler, (uint8_t)local, true);
    }

    int upvalue = resolveUpvalue(compiler->enclosing, name);
    if (upvalue != -1) {
        return addUpvalue(compiler, (uint8_t)upvalue, false);
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
    local->is_captured = false;
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
    if (current->scope_depth == 0) return;
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

static uint8_t
argumentList(void)
{
    uint8_t arg_count = 0;
    if (!check(TOKEN_RIGHT_PAREN)) {
        do {
            expression();
            if (arg_count == 255) {
                error("cannot have more than 255 arguments");
            }
            ++arg_count;
        } while (match(TOKEN_COMMA));
    }
    consume(TOKEN_RIGHT_PAREN, "expect ')' after arguments");
    return arg_count;
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
function(FunctionType type)
{
    Compiler compiler;
    initCompiler(&compiler, type);
    beginScope();

    consume(TOKEN_LEFT_PAREN, "expect '(' after function name");
    if (!check(TOKEN_RIGHT_PAREN)) {
        do {
            ++current->function->arity;
            if (current->function->arity > 255) {
                errorAtCurrent("cannot have more than 255 parameters");
            }
            uint8_t constant = parseVariable("expect parameter name");
            defineVariable(constant);
        } while (match(TOKEN_COMMA));
    }
    consume(TOKEN_RIGHT_PAREN, "expect ')' after parameters");
    consume(TOKEN_LEFT_BRACE, "expect '{' before function body");
    block();

    // No need for endScope() since endCompiler() effectively terminates the
    // compiler.
    ObjFunction *function = endCompiler();
    emitBytes(OP_CLOSURE, makeConstant(OBJ_VAL(function)));

    for (int i = 0; i < function->upvalue_count; ++i) {
        emitByte(compiler.upvalues[i].is_local ? 1 : 0);
        emitByte(compiler.upvalues[i].index);
    }
}

static void
method(void)
{
    consume(TOKEN_IDENTIFIER, "expect method name");
    uint8_t constant = identifierConstant(&parser.previous);

    FunctionType type = TYPE_METHOD;
    if (parser.previous.length == 4 && memcmp(parser.previous.start, "init", 4) == 0) {
        type = TYPE_INITIALIZER;
    }

    function(type);
    emitBytes(OP_METHOD, constant);
}

static void
classDeclaration(void)
{
    consume(TOKEN_IDENTIFIER, "expect class name");
    Token class_name = parser.previous;
    uint8_t name_constant = identifierConstant(&parser.previous);
    declareVariable();

    emitBytes(OP_CLASS, name_constant);
    defineVariable(name_constant);

    ClassCompiler class_compiler;
    class_compiler.enclosing = current_class;
    current_class = &class_compiler;

    namedVariable(class_name, false);
    consume(TOKEN_LEFT_BRACE, "expect '{' before class body");
    while (!check(TOKEN_RIGHT_BRACE) && !check(TOKEN_EOF)) {
        method();
    }
    consume(TOKEN_RIGHT_BRACE, "expect '}' after class body");
    emitByte(OP_POP);

    current_class = current_class->enclosing;
}

static void
funDeclaration(void)
{
    uint8_t global = parseVariable("expect function name");

    // Mark the function initialized after compiling its name and before
    // compiling its body to allow a reference to the function in its own body.
    // The programmer cannot call the function until the compiler defines it
    // fully, so this behavior proves safe.
    markInitialized();

    function(TYPE_FUNCTION);
    defineVariable(global);
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
returnStatement(void)
{
    if (current->type == TYPE_SCRIPT) {
        error("cannot return from top-level code");
    }

    if (match(TOKEN_SEMICOLON)) {
        emitReturn();
    } else {
        if (current->type == TYPE_INITIALIZER) {
            error("cannot return a value from an initiailzer");
        }

        expression();
        consume(TOKEN_SEMICOLON, "expect ';' after return value");
        emitByte(OP_RETURN);
    }
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
    } else if (match(TOKEN_RETURN)) {
        returnStatement();
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
    if (match(TOKEN_CLASS)) {
        classDeclaration();
    } else if (match(TOKEN_FUN)) {
        funDeclaration();
    } else if (match(TOKEN_VAR)) {
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
call(bool can_assign)
{
    (void)can_assign;

    uint8_t arg_count = argumentList();
    emitBytes(OP_CALL, arg_count);
}

static void
dot(bool can_assign)
{
    consume(TOKEN_IDENTIFIER, "expect property name after '.'");
    uint8_t name = identifierConstant(&parser.previous);

    if (can_assign && match(TOKEN_EQUAL)) {
        expression();
        emitBytes(OP_SET_PROPERTY, name);
    } else {
        emitBytes(OP_GET_PROPERTY, name);
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
    } else if ((arg = resolveUpvalue(current, &name)) != -1) {
        get_op = OP_GET_UPVALUE;
        set_op = OP_SET_UPVALUE;
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
this(bool can_assign)
{
    (void)can_assign;

    if (current_class == NULL) {
        error("cannot use 'this' outside of a class");
        return;
    }

    variable(false);
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

    ObjFunction *function = endCompiler();
    return parser.had_error ? NULL : function;
}

void
markCompilerRoots(void)
{
    Compiler *compiler = current;
    while (compiler != NULL) {
        markObject((Obj *)compiler->function);
        compiler = compiler->enclosing;
    }
}
