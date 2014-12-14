/* TFTP Engine
 *
 * Copyright (c) 2014  Joachim Nilsson <troglobit@gmail.com>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <poll.h>
#include "uftpd.h"

/* Send @len bytes data in @ctrl->buf */
static int do_send(ctrl_t *ctrl, size_t len)
{
	int     result;
	size_t  hdrsz = ctrl->th->th_msg - ctrl->buf;
	size_t  salen = sizeof(struct sockaddr_in);

	if (ctrl->client_sa.ss_family == AF_INET6)
		salen = sizeof(struct sockaddr_in6);

	if (ctrl->th->th_opcode == OACK)
		hdrsz = ctrl->th->th_stuff - ctrl->buf;

	DBG("tftp sending %zd + %zd bytes ...", hdrsz, len);
	result = sendto(ctrl->sd, ctrl->buf, hdrsz + len, 0, (struct sockaddr *)&ctrl->client_sa, salen);
	if (-1 == result)
		return 1;

	return 0;
}

/* If @block is non-zero, resend that block */
static int send_DATA(ctrl_t *ctrl, int block)
{
	size_t  len;

	memset(ctrl->buf, 0, ctrl->bufsz);

	/* Create message */
	ctrl->th->th_opcode = htons(DATA);
	if (block) {
		int pos = (block - 1) * ctrl->segsize;

		ctrl->th->th_block = htons(block);
		if (-1 == fseek(ctrl->fp, pos, SEEK_SET)) {
			ERR(errno, "Failed resending block");
			return 1;
		}
	} else {
		ctrl->th->th_block = htons((ftell(ctrl->fp) / ctrl->segsize) + 1);
	}

	DBG("tftp block %d reading %zd bytes ...", ctrl->th->th_block, ctrl->segsize);
	len = fread(ctrl->th->th_data, sizeof(char), ctrl->segsize, ctrl->fp);

	return do_send(ctrl, len);
}

#if 0 /* TODO, for client op */
static int send_ACK(ctrl_t *ctrl)
{
	return 0;
}
#endif

/* Acknowledge options sent by client */
static int send_OACK(ctrl_t *ctrl)
{
	char *ptr;

	if (!ctrl->tftp_options)
		return 0;

	memset(ctrl->buf, 0, ctrl->bufsz);

	/* Create message */
	ctrl->th->th_opcode = htons(OACK);

	ptr = &ctrl->th->th_stuff[0];
	if (isset(&ctrl->tftp_options, 1)) {
		ptr += sprintf(ptr, "blksize");
		ptr ++;

		ptr += sprintf(ptr, "%zd", ctrl->segsize);
		ptr ++;
	}

	return do_send(ctrl, ptr - ctrl->buf);
}

static int send_ERROR(ctrl_t *ctrl, int code)
{
	char   *str = strerror(code);
	size_t  len = strlen(str);

	memset(ctrl->buf, 0, ctrl->segsize);

	/* Create error message */
	ctrl->th->th_opcode = htons(ERROR);
	ctrl->th->th_code   = htons(code);
	strlcpy(ctrl->th->th_msg, str, len);

	/* Error is ASCIIZ string, hence +1 */
	return do_send(ctrl, len + 1);
}

static int alloc_buf(ctrl_t *ctrl, size_t segsize)
{
	if (!ctrl) {
		errno = EINVAL;
		return 1;
	}

	ctrl->segsize = segsize;
	ctrl->bufsz   = sizeof(tftp_t) + ctrl->segsize;

	if (ctrl->buf)
		ctrl->buf = realloc(ctrl->buf, ctrl->bufsz);
	else
		ctrl->buf = malloc(ctrl->bufsz);

	if (!ctrl->buf)
		return 1;

	ctrl->th = (tftp_t *)ctrl->buf;

	return 0;
}

/* Parse TFTP payload in RRQ to get filename and optional blksize & timeout */
static int parse_RRQ(ctrl_t *ctrl, char *buf, size_t len)
{
	size_t opt_len = strlen(buf) + 1;

	/* First opt is always filename */
	ctrl->file = strdup(buf);

	do {
		/* Prepare to read options */
		buf += opt_len;
		len -= opt_len;
		opt_len = strlen(buf) + 1;

		if (!strncasecmp(buf, "blksize", 7)) {
			size_t sz = 0;

			buf += opt_len;
			len -= opt_len;
			opt_len = strlen(buf) + 1;

			sscanf(buf, "%zd", &sz);
			if (sz < MIN_SEGSIZE)
				continue; /* Ignore if too small for us. */

			if (alloc_buf(ctrl, sz)) {
				ERR(errno, "Failed reallocating TFTP buffer memory");
				return send_ERROR(ctrl, ENOMEM);
			}

			setbit(&ctrl->tftp_options, 1);
		}
	} while (len);

	return send_OACK(ctrl);
}

static int handle_RRQ(ctrl_t *ctrl)
{
	char *path = compose_path(ctrl, ctrl->file);

	ctrl->fp = fopen(path, "r");
	if (!ctrl->fp) {
		ERR(errno, "Failed opening %s", path);
		return send_ERROR(ctrl, ENOTFOUND);
	}

	return !send_DATA(ctrl, 0);
}

/* TODO: Add support for ACK timeout and resend */
static int handle_ACK(ctrl_t *ctrl, int block)
{
	if (ctrl->fp) {
		if (feof(ctrl->fp)) {
			fclose(ctrl->fp);
			ctrl->fp = NULL;
			return 0;
		}

		DBG("ACK block %d, file still open ... ", block);
		return !send_DATA(ctrl, 0);
	}

	return 0;
}

void tftp_command(ctrl_t *ctrl)
{
	int active = 1;

	DBG("Entering %s() ...", __func__);

	/* Default buffer and segment size */
	if (alloc_buf(ctrl, SEGSIZE)) {
		ERR(errno, "Failed allocating TFTP buffer memory");
		return;
	}

	while (active) {
		ssize_t    len;
		uint16_t   port, op, block;
		socklen_t  addr_len = sizeof(ctrl->client_sa);
		struct pollfd pfd = {
			.fd     = ctrl->sd,
			.events = POLLIN | POLLPRI
		};

		errno = 0;
		if (poll(&pfd, 1, INACTIVITY_TIMER * 1000) <= 0) {
			ERR(errno, "Error or timeout waiting for client");
			break;
		}

		memset(ctrl->buf, 0, ctrl->bufsz);

		DBG("Reading TFTP request ... ");
		len = recvfrom(ctrl->sd, ctrl->buf, ctrl->bufsz, 0, (struct sockaddr *)&ctrl->client_sa, &addr_len);
		if (-1 == len) {
			if (errno == EINTR || errno == EAGAIN)
				break;

			ERR(errno, "Failed reading command/status from client");
			break;
		}

		convert_address(&ctrl->client_sa, ctrl->clientaddr, sizeof(ctrl->clientaddr));
		port   = ntohs(((struct sockaddr_in *)&ctrl->client_sa)->sin_port);
		op     = ntohs(ctrl->th->th_opcode);
		block  = ntohs(ctrl->th->th_block);

		switch (op) {
		case RRQ:
			len -= ctrl->th->th_stuff - ctrl->buf;
			if (parse_RRQ(ctrl, ctrl->th->th_stuff, len)) {
				ERR(errno, "Failed parsing TFTP RRQ");
				active = 0;
				break;
			}
			DBG("tftp RRQ %s from %s:%d", ctrl->file, ctrl->clientaddr, port);
			active = handle_RRQ(ctrl);
			free(ctrl->file);
			break;

		case ERROR:
			DBG("tftp ERROR: %hd", ntohs(ctrl->th->th_code));
			active = 0;
			break;

		case ACK:
			DBG("tftp ACK, block # %hu", block);
			active = handle_ACK(ctrl, block);
			break;

		default:
			DBG("tftp opcode: %hd", op);
			DBG("tftp block#: %hu", block);
			break;
		}
	}

	DBG("Leaving %s() ...", __func__);
}

int tftp_session(int sd)
{
	int pid = 0;
	ctrl_t *ctrl;

	/* NULL can be error or parent process */
	ctrl = new_session(sd, &pid);
	if (!ctrl) {
		int status = 0;

		if (-1 == pid)
			return -1;

		waitpid(pid, &status, WUNTRACED | WCONTINUED);
		return WEXITSTATUS(status);
	}

	tftp_command(ctrl);

	DBG("Exiting ...");
	del_session(ctrl);

	exit(0);
}


/**
 * Local Variables:
 *  version-control: t
 *  indent-tabs-mode: t
 *  c-file-style: "linux"
 * End:
 */

