/* the Music Player Daemon (MPD)
 * Copyright (C) 2003-2007 by Warren Dukes (warren.dukes@gmail.com)
 * This project's homepage is: http://www.musicpd.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "log.h"

#include "conf.h"
#include "myfprintf.h"
#include "utils.h"
#include "os_compat.h"

#define LOG_DATE_BUF_SIZE 16
#define LOG_DATE_LEN (LOG_DATE_BUF_SIZE - 1)
static unsigned int logLevel = LOG_LEVEL_LOW;
static int warningFlushed;
static int stdout_mode = 1;
static char *warningBuffer;
static pthread_mutex_t warning_buffer_lock = PTHREAD_MUTEX_INITIALIZER;
static int out_fd = -1;
static int err_fd = -1;
static const char *out_filename;
static const char *err_filename;

static void redirect_logs(void)
{
	assert(out_fd >= 0);
	assert(err_fd >= 0);
	if (dup2(out_fd, STDOUT_FILENO) < 0)
		FATAL("problems dup2 stdout : %s\n", strerror(errno));
	if (dup2(err_fd, STDERR_FILENO) < 0)
		FATAL("problems dup2 stderr : %s\n", strerror(errno));
}

static const char *log_date(void)
{
	static char buf[LOG_DATE_BUF_SIZE];
	time_t t = time(NULL);
	strftime(buf, LOG_DATE_BUF_SIZE, "%b %d %H:%M : ", localtime(&t));
	return buf;
}

#define BUFFER_LENGTH	4096
static void buffer_warning(const char *fmt, va_list args)
{
	char buffer[BUFFER_LENGTH];
	char *tmp = buffer;
	size_t len = BUFFER_LENGTH;

	if (!stdout_mode) {
		memcpy(buffer, log_date(), LOG_DATE_LEN);
		tmp += LOG_DATE_LEN;
		len -= LOG_DATE_LEN;
	}

	vsnprintf(tmp, len, fmt, args);
	warningBuffer = appendToString(warningBuffer, buffer);

	va_end(args);
}

static void do_log(FILE *fp, const char *fmt, va_list args)
{
	if (!stdout_mode)
		fwrite(log_date(), LOG_DATE_LEN, 1, fp);
	fprintf(fp, "%08x: ", pthread_self());
	vfprintf(fp, fmt, args);
}

void flushWarningLog(void)
{
	char *s;

	pthread_mutex_lock(&warning_buffer_lock);
	s = warningBuffer;

	DEBUG("flushing warning messages\n");

	if (warningBuffer != NULL)
	{
		while (s != NULL) {
			char *next = strchr(s, '\n');
			if (next == NULL) break;
			*next = '\0';
			next++;
			fprintf(stderr, "%s\n", s);
			s = next;
		}

		warningBuffer = NULL;
	}
	warningFlushed = 1;
	pthread_mutex_unlock(&warning_buffer_lock);

	DEBUG("done flushing warning messages\n");
}

void initLog(const int verbose)
{
	ConfigParam *param;

	/* unbuffer stdout, stderr is unbuffered by default, leave it */
	setvbuf(stdout, (char *)NULL, _IONBF, 0);

	if (verbose) {
		logLevel = LOG_LEVEL_DEBUG;
		return;
	}
	if (!(param = getConfigParam(CONF_LOG_LEVEL)))
		return;
	if (0 == strcmp(param->value, "default")) {
		logLevel = LOG_LEVEL_LOW;
	} else if (0 == strcmp(param->value, "secure")) {
		logLevel = LOG_LEVEL_SECURE;
	} else if (0 == strcmp(param->value, "verbose")) {
		logLevel = LOG_LEVEL_DEBUG;
	} else {
		FATAL("unknown log level \"%s\" at line %i\n",
		      param->value, param->line);
	}
}

void open_log_files(const int use_stdout)
{
	mode_t prev;
	ConfigParam *param;

	if (use_stdout) {
		flushWarningLog();
		return;
	}

	prev = umask(0066);
	param = parseConfigFilePath(CONF_LOG_FILE, 1);
	out_filename = param->value;
	out_fd = open(out_filename, O_CREAT | O_WRONLY | O_APPEND, 0666);
	if (out_fd < 0)
		FATAL("problem opening log file \"%s\" (config line %i) for "
		      "writing\n", param->value, param->line);

	param = parseConfigFilePath(CONF_ERROR_FILE, 1);
	err_filename = param->value;
	err_fd = open(err_filename, O_CREAT | O_WRONLY | O_APPEND, 0666);
	if (err_fd < 0)
		FATAL("problem opening error file \"%s\" (config line %i) for "
		      "writing\n", param->value, param->line);

	umask(prev);
}

void setup_log_output(const int use_stdout)
{
	fflush(NULL);
	if (!use_stdout) {
		redirect_logs();
		stdout_mode = 0;
		flushWarningLog();
	}
}

#define log_func(func,level,fp) \
mpd_printf void func(const char *fmt, ...) \
{ \
	if ((int)logLevel >= level) {		\
		va_list args; \
		va_start(args, fmt); \
		do_log(fp, fmt, args); \
		va_end(args); \
	} \
}

log_func(ERROR, 0, stderr)
log_func(LOG, 0, stdout)
log_func(SECURE, LOG_LEVEL_SECURE, stdout)
log_func(DEBUG, LOG_LEVEL_DEBUG, stdout)

#undef log_func

void WARNING(const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);

	pthread_mutex_lock(&warning_buffer_lock);
	if (warningFlushed)
		do_log(stderr, fmt, args);
	else
		buffer_warning(fmt, args);
	pthread_mutex_unlock(&warning_buffer_lock);

	va_end(args);
}

mpd_printf mpd_noreturn void FATAL(const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	do_log(stderr, fmt, args);
	va_end(args);
	exit(EXIT_FAILURE);
}

int cycle_log_files(void)
{
	if (stdout_mode)
		return 0;
	assert(out_filename);
	assert(err_filename);

	DEBUG("Cycling log files...\n");
	close_log_files();

	out_fd = open(out_filename, O_CREAT | O_WRONLY | O_APPEND, 0600);
	if (out_fd < 0) {
		ERROR("error re-opening log file: %s\n", out_filename);
		return -1;
	}

	err_fd = open(err_filename, O_CREAT | O_WRONLY | O_APPEND, 0600);
	if (err_fd < 0) {
		ERROR("error re-opening error file: %s\n", err_filename);
		return -1;
	}

	redirect_logs();
	DEBUG("Done cycling log files\n");
	return 0;
}

void close_log_files(void)
{
	if (stdout_mode)
		return;
	assert(out_fd >= 0);
	assert(err_fd >= 0);
	xclose(out_fd);
	xclose(err_fd);
}

