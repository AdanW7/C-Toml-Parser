#include "toml.h"
#include "test_toml.h"
#include <stdio.h>
#include <string.h>

#if defined(_WIN32) && !defined(__GLIBC__)
static void *tmx_memmem(const void *haystack, size_t haystack_len,
                        const void *needle, size_t needle_len) {
    if (needle_len == 0) return (void *)haystack;
    if (needle_len > haystack_len) return NULL;
    const unsigned char *h = (const unsigned char *)haystack;
    const unsigned char *n = (const unsigned char *)needle;
    for (size_t i = 0; i <= haystack_len - needle_len; i++) {
        if (h[i] == n[0] && memcmp(h + i, n, needle_len) == 0) {
            return (void *)(h + i);
        }
    }
    return NULL;
}
#    define memmem tmx_memmem
#endif

// ---------------------------------------------------------------------------
// Usage
// ---------------------------------------------------------------------------

static void print_usage(const char *prog) {
    printf("Usage: %s (--test | <file.toml>)\n"
           "\n"
           "  --test         Parse the built-in test document and run checks.\n"
           "  <file.toml>    Parse a TOML file and dump its tree.\n"
           "  -h, --help     Show this help message.\n",
           prog);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char *argv[]) {
    if (argc == 2 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
        print_usage(argv[0]);
        return 0;
    }

    if (argc == 2 && !strcmp(argv[1], "--test")) {
        return run_toml_tests();
    }

    if (argc != 2) {
        fprintf(stderr, "error: expected exactly one argument\n\n");
        print_usage(argv[0]);
        return 1;
    }

    const char *path = argv[1];
    printf("=== toml_parse: loading \"%s\" ===\n\n", path);

    toml_result_t result = toml_parse(TOML_FROM_PATH(path));
    if (!result.ok) {
        fprintf(stderr, "Parse failed: %s\n", result.errmsg);
        return 1;
    }

    printf("--- Full tree dump ---\n");
    printf("root = ");
    toml_print_node(result.root, 0);
    putchar('\n');

    toml_free(result);
    return 0;
}
