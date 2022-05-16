#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "memory.h"
#include "object.h"
#include "table.h"
#include "value.h"

#define TABLE_MAX_LOAD 0.75

void
initTable(Table *table)
{
    table->count = 0;
    table->capacity = 0;
    table->entries = NULL;
}

void
freeTable(Table *table)
{
    FREE_ARRAY(Entry, table->entries, table->capacity);
    initTable(table);
}

static Entry *
findEntry(Entry *entries, int capacity, ObjString *key)
{
    // `capacity` must be a power of 2 to replace the original modulo operation
    // with the bit twiddling trick below.
    assert((capacity & (capacity - 1)) == 0);

    // In binary, subtracting a power of two by 1 yields a series of 1 bits.
    // Use this series of 1 bits as a mask to replace modulo operation n % m
    // since they're equivalent when m is a power of 2.
    uint32_t index = key->hash & (capacity - 1);
    Entry *tombstone = NULL;

    // While the loop appears infinite, it will eventually terminate when it
    // finds an empty bucket, and an empty bucket *should* exist because the
    // table dynamically scales to its load factor.
    for (;;) {
        Entry *entry = &entries[index];
        if (entry->key == NULL) {
            if (IS_NIL(entry->value)) {
                // This combination of NULL key and NIL value represents an
                // empty entry.
                return tombstone != NULL ? tombstone : entry;
            } else {
                if (tombstone == NULL) tombstone = entry;
            }
        } else if (entry->key == key) {
            // Since the VM uses string interning, keys may be compared using
            // `==` because they are the same string at the same address.
            return entry;
        }

        // Use the same bit twiddle as above.
        index = (index + 1) & (capacity - 1);
    }
}

bool
tableGet(Table *table, ObjString *key, Value *value)
{
    if (table->count == 0) return false;

    Entry *entry = findEntry(table->entries, table->capacity, key);
    if (entry->key == NULL) return false;

    *value = entry->value;
    return true;
}

static void
adjustCapacity(Table *table, int capacity)
{
    Entry *entries = ALLOCATE(Entry, capacity);
    for (int i = 0; i < capacity; ++i) {
        entries[i].key = NULL;
        entries[i].value = NIL_VAL;
    }

    table->count = 0;
    for (int i = 0; i < table->capacity; ++i) {
        Entry *entry = &table->entries[i];
        if (entry->key == NULL) continue;
        Entry *dest = findEntry(entries, capacity, entry->key);
        dest->key = entry->key;
        dest->value = entry->value;
        ++table->count;
    }

    FREE_ARRAY(Entry, table->entries, table->capacity);
    table->entries = entries;
    table->capacity = capacity;
}

bool
tableSet(Table *table, ObjString *key, Value value)
{
    if (table->count + 1 > table->capacity * TABLE_MAX_LOAD) {
        int capacity = GROW_CAPACITY(table->capacity);
        adjustCapacity(table, capacity);
    }

    Entry *entry = findEntry(table->entries, table->capacity, key);
    bool is_new_key = entry->key == NULL;

    // Do not increment count of elements in array for tombstones -- indicated
    // by a NULL key and a boolean value of true -- because they are considered
    // to be full buckets in terms of load factor. If not considered as full
    // buckets, then the loop in findEntry() may fail to terminate because it
    // fails to find a truly empty bucket.
    if (is_new_key && IS_NIL(entry->value)) ++table->count;

    entry->key = key;
    entry->value = value;
    return is_new_key;
}

bool
tableDelete(Table *table, ObjString *key)
{
    if (table->count == 0) return false;

    Entry *entry = findEntry(table->entries, table->capacity, key);
    if (entry->key == NULL) return false;

    entry->key = NULL;
    entry->value = BOOL_VAL(true);
    return true;
}

void
tableAddAll(Table *from, Table *to)
{
    for (int i = 0; i < from->capacity; ++i) {
        Entry *entry = &from->entries[i];
        if (entry->key != NULL) {
            tableSet(to, entry->key, entry->value);
        }
    }
}

ObjString *
tableFindString(Table *table, const char *chars, int length, uint32_t hash)
{
    if (table->count == 0) return NULL;

    // Use the same bit twiddle described in findEntry() to optimize search by
    // replacing modulo with a bit mask.
    uint32_t index = hash & (table->capacity - 1);
    for (;;) {
        Entry *entry = &table->entries[index];
        if (entry->key == NULL) {
            // Terminate at an empty non-tombstone entry.
            if (IS_NIL(entry->value)) return NULL;
        } else if (entry->key->length == length && entry->key->hash == hash &&
                   memcmp(entry->key->chars, chars, length) == 0) {
            return entry->key;
        }
        index = (index + 1) & (table->capacity - 1);
    }
}

void
tableRemoveWhite(Table *table)
{
    for (int i = 0; i < table->capacity; ++i) {
        Entry *entry = &table->entries[i];
        if (entry->key != NULL && !entry->key->obj.is_marked) {
            tableDelete(table, entry->key);
        }
    }
}

void
markTable(Table *table)
{
    for (int i = 0; i < table->capacity; ++i) {
        Entry *entry = &table->entries[i];
        markObject(&entry->key->obj);
        markValue(entry->value);
    }
}
