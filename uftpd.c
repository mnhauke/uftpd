/* uftpd -- the small no nonsense FTP server
 *
 * Copyright (c) 2013-2014  Xu Wang <wangxu.93@icloud.com>
 * Copyright (c)      2014  Joachim Nilsson <troglobit@gmail.com>
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

#include "uftpd.h"

/* Global daemon settings */
int   port        = 0;
char *home        = NULL;
char  inetd       = 0;
char  background  = 1;
char  debug       = 0;
char  verbose     = 0;
char  do_log      = 0;
char  do_tftp     = 0;
char *logfile     = NULL;
struct passwd *pw = NULL;

static int version(void)
{
	printf("Version %s\n", VERSION);
	return 0;
}

static int usage(void)
{
	printf("\nUsage: %s [-dfivV] [-l LOGFILE] [-p PORT]\n"
	       "\n"
	       "  -h | -?    Show this help text\n"
	       "  -d         Enable developer debug logs\n"
	       "  -f         Run in foreground, do not detach from calling terminal\n"
	       "  -i         Inetd mode, take client connections from stdin\n"
	       "  -l [PATH]  Log to stdout, an optional logfile, or default to syslog\n"
	       "  -p PORT    Port to serve files on, default %d\n"
	       "  -t         Enable TFTP service\n"
	       "  -v         Show program version\n"
	       "  -V         Verbose logging\n"
	       "\n"
	       "Bug report address: %-40s\n\n", __progname, FTP_DEFAULT_PORT, BUGADDR);

	return 0;
}

/*
 * SIGCHLD: one of our children has died
 */
static void sigchld_handler(int UNUSED(sig), siginfo_t *UNUSED(info), void *UNUSED(ctx))
{
	collect_sessions();
}

/*
 * SIGQUIT: request termination
 */
static void sigquit_handler(int sig, siginfo_t *UNUSED(info), void *UNUSED(ctx))
{
	INFO("Recieved signal %d, exiting ...", sig);
	killpg(0, SIGTERM);
	sleep(2);
	killpg(0, SIGKILL);
}

static void sig_init(void)
{
	struct sigaction sa;

	SETSIG(sa, SIGCHLD, sigchld_handler, SA_RESTART);
	SETSIG(sa, SIGTERM, sigquit_handler, SA_RESTART);
	SETSIG(sa, SIGQUIT, sigquit_handler, SA_RESTART);
	SETSIG(sa, SIGHUP, sigquit_handler, SA_RESTART);
	SETSIG(sa, SIGINT, sigquit_handler, SA_RESTART);
}

static void init(void)
{
	sig_init();

	if (!port) {
		struct servent *sv;

		sv = getservbyname(FTP_SERVICE_NAME, FTP_PROTO_NAME);
		if (!sv) {
			port = FTP_DEFAULT_PORT;
			WARN(errno, "Cannot find service %s/%s, falling back to port %d.",
			     FTP_SERVICE_NAME, FTP_PROTO_NAME, ntohs(port));
		} else {
			port = ntohs(sv->s_port);
		}
	}

	pw = getpwnam(FTP_DEFAULT_USER);
	if (!pw) {
		home = strdup(FTP_DEFAULT_HOME);
		WARN(errno, "Cannot find user %s, falling back to %s as FTP root.",
		     FTP_DEFAULT_USER, home);
	} else {
		home = strdup(pw->pw_dir);
	}
}

int main(int argc, char **argv)
{
	int c;

	while ((c = getopt (argc, argv, "h?dfil::p:tvV")) != EOF) {
		switch (c) {
		case 'd':
			debug = 1;
			break;

		case 'f':
			background = 0;
			break;

		case 'i':
			inetd = 1;
			break;

		case 'l':
			do_log = 1;
			if (optarg)
				logfile = strdup(optarg);
			break;

		case 'p':
			port = atoi(optarg);
			break;

		case 't':
			do_tftp = 1;
			break;

		case 'v':
			return version();

		case 'V':
			verbose = 1;
			break;

		default:
			return usage();
		}
	}

	if (!do_log) {
		do_log = 1;	/* Syslog */
		openlog(__progname, LOG_PID | LOG_NDELAY, LOG_FTP);
	} else {
		do_log = 0;	/* Stderr, or logfile if set */
	}

	init();
	if (inetd) {
		INFO("Started from inetd, serving files from %s ...", home);
		return start_session(STDIN_FILENO);
	}

	if (background) {
		if (-1 == daemon(0, 0)) {
			ERR(errno, "Failed daemonizing");
			return 1;
		}
	}

	return serve_files();
}

/**
 * Local Variables:
 *  version-control: t
 *  indent-tabs-mode: t
 *  c-file-style: "linux"
 * End:
 */
