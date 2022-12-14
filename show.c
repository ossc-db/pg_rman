/*-------------------------------------------------------------------------
 *
 * show.c: show backup catalog.
 *
 * Copyright (c) 2009-2022, NIPPON TELEGRAPH AND TELEPHONE CORPORATION
 *
 *-------------------------------------------------------------------------
 */

#include "pg_rman.h"

static void show_backup_list(FILE *out, parray *backup_list, bool show_all);
static void show_detail_backup_list(FILE *out, parray *backup_list, bool show_all);
static void show_backup_detail(FILE *out, pgBackup *backup);

/*
 * Show backup catalog information.
 * If range is { 0, 0 }, show list of all backup, otherwise show detail of the
 * backup indicated by id.
 */
int
do_show(pgBackupRange *range, bool show_detail, bool show_all)
{
	if (pgBackupRangeIsSingle(range))
	{
		pgBackup *backup;

		backup = catalog_get_backup(range->begin);
		if (backup == NULL)
		{
			char timestamp[100];
			time2iso(timestamp, lengthof(timestamp), range->begin);
			elog(NOTICE, _("backup taken at \"%s\" does not exist"),
				timestamp);
			/* This is not error case */
			return 0;
		}
		show_backup_detail(stdout, backup);

		/* cleanup */
		pgBackupFree(backup);
	}
	else
	{
		parray *backup_list;

		backup_list = catalog_get_backup_list(range);
		if (backup_list == NULL)
			ereport(ERROR,
				(errcode(ERROR_SYSTEM),
				 errmsg("could not get list of backup already taken")));

		if (!show_detail)
			show_backup_list(stdout, backup_list, show_all);
		else
			show_detail_backup_list(stdout, backup_list, show_all);

		/* cleanup */
		parray_walk(backup_list, pgBackupFree);
		parray_free(backup_list);
	}

	return 0;
}

static void
pretty_size(int64 size, char *buf, size_t len)
{
	int exp = 0;

	/* minus means the size is invalid */
	if (size < 0)
	{
		strncpy(buf, "----", len);
		return;
	}

	/* determine postfix */
	while (size > 9999)
	{
		++exp;
		size /= 1000;
	}

	switch (exp)
	{
		case 0:
			snprintf(buf, len, INT64_FORMAT "B", size);
			break;
		case 1:
			snprintf(buf, len, INT64_FORMAT "kB", size);
			break;
		case 2:
			snprintf(buf, len, INT64_FORMAT "MB", size);
			break;
		case 3:
			snprintf(buf, len, INT64_FORMAT "GB", size);
			break;
		case 4:
			snprintf(buf, len, INT64_FORMAT "TB", size);
			break;
		case 5:
			snprintf(buf, len, INT64_FORMAT "PB", size);
			break;
		default:
			strncpy(buf, "***", len);
			break;
	}
}

static TimeLineID
get_parent_tli(TimeLineID child_tli)
{
	TimeLineID	result = 0;
	char		path[MAXPGPATH];
	char		fline[MAXPGPATH];
	FILE	   *fd;

	/* search from timeline history dir */
	snprintf(path, lengthof(path), "%s/%s/%08X.history", backup_path,
		TIMELINE_HISTORY_DIR, child_tli);
	fd = fopen(path, "rt");
	if (fd == NULL)
	{
		if (errno != ENOENT)
			ereport(ERROR,
				(errcode(ERROR_SYSTEM),
				 errmsg("could not open file \"%s\": %s", path,
					strerror(errno))));

		return 0;
	}

	/*
	 * Parse the file...
	 */
	while (fgets(fline, sizeof(fline), fd) != NULL)
	{
		/* skip leading whitespace and check for # comment */
		char	   *ptr;
		char	   *endptr;

		for (ptr = fline; *ptr; ptr++)
		{
			if (!IsSpace(*ptr))
				break;
		}
		if (*ptr == '\0' || *ptr == '#')
			continue;

		/* expect a numeric timeline ID as first field of line */
		result = (TimeLineID) strtoul(ptr, &endptr, 0);
		if (endptr == ptr)
			ereport(ERROR,
				(errcode(ERROR_CORRUPTED),
				 errmsg("syntax error(timeline ID) in history file: %s", fline)));
	}

	fclose(fd);

	/* TLI of the last line is parent TLI */
	return result;
}

static void
show_backup_list(FILE *out, parray *backup_list, bool show_all)
{
	int i;

	/* show header */
	fputs("=====================================================================\n", out);
	fputs(" StartTime           EndTime              Mode    Size   TLI  Status \n", out);
	fputs("=====================================================================\n", out);

	for (i = 0; i < parray_num(backup_list); i++)
	{
		pgBackup *backup;
		char start_time[20];
		char end_time[20];
		char write_bytes_str[10];
		static const char *modes[] = { "", "ARCH", "INCR", "FULL"};

		backup = parray_get(backup_list, i);

		/*
		 * Skip deleted backup, unless --show-all has been specified.
		 *
		 * Just ignore invalid backups.
		 */
		if ((backup->status == BACKUP_STATUS_DELETED && !show_all) ||
			backup->status == BACKUP_STATUS_INVALID)
			continue;

		time2iso(start_time, lengthof(start_time), backup->start_time);
		time2iso(end_time, lengthof(end_time), backup->end_time);

		pretty_size(backup->write_bytes, write_bytes_str,
				lengthof(write_bytes_str));

		fprintf(out, "%-19s  %-19s  %-4s  %6s %5d  %s\n",
			start_time, end_time, modes[backup->backup_mode],
			write_bytes_str, backup->tli, status2str(backup->status));
	}
}

static void
show_detail_backup_list(FILE *out, parray *backup_list, bool show_all)
{
	int		i;

	/* show header */
	fputs("======================================================================================================================\n", out);
	fputs(" StartTime           EndTime              Mode    Data  ArcLog  SrvLog   Total  Compressed  CurTLI  ParentTLI  Status \n", out);
	fputs("======================================================================================================================\n", out);

	for (i = 0; i < parray_num(backup_list); i++)
	{
		static const char *modes[] = { "", "ARCH", "INCR", "FULL"};

		pgBackup *backup;
		char start_time[20];
		char end_time[20];
		char read_data_bytes_str[10] = "----";
		char read_arclog_bytes_str[10] = "----";
		char read_srvlog_bytes_str[10] = "----";
		char write_bytes_str[10];
        char *is_compressed;
		TimeLineID	parent_tli;

		backup = parray_get(backup_list, i);

		/* skip deleted backup and serverlog backup */
		if ((backup->status == BACKUP_STATUS_DELETED || !HAVE_ARCLOG(backup)) &&
			!show_all)
			continue;

		time2iso(start_time, lengthof(start_time), backup->start_time);
		time2iso(end_time, lengthof(end_time), backup->end_time);

	    pretty_size(backup->read_data_bytes, read_data_bytes_str,
				lengthof(read_data_bytes_str));

		if (HAVE_ARCLOG(backup))
			pretty_size(backup->read_arclog_bytes, read_arclog_bytes_str,
					lengthof(read_arclog_bytes_str));

		if (backup->with_serverlog)
			pretty_size(backup->read_srvlog_bytes, read_srvlog_bytes_str,
					lengthof(read_srvlog_bytes_str));

		pretty_size(backup->write_bytes, write_bytes_str,
				lengthof(write_bytes_str));

        if (backup->compress_data)
            is_compressed = "true";
        else
            is_compressed = "false";

		parent_tli = get_parent_tli(backup->tli);

		fprintf(out, "%-19s  %-19s  %-4s  %6s  %6s  %6s  %6s       %5s  %6d  %9d  %s\n",
			start_time, end_time, modes[backup->backup_mode],
            read_data_bytes_str,
            read_arclog_bytes_str,
            read_srvlog_bytes_str,
            write_bytes_str,
            is_compressed,
            backup->tli, parent_tli,
			status2str(backup->status));
	}
}

static void
show_backup_detail(FILE *out, pgBackup *backup)
{
	pgBackupWriteConfigSection(out, backup);
	pgBackupWriteResultSection(out, backup);
}
