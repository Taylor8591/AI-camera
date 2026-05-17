CXX := g++
TARGET := live555_h265_ring_unicast_server
SRC := live555_h265_ring_unicast_server.cpp
OBJ := $(SRC:.cpp=.o)
CC := gcc
CAMERA_TARGET := camera_recorder_live_fifo
CAMERA_SRC := camera_recorder_live_fifo.c
CAMERA_CFLAGS := -Wall -O2
CAMERA_LIBS := /usr/lib/libavdevice.so.60 /usr/lib/libavformat.so.60 /usr/lib/libavcodec.so.60 /usr/lib/libavutil.so.58 /usr/lib/libswscale.so.7 /usr/lib/libswresample.so.4 -lasound -ldrm -lv4l2 -lxcb -lxcb-shm -lm -lpthread
SDL_FLAGS := `sdl2-config --cflags --libs`

INCLUDES := -I/usr/local/include -I/usr/local/include/liveMedia -I/usr/local/include/BasicUsageEnvironment -I/usr/local/include/UsageEnvironment -I/usr/local/include/groupsock
CXXFLAGS := -O2 -g -Wall -std=c++20 $(INCLUDES)

LIBDIR := /usr/local/lib
LIBS := \
	$(LIBDIR)/libliveMedia.a \
	$(LIBDIR)/libgroupsock.a \
	$(LIBDIR)/libBasicUsageEnvironment.a \
	$(LIBDIR)/libUsageEnvironment.a \
	-lssl -lcrypto -lpthread

all: $(TARGET) $(CAMERA_TARGET)

$(TARGET): $(OBJ)
	$(CXX) -o $@ $^ $(LIBS)

$(CAMERA_TARGET): $(CAMERA_SRC)
	$(CC) -g -o $@ $^ $(CAMERA_CFLAGS) $(CAMERA_LIBS) $(SDL_FLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET) $(OBJ) $(CAMERA_TARGET)

.PHONY: all run clean
