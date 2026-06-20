// Usage:
//   Arena a = {0};
//   char *s = arena_strdup(&a, "hello");
//   void *p = arena_alloc(&a, 128);
//   arena_reset(&a);   // reuse memory, keep chunks
//   arena_free(&a);    // release everything

#ifndef ARENA_H_
#define ARENA_H_

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef ARENA_ASSERT
#    include <assert.h>
#    define ARENA_ASSERT assert
#endif

#ifndef ARENA_DEFAULT_CAPACITY
#    define ARENA_DEFAULT_CAPACITY (8 * 1024)
#endif

typedef struct Arena_Chunk Arena_Chunk;

struct Arena_Chunk {
    Arena_Chunk *next;
    size_t capacity;
    size_t used;
    unsigned char data[];
};

typedef struct {
    Arena_Chunk *begin;
    Arena_Chunk *end;
} Arena;

typedef struct {
    Arena_Chunk *chunk;
    size_t used;
} Arena_Mark;

void *arena_alloc(Arena *a, size_t size);
void *arena_realloc(Arena *a, void *oldptr, size_t oldsz, size_t newsz);

char *arena_strdup(Arena *a, const char *cstr);
char *arena_strndup(Arena *a, const char *str, size_t len);
void *arena_memdup(Arena *a, const void *data, size_t size);

Arena_Mark arena_snapshot(Arena *a);
void arena_rewind(Arena *a, Arena_Mark m);
void arena_reset(Arena *a);
void arena_free(Arena *a);

#endif // ARENA_H_

#ifdef ARENA_IMPLEMENTATION
#ifndef ARENA_IMPLEMENTATION_DONE
#    define ARENA_IMPLEMENTATION_DONE

#    ifndef ARENA_MALLOC
#        include <stdlib.h>
#        define ARENA_MALLOC malloc
#        define ARENA_FREE   free
#    endif

#    define ARENA_ALIGN (sizeof(void *) * 2)

static size_t arena_align_up(size_t n) {
    return (n + (ARENA_ALIGN - 1)) & ~(ARENA_ALIGN - 1);
}

static Arena_Chunk *arena_new_chunk(size_t capacity) {
    Arena_Chunk *c =
        (Arena_Chunk *)ARENA_MALLOC(sizeof(Arena_Chunk) + capacity);
    ARENA_ASSERT(c != NULL && "arena: out of memory");
    c->next     = NULL;
    c->capacity = capacity;
    c->used     = 0;
    return c;
}

void *arena_alloc(Arena *a, size_t size) {
    if (size == 0) return NULL;
    size = arena_align_up(size);

    if (a->end == NULL) {
        size_t cap = ARENA_DEFAULT_CAPACITY;
        if (cap < size) cap = size;
        a->begin = arena_new_chunk(cap);
        a->end   = a->begin;
    }

    while (a->end->used + size > a->end->capacity && a->end->next != NULL) {
        a->end = a->end->next;
    }

    if (a->end->used + size > a->end->capacity) {
        size_t cap = ARENA_DEFAULT_CAPACITY;
        if (cap < size) cap = size;
        a->end->next = arena_new_chunk(cap);
        a->end       = a->end->next;
    }

    void *ptr = a->end->data + a->end->used;
    a->end->used += size;
    return ptr;
}

void *arena_realloc(Arena *a, void *oldptr, size_t oldsz, size_t newsz) {
    if (newsz <= oldsz) return oldptr;
    void *newptr = arena_alloc(a, newsz);
    if (oldptr != NULL && oldsz > 0) memcpy(newptr, oldptr, oldsz);
    return newptr;
}

char *arena_strndup(Arena *a, const char *str, size_t len) {
    char *dup = (char *)arena_alloc(a, len + 1);
    memcpy(dup, str, len);
    dup[len] = '\0';
    return dup;
}

char *arena_strdup(Arena *a, const char *cstr) {
    return arena_strndup(a, cstr, strlen(cstr));
}

void *arena_memdup(Arena *a, const void *data, size_t size) {
    void *dup = arena_alloc(a, size);
    memcpy(dup, data, size);
    return dup;
}

Arena_Mark arena_snapshot(Arena *a) {
    if (a->end == NULL) {
        Arena_Mark m = { 0 };
        return m;
    }
    Arena_Mark m;
    m.chunk = a->end;
    m.used  = a->end->used;
    return m;
}

void arena_rewind(Arena *a, Arena_Mark m) {
    if (m.chunk == NULL) {
        arena_reset(a);
        return;
    }
    m.chunk->used = m.used;
    for (Arena_Chunk *c = m.chunk->next; c != NULL; c = c->next)
        c->used = 0;
    a->end = m.chunk;
}

void arena_reset(Arena *a) {
    for (Arena_Chunk *c = a->begin; c != NULL; c = c->next)
        c->used = 0;
    a->end = a->begin;
}

void arena_free(Arena *a) {
    Arena_Chunk *c = a->begin;
    while (c != NULL) {
        Arena_Chunk *next = c->next;
        ARENA_FREE(c);
        c = next;
    }
    a->begin = NULL;
    a->end   = NULL;
}

#endif // ARENA_IMPLEMENTATION_DONE
#endif // ARENA_IMPLEMENTATION
