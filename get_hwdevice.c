#include "common.h"
#include <stdio.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>

void print_avaliable_hw_devices(FILE *f) {
	enum AVHWDeviceType type = AV_HWDEVICE_TYPE_NONE;
	while ((type = av_hwdevice_iterate_types(type)) != AV_HWDEVICE_TYPE_NONE) {
		fprintf(f, "%s ", av_hwdevice_get_type_name(type));
	}
}

void print_suported_hw_devices_for_decoder(FILE *f, const AVCodec *decoder) {
	const AVCodecHWConfig *config = NULL;
	int i = 0;
	while ((config = avcodec_get_hw_config(decoder, i++)) != NULL) {
		if (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) {
			fprintf(f, "%s ", av_hwdevice_get_type_name(config->device_type));
		}
	}
}

const AVCodecHWConfig *get_supported_hwdecoder(enum AVHWDeviceType type,const AVCodec *decoder) {
	const AVCodecHWConfig *config = NULL;
	int i = 0;
	while ((config = avcodec_get_hw_config(decoder, i++)) != NULL) {
		if (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX &&
				config->device_type == type) {
			return config;
		}
	}
	return NULL;
}

/* returns the same as av_find_best_stream*/
int init_avformat(const char *file_name, const AVCodec **decoder, AVFormatContext **input_ctx) {
	int ret;
	/* open the input file */
	if (avformat_open_input(input_ctx, file_name, NULL, NULL) != 0) {
		fprintf(stderr, "Cannot open input file '%s'\n", file_name);
		return -1;
	}

	if (avformat_find_stream_info(*input_ctx, NULL) < 0) {
		fprintf(stderr, "Cannot find input stream information.\n");
		avformat_close_input(input_ctx);
		return -1;
	}

	/* find the video stream information */
	ret = av_find_best_stream(*input_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, decoder, 0);
	if (ret < 0) {
		fprintf(stderr, "Cannot find a video stream in the input file\n");
		avformat_close_input(input_ctx);
	}
	return ret;
}

int hw_decoder_init(AVCodecContext *ctx, const enum AVHWDeviceType type) {
	int err = 0;

	if ((err = av_hwdevice_ctx_create(&ctx->hw_device_ctx, type, NULL, NULL, 0)) < 0) {
		fprintf(stderr, "Failed to create specified HW device.\n");
		return err;
	}

	return err;
}

int hw_decoder_init_auto(AVCodecContext *ctx, AVBufferRef *hw_device_ctx) {
	const AVCodecHWConfig *config;
	enum AVHWDeviceType type;
	HWDevice *dev = NULL;
	int i, err = 0;
	for (i = 0; !dev; i++) {
		config = avcodec_get_hw_config(ist->dec, i);
		if (!config)
			break;
		type = config->device_type;
		// Try to make a new device of this type.
		// If error device will be unchanged and we'll try the next one.
		err = hw_device_init_from_type(type, NULL, &dev);
		if (err < 0) {
			// Can't make a device of this type.
			continue;
		}
		fprintf(stderr, "Using auto "
			"hwaccel type %s with new default device.\n",
			av_hwdevice_get_type_name(type));
	}
	if (dev) {
		ist->hwaccel_device_type = type;
	} else {
		fprintf(stderr, "Auto hwaccel "
			   "disabled: no device found.\n");
		ist->hwaccel_id = HWACCEL_NONE;
		return 0;
	}
/*
	if ((err = av_hwdevice_ctx_create(&hw_device_ctx, type, NULL, NULL, 0)) < 0) {
		fprintf(stderr, "Failed to create specified HW device.\n");
		return err;
	}*/
	ctx->hw_device_ctx = av_buffer_ref(dev->device_ref);

	return err;
}

int hw_device_init_from_type(enum AVHWDeviceType type,
                             const char *device,
                             HWDevice **dev_out)
{
    AVBufferRef *device_ref = NULL;
    HWDevice *dev;
    char *name;
    int err;

    name = hw_device_default_name(type);
    if (!name) {
        err = AVERROR(ENOMEM);
        goto fail;
    }

    err = av_hwdevice_ctx_create(&device_ref, type, device, NULL, 0);
    if (err < 0) {
        av_log(NULL, AV_LOG_ERROR,
               "Device creation failed: %d.\n", err);
        goto fail;
    }

    dev = hw_device_add();
    if (!dev) {
        err = AVERROR(ENOMEM);
        goto fail;
    }

    dev->name = name;
    dev->type = type;
    dev->device_ref = device_ref;

    if (dev_out)
        *dev_out = dev;

    return 0;

fail:
    av_freep(&name);
    av_buffer_unref(&device_ref);
    return err;