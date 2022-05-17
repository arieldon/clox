#ifndef VALUE_H
#define VALUE_H

#include <assert.h>
#include <string.h>

#include "common.h"

typedef struct Obj Obj;
typedef struct ObjString ObjString;

#ifdef NAN_TAG

// NaN tagging is a technique that removes internal padding or fragmentation
// between members in a struct to increase the amount of useful data that fits
// in cache. In other words, it changes the way the VM represents type `Value`
// to improve its cache coherency.
//
// If enabled, use an unsigned 64-bit integer as type `Value` instead of a
// struct. On a 64-bit architecture, the struct also consists of 64 bits total
// since internal padding exists between its type tag and its stored value.
// Because type double, the largest value that `Value` represents, consists of
// 64 bits on a 64-bit architecture, it matches the size of this struct.
//
// More importantly, according to IEEE 754--the standard representation of
// floating point arithmetic--there exists a condition where the CPU ignores a
// significant number of these 64 bits. When all exponent bits of a double are
// set--bits 52 through 62, inclusive--the machine treats the value as a NaN.
// Each NaN as specified by IEEE 754 may be one of two types: signalling or
// quiet. A signalling NaN signifies an illegal operation; a quiet NaN
// signifies a meaningless but usable value. IEEE 754 specifies bit 51, the
// highest mantissa bit, of a floating point value to indicate this property:
// quiet if set, signalling if zero. As previously mentioned, the CPU ignores
// all bits outside of the inclusive range 51 to 62 for NaNs, leaving 52 bits
// for any programmer's play. Intel also occupies bit 50, labeling it "QNaN
// Floating-Point Indefinite", leaving a total 51 bits for play. NaN tagging
// stores values in these remaining bits and uses bit masks to access and
// modify them.
//
// Floating-Point Representation by IEEE 754:
// 63 sign bit | 52-62 exponent bits | 0-51 mantissa bits
//
// Clox uses those remaining bits to represent different Lox types in the
// following ways.
//
// Numbers:
// Lox represents all numbers as floating points, so a valid floating point,
// any non-NaN, represents a number.
//
// Objects:
// The combination of a quiet NaN and a set sign bit represents an Object. In
// this case, the low 48 bits store the address of the Object.
//
// Booleans & Nil:
// The combination of a quiet NaN and an unset sign bit may represent true, 1
// false, or nil. The exact value depends on the two most least significant
// bits.
//
// Behavior outside of these specific cases remains undefined.
//
// Note, this implementation assumes pointers, while 64 bits in total size,
// only use the low 48 bits. This assumption holds for common architectures
// according to Robert Nystrom, and at the very least, it works on my machine.

#define SIGN_BIT    ((uint64_t)0x8000000000000000) // Mask for most significant bit, the sign bit.
#define QNAN        ((uint64_t)0x7ffc000000000000) // Mask for bits 50 to 62 that signify a quiet NaN.

#define TAG_NIL     1
#define TAG_FALSE   2
#define TAG_TRUE    3

typedef uint64_t Value;

#define IS_BOOL(value)   (((value) | 1) == TRUE_VAL) // Both boolean values set least significant bit to 1.
#define IS_NIL(value)    ((value) == NIL_VAL)
#define IS_NUMBER(value) (((value) & QNAN) != QNAN)
#define IS_OBJ(value)    (((value) & (QNAN | SIGN_BIT)) == (QNAN | SIGN_BIT))

#define AS_BOOL(value)   ((value) == TRUE_VAL)
#define AS_NUMBER(value) valueToNum(value)
#define AS_OBJ(value)    ((Obj *)(uintptr_t)((value) & ~(SIGN_BIT | QNAN)))

#define BOOL_VAL(b)      ((b) ? TRUE_VAL : FALSE_VAL)
#define FALSE_VAL        ((Value)(uint64_t)(QNAN | TAG_FALSE))
#define TRUE_VAL         ((Value)(uint64_t)(QNAN | TAG_TRUE))
#define NIL_VAL          ((Value)(uint64_t)(QNAN | TAG_NIL))
#define NUMBER_VAL(num)  numToValue(num)
#define OBJ_VAL(obj)     (Value)(SIGN_BIT | QNAN | (uint64_t)(uintptr_t)(obj))

// In both conversion functions below--valueToNum() and numToValue()--the
// compiler *should* optimize out memcpy() since it's a common way to "type
// pun". memcpy() remains necessary to "type pun" or convert a C double into a
// NaN tagged `Value` in a way supported by the C specification.
//
// To confirm, both x86-64 gcc 12.1 and x86-64 clang 14.0.0 optimize out
// memcpy() with any level of optimization above 0.

static inline double
valueToNum(Value value)
{
    assert(sizeof(double) == sizeof(Value));
    double num;
    memcpy(&num, &value, sizeof(Value));
    return num;
}

static inline Value
numToValue(double num)
{
    assert(sizeof(double) == sizeof(Value));
    Value value;
    memcpy(&value, &num, sizeof(double));
    return value;
}

#else

typedef enum {
    VAL_BOOL,
    VAL_NIL,
    VAL_NUMBER,
    VAL_OBJ,
} ValueType;

typedef struct {
    ValueType type;
    union {
        bool boolean;
        double number;
        Obj *obj;
    } as;
} Value;

#define IS_BOOL(value)    ((value).type == VAL_BOOL)
#define IS_NIL(value)     ((value).type == VAL_NIL)
#define IS_NUMBER(value)  ((value).type == VAL_NUMBER)
#define IS_OBJ(value)     ((value).type == VAL_OBJ)

#define AS_BOOL(value)    ((value).as.boolean)
#define AS_NUMBER(value)  ((value).as.number)
#define AS_OBJ(value)     ((value).as.obj)

#define BOOL_VAL(value)   ((Value){VAL_BOOL, {.boolean = value}})
#define NIL_VAL           ((Value){VAL_NIL, {.number = 0}})
#define NUMBER_VAL(value) ((Value){VAL_NUMBER, {.number = value}})
#define OBJ_VAL(object)   ((Value){VAL_OBJ, {.obj = (Obj *)object}})

#endif

typedef struct {
    int count;
    int capacity;
    Value *values;
} ValueArray;

bool valuesEqual(Value a, Value b);
void initValueArray(ValueArray *array);
void freeValueArray(ValueArray *array);
void writeValueArray(ValueArray *array, Value value);
void printValue(Value value);

#endif
