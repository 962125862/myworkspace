#ifndef DECODER_FFMPEG_H
#define DECODER_FFMPEG_H

#include <stdbool.h>
#include <libavcodec/avcodec.h>

int ffmpeg_init(int videoFormat, int width, int height, int perf_lvl, int buffer_count, int thread_count);
void ffmpeg_destroy(void);
int ffmpeg_decode(unsigned char* indata, int inlen);
AVFrame* ffmpeg_get_frame(bool native_frame);

#endif
