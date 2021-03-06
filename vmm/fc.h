// fc.h : definitions related to memory forensic support.
//
//      Memory analysis in vmm.c is generally instant and work on both live and
//      static memory.
//
//      Forensic memory analysis is more thorough and batch-oriented and is
//      only available for static memory. After general startup a single pass
//      of consisting of multiple forensic activities will start. The result
//      is generally stored in an sqlite database with may be used to query
//      the results.  
//
// (c) Ulf Frisk, 2020
// Author: Ulf Frisk, pcileech@frizk.net
//
#ifndef __FC_H__
#define __FC_H__
#include <windows.h>
#include "vmm.h"
#include "mm_pfn.h"
#include "sqlite/sqlite3.h"

#define FC_SQL_POOL_CONNECTION_NUM          4
#define FC_PHYSMEM_NUM_CHUNKS               0x1000

typedef struct tdFCSQL_INSERTSTRTABLE {
    QWORD id;
    DWORD cwsz;     // WCHAR count (excl. NULL)
    DWORD cbu;      // UTF-8 byte count (excl. NULL)
    DWORD cbj;      // UTF-8 JSON string count (excl. NULL)
} FCSQL_INSERTSTRTABLE, *PFCSQL_INSERTSTRTABLE;

typedef struct tdFC_TIMELINE_INFO {
    DWORD dwId;
    DWORD dwFileSizeUTF8;
    DWORD dwFileSizeJSON;
    CHAR szNameShort[7];        // 6 chars + NULL
    WCHAR wszNameFileUTF8[32];
    WCHAR wszNameFileJSON[32];
} FC_TIMELINE_INFO, *PFC_TIMELINE_INFO;

typedef struct tdFC_CONTEXT {
    BOOL fInitStart;
    BOOL fInitFinish;
    CRITICAL_SECTION Lock;
    struct {
        DWORD tp;                           // type as specified in FC_DATABASE_TYPE_*
        WCHAR wszDatabaseWinPath[MAX_PATH]; // Windows file path
        CHAR szuDatabase[MAX_PATH];         // Sqlite3 database path in UTF-8
        BOOL fSingleThread;                 // enforce single-thread access (used during insert-bound init phase)
        HANDLE hEvent[FC_SQL_POOL_CONNECTION_NUM];
        sqlite3 *hSql[FC_SQL_POOL_CONNECTION_NUM];
        QWORD qwIdStr;
    } db;
    struct {
        DWORD cTp;
        PFC_TIMELINE_INFO pInfo;    // array of cTp items
    } Timeline;
} FC_CONTEXT, *PFC_CONTEXT;



// ----------------------------------------------------------------------------
// FC global variable below:
// ----------------------------------------------------------------------------

PFC_CONTEXT ctxFc;



// ----------------------------------------------------------------------------
// FC INITIALIZATION FUNCTIONALITY BELOW:
// ----------------------------------------------------------------------------

#define FC_DATABASE_TYPE_MEMORY                 1
#define FC_DATABASE_TYPE_TEMPFILE_CLOSE         2
#define FC_DATABASE_TYPE_TEMPFILE_NOCLOSE       3
#define FC_DATABASE_TYPE_TEMPFILE_STATIC        4
#define FC_DATABASE_TYPE_MAX                    4

/*
* Initialize (or re-initialize) the forensic sub-system.
* -- dwDatabaseType = database type as specified by: FC_DATABASE_TYPE_*
* -- fForceReInit
* -- return
*/
_Success_(return)
BOOL FcInitialize(_In_ DWORD dwDatabaseType, _In_ BOOL fForceReInit);

/*
* Close the forensic sub-system.
*/
VOID FcClose();



// ----------------------------------------------------------------------------
// FC DATABASE FUNCTIONALITY BELOW:
// ----------------------------------------------------------------------------

/*
* Retrieve an SQLITE database handle. The retrieved handle must be
* returned with Fc_SqlReserveReturn().
* -- return = an SQLITE handle, or NULL on error.
*/
_Success_(return != NULL)
sqlite3* Fc_SqlReserve();

/*
* Return a SQLITE database handle previously retrieved with Fc_SqlReserve()
* so that other threads may use it.
* -- hSql = the SQLITE database handle.
* -- return = always NULL.
*/
_Success_(return != NULL)
sqlite3* Fc_SqlReserveReturn(_In_opt_ sqlite3 *hSql);

/*
* Execute a single SQLITE database SQL query and return the SQLITE result code.
* -- szSql
* -- return = sqlite return code.
*/
_Success_(return == SQLITE_OK)
int Fc_SqlExec(_In_ LPSTR szSql);

/*
* Execute a single SQLITE database SQL query and return all results as numeric
* 64-bit results in an array that must have capacity to hold all values.
* result and the SQLITE result code.
* -- szSql
* -- cQueryValue = nummber of numeric query arguments-
* -- pqwQueryValues = array of 64-bit query arguments-
* -- cResultValues = max number of numeric query results.
* -- pqwResultValues = array to receive 64-bit query results.
* -- pcResultValues = optional to receive numer of query results read.
* -- return = sqlite return code.
*/
_Success_(return == SQLITE_OK)
int Fc_SqlQueryN(
    _In_ LPSTR szSql,
    _In_ DWORD cQueryValues,
    _In_reads_(cQueryValues) PQWORD pqwQueryValues,
    _In_ DWORD cResultValues,
    _Out_writes_(cResultValues) PQWORD pqwResultValues,
    _Out_opt_ PDWORD pcResultValues
);

/*
* Helper function to insert a string into the database 'str' table.
* NB! the string must not exceed 2048 characters.
* -- hStmt
* -- wsz
* -- owszSub = sub-offset to 2'nd string at the end of wsz (if any).
* -- pThis
* -- return
*/
_Success_(return)
BOOL Fc_SqlInsertStr(
    _In_ sqlite3_stmt *hStmt,
    _In_ LPWSTR wsz,
    _In_ DWORD cwszSubOffset,
    _Out_ PFCSQL_INSERTSTRTABLE pThis
);

/*
* Helper function to do multiple 64-bit binds towards a statement in a 
* convenient way. NB! 32-bit DWORDs must be casted to 64-bit QWORD to
* avoid padding of 0xcccccccc in the high-part.
* -- hStmt
* -- iFirstBind
* -- cInt64
* -- ... = vararg of cInt64 QWORDs to bind to hStmt.
* -- return
*/
_Success_(return == SQLITE_OK)
int Fc_SqlBindMultiInt64(
    _In_ sqlite3_stmt *hStmt,
    _In_ DWORD iFirstBind,
    _In_ DWORD cInt64,
    ...
);



// ----------------------------------------------------------------------------
// FC TIMELINING FUNCTIONALITY BELOW:
// ----------------------------------------------------------------------------

#define FC_LINELENGTH_TIMELINE_UTF8             64
#define FC_LINELENGTH_TIMELINE_JSON             110

#define FC_TIMELINE_ACTION_NONE                 0
#define FC_TIMELINE_ACTION_CREATE               1
#define FC_TIMELINE_ACTION_MODIFY               2
#define FC_TIMELINE_ACTION_READ                 3
#define FC_TIMELINE_ACTION_DELETE               4
#define FC_TIMELINE_ACTION_MAX                  4

static LPCSTR FC_TIMELINE_ACTION_STR[FC_TIMELINE_ACTION_MAX + 1] = {
    "---",
    "CRE",
    "MOD",
    "RD ",
    "DEL",
};

typedef struct tdFC_MAP_TIMELINEENTRY {
    QWORD id;
    QWORD ft;
    DWORD tp;
    DWORD ac;
    DWORD pid;
    QWORD data64;
    QWORD cszuOffset;               // offset to start of "line" in bytes (utf-8)
    QWORD cszjOffset;               // offset to start of "line" in bytes (json)
    DWORD cwszText;                 // WCHAR count not including terminating null
    LPWSTR wszText;                 // LPWSTR pointed into FCOB_MAP_TIMELINE.wszMultiText
    LPWSTR wszTextSub;              // potential sub-text at end of wszText
} FC_MAP_TIMELINEENTRY, *PFC_MAP_TIMELINEENTRY;

typedef struct tdFCOB_MAP_TIMELINE {
    OB ObHdr;
    LPWSTR wszMultiText;            // multi-wstr pointed into by FC_MAP_TIMELINEENTRY.wszText
    DWORD cbMultiText;
    DWORD cMap;                     // # map entries.
    FC_MAP_TIMELINEENTRY pMap[];    // map entries.
} FCOB_MAP_TIMELINE, *PFCOB_MAP_TIMELINE;

/*
* Retrieve a timeline map object consisting of timeline data.
* -- dwTimelineType = the timeline type, 0 for all.
* -- qwId = the minimum timeline id of the entries to retrieve.
* -- cId = the number of timeline entries to retrieve.
* -- ppObTimelineMap
* -- return
*/
_Success_(return)
BOOL FcTimelineMap_GetFromIdRange(
    _In_ DWORD dwTimelineType,
    _In_ QWORD qwId,
    _In_ QWORD cId,
    _Out_ PFCOB_MAP_TIMELINE *ppObTimelineMap
);

/*
* Retrieve the minimum timeline id that exists within a byte range inside a
* timeline file of a specific type.
* -- dwTimelineType = the timeline type, 0 for all.
* -- fJSON = is JSON type, otherwise UTF8 type.
* -- qwFilePos = the file position.
* -- pqwId = pointer to receive the result id.
* -- return
*/
_Success_(return)
BOOL FcTimeline_GetIdFromPosition(
    _In_ DWORD dwTimelineType,
    _In_ BOOL fJSON,
    _In_ QWORD qwFilePos,
    _Out_ PQWORD pqwId
);

#endif /* __FC_H__ */
