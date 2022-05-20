#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "common.h"
#include "compiler.h"
#include "debug.h"
#include "memory.h"
#include "object.h"
#include "vm.h"

// Declare as a global variable to grant entire program access without passing
// around a pointer everywhere. Only a single VM is required or even desirable
// in this case anyway.
VM vm;

static Value
clockNative(int arg_count, Value *args)
{
    (void)arg_count;
    (void)args;
    return NUMBER_VAL((double)clock() / CLOCKS_PER_SEC);
}

static void
resetStack(void)
{
    vm.stack_top = vm.stack;
    vm.open_upvalues = NULL;
    vm.frame_count = 0;
}

static void
runtimeError(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputs("\n", stderr);

    for (int i = vm.frame_count - 1; i >= 0; --i) {
        CallFrame *frame = &vm.frames[i];
        ObjFunction *function = frame->closure->function;
        size_t instruction_offset = frame->ip - function->chunk.code - 1;

        fprintf(stderr, "[line %d] in ", getLine(&function->chunk, instruction_offset));
        if (function->name == NULL) {
            fprintf(stderr, "script\n");
        } else {
            fprintf(stderr, "%s()\n", function->name->chars);
        }
    }

    resetStack();
}

static void
defineNative(const char *name, NativeFn function)
{
    // Since both copyString() and newNative() allocate memory dynamically,
    // push and pop the values to prevent the garbage collector from
    // inadvertently cleaning them.
    push(OBJ_VAL(copyString(name, (int)strlen(name))));
    push(OBJ_VAL(newNative(function)));
    tableSet(&vm.globals, AS_STRING(vm.stack[0]), vm.stack[1]);
    pop();
    pop();
}

void
initVM(void)
{
    resetStack();

    vm.gray_count = 0;
    vm.gray_capacity = 0;
    vm.gray_stack = NULL;

    initTable(&vm.globals);
    initTable(&vm.strings);

    vm.init_string = NULL;
    vm.init_string = copyString("init", 4);

    vm.bytes_allocated = 0;
    vm.next_gc = 1024 * 1024;
    vm.objects = NULL;

    defineNative("clock", clockNative);
}

void
freeVM(void)
{
    freeTable(&vm.globals);
    freeTable(&vm.strings);
    vm.init_string = NULL;
    freeObjects();
}

void
push(Value value)
{
    assert(vm.stack_top < vm.stack + STACK_MAX);
    *vm.stack_top = value;
    ++vm.stack_top;
}

Value
pop(void)
{
    --vm.stack_top;
    return *vm.stack_top;
}

static Value
peek(int distance)
{
    assert(vm.stack_top - 1 - distance >= vm.stack);
    return vm.stack_top[-1 - distance];
}

static bool
call(ObjClosure *closure, int arg_count)
{
    if (arg_count != closure->function->arity) {
        runtimeError("expected %d arguments but got %d", closure->function->arity, arg_count);
        return false;
    }

    if (vm.frame_count == FRAMES_MAX) {
        runtimeError("stack overflow");
        return false;
    }

    CallFrame *frame = &vm.frames[vm.frame_count++];
    frame->closure = closure;
    frame->ip = closure->function->chunk.code;
    frame->slots = vm.stack_top - arg_count - 1;
    return true;
}

static bool
callValue(Value callee, int arg_count)
{
    if (IS_OBJ(callee)) {
        switch (OBJ_TYPE(callee)) {
            case OBJ_BOUND_METHOD: {
                ObjBoundMethod *bound = AS_BOUND_METHOD(callee);
                vm.stack_top[-arg_count - 1] = bound->receiver;
                return call(bound->method, arg_count);
            }
            case OBJ_CLASS: {
                ObjClass *class = AS_CLASS(callee);
                vm.stack_top[-arg_count - 1] = OBJ_VAL(newInstance(class));
                Value initializer;
                if (tableGet(&class->methods, vm.init_string, &initializer)) {
                    return call(AS_CLOSURE(initializer), arg_count);
                } else if (arg_count != 0) {
                    runtimeError("expected 0 arguments but got %d", arg_count);
                    return false;
                }
                return true;
            }
            case OBJ_CLOSURE:
                return call(AS_CLOSURE(callee), arg_count);
            case OBJ_NATIVE: {
                NativeFn native = AS_NATIVE(callee);
                Value result = native(arg_count, vm.stack_top - arg_count);
                vm.stack_top -= arg_count + 1;
                push(result);
                return true;
            }
            default:
                break; // This case indicates a non-callable object type.
        }
    }
    runtimeError("can only call functions and classes");
    return false;
}

static bool
invokeFromClass(ObjClass *class, ObjString *name, int arg_count)
{
    // Combine logic of OP_GET_OPERTY and OP_CALL instructions.
    Value method;
    if (!tableGet(&class->methods, name, &method)) {
        runtimeError("undefined preperty '%s'", name->chars);
        return false;
    }
    return call(AS_CLOSURE(method), arg_count);
}

static bool
invoke(ObjString *name, int arg_count)
{
    Value receiver = peek(arg_count);

    if (!IS_INSTANCE(receiver)) {
        runtimeError("only instances have methods");
        return false;
    }

    ObjInstance *instance = AS_INSTANCE(receiver);

    Value value;
    if (tableGet(&instance->fields, name, &value)) {
        vm.stack_top[-arg_count - 1] = value;
        return callValue(value, arg_count);
    }

    return invokeFromClass(instance->class, name, arg_count);
}

static bool
bindMethod(ObjClass *class, ObjString *name)
{
    Value method;
    if (!tableGet(&class->methods, name, &method)) {
        runtimeError("undefined property '%s'", name->chars);
        return false;
    }

    ObjBoundMethod *bound = newBoundMethod(peek(0), AS_CLOSURE(method));
    pop();
    push(OBJ_VAL(bound));
    return true;
}

static ObjUpvalue *
captureUpvalue(Value * local)
{
    ObjUpvalue *prev_upvalue= NULL;
    ObjUpvalue *upvalue = vm.open_upvalues;

    while (upvalue != NULL && upvalue->location > local) {
        prev_upvalue = upvalue;
        upvalue = upvalue->next;
    }

    if (upvalue != NULL && upvalue->location == local) {
        return upvalue;
    }

    ObjUpvalue *created_upvalue = newUpvalue(local);
    created_upvalue->next = upvalue;

    if (prev_upvalue == NULL) {
        vm.open_upvalues = created_upvalue;
    } else {
        prev_upvalue->next = created_upvalue;
    }

    return created_upvalue;
}

static void
closeUpvalues(Value *last)
{
    while (vm.open_upvalues != NULL && vm.open_upvalues->location >= last) {
        ObjUpvalue *upvalue = vm.open_upvalues;
        upvalue->closed = *upvalue->location;
        upvalue->location = &upvalue->closed;
        vm.open_upvalues = upvalue->next;
    }
}

static void
defineMethod(ObjString *name)
{
    Value method = peek(0);
    ObjClass *class = AS_CLASS(peek(1));
    tableSet(&class->methods, name, method);
    pop();
}

static bool
isFalsey(Value value)
{
    return IS_NIL(value) || (IS_BOOL(value) && !AS_BOOL(value));
}

static void
concatenate(void)
{
    ObjString *b = AS_STRING(peek(0));
    ObjString *a = AS_STRING(peek(1));

    int length = a->length + b->length;
    char *chars = ALLOCATE(char, length + 1);
    memcpy(chars, a->chars, a->length);
    memcpy(chars + a->length, b->chars, b->length);
    chars[length] = '\0';

    ObjString *result = takeString(chars, length);
    pop();
    pop();
    push(OBJ_VAL(result));
}

static InterpretResult
run(void)
{
    CallFrame *frame = &vm.frames[vm.frame_count - 1];

#define READ_BYTE() (*frame->ip++)

#define READ_SHORT() \
    (frame->ip += 2, \
    (uint16_t)((frame->ip[-2] << 8) | frame->ip[-1]))

#define READ_CONSTANT() \
    (frame->closure->function->chunk.constants.values[READ_BYTE()])

#define READ_STRING()   AS_STRING(READ_CONSTANT())

#define BINARY_OP(value_type, op) \
    do { \
        if (!IS_NUMBER(peek(0)) || !IS_NUMBER(peek(1))) { \
            runtimeError("operands must be numbers"); \
            return INTERPRET_RUNTIME_ERROR; \
        } \
        double b = AS_NUMBER(pop()); \
        double a = AS_NUMBER(pop()); \
        push(value_type(a op b)); \
    } while (false)

    // Upwards of 90% of execution time spent here according to Nystrom.
    for (;;) {
#ifdef DEBUG_TRACE_EXECUTION
        printf("          ");
        for (Value *slot = vm.stack; slot < vm.stack_top; ++slot) {
            printf("[ ");
            printValue(*slot);
            printf(" ]");
        }
        printf("\n");
        disassembleInstruction(&frame->closure->function->chunk,
                (int)(frame->ip - frame->closure->function->chunk.code));
#endif

        uint8_t instruction;
        switch (instruction = READ_BYTE()) {
            case OP_CONSTANT: {
                Value constant = READ_CONSTANT();
                push(constant);
                break;
            }
            case OP_NIL:      push(NIL_VAL); break;
            case OP_TRUE:     push(BOOL_VAL(true)); break;
            case OP_FALSE:    push(BOOL_VAL(false)); break;
            case OP_EQUAL: {
                Value b = pop();
                Value a = pop();
                push(BOOL_VAL(valuesEqual(a, b)));
                break;
            }
            case OP_GET_SUPER: {
                ObjString *name = READ_STRING();
                ObjClass *superclass = AS_CLASS(pop());

                if (!bindMethod(superclass, name)) {
                    return INTERPRET_RUNTIME_ERROR;
                }
                break;
            }
            case OP_GET_PROPERTY: {
                if (!IS_INSTANCE(peek(0))) {
                    runtimeError("only instances have properties");
                    return INTERPRET_RUNTIME_ERROR;
                }

                ObjInstance *instance = AS_INSTANCE(peek(0));
                ObjString *name = READ_STRING();

                Value value;
                if (tableGet(&instance->fields, name, &value)) {
                    pop();
                    push(value);
                    break;
                }

                if (!bindMethod(instance->class, name)) {
                    return INTERPRET_RUNTIME_ERROR;
                }
                break;
            }
            case OP_SET_PROPERTY: {
                if (!IS_INSTANCE(peek(1))) {
                    runtimeError("only instances have fields");
                    return INTERPRET_RUNTIME_ERROR;
                }

                ObjInstance *instance = AS_INSTANCE(peek(1));
                tableSet(&instance->fields, READ_STRING(), peek(0));
                Value value = pop();
                pop();
                push(value);
                break;
            }
            case OP_GET_UPVALUE: {
                uint8_t slot = READ_BYTE();
                push(*frame->closure->upvalues[slot]->location);
                break;
            }
            case OP_SET_UPVALUE: {
                uint8_t slot = READ_BYTE();
                *frame->closure->upvalues[slot]->location = peek(0);
                break;
            }
            case OP_GET_LOCAL: {
                uint8_t slot = READ_BYTE();
                push(frame->slots[slot]);
                break;
            }
            case OP_SET_LOCAL: {
                uint8_t slot = READ_BYTE();
                frame->slots[slot] = peek(0);
                break;
            }
            case OP_GET_GLOBAL: {
                ObjString *name = READ_STRING();
                Value value;
                if (!tableGet(&vm.globals, name, &value)) {
                    runtimeError("undefined variable '%s'", name->chars);
                    return INTERPRET_RUNTIME_ERROR;
                }
                push(value);
                break;
            }
            case OP_SET_GLOBAL: {
                ObjString *name = READ_STRING();
                if (tableSet(&vm.globals, name, peek(0))) {
                    tableDelete(&vm.globals, name);
                    runtimeError("undefined variable '%s'", name->chars);
                    return INTERPRET_RUNTIME_ERROR;
                }
                break;
            }
            case OP_DEFINE_GLOBAL: {
                ObjString *name = READ_STRING();
                tableSet(&vm.globals, name, peek(0));
                pop();
                break;
            }
            case OP_POP:      pop(); break;
            case OP_GREATER:  BINARY_OP(BOOL_VAL, >); break;
            case OP_LESSER:   BINARY_OP(BOOL_VAL, <); break;
            case OP_ADD: {
                if (IS_STRING(peek(0)) && IS_STRING(peek(1))) {
                    concatenate();
                } else if (IS_NUMBER(peek(0)) && IS_NUMBER(peek(1))) {
                    double b = AS_NUMBER(pop());
                    double a = AS_NUMBER(pop());
                    push(NUMBER_VAL(a + b));
                } else {
                    runtimeError("operands must be two numbers or two strings");
                    return INTERPRET_RUNTIME_ERROR;
                }
                break;
            }
            case OP_SUBTRACT: BINARY_OP(NUMBER_VAL, -); break;
            case OP_MULTIPLY: BINARY_OP(NUMBER_VAL, *); break;
            case OP_DIVIDE:   BINARY_OP(NUMBER_VAL, /); break;
            case OP_NOT:      push(BOOL_VAL(isFalsey(pop()))); break;
            case OP_NEGATE: {
                if (!IS_NUMBER(peek(0))) {
                    runtimeError("operand must be a number");
                    return INTERPRET_RUNTIME_ERROR;
                }
                vm.stack_top[-1] = NUMBER_VAL(-AS_NUMBER(peek(0)));
                break;
            }
            case OP_PRINT: {
                printValue(pop());
                printf("\n");
                break;
            }
            case OP_JUMP: {
                uint16_t offset = READ_SHORT();
                frame->ip += offset;
                break;
            }
            case OP_JUMP_IF_FALSE: {
                uint16_t offset = READ_SHORT();
                if (isFalsey(peek(0))) frame->ip += offset;
                break;
            }
            case OP_LOOP: {
                uint16_t offset = READ_SHORT();
                frame->ip -= offset;
                break;
            }
            case OP_CALL: {
                int arg_count = READ_BYTE();
                if (!callValue(peek(arg_count), arg_count)) {
                    return INTERPRET_RUNTIME_ERROR;
                }
                frame = &vm.frames[vm.frame_count - 1];
                break;
            }
            case OP_INVOKE: {
                ObjString *method = READ_STRING();
                int arg_count = READ_BYTE();
                if (!invoke(method, arg_count)) {
                    return INTERPRET_RUNTIME_ERROR;
                }
                frame = &vm.frames[vm.frame_count - 1];
                break;
            }
            case OP_SUPER_INVOKE: {
                ObjString *method = READ_STRING();
                int arg_count = READ_BYTE();
                ObjClass *superclass = AS_CLASS(pop());
                if (!invokeFromClass(superclass, method, arg_count)) {
                    return INTERPRET_RUNTIME_ERROR;
                }
                frame = &vm.frames[vm.frame_count - 1];
                break;
            }
            case OP_CLOSURE: {
                ObjFunction *function = AS_FUNCTION(READ_CONSTANT());
                ObjClosure *closure = newClosure(function);
                push(OBJ_VAL(closure));
                for (int i = 0; i < closure->upvalue_count; ++i) {
                    uint8_t is_local = READ_BYTE();
                    uint8_t index = READ_BYTE();
                    if (is_local) {
                        closure->upvalues[i] = captureUpvalue(frame->slots + index);
                    } else {
                        closure->upvalues[i] = frame->closure->upvalues[index];
                    }
                }
                break;
            }
            case OP_CLOSE_UPVALUE:
                closeUpvalues(vm.stack_top - 1);
                pop();
                break;
            case OP_RETURN: {
                Value result = pop();
                closeUpvalues(frame->slots);
                --vm.frame_count;
                if (vm.frame_count == 0) {
                    pop();
                    return INTERPRET_OK;
                }

                vm.stack_top = frame->slots;
                push(result);
                frame = &vm.frames[vm.frame_count - 1];
                break;
            }
            case OP_CLASS:
                push(OBJ_VAL(newClass(READ_STRING())));
                break;
            case OP_INHERIT: {
                // Copy methods from inherited class directly into inheriting
                // class. This is feasible because Lox doesn't allow addition
                // or deletion to the set of methods of a class after
                // declaration.
                Value superclass = peek(1);
                if (!IS_CLASS(superclass)) {
                    runtimeError("superclass must be a class");
                    return INTERPRET_RUNTIME_ERROR;
                }
                ObjClass *subclass = AS_CLASS(peek(0));
                tableAddAll(&AS_CLASS(superclass)->methods, &subclass->methods);
                pop();
                break;
            }
            case OP_METHOD:
                defineMethod(READ_STRING());
                break;
        }
    }

#undef BINARY_OP
#undef READ_SHORT
#undef READ_STRING
#undef READ_BYTE
#undef READ_CONSTANT
}

InterpretResult
interpret(const char *source)
{
    ObjFunction *function = compile(source);
    if (function == NULL) return INTERPRET_COMPILE_ERROR;

    push(OBJ_VAL(function));
    ObjClosure *closure = newClosure(function);
    pop();
    push(OBJ_VAL(closure));
    call(closure, 0);

    return run();
}
