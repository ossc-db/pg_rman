/*-------------------------------------------------------------------------
 *
 * restore.c: restore DB cluster and archived WAL.
 *
 * Copyright (c) 2009-2022, NIPPON TELEGRAPH AND TELEPHONE CORPORATION
 *
 *-------------------------------------------------------------------------
 */

#include "pg_rman.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "catalog/pg_control.h"
#include "common/controldata_utils.h"
#include "common/fe_memutils.h"

#define POSTGRES_CONF "postgresql.conf"
#define POSTGRES_CONF_TMP "postgresql.conf.pg_rman.tmp"
#define PG_RMAN_RECOVERY_CONF "pg_rman_recovery.conf"
#define PG_RMAN_COMMENT "# added by pg_rman"

static void backup_online_files(bool re_recovery);
static void restore_online_files(void);
static void restore_database(pgBackup *backup);
static void restore_archive_logs(pgBackup *backup, bool is_hard_copy);

static void append_include_directive_for_pg_rman(void);
static void include_recovery_configuration(void);
static void configure_recovery_options(const char *target_time,
										 const char *target_xid,
										 const char *target_inclusive,
										 const char *target_action,
										 TimeLineID target_tli,
										 bool target_tli_latest);
static void create_recovery_configuration_file(const char *target_time,
										 const char *target_xid,
										 const char *target_inclusive,
										 const char *target_action,
										 TimeLineID target_tli,
										 bool target_tli_latest);
static void create_recovery_signal(void);
static void remove_include_directive_for_pg_rman(void);
static void remove_standby_signal(void);

static pgRecoveryTarget *checkIfCreateRecoveryConf(const char *target_time,
								 const char *target_xid,
								 const char *target_inclusive,
								 const char *target_action);
static parray * readTimeLineHistory(TimeLineID targetTLI);
static bool satisfy_timeline(const parray *timelines, const pgBackup *backup);
static bool satisfy_recovery_target(const pgBackup *backup, const pgRecoveryTarget *rt);
static TimeLineID get_fullbackup_timeline(parray *backups, const pgRecoveryTarget *rt);
static TimeLineID parse_target_timeline(const char *target_tli_string, TimeLineID cur_tli,
											bool *target_tli_latest);
static TimeLineID findNewestTimeLine(TimeLineID startTLI);
static bool existsTimeLineHistory(TimeLineID probeTLI);
static void print_backup_id(const pgBackup *backup);
static void search_next_wal(const char *path, uint32 *needId, uint32 *needSeg, parray *timelines);

static int wal_segment_size = 0;

int
do_restore(const char *target_time,
		   const char *target_xid,
		   const char *target_inclusive,
		   const char *target_tli_string,
		   const char *target_action,
		   bool is_hard_copy)
{
	int i;
	int base_index;				/* index of base (full) backup */
	int last_restored_index;	/* index of last restored database backup */
	int ret;
	TimeLineID	target_tli;
	bool		target_tli_latest = false;
	TimeLineID	cur_tli;
	TimeLineID	backup_tli;
	parray *backups;
	pgBackup *base_backup = NULL;
	parray *files;
	parray *timelines;
	char timeline_dir[MAXPGPATH];
	char timestamp[20];
	uint32 needId = 0;
	uint32 needSeg = 0;
	pgRecoveryTarget *rt = NULL;

	ControlFileData		*controlFile;
	bool		crc_ok;
	char		ControlFilePath[MAXPGPATH];

	/* PGDATA and ARCLOG_PATH are always required */
	if (pgdata == NULL)
		ereport(ERROR,
			(errcode(ERROR_ARGS),
			 errmsg("required parameter not specified: PGDATA (-D, --pgdata)")));
	if (arclog_path == NULL)
		ereport(ERROR,
			(errcode(ERROR_ARGS),
			 errmsg("required parameter not specified: ARCLOG_PATH (-A, --arclog-path)")));
	if (srvlog_path == NULL)
		ereport(ERROR,
			(errcode(ERROR_ARGS),
			 errmsg("required parameter not specified: SRVLOG_PATH (-S, --srvlog-path)")));

	/* update pgconf_path if user didn't specify */
	if (pgconf_path == NULL)
		pgconf_path = pgdata;

	if (verbose)
	{
		printf(_("========================================\n"));
		printf(_("restore start\n"));
	}

	/* get exclusive lock of backup catalog */
	ret = catalog_lock();
	if (ret == -1)
		ereport(ERROR,
			(errcode(ERROR_SYSTEM),
			 errmsg("could not lock backup catalog")));
	else if (ret == 1)
		ereport(ERROR,
			(errcode(ERROR_ALREADY_RUNNING),
			 errmsg("could not lock backup catalog"),
			 errdetail("Another pg_rman is just running.")));

	/* confirm the PostgreSQL server is not running */
	if (is_pg_running())
		ereport(ERROR,
			(errcode(ERROR_PG_RUNNING),
			 errmsg("PostgreSQL server is running"),
			 errhint("Please stop PostgreSQL server before executing restore.")));

	rt = checkIfCreateRecoveryConf(target_time, target_xid, target_inclusive, target_action);
	if(rt == NULL)
		ereport(ERROR,
			(errcode(ERROR_ARGS),
			 errmsg("could not create recovery.conf or "
					"add recovery-related options to postgresql.conf(after PG12)"),
			 errdetail("The specified options are invalid.")));

	/* get list of backups. (index == 0) is the last backup */
	backups = catalog_get_backup_list(NULL);
	if(!backups)
		ereport(ERROR,
			(errcode(ERROR_SYSTEM),
			 errmsg("could not get list of backup already taken")));

	/* get wal_segment_size from pg_control file, it is needed for check option. */
	if (check)
	{
		snprintf(ControlFilePath, MAXPGPATH, "%s/global/pg_control", pgdata);
		if (fileExists(ControlFilePath))
		{
			controlFile = get_controlfile(pgdata, &crc_ok);
			if (!crc_ok)
				ereport(ERROR,
						(errmsg("control file appears to be corrupt"),
						 errdetail("Calculated CRC checksum does not match value stored in file.")));
			wal_segment_size = controlFile->xlog_seg_size;
			pg_free(controlFile);
		}
		else
		{
			elog(ERROR, _("pg_controldata file \"%s\" does not exist"),
				 ControlFilePath);
		}
	}

	cur_tli = get_current_timeline();
	elog(DEBUG, "the current timeline ID of database cluster is %d", cur_tli);

	backup_tli = get_fullbackup_timeline(backups, rt);
	elog(DEBUG, "the timeline ID of latest full backup is %d", backup_tli);

	/*
	 * determine target timeline;
	 * first parse the user specified string value for the target timeline
	 * passed in via --recovery-target-timeline option. Need this because
	 * the value 'latest' is also supported.
	 */
	if(target_tli_string)
	{
		target_tli = parse_target_timeline(target_tli_string, cur_tli, &target_tli_latest);
		elog(INFO, "the specified target timeline ID is %d", target_tli);
	}
	else
	{
		elog(INFO, "the recovery target timeline ID is not given");

		if (cur_tli != 0)
		{
			target_tli = cur_tli;
			elog(INFO, "use timeline ID of current database cluster as recovery target: %d",
				cur_tli);
		} 
		else
		{
			backup_tli = get_fullbackup_timeline(backups, rt);
			target_tli = backup_tli;
			elog(INFO, "use timeline ID of latest full backup as recovery target: %d",
				backup_tli);
		}
	}


	/*
	 * restore timeline history files and get timeline branches can reach
	 * recovery target point.
	 */
	elog(INFO, "calculating timeline branches to be used to recovery target point");
	join_path_components(timeline_dir, backup_path, TIMELINE_HISTORY_DIR);
	dir_copy_files(timeline_dir, arclog_path);
	timelines = readTimeLineHistory(target_tli);

	/* find last full backup which can be used as base backup. */
	elog(INFO, "searching latest full backup which can be used as restore start point");
	for (i = 0; i < parray_num(backups); i++)
	{
		base_backup = (pgBackup *) parray_get(backups, i);

		if (base_backup->backup_mode < BACKUP_MODE_FULL ||
			base_backup->status != BACKUP_STATUS_OK)
			continue;

#ifndef HAVE_LIBZ
		/* Make sure we won't need decompression we haven't got */
		if (base_backup->compress_data &&
			(HAVE_DATABASE(base_backup) || HAVE_ARCLOG(base_backup)))
		{
			ereport(ERROR,
				(errcode(ERROR_SYSTEM),
				 errmsg("could not restore from compressed backup"),
				 errdetail("Compression is not supported in this installation.")));
		}
#endif
		if (satisfy_timeline(timelines, base_backup) && satisfy_recovery_target(base_backup, rt))
		{
			time2iso(timestamp, lengthof(timestamp), base_backup->start_time);
			elog(INFO, "found the full backup can be used as base in recovery: \"%s\"",
				timestamp);
			goto base_backup_found;
		}
	}
	/* no full backup found, can't restore */
	ereport(ERROR,
		(errcode(ERROR_NO_BACKUP),
		 errmsg("cannot do restore"),
		 errdetail("There is no valid full backup which can be used for given recovery condition.")));

base_backup_found:

	/* first off, backup online WAL and serverlog */
	backup_online_files(cur_tli != 0 && cur_tli != backup_tli);

	/*
	 * Clear restore destination, but don't remove $PGDATA.
	 * To remove symbolic link, get file list with "omit_symlink = false".
	 *
	 * Doing it *after* a good base backup is found so that we don't end up
	 * in a situation where the target data directory is already deleted
	 * but we could not find a valid base backup based on user specified
	 * restore options (perhaps a mistake on user's part but we should be
	 * cautious.)
	 */
	if (!check)
	{
		int x;

		if (verbose)
			printf(_("----------------------------------------\n"));

		elog(INFO, "clearing restore destination");
		files = parray_new();
		dir_list_file(files, pgdata, NULL, false, false);
		parray_qsort(files, pgFileComparePathDesc);	/* delete from leaf */

		for (x = 0; x < parray_num(files); x++)
		{
			pgFile *file = (pgFile *) parray_get(files, i);
			pgFileDelete(file);
		}
		parray_walk(files, pgFileFree);
		parray_free(files);
	}

	/* OK, now proceed to restoring the backup */
	base_index = i;

	if (verbose)
		print_backup_id(base_backup);

	/* restore base backup */
	restore_database(base_backup);

	last_restored_index = base_index;

	/* restore following incremental backup */
	if (verbose)
		printf(_("----------------------------------------\n"));
	elog(INFO, "searching incremental backup to be restored");
	for (i = base_index - 1; i >= 0; i--)
	{
		pgBackup *backup = (pgBackup *) parray_get(backups, i);

		/* don't use incomplete nor different timeline backup */
		if (backup->status != BACKUP_STATUS_OK ||
					backup->tli != base_backup->tli)
			continue;

		/* use database backup only */
		if (backup->backup_mode != BACKUP_MODE_INCREMENTAL)
			continue;

		/* is the backup is necessary for restore to target timeline ? */
		if (!satisfy_timeline(timelines, backup) || !satisfy_recovery_target(backup, rt))
			continue;

		if (verbose)
			print_backup_id(backup);

		time2iso(timestamp, lengthof(timestamp), backup->start_time);
		elog(DEBUG, "found the incremental backup can be used in recovery: \"%s\"",
			timestamp);

		restore_database(backup);
		last_restored_index = i;
	}

	/*
	 * Restore archived WAL which backed up with or after last restored backup.
	 * We don't check the backup->tli because a backup of archived WAL
	 * can contain WALs which were archived in multiple timeline.
	 */
	if (check)
	{
		pgBackup *backup = (pgBackup *) parray_get(backups, last_restored_index);
		needId = (uint32) (backup->start_lsn >> 32);
		needSeg = (uint32) backup->start_lsn / wal_segment_size;
	}

	if (verbose)
		printf(_("----------------------------------------\n"));
	elog(INFO, "searching backup which contained archived WAL files to be restored");
	for (i = last_restored_index; i >= 0; i--)
	{
		pgBackup *backup = (pgBackup *) parray_get(backups, i);

		/* don't use incomplete backup */
		if (backup->status != BACKUP_STATUS_OK)
			continue;

		if (!HAVE_ARCLOG(backup))
			continue;

		/* care timeline junction */
		if (!satisfy_timeline(timelines, backup))
			continue;

		restore_archive_logs(backup, is_hard_copy);

		if (check)
		{
			char	xlogpath[MAXPGPATH];

			pgBackupGetPath(backup, xlogpath, lengthof(xlogpath), ARCLOG_DIR);
			search_next_wal(xlogpath, &needId, &needSeg, timelines);
		}
	}

	/* copy online WAL backup to $PGDATA/pg_wal */
	restore_online_files();

	if (check)
	{
		char	xlogpath[MAXPGPATH];
		if (verbose)
			printf(_("searching archived WAL\n"));

		search_next_wal(arclog_path, &needId, &needSeg, timelines);

		if (verbose)
			printf(_("searching online WAL\n"));

		join_path_components(xlogpath, pgdata, PG_XLOG_DIR);
		search_next_wal(xlogpath, &needId, &needSeg, timelines);

		if (verbose)
			printf(_("all necessary files are found.\n"));
	}

	/* configure recovery-related options */
	configure_recovery_options(target_time, target_xid, target_inclusive,
								 target_action, target_tli, target_tli_latest);

	/* release catalog lock */
	catalog_unlock();

	/* cleanup */
	parray_walk(backups, pgBackupFree);
	parray_free(backups);

	/* print restore complete message */
	if (verbose && !check)
	{
		printf(_("all restore completed\n"));
		printf(_("========================================\n"));
	}
	if (!check)
		ereport(INFO,
			(errmsg("restore complete"),
			 errhint("Recovery will start automatically when the PostgreSQL server is started. After the recovery is done, we recommend to remove recovery-related parameters configured by pg_rman.")));

	return 0;
}	

/*
 * Validate and restore backup.
 */
void
restore_database(pgBackup *backup)
{
	char	timestamp[100];
	char	path[MAXPGPATH];
	char	list_path[MAXPGPATH];
	int		ret;
	parray *files;
	int		i;
	int		num_skipped = 0;

	/* confirm block size compatibility */
	if (backup->block_size != BLCKSZ)
		ereport(ERROR,
			(errcode(ERROR_PG_INCOMPATIBLE),
			 errmsg("BLCKSZ(%d) is not compatible (%d expected)",
				backup->block_size, BLCKSZ)));
	if (backup->wal_block_size != XLOG_BLCKSZ)
		ereport(ERROR,
			(errcode(ERROR_PG_INCOMPATIBLE),
			 errmsg("XLOG_BLCKSZ(%d) is not compatible (%d expected)",
				backup->wal_block_size, XLOG_BLCKSZ)));

	time2iso(timestamp, lengthof(timestamp), backup->start_time);
	if (verbose && !check)
	{
		printf(_("----------------------------------------\n"));
	}

	/*
	 * Validate backup files with its size, because load of CRC calculation is
	 * not light.
	 */
	pgBackupValidate(backup, true, false, true);

	if (backup->backup_mode == BACKUP_MODE_FULL)
		elog(INFO, "restoring database files from the full mode backup \"%s\"",
			timestamp);
	else if (backup->backup_mode == BACKUP_MODE_INCREMENTAL)
		elog(INFO, "restoring database files from the incremental mode backup \"%s\"",
			timestamp);

	/* make directories and symbolic links */
	pgBackupGetPath(backup, path, lengthof(path), MKDIRS_SH_FILE);
	if (!check)
	{
		char pwd[MAXPGPATH];

		/* keep original directory */
		if (getcwd(pwd, sizeof(pwd)) == NULL)
			ereport(ERROR,
				(errcode(ERROR_SYSTEM),
				 errmsg("could not get current working directory: %s", strerror(errno))));

		/* create pgdata directory */
		dir_create_dir(pgdata, DIR_PERMISSION);

		/* change directory to pgdata */
		if (chdir(pgdata))
			ereport(ERROR,
				(errcode(ERROR_SYSTEM),
				 errmsg("could not change directory: %s", strerror(errno))));

		/* Execute mkdirs.sh */
		ret = system(path);
		if (ret != 0)
			ereport(ERROR,
				(errcode(ERROR_SYSTEM),
				 errmsg("could not execute mkdirs.sh: %s", strerror(errno))));

		/* go back to original directory */
		if (chdir(pwd))
			ereport(ERROR,
				(errcode(ERROR_SYSTEM),
				 errmsg("could not change directory: %s", strerror(errno))));
	}

	/*
	 * get list of files which need to be restored.
	 */
	pgBackupGetPath(backup, path, lengthof(path), DATABASE_DIR);
	pgBackupGetPath(backup, list_path, lengthof(list_path), DATABASE_FILE_LIST);
	files = dir_read_file_list(path, list_path);
	for (i = parray_num(files) - 1; i >= 0; i--)
	{
		pgFile *file = (pgFile *) parray_get(files, i);

		/* remove files which are not backed up */
		if (file->write_size == BYTES_INVALID)
			pgFileFree(parray_remove(files, i));
	}

	/* restore files into $PGDATA */
	for (i = 0; i < parray_num(files); i++)
	{
		char from_root[MAXPGPATH];
		pgFile *file = (pgFile *) parray_get(files, i);

		pgBackupGetPath(backup, from_root, lengthof(from_root), DATABASE_DIR);

		/* check for interrupt */
		if (interrupted)
			ereport(FATAL,
				(errcode(ERROR_INTERRUPTED),
				 errmsg("interrupted during restore database")));

		/* print progress in verbose mode */
		if (verbose && !check)
			printf(_("(%d/%lu) %s "), i + 1, (unsigned long) parray_num(files),
				file->path + strlen(from_root) + 1);

		/* directories are created with mkdirs.sh */
		if (S_ISDIR(file->mode))
		{
			num_skipped++;
			if (verbose && !check)
				printf(_("directory, skip\n"));
			goto show_progress;
		}

		/* not backed up */
		if (file->write_size == BYTES_INVALID)
		{
			num_skipped++;
			if (verbose && !check)
				printf(_("not backed up, skip\n"));
			goto show_progress;
		}

		/* restore file */
		if (!check)
			restore_data_file(from_root, pgdata, file, backup->compress_data);

		/* print size of restored file */
		if (verbose && !check)
		{
			printf(_("restored %lu\n"), (unsigned long) file->write_size);
			continue;
		}

show_progress:
		/* print progress in non-verbose format */
		if (progress)
		{
			fprintf(stderr, _("Processed %d of %lu files, skipped %d"),
					i + 1, (unsigned long) parray_num(files), num_skipped);
			if(i + 1 < (unsigned long) parray_num(files))
				fprintf(stderr, "\r");
			else
				fprintf(stderr, "\n");
		}
	}

	/* Delete files which are not in file list. */
	if (!check)
	{
		parray *files_now;

		parray_walk(files, pgFileFree);
		parray_free(files);

		/* re-read file list to change base path to $PGDATA */
		files = dir_read_file_list(pgdata, list_path);
		parray_qsort(files, pgFileComparePathDesc);

		/* get list of files restored to pgdata */
		files_now = parray_new();
		dir_list_file(files_now, pgdata, pgdata_exclude, true, false);
		/* to delete from leaf, sort in reversed order */
		parray_qsort(files_now, pgFileComparePathDesc);

		for (i = 0; i < parray_num(files_now); i++)
		{
			pgFile *file = (pgFile *) parray_get(files_now, i);

			/* If the file is not in the file list, delete it */
			if (parray_bsearch(files, file, pgFileComparePathDesc) == NULL)
			{
				if (verbose)
					printf(_("  delete %s\n"), file->path + strlen(pgdata) + 1);
				pgFileDelete(file);
			}
		}

		parray_walk(files_now, pgFileFree);
		parray_free(files_now);
	}

	/* remove postmaster.pid */
	snprintf(path, lengthof(path), "%s/postmaster.pid", pgdata);
	if (remove(path) == -1 && errno != ENOENT)
		ereport(ERROR,
			(errcode(ERROR_SYSTEM),
			 errmsg("could not remove postmaster.pid: %s", strerror(errno))));

	/* cleanup */
	parray_walk(files, pgFileFree);
	parray_free(files);

	if (verbose && !check)
		printf(_("restore backup completed\n"));
}

/*
 * Restore archived WAL by creating symbolic link which linked to backup WAL in
 * archive directory.
 */
void
restore_archive_logs(pgBackup *backup, bool is_hard_copy)
{
	int i;
	int num_skipped = 0;
	char timestamp[100];
	parray *files;
	char path[MAXPGPATH];
	char list_path[MAXPGPATH];
	char base_path[MAXPGPATH];

	time2iso(timestamp, lengthof(timestamp), backup->start_time);
	if (verbose && !check)
	{
		printf(_("----------------------------------------\n"));
	}

	/*
	 * Validate backup files with its size, because load of CRC calculation is
	 * not light.
	 */
	pgBackupValidate(backup, true, false, false);

	elog(INFO,_("restoring WAL files from backup \"%s\""), timestamp);
	pgBackupGetPath(backup, list_path, lengthof(list_path), ARCLOG_FILE_LIST);
	pgBackupGetPath(backup, base_path, lengthof(list_path), ARCLOG_DIR);
	files = dir_read_file_list(base_path, list_path);
	for (i = 0; i < parray_num(files); i++)
	{
		pgFile *file = (pgFile *) parray_get(files, i);

		/* check for interrupt */
		if (interrupted)
			ereport(FATAL,
				(errcode(ERROR_INTERRUPTED),
				 errmsg("interrupted during restore WAL")));

		/* print progress */
		join_path_components(path, arclog_path, file->path + strlen(base_path) + 1);
		if (verbose && !check)
			printf(_("(%d/%lu) %s "), i + 1, (unsigned long) parray_num(files),
				file->path + strlen(base_path) + 1);

		/* skip files which are not in backup */
		if (file->write_size == BYTES_INVALID)
		{
			if (verbose && !check)
				printf(_("skip(not backed up)\n"));
			goto show_progress;
		}

		/*
		 * skip timeline history files because timeline history files will be
		 * restored from $BACKUP_PATH/timeline_history.
		 */
		if (strstr(file->path, ".history") ==
				file->path + strlen(file->path) - strlen(".history"))
		{
			if (verbose && !check)
				printf(_("skip(timeline history)\n"));
			goto show_progress;
		}

		if (!check)
		{
			if (backup->compress_data)
			{
				copy_file(base_path, arclog_path, file, DECOMPRESSION);
				if (verbose)
					printf(_("decompressed\n"));

				goto show_progress;
			}

			/* even same file exist, use backup file */
			if ((remove(path) == -1) && errno != ENOENT)
				ereport(ERROR,
					(errcode(ERROR_SYSTEM),
					 errmsg("could not remove file \"%s\": %s", path, strerror(errno))));

			if (!is_hard_copy)
			{
				/* create symlink */
				if ((symlink(file->path, path) == -1))
					ereport(ERROR,
						(errcode(ERROR_SYSTEM),
						 errmsg("could not create link to \"%s\": %s",
							file->path, strerror(errno))));

				if (verbose)
					printf(_("linked\n"));
			}
			else
			{
				/* create hard-copy */
				if (!copy_file(base_path, arclog_path, file, NO_COMPRESSION))
					ereport(ERROR,
						(errcode(ERROR_SYSTEM),
						 errmsg("could not copy to \"%s\": %s",
							file->path, strerror(errno))));

				if (verbose)
					printf(_("copied\n"));
			}

show_progress:
		/* print progress in non-verbose format */
		if (progress)
		{
			fprintf(stderr, _("Processed %d of %lu files, skipped %d"),
					i + 1, (unsigned long) parray_num(files), num_skipped);
			if(i + 1 < (unsigned long) parray_num(files))
				fprintf(stderr, "\r");
			else
				fprintf(stderr, "\n");
		}

		}
	}

	parray_walk(files, pgFileFree);
	parray_free(files);
}

static void
configure_recovery_options(const char *target_time,
										 const char *target_xid,
										 const char *target_inclusive,
										 const char *target_action,
										 TimeLineID target_tli,
										 bool target_tli_latest)
{
	/*
	 * Check if postgresql.conf exists in the restored data directory
	 * because a user manages postgresql's configuration files in a
	 * directory different from the data directory using the GUC
	 * `data_directory` parameter. If so, recovery-related parameters
	 * will not work so that user must manage them manually.
	 */
	char path[MAXPGPATH];
	snprintf(path, lengthof(path), "%s/%s", pgconf_path, POSTGRES_CONF);
	if (!fileExists(path))
	{
		elog(WARNING,
			"recovery-related configuration is skipped because postgresql.conf doesn't exist in %s",
			pgconf_path);
		return;
	}

	/*
	 * Configure recovery-related parameters.
	 *
	 * 1. Create the file for recovery-related parameters
	 *
	 * 2. Append an 'include' directive to include the file.
	 * If the 'include' directive configured by pg_rman exists,
	 * remove it first.  The reason why to use an 'include' directive is to
	 * make it easy for users to distinguish it.
	 *
	 * Note: It keeps the user's configuration. There are two reasons.
	 * The first is to avoid making a user puzzled.  The second is that
	 * there is no problem because pg_rman appends the 'include' directive
	 * at the last of postgresql.conf every time so that the pg_rman's
	 * configurations work as valid values.
	 */
	create_recovery_configuration_file(target_time, target_xid, target_inclusive,
								 target_action, target_tli, target_tli_latest);
	include_recovery_configuration();

	/* Create recovery.signal file */
	create_recovery_signal();

	/*
	 * Remove if standby.signal file exists because pg_rman doesn’t treat
	 * the backup as restoring on standby automatically now.
	 */
	remove_standby_signal();
}

static void
remove_include_directive_for_pg_rman()
{
	char path[MAXPGPATH];
	char tmppath[MAXPGPATH];
	char fline[MAXPGPATH];
	FILE *r_fd, *w_fd;

	if (verbose && !check)
	{
		printf(_("----------------------------------------\n"));
	}

	snprintf(path, lengthof(path), "%s/%s", pgconf_path, POSTGRES_CONF);
	snprintf(tmppath, lengthof(path), "%s/%s", pgconf_path, POSTGRES_CONF_TMP);

	elog(INFO, "remove an 'include' directive added by pg_rman in %s if exists", POSTGRES_CONF);

	if (!check)
	{
		elog(DEBUG, "make temporary file \"%s\"", tmppath);

		if ((r_fd = fopen(path, "rt")) == NULL)
			ereport(ERROR,
				(errcode(ERROR_SYSTEM),
				 errmsg("could not open file \"%s\": %s", path, strerror(errno))));

		if ((w_fd = fopen(tmppath, "w")) == NULL)
			ereport(ERROR,
				(errcode(ERROR_SYSTEM),
				 errmsg("could not open file \"%s\": %s", tmppath, strerror(errno))));

		while (r_fd && fgets(fline, sizeof(fline), r_fd) != NULL)
		{
			elog(DEBUG, "%s", fline);

			/* skip the lines which pg_rman configured */
			if (strstr(fline, "include") && strstr(fline, PG_RMAN_RECOVERY_CONF))
				continue;

			fprintf(w_fd, "%s", fline);
		}

		fclose(r_fd);
		fclose(w_fd);

		elog(DEBUG, "overwrite file \"%s\" with \"%s\"", path, tmppath);
		if (rename(tmppath, path) != 0)
			ereport(ERROR,
				(errcode(ERROR_SYSTEM),
				 errmsg("could not overwrite file \"%s\" with \"%s\": %s",
						path, tmppath, strerror(errno))));
	}
}

static void
create_recovery_configuration_file(const char *target_time,
							 const char *target_xid,
							 const char *target_inclusive,
							 const char *target_action,
							 TimeLineID target_tli,
							 bool target_tli_latest)
{
	char path[MAXPGPATH];
	FILE *fp;

	if (verbose && !check)
	{
		printf(_("----------------------------------------\n"));
	}

	snprintf(path, lengthof(path), "%s/%s", pgconf_path, PG_RMAN_RECOVERY_CONF);
	elog(INFO, "create %s for recovery-related parameters.", PG_RMAN_RECOVERY_CONF);

	if (!check)
	{
		/* overwrite if exists */
		if ((fp = fopen(path, "w")) == NULL)
			ereport(ERROR,
					(ERROR_SYSTEM,
					 errmsg("could not create file \"%s\": %s", path, strerror(errno))));

		fprintf(fp, "%s %s\n", PG_RMAN_COMMENT, PROGRAM_VERSION);
		fprintf(fp, "restore_command = 'cp %s/%%f %%p'\n", arclog_path);
		if (target_time)
			fprintf(fp, "recovery_target_time = '%s'\n", target_time);
		if (target_xid)
			fprintf(fp, "recovery_target_xid = '%s'\n", target_xid);
		if (target_inclusive)
			fprintf(fp, "recovery_target_inclusive = '%s'\n", target_inclusive);
		if(target_tli_latest)
			fprintf(fp, "recovery_target_timeline = 'latest'\n");
		else
			fprintf(fp, "recovery_target_timeline = '%u'\n", target_tli);
		if (target_action)
			fprintf(fp, "recovery_target_action = '%s'\n", target_action);

		fclose(fp);
	}
}


static void
append_include_directive_for_pg_rman()
{
	char path[MAXPGPATH];
	FILE *fp;

	if (verbose && !check)
	{
		printf(_("----------------------------------------\n"));
	}

	snprintf(path, lengthof(path), "%s/%s", pgconf_path, POSTGRES_CONF);
	elog(INFO, "append an 'include' directive in %s for %s", POSTGRES_CONF, PG_RMAN_RECOVERY_CONF);

	if (!check)
	{
		fp = fopen(path, "a");
		if (fp == NULL)
			ereport(ERROR,
				(errcode(ERROR_SYSTEM),
				 errmsg("could not open \"%s\": %s", path, strerror(errno))));

		fprintf(fp, "include = '%s' %s %s\n", PG_RMAN_RECOVERY_CONF, PG_RMAN_COMMENT, PROGRAM_VERSION);

		fclose(fp);
	}
}

static void
include_recovery_configuration(void)
{
	remove_include_directive_for_pg_rman();
	append_include_directive_for_pg_rman();
}

static void
create_recovery_signal(void)
{
	char path[MAXPGPATH];
	FILE *fp;

	if (verbose && !check)
		printf(_("----------------------------------------\n"));

	elog(INFO, _("generating recovery.signal"));

	if (!check)
	{
		snprintf(path, lengthof(path), "%s/recovery.signal", pgdata);
		fp = fopen(path, "wt");
		fprintf(fp, "# recovery.signal generated by pg_rman %s\n",
				PROGRAM_VERSION);
		fclose(fp);
	}
}

static void
remove_standby_signal(void)
{
	char path[MAXPGPATH];

	if (verbose && !check)
		printf(_("----------------------------------------\n"));

	elog(INFO, _("removing standby.signal if exists to restore as primary"));

	if (!check)
	{
		if (get_standby_signal_filepath(path, sizeof(path)))
		{
			if (remove(path))
			{
				ereport(ERROR,
					(errcode(ERROR_SYSTEM),
					 errmsg("could not remove \"%s\": %s", path,
						strerror(errno))));
			}
			ereport(INFO,
				(errmsg("removed standby.signal"),
				 errhint("if you want to start as standby, additional manual "
						"setups to make standby.signal and so on are required")));
		}
	}
}

static void
backup_online_files(bool re_recovery)
{
	char work_path[MAXPGPATH];
	char pg_wal_path[MAXPGPATH];
	bool files_exist;
	parray *files;

	if (verbose && !check)
		printf(_("----------------------------------------\n"));
	
	elog(INFO, _("copying online WAL files and server log files"));

	/* get list of files in $BACKUP_PATH/backup/pg_wal */
	files = parray_new();
	snprintf(work_path, lengthof(work_path), "%s/%s/%s", backup_path,
		RESTORE_WORK_DIR, PG_XLOG_DIR);
	dir_list_file(files, work_path, NULL, true, false);

	files_exist = parray_num(files) > 0;

	parray_walk(files, pgFileFree);
	parray_free(files);

	/* If files exist in RESTORE_WORK_DIR and not re-recovery, use them. */
	if (files_exist && !re_recovery)
	{
		if (verbose)
			printf(_("online WALs have been already backed up, use them.\n"));

		return;
	}

	/* backup online WAL */
	snprintf(pg_wal_path, lengthof(pg_wal_path), "%s/pg_wal", pgdata);
	snprintf(work_path, lengthof(work_path), "%s/%s/%s", backup_path,
		RESTORE_WORK_DIR, PG_XLOG_DIR);
	dir_create_dir(work_path, DIR_PERMISSION);
	dir_copy_files(pg_wal_path, work_path);

	/* backup serverlog */
	snprintf(work_path, lengthof(work_path), "%s/%s/%s", backup_path,
		RESTORE_WORK_DIR, SRVLOG_DIR);
	dir_create_dir(work_path, DIR_PERMISSION);
	dir_copy_files(srvlog_path, work_path);
}

static void
restore_online_files(void)
{
	int		i;
	int		num_skipped = 0;
	char	root_backup[MAXPGPATH];
	parray *files_backup;

	/* get list of files in $BACKUP_PATH/backup/pg_wal */
	files_backup = parray_new();
	snprintf(root_backup, lengthof(root_backup), "%s/%s/%s", backup_path,
		RESTORE_WORK_DIR, PG_XLOG_DIR);
	dir_list_file(files_backup, root_backup, NULL, true, false);

	if (verbose && !check)
	{
		printf(_("----------------------------------------\n"));
	}

	elog(INFO, _("restoring online WAL files and server log files"));

	/* restore online WAL */
	for (i = 0; i < parray_num(files_backup); i++)
	{
		pgFile *file = (pgFile *) parray_get(files_backup, i);

		if (S_ISDIR(file->mode))
		{
			char to_path[MAXPGPATH];
			snprintf(to_path, lengthof(to_path), "%s/%s/%s", pgdata,
				PG_XLOG_DIR, file->path + strlen(root_backup) + 1);
			if (verbose && !check)
				printf(_("create directory \"%s\"\n"),
					file->path + strlen(root_backup) + 1);
			if (!check)
				dir_create_dir(to_path, DIR_PERMISSION);
			goto show_progress;
		}
		else if(S_ISREG(file->mode))
		{
			char to_root[MAXPGPATH];
			join_path_components(to_root, pgdata, PG_XLOG_DIR);
			if (verbose && !check)
				printf(_("restore \"%s\"\n"),
					file->path + strlen(root_backup) + 1);
			if (!check)
				copy_file(root_backup, to_root, file, NO_COMPRESSION);
		}

show_progress:
		/* print progress in non-verbose format */
		if (progress)
		{
			fprintf(stderr, _("Processed %d of %lu files, skipped %d"),
					i + 1, (unsigned long) parray_num(files_backup), num_skipped);
			if(i + 1 < (unsigned long) parray_num(files_backup))
				fprintf(stderr, "\r");
			else
				fprintf(stderr, "\n");
		}
	}

	/* cleanup */
	parray_walk(files_backup, pgFileFree);
	parray_free(files_backup);
}

/*
 * Try to read a timeline's history file.
 *
 * If successful, return the list of component pgTimeLine (the ancestor
 * timelines followed by target timeline). If we can't find the history file,
 * assume that the timeline has no parents, and return a list of just the
 * specified timeline ID.
 * based on readTimeLineHistory() in xlog.c
 */
static parray *
readTimeLineHistory(TimeLineID targetTLI)
{
	parray	   *result;
	char		path[MAXPGPATH];
	char		fline[MAXPGPATH];
	FILE	   *fd;
	pgTimeLine *timeline;
	pgTimeLine *last_timeline = NULL;
	int			i;

	result = parray_new();

	/* search from arclog_path first */
	snprintf(path, lengthof(path), "%s/%08X.history", arclog_path,
		targetTLI);
	fd = fopen(path, "rt");
	if (fd == NULL)
	{
		if (errno != ENOENT)
			ereport(ERROR,
				(errcode(ERROR_SYSTEM),
				 errmsg("could not open file \"%s\": %s", path,
					strerror(errno))));

		/* search from restore work directory next */
		snprintf(path, lengthof(path), "%s/%s/%s/%08X.history", backup_path,
			RESTORE_WORK_DIR, PG_XLOG_DIR, targetTLI);
		fd = fopen(path, "rt");
		if (fd == NULL)
		{
			if (errno != ENOENT)
				ereport(ERROR,
					(errcode(ERROR_SYSTEM),
					 errmsg("could not open file \"%s\": %s", path,
						strerror(errno))));
		}
	}

	/*
	 * Parse the file...
	 */
	while (fd && fgets(fline, sizeof(fline), fd) != NULL)
	{
		/* skip leading whitespaces and check for # comment */
		char	   *ptr;
		char	   *endptr;
		uint32		xlogid, xrecoff;

		for (ptr = fline; *ptr; ptr++)
		{
			if (!IsSpace(*ptr))
				break;
		}
		if (*ptr == '\0' || *ptr == '#')
			continue;

		timeline = pgut_new(pgTimeLine);
		timeline->tli = 0;
		timeline->end = 0;

		/* expect a numeric timeline ID as first field of line */
		timeline->tli = (TimeLineID) strtoul(ptr, &endptr, 0);
		if (endptr == ptr)
			ereport(ERROR,
				(errcode(ERROR_CORRUPTED),
				 errmsg("syntax error(timeline ID) in history file: %s", fline)));

		if (last_timeline && timeline->tli <= last_timeline->tli)
			ereport(ERROR,
				(errcode(ERROR_CORRUPTED),
				   errmsg("timeline IDs must be in increasing sequence, but not")));

		/* Build list with newest item first */
		parray_insert(result, 0, timeline);
		last_timeline = timeline;

		/* parse end point(logfname, xid) in the timeline */
		for (ptr = endptr; *ptr; ptr++)
		{
			if (!IsSpace(*ptr))
				break;
		}

		if (*ptr == '\0' || *ptr == '#')
			ereport(ERROR,
				(errcode(ERROR_CORRUPTED),
				 errmsg("end of log file must follow timeline ID, but not")));

		sscanf(ptr, "%X/%08X", &xlogid, &xrecoff);
		timeline->end = (XLogRecPtr) ((uint64) xlogid << 32) | xrecoff;
		/* we ignore the remainder of each line */
	}

	if (fd)
		fclose(fd);

	if (last_timeline && targetTLI <= last_timeline->tli)
		ereport(ERROR,
			(errcode(ERROR_CORRUPTED),
			 errmsg("timeline IDs must be less than child timeline's ID, but not")));

	/* append target timeline */
	timeline = pgut_new(pgTimeLine);
	timeline->tli = targetTLI;
	timeline->end = (uint64) -1;
	parray_insert(result, 0, timeline);

	/* dump timeline branches for debug */
	elog(DEBUG, "the calculated branch history is as below;");
	for (i = 0; i < parray_num(result); i++)
	{
		elog(DEBUG, "stage %d: timeline ID = %d",
			(int)parray_num(result) - i, ((pgTimeLine *)(parray_get(result,i)))->tli);
	}

	return result;
}

static bool
satisfy_recovery_target(const pgBackup *backup, const pgRecoveryTarget *rt)
{
	char backup_timestamp[20];
	char recovery_timestamp_of_backup[20];
	char recovery_target_timestamp[20];
	time2iso(backup_timestamp, lengthof(backup_timestamp), backup->start_time);

	if (rt->xid_specified)
	{
		if(backup->recovery_xid <= rt->recovery_target_xid)
		{
			ereport(DEBUG,
				(errmsg("backup \"%s\" satisfies the condition of recovery target xid",
					backup_timestamp),
				 errdetail("the recovery target xid is %d, the recovery xid of the backup is %d",
					rt->recovery_target_xid, backup->recovery_xid)));
			return true;
		}
		else
			return false;
	}

	if (rt->time_specified)
	{
		if(backup->recovery_time <= rt->recovery_target_time)
		{
			time2iso(recovery_timestamp_of_backup, lengthof(recovery_timestamp_of_backup),
				backup->recovery_time);
			time2iso(recovery_target_timestamp, lengthof(recovery_target_timestamp),
				rt->recovery_target_time);
			ereport(DEBUG,
			(errmsg("backup \"%s\" satisfies the condition of recovery target time",
				backup_timestamp),
			 errdetail("the recovery target time is \"%s\", "
				"the recovery time of the backup is \"%s\"",
				recovery_target_timestamp, recovery_timestamp_of_backup)));
			return true;
		}
		else
			return false;
	}

	return true;
}

static bool
satisfy_timeline(const parray *timelines, const pgBackup *backup)
{
	int i;
	char timestamp[20];

	time2iso(timestamp, lengthof(timestamp), backup->start_time);
	for (i = 0; i < parray_num(timelines); i++)
	{
		pgTimeLine *timeline = (pgTimeLine *) parray_get(timelines, i);
		if (backup->tli == timeline->tli &&
				backup->stop_lsn < timeline->end)
		{
			elog(DEBUG, "backup \"%s\" has the timeline ID %d",
				timestamp, backup->tli);
			return true;
		}
	}
	return false;
}

/* get TLI of the latest full backup */
static TimeLineID
get_fullbackup_timeline(parray *backups, const pgRecoveryTarget *rt)
{
	int			i;
	pgBackup   *base_backup = NULL;
	TimeLineID	ret;

	for (i = 0; i < parray_num(backups); i++)
	{
		base_backup = (pgBackup *) parray_get(backups, i);

		if (base_backup->backup_mode >= BACKUP_MODE_FULL)
		{
			/*
			 * Validate backup files with its size, because load of CRC
			 * calculation is not right.
			 */
			if (base_backup->status == BACKUP_STATUS_DONE)
				pgBackupValidate(base_backup, true, true, false);

			if(!satisfy_recovery_target(base_backup, rt))
				continue;

			if (base_backup->status == BACKUP_STATUS_OK)
				break;
		}
	}
	/* no full backup found, can't restore */
	if (i == parray_num(backups))
		ereport(ERROR,
			(errcode(ERROR_NO_BACKUP),
			 errmsg("cannot do restore"),
			 errdetail("There is no valid full backup which can be used for given recovery condition.")));

	ret = base_backup->tli;

	return ret;
}

static void
print_backup_id(const pgBackup *backup)
{
	char timestamp[100];
	time2iso(timestamp, lengthof(timestamp), backup->start_time);
	printf(_("  %s (%X/%08X)\n"), timestamp,
			(uint32) (backup->stop_lsn >> 32), (uint32) backup->stop_lsn);
}

static void
search_next_wal(const char *path, uint32 *needId, uint32 *needSeg, parray *timelines)
{
	int		i;
	int		j;
	int		count;
	char	xlogfname[MAXFNAMELEN];
	char	pre_xlogfname[MAXFNAMELEN];
	char	xlogpath[MAXPGPATH];
	struct stat	st;

	Assert(wal_segment_size > 0);

	count = 0;
	for (;;)
	{
		for (i = 0; i < parray_num(timelines); i++)
		{
			pgTimeLine *timeline = (pgTimeLine *) parray_get(timelines, i);
			XLogFileName(xlogfname,
						 timeline->tli,
						 *needId * XLogSegmentsPerXLogId(wal_segment_size) + *needSeg,
						 wal_segment_size);
			join_path_components(xlogpath, path, xlogfname);

			if (stat(xlogpath, &st) == 0)
				break;
		}

		/* not found */
		if (i == parray_num(timelines))
		{
			if (count == 1)
				printf(_("\n"));
			else if (count > 1)
				printf(_(" - %s\n"), pre_xlogfname);

			return;
		}

		count++;
		if (count == 1)
			printf(_("%s"), xlogfname);

		strcpy(pre_xlogfname, xlogfname);

		/* delete old TLI */
		for (j = i + 1; j < parray_num(timelines); j++)
			parray_remove(timelines, i + 1);
		/* XXX: should we add a linebreak when we find a timeline? */

		NextLogSeg(*needId, *needSeg, wal_segment_size);
	}
}

static pgRecoveryTarget *
checkIfCreateRecoveryConf(const char *target_time,
                   const char *target_xid,
                   const char *target_inclusive,
                   const char *target_action)
{
	time_t		dummy_time;
	unsigned int	dummy_xid;
	bool		dummy_bool;
	pgRecoveryTarget *rt;

	// init pgRecoveryTarget
	rt = pgut_new(pgRecoveryTarget);
	rt->time_specified = false;
	rt->xid_specified = false;
	rt->recovery_target_time = 0;
	rt->recovery_target_xid  = 0;
	rt->recovery_target_inclusive = false;
	rt->recovery_target_action = NULL;

	if(target_time)
	{
		rt->time_specified = true;

		if(parse_time(target_time, &dummy_time))
			rt->recovery_target_time = dummy_time;
		else
			ereport(ERROR,
				(errcode(ERROR_ARGS),
				 errmsg("could not create recovery.conf or "
						"add recovery-related options to postgresql.conf(after PG12) with %s", target_time)));
	}

	if(target_xid)
	{
		rt->xid_specified = true;

		if(parse_uint32(target_xid, &dummy_xid))
			rt->recovery_target_xid = dummy_xid;
		else
			ereport(ERROR,
				(errcode(ERROR_ARGS),
				 errmsg("could not create recovery.conf or "
						"add recovery-related options to postgresql.conf(after PG12) with %s", target_xid)));
	}

	if(target_inclusive)
	{
		if(parse_bool(target_inclusive, &dummy_bool))
			rt->recovery_target_inclusive = dummy_bool;
		else
			ereport(ERROR,
				(errcode(ERROR_ARGS),
				 errmsg("could not create recovery.conf or "
						"add recovery-related options to postgresql.conf(after PG12) with %s", target_inclusive)));
	}

	if(target_action)
	{
		if (pg_strcasecmp("pause", target_action) == 0 ||
			pg_strcasecmp("promote", target_action) == 0 ||
			pg_strcasecmp("shutdown", target_action) == 0 )
		{
			/* Although this value doesn't be used, set to match "recovery_target_inclusive". */
			rt->recovery_target_action = target_action;
		} else
			ereport(ERROR,
				(errcode(ERROR_ARGS),
				 errmsg("could not create recovery.conf or "
						"add recovery-related options to postgresql.conf(after PG12) with %s", target_action)));
	}

	return rt;

}

/*
 * Parse the string value passed via --recovery-target-timeline.
 * We need this to support 'latest' as a value for the above
 * parameter.
 *
 * returns an appropriate TimeLineID value and additionally sets
 * *target_tli_latest to 'true' if the input value is 'latest'
 */

static TimeLineID
parse_target_timeline(const char *target_tli_string, TimeLineID cur_tli,
						bool *target_tli_latest)
{
	int32 tmp;
	TimeLineID	result = 0;

	if(strcmp(target_tli_string, "latest") != 0)
	{
		*target_tli_latest = false;
		if(parse_int32(target_tli_string, &tmp))
			result = (TimeLineID) tmp;
		else
			ereport(ERROR,
				(errcode(ERROR_ARGS),
				 errmsg("given value for --recovery-target-timeline is invalid"),
				 errdetail("Timeline value should be either an unsigned 32bit integer "
						"or the string literal 'latest'")));
	}
	else
	{
		*target_tli_latest = true;
		result = findNewestTimeLine(cur_tli);
	}

	return result;
}

/*
 * From PostgreSQL source tree:
 *   src/backend/access/transam/timeline.c
 */
static TimeLineID
findNewestTimeLine(TimeLineID startTLI)
{
	TimeLineID  newestTLI;
	TimeLineID  probeTLI;

	/*
	 * The algorithm is just to probe for the existence of timeline history
	 * files.  XXX is it useful to allow gaps in the sequence?
	 */
	newestTLI = startTLI;

	for (probeTLI = startTLI + 1;; probeTLI++)
	{
		if (existsTimeLineHistory(probeTLI))
			newestTLI = probeTLI;       /* probeTLI exists */
		else	
			break;		/* doesn't exist, assume we're done */
	}

	return newestTLI;
}

/*
 * Similar to a function with the same name from PostgreSQL source tree:
 *   src/backend/access/transam/timeline.c
 */

static bool
existsTimeLineHistory(TimeLineID probeTLI)
{
	FILE	   *fd;
	char		path[MAXPGPATH];

	/* Timeline 1 does not have a history file, so no need to check */
	if (probeTLI == 1)
		return false;

	snprintf(path, lengthof(path), "%s/%08X.history", arclog_path, probeTLI);
	fd = fopen(path, "rt");

	if(fd != NULL)
	{
		fclose(fd);
		return true;
	}
	else if (errno != ENOENT)
		ereport(ERROR,
			(errcode(ERROR_SYSTEM),
			 errmsg("could not open file \"%s\": %s", path, strerror(errno))));

	return false;
}
