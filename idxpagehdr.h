/*-------------------------------------------------------------------------
 *
 * idxpagehdr.h: description of Index-AM specific content inside data pages
 *
 * Currently, only need GIN index related struct, because it's the only
 * index AM implementation that fails to set the values of PageHeaderData
 * field(s) correctly, which pg_rman's page parsing routine depends on
 * being correct.
 *
 * Try to keep in sync with the definition in the respective backend header
 * file!!!  Although, incompatible changes to the definitions in this file
 * will be rare.
 *
 * Copyright (c) 2009-2017, NIPPON TELEGRAPH AND TELEPHONE CORPORATION
 *
 *-------------------------------------------------------------------------
 */
#ifndef IDXPAGEHDR_H
#define IDXPAGEHDR_H

/* ------ GIN index related definitions --------- */

/* Page numbers of fixed-location pages */
#define GIN_METAPAGE_BLKNO	(0)

typedef struct GinMetaPageData
{
	/*
	 * Pointers to head and tail of pending list, which consists of GIN_LIST
	 * pages.  These store fast-inserted entries that haven't yet been moved
	 * into the regular GIN structure.
	 */
	BlockNumber head;
	BlockNumber tail;

	/*
	 * Free space in bytes in the pending list's tail page.
	 */
	uint32		tailFreeSize;

	/*
	 * We store both number of pages and number of heap tuples that are in the
	 * pending list.
	 */
	BlockNumber nPendingPages;
	int64		nPendingHeapTuples;

	/*
	 * Statistics for planner use (accurate as of last VACUUM)
	 */
	BlockNumber nTotalPages;
	BlockNumber nEntryPages;
	BlockNumber nDataPages;
	int64		nEntries;

	/*
	 * GIN version number (ideally this should have been at the front, but too
	 * late now.  Don't move it!)
	 *
	 * Currently 2 (for indexes initialized in 9.4 or later)
	 *
	 * Version 1 (indexes initialized in version 9.1, 9.2 or 9.3), is
	 * compatible, but may contain uncompressed posting tree (leaf) pages and
	 * posting lists. They will be converted to compressed format when
	 * modified.
	 *
	 * Version 0 (indexes initialized in 9.0 or before) is compatible but may
	 * be missing null entries, including both null keys and placeholders.
	 * Reject full-index-scan attempts on such indexes.
	 */
	int32		ginVersion;
} GinMetaPageData;

#define GIN_CURRENT_VERSION		2

#define GinPageGetMeta(p) \
	((GinMetaPageData *) PageGetContents(p))

#define IS_GIN_INDEX_METAPAGE(blkno, pagedata) \
		((blkno) == GIN_METAPAGE_BLKNO && \
		 ((GinMetaPageData *) PageGetContents((pagedata)))->ginVersion == \
														GIN_CURRENT_VERSION)

#endif	/*IDXPAGEHDR_H */
