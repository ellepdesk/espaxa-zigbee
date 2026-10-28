# Builds the firmware's pure C modules for the host, so they can be unit
# tested from Python.
#
#   build/libespaxa_test.so - motion.c and battery_level.c from src/, plus
#                             shim.c: a fake AXA, a fake clock and an
#                             extern API for ctypes
#
# Run `make` here, or just run pytest (conftest.py invokes this Makefile).

CC ?= cc
SRC_DIR := ../src
BUILD_DIR := build

CFLAGS := -std=c11 -g -O0 -fPIC -Wall -Wextra -Werror -Wshadow -I$(SRC_DIR)
LDFLAGS := -shared

SO := $(BUILD_DIR)/libespaxa_test.so
OBJS := $(BUILD_DIR)/motion.o $(BUILD_DIR)/battery_level.o $(BUILD_DIR)/shim.o

.PHONY: all clean
all: $(SO)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c $(SRC_DIR)/%.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/shim.o: shim.c $(SRC_DIR)/motion.h $(SRC_DIR)/battery_level.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(SO): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

clean:
	rm -rf $(BUILD_DIR)
