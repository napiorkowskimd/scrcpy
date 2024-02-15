#ifndef FORMAT_CONVERTER_
#define FORMAT_CONVERTER_

#include <stdbool.h>

#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>

struct sc_format_converter {
    enum AVPixelFormat src_format;
    struct SwsContext *ctx;
    AVFrame *dst_frame;
};

bool sc_format_converter_init(struct sc_format_converter* converter, int width, int height,
    enum AVPixelFormat src_format, enum AVPixelFormat dst_format);
void sc_format_converter_reset(struct sc_format_converter* converter);
bool sc_format_converter_ready(struct sc_format_converter* converter, const AVFrame *src);
AVFrame *sc_format_converter_convert(struct sc_format_converter* converter, const AVFrame *src);

#endif // FORMAT_CONVERTER_
