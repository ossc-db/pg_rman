/*-------------------------------------------------------------------------
 *
 * pg_rman.h: Backup/Recovery manager for PostgreSQL.
 *
 * Copyright (c) 2009-2021, NIPPON TELEGRAPH AND TELEPHONE CORPORATION
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_RMAN_H
#define PG_RMAN_H

#define FRONTEND	1
#include "postgres_fe.h"

#include <limits.h>
#include "libpq-fe.h"

#include "access/xlog_internal.h"
#include "pgut/pgut.h"
#include "access/xlogdefs.h"
#include "storage/bufpage.h"
#include "port/pg_crc32c.h"
#include "parray.h"

#define TXID_CURRENT_SQL	"SELECT txid_current();"

/* Directory/File names */
#define DATABASE_DIR			"database"
#define ARCLOG_DIR				"arclog"
#define SRVLOG_DIR				"srvlog"
#define RESTORE_WORK_DIR		"backup"
#define PG_XLOG_DIR				"pg_wal"
#define PG_TBLSPC_DIR			"pg_tblspc"
#define TIMELINE_HISTORY_DIR	"timeline_history"
#define BACKUP_INI_FILE			"backup.ini"
#define PG_RMAN_INI_FILE		"pg_rman.ini"
#define SYSTEM_IDENTIFIER_FILE	"system_identifier"
#define MKDIRS_SH_FILE			"mkdirs.sh"
#define DATABASE_FILE_LIST		"file_database.txt"
#define ARCLOG_FILE_LIST		"file_arclog.txt"
#define SRVLOG_FILE_LIST		"file_srvlog.txt"
#define SNAPSHOT_SCRIPT_FILE	"snapshot_script"
#define PG_BACKUP_LABEL_FILE	"backup_label"
#define PG_TBLSPC_MAP_FILE		"tablespace_map"
#define PG_BLACK_LIST			"black_list"

/* Snapshot script command */
#define SNAPSHOT_FREEZE			"freeze"
#define SNAPSHOT_UNFREEZE		"unfreeze"
#define SNAPSHOT_SPLIT			"split"
#define SNAPSHOT_RESYNC			"resync"
#define SNAPSHOT_MOUNT			"mount"
#define SNAPSHOT_UMOUNT			"umount"

/* Directory/File permission */
#define DIR_PERMISSION		(0700)
#define FILE_PERMISSION		(0600)

/* Exit code */
#define ERROR_ARCHIVE_FAILED	20	/* cannot archive xlog file */
#define ERROR_NO_BACKUP			21	/* backup was not found in the catalog */
#define ERROR_CORRUPTED			22	/* backup catalog is corrupted */
#define ERROR_ALREADY_RUNNING	23	/* another pg_rman is running */
#define ERROR_PG_INCOMPATIBLE	24	/* block size is not compatible */
#define ERROR_PG_RUNNING		25	/* PostgreSQL server is running */
#define ERROR_PID_BROKEN		26	/* postmaster.pid file is broken */

/* backup mode file */
typedef struct pgFile
{
	time_t	mtime;			/* time of last modification */
	mode_t	mode;			/* protection (file type and permission) */
	size_t	size;			/* size of the file */
	size_t	read_size;		/* size of the portion read (if only some pages are
							   backed up partially, it's different from size) */
	size_t	write_size;		/* size of the backed-up file. BYTES_INVALID means
							   that the file existed but was not backed up
							   because not modified since last backup. */
	pg_crc32c crc;			/* CRC value of the file, regular file only */
	char   *linked;			/* path of the linked file */
	bool	is_datafile;	/* true if the file is PostgreSQL data file */
	char	path[1]; 		/* path of the file */
} pgFile;

typedef struct pgBackupRange
{
	time_t	begin;
	time_t	end;			/* begin +1 when one backup is target */
} pgBackupRange;

/*
 * Along with each data page, the following information is written to the
 * backup.
 */
typedef struct BackupPageHeader
{
    BlockNumber block;          /* block number */
    uint16      hole_offset;    /* number of bytes before "hole" */
    uint16      hole_length;    /* number of bytes in "hole" */
    bool        endpoint;       /* If set to true, this page marks the end
                                   of relation, which means any subsequent
                                   pages are truncated. */
} BackupPageHeader;


#define pgBackupRangeIsValid(range)	\
	(((range)->begin != (time_t) 0) || ((range)->end != (time_t) 0))
#define pgBackupRangeIsSingle(range) \
	(pgBackupRangeIsValid(range) && (range)->begin == ((range)->end))

#define IsValidTime(tm)	\
	((tm.tm_sec >= 0 && tm.tm_sec <= 60) && 	/* range check for tm_sec (0-60)  */ \
	 (tm.tm_min >= 0 && tm.tm_min <= 59) && 	/* range check for tm_min (0-59)  */ \
	 (tm.tm_hour >= 0 && tm.tm_hour <= 23) && 	/* range check for tm_hour(0-23)  */ \
	 (tm.tm_mday >= 1 && tm.tm_mday <= 31) && 	/* range check for tm_mday(1-31)  */ \
	 (tm.tm_mon >= 0 && tm.tm_mon <= 11) && 	/* range check for tm_mon (0-23)  */ \
	 (tm.tm_year + 1900 >= 1900)) 			/* range check for tm_year(70-)    */

/* Backup status */
/* XXX re-order ? */
typedef enum BackupStatus
{
	BACKUP_STATUS_INVALID,		/* the pgBackup is invalid */
	BACKUP_STATUS_OK,			/* completed backup */
	BACKUP_STATUS_RUNNING,		/* running backup */
	BACKUP_STATUS_ERROR,		/* aborted because of unexpected error */
	BACKUP_STATUS_DELETING,		/* data files are being deleted */
	BACKUP_STATUS_DELETED,		/* data files have been deleted */
	BACKUP_STATUS_DONE,			/* completed but not validated yet */
	BACKUP_STATUS_CORRUPT		/* files are corrupted, not available */
} BackupStatus;

typedef enum BackupMode
{
	BACKUP_MODE_INVALID,
	BACKUP_MODE_ARCHIVE,		/* archive only */
	BACKUP_MODE_INCREMENTAL,	/* incremental backup */
	BACKUP_MODE_FULL			/* full backup */
} BackupMode;

/*
 * pg_rman takes backup into the directory $BACKUP_PATH/<date>/<time>.
 *
 * status == -1 indicates the pgBackup is invalid.
 */
typedef struct pgBackup
{
	/* Backup Level */
	BackupMode	backup_mode;
	bool		with_serverlog;
	bool		compress_data;
	bool		full_backup_on_error;

	/* Status - one of BACKUP_STATUS_xxx */
	BackupStatus	status;

	/* Timestamp, etc. */
	TimeLineID	tli;

	XLogRecPtr	start_lsn;
	XLogRecPtr	stop_lsn;

	time_t		start_time;
	time_t		end_time;
	time_t		recovery_time;
	uint32		recovery_xid;

	/* Size (-1 means not-backup'ed) */
	int64		total_data_bytes;
	int64		read_data_bytes;
	int64		read_arclog_bytes;
	int64		read_srvlog_bytes;
	int64		write_bytes;

	/* data/wal block size for compatibility check */
	uint32		block_size;
	uint32		wal_block_size;

	/* if backup from standby or not */
	bool		is_from_standby;

} pgBackup;

typedef struct pgBackupOption
{
	bool smooth_checkpoint;
	int  keep_arclog_files;
	int  keep_arclog_days;
	int  keep_srvlog_files;
	int  keep_srvlog_days;
	int  keep_data_generations;
	int  keep_data_days;
	char *standby_host;
	char *standby_port;
} pgBackupOption;


/* special values of pgBackup */
#define KEEP_INFINITE			(INT_MAX)
#define BYTES_INVALID			(-1)

#define HAVE_DATABASE(backup)	((backup)->backup_mode >= BACKUP_MODE_INCREMENTAL)
#define HAVE_ARCLOG(backup)		((backup)->backup_mode >= BACKUP_MODE_ARCHIVE)
#define TOTAL_READ_SIZE(backup)	\
	((HAVE_DATABASE((backup)) ? (backup)->read_data_bytes : 0) + \
	 (HAVE_ARCLOG((backup)) ? (backup)->read_arclog_bytes : 0) + \
	 ((backup)->with_serverlog ? (backup)->read_srvlog_bytes : 0))

typedef struct pgTimeLine
{
	TimeLineID	tli;
	XLogRecPtr	end;
} pgTimeLine;

typedef struct pgRecoveryTarget
{
	bool		time_specified;
	time_t		recovery_target_time;
	bool		xid_specified;
	unsigned int	recovery_target_xid;
	bool		recovery_target_inclusive;
} pgRecoveryTarget;

typedef enum CompressionMode
{
	NO_COMPRESSION,
	COMPRESSION,
	DECOMPRESSION,
} CompressionMode;

/*
 * return pointer that exceeds the length of prefix from character string.
 * ex. str="/xxx/yyy/zzz", prefix="/xxx/yyy", return="zzz".
 */
#define JoinPathEnd(str, prefix) \
	((strlen(str) <= strlen(prefix)) ? "" : str + strlen(prefix) + 1)

/* path configuration */
extern char *backup_path;
extern char *pgdata;
extern char *arclog_path;
extern char *srvlog_path;

/* common configuration */
extern bool verbose;
extern bool progress;
extern bool check;

/* current settings */
extern pgBackup current;

/* exclude directory list for $PGDATA file listing */
extern const char *pgdata_exclude[];

/* Is data checksum enabled per cluster-settings? */
extern bool data_checksum_enabled;

/* in backup.c */
extern int do_backup(pgBackupOption bkupopt);
extern BackupMode parse_backup_mode(const char *value, int elevel);

/* in restore.c */
extern int do_restore(const char *target_time,
					  const char *target_xid,
					  const char *target_inclusive,
					  const char *target_tli_string,
					  bool is_hard_copy);

/* in init.c */
extern int do_init(void);

/* in show.c */
extern int do_show(pgBackupRange *range, bool show_detail, bool show_all);

/* in delete.c */
extern int do_delete(pgBackupRange *range, bool force);
extern void pgBackupDelete(int keep_generations, int keep_days);
extern int do_purge(void);
extern char * getCountSuffix(int number);

/* in validate.c */
extern int do_validate(pgBackupRange *range);
extern void pgBackupValidate(pgBackup *backup, bool size_only, bool for_get_timeline, bool with_database);

/* in catalog.c */
extern pgBackup *catalog_get_backup(time_t timestamp);
extern parray *catalog_get_backup_list(const pgBackupRange *range);
extern pgBackup *catalog_get_last_full_backup(parray *backup_list);
extern pgBackup *catalog_get_last_arclog_backup(parray *backup_list);
extern pgBackup *catalog_get_last_srvlog_backup(parray *backup_list);

extern int catalog_lock(void);
extern void catalog_unlock(void);

extern void catalog_init_config(pgBackup *backup);
extern void check_system_identifier(void);
extern TimeLineID get_current_timeline(void);

extern void pgBackupWriteConfigSection(FILE *out, pgBackup *backup);
extern void pgBackupWriteResultSection(FILE *out, pgBackup *backup);
extern void pgBackupWriteIni(pgBackup *backup);
extern void pgBackupGetPath(const pgBackup *backup, char *path, size_t len, const char *subdir);
extern int pgBackupCreateDir(pgBackup *backup);
extern void pgBackupFree(void *backup);
extern int pgBackupCompareId(const void *f1, const void *f2);
extern int pgBackupCompareIdDesc(const void *f1, const void *f2);

/* in dir.c */
extern void dir_list_file(parray *files, const char *root, const char *exclude[], bool omit_symlink, bool add_root);
extern void dir_list_file_internal(parray *files, const char *root, const char *exclude[],
					bool omit_symlink, bool add_root, parray *black_list);
extern void dir_print_mkdirs_sh(FILE *out, const parray *files, const char *root);
extern void dir_print_file_list(FILE *out, const parray *files, const char *root, const char *prefix);
extern parray *dir_read_file_list(const char *root, const char *file_txt);

extern int dir_create_dir(const char *path, mode_t mode);
extern void dir_copy_files(const char *from_root, const char *to_root);
extern void delete_parent_dir(const char *path);

extern void pgFileDelete(pgFile *file);
extern void pgFileFree(void *file);
extern pg_crc32c pgFileGetCRC(pgFile *file);
extern int pgFileComparePath(const void *f1, const void *f2);
extern int pgFileComparePathDesc(const void *f1, const void *f2);
extern int pgFileCompareMtime(const void *f1, const void *f2);
extern int pgFileCompareMtimeDesc(const void *f1, const void *f2);

/* in xlog.c */
extern bool xlog_is_complete_wal(const pgFile *file, int wal_segment_size);
extern bool xlog_logfname2lsn(const char *logfname, XLogRecPtr *lsn);
extern void xlog_fname(char *fname, size_t len, TimeLineID tli, XLogRecPtr *lsn,
					   int wal_segment_size);

/* in data.c */
extern bool backup_data_file(const char *from_root, const char *to_root,
							 pgFile *file, const XLogRecPtr *lsn, bool compress, bool prev_file_not_found);
extern void restore_data_file(const char *from_root, const char *to_root,
							  pgFile *file, bool compress);
extern bool copy_file(const char *from_root, const char *to_root,
					  pgFile *file, CompressionMode compress);
extern pgFile *write_stop_backup_file(pgBackup *backup, const char *buf, int len, const char *file_name);
extern bool fileExists(const char *path);
extern bool get_standby_signal_filepath(char *path, size_t size);

/* in util.c */
extern void time2iso(char *buf, size_t len, time_t time);
extern const char *status2str(BackupStatus status);
extern void remove_trailing_space(char *buf, int comment_mark);
extern void remove_not_digit(char *buf, size_t len, const char *str);

/* in pgsql_src/pg_ctl.c */
extern bool is_pg_running(void);

/*
 * Set of macros for CRC calculations.
 */
#define PGRMAN_INIT_CRC32(crc) INIT_CRC32C(crc)
#define PGRMAN_FIN_CRC32(crc) FIN_CRC32C(crc)
#define PGRMAN_COMP_CRC32(crc, data, len) COMP_CRC32C(crc, data, len)
#define PGRMAN_EQ_CRC32(c1, c2) EQ_CRC32C(c1, c2)

#define NextLogSeg(logId, logSeg, wal_segment_size)	\
	do { \
		if ((logSeg) >= XLogSegmentsPerXLogId((wal_segment_size)) - 1) \
		{ \
			(logId)++; \
			(logSeg) = 0; \
		} \
		else \
			(logSeg)++; \
	} while (0)

#endif /* PG_RMAN_H */
