#ifndef AV_DEMUXER_
#define AV_DEMUXER_

// Include config so that avformat doesn't redefine _POSIX_C_SOURCE
#include "config.h"

#include <libavformat/avformat.h>

#include "trait/demuxer.h"
#include "util/thread.h"

struct sc_av_demuxer {
    struct sc_demuxer demuxer;

    const char *filename;
    const char *av_opts;
    AVFormatContext *format_context;
    int stream_index;
};

enum sc_demuxer_status
sc_av_demuxer_configure(struct sc_demuxer *demuxer, AVCodecContext **out_context);

bool
sc_av_demuxer_get_frame(struct sc_demuxer *demuxer, AVPacket *packet);
void
sc_av_demuxer_init(struct sc_av_demuxer *av_demuxer, const char *filename,
                   const char *av_opts, const struct sc_demuxer_callbacks *cbs,
                   void *cbs_userdata);

#endif // AV_DEMUXER_