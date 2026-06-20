#include "toml.h"
#include "arena.h"

#include <assert.h>
#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * forward declarations
 * -------------------------------------------------------------------------*/
typedef struct toml_token_t toml_token_t;
typedef struct toml_lexer_t toml_lexer_t;
typedef struct toml_lex_mark_t toml_lex_mark_t;
typedef struct toml_parser_t toml_parser_t;

typedef enum e_lex_mode {
    eLexHeader,
    eLexKey,
    eLexValue,
} e_lex_mode;

static int utf8_to_ucs(const char *orig, int len, uint32_t *out);
static int ucs_to_utf8(uint32_t code, char buf[4]);
static void lexer_init(toml_lexer_t *lexer, const char *src, int len,
                       char *errbuf, int errbufsz, e_toml_version version);
static int lex_header(toml_lexer_t *lexer, toml_token_t *token);
static int lex_key(toml_lexer_t *lexer, toml_token_t *token);
static int lex_value(toml_lexer_t *lexer, toml_token_t *token);
static int lex_next(toml_lexer_t *lexer, e_lex_mode mode, toml_token_t *token);
static toml_lex_mark_t lex_mark(toml_lexer_t *lexer);
static void lex_restore(toml_lexer_t *lexer, toml_lex_mark_t mark);
static int parse_norm(toml_parser_t *parser, toml_token_t token,
                      char_span_s *out);
static int parse_value(toml_parser_t *parser, toml_token_t token,
                       toml_node_t *out);
static int parse_kv_expr(toml_parser_t *parser, toml_token_t token);
static int parse_std_table(toml_parser_t *parser, toml_token_t token);
static int parse_array_table(toml_parser_t *parser, toml_token_t token);

/* ---------------------------------------------------------------------------
 * Arena-backed allocation
 * -------------------------------------------------------------------------*/
typedef struct toml_alloc_hdr_t {
    size_t size;
} toml_alloc_hdr_t;

static Arena toml_fallback_arena = { 0 };
static Arena *toml_active_arena  = &toml_fallback_arena;

static Arena *toml_set_active_arena(Arena *arena) {
    Arena *old        = toml_active_arena;
    toml_active_arena = arena ? arena : &toml_fallback_arena;
    return old;
}

static void *toml_arena_alloc(size_t size) {
    if (size == 0) size = 1;
    toml_alloc_hdr_t *hdr = (toml_alloc_hdr_t *)arena_alloc(
        toml_active_arena, sizeof(toml_alloc_hdr_t) + size);
    if (!hdr) return NULL;
    hdr->size = size;
    return hdr + 1;
}

static void *toml_arena_realloc(void *ptr, size_t size) {
    if (!ptr) return toml_arena_alloc(size);
    if (size == 0) size = 1;
    toml_alloc_hdr_t *old = ((toml_alloc_hdr_t *)ptr) - 1;
    void *next            = toml_arena_alloc(size);
    if (!next) return NULL;
    memcpy(next, ptr, old->size < size ? old->size : size);
    return next;
}

static void toml_arena_free(void *ptr) {
    (void)ptr;
}

#define TOML_ALLOC(n)      toml_arena_alloc((n))
#define TOML_REALLOC(p, n) toml_arena_realloc((p), (n))
#define TOML_FREE(p)       toml_arena_free((p))

toml_alloc_t toml_default_alloc(void) {
    return (toml_alloc_t){ 0 };
}

void toml_set_alloc(toml_alloc_t allocactor) {
    (void)allocactor;
}

typedef toml_node_t *toml_node_ptr_t; /* may be NULL  */
typedef toml_node_t *toml_node_ref_t; /* must not be NULL */

/* ---------------------------------------------------------------------------
     * Error buffer helpers
     * -------------------------------------------------------------------------*/
#define PTR_AVAIL(ptr, end) ((ptr) < (end) ? (size_t)((end) - (ptr)) : 0)
static int errbuf_set(char_slice_s err_buf, int line, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char *write_pos  = err_buf.ptr;
    char *buffer_end = write_pos + err_buf.len;
    if (line) {
        snprintf(write_pos, PTR_AVAIL(write_pos, buffer_end), "(line %d) ",
                 line);
        write_pos += strlen(write_pos);
    }
    vsnprintf(write_pos, PTR_AVAIL(write_pos, buffer_end), fmt, args);
    va_end(args);
    return -1;
}
#undef PTR_AVAIL

typedef struct toml_pool_t {
    Arena arena;
    size_t used;
} toml_pool_t;

static toml_pool_t *pool_create(size_t initial) {
    (void)initial;
    Arena arena       = { 0 };
    toml_pool_t *pool = (toml_pool_t *)arena_alloc(&arena, sizeof(*pool));
    if (!pool) return NULL;
    *pool = (toml_pool_t){ .arena = arena };
    return pool;
}

static void pool_destroy(toml_pool_t *pool) {
    if (!pool) return;
    Arena arena = pool->arena;
    arena_free(&arena);
}

static size_t pool_total_used(toml_pool_t *pool) {
    return pool ? pool->used : 0;
}

static char *pool_alloc(toml_pool_t *pool, size_t n) {
    if (!pool) return NULL;
    char *ret = (char *)arena_alloc(&pool->arena, n ? n : 1);
    if (!ret) return NULL;
    pool->used += n;
    return ret;
}

/* ---------------------------------------------------------------------------
 * Growable cell
 * -------------------------------------------------------------------------*/
typedef struct cell_hdr_t {
    uint32_t cap;
    uint32_t top;
} cell_hdr_t;

static char *cell_resize(char *p, int size) {
    assert(size >= 0);
    if (!p) {
        cell_hdr_t *hdr = TOML_REALLOC(NULL, (size_t)size + sizeof(cell_hdr_t));
        if (!hdr) return NULL;
        hdr->cap = hdr->top = (uint32_t)size;
        return (char *)&hdr[1];
    }
    cell_hdr_t *hdr = (cell_hdr_t *)(p - sizeof(cell_hdr_t));
    if ((uint32_t)size <= hdr->cap) {
        hdr->top = (uint32_t)size;
        return p;
    }
    size_t newcap =
        (size < 256) ? (size_t)size * 2 : (size_t)size + (size_t)size / 2 + 64;
    cell_hdr_t *nh = TOML_REALLOC(hdr, newcap + sizeof(cell_hdr_t));
    if (!nh) return NULL;
    nh->cap = (uint32_t)newcap;
    nh->top = (uint32_t)size;
    return (char *)&nh[1];
}

static void cell_free(char *p) {
    if (p) TOML_FREE((cell_hdr_t *)(p - sizeof(cell_hdr_t)));
}

/* ---------------------------------------------------------------------------
     * Limits
     * -------------------------------------------------------------------------*/
#define NESTING_MAX     512
#define TABLE_COUNT_MAX INT32_MAX
#define ARRAY_COUNT_MAX INT32_MAX

/* ---------------------------------------------------------------------------
 * Lexer token types
 * -------------------------------------------------------------------------*/
typedef enum e_tok_type {
    eTokDot = 1,
    eTokEqual,
    eTokComma,
    eTokLBrack,
    eTokLLBrack,
    eTokRBrack,
    eTokRRBrack,
    eTokLBrace,
    eTokRBrace,
    eTokLit,
    eTokString,
    eTokMlString,
    eTokLitString,
    eTokMlLitString,
    eTokTime,
    eTokDate,
    eTokDatetime,
    eTokDatetimeTz,
    eTokInteger,
    eTokFloat,
    eTokBool,
    eTokNewline,
    eTokEof = -5000,
} e_tok_type;

typedef enum e_sep_state {
    eSepNeedValue,
    eSepNeedComma,
} e_sep_state;

typedef enum e_table_context {
    eTableCtxRoot,
    eTableCtxStd,
    eTableCtxArray,
} e_table_context;

typedef enum e_walk_mode {
    eWalkHeader,
    eWalkDotted,
} e_walk_mode;

struct toml_token_t {
    e_tok_type type;
    int line;
    int col;
    char_span_s raw;
    union {
        const char *esc_start;
        int64_t i64;
        double f64;
        bool boolean;
        toml_timestamp_t timestamp;
    } as;
};

/* ---------------------------------------------------------------------------
 * Lexer
 * -------------------------------------------------------------------------*/
struct toml_lexer_t {
    const char *src;
    const char *end;
    const char *cur;
    int line;
    const char *line_start;
    char *errmsg;
    char_slice_s err_buf;
    e_toml_version version;
    char nesting[NESTING_MAX];
    int nesting_depth;
};

struct toml_lex_mark_t {
    toml_lexer_t *lexer;
    const char *cur;
    int line;
    const char *line_start;
    int nesting_depth;
};

/* ---------------------------------------------------------------------------
     * Key parts (dotted key decomposition)
     * -------------------------------------------------------------------------*/
#define KEY_PARTS_INLINE 8

typedef struct toml_keypart_t {
    size_t count;
    size_t cap;
    char_span_s *parts;
    char_span_s inline_[KEY_PARTS_INLINE];
} toml_keypart_t;

static void keypart_free(toml_keypart_t *kp) {
    if (kp->parts && kp->parts != kp->inline_) TOML_FREE(kp->parts);
    kp->parts = NULL;
    kp->cap   = 0;
    kp->count = 0;
}

static int keypart_push(toml_keypart_t *kp, char_span_s part,
                        const char **reason) {
    if (kp->cap == 0) {
        kp->parts = kp->inline_;
        kp->cap   = KEY_PARTS_INLINE;
    } else if (kp->count >= kp->cap) {
        size_t newcap = kp->cap * 2;
        char_span_s *newparts;
        if (kp->parts == kp->inline_) {
            newparts = TOML_ALLOC(sizeof(char_span_s) * (size_t)newcap);
            if (!newparts) {
                *reason = "out of memory";
                return -1;
            }
            memcpy(newparts, kp->inline_, sizeof(char_span_s) * kp->count);
        } else {
            newparts = TOML_REALLOC(kp->parts, sizeof(char_span_s) * newcap);
            if (!newparts) {
                *reason = "out of memory";
                return -1;
            }
        }
        kp->parts = newparts;
        kp->cap   = (int)newcap;
    }
    kp->parts[kp->count++] = part;
    return 0;
}

/* ---------------------------------------------------------------------------
 * Parser
 * -------------------------------------------------------------------------*/
struct toml_parser_t {
    toml_lexer_t lexer;
    toml_node_t root;
    struct {
        toml_node_t *table;
        e_table_context kind;
        int line;
    } cur;
    toml_pool_t *pool;
    char_slice_s errbuff;
    e_toml_version version;
};

/* ---------------------------------------------------------------------------
 * sentinel node
 * -------------------------------------------------------------------------*/
static const toml_node_t NODE_ZERO = { 0 };

/* ---------------------------------------------------------------------------
 * Timestamp helpers
 * -------------------------------------------------------------------------*/

static bool ts_date_equiv(toml_timestamp_t a, toml_timestamp_t b) {
    return a.year == b.year && a.month == b.month && a.day == b.day;
}
static bool ts_time_equiv(toml_timestamp_t a, toml_timestamp_t b) {
    return a.hour == b.hour && a.minute == b.minute && a.second == b.second
           && a.usec == b.usec;
}
#define TOML_TIMESTAMP_UNSET                                                   \
    ((toml_timestamp_t){ .year   = -1,                                         \
                         .month  = -1,                                         \
                         .day    = -1,                                         \
                         .hour   = -1,                                         \
                         .minute = -1,                                         \
                         .second = -1,                                         \
                         .usec   = -1,                                         \
                         .tz     = -1 })

/* ---------------------------------------------------------------------------
 * Node construction helpers
 * -------------------------------------------------------------------------*/

static toml_node_t make_node(e_toml_type type) {
    toml_node_t n = { 0 };
    n.type        = type;
    switch (type) {
        case eTomlDate:
        case eTomlTime:
        case eTomlDatetime:
        case eTomlDatetimeTz:
            n.as.timestamp = TOML_TIMESTAMP_UNSET;
            break;
        default:
            break;
    }
    return n;
}

static toml_node_t make_node_at(e_toml_type type, int line, int col) {
    toml_node_t n = make_node(type);
    n.line        = line;
    n.col         = col;
    return n;
}

/* ---------------------------------------------------------------------------
 * Node memory management
 * -------------------------------------------------------------------------*/

static void node_free(toml_node_t *n) {
    if (n->type == eTomlTable) {
        TOML_TABLE_FOREACH (n, e) {
            node_free(&e->value);
        }
        cell_free((char *)n->as.table.entries);
    } else if (n->type == eTomlArray) {
        TOML_ARRAY_FOREACH (n, item) {
            node_free(item);
        }
        cell_free((char *)n->as.array.items);
    }
    *n = NODE_ZERO;
}

/* ---------------------------------------------------------------------------
 * Table state helpers
 * -------------------------------------------------------------------------*/

static bool table_is_extensible(toml_parser_t *parser, toml_node_t *tab) {
    if (tab->table_state == eTomlTableFrozen) return false;
    if (tab->table_state == eTomlTableInline
        && parser->version == eTomlVersion10)
        return false;
    return true;
}

static bool table_can_define(toml_node_t *tab) {
    return tab->table_state == eTomlTableImplicit
           || tab->table_state == eTomlTableHeader
           || tab->table_state == eTomlTableInline;
}

static void set_table_state_recursive(toml_node_t *node,
                                      e_toml_table_state state) {
    if (node->type == eTomlTable) node->table_state = state;
    if (node->type == eTomlArray) {
        TOML_ARRAY_FOREACH (node, item) {
            set_table_state_recursive(item, state);
        }
    } else if (node->type == eTomlTable) {
        TOML_TABLE_FOREACH (node, entry) {
            set_table_state_recursive(&entry->value, state);
        }
    }
}

/* ---------------------------------------------------------------------------
 * Table operations
 * -------------------------------------------------------------------------*/

/* Returns index of entry with matching key, or -1. */
static int table_find(toml_node_t *tab, char_span_s key) {
    assert(tab->type == eTomlTable);
    TOML_TABLE_ENUMERATE (tab, e, i) {
        if (e->key_len == key.len && 0 == memcmp(e->key, key.ptr, key.len)) {
            return i;
        }
    }
    return -1;
}

static toml_node_ptr_t table_emplace(toml_node_t *tab, char_span_s key,
                                     const char **reason) {
    assert(tab->type == eTomlTable);
    int i = table_find(tab, key);
    if (i >= 0) return &tab->as.table.entries[i].value;

    int n = tab->as.table.len;
    if (n >= TABLE_COUNT_MAX) {
        *reason = "table too large";
        return NULL;
    }

    toml_entry_t *new_entries = (toml_entry_t *)cell_resize(
        (char *)tab->as.table.entries, sizeof(toml_entry_t) * (n + 1));
    if (!new_entries) {
        *reason = "out of memory";
        return NULL;
    }

    tab->as.table.entries            = new_entries;
    tab->as.table.entries[n].key     = key.ptr;
    tab->as.table.entries[n].key_len = key.len;
    tab->as.table.entries[n].value   = NODE_ZERO;
    tab->as.table.len                = n + 1;

    return &tab->as.table.entries[n].value;
}

static int table_insert(toml_node_t *tab, char_span_s key, toml_node_t val,
                        const char **reason) {
    assert(tab->type == eTomlTable);
    toml_node_ptr_t slot = table_emplace(tab, key, reason);
    if (!slot) return -1;
    if (slot->type) {
        *reason = "duplicate key";
        return -1;
    }
    *slot = val;
    return 0;
}

static inline toml_node_t *tab_last_value(toml_node_t *tab) {
    assert(tab->type == eTomlTable && tab->as.table.len > 0);
    return &tab->as.table.entries[tab->as.table.len - 1].value;
}

/* ---------------------------------------------------------------------------
 * Array operations
 * -------------------------------------------------------------------------*/

static toml_node_ptr_t arr_push(toml_node_t *arr, const char **reason) {
    assert(arr->type == eTomlArray);
    int n = arr->as.array.len;
    if (n >= ARRAY_COUNT_MAX) {
        *reason = "array too large";
        return NULL;
    }
    toml_node_t *elems = (toml_node_t *)cell_resize(
        (char *)arr->as.array.items, sizeof(toml_node_t) * (n + 1));
    if (!elems) {
        *reason = "out of memory";
        return NULL;
    }
    arr->as.array.items    = elems;
    arr->as.array.items[n] = NODE_ZERO;
    arr->as.array.len      = n + 1;
    return &arr->as.array.items[n];
}

/* ---------------------------------------------------------------------------
 * Node deep copy
 * -------------------------------------------------------------------------*/

static int node_copy(toml_node_t *dst, toml_node_t src, toml_pool_t *pool,
                     const char **reason) {
    *dst             = make_node_at(src.type, src.line, src.col);
    dst->table_state = src.table_state;
    switch (src.type) {
        case eTomlString: {
            char *buf = pool_alloc(pool, src.as.string.len + 1);
            if (!buf) {
                *reason = "out of memory";
                goto cleanup;
            }
            dst->as.string.ptr = buf;
            dst->as.string.len = src.as.string.len;
            memcpy(buf, src.as.string.ptr, src.as.string.len + 1);
            break;
        }
        case eTomlTable:
            TOML_TABLE_FOREACH (&src, se) {
                char_span_s key      = { se->key, se->key_len };
                toml_node_ptr_t slot = table_emplace(dst, key, reason);
                if (!slot) goto cleanup;
                if (node_copy(slot, se->value, pool, reason)) goto cleanup;
            }
            break;
        case eTomlArray:
            TOML_ARRAY_FOREACH (&src, item) {
                toml_node_ptr_t slot = arr_push(dst, reason);
                if (!slot) goto cleanup;
                if (node_copy(slot, *item, pool, reason)) goto cleanup;
            }
            break;
        default:
            *dst = src;
            break;
    }
    return 0;
cleanup:
    node_free(dst);
    return -1;
}

/* ---------------------------------------------------------------------------
 * Node equivalence
 * -------------------------------------------------------------------------*/

static bool node_equiv(toml_node_t a, toml_node_t b) {
    if (a.type != b.type) return false;
    switch (a.type) {
        case eTomlString:
            return a.as.string.len == b.as.string.len
                   && 0
                          == memcmp(a.as.string.ptr, b.as.string.ptr,
                                    a.as.string.len);
        case eTomlInt:
            return a.as.i64 == b.as.i64;
        case eTomlFloat:
            return a.as.f64 == b.as.f64 || (isnan(a.as.f64) && isnan(b.as.f64));
        case eTomlBool:
            return !!a.as.boolean == !!b.as.boolean;
        case eTomlDate:
            return ts_date_equiv(a.as.timestamp, b.as.timestamp);
        case eTomlTime:
            return ts_time_equiv(a.as.timestamp, b.as.timestamp);
        case eTomlDatetime:
            return ts_date_equiv(a.as.timestamp, b.as.timestamp)
                   && ts_time_equiv(a.as.timestamp, b.as.timestamp);
        case eTomlDatetimeTz:
            return ts_date_equiv(a.as.timestamp, b.as.timestamp)
                   && ts_time_equiv(a.as.timestamp, b.as.timestamp)
                   && a.as.timestamp.tz == b.as.timestamp.tz;
        case eTomlArray: {
            int n = a.as.array.len;
            if (n != b.as.array.len) return false;
            TOML_ARRAY_ENUMERATE (&a, ai, i) {
                if (!node_equiv(*ai, *TOML_ARRAY_ITEM(&b, i))) {
                    return false;
                }
            }
            return true;
        }
        case eTomlTable: {
            if (a.as.table.len != b.as.table.len) return false;
            TOML_TABLE_FOREACH (&a, ae) {
                char_span_s key = { ae->key, ae->key_len };
                bool found      = false;
                TOML_TABLE_FOREACH (&b, be) {
                    if (be->key_len == key.len
                        && 0 == memcmp(be->key, key.ptr, key.len)) {
                        if (!node_equiv(ae->value, be->value)) return false;
                        found = true;
                        break;
                    }
                }
                if (!found) return false;
            }
            return true;
        }

        default:
            break;
    }
    return false;
}

/* ---------------------------------------------------------------------------
 * Node merge
 * -------------------------------------------------------------------------*/

static bool is_table_array(toml_node_t node) {
    if (node.type != eTomlArray || node.as.array.len == 0) return false;
    TOML_ARRAY_FOREACH (&node, item) {
        if (item->type != eTomlTable) {
            return false;
        }
    }
    return true;
}

static int node_merge(toml_node_t *dst, toml_node_t src, toml_pool_t *pool,
                      const char **reason) {
    if (dst->type != src.type) {
        node_free(dst);
        return node_copy(dst, src, pool, reason);
    }
    switch (src.type) {
        case eTomlTable:
            TOML_TABLE_FOREACH (&src, se) {
                char_span_s key      = { se->key, se->key_len };
                toml_node_ptr_t slot = table_emplace(dst, key, reason);
                if (!slot) return -1;
                if (slot->type) {
                    if (node_merge(slot, se->value, pool, reason)) return -1;
                } else {
                    node_free(slot);
                    if (node_copy(slot, se->value, pool, reason)) return -1;
                }
            }
            return 0;
        case eTomlArray:
            if (is_table_array(src)) {
                TOML_ARRAY_FOREACH (&src, item) {
                    toml_node_ptr_t slot = arr_push(dst, reason);
                    if (!slot) return -1;
                    if (node_copy(slot, *item, pool, reason)) return -1;
                }
                return 0;
            }
        default:
            break;
    }
    node_free(dst);
    return node_copy(dst, src, pool, reason);
}

/* ---------------------------------------------------------------------------
 * Token -> node converters
 * -------------------------------------------------------------------------*/

static int tok_to_string(toml_parser_t *parser, toml_token_t token,
                         toml_node_t *out) {
    *out = make_node_at(eTomlString, token.line, token.col);
    char_span_s span;
    if (parse_norm(parser, token, &span)) return -1;
    out->as.string.ptr = span.ptr;
    out->as.string.len = span.len;
    return 0;
}

static int tok_to_timestamp(toml_parser_t *parser, toml_token_t token,
                            toml_node_t *out) {
    (void)parser;
    static const e_toml_type map[] = {
        [eTokTime]       = eTomlTime,
        [eTokDate]       = eTomlDate,
        [eTokDatetime]   = eTomlDatetime,
        [eTokDatetimeTz] = eTomlDatetimeTz,
    };
    switch (token.type) {
        case eTokTime:
        case eTokDate:
        case eTokDatetime:
        case eTokDatetimeTz:
            break;
        default:
            assert(0 && "unexpected token type");
            return -1;
    }
    *out              = make_node_at(map[token.type], token.line, token.col);
    out->as.timestamp = token.as.timestamp;
    return 0;
}

static int tok_to_int(toml_parser_t *parser, toml_token_t token,
                      toml_node_t *out) {
    (void)parser;
    assert(token.type == eTokInteger);
    *out        = make_node_at(eTomlInt, token.line, token.col);
    out->as.i64 = token.as.i64;
    return 0;
}

static int tok_to_float(toml_parser_t *parser, toml_token_t token,
                        toml_node_t *out) {
    (void)parser;
    assert(token.type == eTokFloat);
    *out        = make_node_at(eTomlFloat, token.line, token.col);
    out->as.f64 = token.as.f64;
    return 0;
}

static int tok_to_bool(toml_parser_t *parser, toml_token_t token,
                       toml_node_t *out) {
    (void)parser;
    assert(token.type == eTokBool);
    *out            = make_node_at(eTomlBool, token.line, token.col);
    out->as.boolean = token.as.boolean;
    return 0;
}
typedef int (*tok_converter_fn)(toml_parser_t *, toml_token_t, toml_node_t *);
static const tok_converter_fn tok_converters[] = {
    [eTokString] = tok_to_string,      [eTokMlString] = tok_to_string,
    [eTokLitString] = tok_to_string,   [eTokMlLitString] = tok_to_string,
    [eTokTime] = tok_to_timestamp,     [eTokDate] = tok_to_timestamp,
    [eTokDatetime] = tok_to_timestamp, [eTokDatetimeTz] = tok_to_timestamp,
    [eTokInteger] = tok_to_int,        [eTokFloat] = tok_to_float,
    [eTokBool] = tok_to_bool,
};
#define TOK_CONVERTERS_SIZE                                                    \
    (int)(sizeof(tok_converters) / sizeof(tok_converters[0]))

/* ---------------------------------------------------------------------------
 * Parser – key parsing
 * -------------------------------------------------------------------------*/

static int parse_key(toml_parser_t *parser, toml_token_t token,
                     toml_keypart_t *out) {
    if (token.type != eTokString && token.type != eTokLitString
        && token.type != eTokLit)
        return errbuf_set(parser->errbuff, token.line, "missing key");

    const char *reason;
    char_span_s span;
    if (parse_norm(parser, token, &span))
        return errbuf_set(parser->errbuff, token.line,
                          "unable to normalize key string");
    if (keypart_push(out, span, &reason))
        return errbuf_set(parser->errbuff, token.line, "%s", reason);

    for (;;) {
        toml_lex_mark_t mark = lex_mark(&parser->lexer);
        if (lex_next(&parser->lexer, eLexKey, &token)) return -1;
        if (token.type != eTokDot) {
            lex_restore(&parser->lexer, mark);
            break;
        }
        if (lex_key(&parser->lexer, &token)) return -1;
        if (token.type != eTokString && token.type != eTokLitString
            && token.type != eTokLit)
            return errbuf_set(parser->errbuff, token.line,
                              "expected key after '.'");
        if (parse_norm(parser, token, &span)) return -1;
        if (keypart_push(out, span, &reason))
            return errbuf_set(parser->errbuff, token.line, "%s", reason);
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * Parser – table walk
 * -------------------------------------------------------------------------*/

static toml_node_ptr_t table_walk(toml_parser_t *parser, int line, int col,
                                  toml_node_t *root, toml_keypart_t *kp,
                                  e_walk_mode mode) {
    toml_node_t *tab = root;
    for (size_t i = 0; i < kp->count; i++) {
        const char *reason;
        int j = table_find(tab, kp->parts[i]);
        if (j < 0) {
            if (!table_is_extensible(parser, tab)) {
                errbuf_set(parser->errbuff, line,
                           "inline table cannot be extended");
                return NULL;
            }
            toml_node_t newtab = make_node_at(eTomlTable, line, col);
            newtab.table_state =
                mode == eWalkHeader ? eTomlTableHeader : eTomlTableImplicit;
            if (table_insert(tab, kp->parts[i], newtab, &reason)) {
                errbuf_set(parser->errbuff, line, "%s", reason);
                return NULL;
            }
            tab = tab_last_value(tab);
            continue;
        }

        toml_node_t *slot = &tab->as.table.entries[j].value;
        if (slot->type == eTomlTable) {
            tab = slot;
            continue;
        }

        if (slot->type == eTomlArray) {
            if (mode == eWalkDotted) {
                errbuf_set(parser->errbuff, line,
                           "previously declared array '%.*s'", kp->parts[i].len,
                           kp->parts[i].ptr);
                return NULL;
            }
            if (slot->as.array.len <= 0) {
                errbuf_set(parser->errbuff, line,
                           "array '%.*s' has no elements", kp->parts[i].len,
                           kp->parts[i].ptr);
                return NULL;
            }
            toml_node_t *last = &slot->as.array.items[slot->as.array.len - 1];
            if (last->type != eTomlTable) {
                errbuf_set(parser->errbuff, line,
                           "array '%.*s' must be an array of tables",
                           kp->parts[i].len, kp->parts[i].ptr);
                return NULL;
            }
            if (last->table_state == eTomlTableFrozen) {
                errbuf_set(parser->errbuff, line,
                           "inline table cannot be extended");
                return NULL;
            }
            tab = last;
            continue;
        }

        errbuf_set(parser->errbuff, line, "cannot locate table at key '%.*s'",
                   kp->parts[i].len, kp->parts[i].ptr);
        return NULL;
    }
    return tab;
}

/* ---------------------------------------------------------------------------
 * Parser – inline array
 * -------------------------------------------------------------------------*/

static int parse_inline_array(toml_parser_t *parser, toml_token_t token,
                              toml_node_t *out) {
    assert(token.type == eTokLBrack);
    *out                  = make_node_at(eTomlArray, token.line, token.col);
    e_sep_state sep_state = eSepNeedValue;

    for (;;) {
        do {
            if (lex_value(&parser->lexer, &token)) return -1;
        } while (token.type == eTokNewline);

        if (token.type == eTokRBrack) break;

        if (token.type == eTokComma) {
            if (sep_state != eSepNeedComma)
                return errbuf_set(parser->errbuff, token.line,
                                  "unexpected comma in array");
            sep_state = eSepNeedValue;
            continue;
        }

        if (sep_state == eSepNeedComma)
            return errbuf_set(parser->errbuff, token.line,
                              "missing comma in array");

        toml_node_t val = NODE_ZERO;
        if (parse_value(parser, token, &val)) {
            node_free(&val);
            return -1;
        }

        const char *reason;
        toml_node_ptr_t slot = arr_push(out, &reason);
        if (!slot) {
            node_free(&val);
            return errbuf_set(parser->errbuff, token.line, "array push: %s",
                              reason);
        }
        *slot     = val;
        sep_state = eSepNeedComma;
    }

    set_table_state_recursive(out, eTomlTableFrozen);
    return 0;
}

/* ---------------------------------------------------------------------------
 * Parser – inline table
 * -------------------------------------------------------------------------*/

static int parse_inline_table(toml_parser_t *parser, toml_token_t token,
                              toml_node_t *out) {
    assert(token.type == eTokLBrace);
    *out                  = make_node_at(eTomlTable, token.line, token.col);
    e_sep_state sep_state = eSepNeedValue;

    for (;;) {
        do {
            if (lex_key(&parser->lexer, &token)) return -1;
        } while (token.type == eTokNewline);

        if (token.type == eTokRBrace) break;

        if (token.type == eTokComma) {
            if (sep_state != eSepNeedComma)
                return errbuf_set(parser->errbuff, token.line,
                                  "unexpected comma in inline table");
            sep_state = eSepNeedValue;
            continue;
        }

        if (sep_state == eSepNeedComma)
            return errbuf_set(parser->errbuff, token.line,
                              "missing comma in inline table");

        int kline = token.line, kcol = token.col;
        toml_keypart_t kp = { 0 };
        if (parse_key(parser, token, &kp)) {
            keypart_free(&kp);
            return -1;
        }
        assert(kp.count > 0);

        char_span_s last_part = kp.parts[--kp.count];
        toml_node_ptr_t tab =
            table_walk(parser, kline, kcol, out, &kp, eWalkDotted);
        if (!tab) {
            keypart_free(&kp);
            return -1;
        }
        if (!table_is_extensible(parser, tab)) {
            keypart_free(&kp);
            return errbuf_set(parser->errbuff, kline,
                              "inline table cannot be extended");
        }
        tab->table_state = eTomlTableDefined;

        if (lex_value(&parser->lexer, &token)) {
            keypart_free(&kp);
            return -1;
        }
        if (token.type != eTokEqual) {
            keypart_free(&kp);
            return errbuf_set(parser->errbuff, token.line, "expected '='");
        }
        if (lex_value(&parser->lexer, &token)) {
            keypart_free(&kp);
            return -1;
        }

        toml_node_t val = NODE_ZERO;
        if (parse_value(parser, token, &val)) {
            node_free(&val);
            keypart_free(&kp);
            return -1;
        }

        const char *reason;
        if (table_insert(tab, last_part, val, &reason)) {
            node_free(&val);
            keypart_free(&kp);
            return errbuf_set(parser->errbuff, kline, "%s", reason);
        }
        keypart_free(&kp);
        sep_state = eSepNeedComma;
    }

    set_table_state_recursive(out, parser->version == eTomlVersion10
                                       ? eTomlTableFrozen
                                       : eTomlTableInline);
    return 0;
}

/* ---------------------------------------------------------------------------
 * Parser – value dispatch
 * -------------------------------------------------------------------------*/

static int parse_value(toml_parser_t *parser, toml_token_t token,
                       toml_node_t *out) {
    *out = NODE_ZERO;
    if (token.type == eTokLBrack) return parse_inline_array(parser, token, out);
    if (token.type == eTokLBrace) return parse_inline_table(parser, token, out);
    if (token.type >= 0 && token.type < TOK_CONVERTERS_SIZE
        && tok_converters[token.type])
        return tok_converters[token.type](parser, token, out);
    return errbuf_set(parser->errbuff, token.line, "expected value");
}

/* ---------------------------------------------------------------------------
 * Parser – top-level [std.table]
 * -------------------------------------------------------------------------*/

static int parse_std_table(toml_parser_t *parser, toml_token_t token) {
    assert(token.type == eTokLBrack);
    if (lex_key(&parser->lexer, &token)) return -1;

    int kline = token.line, kcol = token.col;
    toml_keypart_t kp = { 0 };
    if (parse_key(parser, token, &kp)) {
        keypart_free(&kp);
        return -1;
    }

    if (lex_key(&parser->lexer, &token)) {
        keypart_free(&kp);
        return -1;
    }
    if (token.type != eTokRBrack) {
        keypart_free(&kp);
        return errbuf_set(parser->errbuff, token.line, "expected ']'");
    }

    char_span_s last_part = kp.parts[--kp.count];
    toml_node_ptr_t tab =
        table_walk(parser, kline, kcol, &parser->root, &kp, eWalkHeader);
    if (!tab) {
        keypart_free(&kp);
        return -1;
    }

    int j = table_find(tab, last_part);
    if (j < 0) {
        if (!table_is_extensible(parser, tab)) {
            keypart_free(&kp);
            return errbuf_set(parser->errbuff, kline,
                              "inline table cannot be extended");
        }
        const char *reason;
        toml_node_t newtab = make_node_at(eTomlTable, kline, kcol);
        newtab.table_state = eTomlTableHeader;
        if (table_insert(tab, last_part, newtab, &reason)) {
            keypart_free(&kp);
            return errbuf_set(parser->errbuff, kline, "%s", reason);
        }
        tab = tab_last_value(tab);
    } else {
        tab = &tab->as.table.entries[j].value;
        if (!table_is_extensible(parser, tab)) {
            keypart_free(&kp);
            return errbuf_set(parser->errbuff, kline,
                              "inline table cannot be extended");
        }
        if (!table_can_define(tab)) {
            keypart_free(&kp);
            return errbuf_set(parser->errbuff, kline,
                              tab->table_state == eTomlTableDefined
                                  ? "table defined more than once"
                                  : "table already defined");
        }
    }

    tab->table_state  = eTomlTableDefined;
    tab->line         = kline;
    tab->col          = kcol;
    parser->cur.table = tab;
    parser->cur.kind  = eTableCtxStd;
    parser->cur.line  = kline;
    keypart_free(&kp);
    return 0;
}

/* ---------------------------------------------------------------------------
 * Parser – top-level [[array.table]]
 * -------------------------------------------------------------------------*/

static int parse_array_table(toml_parser_t *parser, toml_token_t token) {
    assert(token.type == eTokLLBrack);
    if (lex_key(&parser->lexer, &token)) return -1;

    int kline = token.line, kcol = token.col;
    toml_keypart_t kp = { 0 };
    if (parse_key(parser, token, &kp)) {
        keypart_free(&kp);
        return -1;
    }

    toml_token_t close;
    if (lex_header(&parser->lexer, &close)) {
        keypart_free(&kp);
        return -1;
    }
    if (close.type != eTokRRBrack) {
        keypart_free(&kp);
        return errbuf_set(parser->errbuff, close.line, "expected ']]'");
    }

    assert(kp.count > 0);
    char_span_s last_part = kp.parts[--kp.count];
    toml_node_ptr_t tab =
        table_walk(parser, kline, kcol, &parser->root, &kp, eWalkHeader);
    if (!tab) {
        keypart_free(&kp);
        return -1;
    }

    const char *reason;
    int idx = table_find(tab, last_part);
    if (idx < 0) {
        if (!table_is_extensible(parser, tab)) {
            keypart_free(&kp);
            return errbuf_set(parser->errbuff, kline,
                              "inline table cannot be extended");
        }
        toml_node_t newarr = make_node_at(eTomlArray, kline, kcol);
        if (table_insert(tab, last_part, newarr, &reason)) {
            keypart_free(&kp);
            return errbuf_set(parser->errbuff, kline, "%s", reason);
        }
        idx = table_find(tab, last_part);
        assert(idx >= 0);
    }

    if (tab->as.table.entries[idx].value.type != eTomlArray) {
        keypart_free(&kp);
        return errbuf_set(parser->errbuff, kline, "entry must be an array");
    }

    toml_node_t *arr = &tab->as.table.entries[idx].value;
    if (arr->table_state == eTomlTableFrozen) {
        keypart_free(&kp);
        return errbuf_set(parser->errbuff, kline,
                          "cannot extend a static array");
    }

    toml_node_ptr_t slot = arr_push(arr, &reason);
    if (!slot) {
        keypart_free(&kp);
        return errbuf_set(parser->errbuff, kline, "%s", reason);
    }
    *slot             = make_node_at(eTomlTable, kline, kcol);
    slot->table_state = eTomlTableDefined;

    parser->cur.table = &arr->as.array.items[arr->as.array.len - 1];
    parser->cur.kind  = eTableCtxArray;
    parser->cur.line  = kline;
    assert(parser->cur.table->type == eTomlTable);
    keypart_free(&kp);
    return 0;
}

/* ---------------------------------------------------------------------------
 * Parser – key/value expression
 * -------------------------------------------------------------------------*/

static int parse_kv_expr(toml_parser_t *parser, toml_token_t token) {
    int kline = token.line, kcol = token.col;
    toml_keypart_t kp = { 0 };
    if (parse_key(parser, token, &kp)) {
        keypart_free(&kp);
        return -1;
    }

    if (lex_key(&parser->lexer, &token)) {
        keypart_free(&kp);
        return -1;
    }
    if (token.type != eTokEqual) {
        keypart_free(&kp);
        return errbuf_set(parser->errbuff, token.line, "expected '='");
    }

    const char *reason;
    toml_node_t *tab      = parser->cur.table;
    size_t original_count = kp.count;
    kp.count--;
    tab      = table_walk(parser, kline, kcol, tab, &kp, eWalkDotted);
    kp.count = original_count;
    if (!tab) {
        keypart_free(&kp);
        return -1;
    }

    if (!table_is_extensible(parser, tab)) {
        keypart_free(&kp);
        return errbuf_set(parser->errbuff, kline,
                          "inline table cannot be extended");
    }
    if (kp.count > 1 && tab->table_state == eTomlTableDefined
        && parser->version == eTomlVersion10) {
        keypart_free(&kp);
        return errbuf_set(
            parser->errbuff, kline,
            "cannot extend previously defined table with dotted key");
    }

    if (lex_value(&parser->lexer, &token)) {
        keypart_free(&kp);
        return -1;
    }

    toml_node_t val = NODE_ZERO;
    if (parse_value(parser, token, &val)) {
        node_free(&val);
        keypart_free(&kp);
        return -1;
    }

    if (table_insert(tab, kp.parts[kp.count - 1], val, &reason)) {
        node_free(&val);
        keypart_free(&kp);
        return errbuf_set(parser->errbuff, kline, "%s", reason);
    }
    keypart_free(&kp);
    return 0;
}

/* ---------------------------------------------------------------------------
 * Parser – string normalisation (escape processing)
 * -------------------------------------------------------------------------*/

static int parse_norm(toml_parser_t *parser, toml_token_t token,
                      char_span_s *out) {
    char *buf = pool_alloc(parser->pool, token.raw.len + 1);
    if (!buf) return errbuf_set(parser->errbuff, token.line, "out of memory");

    memcpy(buf, token.raw.ptr, token.raw.len);
    buf[token.raw.len] = '\0';
    out->ptr           = buf;
    out->len           = token.raw.len;

    switch (token.type) {
        case eTokLit:
        case eTokLitString:
        case eTokMlLitString:
            return 0;
        case eTokString:
        case eTokMlString:
            break;
        default:
            return errbuf_set(
                parser->errbuff, 0,
                "internal: parse_norm called on non-string token");
    }

    if (!token.as.esc_start) return 0;

    char *p   = buf + (token.as.esc_start - token.raw.ptr);
    char *dst = p;
    assert(*p == '\\');

    while (*p) {
        if (*p != '\\') {
            *dst++ = *p++;
            continue;
        }
        switch (p[1]) {
            case '"':
            case '\\':
                *dst++ = p[1];
                p += 2;
                continue;
            case 'b':
                *dst++ = '\b';
                p += 2;
                continue;
            case 't':
                *dst++ = '\t';
                p += 2;
                continue;
            case 'n':
                *dst++ = '\n';
                p += 2;
                continue;
            case 'f':
                *dst++ = '\f';
                p += 2;
                continue;
            case 'r':
                *dst++ = '\r';
                p += 2;
                continue;
            case 'e':
                if (parser->version == eTomlVersion10)
                    return errbuf_set(parser->errbuff, token.line,
                                      "unknown escape '\\e'");
                *dst++ = '\033';
                p += 2;
                continue;
            case 'x': {
                if (parser->version == eTomlVersion10)
                    return errbuf_set(parser->errbuff, token.line,
                                      "unknown escape '\\x'");
                char tmp[3];
                memcpy(tmp, p + 2, 2);
                tmp[2]      = '\0';
                int32_t ucs = (int32_t)strtol(tmp, NULL, 16);
                int n       = ucs_to_utf8((uint32_t)ucs, dst);
                if (n < 0)
                    return errbuf_set(parser->errbuff, token.line,
                                      "invalid \\x escape");
                dst += n;
                p += 4;
                continue;
            }
            case 'u':
            case 'U': {
                int sz = (p[1] == 'u') ? 4 : 8;
                char tmp[9];
                memcpy(tmp, p + 2, sz);
                tmp[sz]      = '\0';
                uint32_t ucs = (uint32_t)strtoul(tmp, NULL, 16);
                if (ucs >= 0xD800 && ucs <= 0xDFFF)
                    return errbuf_set(
                        parser->errbuff, token.line,
                        "surrogate codepoint \\u%04X is not allowed", ucs);
                if (ucs > 0x10FFFF)
                    return errbuf_set(
                        parser->errbuff, token.line,
                        "codepoint \\U%08X exceeds Unicode maximum (0x10FFFF)",
                        ucs);
                int n = ucs_to_utf8(ucs, dst);
                if (n < 0)
                    return errbuf_set(parser->errbuff, token.line,
                                      "invalid unicode codepoint");
                dst += n;
                p += 2 + sz;
                continue;
            }
            case ' ':
            case '\t':
            case '\r':
                p++;
                p += strspn(p, " \t\r");
                if (*p != '\n')
                    return errbuf_set(
                        parser->errbuff, token.line,
                        "unexpected char after line-ending backslash");
            case '\n':
                p++;
                p += strspn(p, " \t\r\n");
                continue;
            default:
                return errbuf_set(parser->errbuff, token.line,
                                  "unknown escape '\\%c'", p[1]);
        }
    }
    *dst     = '\0';
    out->len = (int)(dst - out->ptr);
    return 0;
}

/* ===========================================================================
 * LEXER IMPLEMENTATION
 * =========================================================================*/

static int lex_advance(toml_lexer_t *lexer) {
    int ch        = (int)eTokEof;
    const char *p = lexer->cur;
    if (p < lexer->end) {
        ch = (unsigned char)*p++;
        if (ch == '\r' && p < lexer->end && *p == '\n')
            ch = (unsigned char)*p++;
    }
    lexer->cur = p;
    if (ch == '\n') {
        lexer->line++;
        lexer->line_start = p;
    }
    return ch;
}

static bool lx_peek(toml_lexer_t *lexer, int ch) {
    const char *p = lexer->cur;
    if (p < lexer->end && (unsigned char)*p == ch) return true;
    if (ch == '\n' && p + 1 < lexer->end && p[0] == '\r' && p[1] == '\n')
        return true;
    return false;
}

static bool lx_peek_any(toml_lexer_t *lexer, const char *chars) {
    for (const char *c = chars; *c; c++)
        if (lx_peek(lexer, (unsigned char)*c)) return true;
    return false;
}

static bool lx_peek_n(toml_lexer_t *lexer, int ch, int n) {
    if (ch == '\n') {
        assert(0 && "lx_peek_n does not support '\\n'; use lx_peek");
        return false;
    }
    if (lexer->cur + n > lexer->end) return false;
    for (int i = 0; i < n; i++)
        if ((unsigned char)lexer->cur[i] != ch) return false;
    return true;
}

static toml_token_t make_token(toml_lexer_t *lexer, e_tok_type type) {
    toml_token_t token = { 0 };
    token.type         = type;
    token.raw.ptr      = lexer->cur;
    token.line         = lexer->line;
    token.col          = (int)(lexer->cur - lexer->line_start) + 1;
    return token;
}

static bool is_printable_char(int ch) {
    return isprint((unsigned char)ch) || (ch & 0x80);
}

static bool is_hex(int ch) {
    ch = toupper((unsigned char)ch);
    return ('0' <= ch && ch <= '9') || ('A' <= ch && ch <= 'F');
}

static void lexer_init(toml_lexer_t *lexer, const char *src, int len,
                       char *errbuf, int errbufsz, e_toml_version version) {
    memset(lexer, 0, sizeof(*lexer));
    lexer->src         = src;
    lexer->end         = src + len;
    lexer->cur         = src;
    lexer->line        = 1;
    lexer->line_start  = src;
    lexer->err_buf.ptr = errbuf;
    lexer->err_buf.len = errbufsz;
    lexer->version     = version;
}

static int lex_escape(toml_lexer_t *lexer) {
    const char *p = lexer->cur;
    if (p >= lexer->end) return 0;
    int ch = (unsigned char)*p++;
    if (ch && strchr("btnfr\"\\", ch)) return (int)(p - lexer->cur);
    if (ch == 'e' && lexer->version == eTomlVersion11)
        return (int)(p - lexer->cur);
    if (ch == 'e')
        return errbuf_set(lexer->err_buf, lexer->line, "unknown escape '\\e'");
    if (ch == 'x' && lexer->version == eTomlVersion10)
        return errbuf_set(lexer->err_buf, lexer->line, "unknown escape '\\x'");
    int hex_len = (ch == 'x') ? 2 : (ch == 'u') ? 4 : (ch == 'U') ? 8 : 0;
    if (hex_len) {
        int i = 0;
        for (; i < hex_len && p < lexer->end && is_hex((unsigned char)*p);
             i++, p++) {
        }
        if (i != hex_len)
            return errbuf_set(lexer->err_buf, lexer->line,
                              "expected %d hex digits after \\%c", hex_len, ch);
        return (int)(p - lexer->cur);
    }
    return 0;
}

static int lex_multiline_string(toml_lexer_t *lexer, toml_token_t *token) {
    assert(lx_peek_n(lexer, '"', 3));
    lex_advance(lexer);
    lex_advance(lexer);
    lex_advance(lexer);
    if (lx_peek(lexer, '\n')) lex_advance(lexer);

    *token                = make_token(lexer, eTokMlString);
    const char *esc_start = NULL;

    for (;;) {
        int ch = lex_advance(lexer);
        if (ch == (int)eTokEof)
            return errbuf_set(lexer->err_buf, lexer->line,
                              "unterminated \"\"\"");

        if (ch == '"') {
            int nquotes = 1;
            while (nquotes < 5 && lx_peek(lexer, '"')) {
                lex_advance(lexer);
                nquotes++;
            }
            if (nquotes == 5 && lx_peek(lexer, '"'))
                return errbuf_set(lexer->err_buf, lexer->line,
                                  "too many consecutive double-quotes");
            if (nquotes >= 3) {
                token->raw.len      = (int)((lexer->cur - 3) - token->raw.ptr);
                token->as.esc_start = esc_start;
                return 0;
            }
            continue;
        }

        if (ch == '\\') {
            if (!esc_start) {
                esc_start = lexer->cur - 1;
                assert(*esc_start == '\\');
            }
            int n = lex_escape(lexer);
            if (n < 0) return -1;
            if (n > 0) {
                lexer->cur += n;
                continue;
            }
            /* Line-ending backslash: optional trailing spaces then newline,
             * followed by skipping all leading whitespace on the next line. */
            ch = lex_advance(lexer);
            if (ch == ' ' || ch == '\t') {
                while (ch != (int)eTokEof && (ch == ' ' || ch == '\t'))
                    ch = lex_advance(lexer);
                if (ch != '\n')
                    return errbuf_set(lexer->err_buf, lexer->line,
                                      "bad escape in string");
            }
            if (ch == '\n') {
                while (lx_peek_any(lexer, " \t\n"))
                    lex_advance(lexer);
                continue;
            }
            return errbuf_set(lexer->err_buf, lexer->line,
                              "bad escape in string");
        }

        if (!(is_printable_char(ch) || (ch && strchr(" \t\n", ch))))
            return errbuf_set(lexer->err_buf, lexer->line,
                              "invalid character in string");
    }
}
static int lex_string(toml_lexer_t *lexer, toml_token_t *token) {
    assert(lx_peek(lexer, '"'));
    if (lx_peek_n(lexer, '"', 3)) return lex_multiline_string(lexer, token);
    lex_advance(lexer);

    *token                = make_token(lexer, eTokString);
    const char *esc_start = NULL;

    while (!lx_peek(lexer, '"')) {
        int ch = lex_advance(lexer);
        if (ch == (int)eTokEof)
            return errbuf_set(lexer->err_buf, lexer->line,
                              "unterminated string");
        if (ch != '\\') {
            if (!(is_printable_char(ch) || ch == ' ' || ch == '\t'))
                return errbuf_set(lexer->err_buf, lexer->line,
                                  "invalid character in string");
            continue;
        }
        if (!esc_start) {
            esc_start = lexer->cur - 1;
            assert(*esc_start == '\\');
        }
        int n = lex_escape(lexer);
        if (n < 0) return -1;
        if (n > 0) {
            lexer->cur += n;
            continue;
        }
        return errbuf_set(lexer->err_buf, lexer->line, "bad escape in string");
    }
    token->raw.len      = (int)(lexer->cur - token->raw.ptr);
    token->as.esc_start = esc_start;
    assert(lx_peek(lexer, '"'));
    lex_advance(lexer);
    return 0;
}

static int lex_multiline_litstring(toml_lexer_t *lexer, toml_token_t *token) {
    assert(lx_peek_n(lexer, '\'', 3));
    lex_advance(lexer);
    lex_advance(lexer);
    lex_advance(lexer);
    if (lx_peek(lexer, '\n')) lex_advance(lexer);

    *token = make_token(lexer, eTokMlLitString);

    for (;;) {
        int ch = lex_advance(lexer);
        if (ch == (int)eTokEof)
            return errbuf_set(lexer->err_buf, lexer->line, "unterminated '''");

        if (ch == '\'') {
            int nquotes = 1;
            while (nquotes < 5 && lx_peek(lexer, '\'')) {
                lex_advance(lexer);
                nquotes++;
            }
            if (nquotes == 5 && lx_peek(lexer, '\''))
                return errbuf_set(lexer->err_buf, lexer->line,
                                  "too many consecutive single-quotes");
            if (nquotes >= 3) {
                token->raw.len = (int)((lexer->cur - 3) - token->raw.ptr);
                return 0;
            }
            continue;
        }

        if (!(is_printable_char(ch) || (ch && strchr(" \t\n", ch))))
            return errbuf_set(lexer->err_buf, lexer->line,
                              "invalid character in string");
    }
}

static int lex_litstring(toml_lexer_t *lexer, toml_token_t *token) {
    assert(lx_peek(lexer, '\''));
    if (lx_peek_n(lexer, '\'', 3)) return lex_multiline_litstring(lexer, token);
    lex_advance(lexer);

    *token = make_token(lexer, eTokLitString);
    while (!lx_peek(lexer, '\'')) {
        int ch = lex_advance(lexer);
        if (ch == (int)eTokEof)
            return errbuf_set(lexer->err_buf, lexer->line,
                              "unterminated string");
        if (!(is_printable_char(ch) || ch == '\t'))
            return errbuf_set(lexer->err_buf, lexer->line,
                              "invalid character in string");
    }
    token->raw.len = (int)(lexer->cur - token->raw.ptr);
    assert(lx_peek(lexer, '\''));
    lex_advance(lexer);
    return 0;
}

/* ---------------------------------------------------------------------------
 * Date/time helpers
 * -------------------------------------------------------------------------*/

static bool is_valid_date(int year, int month, int day) {
    if (year < 0) return false;
    if (month < 1 || month > 12) return false;
    bool leap  = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
    int days[] = { 31, 28 + (int)leap, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    return day >= 1 && day <= days[month - 1];
}

static bool is_valid_time(int hour, int min, int sec, int usec) {
    return hour >= 0 && hour <= 23 && min >= 0 && min <= 59 && sec >= 0
           && sec <= 60 && usec >= 0;
}

static bool is_valid_tz(int minutes) {
    minutes = minutes < 0 ? -minutes : minutes;
    return (minutes / 60) <= 23 && (minutes % 60) < 60;
}

static int read_int_n(const char *p, int *out) {
    const char *s = p;
    int64_t v     = 0;
    while (isdigit((unsigned char)*p)) {
        v = v * 10 + (*p++ - '0');
        if (v > INT_MAX) return 0;
    }
    *out = (int)v;
    return (int)(p - s);
}

static int read_date(const char *p, int16_t *year, int16_t *month,
                     int16_t *day) {
    const char *s = p;
    int n, y, mo, d;
    n = read_int_n(p, &y);
    if (n != 4 || p[4] != '-') return 0;
    p += n + 1;
    n = read_int_n(p, &mo);
    if (n != 2 || p[2] != '-') return 0;
    p += n + 1;
    n = read_int_n(p, &d);
    if (n != 2) return 0;
    p += n;
    *year  = (int16_t)y;
    *month = (int16_t)mo;
    *day   = (int16_t)d;
    assert(p - s == 10);
    return (int)(p - s);
}

static int read_time(e_toml_version version, const char *p, int16_t *hour,
                     int16_t *min, int16_t *sec, int32_t *usec) {
    const char *s = p;
    int n, h, mi, se;

    n = read_int_n(p, &h);
    if (n != 2 || p[2] != ':') return 0;
    p += 3;

    n = read_int_n(p, &mi);
    if (n != 2) return 0;
    p += 2;

    if (p[0] != ':') {
        if (version == eTomlVersion10) return 0;
        *hour = (int16_t)h;
        *min  = (int16_t)mi;
        *sec  = 0;
        *usec = 0;
        return (int)(p - s);
    }
    p++;

    n = read_int_n(p, &se);
    if (n != 2) return 0;
    p += 2;

    *usec = 0;
    if (p[0] == '.') {
        p++;
        if (!isdigit((unsigned char)*p)) return 0;
        int factor = 100000;
        while (isdigit((unsigned char)*p) && factor) {
            *usec += (*p++ - '0') * factor;
            factor /= 10;
        }
        while (isdigit((unsigned char)*p))
            p++;
    }

    *hour = (int16_t)h;
    *min  = (int16_t)mi;
    *sec  = (int16_t)se;
    return (int)(p - s);
}

static int read_tz(const char *p, char *sign, int *tzhour, int *tzmin) {
    const char *s = p;
    *tzhour = *tzmin = 0;
    *sign            = '+';
    if (*p == 'Z' || *p == 'z') return 1;
    *sign = *p++;
    if (*sign != '+' && *sign != '-') return 0;
    int n;
    n = read_int_n(p, tzhour);
    if (n != 2 || p[2] != ':') return 0;
    p += 3;
    n = read_int_n(p, tzmin);
    if (n != 2) return 0;
    p += 2;
    return (int)(p - s);
}

static void lx_copystr(toml_lexer_t *lexer, char *dst, int dstsz) {
    assert(dstsz > 0);
    int n = (int)(lexer->end - lexer->cur);
    if (n > dstsz - 1) n = dstsz - 1;
    if (n > 0) memcpy(dst, lexer->cur, n);
    dst[n] = '\0';
}

typedef struct {
    e_tok_type type;
    int consumed;
    toml_timestamp_t timestamp;
} ts_parse_t;

static int parse_timestamp(e_toml_version version, const char *buf,
                           ts_parse_t *out) {
    *out = (ts_parse_t){
        .type      = eTokEof,
        .timestamp = TOML_TIMESTAMP_UNSET,
    };

    const char *p = buf;
    int n;

    if (isdigit((unsigned char)p[0]) && isdigit((unsigned char)p[1])
        && p[2] == ':') {
        n = read_time(version, buf, &out->timestamp.hour,
                      &out->timestamp.minute, &out->timestamp.second,
                      &out->timestamp.usec);
        if (!n) return -1;
        out->type     = eTokTime;
        out->consumed = n;
        return 0;
    }

    n = read_date(p, &out->timestamp.year, &out->timestamp.month,
                  &out->timestamp.day);
    if (!n) return -1;
    out->type = eTokDate;
    p += n;

    bool has_time = (p[0] == 'T' || p[0] == ' ' || p[0] == 't')
                    && isdigit((unsigned char)p[1])
                    && isdigit((unsigned char)p[2]) && p[3] == ':';
    if (!has_time) {
        out->consumed = (int)(p - buf);
        return 0;
    }

    n = read_time(version, p + 1, &out->timestamp.hour, &out->timestamp.minute,
                  &out->timestamp.second, &out->timestamp.usec);
    if (!n) return -1;
    out->type = eTokDatetime;
    p += 1 + n;

    char sign;
    int tzhour, tzmin;
    n = read_tz(p, &sign, &tzhour, &tzmin);
    if (n) {
        if (tzmin < 0 || tzmin >= 60) return -1;
        out->timestamp.tz = (tzhour * 60 + tzmin) * (sign == '-' ? -1 : 1);
        out->type         = eTokDatetimeTz;
        p += n;
    }

    out->consumed = (int)(p - buf);
    return 0;
}

static int lex_timestamp(toml_lexer_t *lexer, toml_token_t *token) {
    char buf[80];
    lx_copystr(lexer, buf, sizeof(buf));

    ts_parse_t ts;
    if (parse_timestamp(lexer->version, buf, &ts) != 0) {
        const char *msg = (ts.type == eTokEof) ? "invalid date or time"
                          : (ts.type == eTokDate)
                              ? "invalid timestamp time part"
                              : "invalid timezone";
        return errbuf_set(lexer->err_buf, lexer->line, msg);
    }

    *token         = make_token(lexer, ts.type);
    token->raw.len = ts.consumed;
    lexer->cur += ts.consumed;
    token->as.timestamp = ts.timestamp;

    switch (ts.type) {
        case eTokTime:
            if (!is_valid_time(ts.timestamp.hour, ts.timestamp.minute,
                               ts.timestamp.second, ts.timestamp.usec))
                return errbuf_set(lexer->err_buf, lexer->line, "invalid time");
            break;
        case eTokDate:
            if (!is_valid_date(ts.timestamp.year, ts.timestamp.month,
                               ts.timestamp.day))
                return errbuf_set(lexer->err_buf, lexer->line, "invalid date");
            break;
        case eTokDatetime:
        case eTokDatetimeTz:
            if (!is_valid_date(ts.timestamp.year, ts.timestamp.month,
                               ts.timestamp.day))
                return errbuf_set(lexer->err_buf, lexer->line,
                                  "invalid date in datetime");
            if (!is_valid_time(ts.timestamp.hour, ts.timestamp.minute,
                               ts.timestamp.second, ts.timestamp.usec))
                return errbuf_set(lexer->err_buf, lexer->line,
                                  "invalid time in datetime");
            if (ts.type == eTokDatetimeTz && !is_valid_tz(ts.timestamp.tz))
                return errbuf_set(lexer->err_buf, lexer->line,
                                  "invalid timezone");
            break;
        default:
            assert(0);
            return errbuf_set(lexer->err_buf, lexer->line, "internal error");
    }

    return 0;
}
static int lex_time(toml_lexer_t *lexer, toml_token_t *token) {
    if (lex_timestamp(lexer, token)) return -1;
    if (token->type != eTokTime)
        return errbuf_set(lexer->err_buf, lexer->line, "invalid time");
    return 0;
}

/* ---------------------------------------------------------------------------
 * Number lexing
 * -------------------------------------------------------------------------*/

static int normalize_numstr(char *buf, int base, const char **reason) {
    int len = (int)strlen(buf);
    int w   = 0;
    for (int i = 0; i < len; i++) {
        if (buf[i] != '_') {
            buf[w++] = buf[i];
            continue;
        }
        int left      = (i == 0) ? 0 : (unsigned char)buf[i - 1];
        int right     = (i + 1 < len) ? (unsigned char)buf[i + 1] : 0;
        bool left_ok  = isdigit(left) || (base == 16 && is_hex(left));
        bool right_ok = isdigit(right) || (base == 16 && is_hex(right));
        if (!left_ok || !right_ok) {
            *reason = "underscore only allowed between digits";
            return -1;
        }
    }
    buf[w] = '\0';

    for (int i = 0; buf[i]; i++) {
        if (buf[i] == '.') {
            if (i == 0 || !isdigit((unsigned char)buf[i - 1])
                || !isdigit((unsigned char)buf[i + 1])) {
                *reason = "decimal point must be surrounded by digits";
                return -1;
            }
        } else if ('A' <= buf[i] && buf[i] <= 'Z') {
            buf[i] = (char)tolower((unsigned char)buf[i]);
        }
    }

    if (base == 10) {
        char *p = buf + (*buf == '+' || *buf == '-' ? 1 : 0);
        if (p[0] == '0' && isdigit((unsigned char)p[1])) {
            *reason = "leading zero in integer";
            return -1;
        }
    }
    return 0;
}

static int digit_value(int ch) {
    if ('0' <= ch && ch <= '9') return ch - '0';
    ch = tolower((unsigned char)ch);
    if ('a' <= ch && ch <= 'f') return ch - 'a' + 10;
    return -1;
}

static int parse_i64_checked(const char *buf, int base, int64_t *out,
                             const char **reason) {
    bool neg      = false;
    const char *p = buf;
    if (*p == '+' || *p == '-') {
        neg = *p == '-';
        p++;
    }
    if (!*p) {
        *reason = "invalid integer";
        return -1;
    }

    uint64_t limit = neg ? (uint64_t)INT64_MAX + 1u : (uint64_t)INT64_MAX;
    uint64_t value = 0;
    for (; *p; p++) {
        int digit = digit_value((unsigned char)*p);
        if (digit < 0 || digit >= base) {
            *reason = "invalid integer";
            return -1;
        }
        if (value > (limit - (uint64_t)digit) / (uint64_t)base) {
            *reason = "integer out of range";
            return -1;
        }
        value = value * (uint64_t)base + (uint64_t)digit;
    }

    if (neg && value == (uint64_t)INT64_MAX + 1u) {
        *out = INT64_MIN;
    } else if (neg) {
        *out = -(int64_t)value;
    } else {
        *out = (int64_t)value;
    }
    return 0;
}

static int lex_float(toml_lexer_t *lexer, toml_token_t *token) {
    char buf[512];
    lx_copystr(lexer, buf, sizeof(buf));
    char *p = buf + (*buf == '+' || *buf == '-' ? 1 : 0);
    if (0 == memcmp(p, "nan", 3) || 0 == memcmp(p, "inf", 3))
        p += 3;
    else
        p += strspn(p, "_0123456789eE.+-");
    int len  = (int)(p - buf);
    buf[len] = '\0';

    const char *reason;
    if (normalize_numstr(buf, 10, &reason))
        return errbuf_set(lexer->err_buf, lexer->line, "%s", reason);

    char *end;
    double f64 = strtod(buf, &end);
    if (*end || end == buf)
        return errbuf_set(lexer->err_buf, lexer->line, "invalid float");

    bool special = strcmp(buf, "inf") == 0 || strcmp(buf, "+inf") == 0
                   || strcmp(buf, "-inf") == 0;
    if (isinf(f64) && !special)
        return errbuf_set(lexer->err_buf, lexer->line, "float overflow");

    if (isnan(f64)) f64 = (double)NAN;

    *token         = make_token(lexer, eTokFloat);
    token->as.f64  = f64;
    token->raw.len = len;
    lexer->cur += len;
    return 0;
}

static int lex_number(toml_lexer_t *lexer, toml_token_t *token) {
    char buf[512];
    lx_copystr(lexer, buf, sizeof(buf));
    char *p = buf;
    const char *reason;

    if (p[0] == '0') {
        const char *span = NULL;
        int base         = 0;
        switch (p[1]) {
            case 'x':
                base = 16;
                span = "_0123456789abcdefABCDEF";
                break;
            case 'o':
                base = 8;
                span = "_01234567";
                break;
            case 'b':
                base = 2;
                span = "_01";
                break;
        }
        if (base) {
            p += 2 + strspn(p + 2, span);
            int len  = (int)(p - buf);
            buf[len] = '\0';
            if (normalize_numstr(buf + 2, base, &reason))
                return errbuf_set(lexer->err_buf, lexer->line, "%s", reason);
            int64_t i64;
            if (parse_i64_checked(buf + 2, base, &i64, &reason))
                return errbuf_set(lexer->err_buf, lexer->line, "%s", reason);
            *token         = make_token(lexer, eTokInteger);
            token->as.i64  = i64;
            token->raw.len = len;
            lexer->cur += len;
            return 0;
        }
    }

    if (*p == '+' || *p == '-') p++;
    if (*p == 'i' || *p == 'n') return lex_float(lexer, token);

    p        = buf + strspn(buf, "0123456789_+-.eE");
    int len  = (int)(p - buf);
    buf[len] = '\0';

    if (normalize_numstr(buf, 10, &reason))
        return errbuf_set(lexer->err_buf, lexer->line, "%s", reason);

    if (strchr(buf, '.') || strchr(buf, 'e') || strchr(buf, 'E'))
        return lex_float(lexer, token);

    int64_t i64;
    if (parse_i64_checked(buf, 10, &i64, &reason))
        return errbuf_set(lexer->err_buf, lexer->line, "%s", reason);
    *token         = make_token(lexer, eTokInteger);
    token->as.i64  = i64;
    token->raw.len = len;
    lexer->cur += len;
    return 0;
}

static int lex_bool(toml_lexer_t *lexer, toml_token_t *token) {
    char buf[10];
    lx_copystr(lexer, buf, sizeof(buf));
    bool val;
    const char *p = buf;
    if (strncmp(p, "true", 4) == 0) {
        val = true;
        p += 4;
    } else if (strncmp(p, "false", 5) == 0) {
        val = false;
        p += 5;
    } else
        return errbuf_set(lexer->err_buf, lexer->line, "invalid boolean");
    if (*p && !strchr("# \r\n\t,}]", *p))
        return errbuf_set(lexer->err_buf, lexer->line, "invalid boolean");
    int len           = (int)(p - buf);
    *token            = make_token(lexer, eTokBool);
    token->as.boolean = val;
    token->raw.len    = len;
    lexer->cur += len;
    return 0;
}

static bool test_is_time(const char *p, const char *end) {
    return &p[2] < end && isdigit((unsigned char)p[0])
           && isdigit((unsigned char)p[1]) && p[2] == ':';
}

static bool test_is_date(const char *p, const char *end) {
    return &p[4] < end && isdigit((unsigned char)p[0])
           && isdigit((unsigned char)p[1]) && isdigit((unsigned char)p[2])
           && isdigit((unsigned char)p[3]) && p[4] == '-';
}

static bool test_is_bool(const char *p, const char *end) {
    return p < end && (*p == 't' || *p == 'f');
}

static bool test_is_number(const char *p, const char *end) {
    if (p < end && *p && strchr("0123456789+-._", *p)) return true;
    if (&p[2] < end && (memcmp(p, "nan", 3) == 0 || memcmp(p, "inf", 3) == 0))
        return true;
    return false;
}

static int lex_nonstring_literal(toml_lexer_t *lexer, toml_token_t *token) {
    if (test_is_time(lexer->cur, lexer->end)) return lex_time(lexer, token);
    if (test_is_date(lexer->cur, lexer->end))
        return lex_timestamp(lexer, token);
    if (test_is_bool(lexer->cur, lexer->end)) return lex_bool(lexer, token);
    if (test_is_number(lexer->cur, lexer->end)) return lex_number(lexer, token);
    return errbuf_set(lexer->err_buf, lexer->line, "invalid value");
}

static int lex_bare_key(toml_lexer_t *lexer, toml_token_t *token) {
    *token        = make_token(lexer, eTokLit);
    const char *p = lexer->cur;
    while (p < lexer->end) {
        if (isalnum((unsigned char)*p) || *p == '_' || *p == '-') {
            p++;
            continue;
        }
        if ((unsigned char)*p >= 0x80) {
            uint32_t code_point;
            int n = utf8_to_ucs(p, (int)(lexer->end - p), &code_point);
            (void)code_point;
            if (n <= 0)
                return errbuf_set(lexer->err_buf, lexer->line,
                                  "invalid UTF-8 in bare key");
            break;
        }
        break;
    }
    token->raw.len = (int)(p - token->raw.ptr);
    lexer->cur     = p;
    if (token->raw.len == 0) {
        return errbuf_set(lexer->err_buf, lexer->line,
                          "bare key cannot be empty");
    }
    return 0;
}

static toml_lex_mark_t lex_mark(toml_lexer_t *lexer) {
    return (toml_lex_mark_t){ lexer, lexer->cur, lexer->line, lexer->line_start,
                              lexer->nesting_depth };
}

static void lex_restore(toml_lexer_t *lexer, toml_lex_mark_t mark) {
    assert(mark.lexer == lexer);
    lexer->cur           = mark.cur;
    lexer->line          = mark.line;
    lexer->line_start    = mark.line_start;
    lexer->nesting_depth = mark.nesting_depth;
}

static int lex_next(toml_lexer_t *lexer, e_lex_mode mode, toml_token_t *token) {
    static const e_tok_type char_map[128] = {
        ['\n'] = eTokNewline, ['.'] = eTokDot,    ['='] = eTokEqual,
        [','] = eTokComma,    ['{'] = eTokLBrace, ['}'] = eTokRBrace,
    };

    for (;;) {
        *token = make_token(lexer, eTokEof);
        int ch = lex_advance(lexer);

        if (ch == (int)eTokEof) return 0;

        if (ch == ' ' || ch == '\t') continue;

        if (ch == '#') {
            while (!lx_peek(lexer, '\n')) {
                ch = lex_advance(lexer);
                if (ch == (int)eTokEof) break;
                if ((ch >= 0 && ch <= 0x08) || (ch >= 0x0a && ch <= 0x1f)
                    || ch == 0x7f)
                    return errbuf_set(lexer->err_buf, lexer->line,
                                      "invalid control character in comment");
            }
            continue;
        }

        token->raw.len = 1;

        if (ch >= 0 && ch < 128 && char_map[ch]) {
            token->type = char_map[ch];
            return 0;
        }

        switch (ch) {
            case '[':
                token->type = eTokLBrack;
                if (mode == eLexHeader && lx_peek(lexer, '[')) {
                    lex_advance(lexer);
                    token->type    = eTokLLBrack;
                    token->raw.len = 2;
                }
                break;
            case ']':
                token->type = eTokRBrack;
                if (mode == eLexHeader && lx_peek(lexer, ']')) {
                    lex_advance(lexer);
                    token->type    = eTokRRBrack;
                    token->raw.len = 2;
                }
                break;
            case '"':
                lexer->cur--;
                return lex_string(lexer, token) ? -1 : 0;
            case '\'':
                lexer->cur--;
                return lex_litstring(lexer, token) ? -1 : 0;
            default:
                lexer->cur--;
                return (mode == eLexValue ? lex_nonstring_literal(lexer, token)
                                          : lex_bare_key(lexer, token))
                           ? -1
                           : 0;
        }

        return 0;
    }
}
static int lex_check_depth(toml_lexer_t *lexer, toml_token_t *token) {
    char open;
    switch (token->type) {
        case eTokLBrack:
        case eTokLBrace:
            if (lexer->nesting_depth >= NESTING_MAX)
                return errbuf_set(lexer->err_buf, lexer->line,
                                  "nesting too deep");
            lexer->nesting[lexer->nesting_depth++] =
                token->type == eTokLBrack ? '[' : '{';
            break;
        case eTokRBrack:
        case eTokRBrace:
            if (lexer->nesting_depth <= 0)
                return errbuf_set(lexer->err_buf, lexer->line,
                                  "unexpected closing delimiter");
            open = lexer->nesting[--lexer->nesting_depth];
            if ((token->type == eTokRBrack && open != '[')
                || (token->type == eTokRBrace && open != '{'))
                return errbuf_set(lexer->err_buf, lexer->line,
                                  token->type == eTokRBrack ? "expected '}'"
                                                            : "expected ']'");
            break;
        default:
            break;
    }
    return 0;
}

static int lex_header(toml_lexer_t *lexer, toml_token_t *token) {
    if (lexer->errmsg) return -1;
    if (lex_next(lexer, eLexHeader, token) || lex_check_depth(lexer, token)) {
        lexer->errmsg = lexer->err_buf.ptr;
        return -1;
    }
    return 0;
}

static int lex_key(toml_lexer_t *lexer, toml_token_t *token) {
    if (lexer->errmsg) return -1;
    if (lex_next(lexer, eLexKey, token) || lex_check_depth(lexer, token)) {
        lexer->errmsg = lexer->err_buf.ptr;
        return -1;
    }
    return 0;
}

static int lex_value(toml_lexer_t *lexer, toml_token_t *token) {
    if (lexer->errmsg) return -1;
    if (lex_next(lexer, eLexValue, token) || lex_check_depth(lexer, token)) {
        lexer->errmsg = lexer->err_buf.ptr;
        return -1;
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * UTF-8 / UCS conversion
 * -------------------------------------------------------------------------*/

static int utf8_to_ucs(const char *original, int len, uint32_t *out) {
    const unsigned char *b = (const unsigned char *)original;
    unsigned i             = *b++;
    uint32_t v;

    if (!(i >> 7)) {
        if (len < 1) return -1;
        return *out = i, 1;
    }
    if (i >> 5 == 0x06) {
        if (len < 2) return -1;
        v = i & 0x1f;
        i = *b++;
        if (i >> 6 != 0x02) return -1;
        v           = (v << 6) | (i & 0x3f);
        return *out = v, (const char *)b - original;
    }
    if (i >> 4 == 0x0E) {
        if (len < 3) return -1;
        v = i & 0x0F;
        for (int j = 0; j < 2; j++) {
            i = *b++;
            if (i >> 6 != 0x02) return -1;
            v = (v << 6) | (i & 0x3f);
        }
        return *out = v, (const char *)b - original;
    }
    if (i >> 3 == 0x1E) {
        if (len < 4) return -1;
        v = i & 0x07;
        for (int j = 0; j < 3; j++) {
            i = *b++;
            if (i >> 6 != 0x02) return -1;
            v = (v << 6) | (i & 0x3f);
        }
        return *out = v, (const char *)b - original;
    }
    return -1;
}

static int ucs_to_utf8(uint32_t code, char buf[4]) {
    if ((code >= 0xD800 && code <= 0xDFFF) || code > 0x10FFFF) return -1;
    if (code <= 0x7F) {
        buf[0] = (char)code;
        return 1;
    }
    if (code <= 0x7FF) {
        buf[0] = (char)(0xC0 | (code >> 6));
        buf[1] = (char)(0x80 | (code & 0x3F));
        return 2;
    }
    if (code <= 0xFFFF) {
        buf[0] = (char)(0xE0 | (code >> 12));
        buf[1] = (char)(0x80 | ((code >> 6) & 0x3F));
        buf[2] = (char)(0x80 | (code & 0x3F));
        return 3;
    }
    buf[0] = (char)(0xF0 | (code >> 18));
    buf[1] = (char)(0x80 | ((code >> 12) & 0x3F));
    buf[2] = (char)(0x80 | ((code >> 6) & 0x3F));
    buf[3] = (char)(0x80 | (code & 0x3F));
    return 4;
}

/* ---------------------------------------------------------------------------
 * Public API implementations
 * -------------------------------------------------------------------------*/

static toml_result_t toml_parse_span_version(char_span_s input,
                                             e_toml_version version) {
    toml_result_t result = { 0 };
    toml_parser_t parser = { 0 };

    if (!input.ptr) {
        snprintf(result.errmsg, sizeof(result.errmsg), "src is NULL");
        return result;
    }
    if (input.len >= 3 && (unsigned char)input.ptr[0] == 0xEF
        && (unsigned char)input.ptr[1] == 0xBB
        && (unsigned char)input.ptr[2] == 0xBF) {
        input.ptr += 3;
        input.len -= 3;
    }

    parser.root        = make_node_at(eTomlTable, 1, 1);
    parser.cur.table   = &parser.root;
    parser.cur.kind    = eTableCtxRoot;
    parser.cur.line    = 1;
    parser.errbuff.ptr = result.errmsg;
    parser.errbuff.len = (int)sizeof(result.errmsg);
    parser.version     = version;
    parser.pool        = pool_create(input.len + 10);
    if (!parser.pool) {
        snprintf(result.errmsg, sizeof(result.errmsg), "out of memory");
        return result;
    }
    Arena *old_arena = toml_set_active_arena(&parser.pool->arena);

    lexer_init(&parser.lexer, input.ptr, input.len, parser.errbuff.ptr,
               parser.errbuff.len, version);

    for (;;) {
        toml_token_t token;
        if (lex_header(&parser.lexer, &token)) goto cleanup;
        if (token.type == eTokEof) break;
        switch (token.type) {
            case eTokNewline:
                continue;
            case eTokLBrack:
                if (parse_std_table(&parser, token)) goto cleanup;
                break;
            case eTokLLBrack:
                if (parse_array_table(&parser, token)) goto cleanup;
                break;
            default:
                if (parse_kv_expr(&parser, token)) goto cleanup;
                break;
        }
        if (lex_header(&parser.lexer, &token)) goto cleanup;
        if (token.type == eTokEof || token.type == eTokNewline) continue;
        errbuf_set(parser.errbuff, token.line, "expected newline or EOF");
        goto cleanup;
    }

    result.ok        = true;
    result.root      = parser.root;
    result._internal = parser.pool;
    toml_set_active_arena(old_arena);
    return result;

cleanup:
    node_free(&parser.root);
    pool_destroy(parser.pool);
    toml_set_active_arena(old_arena);
    result.ok = false;
    if (!result.errmsg[0])
        snprintf(result.errmsg, sizeof(result.errmsg),
                 "parse error near line %d", parser.lexer.line);
    return result;
}

toml_result_t toml_parse_span(char_span_s input) {
    return toml_parse_span_version(input, eTomlVersion11);
}

static toml_result_t toml_parse_file_version(FILE *fp, e_toml_version version) {
    toml_result_t result = { 0 };
    if (!fp) {
        snprintf(result.errmsg, sizeof(result.errmsg), "fp is NULL");
        return result;
    }

    enum {
        CHUNK = 8 * 1024
    };
    char *buf        = NULL;
    int top          = 0;
    bool err         = false;
    Arena file_arena = { 0 };
    Arena *old_arena = toml_set_active_arena(&file_arena);

    {
        long cur = ftell(fp);
        if (cur >= 0) {
            if (fseek(fp, 0, SEEK_END) == 0) {
                long end = ftell(fp);
                fseek(fp, cur, SEEK_SET);
                if (end > cur && (end - cur) < INT_MAX - 1) {
                    int presize = (int)(end - cur);
                    char *tmp   = cell_resize(NULL, presize + 1);
                    if (tmp) buf = tmp;
                }
            }
        }
    }

    for (;;) {
        if (top > INT_MAX - CHUNK - 1) {
            snprintf(result.errmsg, sizeof(result.errmsg), "file too large");
            err = true;
            break;
        }
        char *tmp = cell_resize(buf, top + CHUNK + 1);
        if (!tmp) {
            snprintf(result.errmsg, sizeof(result.errmsg), "out of memory");
            err = true;
            break;
        }
        buf      = tmp;
        size_t n = fread(buf + top, 1, CHUNK, fp);
        top += (int)n;
        if (n < CHUNK) {
            if (ferror(fp)) {
                snprintf(result.errmsg, sizeof(result.errmsg),
                         "error reading file");
                err = true;
            }
            break;
        }
    }

    if (err) {
        cell_free(buf);
        toml_set_active_arena(old_arena);
        arena_free(&file_arena);
        return result;
    }
    buf[top] = '\0';
    result   = toml_parse_span_version((char_span_s){ .ptr = buf, .len = top },
                                       version);
    cell_free(buf);
    toml_set_active_arena(old_arena);
    arena_free(&file_arena);
    return result;
}

toml_result_t toml_parse_file(FILE *fp) {
    return toml_parse_file_version(fp, eTomlVersion11);
}

static toml_result_t toml_parse_file_path_version(const char *path,
                                                  e_toml_version version) {
    toml_result_t result = { 0 };
    FILE *fp             = fopen(path, "r");
    if (!fp) {
        snprintf(result.errmsg, sizeof(result.errmsg), "fopen failed: %s",
                 path);
        return result;
    }
    result = toml_parse_file_version(fp, version);
    fclose(fp);
    return result;
}

toml_result_t toml_parse_file_path(const char *path) {
    return toml_parse_file_path_version(path, eTomlVersion11);
}

toml_result_t toml_parse(toml_input_t input) {
    switch (input.kind) {
        case eTOML_INPUT_SRC:
            return toml_parse_span_version(input.input, input.version);
        case eTOML_INPUT_FILE:
            return toml_parse_file_version(input.fp, input.version);
        case eTOML_INPUT_PATH:
            return toml_parse_file_path_version(input.path, input.version);
        default: {
            toml_result_t r = { 0 };
            snprintf(r.errmsg, sizeof(r.errmsg), "unknown input kind");
            return r;
        }
    }
}

void toml_free(toml_result_t result) {
    node_free(&result.root);
    pool_destroy((toml_pool_t *)result._internal);
}

toml_node_t toml_get(toml_node_t tab, const char *key) {
    if (tab.type == eTomlTable) {
        int klen = (int)strlen(key);
        TOML_TABLE_FOREACH (&tab, e) {
            if (e->key_len == klen && 0 == memcmp(e->key, key, klen)) {
                return e->value;
            }
        }
    }
    return NODE_ZERO;
}

toml_node_t toml_seek(toml_node_t tab, const char *dotted_key) {
    if (tab.type != eTomlTable) return NODE_ZERO;

    int klen      = (int)strlen(dotted_key) + 1;
    Arena scratch = { 0 };
    char *buf     = (char *)arena_alloc(&scratch, (size_t)klen);
    if (!buf) return NODE_ZERO;
    memcpy(buf, dotted_key, klen);

    toml_node_t cur = tab;
    char *p         = buf;

    for (;;) {
        if (cur.type != eTomlTable) break;

        char *seg;
        int seg_len;

        if (*p == '"' || *p == '\'') {
            char q      = *p;
            char *start = p + 1;
            char *close = strchr(start, q);
            if (!close) {
                cur = NODE_ZERO;
                break;
            }
            seg     = start;
            seg_len = (int)(close - start);

            char *after = close + 1;
            if (*after == '.') {
                *close = '\0';
                cur    = toml_get(cur, seg);
                p      = after + 1;
                continue;
            } else if (*after == '\0') {
                *close = '\0';
                cur    = toml_get(cur, seg);
                break;
            } else {
                cur = NODE_ZERO;
                break;
            }
        } else {
            char *dot = strchr(p, '.');
            if (!dot) {
                cur = toml_get(cur, p);
                break;
            }
            *dot    = '\0';
            seg     = p;
            seg_len = (int)(dot - p);
            (void)seg_len;
            cur = toml_get(cur, seg);
            p   = dot + 1;
            continue;
        }
    }

    arena_free(&scratch);
    return cur;
}

toml_result_t toml_merge(const toml_result_t *self,
                         const toml_result_t *other) {
    const char *reason = "";
    toml_result_t ret  = { 0 };
    toml_pool_t *pool  = NULL;
    Arena *old_arena   = NULL;

    if (!self->ok) {
        reason = "param error: first result not ok";
        goto cleanup;
    }
    if (!other->ok) {
        reason = "param error: second result not ok";
        goto cleanup;
    }

    {
        toml_pool_t *pa = (toml_pool_t *)self->_internal;
        toml_pool_t *pb = (toml_pool_t *)other->_internal;
        size_t asz      = pool_total_used(pa);
        size_t bsz      = pool_total_used(pb);
        pool            = pool_create(asz + bsz);
        if (!pool) {
            reason = "out of memory";
            goto cleanup;
        }
    }

    old_arena = toml_set_active_arena(&pool->arena);
    if (node_copy(&ret.root, self->root, pool, &reason)) goto cleanup;
    if (node_merge(&ret.root, other->root, pool, &reason)) goto cleanup;
    toml_set_active_arena(old_arena);
    old_arena = NULL;

    ret.ok        = true;
    ret._internal = pool;
    return ret;

cleanup:
    if (old_arena) toml_set_active_arena(old_arena);
    pool_destroy(pool);
    snprintf(ret.errmsg, sizeof(ret.errmsg), "%s", reason);
    return ret;
}

bool toml_equiv(const toml_result_t *self, const toml_result_t *other) {
    if (!(self->ok && other->ok)) return false;
    return node_equiv(self->root, other->root);
}

bool toml_is_string(toml_node_t n) {
    return n.type == eTomlString;
}

bool toml_is_int(toml_node_t n) {
    return n.type == eTomlInt;
}
bool toml_is_float(toml_node_t n) {
    return n.type == eTomlFloat;
}
bool toml_is_bool(toml_node_t n) {
    return n.type == eTomlBool;
}
bool toml_is_date(toml_node_t n) {
    return n.type == eTomlDate;
}
bool toml_is_time(toml_node_t n) {
    return n.type == eTomlTime;
}
bool toml_is_datatime(toml_node_t n) {
    return n.type == eTomlDatetime;
}
bool toml_is_datatime_tz(toml_node_t n) {
    return n.type == eTomlDatetimeTz;
}
bool toml_is_array(toml_node_t n) {
    return n.type == eTomlArray;
}
bool toml_is_table(toml_node_t n) {
    return n.type == eTomlTable;
}
bool toml_found(toml_node_t n) {
    return n.type != eTomlUnknown;
}

const char *toml_string(toml_node_t n) {
    return n.as.string.ptr;
}
int toml_string_len(toml_node_t n) {
    return n.as.string.len;
}
int64_t toml_int(toml_node_t n) {
    return n.as.i64;
}
double toml_float(toml_node_t n) {
    return n.as.f64;
}
bool toml_bool(toml_node_t n) {
    return n.as.boolean;
}
toml_timestamp_t toml_timestamp(toml_node_t n) {
    return n.as.timestamp;
}
toml_result_t toml_clone(const toml_result_t *src) {
    toml_result_t ret = { 0 };
    if (!src->ok) {
        snprintf(ret.errmsg, sizeof(ret.errmsg),
                 "param error: source result not ok");
        return ret;
    }
    toml_pool_t *pool =
        pool_create(pool_total_used((toml_pool_t *)src->_internal));
    if (!pool) {
        snprintf(ret.errmsg, sizeof(ret.errmsg), "out of memory");
        return ret;
    }
    const char *reason;
    Arena *old_arena = toml_set_active_arena(&pool->arena);
    if (node_copy(&ret.root, src->root, pool, &reason)) {
        toml_set_active_arena(old_arena);
        pool_destroy(pool);
        snprintf(ret.errmsg, sizeof(ret.errmsg), "%s", reason);
        return ret;
    }
    toml_set_active_arena(old_arena);
    ret.ok        = true;
    ret._internal = pool;
    return ret;
}
