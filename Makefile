# ------------------------------------------------------------------------------
# Build Targets
# ------------------------------------------------------------------------------
#
#   make / make all
#       → Builds bin/toml_parse (debug)
#
#   make release
#       → Builds bin/toml_parse (optimized)
#
#   make san
#       → Builds bin/toml_parse with sanitizers enabled
#
#   make clean
#       → Removes bin/ and obj/ directories
#
# ------------------------------------------------------------------------------
CC      = cc
CFLAGS  = -std=c11 -Wall -Wextra -Wpedantic -Werror
LDFLAGS = -lm

BIN_DIR = bin
OBJ_DIR = obj

TARGET  = $(BIN_DIR)/toml_parse
SRCS    = toml_parse.c
OBJS    = $(SRCS:%.c=$(OBJ_DIR)/%.o)

SAN_FLAGS = -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
            -fno-optimize-sibling-calls

.PHONY: all clean release san

all: CFLAGS += -g -O0 -DDEBUG
all: $(TARGET)

release: CFLAGS += -O2 -DNDEBUG
release: $(TARGET)

san: CFLAGS  += $(SAN_FLAGS)
san: LDFLAGS += -fsanitize=address,undefined
san: $(TARGET)

$(TARGET): $(OBJS) | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

$(OBJ_DIR)/%.o: %.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BIN_DIR) $(OBJ_DIR):
	mkdir -p $@

clean:
	rm -rf $(BIN_DIR) $(OBJ_DIR)
