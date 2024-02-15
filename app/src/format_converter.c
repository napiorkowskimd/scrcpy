#include "format_converter.h"
#include <libavutil/frame.h>
#include <libswscale/swscale.h>

bool sc_format_converter_init(struct sc_format_converter *converter, int width,
                              int height, enum AVPixelFormat src_format,
                              enum AVPixelFormat dst_format) {
  converter->ctx = sws_getContext(width, height, src_format, width, height,
                                  dst_format, SWS_BITEXACT, NULL, NULL, NULL);
  if (converter->ctx == NULL) {
    return false;
  }
  converter->dst_frame = av_frame_alloc();
  converter->dst_frame->format = dst_format;
  converter->dst_frame->width = width;
  converter->dst_frame->height = height;
  converter->src_format = src_format;
  if (0 != av_frame_get_buffer(converter->dst_frame, 0)) {
    return false;
  }
  return true;
}

void sc_format_converter_reset(struct sc_format_converter *converter) {
  if (converter->dst_frame) {
    av_frame_free(&converter->dst_frame);
  }
  if (converter->ctx) {
    sws_freeContext(converter->ctx);
  }
}

bool sc_format_converter_ready(struct sc_format_converter *converter,
                               const AVFrame *src) {
  if (converter->ctx == NULL) {
    return false;
  }

  if (src->width != converter->dst_frame->width ||
      src->height != converter->dst_frame->height) {
    return false;
  }

  if (src->format != converter->src_format) {
    return false;
  }
  return true;
}

AVFrame *sc_format_converter_convert(struct sc_format_converter *converter,
                                     const AVFrame *src) {
  sws_scale(converter->ctx, (const uint8_t *const *)src->data, src->linesize, 0, src->height,
            converter->dst_frame->data, converter->dst_frame->linesize);
  return converter->dst_frame;
}