/*-------------------------------------------------------------------------
 *
 * pg_tde_fe.h
 *	  TDE frontend defines.
 *
 *-------------------------------------------------------------------------
 */
#ifdef FRONTEND
#include "postgres.h"

#include "common/logging.h"
#include "common/file_perm.h"

#define LWLockAcquire(lock, mode) NULL
#define LWLockRelease(lock_files) NULL
#define LWLock void
#define tde_lwlock_mk_files() NULL
#define tde_lwlock_mk_cache() NULL
#define tde_provider_info_lock() NULL

/*
 * Error handling
 */
static void fe_errmsg(const char *fmt, ...);
static void fe_errhint(const char *fmt, ...);
static void fe_errdetail(const char *fmt, ...);

int fe_error_level = 0;

#define errmsg(...) fe_errmsg(__VA_ARGS__)
#define errhint(...) fe_errhint(__VA_ARGS__)
#define errdetail(...) fe_errdetail(__VA_ARGS__)

#define errcode_for_file_access() NULL
#define errcode(e) NULL

#define elog(elevel, ...) pgtde_elog(elevel, __VA_ARGS__)
#define pgtde_elog(elevel, ...) \
	do {							\
		fe_error_level = elevel;	\
		fe_errmsg(__VA_ARGS__);		\
	} while(0)

#define ereport(elevel, ...) pgtde_ereport(elevel, __VA_ARGS__)
#define pgtde_ereport(elevel, ...)		\
	do {							\
		fe_error_level = elevel;	\
		__VA_ARGS__;				\
	} while(0)

void
fe_errmsg(const char *fmt, ...)
{
	va_list		ap;

	va_start(ap, fmt);

	if (fe_error_level >= ERROR)
		pg_log_error(fmt, ap);
	else if (fe_error_level >= WARNING)
		pg_log_warning(fmt, ap);
	else if (fe_error_level >= LOG)
		pg_log_info(fmt, ap);
	else
		pg_log_debug(fmt, ap);

	va_end(ap);
}

void
fe_errhint(const char *fmt, ...)
{
	va_list		ap;

	va_start(ap, fmt);

	if (fe_error_level >= ERROR)
		pg_log_error_hint(fmt, ap);
	else if (fe_error_level >= WARNING)
		pg_log_warning_hint(fmt, ap);
	else if (fe_error_level >= LOG)
		pg_log_info_hint(fmt, ap);
	else
		pg_log_debug_hint(fmt, ap);

	va_end(ap);
}
void
fe_errdetail(const char *fmt, ...)
{
	va_list		ap;

	va_start(ap, fmt);

	if (fe_error_level >= ERROR)
		pg_log_error_detail(fmt, ap);
	else if (fe_error_level >= WARNING)
		pg_log_warning_detail(fmt, ap);
	else if (fe_error_level >= LOG)
		pg_log_info_detail(fmt, ap);
	else
		pg_log_debug_detail(fmt, ap);

	va_end(ap);
}

#define pg_fsync(fd) fsync(fd)

#define BasicOpenFile(fname, flags) open(fname, flags, PG_FILE_MODE_OWNER)

#endif		/* FRONTEND */
