#include "common.h"
#include <sys/time.h>
#include <sys/sysinfo.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>

#include "get_hwdevice.h"
#include "ansi_colors.h"

typedef int(*receive_frame_callback)(AVCodecContext*);

typedef struct {
	const char *file_name;
	struct timeval start;
	enum AVPixelFormat hw_pix_fmt;
	AVFrame *frame;
	AVFrame *sw_frame;
} frame_user_data_t;

static uint64_t get_diff_time_ms(struct timeval start, struct timeval end);

static void print_help(const char *program_name);
static int receive_frame(AVCodecContext *ctx);
static enum AVPixelFormat get_hw_format(AVCodecContext *ctx, const enum AVPixelFormat *pix_fmts);
static int decode_frames(AVCodecContext *avctx, AVPacket *packet, enum AVHWDeviceType type,
			 receive_frame_callback callback);
static int parse_hardware_codecs(const char *device_type, const AVCodec *decoder);

int main(int argc, char *argv[]) {
	AVFormatContext *input_ctx = NULL;
	const AVCodec *decoder = NULL;
	AVStream *video = NULL;
	AVCodecContext *decoder_ctx = NULL;
	AVPacket *packet = NULL;
	enum AVHWDeviceType type;
	int video_stream, ret = 0;
	const char *device_type = NULL, *input_file = NULL, *output_file = NULL;
	frame_user_data_t frame_user_data = {NULL, { 0, 0 }, AV_PIX_FMT_NONE, NULL, NULL};

	if (argc < 3 || argc > 4) {
		print_help(argv[0]);
		return -1;
	}

	if (argc == 3) {
		input_file = argv[1];
		output_file = argv[2];
	} else {
		device_type = argv[1];
		input_file = argv[2];
		output_file = argv[3];
	}
	frame_user_data.file_name = output_file;

	if (!(frame_user_data.frame = av_frame_alloc())) {
		fprintf(stderr, "Can not alloc frame\n");
		ret = AVERROR(ENOMEM);
		return ret;
	}
	if (!(frame_user_data.sw_frame = av_frame_alloc())) {
		fprintf(stderr, "Can not alloc frame\n");
		ret = AVERROR(ENOMEM);
		goto fail_frame;
	}

	ret = init_avformat(input_file, &decoder, &input_ctx);
	if (ret < 0) {
		fprintf(stderr, "Failed to init avformat\n");
		goto fail_sw_frame;
	}

	video_stream = ret;
	video = input_ctx->streams[video_stream];

	ret = parse_hardware_codecs(device_type, decoder);
	if (ret < 0) {
		print_help(argv[0]);
		goto fail_inctx;
	}
	type = ret;

	decoder = avcodec_find_decoder(video->codecpar->codec_id);
	if (!decoder) {
		fprintf(stderr, "Failed to find codec\n");
		ret = AVERROR(EINVAL);
		goto fail_inctx;
	}

	if (!(decoder_ctx = avcodec_alloc_context3(decoder))) {
		ret = AVERROR(ENOMEM);
		goto fail_inctx;
	}

	decoder_ctx->opaque = &frame_user_data;

	if ((ret = avcodec_parameters_to_context(decoder_ctx, video->codecpar)) < 0) {
		fprintf(stderr, "Failed to copy codec parameters to decoder context\n");
		goto fail_decctx;
	}

	packet = av_packet_alloc();
	if (!packet) {
		fprintf(stderr, "Failed to allocate AVPacket\n");
		ret = AVERROR(ENOMEM);
		goto fail_decctx;
	}

	if (type != AV_HWDEVICE_TYPE_NONE) {
		frame_user_data.hw_pix_fmt = get_supported_hwdecoder(type, decoder)->pix_fmt;
		decoder_ctx->get_format = get_hw_format;
		if ((ret = hw_decoder_init(decoder_ctx, type)) < 0)
			goto fail_packet;
	} else {
		decoder_ctx->thread_count = get_nprocs();
		decoder_ctx->thread_type = FF_THREAD_FRAME;
	}

	if ((ret = avcodec_open2(decoder_ctx, decoder, NULL)) < 0) {
		fprintf(stderr, "Failed to open codec for stream #%u\n", video_stream);
		goto fail_hwdec;
	}
	type = AV_HWDEVICE_TYPE_NONE;
	/* actual decoding and dump the raw data */
	while (ret >= 0) {
		if ((ret = av_read_frame(input_ctx, packet)) < 0)
			break;
		if (video_stream == packet->stream_index) {
			ret = decode_frames(decoder_ctx, packet, type,
					    receive_frame);
		}
		av_packet_unref(packet);
	}

	/* flush the decoder */
	ret = decode_frames(decoder_ctx, NULL, type, NULL);

	fail_hwdec:
	if (decoder_ctx->hw_device_ctx)
		av_buffer_unref(&decoder_ctx->hw_device_ctx);
	fail_packet:
	av_packet_free(&packet);
	fail_decctx:
	avcodec_free_context(&decoder_ctx);
	fail_inctx:
	avformat_close_input(&input_ctx);
	fail_sw_frame:
	av_frame_free(&frame_user_data.sw_frame);
	fail_frame:
	av_frame_free(&frame_user_data.frame);
	return ret;
}

static void print_help(const char *program_name) {
	fprintf(stderr,
	"If only one hardware device is available, it will be used by default.\n" \
	"Usage: %1$s <input file> <output file>\n" \
	"If multiple hardware devices are available, the desired device must be specified.\n" \
	"Usage: %1$s <device type> <input file> <output file>\n\n" \
	"Available device types:\n", program_name);
	print_avaliable_hw_devices(stderr);
	puts("");
}

static uint64_t get_diff_time_ms(struct timeval start, struct timeval end) {
	uint64_t start_ms = (uint64_t)start.tv_sec * 1000ull + start.tv_usec / 1000ull;
	uint64_t end_ms = (uint64_t)end.tv_sec * 1000ull + end.tv_usec / 1000ull;
	return end_ms - start_ms;
}

static int receive_frame(AVCodecContext *ctx) {
	frame_user_data_t *frame_data = (frame_user_data_t*)ctx->opaque;
	AVFrame *frame = frame_data->frame;
	static char filename[1024];
	int xsize = frame->width;
	int ysize = frame->height;
	int wrap = frame->linesize[0];
	int i;
	unsigned char *buf = frame->data[0];
	FILE *f;
	struct timeval end;

	if (strlen(frame_data->file_name) == 0) {
		if (ctx->frame_number == 1) {
			gettimeofday(&frame_data->start, NULL);
		} else if (ctx->frame_number%10 == 2) {
			gettimeofday(&end, NULL);
			uint64_t ms = get_diff_time_ms(frame_data->start, end);
			double fps = (double)ctx->frame_number * 1000.0 / (double)ms;
			printf("Frame % 7d decoded in % 7lld ms, FPS: %3.2lf\r", ctx->frame_number, ms, fps);
			fflush(stdout);
		}
		return 0;
	}

	if (0) {
		snprintf(filename, sizeof(filename), "%s-%d",
			 frame_data->file_name, ctx->frame_number);
		f = fopen(filename, "wb");
		fprintf(f, "P5\n%d %d\n%d\n", xsize, ysize, 255);
		for (i = 0; i < ysize; i++)
			fwrite(buf + i * wrap, 1, xsize, f);
		fclose(f);
	}

	return 0;
}

static int parse_hardware_codecs(const char *device_type, const AVCodec *decoder) {
	int type;
	if (avcodec_get_hw_config(decoder, 0) == NULL) {
		fprintf(stderr, "Hardware decoding for %s not supported.\nDefulating to software decoding...\n", decoder->name);
		return AV_HWDEVICE_TYPE_NONE;
	}
	// If no device type is specified, use the first one if there is only one.
	if (device_type == NULL) {
		if (avcodec_get_hw_config(decoder, 1) == NULL) {
			return avcodec_get_hw_config(decoder, 0)->device_type;
		} else {
			fprintf(stderr, COLOR_LIGHT_CYAN
			"Multiple hardware decoders found, please specify one.\n");
			print_suported_hw_devices_for_decoder(stderr, decoder);
			puts(COLOR_RESET "\n");
			return -1;
		}
	}
	if (!strcmp(device_type, "none"))
		return AV_HWDEVICE_TYPE_NONE;

	type = av_hwdevice_find_type_by_name(device_type);

	// check if hardware decoder exists
	if (type == AV_HWDEVICE_TYPE_NONE) {
		fprintf(stderr, COLOR_LIGHT_CYAN
		"Hardware decoder not found. Please chose one of.\n");
		print_suported_hw_devices_for_decoder(stderr, decoder);
		puts(COLOR_RESET "\n");
		return -1;
	}
	if (get_supported_hwdecoder(type, decoder) == NULL) {
		fprintf(stderr, "Decoder %s does not support %s.\n", av_hwdevice_get_type_name(type), decoder->name);
		return -1;
	}
	return type;
}

static enum AVPixelFormat get_hw_format(AVCodecContext *ctx, const enum AVPixelFormat *pix_fmts) {
	frame_user_data_t *frame_data = (frame_user_data_t*)ctx->opaque;
	const enum AVPixelFormat *p;
	(void)ctx;

	for (p = pix_fmts; *p != -1; p++) {
		if (*p == frame_data->hw_pix_fmt)
			return *p;
	}
	// we can change to sw format
	// Pixel format not supported in HW
	fprintf(stderr, "Failed to get HW surface format.\n");
	return AV_PIX_FMT_NONE;
}

static int decode_frames(AVCodecContext *avctx, AVPacket *packet, enum AVHWDeviceType type,
			 receive_frame_callback callback) {
	frame_user_data_t *frame_data = (frame_user_data_t*)avctx->opaque;
	AVFrame *sw_frame = frame_data->sw_frame, *frame = frame_data->frame;
	int ret = 0;

	ret = avcodec_send_packet(avctx, packet);
	if (ret < 0) {
		fprintf(stderr, "Error during decoding\n");
		return ret;
	}

	while (1) {
		ret = avcodec_receive_frame(avctx, frame);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
			return 0;
		} else if (ret < 0) {
			fprintf(stderr, "Error while decoding\n");
			return ret;
		}

		if (frame->format == frame_data->hw_pix_fmt && type != AV_HWDEVICE_TYPE_NONE) {
			if ((ret = av_hwframe_transfer_data(sw_frame, frame, 0)) < 0) {
				fprintf(stderr, "Error transferring the data to system memory\n");
				return ret;
			}
			frame_data->frame = sw_frame;
		}

		if (callback)
			ret = (*callback)(avctx);
	}
}
