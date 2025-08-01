
#if defined(SQLITE_ENABLE_SESSION) && defined(SQLITE_ENABLE_PREUPDATE_HOOK)
#include "sqlite3session.h"
#include <assert.h>
#include <string.h>

#ifndef SQLITE_AMALGAMATION
# include "sqliteInt.h"
# include "vdbeInt.h"
#endif

typedef struct SessionTable SessionTable;
typedef struct SessionChange SessionChange;
typedef struct SessionBuffer SessionBuffer;
typedef struct SessionInput SessionInput;

/*
** Minimum chunk size used by streaming versions of functions.
*/
#ifndef SESSIONS_STRM_CHUNK_SIZE
# ifdef SQLITE_TEST
#   define SESSIONS_STRM_CHUNK_SIZE 64
# else
#   define SESSIONS_STRM_CHUNK_SIZE 1024
# endif
#endif

#define SESSIONS_ROWID "_rowid_"

static int sessions_strm_chunk_size = SESSIONS_STRM_CHUNK_SIZE;

typedef struct SessionHook SessionHook;
struct SessionHook {
  void *pCtx;
  int (*xOld)(void*,int,sqlite3_value**);
  int (*xNew)(void*,int,sqlite3_value**);
  int (*xCount)(void*);
  int (*xDepth)(void*);
};

/*
** Session handle structure.
*/
struct sqlite3_session {
  sqlite3 *db;                    /* Database handle session is attached to */
  char *zDb;                      /* Name of database session is attached to */
  int bEnableSize;                /* True if changeset_size() enabled */
  int bEnable;                    /* True if currently recording */
  int bIndirect;                  /* True if all changes are indirect */
  int bAutoAttach;                /* True to auto-attach tables */
  int bImplicitPK;                /* True to handle tables with implicit PK */
  int rc;                         /* Non-zero if an error has occurred */
  void *pFilterCtx;               /* First argument to pass to xTableFilter */
  int (*xTableFilter)(void *pCtx, const char *zTab);
  i64 nMalloc;                    /* Number of bytes of data allocated */
  i64 nMaxChangesetSize;
  sqlite3_value *pZeroBlob;       /* Value containing X'' */
  sqlite3_session *pNext;         /* Next session object on same db. */
  SessionTable *pTable;           /* List of attached tables */
  SessionHook hook;               /* APIs to grab new and old data with */
};

/*
** Instances of this structure are used to build strings or binary records.
*/
struct SessionBuffer {
  u8 *aBuf;                       /* Pointer to changeset buffer */
  int nBuf;                       /* Size of buffer aBuf */
  int nAlloc;                     /* Size of allocation containing aBuf */
};

/*
** An object of this type is used internally as an abstraction for 
** input data. Input data may be supplied either as a single large buffer
** (e.g. sqlite3changeset_start()) or using a stream function (e.g.
**  sqlite3changeset_start_strm()).
**
** bNoDiscard:
**   If true, then the only time data is discarded is as a result of explicit
**   sessionDiscardData() calls. Not within every sessionInputBuffer() call.
*/
struct SessionInput {
  int bNoDiscard;                 /* If true, do not discard in InputBuffer() */
  int iCurrent;                   /* Offset in aData[] of current change */
  int iNext;                      /* Offset in aData[] of next change */
  u8 *aData;                      /* Pointer to buffer containing changeset */
  int nData;                      /* Number of bytes in aData */

  SessionBuffer buf;              /* Current read buffer */
  int (*xInput)(void*, void*, int*);        /* Input stream call (or NULL) */
  void *pIn;                                /* First argument to xInput */
  int bEof;                       /* Set to true after xInput finished */
};

/*
** Structure for changeset iterators.
*/
struct sqlite3_changeset_iter {
  SessionInput in;                /* Input buffer or stream */
  SessionBuffer tblhdr;           /* Buffer to hold apValue/zTab/abPK/ */
  int bPatchset;                  /* True if this is a patchset */
  int bInvert;                    /* True to invert changeset */
  int bSkipEmpty;                 /* Skip noop UPDATE changes */
  int rc;                         /* Iterator error code */
  sqlite3_stmt *pConflict;        /* Points to conflicting row, if any */
  char *zTab;                     /* Current table */
  int nCol;                       /* Number of columns in zTab */
  int op;                         /* Current operation */
  int bIndirect;                  /* True if current change was indirect */
  u8 *abPK;                       /* Primary key array */
  sqlite3_value **apValue;        /* old.* and new.* values */
};

/*
** Each session object maintains a set of the following structures, one
** for each table the session object is monitoring. The structures are
** stored in a linked list starting at sqlite3_session.pTable.
**
** The keys of the SessionTable.aChange[] hash table are all rows that have
** been modified in any way since the session object was attached to the
** table.
**
** The data associated with each hash-table entry is a structure containing
** a subset of the initial values that the modified row contained at the
** start of the session. Or no initial values if the row was inserted.
**
** pDfltStmt:
**   This is only used by the sqlite3changegroup_xxx() APIs, not by
**   regular sqlite3_session objects. It is a SELECT statement that
**   selects the default value for each table column. For example,
**   if the table is 
**
**      CREATE TABLE xx(a DEFAULT 1, b, c DEFAULT 'abc')
**
**   then this variable is the compiled version of:
**
**      SELECT 1, NULL, 'abc'
*/
struct SessionTable {
  SessionTable *pNext;
  char *zName;                    /* Local name of table */
  int nCol;                       /* Number of non-hidden columns */
  int nTotalCol;                  /* Number of columns including hidden */
  int bStat1;                     /* True if this is sqlite_stat1 */
  int bRowid;                     /* True if this table uses rowid for PK */
  const char **azCol;             /* Column names */
  const char **azDflt;            /* Default value expressions */
  int *aiIdx;                     /* Index to pass to xNew/xOld */
  u8 *abPK;                       /* Array of primary key flags */
  int nEntry;                     /* Total number of entries in hash table */
  int nChange;                    /* Size of apChange[] array */
  SessionChange **apChange;       /* Hash table buckets */
  sqlite3_stmt *pDfltStmt;
};

/* 
** RECORD FORMAT:
**
** The following record format is similar to (but not compatible with) that 
** used in SQLite database files. This format is used as part of the 
** change-set binary format, and so must be architecture independent.
**
** Unlike the SQLite database record format, each field is self-contained -
** there is no separation of header and data. Each field begins with a
** single byte describing its type, as follows:
**
**       0x00: Undefined value.
**       0x01: Integer value.
**       0x02: Real value.
**       0x03: Text value.
**       0x04: Blob value.
**       0x05: SQL NULL value.
**
** Note that the above match the definitions of SQLITE_INTEGER, SQLITE_TEXT
** and so on in sqlite3.h. For undefined and NULL values, the field consists
** only of the single type byte. For other types of values, the type byte
** is followed by:
**
**   Text values:
**     A varint containing the number of bytes in the value (encoded using
**     UTF-8). Followed by a buffer containing the UTF-8 representation
**     of the text value. There is no nul terminator.
**
**   Blob values:
**     A varint containing the number of bytes in the value, followed by
**     a buffer containing the value itself.
**
**   Integer values:
**     An 8-byte big-endian integer value.
**
**   Real values:
**     An 8-byte big-endian IEEE 754-2008 real value.
**
** Varint values are encoded in the same way as varints in the SQLite 
** record format.
**
** CHANGESET FORMAT:
**
** A changeset is a collection of DELETE, UPDATE and INSERT operations on
** one or more tables. Operations on a single table are grouped together,
** but may occur in any order (i.e. deletes, updates and inserts are all
** mixed together).
**
** Each group of changes begins with a table header:
**
**   1 byte: Constant 0x54 (capital 'T')
**   Varint: Number of columns in the table.
**   nCol bytes: 0x01 for PK columns, 0x00 otherwise.
**   N bytes: Unqualified table name (encoded using UTF-8). Nul-terminated.
**
** Followed by one or more changes to the table.
**
**   1 byte: Either SQLITE_INSERT (0x12), UPDATE (0x17) or DELETE (0x09).
**   1 byte: The "indirect-change" flag.
**   old.* record: (delete and update only)
**   new.* record: (insert and update only)
**
** The "old.*" and "new.*" records, if present, are N field records in the
** format described above under "RECORD FORMAT", where N is the number of
** columns in the table. The i'th field of each record is associated with
** the i'th column of the table, counting from left to right in the order
** in which columns were declared in the CREATE TABLE statement.
**
** The new.* record that is part of each INSERT change contains the values
** that make up the new row. Similarly, the old.* record that is part of each
** DELETE change contains the values that made up the row that was deleted 
** from the database. In the changeset format, the records that are part
** of INSERT or DELETE changes never contain any undefined (type byte 0x00)
** fields.
**
** Within the old.* record associated with an UPDATE change, all fields
** associated with table columns that are not PRIMARY KEY columns and are
** not modified by the UPDATE change are set to "undefined". Other fields
** are set to the values that made up the row before the UPDATE that the
** change records took place. Within the new.* record, fields associated 
** with table columns modified by the UPDATE change contain the new 
** values. Fields associated with table columns that are not modified
** are set to "undefined".
**
** PATCHSET FORMAT:
**
** A patchset is also a collection of changes. It is similar to a changeset,
** but leaves undefined those fields that are not useful if no conflict
** resolution is required when applying the changeset.
**
** Each group of changes begins with a table header:
**
**   1 byte: Constant 0x50 (capital 'P')
**   Varint: Number of columns in the table.
**   nCol bytes: 0x01 for PK columns, 0x00 otherwise.
**   N bytes: Unqualified table name (encoded using UTF-8). Nul-terminated.
**
** Followed by one or more changes to the table.
**
**   1 byte: Either SQLITE_INSERT (0x12), UPDATE (0x17) or DELETE (0x09).
**   1 byte: The "indirect-change" flag.
**   single record: (PK fields for DELETE, PK and modified fields for UPDATE,
**                   full record for INSERT).
**
** As in the changeset format, each field of the single record that is part
** of a patchset change is associated with the correspondingly positioned
** table column, counting from left to right within the CREATE TABLE 
** statement.
**
** For a DELETE change, all fields within the record except those associated
** with PRIMARY KEY columns are omitted. The PRIMARY KEY fields contain the
** values identifying the row to delete.
**
** For an UPDATE change, all fields except those associated with PRIMARY KEY
** columns and columns that are modified by the UPDATE are set to "undefined".
** PRIMARY KEY fields contain the values identifying the table row to update,
** and fields associated with modified columns contain the new column values.
**
** The records associated with INSERT changes are in the same format as for
** changesets. It is not possible for a record associated with an INSERT
** change to contain a field set to "undefined".
**
** REBASE BLOB FORMAT:
**
** A rebase blob may be output by sqlite3changeset_apply_v2() and its 
** streaming equivalent for use with the sqlite3_rebaser APIs to rebase
** existing changesets. A rebase blob contains one entry for each conflict
** resolved using either the OMIT or REPLACE strategies within the apply_v2()
** call.
**
** The format used for a rebase blob is very similar to that used for
** changesets. All entries related to a single table are grouped together.
**
** Each group of entries begins with a table header in changeset format:
**
**   1 byte: Constant 0x54 (capital 'T')
**   Varint: Number of columns in the table.
**   nCol bytes: 0x01 for PK columns, 0x00 otherwise.
**   N bytes: Unqualified table name (encoded using UTF-8). Nul-terminated.
**
** Followed by one or more entries associated with the table.
**
**   1 byte: Either SQLITE_INSERT (0x12), DELETE (0x09).
**   1 byte: Flag. 0x01 for REPLACE, 0x00 for OMIT.
**   record: (in the record format defined above).
**
** In a rebase blob, the first field is set to SQLITE_INSERT if the change
** that caused the conflict was an INSERT or UPDATE, or to SQLITE_DELETE if
** it was a DELETE. The second field is set to 0x01 if the conflict 
** resolution strategy was REPLACE, or 0x00 if it was OMIT.
**
** If the change that caused the conflict was a DELETE, then the single
** record is a copy of the old.* record from the original changeset. If it
** was an INSERT, then the single record is a copy of the new.* record. If
** the conflicting change was an UPDATE, then the single record is a copy
** of the new.* record with the PK fields filled in based on the original
** old.* record.
*/

/*
** For each row modified during a session, there exists a single instance of
** this structure stored in a SessionTable.aChange[] hash table.
*/
struct SessionChange {
  u8 op;                          /* One of UPDATE, DELETE, INSERT */
  u8 bIndirect;                   /* True if this change is "indirect" */
  u16 nRecordField;               /* Number of fields in aRecord[] */
  int nMaxSize;                   /* Max size of eventual changeset record */
  int nRecord;                    /* Number of bytes in buffer aRecord[] */
  u8 *aRecord;                    /* Buffer containing old.* record */
  SessionChange *pNext;           /* For hash-table collisions */
};

/*
** Write a varint with value iVal into the buffer at aBuf. Return the 
** number of bytes written.
*/
static int sessionVarintPut(u8 *aBuf, int iVal){
  return putVarint32(aBuf, iVal);
}

/*
** Return the number of bytes required to store value iVal as a varint.
*/
static int sessionVarintLen(int iVal){
  return sqlite3VarintLen(iVal);
}

/*
** Read a varint value from aBuf[] into *piVal. Return the number of 
** bytes read.
*/
static int sessionVarintGet(const u8 *aBuf, int *piVal){
  return getVarint32(aBuf, *piVal);
}

/* Load an unaligned and unsigned 32-bit integer */
#define SESSION_UINT32(x) (((u32)(x)[0]<<24)|((x)[1]<<16)|((x)[2]<<8)|(x)[3])

/*
** Read a 64-bit big-endian integer value from buffer aRec[]. Return
** the value read.
*/
static sqlite3_int64 sessionGetI64(u8 *aRec){
  u64 x = SESSION_UINT32(aRec);
  u32 y = SESSION_UINT32(aRec+4);
  x = (x<<32) + y;
  return (sqlite3_int64)x;
}

/*
** Write a 64-bit big-endian integer value to the buffer aBuf[].
*/
static void sessionPutI64(u8 *aBuf, sqlite3_int64 i){
  aBuf[0] = (i>>56) & 0xFF;
  aBuf[1] = (i>>48) & 0xFF;
  aBuf[2] = (i>>40) & 0xFF;
  aBuf[3] = (i>>32) & 0xFF;
  aBuf[4] = (i>>24) & 0xFF;
  aBuf[5] = (i>>16) & 0xFF;
  aBuf[6] = (i>> 8) & 0xFF;
  aBuf[7] = (i>> 0) & 0xFF;
}

/*
** This function is used to serialize the contents of value pValue (see
** comment titled "RECORD FORMAT" above).
**
** If it is non-NULL, the serialized form of the value is written to 
** buffer aBuf. *pnWrite is set to the number of bytes written before
** returning. Or, if aBuf is NULL, the only thing this function does is
** set *pnWrite.
**
** If no error occurs, SQLITE_OK is returned. Or, if an OOM error occurs
** within a call to sqlite3_value_text() (may fail if the db is utf-16)) 
** SQLITE_NOMEM is returned.
*/
static int sessionSerializeValue(
  u8 *aBuf,                       /* If non-NULL, write serialized value here */
  sqlite3_value *pValue,          /* Value to serialize */
  sqlite3_int64 *pnWrite          /* IN/OUT: Increment by bytes written */
){
  int nByte;                      /* Size of serialized value in bytes */

  if( pValue ){
    int eType;                    /* Value type (SQLITE_NULL, TEXT etc.) */
  
    eType = sqlite3_value_type(pValue);
    if( aBuf ) aBuf[0] = eType;
  
    switch( eType ){
      case SQLITE_NULL: 
        nByte = 1;
        break;
  
      case SQLITE_INTEGER: 
      case SQLITE_FLOAT:
        if( aBuf ){
          /* TODO: SQLite does something special to deal with mixed-endian
          ** floating point values (e.g. ARM7). This code probably should
          ** too.  */
          u64 i;
          if( eType==SQLITE_INTEGER ){
            i = (u64)sqlite3_value_int64(pValue);
          }else{
            double r;
            assert( sizeof(double)==8 && sizeof(u64)==8 );
            r = sqlite3_value_double(pValue);
            memcpy(&i, &r, 8);
          }
          sessionPutI64(&aBuf[1], i);
        }
        nByte = 9; 
        break;
  
      default: {
        u8 *z;
        int n;
        int nVarint;
  
        assert( eType==SQLITE_TEXT || eType==SQLITE_BLOB );
        if( eType==SQLITE_TEXT ){
          z = (u8 *)sqlite3_value_text(pValue);
        }else{
          z = (u8 *)sqlite3_value_blob(pValue);
        }
        n = sqlite3_value_bytes(pValue);
        if( z==0 && (eType!=SQLITE_BLOB || n>0) ) return SQLITE_NOMEM;
        nVarint = sessionVarintLen(n);
  
        if( aBuf ){
          sessionVarintPut(&aBuf[1], n);
          if( n>0 ) memcpy(&aBuf[nVarint + 1], z, n);
        }
  
        nByte = 1 + nVarint + n;
        break;
      }
    }
  }else{
    nByte = 1;
    if( aBuf ) aBuf[0] = '\0';
  }

  if( pnWrite ) *pnWrite += nByte;
  return SQLITE_OK;
}

/*
** Allocate and return a pointer to a buffer nByte bytes in size. If
** pSession is not NULL, increase the sqlite3_session.nMalloc variable
** by the number of bytes allocated.
*/
static void *sessionMalloc64(sqlite3_session *pSession, i64 nByte){
  void *pRet = sqlite3_malloc64(nByte);
  if( pSession ) pSession->nMalloc += sqlite3_msize(pRet);
  return pRet;
}

/*
** Free buffer pFree, which must have been allocated by an earlier
** call to sessionMalloc64(). If pSession is not NULL, decrease the
** sqlite3_session.nMalloc counter by the number of bytes freed.
*/
static void sessionFree(sqlite3_session *pSession, void *pFree){
  if( pSession ) pSession->nMalloc -= sqlite3_msize(pFree);
  sqlite3_free(pFree);
}

/*
** This macro is used to calculate hash key values for data structures. In
** order to use this macro, the entire data structure must be represented
** as a series of unsigned integers. In order to calculate a hash-key value
** for a data structure represented as three such integers, the macro may
** then be used as follows:
**
**    int hash_key_value;
**    hash_key_value = HASH_APPEND(0, <value 1>);
**    hash_key_value = HASH_APPEND(hash_key_value, <value 2>);
**    hash_key_value = HASH_APPEND(hash_key_value, <value 3>);
**
** In practice, the data structures this macro is used for are the primary
** key values of modified rows.
*/
#define HASH_APPEND(hash, add) ((hash) << 3) ^ (hash) ^ (unsigned int)(add)

/*
** Append the hash of the 64-bit integer passed as the second argument to the
** hash-key value passed as the first. Return the new hash-key value.
*/
static unsigned int sessionHashAppendI64(unsigned int h, i64 i){
  h = HASH_APPEND(h, i & 0xFFFFFFFF);
  return HASH_APPEND(h, (i>>32)&0xFFFFFFFF);
}

/*
** Append the hash of the blob passed via the second and third arguments to 
** the hash-key value passed as the first. Return the new hash-key value.
*/
static unsigned int sessionHashAppendBlob(unsigned int h, int n, const u8 *z){
  int i;
  for(i=0; i<n; i++) h = HASH_APPEND(h, z[i]);
  return h;
}

/*
** Append the hash of the data type passed as the second argument to the
** hash-key value passed as the first. Return the new hash-key value.
*/
static unsigned int sessionHashAppendType(unsigned int h, int eType){
  return HASH_APPEND(h, eType);
}

/*
** This function may only be called from within a pre-update callback.
** It calculates a hash based on the primary key values of the old.* or 
** new.* row currently available and, assuming no error occurs, writes it to
** *piHash before returning. If the primary key contains one or more NULL
** values, *pbNullPK is set to true before returning.
**
** If an error occurs, an SQLite error code is returned and the final values
** of *piHash asn *pbNullPK are undefined. Otherwise, SQLITE_OK is returned
** and the output variables are set as described above.
*/
static int sessionPreupdateHash(
  sqlite3_session *pSession,      /* Session object that owns pTab */
  i64 iRowid,
  SessionTable *pTab,             /* Session table handle */
  int bNew,                       /* True to hash the new.* PK */
  int *piHash,                    /* OUT: Hash value */
  int *pbNullPK                   /* OUT: True if there are NULL values in PK */
){
  unsigned int h = 0;             /* Hash value to return */
  int i;                          /* Used to iterate through columns */

  assert( pTab->nTotalCol==pSession->hook.xCount(pSession->hook.pCtx) );
  if( pTab->bRowid ){
    h = sessionHashAppendI64(h, iRowid);
  }else{
    assert( *pbNullPK==0 );
    for(i=0; i<pTab->nCol; i++){
      if( pTab->abPK[i] ){
        int rc;
        int eType;
        sqlite3_value *pVal;
        int iIdx = pTab->aiIdx[i];

        if( bNew ){
          rc = pSession->hook.xNew(pSession->hook.pCtx, iIdx, &pVal);
        }else{
          rc = pSession->hook.xOld(pSession->hook.pCtx, iIdx, &pVal);
        }
        if( rc!=SQLITE_OK ) return rc;

        eType = sqlite3_value_type(pVal);
        h = sessionHashAppendType(h, eType);
        if( eType==SQLITE_INTEGER || eType==SQLITE_FLOAT ){
          i64 iVal;
          if( eType==SQLITE_INTEGER ){
            iVal = sqlite3_value_int64(pVal);
          }else{
            double rVal = sqlite3_value_double(pVal);
            assert( sizeof(iVal)==8 && sizeof(rVal)==8 );
            memcpy(&iVal, &rVal, 8);
          }
          h = sessionHashAppendI64(h, iVal);
        }else if( eType==SQLITE_TEXT || eType==SQLITE_BLOB ){
          const u8 *z;
          int n;
          if( eType==SQLITE_TEXT ){
            z = (const u8 *)sqlite3_value_text(pVal);
          }else{
            z = (const u8 *)sqlite3_value_blob(pVal);
          }
          n = sqlite3_value_bytes(pVal);
          if( !z && (eType!=SQLITE_BLOB || n>0) ) return SQLITE_NOMEM;
          h = sessionHashAppendBlob(h, n, z);
        }else{
          assert( eType==SQLITE_NULL );
          assert( pTab->bStat1==0 || i!=1 );
          *pbNullPK = 1;
        }
      }
    }
  }

  *piHash = (h % pTab->nChange);
  return SQLITE_OK;
}

/*
** The buffer that the argument points to contains a serialized SQL value.
** Return the number of bytes of space occupied by the value (including
** the type byte).
*/
static int sessionSerialLen(const u8 *a){
  int e;
  int n;
  assert( a!=0 );
  e = *a;
  if( e==0 || e==0xFF ) return 1;
  if( e==SQLITE_NULL ) return 1;
  if( e==SQLITE_INTEGER || e==SQLITE_FLOAT ) return 9;
  return sessionVarintGet(&a[1], &n) + 1 + n;
}

/*
** Based on the primary key values stored in change aRecord, calculate a
** hash key. Assume the has table has nBucket buckets. The hash keys
** calculated by this function are compatible with those calculated by
** sessionPreupdateHash().
**
** The bPkOnly argument is non-zero if the record at aRecord[] is from
** a patchset DELETE. In this case the non-PK fields are omitted entirely.
*/
static unsigned int sessionChangeHash(
  SessionTable *pTab,             /* Table handle */
  int bPkOnly,                    /* Record consists of PK fields only */
  u8 *aRecord,                    /* Change record */
  int nBucket                     /* Assume this many buckets in hash table */
){
  unsigned int h = 0;             /* Value to return */
  int i;                          /* Used to iterate through columns */
  u8 *a = aRecord;                /* Used to iterate through change record */

  for(i=0; i<pTab->nCol; i++){
    int eType = *a;
    int isPK = pTab->abPK[i];
    if( bPkOnly && isPK==0 ) continue;

    /* It is not possible for eType to be SQLITE_NULL here. The session 
    ** module does not record changes for rows with NULL values stored in
    ** primary key columns. */
    assert( eType==SQLITE_INTEGER || eType==SQLITE_FLOAT 
         || eType==SQLITE_TEXT || eType==SQLITE_BLOB 
         || eType==SQLITE_NULL || eType==0 
    );
    assert( !isPK || (eType!=0 && eType!=SQLITE_NULL) );

    if( isPK ){
      a++;
      h = sessionHashAppendType(h, eType);
      if( eType==SQLITE_INTEGER || eType==SQLITE_FLOAT ){
        h = sessionHashAppendI64(h, sessionGetI64(a));
        a += 8;
      }else{
        int n; 
        a += sessionVarintGet(a, &n);
        h = sessionHashAppendBlob(h, n, a);
        a += n;
      }
    }else{
      a += sessionSerialLen(a);
    }
  }
  return (h % nBucket);
}

/*
** Arguments aLeft and aRight are pointers to change records for table pTab.
** This function returns true if the two records apply to the same row (i.e.
** have the same values stored in the primary key columns), or false 
** otherwise.
*/
static int sessionChangeEqual(
  SessionTable *pTab,             /* Table used for PK definition */
  int bLeftPkOnly,                /* True if aLeft[] contains PK fields only */
  u8 *aLeft,                      /* Change record */
  int bRightPkOnly,               /* True if aRight[] contains PK fields only */
  u8 *aRight                      /* Change record */
){
  u8 *a1 = aLeft;                 /* Cursor to iterate through aLeft */
  u8 *a2 = aRight;                /* Cursor to iterate through aRight */
  int iCol;                       /* Used to iterate through table columns */

  for(iCol=0; iCol<pTab->nCol; iCol++){
    if( pTab->abPK[iCol] ){
      int n1 = sessionSerialLen(a1);
      int n2 = sessionSerialLen(a2);

      if( n1!=n2 || memcmp(a1, a2, n1) ){
        return 0;
      }
      a1 += n1;
      a2 += n2;
    }else{
      if( bLeftPkOnly==0 ) a1 += sessionSerialLen(a1);
      if( bRightPkOnly==0 ) a2 += sessionSerialLen(a2);
    }
  }

  return 1;
}

/*
** Arguments aLeft and aRight both point to buffers containing change
** records with nCol columns. This function "merges" the two records into
** a single records which is written to the buffer at *paOut. *paOut is
** then set to point to one byte after the last byte written before 
** returning.
**
** The merging of records is done as follows: For each column, if the 
** aRight record contains a value for the column, copy the value from
** their. Otherwise, if aLeft contains a value, copy it. If neither
** record contains a value for a given column, then neither does the
** output record.
*/
static void sessionMergeRecord(
  u8 **paOut, 
  int nCol,
  u8 *aLeft,
  u8 *aRight
){
  u8 *a1 = aLeft;                 /* Cursor used to iterate through aLeft */
  u8 *a2 = aRight;                /* Cursor used to iterate through aRight */
  u8 *aOut = *paOut;              /* Output cursor */
  int iCol;                       /* Used to iterate from 0 to nCol */

  for(iCol=0; iCol<nCol; iCol++){
    int n1 = sessionSerialLen(a1);
    int n2 = sessionSerialLen(a2);
    if( *a2 ){
      memcpy(aOut, a2, n2);
      aOut += n2;
    }else{
      memcpy(aOut, a1, n1);
      aOut += n1;
    }
    a1 += n1;
    a2 += n2;
  }

  *paOut = aOut;
}

/*
** This is a helper function used by sessionMergeUpdate().
**
** When this function is called, both *paOne and *paTwo point to a value 
** within a change record. Before it returns, both have been advanced so 
** as to point to the next value in the record.
**
** If, when this function is called, *paTwo points to a valid value (i.e.
** *paTwo[0] is not 0x00 - the "no value" placeholder), a copy of the *paTwo
** pointer is returned and *pnVal is set to the number of bytes in the 
** serialized value. Otherwise, a copy of *paOne is returned and *pnVal
** set to the number of bytes in the value at *paOne. If *paOne points
** to the "no value" placeholder, *pnVal is set to 1. In other words:
**
**   if( *paTwo is valid ) return *paTwo;
**   return *paOne;
**
*/
static u8 *sessionMergeValue(
  u8 **paOne,                     /* IN/OUT: Left-hand buffer pointer */
  u8 **paTwo,                     /* IN/OUT: Right-hand buffer pointer */
  int *pnVal                      /* OUT: Bytes in returned value */
){
  u8 *a1 = *paOne;
  u8 *a2 = *paTwo;
  u8 *pRet = 0;
  int n1;

  assert( a1 );
  if( a2 ){
    int n2 = sessionSerialLen(a2);
    if( *a2 ){
      *pnVal = n2;
      pRet = a2;
    }
    *paTwo = &a2[n2];
  }

  n1 = sessionSerialLen(a1);
  if( pRet==0 ){
    *pnVal = n1;
    pRet = a1;
  }
  *paOne = &a1[n1];

  return pRet;
}

/*
** This function is used by changeset_concat() to merge two UPDATE changes
** on the same row.
*/
static int sessionMergeUpdate(
  u8 **paOut,                     /* IN/OUT: Pointer to output buffer */
  SessionTable *pTab,             /* Table change pertains to */
  int bPatchset,                  /* True if records are patchset records */
  u8 *aOldRecord1,                /* old.* record for first change */
  u8 *aOldRecord2,                /* old.* record for second change */
  u8 *aNewRecord1,                /* new.* record for first change */
  u8 *aNewRecord2                 /* new.* record for second change */
){
  u8 *aOld1 = aOldRecord1;
  u8 *aOld2 = aOldRecord2;
  u8 *aNew1 = aNewRecord1;
  u8 *aNew2 = aNewRecord2;

  u8 *aOut = *paOut;
  int i;

  if( bPatchset==0 ){
    int bRequired = 0;

    assert( aOldRecord1 && aNewRecord1 );

    /* Write the old.* vector first. */
    for(i=0; i<pTab->nCol; i++){
      int nOld;
      u8 *aOld;
      int nNew;
      u8 *aNew;

      aOld = sessionMergeValue(&aOld1, &aOld2, &nOld);
      aNew = sessionMergeValue(&aNew1, &aNew2, &nNew);
      if( pTab->abPK[i] || nOld!=nNew || memcmp(aOld, aNew, nNew) ){
        if( pTab->abPK[i]==0 ) bRequired = 1;
        memcpy(aOut, aOld, nOld);
        aOut += nOld;
      }else{
        *(aOut++) = '\0';
      }
    }

    if( !bRequired ) return 0;
  }

  /* Write the new.* vector */
  aOld1 = aOldRecord1;
  aOld2 = aOldRecord2;
  aNew1 = aNewRecord1;
  aNew2 = aNewRecord2;
  for(i=0; i<pTab->nCol; i++){
    int nOld;
    u8 *aOld;
    int nNew;
    u8 *aNew;

    aOld = sessionMergeValue(&aOld1, &aOld2, &nOld);
    aNew = sessionMergeValue(&aNew1, &aNew2, &nNew);
    if( bPatchset==0 
     && (pTab->abPK[i] || (nOld==nNew && 0==memcmp(aOld, aNew, nNew))) 
    ){
      *(aOut++) = '\0';
    }else{
      memcpy(aOut, aNew, nNew);
      aOut += nNew;
    }
  }

  *paOut = aOut;
  return 1;
}

/*
** This function is only called from within a pre-update-hook callback.
** It determines if the current pre-update-hook change affects the same row
** as the change stored in argument pChange. If so, it returns true. Otherwise
** if the pre-update-hook does not affect the same row as pChange, it returns
** false.
*/
static int sessionPreupdateEqual(
  sqlite3_session *pSession,      /* Session object that owns SessionTable */
  i64 iRowid,                     /* Rowid value if pTab->bRowid */
  SessionTable *pTab,             /* Table associated with change */
  SessionChange *pChange,         /* Change to compare to */
  int op                          /* Current pre-update operation */
){
  int iCol;                       /* Used to iterate through columns */
  u8 *a = pChange->aRecord;       /* Cursor used to scan change record */

  if( pTab->bRowid ){
    if( a[0]!=SQLITE_INTEGER ) return 0;
    return sessionGetI64(&a[1])==iRowid;
  }

  assert( op==SQLITE_INSERT || op==SQLITE_UPDATE || op==SQLITE_DELETE );
  for(iCol=0; iCol<pTab->nCol; iCol++){
    if( !pTab->abPK[iCol] ){
      a += sessionSerialLen(a);
    }else{
      sqlite3_value *pVal;        /* Value returned by preupdate_new/old */
      int rc;                     /* Error code from preupdate_new/old */
      int eType = *a++;           /* Type of value from change record */
      int iIdx = pTab->aiIdx[iCol];

      /* The following calls to preupdate_new() and preupdate_old() can not
      ** fail. This is because they cache their return values, and by the
      ** time control flows to here they have already been called once from
      ** within sessionPreupdateHash(). The first two asserts below verify
      ** this (that the method has already been called). */
      if( op==SQLITE_INSERT ){
        /* assert( db->pPreUpdate->pNewUnpacked || db->pPreUpdate->aNew ); */
        rc = pSession->hook.xNew(pSession->hook.pCtx, iIdx, &pVal);
      }else{
        /* assert( db->pPreUpdate->pUnpacked ); */
        rc = pSession->hook.xOld(pSession->hook.pCtx, iIdx, &pVal);
      }
      assert( rc==SQLITE_OK );
      (void)rc;                   /* Suppress warning about unused variable */
      if( sqlite3_value_type(pVal)!=eType ) return 0;

      /* A SessionChange object never has a NULL value in a PK column */
      assert( eType==SQLITE_INTEGER || eType==SQLITE_FLOAT
           || eType==SQLITE_BLOB    || eType==SQLITE_TEXT
      );

      if( eType==SQLITE_INTEGER || eType==SQLITE_FLOAT ){
        i64 iVal = sessionGetI64(a);
        a += 8;
        if( eType==SQLITE_INTEGER ){
          if( sqlite3_value_int64(pVal)!=iVal ) return 0;
        }else{
          double rVal;
          assert( sizeof(iVal)==8 && sizeof(rVal)==8 );
          memcpy(&rVal, &iVal, 8);
          if( sqlite3_value_double(pVal)!=rVal ) return 0;
        }
      }else{
        int n;
        const u8 *z;
        a += sessionVarintGet(a, &n);
        if( sqlite3_value_bytes(pVal)!=n ) return 0;
        if( eType==SQLITE_TEXT ){
          z = sqlite3_value_text(pVal);
        }else{
          z = sqlite3_value_blob(pVal);
        }
        if( n>0 && memcmp(a, z, n) ) return 0;
        a += n;
      }
    }
  }

  return 1;
}

/*
** If required, grow the hash table used to store changes on table pTab 
** (part of the session pSession). If a fatal OOM error occurs, set the
** session object to failed and return SQLITE_ERROR. Otherwise, return
** SQLITE_OK.
**
** It is possible that a non-fatal OOM error occurs in this function. In
** that case the hash-table does not grow, but SQLITE_OK is returned anyway.
** Growing the hash table in this case is a performance optimization only,
** it is not required for correct operation.
*/
static int sessionGrowHash(
  sqlite3_session *pSession,      /* For memory accounting. May be NULL */
  int bPatchset, 
  SessionTable *pTab
){
  if( pTab->nChange==0 || pTab->nEntry>=(pTab->nChange/2) ){
    int i;
    SessionChange **apNew;
    sqlite3_int64 nNew = 2*(sqlite3_int64)(pTab->nChange ? pTab->nChange : 128);

    apNew = (SessionChange**)sessionMalloc64(
        pSession, sizeof(SessionChange*) * nNew
    );
    if( apNew==0 ){
      if( pTab->nChange==0 ){
        return SQLITE_ERROR;
      }
      return SQLITE_OK;
    }
    memset(apNew, 0, sizeof(SessionChange *) * nNew);

    for(i=0; i<pTab->nChange; i++){
      SessionChange *p;
      SessionChange *pNext;
      for(p=pTab->apChange[i]; p; p=pNext){
        int bPkOnly = (p->op==SQLITE_DELETE && bPatchset);
        int iHash = sessionChangeHash(pTab, bPkOnly, p->aRecord, nNew);
        pNext = p->pNext;
        p->pNext = apNew[iHash];
        apNew[iHash] = p;
      }
    }

    sessionFree(pSession, pTab->apChange);
    pTab->nChange = nNew;
    pTab->apChange = apNew;
  }

  return SQLITE_OK;
}

/*
** This function queries the database for the names of the columns of table
** zThis, in schema zDb.
**
** Otherwise, if they are not NULL, variable *pnCol is set to the number
** of columns in the database table and variable *pzTab is set to point to a
** nul-terminated copy of the table name. *pazCol (if not NULL) is set to
** point to an array of pointers to column names. And *pabPK (again, if not
** NULL) is set to point to an array of booleans - true if the corresponding
** column is part of the primary key.
**
** For example, if the table is declared as:
**
**     CREATE TABLE tbl1(w, x DEFAULT 'abc', y, z, PRIMARY KEY(w, z));
**
** Then the five output variables are populated as follows:
**
**     *pnCol  = 4
**     *pzTab  = "tbl1"
**     *pazCol = {"w", "x", "y", "z"}
**     *pazDflt = {NULL, 'abc', NULL, NULL}
**     *pabPK  = {1, 0, 0, 1}
**
** All returned buffers are part of the same single allocation, which must
** be freed using sqlite3_free() by the caller
*/
static int sessionTableInfo(
  sqlite3_session *pSession,      /* For memory accounting. May be NULL */
  sqlite3 *db,                    /* Database connection */
  const char *zDb,                /* Name of attached database (e.g. "main") */
  const char *zThis,              /* Table name */
  int *pnCol,                     /* OUT: number of columns */
  int *pnTotalCol,                /* OUT: number of hidden columns */
  const char **pzTab,             /* OUT: Copy of zThis */
  const char ***pazCol,           /* OUT: Array of column names for table */
  const char ***pazDflt,          /* OUT: Array of default value expressions */
  int **paiIdx,                   /* OUT: Array of xNew/xOld indexes */
  u8 **pabPK,                     /* OUT: Array of booleans - true for PK col */
  int *pbRowid                    /* OUT: True if only PK is a rowid */
){
  char *zPragma;
  sqlite3_stmt *pStmt;
  int rc;
  sqlite3_int64 nByte;
  int nDbCol = 0;
  int nThis;
  int i;
  u8 *pAlloc = 0;
  char **azCol = 0;
  char **azDflt = 0;
  u8 *abPK = 0;
  int *aiIdx = 0;
  int bRowid = 0;                 /* Set to true to use rowid as PK */

  assert( pazCol && pabPK );

  *pazCol = 0;
  *pabPK = 0;
  *pnCol = 0;
  if( pnTotalCol ) *pnTotalCol = 0;
  if( paiIdx ) *paiIdx = 0;
  if( pzTab ) *pzTab = 0;
  if( pazDflt ) *pazDflt = 0;

  nThis = sqlite3Strlen30(zThis);
  if( nThis==12 && 0==sqlite3_stricmp("sqlite_stat1", zThis) ){
    rc = sqlite3_table_column_metadata(db, zDb, zThis, 0, 0, 0, 0, 0, 0);
    if( rc==SQLITE_OK ){
      /* For sqlite_stat1, pretend that (tbl,idx) is the PRIMARY KEY. */
      zPragma = sqlite3_mprintf(
          "SELECT 0, 'tbl',  '', 0, '', 1, 0     UNION ALL "
          "SELECT 1, 'idx',  '', 0, '', 2, 0     UNION ALL "
          "SELECT 2, 'stat', '', 0, '', 0, 0"
      );
    }else if( rc==SQLITE_ERROR ){
      zPragma = sqlite3_mprintf("");
    }else{
      return rc;
    }
  }else{
    zPragma = sqlite3_mprintf("PRAGMA '%q'.table_xinfo('%q')", zDb, zThis);
  }
  if( !zPragma ){
    return SQLITE_NOMEM;
  }

  rc = sqlite3_prepare_v2(db, zPragma, -1, &pStmt, 0);
  sqlite3_free(zPragma);
  if( rc!=SQLITE_OK ){
    return rc;
  }

  nByte = nThis + 1;
  bRowid = (pbRowid!=0);
  while( SQLITE_ROW==sqlite3_step(pStmt) ){
    nByte += sqlite3_column_bytes(pStmt, 1);          /* name */
    nByte += sqlite3_column_bytes(pStmt, 4);          /* dflt_value */
    if( sqlite3_column_int(pStmt, 6)==0 ){            /* !hidden */
      nDbCol++;
    }
    if( sqlite3_column_int(pStmt, 5) ) bRowid = 0;    /* pk */
  }
  if( nDbCol==0 ) bRowid = 0;
  nDbCol += bRowid;
  nByte += strlen(SESSIONS_ROWID);
  rc = sqlite3_reset(pStmt);

  if( rc==SQLITE_OK ){
    nByte += nDbCol * (sizeof(const char *)*2 +sizeof(int)+sizeof(u8) + 1 + 1);
    pAlloc = sessionMalloc64(pSession, nByte);
    if( pAlloc==0 ){
      rc = SQLITE_NOMEM;
    }else{
      memset(pAlloc, 0, nByte);
    }
  }
  if( rc==SQLITE_OK ){
    azCol = (char **)pAlloc;
    azDflt = (char**)&azCol[nDbCol];
    aiIdx = (int*)&azDflt[nDbCol];
    abPK = (u8 *)&aiIdx[nDbCol];
    pAlloc = &abPK[nDbCol];
    if( pzTab ){
      memcpy(pAlloc, zThis, nThis+1);
      *pzTab = (char *)pAlloc;
      pAlloc += nThis+1;
    }
  
    i = 0;
    if( bRowid ){
      size_t nName = strlen(SESSIONS_ROWID);
      memcpy(pAlloc, SESSIONS_ROWID, nName+1);
      azCol[i] = (char*)pAlloc;
      pAlloc += nName+1;
      abPK[i] = 1;
      aiIdx[i] = -1;
      i++;
    }
    while( SQLITE_ROW==sqlite3_step(pStmt) ){
      if( sqlite3_column_int(pStmt, 6)==0 ){            /* !hidden */
        int nName = sqlite3_column_bytes(pStmt, 1);
        int nDflt = sqlite3_column_bytes(pStmt, 4);
        const unsigned char *zName = sqlite3_column_text(pStmt, 1);
        const unsigned char *zDflt = sqlite3_column_text(pStmt, 4);

        if( zName==0 ) break;
        memcpy(pAlloc, zName, nName+1);
        azCol[i] = (char *)pAlloc;
        pAlloc += nName+1;
        if( zDflt ){
          memcpy(pAlloc, zDflt, nDflt+1);
          azDflt[i] = (char *)pAlloc;
          pAlloc += nDflt+1;
        }else{
          azDflt[i] = 0;
        }
        abPK[i] = sqlite3_column_int(pStmt, 5);
        aiIdx[i] = sqlite3_column_int(pStmt, 0);
        i++;
      }
      if( pnTotalCol ) (*pnTotalCol)++;
    }
    rc = sqlite3_reset(pStmt);
  }

  /* If successful, populate the output variables. Otherwise, zero them and
  ** free any allocation made. An error code will be returned in this case.
  */
  if( rc==SQLITE_OK ){
    *pazCol = (const char**)azCol;
    if( pazDflt ) *pazDflt = (const char**)azDflt;
    *pabPK = abPK;
    *pnCol = nDbCol;
    if( paiIdx ) *paiIdx = aiIdx;
  }else{
    sessionFree(pSession, azCol);
  }
  if( pbRowid ) *pbRowid = bRowid;
  sqlite3_finalize(pStmt);
  return rc;
}

/*
** This function is called to initialize the SessionTable.nCol, azCol[]
** abPK[] and azDflt[] members of SessionTable object pTab. If these
** fields are already initialized, this function is a no-op.
**
** If an error occurs, an error code is stored in sqlite3_session.rc and
** non-zero returned. Or, if no error occurs but the table has no primary
** key, sqlite3_session.rc is left set to SQLITE_OK and non-zero returned to
** indicate that updates on this table should be ignored. SessionTable.abPK 
** is set to NULL in this case.
*/
static int sessionInitTable(
  sqlite3_session *pSession,      /* Optional session handle */
  SessionTable *pTab,             /* Table object to initialize */
  sqlite3 *db,                    /* Database handle to read schema from */
  const char *zDb                 /* Name of db - "main", "temp" etc. */
){
  int rc = SQLITE_OK;

  if( pTab->nCol==0 ){
    u8 *abPK;
    assert( pTab->azCol==0 || pTab->abPK==0 );
    sqlite3_free(pTab->azCol);
    pTab->abPK = 0;
    rc = sessionTableInfo(pSession, db, zDb, 
        pTab->zName, &pTab->nCol, &pTab->nTotalCol, 0, &pTab->azCol, 
        &pTab->azDflt, &pTab->aiIdx, &abPK,
        ((pSession==0 || pSession->bImplicitPK) ? &pTab->bRowid : 0)
    );
    if( rc==SQLITE_OK ){
      int i;
      for(i=0; i<pTab->nCol; i++){
        if( abPK[i] ){
          pTab->abPK = abPK;
          break;
        }
      }
      if( 0==sqlite3_stricmp("sqlite_stat1", pTab->zName) ){
        pTab->bStat1 = 1;
      }

      if( pSession && pSession->bEnableSize ){
        pSession->nMaxChangesetSize += (
          1 + sessionVarintLen(pTab->nCol) + pTab->nCol + strlen(pTab->zName)+1
        );
      }
    }
  }

  if( pSession ){
    pSession->rc = rc;
    return (rc || pTab->abPK==0);
  }
  return rc;
}

/*
** Re-initialize table object pTab.
*/
static int sessionReinitTable(sqlite3_session *pSession, SessionTable *pTab){
  int nCol = 0;
  int nTotalCol = 0;
  const char **azCol = 0;
  const char **azDflt = 0;
  int *aiIdx = 0;
  u8 *abPK = 0; 
  int bRowid = 0;

  assert( pSession->rc==SQLITE_OK );

  pSession->rc = sessionTableInfo(pSession, pSession->db, pSession->zDb, 
      pTab->zName, &nCol, &nTotalCol, 0, &azCol, &azDflt, &aiIdx, &abPK,
      (pSession->bImplicitPK ? &bRowid : 0)
  );
  if( pSession->rc==SQLITE_OK ){
    if( pTab->nCol>nCol || pTab->bRowid!=bRowid ){
      pSession->rc = SQLITE_SCHEMA;
    }else{
      int ii;
      int nOldCol = pTab->nCol;
      for(ii=0; ii<nCol; ii++){
        if( ii<pTab->nCol ){
          if( pTab->abPK[ii]!=abPK[ii] ){
            pSession->rc = SQLITE_SCHEMA;
          }
        }else if( abPK[ii] ){
          pSession->rc = SQLITE_SCHEMA;
        }
      }

      if( pSession->rc==SQLITE_OK ){
        const char **a = pTab->azCol;
        pTab->azCol = azCol;
        pTab->nCol = nCol;
        pTab->nTotalCol = nTotalCol;
        pTab->azDflt = azDflt;
        pTab->abPK = abPK;
        pTab->aiIdx = aiIdx;
        azCol = a;
      }
      if( pSession->bEnableSize ){
        pSession->nMaxChangesetSize += (nCol - nOldCol);
        pSession->nMaxChangesetSize += sessionVarintLen(nCol);
        pSession->nMaxChangesetSize -= sessionVarintLen(nOldCol);
      }
    }
  }

  sqlite3_free((char*)azCol);
  return pSession->rc;
}

/*
** Session-change object (*pp) contains an old.* record with fewer than
** nCol fields. This function updates it with the default values for
** the missing fields.
*/
static void sessionUpdateOneChange(
  sqlite3_session *pSession,      /* For memory accounting */
  int *pRc,                       /* IN/OUT: Error code */
  SessionChange **pp,             /* IN/OUT: Change object to update */
  int nCol,                       /* Number of columns now in table */
  sqlite3_stmt *pDflt             /* SELECT <default-values...> */
){
  SessionChange *pOld = *pp;

  while( pOld->nRecordField<nCol ){
    SessionChange *pNew = 0;
    int nByte = 0;
    int nIncr = 0;
    int iField = pOld->nRecordField;
    int eType = sqlite3_column_type(pDflt, iField);
    switch( eType ){
      case SQLITE_NULL:
        nIncr = 1;
        break;
      case SQLITE_INTEGER:
      case SQLITE_FLOAT:
        nIncr = 9;
        break;
      default: {
        int n = sqlite3_column_bytes(pDflt, iField);
        nIncr = 1 + sessionVarintLen(n) + n;
        assert( eType==SQLITE_TEXT || eType==SQLITE_BLOB );
        break;
      }
    }

    nByte = nIncr + (sizeof(SessionChange) + pOld->nRecord);
    pNew = sessionMalloc64(pSession, nByte);
    if( pNew==0 ){
      *pRc = SQLITE_NOMEM;
      return;
    }else{
      memcpy(pNew, pOld, sizeof(SessionChange));
      pNew->aRecord = (u8*)&pNew[1];
      memcpy(pNew->aRecord, pOld->aRecord, pOld->nRecord);
      pNew->aRecord[pNew->nRecord++] = (u8)eType;
      switch( eType ){
        case SQLITE_INTEGER: {
          i64 iVal = sqlite3_column_int64(pDflt, iField);
          sessionPutI64(&pNew->aRecord[pNew->nRecord], iVal); 
          pNew->nRecord += 8;
          break;
        }
          
        case SQLITE_FLOAT: {
          double rVal = sqlite3_column_double(pDflt, iField);
          i64 iVal = 0;
          memcpy(&iVal, &rVal, sizeof(rVal));
          sessionPutI64(&pNew->aRecord[pNew->nRecord], iVal); 
          pNew->nRecord += 8;
          break;
        }

        case SQLITE_TEXT: {
          int n = sqlite3_column_bytes(pDflt, iField);
          const char *z = (const char*)sqlite3_column_text(pDflt, iField);
          pNew->nRecord += sessionVarintPut(&pNew->aRecord[pNew->nRecord], n); 
          memcpy(&pNew->aRecord[pNew->nRecord], z, n);
          pNew->nRecord += n;
          break;
        }

        case SQLITE_BLOB: {
          int n = sqlite3_column_bytes(pDflt, iField);
          const u8 *z = (const u8*)sqlite3_column_blob(pDflt, iField);
          pNew->nRecord += sessionVarintPut(&pNew->aRecord[pNew->nRecord], n); 
          memcpy(&pNew->aRecord[pNew->nRecord], z, n);
          pNew->nRecord += n;
          break;
        }

        default:
          assert( eType==SQLITE_NULL );
          break;
      }

      sessionFree(pSession, pOld);
      *pp = pOld = pNew;
      pNew->nRecordField++;
      pNew->nMaxSize += nIncr;
      if( pSession ){
        pSession->nMaxChangesetSize += nIncr;
      }
    }
  }
}

/*
** Ensure that there is room in the buffer to append nByte bytes of data.
** If not, use sqlite3_realloc() to grow the buffer so that there is.
**
** If successful, return zero. Otherwise, if an OOM condition is encountered,
** set *pRc to SQLITE_NOMEM and return non-zero.
*/
static int sessionBufferGrow(SessionBuffer *p, i64 nByte, int *pRc){
#define SESSION_MAX_BUFFER_SZ (0x7FFFFF00 - 1) 
  i64 nReq = p->nBuf + nByte;
  if( *pRc==SQLITE_OK && nReq>p->nAlloc ){
    u8 *aNew;
    i64 nNew = p->nAlloc ? p->nAlloc : 128;

    do {
      nNew = nNew*2;
    }while( nNew<nReq );

    /* The value of SESSION_MAX_BUFFER_SZ is copied from the implementation
    ** of sqlite3_realloc64(). Allocations greater than this size in bytes
    ** always fail. It is used here to ensure that this routine can always
    ** allocate up to this limit - instead of up to the largest power of
    ** two smaller than the limit.  */
    if( nNew>SESSION_MAX_BUFFER_SZ ){
      nNew = SESSION_MAX_BUFFER_SZ;
      if( nNew<nReq ){
        *pRc = SQLITE_NOMEM;
        return 1;
      }
    }

    aNew = (u8 *)sqlite3_realloc64(p->aBuf, nNew);
    if( 0==aNew ){
      *pRc = SQLITE_NOMEM;
    }else{
      p->aBuf = aNew;
      p->nAlloc = nNew;
    }
  }
  return (*pRc!=SQLITE_OK);
}


/*
** This function is a no-op if *pRc is other than SQLITE_OK when it is 
** called. Otherwise, append a string to the buffer. All bytes in the string
** up to (but not including) the nul-terminator are written to the buffer.
**
** If an OOM condition is encountered, set *pRc to SQLITE_NOMEM before
** returning.
*/
static void sessionAppendStr(
  SessionBuffer *p, 
  const char *zStr, 
  int *pRc
){
  int nStr = sqlite3Strlen30(zStr);
  if( 0==sessionBufferGrow(p, nStr+1, pRc) ){
    memcpy(&p->aBuf[p->nBuf], zStr, nStr);
    p->nBuf += nStr;
    p->aBuf[p->nBuf] = 0x00;
  }
}

/*
** Format a string using printf() style formatting and then append it to the
** buffer using sessionAppendString().
*/
static void sessionAppendPrintf(
  SessionBuffer *p,               /* Buffer to append to */
  int *pRc, 
  const char *zFmt,
  ...
){
  if( *pRc==SQLITE_OK ){
    char *zApp = 0;
    va_list ap;
    va_start(ap, zFmt);
    zApp = sqlite3_vmprintf(zFmt, ap);
    if( zApp==0 ){
      *pRc = SQLITE_NOMEM;
    }else{
      sessionAppendStr(p, zApp, pRc);
    }
    va_end(ap);
    sqlite3_free(zApp);
  }
}

/*
** Prepare a statement against database handle db that SELECTs a single
** row containing the default values for each column in table pTab. For
** example, if pTab is declared as:
**
**   CREATE TABLE pTab(a PRIMARY KEY, b DEFAULT 123, c DEFAULT 'abcd');
**
** Then this function prepares and returns the SQL statement:
**
**   SELECT NULL, 123, 'abcd';
*/
static int sessionPrepareDfltStmt(
  sqlite3 *db,                    /* Database handle */
  SessionTable *pTab,             /* Table to prepare statement for */
  sqlite3_stmt **ppStmt           /* OUT: Statement handle */
){
  SessionBuffer sql = {0,0,0};
  int rc = SQLITE_OK;
  const char *zSep = " ";
  int ii = 0;

  *ppStmt = 0;
  sessionAppendPrintf(&sql, &rc, "SELECT");
  for(ii=0; ii<pTab->nCol; ii++){
    const char *zDflt = pTab->azDflt[ii] ? pTab->azDflt[ii] : "NULL";
    sessionAppendPrintf(&sql, &rc, "%s%s", zSep, zDflt);
    zSep = ", ";
  }
  if( rc==SQLITE_OK ){
    rc = sqlite3_prepare_v2(db, (const char*)sql.aBuf, -1, ppStmt, 0);
  }
  sqlite3_free(sql.aBuf);

  return rc;
}

/*
** Table pTab has one or more existing change-records with old.* records
** with fewer than pTab->nCol columns. This function updates all such 
** change-records with the default values for the missing columns.
*/
static int sessionUpdateChanges(sqlite3_session *pSession, SessionTable *pTab){
  sqlite3_stmt *pStmt = 0;
  int rc = pSession->rc;

  rc = sessionPrepareDfltStmt(pSession->db, pTab, &pStmt);
  if( rc==SQLITE_OK && SQLITE_ROW==sqlite3_step(pStmt) ){
    int ii = 0;
    SessionChange **pp = 0;
    for(ii=0; ii<pTab->nChange; ii++){
      for(pp=&pTab->apChange[ii]; *pp; pp=&((*pp)->pNext)){
        if( (*pp)->nRecordField!=pTab->nCol ){
          sessionUpdateOneChange(pSession, &rc, pp, pTab->nCol, pStmt);
        }
      }
    }
  }

  pSession->rc = rc;
  rc = sqlite3_finalize(pStmt);
  if( pSession->rc==SQLITE_OK ) pSession->rc = rc;
  return pSession->rc;
}

/*
** Versions of the four methods in object SessionHook for use with the
** sqlite_stat1 table. The purpose of this is to substitute a zero-length
** blob each time a NULL value is read from the "idx" column of the
** sqlite_stat1 table.
*/
typedef struct SessionStat1Ctx SessionStat1Ctx;
struct SessionStat1Ctx {
  SessionHook hook;
  sqlite3_session *pSession;
};
static int sessionStat1Old(void *pCtx, int iCol, sqlite3_value **ppVal){
  SessionStat1Ctx *p = (SessionStat1Ctx*)pCtx;
  sqlite3_value *pVal = 0;
  int rc = p->hook.xOld(p->hook.pCtx, iCol, &pVal);
  if( rc==SQLITE_OK && iCol==1 && sqlite3_value_type(pVal)==SQLITE_NULL ){
    pVal = p->pSession->pZeroBlob;
  }
  *ppVal = pVal;
  return rc;
}
static int sessionStat1New(void *pCtx, int iCol, sqlite3_value **ppVal){
  SessionStat1Ctx *p = (SessionStat1Ctx*)pCtx;
  sqlite3_value *pVal = 0;
  int rc = p->hook.xNew(p->hook.pCtx, iCol, &pVal);
  if( rc==SQLITE_OK && iCol==1 && sqlite3_value_type(pVal)==SQLITE_NULL ){
    pVal = p->pSession->pZeroBlob;
  }
  *ppVal = pVal;
  return rc;
}
static int sessionStat1Count(void *pCtx){
  SessionStat1Ctx *p = (SessionStat1Ctx*)pCtx;
  return p->hook.xCount(p->hook.pCtx);
}
static int sessionStat1Depth(void *pCtx){
  SessionStat1Ctx *p = (SessionStat1Ctx*)pCtx;
  return p->hook.xDepth(p->hook.pCtx);
}

static int sessionUpdateMaxSize(
  int op,
  sqlite3_session *pSession,      /* Session object pTab is attached to */
  SessionTable *pTab,             /* Table that change applies to */
  SessionChange *pC               /* Update pC->nMaxSize */
){
  i64 nNew = 2;
  if( pC->op==SQLITE_INSERT ){
    if( pTab->bRowid ) nNew += 9;
    if( op!=SQLITE_DELETE ){
      int ii;
      for(ii=0; ii<pTab->nCol; ii++){
        sqlite3_value *p = 0;
        pSession->hook.xNew(pSession->hook.pCtx, pTab->aiIdx[ii], &p);
        sessionSerializeValue(0, p, &nNew);
      }
    }
  }else if( op==SQLITE_DELETE ){
    nNew += pC->nRecord;
    if( sqlite3_preupdate_blobwrite(pSession->db)>=0 ){
      nNew += pC->nRecord;
    }
  }else{
    int ii;
    u8 *pCsr = pC->aRecord;
    if( pTab->bRowid ){
      nNew += 9 + 1;
      pCsr += 9;
    }
    for(ii=pTab->bRowid; ii<pTab->nCol; ii++){
      int bChanged = 1;
      int nOld = 0;
      int eType;
      int iIdx = pTab->aiIdx[ii];
      sqlite3_value *p = 0;
      pSession->hook.xNew(pSession->hook.pCtx, iIdx, &p);
      if( p==0 ){
        return SQLITE_NOMEM;
      }

      eType = *pCsr++;
      switch( eType ){
        case SQLITE_NULL:
          bChanged = sqlite3_value_type(p)!=SQLITE_NULL;
          break;

        case SQLITE_FLOAT:
        case SQLITE_INTEGER: {
          if( eType==sqlite3_value_type(p) ){
            sqlite3_int64 iVal = sessionGetI64(pCsr);
            if( eType==SQLITE_INTEGER ){
              bChanged = (iVal!=sqlite3_value_int64(p));
            }else{
              double dVal;
              memcpy(&dVal, &iVal, 8);
              bChanged = (dVal!=sqlite3_value_double(p));
            }
          }
          nOld = 8;
          pCsr += 8;
          break;
        }

        default: {
          int nByte;
          nOld = sessionVarintGet(pCsr, &nByte);
          pCsr += nOld;
          nOld += nByte;
          assert( eType==SQLITE_TEXT || eType==SQLITE_BLOB );
          if( eType==sqlite3_value_type(p) 
           && nByte==sqlite3_value_bytes(p)
           && (nByte==0 || 0==memcmp(pCsr, sqlite3_value_blob(p), nByte))
          ){
            bChanged = 0;
          }
          pCsr += nByte;
          break;
        }
      }

      if( bChanged && pTab->abPK[ii] ){
        nNew = pC->nRecord + 2;
        break;
      }

      if( bChanged ){
        nNew += 1 + nOld;
        sessionSerializeValue(0, p, &nNew);
      }else if( pTab->abPK[ii] ){
        nNew += 2 + nOld;
      }else{
        nNew += 2;
      }
    }
  }

  if( nNew>pC->nMaxSize ){
    int nIncr = nNew - pC->nMaxSize;
    pC->nMaxSize = nNew;
    pSession->nMaxChangesetSize += nIncr;
  }
  return SQLITE_OK;
}

/*
** This function is only called from with a pre-update-hook reporting a 
** change on table pTab (attached to session pSession). The type of change
** (UPDATE, INSERT, DELETE) is specified by the first argument.
**
** Unless one is already present or an error occurs, an entry is added
** to the changed-rows hash table associated with table pTab.
*/
static void sessionPreupdateOneChange(
  int op,                         /* One of SQLITE_UPDATE, INSERT, DELETE */
  i64 iRowid,
  sqlite3_session *pSession,      /* Session object pTab is attached to */
  SessionTable *pTab              /* Table that change applies to */
){
  int iHash; 
  int bNull = 0; 
  int rc = SQLITE_OK;
  int nExpect = 0;
  SessionStat1Ctx stat1 = {{0,0,0,0,0},0};

  if( pSession->rc ) return;

  /* Load table details if required */
  if( sessionInitTable(pSession, pTab, pSession->db, pSession->zDb) ) return;

  /* Check the number of columns in this xPreUpdate call matches the 
  ** number of columns in the table.  */
  nExpect = pSession->hook.xCount(pSession->hook.pCtx);
  if( pTab->nTotalCol<nExpect ){
    if( sessionReinitTable(pSession, pTab) ) return;
    if( sessionUpdateChanges(pSession, pTab) ) return;
  }
  if( pTab->nTotalCol!=nExpect ){
    pSession->rc = SQLITE_SCHEMA;
    return;
  }

  /* Grow the hash table if required */
  if( sessionGrowHash(pSession, 0, pTab) ){
    pSession->rc = SQLITE_NOMEM;
    return;
  }

  if( pTab->bStat1 ){
    stat1.hook = pSession->hook;
    stat1.pSession = pSession;
    pSession->hook.pCtx = (void*)&stat1;
    pSession->hook.xNew = sessionStat1New;
    pSession->hook.xOld = sessionStat1Old;
    pSession->hook.xCount = sessionStat1Count;
    pSession->hook.xDepth = sessionStat1Depth;
    if( pSession->pZeroBlob==0 ){
      sqlite3_value *p = sqlite3ValueNew(0);
      if( p==0 ){
        rc = SQLITE_NOMEM;
        goto error_out;
      }
      sqlite3ValueSetStr(p, 0, "", 0, SQLITE_STATIC);
      pSession->pZeroBlob = p;
    }
  }

  /* Calculate the hash-key for this change. If the primary key of the row
  ** includes a NULL value, exit early. Such changes are ignored by the
  ** session module. */
  rc = sessionPreupdateHash(
      pSession, iRowid, pTab, op==SQLITE_INSERT, &iHash, &bNull
  );
  if( rc!=SQLITE_OK ) goto error_out;

  if( bNull==0 ){
    /* Search the hash table for an existing record for this row. */
    SessionChange *pC;
    for(pC=pTab->apChange[iHash]; pC; pC=pC->pNext){
      if( sessionPreupdateEqual(pSession, iRowid, pTab, pC, op) ) break;
    }

    if( pC==0 ){
      /* Create a new change object containing all the old values (if
      ** this is an SQLITE_UPDATE or SQLITE_DELETE), or just the PK
      ** values (if this is an INSERT). */
      sqlite3_int64 nByte;    /* Number of bytes to allocate */
      int i;                  /* Used to iterate through columns */
  
      assert( rc==SQLITE_OK );
      pTab->nEntry++;
  
      /* Figure out how large an allocation is required */
      nByte = sizeof(SessionChange);
      for(i=pTab->bRowid; i<pTab->nCol; i++){
        int iIdx = pTab->aiIdx[i];
        sqlite3_value *p = 0;
        if( op!=SQLITE_INSERT ){
          /* This may fail if the column has a non-NULL default and was added 
          ** using ALTER TABLE ADD COLUMN after this record was created. */
          rc = pSession->hook.xOld(pSession->hook.pCtx, iIdx, &p);
        }else if( pTab->abPK[i] ){
          TESTONLY(int trc = ) pSession->hook.xNew(pSession->hook.pCtx,iIdx,&p);
          assert( trc==SQLITE_OK );
        }

        if( rc==SQLITE_OK ){
          /* This may fail if SQLite value p contains a utf-16 string that must
          ** be converted to utf-8 and an OOM error occurs while doing so. */
          rc = sessionSerializeValue(0, p, &nByte);
        }
        if( rc!=SQLITE_OK ) goto error_out;
      }
      if( pTab->bRowid ){
        nByte += 9;               /* Size of rowid field - an integer */
      }
  
      /* Allocate the change object */
      pC = (SessionChange*)sessionMalloc64(pSession, nByte);
      if( !pC ){
        rc = SQLITE_NOMEM;
        goto error_out;
      }else{
        memset(pC, 0, sizeof(SessionChange));
        pC->aRecord = (u8 *)&pC[1];
      }
  
      /* Populate the change object. None of the preupdate_old(),
      ** preupdate_new() or SerializeValue() calls below may fail as all
      ** required values and encodings have already been cached in memory.
      ** It is not possible for an OOM to occur in this block. */
      nByte = 0;
      if( pTab->bRowid ){
        pC->aRecord[0] = SQLITE_INTEGER;
        sessionPutI64(&pC->aRecord[1], iRowid);
        nByte = 9;
      }
      for(i=pTab->bRowid; i<pTab->nCol; i++){
        sqlite3_value *p = 0;
        int iIdx = pTab->aiIdx[i];
        if( op!=SQLITE_INSERT ){
          pSession->hook.xOld(pSession->hook.pCtx, iIdx, &p);
        }else if( pTab->abPK[i] ){
          pSession->hook.xNew(pSession->hook.pCtx, iIdx, &p);
        }
        sessionSerializeValue(&pC->aRecord[nByte], p, &nByte);
      }

      /* Add the change to the hash-table */
      if( pSession->bIndirect || pSession->hook.xDepth(pSession->hook.pCtx) ){
        pC->bIndirect = 1;
      }
      pC->nRecordField = pTab->nCol;
      pC->nRecord = nByte;
      pC->op = op;
      pC->pNext = pTab->apChange[iHash];
      pTab->apChange[iHash] = pC;

    }else if( pC->bIndirect ){
      /* If the existing change is considered "indirect", but this current
      ** change is "direct", mark the change object as direct. */
      if( pSession->hook.xDepth(pSession->hook.pCtx)==0 
       && pSession->bIndirect==0 
      ){
        pC->bIndirect = 0;
      }
    }

    assert( rc==SQLITE_OK );
    if( pSession->bEnableSize ){
      rc = sessionUpdateMaxSize(op, pSession, pTab, pC);
    }
  }


  /* If an error has occurred, mark the session object as failed. */
 error_out:
  if( pTab->bStat1 ){
    pSession->hook = stat1.hook;
  }
  if( rc!=SQLITE_OK ){
    pSession->rc = rc;
  }
}

static int sessionFindTable(
  sqlite3_session *pSession, 
  const char *zName,
  SessionTable **ppTab
){
  int rc = SQLITE_OK;
  int nName = sqlite3Strlen30(zName);
  SessionTable *pRet;

  /* Search for an existing table */
  for(pRet=pSession->pTable; pRet; pRet=pRet->pNext){
    if( 0==sqlite3_strnicmp(pRet->zName, zName, nName+1) ) break;
  }

  if( pRet==0 && pSession->bAutoAttach ){
    /* If there is a table-filter configured, invoke it. If it returns 0,
    ** do not automatically add the new table. */
    if( pSession->xTableFilter==0
     || pSession->xTableFilter(pSession->pFilterCtx, zName) 
    ){
      rc = sqlite3session_attach(pSession, zName);
      if( rc==SQLITE_OK ){
        pRet = pSession->pTable;
        while( ALWAYS(pRet) && pRet->pNext ){
          pRet = pRet->pNext;
        }
        assert( pRet!=0 );
        assert( 0==sqlite3_strnicmp(pRet->zName, zName, nName+1) );
      }
    }
  }

  assert( rc==SQLITE_OK || pRet==0 );
  *ppTab = pRet;
  return rc;
}

/*
** The 'pre-update' hook registered by this module with SQLite databases.
*/
static void xPreUpdate(
  void *pCtx,                     /* Copy of third arg to preupdate_hook() */
  sqlite3 *db,                    /* Database handle */
  int op,                         /* SQLITE_UPDATE, DELETE or INSERT */
  char const *zDb,                /* Database name */
  char const *zName,              /* Table name */
  sqlite3_int64 iKey1,            /* Rowid of row about to be deleted/updated */
  sqlite3_int64 iKey2             /* New rowid value (for a rowid UPDATE) */
){
  sqlite3_session *pSession;
  int nDb = sqlite3Strlen30(zDb);

  assert( sqlite3_mutex_held(db->mutex) );
  (void)iKey1;
  (void)iKey2;

  for(pSession=(sqlite3_session *)pCtx; pSession; pSession=pSession->pNext){
    SessionTable *pTab;

    /* If this session is attached to a different database ("main", "temp" 
    ** etc.), or if it is not currently enabled, there is nothing to do. Skip 
    ** to the next session object attached to this database. */
    if( pSession->bEnable==0 ) continue;
    if( pSession->rc ) continue;
    if( sqlite3_strnicmp(zDb, pSession->zDb, nDb+1) ) continue;

    pSession->rc = sessionFindTable(pSession, zName, &pTab);
    if( pTab ){
      assert( pSession->rc==SQLITE_OK );
      assert( op==SQLITE_UPDATE || iKey1==iKey2 );
      sessionPreupdateOneChange(op, iKey1, pSession, pTab);
      if( op==SQLITE_UPDATE ){
        sessionPreupdateOneChange(SQLITE_INSERT, iKey2, pSession, pTab);
      }
    }
  }
}

/*
** The pre-update hook implementations.
*/
static int sessionPreupdateOld(void *pCtx, int iVal, sqlite3_value **ppVal){
  return sqlite3_preupdate_old((sqlite3*)pCtx, iVal, ppVal);
}
static int sessionPreupdateNew(void *pCtx, int iVal, sqlite3_value **ppVal){
  return sqlite3_preupdate_new((sqlite3*)pCtx, iVal, ppVal);
}
static int sessionPreupdateCount(void *pCtx){
  return sqlite3_preupdate_count((sqlite3*)pCtx);
}
static int sessionPreupdateDepth(void *pCtx){
  return sqlite3_preupdate_depth((sqlite3*)pCtx);
}

/*
** Install the pre-update hooks on the session object passed as the only
** argument.
*/
static void sessionPreupdateHooks(
  sqlite3_session *pSession
){
  pSession->hook.pCtx = (void*)pSession->db;
  pSession->hook.xOld = sessionPreupdateOld;
  pSession->hook.xNew = sessionPreupdateNew;
  pSession->hook.xCount = sessionPreupdateCount;
  pSession->hook.xDepth = sessionPreupdateDepth;
}

typedef struct SessionDiffCtx SessionDiffCtx;
struct SessionDiffCtx {
  sqlite3_stmt *pStmt;
  int bRowid;
  int nOldOff;
};

/*
** The diff hook implementations.
*/
static int sessionDiffOld(void *pCtx, int iVal, sqlite3_value **ppVal){
  SessionDiffCtx *p = (SessionDiffCtx*)pCtx;
  *ppVal = sqlite3_column_value(p->pStmt, iVal+p->nOldOff+p->bRowid);
  return SQLITE_OK;
}
static int sessionDiffNew(void *pCtx, int iVal, sqlite3_value **ppVal){
  SessionDiffCtx *p = (SessionDiffCtx*)pCtx;
  *ppVal = sqlite3_column_value(p->pStmt, iVal+p->bRowid);
   return SQLITE_OK;
}
static int sessionDiffCount(void *pCtx){
  SessionDiffCtx *p = (SessionDiffCtx*)pCtx;
  return (p->nOldOff ? p->nOldOff : sqlite3_column_count(p->pStmt)) - p->bRowid;
}
static int sessionDiffDepth(void *pCtx){
  (void)pCtx;
  return 0;
}

/*
** Install the diff hooks on the session object passed as the only
** argument.
*/
static void sessionDiffHooks(
  sqlite3_session *pSession,
  SessionDiffCtx *pDiffCtx
){
  pSession->hook.pCtx = (void*)pDiffCtx;
  pSession->hook.xOld = sessionDiffOld;
  pSession->hook.xNew = sessionDiffNew;
  pSession->hook.xCount = sessionDiffCount;
  pSession->hook.xDepth = sessionDiffDepth;
}

static char *sessionExprComparePK(
  int nCol,
  const char *zDb1, const char *zDb2, 
  const char *zTab,
  const char **azCol, u8 *abPK
){
  int i;
  const char *zSep = "";
  char *zRet = 0;

  for(i=0; i<nCol; i++){
    if( abPK[i] ){
      zRet = sqlite3_mprintf("%z%s\"%w\".\"%w\".\"%w\"=\"%w\".\"%w\".\"%w\"",
          zRet, zSep, zDb1, zTab, azCol[i], zDb2, zTab, azCol[i]
      );
      zSep = " AND ";
      if( zRet==0 ) break;
    }
  }

  return zRet;
}

static char *sessionExprCompareOther(
  int nCol,
  const char *zDb1, const char *zDb2, 
  const char *zTab,
  const char **azCol, u8 *abPK
){
  int i;
  const char *zSep = "";
  char *zRet = 0;
  int bHave = 0;

  for(i=0; i<nCol; i++){
    if( abPK[i]==0 ){
      bHave = 1;
      zRet = sqlite3_mprintf(
          "%z%s\"%w\".\"%w\".\"%w\" IS NOT \"%w\".\"%w\".\"%w\"",
          zRet, zSep, zDb1, zTab, azCol[i], zDb2, zTab, azCol[i]
      );
      zSep = " OR ";
      if( zRet==0 ) break;
    }
  }

  if( bHave==0 ){
    assert( zRet==0 );
    zRet = sqlite3_mprintf("0");
  }

  return zRet;
}

static char *sessionSelectFindNew(
  const char *zDb1,      /* Pick rows in this db only */
  const char *zDb2,      /* But not in this one */
  int bRowid,
  const char *zTbl,      /* Table name */
  const char *zExpr
){
  const char *zSel = (bRowid ? SESSIONS_ROWID ", *" : "*");
  char *zRet = sqlite3_mprintf(
      "SELECT %s FROM \"%w\".\"%w\" WHERE NOT EXISTS ("
      "  SELECT 1 FROM \"%w\".\"%w\" WHERE %s"
      ")",
      zSel, zDb1, zTbl, zDb2, zTbl, zExpr
  );
  return zRet;
}

static int sessionDiffFindNew(
  int op,
  sqlite3_session *pSession,
  SessionTable *pTab,
  const char *zDb1,
  const char *zDb2,
  char *zExpr
){
  int rc = SQLITE_OK;
  char *zStmt = sessionSelectFindNew(
      zDb1, zDb2, pTab->bRowid, pTab->zName, zExpr
  );

  if( zStmt==0 ){
    rc = SQLITE_NOMEM;
  }else{
    sqlite3_stmt *pStmt;
    rc = sqlite3_prepare(pSession->db, zStmt, -1, &pStmt, 0);
    if( rc==SQLITE_OK ){
      SessionDiffCtx *pDiffCtx = (SessionDiffCtx*)pSession->hook.pCtx;
      pDiffCtx->pStmt = pStmt;
      pDiffCtx->nOldOff = 0;
      pDiffCtx->bRowid = pTab->bRowid;
      while( SQLITE_ROW==sqlite3_step(pStmt) ){
        i64 iRowid = (pTab->bRowid ? sqlite3_column_int64(pStmt, 0) : 0);
        sessionPreupdateOneChange(op, iRowid, pSession, pTab);
      }
      rc = sqlite3_finalize(pStmt);
    }
    sqlite3_free(zStmt);
  }

  return rc;
}

/*
** Return a comma-separated list of the fully-qualified (with both database
** and table name) column names from table pTab. e.g.
**
**    "main"."t1"."a", "main"."t1"."b", "main"."t1"."c"
*/
static char *sessionAllCols(
  const char *zDb,
  SessionTable *pTab
){
  int ii;
  char *zRet = 0;
  for(ii=0; ii<pTab->nCol; ii++){
    zRet = sqlite3_mprintf("%z%s\"%w\".\"%w\".\"%w\"",
        zRet, (zRet ? ", " : ""), zDb, pTab->zName, pTab->azCol[ii] 
    );
    if( !zRet ) break;
  }
  return zRet;
}

static int sessionDiffFindModified(
  sqlite3_session *pSession, 
  SessionTable *pTab, 
  const char *zFrom, 
  const char *zExpr
){
  int rc = SQLITE_OK;

  char *zExpr2 = sessionExprCompareOther(pTab->nCol,
      pSession->zDb, zFrom, pTab->zName, pTab->azCol, pTab->abPK
  );
  if( zExpr2==0 ){
    rc = SQLITE_NOMEM;
  }else{
    char *z1 = sessionAllCols(pSession->zDb, pTab);
    char *z2 = sessionAllCols(zFrom, pTab);
    char *zStmt = sqlite3_mprintf(
        "SELECT %s,%s FROM \"%w\".\"%w\", \"%w\".\"%w\" WHERE %s AND (%z)",
        z1, z2, pSession->zDb, pTab->zName, zFrom, pTab->zName, zExpr, zExpr2
    );
    if( zStmt==0 || z1==0 || z2==0 ){
      rc = SQLITE_NOMEM;
    }else{
      sqlite3_stmt *pStmt;
      rc = sqlite3_prepare(pSession->db, zStmt, -1, &pStmt, 0);

      if( rc==SQLITE_OK ){
        SessionDiffCtx *pDiffCtx = (SessionDiffCtx*)pSession->hook.pCtx;
        pDiffCtx->pStmt = pStmt;
        pDiffCtx->nOldOff = pTab->nCol;
        while( SQLITE_ROW==sqlite3_step(pStmt) ){
          i64 iRowid = (pTab->bRowid ? sqlite3_column_int64(pStmt, 0) : 0);
          sessionPreupdateOneChange(SQLITE_UPDATE, iRowid, pSession, pTab);
        }
        rc = sqlite3_finalize(pStmt);
      }
    }
    sqlite3_free(zStmt);
    sqlite3_free(z1);
    sqlite3_free(z2);
  }

  return rc;
}

int sqlite3session_diff(
  sqlite3_session *pSession,
  const char *zFrom,
  const char *zTbl,
  char **pzErrMsg
){
  const char *zDb = pSession->zDb;
  int rc = pSession->rc;
  SessionDiffCtx d;

  memset(&d, 0, sizeof(d));
  sessionDiffHooks(pSession, &d);

  sqlite3_mutex_enter(sqlite3_db_mutex(pSession->db));
  if( pzErrMsg ) *pzErrMsg = 0;
  if( rc==SQLITE_OK ){
    char *zExpr = 0;
    sqlite3 *db = pSession->db;
    SessionTable *pTo;            /* Table zTbl */

    /* Locate and if necessary initialize the target table object */
    pSession->bAutoAttach++;
    rc = sessionFindTable(pSession, zTbl, &pTo);
    pSession->bAutoAttach--;
    if( pTo==0 ) goto diff_out;
    if( sessionInitTable(pSession, pTo, pSession->db, pSession->zDb) ){
      rc = pSession->rc;
      goto diff_out;
    }

    /* Check the table schemas match */
    if( rc==SQLITE_OK ){
      int bHasPk = 0;
      int bMismatch = 0;
      int nCol = 0;               /* Columns in zFrom.zTbl */
      int bRowid = 0;
      u8 *abPK = 0;
      const char **azCol = 0;
      char *zDbExists = 0;

      /* Check that database zFrom is attached.  */
      zDbExists = sqlite3_mprintf("SELECT * FROM %Q.sqlite_schema", zFrom);
      if( zDbExists==0 ){
        rc = SQLITE_NOMEM;
      }else{
        sqlite3_stmt *pDbExists = 0;
        rc = sqlite3_prepare_v2(db, zDbExists, -1, &pDbExists, 0);
        if( rc==SQLITE_ERROR ){
          rc = SQLITE_OK;
          nCol = -1;
        }
        sqlite3_finalize(pDbExists);
        sqlite3_free(zDbExists);
      }

      if( rc==SQLITE_OK && nCol==0 ){
        rc = sessionTableInfo(0, db, zFrom, zTbl, 
            &nCol, 0, 0, &azCol, 0, 0, &abPK, 
            pSession->bImplicitPK ? &bRowid : 0
        );
      }
      if( rc==SQLITE_OK ){
        if( pTo->nCol!=nCol ){
          if( nCol<=0 ){
            rc = SQLITE_SCHEMA;
            if( pzErrMsg ){
              *pzErrMsg = sqlite3_mprintf("no such table: %s.%s", zFrom, zTbl);
            }
          }else{
            bMismatch = 1;
          }
        }else{
          int i;
          for(i=0; i<nCol; i++){
            if( pTo->abPK[i]!=abPK[i] ) bMismatch = 1;
            if( sqlite3_stricmp(azCol[i], pTo->azCol[i]) ) bMismatch = 1;
            if( abPK[i] ) bHasPk = 1;
          }
        }
      }
      sqlite3_free((char*)azCol);
      if( bMismatch ){
        if( pzErrMsg ){
          *pzErrMsg = sqlite3_mprintf("table schemas do not match");
        }
        rc = SQLITE_SCHEMA;
      }
      if( bHasPk==0 ){
        /* Ignore tables with no primary keys */
        goto diff_out;
      }
    }

    if( rc==SQLITE_OK ){
      zExpr = sessionExprComparePK(pTo->nCol, 
          zDb, zFrom, pTo->zName, pTo->azCol, pTo->abPK
      );
    }

    /* Find new rows */
    if( rc==SQLITE_OK ){
      rc = sessionDiffFindNew(SQLITE_INSERT, pSession, pTo, zDb, zFrom, zExpr);
    }

    /* Find old rows */
    if( rc==SQLITE_OK ){
      rc = sessionDiffFindNew(SQLITE_DELETE, pSession, pTo, zFrom, zDb, zExpr);
    }

    /* Find modified rows */
    if( rc==SQLITE_OK ){
      rc = sessionDiffFindModified(pSession, pTo, zFrom, zExpr);
    }

    sqlite3_free(zExpr);
  }

 diff_out:
  sessionPreupdateHooks(pSession);
  sqlite3_mutex_leave(sqlite3_db_mutex(pSession->db));
  return rc;
}

/*
** Create a session object. This session object will record changes to
** database zDb attached to connection db.
*/
int sqlite3session_create(
  sqlite3 *db,                    /* Database handle */
  const char *zDb,                /* Name of db (e.g. "main") */
  sqlite3_session **ppSession     /* OUT: New session object */
){
  sqlite3_session *pNew;          /* Newly allocated session object */
  sqlite3_session *pOld;          /* Session object already attached to db */
  int nDb = sqlite3Strlen30(zDb); /* Length of zDb in bytes */

  /* Zero the output value in case an error occurs. */
  *ppSession = 0;

  /* Allocate and populate the new session object. */
  pNew = (sqlite3_session *)sqlite3_malloc64(sizeof(sqlite3_session) + nDb + 1);
  if( !pNew ) return SQLITE_NOMEM;
  memset(pNew, 0, sizeof(sqlite3_session));
  pNew->db = db;
  pNew->zDb = (char *)&pNew[1];
  pNew->bEnable = 1;
  memcpy(pNew->zDb, zDb, nDb+1);
  sessionPreupdateHooks(pNew);

  /* Add the new session object to the linked list of session objects 
  ** attached to database handle $db. Do this under the cover of the db
  ** handle mutex.  */
  sqlite3_mutex_enter(sqlite3_db_mutex(db));
  pOld = (sqlite3_session*)sqlite3_preupdate_hook(db, xPreUpdate, (void*)pNew);
  pNew->pNext = pOld;
  sqlite3_mutex_leave(sqlite3_db_mutex(db));

  *ppSession = pNew;
  return SQLITE_OK;
}

/*
** Free the list of table objects passed as the first argument. The contents
** of the changed-rows hash tables are also deleted.
*/
static void sessionDeleteTable(sqlite3_session *pSession, SessionTable *pList){
  SessionTable *pNext;
  SessionTable *pTab;

  for(pTab=pList; pTab; pTab=pNext){
    int i;
    pNext = pTab->pNext;
    for(i=0; i<pTab->nChange; i++){
      SessionChange *p;
      SessionChange *pNextChange;
      for(p=pTab->apChange[i]; p; p=pNextChange){
        pNextChange = p->pNext;
        sessionFree(pSession, p);
      }
    }
    sqlite3_finalize(pTab->pDfltStmt);
    sessionFree(pSession, (char*)pTab->azCol);  /* cast works around VC++ bug */
    sessionFree(pSession, pTab->apChange);
    sessionFree(pSession, pTab);
  }
}

/*
** Delete a session object previously allocated using sqlite3session_create().
*/
void sqlite3session_delete(sqlite3_session *pSession){
  sqlite3 *db = pSession->db;
  sqlite3_session *pHead;
  sqlite3_session **pp;

  /* Unlink the session from the linked list of sessions attached to the
  ** database handle. Hold the db mutex while doing so.  */
  sqlite3_mutex_enter(sqlite3_db_mutex(db));
  pHead = (sqlite3_session*)sqlite3_preupdate_hook(db, 0, 0);
  for(pp=&pHead; ALWAYS((*pp)!=0); pp=&((*pp)->pNext)){
    if( (*pp)==pSession ){
      *pp = (*pp)->pNext;
      if( pHead ) sqlite3_preupdate_hook(db, xPreUpdate, (void*)pHead);
      break;
    }
  }
  sqlite3_mutex_leave(sqlite3_db_mutex(db));
  sqlite3ValueFree(pSession->pZeroBlob);

  /* Delete all attached table objects. And the contents of their 
  ** associated hash-tables. */
  sessionDeleteTable(pSession, pSession->pTable);

  /* Free the session object. */
  sqlite3_free(pSession);
}

/*
** Set a table filter on a Session Object.
*/
void sqlite3session_table_filter(
  sqlite3_session *pSession, 
  int(*xFilter)(void*, const char*),
  void *pCtx                      /* First argument passed to xFilter */
){
  pSession->bAutoAttach = 1;
  pSession->pFilterCtx = pCtx;
  pSession->xTableFilter = xFilter;
}

/*
** Attach a table to a session. All subsequent changes made to the table
** while the session object is enabled will be recorded.
**
** Only tables that have a PRIMARY KEY defined may be attached. It does
** not matter if the PRIMARY KEY is an "INTEGER PRIMARY KEY" (rowid alias)
** or not.
*/
int sqlite3session_attach(
  sqlite3_session *pSession,      /* Session object */
  const char *zName               /* Table name */
){
  int rc = SQLITE_OK;
  sqlite3_mutex_enter(sqlite3_db_mutex(pSession->db));

  if( !zName ){
    pSession->bAutoAttach = 1;
  }else{
    SessionTable *pTab;           /* New table object (if required) */
    int nName;                    /* Number of bytes in string zName */

    /* First search for an existing entry. If one is found, this call is
    ** a no-op. Return early. */
    nName = sqlite3Strlen30(zName);
    for(pTab=pSession->pTable; pTab; pTab=pTab->pNext){
      if( 0==sqlite3_strnicmp(pTab->zName, zName, nName+1) ) break;
    }

    if( !pTab ){
      /* Allocate new SessionTable object. */
      int nByte = sizeof(SessionTable) + nName + 1;
      pTab = (SessionTable*)sessionMalloc64(pSession, nByte);
      if( !pTab ){
        rc = SQLITE_NOMEM;
      }else{
        /* Populate the new SessionTable object and link it into the list.
        ** The new object must be linked onto the end of the list, not 
        ** simply added to the start of it in order to ensure that tables
        ** appear in the correct order when a changeset or patchset is
        ** eventually generated. */
        SessionTable **ppTab;
        memset(pTab, 0, sizeof(SessionTable));
        pTab->zName = (char *)&pTab[1];
        memcpy(pTab->zName, zName, nName+1);
        for(ppTab=&pSession->pTable; *ppTab; ppTab=&(*ppTab)->pNext);
        *ppTab = pTab;
      }
    }
  }

  sqlite3_mutex_leave(sqlite3_db_mutex(pSession->db));
  return rc;
}

/*
** Append the value passed as the second argument to the buffer passed
** as the first.
**
** This function is a no-op if *pRc is non-zero when it is called.
** Otherwise, if an error occurs, *pRc is set to an SQLite error code
** before returning.
*/
static void sessionAppendValue(SessionBuffer *p, sqlite3_value *pVal, int *pRc){
  int rc = *pRc;
  if( rc==SQLITE_OK ){
    sqlite3_int64 nByte = 0;
    rc = sessionSerializeValue(0, pVal, &nByte);
    sessionBufferGrow(p, nByte, &rc);
    if( rc==SQLITE_OK ){
      rc = sessionSerializeValue(&p->aBuf[p->nBuf], pVal, 0);
      p->nBuf += nByte;
    }else{
      *pRc = rc;
    }
  }
}

/*
** This function is a no-op if *pRc is other than SQLITE_OK when it is 
** called. Otherwise, append a single byte to the buffer. 
**
** If an OOM condition is encountered, set *pRc to SQLITE_NOMEM before
** returning.
*/
static void sessionAppendByte(SessionBuffer *p, u8 v, int *pRc){
  if( 0==sessionBufferGrow(p, 1, pRc) ){
    p->aBuf[p->nBuf++] = v;
  }
}

/*
** This function is a no-op if *pRc is other than SQLITE_OK when it is 
** called. Otherwise, append a single varint to the buffer. 
**
** If an OOM condition is encountered, set *pRc to SQLITE_NOMEM before
** returning.
*/
static void sessionAppendVarint(SessionBuffer *p, int v, int *pRc){
  if( 0==sessionBufferGrow(p, 9, pRc) ){
    p->nBuf += sessionVarintPut(&p->aBuf[p->nBuf], v);
  }
}

/*
** This function is a no-op if *pRc is other than SQLITE_OK when it is 
** called. Otherwise, append a blob of data to the buffer. 
**
** If an OOM condition is encountered, set *pRc to SQLITE_NOMEM before
** returning.
*/
static void sessionAppendBlob(
  SessionBuffer *p, 
  const u8 *aBlob, 
  int nBlob, 
  int *pRc
){
  if( nBlob>0 && 0==sessionBufferGrow(p, nBlob, pRc) ){
    memcpy(&p->aBuf[p->nBuf], aBlob, nBlob);
    p->nBuf += nBlob;
  }
}

/*
** This function is a no-op if *pRc is other than SQLITE_OK when it is 
** called. Otherwise, append the string representation of integer iVal
** to the buffer. No nul-terminator is written.
**
** If an OOM condition is encountered, set *pRc to SQLITE_NOMEM before
** returning.
*/
static void sessionAppendInteger(
  SessionBuffer *p,               /* Buffer to append to */
  int iVal,                       /* Value to write the string rep. of */
  int *pRc                        /* IN/OUT: Error code */
){
  char aBuf[24];
  sqlite3_snprintf(sizeof(aBuf)-1, aBuf, "%d", iVal);
  sessionAppendStr(p, aBuf, pRc);
}

/*
** This function is a no-op if *pRc is other than SQLITE_OK when it is 
** called. Otherwise, append the string zStr enclosed in quotes (") and
** with any embedded quote characters escaped to the buffer. No 
** nul-terminator byte is written.
**
** If an OOM condition is encountered, set *pRc to SQLITE_NOMEM before
** returning.
*/
static void sessionAppendIdent(
  SessionBuffer *p,               /* Buffer to a append to */
  const char *zStr,               /* String to quote, escape and append */
  int *pRc                        /* IN/OUT: Error code */
){
  int nStr = sqlite3Strlen30(zStr)*2 + 2 + 2;
  if( 0==sessionBufferGrow(p, nStr, pRc) ){
    char *zOut = (char *)&p->aBuf[p->nBuf];
    const char *zIn = zStr;
    *zOut++ = '"';
    if( zIn!=0 ){
      while( *zIn ){
        if( *zIn=='"' ) *zOut++ = '"';
        *zOut++ = *(zIn++);
      }
    }
    *zOut++ = '"';
    p->nBuf = (int)((u8 *)zOut - p->aBuf);
    p->aBuf[p->nBuf] = 0x00;
  }
}

/*
** This function is a no-op if *pRc is other than SQLITE_OK when it is
** called. Otherwse, it appends the serialized version of the value stored
** in column iCol of the row that SQL statement pStmt currently points
** to to the buffer.
*/
static void sessionAppendCol(
  SessionBuffer *p,               /* Buffer to append to */
  sqlite3_stmt *pStmt,            /* Handle pointing to row containing value */
  int iCol,                       /* Column to read value from */
  int *pRc                        /* IN/OUT: Error code */
){
  if( *pRc==SQLITE_OK ){
    int eType = sqlite3_column_type(pStmt, iCol);
    sessionAppendByte(p, (u8)eType, pRc);
    if( eType==SQLITE_INTEGER || eType==SQLITE_FLOAT ){
      sqlite3_int64 i;
      u8 aBuf[8];
      if( eType==SQLITE_INTEGER ){
        i = sqlite3_column_int64(pStmt, iCol);
      }else{
        double r = sqlite3_column_double(pStmt, iCol);
        memcpy(&i, &r, 8);
      }
      sessionPutI64(aBuf, i);
      sessionAppendBlob(p, aBuf, 8, pRc);
    }
    if( eType==SQLITE_BLOB || eType==SQLITE_TEXT ){
      u8 *z;
      int nByte;
      if( eType==SQLITE_BLOB ){
        z = (u8 *)sqlite3_column_blob(pStmt, iCol);
      }else{
        z = (u8 *)sqlite3_column_text(pStmt, iCol);
      }
      nByte = sqlite3_column_bytes(pStmt, iCol);
      if( z || (eType==SQLITE_BLOB && nByte==0) ){
        sessionAppendVarint(p, nByte, pRc);
        sessionAppendBlob(p, z, nByte, pRc);
      }else{
        *pRc = SQLITE_NOMEM;
      }
    }
  }
}

/*
**
** This function appends an update change to the buffer (see the comments 
** under "CHANGESET FORMAT" at the top of the file). An update change 
** consists of:
**
**   1 byte:  SQLITE_UPDATE (0x17)
**   n bytes: old.* record (see RECORD FORMAT)
**   m bytes: new.* record (see RECORD FORMAT)
**
** The SessionChange object passed as the third argument contains the
** values that were stored in the row when the session began (the old.*
** values). The statement handle passed as the second argument points
** at the current version of the row (the new.* values).
**
** If all of the old.* values are equal to their corresponding new.* value
** (i.e. nothing has changed), then no data at all is appended to the buffer.
**
** Otherwise, the old.* record contains all primary key values and the 
** original values of any fields that have been modified. The new.* record 
** contains the new values of only those fields that have been modified.
*/ 
static int sessionAppendUpdate(
  SessionBuffer *pBuf,            /* Buffer to append to */
  int bPatchset,                  /* True for "patchset", 0 for "changeset" */
  sqlite3_stmt *pStmt,            /* Statement handle pointing at new row */
  SessionChange *p,               /* Object containing old values */
  u8 *abPK                        /* Boolean array - true for PK columns */
){
  int rc = SQLITE_OK;
  SessionBuffer buf2 = {0,0,0}; /* Buffer to accumulate new.* record in */
  int bNoop = 1;                /* Set to zero if any values are modified */
  int nRewind = pBuf->nBuf;     /* Set to zero if any values are modified */
  int i;                        /* Used to iterate through columns */
  u8 *pCsr = p->aRecord;        /* Used to iterate through old.* values */

  assert( abPK!=0 );
  sessionAppendByte(pBuf, SQLITE_UPDATE, &rc);
  sessionAppendByte(pBuf, p->bIndirect, &rc);
  for(i=0; i<sqlite3_column_count(pStmt); i++){
    int bChanged = 0;
    int nAdvance;
    int eType = *pCsr;
    switch( eType ){
      case SQLITE_NULL:
        nAdvance = 1;
        if( sqlite3_column_type(pStmt, i)!=SQLITE_NULL ){
          bChanged = 1;
        }
        break;

      case SQLITE_FLOAT:
      case SQLITE_INTEGER: {
        nAdvance = 9;
        if( eType==sqlite3_column_type(pStmt, i) ){
          sqlite3_int64 iVal = sessionGetI64(&pCsr[1]);
          if( eType==SQLITE_INTEGER ){
            if( iVal==sqlite3_column_int64(pStmt, i) ) break;
          }else{
            double dVal;
            memcpy(&dVal, &iVal, 8);
            if( dVal==sqlite3_column_double(pStmt, i) ) break;
          }
        }
        bChanged = 1;
        break;
      }

      default: {
        int n;
        int nHdr = 1 + sessionVarintGet(&pCsr[1], &n);
        assert( eType==SQLITE_TEXT || eType==SQLITE_BLOB );
        nAdvance = nHdr + n;
        if( eType==sqlite3_column_type(pStmt, i) 
         && n==sqlite3_column_bytes(pStmt, i) 
         && (n==0 || 0==memcmp(&pCsr[nHdr], sqlite3_column_blob(pStmt, i), n))
        ){
          break;
        }
        bChanged = 1;
      }
    }

    /* If at least one field has been modified, this is not a no-op. */
    if( bChanged ) bNoop = 0;

    /* Add a field to the old.* record. This is omitted if this module is
    ** currently generating a patchset. */
    if( bPatchset==0 ){
      if( bChanged || abPK[i] ){
        sessionAppendBlob(pBuf, pCsr, nAdvance, &rc);
      }else{
        sessionAppendByte(pBuf, 0, &rc);
      }
    }

    /* Add a field to the new.* record. Or the only record if currently
    ** generating a patchset.  */
    if( bChanged || (bPatchset && abPK[i]) ){
      sessionAppendCol(&buf2, pStmt, i, &rc);
    }else{
      sessionAppendByte(&buf2, 0, &rc);
    }

    pCsr += nAdvance;
  }

  if( bNoop ){
    pBuf->nBuf = nRewind;
  }else{
    sessionAppendBlob(pBuf, buf2.aBuf, buf2.nBuf, &rc);
  }
  sqlite3_free(buf2.aBuf);

  return rc;
}

/*
** Append a DELETE change to the buffer passed as the first argument. Use
** the changeset format if argument bPatchset is zero, or the patchset
** format otherwise.
*/
static int sessionAppendDelete(
  SessionBuffer *pBuf,            /* Buffer to append to */
  int bPatchset,                  /* True for "patchset", 0 for "changeset" */
  SessionChange *p,               /* Object containing old values */
  int nCol,                       /* Number of columns in table */
  u8 *abPK                        /* Boolean array - true for PK columns */
){
  int rc = SQLITE_OK;

  sessionAppendByte(pBuf, SQLITE_DELETE, &rc);
  sessionAppendByte(pBuf, p->bIndirect, &rc);

  if( bPatchset==0 ){
    sessionAppendBlob(pBuf, p->aRecord, p->nRecord, &rc);
  }else{
    int i;
    u8 *a = p->aRecord;
    for(i=0; i<nCol; i++){
      u8 *pStart = a;
      int eType = *a++;

      switch( eType ){
        case 0:
        case SQLITE_NULL:
          assert( abPK[i]==0 );
          break;

        case SQLITE_FLOAT:
        case SQLITE_INTEGER:
          a += 8;
          break;

        default: {
          int n;
          a += sessionVarintGet(a, &n);
          a += n;
          break;
        }
      }
      if( abPK[i] ){
        sessionAppendBlob(pBuf, pStart, (int)(a-pStart), &rc);
      }
    }
    assert( (a - p->aRecord)==p->nRecord );
  }

  return rc;
}

/*
** Formulate and prepare a SELECT statement to retrieve a row from table
** zTab in database zDb based on its primary key. i.e.
**
**   SELECT *, <noop-test> FROM zDb.zTab WHERE (pk1, pk2,...) IS (?1, ?2,...)
**
** where <noop-test> is: 
**
**   1 AND (?A OR ?1 IS <column>) AND ...
**
** for each non-pk <column>.
*/
static int sessionSelectStmt(
  sqlite3 *db,                    /* Database handle */
  int bIgnoreNoop,
  const char *zDb,                /* Database name */
  const char *zTab,               /* Table name */
  int bRowid,
  int nCol,                       /* Number of columns in table */
  const char **azCol,             /* Names of table columns */
  u8 *abPK,                       /* PRIMARY KEY  array */
  sqlite3_stmt **ppStmt           /* OUT: Prepared SELECT statement */
){
  int rc = SQLITE_OK;
  char *zSql = 0;
  const char *zSep = "";
  int nSql = -1;
  int i;

  SessionBuffer cols = {0, 0, 0};
  SessionBuffer nooptest = {0, 0, 0};
  SessionBuffer pkfield = {0, 0, 0};
  SessionBuffer pkvar = {0, 0, 0};

  sessionAppendStr(&nooptest, ", 1", &rc);

  if( 0==sqlite3_stricmp("sqlite_stat1", zTab) ){
    sessionAppendStr(&nooptest, " AND (?6 OR ?3 IS stat)", &rc);
    sessionAppendStr(&pkfield, "tbl, idx", &rc);
    sessionAppendStr(&pkvar, 
        "?1, (CASE WHEN ?2=X'' THEN NULL ELSE ?2 END)", &rc
    );
    sessionAppendStr(&cols, "tbl, ?2, stat", &rc);
  }else{
  #if 0
    if( bRowid ){
      sessionAppendStr(&cols, SESSIONS_ROWID, &rc);
    }
    #endif
    for(i=0; i<nCol; i++){
      if( cols.nBuf ) sessionAppendStr(&cols, ", ", &rc);
      sessionAppendIdent(&cols, azCol[i], &rc);
      if( abPK[i] ){
        sessionAppendStr(&pkfield, zSep, &rc);
        sessionAppendStr(&pkvar, zSep, &rc);
        zSep = ", ";
        sessionAppendIdent(&pkfield, azCol[i], &rc);
        sessionAppendPrintf(&pkvar, &rc, "?%d", i+1);
      }else{
        sessionAppendPrintf(&nooptest, &rc, 
            " AND (?%d OR ?%d IS %w.%w)", i+1+nCol, i+1, zTab, azCol[i]
        );
      }
    }
  }

  if( rc==SQLITE_OK ){
    zSql = sqlite3_mprintf(
        "SELECT %s%s FROM %Q.%Q WHERE (%s) IS (%s)",
        (char*)cols.aBuf, (bIgnoreNoop ? (char*)nooptest.aBuf : ""), 
        zDb, zTab, (char*)pkfield.aBuf, (char*)pkvar.aBuf
    );
    if( zSql==0 ) rc = SQLITE_NOMEM;
  }

#if 0
  if( 0==sqlite3_stricmp("sqlite_stat1", zTab) ){
    zSql = sqlite3_mprintf(
        "SELECT tbl, ?2, stat FROM %Q.sqlite_stat1 WHERE tbl IS ?1 AND "
        "idx IS (CASE WHEN ?2=X'' THEN NULL ELSE ?2 END)", zDb
    );
    if( zSql==0 ) rc = SQLITE_NOMEM;
  }else{
    const char *zSep = "";
    SessionBuffer buf = {0, 0, 0};

    sessionAppendStr(&buf, "SELECT * FROM ", &rc);
    sessionAppendIdent(&buf, zDb, &rc);
    sessionAppendStr(&buf, ".", &rc);
    sessionAppendIdent(&buf, zTab, &rc);
    sessionAppendStr(&buf, " WHERE ", &rc);
    for(i=0; i<nCol; i++){
      if( abPK[i] ){
        sessionAppendStr(&buf, zSep, &rc);
        sessionAppendIdent(&buf, azCol[i], &rc);
        sessionAppendStr(&buf, " IS ?", &rc);
        sessionAppendInteger(&buf, i+1, &rc);
        zSep = " AND ";
      }
    }
    zSql = (char*)buf.aBuf;
    nSql = buf.nBuf;
  }
#endif

  if( rc==SQLITE_OK ){
    rc = sqlite3_prepare_v2(db, zSql, nSql, ppStmt, 0);
  }
  sqlite3_free(zSql);
  sqlite3_free(nooptest.aBuf);
  sqlite3_free(pkfield.aBuf);
  sqlite3_free(pkvar.aBuf);
  sqlite3_free(cols.aBuf);
  return rc;
}

/*
** Bind the PRIMARY KEY values from the change passed in argument pChange
** to the SELECT statement passed as the first argument. The SELECT statement
** is as prepared by function sessionSelectStmt().
**
** Return SQLITE_OK if all PK values are successfully bound, or an SQLite
** error code (e.g. SQLITE_NOMEM) otherwise.
*/
static int sessionSelectBind(
  sqlite3_stmt *pSelect,          /* SELECT from sessionSelectStmt() */
  int nCol,                       /* Number of columns in table */
  u8 *abPK,                       /* PRIMARY KEY array */
  SessionChange *pChange          /* Change structure */
){
  int i;
  int rc = SQLITE_OK;
  u8 *a = pChange->aRecord;

  for(i=0; i<nCol && rc==SQLITE_OK; i++){
    int eType = *a++;

    switch( eType ){
      case 0:
      case SQLITE_NULL:
        assert( abPK[i]==0 );
        break;

      case SQLITE_INTEGER: {
        if( abPK[i] ){
          i64 iVal = sessionGetI64(a);
          rc = sqlite3_bind_int64(pSelect, i+1, iVal);
        }
        a += 8;
        break;
      }

      case SQLITE_FLOAT: {
        if( abPK[i] ){
          double rVal;
          i64 iVal = sessionGetI64(a);
          memcpy(&rVal, &iVal, 8);
          rc = sqlite3_bind_double(pSelect, i+1, rVal);
        }
        a += 8;
        break;
      }

      case SQLITE_TEXT: {
        int n;
        a += sessionVarintGet(a, &n);
        if( abPK[i] ){
          rc = sqlite3_bind_text(pSelect, i+1, (char *)a, n, SQLITE_TRANSIENT);
        }
        a += n;
        break;
      }

      default: {
        int n;
        assert( eType==SQLITE_BLOB );
        a += sessionVarintGet(a, &n);
        if( abPK[i] ){
          rc = sqlite3_bind_blob(pSelect, i+1, a, n, SQLITE_TRANSIENT);
        }
        a += n;
        break;
      }
    }
  }

  return rc;
}

/*
** This function is a no-op if *pRc is set to other than SQLITE_OK when it
** is called. Otherwise, append a serialized table header (part of the binary 
** changeset format) to buffer *pBuf. If an error occurs, set *pRc to an
** SQLite error code before returning.
*/
static void sessionAppendTableHdr(
  SessionBuffer *pBuf,            /* Append header to this buffer */
  int bPatchset,                  /* Use the patchset format if true */
  SessionTable *pTab,             /* Table object to append header for */
  int *pRc                        /* IN/OUT: Error code */
){
  /* Write a table header */
  sessionAppendByte(pBuf, (bPatchset ? 'P' : 'T'), pRc);
  sessionAppendVarint(pBuf, pTab->nCol, pRc);
  sessionAppendBlob(pBuf, pTab->abPK, pTab->nCol, pRc);
  sessionAppendBlob(pBuf, (u8 *)pTab->zName, (int)strlen(pTab->zName)+1, pRc);
}

/*
** Generate either a changeset (if argument bPatchset is zero) or a patchset
** (if it is non-zero) based on the current contents of the session object
** passed as the first argument.
**
** If no error occurs, SQLITE_OK is returned and the new changeset/patchset
** stored in output variables *pnChangeset and *ppChangeset. Or, if an error
** occurs, an SQLite error code is returned and both output variables set 
** to 0.
*/
static int sessionGenerateChangeset(
  sqlite3_session *pSession,      /* Session object */
  int bPatchset,                  /* True for patchset, false for changeset */
  int (*xOutput)(void *pOut, const void *pData, int nData),
  void *pOut,                     /* First argument for xOutput */
  int *pnChangeset,               /* OUT: Size of buffer at *ppChangeset */
  void **ppChangeset              /* OUT: Buffer containing changeset */
){
  sqlite3 *db = pSession->db;     /* Source database handle */
  SessionTable *pTab;             /* Used to iterate through attached tables */
  SessionBuffer buf = {0,0,0};    /* Buffer in which to accumulate changeset */
  int rc;                         /* Return code */

  assert( xOutput==0 || (pnChangeset==0 && ppChangeset==0) );
  assert( xOutput!=0 || (pnChangeset!=0 && ppChangeset!=0) );

  /* Zero the output variables in case an error occurs. If this session
  ** object is already in the error state (sqlite3_session.rc != SQLITE_OK),
  ** this call will be a no-op.  */
  if( xOutput==0 ){
    assert( pnChangeset!=0  && ppChangeset!=0 );
    *pnChangeset = 0;
    *ppChangeset = 0;
  }

  if( pSession->rc ) return pSession->rc;
  rc = sqlite3_exec(pSession->db, "SAVEPOINT changeset", 0, 0, 0);
  if( rc!=SQLITE_OK ) return rc;

  sqlite3_mutex_enter(sqlite3_db_mutex(db));

  for(pTab=pSession->pTable; rc==SQLITE_OK && pTab; pTab=pTab->pNext){
    if( pTab->nEntry ){
      const char *zName = pTab->zName;
      int i;                      /* Used to iterate through hash buckets */
      sqlite3_stmt *pSel = 0;     /* SELECT statement to query table pTab */
      int nRewind = buf.nBuf;     /* Initial size of write buffer */
      int nNoop;                  /* Size of buffer after writing tbl header */
      int nOldCol = pTab->nCol;

      /* Check the table schema is still Ok. */
      rc = sessionReinitTable(pSession, pTab);
      if( rc==SQLITE_OK && pTab->nCol!=nOldCol ){
        rc = sessionUpdateChanges(pSession, pTab);
      }

      /* Write a table header */
      sessionAppendTableHdr(&buf, bPatchset, pTab, &rc);

      /* Build and compile a statement to execute: */
      if( rc==SQLITE_OK ){
        rc = sessionSelectStmt(db, 0, pSession->zDb, 
            zName, pTab->bRowid, pTab->nCol, pTab->azCol, pTab->abPK, &pSel
        );
      }

      nNoop = buf.nBuf;
      for(i=0; i<pTab->nChange && rc==SQLITE_OK; i++){
        SessionChange *p;         /* Used to iterate through changes */

        for(p=pTab->apChange[i]; rc==SQLITE_OK && p; p=p->pNext){
          rc = sessionSelectBind(pSel, pTab->nCol, pTab->abPK, p);
          if( rc!=SQLITE_OK ) continue;
          if( sqlite3_step(pSel)==SQLITE_ROW ){
            if( p->op==SQLITE_INSERT ){
              int iCol;
              sessionAppendByte(&buf, SQLITE_INSERT, &rc);
              sessionAppendByte(&buf, p->bIndirect, &rc);
              for(iCol=0; iCol<pTab->nCol; iCol++){
                sessionAppendCol(&buf, pSel, iCol, &rc);
              }
            }else{
              assert( pTab->abPK!=0 );
              rc = sessionAppendUpdate(&buf, bPatchset, pSel, p, pTab->abPK);
            }
          }else if( p->op!=SQLITE_INSERT ){
            rc = sessionAppendDelete(&buf, bPatchset, p, pTab->nCol,pTab->abPK);
          }
          if( rc==SQLITE_OK ){
            rc = sqlite3_reset(pSel);
          }

          /* If the buffer is now larger than sessions_strm_chunk_size, pass
          ** its contents to the xOutput() callback. */
          if( xOutput 
           && rc==SQLITE_OK 
           && buf.nBuf>nNoop 
           && buf.nBuf>sessions_strm_chunk_size 
          ){
            rc = xOutput(pOut, (void*)buf.aBuf, buf.nBuf);
            nNoop = -1;
            buf.nBuf = 0;
          }

        }
      }

      sqlite3_finalize(pSel);
      if( buf.nBuf==nNoop ){
        buf.nBuf = nRewind;
      }
    }
  }

  if( rc==SQLITE_OK ){
    if( xOutput==0 ){
      *pnChangeset = buf.nBuf;
      *ppChangeset = buf.aBuf;
      buf.aBuf = 0;
    }else if( buf.nBuf>0 ){
      rc = xOutput(pOut, (void*)buf.aBuf, buf.nBuf);
    }
  }

  sqlite3_free(buf.aBuf);
  sqlite3_exec(db, "RELEASE changeset", 0, 0, 0);
  sqlite3_mutex_leave(sqlite3_db_mutex(db));
  return rc;
}

/*
** Obtain a changeset object containing all changes recorded by the 
** session object passed as the first argument.
**
** It is the responsibility of the caller to eventually free the buffer 
** using sqlite3_free().
*/
int sqlite3session_changeset(
  sqlite3_session *pSession,      /* Session object */
  int *pnChangeset,               /* OUT: Size of buffer at *ppChangeset */
  void **ppChangeset              /* OUT: Buffer containing changeset */
){
  int rc;

  if( pnChangeset==0 || ppChangeset==0 ) return SQLITE_MISUSE;
  rc = sessionGenerateChangeset(pSession, 0, 0, 0, pnChangeset, ppChangeset);
  assert( rc || pnChangeset==0 
       || pSession->bEnableSize==0 || *pnChangeset<=pSession->nMaxChangesetSize 
  );
  return rc;
}

/*
** Streaming version of sqlite3session_changeset().
*/
int sqlite3session_changeset_strm(
  sqlite3_session *pSession,
  int (*xOutput)(void *pOut, const void *pData, int nData),
  void *pOut
){
  if( xOutput==0 ) return SQLITE_MISUSE;
  return sessionGenerateChangeset(pSession, 0, xOutput, pOut, 0, 0);
}

/*
** Streaming version of sqlite3session_patchset().
*/
int sqlite3session_patchset_strm(
  sqlite3_session *pSession,
  int (*xOutput)(void *pOut, const void *pData, int nData),
  void *pOut
){
  if( xOutput==0 ) return SQLITE_MISUSE;
  return sessionGenerateChangeset(pSession, 1, xOutput, pOut, 0, 0);
}

/*
** Obtain a patchset object containing all changes recorded by the 
** session object passed as the first argument.
**
** It is the responsibility of the caller to eventually free the buffer 
** using sqlite3_free().
*/
int sqlite3session_patchset(
  sqlite3_session *pSession,      /* Session object */
  int *pnPatchset,                /* OUT: Size of buffer at *ppChangeset */
  void **ppPatchset               /* OUT: Buffer containing changeset */
){
  if( pnPatchset==0 || ppPatchset==0 ) return SQLITE_MISUSE;
  return sessionGenerateChangeset(pSession, 1, 0, 0, pnPatchset, ppPatchset);
}

/*
** Enable or disable the session object passed as the first argument.
*/
int sqlite3session_enable(sqlite3_session *pSession, int bEnable){
  int ret;
  sqlite3_mutex_enter(sqlite3_db_mutex(pSession->db));
  if( bEnable>=0 ){
    pSession->bEnable = bEnable;
  }
  ret = pSession->bEnable;
  sqlite3_mutex_leave(sqlite3_db_mutex(pSession->db));
  return ret;
}

/*
** Enable or disable the session object passed as the first argument.
*/
int sqlite3session_indirect(sqlite3_session *pSession, int bIndirect){
  int ret;
  sqlite3_mutex_enter(sqlite3_db_mutex(pSession->db));
  if( bIndirect>=0 ){
    pSession->bIndirect = bIndirect;
  }
  ret = pSession->bIndirect;
  sqlite3_mutex_leave(sqlite3_db_mutex(pSession->db));
  return ret;
}

/*
** Return true if there have been no changes to monitored tables recorded
** by the session object passed as the only argument.
*/
int sqlite3session_isempty(sqlite3_session *pSession){
  int ret = 0;
  SessionTable *pTab;

  sqlite3_mutex_enter(sqlite3_db_mutex(pSession->db));
  for(pTab=pSession->pTable; pTab && ret==0; pTab=pTab->pNext){
    ret = (pTab->nEntry>0);
  }
  sqlite3_mutex_leave(sqlite3_db_mutex(pSession->db));

  return (ret==0);
}

/*
** Return the amount of heap memory in use.
*/
sqlite3_int64 sqlite3session_memory_used(sqlite3_session *pSession){
  return pSession->nMalloc;
}

/*
** Configure the session object passed as the first argument.
*/
int sqlite3session_object_config(sqlite3_session *pSession, int op, void *pArg){
  int rc = SQLITE_OK;
  switch( op ){
    case SQLITE_SESSION_OBJCONFIG_SIZE: {
      int iArg = *(int*)pArg;
      if( iArg>=0 ){
        if( pSession->pTable ){
          rc = SQLITE_MISUSE;
        }else{
          pSession->bEnableSize = (iArg!=0);
        }
      }
      *(int*)pArg = pSession->bEnableSize;
      break;
    }

    case SQLITE_SESSION_OBJCONFIG_ROWID: {
      int iArg = *(int*)pArg;
      if( iArg>=0 ){
        if( pSession->pTable ){
          rc = SQLITE_MISUSE;
        }else{
          pSession->bImplicitPK = (iArg!=0);
        }
      }
      *(int*)pArg = pSession->bImplicitPK;
      break;
    }

    default:
      rc = SQLITE_MISUSE;
  }

  return rc;
}

/*
** Return the maximum size of sqlite3session_changeset() output.
*/
sqlite3_int64 sqlite3session_changeset_size(sqlite3_session *pSession){
  return pSession->nMaxChangesetSize;
}

/*
** Do the work for either sqlite3changeset_start() or start_strm().
*/
static int sessionChangesetStart(
  sqlite3_changeset_iter **pp,    /* OUT: Changeset iterator handle */
  int (*xInput)(void *pIn, void *pData, int *pnData),
  void *pIn,
  int nChangeset,                 /* Size of buffer pChangeset in bytes */
  void *pChangeset,               /* Pointer to buffer containing changeset */
  int bInvert,                    /* True to invert changeset */
  int bSkipEmpty                  /* True to skip empty UPDATE changes */
){
  sqlite3_changeset_iter *pRet;   /* Iterator to return */
  int nByte;                      /* Number of bytes to allocate for iterator */

  assert( xInput==0 || (pChangeset==0 && nChangeset==0) );

  /* Zero the output variable in case an error occurs. */
  *pp = 0;

  /* Allocate and initialize the iterator structure. */
  nByte = sizeof(sqlite3_changeset_iter);
  pRet = (sqlite3_changeset_iter *)sqlite3_malloc(nByte);
  if( !pRet ) return SQLITE_NOMEM;
  memset(pRet, 0, sizeof(sqlite3_changeset_iter));
  pRet->in.aData = (u8 *)pChangeset;
  pRet->in.nData = nChangeset;
  pRet->in.xInput = xInput;
  pRet->in.pIn = pIn;
  pRet->in.bEof = (xInput ? 0 : 1);
  pRet->bInvert = bInvert;
  pRet->bSkipEmpty = bSkipEmpty;

  /* Populate the output variable and return success. */
  *pp = pRet;
  return SQLITE_OK;
}

/*
** Create an iterator used to iterate through the contents of a changeset.
*/
int sqlite3changeset_start(
  sqlite3_changeset_iter **pp,    /* OUT: Changeset iterator handle */
  int nChangeset,                 /* Size of buffer pChangeset in bytes */
  void *pChangeset                /* Pointer to buffer containing changeset */
){
  return sessionChangesetStart(pp, 0, 0, nChangeset, pChangeset, 0, 0);
}
int sqlite3changeset_start_v2(
  sqlite3_changeset_iter **pp,    /* OUT: Changeset iterator handle */
  int nChangeset,                 /* Size of buffer pChangeset in bytes */
  void *pChangeset,               /* Pointer to buffer containing changeset */
  int flags
){
  int bInvert = !!(flags & SQLITE_CHANGESETSTART_INVERT);
  return sessionChangesetStart(pp, 0, 0, nChangeset, pChangeset, bInvert, 0);
}

/*
** Streaming version of sqlite3changeset_start().
*/
int sqlite3changeset_start_strm(
  sqlite3_changeset_iter **pp,    /* OUT: Changeset iterator handle */
  int (*xInput)(void *pIn, void *pData, int *pnData),
  void *pIn
){
  return sessionChangesetStart(pp, xInput, pIn, 0, 0, 0, 0);
}
int sqlite3changeset_start_v2_strm(
  sqlite3_changeset_iter **pp,    /* OUT: Changeset iterator handle */
  int (*xInput)(void *pIn, void *pData, int *pnData),
  void *pIn,
  int flags
){
  int bInvert = !!(flags & SQLITE_CHANGESETSTART_INVERT);
  return sessionChangesetStart(pp, xInput, pIn, 0, 0, bInvert, 0);
}

/*
** If the SessionInput object passed as the only argument is a streaming
** object and the buffer is full, discard some data to free up space.
*/
static void sessionDiscardData(SessionInput *pIn){
  if( pIn->xInput && pIn->iCurrent>=sessions_strm_chunk_size ){
    int nMove = pIn->buf.nBuf - pIn->iCurrent;
    assert( nMove>=0 );
    if( nMove>0 ){
      memmove(pIn->buf.aBuf, &pIn->buf.aBuf[pIn->iCurrent], nMove);
    }
    pIn->buf.nBuf -= pIn->iCurrent;
    pIn->iNext -= pIn->iCurrent;
    pIn->iCurrent = 0;
    pIn->nData = pIn->buf.nBuf;
  }
}

/*
** Ensure that there are at least nByte bytes available in the buffer. Or,
** if there are not nByte bytes remaining in the input, that all available
** data is in the buffer.
**
** Return an SQLite error code if an error occurs, or SQLITE_OK otherwise.
*/
static int sessionInputBuffer(SessionInput *pIn, int nByte){
  int rc = SQLITE_OK;
  if( pIn->xInput ){
    while( !pIn->bEof && (pIn->iNext+nByte)>=pIn->nData && rc==SQLITE_OK ){
      int nNew = sessions_strm_chunk_size;

      if( pIn->bNoDiscard==0 ) sessionDiscardData(pIn);
      if( SQLITE_OK==sessionBufferGrow(&pIn->buf, nNew, &rc) ){
        rc = pIn->xInput(pIn->pIn, &pIn->buf.aBuf[pIn->buf.nBuf], &nNew);
        if( nNew==0 ){
          pIn->bEof = 1;
        }else{
          pIn->buf.nBuf += nNew;
        }
      }

      pIn->aData = pIn->buf.aBuf;
      pIn->nData = pIn->buf.nBuf;
    }
  }
  return rc;
}

/*
** When this function is called, *ppRec points to the start of a record
** that contains nCol values. This function advances the pointer *ppRec
** until it points to the byte immediately following that record.
*/
static void sessionSkipRecord(
  u8 **ppRec,                     /* IN/OUT: Record pointer */
  int nCol                        /* Number of values in record */
){
  u8 *aRec = *ppRec;
  int i;
  for(i=0; i<nCol; i++){
    int eType = *aRec++;
    if( eType==SQLITE_TEXT || eType==SQLITE_BLOB ){
      int nByte;
      aRec += sessionVarintGet((u8*)aRec, &nByte);
      aRec += nByte;
    }else if( eType==SQLITE_INTEGER || eType==SQLITE_FLOAT ){
      aRec += 8;
    }
  }

  *ppRec = aRec;
}

/*
** This function sets the value of the sqlite3_value object passed as the
** first argument to a copy of the string or blob held in the aData[] 
** buffer. SQLITE_OK is returned if successful, or SQLITE_NOMEM if an OOM
** error occurs.
*/
static int sessionValueSetStr(
  sqlite3_value *pVal,            /* Set the value of this object */
  u8 *aData,                      /* Buffer containing string or blob data */
  int nData,                      /* Size of buffer aData[] in bytes */
  u8 enc                          /* String encoding (0 for blobs) */
){
  /* In theory this code could just pass SQLITE_TRANSIENT as the final
  ** argument to sqlite3ValueSetStr() and have the copy created 
  ** automatically. But doing so makes it difficult to detect any OOM
  ** error. Hence the code to create the copy externally. */
  u8 *aCopy = sqlite3_malloc64((sqlite3_int64)nData+1);
  if( aCopy==0 ) return SQLITE_NOMEM;
  memcpy(aCopy, aData, nData);
  sqlite3ValueSetStr(pVal, nData, (char*)aCopy, enc, sqlite3_free);
  return SQLITE_OK;
}

/*
** Deserialize a single record from a buffer in memory. See "RECORD FORMAT"
** for details.
**
** When this function is called, *paChange points to the start of the record
** to deserialize. Assuming no error occurs, *paChange is set to point to
** one byte after the end of the same record before this function returns.
** If the argument abPK is NULL, then the record contains nCol values. Or,
** if abPK is other than NULL, then the record contains only the PK fields
** (in other words, it is a patchset DELETE record).
**
** If successful, each element of the apOut[] array (allocated by the caller)
** is set to point to an sqlite3_value object containing the value read
** from the corresponding position in the record. If that value is not
** included in the record (i.e. because the record is part of an UPDATE change
** and the field was not modified), the corresponding element of apOut[] is
** set to NULL.
**
** It is the responsibility of the caller to free all sqlite_value structures
** using sqlite3_free().
**
** If an error occurs, an SQLite error code (e.g. SQLITE_NOMEM) is returned.
** The apOut[] array may have been partially populated in this case.
*/
static int sessionReadRecord(
  SessionInput *pIn,              /* Input data */
  int nCol,                       /* Number of values in record */
  u8 *abPK,                       /* Array of primary key flags, or NULL */
  sqlite3_value **apOut,          /* Write values to this array */
  int *pbEmpty
){
  int i;                          /* Used to iterate through columns */
  int rc = SQLITE_OK;

  assert( pbEmpty==0 || *pbEmpty==0 );
  if( pbEmpty ) *pbEmpty = 1;
  for(i=0; i<nCol && rc==SQLITE_OK; i++){
    int eType = 0;                /* Type of value (SQLITE_NULL, TEXT etc.) */
    if( abPK && abPK[i]==0 ) continue;
    rc = sessionInputBuffer(pIn, 9);
    if( rc==SQLITE_OK ){
      if( pIn->iNext>=pIn->nData ){
        rc = SQLITE_CORRUPT_BKPT;
      }else{
        eType = pIn->aData[pIn->iNext++];
        assert( apOut[i]==0 );
        if( eType ){
          if( pbEmpty ) *pbEmpty = 0;
          apOut[i] = sqlite3ValueNew(0);
          if( !apOut[i] ) rc = SQLITE_NOMEM;
        }
      }
    }

    if( rc==SQLITE_OK ){
      u8 *aVal = &pIn->aData[pIn->iNext];
      if( eType==SQLITE_TEXT || eType==SQLITE_BLOB ){
        int nByte;
        pIn->iNext += sessionVarintGet(aVal, &nByte);
        rc = sessionInputBuffer(pIn, nByte);
        if( rc==SQLITE_OK ){
          if( nByte<0 || nByte>pIn->nData-pIn->iNext ){
            rc = SQLITE_CORRUPT_BKPT;
          }else{
            u8 enc = (eType==SQLITE_TEXT ? SQLITE_UTF8 : 0);
            rc = sessionValueSetStr(apOut[i],&pIn->aData[pIn->iNext],nByte,enc);
            pIn->iNext += nByte;
          }
        }
      }
      if( eType==SQLITE_INTEGER || eType==SQLITE_FLOAT ){
        if( (pIn->nData-pIn->iNext)<8 ){
          rc = SQLITE_CORRUPT_BKPT;
        }else{
          sqlite3_int64 v = sessionGetI64(aVal);
          if( eType==SQLITE_INTEGER ){
            sqlite3VdbeMemSetInt64(apOut[i], v);
          }else{
            double d;
            memcpy(&d, &v, 8);
            sqlite3VdbeMemSetDouble(apOut[i], d);
          }
          pIn->iNext += 8;
        }
      }
    }
  }

  return rc;
}

/*
** The input pointer currently points to the second byte of a table-header.
** Specifically, to the following:
**
**   + number of columns in table (varint)
**   + array of PK flags (1 byte per column),
**   + table name (nul terminated).
**
** This function ensures that all of the above is present in the input 
** buffer (i.e. that it can be accessed without any calls to xInput()).
** If successful, SQLITE_OK is returned. Otherwise, an SQLite error code.
** The input pointer is not moved.
*/
static int sessionChangesetBufferTblhdr(SessionInput *pIn, int *pnByte){
  int rc = SQLITE_OK;
  int nCol = 0;
  int nRead = 0;

  rc = sessionInputBuffer(pIn, 9);
  if( rc==SQLITE_OK ){
    nRead += sessionVarintGet(&pIn->aData[pIn->iNext + nRead], &nCol);
    /* The hard upper limit for the number of columns in an SQLite
    ** database table is, according to sqliteLimit.h, 32676. So 
    ** consider any table-header that purports to have more than 65536 
    ** columns to be corrupt. This is convenient because otherwise, 
    ** if the (nCol>65536) condition below were omitted, a sufficiently 
    ** large value for nCol may cause nRead to wrap around and become 
    ** negative. Leading to a crash. */
    if( nCol<0 || nCol>65536 ){
      rc = SQLITE_CORRUPT_BKPT;
    }else{
      rc = sessionInputBuffer(pIn, nRead+nCol+100);
      nRead += nCol;
    }
  }

  while( rc==SQLITE_OK ){
    while( (pIn->iNext + nRead)<pIn->nData && pIn->aData[pIn->iNext + nRead] ){
      nRead++;
    }
    if( (pIn->iNext + nRead)<pIn->nData ) break;
    rc = sessionInputBuffer(pIn, nRead + 100);
  }
  *pnByte = nRead+1;
  return rc;
}

/*
** The input pointer currently points to the first byte of the first field
** of a record consisting of nCol columns. This function ensures the entire
** record is buffered. It does not move the input pointer.
**
** If successful, SQLITE_OK is returned and *pnByte is set to the size of
** the record in bytes. Otherwise, an SQLite error code is returned. The
** final value of *pnByte is undefined in this case.
*/
static int sessionChangesetBufferRecord(
  SessionInput *pIn,              /* Input data */
  int nCol,                       /* Number of columns in record */
  int *pnByte                     /* OUT: Size of record in bytes */
){
  int rc = SQLITE_OK;
  int nByte = 0;
  int i;
  for(i=0; rc==SQLITE_OK && i<nCol; i++){
    int eType;
    rc = sessionInputBuffer(pIn, nByte + 10);
    if( rc==SQLITE_OK ){
      eType = pIn->aData[pIn->iNext + nByte++];
      if( eType==SQLITE_TEXT || eType==SQLITE_BLOB ){
        int n;
        nByte += sessionVarintGet(&pIn->aData[pIn->iNext+nByte], &n);
        nByte += n;
        rc = sessionInputBuffer(pIn, nByte);
      }else if( eType==SQLITE_INTEGER || eType==SQLITE_FLOAT ){
        nByte += 8;
      }
    }
  }
  *pnByte = nByte;
  return rc;
}

/*
** The input pointer currently points to the second byte of a table-header.
** Specifically, to the following:
**
**   + number of columns in table (varint)
**   + array of PK flags (1 byte per column),
**   + table name (nul terminated).
**
** This function decodes the table-header and populates the p->nCol, 
** p->zTab and p->abPK[] variables accordingly. The p->apValue[] array is 
** also allocated or resized according to the new value of p->nCol. The
** input pointer is left pointing to the byte following the table header.
**
** If successful, SQLITE_OK is returned. Otherwise, an SQLite error code
** is returned and the final values of the various fields enumerated above
** are undefined.
*/
static int sessionChangesetReadTblhdr(sqlite3_changeset_iter *p){
  int rc;
  int nCopy;
  assert( p->rc==SQLITE_OK );

  rc = sessionChangesetBufferTblhdr(&p->in, &nCopy);
  if( rc==SQLITE_OK ){
    int nByte;
    int nVarint;
    nVarint = sessionVarintGet(&p->in.aData[p->in.iNext], &p->nCol);
    if( p->nCol>0 ){
      nCopy -= nVarint;
      p->in.iNext += nVarint;
      nByte = p->nCol * sizeof(sqlite3_value*) * 2 + nCopy;
      p->tblhdr.nBuf = 0;
      sessionBufferGrow(&p->tblhdr, nByte, &rc);
    }else{
      rc = SQLITE_CORRUPT_BKPT;
    }
  }

  if( rc==SQLITE_OK ){
    size_t iPK = sizeof(sqlite3_value*)*p->nCol*2;
    memset(p->tblhdr.aBuf, 0, iPK);
    memcpy(&p->tblhdr.aBuf[iPK], &p->in.aData[p->in.iNext], nCopy);
    p->in.iNext += nCopy;
  }

  p->apValue = (sqlite3_value**)p->tblhdr.aBuf;
  if( p->apValue==0 ){
    p->abPK = 0;
    p->zTab = 0;
  }else{
    p->abPK = (u8*)&p->apValue[p->nCol*2];
    p->zTab = p->abPK ? (char*)&p->abPK[p->nCol] : 0;
  }
  return (p->rc = rc);
}

/*
** Advance the changeset iterator to the next change. The differences between
** this function and sessionChangesetNext() are that
**
**   * If pbEmpty is not NULL and the change is a no-op UPDATE (an UPDATE
**     that modifies no columns), this function sets (*pbEmpty) to 1.
**
**   * If the iterator is configured to skip no-op UPDATEs,
**     sessionChangesetNext() does that. This function does not.
*/
static int sessionChangesetNextOne(
  sqlite3_changeset_iter *p,      /* Changeset iterator */
  u8 **paRec,                     /* If non-NULL, store record pointer here */
  int *pnRec,                     /* If non-NULL, store size of record here */
  int *pbNew,                     /* If non-NULL, true if new table */
  int *pbEmpty
){
  int i;
  u8 op;

  assert( (paRec==0 && pnRec==0) || (paRec && pnRec) );
  assert( pbEmpty==0 || *pbEmpty==0 );

  /* If the iterator is in the error-state, return immediately. */
  if( p->rc!=SQLITE_OK ) return p->rc;

  /* Free the current contents of p->apValue[], if any. */
  if( p->apValue ){
    for(i=0; i<p->nCol*2; i++){
      sqlite3ValueFree(p->apValue[i]);
    }
    memset(p->apValue, 0, sizeof(sqlite3_value*)*p->nCol*2);
  }

  /* Make sure the buffer contains at least 10 bytes of input data, or all
  ** remaining data if there are less than 10 bytes available. This is
  ** sufficient either for the 'T' or 'P' byte and the varint that follows
  ** it, or for the two single byte values otherwise. */
  p->rc = sessionInputBuffer(&p->in, 2);
  if( p->rc!=SQLITE_OK ) return p->rc;

  p->in.iCurrent = p->in.iNext;
  sessionDiscardData(&p->in);

  /* If the iterator is already at the end of the changeset, return DONE. */
  if( p->in.iNext>=p->in.nData ){
    return SQLITE_DONE;
  }

  op = p->in.aData[p->in.iNext++];
  while( op=='T' || op=='P' ){
    if( pbNew ) *pbNew = 1;
    p->bPatchset = (op=='P');
    if( sessionChangesetReadTblhdr(p) ) return p->rc;
    if( (p->rc = sessionInputBuffer(&p->in, 2)) ) return p->rc;
    p->in.iCurrent = p->in.iNext;
    if( p->in.iNext>=p->in.nData ) return SQLITE_DONE;
    op = p->in.aData[p->in.iNext++];
  }

  if( p->zTab==0 || (p->bPatchset && p->bInvert) ){
    /* The first record in the changeset is not a table header. Must be a
    ** corrupt changeset. */
    assert( p->in.iNext==1 || p->zTab );
    return (p->rc = SQLITE_CORRUPT_BKPT);
  }

  p->op = op;
  p->bIndirect = p->in.aData[p->in.iNext++];
  if( p->op!=SQLITE_UPDATE && p->op!=SQLITE_DELETE && p->op!=SQLITE_INSERT ){
    return (p->rc = SQLITE_CORRUPT_BKPT);
  }

  if( paRec ){ 
    int nVal;                     /* Number of values to buffer */
    if( p->bPatchset==0 && op==SQLITE_UPDATE ){
      nVal = p->nCol * 2;
    }else if( p->bPatchset && op==SQLITE_DELETE ){
      nVal = 0;
      for(i=0; i<p->nCol; i++) if( p->abPK[i] ) nVal++;
    }else{
      nVal = p->nCol;
    }
    p->rc = sessionChangesetBufferRecord(&p->in, nVal, pnRec);
    if( p->rc!=SQLITE_OK ) return p->rc;
    *paRec = &p->in.aData[p->in.iNext];
    p->in.iNext += *pnRec;
  }else{
    sqlite3_value **apOld = (p->bInvert ? &p->apValue[p->nCol] : p->apValue);
    sqlite3_value **apNew = (p->bInvert ? p->apValue : &p->apValue[p->nCol]);

    /* If this is an UPDATE or DELETE, read the old.* record. */
    if( p->op!=SQLITE_INSERT && (p->bPatchset==0 || p->op==SQLITE_DELETE) ){
      u8 *abPK = p->bPatchset ? p->abPK : 0;
      p->rc = sessionReadRecord(&p->in, p->nCol, abPK, apOld, 0);
      if( p->rc!=SQLITE_OK ) return p->rc;
    }

    /* If this is an INSERT or UPDATE, read the new.* record. */
    if( p->op!=SQLITE_DELETE ){
      p->rc = sessionReadRecord(&p->in, p->nCol, 0, apNew, pbEmpty);
      if( p->rc!=SQLITE_OK ) return p->rc;
    }

    if( (p->bPatchset || p->bInvert) && p->op==SQLITE_UPDATE ){
      /* If this is an UPDATE that is part of a patchset, then all PK and
      ** modified fields are present in the new.* record. The old.* record
      ** is currently completely empty. This block shifts the PK fields from
      ** new.* to old.*, to accommodate the code that reads these arrays.  */
      for(i=0; i<p->nCol; i++){
        assert( p->bPatchset==0 || p->apValue[i]==0 );
        if( p->abPK[i] ){
          assert( p->apValue[i]==0 );
          p->apValue[i] = p->apValue[i+p->nCol];
          if( p->apValue[i]==0 ) return (p->rc = SQLITE_CORRUPT_BKPT);
          p->apValue[i+p->nCol] = 0;
        }
      }
    }else if( p->bInvert ){
      if( p->op==SQLITE_INSERT ) p->op = SQLITE_DELETE;
      else if( p->op==SQLITE_DELETE ) p->op = SQLITE_INSERT;
    }

    /* If this is an UPDATE that is part of a changeset, then check that
    ** there are no fields in the old.* record that are not (a) PK fields,
    ** or (b) also present in the new.* record. 
    **
    ** Such records are technically corrupt, but the rebaser was at one
    ** point generating them. Under most circumstances this is benign, but
    ** can cause spurious SQLITE_RANGE errors when applying the changeset. */
    if( p->bPatchset==0 && p->op==SQLITE_UPDATE){
      for(i=0; i<p->nCol; i++){
        if( p->abPK[i]==0 && p->apValue[i+p->nCol]==0 ){
          sqlite3ValueFree(p->apValue[i]);
          p->apValue[i] = 0;
        }
      }
    }
  }

  return SQLITE_ROW;
}

/*
** Advance the changeset iterator to the next change.
**
** If both paRec and pnRec are NULL, then this function works like the public
** API sqlite3changeset_next(). If SQLITE_ROW is returned, then the
** sqlite3changeset_new() and old() APIs may be used to query for values.
**
** Otherwise, if paRec and pnRec are not NULL, then a pointer to the change
** record is written to *paRec before returning and the number of bytes in
** the record to *pnRec.
**
** Either way, this function returns SQLITE_ROW if the iterator is 
** successfully advanced to the next change in the changeset, an SQLite 
** error code if an error occurs, or SQLITE_DONE if there are no further 
** changes in the changeset.
*/
static int sessionChangesetNext(
  sqlite3_changeset_iter *p,      /* Changeset iterator */
  u8 **paRec,                     /* If non-NULL, store record pointer here */
  int *pnRec,                     /* If non-NULL, store size of record here */
  int *pbNew                      /* If non-NULL, true if new table */
){
  int bEmpty;
  int rc;
  do {
    bEmpty = 0;
    rc = sessionChangesetNextOne(p, paRec, pnRec, pbNew, &bEmpty);
  }while( rc==SQLITE_ROW && p->bSkipEmpty && bEmpty);
  return rc;
}

/*
** Advance an iterator created by sqlite3changeset_start() to the next
** change in the changeset. This function may return SQLITE_ROW, SQLITE_DONE
** or SQLITE_CORRUPT.
**
** This function may not be called on iterators passed to a conflict handler
** callback by changeset_apply().
*/
int sqlite3changeset_next(sqlite3_changeset_iter *p){
  return sessionChangesetNext(p, 0, 0, 0);
}

/*
** The following function extracts information on the current change
** from a changeset iterator. It may only be called after changeset_next()
** has returned SQLITE_ROW.
*/
int sqlite3changeset_op(
  sqlite3_changeset_iter *pIter,  /* Iterator handle */
  const char **pzTab,             /* OUT: Pointer to table name */
  int *pnCol,                     /* OUT: Number of columns in table */
  int *pOp,                       /* OUT: SQLITE_INSERT, DELETE or UPDATE */
  int *pbIndirect                 /* OUT: True if change is indirect */
){
  *pOp = pIter->op;
  *pnCol = pIter->nCol;
  *pzTab = pIter->zTab;
  if( pbIndirect ) *pbIndirect = pIter->bIndirect;
  return SQLITE_OK;
}

/*
** Return information regarding the PRIMARY KEY and number of columns in
** the database table affected by the change that pIter currently points
** to. This function may only be called after changeset_next() returns
** SQLITE_ROW.
*/
int sqlite3changeset_pk(
  sqlite3_changeset_iter *pIter,  /* Iterator object */
  unsigned char **pabPK,          /* OUT: Array of boolean - true for PK cols */
  int *pnCol                      /* OUT: Number of entries in output array */
){
  *pabPK = pIter->abPK;
  if( pnCol ) *pnCol = pIter->nCol;
  return SQLITE_OK;
}

/*
** This function may only be called while the iterator is pointing to an
** SQLITE_UPDATE or SQLITE_DELETE change (see sqlite3changeset_op()).
** Otherwise, SQLITE_MISUSE is returned.
**
** It sets *ppValue to point to an sqlite3_value structure containing the
** iVal'th value in the old.* record. Or, if that particular value is not
** included in the record (because the change is an UPDATE and the field
** was not modified and is not a PK column), set *ppValue to NULL.
**
** If value iVal is out-of-range, SQLITE_RANGE is returned and *ppValue is
** not modified. Otherwise, SQLITE_OK.
*/
int sqlite3changeset_old(
  sqlite3_changeset_iter *pIter,  /* Changeset iterator */
  int iVal,                       /* Index of old.* value to retrieve */
  sqlite3_value **ppValue         /* OUT: Old value (or NULL pointer) */
){
  if( pIter->op!=SQLITE_UPDATE && pIter->op!=SQLITE_DELETE ){
    return SQLITE_MISUSE;
  }
  if( iVal<0 || iVal>=pIter->nCol ){
    return SQLITE_RANGE;
  }
  *ppValue = pIter->apValue[iVal];
  return SQLITE_OK;
}

/*
** This function may only be called while the iterator is pointing to an
** SQLITE_UPDATE or SQLITE_INSERT change (see sqlite3changeset_op()).
** Otherwise, SQLITE_MISUSE is returned.
**
** It sets *ppValue to point to an sqlite3_value structure containing the
** iVal'th value in the new.* record. Or, if that particular value is not
** included in the record (because the change is an UPDATE and the field
** was not modified), set *ppValue to NULL.
**
** If value iVal is out-of-range, SQLITE_RANGE is returned and *ppValue is
** not modified. Otherwise, SQLITE_OK.
*/
int sqlite3changeset_new(
  sqlite3_changeset_iter *pIter,  /* Changeset iterator */
  int iVal,                       /* Index of new.* value to retrieve */
  sqlite3_value **ppValue         /* OUT: New value (or NULL pointer) */
){
  if( pIter->op!=SQLITE_UPDATE && pIter->op!=SQLITE_INSERT ){
    return SQLITE_MISUSE;
  }
  if( iVal<0 || iVal>=pIter->nCol ){
    return SQLITE_RANGE;
  }
  *ppValue = pIter->apValue[pIter->nCol+iVal];
  return SQLITE_OK;
}

/*
** The following two macros are used internally. They are similar to the
** sqlite3changeset_new() and sqlite3changeset_old() functions, except that
** they omit all error checking and return a pointer to the requested value.
*/
#define sessionChangesetNew(pIter, iVal) (pIter)->apValue[(pIter)->nCol+(iVal)]
#define sessionChangesetOld(pIter, iVal) (pIter)->apValue[(iVal)]

/*
** This function may only be called with a changeset iterator that has been
** passed to an SQLITE_CHANGESET_DATA or SQLITE_CHANGESET_CONFLICT 
** conflict-handler function. Otherwise, SQLITE_MISUSE is returned.
**
** If successful, *ppValue is set to point to an sqlite3_value structure
** containing the iVal'th value of the conflicting record.
**
** If value iVal is out-of-range or some other error occurs, an SQLite error
** code is returned. Otherwise, SQLITE_OK.
*/
int sqlite3changeset_conflict(
  sqlite3_changeset_iter *pIter,  /* Changeset iterator */
  int iVal,                       /* Index of conflict record value to fetch */
  sqlite3_value **ppValue         /* OUT: Value from conflicting row */
){
  if( !pIter->pConflict ){
    return SQLITE_MISUSE;
  }
  if( iVal<0 || iVal>=pIter->nCol ){
    return SQLITE_RANGE;
  }
  *ppValue = sqlite3_column_value(pIter->pConflict, iVal);
  return SQLITE_OK;
}

/*
** This function may only be called with an iterator passed to an
** SQLITE_CHANGESET_FOREIGN_KEY conflict handler callback. In this case
** it sets the output variable to the total number of known foreign key
** violations in the destination database and returns SQLITE_OK.
**
** In all other cases this function returns SQLITE_MISUSE.
*/
int sqlite3changeset_fk_conflicts(
  sqlite3_changeset_iter *pIter,  /* Changeset iterator */
  int *pnOut                      /* OUT: Number of FK violations */
){
  if( pIter->pConflict || pIter->apValue ){
    return SQLITE_MISUSE;
  }
  *pnOut = pIter->nCol;
  return SQLITE_OK;
}


/*
** Finalize an iterator allocated with sqlite3changeset_start().
**
** This function may not be called on iterators passed to a conflict handler
** callback by changeset_apply().
*/
int sqlite3changeset_finalize(sqlite3_changeset_iter *p){
  int rc = SQLITE_OK;
  if( p ){
    int i;                        /* Used to iterate through p->apValue[] */
    rc = p->rc;
    if( p->apValue ){
      for(i=0; i<p->nCol*2; i++) sqlite3ValueFree(p->apValue[i]);
    }
    sqlite3_free(p->tblhdr.aBuf);
    sqlite3_free(p->in.buf.aBuf);
    sqlite3_free(p);
  }
  return rc;
}

static int sessionChangesetInvert(
  SessionInput *pInput,           /* Input changeset */
  int (*xOutput)(void *pOut, const void *pData, int nData),
  void *pOut,
  int *pnInverted,                /* OUT: Number of bytes in output changeset */
  void **ppInverted               /* OUT: Inverse of pChangeset */
){
  int rc = SQLITE_OK;             /* Return value */
  SessionBuffer sOut;             /* Output buffer */
  int nCol = 0;                   /* Number of cols in current table */
  u8 *abPK = 0;                   /* PK array for current table */
  sqlite3_value **apVal = 0;      /* Space for values for UPDATE inversion */
  SessionBuffer sPK = {0, 0, 0};  /* PK array for current table */

  /* Initialize the output buffer */
  memset(&sOut, 0, sizeof(SessionBuffer));

  /* Zero the output variables in case an error occurs. */
  if( ppInverted ){
    *ppInverted = 0;
    *pnInverted = 0;
  }

  while( 1 ){
    u8 eType;

    /* Test for EOF. */
    if( (rc = sessionInputBuffer(pInput, 2)) ) goto finished_invert;
    if( pInput->iNext>=pInput->nData ) break;
    eType = pInput->aData[pInput->iNext];

    switch( eType ){
      case 'T': {
        /* A 'table' record consists of:
        **
        **   * A constant 'T' character,
        **   * Number of columns in said table (a varint),
        **   * An array of nCol bytes (sPK),
        **   * A nul-terminated table name.
        */
        int nByte;
        int nVar;
        pInput->iNext++;
        if( (rc = sessionChangesetBufferTblhdr(pInput, &nByte)) ){
          goto finished_invert;
        }
        nVar = sessionVarintGet(&pInput->aData[pInput->iNext], &nCol);
        sPK.nBuf = 0;
        sessionAppendBlob(&sPK, &pInput->aData[pInput->iNext+nVar], nCol, &rc);
        sessionAppendByte(&sOut, eType, &rc);
        sessionAppendBlob(&sOut, &pInput->aData[pInput->iNext], nByte, &rc);
        if( rc ) goto finished_invert;

        pInput->iNext += nByte;
        sqlite3_free(apVal);
        apVal = 0;
        abPK = sPK.aBuf;
        break;
      }

      case SQLITE_INSERT:
      case SQLITE_DELETE: {
        int nByte;
        int bIndirect = pInput->aData[pInput->iNext+1];
        int eType2 = (eType==SQLITE_DELETE ? SQLITE_INSERT : SQLITE_DELETE);
        pInput->iNext += 2;
        assert( rc==SQLITE_OK );
        rc = sessionChangesetBufferRecord(pInput, nCol, &nByte);
        sessionAppendByte(&sOut, eType2, &rc);
        sessionAppendByte(&sOut, bIndirect, &rc);
        sessionAppendBlob(&sOut, &pInput->aData[pInput->iNext], nByte, &rc);
        pInput->iNext += nByte;
        if( rc ) goto finished_invert;
        break;
      }

      case SQLITE_UPDATE: {
        int iCol;

        if( 0==apVal ){
          apVal = (sqlite3_value **)sqlite3_malloc64(sizeof(apVal[0])*nCol*2);
          if( 0==apVal ){
            rc = SQLITE_NOMEM;
            goto finished_invert;
          }
          memset(apVal, 0, sizeof(apVal[0])*nCol*2);
        }

        /* Write the header for the new UPDATE change. Same as the original. */
        sessionAppendByte(&sOut, eType, &rc);
        sessionAppendByte(&sOut, pInput->aData[pInput->iNext+1], &rc);

        /* Read the old.* and new.* records for the update change. */
        pInput->iNext += 2;
        rc = sessionReadRecord(pInput, nCol, 0, &apVal[0], 0);
        if( rc==SQLITE_OK ){
          rc = sessionReadRecord(pInput, nCol, 0, &apVal[nCol], 0);
        }

        /* Write the new old.* record. Consists of the PK columns from the
        ** original old.* record, and the other values from the original
        ** new.* record. */
        for(iCol=0; iCol<nCol; iCol++){
          sqlite3_value *pVal = apVal[iCol + (abPK[iCol] ? 0 : nCol)];
          sessionAppendValue(&sOut, pVal, &rc);
        }

        /* Write the new new.* record. Consists of a copy of all values
        ** from the original old.* record, except for the PK columns, which
        ** are set to "undefined". */
        for(iCol=0; iCol<nCol; iCol++){
          sqlite3_value *pVal = (abPK[iCol] ? 0 : apVal[iCol]);
          sessionAppendValue(&sOut, pVal, &rc);
        }

        for(iCol=0; iCol<nCol*2; iCol++){
          sqlite3ValueFree(apVal[iCol]);
        }
        memset(apVal, 0, sizeof(apVal[0])*nCol*2);
        if( rc!=SQLITE_OK ){
          goto finished_invert;
        }

        break;
      }

      default:
        rc = SQLITE_CORRUPT_BKPT;
        goto finished_invert;
    }

    assert( rc==SQLITE_OK );
    if( xOutput && sOut.nBuf>=sessions_strm_chunk_size ){
      rc = xOutput(pOut, sOut.aBuf, sOut.nBuf);
      sOut.nBuf = 0;
      if( rc!=SQLITE_OK ) goto finished_invert;
    }
  }

  assert( rc==SQLITE_OK );
  if( pnInverted && ALWAYS(ppInverted) ){
    *pnInverted = sOut.nBuf;
    *ppInverted = sOut.aBuf;
    sOut.aBuf = 0;
  }else if( sOut.nBuf>0 && ALWAYS(xOutput!=0) ){
    rc = xOutput(pOut, sOut.aBuf, sOut.nBuf);
  }

 finished_invert:
  sqlite3_free(sOut.aBuf);
  sqlite3_free(apVal);
  sqlite3_free(sPK.aBuf);
  return rc;
}


/*
** Invert a changeset object.
*/
int sqlite3changeset_invert(
  int nChangeset,                 /* Number of bytes in input */
  const void *pChangeset,         /* Input changeset */
  int *pnInverted,                /* OUT: Number of bytes in output changeset */
  void **ppInverted               /* OUT: Inverse of pChangeset */
){
  SessionInput sInput;

  /* Set up the input stream */
  memset(&sInput, 0, sizeof(SessionInput));
  sInput.nData = nChangeset;
  sInput.aData = (u8*)pChangeset;

  return sessionChangesetInvert(&sInput, 0, 0, pnInverted, ppInverted);
}

/*
** Streaming version of sqlite3changeset_invert().
*/
int sqlite3changeset_invert_strm(
  int (*xInput)(void *pIn, void *pData, int *pnData),
  void *pIn,
  int (*xOutput)(void *pOut, const void *pData, int nData),
  void *pOut
){
  SessionInput sInput;
  int rc;

  /* Set up the input stream */
  memset(&sInput, 0, sizeof(SessionInput));
  sInput.xInput = xInput;
  sInput.pIn = pIn;

  rc = sessionChangesetInvert(&sInput, xOutput, pOut, 0, 0);
  sqlite3_free(sInput.buf.aBuf);
  return rc;
}


typedef struct SessionUpdate SessionUpdate;
struct SessionUpdate {
  sqlite3_stmt *pStmt;
  u32 *aMask;
  SessionUpdate *pNext;
};

typedef struct SessionApplyCtx SessionApplyCtx;
struct SessionApplyCtx {
  sqlite3 *db;
  sqlite3_stmt *pDelete;          /* DELETE statement */
  sqlite3_stmt *pInsert;          /* INSERT statement */
  sqlite3_stmt *pSelect;          /* SELECT statement */
  int nCol;                       /* Size of azCol[] and abPK[] arrays */
  const char **azCol;             /* Array of column names */
  u8 *abPK;                       /* Boolean array - true if column is in PK */
  u32 *aUpdateMask;               /* Used by sessionUpdateFind */
  SessionUpdate *pUp;
  int bStat1;                     /* True if table is sqlite_stat1 */
  int bDeferConstraints;          /* True to defer constraints */
  int bInvertConstraints;         /* Invert when iterating constraints buffer */
  SessionBuffer constraints;      /* Deferred constraints are stored here */
  SessionBuffer rebase;           /* Rebase information (if any) here */
  u8 bRebaseStarted;              /* If table header is already in rebase */
  u8 bRebase;                     /* True to collect rebase information */
  u8 bIgnoreNoop;                 /* True to ignore no-op conflicts */
  int bRowid;
};

/* Number of prepared UPDATE statements to cache. */
#define SESSION_UPDATE_CACHE_SZ 12

/*
** Find a prepared UPDATE statement suitable for the UPDATE step currently
** being visited by the iterator. The UPDATE is of the form:
**
**   UPDATE tbl SET col = ?, col2 = ? WHERE pk1 IS ? AND pk2 IS ?
*/
static int sessionUpdateFind(
  sqlite3_changeset_iter *pIter,
  SessionApplyCtx *p,
  int bPatchset,
  sqlite3_stmt **ppStmt
){
  int rc = SQLITE_OK;
  SessionUpdate *pUp = 0;
  int nCol = pIter->nCol;
  int nU32 = (pIter->nCol+33)/32;
  int ii;

  if( p->aUpdateMask==0 ){
    p->aUpdateMask = sqlite3_malloc(nU32*sizeof(u32));
    if( p->aUpdateMask==0 ){
      rc = SQLITE_NOMEM;
    }
  }

  if( rc==SQLITE_OK ){
    memset(p->aUpdateMask, 0, nU32*sizeof(u32));
    rc = SQLITE_CORRUPT;
    for(ii=0; ii<pIter->nCol; ii++){
      if( sessionChangesetNew(pIter, ii) ){
        p->aUpdateMask[ii/32] |= (1<<(ii%32));
        rc = SQLITE_OK;
      }
    }
  }

  if( rc==SQLITE_OK ){
    if( bPatchset ) p->aUpdateMask[nCol/32] |= (1<<(nCol%32));

    if( p->pUp ){
      int nUp = 0;
      SessionUpdate **pp = &p->pUp;
      while( 1 ){
        nUp++;
        if( 0==memcmp(p->aUpdateMask, (*pp)->aMask, nU32*sizeof(u32)) ){
          pUp = *pp;
          *pp = pUp->pNext;
          pUp->pNext = p->pUp;
          p->pUp = pUp;
          break;
        }

        if( (*pp)->pNext ){
          pp = &(*pp)->pNext;
        }else{
          if( nUp>=SESSION_UPDATE_CACHE_SZ ){
            sqlite3_finalize((*pp)->pStmt);
            sqlite3_free(*pp);
            *pp = 0;
          }
          break;
        }
      }
    }

    if( pUp==0 ){
      int nByte = sizeof(SessionUpdate) * nU32*sizeof(u32);
      int bStat1 = (sqlite3_stricmp(pIter->zTab, "sqlite_stat1")==0);
      pUp = (SessionUpdate*)sqlite3_malloc(nByte);
      if( pUp==0 ){
        rc = SQLITE_NOMEM;
      }else{
        const char *zSep = "";
        SessionBuffer buf;

        memset(&buf, 0, sizeof(buf));
        pUp->aMask = (u32*)&pUp[1];
        memcpy(pUp->aMask, p->aUpdateMask, nU32*sizeof(u32));

        sessionAppendStr(&buf, "UPDATE main.", &rc);
        sessionAppendIdent(&buf, pIter->zTab, &rc);
        sessionAppendStr(&buf, " SET ", &rc);

        /* Create the assignments part of the UPDATE */
        for(ii=0; ii<pIter->nCol; ii++){
          if( p->abPK[ii]==0 && sessionChangesetNew(pIter, ii) ){
            sessionAppendStr(&buf, zSep, &rc);
            sessionAppendIdent(&buf, p->azCol[ii], &rc);
            sessionAppendStr(&buf, " = ?", &rc);
            sessionAppendInteger(&buf, ii*2+1, &rc);
            zSep = ", ";
          }
        }

        /* Create the WHERE clause part of the UPDATE */
        zSep = "";
        sessionAppendStr(&buf, " WHERE ", &rc);
        for(ii=0; ii<pIter->nCol; ii++){
          if( p->abPK[ii] || (bPatchset==0 && sessionChangesetOld(pIter, ii)) ){
            sessionAppendStr(&buf, zSep, &rc);
            if( bStat1 && ii==1 ){
              assert( sqlite3_stricmp(p->azCol[ii], "idx")==0 );
              sessionAppendStr(&buf, 
                  "idx IS CASE "
                  "WHEN length(?4)=0 AND typeof(?4)='blob' THEN NULL "
                  "ELSE ?4 END ", &rc
              );
            }else{
              sessionAppendIdent(&buf, p->azCol[ii], &rc);
              sessionAppendStr(&buf, " IS ?", &rc);
              sessionAppendInteger(&buf, ii*2+2, &rc);
            }
            zSep = " AND ";
          }
        }

        if( rc==SQLITE_OK ){
          char *zSql = (char*)buf.aBuf;
          rc = sqlite3_prepare_v2(p->db, zSql, buf.nBuf, &pUp->pStmt, 0);
        }

        if( rc!=SQLITE_OK ){
          sqlite3_free(pUp);
          pUp = 0;
        }else{
          pUp->pNext = p->pUp;
          p->pUp = pUp;
        }
        sqlite3_free(buf.aBuf);
      }
    }
  }

  assert( (rc==SQLITE_OK)==(pUp!=0) );
  if( pUp ){
    *ppStmt = pUp->pStmt;
  }else{
    *ppStmt = 0;
  }
  return rc;
}

/*
** Free all cached UPDATE statements.
*/
static void sessionUpdateFree(SessionApplyCtx *p){
  SessionUpdate *pUp;
  SessionUpdate *pNext;
  for(pUp=p->pUp; pUp; pUp=pNext){
    pNext = pUp->pNext;
    sqlite3_finalize(pUp->pStmt);
    sqlite3_free(pUp);
  }
  p->pUp = 0;
  sqlite3_free(p->aUpdateMask);
  p->aUpdateMask = 0;
}

/*
** Formulate a statement to DELETE a row from database db. Assuming a table
** structure like this:
**
**     CREATE TABLE x(a, b, c, d, PRIMARY KEY(a, c));
**
** The DELETE statement looks like this:
**
**     DELETE FROM x WHERE a = :1 AND c = :3 AND (:5 OR b IS :2 AND d IS :4)
**
** Variable :5 (nCol+1) is a boolean. It should be set to 0 if we require
** matching b and d values, or 1 otherwise. The second case comes up if the
** conflict handler is invoked with NOTFOUND and returns CHANGESET_REPLACE.
**
** If successful, SQLITE_OK is returned and SessionApplyCtx.pDelete is left
** pointing to the prepared version of the SQL statement.
*/
static int sessionDeleteRow(
  sqlite3 *db,                    /* Database handle */
  const char *zTab,               /* Table name */
  SessionApplyCtx *p              /* Session changeset-apply context */
){
  int i;
  const char *zSep = "";
  int rc = SQLITE_OK;
  SessionBuffer buf = {0, 0, 0};
  int nPk = 0;

  sessionAppendStr(&buf, "DELETE FROM main.", &rc);
  sessionAppendIdent(&buf, zTab, &rc);
  sessionAppendStr(&buf, " WHERE ", &rc);

  for(i=0; i<p->nCol; i++){
    if( p->abPK[i] ){
      nPk++;
      sessionAppendStr(&buf, zSep, &rc);
      sessionAppendIdent(&buf, p->azCol[i], &rc);
      sessionAppendStr(&buf, " = ?", &rc);
      sessionAppendInteger(&buf, i+1, &rc);
      zSep = " AND ";
    }
  }

  if( nPk<p->nCol ){
    sessionAppendStr(&buf, " AND (?", &rc);
    sessionAppendInteger(&buf, p->nCol+1, &rc);
    sessionAppendStr(&buf, " OR ", &rc);

    zSep = "";
    for(i=0; i<p->nCol; i++){
      if( !p->abPK[i] ){
        sessionAppendStr(&buf, zSep, &rc);
        sessionAppendIdent(&buf, p->azCol[i], &rc);
        sessionAppendStr(&buf, " IS ?", &rc);
        sessionAppendInteger(&buf, i+1, &rc);
        zSep = "AND ";
      }
    }
    sessionAppendStr(&buf, ")", &rc);
  }

  if( rc==SQLITE_OK ){
    rc = sqlite3_prepare_v2(db, (char *)buf.aBuf, buf.nBuf, &p->pDelete, 0);
  }
  sqlite3_free(buf.aBuf);

  return rc;
}

/*
** Formulate and prepare an SQL statement to query table zTab by primary
** key. Assuming the following table structure:
**
**     CREATE TABLE x(a, b, c, d, PRIMARY KEY(a, c));
**
** The SELECT statement looks like this:
**
**     SELECT * FROM x WHERE a = ?1 AND c = ?3
**
** If successful, SQLITE_OK is returned and SessionApplyCtx.pSelect is left
** pointing to the prepared version of the SQL statement.
*/
static int sessionSelectRow(
  sqlite3 *db,                    /* Database handle */
  const char *zTab,               /* Table name */
  SessionApplyCtx *p              /* Session changeset-apply context */
){
  /* TODO */
  return sessionSelectStmt(db, p->bIgnoreNoop,
      "main", zTab, p->bRowid, p->nCol, p->azCol, p->abPK, &p->pSelect
  );
}

/*
** Formulate and prepare an INSERT statement to add a record to table zTab.
** For example:
**
**     INSERT INTO main."zTab" VALUES(?1, ?2, ?3 ...);
**
** If successful, SQLITE_OK is returned and SessionApplyCtx.pInsert is left
** pointing to the prepared version of the SQL statement.
*/
static int sessionInsertRow(
  sqlite3 *db,                    /* Database handle */
  const char *zTab,               /* Table name */
  SessionApplyCtx *p              /* Session changeset-apply context */
){
  int rc = SQLITE_OK;
  int i;
  SessionBuffer buf = {0, 0, 0};

  sessionAppendStr(&buf, "INSERT INTO main.", &rc);
  sessionAppendIdent(&buf, zTab, &rc);
  sessionAppendStr(&buf, "(", &rc);
  for(i=0; i<p->nCol; i++){
    if( i!=0 ) sessionAppendStr(&buf, ", ", &rc);
    sessionAppendIdent(&buf, p->azCol[i], &rc);
  }

  sessionAppendStr(&buf, ") VALUES(?", &rc);
  for(i=1; i<p->nCol; i++){
    sessionAppendStr(&buf, ", ?", &rc);
  }
  sessionAppendStr(&buf, ")", &rc);

  if( rc==SQLITE_OK ){
    rc = sqlite3_prepare_v2(db, (char *)buf.aBuf, buf.nBuf, &p->pInsert, 0);
  }
  sqlite3_free(buf.aBuf);
  return rc;
}

static int sessionPrepare(sqlite3 *db, sqlite3_stmt **pp, const char *zSql){
  return sqlite3_prepare_v2(db, zSql, -1, pp, 0);
}

/*
** Prepare statements for applying changes to the sqlite_stat1 table.
** These are similar to those created by sessionSelectRow(),
** sessionInsertRow(), sessionUpdateRow() and sessionDeleteRow() for 
** other tables.
*/
static int sessionStat1Sql(sqlite3 *db, SessionApplyCtx *p){
  int rc = sessionSelectRow(db, "sqlite_stat1", p);
  if( rc==SQLITE_OK ){
    rc = sessionPrepare(db, &p->pInsert,
        "INSERT INTO main.sqlite_stat1 VALUES(?1, "
        "CASE WHEN length(?2)=0 AND typeof(?2)='blob' THEN NULL ELSE ?2 END, "
        "?3)"
    );
  }
  if( rc==SQLITE_OK ){
    rc = sessionPrepare(db, &p->pDelete,
        "DELETE FROM main.sqlite_stat1 WHERE tbl=?1 AND idx IS "
        "CASE WHEN length(?2)=0 AND typeof(?2)='blob' THEN NULL ELSE ?2 END "
        "AND (?4 OR stat IS ?3)"
    );
  }
  return rc;
}

/*
** A wrapper around sqlite3_bind_value() that detects an extra problem. 
** See comments in the body of this function for details.
*/
static int sessionBindValue(
  sqlite3_stmt *pStmt,            /* Statement to bind value to */
  int i,                          /* Parameter number to bind to */
  sqlite3_value *pVal             /* Value to bind */
){
  int eType = sqlite3_value_type(pVal);
  /* COVERAGE: The (pVal->z==0) branch is never true using current versions
  ** of SQLite. If a malloc fails in an sqlite3_value_xxx() function, either
  ** the (pVal->z) variable remains as it was or the type of the value is
  ** set to SQLITE_NULL.  */
  if( (eType==SQLITE_TEXT || eType==SQLITE_BLOB) && pVal->z==0 ){
    /* This condition occurs when an earlier OOM in a call to
    ** sqlite3_value_text() or sqlite3_value_blob() (perhaps from within
    ** a conflict-handler) has zeroed the pVal->z pointer. Return NOMEM. */
    return SQLITE_NOMEM;
  }
  return sqlite3_bind_value(pStmt, i, pVal);
}

/*
** Iterator pIter must point to an SQLITE_INSERT entry. This function 
** transfers new.* values from the current iterator entry to statement
** pStmt. The table being inserted into has nCol columns.
**
** New.* value $i from the iterator is bound to variable ($i+1) of 
** statement pStmt. If parameter abPK is NULL, all values from 0 to (nCol-1)
** are transfered to the statement. Otherwise, if abPK is not NULL, it points
** to an array nCol elements in size. In this case only those values for 
** which abPK[$i] is true are read from the iterator and bound to the 
** statement.
**
** An SQLite error code is returned if an error occurs. Otherwise, SQLITE_OK.
*/
static int sessionBindRow(
  sqlite3_changeset_iter *pIter,  /* Iterator to read values from */
  int(*xValue)(sqlite3_changeset_iter *, int, sqlite3_value **),
  int nCol,                       /* Number of columns */
  u8 *abPK,                       /* If not NULL, bind only if true */
  sqlite3_stmt *pStmt             /* Bind values to this statement */
){
  int i;
  int rc = SQLITE_OK;

  /* Neither sqlite3changeset_old or sqlite3changeset_new can fail if the
  ** argument iterator points to a suitable entry. Make sure that xValue 
  ** is one of these to guarantee that it is safe to ignore the return 
  ** in the code below. */
  assert( xValue==sqlite3changeset_old || xValue==sqlite3changeset_new );

  for(i=0; rc==SQLITE_OK && i<nCol; i++){
    if( !abPK || abPK[i] ){
      sqlite3_value *pVal = 0;
      (void)xValue(pIter, i, &pVal);
      if( pVal==0 ){
        /* The value in the changeset was "undefined". This indicates a
        ** corrupt changeset blob.  */
        rc = SQLITE_CORRUPT_BKPT;
      }else{
        rc = sessionBindValue(pStmt, i+1, pVal);
      }
    }
  }
  return rc;
}

/*
** SQL statement pSelect is as generated by the sessionSelectRow() function.
** This function binds the primary key values from the change that changeset
** iterator pIter points to to the SELECT and attempts to seek to the table
** entry. If a row is found, the SELECT statement left pointing at the row 
** and SQLITE_ROW is returned. Otherwise, if no row is found and no error
** has occured, the statement is reset and SQLITE_OK is returned. If an
** error occurs, the statement is reset and an SQLite error code is returned.
**
** If this function returns SQLITE_ROW, the caller must eventually reset() 
** statement pSelect. If any other value is returned, the statement does
** not require a reset().
**
** If the iterator currently points to an INSERT record, bind values from the
** new.* record to the SELECT statement. Or, if it points to a DELETE or
** UPDATE, bind values from the old.* record. 
*/
static int sessionSeekToRow(
  sqlite3_changeset_iter *pIter,  /* Changeset iterator */
  SessionApplyCtx *p
){
  sqlite3_stmt *pSelect = p->pSelect;
  int rc;                         /* Return code */
  int nCol;                       /* Number of columns in table */
  int op;                         /* Changset operation (SQLITE_UPDATE etc.) */
  const char *zDummy;             /* Unused */

  sqlite3_clear_bindings(pSelect);
  sqlite3changeset_op(pIter, &zDummy, &nCol, &op, 0);
  rc = sessionBindRow(pIter, 
      op==SQLITE_INSERT ? sqlite3changeset_new : sqlite3changeset_old,
      nCol, p->abPK, pSelect
  );

  if( op!=SQLITE_DELETE && p->bIgnoreNoop ){
    int ii;
    for(ii=0; rc==SQLITE_OK && ii<nCol; ii++){
      if( p->abPK[ii]==0 ){
        sqlite3_value *pVal = 0;
        sqlite3changeset_new(pIter, ii, &pVal);
        sqlite3_bind_int(pSelect, ii+1+nCol, (pVal==0));
        if( pVal ) rc = sessionBindValue(pSelect, ii+1, pVal);
      }
    }
  }

  if( rc==SQLITE_OK ){
    rc = sqlite3_step(pSelect);
    if( rc!=SQLITE_ROW ) rc = sqlite3_reset(pSelect);
  }

  return rc;
}

/*
** This function is called from within sqlite3changeset_apply_v2() when
** a conflict is encountered and resolved using conflict resolution
** mode eType (either SQLITE_CHANGESET_OMIT or SQLITE_CHANGESET_REPLACE)..
** It adds a conflict resolution record to the buffer in 
** SessionApplyCtx.rebase, which will eventually be returned to the caller
** of apply_v2() as the "rebase" buffer.
**
** Return SQLITE_OK if successful, or an SQLite error code otherwise.
*/
static int sessionRebaseAdd(
  SessionApplyCtx *p,             /* Apply context */
  int eType,                      /* Conflict resolution (OMIT or REPLACE) */
  sqlite3_changeset_iter *pIter   /* Iterator pointing at current change */
){
  int rc = SQLITE_OK;
  if( p->bRebase ){
    int i;
    int eOp = pIter->op;
    if( p->bRebaseStarted==0 ){
      /* Append a table-header to the rebase buffer */
      const char *zTab = pIter->zTab;
      sessionAppendByte(&p->rebase, 'T', &rc);
      sessionAppendVarint(&p->rebase, p->nCol, &rc);
      sessionAppendBlob(&p->rebase, p->abPK, p->nCol, &rc);
      sessionAppendBlob(&p->rebase, (u8*)zTab, (int)strlen(zTab)+1, &rc);
      p->bRebaseStarted = 1;
    }

    assert( eType==SQLITE_CHANGESET_REPLACE||eType==SQLITE_CHANGESET_OMIT );
    assert( eOp==SQLITE_DELETE || eOp==SQLITE_INSERT || eOp==SQLITE_UPDATE );

    sessionAppendByte(&p->rebase, 
        (eOp==SQLITE_DELETE ? SQLITE_DELETE : SQLITE_INSERT), &rc
        );
    sessionAppendByte(&p->rebase, (eType==SQLITE_CHANGESET_REPLACE), &rc);
    for(i=0; i<p->nCol; i++){
      sqlite3_value *pVal = 0;
      if( eOp==SQLITE_DELETE || (eOp==SQLITE_UPDATE && p->abPK[i]) ){
        sqlite3changeset_old(pIter, i, &pVal);
      }else{
        sqlite3changeset_new(pIter, i, &pVal);
      }
      sessionAppendValue(&p->rebase, pVal, &rc);
    }
  }
  return rc;
}

/*
** Invoke the conflict handler for the change that the changeset iterator
** currently points to.
**
** Argument eType must be either CHANGESET_DATA or CHANGESET_CONFLICT.
** If argument pbReplace is NULL, then the type of conflict handler invoked
** depends solely on eType, as follows:
**
**    eType value                 Value passed to xConflict
**    -------------------------------------------------
**    CHANGESET_DATA              CHANGESET_NOTFOUND
**    CHANGESET_CONFLICT          CHANGESET_CONSTRAINT
**
** Or, if pbReplace is not NULL, then an attempt is made to find an existing
** record with the same primary key as the record about to be deleted, updated
** or inserted. If such a record can be found, it is available to the conflict
** handler as the "conflicting" record. In this case the type of conflict
** handler invoked is as follows:
**
**    eType value         PK Record found?   Value passed to xConflict
**    ----------------------------------------------------------------
**    CHANGESET_DATA      Yes                CHANGESET_DATA
**    CHANGESET_DATA      No                 CHANGESET_NOTFOUND
**    CHANGESET_CONFLICT  Yes                CHANGESET_CONFLICT
**    CHANGESET_CONFLICT  No                 CHANGESET_CONSTRAINT
**
** If pbReplace is not NULL, and a record with a matching PK is found, and
** the conflict handler function returns SQLITE_CHANGESET_REPLACE, *pbReplace
** is set to non-zero before returning SQLITE_OK.
**
** If the conflict handler returns SQLITE_CHANGESET_ABORT, SQLITE_ABORT is
** returned. Or, if the conflict handler returns an invalid value, 
** SQLITE_MISUSE. If the conflict handler returns SQLITE_CHANGESET_OMIT,
** this function returns SQLITE_OK.
*/
static int sessionConflictHandler(
  int eType,                      /* Either CHANGESET_DATA or CONFLICT */
  SessionApplyCtx *p,             /* changeset_apply() context */
  sqlite3_changeset_iter *pIter,  /* Changeset iterator */
  int(*xConflict)(void *, int, sqlite3_changeset_iter*),
  void *pCtx,                     /* First argument for conflict handler */
  int *pbReplace                  /* OUT: Set to true if PK row is found */
){
  int res = 0;                    /* Value returned by conflict handler */
  int rc;
  int nCol;
  int op;
  const char *zDummy;

  sqlite3changeset_op(pIter, &zDummy, &nCol, &op, 0);

  assert( eType==SQLITE_CHANGESET_CONFLICT || eType==SQLITE_CHANGESET_DATA );
  assert( SQLITE_CHANGESET_CONFLICT+1==SQLITE_CHANGESET_CONSTRAINT );
  assert( SQLITE_CHANGESET_DATA+1==SQLITE_CHANGESET_NOTFOUND );

  /* Bind the new.* PRIMARY KEY values to the SELECT statement. */
  if( pbReplace ){
    rc = sessionSeekToRow(pIter, p);
  }else{
    rc = SQLITE_OK;
  }

  if( rc==SQLITE_ROW ){
    /* There exists another row with the new.* primary key. */
    if( p->bIgnoreNoop 
     && sqlite3_column_int(p->pSelect, sqlite3_column_count(p->pSelect)-1)
    ){
      res = SQLITE_CHANGESET_OMIT;
    }else{
      pIter->pConflict = p->pSelect;
      res = xConflict(pCtx, eType, pIter);
      pIter->pConflict = 0;
    }
    rc = sqlite3_reset(p->pSelect);
  }else if( rc==SQLITE_OK ){
    if( p->bDeferConstraints && eType==SQLITE_CHANGESET_CONFLICT ){
      /* Instead of invoking the conflict handler, append the change blob
      ** to the SessionApplyCtx.constraints buffer. */
      u8 *aBlob = &pIter->in.aData[pIter->in.iCurrent];
      int nBlob = pIter->in.iNext - pIter->in.iCurrent;
      sessionAppendBlob(&p->constraints, aBlob, nBlob, &rc);
      return SQLITE_OK;
    }else{
      /* No other row with the new.* primary key. */
      res = xConflict(pCtx, eType+1, pIter);
      if( res==SQLITE_CHANGESET_REPLACE ) rc = SQLITE_MISUSE;
    }
  }

  if( rc==SQLITE_OK ){
    switch( res ){
      case SQLITE_CHANGESET_REPLACE:
        assert( pbReplace );
        *pbReplace = 1;
        break;

      case SQLITE_CHANGESET_OMIT:
        break;

      case SQLITE_CHANGESET_ABORT:
        rc = SQLITE_ABORT;
        break;

      default:
        rc = SQLITE_MISUSE;
        break;
    }
    if( rc==SQLITE_OK ){
      rc = sessionRebaseAdd(p, res, pIter);
    }
  }

  return rc;
}

/*
** Attempt to apply the change that the iterator passed as the first argument
** currently points to to the database. If a conflict is encountered, invoke
** the conflict handler callback.
**
** If argument pbRetry is NULL, then ignore any CHANGESET_DATA conflict. If
** one is encountered, update or delete the row with the matching primary key
** instead. Or, if pbRetry is not NULL and a CHANGESET_DATA conflict occurs,
** invoke the conflict handler. If it returns CHANGESET_REPLACE, set *pbRetry
** to true before returning. In this case the caller will invoke this function
** again, this time with pbRetry set to NULL.
**
** If argument pbReplace is NULL and a CHANGESET_CONFLICT conflict is 
** encountered invoke the conflict handler with CHANGESET_CONSTRAINT instead.
** Or, if pbReplace is not NULL, invoke it with CHANGESET_CONFLICT. If such
** an invocation returns SQLITE_CHANGESET_REPLACE, set *pbReplace to true
** before retrying. In this case the caller attempts to remove the conflicting
** row before invoking this function again, this time with pbReplace set 
** to NULL.
**
** If any conflict handler returns SQLITE_CHANGESET_ABORT, this function
** returns SQLITE_ABORT. Otherwise, if no error occurs, SQLITE_OK is 
** returned.
*/
static int sessionApplyOneOp(
  sqlite3_changeset_iter *pIter,  /* Changeset iterator */
  SessionApplyCtx *p,             /* changeset_apply() context */
  int(*xConflict)(void *, int, sqlite3_changeset_iter *),
  void *pCtx,                     /* First argument for the conflict handler */
  int *pbReplace,                 /* OUT: True to remove PK row and retry */
  int *pbRetry                    /* OUT: True to retry. */
){
  const char *zDummy;
  int op;
  int nCol;
  int rc = SQLITE_OK;

  assert( p->pDelete && p->pInsert && p->pSelect );
  assert( p->azCol && p->abPK );
  assert( !pbReplace || *pbReplace==0 );

  sqlite3changeset_op(pIter, &zDummy, &nCol, &op, 0);

  if( op==SQLITE_DELETE ){

    /* Bind values to the DELETE statement. If conflict handling is required,
    ** bind values for all columns and set bound variable (nCol+1) to true.
    ** Or, if conflict handling is not required, bind just the PK column
    ** values and, if it exists, set (nCol+1) to false. Conflict handling
    ** is not required if:
    **
    **   * this is a patchset, or
    **   * (pbRetry==0), or
    **   * all columns of the table are PK columns (in this case there is
    **     no (nCol+1) variable to bind to).
    */
    u8 *abPK = (pIter->bPatchset ? p->abPK : 0);
    rc = sessionBindRow(pIter, sqlite3changeset_old, nCol, abPK, p->pDelete);
    if( rc==SQLITE_OK && sqlite3_bind_parameter_count(p->pDelete)>nCol ){
      rc = sqlite3_bind_int(p->pDelete, nCol+1, (pbRetry==0 || abPK));
    }
    if( rc!=SQLITE_OK ) return rc;

    sqlite3_step(p->pDelete);
    rc = sqlite3_reset(p->pDelete);
    if( rc==SQLITE_OK && sqlite3_changes(p->db)==0 && p->bIgnoreNoop==0 ){
      rc = sessionConflictHandler(
          SQLITE_CHANGESET_DATA, p, pIter, xConflict, pCtx, pbRetry
      );
    }else if( (rc&0xff)==SQLITE_CONSTRAINT ){
      rc = sessionConflictHandler(
          SQLITE_CHANGESET_CONFLICT, p, pIter, xConflict, pCtx, 0
      );
    }

  }else if( op==SQLITE_UPDATE ){
    int i;
    sqlite3_stmt *pUp = 0;
    int bPatchset = (pbRetry==0 || pIter->bPatchset);

    rc = sessionUpdateFind(pIter, p, bPatchset, &pUp);

    /* Bind values to the UPDATE statement. */
    for(i=0; rc==SQLITE_OK && i<nCol; i++){
      sqlite3_value *pOld = sessionChangesetOld(pIter, i);
      sqlite3_value *pNew = sessionChangesetNew(pIter, i);
      if( p->abPK[i] || (bPatchset==0 && pOld) ){
        rc = sessionBindValue(pUp, i*2+2, pOld);
      }
      if( rc==SQLITE_OK && pNew ){
        rc = sessionBindValue(pUp, i*2+1, pNew);
      }
    }
    if( rc!=SQLITE_OK ) return rc;

    /* Attempt the UPDATE. In the case of a NOTFOUND or DATA conflict,
    ** the result will be SQLITE_OK with 0 rows modified. */
    sqlite3_step(pUp);
    rc = sqlite3_reset(pUp);

    if( rc==SQLITE_OK && sqlite3_changes(p->db)==0 ){
      /* A NOTFOUND or DATA error. Search the table to see if it contains
      ** a row with a matching primary key. If so, this is a DATA conflict.
      ** Otherwise, if there is no primary key match, it is a NOTFOUND. */

      rc = sessionConflictHandler(
          SQLITE_CHANGESET_DATA, p, pIter, xConflict, pCtx, pbRetry
      );

    }else if( (rc&0xff)==SQLITE_CONSTRAINT ){
      /* This is always a CONSTRAINT conflict. */
      rc = sessionConflictHandler(
          SQLITE_CHANGESET_CONFLICT, p, pIter, xConflict, pCtx, 0
      );
    }

  }else{
    assert( op==SQLITE_INSERT );
    if( p->bStat1 ){
      /* Check if there is a conflicting row. For sqlite_stat1, this needs
      ** to be done using a SELECT, as there is no PRIMARY KEY in the 
      ** database schema to throw an exception if a duplicate is inserted.  */
      rc = sessionSeekToRow(pIter, p);
      if( rc==SQLITE_ROW ){
        rc = SQLITE_CONSTRAINT;
        sqlite3_reset(p->pSelect);
      }
    }

    if( rc==SQLITE_OK ){
      rc = sessionBindRow(pIter, sqlite3changeset_new, nCol, 0, p->pInsert);
      if( rc!=SQLITE_OK ) return rc;

      sqlite3_step(p->pInsert);
      rc = sqlite3_reset(p->pInsert);
    }

    if( (rc&0xff)==SQLITE_CONSTRAINT ){
      rc = sessionConflictHandler(
          SQLITE_CHANGESET_CONFLICT, p, pIter, xConflict, pCtx, pbReplace
      );
    }
  }

  return rc;
}

/*
** Attempt to apply the change that the iterator passed as the first argument
** currently points to to the database. If a conflict is encountered, invoke
** the conflict handler callback.
**
** The difference between this function and sessionApplyOne() is that this
** function handles the case where the conflict-handler is invoked and 
** returns SQLITE_CHANGESET_REPLACE - indicating that the change should be
** retried in some manner.
*/
static int sessionApplyOneWithRetry(
  sqlite3 *db,                    /* Apply change to "main" db of this handle */
  sqlite3_changeset_iter *pIter,  /* Changeset iterator to read change from */
  SessionApplyCtx *pApply,        /* Apply context */
  int(*xConflict)(void*, int, sqlite3_changeset_iter*),
  void *pCtx                      /* First argument passed to xConflict */
){
  int bReplace = 0;
  int bRetry = 0;
  int rc;

  rc = sessionApplyOneOp(pIter, pApply, xConflict, pCtx, &bReplace, &bRetry);
  if( rc==SQLITE_OK ){
    /* If the bRetry flag is set, the change has not been applied due to an
    ** SQLITE_CHANGESET_DATA problem (i.e. this is an UPDATE or DELETE and
    ** a row with the correct PK is present in the db, but one or more other
    ** fields do not contain the expected values) and the conflict handler 
    ** returned SQLITE_CHANGESET_REPLACE. In this case retry the operation,
    ** but pass NULL as the final argument so that sessionApplyOneOp() ignores
    ** the SQLITE_CHANGESET_DATA problem.  */
    if( bRetry ){
      assert( pIter->op==SQLITE_UPDATE || pIter->op==SQLITE_DELETE );
      rc = sessionApplyOneOp(pIter, pApply, xConflict, pCtx, 0, 0);
    }

    /* If the bReplace flag is set, the change is an INSERT that has not
    ** been performed because the database already contains a row with the
    ** specified primary key and the conflict handler returned
    ** SQLITE_CHANGESET_REPLACE. In this case remove the conflicting row
    ** before reattempting the INSERT.  */
    else if( bReplace ){
      assert( pIter->op==SQLITE_INSERT );
      rc = sqlite3_exec(db, "SAVEPOINT replace_op", 0, 0, 0);
      if( rc==SQLITE_OK ){
        rc = sessionBindRow(pIter, 
            sqlite3changeset_new, pApply->nCol, pApply->abPK, pApply->pDelete);
        sqlite3_bind_int(pApply->pDelete, pApply->nCol+1, 1);
      }
      if( rc==SQLITE_OK ){
        sqlite3_step(pApply->pDelete);
        rc = sqlite3_reset(pApply->pDelete);
      }
      if( rc==SQLITE_OK ){
        rc = sessionApplyOneOp(pIter, pApply, xConflict, pCtx, 0, 0);
      }
      if( rc==SQLITE_OK ){
        rc = sqlite3_exec(db, "RELEASE replace_op", 0, 0, 0);
      }
    }
  }

  return rc;
}

/*
** Retry the changes accumulated in the pApply->constraints buffer.
*/
static int sessionRetryConstraints(
  sqlite3 *db, 
  int bPatchset,
  const char *zTab,
  SessionApplyCtx *pApply,
  int(*xConflict)(void*, int, sqlite3_changeset_iter*),
  void *pCtx                      /* First argument passed to xConflict */
){
  int rc = SQLITE_OK;

  while( pApply->constraints.nBuf ){
    sqlite3_changeset_iter *pIter2 = 0;
    SessionBuffer cons = pApply->constraints;
    memset(&pApply->constraints, 0, sizeof(SessionBuffer));

    rc = sessionChangesetStart(
        &pIter2, 0, 0, cons.nBuf, cons.aBuf, pApply->bInvertConstraints, 1
    );
    if( rc==SQLITE_OK ){
      size_t nByte = 2*pApply->nCol*sizeof(sqlite3_value*);
      int rc2;
      pIter2->bPatchset = bPatchset;
      pIter2->zTab = (char*)zTab;
      pIter2->nCol = pApply->nCol;
      pIter2->abPK = pApply->abPK;
      sessionBufferGrow(&pIter2->tblhdr, nByte, &rc);
      pIter2->apValue = (sqlite3_value**)pIter2->tblhdr.aBuf;
      if( rc==SQLITE_OK ) memset(pIter2->apValue, 0, nByte);

      while( rc==SQLITE_OK && SQLITE_ROW==sqlite3changeset_next(pIter2) ){
        rc = sessionApplyOneWithRetry(db, pIter2, pApply, xConflict, pCtx);
      }

      rc2 = sqlite3changeset_finalize(pIter2);
      if( rc==SQLITE_OK ) rc = rc2;
    }
    assert( pApply->bDeferConstraints || pApply->constraints.nBuf==0 );

    sqlite3_free(cons.aBuf);
    if( rc!=SQLITE_OK ) break;
    if( pApply->constraints.nBuf>=cons.nBuf ){
      /* No progress was made on the last round. */
      pApply->bDeferConstraints = 0;
    }
  }

  return rc;
}

/*
** Argument pIter is a changeset iterator that has been initialized, but
** not yet passed to sqlite3changeset_next(). This function applies the 
** changeset to the main database attached to handle "db". The supplied
** conflict handler callback is invoked to resolve any conflicts encountered
** while applying the change.
*/
static int sessionChangesetApply(
  sqlite3 *db,                    /* Apply change to "main" db of this handle */
  sqlite3_changeset_iter *pIter,  /* Changeset to apply */
  int(*xFilter)(
    void *pCtx,                   /* Copy of sixth arg to _apply() */
    const char *zTab              /* Table name */
  ),
  int(*xFilterIter)(
    void *pCtx,                   /* Copy of sixth arg to _apply() */
    sqlite3_changeset_iter *p
  ),
  int(*xConflict)(
    void *pCtx,                   /* Copy of fifth arg to _apply() */
    int eConflict,                /* DATA, MISSING, CONFLICT, CONSTRAINT */
    sqlite3_changeset_iter *p     /* Handle describing change and conflict */
  ),
  void *pCtx,                     /* First argument passed to xConflict */
  void **ppRebase, int *pnRebase, /* OUT: Rebase information */
  int flags                       /* SESSION_APPLY_XXX flags */
){
  int schemaMismatch = 0;
  int rc = SQLITE_OK;             /* Return code */
  const char *zTab = 0;           /* Name of current table */
  int nTab = 0;                   /* Result of sqlite3Strlen30(zTab) */
  SessionApplyCtx sApply;         /* changeset_apply() context object */
  int bPatchset;
  u64 savedFlag = db->flags & SQLITE_FkNoAction;

  assert( xConflict!=0 );

  sqlite3_mutex_enter(sqlite3_db_mutex(db));
  if( flags & SQLITE_CHANGESETAPPLY_FKNOACTION ){
    db->flags |= ((u64)SQLITE_FkNoAction);
    db->aDb[0].pSchema->schema_cookie -= 32;
  }

  pIter->in.bNoDiscard = 1;
  memset(&sApply, 0, sizeof(sApply));
  sApply.bRebase = (ppRebase && pnRebase);
  sApply.bInvertConstraints = !!(flags & SQLITE_CHANGESETAPPLY_INVERT);
  sApply.bIgnoreNoop = !!(flags & SQLITE_CHANGESETAPPLY_IGNORENOOP);
  if( (flags & SQLITE_CHANGESETAPPLY_NOSAVEPOINT)==0 ){
    rc = sqlite3_exec(db, "SAVEPOINT changeset_apply", 0, 0, 0);
  }
  if( rc==SQLITE_OK ){
    rc = sqlite3_exec(db, "PRAGMA defer_foreign_keys = 1", 0, 0, 0);
  }
  while( rc==SQLITE_OK && SQLITE_ROW==sqlite3changeset_next(pIter) ){
    int nCol;
    int op;
    const char *zNew;
    
    sqlite3changeset_op(pIter, &zNew, &nCol, &op, 0);

    if( zTab==0 || sqlite3_strnicmp(zNew, zTab, nTab+1) ){
      u8 *abPK;

      rc = sessionRetryConstraints(
          db, pIter->bPatchset, zTab, &sApply, xConflict, pCtx
      );
      if( rc!=SQLITE_OK ) break;

      sessionUpdateFree(&sApply);
      sqlite3_free((char*)sApply.azCol);  /* cast works around VC++ bug */
      sqlite3_finalize(sApply.pDelete);
      sqlite3_finalize(sApply.pInsert);
      sqlite3_finalize(sApply.pSelect);
      sApply.db = db;
      sApply.pDelete = 0;
      sApply.pInsert = 0;
      sApply.pSelect = 0;
      sApply.nCol = 0;
      sApply.azCol = 0;
      sApply.abPK = 0;
      sApply.bStat1 = 0;
      sApply.bDeferConstraints = 1;
      sApply.bRebaseStarted = 0;
      sApply.bRowid = 0;
      memset(&sApply.constraints, 0, sizeof(SessionBuffer));

      /* If an xFilter() callback was specified, invoke it now. If the 
      ** xFilter callback returns zero, skip this table. If it returns
      ** non-zero, proceed. */
      schemaMismatch = (xFilter && (0==xFilter(pCtx, zNew)));
      if( schemaMismatch ){
        zTab = sqlite3_mprintf("%s", zNew);
        if( zTab==0 ){
          rc = SQLITE_NOMEM;
          break;
        }
        nTab = (int)strlen(zTab);
        sApply.azCol = (const char **)zTab;
      }else{
        int nMinCol = 0;
        int i;

        sqlite3changeset_pk(pIter, &abPK, 0);
        rc = sessionTableInfo(0, db, "main", zNew, 
            &sApply.nCol, 0, &zTab, &sApply.azCol, 0, 0, 
            &sApply.abPK, &sApply.bRowid
        );
        if( rc!=SQLITE_OK ) break;
        for(i=0; i<sApply.nCol; i++){
          if( sApply.abPK[i] ) nMinCol = i+1;
        }
  
        if( sApply.nCol==0 ){
          schemaMismatch = 1;
          sqlite3_log(SQLITE_SCHEMA, 
              "sqlite3changeset_apply(): no such table: %s", zTab
          );
        }
        else if( sApply.nCol<nCol ){
          schemaMismatch = 1;
          sqlite3_log(SQLITE_SCHEMA, 
              "sqlite3changeset_apply(): table %s has %d columns, "
              "expected %d or more", 
              zTab, sApply.nCol, nCol
          );
        }
        else if( nCol<nMinCol || memcmp(sApply.abPK, abPK, nCol)!=0 ){
          schemaMismatch = 1;
          sqlite3_log(SQLITE_SCHEMA, "sqlite3changeset_apply(): "
              "primary key mismatch for table %s", zTab
          );
        }
        else{
          sApply.nCol = nCol;
          if( 0==sqlite3_stricmp(zTab, "sqlite_stat1") ){
            if( (rc = sessionStat1Sql(db, &sApply) ) ){
              break;
            }
            sApply.bStat1 = 1;
          }else{
            if( (rc = sessionSelectRow(db, zTab, &sApply))
             || (rc = sessionDeleteRow(db, zTab, &sApply))
             || (rc = sessionInsertRow(db, zTab, &sApply))
            ){
              break;
            }
            sApply.bStat1 = 0;
          }
        }
        nTab = sqlite3Strlen30(zTab);
      }
    }

    /* If there is a schema mismatch on the current table, proceed to the
    ** next change. A log message has already been issued. */
    if( schemaMismatch ) continue;

    /* If this is a call to apply_v3(), invoke xFilterIter here. */
    if( xFilterIter && 0==xFilterIter(pCtx, pIter) ) continue;

    rc = sessionApplyOneWithRetry(db, pIter, &sApply, xConflict, pCtx);
  }

  bPatchset = pIter->bPatchset;
  if( rc==SQLITE_OK ){
    rc = sqlite3changeset_finalize(pIter);
  }else{
    sqlite3changeset_finalize(pIter);
  }

  if( rc==SQLITE_OK ){
    rc = sessionRetryConstraints(db, bPatchset, zTab, &sApply, xConflict, pCtx);
  }

  if( rc==SQLITE_OK ){
    int nFk, notUsed;
    sqlite3_db_status(db, SQLITE_DBSTATUS_DEFERRED_FKS, &nFk, &notUsed, 0);
    if( nFk!=0 ){
      int res = SQLITE_CHANGESET_ABORT;
      sqlite3_changeset_iter sIter;
      memset(&sIter, 0, sizeof(sIter));
      sIter.nCol = nFk;
      res = xConflict(pCtx, SQLITE_CHANGESET_FOREIGN_KEY, &sIter);
      if( res!=SQLITE_CHANGESET_OMIT ){
        rc = SQLITE_CONSTRAINT;
      }
    }
  }

  {
    int rc2 = sqlite3_exec(db, "PRAGMA defer_foreign_keys = 0", 0, 0, 0);
    if( rc==SQLITE_OK ) rc = rc2;
  }

  if( (flags & SQLITE_CHANGESETAPPLY_NOSAVEPOINT)==0 ){
    if( rc==SQLITE_OK ){
      rc = sqlite3_exec(db, "RELEASE changeset_apply", 0, 0, 0);
    }
    if( rc!=SQLITE_OK ){
      sqlite3_exec(db, "ROLLBACK TO changeset_apply", 0, 0, 0);
      sqlite3_exec(db, "RELEASE changeset_apply", 0, 0, 0);
    }
  }

  assert( sApply.bRebase || sApply.rebase.nBuf==0 );
  if( rc==SQLITE_OK && bPatchset==0 && sApply.bRebase ){
    *ppRebase = (void*)sApply.rebase.aBuf;
    *pnRebase = sApply.rebase.nBuf;
    sApply.rebase.aBuf = 0;
  }
  sessionUpdateFree(&sApply);
  sqlite3_finalize(sApply.pInsert);
  sqlite3_finalize(sApply.pDelete);
  sqlite3_finalize(sApply.pSelect);
  sqlite3_free((char*)sApply.azCol);  /* cast works around VC++ bug */
  sqlite3_free((char*)sApply.constraints.aBuf);
  sqlite3_free((char*)sApply.rebase.aBuf);

  if( (flags & SQLITE_CHANGESETAPPLY_FKNOACTION) && savedFlag==0 ){
    assert( db->flags & SQLITE_FkNoAction );
    db->flags &= ~((u64)SQLITE_FkNoAction);
    db->aDb[0].pSchema->schema_cookie -= 32;
  }
  sqlite3_mutex_leave(sqlite3_db_mutex(db));
  return rc;
}

/*
** This function is called by all six sqlite3changeset_apply() variants:
**
**   +  sqlite3changeset_apply()
**   +  sqlite3changeset_apply_v2()
**   +  sqlite3changeset_apply_v3()
**   +  sqlite3changeset_apply_strm()
**   +  sqlite3changeset_apply_strm_v2()
**   +  sqlite3changeset_apply_strm_v3()
**
** Arguments passed to this function are as follows:
**
** db:
**   Database handle to apply changeset to main database of.
**
** nChangeset/pChangeset:
**   These are both passed zero for the streaming variants. For the normal
**   apply() functions, these are passed the size of and the buffer containing
**   the changeset, respectively.
**
** xInput/pIn:
**   These are both passed zero for the normal variants. For the streaming
**   apply() functions, these are passed the input callback and context 
**   pointer, respectively.
**
** xFilter:
**   The filter function as passed to apply() or apply_v2() (to filter by
**   table name), if any. This is always NULL for apply_v3() calls. 
**
** xFilterIter:
**   The filter function as passed to apply_v3(), if any.
**
** xConflict:
**   The conflict handler callback (must not be NULL).
**
** pCtx:
**   The context pointer passed to the xFilter and xConflict handler callbacks.
**
** ppRebase, pnRebase:
**   Zero for apply(). The rebase changeset output pointers, if any, for
**   apply_v2() and apply_v3().
**
** flags:
**   Zero for apply(). The flags parameter for apply_v2() and apply_v3().
*/
static int sessionChangesetApplyV23(
  sqlite3 *db,                    /* Apply change to "main" db of this handle */
  int nChangeset,                 /* Size of changeset in bytes */
  void *pChangeset,               /* Changeset blob */
  int (*xInput)(void *pIn, void *pData, int *pnData), /* Input function */
  void *pIn,                                          /* First arg for xInput */
  int(*xFilter)(
    void *pCtx,                   /* Copy of sixth arg to _apply() */
    const char *zTab              /* Table name */
  ),
  int(*xFilterIter)(
    void *pCtx,                   /* Copy of sixth arg to _apply() */
    sqlite3_changeset_iter *p     /* Handle describing current change */
  ),
  int(*xConflict)(
    void *pCtx,                   /* Copy of sixth arg to _apply() */
    int eConflict,                /* DATA, MISSING, CONFLICT, CONSTRAINT */
    sqlite3_changeset_iter *p     /* Handle describing change and conflict */
  ),
  void *pCtx,                     /* First argument passed to xConflict */
  void **ppRebase, int *pnRebase,
  int flags
){
  sqlite3_changeset_iter *pIter;  /* Iterator to skip through changeset */  
  int bInverse = !!(flags & SQLITE_CHANGESETAPPLY_INVERT);
  int rc = sessionChangesetStart(
      &pIter, xInput, pIn, nChangeset, pChangeset, bInverse, 1
  );
  if( rc==SQLITE_OK ){
    rc = sessionChangesetApply(db, pIter, 
        xFilter, xFilterIter, xConflict, pCtx, ppRebase, pnRebase, flags
    );
  }
  return rc;
}

/*
** Apply the changeset passed via pChangeset/nChangeset to the main 
** database attached to handle "db".
*/
int sqlite3changeset_apply_v2(
  sqlite3 *db,                    /* Apply change to "main" db of this handle */
  int nChangeset,                 /* Size of changeset in bytes */
  void *pChangeset,               /* Changeset blob */
  int(*xFilter)(
    void *pCtx,                   /* Copy of sixth arg to _apply() */
    const char *zTab              /* Table name */
  ),
  int(*xConflict)(
    void *pCtx,                   /* Copy of sixth arg to _apply() */
    int eConflict,                /* DATA, MISSING, CONFLICT, CONSTRAINT */
    sqlite3_changeset_iter *p     /* Handle describing change and conflict */
  ),
  void *pCtx,                     /* First argument passed to xConflict */
  void **ppRebase, int *pnRebase,
  int flags
){
  return sessionChangesetApplyV23(db, 
      nChangeset, pChangeset, 0, 0, 
      xFilter, 0, xConflict, pCtx, 
      ppRebase, pnRebase, flags
  );
}

/*
** Apply the changeset passed via pChangeset/nChangeset to the main 
** database attached to handle "db".
*/
int sqlite3changeset_apply_v3(
  sqlite3 *db,                    /* Apply change to "main" db of this handle */
  int nChangeset,                 /* Size of changeset in bytes */
  void *pChangeset,               /* Changeset blob */
  int(*xFilter)(
    void *pCtx,                   /* Copy of sixth arg to _apply() */
    sqlite3_changeset_iter *p     /* Handle describing current change */
  ),
  int(*xConflict)(
    void *pCtx,                   /* Copy of sixth arg to _apply() */
    int eConflict,                /* DATA, MISSING, CONFLICT, CONSTRAINT */
    sqlite3_changeset_iter *p     /* Handle describing change and conflict */
  ),
  void *pCtx,                     /* First argument passed to xConflict */
  void **ppRebase, int *pnRebase,
  int flags
){
  return sessionChangesetApplyV23(db, 
      nChangeset, pChangeset, 0, 0,
      0, xFilter, xConflict, pCtx, 
      ppRebase, pnRebase, flags
  );
}

/*
** Apply the changeset passed via pChangeset/nChangeset to the main database
** attached to handle "db". Invoke the supplied conflict handler callback
** to resolve any conflicts encountered while applying the change.
*/
int sqlite3changeset_apply(
  sqlite3 *db,                    /* Apply change to "main" db of this handle */
  int nChangeset,                 /* Size of changeset in bytes */
  void *pChangeset,               /* Changeset blob */
  int(*xFilter)(
    void *pCtx,                   /* Copy of sixth arg to _apply() */
    const char *zTab              /* Table name */
  ),
  int(*xConflict)(
    void *pCtx,                   /* Copy of fifth arg to _apply() */
    int eConflict,                /* DATA, MISSING, CONFLICT, CONSTRAINT */
    sqlite3_changeset_iter *p     /* Handle describing change and conflict */
  ),
  void *pCtx                      /* First argument passed to xConflict */
){
  return sessionChangesetApplyV23(db, 
      nChangeset, pChangeset, 0, 0, 
      xFilter, 0, xConflict, pCtx, 
      0, 0, 0
  );
}

/*
** Apply the changeset passed via xInput/pIn to the main database
** attached to handle "db". Invoke the supplied conflict handler callback
** to resolve any conflicts encountered while applying the change.
*/
int sqlite3changeset_apply_v3_strm(
  sqlite3 *db,                    /* Apply change to "main" db of this handle */
  int (*xInput)(void *pIn, void *pData, int *pnData), /* Input function */
  void *pIn,                                          /* First arg for xInput */
  int(*xFilter)(
    void *pCtx,                   /* Copy of sixth arg to _apply() */
    sqlite3_changeset_iter *p
  ),
  int(*xConflict)(
    void *pCtx,                   /* Copy of sixth arg to _apply() */
    int eConflict,                /* DATA, MISSING, CONFLICT, CONSTRAINT */
    sqlite3_changeset_iter *p     /* Handle describing change and conflict */
  ),
  void *pCtx,                     /* First argument passed to xConflict */
  void **ppRebase, int *pnRebase,
  int flags
){
  return sessionChangesetApplyV23(db, 
      0, 0, xInput, pIn, 
      0, xFilter, xConflict, pCtx, 
      ppRebase, pnRebase, flags
  );
}
int sqlite3changeset_apply_v2_strm(
  sqlite3 *db,                    /* Apply change to "main" db of this handle */
  int (*xInput)(void *pIn, void *pData, int *pnData), /* Input function */
  void *pIn,                                          /* First arg for xInput */
  int(*xFilter)(
    void *pCtx,                   /* Copy of sixth arg to _apply() */
    const char *zTab              /* Table name */
  ),
  int(*xConflict)(
    void *pCtx,                   /* Copy of sixth arg to _apply() */
    int eConflict,                /* DATA, MISSING, CONFLICT, CONSTRAINT */
    sqlite3_changeset_iter *p     /* Handle describing change and conflict */
  ),
  void *pCtx,                     /* First argument passed to xConflict */
  void **ppRebase, int *pnRebase,
  int flags
){
  return sessionChangesetApplyV23(db, 
      0, 0, xInput, pIn, 
      xFilter, 0, xConflict, pCtx, 
      ppRebase, pnRebase, flags
  );
}
int sqlite3changeset_apply_strm(
  sqlite3 *db,                    /* Apply change to "main" db of this handle */
  int (*xInput)(void *pIn, void *pData, int *pnData), /* Input function */
  void *pIn,                                          /* First arg for xInput */
  int(*xFilter)(
    void *pCtx,                   /* Copy of sixth arg to _apply() */
    const char *zTab              /* Table name */
  ),
  int(*xConflict)(
    void *pCtx,                   /* Copy of sixth arg to _apply() */
    int eConflict,                /* DATA, MISSING, CONFLICT, CONSTRAINT */
    sqlite3_changeset_iter *p     /* Handle describing change and conflict */
  ),
  void *pCtx                      /* First argument passed to xConflict */
){
  return sessionChangesetApplyV23(db, 
      0, 0, xInput, pIn, 
      xFilter, 0, xConflict, pCtx, 
      0, 0, 0
  );
}

/*
** sqlite3_changegroup handle.
*/
struct sqlite3_changegroup {
  int rc;                         /* Error code */
  int bPatch;                     /* True to accumulate patchsets */
  SessionTable *pList;            /* List of tables in current patch */
  SessionBuffer rec;

  sqlite3 *db;                    /* Configured by changegroup_schema() */
  char *zDb;                      /* Configured by changegroup_schema() */
};

/*
** This function is called to merge two changes to the same row together as
** part of an sqlite3changeset_concat() operation. A new change object is
** allocated and a pointer to it stored in *ppNew.
*/
static int sessionChangeMerge(
  SessionTable *pTab,             /* Table structure */
  int bRebase,                    /* True for a rebase hash-table */
  int bPatchset,                  /* True for patchsets */
  SessionChange *pExist,          /* Existing change */
  int op2,                        /* Second change operation */
  int bIndirect,                  /* True if second change is indirect */
  u8 *aRec,                       /* Second change record */
  int nRec,                       /* Number of bytes in aRec */
  SessionChange **ppNew           /* OUT: Merged change */
){
  SessionChange *pNew = 0;
  int rc = SQLITE_OK;
  assert( aRec!=0 );

  if( !pExist ){
    pNew = (SessionChange *)sqlite3_malloc64(sizeof(SessionChange) + nRec);
    if( !pNew ){
      return SQLITE_NOMEM;
    }
    memset(pNew, 0, sizeof(SessionChange));
    pNew->op = op2;
    pNew->bIndirect = bIndirect;
    pNew->aRecord = (u8*)&pNew[1];
    if( bIndirect==0 || bRebase==0 ){
      pNew->nRecord = nRec;
      memcpy(pNew->aRecord, aRec, nRec);
    }else{
      int i;
      u8 *pIn = aRec;
      u8 *pOut = pNew->aRecord;
      for(i=0; i<pTab->nCol; i++){
        int nIn = sessionSerialLen(pIn);
        if( *pIn==0 ){
          *pOut++ = 0;
        }else if( pTab->abPK[i]==0 ){
          *pOut++ = 0xFF;
        }else{
          memcpy(pOut, pIn, nIn);
          pOut += nIn;
        }
        pIn += nIn;
      }
      pNew->nRecord = pOut - pNew->aRecord;
    }
  }else if( bRebase ){
    if( pExist->op==SQLITE_DELETE && pExist->bIndirect ){
      *ppNew = pExist;
    }else{
      sqlite3_int64 nByte = nRec + pExist->nRecord + sizeof(SessionChange);
      pNew = (SessionChange*)sqlite3_malloc64(nByte);
      if( pNew==0 ){
        rc = SQLITE_NOMEM;
      }else{
        int i;
        u8 *a1 = pExist->aRecord;
        u8 *a2 = aRec;
        u8 *pOut;

        memset(pNew, 0, nByte);
        pNew->bIndirect = bIndirect || pExist->bIndirect;
        pNew->op = op2;
        pOut = pNew->aRecord = (u8*)&pNew[1];

        for(i=0; i<pTab->nCol; i++){
          int n1 = sessionSerialLen(a1);
          int n2 = sessionSerialLen(a2);
          if( *a1==0xFF || (pTab->abPK[i]==0 && bIndirect) ){
            *pOut++ = 0xFF;
          }else if( *a2==0 ){
            memcpy(pOut, a1, n1);
            pOut += n1;
          }else{
            memcpy(pOut, a2, n2);
            pOut += n2;
          }
          a1 += n1;
          a2 += n2;
        }
        pNew->nRecord = pOut - pNew->aRecord;
      }
      sqlite3_free(pExist);
    }
  }else{
    int op1 = pExist->op;

    /* 
    **   op1=INSERT, op2=INSERT      ->      Unsupported. Discard op2.
    **   op1=INSERT, op2=UPDATE      ->      INSERT.
    **   op1=INSERT, op2=DELETE      ->      (none)
    **
    **   op1=UPDATE, op2=INSERT      ->      Unsupported. Discard op2.
    **   op1=UPDATE, op2=UPDATE      ->      UPDATE.
    **   op1=UPDATE, op2=DELETE      ->      DELETE.
    **
    **   op1=DELETE, op2=INSERT      ->      UPDATE.
    **   op1=DELETE, op2=UPDATE      ->      Unsupported. Discard op2.
    **   op1=DELETE, op2=DELETE      ->      Unsupported. Discard op2.
    */   
    if( (op1==SQLITE_INSERT && op2==SQLITE_INSERT)
     || (op1==SQLITE_UPDATE && op2==SQLITE_INSERT)
     || (op1==SQLITE_DELETE && op2==SQLITE_UPDATE)
     || (op1==SQLITE_DELETE && op2==SQLITE_DELETE)
    ){
      pNew = pExist;
    }else if( op1==SQLITE_INSERT && op2==SQLITE_DELETE ){
      sqlite3_free(pExist);
      assert( pNew==0 );
    }else{
      u8 *aExist = pExist->aRecord;
      sqlite3_int64 nByte;
      u8 *aCsr;

      /* Allocate a new SessionChange object. Ensure that the aRecord[]
      ** buffer of the new object is large enough to hold any record that
      ** may be generated by combining the input records.  */
      nByte = sizeof(SessionChange) + pExist->nRecord + nRec;
      pNew = (SessionChange *)sqlite3_malloc64(nByte);
      if( !pNew ){
        sqlite3_free(pExist);
        return SQLITE_NOMEM;
      }
      memset(pNew, 0, sizeof(SessionChange));
      pNew->bIndirect = (bIndirect && pExist->bIndirect);
      aCsr = pNew->aRecord = (u8 *)&pNew[1];

      if( op1==SQLITE_INSERT ){             /* INSERT + UPDATE */
        u8 *a1 = aRec;
        assert( op2==SQLITE_UPDATE );
        pNew->op = SQLITE_INSERT;
        if( bPatchset==0 ) sessionSkipRecord(&a1, pTab->nCol);
        sessionMergeRecord(&aCsr, pTab->nCol, aExist, a1);
      }else if( op1==SQLITE_DELETE ){       /* DELETE + INSERT */
        assert( op2==SQLITE_INSERT );
        pNew->op = SQLITE_UPDATE;
        if( bPatchset ){
          memcpy(aCsr, aRec, nRec);
          aCsr += nRec;
        }else{
          if( 0==sessionMergeUpdate(&aCsr, pTab, bPatchset, aExist, 0,aRec,0) ){
            sqlite3_free(pNew);
            pNew = 0;
          }
        }
      }else if( op2==SQLITE_UPDATE ){       /* UPDATE + UPDATE */
        u8 *a1 = aExist;
        u8 *a2 = aRec;
        assert( op1==SQLITE_UPDATE );
        if( bPatchset==0 ){
          sessionSkipRecord(&a1, pTab->nCol);
          sessionSkipRecord(&a2, pTab->nCol);
        }
        pNew->op = SQLITE_UPDATE;
        if( 0==sessionMergeUpdate(&aCsr, pTab, bPatchset, aRec, aExist,a1,a2) ){
          sqlite3_free(pNew);
          pNew = 0;
        }
      }else{                                /* UPDATE + DELETE */
        assert( op1==SQLITE_UPDATE && op2==SQLITE_DELETE );
        pNew->op = SQLITE_DELETE;
        if( bPatchset ){
          memcpy(aCsr, aRec, nRec);
          aCsr += nRec;
        }else{
          sessionMergeRecord(&aCsr, pTab->nCol, aRec, aExist);
        }
      }

      if( pNew ){
        pNew->nRecord = (int)(aCsr - pNew->aRecord);
      }
      sqlite3_free(pExist);
    }
  }

  *ppNew = pNew;
  return rc;
}

/*
** Check if a changeset entry with nCol columns and the PK array passed
** as the final argument to this function is compatible with SessionTable
** pTab. If so, return 1. Otherwise, if they are incompatible in some way,
** return 0.
*/
static int sessionChangesetCheckCompat(
  SessionTable *pTab,
  int nCol,
  u8 *abPK
){
  if( pTab->azCol && nCol<pTab->nCol ){
    int ii;
    for(ii=0; ii<pTab->nCol; ii++){
      u8 bPK = (ii < nCol) ? abPK[ii] : 0;
      if( pTab->abPK[ii]!=bPK ) return 0;
    }
    return 1;
  }
  return (pTab->nCol==nCol && 0==memcmp(abPK, pTab->abPK, nCol));
}

static int sessionChangesetExtendRecord(
  sqlite3_changegroup *pGrp,
  SessionTable *pTab, 
  int nCol, 
  int op, 
  const u8 *aRec, 
  int nRec, 
  SessionBuffer *pOut
){
  int rc = SQLITE_OK;
  int ii = 0;

  assert( pTab->azCol );
  assert( nCol<pTab->nCol );

  pOut->nBuf = 0;
  if( op==SQLITE_INSERT || (op==SQLITE_DELETE && pGrp->bPatch==0) ){
    /* Append the missing default column values to the record. */
    sessionAppendBlob(pOut, aRec, nRec, &rc);
    if( rc==SQLITE_OK && pTab->pDfltStmt==0 ){
      rc = sessionPrepareDfltStmt(pGrp->db, pTab, &pTab->pDfltStmt);
      if( rc==SQLITE_OK && SQLITE_ROW!=sqlite3_step(pTab->pDfltStmt) ){
        rc = sqlite3_errcode(pGrp->db);
      }
    }
    for(ii=nCol; rc==SQLITE_OK && ii<pTab->nCol; ii++){
      int eType = sqlite3_column_type(pTab->pDfltStmt, ii);
      sessionAppendByte(pOut, eType, &rc);
      switch( eType ){
        case SQLITE_FLOAT:
        case SQLITE_INTEGER: {
          i64 iVal;
          if( eType==SQLITE_INTEGER ){
            iVal = sqlite3_column_int64(pTab->pDfltStmt, ii);
          }else{
            double rVal = sqlite3_column_int64(pTab->pDfltStmt, ii);
            memcpy(&iVal, &rVal, sizeof(i64));
          }
          if( SQLITE_OK==sessionBufferGrow(pOut, 8, &rc) ){
            sessionPutI64(&pOut->aBuf[pOut->nBuf], iVal);
            pOut->nBuf += 8;
          }
          break;
        }

        case SQLITE_BLOB:
        case SQLITE_TEXT: {
          int n = sqlite3_column_bytes(pTab->pDfltStmt, ii);
          sessionAppendVarint(pOut, n, &rc);
          if( eType==SQLITE_TEXT ){
            const u8 *z = (const u8*)sqlite3_column_text(pTab->pDfltStmt, ii);
            sessionAppendBlob(pOut, z, n, &rc);
          }else{
            const u8 *z = (const u8*)sqlite3_column_blob(pTab->pDfltStmt, ii);
            sessionAppendBlob(pOut, z, n, &rc);
          }
          break;
        }

        default:
          assert( eType==SQLITE_NULL );
          break;
      }
    }
  }else if( op==SQLITE_UPDATE ){
    /* Append missing "undefined" entries to the old.* record. And, if this
    ** is an UPDATE, to the new.* record as well.  */
    int iOff = 0;
    if( pGrp->bPatch==0 ){
      for(ii=0; ii<nCol; ii++){
        iOff += sessionSerialLen(&aRec[iOff]);
      }
      sessionAppendBlob(pOut, aRec, iOff, &rc);
      for(ii=0; ii<(pTab->nCol-nCol); ii++){
        sessionAppendByte(pOut, 0x00, &rc);
      }
    }

    sessionAppendBlob(pOut, &aRec[iOff], nRec-iOff, &rc);
    for(ii=0; ii<(pTab->nCol-nCol); ii++){
      sessionAppendByte(pOut, 0x00, &rc);
    }
  }else{
    assert( op==SQLITE_DELETE && pGrp->bPatch );
    sessionAppendBlob(pOut, aRec, nRec, &rc);
  }

  return rc;
}

/*
** Locate or create a SessionTable object that may be used to add the
** change currently pointed to by iterator pIter to changegroup pGrp.
** If successful, set output variable (*ppTab) to point to the table
** object and return SQLITE_OK. Otherwise, if some error occurs, return
** an SQLite error code and leave (*ppTab) set to NULL.
*/
static int sessionChangesetFindTable(
  sqlite3_changegroup *pGrp, 
  const char *zTab, 
  sqlite3_changeset_iter *pIter, 
  SessionTable **ppTab
){
  int rc = SQLITE_OK;
  SessionTable *pTab = 0;
  int nTab = (int)strlen(zTab);
  u8 *abPK = 0;
  int nCol = 0;

  *ppTab = 0;
  sqlite3changeset_pk(pIter, &abPK, &nCol);

  /* Search the list for an existing table */
  for(pTab = pGrp->pList; pTab; pTab=pTab->pNext){
    if( 0==sqlite3_strnicmp(pTab->zName, zTab, nTab+1) ) break;
  }

  /* If one was not found above, create a new table now */
  if( !pTab ){
    SessionTable **ppNew;

    pTab = sqlite3_malloc64(sizeof(SessionTable) + nCol + nTab+1);
    if( !pTab ){
      return SQLITE_NOMEM;
    }
    memset(pTab, 0, sizeof(SessionTable));
    pTab->nCol = nCol;
    pTab->abPK = (u8*)&pTab[1];
    memcpy(pTab->abPK, abPK, nCol);
    pTab->zName = (char*)&pTab->abPK[nCol];
    memcpy(pTab->zName, zTab, nTab+1);

    if( pGrp->db ){
      pTab->nCol = 0;
      rc = sessionInitTable(0, pTab, pGrp->db, pGrp->zDb);
      if( rc ){
        assert( pTab->azCol==0 );
        sqlite3_free(pTab);
        return rc;
      }
    }

    /* The new object must be linked on to the end of the list, not
    ** simply added to the start of it. This is to ensure that the
    ** tables within the output of sqlite3changegroup_output() are in 
    ** the right order.  */
    for(ppNew=&pGrp->pList; *ppNew; ppNew=&(*ppNew)->pNext);
    *ppNew = pTab;
  }

  /* Check that the table is compatible. */
  if( !sessionChangesetCheckCompat(pTab, nCol, abPK) ){
    rc = SQLITE_SCHEMA;
  }

  *ppTab = pTab;
  return rc;
}

/*
** Add the change currently indicated by iterator pIter to the hash table
** belonging to changegroup pGrp.
*/
static int sessionOneChangeToHash(
  sqlite3_changegroup *pGrp,
  sqlite3_changeset_iter *pIter,
  int bRebase
){
  int rc = SQLITE_OK;
  int nCol = 0;
  int op = 0;
  int iHash = 0;
  int bIndirect = 0;
  SessionChange *pChange = 0;
  SessionChange *pExist = 0;
  SessionChange **pp = 0;
  SessionTable *pTab = 0;
  u8 *aRec = &pIter->in.aData[pIter->in.iCurrent + 2];
  int nRec = (pIter->in.iNext - pIter->in.iCurrent) - 2;

  assert( nRec>0 );

  /* Ensure that only changesets, or only patchsets, but not a mixture
  ** of both, are being combined. It is an error to try to combine a
  ** changeset and a patchset.  */
  if( pGrp->pList==0 ){
    pGrp->bPatch = pIter->bPatchset;
  }else if( pIter->bPatchset!=pGrp->bPatch ){
    rc = SQLITE_ERROR;
  }

  if( rc==SQLITE_OK ){
    const char *zTab = 0;
    sqlite3changeset_op(pIter, &zTab, &nCol, &op, &bIndirect);
    rc = sessionChangesetFindTable(pGrp, zTab, pIter, &pTab);
  }

  if( rc==SQLITE_OK && nCol<pTab->nCol ){
    SessionBuffer *pBuf = &pGrp->rec;
    rc = sessionChangesetExtendRecord(pGrp, pTab, nCol, op, aRec, nRec, pBuf);
    aRec = pBuf->aBuf;
    nRec = pBuf->nBuf;
    assert( pGrp->db );
  }

  if( rc==SQLITE_OK && sessionGrowHash(0, pIter->bPatchset, pTab) ){
    rc = SQLITE_NOMEM;
  }

  if( rc==SQLITE_OK ){
    /* Search for existing entry. If found, remove it from the hash table. 
    ** Code below may link it back in.  */
    iHash = sessionChangeHash(
        pTab, (pIter->bPatchset && op==SQLITE_DELETE), aRec, pTab->nChange
    );
    for(pp=&pTab->apChange[iHash]; *pp; pp=&(*pp)->pNext){
      int bPkOnly1 = 0;
      int bPkOnly2 = 0;
      if( pIter->bPatchset ){
        bPkOnly1 = (*pp)->op==SQLITE_DELETE;
        bPkOnly2 = op==SQLITE_DELETE;
      }
      if( sessionChangeEqual(pTab, bPkOnly1, (*pp)->aRecord, bPkOnly2, aRec) ){
        pExist = *pp;
        *pp = (*pp)->pNext;
        pTab->nEntry--;
        break;
      }
    }
  }

  if( rc==SQLITE_OK ){
    rc = sessionChangeMerge(pTab, bRebase, 
        pIter->bPatchset, pExist, op, bIndirect, aRec, nRec, &pChange
    );
  }
  if( rc==SQLITE_OK && pChange ){
    pChange->pNext = pTab->apChange[iHash];
    pTab->apChange[iHash] = pChange;
    pTab->nEntry++;
  }

  if( rc==SQLITE_OK ) rc = pIter->rc;
  return rc;
}

/*
** Add all changes in the changeset traversed by the iterator passed as
** the first argument to the changegroup hash tables.
*/
static int sessionChangesetToHash(
  sqlite3_changeset_iter *pIter,   /* Iterator to read from */
  sqlite3_changegroup *pGrp,       /* Changegroup object to add changeset to */
  int bRebase                      /* True if hash table is for rebasing */
){
  u8 *aRec;
  int nRec;
  int rc = SQLITE_OK;

  pIter->in.bNoDiscard = 1;
  while( SQLITE_ROW==(sessionChangesetNext(pIter, &aRec, &nRec, 0)) ){
    rc = sessionOneChangeToHash(pGrp, pIter, bRebase);
    if( rc!=SQLITE_OK ) break;
  }

  if( rc==SQLITE_OK ) rc = pIter->rc;
  return rc;
}

/*
** Serialize a changeset (or patchset) based on all changesets (or patchsets)
** added to the changegroup object passed as the first argument.
**
** If xOutput is not NULL, then the changeset/patchset is returned to the
** user via one or more calls to xOutput, as with the other streaming
** interfaces. 
**
** Or, if xOutput is NULL, then (*ppOut) is populated with a pointer to a
** buffer containing the output changeset before this function returns. In
** this case (*pnOut) is set to the size of the output buffer in bytes. It
** is the responsibility of the caller to free the output buffer using
** sqlite3_free() when it is no longer required.
**
** If successful, SQLITE_OK is returned. Or, if an error occurs, an SQLite
** error code. If an error occurs and xOutput is NULL, (*ppOut) and (*pnOut)
** are both set to 0 before returning.
*/
static int sessionChangegroupOutput(
  sqlite3_changegroup *pGrp,
  int (*xOutput)(void *pOut, const void *pData, int nData),
  void *pOut,
  int *pnOut,
  void **ppOut
){
  int rc = SQLITE_OK;
  SessionBuffer buf = {0, 0, 0};
  SessionTable *pTab;
  assert( xOutput==0 || (ppOut==0 && pnOut==0) );

  /* Create the serialized output changeset based on the contents of the
  ** hash tables attached to the SessionTable objects in list p->pList. 
  */
  for(pTab=pGrp->pList; rc==SQLITE_OK && pTab; pTab=pTab->pNext){
    int i;
    if( pTab->nEntry==0 ) continue;

    sessionAppendTableHdr(&buf, pGrp->bPatch, pTab, &rc);
    for(i=0; i<pTab->nChange; i++){
      SessionChange *p;
      for(p=pTab->apChange[i]; p; p=p->pNext){
        sessionAppendByte(&buf, p->op, &rc);
        sessionAppendByte(&buf, p->bIndirect, &rc);
        sessionAppendBlob(&buf, p->aRecord, p->nRecord, &rc);
        if( rc==SQLITE_OK && xOutput && buf.nBuf>=sessions_strm_chunk_size ){
          rc = xOutput(pOut, buf.aBuf, buf.nBuf);
          buf.nBuf = 0;
        }
      }
    }
  }

  if( rc==SQLITE_OK ){
    if( xOutput ){
      if( buf.nBuf>0 ) rc = xOutput(pOut, buf.aBuf, buf.nBuf);
    }else if( ppOut ){
      *ppOut = buf.aBuf;
      if( pnOut ) *pnOut = buf.nBuf;
      buf.aBuf = 0;
    }
  }
  sqlite3_free(buf.aBuf);

  return rc;
}

/*
** Allocate a new, empty, sqlite3_changegroup.
*/
int sqlite3changegroup_new(sqlite3_changegroup **pp){
  int rc = SQLITE_OK;             /* Return code */
  sqlite3_changegroup *p;         /* New object */
  p = (sqlite3_changegroup*)sqlite3_malloc(sizeof(sqlite3_changegroup));
  if( p==0 ){
    rc = SQLITE_NOMEM;
  }else{
    memset(p, 0, sizeof(sqlite3_changegroup));
  }
  *pp = p;
  return rc;
}

/*
** Provide a database schema to the changegroup object.
*/
int sqlite3changegroup_schema(
  sqlite3_changegroup *pGrp, 
  sqlite3 *db, 
  const char *zDb
){
  int rc = SQLITE_OK;

  if( pGrp->pList || pGrp->db ){
    /* Cannot add a schema after one or more calls to sqlite3changegroup_add(),
    ** or after sqlite3changegroup_schema() has already been called. */
    rc = SQLITE_MISUSE;
  }else{
    pGrp->zDb = sqlite3_mprintf("%s", zDb);
    if( pGrp->zDb==0 ){
      rc = SQLITE_NOMEM;
    }else{
      pGrp->db = db;
    }
  }
  return rc;
}

/*
** Add the changeset currently stored in buffer pData, size nData bytes,
** to changeset-group p.
*/
int sqlite3changegroup_add(sqlite3_changegroup *pGrp, int nData, void *pData){
  sqlite3_changeset_iter *pIter;  /* Iterator opened on pData/nData */
  int rc;                         /* Return code */

  rc = sqlite3changeset_start(&pIter, nData, pData);
  if( rc==SQLITE_OK ){
    rc = sessionChangesetToHash(pIter, pGrp, 0);
  }
  sqlite3changeset_finalize(pIter);
  return rc;
}

/*
** Add a single change to a changeset-group.
*/
int sqlite3changegroup_add_change(
  sqlite3_changegroup *pGrp,
  sqlite3_changeset_iter *pIter
){
  int rc = SQLITE_OK;

  if( pIter->in.iCurrent==pIter->in.iNext 
   || pIter->rc!=SQLITE_OK 
   || pIter->bInvert
  ){
    /* Iterator does not point to any valid entry or is an INVERT iterator. */
    rc = SQLITE_ERROR;
  }else{
    pIter->in.bNoDiscard = 1;
    rc = sessionOneChangeToHash(pGrp, pIter, 0);
  }
  return rc;
}

/*
** Obtain a buffer containing a changeset representing the concatenation
** of all changesets added to the group so far.
*/
int sqlite3changegroup_output(
    sqlite3_changegroup *pGrp,
    int *pnData,
    void **ppData
){
  return sessionChangegroupOutput(pGrp, 0, 0, pnData, ppData);
}

/*
** Streaming versions of changegroup_add().
*/
int sqlite3changegroup_add_strm(
  sqlite3_changegroup *pGrp,
  int (*xInput)(void *pIn, void *pData, int *pnData),
  void *pIn
){
  sqlite3_changeset_iter *pIter;  /* Iterator opened on pData/nData */
  int rc;                         /* Return code */

  rc = sqlite3changeset_start_strm(&pIter, xInput, pIn);
  if( rc==SQLITE_OK ){
    rc = sessionChangesetToHash(pIter, pGrp, 0);
  }
  sqlite3changeset_finalize(pIter);
  return rc;
}

/*
** Streaming versions of changegroup_output().
*/
int sqlite3changegroup_output_strm(
  sqlite3_changegroup *pGrp,
  int (*xOutput)(void *pOut, const void *pData, int nData), 
  void *pOut
){
  return sessionChangegroupOutput(pGrp, xOutput, pOut, 0, 0);
}

/*
** Delete a changegroup object.
*/
void sqlite3changegroup_delete(sqlite3_changegroup *pGrp){
  if( pGrp ){
    sqlite3_free(pGrp->zDb);
    sessionDeleteTable(0, pGrp->pList);
    sqlite3_free(pGrp->rec.aBuf);
    sqlite3_free(pGrp);
  }
}

/* 
** Combine two changesets together.
*/
int sqlite3changeset_concat(
  int nLeft,                      /* Number of bytes in lhs input */
  void *pLeft,                    /* Lhs input changeset */
  int nRight                      /* Number of bytes in rhs input */,
  void *pRight,                   /* Rhs input changeset */
  int *pnOut,                     /* OUT: Number of bytes in output changeset */
  void **ppOut                    /* OUT: changeset (left <concat> right) */
){
  sqlite3_changegroup *pGrp;
  int rc;

  rc = sqlite3changegroup_new(&pGrp);
  if( rc==SQLITE_OK ){
    rc = sqlite3changegroup_add(pGrp, nLeft, pLeft);
  }
  if( rc==SQLITE_OK ){
    rc = sqlite3changegroup_add(pGrp, nRight, pRight);
  }
  if( rc==SQLITE_OK ){
    rc = sqlite3changegroup_output(pGrp, pnOut, ppOut);
  }
  sqlite3changegroup_delete(pGrp);

  return rc;
}

/*
** Streaming version of sqlite3changeset_concat().
*/
int sqlite3changeset_concat_strm(
  int (*xInputA)(void *pIn, void *pData, int *pnData),
  void *pInA,
  int (*xInputB)(void *pIn, void *pData, int *pnData),
  void *pInB,
  int (*xOutput)(void *pOut, const void *pData, int nData),
  void *pOut
){
  sqlite3_changegroup *pGrp;
  int rc;

  rc = sqlite3changegroup_new(&pGrp);
  if( rc==SQLITE_OK ){
    rc = sqlite3changegroup_add_strm(pGrp, xInputA, pInA);
  }
  if( rc==SQLITE_OK ){
    rc = sqlite3changegroup_add_strm(pGrp, xInputB, pInB);
  }
  if( rc==SQLITE_OK ){
    rc = sqlite3changegroup_output_strm(pGrp, xOutput, pOut);
  }
  sqlite3changegroup_delete(pGrp);

  return rc;
}

/*
** Changeset rebaser handle.
*/
struct sqlite3_rebaser {
  sqlite3_changegroup grp;        /* Hash table */
};

/*
** Buffers a1 and a2 must both contain a sessions module record nCol
** fields in size. This function appends an nCol sessions module 
** record to buffer pBuf that is a copy of a1, except that for
** each field that is undefined in a1[], swap in the field from a2[].
*/
static void sessionAppendRecordMerge(
  SessionBuffer *pBuf,            /* Buffer to append to */
  int nCol,                       /* Number of columns in each record */
  u8 *a1, int n1,                 /* Record 1 */
  u8 *a2, int n2,                 /* Record 2 */
  int *pRc                        /* IN/OUT: error code */
){
  sessionBufferGrow(pBuf, n1+n2, pRc);
  if( *pRc==SQLITE_OK ){
    int i;
    u8 *pOut = &pBuf->aBuf[pBuf->nBuf];
    for(i=0; i<nCol; i++){
      int nn1 = sessionSerialLen(a1);
      int nn2 = sessionSerialLen(a2);
      if( *a1==0 || *a1==0xFF ){
        memcpy(pOut, a2, nn2);
        pOut += nn2;
      }else{
        memcpy(pOut, a1, nn1);
        pOut += nn1;
      }
      a1 += nn1;
      a2 += nn2;
    }

    pBuf->nBuf = pOut-pBuf->aBuf;
    assert( pBuf->nBuf<=pBuf->nAlloc );
  }
}

/*
** This function is called when rebasing a local UPDATE change against one 
** or more remote UPDATE changes. The aRec/nRec buffer contains the current
** old.* and new.* records for the change. The rebase buffer (a single
** record) is in aChange/nChange. The rebased change is appended to buffer
** pBuf.
**
** Rebasing the UPDATE involves: 
**
**   * Removing any changes to fields for which the corresponding field
**     in the rebase buffer is set to "replaced" (type 0xFF). If this
**     means the UPDATE change updates no fields, nothing is appended
**     to the output buffer.
**
**   * For each field modified by the local change for which the 
**     corresponding field in the rebase buffer is not "undefined" (0x00)
**     or "replaced" (0xFF), the old.* value is replaced by the value
**     in the rebase buffer.
*/
static void sessionAppendPartialUpdate(
  SessionBuffer *pBuf,            /* Append record here */
  sqlite3_changeset_iter *pIter,  /* Iterator pointed at local change */
  u8 *aRec, int nRec,             /* Local change */
  u8 *aChange, int nChange,       /* Record to rebase against */
  int *pRc                        /* IN/OUT: Return Code */
){
  sessionBufferGrow(pBuf, 2+nRec+nChange, pRc);
  if( *pRc==SQLITE_OK ){
    int bData = 0;
    u8 *pOut = &pBuf->aBuf[pBuf->nBuf];
    int i;
    u8 *a1 = aRec;
    u8 *a2 = aChange;

    *pOut++ = SQLITE_UPDATE;
    *pOut++ = pIter->bIndirect;
    for(i=0; i<pIter->nCol; i++){
      int n1 = sessionSerialLen(a1);
      int n2 = sessionSerialLen(a2);
      if( pIter->abPK[i] || a2[0]==0 ){
        if( !pIter->abPK[i] && a1[0] ) bData = 1;
        memcpy(pOut, a1, n1);
        pOut += n1;
      }else if( a2[0]!=0xFF && a1[0] ){
        bData = 1;
        memcpy(pOut, a2, n2);
        pOut += n2;
      }else{
        *pOut++ = '\0';
      }
      a1 += n1;
      a2 += n2;
    }
    if( bData ){
      a2 = aChange;
      for(i=0; i<pIter->nCol; i++){
        int n1 = sessionSerialLen(a1);
        int n2 = sessionSerialLen(a2);
        if( pIter->abPK[i] || a2[0]!=0xFF ){
          memcpy(pOut, a1, n1);
          pOut += n1;
        }else{
          *pOut++ = '\0';
        }
        a1 += n1;
        a2 += n2;
      }
      pBuf->nBuf = (pOut - pBuf->aBuf);
    }
  }
}

/*
** pIter is configured to iterate through a changeset. This function rebases 
** that changeset according to the current configuration of the rebaser 
** object passed as the first argument. If no error occurs and argument xOutput
** is not NULL, then the changeset is returned to the caller by invoking
** xOutput zero or more times and SQLITE_OK returned. Or, if xOutput is NULL,
** then (*ppOut) is set to point to a buffer containing the rebased changeset
** before this function returns. In this case (*pnOut) is set to the size of
** the buffer in bytes.  It is the responsibility of the caller to eventually
** free the (*ppOut) buffer using sqlite3_free(). 
**
** If an error occurs, an SQLite error code is returned. If ppOut and
** pnOut are not NULL, then the two output parameters are set to 0 before
** returning.
*/
static int sessionRebase(
  sqlite3_rebaser *p,             /* Rebaser hash table */
  sqlite3_changeset_iter *pIter,  /* Input data */
  int (*xOutput)(void *pOut, const void *pData, int nData),
  void *pOut,                     /* Context for xOutput callback */
  int *pnOut,                     /* OUT: Number of bytes in output changeset */
  void **ppOut                    /* OUT: Inverse of pChangeset */
){
  int rc = SQLITE_OK;
  u8 *aRec = 0;
  int nRec = 0;
  int bNew = 0;
  SessionTable *pTab = 0;
  SessionBuffer sOut = {0,0,0};

  while( SQLITE_ROW==sessionChangesetNext(pIter, &aRec, &nRec, &bNew) ){
    SessionChange *pChange = 0;
    int bDone = 0;

    if( bNew ){
      const char *zTab = pIter->zTab;
      for(pTab=p->grp.pList; pTab; pTab=pTab->pNext){
        if( 0==sqlite3_stricmp(pTab->zName, zTab) ) break;
      }
      bNew = 0;

      /* A patchset may not be rebased */
      if( pIter->bPatchset ){
        rc = SQLITE_ERROR;
      }

      /* Append a table header to the output for this new table */
      sessionAppendByte(&sOut, pIter->bPatchset ? 'P' : 'T', &rc);
      sessionAppendVarint(&sOut, pIter->nCol, &rc);
      sessionAppendBlob(&sOut, pIter->abPK, pIter->nCol, &rc);
      sessionAppendBlob(&sOut,(u8*)pIter->zTab,(int)strlen(pIter->zTab)+1,&rc);
    }

    if( pTab && rc==SQLITE_OK ){
      int iHash = sessionChangeHash(pTab, 0, aRec, pTab->nChange);

      for(pChange=pTab->apChange[iHash]; pChange; pChange=pChange->pNext){
        if( sessionChangeEqual(pTab, 0, aRec, 0, pChange->aRecord) ){
          break;
        }
      }
    }

    if( pChange ){
      assert( pChange->op==SQLITE_DELETE || pChange->op==SQLITE_INSERT );
      switch( pIter->op ){
        case SQLITE_INSERT:
          if( pChange->op==SQLITE_INSERT ){
            bDone = 1;
            if( pChange->bIndirect==0 ){
              sessionAppendByte(&sOut, SQLITE_UPDATE, &rc);
              sessionAppendByte(&sOut, pIter->bIndirect, &rc);
              sessionAppendBlob(&sOut, pChange->aRecord, pChange->nRecord, &rc);
              sessionAppendBlob(&sOut, aRec, nRec, &rc);
            }
          }
          break;

        case SQLITE_UPDATE:
          bDone = 1;
          if( pChange->op==SQLITE_DELETE ){
            if( pChange->bIndirect==0 ){
              u8 *pCsr = aRec;
              sessionSkipRecord(&pCsr, pIter->nCol);
              sessionAppendByte(&sOut, SQLITE_INSERT, &rc);
              sessionAppendByte(&sOut, pIter->bIndirect, &rc);
              sessionAppendRecordMerge(&sOut, pIter->nCol,
                  pCsr, nRec-(pCsr-aRec), 
                  pChange->aRecord, pChange->nRecord, &rc
              );
            }
          }else{
            sessionAppendPartialUpdate(&sOut, pIter,
                aRec, nRec, pChange->aRecord, pChange->nRecord, &rc
            );
          }
          break;

        default:
          assert( pIter->op==SQLITE_DELETE );
          bDone = 1;
          if( pChange->op==SQLITE_INSERT ){
            sessionAppendByte(&sOut, SQLITE_DELETE, &rc);
            sessionAppendByte(&sOut, pIter->bIndirect, &rc);
            sessionAppendRecordMerge(&sOut, pIter->nCol,
                pChange->aRecord, pChange->nRecord, aRec, nRec, &rc
            );
          }
          break;
      }
    }

    if( bDone==0 ){
      sessionAppendByte(&sOut, pIter->op, &rc);
      sessionAppendByte(&sOut, pIter->bIndirect, &rc);
      sessionAppendBlob(&sOut, aRec, nRec, &rc);
    }
    if( rc==SQLITE_OK && xOutput && sOut.nBuf>sessions_strm_chunk_size ){
      rc = xOutput(pOut, sOut.aBuf, sOut.nBuf);
      sOut.nBuf = 0;
    }
    if( rc ) break;
  }

  if( rc!=SQLITE_OK ){
    sqlite3_free(sOut.aBuf);
    memset(&sOut, 0, sizeof(sOut));
  }

  if( rc==SQLITE_OK ){
    if( xOutput ){
      if( sOut.nBuf>0 ){
        rc = xOutput(pOut, sOut.aBuf, sOut.nBuf);
      }
    }else if( ppOut ){
      *ppOut = (void*)sOut.aBuf;
      *pnOut = sOut.nBuf;
      sOut.aBuf = 0;
    }
  }
  sqlite3_free(sOut.aBuf);
  return rc;
}

/* 
** Create a new rebaser object.
*/
int sqlite3rebaser_create(sqlite3_rebaser **ppNew){
  int rc = SQLITE_OK;
  sqlite3_rebaser *pNew;

  pNew = sqlite3_malloc(sizeof(sqlite3_rebaser));
  if( pNew==0 ){
    rc = SQLITE_NOMEM;
  }else{
    memset(pNew, 0, sizeof(sqlite3_rebaser));
  }
  *ppNew = pNew;
  return rc;
}

/* 
** Call this one or more times to configure a rebaser.
*/
int sqlite3rebaser_configure(
  sqlite3_rebaser *p, 
  int nRebase, const void *pRebase
){
  sqlite3_changeset_iter *pIter = 0;   /* Iterator opened on pData/nData */
  int rc;                              /* Return code */
  rc = sqlite3changeset_start(&pIter, nRebase, (void*)pRebase);
  if( rc==SQLITE_OK ){
    rc = sessionChangesetToHash(pIter, &p->grp, 1);
  }
  sqlite3changeset_finalize(pIter);
  return rc;
}

/* 
** Rebase a changeset according to current rebaser configuration 
*/
int sqlite3rebaser_rebase(
  sqlite3_rebaser *p,
  int nIn, const void *pIn, 
  int *pnOut, void **ppOut 
){
  sqlite3_changeset_iter *pIter = 0;   /* Iterator to skip through input */  
  int rc = sqlite3changeset_start(&pIter, nIn, (void*)pIn);

  if( rc==SQLITE_OK ){
    rc = sessionRebase(p, pIter, 0, 0, pnOut, ppOut);
    sqlite3changeset_finalize(pIter);
  }

  return rc;
}

/* 
** Rebase a changeset according to current rebaser configuration 
*/
int sqlite3rebaser_rebase_strm(
  sqlite3_rebaser *p,
  int (*xInput)(void *pIn, void *pData, int *pnData),
  void *pIn,
  int (*xOutput)(void *pOut, const void *pData, int nData),
  void *pOut
){
  sqlite3_changeset_iter *pIter = 0;   /* Iterator to skip through input */  
  int rc = sqlite3changeset_start_strm(&pIter, xInput, pIn);

  if( rc==SQLITE_OK ){
    rc = sessionRebase(p, pIter, xOutput, pOut, 0, 0);
    sqlite3changeset_finalize(pIter);
  }

  return rc;
}

/* 
** Destroy a rebaser object 
*/
void sqlite3rebaser_delete(sqlite3_rebaser *p){
  if( p ){
    sessionDeleteTable(0, p->grp.pList);
    sqlite3_free(p->grp.rec.aBuf);
    sqlite3_free(p);
  }
}

/* 
** Global configuration
*/
int sqlite3session_config(int op, void *pArg){
  int rc = SQLITE_OK;
  switch( op ){
    case SQLITE_SESSION_CONFIG_STRMSIZE: {
      int *pInt = (int*)pArg;
      if( *pInt>0 ){
        sessions_strm_chunk_size = *pInt;
      }
      *pInt = sessions_strm_chunk_size;
      break;
    }
    default:
      rc = SQLITE_MISUSE;
      break;
  }
  return rc;
}

#endif /* SQLITE_ENABLE_SESSION && SQLITE_ENABLE_PREUPDATE_HOOK */
