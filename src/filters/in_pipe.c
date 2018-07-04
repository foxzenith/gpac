/*
 *			GPAC - Multimedia Framework C SDK
 *
 *			Authors: Jean Le Feuvre
 *			Copyright (c) Telecom ParisTech 2018
 *					All rights reserved
 *
 *  This file is part of GPAC / pipe input filter
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

#ifdef WIN32

#else

#include <fcntl.h>
#include <unistd.h>

#ifndef __BEOS__
#include <errno.h>
#endif

#endif


typedef struct
{
	//options
	char *src;
	char *ext;
	char *mime;
	u32 block_size;
	Bool blk, nc, mkp;

	//only one output pid declared
	GF_FilterPid *pid;

	int fd;
	u64 bytes_read;

	Bool is_end, pck_out, is_first, owns_pipe;
	Bool do_reconfigure;
	char *buffer;
} GF_PipeInCtx;

static char szErrString[100];

const char *gf_errno_get_string(int errnoval)
{
	switch (errnoval) {
	case EPERM: return "Operation not permitted";
	case ENOENT: return "No such file or directory";
	case ESRCH: return "No such process";
	case EINTR: return "Interrupted system call";
	case EIO: return "Input/output error";
	case ENXIO: return "Device not configured";
	case E2BIG: return "Argument list too long";
	case ENOEXEC: return "Exec format error";
	case EBADF: return "Bad file descriptor";
	case ECHILD: return "No child processes";
	case EDEADLK: return "Resource deadlock avoided";
	case ENOMEM: return "Cannot allocate memory";
	case EACCES: return "Permission denied";
	case EFAULT: return "Bad address";
	case EBUSY: return "Device / Resource busy";
	case EEXIST: return "File exists";
	case EXDEV: return "Cross-device link";
	case ENODEV: return "Operation not supported by device";
	case ENOTDIR: return "Not a directory";
	case EISDIR: return "Is a directory";
	case EINVAL: return "Invalid argument";
	case ENFILE: return "Too many open files in system";
	case EMFILE: return "Too many open files";
	case ENOTTY: return "Inappropriate ioctl for device";
	case EFBIG: return "File too large";
	case ENOSPC: return "No space left on device";
	case ESPIPE: return "Illegal seek";
	case EROFS: return "Read-only file system";
	case EPIPE: return "Broken pipe";
	case EAGAIN: return "Operation would block";
	case EINPROGRESS: return "Operation now in progress";
	case EALREADY: return "Operation already in progress";

	default: sprintf(szErrString, "Unknown error (%d)", errnoval); return szErrString;
	}
}

static GF_Err pipein_initialize(GF_Filter *filter)
{
	GF_PipeInCtx *ctx = (GF_PipeInCtx *) gf_filter_get_udta(filter);
	char *frag_par = NULL;
	char *cgi_par = NULL;
	char *src;

	if (!ctx || !ctx->src) return GF_BAD_PARAM;

	if (strnicmp(ctx->src, "pipe:/", 6) && strstr(ctx->src, "://"))  {
		gf_filter_setup_failure(filter, GF_NOT_SUPPORTED);
		return GF_NOT_SUPPORTED;
	}


	//strip any fragment identifer
	frag_par = strchr(ctx->src, '#');
	if (frag_par) frag_par[0] = 0;
	cgi_par = strchr(ctx->src, '?');
	if (cgi_par) cgi_par[0] = 0;

	src = (char *) ctx->src;
	if (!strnicmp(ctx->src, "pipe://", 7)) src += 7;
	else if (!strnicmp(ctx->src, "pipe:", 5)) src += 5;

	if (!gf_file_exists(src) && ctx->mkp) {
#ifdef WIN32
#elif defined(GPAC_CONFIG_DARWIN)
		mknod(src,S_IFIFO | 0666, 0);
#else
		mkfifo(src, 0666);
#endif
		ctx->owns_pipe = GF_TRUE;
	}

#ifdef WIN32
#else
	ctx->fd = open(src, ctx->blk ? O_RDONLY : O_RDONLY|O_NONBLOCK );
#endif

	if (ctx->fd<0) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_MMIO, ("[PipeIn] Failed to open %s: %s\n", src, gf_errno_get_string(errno) ));

		if (frag_par) frag_par[0] = '#';
		if (cgi_par) cgi_par[0] = '?';

		gf_filter_setup_failure(filter, GF_URL_ERROR);
		ctx->owns_pipe = GF_FALSE;
		return GF_URL_ERROR;
	}
	GF_LOG(GF_LOG_INFO, GF_LOG_MMIO, ("[PipeIn] opening %s\n", src));

	ctx->is_end = GF_FALSE;

	if (frag_par) frag_par[0] = '#';
	if (cgi_par) cgi_par[0] = '?';

	ctx->is_first = GF_TRUE;
	if (!ctx->buffer)
		ctx->buffer = gf_malloc(ctx->block_size +1);

	gf_filter_post_process_task(filter);
	return GF_OK;
}

static void pipein_finalize(GF_Filter *filter)
{
	GF_PipeInCtx *ctx = (GF_PipeInCtx *) gf_filter_get_udta(filter);

#ifdef WIN32
#else
	if (ctx->fd>=0) close(ctx->fd);
#endif
	if (ctx->buffer) gf_free(ctx->buffer);

	if (ctx->owns_pipe)
		gf_delete_file(ctx->src);

}

static GF_FilterProbeScore pipein_probe_url(const char *url, const char *mime_type)
{
	char *frag_par = NULL;
	char *cgi_par = NULL;
	char *src = (char *) url;
	Bool res;
	if (!strnicmp(url, "pipe://", 7)) src += 7;
	else if (!strnicmp(url, "pipe:", 5)) src += 5;

	//strip any fragment identifer
	frag_par = strchr(url, '#');
	if (frag_par) frag_par[0] = 0;
	cgi_par = strchr(url, '?');
	if (cgi_par) cgi_par[0] = 0;

	res = gf_file_exists(src);

	if (frag_par) frag_par[0] = '#';
	if (cgi_par) cgi_par[0] = '?';

	return res ? GF_FPROBE_SUPPORTED : GF_FPROBE_NOT_SUPPORTED;
}

static Bool pipein_process_event(GF_Filter *filter, const GF_FilterEvent *evt)
{
	GF_PipeInCtx *ctx = (GF_PipeInCtx *) gf_filter_get_udta(filter);

	if (evt->base.on_pid && (evt->base.on_pid != ctx->pid))
		return GF_FALSE;

	switch (evt->base.type) {
	case GF_FEVT_PLAY:
		return GF_TRUE;
	case GF_FEVT_STOP:
		//stop sending data
		ctx->is_end = GF_TRUE;
		return GF_TRUE;
	case GF_FEVT_SOURCE_SEEK:
		GF_LOG(GF_LOG_WARNING, GF_LOG_MMIO, ("[PipeIn] Seek request not possible on pipes, ignoring\n"));
		return GF_TRUE;
	case GF_FEVT_SOURCE_SWITCH:
		assert(ctx->is_end);
		if (evt->seek.source_switch) {
			GF_LOG(GF_LOG_WARNING, GF_LOG_MMIO, ("[PipeIn] source switch request not possible on pipes, ignoring\n"));
		}
		pipein_initialize(filter);
		gf_filter_post_process_task(filter);
		break;
	default:
		break;
	}
	return GF_FALSE;
}


static void pipein_pck_destructor(GF_Filter *filter, GF_FilterPid *pid, GF_FilterPacket *pck)
{
	GF_PipeInCtx *ctx = (GF_PipeInCtx *) gf_filter_get_udta(filter);
	ctx->pck_out = GF_FALSE;
	//ready to process again
	gf_filter_post_process_task(filter);
}

GF_Err filein_declare_pid(GF_Filter *filter, GF_FilterPid **pid, const char *url, const char *local_file, const char *mime_type, const char *ext, char *probe_data, u32 probe_size);;

static GF_Err pipein_process(GF_Filter *filter)
{
	GF_Err e;
	u32 to_read;
	s32 nb_read;
	GF_FilterPacket *pck;
	GF_PipeInCtx *ctx = (GF_PipeInCtx *) gf_filter_get_udta(filter);

	if (ctx->is_end)
		return GF_EOS;

	//until packet is released we return EOS (no processing), and ask for processing again upon release
	if (ctx->pck_out)
		return GF_EOS;

	if (ctx->pid && gf_filter_pid_would_block(ctx->pid)) {
		assert(0);
		return GF_OK;
	}

	to_read = ctx->block_size;

	errno = 0;
	nb_read = (s32) read(ctx->fd, ctx->buffer, to_read);
	if (nb_read <= 0) {
		s32 res = errno;
		if (res == EAGAIN) {
			//non blocking pipe with writers active
		} else if (read<0) {
			GF_LOG(GF_LOG_ERROR, GF_LOG_MMIO, ("[PipeIn] Failed to read, error %s\n", gf_errno_get_string(res) ));
			return GF_IO_ERR;
		} else if (!ctx->nc) {
			GF_LOG(GF_LOG_DEBUG, GF_LOG_MMIO, ("[PipeIn] end of stream detected\n"));
			gf_filter_pid_set_eos(ctx->pid);
			close(ctx->fd);
			ctx->fd=-1;
			return GF_EOS;
		}
		return GF_OK;
	}

	ctx->buffer[nb_read] = 0;
	if (!ctx->pid || ctx->do_reconfigure) {
		ctx->do_reconfigure = GF_FALSE;
		e = filein_declare_pid(filter, &ctx->pid, ctx->src, NULL, ctx->mime, ctx->ext, ctx->buffer, nb_read);
		if (e) return e;
		gf_filter_pid_set_info(ctx->pid, GF_PROP_PID_FILE_CACHED, &PROP_BOOL(GF_FALSE) );
		gf_filter_pid_set_property(ctx->pid, GF_PROP_PID_PLAYBACK_MODE, &PROP_UINT(GF_PLAYBACK_MODE_NONE) );
	}
	pck = gf_filter_pck_new_shared(ctx->pid, ctx->buffer, nb_read, pipein_pck_destructor);
	if (!pck)
		return GF_OK;

	gf_filter_pck_set_framing(pck, ctx->is_first, ctx->is_end);
	gf_filter_pck_set_sap(pck, GF_FILTER_SAP_1);

	ctx->is_first = GF_FALSE;
	ctx->pck_out = GF_TRUE;
	gf_filter_pck_send(pck);
	ctx->bytes_read += nb_read;

	if (ctx->is_end) {
		gf_filter_pid_set_eos(ctx->pid);
		return GF_EOS;
	}
	return ctx->pck_out ? GF_EOS : GF_OK;
}



#define OFFS(_n)	#_n, offsetof(GF_PipeInCtx, _n)

static const GF_FilterArgs PipeInArgs[] =
{
	{ OFFS(src), "location of source content", GF_PROP_NAME, NULL, NULL, GF_FALSE},
	{ OFFS(block_size), "buffer size used to read file", GF_PROP_UINT, "5000", NULL, GF_FALSE},
	{ OFFS(ext), "indicates file extension of pipe data", GF_PROP_STRING, NULL, NULL, GF_FALSE},
	{ OFFS(mime), "indicates mime type of pipe data", GF_PROP_STRING, NULL, NULL, GF_FALSE},
	{ OFFS(blk), "opens pipe in block mode", GF_PROP_BOOL, "true", NULL, GF_FALSE},
	{ OFFS(nc), "do not close pipe if nothing is read - end of stream will never be triggered", GF_PROP_BOOL, "false", NULL, GF_FALSE},
	{ OFFS(mkp), "create pipe if not found - this will delete the pipe file upon destruction", GF_PROP_BOOL, "false", NULL, GF_FALSE},
	{0}
};

static const GF_FilterCapability PipeInCaps[] =
{
	CAP_UINT(GF_CAPS_OUTPUT,  GF_PROP_PID_STREAM_TYPE, GF_STREAM_FILE),
};

GF_FilterRegister PipeInRegister = {
	.name = "pin",
	.description = "pipe input",
	.comment = "This filter handles generic input pipes (mono-directionnal) in blocking or non blocking mode.\n"\
		"Input pipes cannot seek\n"\
		"The assoicated protocol scheme is pipe:// when loaded as a generic input (eg, -i pipe://URL where URL is a relative or absolute pipe name)\n"\
		"It can be set to run forever (until the session is closed), ignoring any potential pipe close on the writing side\n"\
		"Data format of the pipe must currently be specified using extension (either in file name or through ext option) or mime type\n",
	.private_size = sizeof(GF_PipeInCtx),
	.args = PipeInArgs,
	.initialize = pipein_initialize,
	SETCAPS(PipeInCaps),
	.finalize = pipein_finalize,
	.process = pipein_process,
	.process_event = pipein_process_event,
	.probe_url = pipein_probe_url
};


const GF_FilterRegister *pipein_register(GF_FilterSession *session)
{
	return &PipeInRegister;
}
