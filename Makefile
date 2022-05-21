BIN := clox

BUILD_DIR := ./build
SRC_DIR := ./src
INC_DIR := ./include

SRCS := $(wildcard $(SRC_DIR)/*.c)
OBJS := $(SRCS:%=$(BUILD_DIR)/%.o)
DEPS := $(OBJS:.o=.d)

LDFLAGS += -fsanitize=address
CFLAGS := -g3 -Wall -Wextra
CPPFLAGS := -I$(INC_DIR) -MMD -MP

$(BUILD_DIR)/$(BIN): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)

$(BUILD_DIR)/%.c.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

clean:
	rm -r $(BUILD_DIR)

.PHONY: clean

-include $(DEPS)
