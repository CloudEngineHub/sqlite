/*
** 2014 May 31
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
******************************************************************************
**
** Low level access to the FTS index stored in the database file. The 
** routines in this file file implement all read and write access to the
** %_data table. Other parts of the system access this functionality via
** the interface defined in fts5Int.h.
*/


#include "fts5Int.h"

/*
** Overview:
**
** The %_data table contains all the FTS indexes for an FTS5 virtual table.
** As well as the main term index, there may be up to 31 prefix indexes.
** The format is similar to FTS3/4, except that:
**
**   * all segment b-tree leaf data is stored in fixed size page records 
**     (e.g. 1000 bytes). A single doclist may span multiple pages. Care is 
**     taken to ensure it is possible to iterate in either direction through 
**     the entries in a doclist, or to seek to a specific entry within a 
**     doclist, without loading it into memory.
**
**   * large doclists that span many pages have associated "doclist index"
**     records that contain a copy of the first rowid on each page spanned by
**     the doclist. This is used to speed up seek operations, and merges of
**     large doclists with very small doclists.
**
**   * extra fields in the "structure record" record the state of ongoing
**     incremental merge operations.
**
*/


#define FTS5_OPT_WORK_UNIT  1000  /* Number of leaf pages per optimize step */
#define FTS5_WORK_UNIT      64    /* Number of leaf pages in unit of work */

#define FTS5_MIN_DLIDX_SIZE 4     /* Add dlidx if this many empty pages */

#define FTS5_MAIN_PREFIX '0'

#if FTS5_MAX_PREFIX_INDEXES > 31
# error "FTS5_MAX_PREFIX_INDEXES is too large"
#endif

#define FTS5_MAX_LEVEL 64

/*
** There are two versions of the format used for the structure record:
**
**   1. the legacy format, that may be read by all fts5 versions, and
**
**   2. the V2 format, which is used by contentless_delete=1 databases.
**
** Both begin with a 4-byte "configuration cookie" value. Then, a legacy
** format structure record contains a varint - the number of levels in
** the structure. Whereas a V2 structure record contains the constant
** 4 bytes [0xff 0x00 0x00 0x01]. This is unambiguous as the value of a
** varint has to be at least 16256 to begin with "0xFF". And the default
** maximum number of levels is 64. 
**
** See below for more on structure record formats.
*/
#define FTS5_STRUCTURE_V2 "\xFF\x00\x00\x01"

/*
** Details:
**
** The %_data table managed by this module,
**
**     CREATE TABLE %_data(id INTEGER PRIMARY KEY, block BLOB);
**
** , contains the following 6 types of records. See the comments surrounding
** the FTS5_*_ROWID macros below for a description of how %_data rowids are 
** assigned to each fo them.
**
** 1. Structure Records:
**
**   The set of segments that make up an index - the index structure - are
**   recorded in a single record within the %_data table. The record consists
**   of a single 32-bit configuration cookie value followed by a list of
**   SQLite varints. 
**
**   If the structure record is a V2 record, the configuration cookie is 
**   followed by the following 4 bytes: [0xFF 0x00 0x00 0x01]. 
**
**   Next, the record continues with three varints:
**
**     + number of levels,
**     + total number of segments on all levels,
**     + value of write counter.
**
**   Then, for each level from 0 to nMax:
**
**     + number of input segments in ongoing merge.
**     + total number of segments in level.
**     + for each segment from oldest to newest:
**         + segment id (always > 0)
**         + first leaf page number (often 1, always greater than 0)
**         + final leaf page number
**
**      Then, for V2 structures only:
**
**         + lower origin counter value,
**         + upper origin counter value,
**         + the number of tombstone hash pages.
**
** 2. The Averages Record:
**
**   A single record within the %_data table. The data is a list of varints.
**   The first value is the number of rows in the index. Then, for each column
**   from left to right, the total number of tokens in the column for all
**   rows of the table.
**
** 3. Segment leaves:
**
**   TERM/DOCLIST FORMAT:
**
**     Most of each segment leaf is taken up by term/doclist data. The 
**     general format of term/doclist, starting with the first term
**     on the leaf page, is:
**
**         varint : size of first term
**         blob:    first term data
**         doclist: first doclist
**         zero-or-more {
**           varint:  number of bytes in common with previous term
**           varint:  number of bytes of new term data (nNew)
**           blob:    nNew bytes of new term data
**           doclist: next doclist
**         }
**
**     doclist format:
**
**         varint:  first rowid
**         poslist: first poslist
**         zero-or-more {
**           varint:  rowid delta (always > 0)
**           poslist: next poslist
**         }
**
**     poslist format:
**
**         varint: size of poslist in bytes multiplied by 2, not including
**                 this field. Plus 1 if this entry carries the "delete" flag.
**         collist: collist for column 0
**         zero-or-more {
**           0x01 byte
**           varint: column number (I)
**           collist: collist for column I
**         }
**
**     collist format:
**
**         varint: first offset + 2
**         zero-or-more {
**           varint: offset delta + 2
**         }
**
**   PAGE FORMAT
**
**     Each leaf page begins with a 4-byte header containing 2 16-bit 
**     unsigned integer fields in big-endian format. They are:
**
**       * The byte offset of the first rowid on the page, if it exists
**         and occurs before the first term (otherwise 0).
**
**       * The byte offset of the start of the page footer. If the page
**         footer is 0 bytes in size, then this field is the same as the
**         size of the leaf page in bytes.
**
**     The page footer consists of a single varint for each term located
**     on the page. Each varint is the byte offset of the current term
**     within the page, delta-compressed against the previous value. In
**     other words, the first varint in the footer is the byte offset of
**     the first term, the second is the byte offset of the second less that
**     of the first, and so on.
**
**     The term/doclist format described above is accurate if the entire
**     term/doclist data fits on a single leaf page. If this is not the case,
**     the format is changed in two ways:
**
**       + if the first rowid on a page occurs before the first term, it
**         is stored as a literal value:
**
**             varint:  first rowid
**
**       + the first term on each page is stored in the same way as the
**         very first term of the segment:
**
**             varint : size of first term
**             blob:    first term data
**
** 5. Segment doclist indexes:
**
**   Doclist indexes are themselves b-trees, however they usually consist of
**   a single leaf record only. The format of each doclist index leaf page 
**   is:
**
**     * Flags byte. Bits are:
**         0x01: Clear if leaf is also the root page, otherwise set.
**
**     * Page number of fts index leaf page. As a varint.
**
**     * First rowid on page indicated by previous field. As a varint.
**
**     * A list of varints, one for each subsequent termless page. A 
**       positive delta if the termless page contains at least one rowid, 
**       or an 0x00 byte otherwise.
**
**   Internal doclist index nodes are:
**
**     * Flags byte. Bits are:
**         0x01: Clear for root page, otherwise set.
**
**     * Page number of first child page. As a varint.
**
**     * Copy of first rowid on page indicated by previous field. As a varint.
**
**     * A list of delta-encoded varints - the first rowid on each subsequent
**       child page. 
**
** 6. Tombstone Hash Page
**
**   These records are only ever present in contentless_delete=1 tables. 
**   There are zero or more of these associated with each segment. They
**   are used to store the tombstone rowids for rows contained in the
**   associated segments.
**
**   The set of nHashPg tombstone hash pages associated with a single 
**   segment together form a single hash table containing tombstone rowids.
**   To find the page of the hash on which a key might be stored:
**
**       iPg = (rowid % nHashPg)
**
**   Then, within page iPg, which has nSlot slots:
**
**       iSlot = (rowid / nHashPg) % nSlot
**
**   Each tombstone hash page begins with an 8 byte header: 
**
**     1-byte:  Key-size (the size in bytes of each slot). Either 4 or 8.
**     1-byte:  rowid-0-tombstone flag. This flag is only valid on the 
**              first tombstone hash page for each segment (iPg=0). If set,
**              the hash table contains rowid 0. If clear, it does not.
**              Rowid 0 is handled specially.
**     2-bytes: unused.
**     4-bytes: Big-endian integer containing number of entries on page.
**
**   Following this are nSlot 4 or 8 byte slots (depending on the key-size
**   in the first byte of the page header). The number of slots may be
**   determined based on the size of the page record and the key-size:
**
**     nSlot = (nByte - 8) / key-size
*/

/*
** Rowids for the averages and structure records in the %_data table.
*/
#define FTS5_AVERAGES_ROWID     1    /* Rowid used for the averages record */
#define FTS5_STRUCTURE_ROWID   10    /* The structure record */

/*
** Macros determining the rowids used by segment leaves and dlidx leaves
** and nodes. All nodes and leaves are stored in the %_data table with large
** positive rowids.
**
** Each segment has a unique non-zero 16-bit id.
**
** The rowid for each segment leaf is found by passing the segment id and 
** the leaf page number to the FTS5_SEGMENT_ROWID macro. Leaves are numbered
** sequentially starting from 1.
*/
#define FTS5_DATA_ID_B     16     /* Max seg id number 65535 */
#define FTS5_DATA_DLI_B     1     /* Doclist-index flag (1 bit) */
#define FTS5_DATA_HEIGHT_B  5     /* Max dlidx tree height of 32 */
#define FTS5_DATA_PAGE_B   31     /* Max page number of 2147483648 */

#define fts5_dri(segid, dlidx, height, pgno) (                                 \
 ((i64)(segid)  << (FTS5_DATA_PAGE_B+FTS5_DATA_HEIGHT_B+FTS5_DATA_DLI_B)) +    \
 ((i64)(dlidx)  << (FTS5_DATA_PAGE_B + FTS5_DATA_HEIGHT_B)) +                  \
 ((i64)(height) << (FTS5_DATA_PAGE_B)) +                                       \
 ((i64)(pgno))                                                                 \
)

#define FTS5_SEGMENT_ROWID(segid, pgno)       fts5_dri(segid, 0, 0, pgno)
#define FTS5_DLIDX_ROWID(segid, height, pgno) fts5_dri(segid, 1, height, pgno)
#define FTS5_TOMBSTONE_ROWID(segid,ipg)       fts5_dri(segid+(1<<16), 0, 0, ipg)

#ifdef SQLITE_DEBUG
int sqlite3Fts5Corrupt() { return SQLITE_CORRUPT_VTAB; }
#endif


/*
** Each time a blob is read from the %_data table, it is padded with this
** many zero bytes. This makes it easier to decode the various record formats
** without overreading if the records are corrupt.
*/
#define FTS5_DATA_ZERO_PADDING 8
#define FTS5_DATA_PADDING 20

typedef struct Fts5Data Fts5Data;
typedef struct Fts5DlidxIter Fts5DlidxIter;
typedef struct Fts5DlidxLvl Fts5DlidxLvl;
typedef struct Fts5DlidxWriter Fts5DlidxWriter;
typedef struct Fts5Iter Fts5Iter;
typedef struct Fts5PageWriter Fts5PageWriter;
typedef struct Fts5SegIter Fts5SegIter;
typedef struct Fts5DoclistIter Fts5DoclistIter;
typedef struct Fts5SegWriter Fts5SegWriter;
typedef struct Fts5Structure Fts5Structure;
typedef struct Fts5StructureLevel Fts5StructureLevel;
typedef struct Fts5StructureSegment Fts5StructureSegment;
typedef struct Fts5TokenDataIter Fts5TokenDataIter;
typedef struct Fts5TokenDataMap Fts5TokenDataMap;
typedef struct Fts5TombstoneArray Fts5TombstoneArray;

struct Fts5Data {
  u8 *p;                          /* Pointer to buffer containing record */
  int nn;                         /* Size of record in bytes */
  int szLeaf;                     /* Size of leaf without page-index */
};

/*
** One object per %_data table.
**
** nContentlessDelete:
**   The number of contentless delete operations since the most recent
**   call to fts5IndexFlush() or fts5IndexDiscardData(). This is tracked
**   so that extra auto-merge work can be done by fts5IndexFlush() to
**   account for the delete operations.
*/
struct Fts5Index {
  Fts5Config *pConfig;            /* Virtual table configuration */
  char *zDataTbl;                 /* Name of %_data table */
  int nWorkUnit;                  /* Leaf pages in a "unit" of work */

  /*
  ** Variables related to the accumulation of tokens and doclists within the
  ** in-memory hash tables before they are flushed to disk.
  */
  Fts5Hash *pHash;                /* Hash table for in-memory data */
  int nPendingData;               /* Current bytes of pending data */
  i64 iWriteRowid;                /* Rowid for current doc being written */
  int bDelete;                    /* Current write is a delete */
  int nContentlessDelete;         /* Number of contentless delete ops */
  int nPendingRow;                /* Number of INSERT in hash table */

  /* Error state. */
  int rc;                         /* Current error code */
  int flushRc;

  /* State used by the fts5DataXXX() functions. */
  sqlite3_blob *pReader;          /* RO incr-blob open on %_data table */
  sqlite3_stmt *pWriter;          /* "INSERT ... %_data VALUES(?,?)" */
  sqlite3_stmt *pDeleter;         /* "DELETE FROM %_data ... id>=? AND id<=?" */
  sqlite3_stmt *pIdxWriter;       /* "INSERT ... %_idx VALUES(?,?,?,?)" */
  sqlite3_stmt *pIdxDeleter;      /* "DELETE FROM %_idx WHERE segid=?" */
  sqlite3_stmt *pIdxSelect;
  sqlite3_stmt *pIdxNextSelect;
  int nRead;                      /* Total number of blocks read */

  sqlite3_stmt *pDeleteFromIdx;

  sqlite3_stmt *pDataVersion;
  i64 iStructVersion;             /* data_version when pStruct read */
  Fts5Structure *pStruct;         /* Current db structure (or NULL) */
};

struct Fts5DoclistIter {
  u8 *aEof;                       /* Pointer to 1 byte past end of doclist */

  /* Output variables. aPoslist==0 at EOF */
  i64 iRowid;
  u8 *aPoslist;
  int nPoslist;
  int nSize;
};

/*
** The contents of the "structure" record for each index are represented
** using an Fts5Structure record in memory. Which uses instances of the 
** other Fts5StructureXXX types as components.
**
** nOriginCntr:
**   This value is set to non-zero for structure records created for
**   contentlessdelete=1 tables only. In that case it represents the
**   origin value to apply to the next top-level segment created.
*/
struct Fts5StructureSegment {
  int iSegid;                     /* Segment id */
  int pgnoFirst;                  /* First leaf page number in segment */
  int pgnoLast;                   /* Last leaf page number in segment */

  /* contentlessdelete=1 tables only: */
  u64 iOrigin1;
  u64 iOrigin2;
  int nPgTombstone;               /* Number of tombstone hash table pages */
  u64 nEntryTombstone;            /* Number of tombstone entries that "count" */
  u64 nEntry;                     /* Number of rows in this segment */
};
struct Fts5StructureLevel {
  int nMerge;                     /* Number of segments in incr-merge */
  int nSeg;                       /* Total number of segments on level */
  Fts5StructureSegment *aSeg;     /* Array of segments. aSeg[0] is oldest. */
};
struct Fts5Structure {
  int nRef;                       /* Object reference count */
  u64 nWriteCounter;              /* Total leaves written to level 0 */
  u64 nOriginCntr;                /* Origin value for next top-level segment */
  int nSegment;                   /* Total segments in this structure */
  int nLevel;                     /* Number of levels in this index */
  Fts5StructureLevel aLevel[FLEXARRAY]; /* Array of nLevel level objects */
};

/* Size (in bytes) of an Fts5Structure object holding up to N levels */
#define SZ_FTS5STRUCTURE(N) \
         (offsetof(Fts5Structure,aLevel) + (N)*sizeof(Fts5StructureLevel))

/*
** An object of type Fts5SegWriter is used to write to segments.
*/
struct Fts5PageWriter {
  int pgno;                       /* Page number for this page */
  int iPrevPgidx;                 /* Previous value written into pgidx */
  Fts5Buffer buf;                 /* Buffer containing leaf data */
  Fts5Buffer pgidx;               /* Buffer containing page-index */
  Fts5Buffer term;                /* Buffer containing previous term on page */
};
struct Fts5DlidxWriter {
  int pgno;                       /* Page number for this page */
  int bPrevValid;                 /* True if iPrev is valid */
  i64 iPrev;                      /* Previous rowid value written to page */
  Fts5Buffer buf;                 /* Buffer containing page data */
};
struct Fts5SegWriter {
  int iSegid;                     /* Segid to write to */
  Fts5PageWriter writer;          /* PageWriter object */
  i64 iPrevRowid;                 /* Previous rowid written to current leaf */
  u8 bFirstRowidInDoclist;        /* True if next rowid is first in doclist */
  u8 bFirstRowidInPage;           /* True if next rowid is first in page */
  /* TODO1: Can use (writer.pgidx.n==0) instead of bFirstTermInPage */
  u8 bFirstTermInPage;            /* True if next term will be first in leaf */
  int nLeafWritten;               /* Number of leaf pages written */
  int nEmpty;                     /* Number of contiguous term-less nodes */

  int nDlidx;                     /* Allocated size of aDlidx[] array */
  Fts5DlidxWriter *aDlidx;        /* Array of Fts5DlidxWriter objects */

  /* Values to insert into the %_idx table */
  Fts5Buffer btterm;              /* Next term to insert into %_idx table */
  int iBtPage;                    /* Page number corresponding to btterm */
};

typedef struct Fts5CResult Fts5CResult;
struct Fts5CResult {
  u16 iFirst;                     /* aSeg[] index of firstest iterator */
  u8 bTermEq;                     /* True if the terms are equal */
};

/*
** Object for iterating through a single segment, visiting each term/rowid
** pair in the segment.
**
** pSeg:
**   The segment to iterate through.
**
** iLeafPgno:
**   Current leaf page number within segment.
**
** iLeafOffset:
**   Byte offset within the current leaf that is the first byte of the 
**   position list data (one byte passed the position-list size field).
**
** pLeaf:
**   Buffer containing current leaf page data. Set to NULL at EOF.
**
** iTermLeafPgno, iTermLeafOffset:
**   Leaf page number containing the last term read from the segment. And
**   the offset immediately following the term data.
**
** flags:
**   Mask of FTS5_SEGITER_XXX values. Interpreted as follows:
**
**   FTS5_SEGITER_ONETERM:
**     If set, set the iterator to point to EOF after the current doclist 
**     has been exhausted. Do not proceed to the next term in the segment.
**
**   FTS5_SEGITER_REVERSE:
**     This flag is only ever set if FTS5_SEGITER_ONETERM is also set. If
**     it is set, iterate through rowid in descending order instead of the
**     default ascending order.
**
** iRowidOffset/nRowidOffset/aRowidOffset:
**     These are used if the FTS5_SEGITER_REVERSE flag is set.
**
**     For each rowid on the page corresponding to the current term, the
**     corresponding aRowidOffset[] entry is set to the byte offset of the
**     start of the "position-list-size" field within the page.
**
** iTermIdx:
**     Index of current term on iTermLeafPgno.
**
** apTombstone/nTombstone:
**     These are used for contentless_delete=1 tables only. When the cursor
**     is first allocated, the apTombstone[] array is allocated so that it
**     is large enough for all tombstones hash pages associated with the
**     segment. The pages themselves are loaded lazily from the database as
**     they are required.
*/
struct Fts5SegIter {
  Fts5StructureSegment *pSeg;     /* Segment to iterate through */
  int flags;                      /* Mask of configuration flags */
  int iLeafPgno;                  /* Current leaf page number */
  Fts5Data *pLeaf;                /* Current leaf data */
  Fts5Data *pNextLeaf;            /* Leaf page (iLeafPgno+1) */
  i64 iLeafOffset;                /* Byte offset within current leaf */
  Fts5TombstoneArray *pTombArray; /* Array of tombstone pages */

  /* Next method */
  void (*xNext)(Fts5Index*, Fts5SegIter*, int*);

  /* The page and offset from which the current term was read. The offset 
  ** is the offset of the first rowid in the current doclist.  */
  int iTermLeafPgno;
  int iTermLeafOffset;

  int iPgidxOff;                  /* Next offset in pgidx */
  int iEndofDoclist;

  /* The following are only used if the FTS5_SEGITER_REVERSE flag is set. */
  int iRowidOffset;               /* Current entry in aRowidOffset[] */
  int nRowidOffset;               /* Allocated size of aRowidOffset[] array */
  int *aRowidOffset;              /* Array of offset to rowid fields */

  Fts5DlidxIter *pDlidx;          /* If there is a doclist-index */

  /* Variables populated based on current entry. */
  Fts5Buffer term;                /* Current term */
  i64 iRowid;                     /* Current rowid */
  int nPos;                       /* Number of bytes in current position list */
  u8 bDel;                        /* True if the delete flag is set */
};

static int fts5IndexCorruptRowid(Fts5Index *pIdx, i64 iRowid){
  pIdx->rc = FTS5_CORRUPT;
  sqlite3Fts5ConfigErrmsg(pIdx->pConfig, 
      "fts5: corruption found reading blob %lld from table \"%s\"", 
      iRowid, pIdx->pConfig->zName
  );
  return SQLITE_CORRUPT_VTAB;
}
#define FTS5_CORRUPT_ROWID(pIdx, iRowid) fts5IndexCorruptRowid(pIdx, iRowid)

static int fts5IndexCorruptIter(Fts5Index *pIdx, Fts5SegIter *pIter){
  pIdx->rc = FTS5_CORRUPT;
  sqlite3Fts5ConfigErrmsg(pIdx->pConfig, 
      "fts5: corruption on page %d, segment %d, table \"%s\"", 
      pIter->iLeafPgno, pIter->pSeg->iSegid, pIdx->pConfig->zName
  );
  return SQLITE_CORRUPT_VTAB;
}
#define FTS5_CORRUPT_ITER(pIdx, pIter) fts5IndexCorruptIter(pIdx, pIter)

static int fts5IndexCorruptIdx(Fts5Index *pIdx){
  pIdx->rc = FTS5_CORRUPT;
  sqlite3Fts5ConfigErrmsg(pIdx->pConfig, 
      "fts5: corruption in table \"%s\"", pIdx->pConfig->zName
  );
  return SQLITE_CORRUPT_VTAB;
}
#define FTS5_CORRUPT_IDX(pIdx) fts5IndexCorruptIdx(pIdx)


/*
** Array of tombstone pages. Reference counted.
*/
struct Fts5TombstoneArray {
  int nRef;                         /* Number of pointers to this object */
  int nTombstone;
  Fts5Data *apTombstone[FLEXARRAY]; /* Array of tombstone pages */
};

/* Size (in bytes) of an Fts5TombstoneArray holding up to N tombstones */
#define SZ_FTS5TOMBSTONEARRAY(N) \
  (offsetof(Fts5TombstoneArray,apTombstone)+(N)*sizeof(Fts5Data*))

/*
** Argument is a pointer to an Fts5Data structure that contains a 
** leaf page.
*/
#define ASSERT_SZLEAF_OK(x) assert( \
    (x)->szLeaf==(x)->nn || (x)->szLeaf==fts5GetU16(&(x)->p[2]) \
)

#define FTS5_SEGITER_ONETERM 0x01
#define FTS5_SEGITER_REVERSE 0x02

/* 
** Argument is a pointer to an Fts5Data structure that contains a leaf
** page. This macro evaluates to true if the leaf contains no terms, or
** false if it contains at least one term.
*/
#define fts5LeafIsTermless(x) ((x)->szLeaf >= (x)->nn)

#define fts5LeafTermOff(x, i) (fts5GetU16(&(x)->p[(x)->szLeaf + (i)*2]))

#define fts5LeafFirstRowidOff(x) (fts5GetU16((x)->p))

/*
** Object for iterating through the merged results of one or more segments,
** visiting each term/rowid pair in the merged data.
**
** nSeg is always a power of two greater than or equal to the number of
** segments that this object is merging data from. Both the aSeg[] and
** aFirst[] arrays are sized at nSeg entries. The aSeg[] array is padded
** with zeroed objects - these are handled as if they were iterators opened
** on empty segments.
**
** The results of comparing segments aSeg[N] and aSeg[N+1], where N is an
** even number, is stored in aFirst[(nSeg+N)/2]. The "result" of the 
** comparison in this context is the index of the iterator that currently
** points to the smaller term/rowid combination. Iterators at EOF are
** considered to be greater than all other iterators.
**
** aFirst[1] contains the index in aSeg[] of the iterator that points to
** the smallest key overall. aFirst[0] is unused. 
**
** poslist:
**   Used by sqlite3Fts5IterPoslist() when the poslist needs to be buffered.
**   There is no way to tell if this is populated or not.
**
** pColset:
**   If not NULL, points to an object containing a set of column indices.
**   Only matches that occur in one of these columns will be returned.
**   The Fts5Iter does not own the Fts5Colset object, and so it is not
**   freed when the iterator is closed - it is owned by the upper layer.
*/
struct Fts5Iter {
  Fts5IndexIter base;             /* Base class containing output vars */
  Fts5TokenDataIter *pTokenDataIter;

  Fts5Index *pIndex;              /* Index that owns this iterator */
  Fts5Buffer poslist;             /* Buffer containing current poslist */
  Fts5Colset *pColset;            /* Restrict matches to these columns */

  /* Invoked to set output variables. */
  void (*xSetOutputs)(Fts5Iter*, Fts5SegIter*);

  int nSeg;                       /* Size of aSeg[] array */
  int bRev;                       /* True to iterate in reverse order */
  u8 bSkipEmpty;                  /* True to skip deleted entries */

  i64 iSwitchRowid;               /* Firstest rowid of other than aFirst[1] */
  Fts5CResult *aFirst;            /* Current merge state (see above) */
  Fts5SegIter aSeg[FLEXARRAY];    /* Array of segment iterators */
};

/* Size (in bytes) of an Fts5Iter object holding up to N segment iterators */
#define SZ_FTS5ITER(N)  (offsetof(Fts5Iter,aSeg)+(N)*sizeof(Fts5SegIter))

/*
** An instance of the following type is used to iterate through the contents
** of a doclist-index record.
**
** pData:
**   Record containing the doclist-index data.
**
** bEof:
**   Set to true once iterator has reached EOF.
**
** iOff:
**   Set to the current offset within record pData.
*/
struct Fts5DlidxLvl {
  Fts5Data *pData;              /* Data for current page of this level */
  int iOff;                     /* Current offset into pData */
  int bEof;                     /* At EOF already */
  int iFirstOff;                /* Used by reverse iterators */

  /* Output variables */
  int iLeafPgno;                /* Page number of current leaf page */
  i64 iRowid;                   /* First rowid on leaf iLeafPgno */
};
struct Fts5DlidxIter {
  int nLvl;
  int iSegid;
  Fts5DlidxLvl aLvl[FLEXARRAY];
};

/* Size (in bytes) of an Fts5DlidxIter object with up to N levels */
#define SZ_FTS5DLIDXITER(N) \
          (offsetof(Fts5DlidxIter,aLvl)+(N)*sizeof(Fts5DlidxLvl))

static void fts5PutU16(u8 *aOut, u16 iVal){
  aOut[0] = (iVal>>8);
  aOut[1] = (iVal&0xFF);
}

static u16 fts5GetU16(const u8 *aIn){
  return ((u16)aIn[0] << 8) + aIn[1];
} 

/*
** The only argument points to a buffer at least 8 bytes in size. This
** function interprets the first 8 bytes of the buffer as a 64-bit big-endian
** unsigned integer and returns the result.
*/
static u64 fts5GetU64(u8 *a){
  return ((u64)a[0] << 56)
       + ((u64)a[1] << 48)
       + ((u64)a[2] << 40)
       + ((u64)a[3] << 32)
       + ((u64)a[4] << 24)
       + ((u64)a[5] << 16)
       + ((u64)a[6] << 8)
       + ((u64)a[7] << 0);
}

/*
** The only argument points to a buffer at least 4 bytes in size. This
** function interprets the first 4 bytes of the buffer as a 32-bit big-endian
** unsigned integer and returns the result.
*/
static u32 fts5GetU32(const u8 *a){
  return ((u32)a[0] << 24)
       + ((u32)a[1] << 16)
       + ((u32)a[2] << 8)
       + ((u32)a[3] << 0);
} 

/*
** Write iVal, formated as a 64-bit big-endian unsigned integer, to the
** buffer indicated by the first argument.
*/
static void fts5PutU64(u8 *a, u64 iVal){
  a[0] = ((iVal >> 56) & 0xFF);
  a[1] = ((iVal >> 48) & 0xFF);
  a[2] = ((iVal >> 40) & 0xFF);
  a[3] = ((iVal >> 32) & 0xFF);
  a[4] = ((iVal >> 24) & 0xFF);
  a[5] = ((iVal >> 16) & 0xFF);
  a[6] = ((iVal >>  8) & 0xFF);
  a[7] = ((iVal >>  0) & 0xFF);
}

/*
** Write iVal, formated as a 32-bit big-endian unsigned integer, to the
** buffer indicated by the first argument.
*/
static void fts5PutU32(u8 *a, u32 iVal){
  a[0] = ((iVal >> 24) & 0xFF);
  a[1] = ((iVal >> 16) & 0xFF);
  a[2] = ((iVal >>  8) & 0xFF);
  a[3] = ((iVal >>  0) & 0xFF);
}

/*
** Allocate and return a buffer at least nByte bytes in size.
**
** If an OOM error is encountered, return NULL and set the error code in
** the Fts5Index handle passed as the first argument.
*/
static void *fts5IdxMalloc(Fts5Index *p, sqlite3_int64 nByte){
  return sqlite3Fts5MallocZero(&p->rc, nByte);
}

/*
** Compare the contents of the pLeft buffer with the pRight/nRight blob.
**
** Return -ve if pLeft is smaller than pRight, 0 if they are equal or
** +ve if pRight is smaller than pLeft. In other words:
**
**     res = *pLeft - *pRight
*/
#ifdef SQLITE_DEBUG
static int fts5BufferCompareBlob(
  Fts5Buffer *pLeft,              /* Left hand side of comparison */
  const u8 *pRight, int nRight    /* Right hand side of comparison */
){
  int nCmp = MIN(pLeft->n, nRight);
  int res = memcmp(pLeft->p, pRight, nCmp);
  return (res==0 ? (pLeft->n - nRight) : res);
}
#endif

/*
** Compare the contents of the two buffers using memcmp(). If one buffer
** is a prefix of the other, it is considered the lesser.
**
** Return -ve if pLeft is smaller than pRight, 0 if they are equal or
** +ve if pRight is smaller than pLeft. In other words:
**
**     res = *pLeft - *pRight
*/
static int fts5BufferCompare(Fts5Buffer *pLeft, Fts5Buffer *pRight){
  int nCmp, res;
  nCmp = MIN(pLeft->n, pRight->n);
  assert( nCmp<=0 || pLeft->p!=0 );
  assert( nCmp<=0 || pRight->p!=0 );
  res = fts5Memcmp(pLeft->p, pRight->p, nCmp);
  return (res==0 ? (pLeft->n - pRight->n) : res);
}

static int fts5LeafFirstTermOff(Fts5Data *pLeaf){
  int ret;
  fts5GetVarint32(&pLeaf->p[pLeaf->szLeaf], ret);
  return ret;
}

/*
** Close the read-only blob handle, if it is open.
*/
static void fts5IndexCloseReader(Fts5Index *p){
  if( p->pReader ){
    int rc;
    sqlite3_blob *pReader = p->pReader;
    p->pReader = 0;
    rc = sqlite3_blob_close(pReader);
    if( p->rc==SQLITE_OK ) p->rc = rc;
  }
}

/*
** Retrieve a record from the %_data table.
**
** If an error occurs, NULL is returned and an error left in the 
** Fts5Index object.
*/
static Fts5Data *fts5DataRead(Fts5Index *p, i64 iRowid){
  Fts5Data *pRet = 0;
  if( p->rc==SQLITE_OK ){
    int rc = SQLITE_OK;

    if( p->pReader ){
      /* This call may return SQLITE_ABORT if there has been a savepoint
      ** rollback since it was last used. In this case a new blob handle
      ** is required.  */
      sqlite3_blob *pBlob = p->pReader;
      p->pReader = 0;
      rc = sqlite3_blob_reopen(pBlob, iRowid);
      assert( p->pReader==0 );
      p->pReader = pBlob;
      if( rc!=SQLITE_OK ){
        fts5IndexCloseReader(p);
      }
      if( rc==SQLITE_ABORT ) rc = SQLITE_OK;
    }

    /* If the blob handle is not open at this point, open it and seek 
    ** to the requested entry.  */
    if( p->pReader==0 && rc==SQLITE_OK ){
      Fts5Config *pConfig = p->pConfig;
      rc = sqlite3_blob_open(pConfig->db, 
          pConfig->zDb, p->zDataTbl, "block", iRowid, 0, &p->pReader
      );
    }

    /* If either of the sqlite3_blob_open() or sqlite3_blob_reopen() calls
    ** above returned SQLITE_ERROR, return SQLITE_CORRUPT_VTAB instead.
    ** All the reasons those functions might return SQLITE_ERROR - missing
    ** table, missing row, non-blob/text in block column - indicate 
    ** backing store corruption.  */
    if( rc==SQLITE_ERROR ) rc = FTS5_CORRUPT_ROWID(p, iRowid);

    if( rc==SQLITE_OK ){
      u8 *aOut = 0;               /* Read blob data into this buffer */
      int nByte = sqlite3_blob_bytes(p->pReader);
      int szData = (sizeof(Fts5Data) + 7) & ~7;
      sqlite3_int64 nAlloc = szData + nByte + FTS5_DATA_PADDING;
      pRet = (Fts5Data*)sqlite3_malloc64(nAlloc);
      if( pRet ){
        pRet->nn = nByte;
        aOut = pRet->p = (u8*)pRet + szData;
      }else{
        rc = SQLITE_NOMEM;
      }

      if( rc==SQLITE_OK ){
        rc = sqlite3_blob_read(p->pReader, aOut, nByte, 0);
      }
      if( rc!=SQLITE_OK ){
        sqlite3_free(pRet);
        pRet = 0;
      }else{
        /* TODO1: Fix this */
        pRet->p[nByte] = 0x00;
        pRet->p[nByte+1] = 0x00;
        pRet->szLeaf = fts5GetU16(&pRet->p[2]);
      }
    }
    p->rc = rc;
    p->nRead++;
  }

  assert( (pRet==0)==(p->rc!=SQLITE_OK) );
  assert( pRet==0 || EIGHT_BYTE_ALIGNMENT( pRet->p ) );
  return pRet;
}


/*
** Release a reference to data record returned by an earlier call to
** fts5DataRead().
*/
static void fts5DataRelease(Fts5Data *pData){
  sqlite3_free(pData);
}

static Fts5Data *fts5LeafRead(Fts5Index *p, i64 iRowid){
  Fts5Data *pRet = fts5DataRead(p, iRowid);
  if( pRet ){
    if( pRet->nn<4 || pRet->szLeaf>pRet->nn ){
      FTS5_CORRUPT_ROWID(p, iRowid);
      fts5DataRelease(pRet);
      pRet = 0;
    }
  }
  return pRet;
}

static int fts5IndexPrepareStmt(
  Fts5Index *p,
  sqlite3_stmt **ppStmt,
  char *zSql
){
  if( p->rc==SQLITE_OK ){
    if( zSql ){
      int rc = sqlite3_prepare_v3(p->pConfig->db, zSql, -1,
          SQLITE_PREPARE_PERSISTENT|SQLITE_PREPARE_NO_VTAB,
          ppStmt, 0);
      /* If this prepare() call fails with SQLITE_ERROR, then one of the
      ** %_idx or %_data tables has been removed or modified. Call this
      ** corruption.  */
      p->rc = (rc==SQLITE_ERROR ? SQLITE_CORRUPT : rc);
    }else{
      p->rc = SQLITE_NOMEM;
    }
  }
  sqlite3_free(zSql);
  return p->rc;
}


/*
** INSERT OR REPLACE a record into the %_data table.
*/
static void fts5DataWrite(Fts5Index *p, i64 iRowid, const u8 *pData, int nData){
  if( p->rc!=SQLITE_OK ) return;

  if( p->pWriter==0 ){
    Fts5Config *pConfig = p->pConfig;
    fts5IndexPrepareStmt(p, &p->pWriter, sqlite3_mprintf(
          "REPLACE INTO '%q'.'%q_data'(id, block) VALUES(?,?)", 
          pConfig->zDb, pConfig->zName
    ));
    if( p->rc ) return;
  }

  sqlite3_bind_int64(p->pWriter, 1, iRowid);
  sqlite3_bind_blob(p->pWriter, 2, pData, nData, SQLITE_STATIC);
  sqlite3_step(p->pWriter);
  p->rc = sqlite3_reset(p->pWriter);
  sqlite3_bind_null(p->pWriter, 2);
}

/*
** Execute the following SQL:
**
**     DELETE FROM %_data WHERE id BETWEEN $iFirst AND $iLast
*/
static void fts5DataDelete(Fts5Index *p, i64 iFirst, i64 iLast){
  if( p->rc!=SQLITE_OK ) return;

  if( p->pDeleter==0 ){
    Fts5Config *pConfig = p->pConfig;
    char *zSql = sqlite3_mprintf(
        "DELETE FROM '%q'.'%q_data' WHERE id>=? AND id<=?", 
          pConfig->zDb, pConfig->zName
    );
    if( fts5IndexPrepareStmt(p, &p->pDeleter, zSql) ) return;
  }

  sqlite3_bind_int64(p->pDeleter, 1, iFirst);
  sqlite3_bind_int64(p->pDeleter, 2, iLast);
  sqlite3_step(p->pDeleter);
  p->rc = sqlite3_reset(p->pDeleter);
}

/*
** Remove all records associated with segment iSegid.
*/
static void fts5DataRemoveSegment(Fts5Index *p, Fts5StructureSegment *pSeg){
  int iSegid = pSeg->iSegid;
  i64 iFirst = FTS5_SEGMENT_ROWID(iSegid, 0);
  i64 iLast = FTS5_SEGMENT_ROWID(iSegid+1, 0)-1;
  fts5DataDelete(p, iFirst, iLast);

  if( pSeg->nPgTombstone ){
    i64 iTomb1 = FTS5_TOMBSTONE_ROWID(iSegid, 0);
    i64 iTomb2 = FTS5_TOMBSTONE_ROWID(iSegid, pSeg->nPgTombstone-1);
    fts5DataDelete(p, iTomb1, iTomb2);
  }
  if( p->pIdxDeleter==0 ){
    Fts5Config *pConfig = p->pConfig;
    fts5IndexPrepareStmt(p, &p->pIdxDeleter, sqlite3_mprintf(
          "DELETE FROM '%q'.'%q_idx' WHERE segid=?",
          pConfig->zDb, pConfig->zName
    ));
  }
  if( p->rc==SQLITE_OK ){
    sqlite3_bind_int(p->pIdxDeleter, 1, iSegid);
    sqlite3_step(p->pIdxDeleter);
    p->rc = sqlite3_reset(p->pIdxDeleter);
  }
}

/*
** Release a reference to an Fts5Structure object returned by an earlier 
** call to fts5StructureRead() or fts5StructureDecode().
*/
static void fts5StructureRelease(Fts5Structure *pStruct){
  if( pStruct && 0>=(--pStruct->nRef) ){
    int i;
    assert( pStruct->nRef==0 );
    for(i=0; i<pStruct->nLevel; i++){
      sqlite3_free(pStruct->aLevel[i].aSeg);
    }
    sqlite3_free(pStruct);
  }
}

static void fts5StructureRef(Fts5Structure *pStruct){
  pStruct->nRef++;
}

void *sqlite3Fts5StructureRef(Fts5Index *p){
  fts5StructureRef(p->pStruct);
  return (void*)p->pStruct;
}
void sqlite3Fts5StructureRelease(void *p){
  if( p ){
    fts5StructureRelease((Fts5Structure*)p);
  }
}
int sqlite3Fts5StructureTest(Fts5Index *p, void *pStruct){
  if( p->pStruct!=(Fts5Structure*)pStruct ){
    return SQLITE_ABORT;
  }
  return SQLITE_OK;
}

/*
** Ensure that structure object (*pp) is writable.
**
** This function is a no-op if (*pRc) is not SQLITE_OK when it is called. If
** an error occurs, (*pRc) is set to an SQLite error code before returning.
*/
static void fts5StructureMakeWritable(int *pRc, Fts5Structure **pp){
  Fts5Structure *p = *pp;
  if( *pRc==SQLITE_OK && p->nRef>1 ){
    i64 nByte = SZ_FTS5STRUCTURE(p->nLevel);
    Fts5Structure *pNew;
    pNew = (Fts5Structure*)sqlite3Fts5MallocZero(pRc, nByte);
    if( pNew ){
      int i;
      memcpy(pNew, p, nByte);
      for(i=0; i<p->nLevel; i++) pNew->aLevel[i].aSeg = 0;
      for(i=0; i<p->nLevel; i++){
        Fts5StructureLevel *pLvl = &pNew->aLevel[i];
        nByte = sizeof(Fts5StructureSegment) * pNew->aLevel[i].nSeg;
        pLvl->aSeg = (Fts5StructureSegment*)sqlite3Fts5MallocZero(pRc, nByte);
        if( pLvl->aSeg==0 ){
          for(i=0; i<p->nLevel; i++){
            sqlite3_free(pNew->aLevel[i].aSeg);
          }
          sqlite3_free(pNew);
          return;
        }
        memcpy(pLvl->aSeg, p->aLevel[i].aSeg, nByte);
      }
      p->nRef--;
      pNew->nRef = 1;
    }
    *pp = pNew;
  }
}

/*
** Deserialize and return the structure record currently stored in serialized
** form within buffer pData/nData.
**
** The Fts5Structure.aLevel[] and each Fts5StructureLevel.aSeg[] array
** are over-allocated by one slot. This allows the structure contents
** to be more easily edited.
**
** If an error occurs, *ppOut is set to NULL and an SQLite error code
** returned. Otherwise, *ppOut is set to point to the new object and
** SQLITE_OK returned.
*/
static int fts5StructureDecode(
  const u8 *pData,                /* Buffer containing serialized structure */
  int nData,                      /* Size of buffer pData in bytes */
  int *piCookie,                  /* Configuration cookie value */
  Fts5Structure **ppOut           /* OUT: Deserialized object */
){
  int rc = SQLITE_OK;
  int i = 0;
  int iLvl;
  int nLevel = 0;
  int nSegment = 0;
  sqlite3_int64 nByte;            /* Bytes of space to allocate at pRet */
  Fts5Structure *pRet = 0;        /* Structure object to return */
  int bStructureV2 = 0;           /* True for FTS5_STRUCTURE_V2 */
  u64 nOriginCntr = 0;            /* Largest origin value seen so far */

  /* Grab the cookie value */
  if( piCookie ) *piCookie = sqlite3Fts5Get32(pData);
  i = 4;

  /* Check if this is a V2 structure record. Set bStructureV2 if it is. */
  if( 0==memcmp(&pData[i], FTS5_STRUCTURE_V2, 4) ){
    i += 4;
    bStructureV2 = 1;
  }

  /* Read the total number of levels and segments from the start of the
  ** structure record.  */
  i += fts5GetVarint32(&pData[i], nLevel);
  i += fts5GetVarint32(&pData[i], nSegment);
  if( nLevel>FTS5_MAX_SEGMENT   || nLevel<0
   || nSegment>FTS5_MAX_SEGMENT || nSegment<0
  ){
    return FTS5_CORRUPT;
  }
  nByte = SZ_FTS5STRUCTURE(nLevel);
  pRet = (Fts5Structure*)sqlite3Fts5MallocZero(&rc, nByte);

  if( pRet ){
    pRet->nRef = 1;
    pRet->nLevel = nLevel;
    pRet->nSegment = nSegment;
    i += sqlite3Fts5GetVarint(&pData[i], &pRet->nWriteCounter);

    for(iLvl=0; rc==SQLITE_OK && iLvl<nLevel; iLvl++){
      Fts5StructureLevel *pLvl = &pRet->aLevel[iLvl];
      int nTotal = 0;
      int iSeg;

      if( i>=nData ){
        rc = FTS5_CORRUPT;
      }else{
        i += fts5GetVarint32(&pData[i], pLvl->nMerge);
        i += fts5GetVarint32(&pData[i], nTotal);
        if( nTotal<pLvl->nMerge ) rc = FTS5_CORRUPT;
        pLvl->aSeg = (Fts5StructureSegment*)sqlite3Fts5MallocZero(&rc, 
            nTotal * sizeof(Fts5StructureSegment)
        );
        nSegment -= nTotal;
      }

      if( rc==SQLITE_OK ){
        pLvl->nSeg = nTotal;
        for(iSeg=0; iSeg<nTotal; iSeg++){
          Fts5StructureSegment *pSeg = &pLvl->aSeg[iSeg];
          if( i>=nData ){
            rc = FTS5_CORRUPT;
            break;
          }
          assert( pSeg!=0 );
          i += fts5GetVarint32(&pData[i], pSeg->iSegid);
          i += fts5GetVarint32(&pData[i], pSeg->pgnoFirst);
          i += fts5GetVarint32(&pData[i], pSeg->pgnoLast);
          if( bStructureV2 ){
            i += fts5GetVarint(&pData[i], &pSeg->iOrigin1);
            i += fts5GetVarint(&pData[i], &pSeg->iOrigin2);
            i += fts5GetVarint32(&pData[i], pSeg->nPgTombstone);
            i += fts5GetVarint(&pData[i], &pSeg->nEntryTombstone);
            i += fts5GetVarint(&pData[i], &pSeg->nEntry);
            nOriginCntr = MAX(nOriginCntr, pSeg->iOrigin2);
          }
          if( pSeg->pgnoLast<pSeg->pgnoFirst ){
            rc = FTS5_CORRUPT;
            break;
          }
        }
        if( iLvl>0 && pLvl[-1].nMerge && nTotal==0 ) rc = FTS5_CORRUPT;
        if( iLvl==nLevel-1 && pLvl->nMerge ) rc = FTS5_CORRUPT;
      }
    }
    if( nSegment!=0 && rc==SQLITE_OK ) rc = FTS5_CORRUPT;
    if( bStructureV2 ){
      pRet->nOriginCntr = nOriginCntr+1;
    }

    if( rc!=SQLITE_OK ){
      fts5StructureRelease(pRet);
      pRet = 0;
    }
  }

  *ppOut = pRet;
  return rc;
}

/*
** Add a level to the Fts5Structure.aLevel[] array of structure object
** (*ppStruct).
*/
static void fts5StructureAddLevel(int *pRc, Fts5Structure **ppStruct){
  fts5StructureMakeWritable(pRc, ppStruct);
  assert( (ppStruct!=0 && (*ppStruct)!=0) || (*pRc)!=SQLITE_OK );
  if( *pRc==SQLITE_OK ){
    Fts5Structure *pStruct = *ppStruct;
    int nLevel = pStruct->nLevel;
    sqlite3_int64 nByte = SZ_FTS5STRUCTURE(nLevel+2);

    pStruct = sqlite3_realloc64(pStruct, nByte);
    if( pStruct ){
      memset(&pStruct->aLevel[nLevel], 0, sizeof(Fts5StructureLevel));
      pStruct->nLevel++;
      *ppStruct = pStruct;
    }else{
      *pRc = SQLITE_NOMEM;
    }
  }
}

/*
** Extend level iLvl so that there is room for at least nExtra more
** segments.
*/
static void fts5StructureExtendLevel(
  int *pRc, 
  Fts5Structure *pStruct, 
  int iLvl, 
  int nExtra, 
  int bInsert
){
  if( *pRc==SQLITE_OK ){
    Fts5StructureLevel *pLvl = &pStruct->aLevel[iLvl];
    Fts5StructureSegment *aNew;
    sqlite3_int64 nByte;

    nByte = (pLvl->nSeg + nExtra) * sizeof(Fts5StructureSegment);
    aNew = sqlite3_realloc64(pLvl->aSeg, nByte);
    if( aNew ){
      if( bInsert==0 ){
        memset(&aNew[pLvl->nSeg], 0, sizeof(Fts5StructureSegment) * nExtra);
      }else{
        int nMove = pLvl->nSeg * sizeof(Fts5StructureSegment);
        memmove(&aNew[nExtra], aNew, nMove);
        memset(aNew, 0, sizeof(Fts5StructureSegment) * nExtra);
      }
      pLvl->aSeg = aNew;
    }else{
      *pRc = SQLITE_NOMEM;
    }
  }
}

static Fts5Structure *fts5StructureReadUncached(Fts5Index *p){
  Fts5Structure *pRet = 0;
  Fts5Config *pConfig = p->pConfig;
  int iCookie;                    /* Configuration cookie */
  Fts5Data *pData;

  pData = fts5DataRead(p, FTS5_STRUCTURE_ROWID);
  if( p->rc==SQLITE_OK ){
    /* TODO: Do we need this if the leaf-index is appended? Probably... */
    memset(&pData->p[pData->nn], 0, FTS5_DATA_PADDING);
    p->rc = fts5StructureDecode(pData->p, pData->nn, &iCookie, &pRet);
    if( p->rc==SQLITE_OK ){
      if( (pConfig->pgsz==0 || pConfig->iCookie!=iCookie) ){
        p->rc = sqlite3Fts5ConfigLoad(pConfig, iCookie);
      }
    }else if( p->rc==SQLITE_CORRUPT_VTAB ){
      sqlite3Fts5ConfigErrmsg(p->pConfig, 
          "fts5: corrupt structure record for table \"%s\"", p->pConfig->zName
      );
    }
    fts5DataRelease(pData);
    if( p->rc!=SQLITE_OK ){
      fts5StructureRelease(pRet);
      pRet = 0;
    }
  }

  return pRet;
}

static i64 fts5IndexDataVersion(Fts5Index *p){
  i64 iVersion = 0;

  if( p->rc==SQLITE_OK ){
    if( p->pDataVersion==0 ){
      p->rc = fts5IndexPrepareStmt(p, &p->pDataVersion, 
          sqlite3_mprintf("PRAGMA %Q.data_version", p->pConfig->zDb)
          );
      if( p->rc ) return 0;
    }

    if( SQLITE_ROW==sqlite3_step(p->pDataVersion) ){
      iVersion = sqlite3_column_int64(p->pDataVersion, 0);
    }
    p->rc = sqlite3_reset(p->pDataVersion);
  }

  return iVersion;
}

/*
** Read, deserialize and return the structure record.
**
** The Fts5Structure.aLevel[] and each Fts5StructureLevel.aSeg[] array
** are over-allocated as described for function fts5StructureDecode() 
** above.
**
** If an error occurs, NULL is returned and an error code left in the
** Fts5Index handle. If an error has already occurred when this function
** is called, it is a no-op.
*/
static Fts5Structure *fts5StructureRead(Fts5Index *p){

  if( p->pStruct==0 ){
    p->iStructVersion = fts5IndexDataVersion(p);
    if( p->rc==SQLITE_OK ){
      p->pStruct = fts5StructureReadUncached(p);
    }
  }

#if 0
  else{
    Fts5Structure *pTest = fts5StructureReadUncached(p);
    if( pTest ){
      int i, j;
      assert_nc( p->pStruct->nSegment==pTest->nSegment );
      assert_nc( p->pStruct->nLevel==pTest->nLevel );
      for(i=0; i<pTest->nLevel; i++){
        assert_nc( p->pStruct->aLevel[i].nMerge==pTest->aLevel[i].nMerge );
        assert_nc( p->pStruct->aLevel[i].nSeg==pTest->aLevel[i].nSeg );
        for(j=0; j<pTest->aLevel[i].nSeg; j++){
          Fts5StructureSegment *p1 = &pTest->aLevel[i].aSeg[j];
          Fts5StructureSegment *p2 = &p->pStruct->aLevel[i].aSeg[j];
          assert_nc( p1->iSegid==p2->iSegid );
          assert_nc( p1->pgnoFirst==p2->pgnoFirst );
          assert_nc( p1->pgnoLast==p2->pgnoLast );
        }
      }
      fts5StructureRelease(pTest);
    }
  }
#endif

  if( p->rc!=SQLITE_OK ) return 0;
  assert( p->iStructVersion!=0 );
  assert( p->pStruct!=0 );
  fts5StructureRef(p->pStruct);
  return p->pStruct;
}

static void fts5StructureInvalidate(Fts5Index *p){
  if( p->pStruct ){
    fts5StructureRelease(p->pStruct);
    p->pStruct = 0;
  }
}

/*
** Return the total number of segments in index structure pStruct. This
** function is only ever used as part of assert() conditions.
*/
#ifdef SQLITE_DEBUG
static int fts5StructureCountSegments(Fts5Structure *pStruct){
  int nSegment = 0;               /* Total number of segments */
  if( pStruct ){
    int iLvl;                     /* Used to iterate through levels */
    for(iLvl=0; iLvl<pStruct->nLevel; iLvl++){
      nSegment += pStruct->aLevel[iLvl].nSeg;
    }
  }

  return nSegment;
}
#endif

#define fts5BufferSafeAppendBlob(pBuf, pBlob, nBlob) {     \
  assert( (pBuf)->nSpace>=((pBuf)->n+nBlob) );             \
  memcpy(&(pBuf)->p[(pBuf)->n], pBlob, nBlob);             \
  (pBuf)->n += nBlob;                                      \
}

#define fts5BufferSafeAppendVarint(pBuf, iVal) {                \
  (pBuf)->n += sqlite3Fts5PutVarint(&(pBuf)->p[(pBuf)->n], (iVal));  \
  assert( (pBuf)->nSpace>=(pBuf)->n );                          \
}


/*
** Serialize and store the "structure" record.
**
** If an error occurs, leave an error code in the Fts5Index object. If an
** error has already occurred, this function is a no-op.
*/
static void fts5StructureWrite(Fts5Index *p, Fts5Structure *pStruct){
  if( p->rc==SQLITE_OK ){
    Fts5Buffer buf;               /* Buffer to serialize record into */
    int iLvl;                     /* Used to iterate through levels */
    int iCookie;                  /* Cookie value to store */
    int nHdr = (pStruct->nOriginCntr>0 ? (4+4+9+9+9) : (4+9+9));

    assert( pStruct->nSegment==fts5StructureCountSegments(pStruct) );
    memset(&buf, 0, sizeof(Fts5Buffer));

    /* Append the current configuration cookie */
    iCookie = p->pConfig->iCookie;
    if( iCookie<0 ) iCookie = 0;

    if( 0==sqlite3Fts5BufferSize(&p->rc, &buf, nHdr) ){
      sqlite3Fts5Put32(buf.p, iCookie);
      buf.n = 4;
      if( pStruct->nOriginCntr>0 ){
        fts5BufferSafeAppendBlob(&buf, FTS5_STRUCTURE_V2, 4);
      }
      fts5BufferSafeAppendVarint(&buf, pStruct->nLevel);
      fts5BufferSafeAppendVarint(&buf, pStruct->nSegment);
      fts5BufferSafeAppendVarint(&buf, (i64)pStruct->nWriteCounter);
    }

    for(iLvl=0; iLvl<pStruct->nLevel; iLvl++){
      int iSeg;                     /* Used to iterate through segments */
      Fts5StructureLevel *pLvl = &pStruct->aLevel[iLvl];
      fts5BufferAppendVarint(&p->rc, &buf, pLvl->nMerge);
      fts5BufferAppendVarint(&p->rc, &buf, pLvl->nSeg);
      assert( pLvl->nMerge<=pLvl->nSeg );

      for(iSeg=0; iSeg<pLvl->nSeg; iSeg++){
        Fts5StructureSegment *pSeg = &pLvl->aSeg[iSeg];
        fts5BufferAppendVarint(&p->rc, &buf, pSeg->iSegid);
        fts5BufferAppendVarint(&p->rc, &buf, pSeg->pgnoFirst);
        fts5BufferAppendVarint(&p->rc, &buf, pSeg->pgnoLast);
        if( pStruct->nOriginCntr>0 ){
          fts5BufferAppendVarint(&p->rc, &buf, pSeg->iOrigin1);
          fts5BufferAppendVarint(&p->rc, &buf, pSeg->iOrigin2);
          fts5BufferAppendVarint(&p->rc, &buf, pSeg->nPgTombstone);
          fts5BufferAppendVarint(&p->rc, &buf, pSeg->nEntryTombstone);
          fts5BufferAppendVarint(&p->rc, &buf, pSeg->nEntry);
        }
      }
    }

    fts5DataWrite(p, FTS5_STRUCTURE_ROWID, buf.p, buf.n);
    fts5BufferFree(&buf);
  }
}

#if 0
static void fts5DebugStructure(int*,Fts5Buffer*,Fts5Structure*);
static void fts5PrintStructure(const char *zCaption, Fts5Structure *pStruct){
  int rc = SQLITE_OK;
  Fts5Buffer buf;
  memset(&buf, 0, sizeof(buf));
  fts5DebugStructure(&rc, &buf, pStruct);
  fprintf(stdout, "%s: %s\n", zCaption, buf.p);
  fflush(stdout);
  fts5BufferFree(&buf);
}
#else
# define fts5PrintStructure(x,y)
#endif

static int fts5SegmentSize(Fts5StructureSegment *pSeg){
  return 1 + pSeg->pgnoLast - pSeg->pgnoFirst;
}

/*
** Return a copy of index structure pStruct. Except, promote as many 
** segments as possible to level iPromote. If an OOM occurs, NULL is 
** returned.
*/
static void fts5StructurePromoteTo(
  Fts5Index *p,
  int iPromote,
  int szPromote,
  Fts5Structure *pStruct
){
  int il, is;
  Fts5StructureLevel *pOut = &pStruct->aLevel[iPromote];

  if( pOut->nMerge==0 ){
    for(il=iPromote+1; il<pStruct->nLevel; il++){
      Fts5StructureLevel *pLvl = &pStruct->aLevel[il];
      if( pLvl->nMerge ) return;
      for(is=pLvl->nSeg-1; is>=0; is--){
        int sz = fts5SegmentSize(&pLvl->aSeg[is]);
        if( sz>szPromote ) return;
        fts5StructureExtendLevel(&p->rc, pStruct, iPromote, 1, 1);
        if( p->rc ) return;
        memcpy(pOut->aSeg, &pLvl->aSeg[is], sizeof(Fts5StructureSegment));
        pOut->nSeg++;
        pLvl->nSeg--;
      }
    }
  }
}

/*
** A new segment has just been written to level iLvl of index structure
** pStruct. This function determines if any segments should be promoted
** as a result. Segments are promoted in two scenarios:
**
**   a) If the segment just written is smaller than one or more segments
**      within the previous populated level, it is promoted to the previous
**      populated level.
**
**   b) If the segment just written is larger than the newest segment on
**      the next populated level, then that segment, and any other adjacent
**      segments that are also smaller than the one just written, are 
**      promoted. 
**
** If one or more segments are promoted, the structure object is updated
** to reflect this.
*/
static void fts5StructurePromote(
  Fts5Index *p,                   /* FTS5 backend object */
  int iLvl,                       /* Index level just updated */
  Fts5Structure *pStruct          /* Index structure */
){
  if( p->rc==SQLITE_OK ){
    int iTst;
    int iPromote = -1;
    int szPromote = 0;            /* Promote anything this size or smaller */
    Fts5StructureSegment *pSeg;   /* Segment just written */
    int szSeg;                    /* Size of segment just written */
    int nSeg = pStruct->aLevel[iLvl].nSeg;

    if( nSeg==0 ) return;
    pSeg = &pStruct->aLevel[iLvl].aSeg[pStruct->aLevel[iLvl].nSeg-1];
    szSeg = (1 + pSeg->pgnoLast - pSeg->pgnoFirst);

    /* Check for condition (a) */
    for(iTst=iLvl-1; iTst>=0 && pStruct->aLevel[iTst].nSeg==0; iTst--);
    if( iTst>=0 ){
      int i;
      int szMax = 0;
      Fts5StructureLevel *pTst = &pStruct->aLevel[iTst];
      assert( pTst->nMerge==0 );
      for(i=0; i<pTst->nSeg; i++){
        int sz = pTst->aSeg[i].pgnoLast - pTst->aSeg[i].pgnoFirst + 1;
        if( sz>szMax ) szMax = sz;
      }
      if( szMax>=szSeg ){
        /* Condition (a) is true. Promote the newest segment on level 
        ** iLvl to level iTst.  */
        iPromote = iTst;
        szPromote = szMax;
      }
    }

    /* If condition (a) is not met, assume (b) is true. StructurePromoteTo()
    ** is a no-op if it is not.  */
    if( iPromote<0 ){
      iPromote = iLvl;
      szPromote = szSeg;
    }
    fts5StructurePromoteTo(p, iPromote, szPromote, pStruct);
  }
}


/*
** Advance the iterator passed as the only argument. If the end of the 
** doclist-index page is reached, return non-zero.
*/
static int fts5DlidxLvlNext(Fts5DlidxLvl *pLvl){
  Fts5Data *pData = pLvl->pData;

  if( pLvl->iOff==0 ){
    assert( pLvl->bEof==0 );
    pLvl->iOff = 1;
    pLvl->iOff += fts5GetVarint32(&pData->p[1], pLvl->iLeafPgno);
    pLvl->iOff += fts5GetVarint(&pData->p[pLvl->iOff], (u64*)&pLvl->iRowid);
    pLvl->iFirstOff = pLvl->iOff;
  }else{
    int iOff;
    for(iOff=pLvl->iOff; iOff<pData->nn; iOff++){
      if( pData->p[iOff] ) break; 
    }

    if( iOff<pData->nn ){
      u64 iVal;
      pLvl->iLeafPgno += (iOff - pLvl->iOff) + 1;
      iOff += fts5GetVarint(&pData->p[iOff], &iVal);
      pLvl->iRowid += iVal;
      pLvl->iOff = iOff;
    }else{
      pLvl->bEof = 1;
    }
  }

  return pLvl->bEof;
}

/*
** Advance the iterator passed as the only argument.
*/
static int fts5DlidxIterNextR(Fts5Index *p, Fts5DlidxIter *pIter, int iLvl){
  Fts5DlidxLvl *pLvl = &pIter->aLvl[iLvl];

  assert( iLvl<pIter->nLvl );
  if( fts5DlidxLvlNext(pLvl) ){
    if( (iLvl+1) < pIter->nLvl ){
      fts5DlidxIterNextR(p, pIter, iLvl+1);
      if( pLvl[1].bEof==0 ){
        fts5DataRelease(pLvl->pData);
        memset(pLvl, 0, sizeof(Fts5DlidxLvl));
        pLvl->pData = fts5DataRead(p, 
            FTS5_DLIDX_ROWID(pIter->iSegid, iLvl, pLvl[1].iLeafPgno)
        );
        if( pLvl->pData ) fts5DlidxLvlNext(pLvl);
      }
    }
  }

  return pIter->aLvl[0].bEof;
}
static int fts5DlidxIterNext(Fts5Index *p, Fts5DlidxIter *pIter){
  return fts5DlidxIterNextR(p, pIter, 0);
}

/*
** The iterator passed as the first argument has the following fields set
** as follows. This function sets up the rest of the iterator so that it
** points to the first rowid in the doclist-index.
**
**   pData:
**     pointer to doclist-index record, 
**
** When this function is called pIter->iLeafPgno is the page number the
** doclist is associated with (the one featuring the term).
*/
static int fts5DlidxIterFirst(Fts5DlidxIter *pIter){
  int i;
  for(i=0; i<pIter->nLvl; i++){
    fts5DlidxLvlNext(&pIter->aLvl[i]);
  }
  return pIter->aLvl[0].bEof;
}


static int fts5DlidxIterEof(Fts5Index *p, Fts5DlidxIter *pIter){
  return p->rc!=SQLITE_OK || pIter->aLvl[0].bEof;
}

static void fts5DlidxIterLast(Fts5Index *p, Fts5DlidxIter *pIter){
  int i;

  /* Advance each level to the last entry on the last page */
  for(i=pIter->nLvl-1; p->rc==SQLITE_OK && i>=0; i--){
    Fts5DlidxLvl *pLvl = &pIter->aLvl[i];
    while( fts5DlidxLvlNext(pLvl)==0 );
    pLvl->bEof = 0;

    if( i>0 ){
      Fts5DlidxLvl *pChild = &pLvl[-1];
      fts5DataRelease(pChild->pData);
      memset(pChild, 0, sizeof(Fts5DlidxLvl));
      pChild->pData = fts5DataRead(p, 
          FTS5_DLIDX_ROWID(pIter->iSegid, i-1, pLvl->iLeafPgno)
      );
    }
  }
}

/*
** Move the iterator passed as the only argument to the previous entry.
*/
static int fts5DlidxLvlPrev(Fts5DlidxLvl *pLvl){
  int iOff = pLvl->iOff;

  assert( pLvl->bEof==0 );
  if( iOff<=pLvl->iFirstOff ){
    pLvl->bEof = 1;
  }else{
    u8 *a = pLvl->pData->p;

    pLvl->iOff = 0;
    fts5DlidxLvlNext(pLvl);
    while( 1 ){
      int nZero = 0;
      int ii = pLvl->iOff;
      u64 delta = 0;

      while( a[ii]==0 ){
        nZero++;
        ii++;
      }
      ii += sqlite3Fts5GetVarint(&a[ii], &delta);

      if( ii>=iOff ) break;
      pLvl->iLeafPgno += nZero+1;
      pLvl->iRowid += delta;
      pLvl->iOff = ii;
    }
  }

  return pLvl->bEof;
}

static int fts5DlidxIterPrevR(Fts5Index *p, Fts5DlidxIter *pIter, int iLvl){
  Fts5DlidxLvl *pLvl = &pIter->aLvl[iLvl];

  assert( iLvl<pIter->nLvl );
  if( fts5DlidxLvlPrev(pLvl) ){
    if( (iLvl+1) < pIter->nLvl ){
      fts5DlidxIterPrevR(p, pIter, iLvl+1);
      if( pLvl[1].bEof==0 ){
        fts5DataRelease(pLvl->pData);
        memset(pLvl, 0, sizeof(Fts5DlidxLvl));
        pLvl->pData = fts5DataRead(p, 
            FTS5_DLIDX_ROWID(pIter->iSegid, iLvl, pLvl[1].iLeafPgno)
        );
        if( pLvl->pData ){
          while( fts5DlidxLvlNext(pLvl)==0 );
          pLvl->bEof = 0;
        }
      }
    }
  }

  return pIter->aLvl[0].bEof;
}
static int fts5DlidxIterPrev(Fts5Index *p, Fts5DlidxIter *pIter){
  return fts5DlidxIterPrevR(p, pIter, 0);
}

/*
** Free a doclist-index iterator object allocated by fts5DlidxIterInit().
*/
static void fts5DlidxIterFree(Fts5DlidxIter *pIter){
  if( pIter ){
    int i;
    for(i=0; i<pIter->nLvl; i++){
      fts5DataRelease(pIter->aLvl[i].pData);
    }
    sqlite3_free(pIter);
  }
}

static Fts5DlidxIter *fts5DlidxIterInit(
  Fts5Index *p,                   /* Fts5 Backend to iterate within */
  int bRev,                       /* True for ORDER BY ASC */
  int iSegid,                     /* Segment id */
  int iLeafPg                     /* Leaf page number to load dlidx for */
){
  Fts5DlidxIter *pIter = 0;
  int i;
  int bDone = 0;

  for(i=0; p->rc==SQLITE_OK && bDone==0; i++){
    sqlite3_int64 nByte = SZ_FTS5DLIDXITER(i+1);
    Fts5DlidxIter *pNew;

    pNew = (Fts5DlidxIter*)sqlite3_realloc64(pIter, nByte);
    if( pNew==0 ){
      p->rc = SQLITE_NOMEM;
    }else{
      i64 iRowid = FTS5_DLIDX_ROWID(iSegid, i, iLeafPg);
      Fts5DlidxLvl *pLvl = &pNew->aLvl[i];
      pIter = pNew;
      memset(pLvl, 0, sizeof(Fts5DlidxLvl));
      pLvl->pData = fts5DataRead(p, iRowid);
      if( pLvl->pData && (pLvl->pData->p[0] & 0x0001)==0 ){
        bDone = 1;
      }
      pIter->nLvl = i+1;
    }
  }

  if( p->rc==SQLITE_OK ){
    pIter->iSegid = iSegid;
    if( bRev==0 ){
      fts5DlidxIterFirst(pIter);
    }else{
      fts5DlidxIterLast(p, pIter);
    }
  }

  if( p->rc!=SQLITE_OK ){
    fts5DlidxIterFree(pIter);
    pIter = 0;
  }

  return pIter;
}

static i64 fts5DlidxIterRowid(Fts5DlidxIter *pIter){
  return pIter->aLvl[0].iRowid;
}
static int fts5DlidxIterPgno(Fts5DlidxIter *pIter){
  return pIter->aLvl[0].iLeafPgno;
}

/*
** Load the next leaf page into the segment iterator.
*/
static void fts5SegIterNextPage(
  Fts5Index *p,                   /* FTS5 backend object */
  Fts5SegIter *pIter              /* Iterator to advance to next page */
){
  Fts5Data *pLeaf;
  Fts5StructureSegment *pSeg = pIter->pSeg;
  fts5DataRelease(pIter->pLeaf);
  pIter->iLeafPgno++;
  if( pIter->pNextLeaf ){
    pIter->pLeaf = pIter->pNextLeaf;
    pIter->pNextLeaf = 0;
  }else if( pIter->iLeafPgno<=pSeg->pgnoLast ){
    pIter->pLeaf = fts5LeafRead(p, 
        FTS5_SEGMENT_ROWID(pSeg->iSegid, pIter->iLeafPgno)
    );
  }else{
    pIter->pLeaf = 0;
  }
  pLeaf = pIter->pLeaf;

  if( pLeaf ){
    pIter->iPgidxOff = pLeaf->szLeaf;
    if( fts5LeafIsTermless(pLeaf) ){
      pIter->iEndofDoclist = pLeaf->nn+1;
    }else{
      pIter->iPgidxOff += fts5GetVarint32(&pLeaf->p[pIter->iPgidxOff],
          pIter->iEndofDoclist
      );
    }
  }
}

/*
** Argument p points to a buffer containing a varint to be interpreted as a
** position list size field. Read the varint and return the number of bytes
** read. Before returning, set *pnSz to the number of bytes in the position
** list, and *pbDel to true if the delete flag is set, or false otherwise.
*/
static int fts5GetPoslistSize(const u8 *p, int *pnSz, int *pbDel){
  int nSz;
  int n = 0;
  fts5FastGetVarint32(p, n, nSz);
  assert_nc( nSz>=0 );
  *pnSz = nSz/2;
  *pbDel = nSz & 0x0001;
  return n;
}

/*
** Fts5SegIter.iLeafOffset currently points to the first byte of a
** position-list size field. Read the value of the field and store it
** in the following variables:
**
**   Fts5SegIter.nPos
**   Fts5SegIter.bDel
**
** Leave Fts5SegIter.iLeafOffset pointing to the first byte of the 
** position list content (if any).
*/
static void fts5SegIterLoadNPos(Fts5Index *p, Fts5SegIter *pIter){
  if( p->rc==SQLITE_OK ){
    int iOff = pIter->iLeafOffset;  /* Offset to read at */
    ASSERT_SZLEAF_OK(pIter->pLeaf);
    if( p->pConfig->eDetail==FTS5_DETAIL_NONE ){
      int iEod = MIN(pIter->iEndofDoclist, pIter->pLeaf->szLeaf);
      pIter->bDel = 0;
      pIter->nPos = 1;
      if( iOff<iEod && pIter->pLeaf->p[iOff]==0 ){
        pIter->bDel = 1;
        iOff++;
        if( iOff<iEod && pIter->pLeaf->p[iOff]==0 ){
          pIter->nPos = 1;
          iOff++;
        }else{
          pIter->nPos = 0;
        }
      }
    }else{
      int nSz;
      fts5FastGetVarint32(pIter->pLeaf->p, iOff, nSz);
      pIter->bDel = (nSz & 0x0001);
      pIter->nPos = nSz>>1;
      assert_nc( pIter->nPos>=0 );
    }
    pIter->iLeafOffset = iOff;
  }
}

static void fts5SegIterLoadRowid(Fts5Index *p, Fts5SegIter *pIter){
  u8 *a = pIter->pLeaf->p;        /* Buffer to read data from */
  i64 iOff = pIter->iLeafOffset;

  ASSERT_SZLEAF_OK(pIter->pLeaf);
  while( iOff>=pIter->pLeaf->szLeaf ){
    fts5SegIterNextPage(p, pIter);
    if( pIter->pLeaf==0 ){
      if( p->rc==SQLITE_OK ) FTS5_CORRUPT_ITER(p, pIter);
      return;
    }
    iOff = 4;
    a = pIter->pLeaf->p;
  }
  iOff += sqlite3Fts5GetVarint(&a[iOff], (u64*)&pIter->iRowid);
  pIter->iLeafOffset = iOff;
}

/*
** Fts5SegIter.iLeafOffset currently points to the first byte of the 
** "nSuffix" field of a term. Function parameter nKeep contains the value
** of the "nPrefix" field (if there was one - it is passed 0 if this is
** the first term in the segment).
**
** This function populates:
**
**   Fts5SegIter.term
**   Fts5SegIter.rowid
**
** accordingly and leaves (Fts5SegIter.iLeafOffset) set to the content of
** the first position list. The position list belonging to document 
** (Fts5SegIter.iRowid).
*/
static void fts5SegIterLoadTerm(Fts5Index *p, Fts5SegIter *pIter, int nKeep){
  u8 *a = pIter->pLeaf->p;        /* Buffer to read data from */
  i64 iOff = pIter->iLeafOffset;  /* Offset to read at */
  int nNew;                       /* Bytes of new data */

  iOff += fts5GetVarint32(&a[iOff], nNew);
  if( iOff+nNew>pIter->pLeaf->szLeaf || nKeep>pIter->term.n || nNew==0 ){
    FTS5_CORRUPT_ITER(p, pIter);
    return;
  }
  pIter->term.n = nKeep;
  fts5BufferAppendBlob(&p->rc, &pIter->term, nNew, &a[iOff]);
  assert( pIter->term.n<=pIter->term.nSpace );
  iOff += nNew;
  pIter->iTermLeafOffset = iOff;
  pIter->iTermLeafPgno = pIter->iLeafPgno;
  pIter->iLeafOffset = iOff;

  if( pIter->iPgidxOff>=pIter->pLeaf->nn ){
    pIter->iEndofDoclist = pIter->pLeaf->nn+1;
  }else{
    int nExtra;
    pIter->iPgidxOff += fts5GetVarint32(&a[pIter->iPgidxOff], nExtra);
    pIter->iEndofDoclist += nExtra;
  }

  fts5SegIterLoadRowid(p, pIter);
}

static void fts5SegIterNext(Fts5Index*, Fts5SegIter*, int*);
static void fts5SegIterNext_Reverse(Fts5Index*, Fts5SegIter*, int*);
static void fts5SegIterNext_None(Fts5Index*, Fts5SegIter*, int*);

static void fts5SegIterSetNext(Fts5Index *p, Fts5SegIter *pIter){
  if( pIter->flags & FTS5_SEGITER_REVERSE ){
    pIter->xNext = fts5SegIterNext_Reverse;
  }else if( p->pConfig->eDetail==FTS5_DETAIL_NONE ){
    pIter->xNext = fts5SegIterNext_None;
  }else{
    pIter->xNext = fts5SegIterNext;
  }
}

/*
** Allocate a tombstone hash page array object (pIter->pTombArray) for 
** the iterator passed as the second argument. If an OOM error occurs, 
** leave an error in the Fts5Index object.
*/
static void fts5SegIterAllocTombstone(Fts5Index *p, Fts5SegIter *pIter){
  const i64 nTomb = (i64)pIter->pSeg->nPgTombstone;
  if( nTomb>0 ){
    i64 nByte = SZ_FTS5TOMBSTONEARRAY(nTomb+1);
    Fts5TombstoneArray *pNew;
    pNew = (Fts5TombstoneArray*)sqlite3Fts5MallocZero(&p->rc, nByte);
    if( pNew ){
      pNew->nTombstone = nTomb;
      pNew->nRef = 1;
      pIter->pTombArray = pNew;
    }
  }
}

/*
** Initialize the iterator object pIter to iterate through the entries in
** segment pSeg. The iterator is left pointing to the first entry when 
** this function returns.
**
** If an error occurs, Fts5Index.rc is set to an appropriate error code. If 
** an error has already occurred when this function is called, it is a no-op.
*/
static void fts5SegIterInit(
  Fts5Index *p,                   /* FTS index object */
  Fts5StructureSegment *pSeg,     /* Description of segment */
  Fts5SegIter *pIter              /* Object to populate */
){
  if( pSeg->pgnoFirst==0 ){
    /* This happens if the segment is being used as an input to an incremental
    ** merge and all data has already been "trimmed". See function
    ** fts5TrimSegments() for details. In this case leave the iterator empty.
    ** The caller will see the (pIter->pLeaf==0) and assume the iterator is
    ** at EOF already. */
    assert( pIter->pLeaf==0 );
    return;
  }

  if( p->rc==SQLITE_OK ){
    memset(pIter, 0, sizeof(*pIter));
    fts5SegIterSetNext(p, pIter);
    pIter->pSeg = pSeg;
    pIter->iLeafPgno = pSeg->pgnoFirst-1;
    do {
      fts5SegIterNextPage(p, pIter);
    }while( p->rc==SQLITE_OK && pIter->pLeaf && pIter->pLeaf->nn==4 );
  }

  if( p->rc==SQLITE_OK && pIter->pLeaf ){
    pIter->iLeafOffset = 4;
    assert( pIter->pLeaf!=0 );
    assert_nc( pIter->pLeaf->nn>4 );
    assert_nc( fts5LeafFirstTermOff(pIter->pLeaf)==4 );
    pIter->iPgidxOff = pIter->pLeaf->szLeaf+1;
    fts5SegIterLoadTerm(p, pIter, 0);
    fts5SegIterLoadNPos(p, pIter);
    fts5SegIterAllocTombstone(p, pIter);
  }
}

/*
** This function is only ever called on iterators created by calls to
** Fts5IndexQuery() with the FTS5INDEX_QUERY_DESC flag set.
**
** The iterator is in an unusual state when this function is called: the
** Fts5SegIter.iLeafOffset variable is set to the offset of the start of
** the position-list size field for the first relevant rowid on the page.
** Fts5SegIter.rowid is set, but nPos and bDel are not.
**
** This function advances the iterator so that it points to the last 
** relevant rowid on the page and, if necessary, initializes the 
** aRowidOffset[] and iRowidOffset variables. At this point the iterator
** is in its regular state - Fts5SegIter.iLeafOffset points to the first
** byte of the position list content associated with said rowid.
*/
static void fts5SegIterReverseInitPage(Fts5Index *p, Fts5SegIter *pIter){
  int eDetail = p->pConfig->eDetail;
  int n = pIter->pLeaf->szLeaf;
  int i = pIter->iLeafOffset;
  u8 *a = pIter->pLeaf->p;
  int iRowidOffset = 0;

  if( n>pIter->iEndofDoclist ){
    n = pIter->iEndofDoclist;
  }

  ASSERT_SZLEAF_OK(pIter->pLeaf);
  while( 1 ){
    u64 iDelta = 0;

    if( eDetail==FTS5_DETAIL_NONE ){
      /* todo */
      if( i<n && a[i]==0 ){
        i++;
        if( i<n && a[i]==0 ) i++;
      }
    }else{
      int nPos;
      int bDummy;
      i += fts5GetPoslistSize(&a[i], &nPos, &bDummy);
      i += nPos;
    }
    if( i>=n ) break;
    i += fts5GetVarint(&a[i], &iDelta);
    pIter->iRowid += iDelta;

    /* If necessary, grow the pIter->aRowidOffset[] array. */
    if( iRowidOffset>=pIter->nRowidOffset ){
      int nNew = pIter->nRowidOffset + 8;
      int *aNew = (int*)sqlite3_realloc64(pIter->aRowidOffset,nNew*sizeof(int));
      if( aNew==0 ){
        p->rc = SQLITE_NOMEM;
        break;
      }
      pIter->aRowidOffset = aNew;
      pIter->nRowidOffset = nNew;
    }

    pIter->aRowidOffset[iRowidOffset++] = pIter->iLeafOffset;
    pIter->iLeafOffset = i;
  }
  pIter->iRowidOffset = iRowidOffset;
  fts5SegIterLoadNPos(p, pIter);
}

/*
**
*/
static void fts5SegIterReverseNewPage(Fts5Index *p, Fts5SegIter *pIter){
  assert( pIter->flags & FTS5_SEGITER_REVERSE );
  assert( pIter->flags & FTS5_SEGITER_ONETERM );

  fts5DataRelease(pIter->pLeaf);
  pIter->pLeaf = 0;
  while( p->rc==SQLITE_OK && pIter->iLeafPgno>pIter->iTermLeafPgno ){
    Fts5Data *pNew;
    pIter->iLeafPgno--;
    pNew = fts5DataRead(p, FTS5_SEGMENT_ROWID(
          pIter->pSeg->iSegid, pIter->iLeafPgno
    ));
    if( pNew ){
      /* iTermLeafOffset may be equal to szLeaf if the term is the last
      ** thing on the page - i.e. the first rowid is on the following page.
      ** In this case leave pIter->pLeaf==0, this iterator is at EOF. */
      if( pIter->iLeafPgno==pIter->iTermLeafPgno ){
        assert( pIter->pLeaf==0 );
        if( pIter->iTermLeafOffset<pNew->szLeaf ){
          pIter->pLeaf = pNew;
          pIter->iLeafOffset = pIter->iTermLeafOffset;
        }
      }else{
        int iRowidOff;
        iRowidOff = fts5LeafFirstRowidOff(pNew);
        if( iRowidOff ){
          if( iRowidOff>=pNew->szLeaf ){
            FTS5_CORRUPT_ITER(p, pIter);
          }else{
            pIter->pLeaf = pNew;
            pIter->iLeafOffset = iRowidOff;
          }
        }
      }

      if( pIter->pLeaf ){
        u8 *a = &pIter->pLeaf->p[pIter->iLeafOffset];
        pIter->iLeafOffset += fts5GetVarint(a, (u64*)&pIter->iRowid);
        break;
      }else{
        fts5DataRelease(pNew);
      }
    }
  }

  if( pIter->pLeaf ){
    pIter->iEndofDoclist = pIter->pLeaf->nn+1;
    fts5SegIterReverseInitPage(p, pIter);
  }
}

/*
** Return true if the iterator passed as the second argument currently
** points to a delete marker. A delete marker is an entry with a 0 byte
** position-list.
*/
static int fts5MultiIterIsEmpty(Fts5Index *p, Fts5Iter *pIter){
  Fts5SegIter *pSeg = &pIter->aSeg[pIter->aFirst[1].iFirst];
  return (p->rc==SQLITE_OK && pSeg->pLeaf && pSeg->nPos==0);
}

/*
** Advance iterator pIter to the next entry.
**
** This version of fts5SegIterNext() is only used by reverse iterators.
*/
static void fts5SegIterNext_Reverse(
  Fts5Index *p,                   /* FTS5 backend object */
  Fts5SegIter *pIter,             /* Iterator to advance */
  int *pbUnused                   /* Unused */
){
  assert( pIter->flags & FTS5_SEGITER_REVERSE );
  assert( pIter->pNextLeaf==0 );
  UNUSED_PARAM(pbUnused);

  if( pIter->iRowidOffset>0 ){
    u8 *a = pIter->pLeaf->p;
    int iOff;
    u64 iDelta;

    pIter->iRowidOffset--;
    pIter->iLeafOffset = pIter->aRowidOffset[pIter->iRowidOffset];
    fts5SegIterLoadNPos(p, pIter);
    iOff = pIter->iLeafOffset;
    if( p->pConfig->eDetail!=FTS5_DETAIL_NONE ){
      iOff += pIter->nPos;
    }
    fts5GetVarint(&a[iOff], &iDelta);
    pIter->iRowid -= iDelta;
  }else{
    fts5SegIterReverseNewPage(p, pIter);
  }
}

/*
** Advance iterator pIter to the next entry.
**
** This version of fts5SegIterNext() is only used if detail=none and the
** iterator is not a reverse direction iterator.
*/
static void fts5SegIterNext_None(
  Fts5Index *p,                   /* FTS5 backend object */
  Fts5SegIter *pIter,             /* Iterator to advance */
  int *pbNewTerm                  /* OUT: Set for new term */
){
  int iOff;

  assert( p->rc==SQLITE_OK );
  assert( (pIter->flags & FTS5_SEGITER_REVERSE)==0 );
  assert( p->pConfig->eDetail==FTS5_DETAIL_NONE );

  ASSERT_SZLEAF_OK(pIter->pLeaf);
  iOff = pIter->iLeafOffset;

  /* Next entry is on the next page */
  while( pIter->pSeg && iOff>=pIter->pLeaf->szLeaf ){
    fts5SegIterNextPage(p, pIter);
    if( p->rc || pIter->pLeaf==0 ) return;
    pIter->iRowid = 0;
    iOff = 4;
  }

  if( iOff<pIter->iEndofDoclist ){
    /* Next entry is on the current page */
    u64 iDelta;
    iOff += sqlite3Fts5GetVarint(&pIter->pLeaf->p[iOff], (u64*)&iDelta);
    pIter->iLeafOffset = iOff;
    pIter->iRowid += iDelta;
  }else if( (pIter->flags & FTS5_SEGITER_ONETERM)==0 ){
    if( pIter->pSeg ){
      int nKeep = 0;
      if( iOff!=fts5LeafFirstTermOff(pIter->pLeaf) ){
        iOff += fts5GetVarint32(&pIter->pLeaf->p[iOff], nKeep);
      }
      pIter->iLeafOffset = iOff;
      fts5SegIterLoadTerm(p, pIter, nKeep);
    }else{
      const u8 *pList = 0;
      const char *zTerm = 0;
      int nTerm = 0;
      int nList;
      sqlite3Fts5HashScanNext(p->pHash);
      sqlite3Fts5HashScanEntry(p->pHash, &zTerm, &nTerm, &pList, &nList);
      if( pList==0 ) goto next_none_eof;
      pIter->pLeaf->p = (u8*)pList;
      pIter->pLeaf->nn = nList;
      pIter->pLeaf->szLeaf = nList;
      pIter->iEndofDoclist = nList;
      sqlite3Fts5BufferSet(&p->rc,&pIter->term, nTerm, (u8*)zTerm);
      pIter->iLeafOffset = fts5GetVarint(pList, (u64*)&pIter->iRowid);
    }

    if( pbNewTerm ) *pbNewTerm = 1;
  }else{
    goto next_none_eof;
  }

  fts5SegIterLoadNPos(p, pIter);

  return;
 next_none_eof:
  fts5DataRelease(pIter->pLeaf);
  pIter->pLeaf = 0;
}


/*
** Advance iterator pIter to the next entry. 
**
** If an error occurs, Fts5Index.rc is set to an appropriate error code. It 
** is not considered an error if the iterator reaches EOF. If an error has 
** already occurred when this function is called, it is a no-op.
*/
static void fts5SegIterNext(
  Fts5Index *p,                   /* FTS5 backend object */
  Fts5SegIter *pIter,             /* Iterator to advance */
  int *pbNewTerm                  /* OUT: Set for new term */
){
  Fts5Data *pLeaf = pIter->pLeaf;
  int iOff;
  int bNewTerm = 0;
  int nKeep = 0;
  u8 *a;
  int n;

  assert( pbNewTerm==0 || *pbNewTerm==0 );
  assert( p->pConfig->eDetail!=FTS5_DETAIL_NONE );

  /* Search for the end of the position list within the current page. */
  a = pLeaf->p;
  n = pLeaf->szLeaf;

  ASSERT_SZLEAF_OK(pLeaf);
  iOff = pIter->iLeafOffset + pIter->nPos;

  if( iOff<n ){
    /* The next entry is on the current page. */
    assert_nc( iOff<=pIter->iEndofDoclist );
    if( iOff>=pIter->iEndofDoclist ){
      bNewTerm = 1;
      if( iOff!=fts5LeafFirstTermOff(pLeaf) ){
        iOff += fts5GetVarint32(&a[iOff], nKeep);
      }
    }else{
      u64 iDelta;
      iOff += sqlite3Fts5GetVarint(&a[iOff], &iDelta);
      pIter->iRowid += iDelta;
      assert_nc( iDelta>0 );
    }
    pIter->iLeafOffset = iOff;

  }else if( pIter->pSeg==0 ){
    const u8 *pList = 0;
    const char *zTerm = 0;
    int nTerm = 0;
    int nList = 0;
    assert( (pIter->flags & FTS5_SEGITER_ONETERM) || pbNewTerm );
    if( 0==(pIter->flags & FTS5_SEGITER_ONETERM) ){
      sqlite3Fts5HashScanNext(p->pHash);
      sqlite3Fts5HashScanEntry(p->pHash, &zTerm, &nTerm, &pList, &nList);
    }
    if( pList==0 ){
      fts5DataRelease(pIter->pLeaf);
      pIter->pLeaf = 0;
    }else{
      pIter->pLeaf->p = (u8*)pList;
      pIter->pLeaf->nn = nList;
      pIter->pLeaf->szLeaf = nList;
      pIter->iEndofDoclist = nList+1;
      sqlite3Fts5BufferSet(&p->rc, &pIter->term, nTerm, (u8*)zTerm);
      pIter->iLeafOffset = fts5GetVarint(pList, (u64*)&pIter->iRowid);
      *pbNewTerm = 1;
    }
  }else{
    iOff = 0;
    /* Next entry is not on the current page */
    while( iOff==0 ){
      fts5SegIterNextPage(p, pIter);
      pLeaf = pIter->pLeaf;
      if( pLeaf==0 ) break;
      ASSERT_SZLEAF_OK(pLeaf);
      if( (iOff = fts5LeafFirstRowidOff(pLeaf)) && iOff<pLeaf->szLeaf ){
        iOff += sqlite3Fts5GetVarint(&pLeaf->p[iOff], (u64*)&pIter->iRowid);
        pIter->iLeafOffset = iOff;

        if( pLeaf->nn>pLeaf->szLeaf ){
          pIter->iPgidxOff = pLeaf->szLeaf + fts5GetVarint32(
              &pLeaf->p[pLeaf->szLeaf], pIter->iEndofDoclist
          );
        }
      }
      else if( pLeaf->nn>pLeaf->szLeaf ){
        pIter->iPgidxOff = pLeaf->szLeaf + fts5GetVarint32(
            &pLeaf->p[pLeaf->szLeaf], iOff
        );
        pIter->iLeafOffset = iOff;
        pIter->iEndofDoclist = iOff;
        bNewTerm = 1;
      }
      assert_nc( iOff<pLeaf->szLeaf );
      if( iOff>pLeaf->szLeaf ){
        FTS5_CORRUPT_ITER(p, pIter);
        return;
      }
    }
  }

  /* Check if the iterator is now at EOF. If so, return early. */
  if( pIter->pLeaf ){
    if( bNewTerm ){
      if( pIter->flags & FTS5_SEGITER_ONETERM ){
        fts5DataRelease(pIter->pLeaf);
        pIter->pLeaf = 0;
      }else{
        fts5SegIterLoadTerm(p, pIter, nKeep);
        fts5SegIterLoadNPos(p, pIter);
        if( pbNewTerm ) *pbNewTerm = 1;
      }
    }else{
      /* The following could be done by calling fts5SegIterLoadNPos(). But
      ** this block is particularly performance critical, so equivalent
      ** code is inlined.  */
      int nSz;
      assert_nc( pIter->iLeafOffset<=pIter->pLeaf->nn );
      fts5FastGetVarint32(pIter->pLeaf->p, pIter->iLeafOffset, nSz);
      pIter->bDel = (nSz & 0x0001);
      pIter->nPos = nSz>>1;
      assert_nc( pIter->nPos>=0 );
    }
  }
}

#define SWAPVAL(T, a, b) { T tmp; tmp=a; a=b; b=tmp; }

#define fts5IndexSkipVarint(a, iOff) {            \
  int iEnd = iOff+9;                              \
  while( (a[iOff++] & 0x80) && iOff<iEnd );       \
}

/*
** Iterator pIter currently points to the first rowid in a doclist. This
** function sets the iterator up so that iterates in reverse order through
** the doclist.
*/
static void fts5SegIterReverse(Fts5Index *p, Fts5SegIter *pIter){
  Fts5DlidxIter *pDlidx = pIter->pDlidx;
  Fts5Data *pLast = 0;
  int pgnoLast = 0;

  if( pDlidx && p->pConfig->iVersion==FTS5_CURRENT_VERSION ){
    int iSegid = pIter->pSeg->iSegid;
    pgnoLast = fts5DlidxIterPgno(pDlidx);
    pLast = fts5LeafRead(p, FTS5_SEGMENT_ROWID(iSegid, pgnoLast));
  }else{
    Fts5Data *pLeaf = pIter->pLeaf;         /* Current leaf data */

    /* Currently, Fts5SegIter.iLeafOffset points to the first byte of
    ** position-list content for the current rowid. Back it up so that it
    ** points to the start of the position-list size field. */
    int iPoslist;
    if( pIter->iTermLeafPgno==pIter->iLeafPgno ){
      iPoslist = pIter->iTermLeafOffset;
    }else{
      iPoslist = 4;
    }
    fts5IndexSkipVarint(pLeaf->p, iPoslist);
    pIter->iLeafOffset = iPoslist;

    /* If this condition is true then the largest rowid for the current
    ** term may not be stored on the current page. So search forward to
    ** see where said rowid really is.  */
    if( pIter->iEndofDoclist>=pLeaf->szLeaf ){
      int pgno;
      Fts5StructureSegment *pSeg = pIter->pSeg;

      /* The last rowid in the doclist may not be on the current page. Search
      ** forward to find the page containing the last rowid.  */
      for(pgno=pIter->iLeafPgno+1; !p->rc && pgno<=pSeg->pgnoLast; pgno++){
        i64 iAbs = FTS5_SEGMENT_ROWID(pSeg->iSegid, pgno);
        Fts5Data *pNew = fts5LeafRead(p, iAbs);
        if( pNew ){
          int iRowid, bTermless;
          iRowid = fts5LeafFirstRowidOff(pNew);
          bTermless = fts5LeafIsTermless(pNew);
          if( iRowid ){
            SWAPVAL(Fts5Data*, pNew, pLast);
            pgnoLast = pgno;
          }
          fts5DataRelease(pNew);
          if( bTermless==0 ) break;
        }
      }
    }
  }

  /* If pLast is NULL at this point, then the last rowid for this doclist
  ** lies on the page currently indicated by the iterator. In this case 
  ** pIter->iLeafOffset is already set to point to the position-list size
  ** field associated with the first relevant rowid on the page.
  **
  ** Or, if pLast is non-NULL, then it is the page that contains the last
  ** rowid. In this case configure the iterator so that it points to the
  ** first rowid on this page.
  */
  if( pLast ){
    int iOff;
    fts5DataRelease(pIter->pLeaf);
    pIter->pLeaf = pLast;
    pIter->iLeafPgno = pgnoLast;
    if( p->rc==SQLITE_OK ){
      iOff = fts5LeafFirstRowidOff(pLast);
      if( iOff>pLast->szLeaf ){
        FTS5_CORRUPT_ITER(p, pIter);
        return;
      }
      iOff += fts5GetVarint(&pLast->p[iOff], (u64*)&pIter->iRowid);
      pIter->iLeafOffset = iOff;

      if( fts5LeafIsTermless(pLast) ){
        pIter->iEndofDoclist = pLast->nn+1;
      }else{
        pIter->iEndofDoclist = fts5LeafFirstTermOff(pLast);
      }
    }
  }

  fts5SegIterReverseInitPage(p, pIter);
}

/*
** Iterator pIter currently points to the first rowid of a doclist.
** There is a doclist-index associated with the final term on the current 
** page. If the current term is the last term on the page, load the 
** doclist-index from disk and initialize an iterator at (pIter->pDlidx).
*/
static void fts5SegIterLoadDlidx(Fts5Index *p, Fts5SegIter *pIter){
  int iSeg = pIter->pSeg->iSegid;
  int bRev = (pIter->flags & FTS5_SEGITER_REVERSE);
  Fts5Data *pLeaf = pIter->pLeaf; /* Current leaf data */

  assert( pIter->flags & FTS5_SEGITER_ONETERM );
  assert( pIter->pDlidx==0 );

  /* Check if the current doclist ends on this page. If it does, return
  ** early without loading the doclist-index (as it belongs to a different
  ** term. */
  if( pIter->iTermLeafPgno==pIter->iLeafPgno 
   && pIter->iEndofDoclist<pLeaf->szLeaf 
  ){
    return;
  }

  pIter->pDlidx = fts5DlidxIterInit(p, bRev, iSeg, pIter->iTermLeafPgno);
}

/*
** The iterator object passed as the second argument currently contains
** no valid values except for the Fts5SegIter.pLeaf member variable. This
** function searches the leaf page for a term matching (pTerm/nTerm).
**
** If the specified term is found on the page, then the iterator is left
** pointing to it. If argument bGe is zero and the term is not found,
** the iterator is left pointing at EOF.
**
** If bGe is non-zero and the specified term is not found, then the
** iterator is left pointing to the smallest term in the segment that
** is larger than the specified term, even if this term is not on the
** current page.
*/
static void fts5LeafSeek(
  Fts5Index *p,                   /* Leave any error code here */
  int bGe,                        /* True for a >= search */
  Fts5SegIter *pIter,             /* Iterator to seek */
  const u8 *pTerm, int nTerm      /* Term to search for */
){
  u32 iOff;
  const u8 *a = pIter->pLeaf->p;
  u32 n = (u32)pIter->pLeaf->nn;

  u32 nMatch = 0;
  u32 nKeep = 0;
  u32 nNew = 0;
  u32 iTermOff;
  u32 iPgidx;                     /* Current offset in pgidx */
  int bEndOfPage = 0;

  assert( p->rc==SQLITE_OK );

  iPgidx = (u32)pIter->pLeaf->szLeaf;
  iPgidx += fts5GetVarint32(&a[iPgidx], iTermOff);
  iOff = iTermOff;
  if( iOff>n ){
    FTS5_CORRUPT_ITER(p, pIter);
    return;
  }

  while( 1 ){

    /* Figure out how many new bytes are in this term */
    fts5FastGetVarint32(a, iOff, nNew);
    if( nKeep<nMatch ){
      goto search_failed;
    }

    assert( nKeep>=nMatch );
    if( nKeep==nMatch ){
      u32 nCmp;
      u32 i;
      nCmp = (u32)MIN(nNew, nTerm-nMatch);
      for(i=0; i<nCmp; i++){
        if( a[iOff+i]!=pTerm[nMatch+i] ) break;
      }
      nMatch += i;

      if( (u32)nTerm==nMatch ){
        if( i==nNew ){
          goto search_success;
        }else{
          goto search_failed;
        }
      }else if( i<nNew && a[iOff+i]>pTerm[nMatch] ){
        goto search_failed;
      }
    }

    if( iPgidx>=n ){
      bEndOfPage = 1;
      break;
    }

    iPgidx += fts5GetVarint32(&a[iPgidx], nKeep);
    iTermOff += nKeep;
    iOff = iTermOff;

    if( iOff>=n ){
      FTS5_CORRUPT_ITER(p, pIter);
      return;
    }

    /* Read the nKeep field of the next term. */
    fts5FastGetVarint32(a, iOff, nKeep);
  }

 search_failed:
  if( bGe==0 ){
    fts5DataRelease(pIter->pLeaf);
    pIter->pLeaf = 0;
    return;
  }else if( bEndOfPage ){
    do {
      fts5SegIterNextPage(p, pIter);
      if( pIter->pLeaf==0 ) return;
      a = pIter->pLeaf->p;
      if( fts5LeafIsTermless(pIter->pLeaf)==0 ){
        iPgidx = (u32)pIter->pLeaf->szLeaf;
        iPgidx += fts5GetVarint32(&pIter->pLeaf->p[iPgidx], iOff);
        if( iOff<4 || (i64)iOff>=pIter->pLeaf->szLeaf ){
          FTS5_CORRUPT_ITER(p, pIter);
          return;
        }else{
          nKeep = 0;
          iTermOff = iOff;
          n = (u32)pIter->pLeaf->nn;
          iOff += fts5GetVarint32(&a[iOff], nNew);
          break;
        }
      }
    }while( 1 );
  }

 search_success:
  if( (i64)iOff+nNew>n || nNew<1 ){
    FTS5_CORRUPT_ITER(p, pIter);
    return;
  }
  pIter->iLeafOffset = iOff + nNew;
  pIter->iTermLeafOffset = pIter->iLeafOffset;
  pIter->iTermLeafPgno = pIter->iLeafPgno;

  fts5BufferSet(&p->rc, &pIter->term, nKeep, pTerm);
  fts5BufferAppendBlob(&p->rc, &pIter->term, nNew, &a[iOff]);

  if( iPgidx>=n ){
    pIter->iEndofDoclist = pIter->pLeaf->nn+1;
  }else{
    int nExtra;
    iPgidx += fts5GetVarint32(&a[iPgidx], nExtra);
    pIter->iEndofDoclist = iTermOff + nExtra;
  }
  pIter->iPgidxOff = iPgidx;

  fts5SegIterLoadRowid(p, pIter);
  fts5SegIterLoadNPos(p, pIter);
}

static sqlite3_stmt *fts5IdxSelectStmt(Fts5Index *p){
  if( p->pIdxSelect==0 ){
    Fts5Config *pConfig = p->pConfig;
    fts5IndexPrepareStmt(p, &p->pIdxSelect, sqlite3_mprintf(
          "SELECT pgno FROM '%q'.'%q_idx' WHERE "
          "segid=? AND term<=? ORDER BY term DESC LIMIT 1",
          pConfig->zDb, pConfig->zName
    ));
  }
  return p->pIdxSelect;
}

/*
** Initialize the object pIter to point to term pTerm/nTerm within segment
** pSeg. If there is no such term in the index, the iterator is set to EOF.
**
** If an error occurs, Fts5Index.rc is set to an appropriate error code. If 
** an error has already occurred when this function is called, it is a no-op.
*/
static void fts5SegIterSeekInit(
  Fts5Index *p,                   /* FTS5 backend */
  const u8 *pTerm, int nTerm,     /* Term to seek to */
  int flags,                      /* Mask of FTS5INDEX_XXX flags */
  Fts5StructureSegment *pSeg,     /* Description of segment */
  Fts5SegIter *pIter              /* Object to populate */
){
  int iPg = 1;
  int bGe = (flags & FTS5INDEX_QUERY_SCAN);
  int bDlidx = 0;                 /* True if there is a doclist-index */
  sqlite3_stmt *pIdxSelect = 0;

  assert( bGe==0 || (flags & FTS5INDEX_QUERY_DESC)==0 );
  assert( pTerm && nTerm );
  memset(pIter, 0, sizeof(*pIter));
  pIter->pSeg = pSeg;

  /* This block sets stack variable iPg to the leaf page number that may
  ** contain term (pTerm/nTerm), if it is present in the segment. */
  pIdxSelect = fts5IdxSelectStmt(p);
  if( p->rc ) return;
  sqlite3_bind_int(pIdxSelect, 1, pSeg->iSegid);
  sqlite3_bind_blob(pIdxSelect, 2, pTerm, nTerm, SQLITE_STATIC);
  if( SQLITE_ROW==sqlite3_step(pIdxSelect) ){
    i64 val = sqlite3_column_int(pIdxSelect, 0);
    iPg = (int)(val>>1);
    bDlidx = (val & 0x0001);
  }
  p->rc = sqlite3_reset(pIdxSelect);
  sqlite3_bind_null(pIdxSelect, 2);

  if( iPg<pSeg->pgnoFirst ){
    iPg = pSeg->pgnoFirst;
    bDlidx = 0;
  }

  pIter->iLeafPgno = iPg - 1;
  fts5SegIterNextPage(p, pIter);

  if( pIter->pLeaf ){
    fts5LeafSeek(p, bGe, pIter, pTerm, nTerm);
  }

  if( p->rc==SQLITE_OK && (bGe==0 || (flags & FTS5INDEX_QUERY_SCANONETERM)) ){
    pIter->flags |= FTS5_SEGITER_ONETERM;
    if( pIter->pLeaf ){
      if( flags & FTS5INDEX_QUERY_DESC ){
        pIter->flags |= FTS5_SEGITER_REVERSE;
      }
      if( bDlidx ){
        fts5SegIterLoadDlidx(p, pIter);
      }
      if( flags & FTS5INDEX_QUERY_DESC ){
        fts5SegIterReverse(p, pIter);
      }
    }
  }

  fts5SegIterSetNext(p, pIter);
  if( 0==(flags & FTS5INDEX_QUERY_SCANONETERM) ){
    fts5SegIterAllocTombstone(p, pIter);
  }

  /* Either:
  **
  **   1) an error has occurred, or
  **   2) the iterator points to EOF, or
  **   3) the iterator points to an entry with term (pTerm/nTerm), or
  **   4) the FTS5INDEX_QUERY_SCAN flag was set and the iterator points
  **      to an entry with a term greater than or equal to (pTerm/nTerm).
  */
  assert_nc( p->rc!=SQLITE_OK                                       /* 1 */
   || pIter->pLeaf==0                                               /* 2 */
   || fts5BufferCompareBlob(&pIter->term, pTerm, nTerm)==0          /* 3 */
   || (bGe && fts5BufferCompareBlob(&pIter->term, pTerm, nTerm)>0)  /* 4 */
  );
}


/*
** SQL used by fts5SegIterNextInit() to find the page to open.
*/
static sqlite3_stmt *fts5IdxNextStmt(Fts5Index *p){
  if( p->pIdxNextSelect==0 ){
    Fts5Config *pConfig = p->pConfig;
    fts5IndexPrepareStmt(p, &p->pIdxNextSelect, sqlite3_mprintf(
          "SELECT pgno FROM '%q'.'%q_idx' WHERE "
          "segid=? AND term>? ORDER BY term ASC LIMIT 1",
          pConfig->zDb, pConfig->zName
    ));
    
  }
  return p->pIdxNextSelect;
}

/*
** This is similar to fts5SegIterSeekInit(), except that it initializes
** the segment iterator to point to the first term following the page
** with pToken/nToken on it.
*/
static void fts5SegIterNextInit(
  Fts5Index *p, 
  const char *pTerm, int nTerm,
  Fts5StructureSegment *pSeg,     /* Description of segment */
  Fts5SegIter *pIter              /* Object to populate */
){
  int iPg = -1;                   /* Page of segment to open */
  int bDlidx = 0;
  sqlite3_stmt *pSel = 0;         /* SELECT to find iPg */

  pSel = fts5IdxNextStmt(p);
  if( pSel ){
    assert( p->rc==SQLITE_OK );
    sqlite3_bind_int(pSel, 1, pSeg->iSegid);
    sqlite3_bind_blob(pSel, 2, pTerm, nTerm, SQLITE_STATIC);

    if( sqlite3_step(pSel)==SQLITE_ROW ){
      i64 val = sqlite3_column_int64(pSel, 0);
      iPg = (int)(val>>1);
      bDlidx = (val & 0x0001);
    }
    p->rc = sqlite3_reset(pSel);
    sqlite3_bind_null(pSel, 2);
    if( p->rc ) return;
  }

  memset(pIter, 0, sizeof(*pIter));
  pIter->pSeg = pSeg;
  pIter->flags |= FTS5_SEGITER_ONETERM;
  if( iPg>=0 ){
    pIter->iLeafPgno = iPg - 1;
    fts5SegIterNextPage(p, pIter);
    fts5SegIterSetNext(p, pIter);
  }
  if( pIter->pLeaf ){
    const u8 *a = pIter->pLeaf->p;
    int iTermOff = 0;

    pIter->iPgidxOff = pIter->pLeaf->szLeaf;
    pIter->iPgidxOff += fts5GetVarint32(&a[pIter->iPgidxOff], iTermOff);
    pIter->iLeafOffset = iTermOff;
    fts5SegIterLoadTerm(p, pIter, 0);
    fts5SegIterLoadNPos(p, pIter);
    if( bDlidx ) fts5SegIterLoadDlidx(p, pIter);

    assert( p->rc!=SQLITE_OK || 
        fts5BufferCompareBlob(&pIter->term, (const u8*)pTerm, nTerm)>0
    );
  }
}

/*
** Initialize the object pIter to point to term pTerm/nTerm within the
** in-memory hash table. If there is no such term in the hash-table, the 
** iterator is set to EOF.
**
** If an error occurs, Fts5Index.rc is set to an appropriate error code. If 
** an error has already occurred when this function is called, it is a no-op.
*/
static void fts5SegIterHashInit(
  Fts5Index *p,                   /* FTS5 backend */
  const u8 *pTerm, int nTerm,     /* Term to seek to */
  int flags,                      /* Mask of FTS5INDEX_XXX flags */
  Fts5SegIter *pIter              /* Object to populate */
){
  int nList = 0;
  const u8 *z = 0;
  int n = 0;
  Fts5Data *pLeaf = 0;

  assert( p->pHash );
  assert( p->rc==SQLITE_OK );

  if( pTerm==0 || (flags & FTS5INDEX_QUERY_SCAN) ){
    const u8 *pList = 0;

    p->rc = sqlite3Fts5HashScanInit(p->pHash, (const char*)pTerm, nTerm);
    sqlite3Fts5HashScanEntry(p->pHash, (const char**)&z, &n, &pList, &nList);
    if( pList ){
      pLeaf = fts5IdxMalloc(p, sizeof(Fts5Data));
      if( pLeaf ){
        pLeaf->p = (u8*)pList;
      }
    }

    /* The call to sqlite3Fts5HashScanInit() causes the hash table to
    ** fill the size field of all existing position lists. This means they
    ** can no longer be appended to. Since the only scenario in which they
    ** can be appended to is if the previous operation on this table was
    ** a DELETE, by clearing the Fts5Index.bDelete flag we can avoid this
    ** possibility altogether.  */
    p->bDelete = 0;
  }else{
    p->rc = sqlite3Fts5HashQuery(p->pHash, sizeof(Fts5Data), 
        (const char*)pTerm, nTerm, (void**)&pLeaf, &nList
    );
    if( pLeaf ){
      pLeaf->p = (u8*)&pLeaf[1];
    }
    z = pTerm;
    n = nTerm;
    pIter->flags |= FTS5_SEGITER_ONETERM;
  }

  if( pLeaf ){
    sqlite3Fts5BufferSet(&p->rc, &pIter->term, n, z);
    pLeaf->nn = pLeaf->szLeaf = nList;
    pIter->pLeaf = pLeaf;
    pIter->iLeafOffset = fts5GetVarint(pLeaf->p, (u64*)&pIter->iRowid);
    pIter->iEndofDoclist = pLeaf->nn;

    if( flags & FTS5INDEX_QUERY_DESC ){
      pIter->flags |= FTS5_SEGITER_REVERSE;
      fts5SegIterReverseInitPage(p, pIter);
    }else{
      fts5SegIterLoadNPos(p, pIter);
    }
  }

  fts5SegIterSetNext(p, pIter);
}

/*
** Array ap[] contains n elements. Release each of these elements using
** fts5DataRelease(). Then free the array itself using sqlite3_free().
*/
static void fts5IndexFreeArray(Fts5Data **ap, int n){
  if( ap ){
    int ii;
    for(ii=0; ii<n; ii++){
      fts5DataRelease(ap[ii]);
    }
    sqlite3_free(ap);
  }
}

/*
** Decrement the ref-count of the object passed as the only argument. If it
** reaches 0, free it and its contents. 
*/
static void fts5TombstoneArrayDelete(Fts5TombstoneArray *p){
  if( p ){
    p->nRef--;
    if( p->nRef<=0 ){
      int ii;
      for(ii=0; ii<p->nTombstone; ii++){
        fts5DataRelease(p->apTombstone[ii]);
      }
      sqlite3_free(p);
    }
  }
}

/*
** Zero the iterator passed as the only argument.
*/
static void fts5SegIterClear(Fts5SegIter *pIter){
  fts5BufferFree(&pIter->term);
  fts5DataRelease(pIter->pLeaf);
  fts5DataRelease(pIter->pNextLeaf);
  fts5TombstoneArrayDelete(pIter->pTombArray);
  fts5DlidxIterFree(pIter->pDlidx);
  sqlite3_free(pIter->aRowidOffset);
  memset(pIter, 0, sizeof(Fts5SegIter));
}

#ifdef SQLITE_DEBUG

/*
** This function is used as part of the big assert() procedure implemented by
** fts5AssertMultiIterSetup(). It ensures that the result currently stored
** in *pRes is the correct result of comparing the current positions of the
** two iterators.
*/
static void fts5AssertComparisonResult(
  Fts5Iter *pIter, 
  Fts5SegIter *p1,
  Fts5SegIter *p2,
  Fts5CResult *pRes
){
  int i1 = p1 - pIter->aSeg;
  int i2 = p2 - pIter->aSeg;

  if( p1->pLeaf || p2->pLeaf ){
    if( p1->pLeaf==0 ){
      assert( pRes->iFirst==i2 );
    }else if( p2->pLeaf==0 ){
      assert( pRes->iFirst==i1 );
    }else{
      int nMin = MIN(p1->term.n, p2->term.n);
      int res = fts5Memcmp(p1->term.p, p2->term.p, nMin);
      if( res==0 ) res = p1->term.n - p2->term.n;

      if( res==0 ){
        assert( pRes->bTermEq==1 );
        assert( p1->iRowid!=p2->iRowid );
        res = ((p1->iRowid > p2->iRowid)==pIter->bRev) ? -1 : 1;
      }else{
        assert( pRes->bTermEq==0 );
      }

      if( res<0 ){
        assert( pRes->iFirst==i1 );
      }else{
        assert( pRes->iFirst==i2 );
      }
    }
  }
}

/*
** This function is a no-op unless SQLITE_DEBUG is defined when this module
** is compiled. In that case, this function is essentially an assert() 
** statement used to verify that the contents of the pIter->aFirst[] array
** are correct.
*/
static void fts5AssertMultiIterSetup(Fts5Index *p, Fts5Iter *pIter){
  if( p->rc==SQLITE_OK ){
    Fts5SegIter *pFirst = &pIter->aSeg[ pIter->aFirst[1].iFirst ];
    int i;

    assert( (pFirst->pLeaf==0)==pIter->base.bEof );

    /* Check that pIter->iSwitchRowid is set correctly. */
    for(i=0; i<pIter->nSeg; i++){
      Fts5SegIter *p1 = &pIter->aSeg[i];
      assert( p1==pFirst 
           || p1->pLeaf==0 
           || fts5BufferCompare(&pFirst->term, &p1->term) 
           || p1->iRowid==pIter->iSwitchRowid
           || (p1->iRowid<pIter->iSwitchRowid)==pIter->bRev
      );
    }

    for(i=0; i<pIter->nSeg; i+=2){
      Fts5SegIter *p1 = &pIter->aSeg[i];
      Fts5SegIter *p2 = &pIter->aSeg[i+1];
      Fts5CResult *pRes = &pIter->aFirst[(pIter->nSeg + i) / 2];
      fts5AssertComparisonResult(pIter, p1, p2, pRes);
    }

    for(i=1; i<(pIter->nSeg / 2); i+=2){
      Fts5SegIter *p1 = &pIter->aSeg[ pIter->aFirst[i*2].iFirst ];
      Fts5SegIter *p2 = &pIter->aSeg[ pIter->aFirst[i*2+1].iFirst ];
      Fts5CResult *pRes = &pIter->aFirst[i];
      fts5AssertComparisonResult(pIter, p1, p2, pRes);
    }
  }
}
#else
# define fts5AssertMultiIterSetup(x,y)
#endif

/*
** Do the comparison necessary to populate pIter->aFirst[iOut].
**
** If the returned value is non-zero, then it is the index of an entry
** in the pIter->aSeg[] array that is (a) not at EOF, and (b) pointing
** to a key that is a duplicate of another, higher priority, 
** segment-iterator in the pSeg->aSeg[] array.
*/
static int fts5MultiIterDoCompare(Fts5Iter *pIter, int iOut){
  int i1;                         /* Index of left-hand Fts5SegIter */
  int i2;                         /* Index of right-hand Fts5SegIter */
  int iRes;
  Fts5SegIter *p1;                /* Left-hand Fts5SegIter */
  Fts5SegIter *p2;                /* Right-hand Fts5SegIter */
  Fts5CResult *pRes = &pIter->aFirst[iOut];

  assert( iOut<pIter->nSeg && iOut>0 );
  assert( pIter->bRev==0 || pIter->bRev==1 );

  if( iOut>=(pIter->nSeg/2) ){
    i1 = (iOut - pIter->nSeg/2) * 2;
    i2 = i1 + 1;
  }else{
    i1 = pIter->aFirst[iOut*2].iFirst;
    i2 = pIter->aFirst[iOut*2+1].iFirst;
  }
  p1 = &pIter->aSeg[i1];
  p2 = &pIter->aSeg[i2];

  pRes->bTermEq = 0;
  if( p1->pLeaf==0 ){           /* If p1 is at EOF */
    iRes = i2;
  }else if( p2->pLeaf==0 ){     /* If p2 is at EOF */
    iRes = i1;
  }else{
    int res = fts5BufferCompare(&p1->term, &p2->term);
    if( res==0 ){
      assert_nc( i2>i1 );
      assert_nc( i2!=0 );
      pRes->bTermEq = 1;
      if( p1->iRowid==p2->iRowid ){
        return i2;
      }
      res = ((p1->iRowid > p2->iRowid)==pIter->bRev) ? -1 : +1;
    }
    assert( res!=0 );
    if( res<0 ){
      iRes = i1;
    }else{
      iRes = i2;
    }
  }

  pRes->iFirst = (u16)iRes;
  return 0;
}

/*
** Move the seg-iter so that it points to the first rowid on page iLeafPgno.
** It is an error if leaf iLeafPgno does not exist. Unless the db is
** a 'secure-delete' db, if it contains no rowids then this is also an error.
*/
static void fts5SegIterGotoPage(
  Fts5Index *p,                   /* FTS5 backend object */
  Fts5SegIter *pIter,             /* Iterator to advance */
  int iLeafPgno
){
  assert( iLeafPgno>pIter->iLeafPgno );

  if( iLeafPgno>pIter->pSeg->pgnoLast ){
    FTS5_CORRUPT_IDX(p);
  }else{
    fts5DataRelease(pIter->pNextLeaf);
    pIter->pNextLeaf = 0;
    pIter->iLeafPgno = iLeafPgno-1;

    while( p->rc==SQLITE_OK ){
      int iOff;
      fts5SegIterNextPage(p, pIter);
      if( pIter->pLeaf==0 ) break;
      iOff = fts5LeafFirstRowidOff(pIter->pLeaf);
      if( iOff>0 ){
        u8 *a = pIter->pLeaf->p;
        int n = pIter->pLeaf->szLeaf;
        if( iOff<4 || iOff>=n ){
          FTS5_CORRUPT_IDX(p);
        }else{
          iOff += fts5GetVarint(&a[iOff], (u64*)&pIter->iRowid);
          pIter->iLeafOffset = iOff;
          fts5SegIterLoadNPos(p, pIter);
        }
        break;
      }
    }
  }
}

/*
** Advance the iterator passed as the second argument until it is at or 
** past rowid iFrom. Regardless of the value of iFrom, the iterator is
** always advanced at least once.
*/
static void fts5SegIterNextFrom(
  Fts5Index *p,                   /* FTS5 backend object */
  Fts5SegIter *pIter,             /* Iterator to advance */
  i64 iMatch                      /* Advance iterator at least this far */
){
  int bRev = (pIter->flags & FTS5_SEGITER_REVERSE);
  Fts5DlidxIter *pDlidx = pIter->pDlidx;
  int iLeafPgno = pIter->iLeafPgno;
  int bMove = 1;

  assert( pIter->flags & FTS5_SEGITER_ONETERM );
  assert( pIter->pDlidx );
  assert( pIter->pLeaf );

  if( bRev==0 ){
    while( !fts5DlidxIterEof(p, pDlidx) && iMatch>fts5DlidxIterRowid(pDlidx) ){
      iLeafPgno = fts5DlidxIterPgno(pDlidx);
      fts5DlidxIterNext(p, pDlidx);
    }
    assert_nc( iLeafPgno>=pIter->iLeafPgno || p->rc );
    if( iLeafPgno>pIter->iLeafPgno ){
      fts5SegIterGotoPage(p, pIter, iLeafPgno);
      bMove = 0;
    }
  }else{
    assert( pIter->pNextLeaf==0 );
    assert( iMatch<pIter->iRowid );
    while( !fts5DlidxIterEof(p, pDlidx) && iMatch<fts5DlidxIterRowid(pDlidx) ){
      fts5DlidxIterPrev(p, pDlidx);
    }
    iLeafPgno = fts5DlidxIterPgno(pDlidx);

    assert( fts5DlidxIterEof(p, pDlidx) || iLeafPgno<=pIter->iLeafPgno );

    if( iLeafPgno<pIter->iLeafPgno ){
      pIter->iLeafPgno = iLeafPgno+1;
      fts5SegIterReverseNewPage(p, pIter);
      bMove = 0;
    }
  }

  do{
    if( bMove && p->rc==SQLITE_OK ) pIter->xNext(p, pIter, 0);
    if( pIter->pLeaf==0 ) break;
    if( bRev==0 && pIter->iRowid>=iMatch ) break;
    if( bRev!=0 && pIter->iRowid<=iMatch ) break;
    bMove = 1;
  }while( p->rc==SQLITE_OK );
}

/*
** Free the iterator object passed as the second argument.
*/
static void fts5MultiIterFree(Fts5Iter *pIter){
  if( pIter ){
    int i;
    for(i=0; i<pIter->nSeg; i++){
      fts5SegIterClear(&pIter->aSeg[i]);
    }
    fts5BufferFree(&pIter->poslist);
    sqlite3_free(pIter);
  }
}

static void fts5MultiIterAdvanced(
  Fts5Index *p,                   /* FTS5 backend to iterate within */
  Fts5Iter *pIter,                /* Iterator to update aFirst[] array for */
  int iChanged,                   /* Index of sub-iterator just advanced */
  int iMinset                     /* Minimum entry in aFirst[] to set */
){
  int i;
  for(i=(pIter->nSeg+iChanged)/2; i>=iMinset && p->rc==SQLITE_OK; i=i/2){
    int iEq;
    if( (iEq = fts5MultiIterDoCompare(pIter, i)) ){
      Fts5SegIter *pSeg = &pIter->aSeg[iEq];
      assert( p->rc==SQLITE_OK );
      pSeg->xNext(p, pSeg, 0);
      i = pIter->nSeg + iEq;
    }
  }
}

/*
** Sub-iterator iChanged of iterator pIter has just been advanced. It still
** points to the same term though - just a different rowid. This function
** attempts to update the contents of the pIter->aFirst[] accordingly.
** If it does so successfully, 0 is returned. Otherwise 1.
**
** If non-zero is returned, the caller should call fts5MultiIterAdvanced()
** on the iterator instead. That function does the same as this one, except
** that it deals with more complicated cases as well.
*/ 
static int fts5MultiIterAdvanceRowid(
  Fts5Iter *pIter,                /* Iterator to update aFirst[] array for */
  int iChanged,                   /* Index of sub-iterator just advanced */
  Fts5SegIter **ppFirst
){
  Fts5SegIter *pNew = &pIter->aSeg[iChanged];

  if( pNew->iRowid==pIter->iSwitchRowid
   || (pNew->iRowid<pIter->iSwitchRowid)==pIter->bRev
  ){
    int i;
    Fts5SegIter *pOther = &pIter->aSeg[iChanged ^ 0x0001];
    pIter->iSwitchRowid = pIter->bRev ? SMALLEST_INT64 : LARGEST_INT64;
    for(i=(pIter->nSeg+iChanged)/2; 1; i=i/2){
      Fts5CResult *pRes = &pIter->aFirst[i];

      assert( pNew->pLeaf );
      assert( pRes->bTermEq==0 || pOther->pLeaf );

      if( pRes->bTermEq ){
        if( pNew->iRowid==pOther->iRowid ){
          return 1;
        }else if( (pOther->iRowid>pNew->iRowid)==pIter->bRev ){
          pIter->iSwitchRowid = pOther->iRowid;
          pNew = pOther;
        }else if( (pOther->iRowid>pIter->iSwitchRowid)==pIter->bRev ){
          pIter->iSwitchRowid = pOther->iRowid;
        }
      }
      pRes->iFirst = (u16)(pNew - pIter->aSeg);
      if( i==1 ) break;

      pOther = &pIter->aSeg[ pIter->aFirst[i ^ 0x0001].iFirst ];
    }
  }

  *ppFirst = pNew;
  return 0;
}

/*
** Set the pIter->bEof variable based on the state of the sub-iterators.
*/
static void fts5MultiIterSetEof(Fts5Iter *pIter){
  Fts5SegIter *pSeg = &pIter->aSeg[ pIter->aFirst[1].iFirst ];
  pIter->base.bEof = pSeg->pLeaf==0;
  pIter->iSwitchRowid = pSeg->iRowid;
}

/*
** The argument to this macro must be an Fts5Data structure containing a
** tombstone hash page. This macro returns the key-size of the hash-page.
*/
#define TOMBSTONE_KEYSIZE(pPg) (pPg->p[0]==4 ? 4 : 8)

#define TOMBSTONE_NSLOT(pPg)   \
  ((pPg->nn > 16) ? ((pPg->nn-8) / TOMBSTONE_KEYSIZE(pPg)) : 1)

/*
** Query a single tombstone hash table for rowid iRowid. Return true if
** it is found or false otherwise. The tombstone hash table is one of
** nHashTable tables.
*/
static int fts5IndexTombstoneQuery(
  Fts5Data *pHash,                /* Hash table page to query */
  int nHashTable,                 /* Number of pages attached to segment */
  u64 iRowid                      /* Rowid to query hash for */
){
  const int szKey = TOMBSTONE_KEYSIZE(pHash);
  const int nSlot = TOMBSTONE_NSLOT(pHash);
  int iSlot = (iRowid / nHashTable) % nSlot;
  int nCollide = nSlot;

  if( iRowid==0 ){
    return pHash->p[1];
  }else if( szKey==4 ){
    u32 *aSlot = (u32*)&pHash->p[8];
    while( aSlot[iSlot] ){
      if( fts5GetU32((u8*)&aSlot[iSlot])==iRowid ) return 1;
      if( nCollide--==0 ) break;
      iSlot = (iSlot+1)%nSlot;
    }
  }else{
    u64 *aSlot = (u64*)&pHash->p[8];
    while( aSlot[iSlot] ){
      if( fts5GetU64((u8*)&aSlot[iSlot])==iRowid ) return 1;
      if( nCollide--==0 ) break;
      iSlot = (iSlot+1)%nSlot;
    }
  }

  return 0;
}

/*
** Return true if the iterator passed as the only argument points
** to an segment entry for which there is a tombstone. Return false
** if there is no tombstone or if the iterator is already at EOF.
*/
static int fts5MultiIterIsDeleted(Fts5Iter *pIter){
  int iFirst = pIter->aFirst[1].iFirst;
  Fts5SegIter *pSeg = &pIter->aSeg[iFirst];
  Fts5TombstoneArray *pArray = pSeg->pTombArray;

  if( pSeg->pLeaf && pArray ){
    /* Figure out which page the rowid might be present on. */
    int iPg = ((u64)pSeg->iRowid) % pArray->nTombstone;
    assert( iPg>=0 );

    /* If tombstone hash page iPg has not yet been loaded from the 
    ** database, load it now. */
    if( pArray->apTombstone[iPg]==0 ){
      pArray->apTombstone[iPg] = fts5DataRead(pIter->pIndex,
          FTS5_TOMBSTONE_ROWID(pSeg->pSeg->iSegid, iPg)
      );
      if( pArray->apTombstone[iPg]==0 ) return 0;
    }

    return fts5IndexTombstoneQuery(
        pArray->apTombstone[iPg],
        pArray->nTombstone,
        pSeg->iRowid
    );
  }

  return 0;
}

/*
** Move the iterator to the next entry. 
**
** If an error occurs, an error code is left in Fts5Index.rc. It is not 
** considered an error if the iterator reaches EOF, or if it is already at 
** EOF when this function is called.
*/
static void fts5MultiIterNext(
  Fts5Index *p, 
  Fts5Iter *pIter,
  int bFrom,                      /* True if argument iFrom is valid */
  i64 iFrom                       /* Advance at least as far as this */
){
  int bUseFrom = bFrom;
  assert( pIter->base.bEof==0 );
  while( p->rc==SQLITE_OK ){
    int iFirst = pIter->aFirst[1].iFirst;
    int bNewTerm = 0;
    Fts5SegIter *pSeg = &pIter->aSeg[iFirst];
    assert( p->rc==SQLITE_OK );
    if( bUseFrom && pSeg->pDlidx ){
      fts5SegIterNextFrom(p, pSeg, iFrom);
    }else{
      pSeg->xNext(p, pSeg, &bNewTerm);
    }

    if( pSeg->pLeaf==0 || bNewTerm 
     || fts5MultiIterAdvanceRowid(pIter, iFirst, &pSeg)
    ){
      fts5MultiIterAdvanced(p, pIter, iFirst, 1);
      fts5MultiIterSetEof(pIter);
      pSeg = &pIter->aSeg[pIter->aFirst[1].iFirst];
      if( pSeg->pLeaf==0 ) return;
    }

    fts5AssertMultiIterSetup(p, pIter);
    assert( pSeg==&pIter->aSeg[pIter->aFirst[1].iFirst] && pSeg->pLeaf );
    if( (pIter->bSkipEmpty==0 || pSeg->nPos) 
      && 0==fts5MultiIterIsDeleted(pIter)
    ){
      pIter->xSetOutputs(pIter, pSeg);
      return;
    }
    bUseFrom = 0;
  }
}

static void fts5MultiIterNext2(
  Fts5Index *p, 
  Fts5Iter *pIter,
  int *pbNewTerm                  /* OUT: True if *might* be new term */
){
  assert( pIter->bSkipEmpty );
  if( p->rc==SQLITE_OK ){
    *pbNewTerm = 0;
    do{
      int iFirst = pIter->aFirst[1].iFirst;
      Fts5SegIter *pSeg = &pIter->aSeg[iFirst];
      int bNewTerm = 0;

      assert( p->rc==SQLITE_OK );
      pSeg->xNext(p, pSeg, &bNewTerm);
      if( pSeg->pLeaf==0 || bNewTerm 
       || fts5MultiIterAdvanceRowid(pIter, iFirst, &pSeg)
      ){
        fts5MultiIterAdvanced(p, pIter, iFirst, 1);
        fts5MultiIterSetEof(pIter);
        *pbNewTerm = 1;
      }
      fts5AssertMultiIterSetup(p, pIter);

    }while( (fts5MultiIterIsEmpty(p, pIter) || fts5MultiIterIsDeleted(pIter)) 
         && (p->rc==SQLITE_OK)
    );
  }
}

static void fts5IterSetOutputs_Noop(Fts5Iter *pUnused1, Fts5SegIter *pUnused2){
  UNUSED_PARAM2(pUnused1, pUnused2);
}

static Fts5Iter *fts5MultiIterAlloc(
  Fts5Index *p,                   /* FTS5 backend to iterate within */
  int nSeg
){
  Fts5Iter *pNew;
  i64 nSlot;                      /* Power of two >= nSeg */

  for(nSlot=2; nSlot<nSeg; nSlot=nSlot*2);
  pNew = fts5IdxMalloc(p, 
      SZ_FTS5ITER(nSlot) +                /* pNew + pNew->aSeg[] */
      sizeof(Fts5CResult) * nSlot         /* pNew->aFirst[] */
  );
  if( pNew ){
    pNew->nSeg = nSlot;
    pNew->aFirst = (Fts5CResult*)&pNew->aSeg[nSlot];
    pNew->pIndex = p;
    pNew->xSetOutputs = fts5IterSetOutputs_Noop;
  }
  return pNew;
}

static void fts5PoslistCallback(
  Fts5Index *pUnused, 
  void *pContext, 
  const u8 *pChunk, int nChunk
){
  UNUSED_PARAM(pUnused);
  assert_nc( nChunk>=0 );
  if( nChunk>0 ){
    fts5BufferSafeAppendBlob((Fts5Buffer*)pContext, pChunk, nChunk);
  }
}

typedef struct PoslistCallbackCtx PoslistCallbackCtx;
struct PoslistCallbackCtx {
  Fts5Buffer *pBuf;               /* Append to this buffer */
  Fts5Colset *pColset;            /* Restrict matches to this column */
  int eState;                     /* See above */
};

typedef struct PoslistOffsetsCtx PoslistOffsetsCtx;
struct PoslistOffsetsCtx {
  Fts5Buffer *pBuf;               /* Append to this buffer */
  Fts5Colset *pColset;            /* Restrict matches to this column */
  int iRead;
  int iWrite;
};

/*
** TODO: Make this more efficient!
*/
static int fts5IndexColsetTest(Fts5Colset *pColset, int iCol){
  int i;
  for(i=0; i<pColset->nCol; i++){
    if( pColset->aiCol[i]==iCol ) return 1;
  }
  return 0;
}

static void fts5PoslistOffsetsCallback(
  Fts5Index *pUnused, 
  void *pContext, 
  const u8 *pChunk, int nChunk
){
  PoslistOffsetsCtx *pCtx = (PoslistOffsetsCtx*)pContext;
  UNUSED_PARAM(pUnused);
  assert_nc( nChunk>=0 );
  if( nChunk>0 ){
    int i = 0;
    while( i<nChunk ){
      int iVal;
      i += fts5GetVarint32(&pChunk[i], iVal);
      iVal += pCtx->iRead - 2;
      pCtx->iRead = iVal;
      if( fts5IndexColsetTest(pCtx->pColset, iVal) ){
        fts5BufferSafeAppendVarint(pCtx->pBuf, iVal + 2 - pCtx->iWrite);
        pCtx->iWrite = iVal;
      }
    }
  }
}

static void fts5PoslistFilterCallback(
  Fts5Index *pUnused,
  void *pContext, 
  const u8 *pChunk, int nChunk
){
  PoslistCallbackCtx *pCtx = (PoslistCallbackCtx*)pContext;
  UNUSED_PARAM(pUnused);
  assert_nc( nChunk>=0 );
  if( nChunk>0 ){
    /* Search through to find the first varint with value 1. This is the
    ** start of the next columns hits. */
    int i = 0;
    int iStart = 0;

    if( pCtx->eState==2 ){
      int iCol;
      fts5FastGetVarint32(pChunk, i, iCol);
      if( fts5IndexColsetTest(pCtx->pColset, iCol) ){
        pCtx->eState = 1;
        fts5BufferSafeAppendVarint(pCtx->pBuf, 1);
      }else{
        pCtx->eState = 0;
      }
    }

    do {
      while( i<nChunk && pChunk[i]!=0x01 ){
        while( pChunk[i] & 0x80 ) i++;
        i++;
      }
      if( pCtx->eState ){
        fts5BufferSafeAppendBlob(pCtx->pBuf, &pChunk[iStart], i-iStart);
      }
      if( i<nChunk ){
        int iCol;
        iStart = i;
        i++;
        if( i>=nChunk ){
          pCtx->eState = 2;
        }else{
          fts5FastGetVarint32(pChunk, i, iCol);
          pCtx->eState = fts5IndexColsetTest(pCtx->pColset, iCol);
          if( pCtx->eState ){
            fts5BufferSafeAppendBlob(pCtx->pBuf, &pChunk[iStart], i-iStart);
            iStart = i;
          }
        }
      }
    }while( i<nChunk );
  }
}

static void fts5ChunkIterate(
  Fts5Index *p,                   /* Index object */
  Fts5SegIter *pSeg,              /* Poslist of this iterator */
  void *pCtx,                     /* Context pointer for xChunk callback */
  void (*xChunk)(Fts5Index*, void*, const u8*, int)
){
  int nRem = pSeg->nPos;          /* Number of bytes still to come */
  Fts5Data *pData = 0;
  u8 *pChunk = &pSeg->pLeaf->p[pSeg->iLeafOffset];
  int nChunk = MIN(nRem, pSeg->pLeaf->szLeaf - pSeg->iLeafOffset);
  int pgno = pSeg->iLeafPgno;
  int pgnoSave = 0;

  /* This function does not work with detail=none databases. */
  assert( p->pConfig->eDetail!=FTS5_DETAIL_NONE );

  if( (pSeg->flags & FTS5_SEGITER_REVERSE)==0 ){
    pgnoSave = pgno+1;
  }

  while( 1 ){
    xChunk(p, pCtx, pChunk, nChunk);
    nRem -= nChunk;
    fts5DataRelease(pData);
    if( nRem<=0 ){
      break;
    }else if( pSeg->pSeg==0 ){
      FTS5_CORRUPT_IDX(p);
      return;
    }else{
      pgno++;
      pData = fts5LeafRead(p, FTS5_SEGMENT_ROWID(pSeg->pSeg->iSegid, pgno));
      if( pData==0 ) break;
      pChunk = &pData->p[4];
      nChunk = MIN(nRem, pData->szLeaf - 4);
      if( pgno==pgnoSave ){
        assert( pSeg->pNextLeaf==0 );
        pSeg->pNextLeaf = pData;
        pData = 0;
      }
    }
  }
}

/*
** Iterator pIter currently points to a valid entry (not EOF). This
** function appends the position list data for the current entry to
** buffer pBuf. It does not make a copy of the position-list size
** field.
*/
static void fts5SegiterPoslist(
  Fts5Index *p,
  Fts5SegIter *pSeg,
  Fts5Colset *pColset,
  Fts5Buffer *pBuf
){
  assert( pBuf!=0 );
  assert( pSeg!=0 );
  if( 0==fts5BufferGrow(&p->rc, pBuf, pSeg->nPos+FTS5_DATA_ZERO_PADDING) ){
    assert( pBuf->p!=0 );
    assert( pBuf->nSpace >= pBuf->n+pSeg->nPos+FTS5_DATA_ZERO_PADDING );
    memset(&pBuf->p[pBuf->n+pSeg->nPos], 0, FTS5_DATA_ZERO_PADDING);
    if( pColset==0 ){
      fts5ChunkIterate(p, pSeg, (void*)pBuf, fts5PoslistCallback);
    }else{
      if( p->pConfig->eDetail==FTS5_DETAIL_FULL ){
        PoslistCallbackCtx sCtx;
        sCtx.pBuf = pBuf;
        sCtx.pColset = pColset;
        sCtx.eState = fts5IndexColsetTest(pColset, 0);
        assert( sCtx.eState==0 || sCtx.eState==1 );
        fts5ChunkIterate(p, pSeg, (void*)&sCtx, fts5PoslistFilterCallback);
      }else{
        PoslistOffsetsCtx sCtx;
        memset(&sCtx, 0, sizeof(sCtx));
        sCtx.pBuf = pBuf;
        sCtx.pColset = pColset;
        fts5ChunkIterate(p, pSeg, (void*)&sCtx, fts5PoslistOffsetsCallback);
      }
    }
  }
}

/*
** Parameter pPos points to a buffer containing a position list, size nPos.
** This function filters it according to pColset (which must be non-NULL)
** and sets pIter->base.pData/nData to point to the new position list.
** If memory is required for the new position list, use buffer pIter->poslist.
** Or, if the new position list is a contiguous subset of the input, set
** pIter->base.pData/nData to point directly to it.
**
** This function is a no-op if *pRc is other than SQLITE_OK when it is
** called. If an OOM error is encountered, *pRc is set to SQLITE_NOMEM
** before returning.
*/
static void fts5IndexExtractColset(
  int *pRc,
  Fts5Colset *pColset,            /* Colset to filter on */
  const u8 *pPos, int nPos,       /* Position list */
  Fts5Iter *pIter
){
  if( *pRc==SQLITE_OK ){
    const u8 *p = pPos;
    const u8 *aCopy = p;
    const u8 *pEnd = &p[nPos];    /* One byte past end of position list */
    int i = 0;
    int iCurrent = 0;

    if( pColset->nCol>1 && sqlite3Fts5BufferSize(pRc, &pIter->poslist, nPos) ){
      return;
    }

    while( 1 ){
      while( pColset->aiCol[i]<iCurrent ){
        i++;
        if( i==pColset->nCol ){
          pIter->base.pData = pIter->poslist.p;
          pIter->base.nData = pIter->poslist.n;
          return;
        }
      }

      /* Advance pointer p until it points to pEnd or an 0x01 byte that is
      ** not part of a varint */
      while( p<pEnd && *p!=0x01 ){
        while( *p++ & 0x80 );
      }

      if( pColset->aiCol[i]==iCurrent ){
        if( pColset->nCol==1 ){
          pIter->base.pData = aCopy;
          pIter->base.nData = p-aCopy;
          return;
        }
        fts5BufferSafeAppendBlob(&pIter->poslist, aCopy, p-aCopy);
      }
      if( p>=pEnd ){
        pIter->base.pData = pIter->poslist.p;
        pIter->base.nData = pIter->poslist.n;
        return;
      }
      aCopy = p++;
      iCurrent = *p++;
      if( iCurrent & 0x80 ){
        p--;
        p += fts5GetVarint32(p, iCurrent);
      }
    }
  }

}

/*
** xSetOutputs callback used by detail=none tables.
*/
static void fts5IterSetOutputs_None(Fts5Iter *pIter, Fts5SegIter *pSeg){
  assert( pIter->pIndex->pConfig->eDetail==FTS5_DETAIL_NONE );
  pIter->base.iRowid = pSeg->iRowid;
  pIter->base.nData = pSeg->nPos;
}

/*
** xSetOutputs callback used by detail=full and detail=col tables when no
** column filters are specified.
*/
static void fts5IterSetOutputs_Nocolset(Fts5Iter *pIter, Fts5SegIter *pSeg){
  pIter->base.iRowid = pSeg->iRowid;
  pIter->base.nData = pSeg->nPos;

  assert( pIter->pIndex->pConfig->eDetail!=FTS5_DETAIL_NONE );
  assert( pIter->pColset==0 );

  if( pSeg->iLeafOffset+pSeg->nPos<=pSeg->pLeaf->szLeaf ){
    /* All data is stored on the current page. Populate the output 
    ** variables to point into the body of the page object. */
    pIter->base.pData = &pSeg->pLeaf->p[pSeg->iLeafOffset];
  }else{
    /* The data is distributed over two or more pages. Copy it into the
    ** Fts5Iter.poslist buffer and then set the output pointer to point
    ** to this buffer.  */
    fts5BufferZero(&pIter->poslist);
    fts5SegiterPoslist(pIter->pIndex, pSeg, 0, &pIter->poslist);
    pIter->base.pData = pIter->poslist.p;
  }
}

/*
** xSetOutputs callback used when the Fts5Colset object has nCol==0 (match
** against no columns at all).
*/
static void fts5IterSetOutputs_ZeroColset(Fts5Iter *pIter, Fts5SegIter *pSeg){
  UNUSED_PARAM(pSeg);
  pIter->base.nData = 0;
}

/*
** xSetOutputs callback used by detail=col when there is a column filter
** and there are 100 or more columns. Also called as a fallback from
** fts5IterSetOutputs_Col100 if the column-list spans more than one page.
*/
static void fts5IterSetOutputs_Col(Fts5Iter *pIter, Fts5SegIter *pSeg){
  fts5BufferZero(&pIter->poslist);
  fts5SegiterPoslist(pIter->pIndex, pSeg, pIter->pColset, &pIter->poslist);
  pIter->base.iRowid = pSeg->iRowid;
  pIter->base.pData = pIter->poslist.p;
  pIter->base.nData = pIter->poslist.n;
}

/*
** xSetOutputs callback used when: 
**
**   * detail=col,
**   * there is a column filter, and
**   * the table contains 100 or fewer columns. 
**
** The last point is to ensure all column numbers are stored as 
** single-byte varints.
*/
static void fts5IterSetOutputs_Col100(Fts5Iter *pIter, Fts5SegIter *pSeg){

  assert( pIter->pIndex->pConfig->eDetail==FTS5_DETAIL_COLUMNS );
  assert( pIter->pColset );

  if( pSeg->iLeafOffset+pSeg->nPos>pSeg->pLeaf->szLeaf ){
    fts5IterSetOutputs_Col(pIter, pSeg);
  }else{
    u8 *a = (u8*)&pSeg->pLeaf->p[pSeg->iLeafOffset];
    u8 *pEnd = (u8*)&a[pSeg->nPos]; 
    int iPrev = 0;
    int *aiCol = pIter->pColset->aiCol;
    int *aiColEnd = &aiCol[pIter->pColset->nCol];

    u8 *aOut = pIter->poslist.p;
    int iPrevOut = 0;

    pIter->base.iRowid = pSeg->iRowid;

    while( a<pEnd ){
      iPrev += (int)a++[0] - 2;
      while( *aiCol<iPrev ){
        aiCol++;
        if( aiCol==aiColEnd ) goto setoutputs_col_out;
      }
      if( *aiCol==iPrev ){
        *aOut++ = (u8)((iPrev - iPrevOut) + 2);
        iPrevOut = iPrev;
      }
    }

setoutputs_col_out:
    pIter->base.pData = pIter->poslist.p;
    pIter->base.nData = aOut - pIter->poslist.p;
  }
}

/*
** xSetOutputs callback used by detail=full when there is a column filter.
*/
static void fts5IterSetOutputs_Full(Fts5Iter *pIter, Fts5SegIter *pSeg){
  Fts5Colset *pColset = pIter->pColset;
  pIter->base.iRowid = pSeg->iRowid;

  assert( pIter->pIndex->pConfig->eDetail==FTS5_DETAIL_FULL );
  assert( pColset );

  if( pSeg->iLeafOffset+pSeg->nPos<=pSeg->pLeaf->szLeaf ){
    /* All data is stored on the current page. Populate the output 
    ** variables to point into the body of the page object. */
    const u8 *a = &pSeg->pLeaf->p[pSeg->iLeafOffset];
    int *pRc = &pIter->pIndex->rc;
    fts5BufferZero(&pIter->poslist);
    fts5IndexExtractColset(pRc, pColset, a, pSeg->nPos, pIter);
  }else{
    /* The data is distributed over two or more pages. Copy it into the
    ** Fts5Iter.poslist buffer and then set the output pointer to point
    ** to this buffer.  */
    fts5BufferZero(&pIter->poslist);
    fts5SegiterPoslist(pIter->pIndex, pSeg, pColset, &pIter->poslist);
    pIter->base.pData = pIter->poslist.p;
    pIter->base.nData = pIter->poslist.n;
  }
}

static void fts5IterSetOutputCb(int *pRc, Fts5Iter *pIter){
  assert( pIter!=0 || (*pRc)!=SQLITE_OK );
  if( *pRc==SQLITE_OK ){
    Fts5Config *pConfig = pIter->pIndex->pConfig;
    if( pConfig->eDetail==FTS5_DETAIL_NONE ){
      pIter->xSetOutputs = fts5IterSetOutputs_None;
    }

    else if( pIter->pColset==0 ){
      pIter->xSetOutputs = fts5IterSetOutputs_Nocolset;
    }

    else if( pIter->pColset->nCol==0 ){
      pIter->xSetOutputs = fts5IterSetOutputs_ZeroColset;
    }

    else if( pConfig->eDetail==FTS5_DETAIL_FULL ){
      pIter->xSetOutputs = fts5IterSetOutputs_Full;
    }

    else{
      assert( pConfig->eDetail==FTS5_DETAIL_COLUMNS );
      if( pConfig->nCol<=100 ){
        pIter->xSetOutputs = fts5IterSetOutputs_Col100;
        sqlite3Fts5BufferSize(pRc, &pIter->poslist, pConfig->nCol);
      }else{
        pIter->xSetOutputs = fts5IterSetOutputs_Col;
      }
    }
  }
}

/*
** All the component segment-iterators of pIter have been set up. This
** functions finishes setup for iterator pIter itself.
*/
static void fts5MultiIterFinishSetup(Fts5Index *p, Fts5Iter *pIter){
  int iIter;
  for(iIter=pIter->nSeg-1; iIter>0; iIter--){
    int iEq;
    if( (iEq = fts5MultiIterDoCompare(pIter, iIter)) ){
      Fts5SegIter *pSeg = &pIter->aSeg[iEq];
      if( p->rc==SQLITE_OK ) pSeg->xNext(p, pSeg, 0);
      fts5MultiIterAdvanced(p, pIter, iEq, iIter);
    }
  }
  fts5MultiIterSetEof(pIter);
  fts5AssertMultiIterSetup(p, pIter);

  if( (pIter->bSkipEmpty && fts5MultiIterIsEmpty(p, pIter))
   || fts5MultiIterIsDeleted(pIter)
  ){
    fts5MultiIterNext(p, pIter, 0, 0);
  }else if( pIter->base.bEof==0 ){
    Fts5SegIter *pSeg = &pIter->aSeg[pIter->aFirst[1].iFirst];
    pIter->xSetOutputs(pIter, pSeg);
  }
}

/*
** Allocate a new Fts5Iter object.
**
** The new object will be used to iterate through data in structure pStruct.
** If iLevel is -ve, then all data in all segments is merged. Or, if iLevel
** is zero or greater, data from the first nSegment segments on level iLevel
** is merged.
**
** The iterator initially points to the first term/rowid entry in the 
** iterated data.
*/
static void fts5MultiIterNew(
  Fts5Index *p,                   /* FTS5 backend to iterate within */
  Fts5Structure *pStruct,         /* Structure of specific index */
  int flags,                      /* FTS5INDEX_QUERY_XXX flags */
  Fts5Colset *pColset,            /* Colset to filter on (or NULL) */
  const u8 *pTerm, int nTerm,     /* Term to seek to (or NULL/0) */
  int iLevel,                     /* Level to iterate (-1 for all) */
  int nSegment,                   /* Number of segments to merge (iLevel>=0) */
  Fts5Iter **ppOut                /* New object */
){
  int nSeg = 0;                   /* Number of segment-iters in use */
  int iIter = 0;                  /* */
  int iSeg;                       /* Used to iterate through segments */
  Fts5StructureLevel *pLvl;
  Fts5Iter *pNew;

  assert( (pTerm==0 && nTerm==0) || iLevel<0 );

  /* Allocate space for the new multi-seg-iterator. */
  if( p->rc==SQLITE_OK ){
    if( iLevel<0 ){
      assert( pStruct->nSegment==fts5StructureCountSegments(pStruct) );
      nSeg = pStruct->nSegment;
      nSeg += (p->pHash && 0==(flags & FTS5INDEX_QUERY_SKIPHASH));
    }else{
      nSeg = MIN(pStruct->aLevel[iLevel].nSeg, nSegment);
    }
  }
  *ppOut = pNew = fts5MultiIterAlloc(p, nSeg);
  if( pNew==0 ){
    assert( p->rc!=SQLITE_OK );
    goto fts5MultiIterNew_post_check;
  }
  pNew->bRev = (0!=(flags & FTS5INDEX_QUERY_DESC));
  pNew->bSkipEmpty = (0!=(flags & FTS5INDEX_QUERY_SKIPEMPTY));
  pNew->pColset = pColset;
  if( (flags & FTS5INDEX_QUERY_NOOUTPUT)==0 ){
    fts5IterSetOutputCb(&p->rc, pNew);
  }

  /* Initialize each of the component segment iterators. */
  if( p->rc==SQLITE_OK ){
    if( iLevel<0 ){
      Fts5StructureLevel *pEnd = &pStruct->aLevel[pStruct->nLevel];
      if( p->pHash && 0==(flags & FTS5INDEX_QUERY_SKIPHASH) ){
        /* Add a segment iterator for the current contents of the hash table. */
        Fts5SegIter *pIter = &pNew->aSeg[iIter++];
        fts5SegIterHashInit(p, pTerm, nTerm, flags, pIter);
      }
      for(pLvl=&pStruct->aLevel[0]; pLvl<pEnd; pLvl++){
        for(iSeg=pLvl->nSeg-1; iSeg>=0; iSeg--){
          Fts5StructureSegment *pSeg = &pLvl->aSeg[iSeg];
          Fts5SegIter *pIter = &pNew->aSeg[iIter++];
          if( pTerm==0 ){
            fts5SegIterInit(p, pSeg, pIter);
          }else{
            fts5SegIterSeekInit(p, pTerm, nTerm, flags, pSeg, pIter);
          }
        }
      }
    }else{
      pLvl = &pStruct->aLevel[iLevel];
      for(iSeg=nSeg-1; iSeg>=0; iSeg--){
        fts5SegIterInit(p, &pLvl->aSeg[iSeg], &pNew->aSeg[iIter++]);
      }
    }
    assert( iIter==nSeg );
  }

  /* If the above was successful, each component iterator now points 
  ** to the first entry in its segment. In this case initialize the 
  ** aFirst[] array. Or, if an error has occurred, free the iterator
  ** object and set the output variable to NULL.  */
  if( p->rc==SQLITE_OK ){
    fts5MultiIterFinishSetup(p, pNew);
  }else{
    fts5MultiIterFree(pNew);
    *ppOut = 0;
  }

fts5MultiIterNew_post_check:
  assert( (*ppOut)!=0 || p->rc!=SQLITE_OK );
  return;
}

/*
** Create an Fts5Iter that iterates through the doclist provided
** as the second argument.
*/
static void fts5MultiIterNew2(
  Fts5Index *p,                   /* FTS5 backend to iterate within */
  Fts5Data *pData,                /* Doclist to iterate through */
  int bDesc,                      /* True for descending rowid order */
  Fts5Iter **ppOut                /* New object */
){
  Fts5Iter *pNew;
  pNew = fts5MultiIterAlloc(p, 2);
  if( pNew ){
    Fts5SegIter *pIter = &pNew->aSeg[1];
    pIter->flags = FTS5_SEGITER_ONETERM;
    if( pData->szLeaf>0 ){
      pIter->pLeaf = pData;
      pIter->iLeafOffset = fts5GetVarint(pData->p, (u64*)&pIter->iRowid);
      pIter->iEndofDoclist = pData->nn;
      pNew->aFirst[1].iFirst = 1;
      if( bDesc ){
        pNew->bRev = 1;
        pIter->flags |= FTS5_SEGITER_REVERSE;
        fts5SegIterReverseInitPage(p, pIter);
      }else{
        fts5SegIterLoadNPos(p, pIter);
      }
      pData = 0;
    }else{
      pNew->base.bEof = 1;
    }
    fts5SegIterSetNext(p, pIter);

    *ppOut = pNew;
  }

  fts5DataRelease(pData);
}

/*
** Return true if the iterator is at EOF or if an error has occurred. 
** False otherwise.
*/
static int fts5MultiIterEof(Fts5Index *p, Fts5Iter *pIter){
  assert( pIter!=0 || p->rc!=SQLITE_OK );
  assert( p->rc!=SQLITE_OK
      || (pIter->aSeg[ pIter->aFirst[1].iFirst ].pLeaf==0)==pIter->base.bEof 
  );
  return (p->rc || pIter->base.bEof);
}

/*
** Return the rowid of the entry that the iterator currently points
** to. If the iterator points to EOF when this function is called the
** results are undefined.
*/
static i64 fts5MultiIterRowid(Fts5Iter *pIter){
  assert( pIter->aSeg[ pIter->aFirst[1].iFirst ].pLeaf );
  return pIter->aSeg[ pIter->aFirst[1].iFirst ].iRowid;
}

/*
** Move the iterator to the next entry at or following iMatch.
*/
static void fts5MultiIterNextFrom(
  Fts5Index *p, 
  Fts5Iter *pIter, 
  i64 iMatch
){
  while( 1 ){
    i64 iRowid;
    fts5MultiIterNext(p, pIter, 1, iMatch);
    if( fts5MultiIterEof(p, pIter) ) break;
    iRowid = fts5MultiIterRowid(pIter);
    if( pIter->bRev==0 && iRowid>=iMatch ) break;
    if( pIter->bRev!=0 && iRowid<=iMatch ) break;
  }
}

/*
** Return a pointer to a buffer containing the term associated with the 
** entry that the iterator currently points to.
*/
static const u8 *fts5MultiIterTerm(Fts5Iter *pIter, int *pn){
  Fts5SegIter *p = &pIter->aSeg[ pIter->aFirst[1].iFirst ];
  *pn = p->term.n;
  return p->term.p;
}

/*
** Allocate a new segment-id for the structure pStruct. The new segment
** id must be between 1 and 65335 inclusive, and must not be used by 
** any currently existing segment. If a free segment id cannot be found,
** SQLITE_FULL is returned.
**
** If an error has already occurred, this function is a no-op. 0 is 
** returned in this case.
*/
static int fts5AllocateSegid(Fts5Index *p, Fts5Structure *pStruct){
  int iSegid = 0;

  if( p->rc==SQLITE_OK ){
    if( pStruct->nSegment>=FTS5_MAX_SEGMENT ){
      p->rc = SQLITE_FULL;
    }else{
      /* FTS5_MAX_SEGMENT is currently defined as 2000. So the following
      ** array is 63 elements, or 252 bytes, in size.  */
      u32 aUsed[(FTS5_MAX_SEGMENT+31) / 32];
      int iLvl, iSeg;
      int i;
      u32 mask;
      memset(aUsed, 0, sizeof(aUsed));
      for(iLvl=0; iLvl<pStruct->nLevel; iLvl++){
        for(iSeg=0; iSeg<pStruct->aLevel[iLvl].nSeg; iSeg++){
          int iId = pStruct->aLevel[iLvl].aSeg[iSeg].iSegid;
          if( iId<=FTS5_MAX_SEGMENT && iId>0 ){
            aUsed[(iId-1) / 32] |= (u32)1 << ((iId-1) % 32);
          }
        }
      }

      for(i=0; aUsed[i]==0xFFFFFFFF; i++);
      mask = aUsed[i];
      for(iSegid=0; mask & ((u32)1 << iSegid); iSegid++);
      iSegid += 1 + i*32;

#ifdef SQLITE_DEBUG
      for(iLvl=0; iLvl<pStruct->nLevel; iLvl++){
        for(iSeg=0; iSeg<pStruct->aLevel[iLvl].nSeg; iSeg++){
          assert_nc( iSegid!=pStruct->aLevel[iLvl].aSeg[iSeg].iSegid );
        }
      }
      assert_nc( iSegid>0 && iSegid<=FTS5_MAX_SEGMENT );

      {
        sqlite3_stmt *pIdxSelect = fts5IdxSelectStmt(p);
        if( p->rc==SQLITE_OK ){
          u8 aBlob[2] = {0xff, 0xff};
          sqlite3_bind_int(pIdxSelect, 1, iSegid);
          sqlite3_bind_blob(pIdxSelect, 2, aBlob, 2, SQLITE_STATIC);
          assert_nc( sqlite3_step(pIdxSelect)!=SQLITE_ROW );
          p->rc = sqlite3_reset(pIdxSelect);
          sqlite3_bind_null(pIdxSelect, 2);
        }
      }
#endif
    }
  }

  return iSegid;
}

/*
** Discard all data currently cached in the hash-tables.
*/
static void fts5IndexDiscardData(Fts5Index *p){
  assert( p->pHash || p->nPendingData==0 );
  if( p->pHash ){
    sqlite3Fts5HashClear(p->pHash);
    p->nPendingData = 0;
    p->nPendingRow = 0;
    p->flushRc = SQLITE_OK;
  }
  p->nContentlessDelete = 0;
}

/*
** Return the size of the prefix, in bytes, that buffer 
** (pNew/<length-unknown>) shares with buffer (pOld/nOld).
**
** Buffer (pNew/<length-unknown>) is guaranteed to be greater 
** than buffer (pOld/nOld).
*/
static int fts5PrefixCompress(int nOld, const u8 *pOld, const u8 *pNew){
  int i;
  for(i=0; i<nOld; i++){
    if( pOld[i]!=pNew[i] ) break;
  }
  return i;
}

static void fts5WriteDlidxClear(
  Fts5Index *p, 
  Fts5SegWriter *pWriter,
  int bFlush                      /* If true, write dlidx to disk */
){
  int i;
  assert( bFlush==0 || (pWriter->nDlidx>0 && pWriter->aDlidx[0].buf.n>0) );
  for(i=0; i<pWriter->nDlidx; i++){
    Fts5DlidxWriter *pDlidx = &pWriter->aDlidx[i];
    if( pDlidx->buf.n==0 ) break;
    if( bFlush ){
      assert( pDlidx->pgno!=0 );
      fts5DataWrite(p, 
          FTS5_DLIDX_ROWID(pWriter->iSegid, i, pDlidx->pgno),
          pDlidx->buf.p, pDlidx->buf.n
      );
    }
    sqlite3Fts5BufferZero(&pDlidx->buf);
    pDlidx->bPrevValid = 0;
  }
}

/*
** Grow the pWriter->aDlidx[] array to at least nLvl elements in size.
** Any new array elements are zeroed before returning.
*/
static int fts5WriteDlidxGrow(
  Fts5Index *p,
  Fts5SegWriter *pWriter,
  int nLvl
){
  if( p->rc==SQLITE_OK && nLvl>=pWriter->nDlidx ){
    Fts5DlidxWriter *aDlidx = (Fts5DlidxWriter*)sqlite3_realloc64(
        pWriter->aDlidx, sizeof(Fts5DlidxWriter) * nLvl
    );
    if( aDlidx==0 ){
      p->rc = SQLITE_NOMEM;
    }else{
      size_t nByte = sizeof(Fts5DlidxWriter) * (nLvl - pWriter->nDlidx);
      memset(&aDlidx[pWriter->nDlidx], 0, nByte);
      pWriter->aDlidx = aDlidx;
      pWriter->nDlidx = nLvl;
    }
  }
  return p->rc;
}

/*
** If the current doclist-index accumulating in pWriter->aDlidx[] is large
** enough, flush it to disk and return 1. Otherwise discard it and return
** zero.
*/
static int fts5WriteFlushDlidx(Fts5Index *p, Fts5SegWriter *pWriter){
  int bFlag = 0;

  /* If there were FTS5_MIN_DLIDX_SIZE or more empty leaf pages written
  ** to the database, also write the doclist-index to disk.  */
  if( pWriter->aDlidx[0].buf.n>0 && pWriter->nEmpty>=FTS5_MIN_DLIDX_SIZE ){
    bFlag = 1;
  }
  fts5WriteDlidxClear(p, pWriter, bFlag);
  pWriter->nEmpty = 0;
  return bFlag;
}

/*
** This function is called whenever processing of the doclist for the 
** last term on leaf page (pWriter->iBtPage) is completed. 
**
** The doclist-index for that term is currently stored in-memory within the
** Fts5SegWriter.aDlidx[] array. If it is large enough, this function
** writes it out to disk. Or, if it is too small to bother with, discards
** it.
**
** Fts5SegWriter.btterm currently contains the first term on page iBtPage.
*/
static void fts5WriteFlushBtree(Fts5Index *p, Fts5SegWriter *pWriter){
  int bFlag;

  assert( pWriter->iBtPage || pWriter->nEmpty==0 );
  if( pWriter->iBtPage==0 ) return;
  bFlag = fts5WriteFlushDlidx(p, pWriter);

  if( p->rc==SQLITE_OK ){
    const char *z = (pWriter->btterm.n>0?(const char*)pWriter->btterm.p:"");
    /* The following was already done in fts5WriteInit(): */
    /* sqlite3_bind_int(p->pIdxWriter, 1, pWriter->iSegid); */
    sqlite3_bind_blob(p->pIdxWriter, 2, z, pWriter->btterm.n, SQLITE_STATIC);
    sqlite3_bind_int64(p->pIdxWriter, 3, bFlag + ((i64)pWriter->iBtPage<<1));
    sqlite3_step(p->pIdxWriter);
    p->rc = sqlite3_reset(p->pIdxWriter);
    sqlite3_bind_null(p->pIdxWriter, 2);
  }
  pWriter->iBtPage = 0;
}

/*
** This is called once for each leaf page except the first that contains
** at least one term. Argument (nTerm/pTerm) is the split-key - a term that
** is larger than all terms written to earlier leaves, and equal to or
** smaller than the first term on the new leaf.
**
** If an error occurs, an error code is left in Fts5Index.rc. If an error
** has already occurred when this function is called, it is a no-op.
*/
static void fts5WriteBtreeTerm(
  Fts5Index *p,                   /* FTS5 backend object */
  Fts5SegWriter *pWriter,         /* Writer object */
  int nTerm, const u8 *pTerm      /* First term on new page */
){
  fts5WriteFlushBtree(p, pWriter);
  if( p->rc==SQLITE_OK ){
    fts5BufferSet(&p->rc, &pWriter->btterm, nTerm, pTerm);
    pWriter->iBtPage = pWriter->writer.pgno;
  }
}

/*
** This function is called when flushing a leaf page that contains no
** terms at all to disk.
*/
static void fts5WriteBtreeNoTerm(
  Fts5Index *p,                   /* FTS5 backend object */
  Fts5SegWriter *pWriter          /* Writer object */
){
  /* If there were no rowids on the leaf page either and the doclist-index
  ** has already been started, append an 0x00 byte to it.  */
  if( pWriter->bFirstRowidInPage && pWriter->aDlidx[0].buf.n>0 ){
    Fts5DlidxWriter *pDlidx = &pWriter->aDlidx[0];
    assert( pDlidx->bPrevValid );
    sqlite3Fts5BufferAppendVarint(&p->rc, &pDlidx->buf, 0);
  }

  /* Increment the "number of sequential leaves without a term" counter. */
  pWriter->nEmpty++;
}

static i64 fts5DlidxExtractFirstRowid(Fts5Buffer *pBuf){
  i64 iRowid;
  int iOff;

  iOff = 1 + fts5GetVarint(&pBuf->p[1], (u64*)&iRowid);
  fts5GetVarint(&pBuf->p[iOff], (u64*)&iRowid);
  return iRowid;
}

/*
** Rowid iRowid has just been appended to the current leaf page. It is the
** first on the page. This function appends an appropriate entry to the current
** doclist-index.
*/
static void fts5WriteDlidxAppend(
  Fts5Index *p, 
  Fts5SegWriter *pWriter, 
  i64 iRowid
){
  int i;
  int bDone = 0;

  for(i=0; p->rc==SQLITE_OK && bDone==0; i++){
    i64 iVal;
    Fts5DlidxWriter *pDlidx = &pWriter->aDlidx[i];

    if( pDlidx->buf.n>=p->pConfig->pgsz ){
      /* The current doclist-index page is full. Write it to disk and push
      ** a copy of iRowid (which will become the first rowid on the next
      ** doclist-index leaf page) up into the next level of the b-tree 
      ** hierarchy. If the node being flushed is currently the root node,
      ** also push its first rowid upwards. */
      pDlidx->buf.p[0] = 0x01;    /* Not the root node */
      fts5DataWrite(p, 
          FTS5_DLIDX_ROWID(pWriter->iSegid, i, pDlidx->pgno),
          pDlidx->buf.p, pDlidx->buf.n
      );
      fts5WriteDlidxGrow(p, pWriter, i+2);
      pDlidx = &pWriter->aDlidx[i];
      if( p->rc==SQLITE_OK && pDlidx[1].buf.n==0 ){
        i64 iFirst = fts5DlidxExtractFirstRowid(&pDlidx->buf);

        /* This was the root node. Push its first rowid up to the new root. */
        pDlidx[1].pgno = pDlidx->pgno;
        sqlite3Fts5BufferAppendVarint(&p->rc, &pDlidx[1].buf, 0);
        sqlite3Fts5BufferAppendVarint(&p->rc, &pDlidx[1].buf, pDlidx->pgno);
        sqlite3Fts5BufferAppendVarint(&p->rc, &pDlidx[1].buf, iFirst);
        pDlidx[1].bPrevValid = 1;
        pDlidx[1].iPrev = iFirst;
      }

      sqlite3Fts5BufferZero(&pDlidx->buf);
      pDlidx->bPrevValid = 0;
      pDlidx->pgno++;
    }else{
      bDone = 1;
    }

    if( pDlidx->bPrevValid ){
      iVal = (u64)iRowid - (u64)pDlidx->iPrev;
    }else{
      i64 iPgno = (i==0 ? pWriter->writer.pgno : pDlidx[-1].pgno);
      assert( pDlidx->buf.n==0 );
      sqlite3Fts5BufferAppendVarint(&p->rc, &pDlidx->buf, !bDone);
      sqlite3Fts5BufferAppendVarint(&p->rc, &pDlidx->buf, iPgno);
      iVal = iRowid;
    }

    sqlite3Fts5BufferAppendVarint(&p->rc, &pDlidx->buf, iVal);
    pDlidx->bPrevValid = 1;
    pDlidx->iPrev = iRowid;
  }
}

static void fts5WriteFlushLeaf(Fts5Index *p, Fts5SegWriter *pWriter){
  static const u8 zero[] = { 0x00, 0x00, 0x00, 0x00 };
  Fts5PageWriter *pPage = &pWriter->writer;
  i64 iRowid;

  assert( (pPage->pgidx.n==0)==(pWriter->bFirstTermInPage) );

  /* Set the szLeaf header field. */
  assert( 0==fts5GetU16(&pPage->buf.p[2]) );
  fts5PutU16(&pPage->buf.p[2], (u16)pPage->buf.n);

  if( pWriter->bFirstTermInPage ){
    /* No term was written to this page. */
    assert( pPage->pgidx.n==0 );
    fts5WriteBtreeNoTerm(p, pWriter);
  }else{
    /* Append the pgidx to the page buffer. Set the szLeaf header field. */
    fts5BufferAppendBlob(&p->rc, &pPage->buf, pPage->pgidx.n, pPage->pgidx.p);
  }

  /* Write the page out to disk */
  iRowid = FTS5_SEGMENT_ROWID(pWriter->iSegid, pPage->pgno);
  fts5DataWrite(p, iRowid, pPage->buf.p, pPage->buf.n);

  /* Initialize the next page. */
  fts5BufferZero(&pPage->buf);
  fts5BufferZero(&pPage->pgidx);
  fts5BufferAppendBlob(&p->rc, &pPage->buf, 4, zero);
  pPage->iPrevPgidx = 0;
  pPage->pgno++;

  /* Increase the leaves written counter */
  pWriter->nLeafWritten++;

  /* The new leaf holds no terms or rowids */
  pWriter->bFirstTermInPage = 1;
  pWriter->bFirstRowidInPage = 1;
}

/*
** Append term pTerm/nTerm to the segment being written by the writer passed
** as the second argument.
**
** If an error occurs, set the Fts5Index.rc error code. If an error has 
** already occurred, this function is a no-op.
*/
static void fts5WriteAppendTerm(
  Fts5Index *p, 
  Fts5SegWriter *pWriter,
  int nTerm, const u8 *pTerm 
){
  int nPrefix;                    /* Bytes of prefix compression for term */
  Fts5PageWriter *pPage = &pWriter->writer;
  Fts5Buffer *pPgidx = &pWriter->writer.pgidx;
  int nMin = MIN(pPage->term.n, nTerm);

  assert( p->rc==SQLITE_OK );
  assert( pPage->buf.n>=4 );
  assert( pPage->buf.n>4 || pWriter->bFirstTermInPage );

  /* If the current leaf page is full, flush it to disk. */
  if( (pPage->buf.n + pPgidx->n + nTerm + 2)>=p->pConfig->pgsz ){
    if( pPage->buf.n>4 ){
      fts5WriteFlushLeaf(p, pWriter);
      if( p->rc!=SQLITE_OK ) return;
    }
    fts5BufferGrow(&p->rc, &pPage->buf, nTerm+FTS5_DATA_PADDING);
  }
  
  /* TODO1: Updating pgidx here. */
  pPgidx->n += sqlite3Fts5PutVarint(
      &pPgidx->p[pPgidx->n], pPage->buf.n - pPage->iPrevPgidx
  );
  pPage->iPrevPgidx = pPage->buf.n;
#if 0
  fts5PutU16(&pPgidx->p[pPgidx->n], pPage->buf.n);
  pPgidx->n += 2;
#endif

  if( pWriter->bFirstTermInPage ){
    nPrefix = 0;
    if( pPage->pgno!=1 ){
      /* This is the first term on a leaf that is not the leftmost leaf in
      ** the segment b-tree. In this case it is necessary to add a term to
      ** the b-tree hierarchy that is (a) larger than the largest term 
      ** already written to the segment and (b) smaller than or equal to
      ** this term. In other words, a prefix of (pTerm/nTerm) that is one
      ** byte longer than the longest prefix (pTerm/nTerm) shares with the
      ** previous term. 
      **
      ** Usually, the previous term is available in pPage->term. The exception
      ** is if this is the first term written in an incremental-merge step.
      ** In this case the previous term is not available, so just write a
      ** copy of (pTerm/nTerm) into the parent node. This is slightly
      ** inefficient, but still correct.  */
      int n = nTerm;
      if( pPage->term.n ){
        n = 1 + fts5PrefixCompress(nMin, pPage->term.p, pTerm);
      }
      fts5WriteBtreeTerm(p, pWriter, n, pTerm);
      if( p->rc!=SQLITE_OK ) return;
      pPage = &pWriter->writer;
    }
  }else{
    nPrefix = fts5PrefixCompress(nMin, pPage->term.p, pTerm);
    fts5BufferAppendVarint(&p->rc, &pPage->buf, nPrefix);
  }

  /* Append the number of bytes of new data, then the term data itself
  ** to the page. */
  fts5BufferAppendVarint(&p->rc, &pPage->buf, nTerm - nPrefix);
  fts5BufferAppendBlob(&p->rc, &pPage->buf, nTerm - nPrefix, &pTerm[nPrefix]);

  /* Update the Fts5PageWriter.term field. */
  fts5BufferSet(&p->rc, &pPage->term, nTerm, pTerm);
  pWriter->bFirstTermInPage = 0;

  pWriter->bFirstRowidInPage = 0;
  pWriter->bFirstRowidInDoclist = 1;

  assert( p->rc || (pWriter->nDlidx>0 && pWriter->aDlidx[0].buf.n==0) );
  pWriter->aDlidx[0].pgno = pPage->pgno;
}

/*
** Append a rowid and position-list size field to the writers output. 
*/
static void fts5WriteAppendRowid(
  Fts5Index *p, 
  Fts5SegWriter *pWriter,
  i64 iRowid
){
  if( p->rc==SQLITE_OK ){
    Fts5PageWriter *pPage = &pWriter->writer;

    if( (pPage->buf.n + pPage->pgidx.n)>=p->pConfig->pgsz ){
      fts5WriteFlushLeaf(p, pWriter);
    }

    /* If this is to be the first rowid written to the page, set the 
    ** rowid-pointer in the page-header. Also append a value to the dlidx
    ** buffer, in case a doclist-index is required.  */
    if( pWriter->bFirstRowidInPage ){
      fts5PutU16(pPage->buf.p, (u16)pPage->buf.n);
      fts5WriteDlidxAppend(p, pWriter, iRowid);
    }

    /* Write the rowid. */
    if( pWriter->bFirstRowidInDoclist || pWriter->bFirstRowidInPage ){
      fts5BufferAppendVarint(&p->rc, &pPage->buf, iRowid);
    }else{
      assert_nc( p->rc || iRowid>pWriter->iPrevRowid );
      fts5BufferAppendVarint(&p->rc, &pPage->buf, 
          (u64)iRowid - (u64)pWriter->iPrevRowid
      );
    }
    pWriter->iPrevRowid = iRowid;
    pWriter->bFirstRowidInDoclist = 0;
    pWriter->bFirstRowidInPage = 0;
  }
}

static void fts5WriteAppendPoslistData(
  Fts5Index *p, 
  Fts5SegWriter *pWriter, 
  const u8 *aData, 
  int nData
){
  Fts5PageWriter *pPage = &pWriter->writer;
  const u8 *a = aData;
  int n = nData;
  
  assert( p->pConfig->pgsz>0 || p->rc!=SQLITE_OK );
  while( p->rc==SQLITE_OK 
     && (pPage->buf.n + pPage->pgidx.n + n)>=p->pConfig->pgsz 
  ){
    int nReq = p->pConfig->pgsz - pPage->buf.n - pPage->pgidx.n;
    int nCopy = 0;
    while( nCopy<nReq ){
      i64 dummy;
      nCopy += fts5GetVarint(&a[nCopy], (u64*)&dummy);
    }
    fts5BufferAppendBlob(&p->rc, &pPage->buf, nCopy, a);
    a += nCopy;
    n -= nCopy;
    fts5WriteFlushLeaf(p, pWriter);
  }
  if( n>0 ){
    fts5BufferAppendBlob(&p->rc, &pPage->buf, n, a);
  }
}

/*
** Flush any data cached by the writer object to the database. Free any
** allocations associated with the writer.
*/
static void fts5WriteFinish(
  Fts5Index *p, 
  Fts5SegWriter *pWriter,         /* Writer object */
  int *pnLeaf                     /* OUT: Number of leaf pages in b-tree */
){
  int i;
  Fts5PageWriter *pLeaf = &pWriter->writer;
  if( p->rc==SQLITE_OK ){
    assert( pLeaf->pgno>=1 );
    if( pLeaf->buf.n>4 ){
      fts5WriteFlushLeaf(p, pWriter);
    }
    *pnLeaf = pLeaf->pgno-1;
    if( pLeaf->pgno>1 ){
      fts5WriteFlushBtree(p, pWriter);
    }
  }
  fts5BufferFree(&pLeaf->term);
  fts5BufferFree(&pLeaf->buf);
  fts5BufferFree(&pLeaf->pgidx);
  fts5BufferFree(&pWriter->btterm);

  for(i=0; i<pWriter->nDlidx; i++){
    sqlite3Fts5BufferFree(&pWriter->aDlidx[i].buf);
  }
  sqlite3_free(pWriter->aDlidx);
}

static void fts5WriteInit(
  Fts5Index *p, 
  Fts5SegWriter *pWriter, 
  int iSegid
){
  const int nBuffer = p->pConfig->pgsz + FTS5_DATA_PADDING;

  memset(pWriter, 0, sizeof(Fts5SegWriter));
  pWriter->iSegid = iSegid;

  fts5WriteDlidxGrow(p, pWriter, 1);
  pWriter->writer.pgno = 1;
  pWriter->bFirstTermInPage = 1;
  pWriter->iBtPage = 1;

  assert( pWriter->writer.buf.n==0 );
  assert( pWriter->writer.pgidx.n==0 );

  /* Grow the two buffers to pgsz + padding bytes in size. */
  sqlite3Fts5BufferSize(&p->rc, &pWriter->writer.pgidx, nBuffer);
  sqlite3Fts5BufferSize(&p->rc, &pWriter->writer.buf, nBuffer);

  if( p->pIdxWriter==0 ){
    Fts5Config *pConfig = p->pConfig;
    fts5IndexPrepareStmt(p, &p->pIdxWriter, sqlite3_mprintf(
          "INSERT INTO '%q'.'%q_idx'(segid,term,pgno) VALUES(?,?,?)", 
          pConfig->zDb, pConfig->zName
    ));
  }

  if( p->rc==SQLITE_OK ){
    /* Initialize the 4-byte leaf-page header to 0x00. */
    memset(pWriter->writer.buf.p, 0, 4);
    pWriter->writer.buf.n = 4;

    /* Bind the current output segment id to the index-writer. This is an
    ** optimization over binding the same value over and over as rows are
    ** inserted into %_idx by the current writer.  */
    sqlite3_bind_int(p->pIdxWriter, 1, pWriter->iSegid);
  }
}

/*
** Iterator pIter was used to iterate through the input segments of on an
** incremental merge operation. This function is called if the incremental
** merge step has finished but the input has not been completely exhausted.
*/
static void fts5TrimSegments(Fts5Index *p, Fts5Iter *pIter){
  int i;
  Fts5Buffer buf;
  memset(&buf, 0, sizeof(Fts5Buffer));
  for(i=0; i<pIter->nSeg && p->rc==SQLITE_OK; i++){
    Fts5SegIter *pSeg = &pIter->aSeg[i];
    if( pSeg->pSeg==0 ){
      /* no-op */
    }else if( pSeg->pLeaf==0 ){
      /* All keys from this input segment have been transfered to the output.
      ** Set both the first and last page-numbers to 0 to indicate that the
      ** segment is now empty. */
      pSeg->pSeg->pgnoLast = 0;
      pSeg->pSeg->pgnoFirst = 0;
    }else{
      int iOff = pSeg->iTermLeafOffset;     /* Offset on new first leaf page */
      i64 iLeafRowid;
      Fts5Data *pData;
      int iId = pSeg->pSeg->iSegid;
      u8 aHdr[4] = {0x00, 0x00, 0x00, 0x00};

      iLeafRowid = FTS5_SEGMENT_ROWID(iId, pSeg->iTermLeafPgno);
      pData = fts5LeafRead(p, iLeafRowid);
      if( pData ){
        if( iOff>pData->szLeaf ){
          /* This can occur if the pages that the segments occupy overlap - if
          ** a single page has been assigned to more than one segment. In
          ** this case a prior iteration of this loop may have corrupted the
          ** segment currently being trimmed.  */
          FTS5_CORRUPT_ROWID(p, iLeafRowid);
        }else{
          fts5BufferZero(&buf);
          fts5BufferGrow(&p->rc, &buf, pData->nn);
          fts5BufferAppendBlob(&p->rc, &buf, sizeof(aHdr), aHdr);
          fts5BufferAppendVarint(&p->rc, &buf, pSeg->term.n);
          fts5BufferAppendBlob(&p->rc, &buf, pSeg->term.n, pSeg->term.p);
          fts5BufferAppendBlob(&p->rc, &buf,pData->szLeaf-iOff,&pData->p[iOff]);
          if( p->rc==SQLITE_OK ){
            /* Set the szLeaf field */
            fts5PutU16(&buf.p[2], (u16)buf.n);
          }

          /* Set up the new page-index array */
          fts5BufferAppendVarint(&p->rc, &buf, 4);
          if( pSeg->iLeafPgno==pSeg->iTermLeafPgno 
           && pSeg->iEndofDoclist<pData->szLeaf
           && pSeg->iPgidxOff<=pData->nn
          ){
            int nDiff = pData->szLeaf - pSeg->iEndofDoclist;
            fts5BufferAppendVarint(&p->rc, &buf, buf.n - 1 - nDiff - 4);
            fts5BufferAppendBlob(&p->rc, &buf, 
                pData->nn - pSeg->iPgidxOff, &pData->p[pSeg->iPgidxOff]
            );
          }

          pSeg->pSeg->pgnoFirst = pSeg->iTermLeafPgno;
          fts5DataDelete(p, FTS5_SEGMENT_ROWID(iId, 1), iLeafRowid);
          fts5DataWrite(p, iLeafRowid, buf.p, buf.n);
        }
        fts5DataRelease(pData);
      }
    }
  }
  fts5BufferFree(&buf);
}

static void fts5MergeChunkCallback(
  Fts5Index *p, 
  void *pCtx, 
  const u8 *pChunk, int nChunk
){
  Fts5SegWriter *pWriter = (Fts5SegWriter*)pCtx;
  fts5WriteAppendPoslistData(p, pWriter, pChunk, nChunk);
}

/*
**
*/
static void fts5IndexMergeLevel(
  Fts5Index *p,                   /* FTS5 backend object */
  Fts5Structure **ppStruct,       /* IN/OUT: Stucture of index */
  int iLvl,                       /* Level to read input from */
  int *pnRem                      /* Write up to this many output leaves */
){
  Fts5Structure *pStruct = *ppStruct;
  Fts5StructureLevel *pLvl = &pStruct->aLevel[iLvl];
  Fts5StructureLevel *pLvlOut;
  Fts5Iter *pIter = 0;       /* Iterator to read input data */
  int nRem = pnRem ? *pnRem : 0;  /* Output leaf pages left to write */
  int nInput;                     /* Number of input segments */
  Fts5SegWriter writer;           /* Writer object */
  Fts5StructureSegment *pSeg;     /* Output segment */
  Fts5Buffer term;
  int bOldest;                    /* True if the output segment is the oldest */
  int eDetail = p->pConfig->eDetail;
  const int flags = FTS5INDEX_QUERY_NOOUTPUT;
  int bTermWritten = 0;           /* True if current term already output */

  assert( iLvl<pStruct->nLevel );
  assert( pLvl->nMerge<=pLvl->nSeg );

  memset(&writer, 0, sizeof(Fts5SegWriter));
  memset(&term, 0, sizeof(Fts5Buffer));
  if( pLvl->nMerge ){
    pLvlOut = &pStruct->aLevel[iLvl+1];
    assert( pLvlOut->nSeg>0 );
    nInput = pLvl->nMerge;
    pSeg = &pLvlOut->aSeg[pLvlOut->nSeg-1];

    fts5WriteInit(p, &writer, pSeg->iSegid);
    writer.writer.pgno = pSeg->pgnoLast+1;
    writer.iBtPage = 0;
  }else{
    int iSegid = fts5AllocateSegid(p, pStruct);

    /* Extend the Fts5Structure object as required to ensure the output
    ** segment exists. */
    if( iLvl==pStruct->nLevel-1 ){
      fts5StructureAddLevel(&p->rc, ppStruct);
      pStruct = *ppStruct;
    }
    fts5StructureExtendLevel(&p->rc, pStruct, iLvl+1, 1, 0);
    if( p->rc ) return;
    pLvl = &pStruct->aLevel[iLvl];
    pLvlOut = &pStruct->aLevel[iLvl+1];

    fts5WriteInit(p, &writer, iSegid);

    /* Add the new segment to the output level */
    pSeg = &pLvlOut->aSeg[pLvlOut->nSeg];
    pLvlOut->nSeg++;
    pSeg->pgnoFirst = 1;
    pSeg->iSegid = iSegid;
    pStruct->nSegment++;

    /* Read input from all segments in the input level */
    nInput = pLvl->nSeg;

    /* Set the range of origins that will go into the output segment. */
    if( pStruct->nOriginCntr>0 ){
      pSeg->iOrigin1 = pLvl->aSeg[0].iOrigin1;
      pSeg->iOrigin2 = pLvl->aSeg[pLvl->nSeg-1].iOrigin2;
    }
  }
  bOldest = (pLvlOut->nSeg==1 && pStruct->nLevel==iLvl+2);

  assert( iLvl>=0 );
  for(fts5MultiIterNew(p, pStruct, flags, 0, 0, 0, iLvl, nInput, &pIter);
      fts5MultiIterEof(p, pIter)==0;
      fts5MultiIterNext(p, pIter, 0, 0)
  ){
    Fts5SegIter *pSegIter = &pIter->aSeg[ pIter->aFirst[1].iFirst ];
    int nPos;                     /* position-list size field value */
    int nTerm;
    const u8 *pTerm;

    pTerm = fts5MultiIterTerm(pIter, &nTerm);
    if( nTerm!=term.n || fts5Memcmp(pTerm, term.p, nTerm) ){
      if( pnRem && writer.nLeafWritten>nRem ){
        break;
      }
      fts5BufferSet(&p->rc, &term, nTerm, pTerm);
      bTermWritten =0;
    }

    /* Check for key annihilation. */
    if( pSegIter->nPos==0 && (bOldest || pSegIter->bDel==0) ) continue;

    if( p->rc==SQLITE_OK && bTermWritten==0 ){
      /* This is a new term. Append a term to the output segment. */
      fts5WriteAppendTerm(p, &writer, nTerm, pTerm);
      bTermWritten = 1;
    }

    /* Append the rowid to the output */
    /* WRITEPOSLISTSIZE */
    fts5WriteAppendRowid(p, &writer, fts5MultiIterRowid(pIter));

    if( eDetail==FTS5_DETAIL_NONE ){
      if( pSegIter->bDel ){
        fts5BufferAppendVarint(&p->rc, &writer.writer.buf, 0);
        if( pSegIter->nPos>0 ){
          fts5BufferAppendVarint(&p->rc, &writer.writer.buf, 0);
        }
      }
    }else{
      /* Append the position-list data to the output */
      nPos = pSegIter->nPos*2 + pSegIter->bDel;
      fts5BufferAppendVarint(&p->rc, &writer.writer.buf, nPos);
      fts5ChunkIterate(p, pSegIter, (void*)&writer, fts5MergeChunkCallback);
    }
  }

  /* Flush the last leaf page to disk. Set the output segment b-tree height
  ** and last leaf page number at the same time.  */
  fts5WriteFinish(p, &writer, &pSeg->pgnoLast);

  assert( pIter!=0 || p->rc!=SQLITE_OK );
  if( fts5MultiIterEof(p, pIter) ){
    int i;

    /* Remove the redundant segments from the %_data table */
    assert( pSeg->nEntry==0 );
    for(i=0; i<nInput; i++){
      Fts5StructureSegment *pOld = &pLvl->aSeg[i];
      pSeg->nEntry += (pOld->nEntry - pOld->nEntryTombstone);
      fts5DataRemoveSegment(p, pOld);
    }

    /* Remove the redundant segments from the input level */
    if( pLvl->nSeg!=nInput ){
      int nMove = (pLvl->nSeg - nInput) * sizeof(Fts5StructureSegment);
      memmove(pLvl->aSeg, &pLvl->aSeg[nInput], nMove);
    }
    pStruct->nSegment -= nInput;
    pLvl->nSeg -= nInput;
    pLvl->nMerge = 0;
    if( pSeg->pgnoLast==0 ){
      pLvlOut->nSeg--;
      pStruct->nSegment--;
    }
  }else{
    assert( pSeg->pgnoLast>0 );
    fts5TrimSegments(p, pIter);
    pLvl->nMerge = nInput;
  }

  fts5MultiIterFree(pIter);
  fts5BufferFree(&term);
  if( pnRem ) *pnRem -= writer.nLeafWritten;
}

/*
** If this is not a contentless_delete=1 table, or if the 'deletemerge'
** configuration option is set to 0, then this function always returns -1.
** Otherwise, it searches the structure object passed as the second argument
** for a level suitable for merging due to having a large number of 
** tombstones in the tombstone hash. If one is found, its index is returned.
** Otherwise, if there is no suitable level, -1.
*/
static int fts5IndexFindDeleteMerge(Fts5Index *p, Fts5Structure *pStruct){
  Fts5Config *pConfig = p->pConfig;
  int iRet = -1;
  if( pConfig->bContentlessDelete && pConfig->nDeleteMerge>0 ){
    int ii;
    int nBest = 0;

    for(ii=0; ii<pStruct->nLevel; ii++){
      Fts5StructureLevel *pLvl = &pStruct->aLevel[ii];
      i64 nEntry = 0;
      i64 nTomb = 0;
      int iSeg;
      for(iSeg=0; iSeg<pLvl->nSeg; iSeg++){
        nEntry += pLvl->aSeg[iSeg].nEntry;
        nTomb += pLvl->aSeg[iSeg].nEntryTombstone;
      }
      assert_nc( nEntry>0 || pLvl->nSeg==0 );
      if( nEntry>0 ){
        int nPercent = (nTomb * 100) / nEntry;
        if( nPercent>=pConfig->nDeleteMerge && nPercent>nBest ){
          iRet = ii;
          nBest = nPercent;
        }
      }

      /* If pLvl is already the input level to an ongoing merge, look no
      ** further for a merge candidate. The caller should be allowed to
      ** continue merging from pLvl first.  */
      if( pLvl->nMerge ) break;
    }
  }
  return iRet;
}

/*
** Do up to nPg pages of automerge work on the index.
**
** Return true if any changes were actually made, or false otherwise.
*/
static int fts5IndexMerge(
  Fts5Index *p,                   /* FTS5 backend object */
  Fts5Structure **ppStruct,       /* IN/OUT: Current structure of index */
  int nPg,                        /* Pages of work to do */
  int nMin                        /* Minimum number of segments to merge */
){
  int nRem = nPg;
  int bRet = 0;
  Fts5Structure *pStruct = *ppStruct;
  while( nRem>0 && p->rc==SQLITE_OK ){
    int iLvl;                   /* To iterate through levels */
    int iBestLvl = 0;           /* Level offering the most input segments */
    int nBest = 0;              /* Number of input segments on best level */

    /* Set iBestLvl to the level to read input segments from. Or to -1 if
    ** there is no level suitable to merge segments from.  */
    assert( pStruct->nLevel>0 );
    for(iLvl=0; iLvl<pStruct->nLevel; iLvl++){
      Fts5StructureLevel *pLvl = &pStruct->aLevel[iLvl];
      if( pLvl->nMerge ){
        if( pLvl->nMerge>nBest ){
          iBestLvl = iLvl;
          nBest = nMin;
        }
        break;
      }
      if( pLvl->nSeg>nBest ){
        nBest = pLvl->nSeg;
        iBestLvl = iLvl;
      }
    }
    if( nBest<nMin ){
      iBestLvl = fts5IndexFindDeleteMerge(p, pStruct);
    }

    if( iBestLvl<0 ) break;
    bRet = 1;
    fts5IndexMergeLevel(p, &pStruct, iBestLvl, &nRem);
    if( p->rc==SQLITE_OK && pStruct->aLevel[iBestLvl].nMerge==0 ){
      fts5StructurePromote(p, iBestLvl+1, pStruct);
    }

    if( nMin==1 ) nMin = 2;
  }
  *ppStruct = pStruct;
  return bRet;
}

/*
** A total of nLeaf leaf pages of data has just been flushed to a level-0
** segment. This function updates the write-counter accordingly and, if
** necessary, performs incremental merge work.
**
** If an error occurs, set the Fts5Index.rc error code. If an error has 
** already occurred, this function is a no-op.
*/
static void fts5IndexAutomerge(
  Fts5Index *p,                   /* FTS5 backend object */
  Fts5Structure **ppStruct,       /* IN/OUT: Current structure of index */
  int nLeaf                       /* Number of output leaves just written */
){
  if( p->rc==SQLITE_OK && p->pConfig->nAutomerge>0 && ALWAYS((*ppStruct)!=0) ){
    Fts5Structure *pStruct = *ppStruct;
    u64 nWrite;                   /* Initial value of write-counter */
    int nWork;                    /* Number of work-quanta to perform */
    int nRem;                     /* Number of leaf pages left to write */

    /* Update the write-counter. While doing so, set nWork. */
    nWrite = pStruct->nWriteCounter;
    nWork = (int)(((nWrite + nLeaf) / p->nWorkUnit) - (nWrite / p->nWorkUnit));
    pStruct->nWriteCounter += nLeaf;
    nRem = (int)(p->nWorkUnit * nWork * pStruct->nLevel);

    fts5IndexMerge(p, ppStruct, nRem, p->pConfig->nAutomerge);
  }
}

static void fts5IndexCrisismerge(
  Fts5Index *p,                   /* FTS5 backend object */
  Fts5Structure **ppStruct        /* IN/OUT: Current structure of index */
){
  const int nCrisis = p->pConfig->nCrisisMerge;
  Fts5Structure *pStruct = *ppStruct;
  if( pStruct && pStruct->nLevel>0 ){
    int iLvl = 0;
    while( p->rc==SQLITE_OK && pStruct->aLevel[iLvl].nSeg>=nCrisis ){
      fts5IndexMergeLevel(p, &pStruct, iLvl, 0);
      assert( p->rc!=SQLITE_OK || pStruct->nLevel>(iLvl+1) );
      fts5StructurePromote(p, iLvl+1, pStruct);
      iLvl++;
    }
    *ppStruct = pStruct;
  }
}

static int fts5IndexReturn(Fts5Index *p){
  int rc = p->rc;
  p->rc = SQLITE_OK;
  return rc;
}

/*
** Close the read-only blob handle, if it is open.
*/
void sqlite3Fts5IndexCloseReader(Fts5Index *p){
  fts5IndexCloseReader(p);
  fts5IndexReturn(p);
}

typedef struct Fts5FlushCtx Fts5FlushCtx;
struct Fts5FlushCtx {
  Fts5Index *pIdx;
  Fts5SegWriter writer; 
};

/*
** Buffer aBuf[] contains a list of varints, all small enough to fit
** in a 32-bit integer. Return the size of the largest prefix of this 
** list nMax bytes or less in size.
*/
static int fts5PoslistPrefix(const u8 *aBuf, int nMax){
  int ret;
  u32 dummy;
  ret = fts5GetVarint32(aBuf, dummy);
  if( ret<nMax ){
    while( 1 ){
      int i = fts5GetVarint32(&aBuf[ret], dummy);
      if( (ret + i) > nMax ) break;
      ret += i;
    }
  }
  return ret;
}

/*
** Execute the SQL statement:
**
**    DELETE FROM %_idx WHERE (segid, (pgno/2)) = ($iSegid, $iPgno);
**
** This is used when a secure-delete operation removes the last term
** from a segment leaf page. In that case the %_idx entry is removed 
** too. This is done to ensure that if all instances of a token are
** removed from an fts5 database in secure-delete mode, no trace of
** the token itself remains in the database.
*/
static void fts5SecureDeleteIdxEntry(
  Fts5Index *p,                   /* FTS5 backend object */
  int iSegid,                     /* Id of segment to delete entry for */
  int iPgno                       /* Page number within segment */
){
  if( iPgno!=1 ){
    assert( p->pConfig->iVersion==FTS5_CURRENT_VERSION_SECUREDELETE );
    if( p->pDeleteFromIdx==0 ){
      fts5IndexPrepareStmt(p, &p->pDeleteFromIdx, sqlite3_mprintf(
          "DELETE FROM '%q'.'%q_idx' WHERE (segid, (pgno/2)) = (?1, ?2)",
          p->pConfig->zDb, p->pConfig->zName
      ));
    }
    if( p->rc==SQLITE_OK ){
      sqlite3_bind_int(p->pDeleteFromIdx, 1, iSegid);
      sqlite3_bind_int(p->pDeleteFromIdx, 2, iPgno);
      sqlite3_step(p->pDeleteFromIdx);
      p->rc = sqlite3_reset(p->pDeleteFromIdx);
    }
  }
}

/*
** This is called when a secure-delete operation removes a position-list
** that overflows onto segment page iPgno of segment pSeg. This function
** rewrites node iPgno, and possibly one or more of its right-hand peers,
** to remove this portion of the position list.
**
** Output variable (*pbLastInDoclist) is set to true if the position-list
** removed is followed by a new term or the end-of-segment, or false if
** it is followed by another rowid/position list.
*/
static void fts5SecureDeleteOverflow(
  Fts5Index *p,
  Fts5StructureSegment *pSeg,
  int iPgno,
  int *pbLastInDoclist
){
  const int bDetailNone = (p->pConfig->eDetail==FTS5_DETAIL_NONE);
  int pgno;
  Fts5Data *pLeaf = 0;
  assert( iPgno!=1 );

  *pbLastInDoclist = 1;
  for(pgno=iPgno; p->rc==SQLITE_OK && pgno<=pSeg->pgnoLast; pgno++){
    i64 iRowid = FTS5_SEGMENT_ROWID(pSeg->iSegid, pgno);
    int iNext = 0;
    u8 *aPg = 0;

    pLeaf = fts5DataRead(p, iRowid);
    if( pLeaf==0 ) break;
    aPg = pLeaf->p;

    iNext = fts5GetU16(&aPg[0]);
    if( iNext!=0 ){
      *pbLastInDoclist = 0;
    }
    if( iNext==0 && pLeaf->szLeaf!=pLeaf->nn ){
      fts5GetVarint32(&aPg[pLeaf->szLeaf], iNext);
    }

    if( iNext==0 ){
      /* The page contains no terms or rowids. Replace it with an empty
      ** page and move on to the right-hand peer.  */
      const u8 aEmpty[] = {0x00, 0x00, 0x00, 0x04}; 
      assert_nc( bDetailNone==0 || pLeaf->nn==4 );
      if( bDetailNone==0 ) fts5DataWrite(p, iRowid, aEmpty, sizeof(aEmpty));
      fts5DataRelease(pLeaf);
      pLeaf = 0;
    }else if( bDetailNone ){
      break;
    }else if( iNext>=pLeaf->szLeaf || pLeaf->nn<pLeaf->szLeaf || iNext<4 ){
      FTS5_CORRUPT_ROWID(p, iRowid);
      break;
    }else{
      int nShift = iNext - 4;
      int nPg;

      int nIdx = 0;
      u8 *aIdx = 0;

      /* Unless the current page footer is 0 bytes in size (in which case
      ** the new page footer will be as well), allocate and populate a 
      ** buffer containing the new page footer. Set stack variables aIdx 
      ** and nIdx accordingly.  */
      if( pLeaf->nn>pLeaf->szLeaf ){
        int iFirst = 0;
        int i1 = pLeaf->szLeaf;
        int i2 = 0;

        i1 += fts5GetVarint32(&aPg[i1], iFirst);
        if( iFirst<iNext ){
          FTS5_CORRUPT_ROWID(p, iRowid);
          break;
        }
        aIdx = sqlite3Fts5MallocZero(&p->rc, (pLeaf->nn-pLeaf->szLeaf)+2);
        if( aIdx==0 ) break;
        i2 = sqlite3Fts5PutVarint(aIdx, iFirst-nShift);
        if( i1<pLeaf->nn ){
          memcpy(&aIdx[i2], &aPg[i1], pLeaf->nn-i1);
          i2 += (pLeaf->nn-i1);
        }
        nIdx = i2;
      }

      /* Modify the contents of buffer aPg[]. Set nPg to the new size 
      ** in bytes. The new page is always smaller than the old.  */
      nPg = pLeaf->szLeaf - nShift;
      memmove(&aPg[4], &aPg[4+nShift], nPg-4);
      fts5PutU16(&aPg[2], nPg);
      if( fts5GetU16(&aPg[0]) ) fts5PutU16(&aPg[0], 4);
      if( nIdx>0 ){
        memcpy(&aPg[nPg], aIdx, nIdx);
        nPg += nIdx;
      }
      sqlite3_free(aIdx);

      /* Write the new page to disk and exit the loop */
      assert( nPg>4 || fts5GetU16(aPg)==0 );
      fts5DataWrite(p, iRowid, aPg, nPg);
      break;
    }
  }
  fts5DataRelease(pLeaf);
}

/*
** Completely remove the entry that pSeg currently points to from 
** the database.
*/
static void fts5DoSecureDelete(
  Fts5Index *p,
  Fts5SegIter *pSeg
){
  const int bDetailNone = (p->pConfig->eDetail==FTS5_DETAIL_NONE);
  int iSegid = pSeg->pSeg->iSegid;
  u8 *aPg = pSeg->pLeaf->p;
  int nPg = pSeg->pLeaf->nn;
  int iPgIdx = pSeg->pLeaf->szLeaf;

  u64 iDelta = 0;
  int iNextOff = 0;
  int iOff = 0;
  int nIdx = 0;
  u8 *aIdx = 0;
  int bLastInDoclist = 0;
  int iIdx = 0;
  int iStart = 0;
  int iDelKeyOff = 0;       /* Offset of deleted key, if any */

  nIdx = nPg-iPgIdx;
  aIdx = sqlite3Fts5MallocZero(&p->rc, ((i64)nIdx)+16);
  if( p->rc ) return;
  memcpy(aIdx, &aPg[iPgIdx], nIdx);

  /* At this point segment iterator pSeg points to the entry
  ** this function should remove from the b-tree segment. 
  **
  ** In detail=full or detail=column mode, pSeg->iLeafOffset is the 
  ** offset of the first byte in the position-list for the entry to 
  ** remove. Immediately before this comes two varints that will also
  ** need to be removed:
  **
  **     + the rowid or delta rowid value for the entry, and
  **     + the size of the position list in bytes.
  **
  ** Or, in detail=none mode, there is a single varint prior to 
  ** pSeg->iLeafOffset - the rowid or delta rowid value.
  **
  ** This block sets the following variables:
  **
  **   iStart:
  **     The offset of the first byte of the rowid or delta-rowid
  **     value for the doclist entry being removed.
  **
  **   iDelta:
  **     The value of the rowid or delta-rowid value for the doclist
  **     entry being removed.
  **
  **   iNextOff:
  **     The offset of the next entry following the position list
  **     for the one being removed. If the position list for this
  **     entry overflows onto the next leaf page, this value will be
  **     greater than pLeaf->szLeaf.
  */
  {
    int iSOP;                     /* Start-Of-Position-list */
    if( pSeg->iLeafPgno==pSeg->iTermLeafPgno ){
      iStart = pSeg->iTermLeafOffset;
    }else{
      iStart = fts5GetU16(&aPg[0]);
    }

    iSOP = iStart + fts5GetVarint(&aPg[iStart], &iDelta);
    assert_nc( iSOP<=pSeg->iLeafOffset );

    if( bDetailNone ){
      while( iSOP<pSeg->iLeafOffset ){
        if( aPg[iSOP]==0x00 ) iSOP++;
        if( aPg[iSOP]==0x00 ) iSOP++;
        iStart = iSOP;
        iSOP = iStart + fts5GetVarint(&aPg[iStart], &iDelta);
      }

      iNextOff = iSOP;
      if( iNextOff<pSeg->iEndofDoclist && aPg[iNextOff]==0x00 ) iNextOff++;
      if( iNextOff<pSeg->iEndofDoclist && aPg[iNextOff]==0x00 ) iNextOff++;

    }else{
      int nPos = 0;
      iSOP += fts5GetVarint32(&aPg[iSOP], nPos);
      while( iSOP<pSeg->iLeafOffset ){
        iStart = iSOP + (nPos/2);
        iSOP = iStart + fts5GetVarint(&aPg[iStart], &iDelta);
        iSOP += fts5GetVarint32(&aPg[iSOP], nPos);
      }
      assert_nc( iSOP==pSeg->iLeafOffset );
      iNextOff = pSeg->iLeafOffset + pSeg->nPos;
    }
  }

  iOff = iStart;

  /* If the position-list for the entry being removed flows over past
  ** the end of this page, delete the portion of the position-list on the
  ** next page and beyond.
  **
  ** Set variable bLastInDoclist to true if this entry happens 
  ** to be the last rowid in the doclist for its term.  */
  if( iNextOff>=iPgIdx ){
    int pgno = pSeg->iLeafPgno+1;
    fts5SecureDeleteOverflow(p, pSeg->pSeg, pgno, &bLastInDoclist);
    iNextOff = iPgIdx;
  }

  if( pSeg->bDel==0 ){
    if( iNextOff!=iPgIdx ){
      /* Loop through the page-footer. If iNextOff (offset of the
      ** entry following the one we are removing) is equal to the 
      ** offset of a key on this page, then the entry is the last 
      ** in its doclist. */
      int iKeyOff = 0;
      for(iIdx=0; iIdx<nIdx; /* no-op */){
        u32 iVal = 0;
        iIdx += fts5GetVarint32(&aIdx[iIdx], iVal);
        iKeyOff += iVal;
        if( iKeyOff==iNextOff ){
          bLastInDoclist = 1;
        }
      }
    }

    /* If this is (a) the first rowid on a page and (b) is not followed by
    ** another position list on the same page, set the "first-rowid" field
    ** of the header to 0.  */
    if( fts5GetU16(&aPg[0])==iStart && (bLastInDoclist || iNextOff==iPgIdx) ){
      fts5PutU16(&aPg[0], 0);
    }
  }

  if( pSeg->bDel ){
    iOff += sqlite3Fts5PutVarint(&aPg[iOff], iDelta);
    aPg[iOff++] = 0x01;
  }else if( bLastInDoclist==0 ){
    if( iNextOff!=iPgIdx ){
      u64 iNextDelta = 0;
      iNextOff += fts5GetVarint(&aPg[iNextOff], &iNextDelta);
      iOff += sqlite3Fts5PutVarint(&aPg[iOff], iDelta + iNextDelta);
    }
  }else if( 
      pSeg->iLeafPgno==pSeg->iTermLeafPgno 
   && iStart==pSeg->iTermLeafOffset 
  ){
    /* The entry being removed was the only position list in its
    ** doclist. Therefore the term needs to be removed as well. */
    int iKey = 0;
    int iKeyOff = 0;

    /* Set iKeyOff to the offset of the term that will be removed - the
    ** last offset in the footer that is not greater than iStart. */
    for(iIdx=0; iIdx<nIdx; iKey++){
      u32 iVal = 0;
      iIdx += fts5GetVarint32(&aIdx[iIdx], iVal);
      if( (iKeyOff+iVal)>(u32)iStart ) break;
      iKeyOff += iVal;
    }
    assert_nc( iKey>=1 );

    /* Set iDelKeyOff to the value of the footer entry to remove from 
    ** the page. */
    iDelKeyOff = iOff = iKeyOff;

    if( iNextOff!=iPgIdx ){
      /* This is the only position-list associated with the term, and there
      ** is another term following it on this page. So the subsequent term
      ** needs to be moved to replace the term associated with the entry
      ** being removed. */
      int nPrefix = 0;
      int nSuffix = 0;
      int nPrefix2 = 0;
      int nSuffix2 = 0;

      iDelKeyOff = iNextOff;
      iNextOff += fts5GetVarint32(&aPg[iNextOff], nPrefix2);
      iNextOff += fts5GetVarint32(&aPg[iNextOff], nSuffix2);

      if( iKey!=1 ){
        iKeyOff += fts5GetVarint32(&aPg[iKeyOff], nPrefix);
      }
      iKeyOff += fts5GetVarint32(&aPg[iKeyOff], nSuffix);

      nPrefix = MIN(nPrefix, nPrefix2);
      nSuffix = (nPrefix2 + nSuffix2) - nPrefix;

      if( (iKeyOff+nSuffix)>iPgIdx || (iNextOff+nSuffix2)>iPgIdx ){
        FTS5_CORRUPT_IDX(p);
      }else{
        if( iKey!=1 ){
          iOff += sqlite3Fts5PutVarint(&aPg[iOff], nPrefix);
        }
        iOff += sqlite3Fts5PutVarint(&aPg[iOff], nSuffix);
        if( nPrefix2>pSeg->term.n ){
          FTS5_CORRUPT_IDX(p);
        }else if( nPrefix2>nPrefix ){
          memcpy(&aPg[iOff], &pSeg->term.p[nPrefix], nPrefix2-nPrefix);
          iOff += (nPrefix2-nPrefix);
        }
        memmove(&aPg[iOff], &aPg[iNextOff], nSuffix2);
        iOff += nSuffix2;
        iNextOff += nSuffix2;
      }
    }
  }else if( iStart==4 ){
    int iPgno;

    assert_nc( pSeg->iLeafPgno>pSeg->iTermLeafPgno );
    /* The entry being removed may be the only position list in
    ** its doclist. */
    for(iPgno=pSeg->iLeafPgno-1; iPgno>pSeg->iTermLeafPgno; iPgno-- ){
      Fts5Data *pPg = fts5DataRead(p, FTS5_SEGMENT_ROWID(iSegid, iPgno));
      int bEmpty = (pPg && pPg->nn==4);
      fts5DataRelease(pPg);
      if( bEmpty==0 ) break;
    }

    if( iPgno==pSeg->iTermLeafPgno ){
      i64 iId = FTS5_SEGMENT_ROWID(iSegid, pSeg->iTermLeafPgno);
      Fts5Data *pTerm = fts5DataRead(p, iId);
      if( pTerm && pTerm->szLeaf==pSeg->iTermLeafOffset ){
        u8 *aTermIdx = &pTerm->p[pTerm->szLeaf];
        int nTermIdx = pTerm->nn - pTerm->szLeaf;
        int iTermIdx = 0;
        int iTermOff = 0;

        while( 1 ){
          u32 iVal = 0;
          int nByte = fts5GetVarint32(&aTermIdx[iTermIdx], iVal);
          iTermOff += iVal;
          if( (iTermIdx+nByte)>=nTermIdx ) break;
          iTermIdx += nByte;
        }
        nTermIdx = iTermIdx;

        memmove(&pTerm->p[iTermOff], &pTerm->p[pTerm->szLeaf], nTermIdx);
        fts5PutU16(&pTerm->p[2], iTermOff);

        fts5DataWrite(p, iId, pTerm->p, iTermOff+nTermIdx);
        if( nTermIdx==0 ){
          fts5SecureDeleteIdxEntry(p, iSegid, pSeg->iTermLeafPgno);
        }
      }
      fts5DataRelease(pTerm);
    }
  }

  /* Assuming no error has occurred, this block does final edits to the
  ** leaf page before writing it back to disk. Input variables are:
  **
  **   nPg: Total initial size of leaf page.
  **   iPgIdx: Initial offset of page footer.
  **
  **   iOff: Offset to move data to
  **   iNextOff: Offset to move data from
  */
  if( p->rc==SQLITE_OK ){
    const int nMove = nPg - iNextOff;     /* Number of bytes to move */
    int nShift = iNextOff - iOff;         /* Distance to move them */

    int iPrevKeyOut = 0;
    int iKeyIn = 0;

    memmove(&aPg[iOff], &aPg[iNextOff], nMove);
    iPgIdx -= nShift;
    nPg = iPgIdx;
    fts5PutU16(&aPg[2], iPgIdx);

    for(iIdx=0; iIdx<nIdx; /* no-op */){
      u32 iVal = 0;
      iIdx += fts5GetVarint32(&aIdx[iIdx], iVal);
      iKeyIn += iVal;
      if( iKeyIn!=iDelKeyOff ){
        int iKeyOut = (iKeyIn - (iKeyIn>iOff ? nShift : 0));
        nPg += sqlite3Fts5PutVarint(&aPg[nPg], iKeyOut - iPrevKeyOut);
        iPrevKeyOut = iKeyOut;
      }
    }

    if( iPgIdx==nPg && nIdx>0 && pSeg->iLeafPgno!=1 ){
      fts5SecureDeleteIdxEntry(p, iSegid, pSeg->iLeafPgno);
    }

    assert_nc( nPg>4 || fts5GetU16(aPg)==0 );
    fts5DataWrite(p, FTS5_SEGMENT_ROWID(iSegid,pSeg->iLeafPgno), aPg, nPg);
  }
  sqlite3_free(aIdx);
}

/*
** This is called as part of flushing a delete to disk in 'secure-delete'
** mode. It edits the segments within the database described by argument
** pStruct to remove the entries for term zTerm, rowid iRowid.
**
** Return SQLITE_OK if successful, or an SQLite error code if an error
** has occurred. Any error code is also stored in the Fts5Index handle.
*/
static int fts5FlushSecureDelete(
  Fts5Index *p,
  Fts5Structure *pStruct,
  const char *zTerm,
  int nTerm,
  i64 iRowid
){
  const int f = FTS5INDEX_QUERY_SKIPHASH;
  Fts5Iter *pIter = 0;            /* Used to find term instance */

  /* If the version number has not been set to SECUREDELETE, do so now. */
  if( p->pConfig->iVersion!=FTS5_CURRENT_VERSION_SECUREDELETE ){
    Fts5Config *pConfig = p->pConfig;
    sqlite3_stmt *pStmt = 0;
    fts5IndexPrepareStmt(p, &pStmt, sqlite3_mprintf(
          "REPLACE INTO %Q.'%q_config' VALUES ('version', %d)",
          pConfig->zDb, pConfig->zName, FTS5_CURRENT_VERSION_SECUREDELETE
    ));
    if( p->rc==SQLITE_OK ){
      int rc;
      sqlite3_step(pStmt);
      rc = sqlite3_finalize(pStmt);
      if( p->rc==SQLITE_OK ) p->rc = rc;
      pConfig->iCookie++;
      pConfig->iVersion = FTS5_CURRENT_VERSION_SECUREDELETE;
    }
  }

  fts5MultiIterNew(p, pStruct, f, 0, (const u8*)zTerm, nTerm, -1, 0, &pIter);
  if( fts5MultiIterEof(p, pIter)==0 ){
    i64 iThis = fts5MultiIterRowid(pIter);
    if( iThis<iRowid ){
      fts5MultiIterNextFrom(p, pIter, iRowid);
    }

    if( p->rc==SQLITE_OK 
     && fts5MultiIterEof(p, pIter)==0 
     && iRowid==fts5MultiIterRowid(pIter)
    ){
      Fts5SegIter *pSeg = &pIter->aSeg[pIter->aFirst[1].iFirst];
      fts5DoSecureDelete(p, pSeg);
    }
  }

  fts5MultiIterFree(pIter);
  return p->rc;
}


/*
** Flush the contents of in-memory hash table iHash to a new level-0 
** segment on disk. Also update the corresponding structure record.
**
** If an error occurs, set the Fts5Index.rc error code. If an error has 
** already occurred, this function is a no-op.
*/
static void fts5FlushOneHash(Fts5Index *p){
  Fts5Hash *pHash = p->pHash;
  Fts5Structure *pStruct;
  int iSegid;
  int pgnoLast = 0;                 /* Last leaf page number in segment */

  /* Obtain a reference to the index structure and allocate a new segment-id
  ** for the new level-0 segment.  */
  pStruct = fts5StructureRead(p);
  fts5StructureInvalidate(p);

  if( sqlite3Fts5HashIsEmpty(pHash)==0 ){
    iSegid = fts5AllocateSegid(p, pStruct);
    if( iSegid ){
      const int pgsz = p->pConfig->pgsz;
      int eDetail = p->pConfig->eDetail;
      int bSecureDelete = p->pConfig->bSecureDelete;
      Fts5StructureSegment *pSeg; /* New segment within pStruct */
      Fts5Buffer *pBuf;           /* Buffer in which to assemble leaf page */
      Fts5Buffer *pPgidx;         /* Buffer in which to assemble pgidx */
  
      Fts5SegWriter writer;
      fts5WriteInit(p, &writer, iSegid);
  
      pBuf = &writer.writer.buf;
      pPgidx = &writer.writer.pgidx;
  
      /* fts5WriteInit() should have initialized the buffers to (most likely)
      ** the maximum space required. */
      assert( p->rc || pBuf->nSpace>=(pgsz + FTS5_DATA_PADDING) );
      assert( p->rc || pPgidx->nSpace>=(pgsz + FTS5_DATA_PADDING) );
  
      /* Begin scanning through hash table entries. This loop runs once for each
      ** term/doclist currently stored within the hash table. */
      if( p->rc==SQLITE_OK ){
        p->rc = sqlite3Fts5HashScanInit(pHash, 0, 0);
      }
      while( p->rc==SQLITE_OK && 0==sqlite3Fts5HashScanEof(pHash) ){
        const char *zTerm;        /* Buffer containing term */
        int nTerm;                /* Size of zTerm in bytes */
        const u8 *pDoclist;       /* Pointer to doclist for this term */
        int nDoclist;             /* Size of doclist in bytes */
  
        /* Get the term and doclist for this entry. */
        sqlite3Fts5HashScanEntry(pHash, &zTerm, &nTerm, &pDoclist, &nDoclist);
        if( bSecureDelete==0 ){
          fts5WriteAppendTerm(p, &writer, nTerm, (const u8*)zTerm);
          if( p->rc!=SQLITE_OK ) break;
          assert( writer.bFirstRowidInPage==0 );
        }
  
        if( !bSecureDelete && pgsz>=(pBuf->n + pPgidx->n + nDoclist + 1) ){
          /* The entire doclist will fit on the current leaf. */
          fts5BufferSafeAppendBlob(pBuf, pDoclist, nDoclist);
        }else{
          int bTermWritten = !bSecureDelete;
          i64 iRowid = 0;
          i64 iPrev = 0;
          int iOff = 0;
  
          /* The entire doclist will not fit on this leaf. The following 
          ** loop iterates through the poslists that make up the current 
          ** doclist.  */
          while( p->rc==SQLITE_OK && iOff<nDoclist ){
            u64 iDelta = 0;
            iOff += fts5GetVarint(&pDoclist[iOff], &iDelta);
            iRowid += iDelta;
  
            /* If in secure delete mode, and if this entry in the poslist is
            ** in fact a delete, then edit the existing segments directly
            ** using fts5FlushSecureDelete().  */
            if( bSecureDelete ){
              if( eDetail==FTS5_DETAIL_NONE ){
                if( iOff<nDoclist && pDoclist[iOff]==0x00 
                 && !fts5FlushSecureDelete(p, pStruct, zTerm, nTerm, iRowid)
                ){
                  iOff++;
                  if( iOff<nDoclist && pDoclist[iOff]==0x00 ){
                    iOff++;
                    nDoclist = 0;
                  }else{
                    continue;
                  }
                }
              }else if( (pDoclist[iOff] & 0x01) 
                && !fts5FlushSecureDelete(p, pStruct, zTerm, nTerm, iRowid)
              ){
                if( p->rc!=SQLITE_OK || pDoclist[iOff]==0x01 ){
                  iOff++;
                  continue;
                }
              }
            }
  
            if( p->rc==SQLITE_OK && bTermWritten==0 ){
              fts5WriteAppendTerm(p, &writer, nTerm, (const u8*)zTerm);
              bTermWritten = 1;
              assert( p->rc!=SQLITE_OK || writer.bFirstRowidInPage==0 );
            }
            
            if( writer.bFirstRowidInPage ){
              fts5PutU16(&pBuf->p[0], (u16)pBuf->n);   /* first rowid on page */
              pBuf->n += sqlite3Fts5PutVarint(&pBuf->p[pBuf->n], iRowid);
              writer.bFirstRowidInPage = 0;
              fts5WriteDlidxAppend(p, &writer, iRowid);
            }else{
              u64 iRowidDelta = (u64)iRowid - (u64)iPrev;
              pBuf->n += sqlite3Fts5PutVarint(&pBuf->p[pBuf->n], iRowidDelta);
            }
            if( p->rc!=SQLITE_OK ) break;
            assert( pBuf->n<=pBuf->nSpace );
            iPrev = iRowid;
  
            if( eDetail==FTS5_DETAIL_NONE ){
              if( iOff<nDoclist && pDoclist[iOff]==0 ){
                pBuf->p[pBuf->n++] = 0;
                iOff++;
                if( iOff<nDoclist && pDoclist[iOff]==0 ){
                  pBuf->p[pBuf->n++] = 0;
                  iOff++;
                }
              }
              if( (pBuf->n + pPgidx->n)>=pgsz ){
                fts5WriteFlushLeaf(p, &writer);
              }
            }else{
              int bDel = 0;
              int nPos = 0;
              int nCopy = fts5GetPoslistSize(&pDoclist[iOff], &nPos, &bDel);
              if( bDel && bSecureDelete ){
                fts5BufferAppendVarint(&p->rc, pBuf, nPos*2);
                iOff += nCopy;
                nCopy = nPos;
              }else{
                nCopy += nPos;
              }
              if( (pBuf->n + pPgidx->n + nCopy) <= pgsz ){
                /* The entire poslist will fit on the current leaf. So copy
                ** it in one go. */
                fts5BufferSafeAppendBlob(pBuf, &pDoclist[iOff], nCopy);
              }else{
                /* The entire poslist will not fit on this leaf. So it needs
                ** to be broken into sections. The only qualification being
                ** that each varint must be stored contiguously.  */
                const u8 *pPoslist = &pDoclist[iOff];
                int iPos = 0;
                while( p->rc==SQLITE_OK ){
                  int nSpace = pgsz - pBuf->n - pPgidx->n;
                  int n = 0;
                  if( (nCopy - iPos)<=nSpace ){
                    n = nCopy - iPos;
                  }else{
                    n = fts5PoslistPrefix(&pPoslist[iPos], nSpace);
                  }
                  assert( n>0 );
                  fts5BufferSafeAppendBlob(pBuf, &pPoslist[iPos], n);
                  iPos += n;
                  if( (pBuf->n + pPgidx->n)>=pgsz ){
                    fts5WriteFlushLeaf(p, &writer);
                  }
                  if( iPos>=nCopy ) break;
                }
              }
              iOff += nCopy;
            }
          }
        }
  
        /* TODO2: Doclist terminator written here. */
        /* pBuf->p[pBuf->n++] = '\0'; */
        assert( pBuf->n<=pBuf->nSpace );
        if( p->rc==SQLITE_OK ) sqlite3Fts5HashScanNext(pHash);
      }
      fts5WriteFinish(p, &writer, &pgnoLast);
  
      assert( p->rc!=SQLITE_OK || bSecureDelete || pgnoLast>0 );
      if( pgnoLast>0 ){
        /* Update the Fts5Structure. It is written back to the database by the
        ** fts5StructureRelease() call below.  */
        if( pStruct->nLevel==0 ){
          fts5StructureAddLevel(&p->rc, &pStruct);
        }
        fts5StructureExtendLevel(&p->rc, pStruct, 0, 1, 0);
        if( p->rc==SQLITE_OK ){
          pSeg = &pStruct->aLevel[0].aSeg[ pStruct->aLevel[0].nSeg++ ];
          pSeg->iSegid = iSegid;
          pSeg->pgnoFirst = 1;
          pSeg->pgnoLast = pgnoLast;
          if( pStruct->nOriginCntr>0 ){
            pSeg->iOrigin1 = pStruct->nOriginCntr;
            pSeg->iOrigin2 = pStruct->nOriginCntr;
            pSeg->nEntry = p->nPendingRow;
            pStruct->nOriginCntr++;
          }
          pStruct->nSegment++;
        }
        fts5StructurePromote(p, 0, pStruct);
      }
    }
  }

  fts5IndexAutomerge(p, &pStruct, pgnoLast + p->nContentlessDelete);
  fts5IndexCrisismerge(p, &pStruct);
  fts5StructureWrite(p, pStruct);
  fts5StructureRelease(pStruct);
}

/*
** Flush any data stored in the in-memory hash tables to the database.
*/
static void fts5IndexFlush(Fts5Index *p){
  /* Unless it is empty, flush the hash table to disk */
  if( p->flushRc ){
    p->rc = p->flushRc;
    return;
  }
  if( p->nPendingData || p->nContentlessDelete ){
    assert( p->pHash );
    fts5FlushOneHash(p);
    if( p->rc==SQLITE_OK ){
      sqlite3Fts5HashClear(p->pHash);
      p->nPendingData = 0;
      p->nPendingRow = 0;
      p->nContentlessDelete = 0;
    }else if( p->nPendingData || p->nContentlessDelete ){
      p->flushRc = p->rc;
    }
  }
}

static Fts5Structure *fts5IndexOptimizeStruct(
  Fts5Index *p, 
  Fts5Structure *pStruct
){
  Fts5Structure *pNew = 0;
  sqlite3_int64 nByte = SZ_FTS5STRUCTURE(1);
  int nSeg = pStruct->nSegment;
  int i;

  /* Figure out if this structure requires optimization. A structure does
  ** not require optimization if either:
  **
  **  1. it consists of fewer than two segments, or 
  **  2. all segments are on the same level, or
  **  3. all segments except one are currently inputs to a merge operation.
  **
  ** In the first case, if there are no tombstone hash pages, return NULL. In
  ** the second, increment the ref-count on *pStruct and return a copy of the
  ** pointer to it.
  */
  if( nSeg==0 ) return 0;
  for(i=0; i<pStruct->nLevel; i++){
    int nThis = pStruct->aLevel[i].nSeg;
    int nMerge = pStruct->aLevel[i].nMerge;
    if( nThis>0 && (nThis==nSeg || (nThis==nSeg-1 && nMerge==nThis)) ){
      if( nSeg==1 && nThis==1 && pStruct->aLevel[i].aSeg[0].nPgTombstone==0 ){
        return 0;
      }
      fts5StructureRef(pStruct);
      return pStruct;
    }
    assert( pStruct->aLevel[i].nMerge<=nThis );
  }

  nByte += (((i64)pStruct->nLevel)+1) * sizeof(Fts5StructureLevel);
  assert( nByte==(i64)SZ_FTS5STRUCTURE(pStruct->nLevel+2) );
  pNew = (Fts5Structure*)sqlite3Fts5MallocZero(&p->rc, nByte);

  if( pNew ){
    Fts5StructureLevel *pLvl;
    nByte = nSeg * sizeof(Fts5StructureSegment);
    pNew->nLevel = MIN(pStruct->nLevel+1, FTS5_MAX_LEVEL);
    pNew->nRef = 1;
    pNew->nWriteCounter = pStruct->nWriteCounter;
    pNew->nOriginCntr = pStruct->nOriginCntr;
    pLvl = &pNew->aLevel[pNew->nLevel-1];
    pLvl->aSeg = (Fts5StructureSegment*)sqlite3Fts5MallocZero(&p->rc, nByte);
    if( pLvl->aSeg ){
      int iLvl, iSeg;
      int iSegOut = 0;
      /* Iterate through all segments, from oldest to newest. Add them to
      ** the new Fts5Level object so that pLvl->aSeg[0] is the oldest
      ** segment in the data structure.  */
      for(iLvl=pStruct->nLevel-1; iLvl>=0; iLvl--){
        for(iSeg=0; iSeg<pStruct->aLevel[iLvl].nSeg; iSeg++){
          pLvl->aSeg[iSegOut] = pStruct->aLevel[iLvl].aSeg[iSeg];
          iSegOut++;
        }
      }
      pNew->nSegment = pLvl->nSeg = nSeg;
    }else{
      sqlite3_free(pNew);
      pNew = 0;
    }
  }

  return pNew;
}

int sqlite3Fts5IndexOptimize(Fts5Index *p){
  Fts5Structure *pStruct;
  Fts5Structure *pNew = 0;

  assert( p->rc==SQLITE_OK );
  fts5IndexFlush(p);
  assert( p->rc!=SQLITE_OK || p->nContentlessDelete==0 );
  pStruct = fts5StructureRead(p);
  assert( p->rc!=SQLITE_OK || pStruct!=0 );
  fts5StructureInvalidate(p);

  if( pStruct ){
    pNew = fts5IndexOptimizeStruct(p, pStruct);
  }
  fts5StructureRelease(pStruct);

  assert( pNew==0 || pNew->nSegment>0 );
  if( pNew ){
    int iLvl;
    for(iLvl=0; pNew->aLevel[iLvl].nSeg==0; iLvl++){}
    while( p->rc==SQLITE_OK && pNew->aLevel[iLvl].nSeg>0 ){
      int nRem = FTS5_OPT_WORK_UNIT;
      fts5IndexMergeLevel(p, &pNew, iLvl, &nRem);
    }

    fts5StructureWrite(p, pNew);
    fts5StructureRelease(pNew);
  }

  return fts5IndexReturn(p); 
}

/*
** This is called to implement the special "VALUES('merge', $nMerge)"
** INSERT command.
*/
int sqlite3Fts5IndexMerge(Fts5Index *p, int nMerge){
  Fts5Structure *pStruct = 0;

  fts5IndexFlush(p);
  pStruct = fts5StructureRead(p);
  if( pStruct ){
    int nMin = p->pConfig->nUsermerge;
    fts5StructureInvalidate(p);
    if( nMerge<0 ){
      Fts5Structure *pNew = fts5IndexOptimizeStruct(p, pStruct);
      fts5StructureRelease(pStruct);
      pStruct = pNew;
      nMin = 1;
      nMerge = nMerge*-1;
    }
    if( pStruct && pStruct->nLevel ){
      if( fts5IndexMerge(p, &pStruct, nMerge, nMin) ){
        fts5StructureWrite(p, pStruct);
      }
    }
    fts5StructureRelease(pStruct);
  }
  return fts5IndexReturn(p);
}

static void fts5AppendRowid(
  Fts5Index *p,
  u64 iDelta,
  Fts5Iter *pUnused,
  Fts5Buffer *pBuf
){
  UNUSED_PARAM(pUnused);
  fts5BufferAppendVarint(&p->rc, pBuf, iDelta);
}

static void fts5AppendPoslist(
  Fts5Index *p,
  u64 iDelta,
  Fts5Iter *pMulti,
  Fts5Buffer *pBuf
){
  int nData = pMulti->base.nData;
  int nByte = nData + 9 + 9 + FTS5_DATA_ZERO_PADDING;
  assert( nData>0 );
  if( p->rc==SQLITE_OK && 0==fts5BufferGrow(&p->rc, pBuf, nByte) ){
    fts5BufferSafeAppendVarint(pBuf, iDelta);
    fts5BufferSafeAppendVarint(pBuf, nData*2);
    fts5BufferSafeAppendBlob(pBuf, pMulti->base.pData, nData);
    memset(&pBuf->p[pBuf->n], 0, FTS5_DATA_ZERO_PADDING);
  }
}


static void fts5DoclistIterNext(Fts5DoclistIter *pIter){
  u8 *p = pIter->aPoslist + pIter->nSize + pIter->nPoslist;

  assert( pIter->aPoslist || (p==0 && pIter->aPoslist==0) );
  if( p>=pIter->aEof ){
    pIter->aPoslist = 0;
  }else{
    i64 iDelta;

    p += fts5GetVarint(p, (u64*)&iDelta);
    pIter->iRowid += iDelta;

    /* Read position list size */
    if( p[0] & 0x80 ){
      int nPos;
      pIter->nSize = fts5GetVarint32(p, nPos);
      pIter->nPoslist = (nPos>>1);
    }else{
      pIter->nPoslist = ((int)(p[0])) >> 1;
      pIter->nSize = 1;
    }

    pIter->aPoslist = p;
    if( &pIter->aPoslist[pIter->nPoslist]>pIter->aEof ){
      pIter->aPoslist = 0;
    }
  }
}

static void fts5DoclistIterInit(
  Fts5Buffer *pBuf, 
  Fts5DoclistIter *pIter
){
  memset(pIter, 0, sizeof(*pIter));
  if( pBuf->n>0 ){
    pIter->aPoslist = pBuf->p;
    pIter->aEof = &pBuf->p[pBuf->n];
    fts5DoclistIterNext(pIter);
  }
}

#if 0
/*
** Append a doclist to buffer pBuf.
**
** This function assumes that space within the buffer has already been
** allocated.
*/
static void fts5MergeAppendDocid(
  Fts5Buffer *pBuf,               /* Buffer to write to */
  i64 *piLastRowid,               /* IN/OUT: Previous rowid written (if any) */
  i64 iRowid                      /* Rowid to append */
){
  assert( pBuf->n!=0 || (*piLastRowid)==0 );
  fts5BufferSafeAppendVarint(pBuf, iRowid - *piLastRowid);
  *piLastRowid = iRowid;
}
#endif

#define fts5MergeAppendDocid(pBuf, iLastRowid, iRowid) {                 \
  assert( (pBuf)->n!=0 || (iLastRowid)==0 );                             \
  fts5BufferSafeAppendVarint((pBuf), (u64)(iRowid) - (u64)(iLastRowid)); \
  (iLastRowid) = (iRowid);                                               \
}

/*
** Swap the contents of buffer *p1 with that of *p2.
*/
static void fts5BufferSwap(Fts5Buffer *p1, Fts5Buffer *p2){
  Fts5Buffer tmp = *p1;
  *p1 = *p2;
  *p2 = tmp;
}

static void fts5NextRowid(Fts5Buffer *pBuf, int *piOff, i64 *piRowid){
  int i = *piOff;
  if( i>=pBuf->n ){
    *piOff = -1;
  }else{
    u64 iVal;
    *piOff = i + sqlite3Fts5GetVarint(&pBuf->p[i], &iVal);
    *piRowid += iVal;
  }
}

/*
** This is the equivalent of fts5MergePrefixLists() for detail=none mode.
** In this case the buffers consist of a delta-encoded list of rowids only.
*/
static void fts5MergeRowidLists(
  Fts5Index *p,                   /* FTS5 backend object */
  Fts5Buffer *p1,                 /* First list to merge */
  int nBuf,                       /* Number of entries in apBuf[] */
  Fts5Buffer *aBuf                /* Array of other lists to merge into p1 */
){
  int i1 = 0;
  int i2 = 0;
  i64 iRowid1 = 0;
  i64 iRowid2 = 0;
  i64 iOut = 0;
  Fts5Buffer *p2 = &aBuf[0];
  Fts5Buffer out;

  (void)nBuf;
  memset(&out, 0, sizeof(out));
  assert( nBuf==1 );
  sqlite3Fts5BufferSize(&p->rc, &out, p1->n + p2->n);
  if( p->rc ) return;

  fts5NextRowid(p1, &i1, &iRowid1);
  fts5NextRowid(p2, &i2, &iRowid2);
  while( i1>=0 || i2>=0 ){
    if( i1>=0 && (i2<0 || iRowid1<iRowid2) ){
      assert( iOut==0 || iRowid1>iOut );
      fts5BufferSafeAppendVarint(&out, iRowid1 - iOut);
      iOut = iRowid1;
      fts5NextRowid(p1, &i1, &iRowid1);
    }else{
      assert( iOut==0 || iRowid2>iOut );
      fts5BufferSafeAppendVarint(&out, iRowid2 - iOut);
      iOut = iRowid2;
      if( i1>=0 && iRowid1==iRowid2 ){
        fts5NextRowid(p1, &i1, &iRowid1);
      }
      fts5NextRowid(p2, &i2, &iRowid2);
    }
  }

  fts5BufferSwap(&out, p1);
  fts5BufferFree(&out);
}

typedef struct PrefixMerger PrefixMerger;
struct PrefixMerger {
  Fts5DoclistIter iter;           /* Doclist iterator */
  i64 iPos;                       /* For iterating through a position list */
  int iOff;
  u8 *aPos;
  PrefixMerger *pNext;            /* Next in docid/poslist order */
};

static void fts5PrefixMergerInsertByRowid(
  PrefixMerger **ppHead, 
  PrefixMerger *p
){
  if( p->iter.aPoslist ){
    PrefixMerger **pp = ppHead;
    while( *pp && p->iter.iRowid>(*pp)->iter.iRowid ){
      pp = &(*pp)->pNext;
    }
    p->pNext = *pp;
    *pp = p;
  }
}

static void fts5PrefixMergerInsertByPosition(
  PrefixMerger **ppHead, 
  PrefixMerger *p
){
  if( p->iPos>=0 ){
    PrefixMerger **pp = ppHead;
    while( *pp && p->iPos>(*pp)->iPos ){
      pp = &(*pp)->pNext;
    }
    p->pNext = *pp;
    *pp = p;
  }
}


/*
** Array aBuf[] contains nBuf doclists. These are all merged in with the
** doclist in buffer p1.
*/
static void fts5MergePrefixLists(
  Fts5Index *p,                   /* FTS5 backend object */
  Fts5Buffer *p1,                 /* First list to merge */
  int nBuf,                       /* Number of buffers in array aBuf[] */
  Fts5Buffer *aBuf                /* Other lists to merge in */ 
){
#define fts5PrefixMergerNextPosition(p) \
  sqlite3Fts5PoslistNext64((p)->aPos,(p)->iter.nPoslist,&(p)->iOff,&(p)->iPos)
#define FTS5_MERGE_NLIST 16
  PrefixMerger aMerger[FTS5_MERGE_NLIST];
  PrefixMerger *pHead = 0;
  int i;
  int nOut = 0;
  Fts5Buffer out = {0, 0, 0};
  Fts5Buffer tmp = {0, 0, 0};
  i64 iLastRowid = 0;

  /* Initialize a doclist-iterator for each input buffer. Arrange them in
  ** a linked-list starting at pHead in ascending order of rowid. Avoid
  ** linking any iterators already at EOF into the linked list at all. */ 
  assert( nBuf+1<=(int)(sizeof(aMerger)/sizeof(aMerger[0])) );
  memset(aMerger, 0, sizeof(PrefixMerger)*(nBuf+1));
  pHead = &aMerger[nBuf];
  fts5DoclistIterInit(p1, &pHead->iter);
  for(i=0; i<nBuf; i++){
    fts5DoclistIterInit(&aBuf[i], &aMerger[i].iter);
    fts5PrefixMergerInsertByRowid(&pHead, &aMerger[i]);
    nOut += aBuf[i].n;
  }
  if( nOut==0 ) return;
  nOut += p1->n + 9 + 10*nBuf;

  /* The maximum size of the output is equal to the sum of the
  ** input sizes + 1 varint (9 bytes). The extra varint is because if the
  ** first rowid in one input is a large negative number, and the first in
  ** the other a non-negative number, the delta for the non-negative
  ** number will be larger on disk than the literal integer value
  ** was.  
  **
  ** Or, if the input position-lists are corrupt, then the output might
  ** include up to (nBuf+1) extra 10-byte positions created by interpreting -1
  ** (the value PoslistNext64() uses for EOF) as a position and appending
  ** it to the output. This can happen at most once for each input 
  ** position-list, hence (nBuf+1) 10 byte paddings.  */
  if( sqlite3Fts5BufferSize(&p->rc, &out, nOut) ) return;

  while( pHead ){
    fts5MergeAppendDocid(&out, iLastRowid, pHead->iter.iRowid);

    if( pHead->pNext && iLastRowid==pHead->pNext->iter.iRowid ){
      /* Merge data from two or more poslists */
      i64 iPrev = 0;
      int nTmp = FTS5_DATA_ZERO_PADDING;
      int nMerge = 0;
      PrefixMerger *pSave = pHead;
      PrefixMerger *pThis = 0;
      int nTail = 0;

      pHead = 0;
      while( pSave && pSave->iter.iRowid==iLastRowid ){
        PrefixMerger *pNext = pSave->pNext;
        pSave->iOff = 0;
        pSave->iPos = 0;
        pSave->aPos = &pSave->iter.aPoslist[pSave->iter.nSize];
        fts5PrefixMergerNextPosition(pSave);
        nTmp += pSave->iter.nPoslist + 10;
        nMerge++;
        fts5PrefixMergerInsertByPosition(&pHead, pSave);
        pSave = pNext;
      }

      if( pHead==0 || pHead->pNext==0 ){
        FTS5_CORRUPT_IDX(p);
        break;
      }

      /* See the earlier comment in this function for an explanation of why
      ** corrupt input position lists might cause the output to consume
      ** at most nMerge*10 bytes of unexpected space. */
      if( sqlite3Fts5BufferSize(&p->rc, &tmp, nTmp+nMerge*10) ){
        break;
      }
      fts5BufferZero(&tmp);

      pThis = pHead;
      pHead = pThis->pNext;
      sqlite3Fts5PoslistSafeAppend(&tmp, &iPrev, pThis->iPos);
      fts5PrefixMergerNextPosition(pThis);
      fts5PrefixMergerInsertByPosition(&pHead, pThis);

      while( pHead->pNext ){
        pThis = pHead;
        if( pThis->iPos!=iPrev ){
          sqlite3Fts5PoslistSafeAppend(&tmp, &iPrev, pThis->iPos);
        }
        fts5PrefixMergerNextPosition(pThis);
        pHead = pThis->pNext;
        fts5PrefixMergerInsertByPosition(&pHead, pThis);
      }

      if( pHead->iPos!=iPrev ){
        sqlite3Fts5PoslistSafeAppend(&tmp, &iPrev, pHead->iPos);
      }
      nTail = pHead->iter.nPoslist - pHead->iOff;

      /* WRITEPOSLISTSIZE */
      assert_nc( tmp.n+nTail<=nTmp );
      assert( tmp.n+nTail<=nTmp+nMerge*10 );
      if( tmp.n+nTail>nTmp-FTS5_DATA_ZERO_PADDING ){
        if( p->rc==SQLITE_OK ) FTS5_CORRUPT_IDX(p);
        break;
      }
      fts5BufferSafeAppendVarint(&out, (tmp.n+nTail) * 2);
      fts5BufferSafeAppendBlob(&out, tmp.p, tmp.n);
      if( nTail>0 ){
        fts5BufferSafeAppendBlob(&out, &pHead->aPos[pHead->iOff], nTail);
      }

      pHead = pSave;
      for(i=0; i<nBuf+1; i++){
        PrefixMerger *pX = &aMerger[i];
        if( pX->iter.aPoslist && pX->iter.iRowid==iLastRowid ){
          fts5DoclistIterNext(&pX->iter);
          fts5PrefixMergerInsertByRowid(&pHead, pX);
        }
      }

    }else{
      /* Copy poslist from pHead to output */
      PrefixMerger *pThis = pHead;
      Fts5DoclistIter *pI = &pThis->iter;
      fts5BufferSafeAppendBlob(&out, pI->aPoslist, pI->nPoslist+pI->nSize);
      fts5DoclistIterNext(pI);
      pHead = pThis->pNext;
      fts5PrefixMergerInsertByRowid(&pHead, pThis);
    }
  }

  fts5BufferFree(p1);
  fts5BufferFree(&tmp);
  memset(&out.p[out.n], 0, FTS5_DATA_ZERO_PADDING);
  *p1 = out;
}


/*
** Iterate through a range of entries in the FTS index, invoking the xVisit
** callback for each of them.
**
** Parameter pToken points to an nToken buffer containing an FTS index term
** (i.e. a document term with the preceding 1 byte index identifier -
** FTS5_MAIN_PREFIX or similar). If bPrefix is true, then the call visits
** all entries for terms that have pToken/nToken as a prefix. If bPrefix
** is false, then only entries with pToken/nToken as the entire key are
** visited. 
**
** If the current table is a tokendata=1 table, then if bPrefix is true then
** each index term is treated separately. However, if bPrefix is false, then
** all index terms corresponding to pToken/nToken are collapsed into a single
** term before the callback is invoked.
**
** The callback invoked for each entry visited is specified by paramter xVisit.
** Each time it is invoked, it is passed a pointer to the Fts5Index object,
** a copy of the 7th paramter to this function (pCtx) and a pointer to the
** iterator that indicates the current entry. If the current entry is the
** first with a new term (i.e. different from that of the previous entry,
** including the very first term), then the final two parameters are passed
** a pointer to the term and its size in bytes, respectively. If the current
** entry is not the first associated with its term, these two parameters
** are passed 0.
**
** If parameter pColset is not NULL, then it is used to filter entries before
** the callback is invoked.
*/
static int fts5VisitEntries(
  Fts5Index *p,                   /* Fts5 index object */
  Fts5Colset *pColset,            /* Columns filter to apply, or NULL */
  u8 *pToken,                     /* Buffer containing token */
  int nToken,                     /* Size of buffer pToken in bytes */
  int bPrefix,                    /* True for a prefix scan */
  void (*xVisit)(Fts5Index*, void *pCtx, Fts5Iter *pIter, const u8*, int),
  void *pCtx                      /* Passed as second argument to xVisit() */
){
  const int flags = (bPrefix ? FTS5INDEX_QUERY_SCAN : 0)
                  | FTS5INDEX_QUERY_SKIPEMPTY 
                  | FTS5INDEX_QUERY_NOOUTPUT;
  Fts5Iter *p1 = 0;     /* Iterator used to gather data from index */
  int bNewTerm = 1;
  Fts5Structure *pStruct = fts5StructureRead(p);

  fts5MultiIterNew(p, pStruct, flags, pColset, pToken, nToken, -1, 0, &p1);
  fts5IterSetOutputCb(&p->rc, p1);
  for( /* no-op */ ;
      fts5MultiIterEof(p, p1)==0;
      fts5MultiIterNext2(p, p1, &bNewTerm)
  ){
    Fts5SegIter *pSeg = &p1->aSeg[ p1->aFirst[1].iFirst ];
    int nNew = 0;
    const u8 *pNew = 0;

    p1->xSetOutputs(p1, pSeg);
    if( p->rc ) break;

    if( bNewTerm ){
      nNew = pSeg->term.n;
      pNew = pSeg->term.p;
      if( nNew<nToken || memcmp(pToken, pNew, nToken) ) break;
    }

    xVisit(p, pCtx, p1, pNew, nNew);
  }
  fts5MultiIterFree(p1);

  fts5StructureRelease(pStruct);
  return p->rc;
}


/*
** Usually, a tokendata=1 iterator (struct Fts5TokenDataIter) accumulates an
** array of these for each row it visits (so all iRowid fields are the same).
** Or, for an iterator used by an "ORDER BY rank" query, it accumulates an
** array of these for the entire query (in which case iRowid fields may take
** a variety of values).
**
** Each instance in the array indicates the iterator (and therefore term)
** associated with position iPos of rowid iRowid. This is used by the
** xInstToken() API.
**
** iRowid:
**   Rowid for the current entry.
**
** iPos:
**   Position of current entry within row. In the usual ((iCol<<32)+iOff)
**   format (e.g. see macros FTS5_POS2COLUMN() and FTS5_POS2OFFSET()).
**
** iIter:
**   If the Fts5TokenDataIter iterator that the entry is part of is
**   actually an iterator (i.e. with nIter>0, not just a container for
**   Fts5TokenDataMap structures), then this variable is an index into
**   the apIter[] array. The corresponding term is that which the iterator
**   at apIter[iIter] currently points to.
**
**   Or, if the Fts5TokenDataIter iterator is just a container object
**   (nIter==0), then iIter is an index into the term.p[] buffer where
**   the term is stored.
**
** nByte:
**   In the case where iIter is an index into term.p[], this variable
**   is the size of the term in bytes. If iIter is an index into apIter[],
**   this variable is unused.
*/
struct Fts5TokenDataMap {
  i64 iRowid;                     /* Row this token is located in */
  i64 iPos;                       /* Position of token */
  int iIter;                      /* Iterator token was read from */
  int nByte;                      /* Length of token in bytes (or 0) */
};

/*
** An object used to supplement Fts5Iter for tokendata=1 iterators.
**
** This object serves two purposes. The first is as a container for an array
** of Fts5TokenDataMap structures, which are used to find the token required
** when the xInstToken() API is used. This is done by the nMapAlloc, nMap and
** aMap[] variables.
*/
struct Fts5TokenDataIter {
  int nMapAlloc;                  /* Allocated size of aMap[] in entries */
  int nMap;                       /* Number of valid entries in aMap[] */
  Fts5TokenDataMap *aMap;         /* Array of (rowid+pos -> token) mappings */

  /* The following are used for prefix-queries only. */
  Fts5Buffer terms;

  /* The following are used for other full-token tokendata queries only. */
  int nIter;
  int nIterAlloc;
  Fts5PoslistReader *aPoslistReader;
  int *aPoslistToIter;
  Fts5Iter *apIter[FLEXARRAY];
};

/* Size in bytes of an Fts5TokenDataIter object holding up to N iterators */
#define SZ_FTS5TOKENDATAITER(N) \
    (offsetof(Fts5TokenDataIter,apIter) + (N)*sizeof(Fts5Iter))

/*
** The two input arrays - a1[] and a2[] - are in sorted order. This function
** merges the two arrays together and writes the result to output array 
** aOut[]. aOut[] is guaranteed to be large enough to hold the result.
**
** Duplicate entries are copied into the output. So the size of the output
** array is always (n1+n2) entries.
*/
static void fts5TokendataMerge(
  Fts5TokenDataMap *a1, int n1,   /* Input array 1 */
  Fts5TokenDataMap *a2, int n2,   /* Input array 2 */
  Fts5TokenDataMap *aOut          /* Output array */
){
  int i1 = 0;
  int i2 = 0;

  assert( n1>=0 && n2>=0 );
  while( i1<n1 || i2<n2 ){
    Fts5TokenDataMap *pOut = &aOut[i1+i2];
    if( i2>=n2 || (i1<n1 && (
        a1[i1].iRowid<a2[i2].iRowid
     || (a1[i1].iRowid==a2[i2].iRowid && a1[i1].iPos<=a2[i2].iPos)
    ))){
      memcpy(pOut, &a1[i1], sizeof(Fts5TokenDataMap));
      i1++;
    }else{
      memcpy(pOut, &a2[i2], sizeof(Fts5TokenDataMap));
      i2++;
    }
  }
}


/*
** Append a mapping to the token-map belonging to object pT.
*/
static void fts5TokendataIterAppendMap(
  Fts5Index *p, 
  Fts5TokenDataIter *pT, 
  int iIter,
  int nByte,
  i64 iRowid, 
  i64 iPos
){
  if( p->rc==SQLITE_OK ){
    if( pT->nMap==pT->nMapAlloc ){
      int nNew = pT->nMapAlloc ? pT->nMapAlloc*2 : 64;
      int nAlloc = nNew * sizeof(Fts5TokenDataMap);
      Fts5TokenDataMap *aNew;

      aNew = (Fts5TokenDataMap*)sqlite3_realloc(pT->aMap, nAlloc);
      if( aNew==0 ){
        p->rc = SQLITE_NOMEM;
        return;
      }

      pT->aMap = aNew;
      pT->nMapAlloc = nNew;
    }

    pT->aMap[pT->nMap].iRowid = iRowid;
    pT->aMap[pT->nMap].iPos = iPos;
    pT->aMap[pT->nMap].iIter = iIter;
    pT->aMap[pT->nMap].nByte = nByte;
    pT->nMap++;
  }
}

/*
** Sort the contents of the pT->aMap[] array.
**
** The sorting algorithm requires a malloc(). If this fails, an error code
** is left in Fts5Index.rc before returning.
*/
static void fts5TokendataIterSortMap(Fts5Index *p, Fts5TokenDataIter *pT){
  Fts5TokenDataMap *aTmp = 0;
  int nByte = pT->nMap * sizeof(Fts5TokenDataMap);

  aTmp = (Fts5TokenDataMap*)sqlite3Fts5MallocZero(&p->rc, nByte);
  if( aTmp ){
    Fts5TokenDataMap *a1 = pT->aMap;
    Fts5TokenDataMap *a2 = aTmp;
    i64 nHalf;

    for(nHalf=1; nHalf<pT->nMap; nHalf=nHalf*2){
      int i1;
      for(i1=0; i1<pT->nMap; i1+=(nHalf*2)){
        int n1 = MIN(nHalf, pT->nMap-i1);
        int n2 = MIN(nHalf, pT->nMap-i1-n1);
        fts5TokendataMerge(&a1[i1], n1, &a1[i1+n1], n2, &a2[i1]);
      }
      SWAPVAL(Fts5TokenDataMap*, a1, a2);
    }

    if( a1!=pT->aMap ){
      memcpy(pT->aMap, a1, pT->nMap*sizeof(Fts5TokenDataMap));
    }
    sqlite3_free(aTmp);

#ifdef SQLITE_DEBUG
    {
      int ii;
      for(ii=1; ii<pT->nMap; ii++){
        Fts5TokenDataMap *p1 = &pT->aMap[ii-1];
        Fts5TokenDataMap *p2 = &pT->aMap[ii];
        assert( p1->iRowid<p2->iRowid 
             || (p1->iRowid==p2->iRowid && p1->iPos<=p2->iPos)
        );
      }
    }
#endif
  }
}

/*
** Delete an Fts5TokenDataIter structure and its contents.
*/
static void fts5TokendataIterDelete(Fts5TokenDataIter *pSet){
  if( pSet ){
    int ii;
    for(ii=0; ii<pSet->nIter; ii++){
      fts5MultiIterFree(pSet->apIter[ii]);
    }
    fts5BufferFree(&pSet->terms);
    sqlite3_free(pSet->aPoslistReader);
    sqlite3_free(pSet->aMap);
    sqlite3_free(pSet);
  }
}


/*
** fts5VisitEntries() context object used by fts5SetupPrefixIterTokendata()
** to pass data to prefixIterSetupTokendataCb().
*/
typedef struct TokendataSetupCtx TokendataSetupCtx;
struct TokendataSetupCtx {
  Fts5TokenDataIter *pT;          /* Object being populated with mappings */
  int iTermOff;                   /* Offset of current term in terms.p[] */
  int nTermByte;                  /* Size of current term in bytes */
};

/*
** fts5VisitEntries() callback used by fts5SetupPrefixIterTokendata(). This
** callback adds an entry to the Fts5TokenDataIter.aMap[] array for each
** position in the current position-list. It doesn't matter that some of
** these may be out of order - they will be sorted later.
*/
static void prefixIterSetupTokendataCb(
  Fts5Index *p, 
  void *pCtx, 
  Fts5Iter *p1, 
  const u8 *pNew,
  int nNew
){
  TokendataSetupCtx *pSetup = (TokendataSetupCtx*)pCtx;
  int iPosOff = 0;
  i64 iPos = 0;

  if( pNew ){
    pSetup->nTermByte = nNew-1;
    pSetup->iTermOff = pSetup->pT->terms.n;
    fts5BufferAppendBlob(&p->rc, &pSetup->pT->terms, nNew-1, pNew+1);
  }

  while( 0==sqlite3Fts5PoslistNext64(
     p1->base.pData, p1->base.nData, &iPosOff, &iPos
  ) ){
    fts5TokendataIterAppendMap(p, 
        pSetup->pT, pSetup->iTermOff, pSetup->nTermByte, p1->base.iRowid, iPos
    );
  }
}


/*
** Context object passed by fts5SetupPrefixIter() to fts5VisitEntries().
*/
typedef struct PrefixSetupCtx PrefixSetupCtx;
struct PrefixSetupCtx {
  void (*xMerge)(Fts5Index*, Fts5Buffer*, int, Fts5Buffer*);
  void (*xAppend)(Fts5Index*, u64, Fts5Iter*, Fts5Buffer*);
  i64 iLastRowid;
  int nMerge;
  Fts5Buffer *aBuf;
  int nBuf;
  Fts5Buffer doclist;
  TokendataSetupCtx *pTokendata;
};

/*
** fts5VisitEntries() callback used by fts5SetupPrefixIter()
*/
static void prefixIterSetupCb(
  Fts5Index *p, 
  void *pCtx, 
  Fts5Iter *p1, 
  const u8 *pNew,
  int nNew
){
  PrefixSetupCtx *pSetup = (PrefixSetupCtx*)pCtx;
  const int nMerge = pSetup->nMerge;

  if( p1->base.nData>0 ){
    if( p1->base.iRowid<=pSetup->iLastRowid && pSetup->doclist.n>0 ){
      int i;
      for(i=0; p->rc==SQLITE_OK && pSetup->doclist.n; i++){
        int i1 = i*nMerge;
        int iStore;
        assert( i1+nMerge<=pSetup->nBuf );
        for(iStore=i1; iStore<i1+nMerge; iStore++){
          if( pSetup->aBuf[iStore].n==0 ){
            fts5BufferSwap(&pSetup->doclist, &pSetup->aBuf[iStore]);
            fts5BufferZero(&pSetup->doclist);
            break;
          }
        }
        if( iStore==i1+nMerge ){
          pSetup->xMerge(p, &pSetup->doclist, nMerge, &pSetup->aBuf[i1]);
          for(iStore=i1; iStore<i1+nMerge; iStore++){
            fts5BufferZero(&pSetup->aBuf[iStore]);
          }
        }
      }
      pSetup->iLastRowid = 0;
    }

    pSetup->xAppend(
        p, (u64)p1->base.iRowid-(u64)pSetup->iLastRowid, p1, &pSetup->doclist
    );
    pSetup->iLastRowid = p1->base.iRowid;
  }

  if( pSetup->pTokendata ){
    prefixIterSetupTokendataCb(p, (void*)pSetup->pTokendata, p1, pNew, nNew);
  }
}

static void fts5SetupPrefixIter(
  Fts5Index *p,                   /* Index to read from */
  int bDesc,                      /* True for "ORDER BY rowid DESC" */
  int iIdx,                       /* Index to scan for data */
  u8 *pToken,                     /* Buffer containing prefix to match */
  int nToken,                     /* Size of buffer pToken in bytes */
  Fts5Colset *pColset,            /* Restrict matches to these columns */
  Fts5Iter **ppIter               /* OUT: New iterator */
){
  Fts5Structure *pStruct;
  PrefixSetupCtx s;
  TokendataSetupCtx s2;

  memset(&s, 0, sizeof(s));
  memset(&s2, 0, sizeof(s2));

  s.nMerge = 1;
  s.iLastRowid = 0;
  s.nBuf = 32;
  if( iIdx==0 
   && p->pConfig->eDetail==FTS5_DETAIL_FULL 
   && p->pConfig->bPrefixInsttoken 
  ){
    s.pTokendata = &s2;
    s2.pT = (Fts5TokenDataIter*)fts5IdxMalloc(p, SZ_FTS5TOKENDATAITER(1));
  }

  if( p->pConfig->eDetail==FTS5_DETAIL_NONE ){
    s.xMerge = fts5MergeRowidLists;
    s.xAppend = fts5AppendRowid;
  }else{
    s.nMerge = FTS5_MERGE_NLIST-1;
    s.nBuf = s.nMerge*8;   /* Sufficient to merge (16^8)==(2^32) lists */
    s.xMerge = fts5MergePrefixLists;
    s.xAppend = fts5AppendPoslist;
  }

  s.aBuf = (Fts5Buffer*)fts5IdxMalloc(p, sizeof(Fts5Buffer)*s.nBuf);
  pStruct = fts5StructureRead(p);
  assert( p->rc!=SQLITE_OK || (s.aBuf && pStruct) );

  if( p->rc==SQLITE_OK ){
    void *pCtx = (void*)&s;
    int i;
    Fts5Data *pData;

    /* If iIdx is non-zero, then it is the number of a prefix-index for
    ** prefixes 1 character longer than the prefix being queried for. That
    ** index contains all the doclists required, except for the one
    ** corresponding to the prefix itself. That one is extracted from the
    ** main term index here.  */
    if( iIdx!=0 ){
      pToken[0] = FTS5_MAIN_PREFIX;
      fts5VisitEntries(p, pColset, pToken, nToken, 0, prefixIterSetupCb, pCtx);
    }

    pToken[0] = FTS5_MAIN_PREFIX + iIdx;
    fts5VisitEntries(p, pColset, pToken, nToken, 1, prefixIterSetupCb, pCtx);

    assert( (s.nBuf%s.nMerge)==0 );
    for(i=0; i<s.nBuf; i+=s.nMerge){
      int iFree;
      if( p->rc==SQLITE_OK ){
        s.xMerge(p, &s.doclist, s.nMerge, &s.aBuf[i]);
      }
      for(iFree=i; iFree<i+s.nMerge; iFree++){
        fts5BufferFree(&s.aBuf[iFree]);
      }
    }

    pData = fts5IdxMalloc(p, sizeof(*pData)
                             + ((i64)s.doclist.n)+FTS5_DATA_ZERO_PADDING);
    assert( pData!=0 || p->rc!=SQLITE_OK );
    if( pData ){
      pData->p = (u8*)&pData[1];
      pData->nn = pData->szLeaf = s.doclist.n;
      if( s.doclist.n ) memcpy(pData->p, s.doclist.p, s.doclist.n);
      fts5MultiIterNew2(p, pData, bDesc, ppIter);
    }

    assert( (*ppIter)!=0 || p->rc!=SQLITE_OK );
    if( p->rc==SQLITE_OK && s.pTokendata ){
      fts5TokendataIterSortMap(p, s2.pT);
      (*ppIter)->pTokenDataIter = s2.pT;
      s2.pT = 0;
    }
  }

  fts5TokendataIterDelete(s2.pT);
  fts5BufferFree(&s.doclist);
  fts5StructureRelease(pStruct);
  sqlite3_free(s.aBuf);
}


/*
** Indicate that all subsequent calls to sqlite3Fts5IndexWrite() pertain
** to the document with rowid iRowid.
*/
int sqlite3Fts5IndexBeginWrite(Fts5Index *p, int bDelete, i64 iRowid){
  assert( p->rc==SQLITE_OK );

  /* Allocate the hash table if it has not already been allocated */
  if( p->pHash==0 ){
    p->rc = sqlite3Fts5HashNew(p->pConfig, &p->pHash, &p->nPendingData);
  }

  /* Flush the hash table to disk if required */
  if( iRowid<p->iWriteRowid 
   || (iRowid==p->iWriteRowid && p->bDelete==0)
   || (p->nPendingData > p->pConfig->nHashSize)
  ){
    fts5IndexFlush(p);
  }

  p->iWriteRowid = iRowid;
  p->bDelete = bDelete;
  if( bDelete==0 ){
    p->nPendingRow++;
  }
  return fts5IndexReturn(p);
}

/*
** Commit data to disk.
*/
int sqlite3Fts5IndexSync(Fts5Index *p){
  assert( p->rc==SQLITE_OK );
  fts5IndexFlush(p);
  fts5IndexCloseReader(p);
  return fts5IndexReturn(p);
}

/*
** Discard any data stored in the in-memory hash tables. Do not write it
** to the database. Additionally, assume that the contents of the %_data
** table may have changed on disk. So any in-memory caches of %_data 
** records must be invalidated.
*/
int sqlite3Fts5IndexRollback(Fts5Index *p){
  fts5IndexCloseReader(p);
  fts5IndexDiscardData(p);
  fts5StructureInvalidate(p);
  return fts5IndexReturn(p);
}

/*
** The %_data table is completely empty when this function is called. This
** function populates it with the initial structure objects for each index,
** and the initial version of the "averages" record (a zero-byte blob).
*/
int sqlite3Fts5IndexReinit(Fts5Index *p){
  Fts5Structure *pTmp;
  union {
    Fts5Structure sFts;
    u8 tmpSpace[SZ_FTS5STRUCTURE(1)];
  } uFts;
  fts5StructureInvalidate(p);
  fts5IndexDiscardData(p);
  pTmp = &uFts.sFts;
  memset(pTmp, 0, SZ_FTS5STRUCTURE(1));
  if( p->pConfig->bContentlessDelete ){
    pTmp->nOriginCntr = 1;
  }
  fts5DataWrite(p, FTS5_AVERAGES_ROWID, (const u8*)"", 0);
  fts5StructureWrite(p, pTmp);
  return fts5IndexReturn(p);
}

/*
** Open a new Fts5Index handle. If the bCreate argument is true, create
** and initialize the underlying %_data table.
**
** If successful, set *pp to point to the new object and return SQLITE_OK.
** Otherwise, set *pp to NULL and return an SQLite error code.
*/
int sqlite3Fts5IndexOpen(
  Fts5Config *pConfig, 
  int bCreate, 
  Fts5Index **pp,
  char **pzErr
){
  int rc = SQLITE_OK;
  Fts5Index *p;                   /* New object */

  *pp = p = (Fts5Index*)sqlite3Fts5MallocZero(&rc, sizeof(Fts5Index));
  if( rc==SQLITE_OK ){
    p->pConfig = pConfig;
    p->nWorkUnit = FTS5_WORK_UNIT;
    p->zDataTbl = sqlite3Fts5Mprintf(&rc, "%s_data", pConfig->zName);
    if( p->zDataTbl && bCreate ){
      rc = sqlite3Fts5CreateTable(
          pConfig, "data", "id INTEGER PRIMARY KEY, block BLOB", 0, pzErr
      );
      if( rc==SQLITE_OK ){
        rc = sqlite3Fts5CreateTable(pConfig, "idx", 
            "segid, term, pgno, PRIMARY KEY(segid, term)", 
            1, pzErr
        );
      }
      if( rc==SQLITE_OK ){
        rc = sqlite3Fts5IndexReinit(p);
      }
    }
  }

  assert( rc!=SQLITE_OK || p->rc==SQLITE_OK );
  if( rc ){
    sqlite3Fts5IndexClose(p);
    *pp = 0;
  }
  return rc;
}

/*
** Close a handle opened by an earlier call to sqlite3Fts5IndexOpen().
*/
int sqlite3Fts5IndexClose(Fts5Index *p){
  int rc = SQLITE_OK;
  if( p ){
    assert( p->pReader==0 );
    fts5StructureInvalidate(p);
    sqlite3_finalize(p->pWriter);
    sqlite3_finalize(p->pDeleter);
    sqlite3_finalize(p->pIdxWriter);
    sqlite3_finalize(p->pIdxDeleter);
    sqlite3_finalize(p->pIdxSelect);
    sqlite3_finalize(p->pIdxNextSelect);
    sqlite3_finalize(p->pDataVersion);
    sqlite3_finalize(p->pDeleteFromIdx);
    sqlite3Fts5HashFree(p->pHash);
    sqlite3_free(p->zDataTbl);
    sqlite3_free(p);
  }
  return rc;
}

/*
** Argument p points to a buffer containing utf-8 text that is n bytes in 
** size. Return the number of bytes in the nChar character prefix of the
** buffer, or 0 if there are less than nChar characters in total.
*/
int sqlite3Fts5IndexCharlenToBytelen(
  const char *p, 
  int nByte, 
  int nChar
){
  int n = 0;
  int i;
  for(i=0; i<nChar; i++){
    if( n>=nByte ) return 0;      /* Input contains fewer than nChar chars */
    if( (unsigned char)p[n++]>=0xc0 ){
      if( n>=nByte ) return 0;
      while( (p[n] & 0xc0)==0x80 ){
        n++;
        if( n>=nByte ){
          if( i+1==nChar ) break;
          return 0;
        }
      }
    }
  }
  return n;
}

/*
** pIn is a UTF-8 encoded string, nIn bytes in size. Return the number of
** unicode characters in the string.
*/
static int fts5IndexCharlen(const char *pIn, int nIn){
  int nChar = 0;            
  int i = 0;
  while( i<nIn ){
    if( (unsigned char)pIn[i++]>=0xc0 ){
      while( i<nIn && (pIn[i] & 0xc0)==0x80 ) i++;
    }
    nChar++;
  }
  return nChar;
}

/*
** Insert or remove data to or from the index. Each time a document is 
** added to or removed from the index, this function is called one or more
** times.
**
** For an insert, it must be called once for each token in the new document.
** If the operation is a delete, it must be called (at least) once for each
** unique token in the document with an iCol value less than zero. The iPos
** argument is ignored for a delete.
*/
int sqlite3Fts5IndexWrite(
  Fts5Index *p,                   /* Index to write to */
  int iCol,                       /* Column token appears in (-ve -> delete) */
  int iPos,                       /* Position of token within column */
  const char *pToken, int nToken  /* Token to add or remove to or from index */
){
  int i;                          /* Used to iterate through indexes */
  int rc = SQLITE_OK;             /* Return code */
  Fts5Config *pConfig = p->pConfig;

  assert( p->rc==SQLITE_OK );
  assert( (iCol<0)==p->bDelete );

  /* Add the entry to the main terms index. */
  rc = sqlite3Fts5HashWrite(
      p->pHash, p->iWriteRowid, iCol, iPos, FTS5_MAIN_PREFIX, pToken, nToken
  );

  for(i=0; i<pConfig->nPrefix && rc==SQLITE_OK; i++){
    const int nChar = pConfig->aPrefix[i];
    int nByte = sqlite3Fts5IndexCharlenToBytelen(pToken, nToken, nChar);
    if( nByte ){
      rc = sqlite3Fts5HashWrite(p->pHash, 
          p->iWriteRowid, iCol, iPos, (char)(FTS5_MAIN_PREFIX+i+1), pToken,
          nByte
      );
    }
  }

  return rc;
}

/*
** pToken points to a buffer of size nToken bytes containing a search 
** term, including the index number at the start, used on a tokendata=1
** table. This function returns true if the term in buffer pBuf matches 
** token pToken/nToken.
*/
static int fts5IsTokendataPrefix(
  Fts5Buffer *pBuf,
  const u8 *pToken,
  int nToken
){
  return (
      pBuf->n>=nToken 
   && 0==memcmp(pBuf->p, pToken, nToken)
   && (pBuf->n==nToken || pBuf->p[nToken]==0x00)
  );
}

/*
** Ensure the segment-iterator passed as the only argument points to EOF.
*/
static void fts5SegIterSetEOF(Fts5SegIter *pSeg){
  fts5DataRelease(pSeg->pLeaf);
  pSeg->pLeaf = 0;
}

static void fts5IterClose(Fts5IndexIter *pIndexIter){
  if( pIndexIter ){
    Fts5Iter *pIter = (Fts5Iter*)pIndexIter;
    Fts5Index *pIndex = pIter->pIndex;
    fts5TokendataIterDelete(pIter->pTokenDataIter);
    fts5MultiIterFree(pIter);
    fts5IndexCloseReader(pIndex);
  }
}

/*
** This function appends iterator pAppend to Fts5TokenDataIter pIn and 
** returns the result.
*/
static Fts5TokenDataIter *fts5AppendTokendataIter(
  Fts5Index *p,                   /* Index object (for error code) */
  Fts5TokenDataIter *pIn,         /* Current Fts5TokenDataIter struct */
  Fts5Iter *pAppend               /* Append this iterator */
){
  Fts5TokenDataIter *pRet = pIn;

  if( p->rc==SQLITE_OK ){
    if( pIn==0 || pIn->nIter==pIn->nIterAlloc ){
      int nAlloc = pIn ? pIn->nIterAlloc*2 : 16;
      int nByte = SZ_FTS5TOKENDATAITER(nAlloc+1);
      Fts5TokenDataIter *pNew = (Fts5TokenDataIter*)sqlite3_realloc(pIn, nByte);

      if( pNew==0 ){
        p->rc = SQLITE_NOMEM;
      }else{
        if( pIn==0 ) memset(pNew, 0, nByte);
        pRet = pNew;
        pNew->nIterAlloc = nAlloc;
      }
    }
  }
  if( p->rc ){
    fts5IterClose((Fts5IndexIter*)pAppend);
  }else{
    pRet->apIter[pRet->nIter++] = pAppend;
  }
  assert( pRet==0 || pRet->nIter<=pRet->nIterAlloc );

  return pRet;
}

/*
** The iterator passed as the only argument must be a tokendata=1 iterator
** (pIter->pTokenDataIter!=0). This function sets the iterator output
** variables (pIter->base.*) according to the contents of the current
** row.
*/
static void fts5IterSetOutputsTokendata(Fts5Iter *pIter){
  int ii;
  int nHit = 0;
  i64 iRowid = SMALLEST_INT64;
  int iMin = 0;

  Fts5TokenDataIter *pT = pIter->pTokenDataIter;

  pIter->base.nData = 0;
  pIter->base.pData = 0;

  for(ii=0; ii<pT->nIter; ii++){
    Fts5Iter *p = pT->apIter[ii];
    if( p->base.bEof==0 ){
      if( nHit==0 || p->base.iRowid<iRowid ){
        iRowid = p->base.iRowid;
        nHit = 1;
        pIter->base.pData = p->base.pData;
        pIter->base.nData = p->base.nData;
        iMin = ii;
      }else if( p->base.iRowid==iRowid ){
        nHit++;
      }
    }
  }

  if( nHit==0 ){
    pIter->base.bEof = 1;
  }else{
    int eDetail = pIter->pIndex->pConfig->eDetail;
    pIter->base.bEof = 0;
    pIter->base.iRowid = iRowid;

    if( nHit==1 && eDetail==FTS5_DETAIL_FULL ){
      fts5TokendataIterAppendMap(pIter->pIndex, pT, iMin, 0, iRowid, -1);
    }else
    if( nHit>1 && eDetail!=FTS5_DETAIL_NONE ){
      int nReader = 0;
      int nByte = 0;
      i64 iPrev = 0;

      /* Allocate array of iterators if they are not already allocated. */
      if( pT->aPoslistReader==0 ){
        pT->aPoslistReader = (Fts5PoslistReader*)sqlite3Fts5MallocZero(
            &pIter->pIndex->rc,
            pT->nIter * (sizeof(Fts5PoslistReader) + sizeof(int))
        );
        if( pT->aPoslistReader==0 ) return;
        pT->aPoslistToIter = (int*)&pT->aPoslistReader[pT->nIter];
      }

      /* Populate an iterator for each poslist that will be merged */
      for(ii=0; ii<pT->nIter; ii++){
        Fts5Iter *p = pT->apIter[ii];
        if( iRowid==p->base.iRowid ){
          pT->aPoslistToIter[nReader] = ii;
          sqlite3Fts5PoslistReaderInit(
              p->base.pData, p->base.nData, &pT->aPoslistReader[nReader++]
          );
          nByte += p->base.nData;
        }
      }

      /* Ensure the output buffer is large enough */
      if( fts5BufferGrow(&pIter->pIndex->rc, &pIter->poslist, nByte+nHit*10) ){
        return;
      }

      /* Ensure the token-mapping is large enough */
      if( eDetail==FTS5_DETAIL_FULL && pT->nMapAlloc<(pT->nMap + nByte) ){
        int nNew = (pT->nMapAlloc + nByte) * 2;
        Fts5TokenDataMap *aNew = (Fts5TokenDataMap*)sqlite3_realloc(
            pT->aMap, nNew*sizeof(Fts5TokenDataMap)
        );
        if( aNew==0 ){
          pIter->pIndex->rc = SQLITE_NOMEM;
          return;
        }
        pT->aMap = aNew;
        pT->nMapAlloc = nNew;
      }

      pIter->poslist.n = 0;

      while( 1 ){
        i64 iMinPos = LARGEST_INT64;

        /* Find smallest position */
        iMin = 0;
        for(ii=0; ii<nReader; ii++){
          Fts5PoslistReader *pReader = &pT->aPoslistReader[ii];
          if( pReader->bEof==0 ){
            if( pReader->iPos<iMinPos ){
              iMinPos = pReader->iPos;
              iMin = ii;
            }
          }
        }

        /* If all readers were at EOF, break out of the loop. */
        if( iMinPos==LARGEST_INT64 ) break;

        sqlite3Fts5PoslistSafeAppend(&pIter->poslist, &iPrev, iMinPos);
        sqlite3Fts5PoslistReaderNext(&pT->aPoslistReader[iMin]);

        if( eDetail==FTS5_DETAIL_FULL ){
          pT->aMap[pT->nMap].iPos = iMinPos;
          pT->aMap[pT->nMap].iIter = pT->aPoslistToIter[iMin];
          pT->aMap[pT->nMap].iRowid = iRowid;
          pT->nMap++;
        }
      }

      pIter->base.pData = pIter->poslist.p;
      pIter->base.nData = pIter->poslist.n;
    }
  }
}

/*
** The iterator passed as the only argument must be a tokendata=1 iterator
** (pIter->pTokenDataIter!=0). This function advances the iterator. If
** argument bFrom is false, then the iterator is advanced to the next
** entry. Or, if bFrom is true, it is advanced to the first entry with
** a rowid of iFrom or greater.
*/
static void fts5TokendataIterNext(Fts5Iter *pIter, int bFrom, i64 iFrom){
  int ii;
  Fts5TokenDataIter *pT = pIter->pTokenDataIter;
  Fts5Index *pIndex = pIter->pIndex;

  for(ii=0; ii<pT->nIter; ii++){
    Fts5Iter *p = pT->apIter[ii];
    if( p->base.bEof==0 
     && (p->base.iRowid==pIter->base.iRowid || (bFrom && p->base.iRowid<iFrom))
    ){
      fts5MultiIterNext(pIndex, p, bFrom, iFrom);
      while( bFrom && p->base.bEof==0 
          && p->base.iRowid<iFrom 
          && pIndex->rc==SQLITE_OK 
      ){
        fts5MultiIterNext(pIndex, p, 0, 0);
      }
    }
  }

  if( pIndex->rc==SQLITE_OK ){
    fts5IterSetOutputsTokendata(pIter);
  }
}

/*
** If the segment-iterator passed as the first argument is at EOF, then
** set pIter->term to a copy of buffer pTerm.
*/
static void fts5TokendataSetTermIfEof(Fts5Iter *pIter, Fts5Buffer *pTerm){
  if( pIter && pIter->aSeg[0].pLeaf==0 ){
    fts5BufferSet(&pIter->pIndex->rc, &pIter->aSeg[0].term, pTerm->n, pTerm->p);
  }
}

/*
** This function sets up an iterator to use for a non-prefix query on a 
** tokendata=1 table. 
*/
static Fts5Iter *fts5SetupTokendataIter(
  Fts5Index *p,                   /* FTS index to query */
  const u8 *pToken,               /* Buffer containing query term */
  int nToken,                     /* Size of buffer pToken in bytes */
  Fts5Colset *pColset             /* Colset to filter on */
){
  Fts5Iter *pRet = 0;
  Fts5TokenDataIter *pSet = 0;
  Fts5Structure *pStruct = 0;
  const int flags = FTS5INDEX_QUERY_SCANONETERM | FTS5INDEX_QUERY_SCAN;

  Fts5Buffer bSeek = {0, 0, 0};
  Fts5Buffer *pSmall = 0;             

  fts5IndexFlush(p);
  pStruct = fts5StructureRead(p);

  while( p->rc==SQLITE_OK ){
    Fts5Iter *pPrev = pSet ? pSet->apIter[pSet->nIter-1] : 0;
    Fts5Iter *pNew = 0;
    Fts5SegIter *pNewIter = 0;
    Fts5SegIter *pPrevIter = 0;

    int iLvl, iSeg, ii;

    pNew = fts5MultiIterAlloc(p, pStruct->nSegment);
    if( pSmall ){
      fts5BufferSet(&p->rc, &bSeek, pSmall->n, pSmall->p);
      fts5BufferAppendBlob(&p->rc, &bSeek, 1, (const u8*)"\0");
    }else{
      fts5BufferSet(&p->rc, &bSeek, nToken, pToken);
    }
    if( p->rc ){
      fts5IterClose((Fts5IndexIter*)pNew);
      break;
    }

    pNewIter = &pNew->aSeg[0];
    pPrevIter = (pPrev ? &pPrev->aSeg[0] : 0);
    for(iLvl=0; iLvl<pStruct->nLevel; iLvl++){
      for(iSeg=pStruct->aLevel[iLvl].nSeg-1; iSeg>=0; iSeg--){
        Fts5StructureSegment *pSeg = &pStruct->aLevel[iLvl].aSeg[iSeg];
        int bDone = 0;

        if( pPrevIter ){
          if( fts5BufferCompare(pSmall, &pPrevIter->term) ){
            memcpy(pNewIter, pPrevIter, sizeof(Fts5SegIter));
            memset(pPrevIter, 0, sizeof(Fts5SegIter));
            bDone = 1;
          }else if( pPrevIter->iEndofDoclist>pPrevIter->pLeaf->szLeaf ){
            fts5SegIterNextInit(p,(const char*)bSeek.p,bSeek.n-1,pSeg,pNewIter);
            bDone = 1;
          }
        }

        if( bDone==0 ){
          fts5SegIterSeekInit(p, bSeek.p, bSeek.n, flags, pSeg, pNewIter);
        }

        if( pPrevIter ){
          if( pPrevIter->pTombArray ){
            pNewIter->pTombArray = pPrevIter->pTombArray;
            pNewIter->pTombArray->nRef++;
          }
        }else{
          fts5SegIterAllocTombstone(p, pNewIter);
        }

        pNewIter++;
        if( pPrevIter ) pPrevIter++;
        if( p->rc ) break;
      }
    }
    fts5TokendataSetTermIfEof(pPrev, pSmall);

    pNew->bSkipEmpty = 1;
    pNew->pColset = pColset;
    fts5IterSetOutputCb(&p->rc, pNew);

    /* Loop through all segments in the new iterator. Find the smallest 
    ** term that any segment-iterator points to. Iterator pNew will be
    ** used for this term. Also, set any iterator that points to a term that
    ** does not match pToken/nToken to point to EOF */
    pSmall = 0;
    for(ii=0; ii<pNew->nSeg; ii++){
      Fts5SegIter *pII = &pNew->aSeg[ii];
      if( 0==fts5IsTokendataPrefix(&pII->term, pToken, nToken) ){
        fts5SegIterSetEOF(pII);
      }
      if( pII->pLeaf && (!pSmall || fts5BufferCompare(pSmall, &pII->term)>0) ){
        pSmall = &pII->term;
      }
    }

    /* If pSmall is still NULL at this point, then the new iterator does
    ** not point to any terms that match the query. So delete it and break
    ** out of the loop - all required iterators have been collected.  */
    if( pSmall==0 ){
      fts5IterClose((Fts5IndexIter*)pNew);
      break;
    }

    /* Append this iterator to the set and continue. */
    pSet = fts5AppendTokendataIter(p, pSet, pNew);
  }

  if( p->rc==SQLITE_OK && pSet ){
    int ii;
    for(ii=0; ii<pSet->nIter; ii++){
      Fts5Iter *pIter = pSet->apIter[ii];
      int iSeg;
      for(iSeg=0; iSeg<pIter->nSeg; iSeg++){
        pIter->aSeg[iSeg].flags |= FTS5_SEGITER_ONETERM;
      }
      fts5MultiIterFinishSetup(p, pIter);
    }
  }
    
  if( p->rc==SQLITE_OK ){
    pRet = fts5MultiIterAlloc(p, 0);
  }
  if( pRet ){
    pRet->nSeg = 0;
    pRet->pTokenDataIter = pSet;
    if( pSet ){
      fts5IterSetOutputsTokendata(pRet);
    }else{
      pRet->base.bEof = 1;
    }
  }else{
    fts5TokendataIterDelete(pSet);
  }

  fts5StructureRelease(pStruct);
  fts5BufferFree(&bSeek);
  return pRet;
}

/*
** Open a new iterator to iterate though all rowid that match the 
** specified token or token prefix.
*/
int sqlite3Fts5IndexQuery(
  Fts5Index *p,                   /* FTS index to query */
  const char *pToken, int nToken, /* Token (or prefix) to query for */
  int flags,                      /* Mask of FTS5INDEX_QUERY_X flags */
  Fts5Colset *pColset,            /* Match these columns only */
  Fts5IndexIter **ppIter          /* OUT: New iterator object */
){
  Fts5Config *pConfig = p->pConfig;
  Fts5Iter *pRet = 0;
  Fts5Buffer buf = {0, 0, 0};

  /* If the QUERY_SCAN flag is set, all other flags must be clear. */
  assert( (flags & FTS5INDEX_QUERY_SCAN)==0 || flags==FTS5INDEX_QUERY_SCAN );

  if( sqlite3Fts5BufferSize(&p->rc, &buf, nToken+1)==0 ){
    int iIdx = 0;                 /* Index to search */
    int iPrefixIdx = 0;           /* +1 prefix index */
    int bTokendata = pConfig->bTokendata;
    assert( buf.p!=0 );
    if( nToken>0 ) memcpy(&buf.p[1], pToken, nToken);

    /* The NOTOKENDATA flag is set when each token in a tokendata=1 table
    ** should be treated individually, instead of merging all those with
    ** a common prefix into a single entry. This is used, for example, by
    ** queries performed as part of an integrity-check, or by the fts5vocab
    ** module.  */
    if( flags & (FTS5INDEX_QUERY_NOTOKENDATA|FTS5INDEX_QUERY_SCAN) ){
      bTokendata = 0;
    }

    /* Figure out which index to search and set iIdx accordingly. If this
    ** is a prefix query for which there is no prefix index, set iIdx to
    ** greater than pConfig->nPrefix to indicate that the query will be
    ** satisfied by scanning multiple terms in the main index.
    **
    ** If the QUERY_TEST_NOIDX flag was specified, then this must be a
    ** prefix-query. Instead of using a prefix-index (if one exists), 
    ** evaluate the prefix query using the main FTS index. This is used
    ** for internal sanity checking by the integrity-check in debug 
    ** mode only.  */
#ifdef SQLITE_DEBUG
    if( pConfig->bPrefixIndex==0 || (flags & FTS5INDEX_QUERY_TEST_NOIDX) ){
      assert( flags & FTS5INDEX_QUERY_PREFIX );
      iIdx = 1+pConfig->nPrefix;
    }else
#endif
    if( flags & FTS5INDEX_QUERY_PREFIX ){
      int nChar = fts5IndexCharlen(pToken, nToken);
      for(iIdx=1; iIdx<=pConfig->nPrefix; iIdx++){
        int nIdxChar = pConfig->aPrefix[iIdx-1];
        if( nIdxChar==nChar ) break;
        if( nIdxChar==nChar+1 ) iPrefixIdx = iIdx;
      }
    }

    if( bTokendata && iIdx==0 ){
      buf.p[0] = FTS5_MAIN_PREFIX;
      pRet = fts5SetupTokendataIter(p, buf.p, nToken+1, pColset);
    }else if( iIdx<=pConfig->nPrefix ){
      /* Straight index lookup */
      Fts5Structure *pStruct = fts5StructureRead(p);
      buf.p[0] = (u8)(FTS5_MAIN_PREFIX + iIdx);
      if( pStruct ){
        fts5MultiIterNew(p, pStruct, flags | FTS5INDEX_QUERY_SKIPEMPTY, 
            pColset, buf.p, nToken+1, -1, 0, &pRet
        );
        fts5StructureRelease(pStruct);
      }
    }else{
      /* Scan multiple terms in the main index for a prefix query. */
      int bDesc = (flags & FTS5INDEX_QUERY_DESC)!=0;
      fts5SetupPrefixIter(p, bDesc, iPrefixIdx, buf.p, nToken+1, pColset,&pRet);
      if( pRet==0 ){
        assert( p->rc!=SQLITE_OK );
      }else{
        assert( pRet->pColset==0 );
        fts5IterSetOutputCb(&p->rc, pRet);
        if( p->rc==SQLITE_OK ){
          Fts5SegIter *pSeg = &pRet->aSeg[pRet->aFirst[1].iFirst];
          if( pSeg->pLeaf ) pRet->xSetOutputs(pRet, pSeg);
        }
      }
    }

    if( p->rc ){
      fts5IterClose((Fts5IndexIter*)pRet);
      pRet = 0;
      fts5IndexCloseReader(p);
    }

    *ppIter = (Fts5IndexIter*)pRet;
    sqlite3Fts5BufferFree(&buf);
  }
  return fts5IndexReturn(p);
}

/*
** Return true if the iterator passed as the only argument is at EOF.
*/
/*
** Move to the next matching rowid. 
*/
int sqlite3Fts5IterNext(Fts5IndexIter *pIndexIter){
  Fts5Iter *pIter = (Fts5Iter*)pIndexIter;
  assert( pIter->pIndex->rc==SQLITE_OK );
  if( pIter->nSeg==0 ){
    assert( pIter->pTokenDataIter );
    fts5TokendataIterNext(pIter, 0, 0);
  }else{
    fts5MultiIterNext(pIter->pIndex, pIter, 0, 0);
  }
  return fts5IndexReturn(pIter->pIndex);
}

/*
** Move to the next matching term/rowid. Used by the fts5vocab module.
*/
int sqlite3Fts5IterNextScan(Fts5IndexIter *pIndexIter){
  Fts5Iter *pIter = (Fts5Iter*)pIndexIter;
  Fts5Index *p = pIter->pIndex;

  assert( pIter->pIndex->rc==SQLITE_OK );

  fts5MultiIterNext(p, pIter, 0, 0);
  if( p->rc==SQLITE_OK ){
    Fts5SegIter *pSeg = &pIter->aSeg[ pIter->aFirst[1].iFirst ];
    if( pSeg->pLeaf && pSeg->term.p[0]!=FTS5_MAIN_PREFIX ){
      fts5DataRelease(pSeg->pLeaf);
      pSeg->pLeaf = 0;
      pIter->base.bEof = 1;
    }
  }

  return fts5IndexReturn(pIter->pIndex);
}

/*
** Move to the next matching rowid that occurs at or after iMatch. The
** definition of "at or after" depends on whether this iterator iterates
** in ascending or descending rowid order.
*/
int sqlite3Fts5IterNextFrom(Fts5IndexIter *pIndexIter, i64 iMatch){
  Fts5Iter *pIter = (Fts5Iter*)pIndexIter;
  if( pIter->nSeg==0 ){
    assert( pIter->pTokenDataIter );
    fts5TokendataIterNext(pIter, 1, iMatch);
  }else{
    fts5MultiIterNextFrom(pIter->pIndex, pIter, iMatch);
  }
  return fts5IndexReturn(pIter->pIndex);
}

/*
** Return the current term.
*/
const char *sqlite3Fts5IterTerm(Fts5IndexIter *pIndexIter, int *pn){
  int n;
  const char *z = (const char*)fts5MultiIterTerm((Fts5Iter*)pIndexIter, &n);
  assert_nc( z || n<=1 );
  *pn = n-1;
  return (z ? &z[1] : 0);
}

/*
** pIter is a prefix query. This function populates pIter->pTokenDataIter
** with an Fts5TokenDataIter object containing mappings for all rows
** matched by the query.
*/
static int fts5SetupPrefixIterTokendata(
  Fts5Iter *pIter,
  const char *pToken,             /* Token prefix to search for */
  int nToken                      /* Size of pToken in bytes */
){
  Fts5Index *p = pIter->pIndex;
  Fts5Buffer token = {0, 0, 0};
  TokendataSetupCtx ctx;

  memset(&ctx, 0, sizeof(ctx));

  fts5BufferGrow(&p->rc, &token, nToken+1);
  assert( token.p!=0 || p->rc!=SQLITE_OK );
  ctx.pT = (Fts5TokenDataIter*)sqlite3Fts5MallocZero(&p->rc,
                                                   SZ_FTS5TOKENDATAITER(1));

  if( p->rc==SQLITE_OK ){

    /* Fill in the token prefix to search for */
    token.p[0] = FTS5_MAIN_PREFIX;
    memcpy(&token.p[1], pToken, nToken);
    token.n = nToken+1;

    fts5VisitEntries(
        p, 0, token.p, token.n, 1, prefixIterSetupTokendataCb, (void*)&ctx
    );

    fts5TokendataIterSortMap(p, ctx.pT);
  }

  if( p->rc==SQLITE_OK ){
    pIter->pTokenDataIter = ctx.pT;
  }else{
    fts5TokendataIterDelete(ctx.pT);
  }
  fts5BufferFree(&token);

  return fts5IndexReturn(p);
}

/*
** This is used by xInstToken() to access the token at offset iOff, column
** iCol of row iRowid. The token is returned via output variables *ppOut
** and *pnOut. The iterator passed as the first argument must be a tokendata=1
** iterator (pIter->pTokenDataIter!=0).
**
** pToken/nToken:
*/
int sqlite3Fts5IterToken(
  Fts5IndexIter *pIndexIter, 
  const char *pToken, int nToken,
  i64 iRowid,
  int iCol, 
  int iOff, 
  const char **ppOut, int *pnOut
){
  Fts5Iter *pIter = (Fts5Iter*)pIndexIter;
  Fts5TokenDataIter *pT = pIter->pTokenDataIter;
  i64 iPos = (((i64)iCol)<<32) + iOff;
  Fts5TokenDataMap *aMap = 0;
  int i1 = 0;
  int i2 = 0;
  int iTest = 0;

  assert( pT || (pToken && pIter->nSeg>0) );
  if( pT==0 ){
    int rc = fts5SetupPrefixIterTokendata(pIter, pToken, nToken);
    if( rc!=SQLITE_OK ) return rc;
    pT = pIter->pTokenDataIter;
  }

  i2 = pT->nMap;
  aMap = pT->aMap;

  while( i2>i1 ){
    iTest = (i1 + i2) / 2;

    if( aMap[iTest].iRowid<iRowid ){
      i1 = iTest+1;
    }else if( aMap[iTest].iRowid>iRowid ){
      i2 = iTest;
    }else{
      if( aMap[iTest].iPos<iPos ){
        if( aMap[iTest].iPos<0 ){
          break;
        }
        i1 = iTest+1;
      }else if( aMap[iTest].iPos>iPos ){
        i2 = iTest;
      }else{
        break;
      }
    }
  }

  if( i2>i1 ){
    if( pIter->nSeg==0 ){
      Fts5Iter *pMap = pT->apIter[aMap[iTest].iIter];
      *ppOut = (const char*)pMap->aSeg[0].term.p+1;
      *pnOut = pMap->aSeg[0].term.n-1;
    }else{
      Fts5TokenDataMap *p = &aMap[iTest];
      *ppOut = (const char*)&pT->terms.p[p->iIter];
      *pnOut = aMap[iTest].nByte;
    }
  }

  return SQLITE_OK;
}

/*
** Clear any existing entries from the token-map associated with the
** iterator passed as the only argument. 
*/
void sqlite3Fts5IndexIterClearTokendata(Fts5IndexIter *pIndexIter){
  Fts5Iter *pIter = (Fts5Iter*)pIndexIter;
  if( pIter && pIter->pTokenDataIter 
   && (pIter->nSeg==0 || pIter->pIndex->pConfig->eDetail!=FTS5_DETAIL_FULL)
  ){
    pIter->pTokenDataIter->nMap = 0;
  }
}

/*
** Set a token-mapping for the iterator passed as the first argument. This
** is used in detail=column or detail=none mode when a token is requested
** using the xInstToken() API. In this case the caller tokenizers the
** current row and configures the token-mapping via multiple calls to this
** function.
*/
int sqlite3Fts5IndexIterWriteTokendata(
  Fts5IndexIter *pIndexIter, 
  const char *pToken, int nToken, 
  i64 iRowid, int iCol, int iOff
){
  Fts5Iter *pIter = (Fts5Iter*)pIndexIter;
  Fts5TokenDataIter *pT = pIter->pTokenDataIter;
  Fts5Index *p = pIter->pIndex;
  i64 iPos = (((i64)iCol)<<32) + iOff;

  assert( p->pConfig->eDetail!=FTS5_DETAIL_FULL );
  assert( pIter->pTokenDataIter || pIter->nSeg>0 );
  if( pIter->nSeg>0 ){
    /* This is a prefix term iterator. */
    if( pT==0 ){
      pT = (Fts5TokenDataIter*)sqlite3Fts5MallocZero(&p->rc,
                                           SZ_FTS5TOKENDATAITER(1));
      pIter->pTokenDataIter = pT;
    }
    if( pT ){
      fts5TokendataIterAppendMap(p, pT, pT->terms.n, nToken, iRowid, iPos);
      fts5BufferAppendBlob(&p->rc, &pT->terms, nToken, (const u8*)pToken);
    }
  }else{
    int ii;
    for(ii=0; ii<pT->nIter; ii++){
      Fts5Buffer *pTerm = &pT->apIter[ii]->aSeg[0].term;
      if( nToken==pTerm->n-1 && memcmp(pToken, pTerm->p+1, nToken)==0 ) break;
    }
    if( ii<pT->nIter ){
      fts5TokendataIterAppendMap(p, pT, ii, 0, iRowid, iPos);
    }
  }
  return fts5IndexReturn(p);
}

/*
** Close an iterator opened by an earlier call to sqlite3Fts5IndexQuery().
*/
void sqlite3Fts5IterClose(Fts5IndexIter *pIndexIter){
  if( pIndexIter ){
    Fts5Index *pIndex = ((Fts5Iter*)pIndexIter)->pIndex;
    fts5IterClose(pIndexIter);
    fts5IndexReturn(pIndex);
  }
}

/*
** Read and decode the "averages" record from the database. 
**
** Parameter anSize must point to an array of size nCol, where nCol is
** the number of user defined columns in the FTS table.
*/
int sqlite3Fts5IndexGetAverages(Fts5Index *p, i64 *pnRow, i64 *anSize){
  int nCol = p->pConfig->nCol;
  Fts5Data *pData;

  *pnRow = 0;
  memset(anSize, 0, sizeof(i64) * nCol);
  pData = fts5DataRead(p, FTS5_AVERAGES_ROWID);
  if( p->rc==SQLITE_OK && pData->nn ){
    int i = 0;
    int iCol;
    i += fts5GetVarint(&pData->p[i], (u64*)pnRow);
    for(iCol=0; i<pData->nn && iCol<nCol; iCol++){
      i += fts5GetVarint(&pData->p[i], (u64*)&anSize[iCol]);
    }
  }

  fts5DataRelease(pData);
  return fts5IndexReturn(p);
}

/*
** Replace the current "averages" record with the contents of the buffer 
** supplied as the second argument.
*/
int sqlite3Fts5IndexSetAverages(Fts5Index *p, const u8 *pData, int nData){
  assert( p->rc==SQLITE_OK );
  fts5DataWrite(p, FTS5_AVERAGES_ROWID, pData, nData);
  return fts5IndexReturn(p);
}

/*
** Return the total number of blocks this module has read from the %_data
** table since it was created.
*/
int sqlite3Fts5IndexReads(Fts5Index *p){
  return p->nRead;
}

/*
** Set the 32-bit cookie value stored at the start of all structure 
** records to the value passed as the second argument.
**
** Return SQLITE_OK if successful, or an SQLite error code if an error
** occurs.
*/
int sqlite3Fts5IndexSetCookie(Fts5Index *p, int iNew){
  int rc;                              /* Return code */
  Fts5Config *pConfig = p->pConfig;    /* Configuration object */
  u8 aCookie[4];                       /* Binary representation of iNew */
  sqlite3_blob *pBlob = 0;

  assert( p->rc==SQLITE_OK );
  sqlite3Fts5Put32(aCookie, iNew);

  rc = sqlite3_blob_open(pConfig->db, pConfig->zDb, p->zDataTbl, 
      "block", FTS5_STRUCTURE_ROWID, 1, &pBlob
  );
  if( rc==SQLITE_OK ){
    sqlite3_blob_write(pBlob, aCookie, 4, 0);
    rc = sqlite3_blob_close(pBlob);
  }

  return rc;
}

int sqlite3Fts5IndexLoadConfig(Fts5Index *p){
  Fts5Structure *pStruct;
  pStruct = fts5StructureRead(p);
  fts5StructureRelease(pStruct);
  return fts5IndexReturn(p);
}

/*
** Retrieve the origin value that will be used for the segment currently
** being accumulated in the in-memory hash table when it is flushed to
** disk. If successful, SQLITE_OK is returned and (*piOrigin) set to
** the queried value. Or, if an error occurs, an error code is returned
** and the final value of (*piOrigin) is undefined.
*/
int sqlite3Fts5IndexGetOrigin(Fts5Index *p, i64 *piOrigin){
  Fts5Structure *pStruct;
  pStruct = fts5StructureRead(p);
  if( pStruct ){
    *piOrigin = pStruct->nOriginCntr;
    fts5StructureRelease(pStruct);
  }
  return fts5IndexReturn(p);
}

/*
** Buffer pPg contains a page of a tombstone hash table - one of nPg pages
** associated with the same segment. This function adds rowid iRowid to
** the hash table. The caller is required to guarantee that there is at
** least one free slot on the page.
**
** If parameter bForce is false and the hash table is deemed to be full
** (more than half of the slots are occupied), then non-zero is returned
** and iRowid not inserted. Or, if bForce is true or if the hash table page
** is not full, iRowid is inserted and zero returned.
*/
static int fts5IndexTombstoneAddToPage(
  Fts5Data *pPg, 
  int bForce,
  int nPg, 
  u64 iRowid
){
  const int szKey = TOMBSTONE_KEYSIZE(pPg);
  const int nSlot = TOMBSTONE_NSLOT(pPg);
  const int nElem = fts5GetU32(&pPg->p[4]);
  int iSlot = (iRowid / nPg) % nSlot;
  int nCollide = nSlot;

  if( szKey==4 && iRowid>0xFFFFFFFF ) return 2;
  if( iRowid==0 ){
    pPg->p[1] = 0x01;
    return 0;
  }

  if( bForce==0 && nElem>=(nSlot/2) ){
    return 1;
  }

  fts5PutU32(&pPg->p[4], nElem+1);
  if( szKey==4 ){
    u32 *aSlot = (u32*)&pPg->p[8];
    while( aSlot[iSlot] ){
      iSlot = (iSlot + 1) % nSlot;
      if( nCollide--==0 ) return 0;
    }
    fts5PutU32((u8*)&aSlot[iSlot], (u32)iRowid);
  }else{
    u64 *aSlot = (u64*)&pPg->p[8];
    while( aSlot[iSlot] ){
      iSlot = (iSlot + 1) % nSlot;
      if( nCollide--==0 ) return 0;
    }
    fts5PutU64((u8*)&aSlot[iSlot], iRowid);
  }

  return 0;
}

/*
** This function attempts to build a new hash containing all the keys 
** currently in the tombstone hash table for segment pSeg. The new
** hash will be stored in the nOut buffers passed in array apOut[].
** All pages of the new hash use key-size szKey (4 or 8).
**
** Return 0 if the hash is successfully rebuilt into the nOut pages. 
** Or non-zero if it is not (because one page became overfull). In this 
** case the caller should retry with a larger nOut parameter.
**
** Parameter pData1 is page iPg1 of the hash table being rebuilt.
*/
static int fts5IndexTombstoneRehash(
  Fts5Index *p,
  Fts5StructureSegment *pSeg,     /* Segment to rebuild hash of */
  Fts5Data *pData1,               /* One page of current hash - or NULL */
  int iPg1,                       /* Which page of the current hash is pData1 */
  int szKey,                      /* 4 or 8, the keysize */
  int nOut,                       /* Number of output pages */
  Fts5Data **apOut                /* Array of output hash pages */
){
  int ii;
  int res = 0;

  /* Initialize the headers of all the output pages */
  for(ii=0; ii<nOut; ii++){
    apOut[ii]->p[0] = szKey;
    fts5PutU32(&apOut[ii]->p[4], 0);
  }

  /* Loop through the current pages of the hash table. */ 
  for(ii=0; res==0 && ii<pSeg->nPgTombstone; ii++){
    Fts5Data *pData = 0;          /* Page ii of the current hash table */
    Fts5Data *pFree = 0;          /* Free this at the end of the loop */

    if( iPg1==ii ){
      pData = pData1;
    }else{
      pFree = pData = fts5DataRead(p, FTS5_TOMBSTONE_ROWID(pSeg->iSegid, ii));
    }

    if( pData ){
      int szKeyIn = TOMBSTONE_KEYSIZE(pData);
      int nSlotIn = (pData->nn - 8) / szKeyIn;
      int iIn;
      for(iIn=0; iIn<nSlotIn; iIn++){
        u64 iVal = 0;

        /* Read the value from slot iIn of the input page into iVal. */
        if( szKeyIn==4 ){
          u32 *aSlot = (u32*)&pData->p[8];
          if( aSlot[iIn] ) iVal = fts5GetU32((u8*)&aSlot[iIn]);
        }else{
          u64 *aSlot = (u64*)&pData->p[8];
          if( aSlot[iIn] ) iVal = fts5GetU64((u8*)&aSlot[iIn]);
        }

        /* If iVal is not 0 at this point, insert it into the new hash table */
        if( iVal ){
          Fts5Data *pPg = apOut[(iVal % nOut)];
          res = fts5IndexTombstoneAddToPage(pPg, 0, nOut, iVal);
          if( res ) break;
        }
      }

      /* If this is page 0 of the old hash, copy the rowid-0-flag from the
      ** old hash to the new.  */
      if( ii==0 ){
        apOut[0]->p[1] = pData->p[1];
      }
    }
    fts5DataRelease(pFree);
  }

  return res;
}

/*
** This is called to rebuild the hash table belonging to segment pSeg.
** If parameter pData1 is not NULL, then one page of the existing hash table
** has already been loaded - pData1, which is page iPg1. The key-size for
** the new hash table is szKey (4 or 8).
**
** If successful, the new hash table is not written to disk. Instead, 
** output parameter (*pnOut) is set to the number of pages in the new
** hash table, and (*papOut) to point to an array of buffers containing
** the new page data.
**
** If an error occurs, an error code is left in the Fts5Index object and
** both output parameters set to 0 before returning.
*/
static void fts5IndexTombstoneRebuild(
  Fts5Index *p,
  Fts5StructureSegment *pSeg,     /* Segment to rebuild hash of */
  Fts5Data *pData1,               /* One page of current hash - or NULL */
  int iPg1,                       /* Which page of the current hash is pData1 */
  int szKey,                      /* 4 or 8, the keysize */
  int *pnOut,                     /* OUT: Number of output pages */
  Fts5Data ***papOut              /* OUT: Output hash pages */
){
  const int MINSLOT = 32;
  int nSlotPerPage = MAX(MINSLOT, (p->pConfig->pgsz - 8) / szKey);
  int nSlot = 0;                  /* Number of slots in each output page */
  int nOut = 0;

  /* Figure out how many output pages (nOut) and how many slots per 
  ** page (nSlot).  There are three possibilities: 
  **
  **   1. The hash table does not yet exist. In this case the new hash
  **      table will consist of a single page with MINSLOT slots.
  **
  **   2. The hash table exists but is currently a single page. In this
  **      case an attempt is made to grow the page to accommodate the new
  **      entry. The page is allowed to grow up to nSlotPerPage (see above)
  **      slots.
  **
  **   3. The hash table already consists of more than one page, or of
  **      a single page already so large that it cannot be grown. In this
  **      case the new hash consists of (nPg*2+1) pages of nSlotPerPage
  **      slots each, where nPg is the current number of pages in the 
  **      hash table.
  */
  if( pSeg->nPgTombstone==0 ){
    /* Case 1. */
    nOut = 1;
    nSlot = MINSLOT;
  }else if( pSeg->nPgTombstone==1 ){
    /* Case 2. */
    int nElem = (int)fts5GetU32(&pData1->p[4]);
    assert( pData1 && iPg1==0 );
    nOut = 1;
    nSlot = MAX(nElem*4, MINSLOT);
    if( nSlot>nSlotPerPage ) nOut = 0; 
  }
  if( nOut==0 ){
    /* Case 3. */
    nOut = (pSeg->nPgTombstone * 2 + 1);
    nSlot = nSlotPerPage;
  }

  /* Allocate the required array and output pages */
  while( 1 ){
    int res = 0;
    int ii = 0;
    int szPage = 0;
    Fts5Data **apOut = 0;

    /* Allocate space for the new hash table */
    assert( nSlot>=MINSLOT );
    apOut = (Fts5Data**)sqlite3Fts5MallocZero(&p->rc, sizeof(Fts5Data*) * nOut);
    szPage = 8 + nSlot*szKey;
    for(ii=0; ii<nOut; ii++){
      Fts5Data *pNew = (Fts5Data*)sqlite3Fts5MallocZero(&p->rc, 
          sizeof(Fts5Data)+szPage
      );
      if( pNew ){
        pNew->nn = szPage;
        pNew->p = (u8*)&pNew[1];
        apOut[ii] = pNew;
      }
    }

    /* Rebuild the hash table. */
    if( p->rc==SQLITE_OK ){
      res = fts5IndexTombstoneRehash(p, pSeg, pData1, iPg1, szKey, nOut, apOut);
    }
    if( res==0 ){
      if( p->rc ){
        fts5IndexFreeArray(apOut, nOut);
        apOut = 0;
        nOut = 0;
      }
      *pnOut = nOut;
      *papOut = apOut;
      break;
    }
    
    /* If control flows to here, it was not possible to rebuild the hash
    ** table. Free all buffers and then try again with more pages. */
    assert( p->rc==SQLITE_OK );
    fts5IndexFreeArray(apOut, nOut);
    nSlot = nSlotPerPage;
    nOut = nOut*2 + 1;
  }
}


/*
** Add a tombstone for rowid iRowid to segment pSeg.
*/
static void fts5IndexTombstoneAdd(
  Fts5Index *p, 
  Fts5StructureSegment *pSeg, 
  u64 iRowid
){
  Fts5Data *pPg = 0;
  int iPg = -1;
  int szKey = 0;
  int nHash = 0;
  Fts5Data **apHash = 0;

  p->nContentlessDelete++;

  if( pSeg->nPgTombstone>0 ){
    iPg = iRowid % pSeg->nPgTombstone;
    pPg = fts5DataRead(p, FTS5_TOMBSTONE_ROWID(pSeg->iSegid,iPg));
    if( pPg==0 ){
      assert( p->rc!=SQLITE_OK );
      return;
    }

    if( 0==fts5IndexTombstoneAddToPage(pPg, 0, pSeg->nPgTombstone, iRowid) ){
      fts5DataWrite(p, FTS5_TOMBSTONE_ROWID(pSeg->iSegid,iPg), pPg->p, pPg->nn);
      fts5DataRelease(pPg);
      return;
    }
  }

  /* Have to rebuild the hash table. First figure out the key-size (4 or 8). */
  szKey = pPg ? TOMBSTONE_KEYSIZE(pPg) : 4;
  if( iRowid>0xFFFFFFFF ) szKey = 8;

  /* Rebuild the hash table */
  fts5IndexTombstoneRebuild(p, pSeg, pPg, iPg, szKey, &nHash, &apHash);
  assert( p->rc==SQLITE_OK || (nHash==0 && apHash==0) );

  /* If all has succeeded, write the new rowid into one of the new hash
  ** table pages, then write them all out to disk. */
  if( nHash ){
    int ii = 0;
    fts5IndexTombstoneAddToPage(apHash[iRowid % nHash], 1, nHash, iRowid);
    for(ii=0; ii<nHash; ii++){
      i64 iTombstoneRowid = FTS5_TOMBSTONE_ROWID(pSeg->iSegid, ii);
      fts5DataWrite(p, iTombstoneRowid, apHash[ii]->p, apHash[ii]->nn);
    }
    pSeg->nPgTombstone = nHash;
    fts5StructureWrite(p, p->pStruct);
  }

  fts5DataRelease(pPg);
  fts5IndexFreeArray(apHash, nHash);
}

/*
** Add iRowid to the tombstone list of the segment or segments that contain
** rows from origin iOrigin. Return SQLITE_OK if successful, or an SQLite
** error code otherwise.
*/
int sqlite3Fts5IndexContentlessDelete(Fts5Index *p, i64 iOrigin, i64 iRowid){
  Fts5Structure *pStruct;
  pStruct = fts5StructureRead(p);
  if( pStruct ){
    int bFound = 0;               /* True after pSeg->nEntryTombstone incr. */
    int iLvl;
    for(iLvl=pStruct->nLevel-1; iLvl>=0; iLvl--){
      int iSeg;
      for(iSeg=pStruct->aLevel[iLvl].nSeg-1; iSeg>=0; iSeg--){
        Fts5StructureSegment *pSeg = &pStruct->aLevel[iLvl].aSeg[iSeg];
        if( pSeg->iOrigin1<=(u64)iOrigin && pSeg->iOrigin2>=(u64)iOrigin ){
          if( bFound==0 ){
            pSeg->nEntryTombstone++;
            bFound = 1;
          }
          fts5IndexTombstoneAdd(p, pSeg, iRowid);
        }
      }
    }
    fts5StructureRelease(pStruct);
  }
  return fts5IndexReturn(p);
}

/*************************************************************************
**************************************************************************
** Below this point is the implementation of the integrity-check 
** functionality.
*/

/*
** Return a simple checksum value based on the arguments.
*/
u64 sqlite3Fts5IndexEntryCksum(
  i64 iRowid, 
  int iCol, 
  int iPos, 
  int iIdx,
  const char *pTerm,
  int nTerm
){
  int i;
  u64 ret = iRowid;
  ret += (ret<<3) + iCol;
  ret += (ret<<3) + iPos;
  if( iIdx>=0 ) ret += (ret<<3) + (FTS5_MAIN_PREFIX + iIdx);
  for(i=0; i<nTerm; i++) ret += (ret<<3) + pTerm[i];
  return ret;
}

#ifdef SQLITE_DEBUG
/*
** This function is purely an internal test. It does not contribute to 
** FTS functionality, or even the integrity-check, in any way.
**
** Instead, it tests that the same set of pgno/rowid combinations are 
** visited regardless of whether the doclist-index identified by parameters
** iSegid/iLeaf is iterated in forwards or reverse order.
*/
static void fts5TestDlidxReverse(
  Fts5Index *p, 
  int iSegid,                     /* Segment id to load from */
  int iLeaf                       /* Load doclist-index for this leaf */
){
  Fts5DlidxIter *pDlidx = 0;
  u64 cksum1 = 13;
  u64 cksum2 = 13;

  for(pDlidx=fts5DlidxIterInit(p, 0, iSegid, iLeaf);
      fts5DlidxIterEof(p, pDlidx)==0;
      fts5DlidxIterNext(p, pDlidx)
  ){
    i64 iRowid = fts5DlidxIterRowid(pDlidx);
    int pgno = fts5DlidxIterPgno(pDlidx);
    assert( pgno>iLeaf );
    cksum1 += iRowid + ((i64)pgno<<32);
  }
  fts5DlidxIterFree(pDlidx);
  pDlidx = 0;

  for(pDlidx=fts5DlidxIterInit(p, 1, iSegid, iLeaf);
      fts5DlidxIterEof(p, pDlidx)==0;
      fts5DlidxIterPrev(p, pDlidx)
  ){
    i64 iRowid = fts5DlidxIterRowid(pDlidx);
    int pgno = fts5DlidxIterPgno(pDlidx);
    assert( fts5DlidxIterPgno(pDlidx)>iLeaf );
    cksum2 += iRowid + ((i64)pgno<<32);
  }
  fts5DlidxIterFree(pDlidx);
  pDlidx = 0;

  if( p->rc==SQLITE_OK && cksum1!=cksum2 ) p->rc = FTS5_CORRUPT;
}

static int fts5QueryCksum(
  Fts5Index *p,                   /* Fts5 index object */
  int iIdx,
  const char *z,                  /* Index key to query for */
  int n,                          /* Size of index key in bytes */
  int flags,                      /* Flags for Fts5IndexQuery */
  u64 *pCksum                     /* IN/OUT: Checksum value */
){
  int eDetail = p->pConfig->eDetail;
  u64 cksum = *pCksum;
  Fts5IndexIter *pIter = 0;
  int rc = sqlite3Fts5IndexQuery(
      p, z, n, (flags | FTS5INDEX_QUERY_NOTOKENDATA), 0, &pIter
  );

  while( rc==SQLITE_OK && ALWAYS(pIter!=0) && 0==sqlite3Fts5IterEof(pIter) ){
    i64 rowid = pIter->iRowid;

    if( eDetail==FTS5_DETAIL_NONE ){
      cksum ^= sqlite3Fts5IndexEntryCksum(rowid, 0, 0, iIdx, z, n);
    }else{
      Fts5PoslistReader sReader;
      for(sqlite3Fts5PoslistReaderInit(pIter->pData, pIter->nData, &sReader);
          sReader.bEof==0;
          sqlite3Fts5PoslistReaderNext(&sReader)
      ){
        int iCol = FTS5_POS2COLUMN(sReader.iPos);
        int iOff = FTS5_POS2OFFSET(sReader.iPos);
        cksum ^= sqlite3Fts5IndexEntryCksum(rowid, iCol, iOff, iIdx, z, n);
      }
    }
    if( rc==SQLITE_OK ){
      rc = sqlite3Fts5IterNext(pIter);
    }
  }
  fts5IterClose(pIter);

  *pCksum = cksum;
  return rc;
}

/*
** Check if buffer z[], size n bytes, contains as series of valid utf-8
** encoded codepoints. If so, return 0. Otherwise, if the buffer does not
** contain valid utf-8, return non-zero.
*/
static int fts5TestUtf8(const char *z, int n){
  int i = 0;
  assert_nc( n>0 );
  while( i<n ){
    if( (z[i] & 0x80)==0x00 ){
      i++;
    }else
    if( (z[i] & 0xE0)==0xC0 ){
      if( i+1>=n || (z[i+1] & 0xC0)!=0x80 ) return 1;
      i += 2;
    }else
    if( (z[i] & 0xF0)==0xE0 ){
      if( i+2>=n || (z[i+1] & 0xC0)!=0x80 || (z[i+2] & 0xC0)!=0x80 ) return 1;
      i += 3;
    }else
    if( (z[i] & 0xF8)==0xF0 ){
      if( i+3>=n || (z[i+1] & 0xC0)!=0x80 || (z[i+2] & 0xC0)!=0x80 ) return 1;
      if( (z[i+2] & 0xC0)!=0x80 ) return 1;
      i += 3;
    }else{
      return 1;
    }
  }

  return 0;
}

/*
** This function is also purely an internal test. It does not contribute to 
** FTS functionality, or even the integrity-check, in any way.
**
** This function sets output variable (*pbFail) to true if the test fails. Or
** leaves it unchanged if the test succeeds.
*/
static void fts5TestTerm(
  Fts5Index *p, 
  Fts5Buffer *pPrev,              /* Previous term */
  const char *z, int n,           /* Possibly new term to test */
  u64 expected,
  u64 *pCksum,
  int *pbFail
){
  int rc = p->rc;
  if( pPrev->n==0 ){
    fts5BufferSet(&rc, pPrev, n, (const u8*)z);
  }else
  if( *pbFail==0 
   && rc==SQLITE_OK 
   && (pPrev->n!=n || memcmp(pPrev->p, z, n)) 
   && (p->pHash==0 || p->pHash->nEntry==0)
  ){
    u64 cksum3 = *pCksum;
    const char *zTerm = (const char*)&pPrev->p[1];  /* term sans prefix-byte */
    int nTerm = pPrev->n-1;            /* Size of zTerm in bytes */
    int iIdx = (pPrev->p[0] - FTS5_MAIN_PREFIX);
    int flags = (iIdx==0 ? 0 : FTS5INDEX_QUERY_PREFIX);
    u64 ck1 = 0;
    u64 ck2 = 0;

    /* Check that the results returned for ASC and DESC queries are
    ** the same. If not, call this corruption.  */
    rc = fts5QueryCksum(p, iIdx, zTerm, nTerm, flags, &ck1);
    if( rc==SQLITE_OK ){
      int f = flags|FTS5INDEX_QUERY_DESC;
      rc = fts5QueryCksum(p, iIdx, zTerm, nTerm, f, &ck2);
    }
    if( rc==SQLITE_OK && ck1!=ck2 ) rc = FTS5_CORRUPT;

    /* If this is a prefix query, check that the results returned if the
    ** the index is disabled are the same. In both ASC and DESC order. 
    **
    ** This check may only be performed if the hash table is empty. This
    ** is because the hash table only supports a single scan query at
    ** a time, and the multi-iter loop from which this function is called
    ** is already performing such a scan. 
    **
    ** Also only do this if buffer zTerm contains nTerm bytes of valid
    ** utf-8. Otherwise, the last part of the buffer contents might contain
    ** a non-utf-8 sequence that happens to be a prefix of a valid utf-8
    ** character stored in the main fts index, which will cause the
    ** test to fail.  */
    if( p->nPendingData==0 && 0==fts5TestUtf8(zTerm, nTerm) ){
      if( iIdx>0 && rc==SQLITE_OK ){
        int f = flags|FTS5INDEX_QUERY_TEST_NOIDX;
        ck2 = 0;
        rc = fts5QueryCksum(p, iIdx, zTerm, nTerm, f, &ck2);
        if( rc==SQLITE_OK && ck1!=ck2 ) rc = FTS5_CORRUPT;
      }
      if( iIdx>0 && rc==SQLITE_OK ){
        int f = flags|FTS5INDEX_QUERY_TEST_NOIDX|FTS5INDEX_QUERY_DESC;
        ck2 = 0;
        rc = fts5QueryCksum(p, iIdx, zTerm, nTerm, f, &ck2);
        if( rc==SQLITE_OK && ck1!=ck2 ) rc = FTS5_CORRUPT;
      }
    }

    cksum3 ^= ck1;
    fts5BufferSet(&rc, pPrev, n, (const u8*)z);

    if( rc==SQLITE_OK && cksum3!=expected ){
      *pbFail = 1;
    }
    *pCksum = cksum3;
  }
  p->rc = rc;
}
 
#else
# define fts5TestDlidxReverse(x,y,z)
# define fts5TestTerm(t,u,v,w,x,y,z)
#endif

/*
** Check that:
**
**   1) All leaves of pSeg between iFirst and iLast (inclusive) exist and
**      contain zero terms.
**   2) All leaves of pSeg between iNoRowid and iLast (inclusive) exist and
**      contain zero rowids.
*/
static void fts5IndexIntegrityCheckEmpty(
  Fts5Index *p,
  Fts5StructureSegment *pSeg,     /* Segment to check internal consistency */
  int iFirst,
  int iNoRowid,
  int iLast
){
  int i;

  /* Now check that the iter.nEmpty leaves following the current leaf
  ** (a) exist and (b) contain no terms. */
  for(i=iFirst; p->rc==SQLITE_OK && i<=iLast; i++){
    Fts5Data *pLeaf = fts5DataRead(p, FTS5_SEGMENT_ROWID(pSeg->iSegid, i));
    if( pLeaf ){
      if( !fts5LeafIsTermless(pLeaf)
       || (i>=iNoRowid && 0!=fts5LeafFirstRowidOff(pLeaf))
      ){
        FTS5_CORRUPT_ROWID(p, FTS5_SEGMENT_ROWID(pSeg->iSegid, i));
      }
    }
    fts5DataRelease(pLeaf);
  }
}

static void fts5IntegrityCheckPgidx(Fts5Index *p, i64 iRowid, Fts5Data *pLeaf){
  i64 iTermOff = 0;
  int ii;

  Fts5Buffer buf1 = {0,0,0};
  Fts5Buffer buf2 = {0,0,0};

  ii = pLeaf->szLeaf;
  while( ii<pLeaf->nn && p->rc==SQLITE_OK ){
    int res;
    i64 iOff;
    int nIncr;

    ii += fts5GetVarint32(&pLeaf->p[ii], nIncr);
    iTermOff += nIncr;
    iOff = iTermOff;

    if( iOff>=pLeaf->szLeaf ){
      FTS5_CORRUPT_ROWID(p, iRowid);
    }else if( iTermOff==nIncr ){
      int nByte;
      iOff += fts5GetVarint32(&pLeaf->p[iOff], nByte);
      if( (iOff+nByte)>pLeaf->szLeaf ){
        FTS5_CORRUPT_ROWID(p, iRowid);
      }else{
        fts5BufferSet(&p->rc, &buf1, nByte, &pLeaf->p[iOff]);
      }
    }else{
      int nKeep, nByte;
      iOff += fts5GetVarint32(&pLeaf->p[iOff], nKeep);
      iOff += fts5GetVarint32(&pLeaf->p[iOff], nByte);
      if( nKeep>buf1.n || (iOff+nByte)>pLeaf->szLeaf ){
        FTS5_CORRUPT_ROWID(p, iRowid);
      }else{
        buf1.n = nKeep;
        fts5BufferAppendBlob(&p->rc, &buf1, nByte, &pLeaf->p[iOff]);
      }

      if( p->rc==SQLITE_OK ){
        res = fts5BufferCompare(&buf1, &buf2);
        if( res<=0 ) FTS5_CORRUPT_ROWID(p, iRowid);
      }
    }
    fts5BufferSet(&p->rc, &buf2, buf1.n, buf1.p);
  }

  fts5BufferFree(&buf1);
  fts5BufferFree(&buf2);
}

static void fts5IndexIntegrityCheckSegment(
  Fts5Index *p,                   /* FTS5 backend object */
  Fts5StructureSegment *pSeg      /* Segment to check internal consistency */
){
  Fts5Config *pConfig = p->pConfig;
  int bSecureDelete = (pConfig->iVersion==FTS5_CURRENT_VERSION_SECUREDELETE);
  sqlite3_stmt *pStmt = 0;
  int rc2;
  int iIdxPrevLeaf = pSeg->pgnoFirst-1;
  int iDlidxPrevLeaf = pSeg->pgnoLast;

  if( pSeg->pgnoFirst==0 ) return;

  fts5IndexPrepareStmt(p, &pStmt, sqlite3_mprintf(
      "SELECT segid, term, (pgno>>1), (pgno&1) FROM %Q.'%q_idx' WHERE segid=%d "
      "ORDER BY 1, 2",
      pConfig->zDb, pConfig->zName, pSeg->iSegid
  ));

  /* Iterate through the b-tree hierarchy.  */
  while( p->rc==SQLITE_OK && SQLITE_ROW==sqlite3_step(pStmt) ){
    i64 iRow;                     /* Rowid for this leaf */
    Fts5Data *pLeaf;              /* Data for this leaf */

    const char *zIdxTerm = (const char*)sqlite3_column_blob(pStmt, 1);
    int nIdxTerm = sqlite3_column_bytes(pStmt, 1);
    int iIdxLeaf = sqlite3_column_int(pStmt, 2);
    int bIdxDlidx = sqlite3_column_int(pStmt, 3);

    /* If the leaf in question has already been trimmed from the segment, 
    ** ignore this b-tree entry. Otherwise, load it into memory. */
    if( iIdxLeaf<pSeg->pgnoFirst ) continue;
    iRow = FTS5_SEGMENT_ROWID(pSeg->iSegid, iIdxLeaf);
    pLeaf = fts5LeafRead(p, iRow);
    if( pLeaf==0 ) break;

    /* Check that the leaf contains at least one term, and that it is equal
    ** to or larger than the split-key in zIdxTerm.  Also check that if there
    ** is also a rowid pointer within the leaf page header, it points to a
    ** location before the term.  */
    if( pLeaf->nn<=pLeaf->szLeaf ){

      if( nIdxTerm==0 
       && pConfig->iVersion==FTS5_CURRENT_VERSION_SECUREDELETE
       && pLeaf->nn==pLeaf->szLeaf
       && pLeaf->nn==4
      ){
        /* special case - the very first page in a segment keeps its %_idx
        ** entry even if all the terms are removed from it by secure-delete 
        ** operations. */
      }else{
        FTS5_CORRUPT_ROWID(p, iRow);
      }

    }else{
      int iOff;                   /* Offset of first term on leaf */
      int iRowidOff;              /* Offset of first rowid on leaf */
      int nTerm;                  /* Size of term on leaf in bytes */
      int res;                    /* Comparison of term and split-key */

      iOff = fts5LeafFirstTermOff(pLeaf);
      iRowidOff = fts5LeafFirstRowidOff(pLeaf);
      if( iRowidOff>=iOff || iOff>=pLeaf->szLeaf ){
        FTS5_CORRUPT_ROWID(p, iRow);
      }else{
        iOff += fts5GetVarint32(&pLeaf->p[iOff], nTerm);
        res = fts5Memcmp(&pLeaf->p[iOff], zIdxTerm, MIN(nTerm, nIdxTerm));
        if( res==0 ) res = nTerm - nIdxTerm;
        if( res<0 ) FTS5_CORRUPT_ROWID(p, iRow);
      }

      fts5IntegrityCheckPgidx(p, iRow, pLeaf);
    }
    fts5DataRelease(pLeaf);
    if( p->rc ) break;

    /* Now check that the iter.nEmpty leaves following the current leaf
    ** (a) exist and (b) contain no terms. */
    fts5IndexIntegrityCheckEmpty(
        p, pSeg, iIdxPrevLeaf+1, iDlidxPrevLeaf+1, iIdxLeaf-1
    );
    if( p->rc ) break;

    /* If there is a doclist-index, check that it looks right. */
    if( bIdxDlidx ){
      Fts5DlidxIter *pDlidx = 0;  /* For iterating through doclist index */
      int iPrevLeaf = iIdxLeaf;
      int iSegid = pSeg->iSegid;
      int iPg = 0;
      i64 iKey;

      for(pDlidx=fts5DlidxIterInit(p, 0, iSegid, iIdxLeaf);
          fts5DlidxIterEof(p, pDlidx)==0;
          fts5DlidxIterNext(p, pDlidx)
      ){

        /* Check any rowid-less pages that occur before the current leaf. */
        for(iPg=iPrevLeaf+1; iPg<fts5DlidxIterPgno(pDlidx); iPg++){
          iKey = FTS5_SEGMENT_ROWID(iSegid, iPg);
          pLeaf = fts5DataRead(p, iKey);
          if( pLeaf ){
            if( fts5LeafFirstRowidOff(pLeaf)!=0 ) FTS5_CORRUPT_ROWID(p, iKey);
            fts5DataRelease(pLeaf);
          }
        }
        iPrevLeaf = fts5DlidxIterPgno(pDlidx);

        /* Check that the leaf page indicated by the iterator really does
        ** contain the rowid suggested by the same. */
        iKey = FTS5_SEGMENT_ROWID(iSegid, iPrevLeaf);
        pLeaf = fts5DataRead(p, iKey);
        if( pLeaf ){
          i64 iRowid;
          int iRowidOff = fts5LeafFirstRowidOff(pLeaf);
          ASSERT_SZLEAF_OK(pLeaf);
          if( iRowidOff>=pLeaf->szLeaf ){
            FTS5_CORRUPT_ROWID(p, iKey);
          }else if( bSecureDelete==0 || iRowidOff>0 ){
            i64 iDlRowid = fts5DlidxIterRowid(pDlidx);
            fts5GetVarint(&pLeaf->p[iRowidOff], (u64*)&iRowid);
            if( iRowid<iDlRowid || (bSecureDelete==0 && iRowid!=iDlRowid) ){
              FTS5_CORRUPT_ROWID(p, iKey);
            }
          }
          fts5DataRelease(pLeaf);
        }
      }

      iDlidxPrevLeaf = iPg;
      fts5DlidxIterFree(pDlidx);
      fts5TestDlidxReverse(p, iSegid, iIdxLeaf);
    }else{
      iDlidxPrevLeaf = pSeg->pgnoLast;
      /* TODO: Check there is no doclist index */
    }

    iIdxPrevLeaf = iIdxLeaf;
  }

  rc2 = sqlite3_finalize(pStmt);
  if( p->rc==SQLITE_OK ) p->rc = rc2;

  /* Page iter.iLeaf must now be the rightmost leaf-page in the segment */
#if 0
  if( p->rc==SQLITE_OK && iter.iLeaf!=pSeg->pgnoLast ){
    p->rc = FTS5_CORRUPT;
  }
#endif
}


/*
** Run internal checks to ensure that the FTS index (a) is internally 
** consistent and (b) contains entries for which the XOR of the checksums
** as calculated by sqlite3Fts5IndexEntryCksum() is cksum.
**
** Return SQLITE_CORRUPT if any of the internal checks fail, or if the
** checksum does not match. Return SQLITE_OK if all checks pass without
** error, or some other SQLite error code if another error (e.g. OOM)
** occurs.
*/
int sqlite3Fts5IndexIntegrityCheck(Fts5Index *p, u64 cksum, int bUseCksum){
  int eDetail = p->pConfig->eDetail;
  u64 cksum2 = 0;                 /* Checksum based on contents of indexes */
  Fts5Buffer poslist = {0,0,0};   /* Buffer used to hold a poslist */
  Fts5Iter *pIter;                /* Used to iterate through entire index */
  Fts5Structure *pStruct;         /* Index structure */
  int iLvl, iSeg;

#ifdef SQLITE_DEBUG
  /* Used by extra internal tests only run if NDEBUG is not defined */
  u64 cksum3 = 0;                 /* Checksum based on contents of indexes */
  Fts5Buffer term = {0,0,0};      /* Buffer used to hold most recent term */
  int bTestFail = 0;
#endif
  const int flags = FTS5INDEX_QUERY_NOOUTPUT;
  
  /* Load the FTS index structure */
  pStruct = fts5StructureRead(p);
  if( pStruct==0 ){
    assert( p->rc!=SQLITE_OK );
    return fts5IndexReturn(p);
  }

  /* Check that the internal nodes of each segment match the leaves */
  for(iLvl=0; iLvl<pStruct->nLevel; iLvl++){
    for(iSeg=0; iSeg<pStruct->aLevel[iLvl].nSeg; iSeg++){
      Fts5StructureSegment *pSeg = &pStruct->aLevel[iLvl].aSeg[iSeg];
      fts5IndexIntegrityCheckSegment(p, pSeg);
    }
  }

  /* The cksum argument passed to this function is a checksum calculated
  ** based on all expected entries in the FTS index (including prefix index
  ** entries). This block checks that a checksum calculated based on the
  ** actual contents of FTS index is identical.
  **
  ** Two versions of the same checksum are calculated. The first (stack
  ** variable cksum2) based on entries extracted from the full-text index
  ** while doing a linear scan of each individual index in turn. 
  **
  ** As each term visited by the linear scans, a separate query for the
  ** same term is performed. cksum3 is calculated based on the entries
  ** extracted by these queries.
  */
  for(fts5MultiIterNew(p, pStruct, flags, 0, 0, 0, -1, 0, &pIter);
      fts5MultiIterEof(p, pIter)==0;
      fts5MultiIterNext(p, pIter, 0, 0)
  ){
    int n;                      /* Size of term in bytes */
    i64 iPos = 0;               /* Position read from poslist */
    int iOff = 0;               /* Offset within poslist */
    i64 iRowid = fts5MultiIterRowid(pIter);
    char *z = (char*)fts5MultiIterTerm(pIter, &n);

    /* If this is a new term, query for it. Update cksum3 with the results. */
    fts5TestTerm(p, &term, z, n, cksum2, &cksum3, &bTestFail);
    if( p->rc ) break;

    if( eDetail==FTS5_DETAIL_NONE ){
      if( 0==fts5MultiIterIsEmpty(p, pIter) ){
        cksum2 ^= sqlite3Fts5IndexEntryCksum(iRowid, 0, 0, -1, z, n);
      }
    }else{
      poslist.n = 0;
      fts5SegiterPoslist(p, &pIter->aSeg[pIter->aFirst[1].iFirst], 0, &poslist);
      fts5BufferAppendBlob(&p->rc, &poslist, 4, (const u8*)"\0\0\0\0");
      while( 0==sqlite3Fts5PoslistNext64(poslist.p, poslist.n, &iOff, &iPos) ){
        int iCol = FTS5_POS2COLUMN(iPos);
        int iTokOff = FTS5_POS2OFFSET(iPos);
        cksum2 ^= sqlite3Fts5IndexEntryCksum(iRowid, iCol, iTokOff, -1, z, n);
      }
    }
  }
  fts5TestTerm(p, &term, 0, 0, cksum2, &cksum3, &bTestFail);

  fts5MultiIterFree(pIter);
  if( p->rc==SQLITE_OK && bUseCksum && cksum!=cksum2 ){
    p->rc = FTS5_CORRUPT;
    sqlite3Fts5ConfigErrmsg(p->pConfig, 
        "fts5: checksum mismatch for table \"%s\"", p->pConfig->zName
    );
  }
#ifdef SQLITE_DEBUG
  /* In SQLITE_DEBUG builds, expensive extra checks were run as part of
  ** the integrity-check above. If no other errors were detected, but one
  ** of these tests failed, set the result to SQLITE_CORRUPT_VTAB here. */
  if( p->rc==SQLITE_OK && bTestFail ){
    p->rc = FTS5_CORRUPT;
  }
  fts5BufferFree(&term);
#endif

  fts5StructureRelease(pStruct);
  fts5BufferFree(&poslist);
  return fts5IndexReturn(p);
}

/*************************************************************************
**************************************************************************
** Below this point is the implementation of the fts5_decode() scalar
** function only.
*/

#if defined(SQLITE_TEST) || defined(SQLITE_FTS5_DEBUG)
/*
** Decode a segment-data rowid from the %_data table. This function is
** the opposite of macro FTS5_SEGMENT_ROWID().
*/
static void fts5DecodeRowid(
  i64 iRowid,                     /* Rowid from %_data table */
  int *pbTombstone,               /* OUT: Tombstone hash flag */
  int *piSegid,                   /* OUT: Segment id */
  int *pbDlidx,                   /* OUT: Dlidx flag */
  int *piHeight,                  /* OUT: Height */
  int *piPgno                     /* OUT: Page number */
){
  *piPgno = (int)(iRowid & (((i64)1 << FTS5_DATA_PAGE_B) - 1));
  iRowid >>= FTS5_DATA_PAGE_B;

  *piHeight = (int)(iRowid & (((i64)1 << FTS5_DATA_HEIGHT_B) - 1));
  iRowid >>= FTS5_DATA_HEIGHT_B;

  *pbDlidx = (int)(iRowid & 0x0001);
  iRowid >>= FTS5_DATA_DLI_B;

  *piSegid = (int)(iRowid & (((i64)1 << FTS5_DATA_ID_B) - 1));
  iRowid >>= FTS5_DATA_ID_B;

  *pbTombstone = (int)(iRowid & 0x0001);
}
#endif /* SQLITE_TEST || SQLITE_FTS5_DEBUG */

#if defined(SQLITE_TEST) || defined(SQLITE_FTS5_DEBUG)
static void fts5DebugRowid(int *pRc, Fts5Buffer *pBuf, i64 iKey){
  int iSegid, iHeight, iPgno, bDlidx, bTomb;     /* Rowid components */
  fts5DecodeRowid(iKey, &bTomb, &iSegid, &bDlidx, &iHeight, &iPgno);

  if( iSegid==0 ){
    if( iKey==FTS5_AVERAGES_ROWID ){
      sqlite3Fts5BufferAppendPrintf(pRc, pBuf, "{averages} ");
    }else{
      sqlite3Fts5BufferAppendPrintf(pRc, pBuf, "{structure}");
    }
  }
  else{
    sqlite3Fts5BufferAppendPrintf(pRc, pBuf, "{%s%ssegid=%d h=%d pgno=%d}",
        bDlidx ? "dlidx " : "", 
        bTomb ? "tombstone " : "", 
        iSegid, iHeight, iPgno
    );
  }
}
#endif /* SQLITE_TEST || SQLITE_FTS5_DEBUG */

#if defined(SQLITE_TEST) || defined(SQLITE_FTS5_DEBUG)
static void fts5DebugStructure(
  int *pRc,                       /* IN/OUT: error code */
  Fts5Buffer *pBuf,
  Fts5Structure *p
){
  int iLvl, iSeg;                 /* Iterate through levels, segments */

  for(iLvl=0; iLvl<p->nLevel; iLvl++){
    Fts5StructureLevel *pLvl = &p->aLevel[iLvl];
    sqlite3Fts5BufferAppendPrintf(pRc, pBuf, 
        " {lvl=%d nMerge=%d nSeg=%d", iLvl, pLvl->nMerge, pLvl->nSeg
    );
    for(iSeg=0; iSeg<pLvl->nSeg; iSeg++){
      Fts5StructureSegment *pSeg = &pLvl->aSeg[iSeg];
      sqlite3Fts5BufferAppendPrintf(pRc, pBuf, " {id=%d leaves=%d..%d",
          pSeg->iSegid, pSeg->pgnoFirst, pSeg->pgnoLast
      );
      if( pSeg->iOrigin1>0 ){
        sqlite3Fts5BufferAppendPrintf(pRc, pBuf, " origin=%lld..%lld",
            pSeg->iOrigin1, pSeg->iOrigin2
        );
      }
      sqlite3Fts5BufferAppendPrintf(pRc, pBuf, "}");
    }
    sqlite3Fts5BufferAppendPrintf(pRc, pBuf, "}");
  }
}
#endif /* SQLITE_TEST || SQLITE_FTS5_DEBUG */

#if defined(SQLITE_TEST) || defined(SQLITE_FTS5_DEBUG)
/*
** This is part of the fts5_decode() debugging aid.
**
** Arguments pBlob/nBlob contain a serialized Fts5Structure object. This
** function appends a human-readable representation of the same object
** to the buffer passed as the second argument. 
*/
static void fts5DecodeStructure(
  int *pRc,                       /* IN/OUT: error code */
  Fts5Buffer *pBuf,
  const u8 *pBlob, int nBlob
){
  int rc;                         /* Return code */
  Fts5Structure *p = 0;           /* Decoded structure object */

  rc = fts5StructureDecode(pBlob, nBlob, 0, &p);
  if( rc!=SQLITE_OK ){
    *pRc = rc;
    return;
  }

  fts5DebugStructure(pRc, pBuf, p);
  fts5StructureRelease(p);
}
#endif /* SQLITE_TEST || SQLITE_FTS5_DEBUG */

#if defined(SQLITE_TEST) || defined(SQLITE_FTS5_DEBUG)
/*
** This is part of the fts5_decode() debugging aid.
**
** Arguments pBlob/nBlob contain an "averages" record. This function 
** appends a human-readable representation of record to the buffer passed 
** as the second argument. 
*/
static void fts5DecodeAverages(
  int *pRc,                       /* IN/OUT: error code */
  Fts5Buffer *pBuf,
  const u8 *pBlob, int nBlob
){
  int i = 0;
  const char *zSpace = "";

  while( i<nBlob ){
    u64 iVal;
    i += sqlite3Fts5GetVarint(&pBlob[i], &iVal);
    sqlite3Fts5BufferAppendPrintf(pRc, pBuf, "%s%d", zSpace, (int)iVal);
    zSpace = " ";
  }
}
#endif /* SQLITE_TEST || SQLITE_FTS5_DEBUG */

#if defined(SQLITE_TEST) || defined(SQLITE_FTS5_DEBUG)
/*
** Buffer (a/n) is assumed to contain a list of serialized varints. Read
** each varint and append its string representation to buffer pBuf. Return
** after either the input buffer is exhausted or a 0 value is read.
**
** The return value is the number of bytes read from the input buffer.
*/
static int fts5DecodePoslist(int *pRc, Fts5Buffer *pBuf, const u8 *a, int n){
  int iOff = 0;
  while( iOff<n ){
    int iVal;
    iOff += fts5GetVarint32(&a[iOff], iVal);
    sqlite3Fts5BufferAppendPrintf(pRc, pBuf, " %d", iVal);
  }
  return iOff;
}
#endif /* SQLITE_TEST || SQLITE_FTS5_DEBUG */

#if defined(SQLITE_TEST) || defined(SQLITE_FTS5_DEBUG)
/*
** The start of buffer (a/n) contains the start of a doclist. The doclist
** may or may not finish within the buffer. This function appends a text
** representation of the part of the doclist that is present to buffer
** pBuf. 
**
** The return value is the number of bytes read from the input buffer.
*/
static int fts5DecodeDoclist(int *pRc, Fts5Buffer *pBuf, const u8 *a, int n){
  i64 iDocid = 0;
  int iOff = 0;

  if( n>0 ){
    iOff = sqlite3Fts5GetVarint(a, (u64*)&iDocid);
    sqlite3Fts5BufferAppendPrintf(pRc, pBuf, " id=%lld", iDocid);
  }
  while( iOff<n ){
    int nPos;
    int bDel;
    iOff += fts5GetPoslistSize(&a[iOff], &nPos, &bDel);
    sqlite3Fts5BufferAppendPrintf(pRc, pBuf, " nPos=%d%s", nPos, bDel?"*":"");
    iOff += fts5DecodePoslist(pRc, pBuf, &a[iOff], MIN(n-iOff, nPos));
    if( iOff<n ){
      i64 iDelta;
      iOff += sqlite3Fts5GetVarint(&a[iOff], (u64*)&iDelta);
      iDocid += iDelta;
      sqlite3Fts5BufferAppendPrintf(pRc, pBuf, " id=%lld", iDocid);
    }
  }

  return iOff;
}
#endif /* SQLITE_TEST || SQLITE_FTS5_DEBUG */

#if defined(SQLITE_TEST) || defined(SQLITE_FTS5_DEBUG)
/*
** This function is part of the fts5_decode() debugging function. It is 
** only ever used with detail=none tables.
**
** Buffer (pData/nData) contains a doclist in the format used by detail=none
** tables. This function appends a human-readable version of that list to
** buffer pBuf.
**
** If *pRc is other than SQLITE_OK when this function is called, it is a
** no-op. If an OOM or other error occurs within this function, *pRc is
** set to an SQLite error code before returning. The final state of buffer
** pBuf is undefined in this case.
*/
static void fts5DecodeRowidList(
  int *pRc,                       /* IN/OUT: Error code */
  Fts5Buffer *pBuf,               /* Buffer to append text to */
  const u8 *pData, int nData      /* Data to decode list-of-rowids from */
){
  int i = 0;
  i64 iRowid = 0;

  while( i<nData ){
    const char *zApp = "";
    u64 iVal;
    i += sqlite3Fts5GetVarint(&pData[i], &iVal);
    iRowid += iVal;

    if( i<nData && pData[i]==0x00 ){
      i++;
      if( i<nData && pData[i]==0x00 ){
        i++;
        zApp = "+";
      }else{
        zApp = "*";
      }
    }

    sqlite3Fts5BufferAppendPrintf(pRc, pBuf, " %lld%s", iRowid, zApp);
  }
}
#endif /* SQLITE_TEST || SQLITE_FTS5_DEBUG */

#if defined(SQLITE_TEST) || defined(SQLITE_FTS5_DEBUG)
static void fts5BufferAppendTerm(int *pRc, Fts5Buffer *pBuf, Fts5Buffer *pTerm){
  int ii;
  fts5BufferGrow(pRc, pBuf, pTerm->n*2 + 1);
  if( *pRc==SQLITE_OK ){
    for(ii=0; ii<pTerm->n; ii++){
      if( pTerm->p[ii]==0x00 ){
        pBuf->p[pBuf->n++] = '\\';
        pBuf->p[pBuf->n++] = '0';
      }else{
        pBuf->p[pBuf->n++] = pTerm->p[ii];
      }
    }
    pBuf->p[pBuf->n] = 0x00;
  }
}
#endif /* SQLITE_TEST || SQLITE_FTS5_DEBUG */

#if defined(SQLITE_TEST) || defined(SQLITE_FTS5_DEBUG)
/*
** The implementation of user-defined scalar function fts5_decode().
*/
static void fts5DecodeFunction(
  sqlite3_context *pCtx,          /* Function call context */
  int nArg,                       /* Number of args (always 2) */
  sqlite3_value **apVal           /* Function arguments */
){
  i64 iRowid;                     /* Rowid for record being decoded */
  int iSegid,iHeight,iPgno,bDlidx;/* Rowid components */
  int bTomb;
  const u8 *aBlob; int n;         /* Record to decode */
  u8 *a = 0;
  Fts5Buffer s;                   /* Build up text to return here */
  int rc = SQLITE_OK;             /* Return code */
  sqlite3_int64 nSpace = 0;
  int eDetailNone = (sqlite3_user_data(pCtx)!=0);

  assert( nArg==2 );
  UNUSED_PARAM(nArg);
  memset(&s, 0, sizeof(Fts5Buffer));
  iRowid = sqlite3_value_int64(apVal[0]);

  /* Make a copy of the second argument (a blob) in aBlob[]. The aBlob[]
  ** copy is followed by FTS5_DATA_ZERO_PADDING 0x00 bytes, which prevents
  ** buffer overreads even if the record is corrupt.  */
  n = sqlite3_value_bytes(apVal[1]);
  aBlob = sqlite3_value_blob(apVal[1]);
  nSpace = ((i64)n) + FTS5_DATA_ZERO_PADDING;
  a = (u8*)sqlite3Fts5MallocZero(&rc, nSpace);
  if( a==0 ) goto decode_out;
  if( n>0 ) memcpy(a, aBlob, n);

  fts5DecodeRowid(iRowid, &bTomb, &iSegid, &bDlidx, &iHeight, &iPgno);

  fts5DebugRowid(&rc, &s, iRowid);
  if( bDlidx ){
    Fts5Data dlidx;
    Fts5DlidxLvl lvl;

    dlidx.p = a;
    dlidx.nn = n;

    memset(&lvl, 0, sizeof(Fts5DlidxLvl));
    lvl.pData = &dlidx;
    lvl.iLeafPgno = iPgno;

    for(fts5DlidxLvlNext(&lvl); lvl.bEof==0; fts5DlidxLvlNext(&lvl)){
      sqlite3Fts5BufferAppendPrintf(&rc, &s, 
          " %d(%lld)", lvl.iLeafPgno, lvl.iRowid
      );
    }
  }else if( bTomb ){
    u32 nElem  = fts5GetU32(&a[4]);
    int szKey = (aBlob[0]==4 || aBlob[0]==8) ? aBlob[0] : 8;
    int nSlot = (n - 8) / szKey;
    int ii;
    sqlite3Fts5BufferAppendPrintf(&rc, &s, " nElem=%d", (int)nElem);
    if( aBlob[1] ){
      sqlite3Fts5BufferAppendPrintf(&rc, &s, " 0");
    }
    for(ii=0; ii<nSlot; ii++){
      u64 iVal = 0;
      if( szKey==4 ){
        u32 *aSlot = (u32*)&aBlob[8];
        if( aSlot[ii] ) iVal = fts5GetU32((u8*)&aSlot[ii]);
      }else{
        u64 *aSlot = (u64*)&aBlob[8];
        if( aSlot[ii] ) iVal = fts5GetU64((u8*)&aSlot[ii]);
      }
      if( iVal!=0 ){
        sqlite3Fts5BufferAppendPrintf(&rc, &s, " %lld", (i64)iVal);
      }
    }
  }else if( iSegid==0 ){
    if( iRowid==FTS5_AVERAGES_ROWID ){
      fts5DecodeAverages(&rc, &s, a, n);
    }else{
      fts5DecodeStructure(&rc, &s, a, n);
    }
  }else if( eDetailNone ){
    Fts5Buffer term;              /* Current term read from page */
    int szLeaf;
    int iPgidxOff = szLeaf = fts5GetU16(&a[2]);
    int iTermOff;
    int nKeep = 0;
    int iOff;

    memset(&term, 0, sizeof(Fts5Buffer));

    /* Decode any entries that occur before the first term. */
    if( szLeaf<n ){
      iPgidxOff += fts5GetVarint32(&a[iPgidxOff], iTermOff);
    }else{
      iTermOff = szLeaf;
    }
    fts5DecodeRowidList(&rc, &s, &a[4], iTermOff-4);

    iOff = iTermOff;
    while( iOff<szLeaf && rc==SQLITE_OK ){
      int nAppend;

      /* Read the term data for the next term*/
      iOff += fts5GetVarint32(&a[iOff], nAppend);
      term.n = nKeep;
      fts5BufferAppendBlob(&rc, &term, nAppend, &a[iOff]);
      sqlite3Fts5BufferAppendPrintf(&rc, &s, " term=");
      fts5BufferAppendTerm(&rc, &s, &term);
      iOff += nAppend;

      /* Figure out where the doclist for this term ends */
      if( iPgidxOff<n ){
        int nIncr;
        iPgidxOff += fts5GetVarint32(&a[iPgidxOff], nIncr);
        iTermOff += nIncr;
      }else{
        iTermOff = szLeaf;
      }
      if( iTermOff>szLeaf ){
        rc = FTS5_CORRUPT;
      }else{
        fts5DecodeRowidList(&rc, &s, &a[iOff], iTermOff-iOff);
      }
      iOff = iTermOff;
      if( iOff<szLeaf ){
        iOff += fts5GetVarint32(&a[iOff], nKeep);
      }
    }

    fts5BufferFree(&term);
  }else{
    Fts5Buffer term;              /* Current term read from page */
    int szLeaf;                   /* Offset of pgidx in a[] */
    int iPgidxOff;
    int iPgidxPrev = 0;           /* Previous value read from pgidx */
    int iTermOff = 0;
    int iRowidOff = 0;
    int iOff;
    int nDoclist;

    memset(&term, 0, sizeof(Fts5Buffer));

    if( n<4 ){
      sqlite3Fts5BufferSet(&rc, &s, 7, (const u8*)"corrupt");
      goto decode_out;
    }else{
      iRowidOff = fts5GetU16(&a[0]);
      iPgidxOff = szLeaf = fts5GetU16(&a[2]);
      if( iPgidxOff<n ){
        fts5GetVarint32(&a[iPgidxOff], iTermOff);
      }else if( iPgidxOff>n ){
        rc = FTS5_CORRUPT;
        goto decode_out;
      }
    }

    /* Decode the position list tail at the start of the page */
    if( iRowidOff!=0 ){
      iOff = iRowidOff;
    }else if( iTermOff!=0 ){
      iOff = iTermOff;
    }else{
      iOff = szLeaf;
    }
    if( iOff>n ){
      rc = FTS5_CORRUPT;
      goto decode_out;
    }
    fts5DecodePoslist(&rc, &s, &a[4], iOff-4);

    /* Decode any more doclist data that appears on the page before the
    ** first term. */
    nDoclist = (iTermOff ? iTermOff : szLeaf) - iOff;
    if( nDoclist+iOff>n ){
      rc = FTS5_CORRUPT;
      goto decode_out;
    }
    fts5DecodeDoclist(&rc, &s, &a[iOff], nDoclist);

    while( iPgidxOff<n && rc==SQLITE_OK ){
      int bFirst = (iPgidxOff==szLeaf);     /* True for first term on page */
      int nByte;                            /* Bytes of data */
      int iEnd;
      
      iPgidxOff += fts5GetVarint32(&a[iPgidxOff], nByte);
      iPgidxPrev += nByte;
      iOff = iPgidxPrev;

      if( iPgidxOff<n ){
        fts5GetVarint32(&a[iPgidxOff], nByte);
        iEnd = iPgidxPrev + nByte;
      }else{
        iEnd = szLeaf;
      }
      if( iEnd>szLeaf ){
        rc = FTS5_CORRUPT;
        break;
      }

      if( bFirst==0 ){
        iOff += fts5GetVarint32(&a[iOff], nByte);
        if( nByte>term.n ){
          rc = FTS5_CORRUPT;
          break;
        }
        term.n = nByte;
      }
      iOff += fts5GetVarint32(&a[iOff], nByte);
      if( iOff+nByte>n ){
        rc = FTS5_CORRUPT;
        break;
      }
      fts5BufferAppendBlob(&rc, &term, nByte, &a[iOff]);
      iOff += nByte;

      sqlite3Fts5BufferAppendPrintf(&rc, &s, " term=");
      fts5BufferAppendTerm(&rc, &s, &term);
      iOff += fts5DecodeDoclist(&rc, &s, &a[iOff], iEnd-iOff);
    }

    fts5BufferFree(&term);
  }
  
 decode_out:
  sqlite3_free(a);
  if( rc==SQLITE_OK ){
    sqlite3_result_text(pCtx, (const char*)s.p, s.n, SQLITE_TRANSIENT);
  }else{
    sqlite3_result_error_code(pCtx, rc);
  }
  fts5BufferFree(&s);
}
#endif /* SQLITE_TEST || SQLITE_FTS5_DEBUG */

#if defined(SQLITE_TEST) || defined(SQLITE_FTS5_DEBUG) 
/*
** The implementation of user-defined scalar function fts5_rowid().
*/
static void fts5RowidFunction(
  sqlite3_context *pCtx,          /* Function call context */
  int nArg,                       /* Number of args (always 2) */
  sqlite3_value **apVal           /* Function arguments */
){
  const char *zArg;
  if( nArg==0 ){
    sqlite3_result_error(pCtx, "should be: fts5_rowid(subject, ....)", -1);
  }else{
    zArg = (const char*)sqlite3_value_text(apVal[0]);
    if( 0==sqlite3_stricmp(zArg, "segment") ){
      i64 iRowid;
      int segid, pgno;
      if( nArg!=3 ){
        sqlite3_result_error(pCtx, 
            "should be: fts5_rowid('segment', segid, pgno))", -1
        );
      }else{
        segid = sqlite3_value_int(apVal[1]);
        pgno = sqlite3_value_int(apVal[2]);
        iRowid = FTS5_SEGMENT_ROWID(segid, pgno);
        sqlite3_result_int64(pCtx, iRowid);
      }
    }else{
      sqlite3_result_error(pCtx, 
        "first arg to fts5_rowid() must be 'segment'" , -1
      );
    }
  }
}
#endif /* SQLITE_TEST || SQLITE_FTS5_DEBUG */

#if defined(SQLITE_TEST) || defined(SQLITE_FTS5_DEBUG)

typedef struct Fts5StructVtab Fts5StructVtab;
struct Fts5StructVtab {
  sqlite3_vtab base;
};

typedef struct Fts5StructVcsr Fts5StructVcsr;
struct Fts5StructVcsr {
  sqlite3_vtab_cursor base;
  Fts5Structure *pStruct;
  int iLevel;
  int iSeg;
  int iRowid;
};

/*
** Create a new fts5_structure() table-valued function.
*/
static int fts5structConnectMethod(
  sqlite3 *db,
  void *pAux,
  int argc, const char *const*argv,
  sqlite3_vtab **ppVtab,
  char **pzErr
){
  Fts5StructVtab *pNew = 0;
  int rc = SQLITE_OK;

  rc = sqlite3_declare_vtab(db, 
      "CREATE TABLE xyz("
          "level, segment, merge, segid, leaf1, leaf2, loc1, loc2, "
          "npgtombstone, nentrytombstone, nentry, struct HIDDEN);"
  );
  if( rc==SQLITE_OK ){
    pNew = sqlite3Fts5MallocZero(&rc, sizeof(*pNew));
  }

  *ppVtab = (sqlite3_vtab*)pNew;
  return rc;
}

/*
** We must have a single struct=? constraint that will be passed through
** into the xFilter method.  If there is no valid struct=? constraint,
** then return an SQLITE_CONSTRAINT error.
*/
static int fts5structBestIndexMethod(
  sqlite3_vtab *tab,
  sqlite3_index_info *pIdxInfo
){
  int i;
  int rc = SQLITE_CONSTRAINT;
  struct sqlite3_index_constraint *p;
  pIdxInfo->estimatedCost = (double)100;
  pIdxInfo->estimatedRows = 100;
  pIdxInfo->idxNum = 0;
  for(i=0, p=pIdxInfo->aConstraint; i<pIdxInfo->nConstraint; i++, p++){
    if( p->usable==0 ) continue;
    if( p->op==SQLITE_INDEX_CONSTRAINT_EQ && p->iColumn==11 ){
      rc = SQLITE_OK;
      pIdxInfo->aConstraintUsage[i].omit = 1;
      pIdxInfo->aConstraintUsage[i].argvIndex = 1;
      break;
    }
  }
  return rc;
}

/*
** This method is the destructor for bytecodevtab objects.
*/
static int fts5structDisconnectMethod(sqlite3_vtab *pVtab){
  Fts5StructVtab *p = (Fts5StructVtab*)pVtab;
  sqlite3_free(p);
  return SQLITE_OK;
}

/*
** Constructor for a new bytecodevtab_cursor object.
*/
static int fts5structOpenMethod(sqlite3_vtab *p, sqlite3_vtab_cursor **ppCsr){
  int rc = SQLITE_OK;
  Fts5StructVcsr *pNew = 0;

  pNew = sqlite3Fts5MallocZero(&rc, sizeof(*pNew));
  *ppCsr = (sqlite3_vtab_cursor*)pNew;

  return SQLITE_OK;
}

/*
** Destructor for a bytecodevtab_cursor.
*/
static int fts5structCloseMethod(sqlite3_vtab_cursor *cur){
  Fts5StructVcsr *pCsr = (Fts5StructVcsr*)cur;
  fts5StructureRelease(pCsr->pStruct);
  sqlite3_free(pCsr);
  return SQLITE_OK;
}


/*
** Advance a bytecodevtab_cursor to its next row of output.
*/
static int fts5structNextMethod(sqlite3_vtab_cursor *cur){
  Fts5StructVcsr *pCsr = (Fts5StructVcsr*)cur;
  Fts5Structure *p = pCsr->pStruct;

  assert( pCsr->pStruct );
  pCsr->iSeg++;
  pCsr->iRowid++;
  while( pCsr->iLevel<p->nLevel && pCsr->iSeg>=p->aLevel[pCsr->iLevel].nSeg ){
    pCsr->iLevel++;
    pCsr->iSeg = 0;
  }
  if( pCsr->iLevel>=p->nLevel ){
    fts5StructureRelease(pCsr->pStruct);
    pCsr->pStruct = 0;
  }
  return SQLITE_OK;
}

/*
** Return TRUE if the cursor has been moved off of the last
** row of output.
*/
static int fts5structEofMethod(sqlite3_vtab_cursor *cur){
  Fts5StructVcsr *pCsr = (Fts5StructVcsr*)cur;
  return pCsr->pStruct==0;
}

static int fts5structRowidMethod(
  sqlite3_vtab_cursor *cur, 
  sqlite_int64 *piRowid
){
  Fts5StructVcsr *pCsr = (Fts5StructVcsr*)cur;
  *piRowid = pCsr->iRowid;
  return SQLITE_OK;
}

/*
** Return values of columns for the row at which the bytecodevtab_cursor
** is currently pointing.
*/
static int fts5structColumnMethod(
  sqlite3_vtab_cursor *cur,   /* The cursor */
  sqlite3_context *ctx,       /* First argument to sqlite3_result_...() */
  int i                       /* Which column to return */
){
  Fts5StructVcsr *pCsr = (Fts5StructVcsr*)cur;
  Fts5Structure *p = pCsr->pStruct;
  Fts5StructureSegment *pSeg = &p->aLevel[pCsr->iLevel].aSeg[pCsr->iSeg];

  switch( i ){
    case 0: /* level */
      sqlite3_result_int(ctx, pCsr->iLevel);
      break;
    case 1: /* segment */
      sqlite3_result_int(ctx, pCsr->iSeg);
      break;
    case 2: /* merge */
      sqlite3_result_int(ctx, pCsr->iSeg < p->aLevel[pCsr->iLevel].nMerge);
      break;
    case 3: /* segid */
      sqlite3_result_int(ctx, pSeg->iSegid);
      break;
    case 4: /* leaf1 */
      sqlite3_result_int(ctx, pSeg->pgnoFirst);
      break;
    case 5: /* leaf2 */
      sqlite3_result_int(ctx, pSeg->pgnoLast);
      break;
    case 6: /* origin1 */
      sqlite3_result_int64(ctx, pSeg->iOrigin1);
      break;
    case 7: /* origin2 */
      sqlite3_result_int64(ctx, pSeg->iOrigin2);
      break;
    case 8: /* npgtombstone */
      sqlite3_result_int(ctx, pSeg->nPgTombstone);
      break;
    case 9: /* nentrytombstone */
      sqlite3_result_int64(ctx, pSeg->nEntryTombstone);
      break;
    case 10: /* nentry */
      sqlite3_result_int64(ctx, pSeg->nEntry);
      break;
  }
  return SQLITE_OK;
}

/*
** Initialize a cursor.
**
**    idxNum==0     means show all subprograms
**    idxNum==1     means show only the main bytecode and omit subprograms.
*/
static int fts5structFilterMethod(
  sqlite3_vtab_cursor *pVtabCursor, 
  int idxNum, const char *idxStr,
  int argc, sqlite3_value **argv
){
  Fts5StructVcsr *pCsr = (Fts5StructVcsr *)pVtabCursor;
  int rc = SQLITE_OK;

  const u8 *aBlob = 0;
  int nBlob = 0;

  assert( argc==1 );
  fts5StructureRelease(pCsr->pStruct);
  pCsr->pStruct = 0;

  nBlob = sqlite3_value_bytes(argv[0]);
  aBlob = (const u8*)sqlite3_value_blob(argv[0]);
  rc = fts5StructureDecode(aBlob, nBlob, 0, &pCsr->pStruct);
  if( rc==SQLITE_OK ){
    pCsr->iLevel = 0;
    pCsr->iRowid = 0;
    pCsr->iSeg = -1;
    rc = fts5structNextMethod(pVtabCursor);
  }

  return rc;
}

#endif /* SQLITE_TEST || SQLITE_FTS5_DEBUG */

/*
** This is called as part of registering the FTS5 module with database
** connection db. It registers several user-defined scalar functions useful
** with FTS5.
**
** If successful, SQLITE_OK is returned. If an error occurs, some other
** SQLite error code is returned instead.
*/
int sqlite3Fts5IndexInit(sqlite3 *db){
#if defined(SQLITE_TEST) || defined(SQLITE_FTS5_DEBUG)
  int rc = sqlite3_create_function(
      db, "fts5_decode", 2, SQLITE_UTF8, 0, fts5DecodeFunction, 0, 0
  );

  if( rc==SQLITE_OK ){
    rc = sqlite3_create_function(
        db, "fts5_decode_none", 2, 
        SQLITE_UTF8, (void*)db, fts5DecodeFunction, 0, 0
    );
  }

  if( rc==SQLITE_OK ){
    rc = sqlite3_create_function(
        db, "fts5_rowid", -1, SQLITE_UTF8, 0, fts5RowidFunction, 0, 0
    );
  }

  if( rc==SQLITE_OK ){
    static const sqlite3_module fts5structure_module = {
      0,                           /* iVersion      */
      0,                           /* xCreate       */
      fts5structConnectMethod,     /* xConnect      */
      fts5structBestIndexMethod,   /* xBestIndex    */
      fts5structDisconnectMethod,  /* xDisconnect   */
      0,                           /* xDestroy      */
      fts5structOpenMethod,        /* xOpen         */
      fts5structCloseMethod,       /* xClose        */
      fts5structFilterMethod,      /* xFilter       */
      fts5structNextMethod,        /* xNext         */
      fts5structEofMethod,         /* xEof          */
      fts5structColumnMethod,      /* xColumn       */
      fts5structRowidMethod,       /* xRowid        */
      0,                           /* xUpdate       */
      0,                           /* xBegin        */
      0,                           /* xSync         */
      0,                           /* xCommit       */
      0,                           /* xRollback     */
      0,                           /* xFindFunction */
      0,                           /* xRename       */
      0,                           /* xSavepoint    */
      0,                           /* xRelease      */
      0,                           /* xRollbackTo   */
      0,                           /* xShadowName   */
      0                            /* xIntegrity    */
    };
    rc = sqlite3_create_module(db, "fts5_structure", &fts5structure_module, 0);
  }
  return rc;
#else
  return SQLITE_OK;
  UNUSED_PARAM(db);
#endif
}


int sqlite3Fts5IndexReset(Fts5Index *p){
  assert( p->pStruct==0 || p->iStructVersion!=0 );
  if( fts5IndexDataVersion(p)!=p->iStructVersion ){
    fts5StructureInvalidate(p);
  }
  return fts5IndexReturn(p);
}
