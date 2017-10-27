/*
 *			GPAC - Multimedia Framework C SDK
 *
 *			Authors: Jean Le Feuvre
 *			Copyright (c) Telecom ParisTech 2000-2017
 *					All rights reserved
 *
 *  This file is part of GPAC / AMR&EVRC&SMV reframer filter
 *
 *  GPAC is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  GPAC is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#include <gpac/filters.h>
#include <gpac/constants.h>

typedef struct
{
	u64 pos;
	Double duration;
} AMRIdx;

typedef struct
{
	//filter args
	Double index_dur;

	//only one input pid declared
	GF_FilterPid *ipid;
	//only one output pid declared
	GF_FilterPid *opid;

	u32 start_offset;
	u32 oti, sample_rate, block_size;


	u64 file_pos, cts;

	GF_Fraction duration;
	Double start_range;
	Bool in_seek;
	u32 timescale;
	Bool is_playing;
	Bool is_file;
	Bool initial_play_done, file_loaded;
	Bool skip_magic;

	u32 hdr;
	u32 resume_from;
	u32 remaining;

	AMRIdx *indexes;
	u32 index_alloc_size, index_size;
} GF_AMRDmxCtx;




GF_Err amrdmx_configure_pid(GF_Filter *filter, GF_FilterPid *pid, Bool is_remove)
{
	const GF_PropertyValue *p;
	GF_AMRDmxCtx *ctx = gf_filter_get_udta(filter);

	if (is_remove) {
		ctx->ipid = NULL;
		gf_filter_pid_remove(ctx->opid);
		return GF_OK;
	}
	if (! gf_filter_pid_check_caps(pid))
		return GF_NOT_SUPPORTED;

	ctx->ipid = pid;
	p = gf_filter_pid_get_property(pid, GF_PROP_PID_TIMESCALE);
	if (p) ctx->timescale = p->value.uint;

	ctx->start_offset = 6;
	ctx->sample_rate = 8000;
	ctx->block_size = 160;

	p = gf_filter_pid_get_property(pid, GF_PROP_PID_OTI);
	if (p) {
		if (ctx->oti && (ctx->oti != p->value.uint)) {
			return GF_NOT_SUPPORTED;
		}
		ctx->oti = p->value.uint;
		if (ctx->oti == GPAC_OTI_AUDIO_AMR_WB) {
			ctx->sample_rate = 16000;
			ctx->block_size = 320;
		}
	}
	return GF_OK;
}

static void amrdmx_check_dur(GF_Filter *filter, GF_AMRDmxCtx *ctx)
{
	FILE *stream;
	u32 i;
	u64 duration, cur_dur;
	char magic[20];
	const GF_PropertyValue *p;
	if (!ctx->opid || ctx->timescale || ctx->file_loaded) return;

	p = gf_filter_pid_get_property(ctx->ipid, GF_PROP_PID_FILEPATH);
	if (!p || !p->value.string) {
		ctx->is_file = GF_FALSE;
		return;
	}
	ctx->is_file = GF_TRUE;

	stream = gf_fopen(p->value.string, "r");
	if (!stream) return;

	ctx->oti = 0;
	ctx->start_offset = 6;
	ctx->sample_rate = 8000;
	ctx->block_size = 160;
	i = (u32) fread(magic, 1, 20, stream);
	if (i != 20) return;

	if (!strnicmp(magic, "#!AMR\n", 6)) {
		fseek(stream, 6, SEEK_SET);
		ctx->oti = GPAC_OTI_AUDIO_AMR;
	}
	else if (!strnicmp(magic, "#!EVRC\n", 7)) {
		fseek(stream, 7, SEEK_SET);
		ctx->start_offset = 7;
		ctx->oti = GPAC_OTI_AUDIO_EVRC_VOICE;
	}
	else if (!strnicmp(magic, "#!SMV\n", 6)) {
		fseek(stream, 6, SEEK_SET);
		ctx->oti = GPAC_OTI_AUDIO_SMV_VOICE;
	}
	else if (!strnicmp(magic, "#!AMR-WB\n", 9)) {
		ctx->oti = GPAC_OTI_AUDIO_AMR_WB;
		ctx->start_offset = 9;
		ctx->sample_rate = 16000;
		ctx->block_size = 320;
		fseek(stream, 9, SEEK_SET);
	}
	else if (!strnicmp(magic, "#!AMR_MC1.0\n", 12)) return;
	else if (!strnicmp(magic, "#!AMR-WB_MC1.0\n", 15)) return;
	else return;

	ctx->index_size = 0;

	duration = 0;
	while (!feof(stream)) {
		u32 size=0;
		u64 pos;
		u8 toc, ft;
		toc = fgetc(stream);

		switch (ctx->oti) {
		case GPAC_OTI_AUDIO_AMR:
			ft = (toc >> 3) & 0x0F;
			size = (u32)GF_AMR_FRAME_SIZE[ft];
			break;
		case GPAC_OTI_AUDIO_AMR_WB:
			ft = (toc >> 3) & 0x0F;
			size = (u32)GF_AMR_WB_FRAME_SIZE[ft];
			break;
		default:
			for (i=0; i<GF_SMV_EVRC_RATE_TO_SIZE_NB; i++) {
				if (GF_SMV_EVRC_RATE_TO_SIZE[2*i]==toc) {
					/*remove rate_type byte*/
					size = (u32)GF_SMV_EVRC_RATE_TO_SIZE[2*i+1] - 1;
					break;
				}
			}
			break;
		}
		duration += ctx->block_size;
		cur_dur += ctx->block_size;
		pos = gf_ftell(stream);
		if (cur_dur > ctx->index_dur * ctx->sample_rate) {
			if (!ctx->index_alloc_size) ctx->index_alloc_size = 10;
			else if (ctx->index_alloc_size == ctx->index_size) ctx->index_alloc_size *= 2;
			ctx->indexes = gf_realloc(ctx->indexes, sizeof(AMRIdx)*ctx->index_alloc_size);
			ctx->indexes[ctx->index_size].pos = pos - 1;
			ctx->indexes[ctx->index_size].duration = duration;
			ctx->indexes[ctx->index_size].duration /= ctx->sample_rate;
			ctx->index_size ++;
			cur_dur = 0;
		}
		if (size) gf_fseek(stream, size, SEEK_CUR);
	}
	gf_fclose(stream);

	if (!ctx->duration.num || (ctx->duration.num  * ctx->sample_rate != duration * ctx->duration.den)) {
		ctx->duration.num = duration;
		ctx->duration.den = ctx->sample_rate;

		gf_filter_pid_set_info(ctx->opid, GF_PROP_PID_DURATION, & PROP_FRAC(ctx->duration));
	}

	p = gf_filter_pid_get_property(ctx->ipid, GF_PROP_PID_FILE_CACHED);
	if (p && p->value.boolean) ctx->file_loaded = GF_TRUE;
}

static void amrdmx_check_pid(GF_Filter *filter, GF_AMRDmxCtx *ctx)
{
	if (ctx->opid) return;
	ctx->opid = gf_filter_pid_new(filter);
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_STREAM_TYPE, & PROP_UINT( GF_STREAM_AUDIO));

	amrdmx_check_dur(filter, ctx);

	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_TIMESCALE, & PROP_UINT(ctx->sample_rate));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_SAMPLE_RATE, & PROP_UINT(ctx->sample_rate));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_NUM_CHANNELS, & PROP_UINT(1) );
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_OTI, & PROP_UINT(ctx->oti ) );
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_SAMPLES_PER_FRAME, & PROP_UINT(ctx->block_size ) );

}

static Bool amrdmx_process_event(GF_Filter *filter, const GF_FilterEvent *evt)
{
	u32 i;
	GF_FilterEvent fevt;
	GF_AMRDmxCtx *ctx = gf_filter_get_udta(filter);

	switch (evt->base.type) {
	case GF_FEVT_PLAY:
		if (!ctx->is_playing) {
			ctx->is_playing = GF_TRUE;
			ctx->cts = 0;
			ctx->remaining = 0;
		}
		if (! ctx->is_file) {
			return GF_FALSE;
		}
		amrdmx_check_dur(filter, ctx);

		ctx->start_range = evt->play.start_range;
		ctx->in_seek = GF_TRUE;
		ctx->file_pos = 0;
		if (ctx->start_range) {
			for (i=1; i<ctx->index_size; i++) {
				if (ctx->indexes[i].duration>ctx->start_range) {
					ctx->cts = ctx->indexes[i-1].duration * ctx->sample_rate;
					ctx->file_pos = ctx->indexes[i-1].pos;
					break;
				}
			}
		}
		if (!ctx->initial_play_done) {
			ctx->initial_play_done = GF_TRUE;
			//seek will not change the current source state, don't send a seek
			if (!ctx->file_pos) {
				ctx->skip_magic = GF_TRUE;
				return GF_TRUE;
			}
		}
		//post a seek
		GF_FEVT_INIT(fevt, GF_FEVT_SOURCE_SEEK, ctx->ipid);
		if (!ctx->file_pos)
			ctx->skip_magic = GF_TRUE;
		fevt.seek.start_offset = ctx->file_pos;
		gf_filter_pid_send_event(ctx->ipid, &fevt);

		//cancel event
		return GF_TRUE;

	case GF_FEVT_STOP:
		ctx->is_playing = GF_FALSE;
		//don't cancel event
		return GF_FALSE;

	case GF_FEVT_SET_SPEED:
		//cancel event
		return GF_TRUE;
	default:
		break;
	}
	//by default don't cancel event - to rework once we have downloading in place
	return GF_FALSE;
}

static GFINLINE void amrdmx_update_cts(GF_AMRDmxCtx *ctx)
{
	if (ctx->timescale) {
		u64 inc = ctx->block_size;
		inc *= ctx->timescale;
		inc /= ctx->sample_rate;
		ctx->cts += inc;
	} else {
		ctx->cts += ctx->block_size;
	}
}

GF_Err amrdmx_process(GF_Filter *filter)
{
	GF_AMRDmxCtx *ctx = gf_filter_get_udta(filter);
	GF_FilterPacket *pck, *dst_pck;
	u64 byte_offset;
	char *data, *output;
	u8 *start;
	u32 pck_size, remain;

	//update duration
	amrdmx_check_dur(filter, ctx);

	if (ctx->opid && !ctx->is_playing)
		return GF_OK;

	pck = gf_filter_pid_get_packet(ctx->ipid);
	if (!pck) {
		if (gf_filter_pid_is_eos(ctx->ipid)) {
			gf_filter_pid_set_eos(ctx->opid);
			assert(ctx->remaining == 0);
			return GF_EOS;
		}
		return GF_OK;
	}

	data = (char *) gf_filter_pck_get_data(pck, &pck_size);
	byte_offset = gf_filter_pck_get_byte_offset(pck);

	start = data;
	remain = pck_size;

	//flush not previously dispatched data
	if (ctx->remaining) {
		u32 to_send = ctx->remaining;
		if (ctx->remaining > pck_size) {
			to_send = pck_size;
			ctx->remaining -= pck_size;
		} else {
			ctx->remaining = 0;
		}
		if (! ctx->in_seek) {
			dst_pck = gf_filter_pck_new_alloc(ctx->opid, to_send, &output);
			memcpy(output, data, to_send);

			gf_filter_pck_set_cts(dst_pck, ctx->cts);
			gf_filter_pck_set_framing(dst_pck, GF_FALSE, ctx->remaining ? GF_FALSE : GF_TRUE);

			gf_filter_pck_send(dst_pck);
		}

		if (ctx->remaining) {
			gf_filter_pid_drop_packet(ctx->ipid);
			return GF_OK;
		}
		amrdmx_update_cts(ctx);
		start += to_send;
		remain -= to_send;
	}

	//input pid sets some timescale - we flushed pending data , update cts
	if (ctx->timescale) {
		u64 cts = gf_filter_pck_get_cts(pck);
		if (cts != GF_FILTER_NO_TS)
			ctx->cts = ctx;
	}
	if (ctx->skip_magic) {
		start += ctx->start_offset;
		remain -= ctx->start_offset;
	}
	if (ctx->resume_from) {
		start += ctx->resume_from;
		remain -= ctx->resume_from;
		ctx->resume_from = 0;
	}


	while (remain) {
		u8 toc, ft;
		u32 size=0, i;

		toc = start[0];
		if (!toc) {
			GF_LOG(GF_LOG_INFO, GF_LOG_MEDIA, ("[AMRDmx] Could not find TOC word in packet, droping\n"));
			break;
		}
		switch (ctx->oti) {
		case GPAC_OTI_AUDIO_AMR:
			ft = (toc >> 3) & 0x0F;
			size = (u32)GF_AMR_FRAME_SIZE[ft];
			break;
		case GPAC_OTI_AUDIO_AMR_WB:
			ft = (toc >> 3) & 0x0F;
			size = (u32)GF_AMR_WB_FRAME_SIZE[ft];
			break;
		default:
			for (i=0; i<GF_SMV_EVRC_RATE_TO_SIZE_NB; i++) {
				if (GF_SMV_EVRC_RATE_TO_SIZE[2*i]==toc) {
					/*remove rate_type byte*/
					size = (u32)GF_SMV_EVRC_RATE_TO_SIZE[2*i+1] - 1;
					break;
				}
			}
			break;
		}

		if (!size) {
			GF_LOG(GF_LOG_INFO, GF_LOG_MEDIA, ("[AMRDmx] Broken TOC, trying resync\n"));
			start++;
			remain--;
			continue;
		}
		//ready to send packet
		amrdmx_check_pid(filter, ctx);

		if (!ctx->is_playing) return GF_OK;
		size++;
		if (size > remain) {
			ctx->remaining = size - remain;
			size = remain;
		}

		if (ctx->in_seek) {
			u64 nb_samples_at_seek = ctx->start_range * ctx->sample_rate;
			if (ctx->cts + ctx->block_size >= nb_samples_at_seek) {
				//u32 samples_to_discard = (ctx->cts + ctx->block_size ) - nb_samples_at_seek;
				ctx->in_seek = GF_FALSE;
			}
		}
		if (!ctx->in_seek) {
			dst_pck = gf_filter_pck_new_alloc(ctx->opid, size, &output);

			memcpy(output, start, size);

			gf_filter_pck_set_cts(dst_pck, ctx->cts);
			gf_filter_pck_set_duration(dst_pck, ctx->block_size);
			gf_filter_pck_set_framing(dst_pck, GF_TRUE, ctx->remaining ? GF_FALSE : GF_TRUE);

			if (byte_offset != GF_FILTER_NO_BO) {
				u64 boffset = byte_offset;
				boffset += (char *) start - data;
				gf_filter_pck_set_byte_offset(dst_pck, boffset);
			}

			gf_filter_pck_send(dst_pck);
		}
		start += size;
		remain -= size;

		ctx->skip_magic = 0;
		if (ctx->remaining) break;
		amrdmx_update_cts(ctx);

		//don't demux too much of input, abort when we would block. This avoid dispatching
		//a huge number of frames in a single call
		if (gf_filter_pid_would_block(ctx->opid)) {
			ctx->resume_from = (char *)start -  (char *)data;
			return GF_OK;
		}
	}
	gf_filter_pid_drop_packet(ctx->ipid);

	return GF_OK;
}

static void amrdmx_finalize(GF_Filter *filter)
{
	GF_AMRDmxCtx *ctx = gf_filter_get_udta(filter);
	if (ctx->indexes) gf_free(ctx->indexes);
}

static const GF_FilterCapability AMRDmxInputs[] =
{
	{},
	CAP_INC_STRING(GF_PROP_PID_MIME, "audio/amr|audio/evrc|audio/smv"),
	{},
	CAP_INC_STRING(GF_PROP_PID_FILE_EXT, "amr|awb|evc|smv"),
};

static const GF_FilterCapability AMRDmxOutputs[] =
{
	CAP_INC_UINT(GF_PROP_PID_STREAM_TYPE, GF_STREAM_AUDIO),
	CAP_INC_UINT(GF_PROP_PID_OTI, GPAC_OTI_AUDIO_AMR),
	CAP_INC_UINT(GF_PROP_PID_OTI, GPAC_OTI_AUDIO_AMR_WB),
	CAP_INC_UINT(GF_PROP_PID_OTI, GPAC_OTI_AUDIO_SMV_VOICE),
	CAP_INC_UINT(GF_PROP_PID_OTI, GPAC_OTI_AUDIO_EVRC_VOICE),
	{}
};

#define OFFS(_n)	#_n, offsetof(GF_AMRDmxCtx, _n)
static const GF_FilterArgs AMRDmxArgs[] =
{
	{ OFFS(index_dur), "indexing window length", GF_PROP_DOUBLE, "1.0", NULL, GF_FALSE},
	{}
};


GF_FilterRegister AMRDmxRegister = {
	.name = "reframe_amr",
	.description = "AMR/EVRC Demux",
	.private_size = sizeof(GF_AMRDmxCtx),
	.args = AMRDmxArgs,
	.finalize = amrdmx_finalize,
	INCAPS(AMRDmxInputs),
	OUTCAPS(AMRDmxOutputs),
	.configure_pid = amrdmx_configure_pid,
	.process = amrdmx_process,
	.process_event = amrdmx_process_event
};


const GF_FilterRegister *amrdmx_register(GF_FilterSession *session)
{
	return &AMRDmxRegister;
}
