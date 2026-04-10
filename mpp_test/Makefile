CC = gcc
CFLAGS = -Wall -O2 -g
LDFLAGS_MPP = -I/usr/include/rockchip -L/usr/lib/rockchip -lrockchip_mpp -lpthread
LDFLAGS_FFMPEG = -lavformat -lavcodec -lavutil

TARGETS = yuv_to_h265 h265_to_mp4 yuv_to_mp4

all: $(TARGETS)

yuv_to_h265: yuv_to_h265.c
	$(CC) $(CFLAGS) -I/usr/include/rockchip $< $(LDFLAGS_MPP) -o $@

h265_to_mp4: h265_to_mp4.c
	$(CC) $(CFLAGS) $< $(LDFLAGS_FFMPEG) -o $@

yuv_to_mp4: yuv_to_mp4.c
	$(CC) $(CFLAGS) -I/usr/include/rockchip $< $(LDFLAGS_MPP) $(LDFLAGS_FFMPEG) -o $@

clean:
	rm -f $(TARGETS)
	rm output.*

.PHONY: all clean
