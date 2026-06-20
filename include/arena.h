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

#ifdef __cplusplus
extern "C" {
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
#ifdef __cplusplus
}
#endif /*__cplusplus*/

#endif // ARENA_H_
