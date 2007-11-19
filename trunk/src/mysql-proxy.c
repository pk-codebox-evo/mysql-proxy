/* Copyright (C) 2007 MySQL AB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */ 

/**
 * \mainpage
 *
 * \section Architecture
 *
 * MySQL Proxy is based around the C10k problem as described by http://kegel.com/c10k.html
 *
 * This leads to some basic features
 * - 10.000 concurrent connections in one program
 * - spreading the load over several backends
 * - each backend might be able to only handle 100 connections (max_connections)
 * 
 * We can implement 
 * - reusing idling backend connections 
 * - splitting client connections into several backend connections
 *
 * Most of the magic is happening in the scripting layer provided by lua (http://lua.org/) which
 * was picked as it:
 *
 * - is very easy to embed
 * - is small (200kb stripped) and efficient (see http://shootout.alioth.debian.org/gp4/benchmark.php?test=all&lang=all)
 * - is easy to read and write
 *
 * \section a walk through the code
 *
 * To understand the code you basicly only have to know about the three files documented below:
 *
 * - mysql-proxy.c
 *   - main()
 *     -# command-line handling
 *     -# plugin loading
 *     -# logging
 * - network-mysqld.c
 *   - network_mysqld_thread() (supposed be called as thread)
 *     -# registers event-halders (event_set(..., network_mysqld_con_accept, ...))
 *     -# calls event_base_dispatch() [libevent] in the mainloop 
 *   - network_mysqld_con_accept()
 *     -# is called when the listen()ing socket gets a incoming connection
 *     -# sets the event-handler for the established connection (e.g. network_mysqld_proxy_connection_init())
 *     -# calls network_mysqld_con_handle() on the connection 
 *   - network_mysqld_con_handle() is the state-machine
 *     -# implements the states of the \ref protocol "MySQL Protocol"
 *     -# calls plugin functions (registered by e.g. network_mysqld_proxy_connection_init()) 
 * - network-mysqld-proxy.c
 *   - implements the \ref proxy_states "proxy specific states"
 *
 * The other files only help those based main modules to do their job:
 *
 * - network-mysqld-proto.c
 *   - the byte functions around the \ref proto "MySQL protocol"
 * - network-socket.c
 *   - basic socket struct 
 * - network-mysqld-table.c
 *   - internal tables to select from on the admin interface (to be removed) 
 * - \ref sql-tokenizer.h "sql-tokenizer.y"
 *   - a flex based tokenizer for MySQL's SQL dialect (no parser) 
 * - network-conn-pool.c
 *   - a connection pool for server connections 
 */


/** @file
 * the user-interface for the MySQL Proxy @see main()
 *
 *  -  command-line handling 
 *  -  config-file parsing
 * 
 *
 * network_mysqld_thread() is the real proxy thread 
 * 
 * @todo move the SQL based help out into a lua script
 */


#define SVN_REVISION "$Rev$"

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <sys/types.h>
#include <sys/stat.h>

#ifdef HAVE_SIGNAL_H
#include <signal.h>
#endif
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>

#ifdef _WIN32
#include <process.h> /* getpid() */
#include <io.h>      /* open() */
#else
#include <unistd.h>
#endif

#include <glib.h>
#include <gmodule.h>

#ifdef HAVE_LUA_H
#include <lua.h>
#endif

#include "network-mysqld.h"
#include "network-mysqld-proto.h"
#include "sys-pedantic.h"

#ifndef _WIN32
static void signal_handler(int sig) {
	switch (sig) {
	case SIGINT: network_mysqld_set_shutdown(); break;
	case SIGTERM: network_mysqld_set_shutdown(); break;
	}
}
#endif

/**
 * turn a GTimeVal into string
 *
 * @return string in ISO notation with micro-seconds
 */
static gchar * g_timeval_string(GTimeVal *t1, GString *str) {
	size_t used_len;
	
	g_string_set_size(str, 63);

	used_len = strftime(str->str, str->allocated_len, "%Y-%m-%dT%H:%M:%S", gmtime(&t1->tv_sec));

	g_assert(used_len < str->allocated_len);
	str->len = used_len;

	g_string_append_printf(str, ".%06ld", t1->tv_usec);

	return str->str;
}


/**
 * log everything to the stdout for now
 */
static void log_func(const gchar *UNUSED_PARAM(log_domain), GLogLevelFlags UNUSED_PARAM(log_level), const gchar *message, gpointer UNUSED_PARAM(user_data)) {
	write(STDERR_FILENO, message, strlen(message));
	write(STDERR_FILENO, "\n", 1);
}

#ifndef _WIN32
/**
 * start the agent in the background 
 * 
 * UNIX-version
 */
static void daemonize(void) {
#ifdef SIGTTOU
	signal(SIGTTOU, SIG_IGN);
#endif
#ifdef SIGTTIN
	signal(SIGTTIN, SIG_IGN);
#endif
#ifdef SIGTSTP
	signal(SIGTSTP, SIG_IGN);
#endif
	if (fork() != 0) exit(0);
	
	if (setsid() == -1) exit(0);

	signal(SIGHUP, SIG_IGN);

	if (fork() != 0) exit(0);
	
	chdir("/");
	
	umask(0);
}
#endif


#define GETTEXT_PACKAGE "mysql-proxy"

int main(int argc, char **argv) {
	network_mysqld *srv;
	
	/* read the command-line options */
	GOptionContext *option_ctx;
	GError *gerr = NULL;
	guint i;
	int exit_code = EXIT_SUCCESS;
	int print_version = 0;
	int daemon_mode = 0;
	const gchar *check_str = NULL;
	cauldron_plugin *p;
	gchar *pid_file = NULL;
	gchar *plugin_dir = NULL;
	gchar *default_file = NULL;
	GOptionEntry *config_entries;
	gchar **plugin_names = NULL;

	GKeyFile *keyfile = NULL;

	GOptionEntry main_entries[] = 
	{
		{ "version",                 'V', 0, G_OPTION_ARG_NONE, NULL, "Show version", NULL },
		{ "daemon",                   0, 0, G_OPTION_ARG_NONE, NULL, "Start in daemon-mode", NULL },
		{ "pid-file",                 0, 0, G_OPTION_ARG_STRING, NULL, "PID file in case we are started as daemon", "<file>" },
		{ "plugin-dir",               0, 0, G_OPTION_ARG_STRING, NULL, "path to the plugins", "<path>" },
		{ "defaults-file",            0, 0, G_OPTION_ARG_STRING, NULL, "configuration file", "<file>" },
		{ "plugins",                  0, 0, G_OPTION_ARG_STRING, NULL, "plugins to load", "<name>[,<name>]" },
		
		{ NULL,                       0, 0, G_OPTION_ARG_NONE,   NULL, NULL, NULL }
	};

	if (!GLIB_CHECK_VERSION(2, 6, 0)) {
		g_error("the glib header are too old, need at least 2.6.0, got: %d.%d.%d", 
				GLIB_MAJOR_VERSION, GLIB_MINOR_VERSION, GLIB_MICRO_VERSION);
	}

	check_str = glib_check_version(GLIB_MAJOR_VERSION, GLIB_MINOR_VERSION, GLIB_MICRO_VERSION);

	if (check_str) {
		g_error("%s, got: lib=%d.%d.%d, headers=%d.%d.%d", 
			check_str,
			glib_major_version, glib_minor_version, glib_micro_version,
			GLIB_MAJOR_VERSION, GLIB_MINOR_VERSION, GLIB_MICRO_VERSION);
	}

	if (!g_module_supported()) {
		g_error("loading modules is not supported on this platform");
	}
	
	srv = network_mysqld_init();

	i = 0;
	main_entries[i++].arg_data  = &(print_version);
	main_entries[i++].arg_data  = &(daemon_mode);
	main_entries[i++].arg_data  = &(pid_file);
	main_entries[i++].arg_data  = &(plugin_dir);
	main_entries[i++].arg_data  = &(default_file);
	main_entries[i++].arg_data  = &(plugin_names);

	g_log_set_default_handler(log_func, NULL);

	option_ctx = g_option_context_new("- MySQL Proxy");
	g_option_context_add_main_entries(option_ctx, main_entries, GETTEXT_PACKAGE);
	g_option_context_set_help_enabled(option_ctx, FALSE);
	g_option_context_set_ignore_unknown_options(option_ctx, TRUE);

	/**
	 * parse once to get the basic options 
	 *
	 * leave the unknown options in the list
	 */
	if (FALSE == g_option_context_parse(option_ctx, &argc, &argv, &gerr)) {
		g_critical("%s", gerr->message);
		
		g_error_free(gerr);
		
		network_mysqld_free(srv);

		return EXIT_FAILURE;
	}

	if (!plugin_dir) plugin_dir = g_strdup(LIBDIR);

	/* if not plugins are specified, load admin and proxy */
	if (!plugin_names) {
		plugin_names = g_new(char *, 3);
		plugin_names[0] = g_strdup("admin");
		plugin_names[1] = g_strdup("proxy");
	}

	if (default_file) {
		keyfile = g_key_file_new();
		g_key_file_set_list_separator(keyfile, ',');

		if (FALSE == g_key_file_load_from_file(keyfile, default_file, G_KEY_FILE_NONE, &gerr)) {
			g_critical("loading configuration from %s failed: %s", 
					default_file,
					gerr->message);

			g_error_free(gerr);
			gerr = NULL;

			return -1;
		}
	}

	/* load the plugins */
	for (i = 0; plugin_names[i]; i++) {
		char *plugin_filename = g_strdup_printf("lib%s.la", plugin_names[i]);

		if (NULL == (p = cauldron_plugin_load(plugin_dir, plugin_filename))) {
			return EXIT_FAILURE;
		}
		g_free(plugin_filename);

		g_ptr_array_add(srv->modules, p);

		if (NULL != (config_entries = cauldron_plugin_get_options(p))) {
			gchar *group_desc = g_strdup_printf("%s-module", plugin_names[i]);
			gchar *help_msg = g_strdup_printf("Show options for the %s-module", plugin_names[i]);
			const gchar *group_name = plugin_names[i];
			const gchar *ini_group_name = "mysql-proxy";

			GOptionGroup *option_grp = g_option_group_new(group_name, group_desc, help_msg, NULL, NULL);
			g_option_group_add_entries(option_grp, config_entries);
			g_option_context_add_group(option_ctx, option_grp);

			g_free(help_msg);
			g_free(group_desc);

			/* parse the new options */
			if (FALSE == g_option_context_parse(option_ctx, &argc, &argv, &gerr)) {
				g_critical("%s", gerr->message);
		
				g_error_free(gerr);

				return EXIT_FAILURE;
			}

			/* all the options are in the group for "mysql-proxy" */
			if (keyfile && g_key_file_has_group(keyfile, ini_group_name)) {
				int j;

				/* set the defaults */
				for (j = 0; config_entries[j].long_name; j++) {
					GOptionEntry *entry = &(config_entries[j]);
					gchar *arg_string;
					gchar **arg_string_array;
					gboolean arg_bool = 0;
					gint arg_int = 0;
					gdouble arg_double = 0;
					gsize len = 0;

					switch (entry->arg) {
					case G_OPTION_ARG_STRING: 
						/* is this option set already */
						if (NULL == entry->arg_data || NULL != *(gchar **)(entry->arg_data)) break;

						arg_string = g_key_file_get_string(keyfile, ini_group_name, entry->long_name, &gerr);
						if (!gerr) {
							*(gchar **)(entry->arg_data) = arg_string;
						}
						break;
					case G_OPTION_ARG_STRING_ARRAY: 
						/* is this option set already */
						if (NULL == entry->arg_data || NULL != *(gchar ***)(entry->arg_data)) break;

						arg_string_array = g_key_file_get_string_list(keyfile, ini_group_name, entry->long_name, &len, &gerr);
						if (!gerr) {
							*(gchar ***)(entry->arg_data) = arg_string_array;
						}
						break;
					case G_OPTION_ARG_NONE: 
						arg_bool = g_key_file_get_boolean(keyfile, ini_group_name, entry->long_name, &gerr);
						if (!gerr) {
							*(int *)(entry->arg_data) = arg_bool;
						}
						break;
					case G_OPTION_ARG_INT: 
						arg_int = g_key_file_get_integer(keyfile, ini_group_name, entry->long_name, &gerr);
						if (!gerr) {
							*(gint *)(entry->arg_data) = arg_int;
						}
						break;
					case G_OPTION_ARG_DOUBLE: 
						arg_double = g_key_file_get_double(keyfile, ini_group_name, entry->long_name, &gerr);
						if (!gerr) {
							*(gint *)(entry->arg_data) = arg_double;
						}
						break;

					}

					if (gerr) {
						if (gerr->code != G_KEY_FILE_ERROR_KEY_NOT_FOUND) {
							g_message("%s", gerr->message);
						}

						g_error_free(gerr);
						gerr = NULL;
					}
				}
			}
		}
	}

	if (keyfile) g_key_file_free(keyfile);
	if (default_file) g_free(default_file);

	/* we know about the options now, lets parse them */
	g_option_context_set_help_enabled(option_ctx, TRUE);
	g_option_context_set_ignore_unknown_options(option_ctx, FALSE);
	if (FALSE == g_option_context_parse(option_ctx, &argc, &argv, &gerr)) {
		g_critical("%s", gerr->message);
		
		g_error_free(gerr);
		
		network_mysqld_free(srv);

		return EXIT_FAILURE;
	}

	g_option_context_free(option_ctx);

	/* after parsing the options we should only have the program name left */
	if (argc > 1) {
		g_critical("unknown option: %s", argv[1]);

		return EXIT_FAILURE;
	}


#if defined(HAVE_LUA_H) && defined(DATADIR)
	/**
	 * if the LUA_PATH is not set, set a good default 
	 */
	if (!g_getenv("LUA_PATH")) {
		g_setenv("LUA_PATH", LUA_PATHSEP LUA_PATHSEP DATADIR "/?.lua", 1);
	}
#endif

	if (print_version) {
		printf("%s\r\n", PACKAGE_STRING);

		network_mysqld_free(srv);
		return 0;
	}

#ifndef _WIN32	
	signal(SIGINT,  signal_handler);
	signal(SIGTERM, signal_handler);
	signal(SIGPIPE, SIG_IGN);

	if (daemon_mode) {
		daemonize();
	}
#endif
	if (pid_file) {
		int fd;
		gchar *pid_str;

		/**
		 * write the PID file
		 */

		if (-1 == (fd = open(pid_file, O_WRONLY|O_TRUNC|O_CREAT, 0600))) {
			g_critical("%s.%d: open(%s) failed: %s", 
					__FILE__, __LINE__,
					pid_file,
					strerror(errno));
			return -1;
		}

		pid_str = g_strdup_printf("%d", getpid());

		write(fd, pid_str, strlen(pid_str));
		g_free(pid_str);

		close(fd);
	}

	if (network_mysqld_thread(srv)) {
		/* looks like we failed */

		exit_code = EXIT_FAILURE;
	}

	network_mysqld_free(srv);

	return exit_code;
}

