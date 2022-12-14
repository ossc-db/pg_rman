/*-------------------------------------------------------------------------
 *
 * validate.c: validate backup files.
 *
 * Copyright (c) 2009-2022, NIPPON TELEGRAPH AND TELEPHONE CORPORATION
 *
 *-------------------------------------------------------------------------
 */

#include "pg_rman.h"

#include <sys/stat.h>

static bool pgBackupValidateFiles(parray *files, const char *root, bool size_only);

/*
 * Validate files in the backup and update its status to OK.
 * If any of files are corrupted, update its status to CORRUPT.
 */
int
do_validate(pgBackupRange *range)
{
	int		i;
	parray *backup_list;
	int ret;
	bool another_pg_rman = false;

	ret = catalog_lock();
	if (ret == 1)
		another_pg_rman = true;

	/* get backup list matches given range */
	backup_list = catalog_get_backup_list(range);
	if(!backup_list)
		ereport(ERROR,
			(errcode(ERROR_SYSTEM),
			 errmsg("could not get list of backup already taken")));

	parray_qsort(backup_list, pgBackupCompareId);
	for (i = 0; i < parray_num(backup_list); i++)
	{
		pgBackup *backup = (pgBackup *)parray_get(backup_list, i);

		/* clean extra backups (switch STATUS to ERROR) */
		if(!another_pg_rman &&
		   (backup->status == BACKUP_STATUS_RUNNING ||
			backup->status == BACKUP_STATUS_DELETING))
		{
			backup->status = BACKUP_STATUS_ERROR;
			pgBackupWriteIni(backup);
		}

		/* Validate completed backups only. */
		if (backup->status != BACKUP_STATUS_DONE)
			continue;

		/* validate with CRC value and update status to OK */
		pgBackupValidate(backup, false, false, (HAVE_DATABASE(backup)));
	}

	/* cleanup */
	parray_walk(backup_list, pgBackupFree);
	parray_free(backup_list);

	catalog_unlock();

	return 0;
}

/*
 * Validate each files in the backup with its size.
 */
void
pgBackupValidate(pgBackup *backup, bool size_only, bool for_get_timeline, bool with_database)
{
	char	timestamp[100];
	char	base_path[MAXPGPATH];
	char	path[MAXPGPATH];
	parray *files;
	bool	corrupted = false;

	time2iso(timestamp, lengthof(timestamp), backup->start_time);
	if(!for_get_timeline)
	{
		if (with_database && backup->with_serverlog)
		{
			if (check)
				elog(INFO, "will validate: \"%s\" backup, archive log files and server log files by %s",
						timestamp, (size_only ? "SIZE" : "CRC"));
			else
				elog(INFO, "validate: \"%s\" backup, archive log files and server log files by %s",
						timestamp, (size_only ? "SIZE" : "CRC"));
		}
		else if (with_database)
		{
			if (check)
				elog(INFO, "will validate: \"%s\" backup and archive log files by %s",
						timestamp, (size_only ? "SIZE" : "CRC"));
			else
				elog(INFO, "validate: \"%s\" backup and archive log files by %s",
						timestamp, (size_only ? "SIZE" : "CRC"));
		}
		else if ((backup->backup_mode == BACKUP_MODE_ARCHIVE) && backup->with_serverlog)
		{
			if (check)
				elog(INFO, "will validate: \"%s\" archive log files and server log files by %s",
						timestamp, (size_only ? "SIZE" : "CRC"));
			else
				elog(INFO, "validate: \"%s\" archive log files and server log files by %s",
						timestamp, (size_only ? "SIZE" : "CRC"));
		}
		else if (backup->backup_mode == BACKUP_MODE_ARCHIVE)
		{
			if (check)
				elog(INFO, "will validate: \"%s\" archive log files by %s",
						timestamp, (size_only ? "SIZE" : "CRC"));
			else
				elog(INFO, "validate: \"%s\" archive log files by %s",
						timestamp, (size_only ? "SIZE" : "CRC"));
		}
	}

	if(!check)
	{
		if (HAVE_DATABASE(backup))
		{
			elog(DEBUG, "checking database files");
			pgBackupGetPath(backup, base_path, lengthof(base_path), DATABASE_DIR);
			pgBackupGetPath(backup, path, lengthof(path),
				DATABASE_FILE_LIST);
			files = dir_read_file_list(base_path, path);
			if (!pgBackupValidateFiles(files, base_path, size_only))
				corrupted = true;
			parray_walk(files, pgFileFree);
			parray_free(files);
		}
		if (HAVE_ARCLOG(backup))
		{
			elog(DEBUG, "checking archive WAL files");
			pgBackupGetPath(backup, base_path, lengthof(base_path), ARCLOG_DIR);
			pgBackupGetPath(backup, path, lengthof(path), ARCLOG_FILE_LIST);
			files = dir_read_file_list(base_path, path);
			if (!pgBackupValidateFiles(files, base_path, size_only))
				corrupted = true;
			parray_walk(files, pgFileFree);
			parray_free(files);
		}
		if (backup->with_serverlog)
		{
			elog(DEBUG, "checking server log files");
			pgBackupGetPath(backup, base_path, lengthof(base_path), SRVLOG_DIR);
			pgBackupGetPath(backup, path, lengthof(path), SRVLOG_FILE_LIST);
			files = dir_read_file_list(base_path, path);
			if (!pgBackupValidateFiles(files, base_path, size_only))
				corrupted = true;
			parray_walk(files, pgFileFree);
			parray_free(files);
		}

		/* update status to OK */
		if (corrupted)
			backup->status = BACKUP_STATUS_CORRUPT;
		else
			backup->status = BACKUP_STATUS_OK;
		pgBackupWriteIni(backup);

		if (corrupted)
			elog(WARNING, "backup \"%s\" is corrupted", timestamp);
		else
			elog(INFO, "backup \"%s\" is valid", timestamp);
	}
}

static const char *
get_relative_path(const char *path, const char *root)
{
	size_t	rootlen = strlen(root);
	if (strncmp(path, root, rootlen) == 0 && path[rootlen] == '/')
		return path + rootlen + 1;
	else
		return path;
}

/*
 * Validate files in the backup with size or CRC.
 */
static bool
pgBackupValidateFiles(parray *files, const char *root, bool size_only)
{
	int		i;

	for (i = 0; i < parray_num(files); i++)
	{
		struct stat st;

		pgFile *file = (pgFile *) parray_get(files, i);

		if (interrupted)
			ereport(FATAL,
				(errcode(ERROR_INTERRUPTED),
				 errmsg("interrupted during validate")));

		/* skipped backup while incremental backup */
		if (file->write_size == BYTES_INVALID || !S_ISREG(file->mode))
			continue;

		/* print progress */
		if (verbose)
			elog(DEBUG, _("(%d/%lu) validating %s"), i + 1, (unsigned long) parray_num(files),
				get_relative_path(file->path, root));

		/* always validate file size */
		if (stat(file->path, &st) == -1)
		{
			if (errno == ENOENT)
				elog(WARNING, _("backup file \"%s\" vanished"), file->path);
			else
				ereport(ERROR,
					(errcode(ERROR_SYSTEM),
					 errmsg("could not stat backup file \"%s\": %s",
						get_relative_path(file->path, root), strerror(errno))));
			return false;
		}
		if (file->write_size != st.st_size)
		{
			elog(WARNING, _("size of backup file \"%s\" must be %lu but %lu"),
				get_relative_path(file->path, root),
				(unsigned long) file->write_size,
				(unsigned long) st.st_size);
			return false;
		}

		/* validate CRC too */
		if (!size_only)
		{
			pg_crc32c	crc;

			crc = pgFileGetCRC(file);
			if (crc != file->crc)
			{
				elog(WARNING, _("CRC calculation showed incorrect result"));
				if(verbose)
				{
					elog(WARNING, _("CRC of backup file \"%s\" must be %X but %X"),
						get_relative_path(file->path, root), file->crc, crc);
				}
				return false;
			}
		}
	}

	return true;
}
