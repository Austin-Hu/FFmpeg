/*
 * Copyright (c) 2019 Intel Corp.
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

/**
 * @file
 * A video filter which can get the mask of detected person(s), with any segment
 * module based on Mask RCNN:
 * (https://github.com/matterport/Mask_RCNN)
 * (https://docs.openvinotoolkit.org/latest/_inference_engine_samples_segmentation_demo_README.html)
 * (FancyVideo from Shanghai Flex team)
 */

#include "libavutil/imgutils.h"
#include "libavutil/internal.h"
#include "libavutil/frame.h"
#include "avfilter.h"
#include "internal.h"
#include "video.h"

// Note: person_id needs bgr24 input
#if CONFIG_FANCYVIDEO
#define VF_SEG_INPUT_FORMAT "bgr24"
#else
#define VF_SEG_INPUT_FORMAT "uyvy422"
#endif

#define UNUSED(x) (void)(x)

typedef struct SEGContext {
} SEGContext;

static int seg_init(AVFilterContext *ctx)
{
    SEGContext *seg_ctx = ctx->priv;

    UNUSED(seg_ctx);
    return 0;
}

static void seg_uninit(AVFilterContext *ctx)
{
    SEGContext *seg_ctx = ctx->priv;

    UNUSED(seg_ctx);
}

static int query_formats(AVFilterContext *ctx)
{
    AVFilterFormats *formats = NULL;
    int fmt;
    const AVPixFmtDescriptor *desc = NULL;
    int ret = 0;

    for (fmt = 0; av_pix_fmt_desc_get(fmt); fmt++) {
        desc = av_pix_fmt_desc_get(fmt);
        if (strcmp(desc->name, VF_SEG_INPUT_FORMAT))
            continue;

        if ((ret = ff_add_format(&formats, fmt)) < 0)
            return ret;
    }

    return ff_set_common_formats(ctx, formats);
}

static int filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    AVFilterContext *ctx = NULL;
    SEGContext *seg_ctx = NULL;

    if (!inlink || !in)
        return AVERROR(EINVAL);

    ctx = inlink->dst;
    seg_ctx = inlink->dst->priv;

roi_end:
    return ff_filter_frame(inlink->dst->outputs[0], in);
}

static const AVFilterPad avfilter_vf_seg_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = filter_frame,
    },
    { NULL }
};

static const AVFilterPad avfilter_vf_seg_outputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
    },
    { NULL }
};

AVFilter ff_vf_seg = {
    .name          = "seg",
    .description   = NULL_IF_CONFIG_SMALL("Get the mask of detected person(s) from input frame."),
    .inputs        = avfilter_vf_seg_inputs,
    .outputs       = avfilter_vf_seg_outputs,
    .init          = seg_init,
    .uninit        = seg_uninit,
    .query_formats = query_formats,
    .priv_size     = sizeof(SEGContext),
};
