/**
 * xrdp: A Remote Desktop Protocol server.
 *
 * Copyright (C) Laxmikant Rashinkar 2004-2014
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Encoder
 */

#if defined(HAVE_CONFIG_H)
#include <config_ac.h>
#endif

#include "xrdp_encoder.h"
#include "xrdp.h"
#include "ms-rdpbcgr.h"
#include "thread_calls.h"
#include "fifo.h"
#include "xrdp_egfx.h"
#include "string_calls.h"

#ifdef XRDP_RFXCODEC
#include "rfxcodec_encode.h"
#endif

#ifdef XRDP_X264
#include "xrdp_encoder_x264.h"
#endif

#define DEFAULT_XRDP_GFX_FRAMES_IN_FLIGHT 2
/* limits used for validate env var XRDP_GFX_FRAMES_IN_FLIGHT */
#define MIN_XRDP_GFX_FRAMES_IN_FLIGHT 1
#define MAX_XRDP_GFX_FRAMES_IN_FLIGHT 16

#define DEFAULT_XRDP_GFX_MAX_COMPRESSED_BYTES (3 * 1024 * 1024)
/* limits used for validate env var XRDP_GFX_MAX_COMPRESSED_BYTES */
#define MIN_XRDP_GFX_MAX_COMPRESSED_BYTES (64 * 1024)
#define MAX_XRDP_GFX_MAX_COMPRESSED_BYTES (256 * 1024 * 1024)

#define XRDP_SURCMD_PREFIX_BYTES 256
#define OUT_DATA_BYTES_DEFAULT_SIZE (16 * 1024 * 1024)

#ifdef XRDP_RFXCODEC
/*
 * LH3 LL3, HH3 HL3, HL2 LH2, LH1 HH2, HH1 HL1
 * https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdprfx/3e9c8af4-7539-4c9d-95de-14b1558b902c
 */

/* standard quality */
static const unsigned char g_rfx_quantization_values_std[] =
{
    0x66, 0x66, 0x77, 0x87, 0x98,
    0x76, 0x77, 0x88, 0x98, 0x99
};

/* low quality */
static const unsigned char g_rfx_quantization_values_lq[] =
{
    0x66, 0x66, 0x77, 0x87, 0x98,
    0xAA, 0xAA, 0xAA, 0xAA, 0xAA /* TODO: tentative value */
};

/* ultra low quality */
static const unsigned char g_rfx_quantization_values_ulq[] =
{
    0x66, 0x66, 0x77, 0x87, 0x98,
    0xBB, 0xBB, 0xBB, 0xBB, 0xBB /* TODO: tentative value */
};
#endif

struct enc_rect
{
    short x1;
    short y1;
    short x2;
    short y2;
};

/*****************************************************************************/
static int
process_enc_jpg(struct xrdp_encoder *self, XRDP_ENC_DATA *enc);
#ifdef XRDP_RFXCODEC
static int
process_enc_rfx(struct xrdp_encoder *self, XRDP_ENC_DATA *enc);
#endif
#ifdef XRDP_X264
static int
process_enc_h264(struct xrdp_encoder *self, XRDP_ENC_DATA *enc);
#endif
static int
process_enc_egfx(struct xrdp_encoder *self, XRDP_ENC_DATA *enc);

/*****************************************************************************/
/* Item destructor for self->fifo_to_proc */
static void
xrdp_enc_data_destructor(void *item, void *closure)
{
    XRDP_ENC_DATA *enc = (XRDP_ENC_DATA *)item;
    if (ENC_IS_BIT_SET(enc->flags, ENC_FLAGS_GFX_BIT))
    {
        g_free(enc->u.gfx.cmd);
    }
    else
    {
        g_free(enc->u.sc.drects);
        g_free(enc->u.sc.crects);
    }
    g_free(enc);
}

/* Item destructor for self->fifo_processed */
static void
xrdp_enc_data_done_destructor(void *item, void *closure)
{
    XRDP_ENC_DATA_DONE *enc_done = (XRDP_ENC_DATA_DONE *)item;
    g_free(enc_done->comp_pad_data);
    g_free(enc_done);
}

/*****************************************************************************/
struct xrdp_encoder *
xrdp_encoder_create(struct xrdp_mm *mm)
{
    LOG_DEVEL(LOG_LEVEL_TRACE, "xrdp_encoder_create:");

    struct xrdp_encoder *self;
    struct xrdp_client_info *client_info;
    char buf[1024];
    int pid;

    client_info = mm->wm->client_info;

    /* RemoteFX 7.1 requires LAN but GFX does not */
    if (client_info->mcs_connection_type != CONNECTION_TYPE_LAN)
    {
        if ((mm->egfx_flags & (XRDP_EGFX_H264 | XRDP_EGFX_RFX_PRO)) == 0)
        {
            return 0;
        }
    }
    if (client_info->bpp < 24)
    {
        return 0;
    }

    self = g_new0(struct xrdp_encoder, 1);
    if (self == NULL)
    {
        return NULL;
    }
    self->mm = mm;
    self->process_enc = process_enc_egfx;
    if (client_info->jpeg_codec_id != 0)
    {
        LOG(LOG_LEVEL_INFO, "xrdp_encoder_create: starting jpeg codec session");
        self->codec_id = client_info->jpeg_codec_id;
        self->in_codec_mode = 1;
        self->codec_quality = client_info->jpeg_prop[0];
        client_info->capture_code = CC_SIMPLE;
        client_info->capture_format = XRDP_a8b8g8r8;
        self->process_enc = process_enc_jpg;
    }
#ifdef XRDP_X264
    else if (mm->egfx_flags & XRDP_EGFX_H264)
    {
        LOG(LOG_LEVEL_INFO,
            "xrdp_encoder_create: starting h264 codec session gfx");
        self->in_codec_mode = 1;
        client_info->capture_code = CC_GFX_A2;
        client_info->capture_format = XRDP_nv12_709fr;
        self->gfx = 1;
    }
    else if (client_info->h264_codec_id != 0)
    {
        LOG(LOG_LEVEL_INFO, "xrdp_encoder_create: starting h264 codec session");
        self->codec_id = client_info->h264_codec_id;
        self->in_codec_mode = 1;
        client_info->capture_code = CC_SUF_A2;
        client_info->capture_format = XRDP_nv12;
        self->process_enc = process_enc_h264;
    }
#endif
#ifdef XRDP_RFXCODEC
    else if (mm->egfx_flags & XRDP_EGFX_RFX_PRO)
    {
        LOG(LOG_LEVEL_INFO,
            "xrdp_encoder_create: starting gfx rfx pro codec session");
        self->in_codec_mode = 1;
        client_info->capture_code = CC_GFX_PRO;
        self->gfx = 1;
        self->num_quants = 2;
        self->quant_idx_y = 0;
        self->quant_idx_u = 1;
        self->quant_idx_v = 1;

        switch (client_info->mcs_connection_type)
        {
            case CONNECTION_TYPE_MODEM:
            case CONNECTION_TYPE_BROADBAND_LOW:
            case CONNECTION_TYPE_SATELLITE:
                self->quants = (const char *) g_rfx_quantization_values_ulq;
                break;
            case CONNECTION_TYPE_BROADBAND_HIGH:
            case CONNECTION_TYPE_WAN:
                self->quants = (const char *) g_rfx_quantization_values_lq;
                break;
            case CONNECTION_TYPE_LAN:
            case CONNECTION_TYPE_AUTODETECT: /* not implemented yet */
            default:
                self->quants = (const char *) g_rfx_quantization_values_std;

        }
    }
    else if (client_info->rfx_codec_id != 0)
    {
        LOG(LOG_LEVEL_INFO, "xrdp_encoder_create: starting rfx codec session");
        self->codec_id = client_info->rfx_codec_id;
        self->in_codec_mode = 1;
        client_info->capture_code = CC_SUF_RFX;
        self->process_enc = process_enc_rfx;
        self->codec_handle_rfx = rfxcodec_encode_create(mm->wm->screen->width,
                                 mm->wm->screen->height,
                                 RFX_FORMAT_YUV, 0);
    }
#endif
    else
    {
        g_free(self);
        return 0;
    }

    LOG_DEVEL(LOG_LEVEL_INFO,
              "init_xrdp_encoder: initializing encoder codec_id %d",
              self->codec_id);

    /* setup required FIFOs */
    self->fifo_to_proc = fifo_create(xrdp_enc_data_destructor);
    self->fifo_processed = fifo_create(xrdp_enc_data_done_destructor);
    self->mutex = tc_mutex_create();

    pid = g_getpid();
    /* setup wait objects for signalling */
    g_snprintf(buf, 1024, "xrdp_%8.8x_encoder_event_to_proc", pid);
    self->xrdp_encoder_event_to_proc = g_create_wait_obj(buf);
    g_snprintf(buf, 1024, "xrdp_%8.8x_encoder_event_processed", pid);
    self->xrdp_encoder_event_processed = g_create_wait_obj(buf);
    g_snprintf(buf, 1024, "xrdp_%8.8x_encoder_term", pid);
    self->xrdp_encoder_term_request = g_create_wait_obj(buf);
    self->xrdp_encoder_term_done = g_create_wait_obj(buf);
    if (client_info->gfx)
    {
        const char *env_var = g_getenv("XRDP_GFX_FRAMES_IN_FLIGHT");
        self->frames_in_flight = DEFAULT_XRDP_GFX_FRAMES_IN_FLIGHT;
        if (env_var != NULL)
        {
            int fif = g_atoix(env_var);
            if (fif >= MIN_XRDP_GFX_FRAMES_IN_FLIGHT &&
                    fif <= MAX_XRDP_GFX_FRAMES_IN_FLIGHT)
            {
                self->frames_in_flight = fif;
                LOG(LOG_LEVEL_INFO, "xrdp_encoder_create: "
                    "XRDP_GFX_FRAMES_IN_FLIGHT set to %d", fif);
            }
            else
            {
                LOG(LOG_LEVEL_INFO, "xrdp_encoder_create: "
                    "XRDP_GFX_FRAMES_IN_FLIGHT set but invalid %s",
                    env_var);
            }
        }
        env_var = g_getenv("XRDP_GFX_MAX_COMPRESSED_BYTES");
        self->max_compressed_bytes = DEFAULT_XRDP_GFX_MAX_COMPRESSED_BYTES;
        if (env_var != NULL)
        {
            int mcb = g_atoix(env_var);
            if (mcb >= MIN_XRDP_GFX_MAX_COMPRESSED_BYTES &&
                    mcb <= MAX_XRDP_GFX_MAX_COMPRESSED_BYTES)
            {
                self->max_compressed_bytes = mcb;
                LOG(LOG_LEVEL_INFO, "xrdp_encoder_create: "
                    "XRDP_GFX_MAX_COMPRESSED_BYTES set to %d", mcb);
            }
            else
            {
                LOG(LOG_LEVEL_INFO, "xrdp_encoder_create: "
                    "XRDP_GFX_MAX_COMPRESSED_BYTES set but invalid %s",
                    env_var);
            }
        }
        LOG_DEVEL(LOG_LEVEL_INFO, "Using %d max_compressed_bytes for encoder",
                  self->max_compressed_bytes);
    }
    else
    {
        self->frames_in_flight = client_info->max_unacknowledged_frame_count;
        self->max_compressed_bytes = client_info->max_fastpath_frag_bytes & ~15;
    }
    /* make sure frames_in_flight is at least 1 */
    self->frames_in_flight = MAX(self->frames_in_flight, 1);

    /* create thread to process messages */
    tc_thread_create(proc_enc_msg, self);

    return self;
}

/*****************************************************************************/
void
xrdp_encoder_delete(struct xrdp_encoder *self)
{
    int index;

    (void)index;

    LOG_DEVEL(LOG_LEVEL_INFO, "xrdp_encoder_delete:");
    if (self == 0)
    {
        return;
    }
    if (self->in_codec_mode == 0)
    {
        return;
    }
    /* tell worker thread to shut down */
    g_set_wait_obj(self->xrdp_encoder_term_request);
    g_obj_wait(&self->xrdp_encoder_term_done, 1, NULL, 0, 5000);
    if (!g_is_wait_obj_set(self->xrdp_encoder_term_done))
    {
        LOG(LOG_LEVEL_WARNING, "Encoder failed to shut down cleanly");
    }

#ifdef XRDP_RFXCODEC
    for (index = 0; index < 16; index++)
    {
        if (self->codec_handle_prfx_gfx[index] != NULL)
        {
            rfxcodec_encode_destroy(self->codec_handle_prfx_gfx[index]);
        }
    }
    if (self->codec_handle_rfx != NULL)
    {
        rfxcodec_encode_destroy(self->codec_handle_rfx);
    }
#endif

#if defined(XRDP_X264)
    for (index = 0; index < 16; index++)
    {
        if (self->codec_handle_h264_gfx[index] != NULL)
        {
            xrdp_encoder_x264_delete(self->codec_handle_h264_gfx[index]);
        }
    }
    if (self->codec_handle_h264 != NULL)
    {
        xrdp_encoder_x264_delete(self->codec_handle_h264);
    }
#endif

    /* destroy wait objects used for signalling */
    g_delete_wait_obj(self->xrdp_encoder_event_to_proc);
    g_delete_wait_obj(self->xrdp_encoder_event_processed);
    g_delete_wait_obj(self->xrdp_encoder_term_request);
    g_delete_wait_obj(self->xrdp_encoder_term_done);

    /* cleanup fifos */
    fifo_delete(self->fifo_to_proc, NULL);
    fifo_delete(self->fifo_processed, NULL);
    tc_mutex_delete(self->mutex);
    g_free(self);
}

/*****************************************************************************/
/* called from encoder thread */
static int
process_enc_jpg(struct xrdp_encoder *self, XRDP_ENC_DATA *enc)
{
    int index;
    int x;
    int y;
    int cx;
    int cy;
    int quality;
    int error;
    int out_data_bytes;
    int count;
    char *out_data;
    XRDP_ENC_DATA_DONE *enc_done;
    struct fifo *fifo_processed;
    tbus mutex;
    tbus event_processed;

    LOG_DEVEL(LOG_LEVEL_DEBUG, "process_enc_jpg:");
    quality = self->codec_quality;
    fifo_processed = self->fifo_processed;
    mutex = self->mutex;
    event_processed = self->xrdp_encoder_event_processed;
    count = enc->u.sc.num_crects;
    for (index = 0; index < count; index++)
    {
        x = enc->u.sc.crects[index * 4 + 0];
        y = enc->u.sc.crects[index * 4 + 1];
        cx = enc->u.sc.crects[index * 4 + 2];
        cy = enc->u.sc.crects[index * 4 + 3];
        if (cx < 1 || cy < 1)
        {
            LOG_DEVEL(LOG_LEVEL_WARNING, "process_enc_jpg: error 1");
            continue;
        }

        LOG_DEVEL(LOG_LEVEL_DEBUG, "process_enc_jpg: x %d y %d cx %d cy %d",
                  x, y, cx, cy);

        out_data_bytes = MAX((cx + 4) * cy * 4, 8192);
        if ((out_data_bytes < 1)
                || (out_data_bytes > OUT_DATA_BYTES_DEFAULT_SIZE))
        {
            LOG_DEVEL(LOG_LEVEL_ERROR, "process_enc_jpg: error 2");
            return 1;
        }
        out_data = (char *) g_malloc(out_data_bytes
                                     + XRDP_SURCMD_PREFIX_BYTES + 2, 0);
        if (out_data == 0)
        {
            LOG_DEVEL(LOG_LEVEL_ERROR, "process_enc_jpg: error 3");
            return 1;
        }

        out_data[256] = 0; /* header bytes */
        out_data[257] = 0;
        error = libxrdp_codec_jpeg_compress(self->mm->wm->session, 0, enc->u.sc.data,
                                            enc->u.sc.width, enc->u.sc.height,
                                            enc->u.sc.width * 4, x, y, cx, cy,
                                            quality,
                                            out_data
                                            + XRDP_SURCMD_PREFIX_BYTES + 2,
                                            &out_data_bytes);
        if (error < 0)
        {
            LOG_DEVEL(LOG_LEVEL_ERROR, "process_enc_jpg: jpeg error %d "
                      "bytes %d", error, out_data_bytes);
            g_free(out_data);
            return 1;
        }
        LOG_DEVEL(LOG_LEVEL_WARNING,
                  "jpeg error %d bytes %d", error, out_data_bytes);
        enc_done = (XRDP_ENC_DATA_DONE *)
                   g_malloc(sizeof(XRDP_ENC_DATA_DONE), 1);
        enc_done->comp_bytes = out_data_bytes + 2;
        enc_done->pad_bytes = 256;
        enc_done->comp_pad_data = out_data;
        enc_done->enc = enc;
        enc_done->last = index == (enc->u.sc.num_crects - 1);
        enc_done->x = x;
        enc_done->y = y;
        enc_done->cx = cx;
        enc_done->cy = cy;
        /* done with msg */
        /* inform main thread done */
        tc_mutex_lock(mutex);
        fifo_add_item(fifo_processed, enc_done);
        tc_mutex_unlock(mutex);
        /* signal completion for main thread */
        g_set_wait_obj(event_processed);
    }
    return 0;
}

#ifdef XRDP_RFXCODEC
/*****************************************************************************/
/* called from encoder thread */
static int
process_enc_rfx(struct xrdp_encoder *self, XRDP_ENC_DATA *enc)
{
    int index;
    int x;
    int y;
    int cx;
    int cy;
    int out_data_bytes;
    int count;
    int tiles_written;
    int all_tiles_written;
    int tiles_left;
    int finished;
    char *out_data;
    XRDP_ENC_DATA_DONE *enc_done;
    struct fifo *fifo_processed;
    tbus mutex;
    tbus event_processed;
    struct rfx_tile *tiles;
    struct rfx_rect *rfxrects;
    int alloc_bytes;
    int encode_flags;
    int encode_passes;

    LOG_DEVEL(LOG_LEVEL_DEBUG, "process_enc_rfx:");
    LOG_DEVEL(LOG_LEVEL_DEBUG, "process_enc_rfx: num_crects %d num_drects %d",
              enc->u.sc.num_crects, enc->u.sc.num_drects);
    fifo_processed = self->fifo_processed;
    mutex = self->mutex;
    event_processed = self->xrdp_encoder_event_processed;

    all_tiles_written = 0;
    encode_passes = 0;
    do
    {
        tiles_written = 0;
        tiles_left = enc->u.sc.num_crects - all_tiles_written;
        out_data = NULL;
        out_data_bytes = 0;

        if ((tiles_left > 0) && (enc->u.sc.num_drects > 0))
        {
            alloc_bytes = XRDP_SURCMD_PREFIX_BYTES;
            alloc_bytes += self->max_compressed_bytes;
            alloc_bytes += sizeof(struct rfx_tile) * tiles_left +
                           sizeof(struct rfx_rect) * enc->u.sc.num_drects;
            out_data = g_new(char, alloc_bytes);
            if (out_data != NULL)
            {
                tiles = (struct rfx_tile *)
                        (out_data + XRDP_SURCMD_PREFIX_BYTES +
                         self->max_compressed_bytes);
                rfxrects = (struct rfx_rect *) (tiles + tiles_left);

                count = tiles_left;
                for (index = 0; index < count; index++)
                {
                    x = enc->u.sc.crects[(index + all_tiles_written) * 4 + 0];
                    y = enc->u.sc.crects[(index + all_tiles_written) * 4 + 1];
                    cx = enc->u.sc.crects[(index + all_tiles_written) * 4 + 2];
                    cy = enc->u.sc.crects[(index + all_tiles_written) * 4 + 3];
                    tiles[index].x = x;
                    tiles[index].y = y;
                    tiles[index].cx = cx;
                    tiles[index].cy = cy;
                    tiles[index].quant_y = self->quant_idx_y;
                    tiles[index].quant_cb = self->quant_idx_u;
                    tiles[index].quant_cr = self->quant_idx_v;
                }

                count = enc->u.sc.num_drects;
                for (index = 0; index < count; index++)
                {
                    x = enc->u.sc.drects[index * 4 + 0];
                    y = enc->u.sc.drects[index * 4 + 1];
                    cx = enc->u.sc.drects[index * 4 + 2];
                    cy = enc->u.sc.drects[index * 4 + 3];
                    rfxrects[index].x = x;
                    rfxrects[index].y = y;
                    rfxrects[index].cx = cx;
                    rfxrects[index].cy = cy;
                }

                out_data_bytes = self->max_compressed_bytes;

                encode_flags = 0;
                if (((int)enc->flags & KEY_FRAME_REQUESTED) && encode_passes == 0)
                {
                    encode_flags = RFX_FLAGS_PRO_KEY;
                }
                tiles_written = rfxcodec_encode_ex(self->codec_handle_rfx,
                                                   out_data + XRDP_SURCMD_PREFIX_BYTES,
                                                   &out_data_bytes, enc->u.sc.data,
                                                   enc->u.sc.width, enc->u.sc.height,
                                                   ((enc->u.sc.width + 63) & ~63) * 4,
                                                   rfxrects, enc->u.sc.num_drects,
                                                   tiles, enc->u.sc.num_crects,
                                                   self->quants, self->num_quants,
                                                   encode_flags);
            }
            ++encode_passes;
        }

        LOG_DEVEL(LOG_LEVEL_DEBUG,
                  "process_enc_rfx: rfxcodec_encode tiles_written %d",
                  tiles_written);
        /* only if enc_done->comp_bytes is not zero is something sent
           to the client but you must always send something back even
           on error so Xorg can get ack */
        enc_done = g_new0(XRDP_ENC_DATA_DONE, 1);
        if (enc_done == NULL)
        {
            return 1;
        }
        enc_done->comp_bytes = tiles_written > 0 ? out_data_bytes : 0;
        enc_done->pad_bytes = XRDP_SURCMD_PREFIX_BYTES;
        enc_done->comp_pad_data = out_data;
        enc_done->enc = enc;
        enc_done->x = enc->u.sc.left;
        enc_done->y = enc->u.sc.top;
        enc_done->cx = enc->u.sc.width;
        enc_done->cy = enc->u.sc.height;
        enc_done->frame_id = enc->u.sc.frame_id;
        enc_done->continuation = all_tiles_written > 0;
        if (tiles_written > 0)
        {
            all_tiles_written += tiles_written;
        }
        finished =
            (all_tiles_written == enc->u.sc.num_crects) || (tiles_written < 0);
        enc_done->last = finished;

        /* done with msg */
        /* inform main thread done */
        tc_mutex_lock(mutex);
        fifo_add_item(fifo_processed, enc_done);
        tc_mutex_unlock(mutex);
    }
    while (!finished);

    /* signal completion for main thread */
    g_set_wait_obj(event_processed);

    return 0;
}
#endif

#if defined(XRDP_X264)

/*****************************************************************************/
static int
out_RFX_AVC420_METABLOCK(struct xrdp_egfx_rect *dst_rect,
                         struct stream *s,
                         struct xrdp_egfx_rect *rects,
                         int num_rects)
{
    struct xrdp_region *reg;
    struct xrdp_rect rect;
    int index;
    int count;

    /* RFX_AVC420_METABLOCK */
    s_push_layer(s, iso_hdr, 4); /* numRegionRects, set later */
    reg = xrdp_region_create(NULL);
    if (reg == NULL)
    {
        return 1;
    }
    for (index = 0; index < num_rects; index++)
    {
        rect.left = MAX(0, rects[index].x1 - dst_rect->x1 - 1);
        rect.top = MAX(0, rects[index].y1 - dst_rect->y1 - 1);
        rect.right = MIN(dst_rect->x2 - dst_rect->x1,
                         rects[index].x2 - dst_rect->x1 + 1);
        rect.bottom = MIN(dst_rect->y2 - dst_rect->y1,
                          rects[index].y2 - dst_rect->y1 + 1);
        xrdp_region_add_rect(reg, &rect);
    }
    index = 0;
    while (xrdp_region_get_rect(reg, index, &rect) == 0)
    {
        out_uint16_le(s, rect.left);
        out_uint16_le(s, rect.top);
        out_uint16_le(s, rect.right);
        out_uint16_le(s, rect.bottom);
        index++;
    }
    xrdp_region_delete(reg);
    count = index;
    while (index > 0)
    {
        out_uint8(s, 23); /* qp */
        out_uint8(s, 100); /* quality level 0..100 */
        index--;
    }
    s_push_layer(s, mcs_hdr, 0);
    s_pop_layer(s, iso_hdr);
    out_uint32_le(s, count); /* numRegionRects */
    s_pop_layer(s, mcs_hdr);
    return 0;
}

/*****************************************************************************/
/* called from encoder thread */
static int
process_enc_h264(struct xrdp_encoder *self, XRDP_ENC_DATA *enc)
{
    LOG_DEVEL(LOG_LEVEL_INFO, "process_enc_h264: dummy func");
    return 0;
}

#endif

/*****************************************************************************/
static int
gfx_send_done(struct xrdp_encoder *self, XRDP_ENC_DATA *enc,
              int comp_bytes, int pad_bytes, char *comp_pad_data,
              int got_frame_id, int frame_id, int is_last)

{
    XRDP_ENC_DATA_DONE *enc_done;

    enc_done = g_new0(XRDP_ENC_DATA_DONE, 1);
    if (enc_done == NULL)
    {
        return 1;
    }
    ENC_SET_BIT(enc_done->flags, ENC_DONE_FLAGS_GFX_BIT);
    enc_done->enc = enc;
    enc_done->last = is_last;
    enc_done->pad_bytes = pad_bytes;
    enc_done->comp_bytes = comp_bytes;
    enc_done->comp_pad_data = comp_pad_data;
    if (got_frame_id)
    {
        ENC_SET_BIT(enc_done->flags, ENC_DONE_FLAGS_FRAME_ID_BIT);
        enc_done->frame_id = frame_id;
    }
    /* inform main thread done */
    tc_mutex_lock(self->mutex);
    fifo_add_item(self->fifo_processed, enc_done);
    tc_mutex_unlock(self->mutex);
    /* signal completion for main thread */
    g_set_wait_obj(self->xrdp_encoder_event_processed);
    return 0;
}

/*****************************************************************************/
static struct stream *
gfx_wiretosurface1(struct xrdp_encoder *self,
                   struct xrdp_egfx_bulk *bulk, struct stream *in_s,
                   XRDP_ENC_DATA *enc)
{
#ifdef XRDP_X264
    int index;
    int surface_id;
    int codec_id;
    int pixel_format;
    int num_rects_d;
    int num_rects_c;
    struct stream *rv;
    short left;
    short top;
    short width;
    short height;
    short twidth;
    short theight;
    int bitmap_data_length;
    int flags;
    struct xrdp_egfx_rect *d_rects;
    struct xrdp_egfx_rect *c_rects;
    struct xrdp_egfx_rect dst_rect;
    int error;
    struct stream ls;
    struct stream *s;
    short *crects;
    struct xrdp_enc_gfx_cmd *enc_gfx_cmd = &(enc->u.gfx);
    int mon_index;
    int connection_type;

    connection_type = self->mm->wm->client_info->mcs_connection_type;

    s = &ls;
    g_memset(s, 0, sizeof(struct stream));
    s->size = self->max_compressed_bytes;
    s->data = g_new(char, s->size);
    if (s->data == NULL)
    {
        return NULL;
    }
    s->p = s->data;
    if (!s_check_rem(in_s, 11))
    {
        g_free(s->data);
        return NULL;
    }
    in_uint16_le(in_s, surface_id);
    in_uint16_le(in_s, codec_id);
    in_uint8(in_s, pixel_format);
    in_uint32_le(in_s, flags);
    mon_index = (flags >> 28) & 0xF;
    in_uint16_le(in_s, num_rects_d);
    if ((num_rects_d < 1) || (num_rects_d > 16 * 1024) ||
            (!s_check_rem(in_s, num_rects_d * 8)))
    {
        g_free(s->data);
        return NULL;
    }
    d_rects = g_new0(struct xrdp_egfx_rect, num_rects_d);
    if (d_rects == NULL)
    {
        g_free(s->data);
        return NULL;
    }
    for (index = 0; index < num_rects_d; index++)
    {
        in_uint16_le(in_s, left);
        in_uint16_le(in_s, top);
        in_uint16_le(in_s, width);
        in_uint16_le(in_s, height);
        d_rects[index].x1 = left;
        d_rects[index].y1 = top;
        d_rects[index].x2 = left + width;
        d_rects[index].y2 = top + height;

    }
    if (!s_check_rem(in_s, 2))
    {
        g_free(s->data);
        g_free(d_rects);
        return NULL;
    }
    in_uint16_le(in_s, num_rects_c);
    if ((num_rects_c < 1) || (num_rects_c > 16 * 1024) ||
            (!s_check_rem(in_s, num_rects_c * 8)))
    {
        g_free(s->data);
        g_free(d_rects);
        return NULL;
    }
    c_rects = g_new0(struct xrdp_egfx_rect, num_rects_c);
    if (c_rects == NULL)
    {
        g_free(s->data);
        g_free(d_rects);
        return NULL;
    }
    crects = g_new(short, num_rects_c * 4);
    if (crects == NULL)
    {
        g_free(s->data);
        g_free(c_rects);
        g_free(d_rects);
        return NULL;
    }
    g_memcpy(crects, in_s->p, num_rects_c * 2 * 4);
    for (index = 0; index < num_rects_c; index++)
    {
        in_uint16_le(in_s, left);
        in_uint16_le(in_s, top);
        in_uint16_le(in_s, width);
        in_uint16_le(in_s, height);
        c_rects[index].x1 = left;
        c_rects[index].y1 = top;
        c_rects[index].x2 = left + width;
        c_rects[index].y2 = top + height;
    }
    if (!s_check_rem(in_s, 8))
    {
        g_free(s->data);
        g_free(c_rects);
        g_free(d_rects);
        g_free(crects);
        return NULL;
    }
    in_uint16_le(in_s, left);
    in_uint16_le(in_s, top);
    in_uint16_le(in_s, width);
    in_uint16_le(in_s, height);
    twidth = width;
    theight = height;
    dst_rect.x1 = 0;
    dst_rect.y1 = 0;
    dst_rect.x2 = width;
    dst_rect.y2 = height;
    LOG_DEVEL(LOG_LEVEL_INFO, "gfx_wiretosurface1: left %d top "
              "%d width %d height %d mon_index %d",
              left, top, width, height, mon_index);
    /* RFX_AVC420_METABLOCK */
    if (out_RFX_AVC420_METABLOCK(&dst_rect, s, d_rects, num_rects_d) != 0)
    {
        g_free(s->data);
        g_free(c_rects);
        g_free(d_rects);
        g_free(crects);
        LOG(LOG_LEVEL_INFO, "10");
        return NULL;
    }

    g_free(c_rects);
    g_free(d_rects);

    if (ENC_IS_BIT_SET(flags, 0))
    {
        /* already compressed */
        out_uint8a(s, enc_gfx_cmd->data, enc_gfx_cmd->data_bytes);
    }
    else
    {
        /* assume NV12 format */
        if (twidth * theight * 3 / 2 > enc_gfx_cmd->data_bytes)
        {
            g_free(s->data);
            g_free(crects);
            return NULL;
        }
        bitmap_data_length = s_rem_out(s);
        if (self->codec_handle_h264_gfx[mon_index] == NULL)
        {
            self->codec_handle_h264_gfx[mon_index] =
                xrdp_encoder_x264_create();
            if (self->codec_handle_h264_gfx[mon_index] == NULL)
            {
                g_free(s->data);
                g_free(crects);
                return NULL;
            }
        }
        error = xrdp_encoder_x264_encode(
                    self->codec_handle_h264_gfx[mon_index], 0,
                    0, 0,
                    width, height, twidth, theight, 0,
                    enc_gfx_cmd->data,
                    crects, num_rects_c,
                    s->p, &bitmap_data_length,
                    connection_type, NULL);
        if (error == 0)
        {
            xstream_seek(s, bitmap_data_length);
        }
        else
        {
            g_free(s->data);
            g_free(crects);
            return NULL;
        }
    }
    s_mark_end(s);
    bitmap_data_length = (int) (s->end - s->data);
    rv = xrdp_egfx_wire_to_surface1(bulk, surface_id,
                                    codec_id,
                                    pixel_format, &dst_rect,
                                    s->data, bitmap_data_length);
    g_free(s->data);
    g_free(crects);
    return rv;
#else
    (void)self;
    (void)bulk;
    (void)in_s;
    (void)enc;
    return NULL;
#endif
}

/*****************************************************************************/
static struct stream *
gfx_wiretosurface2(struct xrdp_encoder *self,
                   struct xrdp_egfx_bulk *bulk, struct stream *in_s,
                   XRDP_ENC_DATA *enc)
{
#ifdef XRDP_RFXCODEC
    int index;
    int surface_id;
    int codec_id;
    int codec_context_id;
    int pixel_format;
    int num_rects_d;
    int num_rects_c;
    struct stream *rv;
    short left;
    short top;
    short width;
    short height;
    char *bitmap_data;
    int bitmap_data_length;
    struct rfx_tile *tiles;
    struct rfx_rect *rfxrects;
    int tiles_compressed;
    int flags;
    int total_tiles;
    int tiles_written;
    int mon_index;

    if (!s_check_rem(in_s, 15))
    {
        return NULL;
    }
    in_uint16_le(in_s, surface_id);
    in_uint16_le(in_s, codec_id);
    in_uint32_le(in_s, codec_context_id);
    in_uint8(in_s, pixel_format);
    in_uint32_le(in_s, flags);
    mon_index = (flags >> 28) & 0xF;
    in_uint16_le(in_s, num_rects_d);
    if ((num_rects_d < 1) || (num_rects_d > 16 * 1024) ||
            (!s_check_rem(in_s, num_rects_d * 8)))
    {
        return NULL;
    }
    rfxrects = g_new0(struct rfx_rect, num_rects_d);
    if (rfxrects == NULL)
    {
        return NULL;
    }
    for (index = 0; index < num_rects_d; index++)
    {
        in_uint16_le(in_s, left);
        in_uint16_le(in_s, top);
        in_uint16_le(in_s, width);
        in_uint16_le(in_s, height);
        rfxrects[index].x = left;
        rfxrects[index].y = top;
        rfxrects[index].cx = width;
        rfxrects[index].cy = height;
    }
    if (!s_check_rem(in_s, 2))
    {
        g_free(rfxrects);
        return NULL;
    }
    in_uint16_le(in_s, num_rects_c);
    if ((num_rects_c < 1) || (num_rects_c > 16 * 1024) ||
            (!s_check_rem(in_s, num_rects_c * 8)))
    {
        g_free(rfxrects);
        return NULL;
    }
    tiles = g_new0(struct rfx_tile, num_rects_c);
    if (tiles == NULL)
    {
        g_free(rfxrects);
        return NULL;
    }
    for (index = 0; index < num_rects_c; index++)
    {
        in_uint16_le(in_s, left);
        in_uint16_le(in_s, top);
        in_uint16_le(in_s, width);
        in_uint16_le(in_s, height);
        tiles[index].x = left;
        tiles[index].y = top;
        tiles[index].cx = width;
        tiles[index].cy = height;
        tiles[index].quant_y = self->quant_idx_y;
        tiles[index].quant_cb = self->quant_idx_u;
        tiles[index].quant_cr = self->quant_idx_v;
    }
    if (!s_check_rem(in_s, 8))
    {
        g_free(tiles);
        g_free(rfxrects);
        return NULL;
    }
    in_uint16_le(in_s, left);
    in_uint16_le(in_s, top);
    in_uint16_le(in_s, width);
    in_uint16_le(in_s, height);
    LOG_DEVEL(LOG_LEVEL_INFO, "gfx_wiretosurface2: left %d top "
              "%d width %d height %d mon_index %d",
              left, top, width, height, mon_index);
    if (self->codec_handle_prfx_gfx[mon_index] == NULL)
    {
        self->codec_handle_prfx_gfx[mon_index] = rfxcodec_encode_create(
                width,
                height,
                RFX_FORMAT_YUV,
                RFX_FLAGS_RLGR1 | RFX_FLAGS_PRO1);
        if (self->codec_handle_prfx_gfx[mon_index] == NULL)
        {
            g_free(tiles);
            g_free(rfxrects);
            return NULL;
        }
    }
    bitmap_data_length = self->max_compressed_bytes;
    bitmap_data = g_new(char, bitmap_data_length);
    if (bitmap_data == NULL)
    {
        g_free(tiles);
        g_free(rfxrects);
        return NULL;
    }
    rv = NULL;
    tiles_written = 0;
    total_tiles = num_rects_c;
    for (;;)
    {
        tiles_compressed =
            rfxcodec_encode(self->codec_handle_prfx_gfx[mon_index],
                            bitmap_data,
                            &bitmap_data_length,
                            enc->u.gfx.data,
                            width, height,
                            ((width + 63) & ~63) * 4,
                            rfxrects, num_rects_d,
                            tiles + tiles_written, total_tiles - tiles_written,
                            self->quants, self->num_quants);
        if (tiles_compressed < 1)
        {
            break;
        }
        tiles_written += tiles_compressed;
        rv = xrdp_egfx_wire_to_surface2(bulk, surface_id,
                                        codec_id, codec_context_id,
                                        pixel_format,
                                        bitmap_data, bitmap_data_length);
        if (rv == NULL)
        {
            break;
        }
        LOG_DEVEL(LOG_LEVEL_INFO, "gfx_wiretosurface2: "
                  "tiles_compressed %d total_tiles %d tiles_written %d",
                  tiles_compressed, total_tiles,
                  tiles_written);
        if (tiles_written >= total_tiles)
        {
            /* ok, done with last tile set */
            break;
        }
        /* we have another tile set, send this one to main thread */
        if (gfx_send_done(self, enc, (int)(rv->end - rv->data), 0,
                          rv->data, 0, 0, 0) != 0)
        {
            free_stream(rv);
            rv = NULL;
            break;
        }
        g_free(rv); /* don't call free_stream() here so s->data is valid */
        rv = NULL;
        bitmap_data_length = self->max_compressed_bytes;
    }
    g_free(tiles);
    g_free(rfxrects);
    g_free(bitmap_data);
    return rv;
#else
    (void)self;
    (void)bulk;
    (void)in_s;
    (void)enc;
    return NULL;
#endif
}

/*****************************************************************************/
static struct stream *
gfx_solidfill(struct xrdp_encoder *self,
              struct xrdp_egfx_bulk *bulk, struct stream *in_s)
{
    int surface_id;
    int pixel;
    int num_rects;
    char *ptr8;
    struct xrdp_egfx_rect *rects;

    if (!s_check_rem(in_s, 8))
    {
        return NULL;
    }
    in_uint16_le(in_s, surface_id);
    in_uint32_le(in_s, pixel);
    in_uint16_le(in_s, num_rects);
    if (!s_check_rem(in_s, num_rects * 8))
    {
        return NULL;
    }
    in_uint8p(in_s, ptr8, num_rects * 8);
    rects = (struct xrdp_egfx_rect *) ptr8;
    return xrdp_egfx_fill_surface(bulk, surface_id, pixel, num_rects, rects);
}

/*****************************************************************************/
static struct stream *
gfx_surfacetosurface(struct xrdp_encoder *self,
                     struct xrdp_egfx_bulk *bulk, struct stream *in_s)
{
    int surface_id_src;
    int surface_id_dst;
    char *ptr8;
    int num_pts;
    struct xrdp_egfx_rect *rects;
    struct xrdp_egfx_point *pts;

    if (!s_check_rem(in_s, 14))
    {
        return NULL;
    }
    in_uint16_le(in_s, surface_id_src);
    in_uint16_le(in_s, surface_id_dst);
    in_uint8p(in_s, ptr8, 8);
    rects = (struct xrdp_egfx_rect *) ptr8;
    in_uint16_le(in_s, num_pts);
    if (!s_check_rem(in_s, num_pts * 4))
    {
        return NULL;
    }
    in_uint8p(in_s, ptr8, num_pts * 4);
    pts = (struct xrdp_egfx_point *) ptr8;
    return xrdp_egfx_surface_to_surface(bulk, surface_id_src, surface_id_dst,
                                        rects, num_pts, pts);
}

/*****************************************************************************/
static struct stream *
gfx_createsurface(struct xrdp_encoder *self,
                  struct xrdp_egfx_bulk *bulk, struct stream *in_s)
{
    int surface_id;
    int width;
    int height;
    int pixel_format;

    if (!s_check_rem(in_s, 7))
    {
        return NULL;
    }
    in_uint16_le(in_s, surface_id);
    in_uint16_le(in_s, width);
    in_uint16_le(in_s, height);
    in_uint8(in_s, pixel_format);
    return xrdp_egfx_create_surface(bulk, surface_id,
                                    width, height, pixel_format);
}

/*****************************************************************************/
static struct stream *
gfx_deletesurface(struct xrdp_encoder *self,
                  struct xrdp_egfx_bulk *bulk, struct stream *in_s)
{
    int surface_id;

    if (!s_check_rem(in_s, 2))
    {
        return NULL;
    }
    in_uint16_le(in_s, surface_id);
    return xrdp_egfx_delete_surface(bulk, surface_id);
}

/*****************************************************************************/
static struct stream *
gfx_startframe(struct xrdp_encoder *self,
               struct xrdp_egfx_bulk *bulk, struct stream *in_s)
{
    int frame_id;
    int time_stamp;

    if (!s_check_rem(in_s, 8))
    {
        return NULL;
    }
    in_uint32_le(in_s, frame_id);
    in_uint32_le(in_s, time_stamp);
    return xrdp_egfx_frame_start(bulk, frame_id, time_stamp);
}

/*****************************************************************************/
static struct stream *
gfx_endframe(struct xrdp_encoder *self,
             struct xrdp_egfx_bulk *bulk, struct stream *in_s, int *aframe_id)
{
    int frame_id;

    if (!s_check_rem(in_s, 4))
    {
        return NULL;
    }
    in_uint32_le(in_s, frame_id);
    *aframe_id = frame_id;
    return xrdp_egfx_frame_end(bulk, frame_id);
}

/*****************************************************************************/
static struct stream *
gfx_resetgraphics(struct xrdp_encoder *self,
                  struct xrdp_egfx_bulk *bulk, struct stream *in_s)
{
    int width;
    int height;
    int monitor_count;
    int index;
    struct monitor_info *mi;
    struct stream *rv;

    if (!s_check_rem(in_s, 12))
    {
        return NULL;
    }
    in_uint32_le(in_s, width);
    in_uint32_le(in_s, height);
    in_uint32_le(in_s, monitor_count);
    if ((monitor_count < 1) || (monitor_count > 16) ||
            !s_check_rem(in_s, monitor_count * 20))
    {
        return NULL;
    }
    mi = g_new0(struct monitor_info, monitor_count);
    if (mi == NULL)
    {
        return NULL;
    }
    for (index = 0; index < monitor_count; index++)
    {
        in_uint32_le(in_s, mi[index].left);
        in_uint32_le(in_s, mi[index].top);
        in_uint32_le(in_s, mi[index].right);
        in_uint32_le(in_s, mi[index].bottom);
        in_uint32_le(in_s, mi[index].is_primary);
    }
    rv = xrdp_egfx_reset_graphics(bulk, width, height, monitor_count, mi);
    g_free(mi);
    return rv;
}

/*****************************************************************************/
static struct stream *
gfx_mapsurfacetooutput(struct xrdp_encoder *self,
                       struct xrdp_egfx_bulk *bulk, struct stream *in_s)
{
    int surface_id;
    int x;
    int y;

    if (!s_check_rem(in_s, 10))
    {
        return NULL;
    }
    in_uint16_le(in_s, surface_id);
    in_uint32_le(in_s, x);
    in_uint32_le(in_s, y);
    return xrdp_egfx_map_surface(bulk, surface_id, x, y);
}

/*****************************************************************************/
/* called from encoder thread */
static int
process_enc_egfx(struct xrdp_encoder *self, XRDP_ENC_DATA *enc)
{
    struct stream *s;
    struct stream in_s;
    struct xrdp_egfx_bulk *bulk;
    int cmd_id;
    int cmd_bytes;
    int frame_id;
    int got_frame_id;
    int error;
    char *holdp;
    char *holdend;

    bulk = self->mm->egfx->bulk;
    g_memset(&in_s, 0, sizeof(in_s));
    in_s.data = enc->u.gfx.cmd;
    in_s.size = enc->u.gfx.cmd_bytes;
    in_s.p = in_s.data;
    in_s.end = in_s.data + in_s.size;
    while (s_check_rem(&in_s, 8))
    {
        s = NULL;
        frame_id = 0;
        got_frame_id = 0;
        holdp = in_s.p;
        in_uint16_le(&in_s, cmd_id);
        in_uint8s(&in_s, 2); /* flags */
        in_uint32_le(&in_s, cmd_bytes);
        if ((cmd_bytes < 8) || (cmd_bytes > 32 * 1024))
        {
            return 1;
        }
        holdend = in_s.end;
        in_s.end = holdp + cmd_bytes;
        LOG_DEVEL(LOG_LEVEL_INFO, "process_enc_egfx: cmd_id %d", cmd_id);
        switch (cmd_id)
        {
            case XR_RDPGFX_CMDID_WIRETOSURFACE_1:       /* 0x0001 */
                s = gfx_wiretosurface1(self, bulk, &in_s, enc);
                break;
            case XR_RDPGFX_CMDID_WIRETOSURFACE_2:       /* 0x0002 */
                s = gfx_wiretosurface2(self, bulk, &in_s, enc);
                break;
            case XR_RDPGFX_CMDID_SOLIDFILL:             /* 0x0004 */
                s = gfx_solidfill(self, bulk, &in_s);
                break;
            case XR_RDPGFX_CMDID_SURFACETOSURFACE:      /* 0x0005 */
                s = gfx_surfacetosurface(self, bulk, &in_s);
                break;
            case XR_RDPGFX_CMDID_CREATESURFACE:         /* 0x0009 */
                s = gfx_createsurface(self, bulk, &in_s);
                break;
            case XR_RDPGFX_CMDID_DELETESURFACE:         /* 0x000A */
                s = gfx_deletesurface(self, bulk, &in_s);
                break;
            case XR_RDPGFX_CMDID_STARTFRAME:            /* 0x000B */
                s = gfx_startframe(self, bulk, &in_s);
                break;
            case XR_RDPGFX_CMDID_ENDFRAME:              /* 0x000C */
                s = gfx_endframe(self, bulk, &in_s, &frame_id);
                got_frame_id = 1;
                break;
            case XR_RDPGFX_CMDID_RESETGRAPHICS:         /* 0x000E */
                s = gfx_resetgraphics(self, bulk, &in_s);
                break;
            case XR_RDPGFX_CMDID_MAPSURFACETOOUTPUT:    /* 0x000F */
                s = gfx_mapsurfacetooutput(self, bulk, &in_s);
                break;
            default:
                break;
        }
        /* setup for next cmd */
        in_s.p = holdp + cmd_bytes;
        in_s.end = holdend;
        if (s != NULL)
        {
            /* send message to main thread */
            error = gfx_send_done(self, enc, (int) (s->end - s->data),
                                  0, s->data, got_frame_id, frame_id,
                                  !s_check_rem(&in_s, 8));
            if (error != 0)
            {
                LOG(LOG_LEVEL_ERROR, "process_enc_egfx: gfx_send_done failed "
                    "error %d", error);
                free_stream(s);
                return 1;
            }
            g_free(s); /* don't call free_stream() here so s->data is valid */
        }
        else
        {
            LOG_DEVEL(LOG_LEVEL_INFO, "process_enc_egfx: nil");
        }
    }
    return 0;
}

/**
 * Encoder thread main loop
 *****************************************************************************/
THREAD_RV THREAD_CC
proc_enc_msg(void *arg)
{
    XRDP_ENC_DATA *enc;
    struct fifo *fifo_to_proc;
    tbus mutex;
    tbus event_to_proc;
    tbus term_obj;
    tbus lterm_obj;
    int robjs_count;
    int wobjs_count;
    int cont;
    int timeout;
    tbus robjs[32];
    tbus wobjs[32];
    struct xrdp_encoder *self;

    LOG_DEVEL(LOG_LEVEL_INFO, "proc_enc_msg: thread is running");

    self = (struct xrdp_encoder *) arg;
    if (self == 0)
    {
        LOG_DEVEL(LOG_LEVEL_DEBUG, "proc_enc_msg: self nil");
        return 0;
    }

    fifo_to_proc = self->fifo_to_proc;
    mutex = self->mutex;
    event_to_proc = self->xrdp_encoder_event_to_proc;

    term_obj = g_get_term();
    lterm_obj = self->xrdp_encoder_term_request;

    cont = 1;
    while (cont)
    {
        timeout = -1;
        robjs_count = 0;
        wobjs_count = 0;
        robjs[robjs_count++] = term_obj;
        robjs[robjs_count++] = lterm_obj;
        robjs[robjs_count++] = event_to_proc;

        if (g_obj_wait(robjs, robjs_count, wobjs, wobjs_count, timeout) != 0)
        {
            /* error, should not get here */
            g_sleep(100);
        }

        if (g_is_wait_obj_set(term_obj)) /* global term */
        {
            LOG(LOG_LEVEL_DEBUG,
                "Received termination signal, stopping the encoder thread");
            break;
        }

        if (g_is_wait_obj_set(lterm_obj)) /* xrdp_mm term */
        {
            LOG_DEVEL(LOG_LEVEL_DEBUG, "proc_enc_msg: xrdp_mm term");
            break;
        }

        if (g_is_wait_obj_set(event_to_proc))
        {
            /* clear it right away */
            g_reset_wait_obj(event_to_proc);
            /* get first msg */
            tc_mutex_lock(mutex);
            enc = (XRDP_ENC_DATA *) fifo_remove_item(fifo_to_proc);
            tc_mutex_unlock(mutex);
            while (enc != 0)
            {
                /* do work */
                self->process_enc(self, enc);
                /* get next msg */
                tc_mutex_lock(mutex);
                enc = (XRDP_ENC_DATA *) fifo_remove_item(fifo_to_proc);
                tc_mutex_unlock(mutex);
            }
        }

    } /* end while (cont) */
    g_set_wait_obj(self->xrdp_encoder_term_done);
    LOG_DEVEL(LOG_LEVEL_DEBUG, "proc_enc_msg: thread exit");
    return 0;
}
