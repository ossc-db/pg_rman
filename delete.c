/*-------------------------------------------------------------------------
 *
 * delete.c: delete backup files.
 *
 * Copyright (c) 2009-2015, NIPPON TELEGRAPH AND TELEPHONE CORPORATION
 *
 *-------------------------------------------------------------------------
 */

#include "pg_rman.h"

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
	char	*backup_mode;

	/* DATE are always required */
	if (!pgBackupRangeIsValid(range))
		ereport(ERROR,
			(errcode(ERROR_ARGS),
			 errmsg("required delete range option not specified: delete DATE")));

	found_boundary_to_keep = false;
	time2iso(given_timestamp, lengthof(given_timestamp), range->begin);

	/* get exclusive lock of backup catalog */
	ret = catalog_lock();
	if (ret == -1)
		ereport(ERROR,
			(errcode(ERROR_SYSTEM),
			 errmsg("could not  lock backup catalog.")));
	else if (ret == 1)
		ereport(ERROR,
			(errcode(ERROR_ALREADY_RUNNING),
			 errmsg("another pg_rman is running, stop delete.")));

	/* get list of backups. */
	backup_list = catalog_get_backup_list(NULL);
	if(!backup_list){
		ereport(ERROR,
			(errcode(ERROR_SYSTEM),
			 errmsg("could not get list of backup already taken.")));
	}

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
		else
		{
			if (!force && !found_boundary_to_keep)
			{
				/* Check whether this is the first validate full backup before the specified DATE */
				if (backup->backup_mode >= BACKUP_MODE_FULL &&
					backup->status == BACKUP_STATUS_OK)
				{
					ereport(WARNING,
						(errmsg("cannot delete backup with start time \"%s\"", backup_timestamp),
						 errdetail("it is the latest full backup necessary for successful recovery")));
					found_boundary_to_keep = true;
				} else {
					backup_mode = (backup->backup_mode == BACKUP_MODE_INCREMENTAL) ? "incremental" : "archive";
					ereport(WARNING,
						(errmsg("cannot delete backup with start time \"%s\"", backup_timestamp),
						errdetail("it is an %s backup necessary"
								" for successful recovery", backup_mode)));
				}

				/* keep this backup */
				continue;
			}

			/* check for interrupt */
			if (interrupted)
			{
				ereport(FATAL,
					(errcode(ERROR_INTERRUPTED),
					 errmsg("interrupted during delete backup")));
			}

			/* Do actual deletion */
			pgBackupDeleteFiles(backup);
		}
	}

	/* release catalog lock */
	catalog_unlock();

	/* cleanup */
	parray_walk(backup_list, pgBackupFree);
	parray_free(backup_list);

	return 0;
}

/*
 * Delete backups that are older than KEEP_xxx_DAYS and have more generations
 * than KEEP_xxx_FILES.
 */
void
pgBackupDelete(int keep_generations, int keep_days)
{
	int		i;
	parray *backup_list;
	int		backup_num;
	time_t	days_threshold = current.start_time - (keep_days * 60 * 60 * 24);


	if (verbose)
	{
		char generations_str[100];
		char days_str[100];

		if (keep_generations == KEEP_INFINITE)
			strncpy(generations_str, "INFINITE",
					lengthof(generations_str));
		else
			snprintf(generations_str, lengthof(generations_str),
					"%d", keep_generations);

		if (keep_days == KEEP_INFINITE)
			strncpy(days_str, "INFINITE", lengthof(days_str));
		else
			snprintf(days_str, lengthof(days_str), "%d", keep_days);

		printf(_("delete old backups (generations=%s, days=%s)\n"),
			generations_str, days_str);
	}

	/* delete files which satisfy both condition */
	if (keep_generations == KEEP_INFINITE || keep_days == KEEP_INFINITE)
	{
		elog(DEBUG, "do not delete old backup files");
		return;
	}

	/* get list of backups. */
	backup_list = catalog_get_backup_list(NULL);

	backup_num = 0;
	/* find delete target backup. */
	for (i = 0; i < parray_num(backup_list); i++)
	{
		pgBackup *backup = (pgBackup *)parray_get(backup_list, i);

		elog(DEBUG, "%s() %lu", __FUNCTION__, backup->start_time);
		/*
		 * when validate full backup was found, we can delete the backup
		 * that is older than it
		 */
		if (backup->backup_mode >= BACKUP_MODE_FULL &&
			backup->status == BACKUP_STATUS_OK)
			backup_num++;

		/* do not include the latest full backup in a count. */
//		if (backup_num - 1 <= keep_generations)
		if (backup_num <= keep_generations)
		{
			elog(DEBUG, "%s() backup are only %d", __FUNCTION__, backup_num);
			continue;
		}

		/*
		 * If the start time of the backup is older than the threshold and
		 * there are enough generations of full backups, delete the backup.
		 */
		if (backup->start_time >= days_threshold)
		{
			elog(DEBUG, "%s() %lu is not older than %lu", __FUNCTION__,
				backup->start_time, days_threshold);
			continue;
		}

		elog(DEBUG, "%s() %lu is older than %lu", __FUNCTION__,
			backup->start_time, days_threshold);

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

	/*
	 * If the backup was deleted already, nothing to do and such situation
	 * is not error.
	 */
	if (backup->status == BACKUP_STATUS_DELETED)
		return 0;

	time2iso(timestamp, lengthof(timestamp), backup->start_time);

	if (check)
	{
		elog(INFO, _("will delete the backup with start time: \"%s\""), timestamp);
	} else {
		elog(INFO, _("delete the backup with start time: \"%s\""), timestamp);
	}

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
				elog(WARNING, _("cannot remove \"%s\": %s"), file->path,
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
			 errmsg("could not lock backup catalog.")));
	else if (ret == 1)
		ereport(ERROR,
			(errcode(ERROR_ALREADY_RUNNING),
			errmsg("another pg_rman is running, stop delete.")));

	/* get list of backups. */
	backup_list = catalog_get_backup_list(NULL);
	if(!backup_list){
		ereport(ERROR,
			(errcode(ERROR_SYSTEM),
			 errmsg("could not get list of backup already taken.")));
	}

	for (i=0; i < parray_num(backup_list); i++)
	{
		backup = parray_get(backup_list, i);

		/* skip living backups */
		if(backup->status != BACKUP_STATUS_DELETED)
			continue;

		time2iso(timestamp, lengthof(timestamp), backup->start_time);
		pgBackupGetPath(backup, path, lengthof(path), NULL);
		
		if (check)
		{
			elog(INFO, _("The DELETED backup \"%s\" will be purged."), timestamp);
			elog(INFO, _("The path is %s"), path);
		}

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
			} else {
				if(verbose)
				{
					elog(DEBUG, _("delete file(%d/%lu) \"%s\"\n"), j + 1,
						(unsigned long) parray_num(files), file->path);
				}

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
			{
				elog(WARNING, _("There are errors in purging backup \"%s\""), timestamp);
			} else {
				elog(INFO, _("The DELETED backup \"%s\" is purged."), timestamp);
			}
		}
	}
	return 0;
}
