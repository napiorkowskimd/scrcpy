#include "packet_demuxer.h"

#include <assert.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/time.h>
#include <stdbool.h>
#include <unistd.h>

#include "events.h"
#include "trait/demuxer.h"
#include "util/binary.h"
#include "util/log.h"

#define DOWNCAST(SINK) container_of(SINK, struct sc_packet_demuxer, demuxer)

#define SC_PACKET_HEADER_SIZE 12

#define SC_PACKET_FLAG_CONFIG (UINT64_C(1) << 63)
#define SC_PACKET_FLAG_KEY_FRAME (UINT64_C(1) << 62)

#define SC_PACKET_PTS_MASK (SC_PACKET_FLAG_KEY_FRAME - 1)

static enum AVCodecID
sc_packet_demuxer_to_avcodec_id(uint32_t codec_id) {
#define SC_CODEC_ID_H264 UINT32_C(0x68323634) // "h264" in ASCII
#define SC_CODEC_ID_H265 UINT32_C(0x68323635) // "h265" in ASCII
#define SC_CODEC_ID_AV1 UINT32_C(0x00617631) // "av1" in ASCII
#define SC_CODEC_ID_OPUS UINT32_C(0x6f707573) // "opus" in ASCII
#define SC_CODEC_ID_AAC UINT32_C(0x00616163) // "aac" in ASCII
#define SC_CODEC_ID_FLAC UINT32_C(0x666c6163) // "flac" in ASCII
#define SC_CODEC_ID_RAW UINT32_C(0x00726177) // "raw" in ASCII
    switch (codec_id) {
        case SC_CODEC_ID_H264:
            return AV_CODEC_ID_H264;
        case SC_CODEC_ID_H265:
            return AV_CODEC_ID_HEVC;
        case SC_CODEC_ID_AV1:
#ifdef SCRCPY_LAVC_HAS_AV1
            return AV_CODEC_ID_AV1;
#else
            LOGE("AV1 not supported by this FFmpeg version");
            return AV_CODEC_ID_NONE;
#endif
        case SC_CODEC_ID_OPUS:
            return AV_CODEC_ID_OPUS;
        case SC_CODEC_ID_AAC:
            return AV_CODEC_ID_AAC;
        case SC_CODEC_ID_FLAC:
            return AV_CODEC_ID_FLAC;
        case SC_CODEC_ID_RAW:
            return AV_CODEC_ID_PCM_S16LE;
        default:
            LOGE("Unknown codec id 0x%08" PRIx32, codec_id);
            return AV_CODEC_ID_NONE;
    }
}

static bool
sc_packet_demuxer_recv_codec_id(struct sc_packet_demuxer *demuxer,
                                uint32_t *codec_id) {
    uint8_t data[4];
    ssize_t r = net_recv_all(demuxer->socket, data, 4);
    if (r < 4) {
        return false;
    }

    *codec_id = sc_read32be(data);
    return true;
}

static bool
sc_packet_demuxer_recv_video_size(struct sc_packet_demuxer *demuxer,
                                  uint32_t *width, uint32_t *height) {
    uint8_t data[8];
    ssize_t r = net_recv_all(demuxer->socket, data, 8);
    if (r < 8) {
        return false;
    }

    *width = sc_read32be(data);
    *height = sc_read32be(data + 4);
    return true;
}

bool
sc_packet_demuxer_get_frame(struct sc_demuxer *base_demuxer,
                            AVPacket *packet) {
    // The video and audio streams contain a sequence of raw packets (as
    // provided by MediaCodec), each prefixed with a "meta" header.
    //
    // The "meta" header length is 12 bytes:
    // [. . . . . . . .|. . . .]. . . . . . . . . . . . . . . ...
    //  <-------------> <-----> <-----------------------------...
    //        PTS        packet        raw packet
    //                    size
    //
    // It is followed by <packet_size> bytes containing the packet/frame.
    //
    // The most significant bits of the PTS are used for packet flags:
    //
    //  byte 7   byte 6   byte 5   byte 4   byte 3   byte 2   byte 1   byte 0
    // CK...... ........ ........ ........ ........ ........ ........ ........
    // ^^<------------------------------------------------------------------->
    // ||                                PTS
    // | `- key frame
    //  `-- config packet
    struct sc_packet_demuxer *demuxer = DOWNCAST(base_demuxer);
    uint8_t header[SC_PACKET_HEADER_SIZE];
    ssize_t r = net_recv_all(demuxer->socket, header, SC_PACKET_HEADER_SIZE);
    if (r < SC_PACKET_HEADER_SIZE) {
        return false;
    }

    uint64_t pts_flags = sc_read64be(header);
    uint32_t len = sc_read32be(&header[8]);
    assert(len);

    if (av_new_packet(packet, len)) {
        LOG_OOM();
        return false;
    }

    r = net_recv_all(demuxer->socket, packet->data, len);
    if (r < 0 || ((uint32_t) r) < len) {
        av_packet_unref(packet);
        return false;
    }

    if (pts_flags & SC_PACKET_FLAG_CONFIG) {
        packet->pts = AV_NOPTS_VALUE;
    } else {
        packet->pts = pts_flags & SC_PACKET_PTS_MASK;
    }

    if (pts_flags & SC_PACKET_FLAG_KEY_FRAME) {
        packet->flags |= AV_PKT_FLAG_KEY;
    }

    packet->dts = packet->pts;
    return true;
}

enum sc_demuxer_status
sc_packet_demuxer_configure(struct sc_demuxer *demuxer, AVCodecContext **out_context) {
    struct sc_packet_demuxer *packet_demuxer = DOWNCAST(demuxer);
    enum sc_demuxer_status status = SC_DEMUXER_STATUS_ERROR;
    *out_context = NULL;

    uint32_t raw_codec_id;
    bool ok = sc_packet_demuxer_recv_codec_id(packet_demuxer, &raw_codec_id);
    if (!ok) {
        LOGE("Demuxer '%s': stream disabled due to connection error",
             demuxer->name);
        goto end;
    }

    if (raw_codec_id == 0) {
        LOGW("Demuxer '%s': stream explicitly disabled by the device",
             demuxer->name);
        sc_packet_source_sinks_disable(&demuxer->packet_source);
        status = SC_DEMUXER_STATUS_DISABLED;
        goto end;
    }

    if (raw_codec_id == 1) {
        LOGE("Demuxer '%s': stream configuration error on the device",
             demuxer->name);
        goto end;
    }

    enum AVCodecID codec_id = sc_packet_demuxer_to_avcodec_id(raw_codec_id);
    if (codec_id == AV_CODEC_ID_NONE) {
        LOGE("Demuxer '%s': stream disabled due to unsupported codec",
             demuxer->name);
        sc_packet_source_sinks_disable(&packet_demuxer->demuxer.packet_source);
        goto end;
    }

    const AVCodec *codec = avcodec_find_decoder(codec_id);
    if (!codec) {
        LOGE("Demuxer '%s': stream disabled due to missing decoder",
             demuxer->name);
        sc_packet_source_sinks_disable(&packet_demuxer->demuxer.packet_source);
        goto end;
    }

    AVCodecContext *codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx) {
        LOG_OOM();
        goto end;
    }

    codec_ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;

    if (codec->type == AVMEDIA_TYPE_VIDEO) {
        uint32_t width;
        uint32_t height;
        ok = sc_packet_demuxer_recv_video_size(packet_demuxer, &width, &height);
        if (!ok) {
            goto error_free_context;
        }

        codec_ctx->width = width;
        codec_ctx->height = height;
        codec_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    } else {
        // Hardcoded audio properties
#ifdef SCRCPY_LAVU_HAS_CHLAYOUT
        codec_ctx->ch_layout = (AVChannelLayout) AV_CHANNEL_LAYOUT_STEREO;
#else
        codec_ctx->channel_layout = AV_CH_LAYOUT_STEREO;
        codec_ctx->channels = 2;
#endif
        codec_ctx->sample_rate = 48000;

        if (raw_codec_id == SC_CODEC_ID_FLAC) {
            // The sample_fmt is not set by the FLAC decoder
            codec_ctx->sample_fmt = AV_SAMPLE_FMT_S16;
        }
    }

    if (avcodec_open2(codec_ctx, codec, NULL) < 0) {
        LOGE("Demuxer '%s': could not open codec", demuxer->name);
        goto error_free_context;
    }

    status = SC_DEMUXER_STATUS_OK;
    *out_context = codec_ctx;
    return status;

error_free_context:
    // This also calls avcodec_close() internally
    avcodec_free_context(&codec_ctx);
end:
    return status;
}


void
sc_packet_demuxer_init(struct sc_packet_demuxer *packet_demuxer,
                       const char *name, sc_socket socket,
                       const struct sc_demuxer_callbacks *cbs,
                       void *cbs_userdata) {
    assert(socket != SC_SOCKET_NONE);

    static const struct sc_demuxer_ops ops = { .configure = sc_packet_demuxer_configure,
                                               .get_frame = sc_packet_demuxer_get_frame };
    packet_demuxer->demuxer.ops = &ops;
    packet_demuxer->demuxer.name = name; // statically allocated
    sc_packet_source_init(&packet_demuxer->demuxer.packet_source);
    assert(cbs && cbs->on_ended);
    packet_demuxer->demuxer.cbs = cbs;
    packet_demuxer->demuxer.cbs_userdata = cbs_userdata;

    packet_demuxer->socket = socket;
}
