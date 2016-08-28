/*-------------------------------------------------------------------------
 *
 * dir.c: directory operation utility.
 *
 * Copyright (c) 2009-2016, NIPPON TELEGRAPH AND TELEPHONE CORPORATION
 *
 *-------------------------------------------------------------------------
 */

#include "pg_rman.h"

#include <libgen.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <time.h>

#include "pgut/pgut-port.h"

/* directory exclusion list for backup mode listing */
const char *pgdata_exclude[] =
{
	"pg_xlog",
	"pg_stat_tmp",
	"pgsql_tmp",
	NULL,			/* arclog_path will be set later */
	NULL,			/* srvlog_path will be set later */
	NULL,			/* 'pg_tblspc' will be set later */
	NULL,			/* sentinel */
};

static pgFile *pgFileNew(const char *path, bool omit_symlink);
static int BlackListCompare(const void *str1, const void *str2);

/* create directory, also create parent directories if necessary */
int
dir_create_dir(const char *dir, mode_t mode)
{
	char copy[MAXPGPATH];
	char *parent;

	strncpy(copy, dir, MAXPGPATH);
	parent = dirname(copy);
	if (access(parent, F_OK) == -1)
		dir_create_dir(parent, mode);
#ifdef __darwin__
	if (mkdir(copy, mode) == -1)
#else
	if (mkdir(dir, mode) == -1)
#endif
	{
		if (errno == EEXIST)	/* already exist */
			return 0;
		ereport(ERROR,
			(errcode(ERROR_SYSTEM),
			 errmsg("could not create directory \"%s\": %s", dir, strerror(errno))));
	}

	return 0;
}

/* delete parent dir of each backup
 * Each backup are located in '$BACKUP_PATH/YYYYMMDD/HHMMSS/'.
 * This function checks whether '$BACKUP_PATH/YYYYMMDD' directory can be deleted.
 */
void
delete_parent_dir(const char *path)
{
	char	*str;
	char	parent_dir_path[MAXPGPATH];

	str = strrchr(path, '/');
	memset(parent_dir_path, 0, lengthof(parent_dir_path));
	strncpy(parent_dir_path, path, str - path);
	if ( rmdir(parent_dir_path) == -1 )
	{
		if (errno == ENOTEMPTY || errno == EEXIST)
			elog(DEBUG, "the directory \"%s\" is not empty, skip deleting", parent_dir_path);
		else
			ereport(WARNING,
				(errcode(ERROR_SYSTEM),
				 errmsg("could not remove directory \"%s\" : %s", parent_dir_path,
					strerror(errno))));
	}
	else
		elog(DEBUG, "the directory \"%s\" is deleted", parent_dir_path);

	return;
}


static pgFile *
pgFileNew(const char *path, bool omit_symlink)
{
	struct stat		st;
	pgFile		   *file;

	/* stat the file */
	if ((omit_symlink ? stat(path, &st) : lstat(path, &st)) == -1)
	{
		/* file not found is not an error case */
		if (errno == ENOENT)
			return NULL;
		ereport(ERROR,
			(errcode(ERROR_SYSTEM),
			 errmsg("could not stat file \"%s\": %s", path, strerror(errno))));
	}

	file = (pgFile *) pgut_malloc(offsetof(pgFile, path) + strlen(path) + 1);

	file->mtime = st.st_mtime;
	file->size = st.st_size;
	file->read_size = 0;
	file->write_size = 0;
	file->mode = st.st_mode;
	file->crc = 0;
	file->is_datafile = false;
	file->linked = NULL;
	strcpy(file->path, path);		/* enough buffer size guaranteed */

	return file;
}

/*
 * Delete file pointed by the pgFile.
 * If the pgFile points directory, the directory must be empty.
 */
void
pgFileDelete(pgFile *file)
{
	if (S_ISDIR(file->mode))
	{
		if (rmdir(file->path) == -1)
		{
			if (errno == ENOENT)
				return;
			else if (errno == ENOTDIR)	/* could be symbolic link */
				goto delete_file;

			ereport(ERROR,
				(errcode(ERROR_SYSTEM),
				 errmsg("could not remove directory \"%s\": %s",
					file->path, strerror(errno))));
		}
		return;
	}

delete_file:
	if (remove(file->path) == -1)
	{
		if (errno == ENOENT)
			return;
		ereport(ERROR,
			(errcode(ERROR_SYSTEM),
			 errmsg("could not remove file \"%s\": %s", file->path, strerror(errno))));
	}
}

pg_crc32c
pgFileGetCRC(pgFile *file)
{
	FILE	   *fp;
	pg_crc32c	crc = 0;
	char		buf[1024];
	size_t		len;
	int			errno_tmp;

	/* open file in binary read mode */
	fp = fopen(file->path, "r");
	if (fp == NULL)
		ereport(ERROR,
			(errcode(ERROR_SYSTEM),
			 errmsg("could not open file \"%s\": %s", file->path, strerror(errno))));

	/* calc CRC of backup file */
	PGRMAN_INIT_CRC32(crc);
	while ((len = fread(buf, 1, sizeof(buf), fp)) == sizeof(buf))
	{
		if (interrupted)
			ereport(FATAL,
				(errcode(ERROR_INTERRUPTED),
				 errmsg("interrupted during CRC calculation")));
		PGRMAN_COMP_CRC32(crc, buf, len);
	}
	errno_tmp = errno;
	if (!feof(fp))
		elog(WARNING, _("could not read \"%s\": %s"), file->path,
			strerror(errno_tmp));
	if (len > 0)
		PGRMAN_COMP_CRC32(crc, buf, len);
	PGRMAN_FIN_CRC32(crc);

	fclose(fp);

	return crc;
}

void
pgFileFree(void *file)
{
	if (file == NULL)
		return;
	free(((pgFile *)file)->linked);
	free(file);
}

/* Compare two pgFile with their path in ascending order of ASCII code. */
int
pgFileComparePath(const void *f1, const void *f2)
{
	pgFile *f1p = *(pgFile **)f1;
	pgFile *f2p = *(pgFile **)f2;

	return strcmp(f1p->path, f2p->path);
}

/* Compare two pgFile with their path in descending order of ASCII code. */
int
pgFileComparePathDesc(const void *f1, const void *f2)
{
	return -pgFileComparePath(f1, f2);
}

/* Compare two pgFile with their modify timestamp. */
int
pgFileCompareMtime(const void *f1, const void *f2)
{
	pgFile *f1p = *(pgFile **)f1;
	pgFile *f2p = *(pgFile **)f2;

	if (f1p->mtime > f2p->mtime)
		return 1;
	else if (f1p->mtime < f2p->mtime)
		return -1;
	else
		return 0;
}

/* Compare two pgFile with their modify timestamp in descending order. */
int
pgFileCompareMtimeDesc(const void *f1, const void *f2)
{
	return -pgFileCompareMtime(f1, f2);
}

static int
BlackListCompare(const void *str1, const void *str2)
{
	return strcmp(*(char **) str1, *(char **) str2);
}

/*
 * List files, symbolic links and directories in the directory "root" and add
 * pgFile objects to "files".  We add "root" to "files" if add_root is true.
 *
 * If the sub-directory name is in "exclude" list, the sub-directory itself is
 * listed but the contents of the sub-directory is ignored.
 *
 * When omit_symlink is true, symbolic link is ignored and only file or
 * directory llnked to will be listed.
 */
void
dir_list_file(parray *files, const char *root, const char *exclude[], bool omit_symlink, bool add_root)
{
	char path[MAXPGPATH];
	char buf[MAXPGPATH * 2];
	char black_item[MAXPGPATH * 2];
	parray *black_list = NULL;

	join_path_components(path, backup_path, PG_BLACK_LIST);
	if (root && pgdata && strcmp(root, pgdata) == 0 &&
	    fileExists(path))
	{
		FILE *black_list_file = NULL;

		black_list = parray_new();
		black_list_file = fopen(path, "r");
		if (black_list_file == NULL)
			ereport(ERROR,
				(errcode(ERROR_SYSTEM),
				 errmsg("could not open black_list: %s", strerror(errno))));

		while (fgets(buf, lengthof(buf), black_list_file) != NULL)
		{
			join_path_components(black_item, pgdata, buf);
			if (black_item[strlen(black_item) - 1] == '\n')
				black_item[strlen(black_item) - 1] = '\0';
			if (black_item[0] == '#' || black_item[0] == '\0')
				continue;
			parray_append(black_list, black_item);
		}
		fclose(black_list_file);
		parray_qsort(black_list, BlackListCompare);
		dir_list_file_internal(files, root, exclude, omit_symlink, add_root, black_list);
	}
	else
		dir_list_file_internal(files, root, exclude, omit_symlink, add_root, NULL);
}

void
dir_list_file_internal(parray *files, const char *root, const char *exclude[],
			bool omit_symlink, bool add_root, parray *black_list)
{
	pgFile *file;

	file = pgFileNew(root, omit_symlink);
	if (file == NULL)
		return;

	/* skip if the file is in black_list defined by user */
	if (black_list && parray_bsearch(black_list, root, BlackListCompare))
	{
		/* found in black_list. skip this item */
		return;
	}

	if (add_root)
		parray_append(files, file);

	/* chase symbolic link chain and find regular file or directory */
	while (S_ISLNK(file->mode))
	{
		ssize_t	len;
		char	linked[MAXPGPATH];

		len = readlink(file->path, linked, sizeof(linked));
		if (len == -1)
		{
			ereport(ERROR,
				(errcode(ERROR_SYSTEM),
				 errmsg("could not read link \"%s\": %s", file->path, strerror(errno))));
		}
		linked[len] = '\0';
		file->linked = pgut_strdup(linked);

		/* make absolute path to read linked file */
		if (linked[0] != '/')
		{
			char	dname[MAXPGPATH];
			char	absolute[MAXPGPATH];

			strncpy(dname, file->path, lengthof(dname));
			join_path_components(absolute, dirname(dname), linked);
			file = pgFileNew(absolute, omit_symlink);
		}
		else
			file = pgFileNew(file->linked, omit_symlink);

		/* linked file is not found, stop following link chain */
		if (file == NULL)
			return;

		parray_append(files, file);
	}

	/*
	 * If the entry was a directory, add it to the list and add call this
	 * function recursively.
	 * If the directory name is in the exclude list, do not list the contents.
	 */
	while (S_ISDIR(file->mode))
	{
		int				i;
		bool			skip = false;
		DIR			    *dir;
		struct dirent   *dent;
		char		    *dirname;

		/* skip entry which matches exclude list */
	   	dirname = strrchr(file->path, '/');
		if (dirname == NULL)
			dirname = file->path;
		else
			dirname++;

		/*
		 * If the item in the exclude list starts with '/', compare to the
		 * absolute path of the directory. Otherwise compare to the directory
		 * name portion.
		 */
		for (i = 0; exclude && exclude[i]; i++)
		{
			if (exclude[i][0] == '/')
			{
				if (strcmp(file->path, exclude[i]) == 0)
				{
					skip = true;
					break;
				}
			}
			else
			{
				if (strcmp(dirname, exclude[i]) == 0)
				{
					skip = true;
					break;
				}
			}
		}
		if (skip)
			break;

		/* open directory and list contents */
		dir = opendir(file->path);
		if (dir == NULL)
		{
			if (errno == ENOENT)
			{
				/* maybe the directory was removed */
				return;
			}
			ereport(ERROR,
				(errcode(ERROR_SYSTEM),
				 errmsg("could not open directory \"%s\": %s",
					file->path, strerror(errno))));
		}

		errno = 0;
		while ((dent = readdir(dir)))
		{
			char child[MAXPGPATH];

			/* skip entries point current dir or parent dir */
			if (strcmp(dent->d_name, ".") == 0 ||
				strcmp(dent->d_name, "..") == 0)
				continue;

			join_path_components(child, file->path, dent->d_name);
			dir_list_file_internal(files, child, exclude, omit_symlink, true, black_list);
		}
		if (errno && errno != ENOENT)
		{
			int errno_tmp = errno;
			closedir(dir);
			ereport(ERROR,
				(errcode(ERROR_SYSTEM),
				 errmsg("could not read directory \"%s\": %s",
					file->path, strerror(errno_tmp))));
		}
		closedir(dir);

		break;	/* pseudo loop */
	}

	parray_qsort(files, pgFileComparePath);
}

/* print mkdirs.sh */
void
dir_print_mkdirs_sh(FILE *out, const parray *files, const char *root)
{
	int i;

	for (i = 0; i < parray_num(files); i++)
	{
		pgFile *file = (pgFile *) parray_get(files, i);
		if (S_ISDIR(file->mode))
		{
			if (strstr(file->path, root) == file->path)
				fprintf(out, "mkdir -m 700 -p %s\n", file->path + strlen(root) + 1);
			else
				fprintf(out, "mkdir -m 700 -p %s\n", file->path);
		}
	}

	fprintf(out, "\n");

	for (i = 0; i < parray_num(files); i++)
	{
		pgFile *file = (pgFile *) parray_get(files, i);
		if (S_ISLNK(file->mode))
		{
			fprintf(out, "rm -f %s\n", file->path + strlen(root) + 1);
			fprintf(out, "ln -s %s %s\n", file->linked, file->path + strlen(root) + 1);
		}
	}
}

/* print file list */
void
dir_print_file_list(FILE *out, const parray *files, const char *root, const char *prefix)
{
	int i;
	int root_len = 0;

	/* calculate length of root directory portion */
	if (root)
	{
		root_len = strlen(root);
		if (root[root_len - 1] != '/')
			root_len++;
	}

	/* print each file in the list */
	for (i = 0; i < parray_num(files); i++)
	{
		pgFile *file = (pgFile *)parray_get(files, i);
		char path[MAXPGPATH];
		char *ptr = file->path;
		char type;

		/* omit root directory portion */
		if (root && strstr(ptr, root) == ptr)
			ptr = JoinPathEnd(ptr, root);

		/* append prefix if not NULL */
		if (prefix)
			join_path_components(path, prefix, ptr);
		else
			strcpy(path, ptr);

		if (S_ISREG(file->mode) && file->is_datafile)
			type = 'F';
		else if (S_ISREG(file->mode) && !file->is_datafile)
			type = 'f';
		else if (S_ISDIR(file->mode))
			type = 'd';
		else if (S_ISLNK(file->mode))
			type = 'l';
		else if (S_ISSOCK(file->mode))
			type = 's';
		else
			type = '?';

		fprintf(out, "%s %c %lu %u 0%o", path, type,
			(unsigned long) file->write_size,
			file->crc, file->mode & (S_IRWXU | S_IRWXG | S_IRWXO));

		if (S_ISLNK(file->mode))
			fprintf(out, " %s\n", file->linked);
		else
		{
			char timestamp[20];
			time2iso(timestamp, 20, file->mtime);
			fprintf(out, " %s\n", timestamp);
		}
	}
}

/*
 * Construct parray of pgFile from the file list.
 * If root is not NULL, path will be absolute path.
 */
parray *
dir_read_file_list(const char *root, const char *file_txt)
{
	FILE   *fp;
	parray *files;
	char	buf[MAXPGPATH * 2];

	fp = fopen(file_txt, "rt");
	if (fp == NULL)
		ereport(ERROR,
			((errno == ENOENT ? errcode(ERROR_CORRUPTED) : errcode(ERROR_SYSTEM)),
			 errmsg("could not open \"%s\": %s", file_txt, strerror(errno))));

	files = parray_new();

	while (fgets(buf, lengthof(buf), fp))
	{
		char			path[MAXPGPATH];
		char			type;
		unsigned long	write_size;
		pg_crc32c		crc;
		unsigned int	mode;	/* bit length of mode_t depends on platforms */
		struct tm		tm;
		pgFile		   *file;

		memset(&tm, 0, sizeof(tm));
		if (sscanf(buf, "%s %c %lu %u %o %d-%d-%d %d:%d:%d",
			path, &type, &write_size, &crc, &mode,
			&tm.tm_year, &tm.tm_mon, &tm.tm_mday,
			&tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 11)
			ereport(ERROR,
				(errcode(ERROR_CORRUPTED),
				 errmsg("invalid format found in \"%s\"", file_txt)));

		if (type != 'f' && type != 'F' && type != 'd' && type != 'l' && type != 's')
			ereport(ERROR,
				(errcode(ERROR_CORRUPTED),
				 errmsg("invalid type '%c' found in \"%s\"", type, file_txt)));

		tm.tm_isdst = -1;

		file = (pgFile *) pgut_malloc(offsetof(pgFile, path) +
					(root ? strlen(root) + 1 : 0) + strlen(path) + 1);

		tm.tm_year -= 1900;
		tm.tm_mon -= 1;
		file->mtime = mktime(&tm);
		file->mode = mode |
			((type == 'f' || type == 'F') ? S_IFREG :
			 type == 'd' ? S_IFDIR : type == 'l' ? S_IFLNK :
			 type == 's' ? S_IFSOCK : 0);
		file->size = 0;
		file->read_size = 0;
		file->write_size = write_size;
		file->crc = crc;
		file->is_datafile = (type == 'F' ? true : false);
		file->linked = NULL;
		if (root)
			sprintf(file->path, "%s/%s", root, path);
		else
			strcpy(file->path, path);

		parray_append(files, file);
	}

	fclose(fp);

	/* file.txt is sorted, so this qsort is redundant */
	parray_qsort(files, pgFileComparePath);

	return files;
}

/* copy contents of directory from_root into to_root */
void
dir_copy_files(const char *from_root, const char *to_root)
{
	int		i;
	parray *files = parray_new();

	/* don't copy root directory */
	dir_list_file(files, from_root, NULL, true, false);

	for (i = 0; i < parray_num(files); i++)
	{
		pgFile *file = (pgFile *) parray_get(files, i);

		if (S_ISDIR(file->mode))
		{
			char to_path[MAXPGPATH];
			join_path_components(to_path, to_root, file->path + strlen(from_root) + 1);
			if (verbose && !check)
				printf(_("create directory \"%s\"\n"),
					file->path + strlen(from_root) + 1);
			if (!check)
				dir_create_dir(to_path, DIR_PERMISSION);
			continue;
		}
		else if(S_ISREG(file->mode))
		{
			if (verbose && !check)
				printf(_("copy \"%s\"\n"),
					file->path + strlen(from_root) + 1);
			if (!check)
				copy_file(from_root, to_root, file, NO_COMPRESSION);
		}
	}

	/* cleanup */
	parray_walk(files, pgFileFree);
	parray_free(files);
}

#ifdef NOT_USED
void
pgFileDump(pgFile *file, FILE *out)
{
	char mtime_str[100];

	fprintf(out, "=================\n");
	if (file)
	{
		time2iso(mtime_str, 100, file->mtime);
		fprintf(out, "mtime=%lu(%s)\n", file->mtime, mtime_str);
		fprintf(out, "size=" UINT64_FORMAT "\n", (uint64)file->size);
		fprintf(out, "read_size=" UINT64_FORMAT "\n", (uint64)file->read_size);
		fprintf(out, "write_size=" UINT64_FORMAT "\n", (uint64)file->write_size);
		fprintf(out, "mode=0%o\n", file->mode);
		fprintf(out, "crc=%u\n", file->crc);
		fprintf(out, "is_datafile=%s\n", file->is_datafile ? "true" : "false");
		fprintf(out, "linked=\"%s\"\n", file->linked ? file->linked : "nil");
		fprintf(out, "path=\"%s\"\n", file->path);
	}
	fprintf(out, "=================\n");
}
#endif
