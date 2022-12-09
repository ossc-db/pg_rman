/*-------------------------------------------------------------------------
 *
 * pg_rman.c: Backup/Recovery manager for PostgreSQL.
 *
 * Copyright (c) 2009-2021, NIPPON TELEGRAPH AND TELEPHONE CORPORATION
 *
 *-------------------------------------------------------------------------
 */

#include "pg_rman.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/stat.h>

const char *PROGRAM_VERSION	= "1.3.14";
const char *PROGRAM_URL		= "http://github.com/ossc-db/pg_rman";
const char *PROGRAM_ISSUES	= "http://github.com/ossc-db/pg_rman/issues";

/* path configuration */
char *backup_path;
char *pgdata;
char *arclog_path;
char *srvlog_path;
char *pgconf_path;

/* common configuration */
bool verbose = false;
bool progress = false;
bool check = false;

/* directory configuration */
pgBackup	current;

/* backup configuration */
static bool		smooth_checkpoint;
static int		keep_arclog_files = KEEP_INFINITE;
static int		keep_arclog_days = KEEP_INFINITE;
static int		keep_srvlog_files = KEEP_INFINITE;
static int		keep_srvlog_days = KEEP_INFINITE;
static int		keep_data_generations = KEEP_INFINITE;
static int		keep_data_days = KEEP_INFINITE;
static char		*standby_host = NULL;
static char		*standby_port = NULL;

/* restore configuration */
static char		   *target_time;
static char		   *target_xid;
static char		   *target_inclusive;
static char		   *target_tli_string;
static char		   *target_action;
static bool		is_hard_copy = false;

/* delete configuration */
static bool		force;

/* show configuration */
static bool			show_all = false;

static void opt_backup_mode(pgut_option *opt, const char *arg);
static void parse_range(pgBackupRange *range, const char *arg1, const char *arg2);

static pgut_option options[] =
{
	/* directory options */
	{ 's', 'D', "pgdata"		, &pgdata		, SOURCE_ENV },
	{ 's', 'A', "arclog-path"	, &arclog_path	, SOURCE_ENV },
	{ 's', 'B', "backup-path"	, &backup_path	, SOURCE_ENV },
	{ 's', 'S', "srvlog-path"	, &srvlog_path	, SOURCE_ENV },
	{ 's', 'G', "pgconf-path"	, &pgconf_path	, SOURCE_ENV },
	/* common options */
	{ 'b', 'v', "verbose"		, &verbose },
	{ 'b', 'P', "progress"		, &progress },
	{ 'b', 'c', "check"			, &check },
	/* backup options */
	{ 'f', 'b', "backup-mode"		    , opt_backup_mode			, SOURCE_ENV },
	{ 'b', 's', "with-serverlog"	    , &current.with_serverlog	, SOURCE_ENV },
	{ 'b', 'Z', "compress-data"		    , &current.compress_data	, SOURCE_ENV },
	{ 'b', 'C', "smooth-checkpoint"	    , &smooth_checkpoint		, SOURCE_ENV },
	{ 'b', 'F', "full-backup-on-error"	, &current.full_backup_on_error		, SOURCE_ENV },
	{ 's', 13, "standby-host"	, &standby_host		, SOURCE_ENV },
	{ 's', 14, "standby-port"	, &standby_port		, SOURCE_ENV },
	/* delete options */
	{ 'b', 'f', "force"	, &force		, SOURCE_ENV },
	/* options with only long name (keep-xxx) */
	{ 'i',  1, "keep-data-generations"	, &keep_data_generations, SOURCE_ENV },
	{ 'i',  2, "keep-data-days"			, &keep_data_days		, SOURCE_ENV },
	{ 'i',  3, "keep-arclog-files"		, &keep_arclog_files	, SOURCE_ENV },
	{ 'i',  4, "keep-arclog-days"		, &keep_arclog_days		, SOURCE_ENV },
	{ 'i',  5, "keep-srvlog-files"		, &keep_srvlog_files	, SOURCE_ENV },
	{ 'i',  6, "keep-srvlog-days"		, &keep_srvlog_days		, SOURCE_ENV },
	/* restore options */
	{ 's',  7, "recovery-target-time"		, &target_time		, SOURCE_ENV },
	{ 's',  8, "recovery-target-xid"		, &target_xid		, SOURCE_ENV },
	{ 's',  9, "recovery-target-inclusive"	, &target_inclusive	, SOURCE_ENV },
	{ 's', 10, "recovery-target-timeline"	, &target_tli_string, SOURCE_ENV },
	{ 's', 11, "recovery-target-action"		, &target_action	, SOURCE_ENV },
	{ 'b', 12, "hard-copy"	, &is_hard_copy		, SOURCE_ENV },
	/* catalog options */
	{ 'b', 'a', "show-all"		, &show_all },
	{ 0 }
};

/*
 * Entry point of pg_rman command.
 */
int
main(int argc, char *argv[])
{
	const char	   *cmd = NULL;
	const char	   *range1 = NULL;
	const char	   *range2 = NULL;
	bool			show_detail = false;
	pgBackupRange	range;
	int				i;

	/* do not buffer progress messages */
	setvbuf(stdout, 0, _IONBF, 0);	/* TODO: remove this */

	/* initialize configuration */
	catalog_init_config(&current);

	/* overwrite configuration with command line arguments */
	i = pgut_getopt(argc, argv, options);

	/* BACKUP_PATH is always required */
	if (backup_path == NULL)
		ereport(ERROR,
			(errcode(ERROR_ARGS),
			 errmsg("required parameter not specified: BACKUP_PATH (-B, --backup-path)")));

	for (; i < argc; i++)
	{
		if (cmd == NULL)
			cmd = argv[i];
		else if (pg_strcasecmp(argv[i], "detail") == 0 &&
				 pg_strcasecmp(cmd, "show") == 0)
			show_detail = true;
		else if (range1 == NULL)
			range1 = argv[i];
		else if (range2 == NULL)
			range2 = argv[i];
		else
			ereport(ERROR,
				(errcode(ERROR_ARGS),
				 errmsg("too many arguments")));
	}

	/* command argument (backup/restore/show/...) is required. */
	if (cmd == NULL)
	{
		help(false);
		return HELP;
	}

	/* get object range argument if any */
	if (range1 && range2)
		parse_range(&range, range1, range2);
	else if (range1)
		parse_range(&range, range1, "");
	else
		range.begin = range.end = 0;

	/* Read default configuration from file. */
	if (backup_path)
	{
		char	path[MAXPGPATH];
		/* Check if backup_path is directory. */
		struct stat stat_buf;
		int rc = stat(backup_path, &stat_buf);

		/* If rc == -1,  there is no file or directory. So it's OK. */
		if(rc != -1 && !S_ISDIR(stat_buf.st_mode))
			ereport(ERROR,
				(errcode(ERROR_ARGS),
				 errmsg("-B, --backup-path must be a path to directory")));

		join_path_components(path, backup_path, PG_RMAN_INI_FILE);
		pgut_readopt(path, options, ERROR_ARGS);
	}

	/* path must be absolute */
	if (backup_path != NULL && !is_absolute_path(backup_path))
		ereport(ERROR,
			(errcode(ERROR_ARGS),
			 errmsg("-B, --backup-path must be an absolute path")));
	if (pgdata != NULL && !is_absolute_path(pgdata))
		ereport(ERROR,
			(errcode(ERROR_ARGS),
			 errmsg("-D, --pgdata must be an absolute path")));
	if (arclog_path != NULL && !is_absolute_path(arclog_path))
		ereport(ERROR,
			(errcode(ERROR_ARGS),
			 errmsg("-A, --arclog-path must be an absolute path")));
	if (srvlog_path != NULL && !is_absolute_path(srvlog_path))
		ereport(ERROR,
			(errcode(ERROR_ARGS),
			 errmsg("-S, --srvlog-path must be an absolute path")));
	if (pgconf_path != NULL && !is_absolute_path(pgconf_path))
		ereport(ERROR,
			(errcode(ERROR_ARGS),
			 errmsg("-G, --pgconf-path must be an absolute path")));

	/* setup exclusion list for file search */
	for (i = 0; pgdata_exclude[i]; i++)		/* find first empty slot */
		;
	if (arclog_path)
		pgdata_exclude[i++] = arclog_path;
	if (srvlog_path)
		pgdata_exclude[i++] = srvlog_path;

	/* do actual operation */
	if (pg_strcasecmp(cmd, "init") == 0)
		return do_init();
	else if (pg_strcasecmp(cmd, "backup") == 0)
	{
		pgBackupOption bkupopt;
		bkupopt.smooth_checkpoint	    = smooth_checkpoint;
		bkupopt.keep_arclog_files	    = keep_arclog_files;
		bkupopt.keep_arclog_days	    = keep_arclog_days;
		bkupopt.keep_srvlog_files	    = keep_srvlog_files;
		bkupopt.keep_srvlog_days	    = keep_srvlog_days;
		bkupopt.keep_data_generations	= keep_data_generations;
		bkupopt.keep_data_days		    = keep_data_days;
		bkupopt.standby_host		    = standby_host;
		bkupopt.standby_port		    = standby_port;
		return do_backup(bkupopt);
	}
	else if (pg_strcasecmp(cmd, "restore") == 0)
		return do_restore(target_time, target_xid,
					target_inclusive, target_tli_string, target_action, is_hard_copy);
	else if (pg_strcasecmp(cmd, "show") == 0)
		return do_show(&range, show_detail, show_all);
	else if (pg_strcasecmp(cmd, "validate") == 0)
		return do_validate(&range);
	else if (pg_strcasecmp(cmd, "delete") == 0)
		return do_delete(&range, force);
	else if (pg_strcasecmp(cmd, "purge") == 0)
		return do_purge();
	else
		ereport(ERROR,
			(errcode(ERROR_ARGS),
			 errmsg("invalid command \"%s\"", cmd)));

	return 0;
}

void
pgut_help(bool details)
{
	printf(_("%s manage backup/recovery of PostgreSQL database.\n\n"), PROGRAM_NAME);
	printf(_("Usage:\n"));
	printf(_("  %s OPTION init\n"), PROGRAM_NAME);
	printf(_("  %s OPTION backup\n"), PROGRAM_NAME);
	printf(_("  %s OPTION restore\n"), PROGRAM_NAME);
	printf(_("  %s OPTION show [DATE]\n"), PROGRAM_NAME);
	printf(_("  %s OPTION show detail [DATE]\n"), PROGRAM_NAME);
	printf(_("  %s OPTION validate [DATE]\n"), PROGRAM_NAME);
	printf(_("  %s OPTION delete DATE\n"), PROGRAM_NAME);
	printf(_("  %s OPTION purge\n"), PROGRAM_NAME);

	if (!details)
		return;

	printf(_("\nCommon Options:\n"));
	printf(_("  -D, --pgdata=PATH         location of the database storage area\n"));
	printf(_("  -A, --arclog-path=PATH    location of archive WAL storage area\n"));
	printf(_("  -S, --srvlog-path=PATH    location of server log storage area\n"));
	printf(_("  -B, --backup-path=PATH    location of the backup storage area\n"));
	printf(_("  -G, --pgconf-path=PATH    location of the configuration storage area\n"));
	printf(_("  -c, --check               show what would have been done\n"));
	printf(_("  -v, --verbose             show what detail messages\n"));
	printf(_("  -P, --progress            show progress of processed files\n"));
	printf(_("\nBackup options:\n"));
	printf(_("  -b, --backup-mode=MODE    full, incremental, or archive\n"));
	printf(_("  -s, --with-serverlog      also backup server log files\n"));
	printf(_("  -Z, --compress-data       compress data backup with zlib\n"));
	printf(_("  -C, --smooth-checkpoint   do smooth checkpoint before backup\n"));
	printf(_("  -F, --full-backup-on-error   switch to full backup mode\n"));
	printf(_("                               if pg_rman cannot find validate full backup\n"));
	printf(_("                               on current timeline\n"));
	printf(_("      NOTE: this option is only used in --backup-mode=incremental or archive.\n"));
	printf(_("  --keep-data-generations=NUM keep NUM generations of full data backup\n"));
	printf(_("  --keep-data-days=NUM        keep enough data backup to recover to N days ago\n"));
	printf(_("  --keep-arclog-files=NUM   keep NUM of archived WAL\n"));
	printf(_("  --keep-arclog-days=DAY    keep archived WAL modified in DAY days\n"));
	printf(_("  --keep-srvlog-files=NUM   keep NUM of serverlogs\n"));
	printf(_("  --keep-srvlog-days=DAY    keep serverlog modified in DAY days\n"));
	printf(_("  --standby-host=HOSTNAME   standby host when taking backup from standby\n"));
	printf(_("  --standby-port=PORT       standby port when taking backup from standby\n"));
	printf(_("\nRestore options:\n"));
	printf(_("  --recovery-target-time    time stamp up to which recovery will proceed\n"));
	printf(_("  --recovery-target-xid     transaction ID up to which recovery will proceed\n"));
	printf(_("  --recovery-target-inclusive whether we stop just after the recovery target\n"));
	printf(_("  --recovery-target-timeline  recovering into a particular timeline\n"));
	printf(_("  --recovery-target-action    action the server should take once the recovery target is reached\n"));
	printf(_("  --hard-copy                 copying archivelog not symbolic link\n"));
	printf(_("\nCatalog options:\n"));
	printf(_("  -a, --show-all            show deleted backup too\n"));
	printf(_("\nDelete options:\n"));
	printf(_("  -f, --force               forcibly delete backup older than given DATE\n"));
}

/*
 * Create range object from one or two arguments.
 * All not-digit characters in the argument(s) are ignored.
 * Both arg1 and arg2 must be valid pointer.
 */
static void
parse_range(pgBackupRange *range, const char *arg1, const char *arg2)
{
	size_t		len = strlen(arg1) + strlen(arg2) + 1;
	char	   *tmp;
	int			num;
	struct tm	tm;

	tmp = pgut_malloc(len);
	tmp[0] = '\0';
	if (arg1 != NULL)
		remove_not_digit(tmp, len, arg1);
	if (arg2 != NULL)
		remove_not_digit(tmp + strlen(tmp), len - strlen(tmp), arg2);

	memset(&tm, 0, sizeof(tm));
	tm.tm_year = 0;		/* tm_year is year - 1900 */
	tm.tm_mon = 0;		/* tm_mon is 0 - 11 */
	tm.tm_mday = 1;		/* tm_mday is 1 - 31 */
	tm.tm_hour = 0;
	tm.tm_min = 0;
	tm.tm_sec = 0;
	num = sscanf(tmp, "%04d %02d %02d %02d %02d %02d",
		&tm.tm_year, &tm.tm_mon, &tm.tm_mday,
		&tm.tm_hour, &tm.tm_min, &tm.tm_sec);

	if (num < 1)
	{
		if (strcmp(tmp,"") != 0)
			ereport(ERROR,
				(errcode(ERROR_ARGS),
				 errmsg("supplied id(%s) is invalid", tmp)));
		else
			ereport(ERROR,
				(errcode(ERROR_ARGS),
				 errmsg("arguments are invalid. near \"%s\"", arg1)));
	}

	free(tmp);

	/* adjust year and month to convert to time_t */
	tm.tm_year -= 1900;
	if (num > 1)
		tm.tm_mon -= 1;
	tm.tm_isdst = -1;

	if(!IsValidTime(tm))
		ereport(ERROR,
			(errcode(ERROR_ARGS),
			 errmsg("supplied time(%s) is invalid", arg1)));

	range->begin = mktime(&tm);

	switch (num)
	{
		case 1:
			tm.tm_year++;
			break;
		case 2:
			tm.tm_mon++;
			break;
		case 3:
			tm.tm_mday++;
			break;
		case 4:
			tm.tm_hour++;
			break;
		case 5:
			tm.tm_min++;
			break;
		case 6:
			tm.tm_sec++;
			break;
	}
	range->end = mktime(&tm);
	range->end--;
}

static void
opt_backup_mode(pgut_option *opt, const char *arg)
{
	current.backup_mode = parse_backup_mode(arg, ERROR);
}
