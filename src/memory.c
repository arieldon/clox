#include <stdlib.h>

#include "compiler.h"
#include "memory.h"
#include "vm.h"

#ifdef DEBUG_LOG_GC
#include <stdio.h>
#include "debug.h"
#endif

#define GC_HEAP_GROW_FACTOR 2

void *
reallocate(void *pointer, size_t old_size, size_t new_size)
{
    vm.bytes_allocated += new_size - old_size;

    if (new_size > old_size) {
#ifdef DEBUG_STRESS_GC
        // Force a collection to run on any attempt to allocate *more* memory.
        collectGarbage();
#endif

        if (vm.bytes_allocated > vm.next_gc) {
            collectGarbage();
        }
    }

    if (new_size == 0) {
        free(pointer);
        return NULL;
    }

    void *result = realloc(pointer, new_size);
    if (result == NULL) exit(1);
    return result;
}

static void
freeObject(Obj *object)
{
#ifdef DEBUG_LOG_GC
    printf("%p free type %d\n", (void *)object, object->type);
#endif

    switch (object->type) {
        case OBJ_BOUND_METHOD:
            FREE(OBJ_BOUND_METHOD, object);
            break;
        case OBJ_CLASS: {
            ObjClass *class = (ObjClass *)object;
            freeTable(&class->methods);
            FREE(ObjClass, object);
            break;
        }
        case OBJ_CLOSURE:
            ObjClosure *closure = (ObjClosure *)object;
            FREE_ARRAY(ObjUpvalue *, closure->upvalues, closure->upvalue_count);
            FREE(ObjClosure, object);
            break;
        case OBJ_FUNCTION: {
            ObjFunction *function = (ObjFunction *)object;
            freeChunk(&function->chunk);
            FREE(ObjFunction, object);
            break;
        }
        case OBJ_INSTANCE: {
            ObjInstance *instance = (ObjInstance *)object;
            freeTable(&instance->fields);
            FREE(ObjInstance, object);
            break;
        }
        case OBJ_NATIVE:
            FREE(ObjNative, object);
            break;
        case OBJ_STRING: {
            ObjString *string = (ObjString *)object;
            FREE_ARRAY(char, string->chars, string->length + 1);
            FREE(ObjString, object);
            break;
        }
        case OBJ_UPVALUE:
            FREE(ObjUpvalue, object);
            break;
    }
}

void
freeObjects(void)
{
    Obj *object = vm.objects;
    while (object != NULL) {
        Obj *next = object->next;
        freeObject(object);
        object = next;
    }

    free(vm.gray_stack);
}

void
markObject(Obj *object)
{
    if (object == NULL) return;
    if (object->is_marked) return;

#ifdef DEBUG_LOG_GC
    printf("%p mark ", (void *)object);
    printValue(OBJ_VAL(object));
    printf("\n");
#endif

    object->is_marked = true;

    if (vm.gray_capacity < vm.gray_count + 1) {
        vm.gray_capacity = GROW_CAPACITY(vm.gray_capacity);
        vm.gray_stack = (Obj **)realloc(vm.gray_stack, sizeof(Obj *) * vm.gray_capacity);

        if (vm.gray_stack == NULL) exit(1);
    }

    vm.gray_stack[vm.gray_count++] = object;
}

void
markValue(Value value)
{
    if (IS_OBJ(value)) markObject(AS_OBJ(value));
}

static void
markArray(ValueArray *array)
{
    for (int i = 0; i < array->count; ++i) {
        markValue(array->values[i]);
    }
}

static void
markRoots(void)
{
    // Local variables and objects on the stack are roots, where a root is an
    // object the VM can reach without following a reference within another
    // object.
    for (Value *slot = vm.stack; slot < vm.stack_top; ++slot) {
        markValue(*slot);
    }

    // Closures stored in the stack of call frames must be marked and kept as
    // well because they contain pointers to constants and upvalues. These may
    // be used in the future.
    for (int i = 0; i < vm.frame_count; ++i) {
        markObject((Obj *)vm.frames[i].closure);
    }

    // The VM can also reach upvalues directly.
    for (ObjUpvalue *upvalue = vm.open_upvalues; upvalue != NULL; upvalue = upvalue->next) {
        markObject((Obj *)upvalue);
    }

    // Globals are roots.
    markTable(&vm.globals);

    // The VM allocates memory for literals and constants during compilation.
    markCompilerRoots();

    // Treat name of init method as root to keep it.
    markObject((Obj *)vm.init_string);
}

static void
blackenObject(Obj *object)
{
    // A black object is an object that is marked but no longer in the stack of
    // gray objects; that is, it is a marked object that has had its references
    // traversed. There does not exist an explicit array or variable to track
    // these blackened objects.

#ifdef DEBUG_LOG_GC
    printf("%p black ", (void *)object);
    printValue(OBJ_VAL(object));
    printf("\n");
#endif

    switch (object->type) {
        case OBJ_BOUND_METHOD: {
            ObjBoundMethod *bound = (ObjBoundMethod *)object;
            markValue(bound->receiver);
            markObject((Obj *)bound->method);
            break;
        }
        case OBJ_CLASS: {
            ObjClass *class = (ObjClass *)object;
            markObject((Obj *)class->name);
            markTable(&class->methods);
            break;
        }
        case OBJ_CLOSURE: {
            ObjClosure *closure = (ObjClosure *)object;
            markObject((Obj *)closure->function);
            for (int i = 0; i < closure->upvalue_count; ++i) {
                markObject((Obj *)closure->upvalues[i]);
            }
            break;
        }
        case OBJ_FUNCTION: {
            ObjFunction *function = (ObjFunction *)object;
            markObject((Obj *)function->name);
            markArray(&function->chunk.constants);
            break;
        }
        case OBJ_INSTANCE: {
            ObjInstance *instance = (ObjInstance *)object;
            markObject((Obj *)instance->class);
            markTable(&instance->fields);
            break;
        }
        case OBJ_UPVALUE:
            markValue(((ObjUpvalue *)object)->closed);
            break;
        case OBJ_NATIVE:
        case OBJ_STRING:
            break;
    }
}

static void
traceReferences(void)
{
    while (vm.gray_count > 0) {
        Obj *object = vm.gray_stack[--vm.gray_count];
        blackenObject(object);
    }
}

static void
sweep(void)
{
    // Free any unmarked or white objects. These are unreachable, so they will
    // not be used in the future.

    Obj *previous = NULL;
    Obj *object = vm.objects;
    while (object != NULL) {
        if (object->is_marked) {
            object->is_marked = false;  // Reset for next GC cycle.
            previous = object;
            object = object->next;
        } else {
            Obj *unreached = object;
            object = object->next;
            if (previous != NULL) {
                // Fill the gap that will exist in the chain of objects once
                // the unreached object is freed.
                previous->next = object;
            } else {
                // Handle edge case: if the first object in the linked list of
                // objects is unreached, move the head of the list forward.
                vm.objects = object;
            }

            freeObject(unreached);
        }
    }
}

void
collectGarbage(void)
{
#ifdef DEBUG_LOG_GC
    printf("-- gc begin\n");
    size_t before = vm.bytes_allocated;
#endif

    // Mark objects that the VM can access directly and push them onto a stack.
    // These are referred to as gray objects.
    markRoots();

    // Exhaust the stack of objects created in markRoots(), traversing the
    // references of each block on the stack. Additional objects may be pushed
    // onto the stack during this phase, and they are processed as well.
    // Reachable objects that have had their referenced traversed and are no
    // longer in the stack of gray objects are referred to as black.
    traceReferences();

    // Free interned strings no longer in use.
    tableRemoveWhite(&vm.strings);

    // Once all reachable objects are marked, free the unreachable objects and
    // reset the marks. Unreachable objects are referred to as white.
    sweep();

    // Dynamically adjust threshold for next cycle of garbage collection to
    // minimize the amount of time spent traversing a large set of objects.
    vm.next_gc = vm.bytes_allocated * GC_HEAP_GROW_FACTOR;

#ifdef DEBUG_LOG_GC
    printf("-- gc end\n");
    printf("   collected %zu bytes (from %zu to %zu) next at %zu\n",
            before - vm.bytes_allocated, before, vm.bytes_allocated, vm.next_gc);
#endif
}
