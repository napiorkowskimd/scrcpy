#ifndef SC_PACKET_DEMUXER_H
#define SC_PACKET_DEMUXER_H

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <stdbool.h>
#include <stdint.h>

#include "trait/demuxer.h"
#include "util/net.h"

struct sc_packet_demuxer {
    struct sc_demuxer demuxer;
    sc_socket socket;
};

// The name must be statically allocated (e.g. a string literal)
void
sc_packet_demuxer_init(struct sc_packet_demuxer *demuxer, const char *name,
                       sc_socket socket, const struct sc_demuxer_callbacks *cbs,
                       void *cbs_userdata);

enum sc_demuxer_status
sc_packet_demuxer_configure(struct sc_demuxer *demuxer, AVCodecContext **out_context);

bool
sc_packet_demuxer_get_frame(struct sc_demuxer *demuxer, AVPacket *packet);

#endif
