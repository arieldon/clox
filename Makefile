BIN := clox
TEST_BIN := test

BUILD_DIR := ./build
SRC_DIR := ./src
INC_DIR := ./include
TEST_DIR := ./test

SRCS := $(wildcard $(SRC_DIR)/*.c)
OBJS := $(SRCS:%=$(BUILD_DIR)/%.o)
DEPS := $(OBJS:.o=.d)

CFLAGS := -Wall -Wextra
CPPFLAGS := -I$(INC_DIR) -MMD -MP

ifeq ($(MODE),debug)
	LDFLAGS += -fsanitize=address
	CFLAGS += -O0 -DDEBUG -g3
else
	CFLAGS += -O2
endif

$(BUILD_DIR)/$(BIN): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)

$(BUILD_DIR)/%.c.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR)

install: $(BUILD_DIR)/$(BIN)
	install -m755 $(BUILD_DIR)/$(BIN) /usr/local/bin

$(BUILD_DIR)/$(TEST_BIN): $(TEST_DIR)/test.c
	$(CC) $< -o $@

test: $(BUILD_DIR)/$(BIN) $(BUILD_DIR)/$(TEST_BIN)
	$(BUILD_DIR)/$(TEST_BIN) $(BUILD_DIR)/$(BIN) $(TEST_DIR)/

.PHONY: clean install test

-include $(DEPS)
