/*
 * Copyright (c) 2013 Paul Geisler
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
 * create or replace the alpha component by a chromakey
 */


#include <string.h>

#include "libavutil/pixfmt.h"
#include "avfilter.h"
#include "libavutil/opt.h"
#include "bufferqueue.h"
#include "drawutils.h"
#include "formats.h"
#include "internal.h"
#include "video.h"
#include "libavutil/eval.h"

#define TS2D(ts) ((ts) == AV_NOPTS_VALUE ? NAN : (double)(ts))

enum { Y, U, V, A };

typedef struct {
    const AVClass* class;
    int frame_requested;
    uint8_t rgba_map[4];
    struct FFBufQueue queue_main;
    int u, v, min, max;
    char* alpha_expr;
    int print_uv;
} AlphaChromakeyContext;

static const char *const var_names[] = {
    "t",
    NULL
};

#define FLAGS AV_OPT_FLAG_VIDEO_PARAM|AV_OPT_FLAG_FILTERING_PARAM
static const AVOption alphachromakey_options[] = {
    {"u", "set the u center"                                , offsetof(AlphaChromakeyContext,u         ), AV_OPT_TYPE_INT   , {.i64=0}     , 0, 255, FLAGS },
    {"v", "set the v center"                                , offsetof(AlphaChromakeyContext,v         ), AV_OPT_TYPE_INT   , {.i64=0}     , 0, 255, FLAGS },
    {"min", "set the minimal tolerance the keying sets in"  , offsetof(AlphaChromakeyContext,min       ), AV_OPT_TYPE_INT   , {.i64=0}     , 0, 255, FLAGS },
    {"max", "set the maximal tolerance the keying completes", offsetof(AlphaChromakeyContext,max       ), AV_OPT_TYPE_INT   , {.i64=0}     , 0, 255, FLAGS },
    {"alpha", "set alpha expression"                        , offsetof(AlphaChromakeyContext,alpha_expr), AV_OPT_TYPE_STRING, {.str = NULL}, 0,   0, FLAGS },
    {"print_uv", "set the maximal tolerance the keying completes", offsetof(AlphaChromakeyContext,print_uv       ), AV_OPT_TYPE_INT   , {.i64=0}     , 0, 255, FLAGS },
    {NULL},
};
AVFILTER_DEFINE_CLASS(alphachromakey);

static const char *shorthand[] = { "u", "v", "min","max","alpha", "print_uv", NULL };

static av_cold int init(AVFilterContext *ctx, const char *args)
{
    AlphaChromakeyContext *context = ctx->priv;

    context->class = &alphachromakey_class;
    av_opt_set_defaults(context);

    return av_opt_set_from_string(context, args, shorthand, "=", ":");
}

static av_cold void uninit(AVFilterContext *ctx)
{
    AlphaChromakeyContext *keyer = ctx->priv;
    ff_bufqueue_discard_all(&keyer->queue_main);
}

static int query_formats(AVFilterContext *ctx)
{
    static const enum AVPixelFormat main_fmts[] = {
        AV_PIX_FMT_YUVA422P, AV_PIX_FMT_YUVA420P,
        AV_PIX_FMT_NONE
    };
    AVFilterFormats *main_formats = ff_make_format_list(main_fmts);
    ff_formats_ref(main_formats, &ctx->inputs[0]->out_formats);
    ff_formats_ref(main_formats, &ctx->outputs[0]->in_formats);
    return 0;
}

static int config_input_main(AVFilterLink *inlink)
{
    AlphaChromakeyContext *keyer = inlink->dst->priv;
    return 0;
}

static int config_output(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    AVFilterLink *mainlink = ctx->inputs[0];

    outlink->w = mainlink->w;
    outlink->h = mainlink->h;
    outlink->time_base = mainlink->time_base;
    outlink->sample_aspect_ratio = mainlink->sample_aspect_ratio;
    outlink->frame_rate = mainlink->frame_rate;
    return 0;
}

static void draw_frame(AVFilterContext *ctx,
                       AVFrame *main_buf)
{
    AlphaChromakeyContext *keyer = ctx->priv;
    
    // re-read parameters from external file uv.txt if existent
    // this allows for realtime parameter adjustment.
    FILE* file=fopen("alphachromakey.params","r");
    if(file)
    {
        char buffer[1024];
        fgets(buffer,1024,file);
        av_opt_set_from_string(keyer, buffer, shorthand, "=", ":");
        fclose(file);
    }

	// evaluate alpha expression
    double alpha=1;
    if(keyer->alpha_expr)
    {
        AVFilterLink *inlink = ctx->inputs[0];
            
        double vars[]={TS2D(main_buf->pts) * av_q2d(inlink->time_base)};
        int ret=av_expr_parse_and_eval(&alpha, keyer->alpha_expr,
                           var_names, vars,
                           NULL, NULL, NULL, NULL, NULL, 0, ctx);
        if(ret<0)
        {
            av_log(ctx, AV_LOG_ERROR, "Bad alpha expression.\n");        
            return AVERROR(EINVAL);
        }
    }
    
    int32_t alpha32=(int32_t)(alpha*256);

    // do the keying
    int h = main_buf->height;
    int w = main_buf->linesize[A];
    int x, y;
    long sum_u=0, sum_v=0, count=1;

    int32_t tola2=keyer->min*(int32_t)keyer->min;
    int32_t tolb2=keyer->max*(int32_t)keyer->max;
    for(y = 0; y < h; y++)  
    {
        int yfact = (main_buf->format==AV_PIX_FMT_YUVA422P ? y : y/2);
        uint8_t* in_u  = main_buf->data[U] + yfact  * main_buf->linesize[U];
        uint8_t* in_v  = main_buf->data[V] + yfact  * main_buf->linesize[V];
        uint8_t* pout  = main_buf->data[A] + y * main_buf->linesize[A];
        for (int x = 0; x < w; x++) {
            // radius threshold with feather, double precision
            int32_t du=in_u[x/2] - (int32_t)(keyer->u);
            int32_t dv=in_v[x/2] - (int32_t)(keyer->v);
            int32_t r2=du*du+dv*dv;

            if      (r2<tola2) r2=0;
            else if (r2<tolb2) r2=(r2-tola2)*255/(tolb2-tola2);
            else               r2=255;

            pout[x]=r2*alpha32/256;
            if(keyer->print_uv && w/2-x>-100 && w/2-x <100 && h/2-y>-100 && h/2-y<100){
                sum_u+=in_u[x/2];
                sum_v+=in_v[x/2];
		count++;
            }
        }
    }
    if(keyer->print_uv) printf("chromakey mean u:%ld v:%ld\n",sum_u/count, sum_v/count);
}

static int filter_frame(AVFilterLink *inlink, AVFrame *buf)
{
    AVFilterContext *ctx = inlink->dst;
    AlphaChromakeyContext *keyer = ctx->priv;

    struct FFBufQueue *queue = &keyer->queue_main;
    ff_bufqueue_add(ctx, queue, buf);

    while (1) {
        AVFrame *main_buf;

        if (!ff_bufqueue_peek(&keyer->queue_main, 0)) break;

        main_buf = ff_bufqueue_get(&keyer->queue_main);

        keyer->frame_requested = 0;
        draw_frame(ctx, main_buf);
        ff_filter_frame(ctx->outputs[0], main_buf);
    }
    return 0;
}

static int request_frame(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    AlphaChromakeyContext *keyer = ctx->priv;
    int in, ret;

    keyer->frame_requested = 1;
    while (keyer->frame_requested) {
        in = ff_bufqueue_peek(&keyer->queue_main, 0) ? 1 : 0;
        ret = ff_request_frame(ctx->inputs[in]);
        if (ret < 0)
            return ret;
    }
    return 0;
}

static const AVFilterPad alphachromakey_inputs[] = {
    {
        .name             = "main",
        .type             = AVMEDIA_TYPE_VIDEO,
        .config_props     = config_input_main,
        .get_video_buffer = ff_null_get_video_buffer,
        .filter_frame     = filter_frame,
        .min_perms        = AV_PERM_READ | AV_PERM_WRITE | AV_PERM_PRESERVE,
    },
    { NULL }
};

static const AVFilterPad alphachromakey_outputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_VIDEO,
        .config_props  = config_output,
        .request_frame = request_frame,
    },
    { NULL }
};

AVFilter avfilter_vf_alphachromakey = {
    .name           = "alphachromakey",
    .description    = NULL_IF_CONFIG_SMALL("Replace the alpha value of the "
                      "input by a chroma key."),
    .init           = init,
    .uninit         = uninit,
    .priv_size      = sizeof(AlphaChromakeyContext),
    .query_formats  = query_formats,
    .inputs         = alphachromakey_inputs,
    .outputs        = alphachromakey_outputs,
};
