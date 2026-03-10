# Simple Makefile for stream_server (no cmake required)
# Requires: FFmpeg development libraries

CC = gcc
CFLAGS = -Wall -Wextra -O2 -std=c11 -I./include $(shell pkg-config --cflags libavcodec libavformat libavutil libswscale)
LDFLAGS = -lpthread -lm $(shell pkg-config --libs libavcodec libavformat libavutil libswscale)

SRCDIR = src
INCDIR = include
BUILDDIR = build

SOURCES = $(wildcard $(SRCDIR)/*.c)
OBJECTS = $(patsubst $(SRCDIR)/%.c,$(BUILDDIR)/%.o,$(SOURCES))
TARGET = $(BUILDDIR)/stream_server

.PHONY: all clean check

all: check $(BUILDDIR) $(TARGET)

check:
	@pkg-config --exists libavcodec || (echo "Error: FFmpeg not found. Run: sudo apt install libavcodec-dev libavformat-dev libavutil-dev libswscale-dev" && exit 1)

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) -o $@ $(LDFLAGS)

$(BUILDDIR)/%.o: $(SRCDIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILDDIR)

# Test targets
test_hw: $(BUILDDIR) $(BUILDDIR)/test_hw_decode

$(BUILDDIR)/test_hw_decode: tests/test_hw_decode.c $(BUILDDIR)/decoder.o $(BUILDDIR)/protocol.o
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

benchmark: $(BUILDDIR) $(BUILDDIR)/test_10sec_load

$(BUILDDIR)/test_10sec_load: tests/test_10sec_load.c $(BUILDDIR)/decoder.o $(BUILDDIR)/protocol.o
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# Install target
install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/
