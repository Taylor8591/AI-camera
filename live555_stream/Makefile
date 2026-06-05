CC ?= gcc
CXX ?= g++

.DEFAULT_GOAL := all

TARGET := live555_h265_ring_unicast_server
TARGET_SRC := live555_h265_ring_unicast_server.cpp
TARGET_OBJ := $(TARGET_SRC:.cpp=.o)

CAMERA_TARGET := camera_recorder_live_fifo
CAMERA_SRC := camera_recorder_live_fifo.c
CAMERA_OBJ := $(CAMERA_SRC:.c=.o)

LAUNCHER_TARGET := camera_live555_launcher
LAUNCHER_SRC := camera_live555_launcher.cpp
LAUNCHER_OBJ := $(LAUNCHER_SRC:.cpp=.o)
LAUNCHER_CAMERA_OBJ := v4l2_preview_sdl_entry.o
LAUNCHER_SERVER_OBJ := live555_h265_ring_unicast_server_entry.o
CAMERA_PREVIEW_SRC := v4l2_preview_sdl.c

SDL_CFLAGS := $(shell sdl2-config --cflags)
SDL_LIBS := $(shell sdl2-config --libs)

LIVE555_INCLUDE_DIR := /usr/local/include
LIVE555_LIB_DIR := /usr/local/lib
ROCKCHIP_INCLUDE_DIR := /usr/include/rockchip
ROCKCHIP_LIB_DIR := /usr/lib/rockchip

CPPFLAGS := \
	-I$(LIVE555_INCLUDE_DIR) \
	-I$(LIVE555_INCLUDE_DIR)/liveMedia \
	-I$(LIVE555_INCLUDE_DIR)/BasicUsageEnvironment \
	-I$(LIVE555_INCLUDE_DIR)/UsageEnvironment \
	-I$(LIVE555_INCLUDE_DIR)/groupsock \
	-MMD -MP

CFLAGS ?= -O2 -g -Wall
CXXFLAGS ?= -O2 -g -Wall -std=c++20

LIVE555_LIBS := \
	$(LIVE555_LIB_DIR)/libliveMedia.a \
	$(LIVE555_LIB_DIR)/libgroupsock.a \
	$(LIVE555_LIB_DIR)/libBasicUsageEnvironment.a \
	$(LIVE555_LIB_DIR)/libUsageEnvironment.a \
	-lssl -lcrypto -lpthread

CAMERA_LIBS := \
	/usr/lib/libavdevice.so.60 \
	/usr/lib/libavformat.so.60 \
	/usr/lib/libavcodec.so.60 \
	/usr/lib/libavutil.so.58 \
	/usr/lib/libswscale.so.7 \
	/usr/lib/libswresample.so.4 \
	-lasound -ldrm -lv4l2 -lxcb -lxcb-shm -lm -lpthread

LAUNCHER_LIBS := \
	$(LIVE555_LIBS) \
	-L$(ROCKCHIP_LIB_DIR) \
	-lrockchip_mpp \
	-lv4l2

ALL_TARGETS := $(TARGET) $(CAMERA_TARGET) $(LAUNCHER_TARGET)
ALL_OBJS := $(TARGET_OBJ) $(CAMERA_OBJ) $(LAUNCHER_OBJ) $(LAUNCHER_CAMERA_OBJ) $(LAUNCHER_SERVER_OBJ)
DEP_FILES := $(ALL_OBJS:.o=.d)

all: $(ALL_TARGETS)

$(TARGET): $(TARGET_OBJ)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LIVE555_LIBS)

$(CAMERA_TARGET): $(CAMERA_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(CAMERA_LIBS) $(SDL_LIBS)

$(LAUNCHER_TARGET): $(LAUNCHER_OBJ) $(LAUNCHER_CAMERA_OBJ) $(LAUNCHER_SERVER_OBJ)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LAUNCHER_LIBS) $(SDL_LIBS)

$(CAMERA_OBJ): CPPFLAGS += $(SDL_CFLAGS)

$(LAUNCHER_CAMERA_OBJ): CPPFLAGS += -I$(ROCKCHIP_INCLUDE_DIR)
$(LAUNCHER_CAMERA_OBJ): CPPFLAGS += $(SDL_CFLAGS)
$(LAUNCHER_CAMERA_OBJ): CFLAGS += -Dmain=v4l2_preview_sdl_entry
$(LAUNCHER_CAMERA_OBJ): $(CAMERA_PREVIEW_SRC)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(LAUNCHER_SERVER_OBJ): CXXFLAGS += -Dmain=live555_server_entry
$(LAUNCHER_SERVER_OBJ): $(TARGET_SRC)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

%.o: %.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(ALL_TARGETS) $(ALL_OBJS) $(DEP_FILES)

.PHONY: all run clean

-include $(DEP_FILES)
