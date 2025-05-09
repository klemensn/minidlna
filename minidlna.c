/* MiniDLNA project
 *
 * http://sourceforge.net/projects/minidlna/
 *
 * MiniDLNA media server
 * Copyright (C) 2008-2012  Justin Maggard
 *
 * This file is part of MiniDLNA.
 *
 * MiniDLNA is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * MiniDLNA is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with MiniDLNA. If not, see <http://www.gnu.org/licenses/>.
 *
 * Portions of the code from the MiniUPnP project:
 *
 * Copyright (c) 2006-2007, Thomas Bernard
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * The name of the author may not be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/file.h>
#include <sys/time.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <fts.h>
#include <time.h>
#include <signal.h>
#include <errno.h>
#include <limits.h>
#include <libgen.h>
#include <pwd.h>
#include <grp.h>

#include "config.h"

#ifdef ENABLE_NLS
#include <locale.h>
#include <libintl.h>
#endif

#include "event.h"
#include "upnpglobalvars.h"
#include "sql.h"
#include "upnphttp.h"
#include "upnpdescgen.h"
#include "minidlnapath.h"
#include "getifaddr.h"
#include "upnpsoap.h"
#include "options.h"
#include "utils.h"
#include "minissdp.h"
#include "minidlnatypes.h"
#include "process.h"
#include "upnpevents.h"
#include "scanner.h"
#include "monitor.h"
#include "libav.h"
#include "log.h"
#include "tivo_beacon.h"
#include "tivo_utils.h"
#include "avahi.h"

#if SQLITE_VERSION_NUMBER < 3005001
# warning "Your SQLite3 library appears to be too old!  Please use 3.5.1 or newer."
# define sqlite3_threadsafe() 0
#endif

static LIST_HEAD(httplisthead, upnphttp) upnphttphead;

/* OpenAndConfHTTPSocket() :
 * setup the socket used to handle incoming HTTP connections. */
static int
OpenAndConfHTTPSocket(unsigned short port)
{
	int s;
	int i = 1;
	struct sockaddr_in listenname;

	/* Initialize client type cache */
	memset(&clients, 0, sizeof(struct client_cache_s));

	s = socket(PF_INET, SOCK_STREAM, 0);
	if (s < 0)
	{
		DPRINTF(E_ERROR, L_GENERAL, "socket(http): %s\n", strerror(errno));
		return -1;
	}

	if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &i, sizeof(i)) < 0)
		DPRINTF(E_WARN, L_GENERAL, "setsockopt(http, SO_REUSEADDR): %s\n", strerror(errno));

	memset(&listenname, 0, sizeof(struct sockaddr_in));
	listenname.sin_family = AF_INET;
	listenname.sin_port = htons(port);
	listenname.sin_addr.s_addr = htonl(INADDR_ANY);

	if (bind(s, (struct sockaddr *)&listenname, sizeof(struct sockaddr_in)) < 0)
	{
		DPRINTF(E_ERROR, L_GENERAL, "bind(http): %s\n", strerror(errno));
		close(s);
		return -1;
	}

	if (listen(s, 16) < 0)
	{
		DPRINTF(E_ERROR, L_GENERAL, "listen(http): %s\n", strerror(errno));
		close(s);
		return -1;
	}

	return s;
}

/* ProcessListen() :
 * accept incoming HTTP connection. */
static void
ProcessListen(struct event *ev)
{
	int shttp;
	socklen_t clientnamelen;
	struct sockaddr_in clientname;
	clientnamelen = sizeof(struct sockaddr_in);

	shttp = accept(ev->fd, (struct sockaddr *)&clientname, &clientnamelen);
	if (shttp<0)
	{
		DPRINTF(E_ERROR, L_GENERAL, "accept(http): %s\n", strerror(errno));
	}
	else
	{
		struct upnphttp * tmp = 0;
		DPRINTF(E_DEBUG, L_GENERAL, "HTTP connection from %s:%d\n",
			inet_ntoa(clientname.sin_addr),
			ntohs(clientname.sin_port) );
		/*if (fcntl(shttp, F_SETFL, O_NONBLOCK) < 0) {
			DPRINTF(E_ERROR, L_GENERAL, "fcntl F_SETFL, O_NONBLOCK\n");
		}*/
		/* Create a new upnphttp object and add it to
		 * the active upnphttp object list */
		tmp = New_upnphttp(shttp);
		if (tmp)
		{
			tmp->clientaddr = clientname.sin_addr;
			LIST_INSERT_HEAD(&upnphttphead, tmp, entries);
		}
		else
		{
			DPRINTF(E_ERROR, L_GENERAL, "New_upnphttp() failed\n");
			close(shttp);
		}
	}
}

/* Handler for the SIGTERM signal (kill) 
 * SIGINT is also handled */
static void
sigterm(int sig)
{
	signal(sig, SIG_IGN);	/* Ignore this signal while we are quitting */

	DPRINTF(E_WARN, L_GENERAL, "received signal %d, good-bye\n", sig);

	quitting = 1;
}

static void
sigusr1(int sig)
{
	signal(sig, sigusr1);
	DPRINTF(E_WARN, L_GENERAL, "received signal %d, clear cache\n", sig);

	memset(&clients, '\0', sizeof(clients));
}

static void
sighup(int sig)
{
	signal(sig, sighup);
	DPRINTF(E_WARN, L_GENERAL, "received signal %d, reloading\n", sig);

	reload_ifaces(1);
	log_reopen();
}

/* record the startup time */
static void
set_startup_time(void)
{
	startup_time = time(NULL);
}

static void
getfriendlyname(char *buf, int len)
{
	char *p = NULL;
	char hn[63];
	int off;

	if (gethostname(hn, sizeof(hn)) == 0)
	{
		strncpyt(buf, hn, len);
		p = strchr(buf, '.');
		if (p)
			*p = '\0';
	}
	else
		strcpy(buf, "Unknown");

	off = strlen(buf);
	off += snprintf(buf+off, len-off, ": ");
#ifdef READYNAS
	FILE *info;
	char ibuf[64], *key, *val;
	snprintf(buf+off, len-off, "ReadyNAS");
	info = fopen("/proc/sys/dev/boot/info", "r");
	if (!info)
		return;
	while ((val = fgets(ibuf, 64, info)) != NULL)
	{
		key = strsep(&val, ": \t");
		val = trim(val);
		if (strcmp(key, "model") == 0)
		{
			snprintf(buf+off, len-off, "%s", val);
			key = strchr(val, ' ');
			if (key)
			{
				strncpyt(modelnumber, key+1, MODELNUMBER_MAX_LEN);
				*key = '\0';
			}
			snprintf(modelname, MODELNAME_MAX_LEN,
				"Windows Media Connect compatible (%s)", val);
		}
		else if (strcmp(key, "serial") == 0)
		{
			strncpyt(serialnumber, val, SERIALNUMBER_MAX_LEN);
			if (serialnumber[0] == '\0')
			{
				char mac_str[13];
				if (getsyshwaddr(mac_str, sizeof(mac_str)) == 0)
					strcpy(serialnumber, mac_str);
				else
					strcpy(serialnumber, "0");
			}
			break;
		}
	}
	fclose(info);
#else
	char * logname;
	logname = getenv("USER");
	if (!logname)
        {
		logname = getenv("LOGNAME");
#ifndef STATIC // Disable for static linking
		if (!logname)
		{
			struct passwd *pwent = getpwuid(geteuid());
			if (pwent)
				logname = pwent->pw_name;
		}
#endif
	}
	snprintf(buf+off, len-off, "%s", logname?logname:"Unknown");
#endif
}

static time_t
_get_dbtime(void)
{
	char path[PATH_MAX];
	struct stat st;

	snprintf(path, sizeof(path), "%s/files.db", db_path);
	if (stat(path, &st) != 0)
		return 0;
	return st.st_mtime;
}

static int
open_db(sqlite3 **sq3)
{
	char path[PATH_MAX];
	int new_db = 0;

	snprintf(path, sizeof(path), "%s/files.db", db_path);
	if (access(path, F_OK) != 0)
	{
		new_db = 1;
		make_dir(db_path, S_ISVTX|S_IRWXU|S_IRWXG|S_IRWXO);
	}
	if (sqlite3_open(path, &db) != SQLITE_OK)
		DPRINTF(E_FATAL, L_GENERAL, "ERROR: Failed to open sqlite database!  Exiting...\n");
	if (sq3)
		*sq3 = db;
	sqlite3_busy_timeout(db, 5000);
	sql_exec(db, "pragma page_size = 4096");
	sql_exec(db, "pragma journal_mode = OFF");
	sql_exec(db, "pragma synchronous = OFF;");
	sql_exec(db, "pragma default_cache_size = 8192;");

	return new_db;
}

/* simplified version of OpenBSD's rm(1) -rf */
static int
rm_tree(char * const *argv)
{
	FTS *fts;
	FTSENT *p;
	int error = 0;

	if ((fts = fts_open(argv, FTS_PHYSICAL | FTS_NOSTAT, NULL)) == NULL)
		DPRINTF(E_ERROR, L_GENERAL, "fts_open: %s\n", strerror(errno));
	while ((p = fts_read(fts)) != NULL) {
		switch (p->fts_info) {
		case FTS_DNR:
			if (p->fts_errno != ENOENT) {
				DPRINTF(E_WARN, L_GENERAL, "fts_read: %s: %s\n", p->fts_path, strerror(p->fts_errno));
				error = 1;
			}
			continue;
		case FTS_ERR:
			DPRINTF(E_FATAL, L_GENERAL, "fts_read: %s: %s\n", p->fts_path, strerror(p->fts_errno));
		case FTS_D:
			continue;
		}

		switch (p->fts_info) {
		case FTS_DP:
		case FTS_DNR:
			if (!rmdir(p->fts_accpath) || errno == ENOENT)
				continue;
			break;
		default:
			if (!unlink(p->fts_accpath) || errno == ENOENT)
				continue;
		}
		DPRINTF(E_WARN, L_GENERAL, "rmdir/unlink: %s: %s\n", p->fts_path, strerror(errno));
		error = 1;
	}
	if (errno)
		DPRINTF(E_ERROR, L_GENERAL, "fts_read: %s\n", strerror(errno));
	fts_close(fts);
	return error;
}

static void
clean_cache(void)
{
	size_t sz = PATH_MAX;
	char files_db[sz], art_cache[sz];
	char * const paths[] = { files_db, art_cache, NULL };

	if (snprintf(files_db,  sz, "%s/files.db",  db_path) >= sz ||
	    snprintf(art_cache, sz, "%s/art_cache", db_path) >= sz ||
	    rm_tree(paths) != 0)
		DPRINTF(E_FATAL, L_GENERAL, "Failed to clean old file cache!  Exiting...\n");
}

static void
check_db(sqlite3 *db, int new_db, pid_t *scanner_pid)
{
	struct media_dir_s *media_path = NULL;
	char **result;
	int i, rows = 0;
	int ret;

	if (!new_db)
	{
		/* Check if any new media dirs appeared */
		media_path = media_dirs;
		while (media_path)
		{
			ret = sql_get_int_field(db, "SELECT TIMESTAMP as TYPE from DETAILS where PATH = %Q",
						media_path->path);
			if (ret != media_path->types)
			{
				ret = 1;
				goto rescan;
			}
			media_path = media_path->next;
		}
		/* Check if any media dirs disappeared */
		sql_get_table(db, "SELECT VALUE from SETTINGS where KEY = 'media_dir'", &result, &rows, NULL);
		for (i=1; i <= rows; i++)
		{
			media_path = media_dirs;
			while (media_path)
			{
				if (strcmp(result[i], media_path->path) == 0)
					break;
				media_path = media_path->next;
			}
			if (!media_path)
			{
				ret = 2;
				sqlite3_free_table(result);
				goto rescan;
			}
		}
		sqlite3_free_table(result);
	}

	ret = db_upgrade(db);
	if (ret != 0)
	{
rescan:
		CLEARFLAG(RESCAN_MASK);
		if (ret < 0)
			DPRINTF(E_WARN, L_GENERAL, "Creating new database at %s/files.db\n", db_path);
		else if (ret == 1)
			DPRINTF(E_WARN, L_GENERAL, "New media_dir detected; rebuilding...\n");
		else if (ret == 2)
			DPRINTF(E_WARN, L_GENERAL, "Removed media_dir detected; rebuilding...\n");
		else
			DPRINTF(E_WARN, L_GENERAL, "Database version mismatch (%d => %d); need to recreate...\n",
				ret, DB_VERSION);
		sqlite3_close(db);

		clean_cache();

		open_db(&db);
		if (CreateDatabase() != 0)
			DPRINTF(E_FATAL, L_GENERAL, "ERROR: Failed to create sqlite database!  Exiting...\n");
	}
	if (ret || GETFLAG(RESCAN_MASK))
	{
#if USE_FORK
		sqlite3_close(db);
		*scanner_pid = fork();
		open_db(&db);
		if (*scanner_pid == 0) /* child (scanner) process */
		{
			start_scanner();
			sqlite3_close(db);
			log_close();
			freeoptions();
			free(children);
			exit(EXIT_SUCCESS);
		}
		else if (*scanner_pid < 0)
		{
			start_scanner();
		}
		else
			SETFLAG(SCANNING_MASK);
#else
		start_scanner();
#endif
	}
}

static int
writepidfile(const char *fname, int pid, uid_t uid)
{
	FILE *pidfile;
	struct stat st;
	char path[PATH_MAX], *dir;
	int ret = 0;

	if(!fname || *fname == '\0')
		return -1;

	/* Create parent directory if it doesn't already exist */
	strncpyt(path, fname, sizeof(path));
	dir = dirname(path);
	if (stat(dir, &st) == 0)
	{
		if (!S_ISDIR(st.st_mode))
		{
			DPRINTF(E_ERROR, L_GENERAL, "Pidfile path is not a directory: %s\n",
				fname);
			return -1;
		}
	}
	else
	{
		if (make_dir(dir, S_IRWXU|S_IRGRP|S_IXGRP|S_IROTH|S_IXOTH) != 0)
		{
			DPRINTF(E_ERROR, L_GENERAL, "Unable to create pidfile directory: %s\n",
				fname);
			return -1;
		}
		if (uid > 0)
		{
			if (chown(dir, uid, -1) != 0)
				DPRINTF(E_WARN, L_GENERAL, "Unable to change pidfile %s ownership: %s\n",
					dir, strerror(errno));
		}
	}
	
	pidfile = fopen(fname, "w");
	if (!pidfile)
	{
		DPRINTF(E_ERROR, L_GENERAL, "Unable to open pidfile for writing %s: %s\n",
			fname, strerror(errno));
		return -1;
	}

	if (fprintf(pidfile, "%d\n", pid) <= 0)
	{
		DPRINTF(E_ERROR, L_GENERAL, 
			"Unable to write to pidfile %s: %s\n", fname, strerror(errno));
		ret = -1;
	}
	if (uid > 0)
	{
		if (fchown(fileno(pidfile), uid, -1) != 0)
			DPRINTF(E_WARN, L_GENERAL, "Unable to change pidfile %s ownership: %s\n",
				fname, strerror(errno));
	}

	fclose(pidfile);

	return ret;
}

static int strtobool(const char *str)
{
	return ((strcasecmp(str, "yes") == 0) ||
		(strcasecmp(str, "true") == 0) ||
		(atoi(str) == 1));
}

static void init_nls(void)
{
#ifdef ENABLE_NLS
	const char *messages, *ctype, *locale_dir;

	ctype = setlocale(LC_CTYPE, "");
	if (!ctype || !strcmp(ctype, "C"))
		ctype = setlocale(LC_CTYPE, "en_US.utf8");
	if (!ctype)
		DPRINTF(E_WARN, L_GENERAL, "Unset locale\n");
	else if (!strstr(ctype, "utf8") && !strstr(ctype, "UTF8") &&
		 !strstr(ctype, "utf-8") && !strstr(ctype, "UTF-8"))
		DPRINTF(E_WARN, L_GENERAL, "Using unsupported non-utf8 locale '%s'\n", ctype);
	messages = setlocale(LC_MESSAGES, "");
	if (!messages)
		messages = "unset";
	locale_dir = bindtextdomain("minidlna", getenv("TEXTDOMAINDIR"));
	DPRINTF(E_DEBUG, L_GENERAL, "Using locale dir '%s' and locale langauge %s/%s\n", locale_dir, messages, ctype);
	textdomain("minidlna");
#endif
}

/* init phase :
 * 1) read configuration file
 * 2) read command line arguments
 * 3) daemonize
 * 4) check and write pid file
 * 5) set startup time stamp
 * 6) compute presentation URL
 * 7) set signal handlers */
static int
init(int argc, char **argv)
{
	int i;
	int pid;
	int debug_flag = 0;
	int verbose_flag = 0;
	int options_flag = 0;
	struct sigaction sa;
	const char * presurl = NULL;
	const char * optionsfile = "/etc/minidlna.conf";
	char mac_str[13];
	char *string, *word;
	char *path;
	char buf[PATH_MAX];
	char log_str[75] = "general,artwork,database,inotify,scanner,metadata,http,ssdp,tivo=warn";
	char *log_level = NULL;
	struct media_dir_s *media_dir;
	int ifaces = 0;
	media_types types;
	uid_t uid = 0;
	gid_t gid = 0;
	int error;

	/* first check if "-f" option is used */
	for (i=2; i<argc; i++)
	{
		if (strcmp(argv[i-1], "-f") == 0)
		{
			optionsfile = argv[i];
			options_flag = 1;
			break;
		}
	}

	/* set up uuid based on mac address */
	if (getsyshwaddr(mac_str, sizeof(mac_str)) < 0)
	{
		DPRINTF(E_OFF, L_GENERAL, "No MAC address found.  Falling back to generic UUID.\n");
		strcpy(mac_str, "554e4b4e4f57");
	}
	snprintf(uuidvalue+5, UUIDVALUE_MAX_LEN-5, "4d696e69-444c-164e-9d41-%s", mac_str);

	getfriendlyname(friendly_name, FRIENDLYNAME_MAX_LEN);
	
	runtime_vars.port = 8200;
	runtime_vars.notify_interval = 895;	/* seconds between SSDP announces */
	runtime_vars.max_connections = 50;
	runtime_vars.root_container = NULL;
	runtime_vars.ifaces[0] = NULL;

	/* read options file first since
	 * command line arguments have final say */
	if (readoptionsfile(optionsfile) < 0)
	{
		/* only error if file exists or using -f */
		if(access(optionsfile, F_OK) == 0 || options_flag)
			DPRINTF(E_FATAL, L_GENERAL, "Error reading configuration file %s\n", optionsfile);
	}

	for (i=0; i<num_options; i++)
	{
		switch (ary_options[i].id)
		{
		case UPNPIFNAME:
			for (string = ary_options[i].value; (word = strtok(string, ",")); string = NULL)
			{
				if (ifaces >= MAX_LAN_ADDR)
				{
					DPRINTF(E_ERROR, L_GENERAL, "Too many interfaces (max: %d), ignoring %s\n",
						MAX_LAN_ADDR, word);
					break;
				}
				while (isspace(*word))
					word++;
				runtime_vars.ifaces[ifaces++] = word;
			}
			break;
		case UPNPPORT:
			runtime_vars.port = atoi(ary_options[i].value);
			break;
		case UPNPPRESENTATIONURL:
			presurl = ary_options[i].value;
			break;
		case UPNPNOTIFY_INTERVAL:
			runtime_vars.notify_interval = atoi(ary_options[i].value);
			break;
		case UPNPSERIAL:
			strncpyt(serialnumber, ary_options[i].value, SERIALNUMBER_MAX_LEN);
			break;				
		case UPNPMODEL_NAME:
			strncpyt(modelname, ary_options[i].value, MODELNAME_MAX_LEN);
			break;
		case UPNPMODEL_NUMBER:
			strncpyt(modelnumber, ary_options[i].value, MODELNUMBER_MAX_LEN);
			break;
		case UPNPFRIENDLYNAME:
			strncpyt(friendly_name, ary_options[i].value, FRIENDLYNAME_MAX_LEN);
			break;
		case UPNPMEDIADIR:
			types = ALL_MEDIA;
			path = ary_options[i].value;
			word = strchr(path, ',');
			if (word && (access(path, F_OK) != 0))
			{
				types = 0;
				while (*path)
				{
					if (*path == ',')
					{
						path++;
						break;
					}
					else if (*path == 'A' || *path == 'a')
						types |= TYPE_AUDIO;
					else if (*path == 'V' || *path == 'v')
						types |= TYPE_VIDEO;
					else if (*path == 'P' || *path == 'p')
						types |= TYPE_IMAGE;
					else
						DPRINTF(E_FATAL, L_GENERAL, "Media directory entry not understood [%s]\n",
							ary_options[i].value);
					path++;
				}
			}
			path = realpath(path, buf);
			if (!path || access(path, F_OK) != 0)
			{
				DPRINTF(E_ERROR, L_GENERAL, "Media directory \"%s\" not accessible [%s]\n",
					ary_options[i].value, strerror(errno));
				break;
			}
			media_dir = calloc(1, sizeof(struct media_dir_s));
			media_dir->path = strdup(path);
			media_dir->types = types;
			if (media_dirs)
			{
				struct media_dir_s *all_dirs = media_dirs;
				while( all_dirs->next )
					all_dirs = all_dirs->next;
				all_dirs->next = media_dir;
			}
			else
				media_dirs = media_dir;
			break;
		case UPNPALBUMART_NAMES:
			for (string = ary_options[i].value; (word = strtok(string, "/")); string = NULL)
			{
				struct album_art_name_s * this_name = calloc(1, sizeof(struct album_art_name_s));
				int len = strlen(word);
				if (word[len-1] == '*')
				{
					word[len-1] = '\0';
					this_name->wildcard = 1;
				}
				this_name->name = strdup(word);
				if (album_art_names)
				{
					struct album_art_name_s * all_names = album_art_names;
					while( all_names->next )
						all_names = all_names->next;
					all_names->next = this_name;
				}
				else
					album_art_names = this_name;
			}
			break;
		case UPNPDBDIR:
			path = realpath(ary_options[i].value, buf);
			if (!path)
				path = (ary_options[i].value);
			make_dir(path, S_ISVTX|S_IRWXU|S_IRWXG|S_IRWXO);
			if (access(path, F_OK) != 0)
				DPRINTF(E_FATAL, L_GENERAL, "Database path not accessible! [%s]\n", path);
			strncpyt(db_path, path, sizeof(db_path));
			break;
		case UPNPLOGDIR:
			path = realpath(ary_options[i].value, buf);
			if (!path)
				path = ary_options[i].value;
			if (snprintf(log_path, sizeof(log_path), "%s", path) > sizeof(log_path))
				DPRINTF(E_FATAL, L_GENERAL, "Log path too long! [%s]\n", path);
			make_dir(log_path, S_ISVTX|S_IRWXU|S_IRWXG|S_IRWXO);
			break;
		case UPNPLOGLEVEL:
			log_level = ary_options[i].value;
			break;
		case UPNPINOTIFY:
			if (!strtobool(ary_options[i].value))
				CLEARFLAG(INOTIFY_MASK);
			break;
		case ENABLE_TIVO:
			if (strtobool(ary_options[i].value))
				SETFLAG(TIVO_MASK);
			break;
		case ENABLE_DLNA_STRICT:
			if (strtobool(ary_options[i].value))
				SETFLAG(DLNA_STRICT_MASK);
			break;
		case ROOT_CONTAINER:
			switch (ary_options[i].value[0]) {
			case '.':
				runtime_vars.root_container = NULL;
				break;
			case 'B':
			case 'b':
				runtime_vars.root_container = BROWSEDIR_ID;
				break;
			case 'M':
			case 'm':
				runtime_vars.root_container = MUSIC_ID;
				break;
			case 'V':
			case 'v':
				runtime_vars.root_container = VIDEO_ID;
				break;
			case 'P':
			case 'p':
				runtime_vars.root_container = IMAGE_ID;
				break;
			default:
				runtime_vars.root_container = ary_options[i].value;
				DPRINTF(E_WARN, L_GENERAL, "Using arbitrary root container [%s]\n",
					ary_options[i].value);
				break;
			}
			break;
		case UPNPMINISSDPDSOCKET:
			minissdpdsocketpath = ary_options[i].value;
			break;
		case UPNPUUID:
			strcpy(uuidvalue+5, ary_options[i].value);
			break;
		case USER_ACCOUNT:
			uid = strtoul(ary_options[i].value, &string, 0);
			if (*string)
			{
				/* Symbolic username given, not UID. */
				struct passwd *entry = getpwnam(ary_options[i].value);
				if (!entry)
					DPRINTF(E_FATAL, L_GENERAL, "Bad user '%s'.\n",
						ary_options[i].value);
				uid = entry->pw_uid;
				if (!gid)
					gid = entry->pw_gid;
			}
			break;
		case FORCE_SORT_CRITERIA:
			force_sort_criteria = ary_options[i].value;
			if (force_sort_criteria[0] == '!')
			{
				SETFLAG(FORCE_ALPHASORT_MASK);
				force_sort_criteria++;
			}
			break;
		case MAX_CONNECTIONS:
			runtime_vars.max_connections = atoi(ary_options[i].value);
			break;
		case MERGE_MEDIA_DIRS:
			if (strtobool(ary_options[i].value))
				SETFLAG(MERGE_MEDIA_DIRS_MASK);
			break;
		case WIDE_LINKS:
			if (strtobool(ary_options[i].value))
				SETFLAG(WIDE_LINKS_MASK);
			break;
		case TIVO_DISCOVERY:
			if (strcasecmp(ary_options[i].value, "beacon") == 0)
				CLEARFLAG(TIVO_BONJOUR_MASK);
			break;
		case ENABLE_SUBTITLES:
			if (!strtobool(ary_options[i].value))
				CLEARFLAG(SUBTITLES_MASK);
			break;
		default:
			DPRINTF(E_ERROR, L_GENERAL, "Unknown option in file %s\n",
				optionsfile);
		}
	}
	if (!log_path[0])
		strncpyt(log_path, DEFAULT_LOG_PATH, sizeof(log_path));
	if (!db_path[0])
		strncpyt(db_path, DEFAULT_DB_PATH, sizeof(db_path));

	/* command line arguments processing */
	for (i=1; i<argc; i++)
	{
		if (argv[i][0] != '-')
		{
			DPRINTF(E_FATAL, L_GENERAL, "Unknown option: %s\n", argv[i]);
		}
		else if (strcmp(argv[i], "--help") == 0)
		{
			runtime_vars.port = -1;
			break;
		}
		else switch(argv[i][1])
		{
		case 't':
			if (i+1 < argc)
				runtime_vars.notify_interval = atoi(argv[++i]);
			else
				DPRINTF(E_FATAL, L_GENERAL, "Option -%c takes one argument.\n", argv[i][1]);
			break;
		case 's':
			if (i+1 < argc)
				strncpyt(serialnumber, argv[++i], SERIALNUMBER_MAX_LEN);
			else
				DPRINTF(E_FATAL, L_GENERAL, "Option -%c takes one argument.\n", argv[i][1]);
			break;
		case 'm':
			if (i+1 < argc)
				strncpyt(modelnumber, argv[++i], MODELNUMBER_MAX_LEN);
			else
				DPRINTF(E_FATAL, L_GENERAL, "Option -%c takes one argument.\n", argv[i][1]);
			break;
		case 'p':
			if (i+1 < argc)
				runtime_vars.port = atoi(argv[++i]);
			else
				DPRINTF(E_FATAL, L_GENERAL, "Option -%c takes one argument.\n", argv[i][1]);
			break;
		case 'P':
			if (i+1 < argc)
			{
				if (argv[++i][0] != '/')
					DPRINTF(E_FATAL, L_GENERAL, "Option -%c requires an absolute filename.\n", argv[i-1][1]);
				else
					pidfilename = argv[i];
			}
			else
				DPRINTF(E_FATAL, L_GENERAL, "Option -%c takes one argument.\n", argv[i][1]);
			break;
		case 'd':
			debug_flag = 1;
		case 'v':
			verbose_flag = 1;
			break;
		case 'L':
			SETFLAG(NO_PLAYLIST_MASK);
			break;
		case 'w':
			if (i+1 < argc)
				presurl = argv[++i];
			else
				DPRINTF(E_FATAL, L_GENERAL, "Option -%c takes one argument.\n", argv[i][1]);
			break;
		case 'i':
			if (i+1 < argc)
			{
				i++;
				if (ifaces >= MAX_LAN_ADDR)
				{
					DPRINTF(E_ERROR, L_GENERAL, "Too many interfaces (max: %d), ignoring %s\n",
						MAX_LAN_ADDR, argv[i]);
					break;
				}
				runtime_vars.ifaces[ifaces++] = argv[i];
			}
			else
				DPRINTF(E_FATAL, L_GENERAL, "Option -%c takes one argument.\n", argv[i][1]);
			break;
		case 'f':
			i++;	/* discarding, the config file is already read */
			break;
		case 'h':
			runtime_vars.port = -1; // triggers help display
			break;
		case 'r':
			SETFLAG(RESCAN_MASK);
			break;
		case 'R':
			clean_cache();
			break;
		case 'u':
			if (i+1 != argc)
			{
				i++;
				uid = strtoul(argv[i], &string, 0);
				if (*string)
				{
					/* Symbolic username given, not UID. */
					struct passwd *entry = getpwnam(argv[i]);
					if (!entry)
						DPRINTF(E_FATAL, L_GENERAL, "Bad user '%s'.\n", argv[i]);
					uid = entry->pw_uid;
					if (!gid)
						gid = entry->pw_gid;
				}
			}
			else
				DPRINTF(E_FATAL, L_GENERAL, "Option -%c takes one argument.\n", argv[i][1]);
			break;
		case 'g':
			if (i+1 != argc)
			{
				i++;
				gid = strtoul(argv[i], &string, 0);
				if (*string)
				{
					/* Symbolic group given, not GID. */
					struct group *grp = getgrnam(argv[i]);
					if (!grp)
						DPRINTF(E_FATAL, L_GENERAL, "Bad group '%s'.\n", argv[i]);
					gid = grp->gr_gid;
				}
			}
			else
				DPRINTF(E_FATAL, L_GENERAL, "Option -%c takes one argument.\n", argv[i][1]);
			break;
#if defined(__linux__) || defined(__APPLE__)
		case 'S':
			SETFLAG(SYSTEMD_MASK);
			break;
#endif
		case 'V':
			printf("Version " MINIDLNA_VERSION "\n");
			exit(0);
			break;
		default:
			DPRINTF(E_ERROR, L_GENERAL, "Unknown option: %s\n", argv[i]);
			runtime_vars.port = -1; // triggers help display
		}
	}

	if (runtime_vars.port <= 0)
	{
		printf("Usage:\n\t"
			"%s [-d] [-v] [-f config_file] [-p port]\n"
			"\t\t[-i network_interface] [-u uid_to_run_as] [-g group_to_run_as]\n"
			"\t\t[-t notify_interval] [-P pid_filename]\n"
			"\t\t[-s serial] [-m model_number]\n"
#ifdef __linux__
			"\t\t[-w url] [-r] [-R] [-L] [-S] [-V] [-h]\n"
#else
			"\t\t[-w url] [-r] [-R] [-L] [-V] [-h]\n"
#endif
			"\nNotes:\n\tNotify interval is in seconds. Default is 895 seconds.\n"
			"\tDefault pid file is %s.\n"
			"\tWith -d minidlna will run in debug mode (not daemonize).\n"
			"\t-w sets the presentation url. Default is http address on port 80\n"
			"\t-v enables verbose output\n"
			"\t-h displays this text\n"
			"\t-r forces a rescan\n"
			"\t-R forces a rebuild\n"
			"\t-L do not create playlists\n"
#if defined(__linux__) || defined(__APPLE__)
			"\t-S changes behaviour for systemd/launchd\n"
#endif
			"\t-V print the version number\n",
			argv[0], pidfilename);
		return 1;
	}

	if (verbose_flag)
	{
		strcpy(log_str+65, "debug");
		log_level = log_str;
	}
	else if (!log_level)
		log_level = log_str;

	/* Set the default log to stdout */
	if (debug_flag)
	{
		pid = getpid();
		strcpy(log_str+65, "maxdebug");
		log_level = log_str;
		log_path[0] = '\0';
	}
	else if (GETFLAG(SYSTEMD_MASK))
	{
		pid = getpid();
		log_path[0] = '\0';
	}
	else
	{
		pid = process_daemonize();
		if (access(db_path, F_OK) != 0)
			make_dir(db_path, S_ISVTX|S_IRWXU|S_IRWXG|S_IRWXO);
	}
	if (log_init(log_level) < 0)
		DPRINTF(E_FATAL, L_GENERAL, "Failed to open log file '%s/" LOGFILE_NAME "': %s\n",
			log_path, strerror(errno));

	if (process_check_if_running(pidfilename) < 0)
		DPRINTF(E_FATAL, L_GENERAL, SERVER_NAME " is already running. EXITING.\n");

	set_startup_time();

	/* presentation url */
	if (presurl)
		strncpyt(presentationurl, presurl, PRESENTATIONURL_MAX_LEN);
	else
		strcpy(presentationurl, "/");

	/* set signal handlers */
	memset(&sa, 0, sizeof(struct sigaction));
	sa.sa_handler = sigterm;
	if (sigaction(SIGTERM, &sa, NULL))
		DPRINTF(E_FATAL, L_GENERAL, "Failed to set %s handler. EXITING.\n", "SIGTERM");
	if (sigaction(SIGINT, &sa, NULL))
		DPRINTF(E_FATAL, L_GENERAL, "Failed to set %s handler. EXITING.\n", "SIGINT");
	if (signal(SIGPIPE, SIG_IGN) == SIG_ERR)
		DPRINTF(E_FATAL, L_GENERAL, "Failed to set %s handler. EXITING.\n", "SIGPIPE");
	if (signal(SIGHUP, &sighup) == SIG_ERR)
		DPRINTF(E_FATAL, L_GENERAL, "Failed to set %s handler. EXITING.\n", "SIGHUP");
	if (signal(SIGUSR2, SIG_IGN) == SIG_ERR)
		DPRINTF(E_FATAL, L_GENERAL, "Failed to set %s handler. EXITING.\n", "SIGUSR2");
	signal(SIGUSR1, &sigusr1);
	sa.sa_handler = process_handle_child_termination;
	if (sigaction(SIGCHLD, &sa, NULL))
		DPRINTF(E_FATAL, L_GENERAL, "Failed to set %s handler. EXITING.\n", "SIGCHLD");

	if (writepidfile(pidfilename, pid, uid) != 0)
		pidfilename = NULL;

	if (uid > 0)
	{
		struct stat st;
		if (stat(db_path, &st) == 0 && st.st_uid != uid && chown(db_path, uid, -1) != 0)
			DPRINTF(E_ERROR, L_GENERAL, "Unable to set db_path [%s] ownership to %d: %s\n",
				db_path, uid, strerror(errno));
	}

	if (gid > 0 && setgid(gid) == -1)
		DPRINTF(E_FATAL, L_GENERAL, "Failed to switch to gid '%d'. [%s] EXITING.\n",
			gid, strerror(errno));

	if (uid > 0 && setuid(uid) == -1)
		DPRINTF(E_FATAL, L_GENERAL, "Failed to switch to uid '%d'. [%s] EXITING.\n",
			uid, strerror(errno));

	children = calloc(runtime_vars.max_connections, sizeof(struct child));
	if (!children)
	{
		DPRINTF(E_ERROR, L_GENERAL, "Allocation failed\n");
		return 1;
	}

	if ((error = event_module.init()) != 0)
		DPRINTF(E_FATAL, L_GENERAL, "Failed to init event module. "
		    "[%s] EXITING.\n", strerror(error));

	return 0;
}

#ifdef HAVE_WATCH
void
start_monitor()
{

	if (!GETFLAG(INOTIFY_MASK))
		return;

	lav_register_all();
	monitor_start();
}
#endif

/* === main === */
/* process HTTP or SSDP requests */
int
main(int argc, char **argv)
{
	int ret, i;
	int shttpl = -1;
	int smonitor = -1;
	struct upnphttp * e = 0;
	struct upnphttp * next;
	struct timeval tv, timeofday, lastnotifytime = {0, 0};
	time_t lastupdatetime = 0, lastdbtime = 0;
	int last_changecnt = 0;
	pid_t scanner_pid = 0;
	struct event ssdpev, httpev, monev;
#ifdef TIVO_SUPPORT
	uint8_t beacon_interval = 5;
	int sbeacon = -1;
	struct sockaddr_in tivo_bcast;
	struct timeval lastbeacontime = {0, 0};
	struct event beaconev;
#endif

	for (i = 0; i < L_MAX; i++)
		log_level[i] = E_WARN;

	ret = init(argc, argv);
	if (ret != 0)
		return 1;
	init_nls();

#ifdef __OpenBSD__
	struct media_dir_s *media_dir;

	for (media_dir = media_dirs; media_dir; media_dir = media_dir->next)
		if (unveil(media_dir->path, "r") == -1)
			DPRINTF(E_FATAL, L_GENERAL, "unveil: %s: %s\n", media_dir->path, strerror(errno));

	struct {
		const char *path;
		const char *perms;
	} uvs[] = {
		{ db_path,     "rwc" },
		{ log_path,    "rwc" },
		{ pidfilename, "rwc" },
		{ NULL,        NULL  }
	}, *uv = uvs;

	for (; uv->path && uv->perms; uv++) {
		if (!uv->path[0])
			continue;
		DPRINTF(E_INFO, L_GENERAL, "unveil: '%s'\n", uv->path);
		if (unveil(uv->path, uv->perms) == -1)
			DPRINTF(E_FATAL, L_GENERAL, "unveil: %s: %s\n", uv->path, strerror(errno));
	}

	if (pledge("stdio rpath wpath cpath inet mcast fattr flock unix proc", NULL) == -1)
		DPRINTF(E_FATAL, L_GENERAL, "pledge: %s\n", strerror(errno));
#endif

	DPRINTF(E_WARN, L_GENERAL, "Starting " SERVER_NAME " version " MINIDLNA_VERSION ".\n");
	if (sqlite3_libversion_number() < 3005001)
	{
		DPRINTF(E_WARN, L_GENERAL, "SQLite library is old.  Please use version 3.5.1 or newer.\n");
	}

	LIST_INIT(&upnphttphead);

	ret = open_db(NULL);
	if (ret == 0)
	{
		updateID = sql_get_int_field(db, "SELECT VALUE from SETTINGS where KEY = 'UPDATE_ID'");
		if (updateID == -1)
			ret = -1;
	}
	check_db(db, ret, &scanner_pid);
	lastdbtime = _get_dbtime();

#ifdef HAVE_WATCH
	if (!GETFLAG(SCANNING_MASK))
		start_monitor();
#endif

	smonitor = OpenAndConfMonitorSocket();
	if (smonitor > 0)
	{
		monev = (struct event ){ .fd = smonitor, .rdwr = EVENT_READ, .process = ProcessMonitorEvent };
		event_module.add(&monev);
	}

	sssdp = OpenAndConfSSDPReceiveSocket();
	if (sssdp < 0)
	{
		DPRINTF(E_INFO, L_GENERAL, "Failed to open socket for receiving SSDP. Trying to use MiniSSDPd\n");
		reload_ifaces(0);	/* populate lan_addr[0].str */
		if (SubmitServicesToMiniSSDPD(lan_addr[0].str, runtime_vars.port) < 0)
			DPRINTF(E_FATAL, L_GENERAL, "Failed to connect to MiniSSDPd. EXITING");
	}
	else
	{
		ssdpev = (struct event ){ .fd = sssdp, .rdwr = EVENT_READ, .process = ProcessSSDPRequest };
		event_module.add(&ssdpev);
	}

	/* open socket for HTTP connections. */
	shttpl = OpenAndConfHTTPSocket(runtime_vars.port);
	if (shttpl < 0)
		DPRINTF(E_FATAL, L_GENERAL, "Failed to open socket for HTTP. EXITING\n");
	DPRINTF(E_WARN, L_GENERAL, "HTTP listening on port %d\n", runtime_vars.port);
	httpev = (struct event ){ .fd = shttpl, .rdwr = EVENT_READ, .process = ProcessListen };
	event_module.add(&httpev);

	if (gettimeofday(&timeofday, 0) < 0)
		DPRINTF(E_FATAL, L_GENERAL, "gettimeofday(): %s\n", strerror(errno));

#ifdef TIVO_SUPPORT
	if (GETFLAG(TIVO_MASK))
	{
		DPRINTF(E_WARN, L_GENERAL, "TiVo support is enabled.\n");
		/* Add TiVo-specific randomize function to sqlite */
		ret = sqlite3_create_function(db, "tivorandom", 1, SQLITE_UTF8, NULL, &TiVoRandomSeedFunc, NULL, NULL);
		if (ret != SQLITE_OK)
			DPRINTF(E_ERROR, L_TIVO, "ERROR: Failed to add sqlite randomize function for TiVo!\n");
		if (GETFLAG(TIVO_BONJOUR_MASK))
		{
			tivo_bonjour_register();
		}
		else
		{
			/* open socket for sending Tivo notifications */
			sbeacon = OpenAndConfTivoBeaconSocket();
			if(sbeacon < 0)
				DPRINTF(E_FATAL, L_GENERAL, "Failed to open sockets for sending Tivo beacon notify "
					"messages. EXITING\n");
			beaconev = (struct event ){ .fd = sbeacon, .rdwr = EVENT_READ, .process = ProcessTiVoBeacon };
			event_module.add(&beaconev);
			tivo_bcast.sin_family = AF_INET;
			tivo_bcast.sin_addr.s_addr = htonl(getBcastAddress());
			tivo_bcast.sin_port = htons(2190);
			lastbeacontime = timeofday;
		}
	}
#endif

	reload_ifaces(0);	/* sends SSDP notifies */
	lastnotifytime = timeofday;

	/* main loop */
	while (!quitting)
	{
		/* Check if we need to send SSDP NOTIFY messages and do it if
		 * needed */
		tv = lastnotifytime;
		tv.tv_sec += runtime_vars.notify_interval;
		if (timevalcmp(&timeofday, &tv, >=))
		{
			DPRINTF(E_DEBUG, L_SSDP, "Sending SSDP notifies\n");
			for (i = 0; i < n_lan_addr; i++)
			{
				SendSSDPNotifies(lan_addr[i].snotify, lan_addr[i].str,
					runtime_vars.port, runtime_vars.notify_interval);
			}
			lastnotifytime = timeofday;
			tv.tv_sec = runtime_vars.notify_interval;
			tv.tv_usec = 0;
		}
		else
		{
			timevalsub(&tv, &timeofday);
		}
#ifdef TIVO_SUPPORT
		if (sbeacon >= 0)
		{
			struct timeval beacontv;

			beacontv = lastbeacontime;
			beacontv.tv_sec += beacon_interval;
			if (timevalcmp(&timeofday, &beacontv, >=))
			{
				sendBeaconMessage(sbeacon, &tivo_bcast, sizeof(struct sockaddr_in), 1);
				lastbeacontime = timeofday;
				/* Beacons should be sent every 5 seconds or
				 * so for the first minute, then every minute
				 * or so thereafter. */
				if (beacon_interval == 5 && (timeofday.tv_sec - startup_time) > 60)
					beacon_interval = 60;
				beacontv.tv_sec = beacon_interval;
				beacontv.tv_usec = 0;
			}
			else
			{
				timevalsub(&beacontv, &timeofday);
			}
			if (timevalcmp(&tv, &beacontv, >))
				tv = beacontv;
		}
#endif

		if (GETFLAG(SCANNING_MASK)) {
			if (kill(scanner_pid, 0) != 0) {
				DPRINTF(E_INFO, L_GENERAL, "Scanner exited\n");
				CLEARFLAG(SCANNING_MASK);
				if (_get_dbtime() != lastdbtime)
					updateID++;
#ifdef HAVE_WATCH
				start_monitor();
#endif
			} else
				/* Keep checking for the scanner every sec. */
				tv.tv_sec = 1;
		}

		event_module.process(&tv);
		if (quitting)
			goto shutdown;

		if (gettimeofday(&timeofday, 0) < 0)
			DPRINTF(E_FATAL, L_GENERAL, "gettimeofday(): %s\n", strerror(errno));

		upnpevents_gc();

		/* increment SystemUpdateID if the content database has changed,
		 * and if there is an active HTTP connection, at most once every 2 seconds */
		if (!LIST_EMPTY(&upnphttphead) &&
		    (timeofday.tv_sec >= (lastupdatetime + 2)))
		{
			if (GETFLAG(SCANNING_MASK))
			{
				time_t dbtime = _get_dbtime();
				if (dbtime != lastdbtime)
				{
					lastdbtime = dbtime;
					last_changecnt = -1;
				}
			}
			if (sqlite3_total_changes(db) != last_changecnt)
			{
				updateID++;
				last_changecnt = sqlite3_total_changes(db);
				upnp_event_var_change_notify(EContentDirectory);
				lastupdatetime = timeofday.tv_sec;
			}
		}
		/* delete finished HTTP connections */
		for (e = upnphttphead.lh_first; e != NULL; e = next)
		{
			next = e->entries.le_next;
			if(e->state >= 100)
			{
				LIST_REMOVE(e, entries);
				Delete_upnphttp(e);
			}
		}
	}

shutdown:
	/* kill the scanner */
	if (GETFLAG(SCANNING_MASK) && scanner_pid)
		kill(scanner_pid, SIGKILL);

	/* close out open sockets */
	while (upnphttphead.lh_first != NULL)
	{
		e = upnphttphead.lh_first;
		LIST_REMOVE(e, entries);
		Delete_upnphttp(e);
	}
	if (sssdp >= 0)
		close(sssdp);
	if (shttpl >= 0)
		close(shttpl);
#ifdef TIVO_SUPPORT
	if (sbeacon >= 0)
		close(sbeacon);
#endif
	if (smonitor >= 0)
		close(smonitor);
	
	for (i = 0; i < n_lan_addr; i++)
	{
		SendSSDPGoodbyes(lan_addr[i].snotify);
		close(lan_addr[i].snotify);
	}

#ifdef HAVE_WATCH
	monitor_stop();
#endif

	/* kill other child processes */
	process_reap_children();
	free(children);

	event_module.fini();

	sql_exec(db, "UPDATE SETTINGS set VALUE = '%u' where KEY = 'UPDATE_ID'", updateID);
	sqlite3_close(db);

	upnpevents_removeSubscribers();

	if (pidfilename && unlink(pidfilename) < 0)
		DPRINTF(E_ERROR, L_GENERAL, "Failed to remove pidfile %s: %s\n", pidfilename, strerror(errno));

	log_close();
	freeoptions();

	exit(EXIT_SUCCESS);
}

