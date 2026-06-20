# ------------------------------------------------------------------------------
# Build Targets
# ------------------------------------------------------------------------------
#
#   make / make all
#       → Builds bin/toml_parse (debug), or bin/toml_parse.exe on Windows
#
#   make release
#       → Builds bin/toml_parse (optimized), or bin/toml_parse.exe on Windows
#
#   make san
#       → Builds bin/toml_parse with sanitizers enabled, or bin/toml_parse.exe on Windows
#
#   make clean
#       → Removes bin/ and obj/ directories
#
# ------------------------------------------------------------------------------
ifeq ($(OS),Windows_NT)
	ifeq ($(origin CC),default)
		CC = gcc
	endif
	EXE_EXT = .exe
else
	CC ?= cc
	EXE_EXT =
endif

CFLAGS  = -std=c11 -Wall -Wextra -Wpedantic -Werror
ifeq ($(OS),Windows_NT)
	CFLAGS += -Wno-error=implicit-fallthrough
endif
LDFLAGS = -lm

BIN_DIR = bin
OBJ_DIR = obj

TARGET  = $(BIN_DIR)/toml_parse$(EXE_EXT)
SRCS    = $(wildcard src/*.c)
CFLAGS += -Iinclude
OBJS    = $(SRCS:src/%.c=$(OBJ_DIR)/%.o)


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


$(OBJ_DIR)/%.o: src/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BIN_DIR) $(OBJ_DIR):
ifeq ($(OS),Windows_NT)
	if not exist "$(subst /,\,$@)" mkdir "$(subst /,\,$@)"
else
	mkdir -p $@
endif

clean:
ifeq ($(OS),Windows_NT)
	if exist "$(subst /,\,$(BIN_DIR))" rmdir /S /Q "$(subst /,\,$(BIN_DIR))"
	if exist "$(subst /,\,$(OBJ_DIR))" rmdir /S /Q "$(subst /,\,$(OBJ_DIR))"
else
	rm -rf $(BIN_DIR) $(OBJ_DIR)
endif
