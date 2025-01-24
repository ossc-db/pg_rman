/*-------------------------------------------------------------------------
 *
 * init.c: manage backup catalog.
 *
 * Copyright (c) 2009-2023, NIPPON TELEGRAPH AND TELEPHONE CORPORATION
 *
 *-------------------------------------------------------------------------
 */

#include "pg_rman.h"

#include "catalog/pg_control.h"
#include "common/controldata_utils.h"

#include <unistd.h>
#include <dirent.h>

static void parse_postgresql_conf(const char *path, char **log_directory,
								  char **archive_command);

/*
 * selects function for scandir.
 */
static int selects(const struct dirent *dir)
{
  return dir->d_name[0] != '.';
}

/*
 * Initialize backup catalog.
 */
int
do_init(void)
{
	char	path[MAXPGPATH];
	char   *log_directory = NULL;
	char   *archive_command = NULL;
	char	controlFilePath[MAXPGPATH];
	FILE   *fp;

	struct dirent **dp;
	int results;
	uint64      sysid = 0;
	ControlFileData *controlFile;
	bool crc_ok;

	if (access(backup_path, F_OK) == 0)
	{
		results = scandir(backup_path, &dp, selects, NULL);

		if(results != 0)
			ereport(ERROR,
				(errcode(ERROR),
				 errmsg("backup catalog already exist and it's not empty")));
	}

	if (pgdata == NULL)
		ereport(ERROR,
			(errcode(ERROR_ARGS),
			 errmsg("required parameter not specified: PGDATA (-D, --pgdata)")));

	/* create backup catalog root directory */
	dir_create_dir(backup_path, DIR_PERMISSION);

	/* create directories for backup of online files */
	join_path_components(path, backup_path, RESTORE_WORK_DIR);
	dir_create_dir(path, DIR_PERMISSION);
	snprintf(path, lengthof(path), "%s/%s/%s", backup_path, RESTORE_WORK_DIR,
		PG_XLOG_DIR);
	dir_create_dir(path, DIR_PERMISSION);
	snprintf(path, lengthof(path), "%s/%s/%s", backup_path, RESTORE_WORK_DIR,
		SRVLOG_DIR);
	dir_create_dir(path, DIR_PERMISSION);

	/* create directory for timeline history files */
	join_path_components(path, backup_path, TIMELINE_HISTORY_DIR);
	dir_create_dir(path, DIR_PERMISSION);

	/* read postgresql.conf */
	if (pgdata)
	{
		join_path_components(path, pgdata, "postgresql.conf");
		parse_postgresql_conf(path, &log_directory, &archive_command);
	}

	/* get system identifier of the current database.*/
	snprintf(controlFilePath, MAXPGPATH, "%s/global/pg_control", pgdata);
	if (fileExists(controlFilePath))
	{
		controlFile = get_controlfile(pgdata, &crc_ok);
		if (!crc_ok)
			ereport(WARNING,
					(errmsg("control file appears to be corrupt"),
					 errdetail("Calculated CRC checksum does not match value stored in file.")));
		sysid = controlFile->system_identifier;
		pg_free(controlFile);
	}
	else
	{
		ereport(ERROR,
				(errmsg("pg_controldata file \"%s\" does not exist", controlFilePath),
				 errhint("Make sure the path to the data cluster directory is correct.")));
	}

	/* register system identifier of target database. */
	join_path_components(path, backup_path, SYSTEM_IDENTIFIER_FILE);
	fp = fopen(path, "wt");
	if (fp == NULL)
		ereport(ERROR,
			(errcode(ERROR_SYSTEM),
			 errmsg("could not create system identifier file: %s", strerror(errno))));
	else
		fprintf(fp, "SYSTEM_IDENTIFIER='" UINT64_FORMAT "'\n", sysid);
	fclose(fp);

	/* create pg_rman.ini */
	join_path_components(path, backup_path, PG_RMAN_INI_FILE);
	fp = fopen(path, "wt");
	if (fp == NULL)
		ereport(ERROR,
			(errcode(ERROR_SYSTEM),
			 errmsg("could not create pg_rman.ini: %s", strerror(errno))));

	/* set ARCLOG_PATH referred with archive_command */
	if (arclog_path == NULL && archive_command && archive_command[0])
	{
		char *command = pgut_strdup(archive_command);
		char *begin;
		char *end;
		char *fname;

		/* example: 'cp "%p" /path/to/arclog/"%f"' */
		for (begin = command; *begin;)
		{
			begin = begin + strspn(begin, " \n\r\t\v");
			end = begin + strcspn(begin, " \n\r\t\v");
			*end = '\0';

			if ((fname = strstr(begin, "%f")) != NULL)
			{
				while (strchr(" \n\r\t\v\"'", *begin))
					begin++;
				fname--;
				while (fname > begin && strchr(" \n\r\t\v\"'/", fname[-1]))
					fname--;
				*fname = '\0';

				if (is_absolute_path(begin))
					arclog_path = pgut_strdup(begin);
				break;
			}

			begin = end + 1;
		}

		free(command);
	}
	if (arclog_path)
	{
		fprintf(fp, "ARCLOG_PATH='%s'\n", arclog_path);
		elog(INFO, "ARCLOG_PATH is set to '%s'", arclog_path);
	}
	else if (archive_command && archive_command[0])
		ereport(WARNING,
			(errmsg("ARCLOG_PATH is not set yet"),
			 errdetail("Pg_rman failed to parse archive_command '%s'.", archive_command),
			 errhint("Please set ARCLOG_PATH in pg_rman.ini or environmental variable.")));
	else
		ereport(WARNING,
			(errmsg("ARCLOG_PATH is not set yet"),
			 errdetail("The archive_command is not set in postgresql.conf."),
			 errhint("Please set ARCLOG_PATH in pg_rman.ini or environmental variable.")));

	/* set SRVLOG_PATH referred with log_directory */
	if (srvlog_path == NULL)
	{
		if (log_directory)
		{
			if (is_absolute_path(log_directory))
				srvlog_path = pgut_strdup(log_directory);
			else
			{
				srvlog_path = pgut_malloc(MAXPGPATH);
				join_path_components(srvlog_path, pgdata, log_directory);
			}
		}
		else if (pgdata)
		{
			/* default: log_directory = 'log' if PostgreSQL version is 10 or above */
			srvlog_path = pgut_malloc(MAXPGPATH);
			join_path_components(srvlog_path, pgdata, "log");
		}
	}
	if (srvlog_path)
	{
		fprintf(fp, "SRVLOG_PATH='%s'\n", srvlog_path);
		elog(INFO, "SRVLOG_PATH is set to '%s'", srvlog_path);
	}

	fprintf(fp, "\n");
	fclose(fp);

	free(archive_command);
	free(log_directory);

	return 0;
}

static void
parse_postgresql_conf(const char *path,
					  char **log_directory,
					  char **archive_command)
{
	pgut_option options[] =
	{
		{ 's', 0, "log_directory"		, NULL, SOURCE_ENV },
		{ 's', 0, "archive_command"		, NULL, SOURCE_ENV },
		{ 0 }
	};

	options[0].var = log_directory;
	options[1].var = archive_command;

	pgut_readopt(path, options, DEBUG);	/* ignore unknown options */
}
