#ifndef DEMUXER_H_
#define DEMUXER_H_

#include <stdatomic.h>

#include "packet_source.h"

#include "util/thread.h"

enum sc_demuxer_status {
    SC_DEMUXER_STATUS_EOS,
    SC_DEMUXER_STATUS_DISABLED,
    SC_DEMUXER_STATUS_OK,
    SC_DEMUXER_STATUS_ERROR,
};

struct sc_demuxer {
    struct sc_packet_source packet_source; // packet source trait
    const struct sc_demuxer_ops *ops;
    
    const struct sc_demuxer_callbacks *cbs;
    void *cbs_userdata;

    const char *name; // must be statically allocated (e.g. a string literal)
    atomic_bool stop;
    sc_thread thread;
};

struct sc_demuxer_ops {
    enum sc_demuxer_status (*configure)(struct sc_demuxer *demuxer, struct AVCodecContext **out_context);
    bool (*get_frame)(struct sc_demuxer *demuxer, struct AVPacket *packet);
};


struct sc_demuxer_callbacks {
    void (*on_ended)(struct sc_demuxer *demuxer, enum sc_demuxer_status,
                     void *userdata);
};

bool
sc_demuxer_start(struct sc_demuxer *demuxer);

void
sc_demuxer_join(struct sc_demuxer *demuxer);

#endif // DEMUXER_H_