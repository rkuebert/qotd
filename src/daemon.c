/*
 * daemon.c
 *
 * qotd - A simple QOTD daemon.
 * Copyright (c) 2015-2016 Ammon Smith
 *
 * qotd is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * qotd is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with qotd.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libgen.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include "arguments.h"
#include "daemon.h"
#include "info.h"
#include "journal.h"
#include "network.h"
#include "quotes.h"
#include "security.h"
#include "signal_handler.h"
#include "standard.h"

/* Static function declarations */
static int daemonize(void);
static int main_loop(void);
static void save_args(const int argc, const char *argv[]);
static void write_pidfile(void);
static void load_config(void);
static void check_config(void);

/* Static member declarations */
static struct options opt;
static bool wrote_pidfile;

static struct {
	int argc;
	const char **argv;
} arguments;

/* Function implementations */
int main(int argc, const char *argv[])
{
	/* Set up signal handlers */
	set_up_handlers();

#if DEBUG
	printf("(Running in debug mode)\n");
#endif /* DEBUG */

	/* Set default values for static variables */
	wrote_pidfile = false;

	/* Make a copy of the arguments */
	save_args(argc, argv);

	/* Load configuration and open journal */
	load_config();

	/* Check security settings */
	if (opt.strict) {
		security_options_check(&opt);
	}

	return opt.daemonize ? daemonize() : main_loop();
}

static int daemonize(void)
{
	pid_t pid;

	/* Fork to background */
	pid = fork();
	if (pid < 0) {
		journal("Unable to fork: %s.\n", strerror(errno));
		cleanup(EXIT_FAILURE, true);
	} else if (pid != 0) {
		/* If we're the parent, then quit */
		journal("Successfully created background daemon, pid %d.\n", pid);
		cleanup(EXIT_SUCCESS, true);
	}

	/* We're the child, so daemonize */
	pid = setsid();

	if (pid < 0) {
		journal("Unable to create new session: %s.\n", strerror(errno));
		cleanup(EXIT_FAILURE, true);
	}

	if (opt.chdir_root) {
		int ret = chdir("/");

		if (ret) {
			journal("Unable to chdir to root dir: %s.\n", strerror(errno));
		}
	}

	return main_loop();
}

static int main_loop(void)
{
	void (*accept_connection)(void);

	write_pidfile();

	switch (opt.iproto) {
	case PROTOCOL_BOTH:
	case PROTOCOL_IPv6:
		set_up_ipv6_socket(&opt);
		break;
	case PROTOCOL_IPv4:
		set_up_ipv4_socket(&opt);
		break;
	default:
		journal("Internal error: invalid enum value for \"iproto\": %d.\n", opt.iproto);
		cleanup(EXIT_INTERNAL, true);
	}

	if (opt.drop_privileges) {
		drop_privileges();
	}

	switch (opt.tproto) {
	case PROTOCOL_TCP:
		accept_connection = &tcp_accept_connection;
		break;
	case PROTOCOL_UDP:
		accept_connection = &udp_accept_connection;
		break;
	default:
		ERR_TRACE();
		journal("Internal error: invalid enum value for \"tproto\": %d.\n", opt.tproto);
		cleanup(EXIT_INTERNAL, true);
		return -1;
	}

	while (true) {
		accept_connection();
	}

	return EXIT_SUCCESS;
}

void cleanup(int retcode, bool quiet)
{
	int ret;

	if (!quiet) {
		journal("Quitting with exit code %d.\n", retcode);
	}

	if (wrote_pidfile) {
		ret = unlink(opt.pidfile);
		if (ret) {
			journal("Unable to remove pid file (%s): %s.\n",
				opt.pidfile, strerror(errno));
		}
	}

	if (opt.pidalloc) {
		FINAL_FREE((char *)opt.pidfile);
	}

	if (opt.quotesalloc) {
		FINAL_FREE((char *)opt.quotesfile);
	}

	destroy_quote_buffers();
	close_socket();
	close_journal();

	exit(retcode);
}

static void load_config(void)
{
	int ret;

	journal("Loading configuration settings...\n");
	parse_args(&opt, arguments.argc, arguments.argv);
	check_config();

	ret = open_quotes_file(&opt);
	if (ret) {
		journal("Unable to open quotes file: %s.\n", strerror(errno));
		cleanup(EXIT_IO, true);
	}
}

static void save_args(int argc, const char *argv[])
{
	arguments.argc = argc;
	arguments.argv = argv;
}

static void write_pidfile()
{
	struct stat statbuf;
	int ret;
	FILE *fh;

	if (opt.pidfile == NULL) {
		journal("No pidfile was written at the request of the user.\n");
		return;
	}

	/* Check if the pidfile already exists */
	ret = stat(opt.pidfile, &statbuf);
	if (ret) {
		if (errno != ENOENT) {
			journal("Unable to stat pid file \"%s\": %s.\n", opt.pidfile, strerror(errno));
			cleanup(EXIT_IO, true);
		}
	} else {
		journal("The pid file already exists. Quitting.\n");
		cleanup(EXIT_FAILURE, true);
	}

	/* Write the pidfile */
	fh = fopen(opt.pidfile, "w+");

	if (!fh) {
		journal("Unable to open pid file: %s.\n", strerror(errno));

		if (opt.require_pidfile) {
			cleanup(EXIT_IO, true);
		} else {
			return;
		}
	}

	ret = fprintf(fh, "%d\n", getpid());
	if (ret < 0) {
		ERR_TRACE();
		perror("Unable to write process id to pid file");

		if (opt.require_pidfile) {
			fclose(fh);
			cleanup(EXIT_IO, true);
		}
	}

	wrote_pidfile = true;

	ret = fclose(fh);
	if (ret) {
		ERR_TRACE();
		journal("Unable to close pid file handle: %s.\n", strerror(errno));

		if (opt.require_pidfile) {
			cleanup(EXIT_IO, true);
		}
	}
}

static void check_config()
{
	struct stat statbuf;
	int ret;

	if (opt.port < MIN_NORMAL_PORT && geteuid() != ROOT_USER_ID) {
		journal("Only root can bind to ports below %d.\n", MIN_NORMAL_PORT);
		cleanup(EXIT_ARGUMENTS, true);
	}

	if (opt.pidfile && opt.pidfile[0] != '/') {
		journal("Specified pid file is not an absolute path.\n");
		cleanup(EXIT_ARGUMENTS, true);
	}

	ret = stat(opt.quotesfile, &statbuf);

	if (ret) {
		ERR_TRACE();
		journal("Unable to stat quotes file \"%s\": %s.\n", opt.quotesfile, strerror(errno));
		cleanup(EXIT_IO, true);
	}

	if (geteuid() == ROOT_USER_ID && opt.drop_privileges && opt.pidfile) {
		char *pidfile = strdup(opt.pidfile);
		ret = stat(dirname(pidfile), &statbuf);
		free(pidfile);
		if (ret) {
			journal("Unable to stat pid file directory \"%s\": %s.\n",
					dirname(pidfile), strerror(errno));
			cleanup(EXIT_IO, true);
		}

		do {
			if (statbuf.st_uid == DAEMON_USER_ID && (statbuf.st_mode & S_IRWXU)) {
				break;
			}

			if (statbuf.st_gid == DAEMON_GROUP_ID && (statbuf.st_mode & S_IRWXG)) {
				break;
			}

			if (statbuf.st_mode & S_IRWXO) {
				break;
			}

			journal("Given the current selection of pidfile, the daemon would be unable\n"
				"to delete it because of the permissions on the holding directory.\n"
				"Either change the pidfile or run without dropping privileges.\n");
			cleanup(EXIT_ARGUMENTS, true);
		} while(0);
	}
}

