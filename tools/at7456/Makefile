CROSS_COMPILE ?= arm-linux-gnueabihf-
CC := $(CROSS_COMPILE)gcc

CPPFLAGS ?=
CFLAGS ?= -O2 -Wall -Wextra -Wpedantic
TARGET_CFLAGS ?= -mcpu=cortex-a7
LDFLAGS ?= -static

BUILD_DIR := build
TARGET := $(BUILD_DIR)/at7456e_font
SOURCE := at7456e_font.c

all: $(TARGET)

$(TARGET): $(SOURCE)
	mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(TARGET_CFLAGS) $(LDFLAGS) -o $@ $<

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all clean
