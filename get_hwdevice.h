#ifndef GET_HWDEVICE_H
#define GET_HWDEVICE_H
#include "common.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>

typedef struct HWDevice {
    char *name;
    enum AVHWDeviceType type;
    AVBufferRef *device_ref;
} HWDevice;

void print_avaliable_hw_devices(FILE *f);

void print_suported_hw_devices_for_decoder(FILE *f, const AVCodec *decoder);

const AVCodecHWConfig *get_supported_hwdecoder(enum AVHWDeviceType type,const AVCodec *decoder);

/* returns the same as av_find_best_stream*/
int init_avformat(const char *file_name, const AVCodec **decoder, AVFormatContext **input_ctx);


int hw_decoder_init(AVCodecContext *ctx, const enum AVHWDeviceType type);
int hw_decoder_init_auto(AVCodecContext *ctx, AVCodec *decoder, enum AVHWDeviceType *type);

#endif // GET_HWDEVICE_H
