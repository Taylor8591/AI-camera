CC ?= gcc
CXX ?= g++

.DEFAULT_GOAL := all

LAUNCHER_TARGET := camera_live555_launcher
LAUNCHER_SRC := camera_live555_launcher.cpp
LAUNCHER_OBJ := $(LAUNCHER_SRC:.cpp=.o)
LAUNCHER_CAMERA_OBJ := v4l2_preview_sdl.o
LAUNCHER_SERVER_OBJ := live555_h265_ring_unicast_server.o
LAUNCHER_MP4_OBJ := fifo_h265_to_mp4.o
CAMERA_PREVIEW_SRC := v4l2_preview_sdl.c
MP4_MUXER_SRC := fifo_h265_to_mp4.c
SERVER_SRC := live555_h265_ring_unicast_server.cpp

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
	-I$(LIVE555_INCLUDE_DIR)/groupsock

CFLAGS ?= -O2 -g -Wall
CXXSTD ?= -std=c++2a
CXXFLAGS ?= -O2 -g -Wall $(CXXSTD)

LIVE555_LIBS := \
	$(LIVE555_LIB_DIR)/libliveMedia.a \
	$(LIVE555_LIB_DIR)/libgroupsock.a \
	$(LIVE555_LIB_DIR)/libBasicUsageEnvironment.a \
	$(LIVE555_LIB_DIR)/libUsageEnvironment.a \
	-lssl -lcrypto -lpthread

LAUNCHER_LIBS := \
	$(LIVE555_LIBS) \
	-L$(ROCKCHIP_LIB_DIR) \
	-lrockchip_mpp \
	-lv4l2 \
	-lavformat \
	-lavcodec \
	-lavutil

ALL_TARGETS := $(LAUNCHER_TARGET)
ALL_OBJS := $(LAUNCHER_OBJ) $(LAUNCHER_CAMERA_OBJ) $(LAUNCHER_SERVER_OBJ) $(LAUNCHER_MP4_OBJ)
DEP_FILES := $(ALL_OBJS:.o=.d)

all: $(ALL_TARGETS)

$(LAUNCHER_TARGET): $(LAUNCHER_OBJ) $(LAUNCHER_CAMERA_OBJ) $(LAUNCHER_SERVER_OBJ) $(LAUNCHER_MP4_OBJ)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LAUNCHER_LIBS) $(SDL_LIBS)
	rm -f $(LAUNCHER_OBJ) $(LAUNCHER_CAMERA_OBJ) $(LAUNCHER_SERVER_OBJ) $(LAUNCHER_MP4_OBJ) \
		$(LAUNCHER_OBJ:.o=.d) $(LAUNCHER_CAMERA_OBJ:.o=.d) $(LAUNCHER_SERVER_OBJ:.o=.d) $(LAUNCHER_MP4_OBJ:.o=.d)

$(LAUNCHER_CAMERA_OBJ): CPPFLAGS += -I$(ROCKCHIP_INCLUDE_DIR)
$(LAUNCHER_CAMERA_OBJ): CPPFLAGS += $(SDL_CFLAGS)
$(LAUNCHER_CAMERA_OBJ): $(CAMERA_PREVIEW_SRC)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(LAUNCHER_SERVER_OBJ): $(SERVER_SRC)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(LAUNCHER_MP4_OBJ): $(MP4_MUXER_SRC)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

%.o: %.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

run: $(LAUNCHER_TARGET)
	./$(LAUNCHER_TARGET)

clean:
	rm -f $(ALL_TARGETS) $(ALL_OBJS) $(DEP_FILES)

.PHONY: all run clean
