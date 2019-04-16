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

static int set_metadata_with_adjusted_roi(AVFrame *main_frame,
        AVFrame *roi_frame)
{
    int vf_roi_input_width = 0, vf_roi_input_height = 0;
    int roi_left = 0, roi_top = 0, roi_width = 0, roi_height = 0;
    int tmp_val = 0;
    AVDictionaryEntry *t = NULL;
    char *str = NULL;
    int length = 0;

    t = av_dict_get(roi_frame->metadata, "base_width", NULL, 0);
    if (t)
        vf_roi_input_width = atoi(t->value);

    t = av_dict_get(roi_frame->metadata, "base_height", NULL, 0);
    if (t)
        vf_roi_input_height = atoi(t->value);

    if ((vf_roi_input_width == 0) || (vf_roi_input_height == 0)) {
        av_log(NULL, AV_LOG_ERROR, "Invalid input for vf_roi filter: %d x %d\n",
                vf_roi_input_width, vf_roi_input_height);
        return -1;
    }

    // Adjust the ROI from vf_roi filter, to the real values
    // relative to original full size.
    t = av_dict_get(roi_frame->metadata, "left", NULL, 0);
    if (t) {
        roi_left = atoi(t->value);
        tmp_val = roi_left * main_frame->width / vf_roi_input_width;
        length = snprintf(NULL, 0, "%d", tmp_val);
        str = malloc(length + 1);
        snprintf(str, length + 1, "%d", tmp_val);
        av_dict_set(&main_frame->metadata, "left", str, 0);
        free(str);
    }

    t = av_dict_get(roi_frame->metadata, "top", NULL, 0);
    if (t) {
        roi_top = atoi(t->value);
        tmp_val = roi_top * main_frame->height / vf_roi_input_height;
        length = snprintf(NULL, 0, "%d", tmp_val);
        str = malloc(length + 1);
        snprintf(str, length + 1, "%d", tmp_val);
        av_dict_set(&main_frame->metadata, "top", str, 0);
        free(str);
    }

    t = av_dict_get(roi_frame->metadata, "width", NULL, 0);
    if (t) {
        roi_width = atoi(t->value);
        tmp_val = roi_width * main_frame->width / vf_roi_input_width;
        length = snprintf(NULL, 0, "%d", tmp_val);
        str = malloc(length + 1);
        snprintf(str, length + 1, "%d", tmp_val);
        av_dict_set(&main_frame->metadata, "width", str, 0);
        free(str);
    }

    t = av_dict_get(roi_frame->metadata, "height", NULL, 0);
    if (t) {
        roi_height = atoi(t->value);
        tmp_val = roi_height * main_frame->height / vf_roi_input_height;
        length = snprintf(NULL, 0, "%d", tmp_val);
        str = malloc(length + 1);
        snprintf(str, length + 1, "%d", tmp_val);
        av_dict_set(&main_frame->metadata, "height", str, 0);
        free(str);
    }

    return 0;
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

    // Adjust and copy the ROI related matadata to the main frame.
    set_metadata_with_adjusted_roi(main_frame, roi_frame);

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
