/*
* Scalable Video Technology for HEVC encoder library plugin
*
* Copyright (c) 2019 Intel Corporation
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
* License along with this program; if not, write to the Free Software
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
*/

#include "EbErrorCodes.h"
#include "EbTime.h"
#include "EbApi.h"

#include "libavutil/common.h"
#include "libavutil/frame.h"
#include "libavutil/opt.h"
#include "libavutil/list.h"

#include "internal.h"
#include "avcodec.h"

typedef enum eos_status {
    EOS_NOT_REACHED = 0,
    EOS_SENT,
    EOS_RECEIVED
}EOS_STATUS;

typedef struct SvtContext {
    AVClass *class;

    EB_H265_ENC_CONFIGURATION enc_params;
    EB_COMPONENTTYPE *svt_handle;
    EB_BUFFERHEADERTYPE in_buf;
    EOS_STATUS eos_flag;

    // User options.
    int profile;
    int hierarchical_level;
    int enc_mode;
    int tier;
    int level;
    int rc_mode;
    int scd;
    int tune;
    int base_layer_switch_mode;
    int qp;
    int aud;
    int asm_type;
    int forced_idr;
    int la_depth;

    struct list_head list; // To store the metadata of each input AVFrame.
} SvtContext;

static int error_mapping(EB_ERRORTYPE svt_ret)
{
    switch (svt_ret) {
    case EB_ErrorInsufficientResources:
        return AVERROR(ENOMEM);

    case EB_ErrorUndefined:
    case EB_ErrorInvalidComponent:
    case EB_ErrorBadParameter:
        return AVERROR(EINVAL);

    case EB_ErrorDestroyThreadFailed:
    case EB_ErrorSemaphoreUnresponsive:
    case EB_ErrorDestroySemaphoreFailed:
    case EB_ErrorCreateMutexFailed:
    case EB_ErrorMutexUnresponsive:
    case EB_ErrorDestroyMutexFailed:
        return AVERROR_EXTERNAL;

    case EB_NoErrorEmptyQueue:
        return AVERROR(EAGAIN);

    case EB_ErrorNone:
        return 0;

    default:
        return AVERROR_UNKNOWN;
    }
}

static void free_buffer(SvtContext *svt_enc)
{
    uint8_t *in_data = svt_enc->in_buf.pBuffer;

    av_freep(&in_data);
}

static EB_ERRORTYPE alloc_buffer(SvtContext *svt_enc)
{
    EB_BUFFERHEADERTYPE *in_buf = &svt_enc->in_buf;
    EB_H265_ENC_INPUT *in_data = NULL;

    memset(in_buf, 0, sizeof(*in_buf));
    in_buf->nSize = sizeof(*in_buf);
    in_buf->sliceType = EB_INVALID_PICTURE;

    in_data = (EB_H265_ENC_INPUT *)av_mallocz(sizeof(*in_data));
    if (in_data) {
        in_buf->pBuffer = (uint8_t *)in_data;
        return EB_ErrorNone;
    } else {
        return EB_ErrorInsufficientResources;
    }
}

static int config_enc_params(EB_H265_ENC_CONFIGURATION *param,
                             AVCodecContext *avctx)
{
    SvtContext *svt_enc = avctx->priv_data;

    param->sourceWidth = avctx->width;
    param->sourceHeight = avctx->height;

    if ((avctx->pix_fmt == AV_PIX_FMT_YUV420P10) ||
        (avctx->pix_fmt == AV_PIX_FMT_YUV422P10) ||
        (avctx->pix_fmt == AV_PIX_FMT_YUV444P10)) {
        av_log(avctx, AV_LOG_DEBUG, "Set 10 bits depth input\n");
        param->encoderBitDepth = 10;
    } else {
        av_log(avctx, AV_LOG_DEBUG, "Set 8 bits depth input\n");
        param->encoderBitDepth = 8;
    }

    if ((avctx->pix_fmt == AV_PIX_FMT_YUV420P) ||
        (avctx->pix_fmt == AV_PIX_FMT_YUV420P10))
        param->encoderColorFormat = EB_YUV420;
    else if ((avctx->pix_fmt == AV_PIX_FMT_YUV422P) ||
             (avctx->pix_fmt == AV_PIX_FMT_YUV422P10))
        param->encoderColorFormat = EB_YUV422;
    else
        param->encoderColorFormat = EB_YUV444;

    param->profile = svt_enc->profile;

    if (FF_PROFILE_HEVC_MAIN_STILL_PICTURE == param->profile) {
        av_log(avctx, AV_LOG_ERROR, "Main Still Picture Profile not supported\n");
        return EB_ErrorBadParameter;
    }

    if ((param->encoderColorFormat >= EB_YUV422) &&
        (param->profile != FF_PROFILE_HEVC_REXT)) {
        av_log(avctx, AV_LOG_WARNING, "Rext Profile forced for 422 or 444\n");
        param->profile = FF_PROFILE_HEVC_REXT;
    }

    if ((FF_PROFILE_HEVC_MAIN == param->profile) &&
        (param->encoderBitDepth > 8)) {
        av_log(avctx, AV_LOG_WARNING, "Main10 Profile forced for 10 bits\n");
        param->profile = FF_PROFILE_HEVC_MAIN_10;
    }

    param->targetBitRate = avctx->bit_rate;

    if (avctx->gop_size > 0)
        param->intraPeriodLength = avctx->gop_size - 1;

    if ((avctx->framerate.num > 0) && (avctx->framerate.den > 0)) {
        param->frameRateNumerator = avctx->framerate.num;
        param->frameRateDenominator =
            avctx->framerate.den * avctx->ticks_per_frame;
    } else {
        param->frameRateNumerator = avctx->time_base.den;
        param->frameRateDenominator =
            avctx->time_base.num * avctx->ticks_per_frame;
    }

    if (param->rateControlMode) {
        param->maxQpAllowed = avctx->qmax;
        param->minQpAllowed = avctx->qmin;
    }

    param->hierarchicalLevels = svt_enc->hierarchical_level;
    param->encMode = svt_enc->enc_mode;
    param->tier = svt_enc->tier;
    param->level = svt_enc->level;
    param->rateControlMode = svt_enc->rc_mode;
    param->sceneChangeDetection = svt_enc->scd;
    param->tune = svt_enc->tune;
    param->baseLayerSwitchMode = svt_enc->base_layer_switch_mode;
    param->qp = svt_enc->qp;
    param->accessUnitDelimiter = svt_enc->aud;
    param->asmType = svt_enc->asm_type;

    param->intraRefreshType =  svt_enc->forced_idr + 1;

    if (svt_enc->la_depth != -1)
        param->lookAheadDistance = svt_enc->la_depth;

    if (avctx->flags & AV_CODEC_FLAG_GLOBAL_HEADER)
        param->codeVpsSpsPps = 0;
    else
        param->codeVpsSpsPps = 1;

    param->codeEosNal = 1;

    return EB_ErrorNone;
}

static void read_in_data(EB_H265_ENC_CONFIGURATION *config,
                         const AVFrame *frame,
                         EB_BUFFERHEADERTYPE *header_ptr)
{
    uint8_t is16bit;
    uint64_t frame_size;
    EB_H265_ENC_INPUT *in_data = (EB_H265_ENC_INPUT *)header_ptr->pBuffer;

    is16bit = config->encoderBitDepth > 8;
    frame_size = (uint64_t)(config->sourceWidth * config->sourceHeight) << is16bit;

    in_data->luma = frame->data[0];
    in_data->cb = frame->data[1];
    in_data->cr = frame->data[2];

    in_data->yStride = frame->linesize[0] >> is16bit;
    in_data->cbStride = frame->linesize[1] >> is16bit;
    in_data->crStride = frame->linesize[2] >> is16bit;

    if (config->encoderColorFormat == EB_YUV420)
        frame_size *= 3/2u;
    else if (config->encoderColorFormat == EB_YUV422)
        frame_size *= 2u;
    else
        frame_size *= 3u;

    header_ptr->nFilledLen += frame_size;
}

typedef struct _EB_FRAMEMETADATA
{
    int64_t pts;
    uint8_t *metadata;
    int size;
    struct list_head list;
} EB_FRAMEMETADATA;

static av_cold int eb_enc_init(AVCodecContext *avctx)
{
    SvtContext *svt_enc = avctx->priv_data;
    EB_ERRORTYPE svt_ret;

    svt_enc->eos_flag = EOS_NOT_REACHED;

    svt_ret = EbInitHandle(&svt_enc->svt_handle, svt_enc, &svt_enc->enc_params);
    if (svt_ret != EB_ErrorNone) {
        av_log(avctx, AV_LOG_ERROR, "Failed to init handle\n");
        return error_mapping(svt_ret);
    }

    svt_ret = config_enc_params(&svt_enc->enc_params, avctx);
    if (svt_ret != EB_ErrorNone) {
        av_log(avctx, AV_LOG_ERROR, "Failed to config parameters\n");
        goto failed_init_handle;
    }

    svt_ret = EbH265EncSetParameter(svt_enc->svt_handle, &svt_enc->enc_params);
    if (svt_ret != EB_ErrorNone) {
        av_log(avctx, AV_LOG_ERROR, "Failed to set parameters\n");
        goto failed_init_handle;
    }

    svt_ret = EbInitEncoder(svt_enc->svt_handle);
    if (svt_ret != EB_ErrorNone) {
        av_log(avctx, AV_LOG_ERROR, "Failed to init encoder\n");
        goto failed_init_handle;
    }

    if (avctx->flags & AV_CODEC_FLAG_GLOBAL_HEADER) {
        EB_BUFFERHEADERTYPE *header_ptr = NULL;

        svt_ret = EbH265EncStreamHeader(svt_enc->svt_handle, &header_ptr);
        if (svt_ret != EB_ErrorNone) {
            av_log(avctx, AV_LOG_ERROR, "Failed to build stream header\n");
            goto failed_init_encoder;
        }

        avctx->extradata_size = header_ptr->nFilledLen;
        avctx->extradata = av_malloc(avctx->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!avctx->extradata) {
            av_log(avctx, AV_LOG_ERROR, "Failed to allocate extradata\n");
            svt_ret = EB_ErrorInsufficientResources;
            goto failed_init_encoder;
        }
        memcpy(avctx->extradata, header_ptr->pBuffer, avctx->extradata_size);
        memset((void *)(avctx->extradata+avctx->extradata_size), 0, AV_INPUT_BUFFER_PADDING_SIZE);
    }

    svt_ret = alloc_buffer(svt_enc);
    if (svt_ret != EB_ErrorNone) {
        av_log(avctx, AV_LOG_ERROR, "Failed to alloc data buffer\n");
        goto failed_init_encoder;
    }

    INIT_LIST_HEAD(&svt_enc->list);
    return 0;

failed_init_encoder:
    EbDeinitEncoder(svt_enc->svt_handle);
failed_init_handle:
    EbDeinitHandle(svt_enc->svt_handle);
    svt_enc->svt_handle = NULL;
    svt_enc = NULL;
    return error_mapping(svt_ret);
}

static int eb_encode_frame(AVCodecContext *avctx, AVPacket *pkt,
                           const AVFrame *frame, int *got_packet)
{
    SvtContext *svt_enc = avctx->priv_data;
    EB_BUFFERHEADERTYPE *header_ptr = &svt_enc->in_buf;
    EB_ERRORTYPE svt_ret;
    int av_ret;
    int metadata_size = 0;
    uint8_t *side_data = NULL;
    struct list_head *pos, *n;
    EB_FRAMEMETADATA *p;

    if (EOS_RECEIVED == svt_enc->eos_flag) {
        *got_packet = 0;
        return 0;
    }

    if (!frame) {
        if (!svt_enc->eos_flag) {
            svt_enc->eos_flag = EOS_SENT;

            header_ptr->nAllocLen = 0;
            header_ptr->nFilledLen = 0;
            header_ptr->nTickCount = 0;
            header_ptr->pBuffer = NULL;
            header_ptr->nFlags = EB_BUFFERFLAG_EOS;

            EbH265EncSendPicture(svt_enc->svt_handle, header_ptr);

            av_log(avctx, AV_LOG_DEBUG, "Sent EOS\n");
        }
    } else {
        read_in_data(&svt_enc->enc_params, frame, header_ptr);
        header_ptr->pts = frame->pts;

        EbH265EncSendPicture(svt_enc->svt_handle, header_ptr);

        av_log(avctx, AV_LOG_DEBUG, "Sent PTS %"PRId64"\n", header_ptr->pts);

        if (frame->metadata) {
            list_for_each_safe(pos, n, &svt_enc->list) {
                p = list_entry(pos, EB_FRAMEMETADATA, list);
                if (frame->pts == p->pts)
                    return 0;
            }

            // Added the entry.
            p = av_mallocz(sizeof(EB_FRAMEMETADATA));
            p->metadata = av_packet_pack_dictionary(frame->metadata, &metadata_size);
            p->size = metadata_size;
            p->pts = frame->pts;
            list_add_tail(&p->list, &svt_enc->list);
        }
    }

    header_ptr = NULL;
    svt_ret = EbH265GetPacket(svt_enc->svt_handle, &header_ptr, svt_enc->eos_flag);

    if (svt_ret == EB_NoErrorEmptyQueue) {
        *got_packet = 0;
        av_log(avctx, AV_LOG_DEBUG, "Received none\n");
        return 0;
    }

    av_log(avctx, AV_LOG_DEBUG, "Received PTS %"PRId64" packet\n", header_ptr->pts);

    av_ret = ff_alloc_packet2(avctx, pkt, header_ptr->nFilledLen, 0);
    if (av_ret) {
        av_log(avctx, AV_LOG_ERROR, "Failed to allocate a packet\n");
        EbH265ReleaseOutBuffer(&header_ptr);
        return av_ret;
    }

    memcpy(pkt->data, header_ptr->pBuffer, header_ptr->nFilledLen);
    pkt->size = header_ptr->nFilledLen;
    pkt->pts  = header_ptr->pts;
    pkt->dts  = header_ptr->dts;

    if ((header_ptr->sliceType == EB_IDR_PICTURE) ||
        (header_ptr->sliceType == EB_I_PICTURE))
        pkt->flags |= AV_PKT_FLAG_KEY;
    if (header_ptr->sliceType == EB_NON_REF_PICTURE)
        pkt->flags |= AV_PKT_FLAG_DISPOSABLE;

    // Copy the backup metadata of AVFrame into the side data of corresponding
    // AVPacket which has the same pts, for the per-frame Metadata feature.
    // Here we've to use the doubly linked list (copied from kernel), because
    // The AVFrames are feeded into encoder sequently (PTS), but AVPackets would
    // be out of order.
    list_for_each_safe(pos, n, &svt_enc->list) {
        p = list_entry(pos, EB_FRAMEMETADATA, list);
        if (pkt->pts == p->pts) {
            if (p->metadata && p->size > 0) {
                side_data = av_packet_new_side_data(pkt, AV_PKT_DATA_STRINGS_METADATA, p->size);
                memcpy(side_data, p->metadata, p->size);

                av_freep(&p->metadata);
                list_del(&p->list);
                av_freep(&p);
            }
            break;
        }
    }

    EbH265ReleaseOutBuffer(&header_ptr);

    *got_packet = 1;

    if (EB_BUFFERFLAG_EOS == header_ptr->nFlags)
       svt_enc->eos_flag = EOS_RECEIVED;

    return 0;
}

static av_cold int eb_enc_close(AVCodecContext *avctx)
{
    SvtContext *svt_enc = avctx->priv_data;

    if (svt_enc) {
        free_buffer(svt_enc);

        if (svt_enc->svt_handle) {
            EbDeinitEncoder(svt_enc->svt_handle);
            EbDeinitHandle(svt_enc->svt_handle);
            svt_enc->svt_handle = NULL;
        }

        svt_enc = NULL;
    }

    return 0;
}

#define OFFSET(x) offsetof(SvtContext, x)
#define VE AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM
static const AVOption options[] = {
    { "asm_type", "Assembly instruction set type [0: C Only, 1: Auto]", OFFSET(asm_type),
      AV_OPT_TYPE_BOOL, { .i64 = 1 }, 0, 1, VE },

    { "aud", "Include Access Unit Delimiter", OFFSET(aud),
      AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1, VE },

    { "bl_mode", "Random Access Prediction Structure type setting", OFFSET(base_layer_switch_mode),
      AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1, VE },

    { "forced-idr", "If forcing keyframes, force them as IDR frames.", OFFSET(forced_idr),
      AV_OPT_TYPE_BOOL,   { .i64 = 0 }, 0, 1, VE },

    { "hielevel", "Hierarchical prediction levels setting", OFFSET(hierarchical_level),
      AV_OPT_TYPE_INT, { .i64 = 3 }, 0, 3, VE , "hielevel"},
        { "flat",   NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 0 },  INT_MIN, INT_MAX, VE, "hielevel" },
        { "1 level", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 1 },  INT_MIN, INT_MAX, VE, "hielevel" },
        { "2 level", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 2 },  INT_MIN, INT_MAX, VE, "hielevel" },
        { "3 level", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 3 },  INT_MIN, INT_MAX, VE, "hielevel" },

    { "la_depth", "Look ahead distance [0, 256]", OFFSET(la_depth),
      AV_OPT_TYPE_INT, { .i64 = -1 }, -1, 256, VE },

    { "level", "Set level (level_idc)", OFFSET(level),
      AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 0xff, VE, "level" },

    { "preset", "Encoding preset [0, 12]",
      OFFSET(enc_mode), AV_OPT_TYPE_INT, { .i64 = 9 }, 0, 12, VE },

    { "profile", "Profile setting, Main Still Picture Profile not supported", OFFSET(profile),
      AV_OPT_TYPE_INT, { .i64 = FF_PROFILE_HEVC_MAIN }, FF_PROFILE_HEVC_MAIN, FF_PROFILE_HEVC_REXT, VE, "profile"},

    { "qp", "QP value for intra frames", OFFSET(qp),
      AV_OPT_TYPE_INT, { .i64 = 32 }, 0, 51, VE },

    { "rc", "Bit rate control mode", OFFSET(rc_mode),
      AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 1, VE , "rc"},
        { "cqp", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 0 },  INT_MIN, INT_MAX, VE, "rc" },
        { "vbr", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 1 },  INT_MIN, INT_MAX, VE, "rc" },

    { "sc_detection", "Scene change detection", OFFSET(scd),
      AV_OPT_TYPE_BOOL, { .i64 = 1 }, 0, 1, VE },

    { "tier", "Set tier (general_tier_flag)", OFFSET(tier),
      AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 1, VE, "tier" },
        { "main", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 0 }, 0, 0, VE, "tier" },
        { "high", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 1 }, 0, 0, VE, "tier" },

    { "tune", "Quality tuning mode", OFFSET(tune), AV_OPT_TYPE_INT, { .i64 = 1 }, 0, 2, VE, "tune" },
        { "sq", "Visually optimized mode", 0,
          AV_OPT_TYPE_CONST, { .i64 = 0 },  INT_MIN, INT_MAX, VE, "tune" },
        { "oq",  "PSNR / SSIM optimized mode",  0,
          AV_OPT_TYPE_CONST, { .i64 = 1 },  INT_MIN, INT_MAX, VE, "tune" },
        { "vmaf", "VMAF optimized mode", 0,
          AV_OPT_TYPE_CONST, { .i64 = 2 },  INT_MIN, INT_MAX, VE, "tune" },

    {NULL},
};

static const AVClass class = {
    .class_name = "libsvt_hevc",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static const AVCodecDefault eb_enc_defaults[] = {
    { "b",         "7M"    },
    { "flags",     "+cgop" },
    { "qmin",      "10"    },
    { "qmax",      "48"    },
    { "g",         "-2"    },
    { NULL },
};

AVCodec ff_libsvt_hevc_encoder = {
    .name           = "libsvt_hevc",
    .long_name      = NULL_IF_CONFIG_SMALL("SVT-HEVC(Scalable Video Technology for HEVC) encoder"),
    .priv_data_size = sizeof(SvtContext),
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_HEVC,
    .init           = eb_enc_init,
    .encode2        = eb_encode_frame,
    .close          = eb_enc_close,
    .capabilities   = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_AUTO_THREADS,
    .pix_fmts       = (const enum AVPixelFormat[]){ AV_PIX_FMT_YUV420P,
                                                    AV_PIX_FMT_YUV420P10,
                                                    AV_PIX_FMT_YUV422P,
                                                    AV_PIX_FMT_YUV420P10,
                                                    AV_PIX_FMT_YUV444P,
                                                    AV_PIX_FMT_YUV444P10,
                                                    AV_PIX_FMT_NONE },
    .priv_class     = &class,
    .defaults       = eb_enc_defaults,
    .caps_internal  = FF_CODEC_CAP_INIT_CLEANUP,
    .wrapper_name   = "libsvt_hevc",
};
