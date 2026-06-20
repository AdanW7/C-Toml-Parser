#ifndef TOML_H
#define TOML_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum e_toml_type {
    eTomlUnknown,
    eTomlString,
    eTomlInt,
    eTomlFloat,
    eTomlBool,
    eTomlDate,
    eTomlTime,
    eTomlDatetime,
    eTomlDatetimeTz,
    eTomlArray,
    eTomlTable,
} e_toml_type;

typedef enum e_toml_version {
    eTomlVersion11 = 0,
    eTomlVersion10 = 1,
} e_toml_version;

typedef enum e_toml_table_state {
    eTomlTableImplicit,
    eTomlTableHeader,
    eTomlTableDefined,
    eTomlTableInline,
    eTomlTableFrozen,
} e_toml_table_state;

typedef struct toml_node_t toml_node_t;
typedef struct toml_entry_t toml_entry_t;

/* ---------------------------------------------------------------------------
 * Span (non-owning string view)
 * -------------------------------------------------------------------------*/
#define TOML_DEFINE_SPAN(T)                                                    \
    struct T##_span_s {                                                        \
        const T *ptr;                                                          \
        int len;                                                               \
    };                                                                         \
    typedef struct T##_span_s T##_span_s;

/* ---------------------------------------------------------------------------
 * slice (owning string view)
 * -------------------------------------------------------------------------*/
#define TOML_DEFINE_SLICE(T)                                                   \
    struct T##_slice_s {                                                       \
        T *ptr;                                                                \
        int len;                                                               \
    };                                                                         \
    typedef struct T##_slice_s T##_slice_s;

TOML_DEFINE_SPAN(char)
TOML_DEFINE_SLICE(char)

typedef struct {
    int16_t year, month, day;
    int16_t hour, minute, second;
    int32_t usec;
    int16_t tz;
} toml_timestamp_t;

struct toml_node_t {
    e_toml_type type;
    e_toml_table_state table_state;
    int line;
    int col;
    union {
        char_span_s string;
        int64_t i64;
        double f64;
        bool boolean;
        toml_timestamp_t timestamp;
        struct {
            int32_t len;
            toml_node_t *items;
        } array;
        struct {
            int32_t len;
            toml_entry_t *entries;
        } table;
    } as;
};

struct toml_entry_t {
    const char *key;
    int key_len;
    toml_node_t value;
};

typedef void *(*toml_realloc_fn)(void *ptr, size_t size);
typedef void (*toml_free_fn)(void *ptr);

typedef struct toml_alloc_t {
    toml_realloc_fn realloc;
    toml_free_fn free;
} toml_alloc_t;

typedef enum {
    eTOML_INPUT_SRC,
    eTOML_INPUT_FILE,
    eTOML_INPUT_PATH
} etoml_input_kind;

typedef struct {
    etoml_input_kind kind;
    e_toml_version version;
    union {
        char_span_s input;
        FILE *fp;
        const char *path;
    };
} toml_input_t;

typedef struct toml_result_t {
    bool ok;
    toml_node_t root;
    char errmsg[512];
    void *_internal;
} toml_result_t;

bool toml_is_string(toml_node_t n);
bool toml_is_int(toml_node_t n);
bool toml_is_float(toml_node_t n);
bool toml_is_bool(toml_node_t n);
bool toml_is_date(toml_node_t n);
bool toml_is_time(toml_node_t n);
bool toml_is_datatime(toml_node_t n);
bool toml_is_datatime_tz(toml_node_t n);
bool toml_is_array(toml_node_t n);
bool toml_is_table(toml_node_t n);

const char *toml_string(toml_node_t n);
int toml_string_len(toml_node_t n);
int64_t toml_int(toml_node_t n);
double toml_float(toml_node_t n);
bool toml_bool(toml_node_t n);
toml_timestamp_t toml_timestamp(toml_node_t n);
toml_result_t toml_clone(const toml_result_t *src);
bool toml_found(toml_node_t n);

toml_alloc_t toml_default_alloc(void);
void toml_set_alloc(toml_alloc_t allocator);

toml_result_t toml_parse(toml_input_t input);
toml_result_t toml_parse_file_path(const char *path);
toml_result_t toml_parse_file(FILE *fp);
toml_result_t toml_parse_span(char_span_s input);
void toml_free(toml_result_t result);

toml_node_t toml_get(toml_node_t table, const char *key);
toml_node_t toml_seek(toml_node_t table, const char *dotted_key);

toml_result_t toml_merge(const toml_result_t *self, const toml_result_t *other);
bool toml_equiv(const toml_result_t *self, const toml_result_t *other);

/* ---------------------------------------------------------------------------
 * Convenience macros parsing toml data / files
 * -------------------------------------------------------------------------*/
#define TOML_FROM_STR(s)                                                       \
    (toml_input_t) {                                                           \
        eTOML_INPUT_SRC, eTomlVersion11, {                                     \
            .input = {(s), (int)strlen(s) }                                    \
        }                                                                      \
    }
#define TOML_FROM_SPAN(p, n)                                                   \
    (toml_input_t) {                                                           \
        eTOML_INPUT_SRC, eTomlVersion11, {                                     \
            .input = {(p), (n) }                                               \
        }                                                                      \
    }
#define TOML_FROM_FILE(f)                                                      \
    (toml_input_t) {                                                           \
        eTOML_INPUT_FILE, eTomlVersion11, {                                    \
            .fp = (f)                                                          \
        }                                                                      \
    }
#define TOML_FROM_PATH(p)                                                      \
    (toml_input_t) {                                                           \
        eTOML_INPUT_PATH, eTomlVersion11, {                                    \
            .path = (p)                                                        \
        }                                                                      \
    }
#define TOML_FROM_STR_V10(s)                                                   \
    (toml_input_t) {                                                           \
        eTOML_INPUT_SRC, eTomlVersion10, {                                     \
            .input = {(s), (int)strlen(s) }                                    \
        }                                                                      \
    }
#define TOML_FROM_SPAN_V10(p, n)                                               \
    (toml_input_t) {                                                           \
        eTOML_INPUT_SRC, eTomlVersion10, {                                     \
            .input = {(p), (n) }                                               \
        }                                                                      \
    }
#define TOML_FROM_FILE_V10(f)                                                  \
    (toml_input_t) {                                                           \
        eTOML_INPUT_FILE, eTomlVersion10, {                                    \
            .fp = (f)                                                          \
        }                                                                      \
    }
#define TOML_FROM_PATH_V10(p)                                                  \
    (toml_input_t) {                                                           \
        eTOML_INPUT_PATH, eTomlVersion10, {                                    \
            .path = (p)                                                        \
        }                                                                      \
    }

/* ---------------------------------------------------------------------------
 * Convenience macros for table iteration and access
 * -------------------------------------------------------------------------*/

#define TOML_TABLE_LEN(node) ((node)->as.table.len)

#define TOML_TABLE_ENTRY(node, i) ((node)->as.table.entries + (i))

#define TOML_TABLE_VALUE(node, i) (&TOML_TABLE_ENTRY((node), (i))->value)

#define TOML_ARRAY_LEN(node) ((node)->as.array.len)

#define TOML_ARRAY_ITEM(node, i) ((node)->as.array.items + (i))

#define TOML_FOREACH_PTR(type, var, first, count)                              \
    for (type * (var) = (first), *var##_end_ = (var) + (count);                \
         (var) < var##_end_; ++(var))

#define TOML_ENUMERATE_PTR(type, var, idx, first, count)                       \
    for (type *var##_first_ = (first), *(var) = var##_first_,                  \
              *var##_end_ = var##_first_ + (count);                            \
         (var) < var##_end_; ++(var))                                          \
        for (int(idx) = (int)((var) - var##_first_), var##_once_ = 1;          \
             var##_once_; var##_once_ = 0)

#define TOML_TABLE_FOREACH(node, entry_var)                                    \
    TOML_FOREACH_PTR (toml_entry_t, entry_var, TOML_TABLE_ENTRY((node), 0),    \
                      TOML_TABLE_LEN(node))

#define TOML_ARRAY_FOREACH(node, item_var)                                     \
    TOML_FOREACH_PTR (toml_node_t, item_var, TOML_ARRAY_ITEM((node), 0),       \
                      TOML_ARRAY_LEN(node))

#define TOML_TABLE_ENUMERATE(node, entry_var, idx_var)                         \
    TOML_ENUMERATE_PTR (toml_entry_t, entry_var, idx_var,                      \
                        TOML_TABLE_ENTRY((node), 0), TOML_TABLE_LEN(node))

#define TOML_ARRAY_ENUMERATE(node, item_var, idx_var)                          \
    TOML_ENUMERATE_PTR (toml_node_t, item_var, idx_var,                        \
                        TOML_ARRAY_ITEM((node), 0), TOML_ARRAY_LEN(node))

/* ---------------------------------------------------------------------------
 * Convenience macros for Printing macros
 * -------------------------------------------------------------------------*/
#define TOML_DATE_FMT        "%04d-%02d-%02d"
#define TOML_TIME_FMT        "%02d:%02d:%02d.%06d"
#define TOML_DATETIME_FMT    TOML_DATE_FMT "T" TOML_TIME_FMT
#define TOML_TZ_FMT          "%c%02d:%02d"
#define TOML_DATETIME_TZ_FMT TOML_DATETIME_FMT " " TOML_TZ_FMT

#define TOML_DATE_ARGS(ts)     (ts).year, (ts).month, (ts).day
#define TOML_TIME_ARGS(ts)     (ts).hour, (ts).minute, (ts).second, (ts).usec
#define TOML_DATETIME_ARGS(ts) TOML_DATE_ARGS(ts), TOML_TIME_ARGS(ts)

#ifdef __cplusplus
}
#endif /*__cplusplus*/
#endif /* TOML_H */
