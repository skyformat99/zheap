/*-------------------------------------------------------------------------
 *
 * undorecord.c
 *	  encode and decode undo records
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/access/undo/undorecord.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/subtrans.h"
#include "access/xact.h"
#include "access/undodiscard.h"
#include "access/undolog.h"
#include "access/undorecord.h"
#include "access/undoinsert.h"
#include "catalog/pg_tablespace.h"
#include "storage/block.h"
#include "storage/buf.h"
#include "storage/bufmgr.h"
#include "miscadmin.h"

/*
 * FIXME:  Do we want to support undo tuple size which is more than the BLCKSZ
 * if not than undo record can spread across 2 buffers at the max.
 */
#define MAX_BUFFER_PER_UNDO    2

/*
 * Consider buffers needed for updating previous transaction's
 * starting undo record. Hence increased by 1.
 */
#define MAX_UNDO_BUFFERS       (MAX_PREPARED_UNDO + 1) * MAX_BUFFER_PER_UNDO

/* Maximum number of undo record that can be prepared before calling insert. */
#define MAX_PREPARED_UNDO 2

/* Workspace for InsertUndoRecord and UnpackUndoRecord. */
static UndoRecordHeader work_hdr;
static UndoRecordRelationDetails work_rd;
static UndoRecordBlock work_blk;
static UndoRecordTransaction work_txn;
static UndoRecordPayload work_payload;

/*
 * Previous top transaction id which inserted the undo.  Whenever a new main
 * transaction try to prepare an undo record we will check if its txid not the
 * same as prev_txid then we will insert the start undo record.
 */
static TransactionId	prev_txid[UndoPersistenceLevels] = { 0 };

/* Undo block number to buffer mapping. */
typedef struct UndoBuffers
{
	BlockNumber		blk;			/* block number */
	Buffer			buf;			/* buffer allocated for the block */
} UndoBuffers;

static UndoBuffers def_buffers[MAX_UNDO_BUFFERS];
static int	buffer_idx;

/*
 * Structure to hold the prepared undo information.
 */
typedef struct PreparedUndoSpace
{
	UndoRecPtr urp;						/* undo record pointer */
	UnpackedUndoRecord *urec;			/* undo record */
	int undo_buffer_idx[MAX_BUFFER_PER_UNDO]; /* undo_buffer array index */
} PreparedUndoSpace;

static PreparedUndoSpace  def_prepared[MAX_PREPARED_UNDO];
static int prepare_idx;
static int	max_prepare_undo = MAX_PREPARED_UNDO;

/*
 * By default prepared_undo and undo_buffer points to the static memory.
 * In case caller wants to support more than default max_prepared undo records
 * then the limit can be increased by calling UndoSetPrepareSize function.
 * Therein, dynamic memory will be allocated and prepared_undo and undo_buffer
 * will start pointing to newly allocated memory, which will be released by
 * UnlockReleaseUndoBuffers and these variables will again set back to their
 * default values.
 */
static PreparedUndoSpace *prepared_undo = def_prepared;
static UndoBuffers *undo_buffer = def_buffers;

/*
 * Structure to hold the previous transaction's undo update information.
 */
typedef struct PreviousTxnUndoRecord
{
	UndoRecPtr	urecptr;	/* current txn's starting urecptr */
	UndoRecPtr	prev_urecptr; /* prev txn's starting urecptr */
	int			starting_pos;	/* offset in uno where urecptr is written */
	int			num_blocks;	/* number of prev_txn_undo_buffers */
	int			prev_txn_undo_buffers[MAX_BUFFER_PER_UNDO]; /* total blocks
														   * to be written */
} PreviousTxnUndoRecord;

static PreviousTxnUndoRecord prev_txn_undo_record;

/* Prototypes for static functions. */
static void UndoRecordSetInfo(UnpackedUndoRecord *uur);
static bool InsertUndoBytes(char *sourceptr, int sourcelen,
				char **writeptr, char *endptr,
				int *my_bytes_written, int *total_bytes_written);
static bool ReadUndoBytes(char *destptr, int readlen,
			  char **readptr, char *endptr,
			  int *my_bytes_read, int *total_bytes_read, bool nocopy);
static UnpackedUndoRecord* UndoGetOneRecord(UnpackedUndoRecord *urec,
											UndoRecPtr urp, RelFileNode rnode,
											UndoPersistence persistence);
static void PrepareUndoRecordUpdateTransInfo(UndoRecPtr urecptr);
static void UndoRecordUpdateTransInfo(void);
static int InsertFindBufferSlot(RelFileNode rnode, BlockNumber blk,
								ReadBufferMode rbm,
								UndoPersistence persistence);
static bool IsPrevTxnUndoDiscarded(UndoLogControl *log,
								   UndoRecPtr prev_xact_urp);

/*
 * Compute and return the expected size of an undo record.
 */
Size
UndoRecordExpectedSize(UnpackedUndoRecord *uur)
{
	Size	size;

	/* Fixme : Temporary hack to allow zheap to set some value for uur_info. */
	/* if (uur->uur_info == 0) */
		UndoRecordSetInfo(uur);

	size = SizeOfUndoRecordHeader;
	if ((uur->uur_info & UREC_INFO_RELATION_DETAILS) != 0)
		size += SizeOfUndoRecordRelationDetails;
	if ((uur->uur_info & UREC_INFO_BLOCK) != 0)
		size += SizeOfUndoRecordBlock;
	if ((uur->uur_info & UREC_INFO_TRANSACTION) != 0)
		size += SizeOfUndoRecordTransaction;
	if ((uur->uur_info & UREC_INFO_PAYLOAD) != 0)
	{
		size += SizeOfUndoRecordPayload;
		size += uur->uur_payload.len;
		size += uur->uur_tuple.len;
	}

	return size;
}

/*
 * Insert as much of an undo record as will fit in the given page.
 * starting_byte is the byte within the give page at which to begin
 * writing, while *already_written is the number of bytes written to
 * previous pages.  Returns true if the remainder of the record was
 * written and false if more bytes remain to be written; in either
 * case, *already_written is set to the number of bytes written thus
 * far.
 *
 * This function assumes that if *already_written is non-zero on entry,
 * the same UnpackedUndoRecord is passed each time.  It also assumes
 * that UnpackUndoRecord is not called between successive calls to
 * InsertUndoRecord for the same UnpackedUndoRecord.
 */
bool
InsertUndoRecord(UnpackedUndoRecord *uur, Page page,
				 int starting_byte, int *already_written)
{
	char   *writeptr = (char *) page + starting_byte;
	char   *endptr = (char *) page + BLCKSZ;
	int		my_bytes_written = *already_written;

	if (uur->uur_info == 0)
		UndoRecordSetInfo(uur);

	/*
	 * If this is the first call, copy the UnpackedUndoRecord into the
	 * temporary variables of the types that will actually be stored in the
	 * undo pages.  We just initialize everything here, on the assumption
	 * that it's not worth adding branches to save a handful of assignments.
	 */
	if (*already_written == 0)
	{
		work_hdr.urec_type = uur->uur_type;
		work_hdr.urec_info = uur->uur_info;
		work_hdr.urec_prevlen = uur->uur_prevlen;
		work_hdr.urec_relfilenode = uur->uur_relfilenode;
		work_hdr.urec_prevxid = uur->uur_prevxid;
		work_hdr.urec_xid = uur->uur_xid;
		work_hdr.urec_cid = uur->uur_cid;
		work_rd.urec_tsid = uur->uur_tsid;
		work_rd.urec_fork = uur->uur_fork;
		work_blk.urec_blkprev = uur->uur_blkprev;
		work_blk.urec_block = uur->uur_block;
		work_blk.urec_offset = uur->uur_offset;
		work_txn.urec_next = uur->uur_next;
		work_payload.urec_payload_len = uur->uur_payload.len;
		work_payload.urec_tuple_len = uur->uur_tuple.len;
	}
	else
	{
		/*
		 * We should have been passed the same record descriptor as before,
		 * or caller has messed up.
		 */
		Assert(work_hdr.urec_type == uur->uur_type);
		Assert(work_hdr.urec_info == uur->uur_info);
		Assert(work_hdr.urec_prevlen == uur->uur_prevlen);
		Assert(work_hdr.urec_relfilenode == uur->uur_relfilenode);
		Assert(work_hdr.urec_prevxid == uur->uur_prevxid);
		Assert(work_hdr.urec_xid == uur->uur_xid);
		Assert(work_hdr.urec_cid == uur->uur_cid);
		Assert(work_rd.urec_tsid == uur->uur_tsid);
		Assert(work_rd.urec_fork == uur->uur_fork);
		Assert(work_blk.urec_blkprev == uur->uur_blkprev);
		Assert(work_blk.urec_block == uur->uur_block);
		Assert(work_blk.urec_offset == uur->uur_offset);
		Assert(work_txn.urec_next == uur->uur_next);
		Assert(work_payload.urec_payload_len == uur->uur_payload.len);
		Assert(work_payload.urec_tuple_len == uur->uur_tuple.len);
	}

	/* Write header (if not already done). */
	if (!InsertUndoBytes((char *) &work_hdr, SizeOfUndoRecordHeader,
						 &writeptr, endptr,
						 &my_bytes_written, already_written))
		return false;

	/* Write relation details (if needed and not already done). */
	if ((uur->uur_info & UREC_INFO_RELATION_DETAILS) != 0 &&
		!InsertUndoBytes((char *) &work_rd, SizeOfUndoRecordRelationDetails,
						 &writeptr, endptr,
						 &my_bytes_written, already_written))
		return false;

	/* Write block information (if needed and not already done). */
	if ((uur->uur_info & UREC_INFO_BLOCK) != 0 &&
		!InsertUndoBytes((char *) &work_blk, SizeOfUndoRecordBlock,
						 &writeptr, endptr,
						 &my_bytes_written, already_written))
		return false;

	/* Write transaction information (if needed and not already done). */
	if ((uur->uur_info & UREC_INFO_TRANSACTION) != 0 &&
		!InsertUndoBytes((char *) &work_txn, SizeOfUndoRecordTransaction,
						 &writeptr, endptr,
						 &my_bytes_written, already_written))
		return false;

	/* Write payload information (if needed and not already done). */
	if ((uur->uur_info & UREC_INFO_PAYLOAD) != 0)
	{
		/* Payload header. */
		if (!InsertUndoBytes((char *) &work_payload, SizeOfUndoRecordPayload,
							 &writeptr, endptr,
							 &my_bytes_written, already_written))
			return false;

		/* Payload bytes. */
		if (uur->uur_payload.len > 0 &&
			!InsertUndoBytes(uur->uur_payload.data, uur->uur_payload.len,
							 &writeptr, endptr,
							 &my_bytes_written, already_written))
			return false;

		/* Tuple bytes. */
		if (uur->uur_tuple.len > 0 &&
			!InsertUndoBytes(uur->uur_tuple.data, uur->uur_tuple.len,
							 &writeptr, endptr,
							 &my_bytes_written, already_written))
			return false;
	}

	/* Hooray! */
	return true;
}

/*
 * Write undo bytes from a particular source, but only to the extent that
 * they weren't written previously and will fit.
 *
 * 'sourceptr' points to the source data, and 'sourcelen' is the length of
 * that data in bytes.
 *
 * 'writeptr' points to the insertion point for these bytes, and is updated
 * for whatever we write.  The insertion point must not pass 'endptr', which
 * represents the end of the buffer into which we are writing.
 *
 * 'my_bytes_written' is a pointer to the count of previous-written bytes
 * from this and following structures in this undo record; that is, any
 * bytes that are part of previous structures in the record have already
 * been subtracted out.  We must update it for the bytes we write.
 *
 * 'total_bytes_written' points to the count of all previously-written bytes,
 * and must likewise be updated for the bytes we write.
 *
 * The return value is false if we ran out of space before writing all
 * the bytes, and otherwise true.
 */
static bool
InsertUndoBytes(char *sourceptr, int sourcelen,
				char **writeptr, char *endptr,
				int *my_bytes_written, int *total_bytes_written)
{
	int		can_write;
	int		remaining;

	/*
	 * If we've previously written all of these bytes, there's nothing
	 * to do except update *my_bytes_written, which we must do to ensure
	 * that the next call to this function gets the right starting value.
	 */
	if (*my_bytes_written >= sourcelen)
	{
		*my_bytes_written -= sourcelen;
		return true;
	}

	/* Compute number of bytes we can write. */
	remaining = sourcelen - *my_bytes_written;
	can_write = Min(remaining, endptr - *writeptr);

	/* Bail out if no bytes can be written. */
	if (can_write == 0)
		return false;

	/* Copy the bytes we can write. */
	memcpy(*writeptr, sourceptr + *my_bytes_written, can_write);

	/* Update bookkeeeping infrormation. */
	*writeptr += can_write;
	*total_bytes_written += can_write;
	*my_bytes_written = 0;

	/* Return true only if we wrote the whole thing. */
	return (can_write == remaining);
}

/*
 * Call UnpackUndoRecord() one or more times to unpack an undo record.  For
 * the first call, starting_byte should be set to the beginning of the undo
 * record within the specified page, and *already_decoded should be set to 0;
 * the function will update it based on the number of bytes decoded.  The
 * return value is true if the entire record was unpacked and false if the
 * record continues on the next page.  In the latter case, the function
 * should be called again with the next page, passing starting_byte as the
 * sizeof(PageHeaderData).
 */
bool UnpackUndoRecord(UnpackedUndoRecord *uur, Page page, int starting_byte,
					  int *already_decoded)
{
	char	*readptr = (char *)page + starting_byte;
	char	*endptr = (char *) page + BLCKSZ;
	int		my_bytes_decoded = *already_decoded;
	bool	is_undo_splited = (my_bytes_decoded > 0) ? true : false;

	/* Decode header (if not already done). */
	if (!ReadUndoBytes((char *) &work_hdr, SizeOfUndoRecordHeader,
					   &readptr, endptr,
					   &my_bytes_decoded, already_decoded, false))
		return false;

	uur->uur_type = work_hdr.urec_type;
	uur->uur_info = work_hdr.urec_info;
	uur->uur_prevlen = work_hdr.urec_prevlen;
	uur->uur_relfilenode = work_hdr.urec_relfilenode;
	uur->uur_prevxid = work_hdr.urec_prevxid;
	uur->uur_xid = work_hdr.urec_xid;
	uur->uur_cid = work_hdr.urec_cid;

	if ((uur->uur_info & UREC_INFO_RELATION_DETAILS) != 0)
	{
		/* Decode header (if not already done). */
		if (!ReadUndoBytes((char *) &work_rd, SizeOfUndoRecordRelationDetails,
							&readptr, endptr,
							&my_bytes_decoded, already_decoded, false))
			return false;

		uur->uur_tsid = work_rd.urec_tsid;
		uur->uur_fork = work_rd.urec_fork;
	}

	if ((uur->uur_info & UREC_INFO_BLOCK) != 0)
	{
		if (!ReadUndoBytes((char *) &work_blk, SizeOfUndoRecordBlock,
							&readptr, endptr,
							&my_bytes_decoded, already_decoded, false))
			return false;

		uur->uur_blkprev = work_blk.urec_blkprev;
		uur->uur_block = work_blk.urec_block;
		uur->uur_offset = work_blk.urec_offset;
	}

	if ((uur->uur_info & UREC_INFO_TRANSACTION) != 0)
	{
		if (!ReadUndoBytes((char *) &work_txn, SizeOfUndoRecordTransaction,
							&readptr, endptr,
							&my_bytes_decoded, already_decoded, false))
			return false;

		uur->uur_next = work_txn.urec_next;
		uur->uur_xidepoch = work_txn.urec_xidepoch;
	}

	/* Read payload information (if needed and not already done). */
	if ((uur->uur_info & UREC_INFO_PAYLOAD) != 0)
	{
		if (!ReadUndoBytes((char *) &work_payload, SizeOfUndoRecordPayload,
							&readptr, endptr,
							&my_bytes_decoded, already_decoded, false))
			return false;

		uur->uur_payload.len = work_payload.urec_payload_len;
		uur->uur_tuple.len = work_payload.urec_tuple_len;

		/*
		 * If we can read the complete record from a single page then just
		 * point payload data and tuple data into the page otherwise allocate
		 * the memory.
		 *
		 * XXX There is possibility of optimization that instead of always
		 * allocating the memory whenever tuple is split we can check if any of
		 * the payload or tuple data falling into the same page then don't
		 * allocate the memory for that.
		 */
		if (!is_undo_splited &&
			uur->uur_payload.len + uur->uur_tuple.len <= (endptr - readptr))
		{
			uur->uur_payload.data = readptr;
			readptr += uur->uur_payload.len;

			uur->uur_tuple.data = readptr;
		}
		else
		{
			if (uur->uur_payload.len > 0 && uur->uur_payload.data == NULL)
				uur->uur_payload.data = (char *) palloc0(uur->uur_payload.len);

			if (uur->uur_tuple.len > 0 && uur->uur_tuple.data == NULL)
				uur->uur_tuple.data = (char *) palloc0(uur->uur_tuple.len);

			if (!ReadUndoBytes((char *) uur->uur_payload.data,
							   uur->uur_payload.len, &readptr, endptr,
							   &my_bytes_decoded, already_decoded, false))
				return false;

			if (!ReadUndoBytes((char *) uur->uur_tuple.data,
							   uur->uur_tuple.len, &readptr, endptr,
							   &my_bytes_decoded, already_decoded, false))
				return false;
		}
	}

	return true;
}

/*
 * Read undo bytes into a particular destination,
 *
 * 'destptr' points to the source data, and 'readlen' is the length of
 * that data to be read in bytes.
 *
 * 'readptr' points to the read point for these bytes, and is updated
 * for how much we read.  The read point must not pass 'endptr', which
 * represents the end of the buffer from which we are reading.
 *
 * 'my_bytes_read' is a pointer to the count of previous-read bytes
 * from this and following structures in this undo record; that is, any
 * bytes that are part of previous structures in the record have already
 * been subtracted out.  We must update it for the bytes we read.
 *
 * 'total_bytes_read' points to the count of all previously-read bytes,
 * and must likewise be updated for the bytes we read.
 *
 * nocopy if this flag is set true then it will just skip the readlen
 * size in undo but it will not copy into the buffer.
 *
 * The return value is false if we ran out of space before read all
 * the bytes, and otherwise true.
 */
static bool
ReadUndoBytes(char *destptr, int readlen, char **readptr, char *endptr,
			  int *my_bytes_read, int *total_bytes_read, bool nocopy)
{
	int		can_read;
	int		remaining;

	if (*my_bytes_read >= readlen)
	{
		*my_bytes_read -= readlen;
		return true;
	}

	/* Compute number of bytes we can read. */
	remaining = readlen - *my_bytes_read;
	can_read = Min(remaining, endptr - *readptr);

	/* Bail out if no bytes can be read. */
	if (can_read == 0)
		return false;

	/* Copy the bytes we can read. */
	if (!nocopy)
		memcpy(destptr + *my_bytes_read, *readptr, can_read);

	/* Update bookkeeping information. */
	*readptr += can_read;
	*total_bytes_read += can_read;
	*my_bytes_read = 0;

	/* Return true only if we wrote the whole thing. */
	return (can_read == remaining);
}

/*
 * Check if previous transactions undo is already discarded.
 *
 * Caller should call this under log->discard_lock
 */
static bool
IsPrevTxnUndoDiscarded(UndoLogControl *log, UndoRecPtr prev_xact_urp)
{
	if (log->oldest_data == InvalidUndoRecPtr)
	{
		/*
		 * oldest_data is not yet initialized.  We have to check
		 * UndoLogIsDiscarded and if it's already discarded then we have
		 * nothing to do.
		 */
		LWLockRelease(&log->discard_lock);
		if (UndoLogIsDiscarded(prev_xact_urp))
			return true;
		LWLockAcquire(&log->discard_lock, LW_SHARED);
	}

	/* Check again if it's already discarded. */
	if (prev_xact_urp < log->oldest_data)
	{
		LWLockRelease(&log->discard_lock);
		return true;
	}

	return false;
}

/*
 * Prepare for Updation of transaction information inside the undo record
 *
 * First prepare undo record for the new transaction will invoke this routine
 * to update its first undo record pointer in previous transaction's first undo
 * record.
 */
void
PrepareUndoRecordUpdateTransInfo(UndoRecPtr urecptr)
{
	UndoRecPtr	prev_xact_urp;
	Buffer		buffer = InvalidBuffer;
	BlockNumber	cur_blk;
	RelFileNode	rnode;
	UndoLogNumber logno = UndoRecPtrGetLogNo(urecptr);
	UndoLogControl *log;
	Page		page;
	char	   *readptr;
	char	   *endptr;
	int			my_bytes_decoded = 0;
	int			already_decoded = 0;
	int			starting_byte;
	int			bufidx;

	log = UndoLogGet(logno);

	/*
	 * TODO: For now we don't know how to build a transaction chain for
	 * temporary undo logs.  That's because this log might have been used by a
	 * different backend, and we can't access its buffers.  What should happen
	 * is that the undo data should be automatically discarded when the other
	 * backend detaches, but that code doesn't exist yet and the undo worker
	 * can't do it either.
	 */
	if (log->meta.persistence == UNDO_TEMP)
		return;

	/*
	 * We can read the previous transaction's location without locking,
	 * because only the backend attached to the log can write to it (or we're
	 * in recovery).
	 */
	Assert(AmAttachedToUndoLog(log) || InRecovery);
	if (log->meta.last_xact_start == 0)
		prev_xact_urp = InvalidUndoRecPtr;
	else
		prev_xact_urp = MakeUndoRecPtr(log->logno, log->meta.last_xact_start);

	/*
	 * If previous transaction's urp is not valid means this backend is
	 * preparing its first undo so fetch the information from the undo log
	 * if it's still invalid urp means this is the first undo record for this
	 * log and we have nothing to update.
	 */
	if (!UndoRecPtrIsValid(prev_xact_urp))
		return;

	/*
	 * Acquire the discard lock before accessing the undo record so that
	 * discard worker doen't remove the record while we are in process of
	 * reading it.
	 */
	LWLockAcquire(&log->discard_lock, LW_SHARED);

	if (IsPrevTxnUndoDiscarded(log, prev_xact_urp))
		return;

	UndoRecPtrAssignRelFileNode(rnode, prev_xact_urp);
	cur_blk = UndoRecPtrGetBlockNum(prev_xact_urp);
	starting_byte = UndoRecPtrGetPageOffset(prev_xact_urp);

	while (true)
	{
		/* Go to the next block if already_decoded is non zero */
		if (already_decoded != 0)
		{
			starting_byte = UndoLogBlockHeaderSize;
			my_bytes_decoded = already_decoded;
			UnlockReleaseBuffer(buffer);
			cur_blk++;
		}

		buffer = ReadBufferWithoutRelcache(rnode, UndoLogForkNum, cur_blk,
										   RBM_NORMAL, NULL,
										   RelPersistenceForUndoPersistence(log->meta.persistence));
		LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
		page = BufferGetPage(buffer);

		readptr = (char *)page + starting_byte;
		endptr = (char *) page + BLCKSZ;

		/* Decode header. */
		if (!ReadUndoBytes((char *) &work_hdr, SizeOfUndoRecordHeader,
						   &readptr, endptr,
						   &my_bytes_decoded, &already_decoded, false))
			continue;

		/* If the undo record has the relation header then just skip it. */
		if ((work_hdr.urec_info & UREC_INFO_RELATION_DETAILS) != 0)
		{
			if (!ReadUndoBytes((char *) &work_rd, SizeOfUndoRecordRelationDetails,
							   &readptr, endptr,
							   &my_bytes_decoded, &already_decoded, true))
				continue;
		}

		/* If the undo record has the block header then just skip it. */
		if ((work_hdr.urec_info & UREC_INFO_BLOCK) != 0)
		{
			if (!ReadUndoBytes((char *) &work_blk, SizeOfUndoRecordBlock,
							   &readptr, endptr,
							   &my_bytes_decoded, &already_decoded, true))
				continue;
		}

		/* The undo record must have transaction header. */
		Assert(work_hdr.urec_info & UREC_INFO_TRANSACTION);

		if (readptr == endptr)
			continue;
		prev_txn_undo_record.num_blocks = 0;
		readptr += urec_next_pos;
		if (readptr >= endptr)	/* end of page reached move to next page */
		{
			int from_start = readptr - endptr;
			starting_byte = UndoLogBlockHeaderSize + from_start;
			UnlockReleaseBuffer(buffer);
			cur_blk++;
			buffer = ReadBufferWithoutRelcache(rnode, UndoLogForkNum, cur_blk,
											   RBM_NORMAL, NULL,
											   RelPersistenceForUndoPersistence(log->meta.persistence));
			LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
			page = BufferGetPage(buffer);

			readptr = (char *)page + starting_byte;
			endptr = (char *) page + BLCKSZ;
		}

		UnlockReleaseBuffer(buffer);

		bufidx = InsertFindBufferSlot(rnode, cur_blk,
									  RBM_NORMAL,
									  log->meta.persistence);
		prev_txn_undo_record.starting_pos = readptr - (char *) page;
		prev_txn_undo_record.prev_txn_undo_buffers[prev_txn_undo_record.num_blocks] = bufidx;
		prev_txn_undo_record.num_blocks++;
		prev_txn_undo_record.urecptr = urecptr;
		prev_txn_undo_record.prev_urecptr = prev_xact_urp;

		if ((endptr - readptr) < SizeOfUrecNext)
		{
			cur_blk++;
			bufidx = InsertFindBufferSlot(rnode, cur_blk,
										  RBM_NORMAL,
										  log->meta.persistence);
			prev_txn_undo_record.prev_txn_undo_buffers[prev_txn_undo_record.num_blocks] = bufidx;
			prev_txn_undo_record.num_blocks++;
		}

		break;
	}

	LWLockRelease(&log->discard_lock);
}

/*
 * Insert the already prepared transaction update undo.
 *
 * Should be called within critcal section.
 */
static void
UndoRecordUpdateTransInfo(void)
{
	UndoLogNumber logno = UndoRecPtrGetLogNo(prev_txn_undo_record.urecptr);
	Page		page;
	char	   *readptr;
	char	   *endptr;
	int			starting_byte;
	int			my_bytes_written = 0;
	int			already_written = 0;
	int			idx = 0;
	UndoRecPtr	prev_urp = InvalidUndoRecPtr;
	UndoLogControl *log;

	log = UndoLogGet(logno);
	prev_urp = prev_txn_undo_record.prev_urecptr;

	/*
	 * Acquire the discard lock before accessing the undo record so that
	 * discard worker doen't remove the record while we are in process of
	 * reading it.
	 */
	LWLockAcquire(&log->discard_lock, LW_SHARED);

	if (IsPrevTxnUndoDiscarded(log, prev_urp))
		return;

	/*
	 * Update the next transactions start urecptr in the transaction
	 * header.
	 */
	starting_byte = prev_txn_undo_record.starting_pos;
	work_txn.urec_next = prev_txn_undo_record.urecptr;

	do
	{
		Buffer  buffer;
		int		buf_idx;

		buf_idx = prev_txn_undo_record.prev_txn_undo_buffers[idx];
		buffer = undo_buffer[buf_idx].buf;
		page = BufferGetPage(buffer);

		readptr = (char *) page + starting_byte;
		endptr = (char *) page + BLCKSZ;
		if (!InsertUndoBytes((char*)&prev_txn_undo_record.urecptr, SizeOfUrecNext,
							&readptr, endptr, &my_bytes_written, &already_written))
		{
			my_bytes_written = already_written;
			MarkBufferDirty(buffer);
			starting_byte = UndoLogBlockHeaderSize;
			idx++;
			continue;
		}

		Assert(already_written == SizeOfUrecNext);
		break;
	} while(true);

	LWLockRelease(&log->discard_lock);
}

/*
 * Set uur_info for an UnpackedUndoRecord appropriately based on which
 * other fields are set.
 */
static void
UndoRecordSetInfo(UnpackedUndoRecord *uur)
{
	if (uur->uur_tsid != DEFAULTTABLESPACE_OID ||
		uur->uur_fork != MAIN_FORKNUM)
		uur->uur_info |= UREC_INFO_RELATION_DETAILS;
	if (uur->uur_block != InvalidBlockNumber)
		uur->uur_info |= UREC_INFO_BLOCK;
	if (uur->uur_next != InvalidUndoRecPtr)
		uur->uur_info |= UREC_INFO_TRANSACTION;
	if (uur->uur_payload.len || uur->uur_tuple.len)
		uur->uur_info |= UREC_INFO_PAYLOAD;
}

/*
 * Find the block number in undo buffer array, if it's present then just return
 * its index otherwise search the buffer and insert an entry.
 *
 * Undo log insertions are append-only.  If the caller is writing new data
 * that begins exactly at the beginning of a page, then there cannot be any
 * useful data after that point.  In that case RBM_ZERO can be passed in as
 * rbm so that we can skip a useless read of a disk block.  In all other
 * cases, RBM_NORMAL should be passed in, to read the page in if it doesn't
 * happen to be already in the buffer pool.
 */
static int
InsertFindBufferSlot(RelFileNode rnode,
					 BlockNumber blk,
					 ReadBufferMode rbm,
					 UndoPersistence persistence)
{
	int 	i;
	Buffer 	buffer;

	/* Don't do anything, if we already have a buffer pinned for the block. */
	for (i = 0; i < buffer_idx; i++)
	{
		if (blk == undo_buffer[i].blk)
			break;
	}

	/*
	 * We did not find the block so allocate the buffer and insert into the
	 * undo buffer array
	 */
	if (i == buffer_idx)
	{
		/*
		 * Fetch the buffer in which we want to insert the undo record.
		 */
		buffer = ReadBufferWithoutRelcache(rnode,
										   UndoLogForkNum,
										   blk,
										   rbm,
										   NULL,
										   RelPersistenceForUndoPersistence(persistence));
		undo_buffer[buffer_idx].buf = buffer;
		undo_buffer[buffer_idx].blk = blk;
		buffer_idx++;
	}

	return i;
}

/*
 * Call UndoSetPrepareSize to set the value of how many maximum prepared can
 * be done before inserting the prepared undo.  If size is > MAX_PREPARED_UNDO
 * then it will allocate extra memory to hold the extra prepared undo.
 */
void
UndoSetPrepareSize(int max_prepare)
{
	if (max_prepare <= MAX_PREPARED_UNDO)
		return;

	prepared_undo = palloc0(max_prepare * sizeof(PreparedUndoSpace));

	/*
	 * Consider buffers needed for updating previous transaction's
	 * starting undo record. Hence increased by 1.
	 */
	undo_buffer = palloc0((max_prepare + 1) * MAX_BUFFER_PER_UNDO *
						 sizeof(UndoBuffers));
	max_prepare_undo = max_prepare;
}

/*
 * Call PrepareUndoInsert to tell the undo subsystem about the undo record you
 * intended to insert.  Upon return, the necessary undo buffers are pinned.
 * This should be done before any critical section is established, since it
 * can fail.
 *
 * If not in recovery, 'xid' should refer to the top transaction id because
 * undo log only stores mapping for the top most transactions.
 * If in recovery, 'xid' refers to the transaction id stored in WAL.
 */
UndoRecPtr
PrepareUndoInsert(UnpackedUndoRecord *urec, UndoPersistence upersistence,
				  TransactionId xid, xl_undolog_meta *undometa)
{
	UndoLogControl *log;
	UndoRecordSize	size;
	UndoRecPtr		urecptr;
	RelFileNode		rnode;
	UndoRecordSize  cur_size = 0;
	BlockNumber		cur_blk;
	TransactionId	txid;
	int				starting_byte;
	int				index = 0;
	int				bufidx;
	bool			need_start_undo = false;
	bool			first_rec_in_recovery;
	ReadBufferMode	rbm;

	/* Already reached maximum prepared limit. */
	if (prepare_idx == max_prepare_undo)
		return InvalidUndoRecPtr;

	/*
	 * If this is the first undo record for this top transaction add the
	 * transaction information to the undo record.
	 *
	 * XXX there is also an option that instead of adding the information to
	 * this record we can prepare a new record which only contain transaction
	 * informations.
	 */
	if (xid == InvalidTransactionId)
	{
		/* we expect during recovery, we always have a valid transaction id. */
		Assert (!InRecovery);
		txid = GetTopTransactionId();
	}
	else
	{
		/*
		 * Assign the top transaction id because undo log only stores mapping for
		 * the top most transactions.
		 */
		Assert (InRecovery || (xid == GetTopTransactionId()));
		txid = xid;
	}

	/*
	 * If this is the first undo record for this transaction then set the
	 * uur_next to the SpecialUndoRecPtr.  This is the indication to allocate
	 * the space for the transaction header and the valid value of the uur_next
	 * will be updated while preparing the first undo record of the next
	 * transaction.
	 */
	first_rec_in_recovery = InRecovery && IsTransactionFirstRec(txid);
	if ((!InRecovery && prev_txid[upersistence] != txid) ||
		first_rec_in_recovery)
		need_start_undo = true;

 resize:
	if (need_start_undo)
	{
		urec->uur_next = SpecialUndoRecPtr;
		urec->uur_xidepoch = GetEpochForXid(txid);
	}
	else
		urec->uur_next = InvalidUndoRecPtr;

	/* calculate the size of the undo record. */
	size = UndoRecordExpectedSize(urec);

	if (InRecovery)
		urecptr = UndoLogAllocateInRecovery(xid, size, upersistence);
	else
		urecptr = UndoLogAllocate(size, upersistence, undometa);

	log = UndoLogGet(UndoRecPtrGetLogNo(urecptr));
	Assert(AmAttachedToUndoLog(log) || InRecovery);

	/*
	 * If we've rewound all the way back to the start of the transaction by
	 * rolling back the first subtransaction (which we can't detect until
	 * after we've allocated some space), we'll need a new transaction header.
	 * If we weren't already generating one, that will make the record larger,
	 * so we'll have to go back and recompute the size.
	 *
	 * TODO: What should we do here if we switched to different undo log
	 * mid-transaction?
	 */
	if (!need_start_undo &&
		log->meta.insert == log->meta.last_xact_start)
	{
		need_start_undo = true;
		urec->uur_info = 0;		/* force recomputation of info bits */
		goto resize;
	}

	/*
	 * If transaction id is switched then update the previous transaction's
	 * start undo record.
	 */
	if (need_start_undo &&
		((!InRecovery && prev_txid[upersistence] != txid) ||
		 first_rec_in_recovery))
	{
		/* Don't update our own start header. */
		if (log->meta.last_xact_start != log->meta.insert)
			PrepareUndoRecordUpdateTransInfo(urecptr);

		/* Remember the current transaction's xid. */
		prev_txid[upersistence] = txid;

		/* Store the current transaction's start undorecptr in the undo log. */
		UndoLogSetLastXactStartPoint(urecptr);
	}

	UndoLogAdvance(urecptr, size, upersistence);
	cur_blk = UndoRecPtrGetBlockNum(urecptr);
	UndoRecPtrAssignRelFileNode(rnode, urecptr);
	starting_byte = UndoRecPtrGetPageOffset(urecptr);

	/*
	 * If we happen to be writing the very first byte into this page, then
	 * there is no need to read from disk.
	 */
	if (starting_byte == UndoLogBlockHeaderSize)
		rbm = RBM_ZERO;
	else
		rbm = RBM_NORMAL;

	do
	{
		bufidx = InsertFindBufferSlot(rnode, cur_blk, rbm,
									  log->meta.persistence);
		if (cur_size == 0)
			cur_size = BLCKSZ - starting_byte;
		else
			cur_size += BLCKSZ - UndoLogBlockHeaderSize;

		/* FIXME: Should we just report error ? */
		Assert(index < MAX_BUFFER_PER_UNDO);

		/* Keep the track of the buffers we have pinned. */
		prepared_undo[prepare_idx].undo_buffer_idx[index++] = bufidx;

		/* Undo record can not fit into this block so go to the next block. */
		cur_blk++;

		/*
		 * If we need more pages they'll be all new so we can definitely skip
		 * reading from disk.
		 */
		rbm = RBM_ZERO;
	} while (cur_size < size);

	/*
	 * Save referenced of undo record pointer as well as undo record.
	 * InsertPreparedUndo will use these to insert the prepared record.
	 */
	prepared_undo[prepare_idx].urec = urec;
	prepared_undo[prepare_idx].urp = urecptr;
	prepare_idx++;

	return urecptr;
}

/*
 * Insert a previously-prepared undo record.  This will lock the buffers
 * pinned in the previous step, write the actual undo record into them,
 * and mark them dirty.  For persistent undo, this step should be performed
 * after entering a critical section; it should never fail.
 */
void
InsertPreparedUndo(void)
{
	Page	page;
	int		starting_byte;
	int		already_written;
	int		bufidx = 0;
	int		idx;
	uint16	undo_len = 0;
	UndoRecPtr	urp;
	UnpackedUndoRecord	*uur;
	UndoLogOffset offset;
	UndoLogControl *log;
	uint16	prev_undolen;

	Assert(prepare_idx > 0);

	/* Lock all the buffers and mark them dirty. */
	for (idx = 0; idx < buffer_idx; idx++)
		LockBuffer(undo_buffer[idx].buf, BUFFER_LOCK_EXCLUSIVE);

	for (idx = 0; idx < prepare_idx; idx++)
	{
		uur = prepared_undo[idx].urec;
		urp = prepared_undo[idx].urp;

		already_written = 0;
		bufidx = 0;
		starting_byte = UndoRecPtrGetPageOffset(urp);
		offset = UndoRecPtrGetOffset(urp);

		/*
		 * We can read meta.prevlen without locking, because only we can write
		 * to it.
		 */
		log = UndoLogGet(UndoRecPtrGetLogNo(urp));
		Assert(AmAttachedToUndoLog(log) || InRecovery);
		prev_undolen = log->meta.prevlen;

		/* store the previous undo record length in the header */
		uur->uur_prevlen = prev_undolen;

		/* if starting a new log then there is no prevlen to store */
		if (offset == UndoLogBlockHeaderSize)
			uur->uur_prevlen = 0;

		/* if starting from a new page then include header in prevlen */
		else if (starting_byte == UndoLogBlockHeaderSize)
				uur->uur_prevlen += UndoLogBlockHeaderSize;

		undo_len = 0;

		do
		{
			PreparedUndoSpace undospace = prepared_undo[idx];
			Buffer  buffer;

			buffer = undo_buffer[undospace.undo_buffer_idx[bufidx]].buf;
			page = BufferGetPage(buffer);

			/*
			 * Initialize the page whenever we try to write the first record
			 * in page.
			 */
			if (starting_byte == UndoLogBlockHeaderSize)
				PageInit(page, BLCKSZ, 0);

			/*
			 * Try to insert the record into the current page. If it doesn't
			 * succeed then recall the routine with the next page.
			 */
			if (InsertUndoRecord(uur, page, starting_byte, &already_written))
			{
				undo_len += already_written;
				MarkBufferDirty(buffer);
				break;
			}

			MarkBufferDirty(buffer);
			starting_byte = UndoLogBlockHeaderSize;
			bufidx++;

			/*
			 * If we are swithing to the next block then consider the header
			 * in total undo length.
			 */
			undo_len += UndoLogBlockHeaderSize;

			Assert(bufidx < MAX_BUFFER_PER_UNDO);
		} while(true);

		prev_undolen = undo_len;

		UndoLogSetPrevLen(UndoRecPtrGetLogNo(urp), prev_undolen);

		if (prev_txn_undo_record.num_blocks > 0)
			UndoRecordUpdateTransInfo();

		/*
		 * Set the current undo location for a transaction.  This is required
		 * to perform rollback during abort of transaction.
		 */
		SetCurrentUndoLocation(urp);
	}
}

/*
 * Unlock and release undo buffers.  This step performed after exiting any
 * critical section.
 */
void
UnlockReleaseUndoBuffers(void)
{
	int	i;
	for (i = 0; i < buffer_idx; i++)
	{
		UnlockReleaseBuffer(undo_buffer[i].buf);
		undo_buffer[i].blk = InvalidBlockNumber;
		undo_buffer[i].buf = InvalidBlockNumber;
	}

	prev_txn_undo_record.num_blocks = 0;

	/* Reset the prepared index. */
	prepare_idx = 0;
	buffer_idx = 0;

	/*
	 * max_prepare_undo limit is changed so free the allocated memory and reset
	 * all the variable back to its default value.
	 */
	if (max_prepare_undo > MAX_PREPARED_UNDO)
	{
		pfree(undo_buffer);
		pfree(prepared_undo);
		undo_buffer = def_buffers;
		prepared_undo = def_prepared;
		max_prepare_undo = MAX_PREPARED_UNDO;
	}
}

/*
 * Helper function for UndoFetchRecord.  It will fetch the undo record pointed
 * by urp and unpack the record into urec.  This function will not release the
 * pin on the buffer if complete record is fetched from one buffer,  now caller
 * can reuse the same urec to fetch the another undo record which is on the
 * same block.  Caller will be responsible to release the buffer inside urec
 * and set it to invalid if he wishes to fetch the record from another block.
 */
static UnpackedUndoRecord*
UndoGetOneRecord(UnpackedUndoRecord *urec, UndoRecPtr urp, RelFileNode rnode,
				 UndoPersistence persistence)
{
	Buffer			 buffer = urec->uur_buffer;
	Page			 page;
	int				 starting_byte = UndoRecPtrGetPageOffset(urp);
	int				 already_decoded = 0;
	BlockNumber		 cur_blk;
	bool			 is_undo_splited = false;

	cur_blk = UndoRecPtrGetBlockNum(urp);

	/* If we already have a previous buffer then no need to allocate new. */
	if (!BufferIsValid(buffer))
	{
		buffer = ReadBufferWithoutRelcache(rnode, UndoLogForkNum, cur_blk,
										   RBM_NORMAL, NULL,
										   RelPersistenceForUndoPersistence(persistence));

		urec->uur_buffer = buffer;
	}

	while (true)
	{
		LockBuffer(buffer, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buffer);

		/*
		 * FIXME: This can be optimized to just fetch header first and only
		 * if matches with block number and offset then fetch the complete
		 * record.
		 */
		if (UnpackUndoRecord(urec, page, starting_byte, &already_decoded))
			break;

		starting_byte = UndoLogBlockHeaderSize;
		is_undo_splited = true;

		/*
		 * Complete record is not fitting into one buffer so release the buffer
		 * pin and also set invalid buffer in the undo record.
		 */
		urec->uur_buffer = InvalidBuffer;
		UnlockReleaseBuffer(buffer);

		/* Go to next block. */
		cur_blk++;
		buffer = ReadBufferWithoutRelcache(rnode, UndoLogForkNum, cur_blk,
										   RBM_NORMAL, NULL,
										   RelPersistenceForUndoPersistence(persistence));
	}

	/*
	 * If we have copied the data then release the buffer. Otherwise just
	 * unlock it.
	 */
	if (is_undo_splited)
		UnlockReleaseBuffer(buffer);
	else
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);

	return urec;
}

/*
 * Fetch the next undo record for given blkno, offset and transaction id (if
 * valid).  We need to match transaction id along with block number and offset
 * because in some cases (like reuse of slot for committed transaction), we
 * need to skip the record if it is modified by a transaction later than the
 * transaction indicated by previous undo record.  For example, consider a
 * case where tuple (ctid - 0,1) is modified by transaction id 500 which
 * belongs to transaction slot 0. Then, the same tuple is modified by
 * transaction id 501 which belongs to transaction slot 1.  Then, both the
 * transaction slots are marked for reuse. Then, again the same tuple is
 * modified by transaction id 502 which has used slot 0.  Now, some
 * transaction which has started before transaction 500 wants to traverse the
 * chain to find visible tuple will keep on rotating infinitely between undo
 * tuple written by 502 and 501.  In such a case, we need to skip the undo
 * tuple written by transaction 502 when we want to find the undo record
 * indicated by the previous pointer of undo tuple written by transaction 501.
 * Start the search from urp.  Caller need to call UndoRecordRelease to release the
 * resources allocated by this function.
 *
 * urec_ptr_out is undo record pointer of the qualified undo record if valid
 * pointer is passed.
 */
UnpackedUndoRecord*
UndoFetchRecord(UndoRecPtr urp, BlockNumber blkno, OffsetNumber offset,
				TransactionId xid, UndoRecPtr *urec_ptr_out,
				SatisfyUndoRecordCallback callback)
{
	RelFileNode		 rnode, prevrnode = {0};
	UnpackedUndoRecord *urec = NULL;
	int	logno;

	if (urec_ptr_out)
		*urec_ptr_out = InvalidUndoRecPtr;

	urec = palloc0(sizeof(UnpackedUndoRecord));

	/* Find the undo record pointer we are interested in. */
	while (true)
	{
		UndoLogControl *log;

		UndoRecPtrAssignRelFileNode(rnode, urp);

		/*
		 * If we have a valid buffer pinned then just ensure that we want to
		 * find the next tuple from the same block.  Otherwise release the
		 * buffer and set it invalid
		 */
		if (BufferIsValid(urec->uur_buffer))
		{
			/*
			 * Undo buffer will be changed if the next undo record belongs to a
			 * different block or undo log.
			 */
			if (UndoRecPtrGetBlockNum(urp) != BufferGetBlockNumber(urec->uur_buffer) ||
				(prevrnode.relNode != rnode.relNode))
			{
				ReleaseBuffer(urec->uur_buffer);
				urec->uur_buffer = InvalidBuffer;
			}
		}
		else
		{
			/*
			 * If there is not a valid buffer in urec->uur_buffer that means we
			 * had copied the payload data and tuple data so free them.
			 */
			if (urec->uur_payload.data)
				pfree(urec->uur_payload.data);
			if (urec->uur_tuple.data)
				pfree(urec->uur_tuple.data);
		}

		/* Reset the urec before fetching the tuple */
		urec->uur_tuple.data = NULL;
		urec->uur_tuple.len = 0;
		urec->uur_payload.data = NULL;
		urec->uur_payload.len = 0;
		prevrnode = rnode;

		logno = UndoRecPtrGetLogNo(urp);
		log = UndoLogGet(logno);

		/*
		 * Prevent UndoDiscardOneLog() from discarding data while we try to
		 * read it.  Usually we would acquire log->mutex to read log->meta
		 * members, but in this case we know that discard can't move without
		 * also holding log->discard_lock.
		 */
		LWLockAcquire(&log->discard_lock, LW_SHARED);
		if (!UndoRecPtrIsValid(log->oldest_data))
		{
			/*
			 * UndoDiscardInfo is not yet initialized. Hence, we've to check
			 * UndoLogIsDiscarded and if it's already discarded then we have
			 * nothing to do.
			 */
			LWLockRelease(&log->discard_lock);
			if (UndoLogIsDiscarded(urp))
			{
				if (BufferIsValid(urec->uur_buffer))
					ReleaseBuffer(urec->uur_buffer);
				return NULL;
			}

			LWLockAcquire(&log->discard_lock, LW_SHARED);
		}

		/* Check if it's already discarded. */
		if (urp < log->oldest_data)
		{
			LWLockRelease(&log->discard_lock);
			if (BufferIsValid(urec->uur_buffer))
				ReleaseBuffer(urec->uur_buffer);
			return NULL;
		}

		/* Fetch the current undo record. */
		urec = UndoGetOneRecord(urec, urp, rnode, log->meta.persistence);
		LWLockRelease(&log->discard_lock);

		if (blkno == InvalidBlockNumber)
			break;

		/* Check whether the undorecord satisfies conditions */
		if (callback(urec, blkno, offset, xid))
			break;

		urp = urec->uur_blkprev;
	}

	if (urec_ptr_out)
		*urec_ptr_out = urp;
	return urec;
}

/*
 * Return the previous undo record pointer.
 */
UndoRecPtr
UndoGetPrevUndoRecptr(UndoRecPtr urp, uint16 prevlen)
{
	UndoLogNumber logno = UndoRecPtrGetLogNo(urp);
	UndoLogOffset offset = UndoRecPtrGetOffset(urp);

	/* calculate the previous undo record pointer */
	return MakeUndoRecPtr (logno, offset - prevlen);
}

/*
 * Release the resources allocated by UndoFetchRecord.
 */
void
UndoRecordRelease(UnpackedUndoRecord *urec)
{
	/*
	 * If the undo record has a valid buffer then just release the buffer
	 * otherwise free the tuple and payload data.
	 */
	if (BufferIsValid(urec->uur_buffer))
	{
		ReleaseBuffer(urec->uur_buffer);
	}
	else
	{
		if (urec->uur_payload.data)
			pfree(urec->uur_payload.data);
		if (urec->uur_tuple.data)
			pfree(urec->uur_tuple.data);
	}

	pfree (urec);
}

/*
 * Called whenever we attach to a new undo log, so that we forget about our
 * translation-unit private state relating to the log we were last attached
 * to.
 */
void
UndoRecordOnUndoLogChange(UndoPersistence persistence)
{
	prev_txid[persistence] = InvalidTransactionId;
}
