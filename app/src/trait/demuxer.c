#include "demuxer.h"

#include "util/log.h"
#include "packet_merger.h"
#include "util/thread.h"


static int
run_demuxer(void *data) {
    struct sc_demuxer *demuxer = data;

    // Flag to report end-of-stream (i.e. device disconnected)
    AVCodecContext *codec_ctx;
    enum sc_demuxer_status status = demuxer->ops->configure(demuxer, &codec_ctx);

    if (status != SC_DEMUXER_STATUS_OK) {
        goto end;
    }

    if (!sc_packet_source_sinks_open(&demuxer->packet_source,
                                     codec_ctx)) {
        goto finally_free_context;
    }

    // Config packets must be merged with the next non-config packet only for
    // H.26x
    bool must_merge_config_packet =
        codec_ctx->codec_id == AV_CODEC_ID_H264 || codec_ctx->codec_id == AV_CODEC_ID_H265;

    struct sc_packet_merger merger;

    if (must_merge_config_packet) {
        sc_packet_merger_init(&merger);
    }

    AVPacket *packet = av_packet_alloc();
    if (!packet) {
        LOG_OOM();
        goto finally_close_sinks;
    }

    for (;;) {
        bool ok = demuxer->ops->get_frame(demuxer, packet);
        if (!ok || demuxer->stop) {
            // end of stream
            status = SC_DEMUXER_STATUS_EOS;
            break;
        }

        if (must_merge_config_packet) {
            // Prepend any config packet to the next media packet
            ok = sc_packet_merger_merge(&merger, packet);
            if (!ok) {
                av_packet_unref(packet);
                break;
            }
        }

        ok = sc_packet_source_sinks_push(&demuxer->packet_source,
                                         packet);
        av_packet_unref(packet);
        if (!ok) {
            // The sink already logged its concrete error
            break;
        }
    }

    LOGD("Demuxer '%s': end of frames", demuxer->name);

    if (must_merge_config_packet) {
        sc_packet_merger_destroy(&merger);
    }

    av_packet_free(&packet);
finally_close_sinks:
    sc_packet_source_sinks_close(&demuxer->packet_source);
finally_free_context:
    // This also calls avcodec_close() internally
    avcodec_free_context(&codec_ctx);
end:
    demuxer->cbs->on_ended(demuxer, status, demuxer->cbs_userdata);

    return 0;
}

bool
sc_demuxer_start(struct sc_demuxer *demuxer) {
    LOGD("Demuxer '%s': starting thread", demuxer->name);

    bool ok = sc_thread_create(&demuxer->thread, run_demuxer,
                               "scrcpy-demuxer", demuxer);
    if (!ok) {
        LOGE("Demuxer '%s': could not start thread", demuxer->name);
        return false;
    }
    return true;
}

void
sc_demuxer_join(struct sc_demuxer *demuxer) {
    demuxer->stop = true;
    sc_thread_join(&demuxer->thread, NULL);
}
