CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror
CPPFLAGS ?= -I.
BUILD_DIR = build

.PHONY: all test test-unit test-network clean

all: $(BUILD_DIR)/race-server $(BUILD_DIR)/race-observer

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/race-server: server.c race.c race.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) server.c race.c -o $@

$(BUILD_DIR)/race-observer: observer.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) observer.c -o $@

$(BUILD_DIR)/test-race: tests/test_race.c race.c race.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_race.c race.c -o $@

test-unit: $(BUILD_DIR)/test-race
	./$(BUILD_DIR)/test-race

test-network: all
	sh tests/test_network.sh

test: test-unit test-network

clean:
	rm -rf $(BUILD_DIR)
