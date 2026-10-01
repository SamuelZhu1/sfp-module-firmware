# Host build of the SFP SDK: Unity unit tests and the simulator binary that
# the pytest suite drives in CI. Runs on Linux with plain GCC, no board.
#
#   make            build and run all unit tests
#   make sim        build ./build/sfp_sim (shell over a fake module)
#   make coverage   unit tests with gcov line coverage of src/

CC      ?= gcc
CFLAGS  ?= -std=c11 -O1 -g -Wall -Wextra -Werror -Wshadow -Wconversion -Wno-sign-conversion
CFLAGS  += -DSFP_TEST_HOOKS -D_POSIX_C_SOURCE=200809L
UNITY_DIR ?= unity
UNITY_URL := https://github.com/ThrowTheSwitch/Unity.git
UNITY_TAG := v2.6.0

SRC_DIR   := ../src
SHELL_DIR := ../../shell
INC   := -I../include -I$(SRC_DIR) -I$(SHELL_DIR)/include -I. -Ifixtures -I$(UNITY_DIR)/src
SDK   := $(SRC_DIR)/sfp_i2c.c $(SRC_DIR)/sfp_decode.c $(SRC_DIR)/sfp_core.c \
         $(SHELL_DIR)/sfp_shell.c fake_port.c
TESTS := test_decode test_i2c test_state test_shell test_fault_campaign
BUILD := build

.PHONY: all test sim coverage clean
all: test

$(UNITY_DIR)/src/unity.c:
	git clone --depth 1 --branch $(UNITY_TAG) $(UNITY_URL) $(UNITY_DIR)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%: %.c $(SDK) $(UNITY_DIR)/src/unity.c | $(BUILD)
	$(CC) $(CFLAGS) $(EXTRA) $(INC) -o $@ $< $(SDK) $(UNITY_DIR)/src/unity.c -lm

test: $(addprefix $(BUILD)/,$(TESTS))
	@set -e; for t in $(TESTS); do echo "== $$t"; ./$(BUILD)/$$t; done

sim: $(BUILD)/sfp_sim

$(BUILD)/sfp_sim: sim_main.c $(SDK) | $(BUILD)
	$(CC) $(CFLAGS) $(INC) -o $@ sim_main.c $(SDK)

coverage:
	$(MAKE) clean
	$(MAKE) test EXTRA="--coverage -O0"
	# each test links its own copy of the SDK; gcovr merges them (pip install gcovr)
	gcovr --root ../.. --filter '../src/' --filter '../../shell/' build | tee $(BUILD)/coverage.txt

clean:
	rm -rf $(BUILD) *.gcov *.gcda *.gcno
