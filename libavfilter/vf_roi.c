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
 * A video filter which can get the ROI(s) of detected person(s), and it would
 * reply on the person_id module
 * (https://github.intel.com/Olympics-Robots/person_id/tree/pure).
 */

#include "libavutil/imgutils.h"
#include "libavutil/internal.h"
#include "libavutil/frame.h"
#include "avfilter.h"
#include "internal.h"
#include "video.h"

// Note: person_id needs bgr8 input
//#define VF_ROI_INPUT_FORMAT "bgr8"
#define VF_ROI_INPUT_FORMAT "uyvy422"

static int roi_init(AVFilterContext *ctx)
{
    return 0;
}

static void roi_uninit(AVFilterContext *ctx)
{
}

static int query_formats(AVFilterContext *ctx)
{
    AVFilterFormats *formats = NULL;
    int fmt;

    for (fmt = 0; av_pix_fmt_desc_get(fmt); fmt++) {
        const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(fmt);
        int ret;
        if (strcmp(desc->name, VF_ROI_INPUT_FORMAT))
            continue;

        if ((ret = ff_add_format(&formats, fmt)) < 0)
            return ret;
    }

    return ff_set_common_formats(ctx, formats);
}

static int filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    if (!inlink || !in)
        return AVERROR(EINVAL);

    // TODO: get the ROI for each input frame, with the person_id.
    av_dict_set(&in->metadata, "top", "0", 0);
    av_dict_set(&in->metadata, "left", "0", 0);
    av_dict_set(&in->metadata, "width", "3200", 0);
    av_dict_set(&in->metadata, "height", "2400", 0);

    return ff_filter_frame(inlink->dst->outputs[0], in);;
}

static const AVFilterPad avfilter_vf_roi_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = filter_frame,
    },
    { NULL }
};

static const AVFilterPad avfilter_vf_roi_outputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
    },
    { NULL }
};

AVFilter ff_vf_roi = {
    .name          = "roi",
    .description   = NULL_IF_CONFIG_SMALL("Get the ROI(s) of detected person(s) from input frame."),
    .inputs        = avfilter_vf_roi_inputs,
    .outputs        = avfilter_vf_roi_outputs,
    .init          = roi_init,
    .uninit        = roi_uninit,
    .query_formats = query_formats,
};
