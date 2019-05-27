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

#include <Python.h>
// Note: FancyVideo needs bgr24 input
#if CONFIG_LIBFANCYVIDEO
#define VF_SEG_INPUT_FORMAT "bgr24"
#else
#define VF_SEG_INPUT_FORMAT "uyvy422"
#endif

#define UNUSED(x) (void)(x)

typedef struct SEGContext {
    PyObject *py_module;
    PyObject *py_func_dict;
    PyObject *py_init;
    PyObject *py_seg;
    PyObject *py_init_args;
    PyObject *py_seg_args;
} SEGContext;

// Hacking to control the Python module to be initialized/uninitialized only once...
static int first_time = 1;

static int seg_init(AVFilterContext *ctx)
{
    SEGContext *seg_ctx = ctx->priv;
    if (!seg_ctx)
        return -1;

    if (first_time)
        return 0;

    Py_Initialize();

    if (!Py_IsInitialized()) {
        av_log(ctx, AV_LOG_ERROR, "Failed to initialize Python extension.\n");
        return -1;
    }

    PyRun_SimpleString("import sys");
    PyRun_SimpleString("import cv2");
    PyRun_SimpleString("import caffe");
    PyRun_SimpleString("import numpy as np");
    PyRun_SimpleString("import time");
    PyRun_SimpleString("import lmdb");
    PyRun_SimpleString("from caffe.proto import caffe_pb2");
    // Set the current directory, otherwise the Python script can't be loaded.
    PyRun_SimpleString("sys.path.append('./libavfilter/')");

    PyRun_SimpleString("person_label = 255");
    PyRun_SimpleString("max_input_height = 480");
    PyRun_SimpleString("max_input_width = 640");

    // Initialize the model network and weights
    seg_ctx->py_module = PyImport_ImportModule("predict6");
    if (!seg_ctx->py_module) {
        av_log(ctx, AV_LOG_ERROR, "Failed to import the Python module.\n");
        return -1;
    }

    seg_ctx->py_func_dict = PyModule_GetDict(seg_ctx->py_module);
    if (!seg_ctx->py_func_dict) {
        av_log(ctx, AV_LOG_ERROR, "Failed to get the function dictionary.\n");
        return -1;
    }

    seg_ctx->py_init = PyDict_GetItemString(seg_ctx->py_func_dict, "init");
    if (!seg_ctx->py_init || !PyCallable_Check(seg_ctx->py_init)) {
        av_log(ctx, AV_LOG_ERROR, "Failed to get the Python init function.\n");
        return -1;
    }

    seg_ctx->py_seg = PyDict_GetItemString(seg_ctx->py_func_dict, "segment");
    if (!seg_ctx->py_seg || !PyCallable_Check(seg_ctx->py_init)) {
        av_log(ctx, AV_LOG_ERROR, "Failed to get the Python segment function.\n");
        return -1;
    }

    seg_ctx->py_init_args = PyTuple_New(0);
    PyObject_CallObject(seg_ctx->py_init, seg_ctx->py_init_args);

    return 0;
}

static void seg_uninit(AVFilterContext *ctx)
{
    SEGContext *seg_ctx = ctx->priv;
#if 0
    PyObject *poAttrList = NULL;
    PyObject *poAttrIter = NULL;
    PyObject *poAttrName = NULL;
    char *oAttrName = NULL;
    PyObject *poAttr = NULL;
#endif

    if (first_time) {
        first_time = 0;
        return;
    }

    if (seg_ctx->py_init_args)
        Py_DECREF(seg_ctx->py_init_args);

    if (seg_ctx->py_seg_args)
        Py_DECREF(seg_ctx->py_seg_args);

    if (seg_ctx->py_seg)
        Py_DECREF(seg_ctx->py_seg);

    if (seg_ctx->py_init)
        Py_DECREF(seg_ctx->py_init);

    if (seg_ctx->py_func_dict)
        Py_DECREF(seg_ctx->py_func_dict);

#if PY_MAJOR_VERSION < 3
    if (seg_ctx->py_module)
        Py_DECREF(seg_ctx->py_module);

    Py_Finalize();
#else
    if (seg_ctx->py_module) {
#if 0
        // Apply the work around from https://github.com/stack-of-tasks/pinocchio/commit/2f0eb3a6789ff3f3255ec147350ff9a2d643aee0
        poAttrList = PyObject_Dir(seg_ctx->py_module);
        poAttrIter = PyObject_GetIter(poAttrList);

        while ((poAttrName = PyIter_Next(poAttrIter)) != NULL) {
            oAttrName = PyUnicode_AS_DATA(poAttrName);

            // Make sure we don't delete any private objects.
            if (!strstr(oAttrName, "__")) {
                poAttr = PyObject_GetAttr(seg_ctx->py_module, poAttrName);

                // Make sure we don't delete any module objects.
                if (poAttr && poAttr->ob_type != seg_ctx->py_module->ob_type)
                    PyObject_SetAttr(seg_ctx->py_module, poAttrName, NULL);
                Py_DecRef(poAttr);
            }
            Py_DecRef(poAttrName);
        }
        Py_DecRef(poAttrIter);
        Py_DecRef(poAttrList);
#endif

        Py_DECREF(seg_ctx->py_module);
    }

    Py_Finalize();

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
        if (strcmp(desc->name, VF_SEG_INPUT_FORMAT))
            continue;

        if ((ret = ff_add_format(&formats, fmt)) < 0)
            return ret;
    }

    return ff_set_common_formats(ctx, formats);
}

static int filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    AVFilterContext *ctx = inlink->dst;
    SEGContext *seg_ctx = NULL;

    if (!inlink || !in)
        return AVERROR(EINVAL);

    seg_ctx = ctx->priv;

    seg_ctx->py_seg_args = PyTuple_New(0);
    PyObject_CallObject(seg_ctx->py_seg, seg_ctx->py_seg_args);

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
