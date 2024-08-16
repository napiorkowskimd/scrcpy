#include "av_demuxer.h"

#include <libavcodec/codec.h>
#include <libavcodec/codec_id.h>
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/opt.h>

#include "common.h"
#include "trait/demuxer.h"
#include "util/log.h"

#define DOWNCAST(SINK) container_of(SINK, struct sc_av_demuxer, demuxer)

void
sc_av_demuxer_init(struct sc_av_demuxer *av_demuxer, const char *filename,
                   const char *av_opts, const struct sc_demuxer_callbacks *cbs,
                   void *cbs_userdata) {
    av_log_set_level(AV_LOG_TRACE);
    static const struct sc_demuxer_ops ops = { .configure = sc_av_demuxer_configure,
                                               .get_frame = sc_av_demuxer_get_frame };

    av_demuxer->demuxer.ops = &ops;
    av_demuxer->demuxer.name = "external_video";
    sc_packet_source_init(&av_demuxer->demuxer.packet_source);
    av_demuxer->demuxer.cbs = cbs;
    av_demuxer->demuxer.cbs_userdata = cbs_userdata;

    av_demuxer->av_opts = av_opts;
    av_demuxer->filename = filename;
}

enum sc_demuxer_status
sc_av_demuxer_configure(struct sc_demuxer *demuxer, AVCodecContext **out_context) {
    struct sc_av_demuxer *av_demuxer = DOWNCAST(demuxer);
    enum sc_demuxer_status status = SC_DEMUXER_STATUS_ERROR;

    avdevice_register_all();

    av_demuxer->format_context = avformat_alloc_context();
    if (!av_demuxer->format_context) {
        LOGE("AVDemuxer: could not create AVFormat context");
        goto end;
    }

    AVDictionary *format_opts = NULL;

    if (0
        != av_dict_parse_string(&format_opts, av_demuxer->av_opts, "=", ",",
                                0)) {
        LOGE("AVDemuxer: could not parse libav parameters");
        goto end;
    }

    if (0
        != avformat_open_input(&av_demuxer->format_context,
                               av_demuxer->filename, NULL, &format_opts)) {
        LOGE("AVDemuxer: could not open uri '%s'", av_demuxer->filename);
        goto end;
    }

    if (avformat_find_stream_info(av_demuxer->format_context, NULL) < 0) {
        LOGE("AVDemuxer: could not get the stream info");
        goto finally_close_input;
    }

    int video_stream_index = -1;
    AVCodecParameters *codec_parameters = NULL;
    for (unsigned int i = 0; i < av_demuxer->format_context->nb_streams; i++) {
        codec_parameters = av_demuxer->format_context->streams[i]->codecpar;

        // when the stream is a video we store its index, codec parameters and
        // codec
        if (codec_parameters->codec_type == AVMEDIA_TYPE_VIDEO) {
            if (video_stream_index == -1) {
                video_stream_index = i;
            }

            LOGD("Video Codec: resolution: %d x %d, codec_id: %d pix_format: %d", codec_parameters->width,
                 codec_parameters->height, codec_parameters->codec_id, codec_parameters->format);
            break;
        }
    }

    if (video_stream_index == -1) {
        LOGD("File %s does not contain a video stream!", av_demuxer->filename);
        goto finally_close_input;
    }

    av_demuxer->stream_index = video_stream_index;

    enum AVCodecID codec_id = codec_parameters->codec_id;
    if (codec_id == AV_CODEC_ID_NONE) {
        LOGE("AVDemuxer: stream disabled due to unsupported codec");
        sc_packet_source_sinks_disable(&av_demuxer->demuxer.packet_source);
        goto finally_close_input;
    }

    const AVCodec *codec = avcodec_find_decoder(codec_id);
    if (!codec) {
        LOGE("AVDemuxer: stream disabled due to missing decoder");
        sc_packet_source_sinks_disable(&av_demuxer->demuxer.packet_source);
        goto finally_close_input;
    }

    AVCodecContext *codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx) {
        LOG_OOM();
        goto finally_close_input;
    }
    avcodec_parameters_to_context(codec_ctx, codec_parameters);
    codec_ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;

    if (avcodec_open2(codec_ctx, codec, NULL) < 0) {
        LOGE("AVDemuxer: could not open codec");
        goto error_free_context;
    }

    status = SC_DEMUXER_STATUS_OK;
    *out_context = codec_ctx;
    return status;

finally_close_input:
    avformat_close_input(&av_demuxer->format_context);
error_free_context:
    // This also calls avcodec_close() internally
    avcodec_free_context(&codec_ctx);
end:
    return status;
}

bool
sc_av_demuxer_get_frame(struct sc_demuxer *demuxer, AVPacket *packet) {
    struct sc_av_demuxer *av_demuxer = DOWNCAST(demuxer);
    while (true) {
        int result = av_read_frame(av_demuxer->format_context, packet);
        if (result < 0) {
            return false;
        }
        if (packet->stream_index == av_demuxer->stream_index) {
            // This is a packet for the selected stream, return it
            return true;
        }
        // Skip packets for other streams
    }
}
