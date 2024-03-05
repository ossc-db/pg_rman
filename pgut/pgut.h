/*-------------------------------------------------------------------------
 *
 * pgut.h
 *
 * Copyright (c) 2009-2023, NIPPON TELEGRAPH AND TELEPHONE CORPORATION
 *
 *-------------------------------------------------------------------------
 */

#ifndef PGUT_H
#define PGUT_H

#include "pg_rman.h"

#include "libpq-fe.h"
#include "pqexpbuffer.h"

#include <assert.h>
#include <sys/time.h>

#if !defined(C_H) && !defined(__cplusplus)
#ifndef bool
typedef char bool;
#endif
#ifndef true
#define true	((bool) 1)
#endif
#ifndef false
#define false	((bool) 0)
#endif
#endif

#define INFINITE_STR		"INFINITE"

typedef enum YesNo
{
	DEFAULT,
	NO,
	YES
} YesNo;

typedef enum pgut_optsrc
{
	SOURCE_DEFAULT,
	SOURCE_ENV,
	SOURCE_FILE,
	SOURCE_CMDLINE,
	SOURCE_CONST
} pgut_optsrc;

/*
 * type:
 *	b: bool (true)
 *	B: bool (false)
 *  f: pgut_optfn
 *	i: 32bit signed integer
 *	u: 32bit unsigned integer
 *	I: 64bit signed integer
 *	U: 64bit unsigned integer
 *	s: string
 *  t: time_t
 *	y: YesNo (YES)
 *	Y: YesNo (NO)
 */
typedef struct pgut_option
{
	char		type;
	char		sname;		/* short name */
	const char *lname;		/* long name */
	void	   *var;		/* pointer to variable */
	pgut_optsrc	allowed;	/* allowed source */
	pgut_optsrc	source;		/* actual source */
} pgut_option;

typedef void (*pgut_optfn) (pgut_option *opt, const char *arg);
typedef void (*pgut_atexit_callback)(bool fatal, void *userdata);

/*
 * pgut client variables and functions
 */
extern const char  *PROGRAM_NAME;
extern const char  *PROGRAM_VERSION;
extern const char  *PROGRAM_URL;
extern const char  *PROGRAM_ISSUES;

extern void	pgut_help(bool details);

/*
 * pgut framework variables and functions
 */
extern const char  *dbname;
extern const char  *host;
extern const char  *port;
extern const char  *username;
extern bool			quiet;

#ifndef PGUT_NO_PROMPT
extern YesNo	prompt_password;
#endif

extern PGconn	   *connection;
extern bool			interrupted;

extern void help(bool details);
extern int pgut_getopt(int argc, char **argv, pgut_option options[]);
extern void pgut_readopt(const char *path, pgut_option options[], int elevel);
extern void pgut_atexit_push(pgut_atexit_callback callback, void *userdata);
extern void pgut_atexit_pop(pgut_atexit_callback callback, void *userdata);

/*
 * Database connections
 */
extern PGconn *pgut_connect(void);
extern void pgut_disconnect(PGconn *conn);
extern PGresult *pgut_execute(PGconn* conn, const char *query, int nParams, const char **params);
extern void pgut_command(PGconn* conn, const char *query, int nParams, const char **params);
extern bool pgut_send(PGconn* conn, const char *query, int nParams, const char **params);
extern int pgut_wait(int num, PGconn *connections[], struct timeval *timeout);

extern PGconn *reconnect(void);
extern void disconnect(void);
extern PGconn *save_connection(void);
extern void restore_saved_connection(void);

extern const char *pgut_get_host(void);
extern const char *pgut_get_port(void);
extern void pgut_set_host(const char *new_host);
extern void pgut_set_port(const char *new_port);

extern PGresult *execute(const char *query, int nParams, const char **params);
extern void command(const char *query, int nParams, const char **params);

/*
 * memory allocators
 */
extern void *pgut_malloc(size_t size);
extern void *pgut_realloc(void *p, size_t size);
extern char *pgut_strdup(const char *str);
extern char *strdup_with_len(const char *str, size_t len);
extern char *strdup_trim(const char *str);

#define pgut_new(type)			((type *) pgut_malloc(sizeof(type)))
#define pgut_newarray(type, n)	((type *) pgut_malloc(sizeof(type) * (n)))

/*
 * file operations
 */
extern FILE *pgut_fopen(const char *path, const char *mode, bool missing_ok);

/*
 * elog
 */
#define DEBUG2		(-5)
#define DEBUG		(-4)
#define INFO		(-3)	/* always shown to stderr */
#define NOTICE		(-2)	/* behavior changes or skips as user expected. */
#define WARNING		(-1)	/* unexpected by user */
#define HELP		1		/* show help messages */
#define ERROR		2		/* ordinal errors */
#define FATAL		3		/* errors happened by interruption */
#define PANIC		4

#define ERROR_SYSTEM			10	/* I/O or system error */
#define ERROR_NOMEM				11	/* memory exhausted */
#define ERROR_ARGS				12	/* some configurations are invalid */
#define ERROR_INTERRUPTED		13	/* interrupted by signal */
#define ERROR_PG_COMMAND		14	/* PostgreSQL query or command error */
#define ERROR_PG_CONNECT		15	/* PostgreSQL connection error */

#undef elog
#undef ereport
#define ereport(elevel, rest) \
	(pgut_errstart(elevel) ? (pgut_errfinish rest) : (void) 0)

extern void elog(int elevel, const char *fmt, ...)
__attribute__((format(printf, 2, 3)));
extern const char * format_elevel(int elevel);
extern bool pgut_errstart(int elevel);
extern void pgut_errfinish(int dummy, ...);
extern void pgut_error(int elevel, const char *msg, const char *detail, const char *hint);
extern int errcode(int errcode);
extern int errmsg(const char *fmt, ...)
__attribute__((format(printf, 1, 2)));
extern int errdetail(const char *fmt, ...)
__attribute__((format(printf, 1, 2)));
extern int errhint(const char *fmt, ...)
__attribute__((format(printf, 1, 2)));

/*
 * Assert
 */
#undef Assert
#undef AssertArg
#undef AssertMacro

#ifdef USE_ASSERT_CHECKING
#define Assert(x)		assert(x)
#define AssertArg(x)	assert(x)
#define AssertMacro(x)	assert(x)
#else
#define Assert(x)		((void) 0)
#define AssertArg(x)	((void) 0)
#define AssertMacro(x)	((void) 0)
#endif

/*
 * StringInfo and string operations
 */
#define STRINGINFO_H

#define StringInfoData			PQExpBufferData
#define StringInfo				PQExpBuffer
#define makeStringInfo			createPQExpBuffer
#define initStringInfo			initPQExpBuffer
#define freeStringInfo			destroyPQExpBuffer
#define termStringInfo			termPQExpBuffer
#define resetStringInfo			resetPQExpBuffer
#define enlargeStringInfo		enlargePQExpBuffer
#define printfStringInfo		printfPQExpBuffer	/* reset + append */
#define appendStringInfo		appendPQExpBuffer
#define appendStringInfoString	appendPQExpBufferStr
#define appendStringInfoChar	appendPQExpBufferChar
#define appendBinaryStringInfo	appendBinaryPQExpBuffer

extern bool	appendStringInfoVA_c(StringInfo str, const char *fmt, va_list args)
__attribute__((format(printf, 2, 0)));
extern int appendStringInfoFile(StringInfo str, FILE *fp);
extern int appendStringInfoFd(StringInfo str, int fd);
extern void trimStringBuffer(StringInfo str);

extern bool parse_bool(const char *value, bool *result);
extern bool parse_bool_with_len(const char *value, size_t len, bool *result);
extern bool parse_int32(const char *value, int32 *result);
extern bool parse_uint32(const char *value, uint32 *result);
extern bool parse_int64(const char *value, int64 *result);
extern bool parse_uint64(const char *value, uint64 *result);
extern bool parse_time(const char *value, time_t *time);
extern bool parse_pair(const char buffer[], char key[], char value[]);

#define IsSpace(c)		(isspace((unsigned char)(c)))
#define IsAlpha(c)		(isalpha((unsigned char)(c)))
#define IsAlnum(c)		(isalnum((unsigned char)(c)))
#define IsIdentHead(c)	(IsAlpha(c) || (c) == '_')
#define IsIdentBody(c)	(IsAlnum(c) || (c) == '_')
#define ToLower(c)		(tolower((unsigned char)(c)))
#define ToUpper(c)		(toupper((unsigned char)(c)))

/*
 * socket operations
 */
extern int wait_for_socket(int sock, struct timeval *timeout);
extern int wait_for_sockets(int nfds, fd_set *fds, struct timeval *timeout);

#ifdef WIN32
extern int sleep(unsigned int seconds);
extern int usleep(unsigned int usec);
#endif

#endif   /* PGUT_H */
