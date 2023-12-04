/*-------------------------------------------------------------------------
 *
 * xlog.c: Parse WAL files.
 *
 * Copyright (c) 2009-2023, NIPPON TELEGRAPH AND TELEPHONE CORPORATION
 *
 *-------------------------------------------------------------------------
 */

#include "pg_rman.h"

#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include "access/xlog_internal.h"

typedef unsigned long Datum;

/*
 * XLogLongPageHeaderData is modified in 8.3, but the layout is compatible
 * except xlp_xlog_blcksz.
 */
typedef union XLogPage
{
	XLogPageHeaderData		header;
	XLogLongPageHeaderData	lheader;
	char					data[XLOG_BLCKSZ];
} XLogPage;

/*
 * Return whether the file is a WAL segment or not.
 * based on ValidXLOGHeader() in src/backend/access/transam/xlog.c.
 */
bool
xlog_is_complete_wal(const pgFile *file, int wal_segment_size)
{
	FILE		   *fp;
	XLogPage		page;

	fp = fopen(file->path, "r");
	if (!fp)
		return false;
	if (fread(&page, 1, sizeof(page), fp) != XLOG_BLCKSZ)
	{
		fclose(fp);
		return false;
	}
	fclose(fp);

	/* check header (assumes PG version >= 8.4) */
	if (page.header.xlp_magic != XLOG_PAGE_MAGIC)
		return false;
	if ((page.header.xlp_info & ~XLP_ALL_FLAGS) != 0)
		return false;
	if ((page.header.xlp_info & XLP_LONG_HEADER) == 0)
		return false;
	if (page.lheader.xlp_seg_size != wal_segment_size)
		return false;
	if (page.lheader.xlp_xlog_blcksz != XLOG_BLCKSZ)
		return false;

	/*
	 * check size (actual file size, not backup file size)
	 * TODO: Support pre-compressed xlog. They might have different file sizes.
	 */
	if (file->size != wal_segment_size)
		return false;

	return true;
}

/*
 * based on XLogFileName() in xlog_internal.h
 */
void
xlog_fname(char *fname, size_t len, TimeLineID tli, XLogRecPtr *lsn,
		   int wal_segment_size)
{
	uint32 xlogid, xrecoff;

	xlogid = (uint32) (*lsn >> 32);
	xrecoff = (uint32) *lsn;

	snprintf(fname, len, "%08X%08X%08X", tli,
		xlogid, xrecoff / wal_segment_size);
}
