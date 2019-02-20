/*
 * video stream protocol
 * Copyright (c) 2013 Luca Barbato
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
 *
 * video stream url_protocol
 */

#include "libavutil/avstring.h"
#include "libavutil/opt.h"
#include "libavutil/pixfmt.h"
#include "fftools/cmdutils.h"
#include "os_support.h"
#include <sys/un.h>
#include "url.h"

#include "evt_recorder.h"

#define VSBUF_MAGIC 0x4EAC812B

typedef struct {
    enum AVPixelFormat pix_fmt;
    unsigned int size_x;
    unsigned int size_y;
    unsigned long long image_ptr;
    unsigned int magic;
} VSBufContext;

static int vsbuf_open(URLContext *h, const char *filename, int flags)
{
    int ret = 0;
    VSBufContext *vc = h->priv_data;

    ret = evt_rec_init();
    if (ret) {
        printf("evt_rec_init error\n");
        return ret;
    }
    printf("%s evt_rec_init  success\n", __func__);
    vc->magic = VSBUF_MAGIC;

    return 0;
}

static int vsbuf_read(URLContext *h, uint8_t *buf, int size)
{
    int buf_size = size;
    VSBufContext *vc = h->priv_data;
    uint8_t *buffer = NULL;

    //printf("vsbuf_read to read size: %d\n", size);
    evt_rec_get_frame((const uint8_t**)&buffer, &buf_size);
    vc->image_ptr = (unsigned long long)buffer;
    //printf("vsbuf_read size %d from %p buffer:%p imagePtr:%p\n", buf_size, buf, buffer,(unsigned char*)vc->image_ptr);
#ifdef ADDITONAL_METADATA
    if(!vsbuf_start_time){
        vsbuf_start_time = evt_get_sys_time_ms((char*)"vsbuf_start_time");
        printf("the first frame reading time - vsbuf_start_time:%lu\n", vsbuf_start_time);
    }
#endif
    //buf_size = 100;
    //evt_dump_buf((char*)"vsbuf_read", buf, buf_size);

    return size;
}

static int vsbuf_close(URLContext *h)
{
    //VSBufContext *vc = h->priv_data;
    evt_rec_exit();
    printf("%s evt_rec_exit\n", __func__);
    return 0;
}

const URLProtocol ff_vsbuf_protocol = {
    .name                = "vsbuf",
    .url_open            = vsbuf_open,
    .url_read            = vsbuf_read,
    .url_close           = vsbuf_close,
    .priv_data_size      = sizeof(VSBufContext),
};
