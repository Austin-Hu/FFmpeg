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

#if CONFIG_LIBPERSONID
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>
#include "person_id_c.h"
#endif

// Note: person_id needs bgr24 input
#if CONFIG_LIBPERSONID
#define VF_ROI_INPUT_FORMAT "bgr24"
#define SKIP_IDENTIFY_NUM 1
#else
#define VF_ROI_INPUT_FORMAT "uyvy422"
#endif

#define UNUSED(x) (void)(x)

typedef struct ROIContext {
    int top;
    int left;
    int width;
    int height;
#if CONFIG_LIBPERSONID
    personid_t *personid;
    int filtered_cnt;
    // Used to record the last ROI info.
    int recorded_top;
    int recorded_left;
    int recorded_width;
    int recorded_height;
#endif
} ROIContext;

#if CONFIG_LIBPERSONID
#define PERSON_ID_CB_MAGIC 0xCB

static void personid_callback(const char *name, int x, int y,
        int width, int height, float dis, float az, void *context)
{
    ROIContext *roi_ctx = (ROIContext *)context;
    int sig = SIGCONT;
    union sigval sv;

    // Assign ROI with correct values.
    // ROI in roi_ctx was set to default (full size).
    if ((x < 0) || (x > roi_ctx->width))
        roi_ctx->left = 0;
    else
        roi_ctx->left = x;

    if ((y < 0) || (y > roi_ctx->height))
        roi_ctx->top = 0;
    else
        roi_ctx->top = y;

    if (width > 0) {
        if (x < 0) {
            if (x + width < roi_ctx->width)
                roi_ctx->width = x + width;
            else
                roi_ctx->width = width;
        } else {
            if (x + width > roi_ctx->width)
                roi_ctx->width -= x;
            else
                roi_ctx->width = width;
        }
    }

    if (height > 0) {
        if (y < 0) {
            if (y + height < roi_ctx->height)
                roi_ctx->height = y + height;
            else
                roi_ctx->height = height;
        } else {
            if (y + height > roi_ctx->height)
                roi_ctx->height -= y;
            else
                roi_ctx->height = height;
        }
    }

    av_log(NULL, AV_LOG_DEBUG,
            "Identified ROI of %s in Frame: (%d, %d), %d x %d, "
            "Context ROI is (%d, %d), %d x %d\n",
            name, x, y, width, height, roi_ctx->left,
            roi_ctx->top, roi_ctx->width, roi_ctx->height);

    sv.sival_int = PERSON_ID_CB_MAGIC;

    if (sigqueue(getpid(), sig, sv) == -1)
        av_log(NULL, AV_LOG_ERROR, "sigqueue %d\n", sig);
}
#endif

static int roi_init(AVFilterContext *ctx)
{
    ROIContext *roi_ctx = ctx->priv;

#if CONFIG_LIBPERSONID
    personid_t *personid = NULL;
    av_log(ctx, AV_LOG_INFO, "Start to initialize person_id\n");

    /* Config personid */
    personid_config_set_data("./data/");
    // Disable show result video
    personid_config_show(0);

    personid = personid_create();
    personid_set_callback(personid, personid_callback, (void *)roi_ctx);
    personid_set_person_name(personid, "Austin", 2);

    personid_start(personid);

    roi_ctx->personid = personid;
    av_log(ctx, AV_LOG_INFO, "Initialized person_id\n");
#else
    UNUSED(roi_ctx);
#endif
    return 0;
}

static void roi_uninit(AVFilterContext *ctx)
{
    ROIContext *roi_ctx = ctx->priv;

#if CONFIG_LIBPERSONID
	personid_exit(roi_ctx->personid);
#else
    UNUSED(roi_ctx);
#endif
}

static int query_formats(AVFilterContext *ctx)
{
    AVFilterFormats *formats = NULL;
    int fmt;
    const AVPixFmtDescriptor *desc = NULL;
    int ret = 0;

    for (fmt = 0; av_pix_fmt_desc_get(fmt); fmt++) {
        desc = av_pix_fmt_desc_get(fmt);
        if (strcmp(desc->name, VF_ROI_INPUT_FORMAT))
            continue;

        if ((ret = ff_add_format(&formats, fmt)) < 0)
            return ret;
    }

    return ff_set_common_formats(ctx, formats);
}

static void meta_data_set_roi(ROIContext *roi_ctx, AVFrame *in)
{
    char *str = NULL;
    int length = 0;

    length = snprintf(NULL, 0, "%d", roi_ctx->top);
    str = malloc(length + 1);
    snprintf(str, length + 1, "%d", roi_ctx->top);
    av_dict_set(&in->metadata, "top", str, 0);
    free(str);

    length = snprintf(NULL, 0, "%d", roi_ctx->left);
    str = malloc(length + 1);
    snprintf(str, length + 1, "%d", roi_ctx->left);
    av_dict_set(&in->metadata, "left", str, 0);
    free(str);

    length = snprintf(NULL, 0, "%d", roi_ctx->width);
    str = malloc(length + 1);
    snprintf(str, length + 1, "%d", roi_ctx->width);
    av_dict_set(&in->metadata, "width", str, 0);
    free(str);

    length = snprintf(NULL, 0, "%d", roi_ctx->height);
    str = malloc(length + 1);
    snprintf(str, length + 1, "%d", roi_ctx->height);
    av_dict_set(&in->metadata, "height", str, 0);
    free(str);

    // Record the width/height of input AVFrame (640x480),
    // to be adjusted later (in dualinput_metadata filter).
    length = snprintf(NULL, 0, "%d", in->width);
    str = malloc(length + 1);
    snprintf(str, length + 1, "%d", in->width);
    av_dict_set(&in->metadata, "base_width", str, 0);
    free(str);

    length = snprintf(NULL, 0, "%d", in->height);
    str = malloc(length + 1);
    snprintf(str, length + 1, "%d", in->height);
    av_dict_set(&in->metadata, "base_height", str, 0);
    free(str);
}

static int filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    AVFilterContext *ctx = NULL;
    ROIContext *roi_ctx = NULL;
#if CONFIG_LIBPERSONID
    sigset_t allSigs;
    siginfo_t si;
    int sig = 0;
    struct timespec timeout;
    timeout.tv_sec = 1;
    timeout.tv_nsec = 0;
#endif

    if (!inlink || !in)
        return AVERROR(EINVAL);

    ctx = inlink->dst;
    roi_ctx = inlink->dst->priv;

    // Set the ROI with default value (full size frame).
    roi_ctx->left = 0;
    roi_ctx->top = 0;
    roi_ctx->width = in->width;
    roi_ctx->height = in->height;

#if CONFIG_LIBPERSONID
    if (in->format != AV_PIX_FMT_BGR24) {
        av_log(ctx, AV_LOG_ERROR, "Unsupported pixel format %d\n", in->format);
        return AVERROR(EINVAL);
    }

    roi_ctx->filtered_cnt++;
    if (roi_ctx->filtered_cnt % SKIP_IDENTIFY_NUM) {
        if ((roi_ctx->recorded_width > 0) && (roi_ctx->recorded_height > 0)) {
            roi_ctx->left = roi_ctx->recorded_left;
            roi_ctx->top = roi_ctx->recorded_top;
            roi_ctx->width = roi_ctx->recorded_width;
            roi_ctx->height = roi_ctx->recorded_height;
        }
        goto roi_end;
    }

    personid_identify(roi_ctx->personid, in->height, in->width, in->data[0]);

    // Wait to get the person_id callback triggerred.
    sigfillset(&allSigs);
    if (sigprocmask(SIG_SETMASK, &allSigs, NULL) == -1)
        av_log(ctx, AV_LOG_ERROR, "sigprocmask\n");

    for (;;) {
        // Note: it's better to use sigtimedwait() with time out.
        sig = sigtimedwait(&allSigs, &si, &timeout);
        if (sig == -1) {
            av_log(ctx, AV_LOG_ERROR,
                    "Identify ROI with time out, sig = %d\n", sig);
            break;
        }

        if ((sig == SIGCONT) &&
                (si.si_value.sival_int == PERSON_ID_CB_MAGIC)) {
            // person_id callback is invoked, and the ROI in ROIContext
            // should be changed.
            roi_ctx->recorded_left = roi_ctx->left;
            roi_ctx->recorded_top = roi_ctx->top;
            roi_ctx->recorded_width = roi_ctx->width;
            roi_ctx->recorded_height = roi_ctx->height;
            roi_ctx->filtered_cnt = 0;
            break;
        }
    }
#endif

roi_end:
    av_log(ctx, AV_LOG_DEBUG, "Set ROI with (%d, %d), %d x %d\n",
            roi_ctx->left, roi_ctx->top, roi_ctx->width, roi_ctx->height);
    meta_data_set_roi(roi_ctx, in);

    return ff_filter_frame(inlink->dst->outputs[0], in);
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
    .outputs       = avfilter_vf_roi_outputs,
    .init          = roi_init,
    .uninit        = roi_uninit,
    .query_formats = query_formats,
    .priv_size     = sizeof(ROIContext),
};
