#ifndef LOG_H
#define LOG_H

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef DEBUG
# define debug logit
#else
# define debug(...)  do {} while (0)
#endif

#ifndef NDEBUG
# define logit(format, ...) \
	internal_logit (__FILE__, __LINE__, __FUNCTION__, format, \
	## __VA_ARGS__)
#else
# define logit(...)  do {} while (0)
#endif

void internal_logit (const char *file, const int line, const char *function,
		const char *format, ...) ATTR_PRINTF(4, 5);

#ifndef NDEBUG
# define LOGIT_ONLY
#else
# define LOGIT_ONLY ATTR_UNUSED
#endif

#if !defined(NDEBUG) && defined(DEBUG)
# define DEBUG_ONLY
#else
# define DEBUG_ONLY ATTR_UNUSED
#endif

void log_init_stream (FILE *f, const char *fn);
void log_circular_start ();
void log_circular_log ();
void log_circular_reset ();
void log_circular_stop ();
void log_close ();

#ifndef NDEBUG
void log_signal (int sig);
#else
# define log_signal(...) do {} while (0)
#endif

#ifdef __cplusplus
}
#endif

#endif
