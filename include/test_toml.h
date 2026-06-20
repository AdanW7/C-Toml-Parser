#ifndef TEST_TOML_H_
#define TEST_TOML_H_

#include "toml.h"

#ifdef __cplusplus
extern "C" {
#endif
void toml_print_node(toml_node_t n, int depth);
int run_toml_tests(void);
#ifdef __cplusplus
extern "C"
}
#endif

#endif /*TEST_TOML_H_*/
