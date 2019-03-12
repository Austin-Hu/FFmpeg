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
 * filter for manipulating frame metadata,
 * with dual inputs (main frame + ROI frame)
 */
#include "libavutil/internal.h"
#include "libavutil/opt.h"
#include "framesync.h"
#include "avfilter.h"
#include "formats.h"
#include "internal.h"
#include "video.h"

enum INPUT_FRAMES {
    MAIN_FRAME,
    ROI_FRAME,
    INPUT_FRAME_NB
};

typedef struct DualInputMetadataContext {
    const AVClass *class;
    FFFrameSync fs;
} DualInputMetadataContext;

static int config_output(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    DualInputMetadataContext *s = ctx->priv;
    int ret;

    if ((ret = ff_framesync_init_dualinput(&s->fs, ctx)) < 0)
        return ret;

    outlink->w = ctx->inputs[MAIN_FRAME]->w;
    outlink->h = ctx->inputs[MAIN_FRAME]->h;
    outlink->time_base = ctx->inputs[MAIN_FRAME]->time_base;

    return ff_framesync_configure(&s->fs);
}

static int do_add_metadata(FFFrameSync *fs)
{
    AVFilterContext *ctx = fs->parent;
    AVFilterLink *outlink = ctx->outputs[0];
    AVFrame *main_frame, *roi_frame;
    int ret = 0;

    ret = ff_framesync_dualinput_get(fs, &main_frame, &roi_frame);
    if (ret)
        return ff_filter_frame(outlink, main_frame);

    if (!roi_frame)
        return ff_filter_frame(outlink, main_frame);

    // Copy the ROI related matadata to the main frame.
    av_dict_copy(&main_frame->metadata, roi_frame->metadata, 0);

    return ff_filter_frame(outlink, main_frame);
}

static av_cold int init(AVFilterContext *ctx)
{
    DualInputMetadataContext *s = ctx->priv;

    s->fs.on_event = do_add_metadata;
    return 0;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    DualInputMetadataContext *s = ctx->priv;

    ff_framesync_uninit(&s->fs);
}

static int activate(AVFilterContext *ctx)
{
    DualInputMetadataContext *s = ctx->priv;
    return ff_framesync_activate(&s->fs);
}

static const AVOption di_metadata_options[] = {
    { NULL }
};

FRAMESYNC_DEFINE_CLASS(di_metadata, DualInputMetadataContext, fs);

static const AVFilterPad avfilter_vf_di_metadata_inputs[] = {
    {
        .name         = "main",
        .type         = AVMEDIA_TYPE_VIDEO,
    },
    {
        .name         = "roi",
        .type         = AVMEDIA_TYPE_VIDEO,
    },
    { NULL }
};

static const AVFilterPad avfilter_vf_di_metadata_outputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_VIDEO,
        .config_props  = config_output,
    },
    { NULL }
};

AVFilter ff_vf_di_metadata = {
    .name          = "di_metadata",
    .description   = NULL_IF_CONFIG_SMALL("Manipulate video frame metadata with dual inputs."),
    .preinit       = di_metadata_framesync_preinit,
    .init          = init,
    .uninit        = uninit,
    .priv_size     = sizeof(DualInputMetadataContext),
    .priv_class    = &di_metadata_class,
    .activate      = activate,
    .inputs        = avfilter_vf_di_metadata_inputs,
    .outputs       = avfilter_vf_di_metadata_outputs,
    .flags       = AVFILTER_FLAG_SUPPORT_TIMELINE_GENERIC,
    //.flags         = AVFILTER_FLAG_SUPPORT_TIMELINE_INTERNAL |
    //    AVFILTER_FLAG_SLICE_THREADS,
};
