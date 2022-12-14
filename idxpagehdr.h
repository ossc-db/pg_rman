/*-------------------------------------------------------------------------
 *
 * idxpagehdr.h: description of Index-AM specific content inside data pages
 *
 * Currently, we need index-content related structs for gin, brin and sp-gist
 * index types, because they fail to to set the values of PageHeaderData
 * field(s) correctly, which pg_rman's page parsing routine depends on
 * being correct, especially pd_lower.  All of these index type designate a
 * metapage and write a metadata struct next to the page header but fail to
 * set pd_lower to the byte address next to where the struct ends.
 *
 * Try to keep in sync with the definition in the respective backend header
 * file!!!  Although, incompatible changes to the definitions in this file
 * will be rare.
 *
 * Copyright (c) 2009-2022, NIPPON TELEGRAPH AND TELEPHONE CORPORATION
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

#define IS_GIN_INDEX_METAPAGE(blkno, pagedata) \
		((blkno) == GIN_METAPAGE_BLKNO && \
		 ((GinMetaPageData *) PageGetContents((pagedata)))->ginVersion == \
														GIN_CURRENT_VERSION)

/* ------ BRIN index related definitions --------- */

/* Metapage definitions */
typedef struct BrinMetaPageData
{
	uint32		brinMagic;
	uint32		brinVersion;
	BlockNumber pagesPerRange;
	BlockNumber lastRevmapPage;
} BrinMetaPageData;

#define BRIN_CURRENT_VERSION		1
#define BRIN_META_MAGIC			0xA8109CFA
#define BRIN_METAPAGE_BLKNO		0

#define IS_BRIN_INDEX_METAPAGE(blkno, pagedata) \
		((blkno) == BRIN_METAPAGE_BLKNO && \
		 ((BrinMetaPageData *) PageGetContents((pagedata)))->brinVersion == \
													BRIN_CURRENT_VERSION && \
		 ((BrinMetaPageData *) PageGetContents((pagedata)))->brinMagic == \
															BRIN_META_MAGIC)

/* ------ SP-GiST index related definitions --------- */

/*
 * Each backend keeps a cache of last-used page info in its index->rd_amcache
 * area.  This is initialized from, and occasionally written back to,
 * shared storage in the index metapage.
 */
typedef struct SpGistLastUsedPage
{
	BlockNumber blkno;			/* block number, or InvalidBlockNumber */
	int			freeSpace;		/* page's free space (could be obsolete!) */
} SpGistLastUsedPage;

/* Note: indexes in cachedPage[] match flag assignments for SpGistGetBuffer */
#define SPGIST_CACHED_PAGES 8

typedef struct SpGistLUPCache
{
	SpGistLastUsedPage cachedPage[SPGIST_CACHED_PAGES];
} SpGistLUPCache;

typedef struct SpGistMetaPageData
{
	uint32		magicNumber;	/* for identity cross-check */
	SpGistLUPCache lastUsedPages;	/* shared storage of last-used info */
} SpGistMetaPageData;

#define SPGIST_MAGIC_NUMBER (0xBA0BABEE)
#define SPGIST_METAPAGE_BLKNO	0

#define IS_SPGIST_INDEX_METAPAGE(blkno, pagedata) \
		((blkno) == SPGIST_METAPAGE_BLKNO && \
	 ((SpGistMetaPageData *) PageGetContents((pagedata)))->magicNumber == \
														SPGIST_MAGIC_NUMBER)

#endif	/*IDXPAGEHDR_H */
