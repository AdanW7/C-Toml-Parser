#include "toml.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

static void toml_print_indent(int depth) {
    for (int i = 0; i < depth * 2; i++)
        putchar(' ');
}

void toml_print_node(toml_node_t n, int depth) {
    switch (n.type) {
        case eTomlString:
            printf("(string) \"%.*s\"", n.as.string.len, n.as.string.ptr);
            break;
        case eTomlInt:
            printf("(int) %" PRId64, n.as.i64);
            break;
        case eTomlFloat:
            printf("(float) %g", n.as.f64);
            break;
        case eTomlBool:
            printf("(bool) %s", n.as.boolean ? "true" : "false");
            break;
        case eTomlDate:
            printf("(date) " TOML_DATE_FMT, TOML_DATE_ARGS(n.as.timestamp));
            break;
        case eTomlTime:
            printf("(time) " TOML_TIME_FMT, TOML_TIME_ARGS(n.as.timestamp));
            break;
        case eTomlDatetime:
            printf("(datetime) " TOML_DATETIME_FMT,
                   TOML_DATETIME_ARGS(n.as.timestamp));
            break;
        case eTomlDatetimeTz: {
            int tz  = n.as.timestamp.tz;
            char sg = tz < 0 ? '-' : '+';
            int ah  = (tz < 0 ? -tz : tz) / 60;
            int am  = (tz < 0 ? -tz : tz) % 60;
            printf("(datetime+tz) " TOML_DATETIME_TZ_FMT,
                   TOML_DATETIME_ARGS(n.as.timestamp), sg, ah, am);
            break;
        }
        case eTomlArray:
            printf("(array)[%d]\n", n.as.array.len);
            for (int i = 0; i < n.as.array.len; i++) {
                toml_print_indent(depth + 1);
                printf("[%d] = ", i);
                toml_print_node(n.as.array.items[i], depth + 1);
                putchar('\n');
            }
            return;
        case eTomlTable:
            printf("(table)[%d]\n", n.as.table.len);
            for (int i = 0; i < n.as.table.len; i++) {
                toml_print_indent(depth + 1);
                toml_entry_t *entry = &n.as.table.entries[i];
                printf("%.*s = ", entry->key_len, entry->key);
                toml_print_node(entry->value, depth + 1);
                putchar('\n');
            }
            return;
        default:
            printf("(unknown)");
            break;
    }
}

const char *toml_test_string =
    "title = \"TOML Parse Test\"\n"
    "version = 42\n"
    "pi = 3.14159\n"
    "enabled = true\n"
    "created = 2024-01-15\n"
    "start_time = 08:30:00\n"
    "last_modified = 2024-01-15T08:30:00\n"
    "published_at = 2024-01-15T08:30:00Z\n"
    "empty_array = []\n"
    "empty_inline = {}\n"
    "[server]\n"
    "host = \"localhost\"\n"
    "port = 8080\n"
    "debug = false\n"
    "[server.tls]\n"
    "enabled = true\n"
    "cert = \"/etc/certs/server.pem\"\n"
    "[database]\n"
    "host = \"db.internal\"\n"
    "port = 5432\n"
    "name = \"myapp\"\n"
    "max_connections = 100\n"
    "timeout = 30.5\n"
    "[database.credentials]\n"
    "user = \"admin\"\n"
    "password = \"s3cr3t\"\n"
    "[[users]]\n"
    "name = \"Alice\"\n"
    "role = \"admin\"\n"
    "score = 9.5\n"
    "[[users]]\n"
    "name = \"Bob\"\n"
    "role = \"viewer\"\n"
    "score = 7.2\n"
    "[[users]]\n"
    "name = \"Carol\"\n"
    "role = \"editor\"\n"
    "score = 8.1\n"
    "[features]\n"
    "# single-line array (no trailing comma)\n"
    "tags = [\"web\", \"api\", \"v2\"]\n"
    "# single-line array (trailing comma)\n"
    "limits = [\n"
    "  100,\n"
    "  200,\n"
    "  500,\n"
    "]\n"
    "# multiline array (no trailing comma)\n"
    "colors = [\"red\", \"green\", \"blue\"]\n"
    "# multiline array (trailing comma)\n"
    "numbers = [\n"
    "  1,\n"
    "  2,\n"
    "  3,\n"
    "]\n"
    "# nested multiline array\n"
    "matrix = [\n"
    "  [1, 2],\n"
    "  [3, 4],\n"
    "]\n"
    "# comments inside multiline array\n"
    "commented_array = [\n"
    "  10,  # first\n"
    "  20,  # second\n"
    "  30,  # third\n"
    "]\n"
    "[metadata]\n"
    "# single-line inline table (no trailing comma)\n"
    "inline_coords = { x = 10, y = 20 }\n"
    "# single-line inline table (trailing comma)\n"
    "inline_coords_tc = {\n"
    "  x = 10,\n"
    "  y = 20,\n"
    "}\n"
    "# multiline inline table (no trailing comma)\n"
    "multiline_inline = { x = 1, y = 2 }\n"
    "# multiline inline table (trailing comma)\n"
    "multiline_inline_tc = {\n"
    "  x = 1,\n"
    "  y = 2,\n"
    "}\n"
    "# multiline inline table with comments\n"
    "multiline_inline_comments = {\n"
    "  x = 10,  # x coordinate\n"
    "  y = 20,  # y coordinate\n"
    "  z = 30,  # z coordinate\n"
    "}\n"
    "# nested multiline inline table\n"
    "nested_inline = {\n"
    "  pos = {\n"
    "    x = 100,\n"
    "    y = 200,\n"
    "  },\n"
    "  size = {\n"
    "    w = 800,\n"
    "    h = 600,\n"
    "  },\n"
    "}\n"
    "description = \"A multi-line\\nstring with escapes\\tand tabs\"\n"
    "unicode_key = \"caf\\u00E9\"\n"
    "raw = 'C:\\Users\\no\\escapes\\here'\n"
    "# inline table containing arrays using both styles\n"
    "mixed = {\n"
    "  tags = [\n"
    "    \"alpha\",\n"
    "    \"beta\",\n"
    "    \"gamma\",\n"
    "  ],\n"
    "  limits = [\n"
    "    1,\n"
    "    2,\n"
    "    3,\n"
    "  ],\n"
    "}\n"
    "# deeply nested TOML 1.1 formatting stress test\n"
    "deep = {\n"
    "  a = {\n"
    "    b = {\n"
    "      c = {\n"
    "        nums = [\n"
    "          1,\n"
    "          2,\n"
    "          3,\n"
    "        ],\n"
    "        vals = {\n"
    "          x = 10,\n"
    "          y = 20,\n"
    "        },\n"
    "      },\n"
    "    },\n"
    "  },\n"
    "}\n"
    "[ints]\n"
    "zero = 0\n"
    "positive = +99\n"
    "negative = -99\n"
    "underscores = 1_000_000\n"
    "large = 9_223_372_036_854_775_807\n"
    "[floats]\n"
    "normal = 3.14\n"
    "underscores = 1_234.567_890\n"
    "exp1 = 1e6\n"
    "exp2 = 5E+22\n"
    "exp3 = -2e-2\n"
    "inf = inf\n"
    "neg_inf = -inf\n"
    "nan = nan\n"
    "neg_nan = -nan  # i think this might not be implemented yet\n"
    "hex = 0xDEADBEEF\n"
    "oct = 0o755\n"
    "bin = 0b11010110\n"
    "[strings]\n"
    "basic = \"hello\\nworld\"\n"
    "literal = 'hello\\nworld'\n"
    "continued = \"\"\"\n"
    "The quick brown \\\n"
    "  fox jumps over \\\n"
    "the lazy dog.\n"
    "\"\"\"\n"
    "multi_basic = \"\"\"\n"
    "hello\n"
    "world\"\"\"\n"
    "multi_literal = '''\n"
    "hello\n"
    "world\n"
    "'''\n"
    "u1 = \"\\u03B1\"\n"
    "u2 = \"\\U0001F600\"\n"
    "[dotted]\n"
    "physical.color = \"orange\"\n"
    "physical.shape = \"round\"\n"
    "site.\"google.com\" = true\n"
    "[quoted]\n"
    "\"127.0.0.1\" = \"localhost\"\n"
    "\"key with spaces\" = 123\n"
    "\"😀\" = \"emoji\"\n"
    "[mixed_array]\n"
    "mixed = [1, \"hello\", true, 3.14]\n"
    "nested = [\n"
    "  [[1], [2]],\n"
    "  [\n"
    "    [3],\n"
    "    [4],\n"
    "  ],\n"
    "]\n"
    "nested_table = {\n"
    "  a = {\n"
    "    b = { c = 123 },\n"
    "  },\n"
    "}\n"
    "[[products]]\n"
    "name = \"Hammer\"\n"
    "[products.details]\n"
    "weight = 10\n"
    "[[products]]\n"
    "name = \"Nail\"\n"
    "[products.details]\n"
    "weight = 1\n"
    "[empty_table]\n";

static int check_failed = 0;

#define CHECK(label, cond)                                                     \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "  FAIL: %s\n", label);                            \
            check_failed++;                                                    \
        } else {                                                               \
            printf("  PASS: %s\n", label);                                     \
        }                                                                      \
    } while (0)

static void run_checks(toml_result_t *r) {
    printf("\n--- Targeted value checks ---\n");

    toml_node_t root = r->root;

    // -------------------------------------------------------------------------
    // Top level scalars
    // -------------------------------------------------------------------------

    toml_node_t title = toml_get(root, "title");
    CHECK("title is string", title.type == eTomlString);
    CHECK("title value",
          strncmp(title.as.string.ptr, "TOML Parse Test", 15) == 0);

    toml_node_t version = toml_get(root, "version");
    CHECK("version is int", version.type == eTomlInt);
    CHECK("version value", version.as.i64 == 42);

    toml_node_t pi = toml_get(root, "pi");
    CHECK("pi is float", pi.type == eTomlFloat);
    CHECK("pi roughly 3.14", pi.as.f64 > 3.14 && pi.as.f64 < 3.15);

    toml_node_t enabled = toml_get(root, "enabled");
    CHECK("enabled is bool", enabled.type == eTomlBool);
    CHECK("enabled is true", enabled.as.boolean == true);

    // -------------------------------------------------------------------------
    // Top level datetime types
    // -------------------------------------------------------------------------

    toml_node_t created = toml_get(root, "created");
    CHECK("created is date", created.type == eTomlDate);
    CHECK("created year", created.as.timestamp.year == 2024);
    CHECK("created month", created.as.timestamp.month == 1);
    CHECK("created day", created.as.timestamp.day == 15);

    toml_node_t start_time = toml_get(root, "start_time");
    CHECK("start_time is time", start_time.type == eTomlTime);
    CHECK("start_time hour", start_time.as.timestamp.hour == 8);
    CHECK("start_time minute", start_time.as.timestamp.minute == 30);
    CHECK("start_time second", start_time.as.timestamp.second == 0);

    toml_node_t last_mod = toml_get(root, "last_modified");
    CHECK("last_modified is datetime", last_mod.type == eTomlDatetime);
    CHECK("last_modified year", last_mod.as.timestamp.year == 2024);
    CHECK("last_modified month", last_mod.as.timestamp.month == 1);
    CHECK("last_modified day", last_mod.as.timestamp.day == 15);
    CHECK("last_modified hour", last_mod.as.timestamp.hour == 8);
    CHECK("last_modified minute", last_mod.as.timestamp.minute == 30);

    toml_node_t published = toml_get(root, "published_at");
    CHECK("published_at is datetime+tz", published.type == eTomlDatetimeTz);
    CHECK("published_at tz is UTC", published.as.timestamp.tz == 0);

    // -------------------------------------------------------------------------
    // Top level empty containers
    // -------------------------------------------------------------------------

    toml_node_t empty_arr = toml_get(root, "empty_array");
    CHECK("empty_array is array", empty_arr.type == eTomlArray);
    CHECK("empty_array len == 0", empty_arr.as.array.len == 0);

    toml_node_t empty_tbl = toml_get(root, "empty_inline");
    CHECK("empty_inline is table", empty_tbl.type == eTomlTable);
    CHECK("empty_inline len == 0", empty_tbl.as.table.len == 0);

    // -------------------------------------------------------------------------
    // [server] — nested tables and boolean false
    // -------------------------------------------------------------------------

    toml_node_t host = toml_seek(root, "server.host");
    CHECK("server.host is string", host.type == eTomlString);
    CHECK("server.host value",
          strncmp(host.as.string.ptr, "localhost", 9) == 0);

    toml_node_t port = toml_seek(root, "server.port");
    CHECK("server.port is int", port.type == eTomlInt);
    CHECK("server.port value", port.as.i64 == 8080);

    toml_node_t debug = toml_seek(root, "server.debug");
    CHECK("server.debug is bool", debug.type == eTomlBool);
    CHECK("server.debug is false", debug.as.boolean == false);

    toml_node_t tls = toml_seek(root, "server.tls.enabled");
    CHECK("server.tls.enabled is bool", tls.type == eTomlBool);
    CHECK("server.tls.enabled true", tls.as.boolean == true);

    toml_node_t cert = toml_seek(root, "server.tls.cert");
    CHECK("server.tls.cert is string", cert.type == eTomlString);
    CHECK("server.tls.cert value",
          strncmp(cert.as.string.ptr, "/etc/certs/server.pem", 21) == 0);

    // -------------------------------------------------------------------------
    // [database]
    // -------------------------------------------------------------------------

    toml_node_t db_name = toml_seek(root, "database.name");
    CHECK("database.name is string", db_name.type == eTomlString);
    CHECK("database.name value",
          strncmp(db_name.as.string.ptr, "myapp", 5) == 0);

    toml_node_t db_port = toml_seek(root, "database.port");
    CHECK("database.port is int", db_port.type == eTomlInt);
    CHECK("database.port value", db_port.as.i64 == 5432);

    toml_node_t max_conn = toml_seek(root, "database.max_connections");
    CHECK("database.max_connections is int", max_conn.type == eTomlInt);
    CHECK("database.max_connections value", max_conn.as.i64 == 100);

    toml_node_t timeout = toml_seek(root, "database.timeout");
    CHECK("database.timeout is float", timeout.type == eTomlFloat);
    CHECK("database.timeout value",
          timeout.as.f64 > 30.4 && timeout.as.f64 < 30.6);

    toml_node_t db_user = toml_seek(root, "database.credentials.user");
    CHECK("database.credentials.user is string", db_user.type == eTomlString);
    CHECK("database.credentials.user value",
          strncmp(db_user.as.string.ptr, "admin", 5) == 0);

    toml_node_t db_pass = toml_seek(root, "database.credentials.password");
    CHECK("database.credentials.password is string",
          db_pass.type == eTomlString);
    CHECK("database.credentials.password value",
          strncmp(db_pass.as.string.ptr, "s3cr3t", 6) == 0);

    // -------------------------------------------------------------------------
    // [[users]] — array of tables
    // -------------------------------------------------------------------------

    toml_node_t users = toml_get(root, "users");
    CHECK("users is array", users.type == eTomlArray);
    CHECK("users has 3 entries", users.as.array.len == 3);

    if (users.type == eTomlArray && users.as.array.len >= 3) {
        toml_node_t u0       = users.as.array.items[0];
        toml_node_t u0_name  = toml_get(u0, "name");
        toml_node_t u0_role  = toml_get(u0, "role");
        toml_node_t u0_score = toml_get(u0, "score");
        CHECK("users[0].name is string", u0_name.type == eTomlString);
        CHECK("users[0].name is Alice",
              strncmp(u0_name.as.string.ptr, "Alice", 5) == 0);
        CHECK("users[0].role is admin",
              strncmp(u0_role.as.string.ptr, "admin", 5) == 0);
        CHECK("users[0].score is float", u0_score.type == eTomlFloat);
        CHECK("users[0].score == 9.5",
              u0_score.as.f64 > 9.4 && u0_score.as.f64 < 9.6);

        toml_node_t u1      = users.as.array.items[1];
        toml_node_t u1_name = toml_get(u1, "name");
        CHECK("users[1].name is Bob",
              strncmp(u1_name.as.string.ptr, "Bob", 3) == 0);

        toml_node_t u2      = users.as.array.items[2];
        toml_node_t u2_role = toml_get(u2, "role");
        CHECK("users[2].role is editor",
              strncmp(u2_role.as.string.ptr, "editor", 6) == 0);
    }

    // -------------------------------------------------------------------------
    // [features] arrays
    // -------------------------------------------------------------------------

    toml_node_t tags = toml_seek(root, "features.tags");
    CHECK("features.tags is array", tags.type == eTomlArray);
    CHECK("features.tags has 3", tags.as.array.len == 3);
    CHECK("features.tags[0] is web",
          strncmp(tags.as.array.items[0].as.string.ptr, "web", 3) == 0);
    CHECK("features.tags[1] is api",
          strncmp(tags.as.array.items[1].as.string.ptr, "api", 3) == 0);
    CHECK("features.tags[2] is v2",
          strncmp(tags.as.array.items[2].as.string.ptr, "v2", 2) == 0);

    toml_node_t limits = toml_seek(root, "features.limits");
    CHECK("limits is array", limits.type == eTomlArray);
    CHECK("limits len == 3", limits.as.array.len == 3);
    CHECK("limits[0] == 100", limits.as.array.items[0].as.i64 == 100);
    CHECK("limits[1] == 200", limits.as.array.items[1].as.i64 == 200);
    CHECK("limits[2] == 500", limits.as.array.items[2].type == eTomlInt
                                  && limits.as.array.items[2].as.i64 == 500);

    toml_node_t colors = toml_seek(root, "features.colors");
    CHECK("colors is array", colors.type == eTomlArray);
    CHECK("colors len == 3", colors.as.array.len == 3);
    CHECK("colors[0] == red",
          strncmp(colors.as.array.items[0].as.string.ptr, "red", 3) == 0);
    CHECK("colors[1] == green",
          strncmp(colors.as.array.items[1].as.string.ptr, "green", 5) == 0);
    CHECK("colors[2] == blue",
          strncmp(colors.as.array.items[2].as.string.ptr, "blue", 4) == 0);

    toml_node_t numbers = toml_seek(root, "features.numbers");
    CHECK("numbers is array", numbers.type == eTomlArray);
    CHECK("numbers len == 3", numbers.as.array.len == 3);
    CHECK("numbers[0] == 1", numbers.as.array.items[0].as.i64 == 1);
    CHECK("numbers[2] == 3", numbers.as.array.items[2].as.i64 == 3);

    toml_node_t matrix = toml_seek(root, "features.matrix");
    CHECK("features.matrix is array", matrix.type == eTomlArray);
    CHECK("features.matrix len == 2", matrix.as.array.len == 2);
    CHECK("features.matrix[0] is arr",
          matrix.as.array.items[0].type == eTomlArray);
    CHECK("features.matrix[0][0] == 1",
          matrix.as.array.items[0].as.array.items[0].as.i64 == 1);
    CHECK("features.matrix[0][1] == 2",
          matrix.as.array.items[0].as.array.items[1].as.i64 == 2);
    CHECK("features.matrix[1][0] == 3",
          matrix.as.array.items[1].as.array.items[0].as.i64 == 3);
    CHECK("features.matrix[1][1] == 4",
          matrix.as.array.items[1].as.array.items[1].as.i64 == 4);

    toml_node_t commented = toml_seek(root, "features.commented_array");
    CHECK("commented_array is array", commented.type == eTomlArray);
    CHECK("commented_array len == 3", commented.as.array.len == 3);
    CHECK("commented_array[0] == 10", commented.as.array.items[0].as.i64 == 10);
    CHECK("commented_array[1] == 20", commented.as.array.items[1].as.i64 == 20);
    CHECK("commented_array[2] == 30", commented.as.array.items[2].as.i64 == 30);

    // -------------------------------------------------------------------------
    // [metadata] — inline tables
    // -------------------------------------------------------------------------

    toml_node_t coords = toml_seek(root, "metadata.inline_coords");
    CHECK("inline_coords is table", coords.type == eTomlTable);
    CHECK("inline_coords.x == 10", toml_get(coords, "x").as.i64 == 10);
    CHECK("inline_coords.y == 20", toml_get(coords, "y").as.i64 == 20);

    toml_node_t coords_tc = toml_seek(root, "metadata.inline_coords_tc");
    CHECK("inline_coords_tc is table", coords_tc.type == eTomlTable);
    CHECK("inline_coords_tc.x == 10", toml_get(coords_tc, "x").as.i64 == 10);
    CHECK("inline_coords_tc.y == 20", toml_get(coords_tc, "y").as.i64 == 20);

    toml_node_t multi = toml_seek(root, "metadata.multiline_inline");
    CHECK("multiline_inline is table", multi.type == eTomlTable);
    CHECK("multiline_inline.x == 1", toml_get(multi, "x").as.i64 == 1);
    CHECK("multiline_inline.y == 2", toml_get(multi, "y").as.i64 == 2);

    toml_node_t multi_tc = toml_seek(root, "metadata.multiline_inline_tc");
    CHECK("multiline_inline_tc is table", multi_tc.type == eTomlTable);
    CHECK("multiline_inline_tc.x == 1", toml_get(multi_tc, "x").as.i64 == 1);
    CHECK("multiline_inline_tc.y == 2", toml_get(multi_tc, "y").as.i64 == 2);

    toml_node_t com = toml_seek(root, "metadata.multiline_inline_comments");
    CHECK("multiline_inline_comments is table", com.type == eTomlTable);
    CHECK("multiline_inline_comments.x == 10", toml_get(com, "x").as.i64 == 10);
    CHECK("multiline_inline_comments.y == 20", toml_get(com, "y").as.i64 == 20);
    CHECK("multiline_inline_comments.z == 30", toml_get(com, "z").as.i64 == 30);

    toml_node_t nested = toml_seek(root, "metadata.nested_inline");
    CHECK("nested_inline is table", nested.type == eTomlTable);
    toml_node_t pos  = toml_get(nested, "pos");
    toml_node_t size = toml_get(nested, "size");
    CHECK("nested.pos is table", pos.type == eTomlTable);
    CHECK("nested.size is table", size.type == eTomlTable);
    CHECK("nested.pos.x == 100", toml_get(pos, "x").as.i64 == 100);
    CHECK("nested.pos.y == 200", toml_get(pos, "y").as.i64 == 200);
    CHECK("nested.size.w == 800", toml_get(size, "w").as.i64 == 800);
    CHECK("nested.size.h == 600", toml_get(size, "h").as.i64 == 600);

    // description: basic escape sequences (\n, \t)
    toml_node_t desc = toml_seek(root, "metadata.description");
    CHECK("description is string", desc.type == eTomlString);
    CHECK("description has newline",
          desc.type == eTomlString
              && memchr(desc.as.string.ptr, '\n', desc.as.string.len) != NULL);
    CHECK("description has tab",
          desc.type == eTomlString
              && memchr(desc.as.string.ptr, '\t', desc.as.string.len) != NULL);

    // unicode_key: \u00E9 → UTF-8 0xC3 0xA9 ("é")
    toml_node_t ukey = toml_seek(root, "metadata.unicode_key");
    CHECK("unicode_key is string", ukey.type == eTomlString);
    CHECK("unicode_key ends with UTF-8 é",
          ukey.type == eTomlString && ukey.as.string.len >= 2
              && (unsigned char)ukey.as.string.ptr[ukey.as.string.len - 2]
                     == 0xC3
              && (unsigned char)ukey.as.string.ptr[ukey.as.string.len - 1]
                     == 0xA9);

    // raw: literal string — backslashes must be preserved verbatim
    toml_node_t raw = toml_seek(root, "metadata.raw");
    CHECK("raw literal string no escapes",
          raw.type == eTomlString
              && strncmp(raw.as.string.ptr, "C:\\Users\\no\\escapes\\here",
                         raw.as.string.len)
                     == 0);

    // inline table containing arrays
    toml_node_t mixed        = toml_seek(root, "metadata.mixed");
    toml_node_t mixed_tags   = toml_get(mixed, "tags");
    toml_node_t mixed_limits = toml_get(mixed, "limits");
    CHECK("mixed is table", mixed.type == eTomlTable);
    CHECK("mixed.tags is array", mixed_tags.type == eTomlArray);
    CHECK("mixed.tags len == 3", mixed_tags.as.array.len == 3);
    CHECK("mixed.tags[0] == alpha",
          strncmp(mixed_tags.as.array.items[0].as.string.ptr, "alpha", 5) == 0);
    CHECK("mixed.tags[2] == gamma",
          strncmp(mixed_tags.as.array.items[2].as.string.ptr, "gamma", 5) == 0);
    CHECK("mixed.limits is array", mixed_limits.type == eTomlArray);
    CHECK("mixed.limits[0] == 1", mixed_limits.as.array.items[0].as.i64 == 1);
    CHECK("mixed.limits[2] == 3", mixed_limits.as.array.items[2].as.i64 == 3);

    // deep nesting stress test
    toml_node_t nums = toml_seek(root, "metadata.deep.a.b.c.nums");
    CHECK("deep nums is array", nums.type == eTomlArray);
    CHECK("deep nums len == 3", nums.as.array.len == 3);
    CHECK("deep nums[0] == 1", nums.as.array.items[0].as.i64 == 1);
    CHECK("deep nums[2] == 3", nums.as.array.items[2].as.i64 == 3);

    toml_node_t vals = toml_seek(root, "metadata.deep.a.b.c.vals");
    CHECK("deep vals is table", vals.type == eTomlTable);
    CHECK("deep vals.x == 10", toml_get(vals, "x").as.i64 == 10);
    CHECK("deep vals.y == 20", toml_get(vals, "y").as.i64 == 20);

    // -------------------------------------------------------------------------
    // [ints]
    // -------------------------------------------------------------------------

    toml_node_t iz = toml_seek(root, "ints.zero");
    CHECK("ints.zero is int", iz.type == eTomlInt);
    CHECK("ints.zero == 0", iz.as.i64 == 0);

    toml_node_t ip = toml_seek(root, "ints.positive");
    CHECK("ints.positive is int", ip.type == eTomlInt);
    CHECK("ints.positive == 99", ip.as.i64 == 99); // leading '+' stripped

    toml_node_t in = toml_seek(root, "ints.negative");
    CHECK("ints.negative is int", in.type == eTomlInt);
    CHECK("ints.negative == -99", in.as.i64 == -99);

    toml_node_t iu = toml_seek(root, "ints.underscores");
    CHECK("ints.underscores is int", iu.type == eTomlInt);
    CHECK("ints.underscores == 1000000", iu.as.i64 == 1000000);

    toml_node_t ilarge = toml_seek(root, "ints.large");
    CHECK("ints.large is int", ilarge.type == eTomlInt);
    CHECK("ints.large == INT64_MAX", ilarge.as.i64 == INT64_MAX);

    // -------------------------------------------------------------------------
    // [floats]
    // -------------------------------------------------------------------------

    toml_node_t fn = toml_seek(root, "floats.normal");
    CHECK("floats.normal is float", fn.type == eTomlFloat);
    CHECK("floats.normal == 3.14", fn.as.f64 > 3.13 && fn.as.f64 < 3.15);

    toml_node_t fu = toml_seek(root, "floats.underscores");
    CHECK("floats.underscores is float", fu.type == eTomlFloat);
    CHECK("floats.underscores == 1234.57",
          fu.as.f64 > 1234.56 && fu.as.f64 < 1234.58);

    toml_node_t fe1 = toml_seek(root, "floats.exp1");
    CHECK("floats.exp1 is float", fe1.type == eTomlFloat);
    CHECK("floats.exp1 == 1e6", fe1.as.f64 == 1e6);

    toml_node_t fe2 = toml_seek(root, "floats.exp2");
    CHECK("floats.exp2 is float", fe2.type == eTomlFloat);
    CHECK("floats.exp2 == 5e22", fe2.as.f64 > 4.9e22 && fe2.as.f64 < 5.1e22);

    toml_node_t fe3 = toml_seek(root, "floats.exp3");
    CHECK("floats.exp3 is float", fe3.type == eTomlFloat);
    CHECK("floats.exp3 == -0.02", fe3.as.f64 > -0.021 && fe3.as.f64 < -0.019);

    toml_node_t finf = toml_seek(root, "floats.inf");
    CHECK("floats.inf is float", finf.type == eTomlFloat);
    CHECK("floats.inf is +inf",
          finf.as.f64 > 0 && finf.as.f64 == finf.as.f64 + 1);

    toml_node_t fninf = toml_seek(root, "floats.neg_inf");
    CHECK("floats.neg_inf is float", fninf.type == eTomlFloat);
    CHECK("floats.neg_inf is -inf",
          fninf.as.f64 < 0 && fninf.as.f64 == fninf.as.f64 - 1);

    toml_node_t fnan = toml_seek(root, "floats.nan");
    CHECK("floats.nan is float", fnan.type == eTomlFloat);
    CHECK("floats.nan is NaN", fnan.as.f64 != fnan.as.f64); // NaN != NaN

    // hex/oct/bin are parsed as integers
    toml_node_t fhex = toml_seek(root, "floats.hex");
    CHECK("floats.hex parsed as int", fhex.type == eTomlInt);
    CHECK("floats.hex == 0xDEADBEEF", fhex.as.i64 == 0xDEADBEEF);

    toml_node_t foct = toml_seek(root, "floats.oct");
    CHECK("floats.oct parsed as int", foct.type == eTomlInt);
    CHECK("floats.oct == 0755 (493)", foct.as.i64 == 0755);

    toml_node_t fbin = toml_seek(root, "floats.bin");
    CHECK("floats.bin parsed as int", fbin.type == eTomlInt);
    CHECK("floats.bin == 0b11010110 (214)", fbin.as.i64 == 0xD6);

    // -------------------------------------------------------------------------
    // [strings]
    // -------------------------------------------------------------------------

    toml_node_t sbasic = toml_seek(root, "strings.basic");
    CHECK("strings.basic is string", sbasic.type == eTomlString);
    CHECK("strings.basic has newline",
          sbasic.type == eTomlString
              && memchr(sbasic.as.string.ptr, '\n', sbasic.as.string.len)
                     != NULL);

    toml_node_t slit = toml_seek(root, "strings.literal");
    CHECK("strings.literal is string", slit.type == eTomlString);
    CHECK("strings.literal has no real newline",
          slit.type == eTomlString
              && memchr(slit.as.string.ptr, '\n', slit.as.string.len) == NULL);
    CHECK("strings.literal contains backslash",
          slit.type == eTomlString
              && memchr(slit.as.string.ptr, '\\', slit.as.string.len) != NULL);

    toml_node_t scont = toml_seek(root, "strings.continued");
    CHECK("strings.continued is string", scont.type == eTomlString);
    CHECK("strings.continued has 'fox'",
          scont.type == eTomlString
              && memmem(scont.as.string.ptr, scont.as.string.len, "fox", 3)
                     != NULL);
    CHECK("strings.continued no leading newline",
          scont.type == eTomlString && scont.as.string.ptr[0] != '\n');

    toml_node_t smb = toml_seek(root, "strings.multi_basic");
    CHECK("strings.multi_basic is string", smb.type == eTomlString);
    CHECK("strings.multi_basic has newline",
          smb.type == eTomlString
              && memchr(smb.as.string.ptr, '\n', smb.as.string.len) != NULL);
    CHECK("strings.multi_basic starts with 'hello'",
          smb.type == eTomlString
              && strncmp(smb.as.string.ptr, "hello", 5) == 0);

    toml_node_t sml = toml_seek(root, "strings.multi_literal");
    CHECK("strings.multi_literal is string", sml.type == eTomlString);
    CHECK("strings.multi_literal has newline",
          sml.type == eTomlString
              && memchr(sml.as.string.ptr, '\n', sml.as.string.len) != NULL);

    toml_node_t su1 = toml_seek(root, "strings.u1");
    CHECK("strings.u1 is string", su1.type == eTomlString);
    CHECK("strings.u1 is UTF-8 α (2 bytes)", su1.as.string.len == 2);
    CHECK("strings.u1 byte0 == 0xCE",
          su1.type == eTomlString
              && (unsigned char)su1.as.string.ptr[0] == 0xCE);
    CHECK("strings.u1 byte1 == 0xB1",
          su1.type == eTomlString
              && (unsigned char)su1.as.string.ptr[1] == 0xB1);

    // \U0001F600 → UTF-8 0xF0 0x9F 0x98 0x80 (😀)
    toml_node_t su2 = toml_seek(root, "strings.u2");
    CHECK("strings.u2 is string", su2.type == eTomlString);
    CHECK("strings.u2 is UTF-8 😀 (4 bytes)", su2.as.string.len == 4);
    CHECK("strings.u2 byte0 == 0xF0",
          su2.type == eTomlString
              && (unsigned char)su2.as.string.ptr[0] == 0xF0);

    // -------------------------------------------------------------------------
    // [dotted]
    // -------------------------------------------------------------------------

    toml_node_t dcolor = toml_seek(root, "dotted.physical.color");
    CHECK("dotted.physical.color is string", dcolor.type == eTomlString);
    CHECK("dotted.physical.color == orange",
          strncmp(dcolor.as.string.ptr, "orange", 6) == 0);

    toml_node_t dshape = toml_seek(root, "dotted.physical.shape");
    CHECK("dotted.physical.shape == round",
          dshape.type == eTomlString
              && strncmp(dshape.as.string.ptr, "round", 5) == 0);

    toml_node_t dgoogle = toml_seek(root, "dotted.site.\"google.com\"");
    CHECK("dotted.site.\"google.com\" is bool", dgoogle.type == eTomlBool);
    CHECK("dotted.site.\"google.com\" is true", dgoogle.as.boolean == true);

    // -------------------------------------------------------------------------
    // [quoted] — quoted keys
    // -------------------------------------------------------------------------

    toml_node_t qip = toml_seek(root, "quoted.\"127.0.0.1\"");
    CHECK("quoted.\"127.0.0.1\" is string", qip.type == eTomlString);
    CHECK("quoted.\"127.0.0.1\" == localhost",
          strncmp(qip.as.string.ptr, "localhost", 9) == 0);

    toml_node_t qsp = toml_seek(root, "quoted.key with spaces");
    CHECK("quoted.\"key with spaces\" is int", qsp.type == eTomlInt);
    CHECK("quoted.\"key with spaces\" == 123", qsp.as.i64 == 123);

    toml_node_t qemoji = toml_seek(root, "quoted.😀");
    CHECK("quoted.\"😀\" is string", qemoji.type == eTomlString);
    CHECK("quoted.\"😀\" == emoji",
          strncmp(qemoji.as.string.ptr, "emoji", 5) == 0);

    // -------------------------------------------------------------------------
    // [mixed_array]
    // -------------------------------------------------------------------------

    // heterogeneous array
    toml_node_t hmix = toml_seek(root, "mixed_array.mixed");
    CHECK("mixed_array.mixed is array", hmix.type == eTomlArray);
    CHECK("mixed_array.mixed len == 4", hmix.as.array.len == 4);
    CHECK("mixed_array.mixed[0] is int",
          hmix.as.array.items[0].type == eTomlInt);
    CHECK("mixed_array.mixed[0] == 1", hmix.as.array.items[0].as.i64 == 1);
    CHECK("mixed_array.mixed[1] is string",
          hmix.as.array.items[1].type == eTomlString);
    CHECK("mixed_array.mixed[1] == hello",
          strncmp(hmix.as.array.items[1].as.string.ptr, "hello", 5) == 0);
    CHECK("mixed_array.mixed[2] is bool",
          hmix.as.array.items[2].type == eTomlBool);
    CHECK("mixed_array.mixed[2] == true",
          hmix.as.array.items[2].as.boolean == true);
    CHECK("mixed_array.mixed[3] is float",
          hmix.as.array.items[3].type == eTomlFloat);
    CHECK("mixed_array.mixed[3] == 3.14",
          hmix.as.array.items[3].as.f64 > 3.13
              && hmix.as.array.items[3].as.f64 < 3.15);

    toml_node_t hn = toml_seek(root, "mixed_array.nested");
    CHECK("mixed_array.nested is array", hn.type == eTomlArray);
    CHECK("mixed_array.nested len == 2", hn.as.array.len == 2);
    if (hn.type == eTomlArray && hn.as.array.len >= 2) {
        toml_node_t row0 = hn.as.array.items[0];
        toml_node_t row1 = hn.as.array.items[1];
        CHECK("nested[0] is array", row0.type == eTomlArray);
        CHECK("nested[0] len == 2", row0.as.array.len == 2);
        CHECK("nested[0][0][0] == 1",
              row0.as.array.items[0].as.array.items[0].as.i64 == 1);
        CHECK("nested[0][1][0] == 2",
              row0.as.array.items[1].as.array.items[0].as.i64 == 2);
        CHECK("nested[1] is array", row1.type == eTomlArray);
        CHECK("nested[1][0][0] == 3",
              row1.as.array.items[0].as.array.items[0].as.i64 == 3);
        CHECK("nested[1][1][0] == 4",
              row1.as.array.items[1].as.array.items[0].as.i64 == 4);
    }

    // nested inline table
    toml_node_t hnt = toml_seek(root, "mixed_array.nested_table.a.b.c");
    CHECK("mixed_array.nested_table.a.b.c is int", hnt.type == eTomlInt);
    CHECK("mixed_array.nested_table.a.b.c == 123", hnt.as.i64 == 123);

    // -------------------------------------------------------------------------
    // [[products]] — array of tables with sub-tables
    // -------------------------------------------------------------------------

    toml_node_t products = toml_get(root, "products");
    CHECK("products is array", products.type == eTomlArray);
    CHECK("products len == 2", products.as.array.len == 2);

    if (products.type == eTomlArray && products.as.array.len >= 2) {
        toml_node_t p0      = products.as.array.items[0];
        toml_node_t p0_name = toml_get(p0, "name");
        toml_node_t p0_wt   = toml_seek(p0, "details.weight");
        CHECK("products[0].name == Hammer",
              p0_name.type == eTomlString
                  && strncmp(p0_name.as.string.ptr, "Hammer", 6) == 0);
        CHECK("products[0].details.weight is int", p0_wt.type == eTomlInt);
        CHECK("products[0].details.weight == 10", p0_wt.as.i64 == 10);

        toml_node_t p1      = products.as.array.items[1];
        toml_node_t p1_name = toml_get(p1, "name");
        toml_node_t p1_wt   = toml_seek(p1, "details.weight");
        CHECK("products[1].name == Nail",
              p1_name.type == eTomlString
                  && strncmp(p1_name.as.string.ptr, "Nail", 4) == 0);
        CHECK("products[1].details.weight == 1", p1_wt.as.i64 == 1);
    }

    // -------------------------------------------------------------------------
    // [empty_table]
    // -------------------------------------------------------------------------

    toml_node_t etbl = toml_get(root, "empty_table");
    CHECK("empty_table is table", etbl.type == eTomlTable);
    CHECK("empty_table len == 0", etbl.as.table.len == 0);
}

// ---------------------------------------------------------------------------
// Merge / equiv smoke tests
// ---------------------------------------------------------------------------

static void run_merge_test(void) {
    printf("\n--- Merge / equiv tests ---\n");

    const char *src_a = "x = 1\ny = 2\n";
    const char *src_b = "y = 99\nz = 3\n";

    toml_result_t a = toml_parse(TOML_FROM_STR(src_a));
    toml_result_t b = toml_parse(TOML_FROM_STR(src_b));

    CHECK("parse a ok", a.ok);
    CHECK("parse b ok", b.ok);

    toml_result_t m = toml_merge(&a, &b);
    CHECK("merge ok", m.ok);

    if (m.ok) {
        toml_node_t x = toml_get(m.root, "x");
        toml_node_t y = toml_get(m.root, "y");
        toml_node_t z = toml_get(m.root, "z");
        CHECK("merged x == 1", x.type == eTomlInt && x.as.i64 == 1);
        CHECK("merged y == 99", y.type == eTomlInt && y.as.i64 == 99);
        CHECK("merged z == 3", z.type == eTomlInt && z.as.i64 == 3);
    }

    toml_result_t a2 = toml_parse(TOML_FROM_STR(src_a));
    CHECK("equiv same source", toml_equiv(&a, &a2));
    CHECK("not equiv different", !toml_equiv(&a, &b));

    toml_free(a);
    toml_free(b);
    toml_free(m);
    toml_free(a2);
}

// ---------------------------------------------------------------------------
// Error case tests
// ---------------------------------------------------------------------------

static void run_error_tests(void) {
    printf("\n--- Error case tests ---\n");

    struct {
        const char *label;
        const char *src;
    } cases[] = {
        { "duplicate key", "x = 1\nx = 2\n" },
        { "missing value", "x =\n" },
        { "bad escape", "x = \"\\q\"\n" },
        { "unterminated string", "x = \"oops\n" },
        { "duplicate table", "[a]\n[a]\n" },
    };

    int n = (int)(sizeof(cases) / sizeof(cases[0]));
    for (int i = 0; i < n; i++) {
        toml_result_t r = toml_parse(TOML_FROM_STR(cases[i].src));
        if (!r.ok) {
            printf("  PASS: %s -> \"%s\"\n", cases[i].label, r.errmsg);
        } else {
            fprintf(stderr, "  FAIL: %s (expected error, got ok)\n",
                    cases[i].label);
            check_failed++;
            toml_free(r);
        }
    }
}

static void run_version_tests(void) {
    printf("\n--- TOML version tests ---\n");

    const char *v11_src = "esc_x = \"\\x41\"\n"
                          "esc_e = \"\\e\"\n"
                          "time = 07:32\n"
                          "dt = 1979-05-27T07:32\n"
                          "odt = 1979-05-27 07:32Z\n"
                          "x = { a = 1 }\n"
                          "x.b = 2\n"
                          "[x]\n"
                          "c = 3\n";
    toml_result_t v11   = toml_parse(TOML_FROM_STR(v11_src));
    CHECK("TOML 1.1 versioned parse ok", v11.ok);
    if (v11.ok) {
        toml_node_t esc_x = toml_get(v11.root, "esc_x");
        toml_node_t esc_e = toml_get(v11.root, "esc_e");
        toml_node_t time  = toml_get(v11.root, "time");
        CHECK("TOML 1.1 accepts \\x", esc_x.type == eTomlString
                                          && esc_x.as.string.len == 1
                                          && esc_x.as.string.ptr[0] == 'A');
        CHECK("TOML 1.1 accepts \\e", esc_e.type == eTomlString
                                          && esc_e.as.string.len == 1
                                          && esc_e.as.string.ptr[0] == '\033');
        CHECK("TOML 1.1 omitted seconds are zero",
              time.type == eTomlTime && time.as.timestamp.second == 0);
        CHECK("TOML 1.1 extends inline table",
              toml_seek(v11.root, "x.b").type == eTomlInt
                  && toml_seek(v11.root, "x.c").type == eTomlInt);
    }
    toml_free(v11);

    struct {
        const char *label;
        const char *src;
    } v10_cases[] = {
        { "TOML 1.0 rejects \\x", "x = \"\\x41\"\n" },
        { "TOML 1.0 rejects \\e", "x = \"\\e\"\n" },
        { "TOML 1.0 rejects omitted seconds", "x = 07:32\n" },
        { "TOML 1.0 rejects inline dotted extension",
          "x = { a = 1 }\nx.b = 2\n" },
        { "TOML 1.0 rejects inline header extension",
          "x = { a = 1 }\n[x]\nb = 2\n" },
    };
    int n = (int)(sizeof(v10_cases) / sizeof(v10_cases[0]));
    for (int i = 0; i < n; i++) {
        toml_result_t r = toml_parse(TOML_FROM_STR_V10(v10_cases[i].src));
        if (!r.ok) {
            printf("  PASS: %s -> \"%s\"\n", v10_cases[i].label, r.errmsg);
        } else {
            fprintf(stderr, "  FAIL: %s (expected error, got ok)\n",
                    v10_cases[i].label);
            check_failed++;
            toml_free(r);
        }
    }
}

static void run_numeric_tests(void) {
    printf("\n--- Numeric boundary tests ---\n");

    const char *valid = "min = -9223372036854775808\n"
                        "max = 9223372036854775807\n"
                        "inf = inf\n"
                        "pinf = +inf\n"
                        "ninf = -inf\n"
                        "nan = nan\n"
                        "pnan = +nan\n"
                        "nnan = -nan\n";
    toml_result_t ok  = toml_parse(TOML_FROM_STR(valid));
    CHECK("numeric boundaries parse ok", ok.ok);
    if (ok.ok) {
        CHECK("INT64_MIN parses", toml_get(ok.root, "min").as.i64 == INT64_MIN);
        CHECK("INT64_MAX parses", toml_get(ok.root, "max").as.i64 == INT64_MAX);
        CHECK("inf parses", isinf(toml_get(ok.root, "inf").as.f64));
        CHECK("+inf parses", isinf(toml_get(ok.root, "pinf").as.f64));
        CHECK("-inf parses", isinf(toml_get(ok.root, "ninf").as.f64));
        CHECK("nan parses", isnan(toml_get(ok.root, "nan").as.f64));
        CHECK("+nan parses", isnan(toml_get(ok.root, "pnan").as.f64));
        CHECK("-nan parses", isnan(toml_get(ok.root, "nnan").as.f64));
    }
    toml_free(ok);

    struct {
        const char *label;
        const char *src;
    } cases[] = {
        { "decimal overflow", "x = 9223372036854775808\n" },
        { "negative decimal overflow", "x = -9223372036854775809\n" },
        { "hex overflow", "x = 0x8000000000000000\n" },
        { "octal overflow", "x = 0o1000000000000000000000\n" },
        { "binary overflow", "x = "
                             "0b10000000000000000000000000000000000000000000000"
                             "00000000000000000\n" },
        { "float overflow", "x = 1e9999\n" },
    };
    int n = (int)(sizeof(cases) / sizeof(cases[0]));
    for (int i = 0; i < n; i++) {
        toml_result_t r = toml_parse(TOML_FROM_STR(cases[i].src));
        if (!r.ok) {
            printf("  PASS: %s -> \"%s\"\n", cases[i].label, r.errmsg);
        } else {
            fprintf(stderr, "  FAIL: %s (expected error, got ok)\n",
                    cases[i].label);
            check_failed++;
            toml_free(r);
        }
    }
}

static void run_input_tests(void) {
    printf("\n--- Input span tests ---\n");

    const char bom_src[] = "\xEF\xBB\xBFx = 1\n";
    toml_result_t bom    = toml_parse(TOML_FROM_SPAN(bom_src, 9));
    CHECK("BOM input parses", bom.ok && toml_get(bom.root, "x").as.i64 == 1);
    toml_free(bom);

    const char wrapped[] = { 'x', ' ', '=', ' ', '1', '\n', '!', '!' };
    toml_result_t span   = toml_parse(TOML_FROM_SPAN(wrapped, 6));
    CHECK("non-NUL span parses",
          span.ok && toml_get(span.root, "x").as.i64 == 1);
    toml_free(span);

    const char bad_key[] = { (char)0xC3, ' ', '=', ' ', '1', '\n' };
    toml_result_t bad    = toml_parse(TOML_FROM_SPAN(bad_key, 6));
    if (!bad.ok) {
        printf("  PASS: malformed UTF-8 key -> \"%s\"\n", bad.errmsg);
    } else {
        fprintf(stderr,
                "  FAIL: malformed UTF-8 key (expected error, got ok)\n");
        check_failed++;
        toml_free(bad);
    }
}

int run_toml_tests(void) {
    check_failed = 0;

    toml_result_t result = toml_parse(TOML_FROM_STR(toml_test_string));
    if (!result.ok) {
        fprintf(stderr, "Parse failed: %s\n", result.errmsg);
        return 1;
    }

    printf("--- Full tree dump ---\n");
    printf("root = ");
    toml_print_node(result.root, 0);
    putchar('\n');

    run_checks(&result);
    run_merge_test();
    run_error_tests();
    run_version_tests();
    run_numeric_tests();
    run_input_tests();

    toml_free(result);

    printf("\n=== %s (%d check(s) failed) ===\n",
           check_failed == 0 ? "ALL PASS" : "FAILURES DETECTED", check_failed);
    return check_failed ? 1 : 0;
}

#undef CHECK
