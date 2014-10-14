/*
 * MOC - music on console
 * Copyright (C) 2004-2005 Damian Pietras <daper@daper.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <locale.h>
#include <assert.h>
#include <popt.h>

#ifdef HAVE_UNAME_SYSCALL
#include <sys/utsname.h>
#endif

#include "common.h"
#include "server.h"
#include "interface.h"
#include "options.h"
#include "protocol.h"
#include "log.h"
#include "compat.h"
#include "decoder.h"
#include "lists.h"
#include "files.h"
#include "rcc.h"

struct parameters
{
	char *config_file;
	int debug;
	int only_server;
	int foreground;
	int append;
	int enqueue;
	int clear;
	int play;
	int dont_run_iface;
	int stop;
	int exit;
	int pause;
	int unpause;
	int next;
	int previous;
	int get_file_info;
	int toggle_pause;
	int playit;
	int seek_by;
	char jump_type;
	int jump_to;
	char *formatted_info_param;
	int get_formatted_info;
	char *adj_volume;
	char *toggle;
	char *on;
	char *off;
};


/* Connect to the server, return fd of the socket or -1 on error. */
static int server_connect ()
{
	struct sockaddr_un sock_name;
	int sock;

	/* Create a socket */
	if ((sock = socket (PF_LOCAL, SOCK_STREAM, 0)) == -1)
		 return -1;

	sock_name.sun_family = AF_LOCAL;
	strcpy (sock_name.sun_path, socket_name());

	if (connect(sock, (struct sockaddr *)&sock_name,
				SUN_LEN(&sock_name)) == -1) {
		close (sock);
		return -1;
	}

	return sock;
}

/* Ping the server.
 * Return 1 if the server responds with EV_PONG, otherwise 1. */
static int ping_server (int sock)
{
	int event;

	send_int(sock, CMD_PING); /* ignore errors - the server could have
				     already closed the connection and sent
				     EV_BUSY */
	if (!get_int(sock, &event))
		fatal ("Error when receiving pong response!");
	return event == EV_PONG ? 1 : 0;
}

/* Check if a directory ./.moc exists and create if needed. */
static void check_moc_dir ()
{
	char *dir_name = create_file_name ("");
	struct stat file_stat;

	/* strip trailing slash */
	dir_name[strlen(dir_name)-1] = 0;

	if (stat (dir_name, &file_stat) == -1) {
		if (errno == ENOENT) {
			if (mkdir (dir_name, 0700) == -1)
				fatal ("Can't create directory %s: %s",
						dir_name, strerror (errno));
		}
		else
			fatal ("Error trying to check for "CONFIG_DIR" directory: %s",
			        strerror (errno));
	}
	else {
		if (!S_ISDIR(file_stat.st_mode) || access (dir_name, W_OK))
			fatal ("%s is not a writable directory!", dir_name);
	}
}

static void sig_chld (int sig LOGIT_ONLY)
{
	int saved_errno;
	pid_t rc;

	log_signal (sig);

	saved_errno = errno;
	do {
		rc = waitpid (-1, NULL, WNOHANG);
	} while (rc > 0);
	errno = saved_errno;
}

/* Run client and the server if needed. */
static void start_moc (const struct parameters *params, lists_t_strs *args)
{
	int list_sock;
	int server_sock = -1;

	if (!params->foreground && (server_sock = server_connect()) == -1) {
		int notify_pipe[2];
		int i = 0;
		ssize_t rc;

		printf ("Running the server...\n");

		/* To notify the client that the server socket is ready */
		if (pipe(notify_pipe))
			fatal ("pipe() failed: %s", strerror(errno));

		switch (fork()) {
			case 0: /* child - start server */
				set_me_server ();
				list_sock = server_init (params->debug, params->foreground);
				rc = write (notify_pipe[1], &i, sizeof(i));
				if (rc < 0)
					fatal ("write() to notify pipe failed: %s",
					        strerror(errno));
				close (notify_pipe[0]);
				close (notify_pipe[1]);
				signal (SIGCHLD, sig_chld);
				server_loop (list_sock);
				options_free ();
				decoder_cleanup ();
				io_cleanup ();
				files_cleanup ();
				rcc_cleanup ();
				compat_cleanup ();
				exit (EXIT_SUCCESS);
			case -1:
				fatal ("fork() failed: %s", strerror(errno));
			default:
				close (notify_pipe[1]);
				if (read(notify_pipe[0], &i, sizeof(i)) != sizeof(i))
					fatal ("Server exited!");
				close (notify_pipe[0]);
				if ((server_sock = server_connect()) == -1) {
					perror ("server_connect()");
					fatal ("Can't connect to the server!");
				}
		}
	}
	else if (!params->foreground && params->only_server)
		fatal ("Server is already running!");
	else if (params->foreground && params->only_server) {
		set_me_server ();
		list_sock = server_init (params->debug, params->foreground);
		server_loop (list_sock);
	}

	if (!params->only_server) {
		signal (SIGPIPE, SIG_IGN);
		if (ping_server(server_sock)) {
			if (!params->dont_run_iface) {
				init_interface (server_sock, params->debug, args);
				interface_loop ();
				interface_end ();
			}
		}
		else
			fatal ("Can't connect to the server!");
	}

	if (!params->foreground && params->only_server)
		send_int (server_sock, CMD_DISCONNECT);

	close (server_sock);
}

static void show_version ()
{
#ifdef HAVE_UNAME_SYSCALL
	int rc;
	struct utsname uts;
#endif

	putchar ('\n');
	printf ("          This is : %s\n", PACKAGE_NAME);
	printf ("          Version : %s\n", PACKAGE_VERSION);

#ifdef PACKAGE_REVISION
	printf ("         Revision : %s\n", PACKAGE_REVISION);
#endif

	/* Show build time */
#ifdef __DATE__
	printf ("            Built : %s", __DATE__);
# ifdef __TIME__
	printf (" %s", __TIME__);
# endif
	putchar ('\n');
#endif

	/* Show compiled-in components */
	printf ("    Compiled with :");
#ifdef HAVE_OSS
	printf (" OSS");
#endif
#ifdef HAVE_SNDIO
	printf (" SNDIO");
#endif
#ifdef HAVE_ALSA
	printf (" ALSA");
#endif
#ifdef HAVE_JACK
	printf (" JACK");
#endif
#ifndef NDEBUG
	printf (" DEBUG");
#endif
#ifdef HAVE_CURL
	printf (" Network streams");
#endif
#ifdef HAVE_SAMPLERATE
	printf (" resample");
#endif
	putchar ('\n');

#ifdef HAVE_UNAME_SYSCALL
	rc = uname (&uts);
	if (rc == 0)
		printf ("       Running on : %s %s %s\n", uts.sysname, uts.release,
	                                                           uts.machine);
#endif

	printf ("           Author : Damian Pietras\n");
	printf ("         Homepage : %s\n", PACKAGE_URL);
	printf ("           E-Mail : %s\n", PACKAGE_BUGREPORT);
	printf ("        Copyright : (C) 2003-2014 Damian Pietras and others\n");
	printf ("          License : GNU General Public License, version 2 or later\n");
	putchar ('\n');
}

/* Show program banner. */
static void show_banner ()
{
	printf ("%s (version %s", PACKAGE_NAME, PACKAGE_VERSION);
#ifdef PACKAGE_REVISION
	printf (", revision %s", PACKAGE_REVISION);
#endif
	printf (")\n");
}

static const char mocp_summary[] = "[OPTIONS] [FILE|DIR ...]";

/* Show program usage. */
static void show_usage (poptContext ctx)
{
	show_banner ();
	poptSetOtherOptionHelp (ctx, mocp_summary);
	poptPrintUsage (ctx, stdout, 0);
}

/* Show program help. */
static void show_help (poptContext ctx)
{
	show_banner ();
	poptSetOtherOptionHelp (ctx, mocp_summary);
	poptPrintHelp (ctx, stdout, 0);
}

/* Send commands requested in params to the server. */
static void server_command (struct parameters *params, lists_t_strs *args)
{
	int sock;

	if ((sock = server_connect()) == -1)
		fatal ("The server is not running!");

	signal (SIGPIPE, SIG_IGN);
	if (ping_server(sock)) {
		if (params->playit)
			interface_cmdline_playit (sock, args);
		if (params->clear)
			interface_cmdline_clear_plist (sock);
		if (params->append)
			interface_cmdline_append (sock, args);
		if (params->enqueue)
			interface_cmdline_enqueue (sock, args);
		if (params->play)
			interface_cmdline_play_first (sock);
		if (params->get_file_info)
			interface_cmdline_file_info (sock);
		if (params->seek_by)
			interface_cmdline_seek_by (sock, params->seek_by);
		if (params->jump_type=='%')
			interface_cmdline_jump_to_percent (sock,params->jump_to);
		if (params->jump_type=='s')
			interface_cmdline_jump_to (sock,params->jump_to);
		if (params->get_formatted_info)
			interface_cmdline_formatted_info (sock,
					params->formatted_info_param);
		if (params->adj_volume)
			interface_cmdline_adj_volume (sock, params->adj_volume);
		if (params->toggle)
			interface_cmdline_set (sock, params->toggle, 2);
		if (params->on)
			interface_cmdline_set (sock, params->on, 1);
		if (params->off)
			interface_cmdline_set (sock, params->off, 0);
		if (params->exit) {
			if (!send_int(sock, CMD_QUIT))
				fatal ("Can't send command!");
		}
		else if (params->stop) {
			if (!send_int(sock, CMD_STOP)
					|| !send_int(sock, CMD_DISCONNECT))
				fatal ("Can't send commands!");
		}
		else if (params->pause) {
			if (!send_int(sock, CMD_PAUSE)
					|| !send_int(sock, CMD_DISCONNECT))
				fatal ("Can't send commands!");
		}
		else if (params->next) {
			if (!send_int(sock, CMD_NEXT)
					|| !send_int(sock, CMD_DISCONNECT))
				fatal ("Can't send commands!");
		}
		else if (params->previous) {
			if (!send_int(sock, CMD_PREV)
					|| !send_int(sock, CMD_DISCONNECT))
				fatal ("Can't send commands!");
		}
		else if (params->unpause) {
			if (!send_int(sock, CMD_UNPAUSE)
					|| !send_int(sock, CMD_DISCONNECT))
				fatal ("Can't send commands!");
		}
		else if (params->toggle_pause) {
			int state;
			int ev;
			int cmd = -1;

			if (!send_int(sock, CMD_GET_STATE))
				fatal ("Can't send commands!");
			if (!get_int(sock, &ev) || ev != EV_DATA
					|| !get_int(sock, &state))
				fatal ("Can't get data from the server!");

			if (state == STATE_PAUSE)
				cmd = CMD_UNPAUSE;
			else if (state == STATE_PLAY)
				cmd = CMD_PAUSE;

			if (cmd != -1 && !send_int(sock, cmd))
				fatal ("Can't send commands!");
			if (!send_int(sock, CMD_DISCONNECT))
				fatal ("Can't send commands!");
		}
	}
	else
		fatal ("Can't connect to the server!");

	close (sock);
}

static long get_num_param (const char *p,const char ** last)
{
	char *e;
	long val;

	val = strtol (p, &e, 10);
	if ((*e&&last==NULL)||e==p)
		fatal ("The parameter should be a number!");

	if (last)
		*last=e;
	return val;
}

/* Log the command line which launched MOC. */
static void log_command_line (int argc ASSERT_ONLY,
                              const char *argv[] ASSERT_ONLY)
{
	lists_t_strs *cmdline LOGIT_ONLY;
	char *str LOGIT_ONLY;

	assert (argc >= 0);
	assert (argv != NULL);
	assert (argv[argc] == NULL);

#ifndef NDEBUG
	cmdline = lists_strs_new (argc);
	if (lists_strs_load (cmdline, argv) > 0)
		str = lists_strs_fmt (cmdline, "%s ");
	else
		str = xstrdup ("No command line available");
	logit ("%s", str);
	free (str);
	lists_strs_free (cmdline);
#endif
}

static void override_config_option (const char *arg, lists_t_strs *deferred)
{
	int len;
	bool append;
	char *ptr, *name, *value;
	enum option_type type;

	assert (arg != NULL);

	ptr = strchr (arg, '=');
	if (ptr == NULL)
		goto error;

	/* Allow for list append operator ("+="). */
	append = (ptr > arg && *(ptr - 1) == '+');

	name = trim (arg, ptr - arg - (append ? 1 : 0));
	if (!name || !name[0])
		goto error;
	type = options_get_type (name);

	if (type == OPTION_LIST) {
		if (deferred) {
			lists_strs_append (deferred, arg);
			free (name);
			return;
		}
	}
	else if (append)
		goto error;

	value = trim (ptr + 1, strlen (ptr + 1));
	if (!value || !value[0])
		goto error;

	if (value[0] == '\'' || value[0] == '"') {
		len = strlen (value);
		if (value[0] != value[len - 1])
			goto error;
		if (strlen (value) < 2)
			goto error;
		memmove (value, value + 1, len - 2);
		value[len - 2] = 0x00;
	}

	if (!options_set_pair (name, value, append))
		goto error;
	options_ignore_config (name);

	free (name);
	free (value);
	return;

error:
	fatal ("Malformed override option: %s", arg);
}

static void process_deferred_overrides (lists_t_strs *deferred)
{
	int ix;
	bool cleared;
	const char marker[] = "*Marker*";
	char **config_decoders;
	lists_t_strs *decoders_option;

	/* We need to shuffle the PreferredDecoders list into the
	 * right order as we load any deferred overriding options. */

	decoders_option = options_get_list ("PreferredDecoders");
	lists_strs_reverse (decoders_option);
	config_decoders = lists_strs_save (decoders_option);
	lists_strs_clear (decoders_option);
	lists_strs_append (decoders_option, marker);

	for (ix = 0; ix < lists_strs_size (deferred); ix += 1)
		override_config_option (lists_strs_at (deferred, ix), NULL);

	cleared = lists_strs_empty (decoders_option) ||
	          strcmp (lists_strs_at (decoders_option, 0), marker) != 0;
	lists_strs_reverse (decoders_option);
	if (!cleared) {
		char **override_decoders;

		free (lists_strs_pop (decoders_option));
		override_decoders = lists_strs_save (decoders_option);
		lists_strs_clear (decoders_option);
		lists_strs_load (decoders_option, (const char **)config_decoders);
		lists_strs_load (decoders_option, (const char **)override_decoders);
		free (override_decoders);
	}
	free (config_decoders);
}

enum {
	CL_HANDLED = 0,
	CL_NOIFACE,
	CL_VERSION,
	CL_HELP,
	CL_USAGE,
	CL_SDRIVER,
	CL_MUSICDIR,
	CL_THEME,
	CL_SETOPTION,
	CL_MOCDIR,
	CL_SYNCPL,
	CL_NOSYNC,
	CL_ASCII,
	CL_JUMP,
	CL_GETINFO
};

static struct parameters params;

static struct poptOption opts[] = {
	{"config", 'C', POPT_ARG_STRING, &params.config_file, CL_HANDLED,
			"Use the specified config file instead of the default", "FILE"},
	{"set-option", 'O', POPT_ARG_STRING, NULL, CL_SETOPTION,
			"Override the configuration option NAME with VALUE", "'NAME=VALUE'"},
	{"moc-dir", 'M', POPT_ARG_STRING, NULL, CL_MOCDIR,
			"Use the specified MOC directory instead of the default", "DIR"},
	{"pause", 'P', POPT_ARG_NONE, &params.pause, CL_NOIFACE,
			"Pause", NULL},
	{"unpause", 'U', POPT_ARG_NONE, &params.unpause, CL_NOIFACE,
			"Unpause", NULL},
	{"toggle-pause", 'G', POPT_ARG_NONE, &params.toggle_pause, CL_NOIFACE,
			"Toggle between playing and paused", NULL},
	{"stop", 's', POPT_ARG_NONE, &params.stop, CL_NOIFACE,
			"Stop playing", NULL},
	{"next", 'f', POPT_ARG_NONE, &params.next, CL_NOIFACE,
			"Play the next song", NULL},
	{"previous", 'r', POPT_ARG_NONE, &params.previous, CL_NOIFACE,
			"Play the previous song", NULL},
	{"version", 'V', POPT_ARG_NONE, NULL, CL_VERSION,
			"Print version information", NULL},
	{"help", 'h', POPT_ARG_NONE, NULL, CL_HELP,
			"Print extended usage", NULL},
	{"usage", 0, POPT_ARG_NONE, NULL, CL_USAGE,
			"Print brief usage", NULL},
#ifndef NDEBUG
	{"debug", 'D', POPT_ARG_NONE, &params.debug, CL_HANDLED,
			"Turn on logging to a file", NULL},
#endif
	{"server", 'S', POPT_ARG_NONE, &params.only_server, CL_HANDLED,
			"Only run the server", NULL},
	{"foreground", 'F', POPT_ARG_NONE, &params.foreground, CL_HANDLED,
			"Run the server in foreground (logging to stdout)", NULL},
	{"sound-driver", 'R', POPT_ARG_STRING, NULL, CL_SDRIVER,
			"Use the first valid sound driver", "DRIVERS"},
	{"music-dir", 'm', POPT_ARG_NONE, NULL, CL_MUSICDIR,
			"Start in MusicDir", NULL},
	{"append", 'a', POPT_ARG_NONE, &params.append, CL_NOIFACE,
			"Append the files/directories/playlists passed in "
			"the command line to playlist", NULL},
	{"recursively", 'e', POPT_ARG_NONE, &params.append, CL_NOIFACE,
			"Alias for --append", NULL},
	{"enqueue", 'q', POPT_ARG_NONE, &params.enqueue, CL_NOIFACE,
			"Add the files given on command line to the queue", NULL},
	{"clear", 'c', POPT_ARG_NONE, &params.clear, CL_NOIFACE,
			"Clear the playlist", NULL},
	{"play", 'p', POPT_ARG_NONE, &params.play, CL_NOIFACE,
			"Start playing from the first item on the playlist", NULL},
	{"playit", 'l', POPT_ARG_NONE, &params.playit, CL_NOIFACE,
			"Play files given on command line without modifying the playlist", NULL},
	{"exit", 'x', POPT_ARG_NONE, &params.exit, CL_NOIFACE,
			"Shutdown the server", NULL},
	{"info", 'i', POPT_ARG_NONE, &params.get_file_info, CL_NOIFACE,
			"Print information about the file currently playing", NULL},
	{"theme", 'T', POPT_ARG_STRING, NULL, CL_THEME,
			"Use the selected theme file (read from ~/.moc/themes if the path is not absolute)", "FILE"},
	{"sync", 'y', POPT_ARG_NONE, NULL, CL_SYNCPL,
			"Synchronize the playlist with other clients", NULL},
	{"nosync", 'n', POPT_ARG_NONE, NULL, CL_NOSYNC,
			"Don't synchronize the playlist with other clients", NULL},
	{"ascii", 'A', POPT_ARG_NONE, NULL, CL_ASCII,
			"Use ASCII characters to draw lines", NULL},
	{"seek", 'k', POPT_ARG_INT, &params.seek_by, CL_NOIFACE,
			"Seek by N seconds (can be negative)", "N"},
	{"jump", 'j', POPT_ARG_STRING, NULL, CL_JUMP,
			"Jump to some position in the current track", "N{%,s}"},
	{"volume", 'v', POPT_ARG_STRING, &params.adj_volume, CL_NOIFACE,
			"Adjust the PCM volume", "[+,-]LEVEL"},
	{"toggle", 't', POPT_ARG_STRING, &params.toggle, CL_NOIFACE,
			"Toggle a control (shuffle, autonext, repeat)", "CONTROL"},
	{"on", 'o', POPT_ARG_STRING, &params.on, CL_NOIFACE,
			"Turn on a control (shuffle, autonext, repeat)", "CONTROL"},
	{"off", 'u', POPT_ARG_STRING, &params.off, CL_NOIFACE,
			"Turn off a control (shuffle, autonext, repeat)", "CONTROL"},
	{"format", 'Q', POPT_ARG_STRING, &params.formatted_info_param, CL_GETINFO,
			"Print formatted information about the file currently playing", "FORMAT"},
	POPT_AUTOALIAS
	POPT_TABLEEND
};

/* Read the POPT configuration files as given in MOCP_POPTRC. */
static void read_mocp_poptrc (poptContext ctx, const char *env_poptrc)
{
	int ix, rc, count;
	lists_t_strs *files;

	logit ("MOCP_POPTRC: %s", env_poptrc);

	files = lists_strs_new (4);
	count = lists_strs_split (files, env_poptrc, ":");
	for (ix = 0; ix < count; ix += 1) {
		const char *fn;

		fn = lists_strs_at (files, ix);
		if (!strlen (fn))
			continue;

		if (!is_secure (fn))
			fatal ("POPT config file is not secure: %s", fn);

		rc = poptReadConfigFile (ctx, fn);
		if (rc < 0)
			fatal ("Error reading POPT config file '%s': %s",
			        fn, poptStrerror (rc));
	}

	lists_strs_free (files);
}

/* Check that the ~/.popt file is secure. */
static void check_popt_secure ()
{
	int len;
	const char *home, dot_popt[] = ".popt";
	char *home_popt;

	home = get_home ();
	len = strlen (home) + strlen (dot_popt) + 2;
	home_popt = xcalloc (len, sizeof (char));
	snprintf (home_popt, len, "%s/%s", home, dot_popt);
	if (!is_secure (home_popt))
		fatal ("POPT config file is not secure: %s", home_popt);
	free (home_popt);
}

/* Read the default POPT configuration file. */
static void read_default_poptrc (poptContext ctx)
{
	int rc;

	check_popt_secure ();
	rc = poptReadDefaultConfig (ctx, 0);
	if (rc != 0)
		fatal ("poptReadDefaultConfig() error: %s\n", poptStrerror (rc));
}

/* Read the POPT configuration files(s). */
static void read_popt_config (poptContext ctx)
{
	const char *env_poptrc;

	env_poptrc = getenv ("MOCP_POPTRC");
	if (env_poptrc)
		read_mocp_poptrc (ctx, env_poptrc);
	else
		read_default_poptrc (ctx);
}

/* Process the command line options. */
static void process_options (poptContext ctx, lists_t_strs *deferred)
{
	int rc;

	while ((rc = poptGetNextOpt (ctx)) >= 0) {
		const char *jump_type, *arg;

		arg = poptGetOptArg (ctx);

		switch (rc) {
		case CL_VERSION:
			show_version ();
			exit (EXIT_SUCCESS);
		case CL_HELP:
			show_help (ctx);
			exit (EXIT_SUCCESS);
		case CL_USAGE:
			show_usage (ctx);
			exit (EXIT_SUCCESS);
		case CL_SDRIVER:
			if (!options_check_list ("SoundDriver", arg))
				fatal ("No such sound driver: %s", arg);
			options_set_list ("SoundDriver", arg, false);
			options_ignore_config ("SoundDriver");
			break;
		case CL_MUSICDIR:
			options_set_bool ("StartInMusicDir", true);
			options_ignore_config ("StartInMusicDir");
			break;
		case CL_NOIFACE:
			params.dont_run_iface = 1;
			break;
		case CL_THEME:
			options_set_str ("ForceTheme", arg);
			break;
		case CL_SETOPTION:
			override_config_option (arg, deferred);
			break;
		case CL_MOCDIR:
			options_set_str ("MOCDir", arg);
			options_ignore_config ("MOCDir");
			break;
		case CL_SYNCPL:
			options_set_bool ("SyncPlaylist", true);
			options_ignore_config ("SyncPlaylist");
			break;
		case CL_NOSYNC:
			options_set_bool ("SyncPlaylist", false);
			options_ignore_config ("SyncPlaylist");
			break;
		case CL_ASCII:
			options_set_bool ("ASCIILines", true);
			options_ignore_config ("ASCIILines");
			break;
		case CL_JUMP:
			arg = poptGetOptArg (ctx);
			params.jump_to = get_num_param (arg, &jump_type);
			if (*jump_type)
				if (!jump_type[1])
					if (*jump_type == '%' || tolower (*jump_type) == 's') {
						params.jump_type = tolower (*jump_type);
						params.dont_run_iface = 1;
						break;
					}
			//TODO: Add message explaining the error
			show_usage (ctx);
			exit (EXIT_FAILURE);
		case CL_GETINFO:
			params.get_formatted_info = 1;
			params.dont_run_iface = 1;
			break;
		default:
			show_usage (ctx);
			exit (EXIT_FAILURE);
		}

		free ((void *) arg);
	}

	if (rc < -1) {
		const char *opt, *alias;

		opt = poptBadOption (ctx, 0);
		alias = poptBadOption (ctx, POPT_BADOPTION_NOALIAS);
		if (!strcmp (opt, alias))
			fatal ("%s: %s\n", opt, poptStrerror (rc));
		else
			fatal ("%s (aliased by %s): %s\n", opt, alias, poptStrerror (rc));
	}
}

/* Process the command line options and arguments. */
static lists_t_strs *process_command_line (int argc, const char *argv[],
                                           lists_t_strs *deferred)
{
	const char **rest;
	poptContext ctx;
	lists_t_strs *result;

	assert (argc >= 0);
	assert (argv != NULL);
	assert (argv[argc] == NULL);
	assert (deferred != NULL);

	ctx = poptGetContext ("mocp", argc, argv, opts, 0);

	read_popt_config (ctx);
	process_options (ctx, deferred);

	if (params.foreground)
		params.only_server = 1;

	result = lists_strs_new (4);
	rest = poptGetArgs (ctx);
	if (rest)
		lists_strs_load (result, rest);

	poptFreeContext (ctx);

	return result;
}

int main (int argc, const char *argv[])
{
	lists_t_strs *deferred_overrides, *args;

#ifdef PACKAGE_REVISION
	logit ("This is Music On Console (revision %s)", PACKAGE_REVISION);
#else
	logit ("This is Music On Console (version %s)", PACKAGE_VERSION);
#endif

#ifdef CONFIGURATION
	logit ("Configured:%s", CONFIGURATION);
#endif

#if !defined(NDEBUG) && defined(HAVE_UNAME_SYSCALL)
	{
		int rc;
		struct utsname uts;

		rc = uname (&uts);
		if (rc == 0)
			logit ("Running on: %s %s %s", uts.sysname, uts.release, uts.machine);
	}
#endif

	log_command_line (argc, argv);

	files_init ();

	if (get_home () == NULL)
		fatal ("Could not determine user's home directory!");

	memset (&params, 0, sizeof(params));
	options_init ();
	deferred_overrides = lists_strs_new (4);

	/* set locale according to the environment variables */
	if (!setlocale(LC_ALL, ""))
		logit ("Could not set locale!");

	args = process_command_line (argc, argv, deferred_overrides);

	if (params.dont_run_iface && params.only_server)
		fatal ("-c, -a and -p options can't be used with --server!");

	if (!params.config_file)
		params.config_file = create_file_name ("config");
	options_parse (params.config_file);

	process_deferred_overrides (deferred_overrides);
	lists_strs_free (deferred_overrides);
	deferred_overrides = NULL;

	check_moc_dir ();

	io_init ();
	rcc_init ();
	decoder_init (params.debug);
	srand (time(NULL));

	if (!params.only_server && params.dont_run_iface)
		server_command (&params, args);
	else
		start_moc (&params, args);

	lists_strs_free (args);
	options_free ();
	decoder_cleanup ();
	io_cleanup ();
	rcc_cleanup ();
	files_cleanup ();
	compat_cleanup ();

	exit (EXIT_SUCCESS);
}
