/*-------------------------------------------------------------------------
 *
 * delete.c: delete backup files.
 *
 * Copyright (c) 2009-2025, NIPPON TELEGRAPH AND TELEPHONE CORPORATION
 *
 *-------------------------------------------------------------------------
 */

#include "pg_rman.h"
#include <time.h>

static int pgBackupDeleteFiles(pgBackup *backup);

/*
 *  Check backup lists and decide which to delete.
 *  The backups which are not necessary for PITR until given date range.
 *  If force option is set true, delete backups taken older than given
 *  date without checking the necessity for PITR.
 */
int
do_delete(pgBackupRange *range, bool force)
{
	int		i;
	int		ret;
	parray *backup_list;
	bool	found_boundary_to_keep;
	char 	backup_timestamp[20];
	char 	given_timestamp[20];

	if (force)
		ereport(WARNING,
				(errmsg("using force option will make some of the remaining backups unusable"),
				 errdetail("Any remaining incremental backups that are older than the oldest"
					 " available full backup cannot be restored.")));

	/* DATE are always required */
	if (!pgBackupRangeIsValid(range))
		ereport(ERROR,
			(errcode(ERROR_ARGS),
			 errmsg("delete range option not specified"),
			 errhint("Please run with 'pg_rman delete DATE'.")));

	found_boundary_to_keep = false;
	time2iso(given_timestamp, lengthof(given_timestamp), range->begin);

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

	/* get list of backups. */
	backup_list = catalog_get_backup_list(NULL);
	if(!backup_list)
		ereport(ERROR,
			(errcode(ERROR_SYSTEM),
			 errmsg("could not get list of backup already taken")));

	/* check each backups and delete if possible */
	for (i = 0; i < parray_num(backup_list); i++)
	{
		pgBackup *backup = (pgBackup *)parray_get(backup_list, i);
		time2iso(backup_timestamp, lengthof(backup_timestamp), backup->start_time);

		/* We keep backups until finding the validated full backup
		 * which is necessary for recovery until specified DATE.
		 */
		if (backup->start_time > range->begin)
			continue;

		if (!force && !found_boundary_to_keep &&
			backup->status == BACKUP_STATUS_OK)
		{
			/* Check whether this is the first validate full backup before the specified DATE */
			if (backup->backup_mode >= BACKUP_MODE_FULL)
			{
				ereport(WARNING,
					(errmsg("cannot delete backup with start time \"%s\"", backup_timestamp),
					 errdetail("This is the latest full backup necessary for successful recovery.")));
				found_boundary_to_keep = true;
			}
			else
				ereport(WARNING,
					(errmsg("cannot delete backup with start time \"%s\"", backup_timestamp),
					errdetail("This is the %s backup necessary for successful"
							  " recovery.",
							  backup->backup_mode == BACKUP_MODE_ARCHIVE
										? "archive" : "incremental")));
			/* keep this backup */
			continue;
		}

		/* check for interrupt */
		if (interrupted)
			ereport(FATAL,
				(errcode(ERROR_INTERRUPTED),
				 errmsg("interrupted during delete backup")));

		/* Do actual deletion */
		pgBackupDeleteFiles(backup);
	}

	/* release catalog lock */
	catalog_unlock();

	/* cleanup */
	parray_walk(backup_list, pgBackupFree);
	parray_free(backup_list);

	return 0;
}

/*
 * Delete backups that are older than KEEP_xxx_DAYS, or have more generations
 * than KEEP_xxx_GENERATIONS.
 */
void
pgBackupDelete(int keep_generations, int keep_days)
{
	int		i;
	parray *backup_list;
	int     existed_generations;
	bool    check_generations;
	bool    check_days;
	bool    last_checked_is_valid_full_backup;
	time_t  tim;
	time_t  keep_after = 0;
	struct tm *ltm;
	char    backup_timestamp[20];
	char    keep_after_timestamp[20];
	char    generations_str[16];
	char    days_str[16];
	char    *count_suffix;

	/* determine whether to check based on the given generation number */
	if (keep_generations == KEEP_INFINITE)
	{
		check_generations = false;
		strncpy(generations_str, "INFINITE",
					lengthof(generations_str));
	}
	else
	{
		check_generations = true;
		snprintf(generations_str, lengthof(generations_str),
					"%d", keep_generations);
	}

	/* determine whether to check based on the given days */
	if (keep_days == KEEP_INFINITE)
	{
		check_days = false;
		strncpy(days_str, "INFINITE", lengthof(days_str));
	}
	else
	{
		check_days = true;
		snprintf(days_str, lengthof(days_str),
				"%d", keep_days);
		/*
		 * Calculate the threshold day from given keep_days.
		 * Any backup taken before this threshold day to be
		 * a candidate for deletion.
		 */
		tim = current.start_time - (keep_days * 60 * 60 * 24);
		ltm = localtime(&tim);
		ltm->tm_hour = 0;
		ltm->tm_min  = 0;
		ltm->tm_sec  = 0;
		keep_after = mktime(ltm);
		time2iso(keep_after_timestamp, lengthof(keep_after_timestamp),
					keep_after);
	}

	if (check_generations && check_days)
		elog(INFO, "start deleting old backup (keep generations = %s AND keep after = %s)",
				generations_str, keep_after_timestamp);
	else if (check_generations)
		elog(INFO, "start deleting old backup (keep generations = %s)",
				generations_str);
	else if (check_days)
		elog(INFO, "start deleting old backup (keep after = %s)",
				keep_after_timestamp);
	else
	{
		elog(DEBUG, "do not delete old backup files");
		return;
	}

	/* get list of backups. */
	backup_list = catalog_get_backup_list(NULL);

	/* find delete target backup. */
	existed_generations = 0;
	last_checked_is_valid_full_backup = false;
	for (i = 0; i < parray_num(backup_list); i++)
	{
		pgBackup *backup = (pgBackup *)parray_get(backup_list, i);
		time2iso(backup_timestamp, lengthof(backup_timestamp),
            backup->start_time);

		elog(DEBUG, "--------------------------------------------");
		elog(DEBUG, "checking backup : \"%s\"", backup_timestamp);

		if (check_generations)
		{
			if (existed_generations < keep_generations)
			{
				/* do not include the full backup just taken in a count */
				if (backup->start_time == current.start_time
					&& backup->status == BACKUP_STATUS_DONE)
				{
					elog(INFO, "does not include the backup just taken");
					continue;
				}
				/* If validate full backup is found,
				 * count it as next generation.
				 */
				if (backup->backup_mode == BACKUP_MODE_FULL
					&& backup->status == BACKUP_STATUS_OK)
				{
					existed_generations++;
					count_suffix = getCountSuffix(existed_generations);
					ereport(INFO,
						(errmsg("backup \"%s\" should be kept", backup_timestamp),
						 errdetail("This is the %d%s latest full backup.",
								existed_generations, count_suffix)));
				}
				else if (((backup->backup_mode == BACKUP_MODE_INCREMENTAL) ||
						   (backup->backup_mode == BACKUP_MODE_ARCHIVE)) &&
							backup->status == BACKUP_STATUS_OK)
				{
					/* incremental backup and archive backup are
					 * belongs to one before generation full backup.
					 */
					count_suffix = getCountSuffix(existed_generations + 1);
					ereport(INFO,
						(errmsg("backup \"%s\" should be kept", backup_timestamp),
						 errdetail("This belongs to the %d%s latest full backup.",
								existed_generations + 1, count_suffix)));
				}
				else
					ereport(WARNING,
						(errmsg("backup \"%s\" is not taken into account", backup_timestamp),
						 errdetail("This is not a valid backup.")));

				/* move to next backup */
				continue;
			}
			else if (existed_generations == keep_generations)
			{
				/* set the flag for check_days because we have just found valid full backup */
				last_checked_is_valid_full_backup = true;
				/* unset flags because the generation condition is already satisfied */
				check_generations = false;
			}
		}

		if (check_days)
		{
			/* If the start time of backup is older than the threshold,
			 * it is a candidate of deletion. Here, it should be avoided
			 * deleting full backup which was taken before the threshold
			 * but is required by incremental or archive backup in keep.
			 */
			if (backup->start_time >= keep_after || !last_checked_is_valid_full_backup)
			{
				/* do not include the full backup just taken in a count */
				if (backup->start_time == current.start_time &&
					backup->status == BACKUP_STATUS_DONE)
				{
					elog(INFO, "does not include the backup just taken");
					continue;
				}

				if (backup->start_time >= keep_after &&
					backup->status == BACKUP_STATUS_OK)
					ereport(INFO,
							(errmsg("backup \"%s\" should be kept", backup_timestamp),
							 errdetail("This is taken after \"%s\".", keep_after_timestamp)));
				else if (backup->start_time < keep_after &&
						 !last_checked_is_valid_full_backup)
					ereport(WARNING,
							(errmsg("backup \"%s\" should be kept", backup_timestamp),
							 errdetail("This is taken before \"%s\", but there is an incremental "
										"or archive backup to be kept which requires this backup.",
										keep_after_timestamp)));
				else
					ereport(WARNING,
						(errmsg("backup \"%s\" is not taken int account", backup_timestamp),
						 errdetail("This is not valid backup.")));

				if(backup->backup_mode == BACKUP_MODE_FULL &&
				   backup->status == BACKUP_STATUS_OK)
					last_checked_is_valid_full_backup = true;
				else if (backup->backup_mode < BACKUP_MODE_FULL &&
						 backup->status == BACKUP_STATUS_OK)
					last_checked_is_valid_full_backup = false;

				/* move to next backup */
				continue;
			}
		}

		/* delete backup and update status to DELETED */
		pgBackupDeleteFiles(backup);
	}

	/* cleanup */
	parray_walk(backup_list, pgBackupFree);
	parray_free(backup_list);
}

/*
 * Delete backup files of the backup and update the status of the backup to
 * BACKUP_STATUS_DELETED.
 */
static int
pgBackupDeleteFiles(pgBackup *backup)
{
	int		i;
	char	path[MAXPGPATH];
	char	timestamp[20];
	parray *files;

	time2iso(timestamp, lengthof(timestamp), backup->start_time);
	/*
	 * If the backup was deleted already, nothing to do and such situation
	 * is not error.
	 */
	if (backup->status == BACKUP_STATUS_DELETED)
	{
		elog(DEBUG, "backup \"%s\" has been already deleted", timestamp);
		return 0;
	}


	if (check)
		elog(INFO, _("will delete the backup with start time: \"%s\""), timestamp);
	else
		elog(INFO, _("delete the backup with start time: \"%s\""), timestamp);

	/*
	 * update STATUS to BACKUP_STATUS_DELETING in preparation for the case which
	 * the error occurs before deleting all backup files.
	 */
	if (!check)
	{
		backup->status = BACKUP_STATUS_DELETING;
		pgBackupWriteIni(backup);
	}

	/* list files to be deleted */
	files = parray_new();
	pgBackupGetPath(backup, path, lengthof(path), DATABASE_DIR);
	dir_list_file(files, path, NULL, true, true);
	pgBackupGetPath(backup, path, lengthof(path), ARCLOG_DIR);
	dir_list_file(files, path, NULL, true, true);
	pgBackupGetPath(backup, path, lengthof(path), SRVLOG_DIR);
	dir_list_file(files, path, NULL, true, true);

	/* delete leaf node first */
	parray_qsort(files, pgFileComparePathDesc);
	for (i = 0; i < parray_num(files); i++)
	{
		pgFile *file = (pgFile *) parray_get(files, i);

		/* print progress */
		if (verbose)
			elog(DEBUG, _("delete file(%d/%lu) \"%s\"\n"), i + 1,
				(unsigned long) parray_num(files), file->path);

		/* skip actual deletion in check mode */
		if (!check)
		{
			if (remove(file->path))
			{
				elog(WARNING, _("could not remove \"%s\": %s"), file->path,
					strerror(errno));
				parray_walk(files, pgFileFree);
				parray_free(files);
				return 1;
			}
		}
	}

	/*
	 * After deleting all of the backup files, update STATUS to
	 * BACKUP_STATUS_DELETED.
	 */
	if (!check)
	{
		backup->status = BACKUP_STATUS_DELETED;
		pgBackupWriteIni(backup);
	}

	parray_walk(files, pgFileFree);
	parray_free(files);

	return 0;
}

/*
 * Remove DELETED backups from BACKUP_PATH directory.
 */
int do_purge(void)
{
	int		i, j;
	int		ret;
	int		any_errors;
	parray *backup_list;
	parray *files;
	pgBackup *backup;
	char 	timestamp[20];
	char	path[MAXPGPATH];

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

	/* get list of backups. */
	backup_list = catalog_get_backup_list(NULL);
	if(!backup_list)
		ereport(ERROR,
			(errcode(ERROR_SYSTEM),
			 errmsg("could not get list of backup already taken")));

	for (i=0; i < parray_num(backup_list); i++)
	{
		backup = parray_get(backup_list, i);

		/* skip living backups */
		if(backup->status != BACKUP_STATUS_DELETED)
			continue;

		time2iso(timestamp, lengthof(timestamp), backup->start_time);
		pgBackupGetPath(backup, path, lengthof(path), NULL);
		
		if (check)
			ereport(INFO,
				(errmsg("DELETED backup \"%s\" will be purged", timestamp),
				 errdetail("The path is %s", path)));

		files = parray_new();
		dir_list_file(files, path, NULL, false, true);
		parray_qsort(files, pgFileComparePathDesc);
		any_errors = 0;
		for (j = 0; j < parray_num(files); j++)
		{
			pgFile *file = (pgFile *) parray_get(files, j);

			/* print progress */
			if (check)
			{
				if (verbose)
				{
					/* skip actual deletion in check mode */
					elog(DEBUG, _("will delete file(%d/%lu) \"%s\"\n"), j + 1,
						(unsigned long) parray_num(files), file->path);
					continue;
				}
			}
			else
			{
				if(verbose)
					elog(DEBUG, _("delete file(%d/%lu) \"%s\"\n"), j + 1,
						(unsigned long) parray_num(files), file->path);

				if (remove(file->path))
				{
					elog(WARNING, _("could not remove \"%s\": %s"), file->path,
						strerror(errno));
					any_errors++;
				}
			}
		}

		if (!check)
		{
			/* check the parent directory where deleted backup belongs to can be deleted. */
			delete_parent_dir(path);

			/* check whether there happens some errors in purging */
			if(any_errors)
				elog(WARNING, _("some errors are occurred in purging backup \"%s\""), timestamp);
			else
				elog(INFO, _("DELETED backup \"%s\" is purged"), timestamp);
		}

		parray_free(files);
	}
	return 0;
}

/*
 * Utility function for selecting suffix for number.
 */
char* getCountSuffix(int number)
{
	return (1 == (number % 100)) ? "st"
				: (2 == (number %100)) ? "nd"
				: (3 == (number %100)) ? "rd"
				: "th";
}
