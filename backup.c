/*-------------------------------------------------------------------------
 *
 * backup.c: backup DB cluster, archived WAL, serverlog.
 *
 * Copyright (c) 2009-2025, NIPPON TELEGRAPH AND TELEPHONE CORPORATION
 *
 *-------------------------------------------------------------------------
 */

#include "pg_rman.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <dirent.h>
#include <time.h>
#include <math.h>

#include "catalog/pg_control.h"
#include "common/controldata_utils.h"
#include "pgut/pgut-port.h"

#define TIMEOUT_ARCHIVE		10		/* wait 10 sec until WAL archive complete */

static bool		 in_backup = false;	/* TODO: more robust logic */
static parray	*cleanup_list;		/* list of command to execute at error processing for snapshot */

/*
 * data_checksum_enabled as read from the control file of the database
 * cluster.  Exposed for use in data.c.
 */
bool	data_checksum_enabled = false;
static void init_data_checksum_enabled(void);

/*
 * Backup routines
 */
static void backup_cleanup(bool fatal, void *userdata);
static void delete_old_files(const char *root, parray *files, int keep_files,
							 int keep_days, bool is_arclog);
static void backup_files(const char *from_root, const char *to_root,
	parray *files, parray *prev_files, const XLogRecPtr *lsn, bool compress, const char *prefix);
static parray *do_backup_database(parray *backup_list, pgBackupOption bkupopt);
static parray *do_backup_arclog(parray *backup_list);
static parray *do_backup_srvlog(parray *backup_list);
static void confirm_block_size(const char *name, int blcksz);
static void pg_backup_start(const char *label, bool smooth, pgBackup *backup);
static parray *pg_backup_stop(pgBackup *backup);
static void pg_switch_wal(pgBackup *backup);
static void get_lsn(PGresult *res, TimeLineID *timeline, XLogRecPtr *lsn);
static void get_xid(PGresult *res, uint32 *xid);
static bool execute_restartpoint(pgBackupOption bkupopt, pgBackup *backup);

static void delete_arclog_link(void);
static void delete_online_wal_backup(void);

static bool dirExists(const char *path);

static void execute_freeze(void);
static void execute_unfreeze(void);
static void execute_split(parray *tblspc_list);
static void execute_resync(void);
static void execute_mount(parray *tblspcmp_list);
static void execute_umount(void);
static void execute_script(const char *mode, bool is_cleanup, parray *output);
static void snapshot_cleanup(bool fatal, void *userdata);
static void add_files(parray *files, const char *root, bool add_root, bool is_pgdata);
static int strCompare(const void *str1, const void *str2);
static void create_file_list(parray *files, const char *root, const char *prefix, bool is_append);
static void check_server_version(void);

static int wal_segment_size = 0;

/*
 * Take a backup of database.
 */
static parray *
do_backup_database(parray *backup_list, pgBackupOption bkupopt)
{
	int			i;
	parray	   *files;				/* backup file list from non-snapshot */
	parray	   *prev_files = NULL;	/* file list of previous database backup */
	FILE	   *fp;
	char		path[MAXPGPATH];
	char		label[1024];
	XLogRecPtr *lsn = NULL;
	char		prev_file_txt[MAXPGPATH];	/* path of the previous backup list file */

	/* repack the options */
	bool	smooth_checkpoint = bkupopt.smooth_checkpoint;

	check_server_version();

	init_data_checksum_enabled();

	if (!HAVE_DATABASE(&current))
	{
		/* check if arclog backup. if arclog backup and no suitable full backup, */
		if (HAVE_ARCLOG(&current))
		{
			pgBackup   *prev_backup;

			/* find last completed database backup */
			prev_backup = catalog_get_last_data_backup(backup_list);
			if (prev_backup == NULL)
			{
				if (current.full_backup_on_error)
				{
					ereport(NOTICE,
						(errmsg("turn to take a full backup"),
						 errdetail("There is no validated full backup with current timeline.")));
					current.backup_mode = BACKUP_MODE_FULL;
				}
				else
					ereport(ERROR,
						(errcode(ERROR_SYSTEM),
						errmsg("cannot take an incremental backup"),
						errdetail("There is no validated full backup with current timeline."),
						errhint("Please take a full backup and validate it before doing an archive backup. "
							"Or use with --full-backup-on-error command line option.")));
			}
			else
				return NULL;
		}
	}

	elog(INFO, _("copying database files"));

	/* initialize size summary */
	current.total_data_bytes = 0;
	current.read_data_bytes = 0;

	/* notify start of backup to PostgreSQL server */
	time2iso(label, lengthof(label), current.start_time);
	strncat(label, " with pg_rman", lengthof(label) - strlen(label) - 1);
	pg_backup_start(label, smooth_checkpoint, &current);

	/* Execute restartpoint on standby once replay reaches the backup LSN */
	if (current.is_from_standby && !execute_restartpoint(bkupopt, &current))
	{
		/*
		 * Disconnecting automatically aborts a non-exclusive backup, so no
		 * need to call pg_backup_stop() do it for us.
		 */
		disconnect();
		ereport(ERROR,
			(errcode(ERROR_SYSTEM),
			 errmsg("could not execute restartpoint")));
	}

	/*
	 * Generate mkdirs.sh required to recreate the directory structure of
	 * PGDATA when restoring.  Omits $PGDATA from being listed in the
	 * commands.  Note that the resulting mkdirs.sh file is part of the
	 * backup.
	 */
	files = parray_new();
	dir_list_file(files, pgdata, NULL, false, false);

	if (!check)
	{
		pgBackupGetPath(&current, path, lengthof(path), MKDIRS_SH_FILE);
		fp = fopen(path, "wt");
		if (fp == NULL)
			ereport(ERROR,
				(errcode(ERROR_SYSTEM),
				 errmsg("could not open make directory script \"%s\": %s",
						path, strerror(errno))));
		dir_print_mkdirs_sh(fp, files, pgdata);
		fclose(fp);
		if (chmod(path, DIR_PERMISSION) == -1)
			ereport(ERROR,
				(errcode(ERROR_SYSTEM),
				 errmsg("could not change mode of \"%s\": %s", path,
						strerror(errno))));
	}

	/* Free no longer needed memory. */
	parray_walk(files, pgFileFree);
	parray_free(files);
	files = NULL;

	/*
	 * To take incremental backup, the file list of the latest validated
	 * full database backup is needed.
	 * TODO: fix for issue #154
	 * When a backup list is deleted with rm command or pg_rman's delete command with '--force' option,
	 * pg_rman can't detect there is a missing piece of backup.
	 * We need the way tracing the backup chains or something else...
	 */
	if (current.backup_mode < BACKUP_MODE_FULL)
	{
		pgBackup   *prev_backup;
		uint32		xlogid, xrecoff;

		/* find last completed database backup */
		prev_backup = catalog_get_last_data_backup(backup_list);
		if (prev_backup == NULL || prev_backup->tli != current.tli)
		{
			if (current.full_backup_on_error)
			{
				ereport(NOTICE,
					(errmsg("turn to take a full backup"),
					 errdetail("There is no validated full backup with current timeline.")));
				current.backup_mode = BACKUP_MODE_FULL;
			}
			else
				ereport(ERROR,
					(errcode(ERROR_SYSTEM),
					 errmsg("cannot take an incremental backup"),
					 errdetail("There is no validated full backup with current timeline."),
					 errhint("Please take a full backup and validate it before doing an incremental backup. "
						"Or use with --full-backup-on-error command line option.")));
		}
		else
		{
			pgBackupGetPath(prev_backup, prev_file_txt, lengthof(prev_file_txt),
				DATABASE_FILE_LIST);
			prev_files = dir_read_file_list(pgdata, prev_file_txt);

			/*
			 * Do backup only pages having larger LSN than previous backup.
			 */
			lsn = &prev_backup->start_lsn;
			xlogid = (uint32) (*lsn >> 32);
			xrecoff = (uint32) *lsn;
			elog(DEBUG, _("backup only the page updated after LSN(%X/%08X)"),
							xlogid, xrecoff);
		}
	}

	/* initialize backup list from non-snapshot */
	files = parray_new();

	/*
	 * Check the existence of the snapshot-script and perform backup using the
	 * snapshot_script if one is provided.
	 */
	join_path_components(path, backup_path, SNAPSHOT_SCRIPT_FILE);
	if (!fileExists(path))
	{
		/* Nope.  Perform a simple backup by copying files. */

		parray		*stop_backup_files;

		/*
		 * List files contained in PGDATA, omitting all sub-directories beside
		 * base, global and pg_tblspc.  Omits $PGDATA from the paths.
		 */
		add_files(files, pgdata, false, true);

		if (current.backup_mode == BACKUP_MODE_FULL)
			elog(DEBUG, "taking full backup of database files");
		else if (current.backup_mode == BACKUP_MODE_INCREMENTAL)
			elog(DEBUG, "taking incremental backup of database files");

		/* Construct the directory for this backup within BACKUP_PATH. */
		pgBackupGetPath(&current, path, lengthof(path), DATABASE_DIR);

		/* Save the files listed above. */
		backup_files(pgdata, path, files, prev_files, lsn, current.compress_data, NULL);

		/*
		 * Notify end of backup and save the backup_label and tablespace_map
		 * files.
		 */
		stop_backup_files = pg_backup_stop(&current);

		/* stop_backup_files must be listed in file_database.txt. */
		files = parray_concat(stop_backup_files, files);

		/*
		 * Construct file_database.txt listing all files we just saved under
		 * DATABASE_DIR.
		 */
		create_file_list(files, pgdata, NULL, false);
	}
	else
	{
		/* Use snapshot_script. */

		parray		*tblspc_list;	/* list of name of TABLESPACE backup from snapshot */
		parray		*tblspcmp_list;	/* list of mounted directory of TABLESPACE in snapshot volume */
		PGresult	*tblspc_res;	/* contain spcname and oid in TABLESPACE */
		parray		*stop_backup_files;	/* list of files that pg_backup_stop() wrote */

		/* if backup is from standby, snapshot backup is unsupported */
		if (current.is_from_standby)
		{
			/*
			 * Disconnecting automatically aborts a non-exclusive backup, so no
			 * need to call pg_backup_stop() do it for us.
			 */
			disconnect();
			ereport(ERROR,
				(errcode(ERROR_SYSTEM),
				 errmsg("cannot take a backup"),
				 errdetail("Taking backup from standby server with snapshot-script is not supported")));
		}

		tblspc_list = parray_new();
		tblspcmp_list = parray_new();
		cleanup_list = parray_new();

		/*
		 * append 'pg_tblspc' to list of directory excluded from copy.
		 * because DB cluster and TABLESPACE are copied separately.
		 */
		for (i = 0; pgdata_exclude[i]; i++);	/* find first empty slot */
		pgdata_exclude[i] = PG_TBLSPC_DIR;

		/* set the error processing for the snapshot */
		pgut_atexit_push(snapshot_cleanup, cleanup_list);

		/* create snapshot volume */
		if (!check)
		{
			/* freeze I/O of the file-system */
			execute_freeze();
			/* create the snapshot, and obtain the name of TABLESPACE backup from snapshot */
			execute_split(tblspc_list);
			/* unfreeze I/O of the file-system */
			execute_unfreeze();
		}

		/*
		 * when DB cluster is not contained in the backup from the snapshot,
		 * DB cluster is added to the backup file list from non-snapshot.
		 */
		parray_qsort(tblspc_list, strCompare);
		if (parray_bsearch(tblspc_list, "PG-DATA", strCompare) == NULL)
			add_files(files, pgdata, false, true);
		else
			/* remove the detected tablespace("PG-DATA") from tblspc_list */
			parray_rm(tblspc_list, "PG-DATA", strCompare);

		/*
		 * select the TABLESPACE backup from non-snapshot,
		 * and append TABLESPACE to the list backup from non-snapshot.
		 * TABLESPACE name and oid is obtained by inquiring of the database.
		 */

		Assert(connection != NULL);
		tblspc_res = execute("SELECT spcname, oid FROM pg_tablespace WHERE "
			"spcname NOT IN ('pg_default', 'pg_global') ORDER BY spcname ASC", 0, NULL);
		for (i = 0; i < PQntuples(tblspc_res); i++)
		{
			char *name = PQgetvalue(tblspc_res, i, 0);
			char *oid = PQgetvalue(tblspc_res, i, 1);

			/* when not found, append it to the backup list from non-snapshot */
			if (parray_bsearch(tblspc_list, name, strCompare) == NULL)
			{
				char dir[MAXPGPATH];
				join_path_components(dir, pgdata, PG_TBLSPC_DIR);
				join_path_components(dir, dir, oid);
				add_files(files, dir, true, false);
			}
			else
				/* remove the detected tablespace from tblspc_list */
				parray_rm(tblspc_list, name, strCompare);
		}

		/*
		 * tblspc_list is not empty,
		 * so snapshot-script output the tablespace name that not exist.
		 */
		if (parray_num(tblspc_list) > 0)
			ereport(ERROR,
				(errcode(ERROR_SYSTEM),
				 errmsg("snapshot-script output the name of tablespace that not exist")));

		/* clear array */
		parray_walk(tblspc_list, free);
		parray_free(tblspc_list);

		/* backup files from non-snapshot */
		pgBackupGetPath(&current, path, lengthof(path), DATABASE_DIR);
		backup_files(pgdata, path, files, prev_files, lsn, current.compress_data, NULL);

		/*
		 * Notify end of backup and write backup_label and tablespace_map
		 * files to backup destination directory.
		 */
		stop_backup_files = pg_backup_stop(&current);
		files = parray_concat(stop_backup_files, files);

		/* create file list of non-snapshot objects */
		create_file_list(files, pgdata, NULL, false);

		/* mount snapshot volume to file-system, and obtain that mounted directory */
		if (!check)
			execute_mount(tblspcmp_list);

		/* backup files from snapshot volume */
		for (i = 0; i < parray_num(tblspcmp_list); i++)
		{
			char *spcname;
			char *mp = NULL;
			char *item = (char *) parray_get(tblspcmp_list, i);
			parray *snapshot_files = parray_new();

			/*
			 * obtain the TABLESPACE name and the directory where it is stored.
			 * Note: strtok() replace the delimiter to '\0'. but no problem because
			 *       it doesn't use former value
			 */
			if ((spcname = strtok(item, "=")) == NULL || (mp = strtok(NULL, "\0")) == NULL)
				ereport(ERROR,
					(errcode(ERROR_SYSTEM),
					 errmsg("snapshot-script output illegal format: %s", item)));

			if (verbose)
			{
				printf(_("========================================\n"));
				printf(_("backup files from snapshot: \"%s\"\n"), spcname);
			}

			/* tablespace storage directory not exist */
			if (!dirExists(mp))
				ereport(ERROR,
					(errcode(ERROR_SYSTEM),
					 errmsg("tablespace storage directory doesn't exist: %s", mp)));

			/*
			 * create the previous backup file list to take incremental backup
			 * from the snapshot volume.
			 */
			if (prev_files != NULL)
				prev_files = dir_read_file_list(mp, prev_file_txt);

			/* when DB cluster is backup from snapshot, it backup from the snapshot */
			if (strcmp(spcname, "PG-DATA") == 0)
			{
				/* append DB cluster to backup file list */
				add_files(snapshot_files, mp, false, true);
				/* backup files of DB cluster from snapshot volume */
				backup_files(mp, path, snapshot_files, prev_files, lsn, current.compress_data, NULL);
				/* create file list of snapshot objects (DB cluster) */
				create_file_list(snapshot_files, mp, NULL, true);
				/* remove the detected tablespace("PG-DATA") from tblspcmp_list */
				parray_rm(tblspcmp_list, "PG-DATA", strCompare);
				i--;
			}
			/* backup TABLESPACE from snapshot volume */
			else
			{
				int j;

				/*
				 * obtain the oid from TABLESPACE information acquired by inquiring of database.
				 * and do backup files of TABLESPACE from snapshot volume.
				 */
				for (j = 0; j < PQntuples(tblspc_res); j++)
				{
					char  dest[MAXPGPATH];
					char  prefix[MAXPGPATH];
					char *name = PQgetvalue(tblspc_res, j, 0);
					char *oid = PQgetvalue(tblspc_res, j, 1);

					if (strcmp(spcname, name) == 0)
					{
						/* append TABLESPACE to backup file list */
						add_files(snapshot_files, mp, true, false);

						/* backup files of TABLESPACE from snapshot volume */
						join_path_components(prefix, PG_TBLSPC_DIR, oid);
						join_path_components(dest, path, prefix);
						backup_files(mp, dest, snapshot_files, prev_files, lsn, current.compress_data, prefix);

						/* create file list of snapshot objects (TABLESPACE) */
						create_file_list(snapshot_files, mp, prefix, true);
						/* remove the detected tablespace("PG-DATA") from tblspcmp_list */
						parray_rm(tblspcmp_list, spcname, strCompare);
						i--;
						break;
					}
				}
			}
			parray_concat(files, snapshot_files);
			parray_free(snapshot_files);
		}

		/*
		 * tblspcmp_list is not empty,
		 * so snapshot-script output the tablespace name that not exist.
		 */
		if (parray_num(tblspcmp_list) > 0)
			ereport(ERROR,
				(errcode(ERROR_SYSTEM),
				 errmsg("snapshot-script output the name of tablespace that not exist")));

		/* clear array */
		parray_walk(tblspcmp_list, free);
		parray_free(tblspcmp_list);

		/* snapshot became unnecessary, annul the snapshot */
		if (!check)
		{
			/* unmount directory of mounted snapshot volume */
			execute_umount();
			/* annul the snapshot */
			execute_resync();
		}

		/* unset the error processing for the snapshot */
		pgut_atexit_pop(snapshot_cleanup, cleanup_list);
		/* don't use 'parray_walk'. element of parray not allocate memory by malloc */
		parray_free(cleanup_list);
		PQclear(tblspc_res);
	}

	/* Update various size fields in current. */
	for (i = 0; i < parray_num(files); i++)
	{
		pgFile *file = (pgFile *) parray_get(files, i);
		if (!S_ISREG(file->mode))
			continue;
		current.total_data_bytes += file->size;
		current.read_data_bytes += file->read_size;
		if (file->write_size != BYTES_INVALID)
			current.write_bytes += file->write_size;
	}

	if (verbose)
	{
		printf(_("database backup completed(read: " INT64_FORMAT " write: " INT64_FORMAT ")\n"),
			current.read_data_bytes, current.write_bytes);
		printf(_("========================================\n"));
	}

	return files;
}

/*
 * Connects to the standby server described by command line options,
 * waits for the minimum WAL location required by this backup to be replayed,
 * and finally performs a restartpoint.
 *
 * Returns false if could not connect to the standby server, although that
 * currently never happens, because pgut_connect() errors out anyway.
 */
static bool
execute_restartpoint(pgBackupOption bkupopt, pgBackup *backup)
{
	PGconn *sby_conn = NULL;
	PGresult	*res;
	XLogRecPtr	replayed_lsn;
	int	sleep_time = 1;

	/*
	 * Change connection to standby server, so that any commands executed from
	 * this point on are sent to the standby server.
	 */
	pgut_set_host(bkupopt.standby_host);
	pgut_set_port(bkupopt.standby_port);
	sby_conn = save_connection();

	if (!sby_conn)
	{
		restore_saved_connection();
		return false;
	}

	while (1)
	{
		uint32 xlogid, xrecoff;

		/*
		 * Wait for standby server to replay WAL up to the LSN returned by
		 * pg_backup_start()
		 */
		res = execute("SELECT * FROM pg_last_wal_replay_lsn()", 0, NULL);
		sscanf(PQgetvalue(res, 0, 0), "%X/%X", &xlogid, &xrecoff);
		PQclear(res);

		replayed_lsn = (XLogRecPtr) ((uint64) xlogid << 32) | xrecoff;
		if (replayed_lsn >= backup->start_lsn)
			break;
		sleep(sleep_time);
		/* next sleep_time is increasing by 2 times. */
		/* ex: 1, 2, 4, 8, 16, 32, 60, 60, 60... */
		sleep_time = (sleep_time < 32) ? sleep_time * 2 : 60;
	}

	/* Perform the restartpoint */
	command("CHECKPOINT", 0, NULL);

	/*
	 * Done sending commands to standby, restore our connection to primary.
	 */
	restore_saved_connection();

	return true;
}

/*
 * backup archived WAL incrementally.
 */
static parray *
do_backup_arclog(parray *backup_list)
{
	int			i;
	parray	   *files;
	parray	   *prev_files = NULL;	/* file list of previous database backup */
	FILE	   *fp;
	char		path[MAXPGPATH];
	char		timeline_dir[MAXPGPATH];
	char		prev_file_txt[MAXPGPATH];
	pgBackup   *prev_backup;
	int64		arclog_write_bytes = 0;
	char		last_wal[MAXPGPATH];

	if (!HAVE_ARCLOG(&current) || check)
		return NULL;

	if (verbose)
	{
		printf(_("========================================\n"));
	}
	elog(INFO, _("copying archived WAL files"));

	/* initialize size summary */
	current.read_arclog_bytes = 0;

	/* switch xlog if database is not backed up */
	if (((uint32) current.stop_lsn)  == 0)
		pg_switch_wal(&current);

	/*
	 * To take incremental backup, the file list of the last completed database
	 * backup is needed.
	 */
	prev_backup = catalog_get_last_arclog_backup(backup_list);
	if (prev_backup == NULL)
		elog(DEBUG, "turn to take a full backup of archived WAL files");

	if (prev_backup)
	{
		pgBackupGetPath(prev_backup, prev_file_txt, lengthof(prev_file_txt),
			ARCLOG_FILE_LIST);
		prev_files = dir_read_file_list(arclog_path, prev_file_txt);
	}

	/* list files with the logical path. omit ARCLOG_PATH */
	files = parray_new();
	dir_list_file(files, arclog_path, NULL, true, false);

	/* remove WALs archived after pg_backup_stop()/pg_switch_wal() */
	xlog_fname(last_wal, lengthof(last_wal), current.tli, &current.stop_lsn,
			   wal_segment_size);
	for (i = 0; i < parray_num(files); i++)
	{
		pgFile *file = (pgFile *) parray_get(files, i);
		char *fname;
		if ((fname = last_dir_separator(file->path)))
			fname++;
		else
			fname = file->path;

		/* to backup backup history files, compare tli/lsn portion only */
		if (strncmp(fname, last_wal, 24) > 0)
		{
			parray_remove(files, i);
			i--;
		}
	}

	elog(DEBUG, "taking backup of archived WAL files");
	pgBackupGetPath(&current, path, lengthof(path), ARCLOG_DIR);
	backup_files(arclog_path, path, files, prev_files, NULL,
				 current.compress_data, NULL);

	/* create file list */
	if (!check)
	{
		pgBackupGetPath(&current, path, lengthof(path), ARCLOG_FILE_LIST);
		fp = fopen(path, "wt");
		if (fp == NULL)
			ereport(ERROR,
				(errcode(ERROR_SYSTEM),
				 errmsg("could not open file list \"%s\": %s", path,
					strerror(errno))));
		dir_print_file_list(fp, files, arclog_path, NULL);
		fclose(fp);
	}

	/* print summary of size of backup files */
	for (i = 0; i < parray_num(files); i++)
	{
		pgFile *file = (pgFile *) parray_get(files, i);
		if (!S_ISREG(file->mode))
			continue;
		current.read_arclog_bytes += file->read_size;
		if (file->write_size != BYTES_INVALID)
		{
			current.write_bytes += file->write_size;
			arclog_write_bytes += file->write_size;
		}
	}

	/*
	 * Backup timeline history files to special directory.
	 * We do this after create file list, because copy_file() update
	 * pgFile->write_size to actual size.
	 */
	join_path_components(timeline_dir, backup_path, TIMELINE_HISTORY_DIR);
	for (i = 0; i < parray_num(files); i++)
	{
		pgFile *file = (pgFile *) parray_get(files, i);
		if (!S_ISREG(file->mode))
			continue;
		if (strstr(file->path, ".history") ==
				file->path + strlen(file->path) - strlen(".history"))
		{
			elog(DEBUG, _("(timeline history) %s"), file->path);
			copy_file(arclog_path, timeline_dir, file, NO_COMPRESSION);
		}
	}

	if (verbose)
	{
		printf(_("archived WAL backup completed(read: " INT64_FORMAT " write: " INT64_FORMAT ")\n"),
			current.read_arclog_bytes, arclog_write_bytes);
		printf(_("========================================\n"));
	}

	return files;
}

/*
 * Take a backup of serverlog.
 */
static parray *
do_backup_srvlog(parray *backup_list)
{
	int			i;
	parray	   *files;
	parray	   *prev_files = NULL;	/* file list of previous database backup */
	FILE	   *fp;
	char		path[MAXPGPATH];
	char		prev_file_txt[MAXPGPATH];
	pgBackup   *prev_backup;
	int64		srvlog_write_bytes = 0;

	if (!current.with_serverlog)
		return NULL;

	if (verbose)
	{
		printf(_("========================================\n"));
	}
	elog(INFO, _("copying server log files"));

	/* initialize size summary */
	current.read_srvlog_bytes = 0;

	/*
	 * To take incremental backup, the file list of the last completed database
	 * backup is needed.
	 */
	prev_backup = catalog_get_last_srvlog_backup(backup_list);
	if (prev_backup == NULL)
		elog(DEBUG, "turn to take a full backup of server log files");

	if (prev_backup)
	{
		pgBackupGetPath(prev_backup, prev_file_txt, lengthof(prev_file_txt),
			SRVLOG_FILE_LIST);
		prev_files = dir_read_file_list(srvlog_path, prev_file_txt);
	}

	/* list files with the logical path. omit SRVLOG_PATH */
	files = parray_new();
	dir_list_file(files, srvlog_path, NULL, true, false);

	pgBackupGetPath(&current, path, lengthof(path), SRVLOG_DIR);
	backup_files(srvlog_path, path, files, prev_files, NULL, false, NULL);

	/* create file list */
	if (!check)
	{
		pgBackupGetPath(&current, path, lengthof(path), SRVLOG_FILE_LIST);
		fp = fopen(path, "wt");
		if (fp == NULL)
			ereport(ERROR,
				(errcode(ERROR_SYSTEM),
				 errmsg("could not open file list \"%s\": %s", path,
					strerror(errno))));
		dir_print_file_list(fp, files, srvlog_path, NULL);
		fclose(fp);
	}

	/* print summary of size of backup mode files */
	for (i = 0; i < parray_num(files); i++)
	{
		pgFile *file = (pgFile *) parray_get(files, i);
		if (!S_ISREG(file->mode))
			continue;
		current.read_srvlog_bytes += file->read_size;
		if (file->write_size != BYTES_INVALID)
		{
			current.write_bytes += file->write_size;
			srvlog_write_bytes += file->write_size;
		}
	}

	if (verbose)
	{
		printf(_("serverlog backup completed(read: " INT64_FORMAT " write: " INT64_FORMAT ")\n"),
			current.read_srvlog_bytes, srvlog_write_bytes);
		printf(_("========================================\n"));
	}

	return files;
}

int
do_backup(pgBackupOption bkupopt)
{
	parray *backup_list;
	parray *files_database;
	parray *files_arclog;
	parray *files_srvlog;
	int    ret;
	char   path[MAXPGPATH];

	/* repack the necessary options */
	int	keep_arclog_files = bkupopt.keep_arclog_files;
	int	keep_arclog_days  = bkupopt.keep_arclog_days;
	int	keep_srvlog_files = bkupopt.keep_srvlog_files;
	int	keep_srvlog_days  = bkupopt.keep_srvlog_days;
	int	keep_data_generations = bkupopt.keep_data_generations;
	int	keep_data_days        = bkupopt.keep_data_days;

	ControlFileData *controlFile;
	bool	crc_ok;

	/* PGDATA and BACKUP_MODE are always required */
	if (pgdata == NULL)
		ereport(ERROR,
			(errcode(ERROR_ARGS),
			 errmsg("required parameter not specified: PGDATA (-D, --pgdata)")));

	if (current.backup_mode == BACKUP_MODE_INVALID)
		ereport(ERROR,
			(errcode(ERROR_ARGS),
			 errmsg("required parameter not specified: BACKUP_MODE (-b, --backup-mode)")));

	/* ARCLOG_PATH is required only when backup archive WAL */
	if (HAVE_ARCLOG(&current) && arclog_path == NULL)
		ereport(ERROR,
			(errcode(ERROR_ARGS),
			 errmsg("required parameter not specified: ARCLOG_PATH (-A, --arclog-path)")));

	/* SRVLOG_PATH is required only when backup serverlog */
	if (current.with_serverlog && srvlog_path == NULL)
		ereport(ERROR,
			(errcode(ERROR_ARGS),
			 errmsg("required parameter not specified: SRVLOG_PATH (-S, --srvlog-path)")));
	/*
	 * If we are taking backup from standby
	 * (ie, $PGDATA has recovery.conf or standby.signal),
	 * check required parameters (ie, standby connection info).
	 */
	if (get_standby_signal_filepath(path, sizeof(path)))
	{
		if (!bkupopt.standby_host || !bkupopt.standby_port)
			ereport(ERROR,
				(errcode(ERROR_SYSTEM),
				 errmsg("please specify both standby host and port")));

		current.is_from_standby = true;
	}
	else
		current.is_from_standby = false;

#ifndef HAVE_LIBZ
	if (current.compress_data)
	{
		ereport(WARNING,
			(errmsg("this pg_rman build does not support compression"),
			 errhint("Please build PostgreSQL with zlib to use compression.")));
		current.compress_data = false;
	}
#endif

	controlFile = get_controlfile(pgdata, &crc_ok);

	if (!crc_ok)
		ereport(WARNING,
				(errmsg("control file appears to be corrupt"),
				 errdetail("Calculated CRC checksum does not match value stored in file.")));
	wal_segment_size = controlFile->xlog_seg_size;
	pg_free(controlFile);

	/* Check that we're working with the correct database cluster */
	check_system_identifier();

	/* show configuration actually used */
	if (verbose)
	{
		printf(_("========================================\n"));
		printf(_("backup start\n"));
		printf(_("----------------------------------------\n"));
		pgBackupWriteConfigSection(stderr, &current);
		printf(_("----------------------------------------\n"));
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
			 errdetail("Another pg_rman is just running. Skip this backup.")));

	/* initialize backup result */
	current.status = BACKUP_STATUS_RUNNING;
	current.tli = 0;		/* get from result of pg_backup_start() */
	current.start_lsn = current.stop_lsn = (XLogRecPtr) 0;
	current.start_time = time(NULL);
	current.end_time = (time_t) 0;
	current.total_data_bytes = BYTES_INVALID;
	current.read_data_bytes = BYTES_INVALID;
	current.read_arclog_bytes = BYTES_INVALID;
	current.read_srvlog_bytes = BYTES_INVALID;
	current.write_bytes = 0;		/* write_bytes is valid always */
	current.block_size = BLCKSZ;
	current.wal_block_size = XLOG_BLCKSZ;
	current.recovery_xid = 0;
	current.recovery_time = (time_t) 0;

	/* create backup directory and backup.ini */
	if (!check)
	{
		if (pgBackupCreateDir(&current))
			ereport(ERROR,
				(errcode(ERROR_SYSTEM),
				 errmsg("could not create backup directory")));
		pgBackupWriteIni(&current);
	}

	elog(DEBUG, "destination directories of backup are initialized");

	/* get list of backups already taken */
	backup_list = catalog_get_backup_list(NULL);
	if(!backup_list)
		ereport(ERROR,
			(errcode(ERROR_SYSTEM),
			 errmsg("could not get list of backup already taken")));

	/* set the error processing function for the backup process */
	pgut_atexit_push(backup_cleanup, NULL);

	/*
	 * Signal for backup_cleanup() that there may actually be some cleanup
	 * for it to do from this point on.
	 */
	in_backup = true;

	/* backup data */
	files_database = do_backup_database(backup_list, bkupopt);

	/* backup archived WAL */
	files_arclog = do_backup_arclog(backup_list);

	/* backup serverlog */
	files_srvlog = do_backup_srvlog(backup_list);

	pgut_atexit_pop(backup_cleanup, NULL);

	/* update backup status to DONE */
	current.end_time = time(NULL);
	current.status = BACKUP_STATUS_DONE;
	if (!check)
		pgBackupWriteIni(&current);

	if (verbose)
	{
		if (TOTAL_READ_SIZE(&current) == 0)
			printf(_("nothing to backup\n"));
		else
			printf(_("all backup completed(read: " INT64_FORMAT " write: "
				INT64_FORMAT ")\n"),
				TOTAL_READ_SIZE(&current), current.write_bytes);
		printf(_("========================================\n"));
	}

	ereport(INFO,
			(errmsg("backup complete")));
	ereport(INFO,
			(errmsg("Please execute 'pg_rman validate' to verify the files are correctly copied.")));

	/*
	 * Delete old files (archived WAL and serverlog) after update of status.
	 */
	if (HAVE_ARCLOG(&current))
		delete_old_files(arclog_path, files_arclog, keep_arclog_files,
			keep_arclog_days, true);
	if (current.with_serverlog)
		delete_old_files(srvlog_path, files_srvlog, keep_srvlog_files,
			keep_srvlog_days, false);

	/* Delete old backup files after all backup operation. */
	pgBackupDelete(keep_data_generations, keep_data_days);

	/* Cleanup backup mode file list */
	if (files_database)
		parray_walk(files_database, pgFileFree);
	parray_free(files_database);
	if (files_arclog)
		parray_walk(files_arclog, pgFileFree);
	parray_free(files_arclog);
	if (files_srvlog)
		parray_walk(files_srvlog, pgFileFree);
	parray_free(files_srvlog);

	/*
	 * If this backup is full backup, delete backup of online WAL.
	 * Note that serverlog files which were backed up during first restoration
	 * don't be delete.
	 * Also delete symbolic link in the archive directory.
	 */
	if (current.backup_mode == BACKUP_MODE_FULL)
	{
		delete_online_wal_backup();
		delete_arclog_link();
	}

	/* release catalog lock */
	catalog_unlock();

	return 0;
}

/*
 * get server version and confirm block sizes.
 */
static void
check_server_version(void)
{
	int		server_version;

	if (connection == NULL)
		reconnect();

	/* confirm server version */
	elog(DEBUG, "checking PostgreSQL server version");

	server_version = PQserverVersion(connection);
	if (server_version < 80400)
		ereport(ERROR,
			(errcode(ERROR_PG_INCOMPATIBLE),
			 errmsg("server version is %d.%d.%d, but must be 8.4 or higher",
				server_version / 10000,
				(server_version / 100) % 100,
				server_version % 100)));

	elog(DEBUG, "server version is %d.%d.%d",
				server_version / 10000,
				(server_version / 100) % 100,
				server_version % 100);

	/* confirm block_size (BLCKSZ) and wal_block_size (XLOG_BLCKSZ) */
	confirm_block_size("block_size", BLCKSZ);
	confirm_block_size("wal_block_size", XLOG_BLCKSZ);

	disconnect();
}

static void
confirm_block_size(const char *name, int blcksz)
{
	PGresult   *res;
	char	   *endp;
	int			block_size;

	elog(DEBUG, "checking block size setting");
	res = execute("SELECT current_setting($1)", 1, &name);
	if (PQntuples(res) != 1 || PQnfields(res) != 1)
		ereport(ERROR,
			(errcode(ERROR_PG_COMMAND),
			 errmsg("could not get %s: %s", name, PQerrorMessage(connection))));
	block_size = strtol(PQgetvalue(res, 0, 0), &endp, 10);
	if (strcmp(name, "block_size") == 0)
		elog(DEBUG, "block size is %d", block_size);
	else if (strcmp(name, "wal_block_size") == 0)
		elog(DEBUG, "wal block size is %d", block_size);

	if ((endp && *endp) || block_size != blcksz)
		ereport(ERROR,
			(errcode(ERROR_PG_INCOMPATIBLE),
			 errmsg("%s(%d) is not compatible(%d expected)", name, block_size, blcksz)));
	PQclear(res);
}

/*
 * Notify start of backup to PostgreSQL server.
 *
 * As of now, this always contacts a primary PostgreSQL server, even in the
 * case of taking a backup from standby.
 */
static void
pg_backup_start(const char *label, bool smooth, pgBackup *backup)
{
	PGresult	   *res;
	const char	   *params[2];
	params[0] = label;

	elog(DEBUG, "executing pg_backup_start()");

	/*
	 * Establish new connection to send backup control commands.  The same
	 * connection is used until the current backup finishes which is required
	 * with the new non-exclusive backup API as of PG version 9.6.
	 */
	reconnect();

	/* 2nd argument is 'fast' (IOW, !smooth) */
	params[1] = smooth ? "false" : "true";

	/* non-exclusive' mode (assumes PG version >= 15) */
	res = execute("SELECT * from pg_walfile_name_offset(pg_backup_start($1, $2))", 2, params);

	if (backup != NULL)
		get_lsn(res, &backup->tli, &backup->start_lsn);

	elog(DEBUG, "backup start point is (WAL file: %s, xrecoff: %s)",
			PQgetvalue(res, 0, 0), PQgetvalue(res, 0, 1));

	PQclear(res);
}

/*
 * Constructs the WAL segment file name using backup->stop_lsn and waits
 * for it to be successfully archived.  The latter is achieved by waiting
 * for <wal_segment_filename>.ready to appear under
 * $PGDATA/pg_wal/archive_status.
 *
 * Note that PGDATA could either refer to primary servers' data directory
 * or standby's.  In the latter case, the waiting will continue until
 * the required WAL segment is fully streamed to the standby server and
 * then archived by its archiver process.
 */
static void
wait_for_archive(pgBackup *backup, const char *sql, int nParams,
				 const char **params)
{
	PGresult	   *res;
	char			done_path[MAXPGPATH];
	int				try_count;

	Assert(connection != NULL);

	res = execute(sql, nParams, params);
	if (backup != NULL)
	{
		get_lsn(res, &backup->tli, &backup->stop_lsn);
		elog(DEBUG, "backup end point is (WAL file: %s, xrecoff: %s)",
				PQgetvalue(res, 0, 0), PQgetvalue(res, 0, 1));
	}

	/* get filename from the result of pg_walfile_name_offset() */
	elog(DEBUG, "waiting for %s is archived", PQgetvalue(res, 0, 0));
	snprintf(done_path, lengthof(done_path),
		"%s/pg_wal/archive_status/%s.done", pgdata, PQgetvalue(res, 0, 0));

	PQclear(res);

	res = execute(TXID_CURRENT_SQL, 0, NULL);
	if(backup != NULL)
	{
		get_xid(res, &backup->recovery_xid);
		backup->recovery_time = time(NULL);
	}

	/* wait until switched WAL is archived */
	try_count = 0;
	while (!fileExists(done_path))
	{
		sleep(1);
		if (interrupted)
			ereport(FATAL,
				(errcode(ERROR_INTERRUPTED),
				 errmsg("interrupted during waiting for WAL archiving")));
		try_count++;
		if (try_count > TIMEOUT_ARCHIVE)
			ereport(ERROR,
				(errcode(ERROR_ARCHIVE_FAILED),
				 errmsg("switched WAL could not be archived in %d seconds",
					TIMEOUT_ARCHIVE)));
	}

	elog(DEBUG, "WAL file containing backup end point is archived after waiting for %d seconds",
			try_count);
}

/*
 * Notify the end of backup to PostgreSQL server.
 *
 * Contacts the primary server and issues pg_backup_stop(), then waits for
 * either the primary or standby server to successfully archive the last
 * needed WAL segment to be archived.  Returns once that's been done.
 *
 * pg_backup_stop() returns 2 more fields in addition
 * to the backup end LSN: backup_label text and tablespace_map text which
 * need to be written to files in the backup root directory.
 *
 * Returns an array of pgFile structs of files written so that caller can add
 * it to the backup file list.
 */
static parray *
pg_backup_stop(pgBackup *backup)
{
	parray		   *result = parray_new();
	pgFile		   *file;
	PGresult	   *res;
	char		   *backup_lsn;
	char		   *backuplabel = NULL;
	int				backuplabel_len;
	char		   *tblspcmap = NULL;
	int				tblspcmap_len;
	const char	   *params[1];

	elog(DEBUG, "executing pg_backup_stop()");

	/*
	 * Non-exclusive backup requires to use same connection as the one
	 * used to issue pg_backup_start().  Remember we did not disconnect
	 * in pg_backup_start() nor did we lose our connection when issuing
	 * commands to standby.
	 */
	Assert(connection != NULL);

	/* Remove annoying NOTICE messages generated by backend */
	res = execute("SET client_min_messages = warning;", 0, NULL);
	PQclear(res);

	/* wait for WAL files to be archived */
	params[0] = "true";
	res = execute("SELECT * FROM pg_backup_stop($1)", 1, params);

	if (res == NULL || PQntuples(res) != 1 || PQnfields(res) != 3)
		ereport(ERROR,
			(errcode(ERROR_PG_COMMAND),
			 errmsg("result of pg_backup_stop($1) is invalid: %s",
				PQerrorMessage(connection))));

	backup_lsn = PQgetvalue(res, 0, 0);
	backuplabel = PQgetvalue(res, 0, 1);
	backuplabel_len = PQgetlength(res, 0, 1);
	tblspcmap = PQgetvalue(res, 0, 2);
	tblspcmap_len = PQgetlength(res, 0, 2);

	Assert(backuplabel_len > 0);
	file = write_stop_backup_file(backup, backuplabel, backuplabel_len,
								  PG_BACKUP_LABEL_FILE);
	parray_append(result, (void *) file);

	if (tblspcmap_len > 0)
	{
		file = write_stop_backup_file(backup, tblspcmap, tblspcmap_len,
									  PG_TBLSPC_MAP_FILE);
		parray_append(result, (void *) file);
	}

	params[0] = backup_lsn;
	wait_for_archive(backup,
		"SELECT * FROM pg_walfile_name_offset($1)",
		1, params);

	/* Done with the connection. */
	disconnect();

	return result;
}

/*
 * Force switch to a new transaction log file and update backup->tli.
 */
static void
pg_switch_wal(pgBackup *backup)
{
	reconnect();

	wait_for_archive(backup,
		"SELECT * FROM pg_walfile_name_offset(pg_switch_wal())",
		0, NULL);

	disconnect();
}

/*
 * Get TimeLineID and LSN from result of pg_walfile_name_offset().
 */
static void
get_lsn(PGresult *res, TimeLineID *timeline, XLogRecPtr *lsn)
{
	uint32 off_upper;
	uint32 xlogid, xrecoff = 0;

	if (res == NULL || PQntuples(res) != 1 || PQnfields(res) != 2)
		ereport(ERROR,
			(errcode(ERROR_PG_COMMAND),
			 errmsg("result of pg_walfile_name_offset() is invalid: %s",
				PQerrorMessage(connection))));

	/* get TimeLineID, LSN from result of pg_backup_stop() */
	if (sscanf(PQgetvalue(res, 0, 0), "%08X%08X%08X",
			timeline, &xlogid, &off_upper) != 3 ||
		sscanf(PQgetvalue(res, 0, 1), "%u", &xrecoff) != 1)
	{
		ereport(ERROR,
			(errcode(ERROR_PG_COMMAND),
			 errmsg("result of pg_walfile_name_offset() is invalid: %s",
				PQerrorMessage(connection))));
	}

	Assert(wal_segment_size > 0);
	xrecoff += off_upper << ((uint32) log2(wal_segment_size));

	*lsn = (XLogRecPtr) ((uint64) xlogid << 32) | xrecoff;
	return;
}

/*
 * Get XID from result of txid_current() after pg_backup_stop().
 */
static void
get_xid(PGresult *res, uint32 *xid)
{
	if(res == NULL || PQntuples(res) != 1 || PQnfields(res) != 1)
		ereport(ERROR,
			(errcode(ERROR_PG_COMMAND),
			 errmsg("result of txid_current() is invalid: %s",
				PQerrorMessage(connection))));

	if(sscanf(PQgetvalue(res, 0, 0), "%u", xid) != 1)
	{
		ereport(ERROR,
			(errcode(ERROR_PG_COMMAND),
			 errmsg("result of txid_current() is invalid: %s",
				PQerrorMessage(connection))));
	}
	elog(DEBUG, "current XID is %s", PQgetvalue(res, 0, 0));
}

/*
 * Return true if the path is a existing directory.
 */
static bool
dirExists(const char *path)
{
	struct stat buf;

	if (stat(path, &buf) == -1 && errno == ENOENT)
		return false;
	else if (S_ISREG(buf.st_mode))
		return false;
	else
		return true;
}

/*
 * Notify end of backup to server when "backup_label" is in the root directory
 * of the DB cluster.
 * Also update backup status to ERROR when the backup is not finished.
 */
static void
backup_cleanup(bool fatal, void *userdata)
{
	if (!in_backup)
		return;

	/* Disconnecting automatically aborts a non-exclusive backup */
	disconnect();

	/*
	 * Update status of backup.ini to ERROR.
	 * end_time != 0 means backup finished
	 */
	if (current.status == BACKUP_STATUS_RUNNING && current.end_time == 0)
	{
		elog(DEBUG, "update backup status from RUNNING to ERROR");
		current.end_time = time(NULL);
		current.status = BACKUP_STATUS_ERROR;
		pgBackupWriteIni(&current);
	}
}

/* take backup about listed file. */
static void
backup_files(const char *from_root,
			 const char *to_root,
			 parray *files,
			 parray *prev_files,
			 const XLogRecPtr *lsn,
			 bool compress,
			 const char *prefix)
{
	int				i;
	int				num_skipped = 0;
	struct timeval	tv;
	bool			prev_file_not_found = false;

	/* sort pathname ascending */
	parray_qsort(files, pgFileComparePath);

	gettimeofday(&tv, NULL);

	/* backup a file or create a directory */
	for (i = 0; i < parray_num(files); i++)
	{
		int			ret;
		struct stat	buf;

		pgFile *file = (pgFile *) parray_get(files, i);

		/* If current time is rewinded, abort this backup. */
		if(tv.tv_sec < file->mtime)
			ereport(FATAL,
				(errcode(ERROR_SYSTEM),
				 errmsg("cannot take a backup"),
				 errdetail("There is a file with future timestamp from system time.\n"
						"Current system time may be rewound."),
				 errhint("The file is %s.\n"
						"If this is a database file, please retry with the full backup mode.\n"
						"If this is a server log or archived WAL file, change the timestamp.",
									file->path)));

		/* check for interrupt */
		if (interrupted)
			ereport(FATAL,
				(errcode(ERROR_INTERRUPTED),
				 errmsg("interrupted during backup")));

		/* For correct operation of incremental backup, 
		 * initialize prev_file_not_found variable to false of 
		 * checking to next backup files.
		 */
		prev_file_not_found = false;
		
		/* print progress in verbose mode */
		if (verbose)
		{
			if (prefix)
			{
				char path[MAXPGPATH];
				join_path_components(path, prefix, file->path + strlen(from_root) + 1);
				printf(_("(%d/%lu) %s "), i + 1, (unsigned long) parray_num(files), path);
			}
			else
				printf(_("(%d/%lu) %s "), i + 1, (unsigned long) parray_num(files),
					file->path + strlen(from_root) + 1);
		}

		/* stat file to get file type, size and modify timestamp */
		ret = stat(file->path, &buf);
		if (ret == -1)
		{
			if (errno == ENOENT)
			{
				/* record as skipped file in file_xxx.txt */
				file->write_size = BYTES_INVALID;
				num_skipped++;
				if (verbose)
					printf(_("skip\n"));
				goto show_progress;
			}
			else
			{
				if (verbose)
					printf("\n");
				ereport(ERROR,
					(errcode(ERROR_SYSTEM),
					 errmsg("could not stat \"%s\": %s",
						file->path, strerror(errno))));
			}
		}

		/* if the entry was a directory, create it in the backup */
		if (S_ISDIR(buf.st_mode))
		{
			char dirpath[MAXPGPATH];

			join_path_components(dirpath, to_root, JoinPathEnd(file->path, from_root));

			if (!check)
				dir_create_dir(dirpath, DIR_PERMISSION);

			if (verbose)
				printf(_("directory\n"));
		}
		else if (S_ISREG(buf.st_mode))
		{
			/* skip files which have not been modified since last backup */
			if (prev_files)
			{
				pgFile *prev_file = NULL;

				/*
				 * If prefix is not NULL, the table space is backup from the snapshot.
				 * Therefore, adjust file name to correspond to the file list.
				 */
				if (prefix)
				{
					int j;

					for (j = 0; j < parray_num(prev_files); j++)
					{
						pgFile *p = (pgFile *) parray_get(prev_files, j);
						char *prev_path;
						char curr_path[MAXPGPATH];

						prev_path = p->path + strlen(from_root) + 1;
						join_path_components(curr_path, prefix, file->path + strlen(from_root) + 1);
						if (strcmp(curr_path, prev_path) == 0)
						{
							prev_file = p;
							break;
						}
					}
				}
				else
				{
					pgFile **p = (pgFile **) parray_bsearch(prev_files, file, pgFileComparePath);
					if (p)
						prev_file = *p;
				}

				if (prev_file)
				{
					if(prev_file->mtime == file->mtime)
					{
						/* record as skipped file in file_xxx.txt */
						file->write_size = BYTES_INVALID;
						num_skipped++;
						if (verbose)
							printf(_("skip\n"));
						goto show_progress;
					}
				}
				else
					prev_file_not_found = true;
			}

			/*
			 * We will wait until the next second of mtime so that backup
			 * file should contain all modifications at the clock of mtime.
			 * timer resolution of ext3 file system is one second.
			 */

			if (tv.tv_sec == file->mtime)
			{
				/* update time and recheck */
				gettimeofday(&tv, NULL);
				while (tv.tv_sec <= file->mtime)
				{
					usleep(1000000 - tv.tv_usec);
					gettimeofday(&tv, NULL);
				}
			}

			/* copy the file into backup */
			if (!(file->is_datafile
					? backup_data_file(from_root, to_root, file, lsn, compress, prev_file_not_found)
					: copy_file(from_root, to_root, file,
								compress ? COMPRESSION : NO_COMPRESSION)))
			{
				/* record as skipped file in file_xxx.txt */
				file->write_size = BYTES_INVALID;
				num_skipped++;
				if (verbose)
					printf(_("skip\n"));
				goto show_progress;
			}

			if (verbose)
			{
				/* print compression rate */
				if (file->write_size != file->size)
					printf(_("compressed %lu (%.2f%% of %lu)\n"),
						(unsigned long) file->write_size,
						100.0 * file->write_size / file->size,
						(unsigned long) file->size);
				else
					printf(_("copied %lu\n"), (unsigned long) file->write_size);

				continue;
			}

show_progress:
			/* print progress in non-verbose format */
			if (progress)
			{
				fprintf(stderr, _("Processed %d of %lu files, skipped %d"),
						i + 1, (unsigned long) parray_num(files), num_skipped);

				if (i + 1 < (unsigned long) parray_num(files))
					fprintf(stderr, "\r");
				else
					fprintf(stderr, "\n");
			}

		}
		else
		{
			if (verbose)
				printf(_(" unexpected file type %d\n"), buf.st_mode);
		}
	}
}

/*
 * Delete server log and archived WAL files through KEEP_xxx_DAYS
 * or more than KEEP_xxx_FILES.
 */
static void
delete_old_files(const char *root,
				 parray *files,
				 int keep_files,
				 int keep_days,
				 bool is_arclog)
{
	int		i;
	int		j;
	int		file_num = 0;
	char	*target_file;
	char	*target_path;
	time_t	tim;
	time_t	days_threshold = 0;
	struct	tm *ltm;
	char 	files_str[100];
	char 	days_str[100];
	char	days_threshold_timestamp[20];


	if (files == NULL)
		return;

	target_file = is_arclog ? "archived WAL" : "server";
	target_path = is_arclog ? "ARCLOG_PATH" : "SRVLOG_PATH";

	if (keep_files == KEEP_INFINITE)
		strncpy(files_str, "INFINITE", lengthof(files_str));
	else
		snprintf(files_str, lengthof(files_str), "%d", keep_files);

	if (keep_days == KEEP_INFINITE)
		strncpy(days_str, "INFINITE", lengthof(days_str));
	else
		snprintf(days_str, lengthof(days_str), "%d", keep_days);

	/* delete files through the given conditions */
	if (keep_files != KEEP_INFINITE && keep_days != KEEP_INFINITE)
		elog(INFO, "start deleting old %s files from %s (keep files = %s, keep days = %s)",
			target_file, target_path, files_str, days_str);
	else if (keep_files != KEEP_INFINITE)
		elog(INFO, "start deleting old %s files from %s (keep files = %s)",
			target_file, target_path, files_str);
	else if (keep_days != KEEP_INFINITE)
		elog(INFO, "start deleting old %s files from %s (keep days = %s)",
			target_file, target_path, days_str);
	else
	{
		elog(DEBUG, "do not delete old %s files", target_file);
		return;
	}

	/* calculate the threshold day from given keep_days. */
	if ( keep_days != KEEP_INFINITE)
	{
		tim = current.start_time - (keep_days * 60 * 60 * 24);
		ltm = localtime(&tim);
		ltm->tm_hour = 0;
		ltm->tm_min  = 0;
		ltm->tm_sec  = 0;
		days_threshold = mktime(ltm);
		time2iso(days_threshold_timestamp, lengthof(days_threshold_timestamp), days_threshold);
		elog(INFO, "the threshold timestamp calculated by keep days is \"%s\"", days_threshold_timestamp);
	}

	parray_qsort(files, pgFileCompareMtime);
	for (i = parray_num(files) - 1; i >= 0; i--)
	{
		pgFile *file = (pgFile *) parray_get(files, i);

		elog(DEBUG, "checking \"%s\"", file->path);
		/* Delete completed WALs only. */
		Assert(wal_segment_size > 0);
		if (is_arclog && !xlog_is_complete_wal(file, wal_segment_size))
		{
			elog(DEBUG, "this is not complete WAL: \"%s\"", file->path);
			continue;
		}

		file_num++;

		/*
		 * If the mtime of the file is older than the threshold,
		 * or there are enough number of files newer than the files,
		 * delete the file.
		 */
		if (keep_files != KEEP_INFINITE)
		{
			if (file_num <= keep_files)
			{
				ereport(DEBUG,
					(errmsg("keep the file : \"%s\"", file->path),
					 errdetail("This is the %d%s latest file.",
						file_num, getCountSuffix(file_num))));
				continue;
			}
		}
		if (keep_days != KEEP_INFINITE)
		{
			if (file->mtime >= days_threshold)
			{
				ereport(DEBUG,
					(errmsg("keep the file : \"%s\"", file->path),
					 errdetail("This is newer than the threshold \"%s\".", days_threshold_timestamp)));
				continue;
			}
		}

		/* Now we found a file should be deleted. */
		elog(INFO, ("delete \"%s\""), file->path + strlen(root) + 1);

		file = (pgFile *) parray_remove(files, i);
		/* delete corresponding backup history file if exists */
		for (j = parray_num(files) - 1; j >= 0; j--)
		{
			pgFile *file2 = (pgFile *)parray_get(files, j);
			if (file->path[0] != '\0' && file2->path[0] != '\0' &&
				strstr(file2->path, file->path) == file2->path)
			{
				file2 = (pgFile *)parray_remove(files, j);
				elog(INFO, "delete \"%s\"",
						file2->path + strlen(root) + 1);
				if (!check)
					pgFileDelete(file2);
				pgFileFree(file2);
			}
		}
		if (!check)
			pgFileDelete(file);
		pgFileFree(file);
	}
}

static void
delete_online_wal_backup(void)
{
	int i;
	parray *files = parray_new();
	char work_path[MAXPGPATH];

	if (verbose)
	{
		printf(_("========================================\n"));
		printf(_("delete online WAL backup\n"));
	}

	snprintf(work_path, lengthof(work_path), "%s/%s/%s", backup_path,
		RESTORE_WORK_DIR, PG_XLOG_DIR);
	/* don't delete root dir */
	dir_list_file(files, work_path, NULL, true, false);
	if (parray_num(files) == 0)
	{
		parray_free(files);
		return;
	}

	parray_qsort(files, pgFileComparePathDesc);	/* delete from leaf */
	for (i = 0; i < parray_num(files); i++)
	{
		pgFile *file = (pgFile *) parray_get(files, i);
		if (verbose)
			printf(_("delete \"%s\"\n"), file->path);
		if (!check)
			pgFileDelete(file);
	}

	parray_walk(files, pgFileFree);
	parray_free(files);
}

/*
 * Remove symbolic links point archived WAL in backup catalog.
 */
static void
delete_arclog_link(void)
{
	int i;
	parray *files = parray_new();

	if (verbose)
	{
		printf(_("========================================\n"));
		printf(_("delete symbolic link in archive directory\n"));
	}

	dir_list_file(files, arclog_path, NULL, false, false);
	for (i = 0; i < parray_num(files); i++)
	{
		pgFile *file = (pgFile *) parray_get(files, i);

		if (!S_ISLNK(file->mode))
			continue;

		if (verbose)
			printf(_("delete \"%s\"\n"), file->path);

		if (!check && remove(file->path) == -1)
			ereport(ERROR,
				(errcode(ERROR_SYSTEM),
				 errmsg("could not remove link \"%s\": %s", file->path,
					strerror(errno))));
	}

	parray_walk(files, pgFileFree);
	parray_free(files);
}

/*
 * Execute the command 'freeze' of snapshot-script.
 * When the command ends normally, 'unfreeze' is added to the cleanup list.
 */
static void
execute_freeze(void)
{
	/* append 'unfreeze' command to cleanup list */
	parray_append(cleanup_list, SNAPSHOT_UNFREEZE);

	/* execute 'freeze' command */
	execute_script(SNAPSHOT_FREEZE, false, NULL);
}

/*
 * Execute the command 'unfreeze' of snapshot-script.
 * Remove 'unfreeze' from the cleanup list before executing the command
 * when 'unfreeze' is included in the cleanup list.
 */
static void
execute_unfreeze(void)
{
	int	i;

	/* remove 'unfreeze' command from cleanup list */
	for (i = 0; i < parray_num(cleanup_list); i++)
	{
		char	*mode;

		mode = (char *) parray_get(cleanup_list, i);
		if (strcmp(mode,SNAPSHOT_UNFREEZE) == 0)
		{
			parray_remove(cleanup_list, i);
			break;
		}
	}
	/* execute 'unfreeze' command */
	execute_script(SNAPSHOT_UNFREEZE, false, NULL);
}

/*
 * Execute the command 'split' of snapshot-script.
 * When the command ends normally, 'resync' is added to the cleanup list.
 */
static void
execute_split(parray *tblspc_list)
{
	/* append 'resync' command to cleanup list */
	parray_append(cleanup_list, SNAPSHOT_RESYNC);

	/* execute 'split' command */
	execute_script(SNAPSHOT_SPLIT, false, tblspc_list);
}

/*
 * Execute the command 'resync' of snapshot-script.
 * Remove 'resync' from the cleanup list before executing the command
 * when 'resync' is included in the cleanup list.
 */
static void
execute_resync(void)
{
	int	i;

	/* remove 'resync' command from cleanup list */
	for (i = 0; i < parray_num(cleanup_list); i++)
	{
		char *mode;

		mode = (char *) parray_get(cleanup_list, i);
		if (strcmp(mode, SNAPSHOT_RESYNC) == 0)
		{
			parray_remove(cleanup_list, i);
			break;
		}
	}
	/* execute 'resync' command */
	execute_script(SNAPSHOT_RESYNC, false, NULL);
}

/*
 * Execute the command 'mount' of snapshot-script.
 * When the command ends normally, 'umount' is added to the cleanup list.
 */
static void
execute_mount(parray *tblspcmp_list)
{
	/* append 'umount' command to cleanup list */
	parray_append(cleanup_list, SNAPSHOT_UMOUNT);

	/* execute 'mount' command */
	execute_script(SNAPSHOT_MOUNT, false, tblspcmp_list);
}

/*
 * Execute the command 'umount' of snapshot-script.
 * Remove 'umount' from the cleanup list before executing the command
 * when 'umount' is included in the cleanup list.
 */
static void
execute_umount(void)
{
	int	i;

	/* remove 'umount' command from cleanup list */
	for (i = 0; i < parray_num(cleanup_list); i++)
	{
		char *mode = (char *) parray_get(cleanup_list, i);

		if (strcmp(mode, SNAPSHOT_UMOUNT) == 0)
		{
			parray_remove(cleanup_list, i);
			break;
		}
	}
	/* execute 'umount' command */
	execute_script(SNAPSHOT_UMOUNT, false, NULL);
}

/*
 * Execute the snapshot-script in the specified mode.
 * A standard output of snapshot-script is stored in the array given to the parameter.
 * If is_cleanup is TRUE, processing is continued.
 */
static void
execute_script(const char *mode, bool is_cleanup, parray *output)
{
	char	 ss_script[MAXPGPATH];
	char	 command[1024];
	char	 fline[2048];
	int		 num;
	FILE	*out;
	parray	*lines;

	/* obtain the path of snapshot-script. */
	join_path_components(ss_script, backup_path, SNAPSHOT_SCRIPT_FILE);
	snprintf(command, sizeof(command),
		"%s %s %s", ss_script, mode, is_cleanup ? "cleanup" : "");

	/* execute snapshot-script */
	out = popen(command, "r");
	if (out == NULL)
		ereport(ERROR,
			(errcode(ERROR_SYSTEM),
			 errmsg("could not execute snapshot-script: %s\n", strerror(errno))));

	/* read STDOUT and store into the array each line */
	lines = parray_new();
	while (fgets(fline, sizeof(fline), out) != NULL)
	{
		/* remove line separator */
		if (fline[strlen(fline) - 1] == '\n')
			fline[strlen(fline) - 1] = '\0';
		parray_append(lines, pgut_strdup(fline));
	}
	pclose(out);

	/*
	 * status of the command is obtained from the last element of the array
	 * if last element is not 'SUCCESS', that means ERROR.
	 */
	num = parray_num(lines);
	if (num <= 0 || strcmp((char *) parray_get(lines, num - 1), "SUCCESS") != 0)
		is_cleanup ? elog(WARNING, _("snapshot-script failed: %s"), mode)
					: ereport(ERROR,
						(errcode(ERROR_SYSTEM),
						 errmsg("snapshot-script failed: %s", mode)));

	/* if output is not NULL, concat array. */
	if (output)
	{
		parray_remove(lines, num -1);	/* remove last element, that is command status */
		parray_concat(output, lines);
	}
	/* if output is NULL, clear directory list */
	else
	{
		parray_walk(lines, free);
		parray_free(lines);
	}
}

/*
 * Delete the unnecessary object created by snapshot-script.
 * The command necessary for the deletion is given from the parameter.
 * When the error occurs, this function is called.
 */
static void
snapshot_cleanup(bool fatal, void *userdata)
{
	parray	*cleanup_list;
	int		 i;

	/* Execute snapshot-script for cleanup */
	cleanup_list = (parray *) userdata;
	for (i = parray_num(cleanup_list) - 1; i >= 0; i--)
		execute_script((char *) parray_get(cleanup_list, i), true, NULL);
}

/*
 * Append files to the backup list array.
 */
static void
add_files(parray *files, const char *root, bool add_root, bool is_pgdata)
{
	parray	*list_file;
	int		 i;

	list_file = parray_new();

	/* list files with the logical path. omit $PGDATA */
	dir_list_file(list_file, root, pgdata_exclude, true, add_root);

	/* mark files that are possible datafile as 'datafile' */
	for (i = 0; i < parray_num(list_file); i++)
	{
		pgFile *file = (pgFile *) parray_get(list_file, i);
		char *relative;
		char *fname;

		/* data file must be a regular file */
		if (!S_ISREG(file->mode))
			continue;

		/* data files are under "base", "global", or "pg_tblspc" */
		relative = file->path + strlen(root) + 1;
		if (is_pgdata &&
			!path_is_prefix_of_path("base", relative) &&
			!path_is_prefix_of_path("global", relative) &&
			!path_is_prefix_of_path("pg_tblspc", relative))
			continue;

		/* name of data file start with digit */
		fname = last_dir_separator(relative);
		if (fname == NULL)
			fname = relative;
		else
			fname++;
		if (!isdigit(fname[0]))
			continue;

		file->is_datafile = true;
	}
	parray_concat(files, list_file);

	parray_free(list_file);
}

/*
 * Comparison function for parray_bsearch() compare the character string.
 */
static int
strCompare(const void *str1, const void *str2)
{
	return strcmp(*(char **) str1, *(char **) str2);
}

/*
 * Output the list of backup files to backup catalog
 */
static void
create_file_list(parray *files, const char *root, const char *prefix, bool is_append)
{
	FILE	*fp;
	char	 path[MAXPGPATH];

	if (!check)
	{
		/* output path is '$BACKUP_PATH/file_database.txt' */
		pgBackupGetPath(&current, path, lengthof(path), DATABASE_FILE_LIST);
		fp = fopen(path, is_append ? "at" : "wt");
		if (fp == NULL)
			ereport(ERROR,
				(errcode(ERROR_SYSTEM),
				 errmsg("could not open file list \"%s\": %s", path,
					strerror(errno))));
		dir_print_file_list(fp, files, root, prefix);
		fclose(fp);
	}
}

/*
 * Initialize data_checksum_enabled by reading the value of
 * data_checksum_version from the PG control file.
 */
static void
init_data_checksum_enabled()
{
	char				controlFilePath[MAXPGPATH];
	ControlFileData    *controlFile;

	/* Read the value of the setting from the control file in PGDATA. */
	snprintf(controlFilePath, MAXPGPATH, "%s/global/pg_control", pgdata);
	if (fileExists(controlFilePath))
	{
		bool	crc_ok;

		controlFile = get_controlfile(pgdata, &crc_ok);
		if (!crc_ok)
		{
			ereport(WARNING,
					(errmsg("control file appears to be corrupt"),
					 errdetail("Calculated CRC checksum does not match value stored in file.")));
			data_checksum_enabled = false;	/* can't really do anything */
		}
		else
			data_checksum_enabled = controlFile->data_checksum_version > 0;

		pg_free(controlFile);
	}
	else
		elog(WARNING, _("pg_controldata file \"%s\" does not exist"),
						controlFilePath);

	elog(DEBUG, "data checksum %s on the initially configured database",
						data_checksum_enabled ? "enabled" : "disabled");
}
