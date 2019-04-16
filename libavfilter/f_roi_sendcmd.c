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
 * send specific command filter (to crop or scale filter)
 */

#include "libavutil/avstring.h"
#include "libavutil/bprint.h"
#include "libavutil/file.h"
#include "libavutil/opt.h"
#include "libavutil/parseutils.h"
#include "avfilter.h"
#include "internal.h"
#include "video.h"

#define SCALE_DST_WIDTH  512
#define SCALE_DST_HEIGHT 512

typedef struct ROISendCmdContext {
    const AVClass *class;
    char *commands_str;
} ROISendCmdContext;

#define OFFSET(x) offsetof(ROISendCmdContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_VIDEO_PARAM
static const AVOption options[] = {
    { "commands", "set commands", OFFSET(commands_str), AV_OPT_TYPE_STRING, {.str = NULL}, 0, 0, FLAGS },
    { "c",        "set commands", OFFSET(commands_str), AV_OPT_TYPE_STRING, {.str = NULL}, 0, 0, FLAGS },
    { NULL }
};

static av_cold int init(AVFilterContext *ctx)
{
    ROISendCmdContext *s = ctx->priv;

    if (!s->commands_str) {
        av_log(ctx, AV_LOG_ERROR,
               "The command option must be specified\n");
        return AVERROR(EINVAL);
    }

    return 0;
}

static av_cold void uninit(AVFilterContext *ctx)
{
}

static int filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    AVFilterContext *ctx = inlink->dst;
    ROISendCmdContext *s = ctx->priv;
    // Currently only support sending command to vf_crop & vf_scale.
    const char *filter_crop = "crop";
    const char *filter_scale = "scale";
    int roi_width = 0, roi_height = 0;
    char buf[1024];
    char tmp[10];

    // s->commands_str is the target VF instance.
    if (!s->commands_str)
        return ff_filter_frame(ctx->outputs[0], in);

    if (av_stristr(s->commands_str, filter_crop)) {
        // Here s->commands_str is actually the name of crop filter instance.
        avfilter_graph_send_command(inlink->graph, s->commands_str, "x",
                av_dict_get(in->metadata, "left", NULL, 0)->value,
                buf, sizeof(buf), AVFILTER_CMD_FLAG_ONE);

        avfilter_graph_send_command(inlink->graph, s->commands_str, "y",
                av_dict_get(in->metadata, "top", NULL, 0)->value,
                buf, sizeof(buf), AVFILTER_CMD_FLAG_ONE);

        avfilter_graph_send_command(inlink->graph, s->commands_str, "w",
                av_dict_get(in->metadata, "width", NULL, 0)->value,
                buf, sizeof(buf), AVFILTER_CMD_FLAG_ONE);

        avfilter_graph_send_command(inlink->graph, s->commands_str, "h",
                av_dict_get(in->metadata, "height", NULL, 0)->value,
                buf, sizeof(buf), AVFILTER_CMD_FLAG_ONE);
    } else if (av_stristr(s->commands_str, filter_scale)) {
        // Here s->commands_str is actually the name of scale filter instance.
        // To keep aspect ratio after scaling.
        roi_width = atoi(av_dict_get(in->metadata, "width", NULL, 0)->value);
        roi_height = atoi(av_dict_get(in->metadata, "height", NULL, 0)->value);

        if (roi_width >= roi_height) {
            sprintf(tmp, "%d", SCALE_DST_WIDTH);
            avfilter_graph_send_command(inlink->graph, s->commands_str, "w",
                    tmp, buf, sizeof(buf), AVFILTER_CMD_FLAG_ONE);

            sprintf(tmp, "%d", roi_height * SCALE_DST_WIDTH / roi_width);
            avfilter_graph_send_command(inlink->graph, s->commands_str, "h",
                    tmp, buf, sizeof(buf), AVFILTER_CMD_FLAG_ONE);
        } else {
            sprintf(tmp, "%d", roi_width * SCALE_DST_HEIGHT / roi_height);
            avfilter_graph_send_command(inlink->graph, s->commands_str, "w",
                    tmp, buf, sizeof(buf), AVFILTER_CMD_FLAG_ONE);

            sprintf(tmp, "%d", SCALE_DST_HEIGHT);
            avfilter_graph_send_command(inlink->graph, s->commands_str, "h",
                    tmp, buf, sizeof(buf), AVFILTER_CMD_FLAG_ONE);
        }
    } else
        av_log(ctx, AV_LOG_ERROR,
                "Invalid command for the %s filter.\n",
                s->class->class_name);

    return ff_filter_frame(inlink->dst->outputs[0], in);
}

#define roi_sendcmd_options options
AVFILTER_DEFINE_CLASS(roi_sendcmd);

static const AVFilterPad roi_sendcmd_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = filter_frame,
    },
    { NULL }
};

static const AVFilterPad roi_sendcmd_outputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
    },
    { NULL }
};

AVFilter ff_vf_roi_sendcmd = {
    .name        = "roi_sendcmd",
    .description = NULL_IF_CONFIG_SMALL("Send specific (crop) command to filters."),
    .init        = init,
    .uninit      = uninit,
    .priv_size   = sizeof(ROISendCmdContext),
    .inputs      = roi_sendcmd_inputs,
    .outputs     = roi_sendcmd_outputs,
    .priv_class  = &roi_sendcmd_class,
};
