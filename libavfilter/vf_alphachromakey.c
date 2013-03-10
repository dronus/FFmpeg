/*
 * Copyright (c) 2012 Steven Robertson
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
 * copy an alpha component from another video's luma
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
    int is_packed_rgb;
    uint8_t rgba_map[4];
    struct FFBufQueue queue_main;
    int u, uw, v, vw;
    char* alpha_expr;
} AlphaChromakeyContext;

static const char *const var_names[] = {
	"t",
	NULL
};


#define FLAGS AV_OPT_FLAG_VIDEO_PARAM|AV_OPT_FLAG_FILTERING_PARAM
static const AVOption alphachromakey_options[] = {
    {"u", "set the u center", offsetof(AlphaChromakeyContext,u ), AV_OPT_TYPE_INT, {.i64=0}, 0, 255, FLAGS },
    {"uw", "set the u width", offsetof(AlphaChromakeyContext,uw), AV_OPT_TYPE_INT, {.i64=0}, 0, 255, FLAGS },
    {"v", "set the v center", offsetof(AlphaChromakeyContext,v ), AV_OPT_TYPE_INT, {.i64=0}, 0, 255, FLAGS },
    {"vw", "set the v width", offsetof(AlphaChromakeyContext,vw), AV_OPT_TYPE_INT, {.i64=0}, 0, 255, FLAGS },
    {"a", "set alpha expression", offsetof(AlphaChromakeyContext,alpha_expr), AV_OPT_TYPE_STRING, {.str = NULL}, 0, 0, FLAGS },
    {NULL},
};
AVFILTER_DEFINE_CLASS(alphachromakey);

static const char *shorthand[] = { "u", "uw", "v","vw","alpha", NULL };

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
        AV_PIX_FMT_YUVA444P, AV_PIX_FMT_YUVA422P, AV_PIX_FMT_YUVA420P,
        AV_PIX_FMT_RGBA, AV_PIX_FMT_BGRA, AV_PIX_FMT_ARGB, AV_PIX_FMT_ABGR,
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
    keyer->is_packed_rgb =
        ff_fill_rgba_map(keyer->rgba_map, inlink->format) >= 0;
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

static int clamp (int x, int a, int b)
{
	return (x < a ? a :  (x>b ? b : x) );
}

static void draw_frame(AVFilterContext *ctx,
                       AVFilterBufferRef *main_buf)
{


    AlphaChromakeyContext *keyer = ctx->priv;

    
    FILE* file=fopen("uv.txt","r");
    if(file){
	char buffer[255];
	fgets(buffer,255,file);
        av_opt_set_from_string(keyer, buffer, shorthand, "=", ":");
	fclose(file);
    }

    double alpha=1;
    if(keyer->alpha_expr){
            AVFilterLink *inlink = ctx->inputs[0];
            
            double vars[]={TS2D(main_buf->pts) * av_q2d(inlink->time_base)};
	    int ret=av_expr_parse_and_eval(&alpha, keyer->alpha_expr,
		                   var_names, vars,
		                   NULL, NULL, NULL, NULL, NULL, 0, ctx);
	    if(ret<0){
		av_log(ctx, AV_LOG_ERROR, "Bad alpha expression.\n");    	
		return AVERROR(EINVAL);
	    }
    }
    int h = main_buf->video->h;
// TODO
    int x, y;
    if (keyer->is_packed_rgb) {
        int x, y;
        uint8_t *pin, *pout;
        for (y = 0; y < h; y++) {
            // pin = alpha_buf->data[0] + y * alpha_buf->linesize[0];
            pin  = main_buf->data[0] + y * main_buf->linesize[0] + keyer->rgba_map[Y];
            pout = main_buf->data[0] + y * main_buf->linesize[0] + keyer->rgba_map[A];
            for (x = 0; x < main_buf->video->w; x++) {
                *pout = *pin;
                pin += 4;
                pout += 4;
            }
        }
    } else {
    	long sum_u=0, sum_v=0, count=0;
        for (y = 0; y < h; y++) {
            int yfact = (main_buf->format==AV_PIX_FMT_YUVA422P ? y : y/2);
            uint8_t* in_u  = main_buf->data[U] + yfact  * main_buf->linesize[U];
            uint8_t* in_v  = main_buf->data[V] + yfact  * main_buf->linesize[V];
            uint8_t* pout  = main_buf->data[A] + y * main_buf->linesize[A];
            for (int x = 0; x < main_buf->linesize[A]; x++) {
            	
            	/*
            	// variant 1: abs ramp with offset and scale
            	int du=abs(in_u[x/2] - keyer->u)*keyer->uw/16;
            	int dv=abs(in_v[x/2] - keyer->v)*keyer->vw/16;
            	int r=du+dv-keyer->offset;
            	pout[x] = (uint8_t) clamp(r,0,255);
            	*/
            	
            	// variant 2: radius threshold with feather, double precision
            	double du=in_u[x/2]/256.0 - keyer->u/256.0, dv=in_v[x/2]/256.0 - keyer->v/256.0;
            	double r=sqrt(du*du+dv*dv);
            	double tola=keyer->uw/256.0;
            	double tolb=keyer->vw/256.0;
            	if (r<tola)      r=0;
            	else if (r<tolb) r=(r-tola)/(tolb-tola);
            	else             r=1;
            	r*=alpha;
            	pout[x]=(int)(r*255);
            	
            	// variant 3: squared radius threshold with feather, integer
     /*       	int du=in_u[x/2] - (int)keyer->u, dv=in_v[x/2] - (int)keyer->v;
            	int r2=sqrt(du*du+dv*dv);
             	int tola2=keyer->uw;
            	int tolb2=keyer->vw;
 
            	if      (r2<tola2) r2=0;
            	else if (r2<tolb2) r2=(r2-tola2)*256 / (tolb2-tola2);
            	else               r2=255;
            	pout[x]=r2;            	*/
            	
            	sum_u+=in_u[x/2]; 
            	sum_v+=in_v[x/2];
           	count++;            	
            	
                /* if(x % 4 ==0){
                    in_u += 1;
                    in_v += 1;
                }
                in_y += 1;*/
//                pout += 1;
            }
        }
        printf("u:%ld v:%ld\n",sum_u/count, sum_v/count);
    }
}

static int filter_frame(AVFilterLink *inlink, AVFilterBufferRef *buf)
{
    AVFilterContext *ctx = inlink->dst;
    AlphaChromakeyContext *keyer = ctx->priv;

    struct FFBufQueue *queue = &keyer->queue_main;
    ff_bufqueue_add(ctx, queue, buf);

    while (1) {
        AVFilterBufferRef *main_buf;

        if (!ff_bufqueue_peek(&keyer->queue_main, 0)) break;

        main_buf = ff_bufqueue_get(&keyer->queue_main);

        keyer->frame_requested = 0;
        draw_frame(ctx, main_buf);
        ff_filter_frame(ctx->outputs[0], main_buf);
        // avfilter_unref_buffer(alpha_buf);
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
