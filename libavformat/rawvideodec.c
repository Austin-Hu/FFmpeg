/*
 * RAW video demuxer
 * Copyright (c) 2001 Fabrice Bellard
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "libavutil/imgutils.h"
#include "libavutil/parseutils.h"
#include "libavutil/pixdesc.h"
#include "libavutil/opt.h"
#include "internal.h"
#include "avformat.h"
#include "url.h"

typedef struct RawVideoDemuxerContext {
    const AVClass *class;     /**< Class for private options. */
    int width, height;        /**< Integers describing video size, set by a private option. */
    char *pixel_format;       /**< Set by a private option. */
    AVRational framerate;     /**< AVRational describing framerate, set by a private option. */
} RawVideoDemuxerContext;

typedef struct AVIOInternal {
    URLContext *h;
} AVIOInternal;

typedef struct {
    enum AVPixelFormat pix_fmt;
    unsigned int size_x;
    unsigned int size_y;
    unsigned long long image_ptr;
    unsigned int magic;
    int (*vsbuf_parse_metadata)(char **key, char **value, int size);
} VSBufContext;

static int rawvideo_read_header(AVFormatContext *ctx)
{
    RawVideoDemuxerContext *s = ctx->priv_data;
    enum AVPixelFormat pix_fmt;
    AVStream *st;
    int packet_size;

    st = avformat_new_stream(ctx, NULL);
    if (!st)
        return AVERROR(ENOMEM);

    st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;

    st->codecpar->codec_id = ctx->iformat->raw_codec_id;

    if ((pix_fmt = av_get_pix_fmt(s->pixel_format)) == AV_PIX_FMT_NONE) {
        av_log(ctx, AV_LOG_ERROR, "No such pixel format: %s.\n",
               s->pixel_format);
        return AVERROR(EINVAL);
    }

    avpriv_set_pts_info(st, 64, s->framerate.den, s->framerate.num);

    st->codecpar->width  = s->width;
    st->codecpar->height = s->height;
    st->codecpar->format = pix_fmt;
    packet_size = av_image_get_buffer_size(st->codecpar->format, s->width, s->height, 1);
    if (packet_size < 0)
        return packet_size;
    ctx->packet_size = packet_size;
    st->codecpar->bit_rate = av_rescale_q(ctx->packet_size,
                                       (AVRational){8,1}, st->time_base);

    return 0;
}


static int rawvideo_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    int ret;
    int i = 0, count = 0;
    static int last_inserted_cnt = 0;
    char *key[100], *val[100];
    AVIOInternal *internal = s->pb->opaque;
    VSBufContext *vc = ((URLContext*)internal->h)->priv_data;

    ret = av_get_packet(s->pb, pkt, s->packet_size);
    pkt->pts = pkt->dts = pkt->pos / s->packet_size;

    if (vc->magic == 0x4EAC812B && vc->vsbuf_parse_metadata) {
        // FIXME: This is a workaround to remove the appended
        // key/values of AVFormatContext, otherwise the metadata
        // of AVFormatContext would increase frame by frame.
        //
        // And the correct way should be adding the fetched
        // metadata into the side data of AVPacket, and
        // transmitting to the metadata of AVFrame later.
        if (last_inserted_cnt > 0 && s->metadata)
            av_dict_partial_free(s->metadata, &last_inserted_cnt);

        count = vc->vsbuf_parse_metadata(key, val, 100);
        for (;i < count;i++)
            av_dict_set(&s->metadata, key[i], val[i], 0);

        last_inserted_cnt = count;
    }
    pkt->stream_index = 0;
    if (ret < 0)
        return ret;
    return 0;
}

#define OFFSET(x) offsetof(RawVideoDemuxerContext, x)
#define DEC AV_OPT_FLAG_DECODING_PARAM
static const AVOption rawvideo_options[] = {
    { "video_size", "set frame size", OFFSET(width), AV_OPT_TYPE_IMAGE_SIZE, {.str = NULL}, 0, 0, DEC },
    { "pixel_format", "set pixel format", OFFSET(pixel_format), AV_OPT_TYPE_STRING, {.str = "yuv420p"}, 0, 0, DEC },
    { "framerate", "set frame rate", OFFSET(framerate), AV_OPT_TYPE_VIDEO_RATE, {.str = "25"}, 0, INT_MAX, DEC },
    { NULL },
};

static const AVClass rawvideo_demuxer_class = {
    .class_name = "rawvideo demuxer",
    .item_name  = av_default_item_name,
    .option     = rawvideo_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

AVInputFormat ff_rawvideo_demuxer = {
    .name           = "rawvideo",
    .long_name      = NULL_IF_CONFIG_SMALL("raw video"),
    .priv_data_size = sizeof(RawVideoDemuxerContext),
    .read_header    = rawvideo_read_header,
    .read_packet    = rawvideo_read_packet,
    .flags          = AVFMT_GENERIC_INDEX,
    .extensions     = "yuv,cif,qcif,rgb",
    .raw_codec_id   = AV_CODEC_ID_RAWVIDEO,
    .priv_class     = &rawvideo_demuxer_class,
};
