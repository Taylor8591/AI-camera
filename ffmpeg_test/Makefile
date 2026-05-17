# Build all C applications in this directory

CC := gcc
CFLAGS := -Wall -O2 -g

LIBS := /usr/lib/libavdevice.so.60 /usr/lib/libavformat.so.60 /usr/lib/libavcodec.so.60 /usr/lib/libavutil.so.58 /usr/lib/libswscale.so.7 /usr/lib/libswresample.so.4 -lasound -ldrm -lv4l2 -lxcb -lxcb-shm -lm -lpthread
SDL_FLAGS := `sdl2-config --cflags --libs`

APP_SRCS := $(wildcard *.c)
TARGETS := $(APP_SRCS:.c=)

# Optional: choose one target to run, e.g. make run RUN_TARGET=camera_recorder
RUN_TARGET ?= $(firstword $(TARGETS))

all: $(TARGETS)

%: %.c
	$(CC) $(CFLAGS) -o $@ $< $(LIBS) $(SDL_FLAGS)

clean:
	rm -f $(TARGETS)

run: $(RUN_TARGET)
	sudo ./$(RUN_TARGET)

list:
	@echo "Sources : $(APP_SRCS)"
	@echo "Targets : $(TARGETS)"

.PHONY: all clean run list
