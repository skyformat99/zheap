/*-------------------------------------------------------------------------
 *
 * zheapamxlog.c
 *	  WAL replay logic for zheap.
 *
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/zheap/zheapamxlog.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/visibilitymap.h"
#include "access/xlog.h"
#include "access/xlogutils.h"
#include "access/zheap.h"
#include "access/zheapam_xlog.h"
#include "storage/freespace.h"

static void
zheap_xlog_insert(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	xl_undo_header	*xlundohdr = (xl_undo_header *) XLogRecGetData(record);
	xl_zheap_insert *xlrec;
	Buffer		buffer;
	Page		page;
	union
	{
		ZHeapTupleHeaderData hdr;
		char		data[MaxZHeapTupleSize];
	}			tbuf;
	ZHeapTupleHeader zhtup;
	UnpackedUndoRecord	undorecord;
	UndoRecPtr	urecptr;
	xl_zheap_header xlhdr;
	uint32		newlen;
	RelFileNode target_node;
	BlockNumber blkno;
	ItemPointerData target_tid;
	XLogRedoAction action;

	xlrec = (xl_zheap_insert *) ((char *) xlundohdr + SizeOfUndoHeader);

	XLogRecGetBlockTag(record, 0, &target_node, NULL, &blkno);
	ItemPointerSetBlockNumber(&target_tid, blkno);
	ItemPointerSetOffsetNumber(&target_tid, xlrec->offnum);

	/*
	 * The visibility map may need to be fixed even if the heap page is
	 * already up-to-date.
	 *
	 * Fixme - This is just for future support of visibility maps with zheap.
	 * Once that is supported, we can test if this code works and remove this
	 * comment after it works.
	 */
	if (xlrec->flags & XLZ_INSERT_ALL_VISIBLE_CLEARED)
	{
		Relation	reln = CreateFakeRelcacheEntry(target_node);
		Buffer		vmbuffer = InvalidBuffer;

		visibilitymap_pin(reln, blkno, &vmbuffer);
		visibilitymap_clear(reln, blkno, vmbuffer, VISIBILITYMAP_VALID_BITS);
		ReleaseBuffer(vmbuffer);
		FreeFakeRelcacheEntry(reln);
	}

	/* prepare an undo record */
	undorecord.uur_type = UNDO_INSERT;
	undorecord.uur_info = 0;
	undorecord.uur_prevlen = 0;
	undorecord.uur_relfilenode = xlundohdr->relfilenode;
	undorecord.uur_prevxid = XLogRecGetXid(record);
	undorecord.uur_xid = XLogRecGetXid(record);
	undorecord.uur_cid = FirstCommandId;
	undorecord.uur_tsid = xlundohdr->tsid;
	undorecord.uur_fork = MAIN_FORKNUM;
	undorecord.uur_blkprev = xlundohdr->blkprev;
	undorecord.uur_block = ItemPointerGetBlockNumber(&target_tid);
	undorecord.uur_offset = ItemPointerGetOffsetNumber(&target_tid);
	undorecord.uur_payload.len = 0;
	undorecord.uur_tuple.len = 0;

	urecptr = PrepareUndoInsert(&undorecord, UNDO_PERSISTENT, XLogRecGetXid(record));
	InsertPreparedUndo();
	SetUndoPageLSNs(lsn);

	/*
	 * undo should be inserted at same location as it was during the actual
	 * insert (DO operation).
	 */
	Assert (urecptr == xlundohdr->urec_ptr);

	/*
	 * If we inserted the first and only tuple on the page, re-initialize the
	 * page from scratch.
	 */
	if (XLogRecGetInfo(record) & XLOG_ZHEAP_INIT_PAGE)
	{
		buffer = XLogInitBufferForRedo(record, 0);
		page = BufferGetPage(buffer);
		ZheapInitPage(page, BufferGetPageSize(buffer));
		action = BLK_NEEDS_REDO;
	}
	else
		action = XLogReadBufferForRedo(record, 0, &buffer);
	if (action == BLK_NEEDS_REDO)
	{
		Size		datalen;
		char	   *data;
		int			trans_slot_id;

		page = BufferGetPage(buffer);

		if (PageGetMaxOffsetNumber(page) + 1 < xlrec->offnum)
			elog(PANIC, "invalid max offset number");

		data = XLogRecGetBlockData(record, 0, &datalen);

		newlen = datalen - SizeOfZHeapHeader;
		Assert(datalen > SizeOfZHeapHeader && newlen <= MaxZHeapTupleSize);
		memcpy((char *) &xlhdr, data, SizeOfZHeapHeader);
		data += SizeOfZHeapHeader;

		zhtup = &tbuf.hdr;
		MemSet((char *) zhtup, 0, SizeofZHeapTupleHeader);
		/* PG73FORMAT: get bitmap [+ padding] [+ oid] + data */
		memcpy((char *) zhtup + SizeofZHeapTupleHeader,
			   data,
			   newlen);
		newlen += SizeofZHeapTupleHeader;
		zhtup->t_infomask2 = xlhdr.t_infomask2;
		zhtup->t_infomask = xlhdr.t_infomask;
		zhtup->t_hoff = xlhdr.t_hoff;

		if (ZPageAddItem(page, (Item) zhtup, newlen, xlrec->offnum,
						 true, true) == InvalidOffsetNumber)
			elog(PANIC, "failed to add tuple");

		trans_slot_id = ZHeapTupleHeaderGetXactSlot(zhtup);
		
		PageSetUNDO(undorecord, page, trans_slot_id, XLogRecGetXid(record), urecptr);
		PageSetLSN(page, lsn);

		if (xlrec->flags & XLZ_INSERT_ALL_VISIBLE_CLEARED)
			PageClearAllVisible(page);

		MarkBufferDirty(buffer);
	}
	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);
	UnlockReleaseUndoBuffers();
}

static void
zheap_xlog_delete(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	xl_undo_header	*xlundohdr = (xl_undo_header *) XLogRecGetData(record);
	Size	recordlen = XLogRecGetDataLen(record);
	xl_zheap_delete *xlrec;
	Buffer		buffer;
	Page		page;
	ZHeapTupleData	zheaptup;
	UnpackedUndoRecord	undorecord;
	UndoRecPtr	urecptr;
	RelFileNode target_node;
	BlockNumber blkno;
	ItemPointerData target_tid;
	XLogRedoAction action;
	Relation	reln;
	ItemId	lp = NULL;

	xlrec = (xl_zheap_delete *) ((char *) xlundohdr + SizeOfUndoHeader);

	XLogRecGetBlockTag(record, 0, &target_node, NULL, &blkno);
	ItemPointerSetBlockNumber(&target_tid, blkno);
	ItemPointerSetOffsetNumber(&target_tid, xlrec->offnum);

	reln = CreateFakeRelcacheEntry(target_node);

	/*
	 * The visibility map may need to be fixed even if the heap page is
	 * already up-to-date.
	 *
	 * Fixme - This is just for future support of visibility maps with zheap.
	 * Once that is supported, we can test if this code works and remove this
	 * comment after it works.
	 */
	if (xlrec->flags & XLZ_DELETE_ALL_VISIBLE_CLEARED)
	{
		Buffer		vmbuffer = InvalidBuffer;

		visibilitymap_pin(reln, blkno, &vmbuffer);
		visibilitymap_clear(reln, blkno, vmbuffer, VISIBILITYMAP_VALID_BITS);
		ReleaseBuffer(vmbuffer);
	}

	action = XLogReadBufferForRedo(record, 0, &buffer);

	page = BufferGetPage(buffer);

	if (PageGetMaxOffsetNumber(page) >= xlrec->offnum)
		lp = PageGetItemId(page, xlrec->offnum);

	if (PageGetMaxOffsetNumber(page) < xlrec->offnum || !ItemIdIsNormal(lp))
		elog(PANIC, "invalid lp");

	zheaptup.t_tableOid = RelationGetRelid(reln);
	zheaptup.t_data = (ZHeapTupleHeader) PageGetItem(page, lp);
	zheaptup.t_len = ItemIdGetLength(lp);
	zheaptup.t_self = target_tid;

	/*
	 * If the WAL stream contains undo tuple, then replace it with the
	 * explicitly stored tuple.
	 */
	if (xlrec->flags & XLZ_HAS_DELETE_UNDOTUPLE)
	{
		char	   *data;
		xl_zheap_header xlhdr;
		union
		{
			ZHeapTupleHeaderData hdr;
			char		data[MaxZHeapTupleSize];
		} tbuf;
		ZHeapTupleHeader zhtup;
		Size	datalen;

		data = (char *) xlrec + SizeOfZHeapDelete;
		memcpy((char *) &xlhdr, data, SizeOfZHeapHeader);
		data += SizeOfZHeapHeader;

		datalen = recordlen - SizeOfUndoHeader - SizeOfZHeapDelete - SizeOfZHeapHeader;

		zhtup = &tbuf.hdr;
		MemSet((char *) zhtup, 0, SizeofZHeapTupleHeader);
		/* PG73FORMAT: get bitmap [+ padding] [+ oid] + data */
		memcpy((char *) zhtup + SizeofZHeapTupleHeader,
			   data,
			   datalen);
		datalen += SizeofZHeapTupleHeader;
		zhtup->t_infomask2 = xlhdr.t_infomask2;
		zhtup->t_infomask = xlhdr.t_infomask;
		zhtup->t_hoff = xlhdr.t_hoff;

		zheaptup.t_data = zhtup;
		zheaptup.t_len = datalen;
	}

	/* prepare an undo record */
	undorecord.uur_type = UNDO_DELETE;
	undorecord.uur_info = 0;
	undorecord.uur_prevlen = 0;
	undorecord.uur_relfilenode = xlundohdr->relfilenode;
	undorecord.uur_prevxid = xlrec->prevxid;
	undorecord.uur_xid = XLogRecGetXid(record);
	undorecord.uur_cid = FirstCommandId;
	undorecord.uur_tsid = xlundohdr->tsid;
	undorecord.uur_fork = MAIN_FORKNUM;
	undorecord.uur_blkprev = xlundohdr->blkprev;
	undorecord.uur_block = ItemPointerGetBlockNumber(&target_tid);
	undorecord.uur_offset = ItemPointerGetOffsetNumber(&target_tid);
	undorecord.uur_payload.len = 0;

	initStringInfo(&undorecord.uur_tuple);

	appendBinaryStringInfo(&undorecord.uur_tuple,
						   (char *) &zheaptup.t_len,
						   sizeof(uint32));
	appendBinaryStringInfo(&undorecord.uur_tuple,
						   (char *) &zheaptup.t_self,
						   sizeof(ItemPointerData));
	appendBinaryStringInfo(&undorecord.uur_tuple,
						   (char *) &zheaptup.t_tableOid,
						   sizeof(Oid));
	appendBinaryStringInfo(&undorecord.uur_tuple,
						   (char *) zheaptup.t_data,
						   zheaptup.t_len);

	urecptr = PrepareUndoInsert(&undorecord, UNDO_PERSISTENT, XLogRecGetXid(record));
	InsertPreparedUndo();
	SetUndoPageLSNs(lsn);

	/*
	 * undo should be inserted at same location as it was during the actual
	 * insert (DO operation).
	 */
	Assert (urecptr == xlundohdr->urec_ptr);

	if (action == BLK_NEEDS_REDO)
	{
		zheaptup.t_data = (ZHeapTupleHeader) PageGetItem(page, lp);
		zheaptup.t_len = ItemIdGetLength(lp);
		ZHeapTupleHeaderSetXactSlot(zheaptup.t_data, xlrec->trans_slot_id);
		zheaptup.t_data->t_infomask &= ~ZHEAP_VIS_STATUS_MASK;
		zheaptup.t_data->t_infomask |= ZHEAP_DELETED;

		PageSetUNDO(undorecord, page, xlrec->trans_slot_id, XLogRecGetXid(record), urecptr);

		/* Mark the page as a candidate for pruning */
		/* Fixme : need to uncomment once we have done this in zheap_delete operation */
		/* PageSetPrunable(page, XLogRecGetXid(record)); */

		if (xlrec->flags & XLZ_DELETE_ALL_VISIBLE_CLEARED)
			PageClearAllVisible(page);

		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);
	}
	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);
	UnlockReleaseUndoBuffers();
	FreeFakeRelcacheEntry(reln);
}

static void
zheap_xlog_update(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	xl_undo_header	*xlundohdr;
	xl_undo_header	*xlnewundohdr;
	xl_zheap_header xlhdr;
	Size	recordlen;
	Size		freespace = 0;
	xl_zheap_update *xlrec;
	Buffer		oldbuffer, newbuffer;
	Page		oldpage, newpage;
	ZHeapTupleData	oldtup;
	ZHeapTupleHeader newtup;
	union
	{
		ZHeapTupleHeaderData hdr;
		char		data[MaxZHeapTupleSize];
	} tbuf;
	UnpackedUndoRecord	undorecord, newundorecord;
	UndoRecPtr	urecptr, newurecptr;
	RelFileNode rnode;
	BlockNumber oldblk, newblk;
	ItemPointerData oldtid, newtid;
	XLogRedoAction oldaction, newaction;
	Relation	reln;
	ItemId	lp = NULL;
	int			trans_slot_id;
	bool	inplace_update;

	xlundohdr = (xl_undo_header *) XLogRecGetData(record);
	xlrec = (xl_zheap_update *) ((char *) xlundohdr + SizeOfUndoHeader);
	recordlen = XLogRecGetDataLen(record);

	if (xlrec->flags & XLZ_NON_INPLACE_UPDATE)
	{
		inplace_update = false;
		xlnewundohdr = (xl_undo_header *) ((char *) xlrec + SizeOfZHeapUpdate);
	}
	else
	{
		inplace_update = true;
	}

	XLogRecGetBlockTag(record, 0, &rnode, NULL, &newblk);
	if (XLogRecGetBlockTag(record, 1, NULL, NULL, &oldblk))
	{
		/* inplace updates are never done across pages */
		Assert(!inplace_update);
	}
	else
		oldblk = newblk;

	ItemPointerSetBlockNumber(&oldtid, oldblk);
	ItemPointerSetOffsetNumber(&oldtid, xlrec->old_offnum);
	ItemPointerSetBlockNumber(&newtid, newblk);
	ItemPointerSetOffsetNumber(&newtid, xlrec->new_offnum);

	reln = CreateFakeRelcacheEntry(rnode);

	/*
	 * The visibility map may need to be fixed even if the zheap page is
	 * already up-to-date.
	 *
	 * Fixme - This is just for future support of visibility maps with zheap.
	 * Once that is supported, we can test if this code works and remove this
	 * comment after it works.
	 */
	if (xlrec->flags & XLZ_UPDATE_OLD_ALL_VISIBLE_CLEARED)
	{
		Buffer		vmbuffer = InvalidBuffer;

		visibilitymap_pin(reln, oldblk, &vmbuffer);
		visibilitymap_clear(reln, oldblk, vmbuffer, VISIBILITYMAP_VALID_BITS);
		ReleaseBuffer(vmbuffer);
	}

	oldaction = XLogReadBufferForRedo(record, (oldblk == newblk) ? 0 : 1, &oldbuffer);

	oldpage = BufferGetPage(oldbuffer);

	if (PageGetMaxOffsetNumber(oldpage) >= xlrec->old_offnum)
		lp = PageGetItemId(oldpage, xlrec->old_offnum);

	if (PageGetMaxOffsetNumber(oldpage) < xlrec->old_offnum || !ItemIdIsNormal(lp))
		elog(PANIC, "invalid lp");

	oldtup.t_tableOid = RelationGetRelid(reln);
	oldtup.t_data = (ZHeapTupleHeader) PageGetItem(oldpage, lp);
	oldtup.t_len = ItemIdGetLength(lp);
	oldtup.t_self = oldtid;

	/*
	 * If the WAL stream contains undo tuple, then replace it with the
	 * explicitly stored tuple.
	 */
	if (xlrec->flags & XLZ_HAS_UPDATE_UNDOTUPLE)
	{
		ZHeapTupleHeader zhtup;
		Size	datalen;
		char	*data;

		data = (char *) xlrec + SizeOfZHeapUpdate;
		memcpy((char *) &xlhdr, data, SizeOfZHeapHeader);
		data += SizeOfZHeapHeader;

		/* There is an additional undo header for non-inplace-update. */
		if (inplace_update)
			datalen = recordlen - SizeOfUndoHeader - SizeOfZHeapUpdate - SizeOfZHeapHeader;
		else
			datalen = recordlen - (2 * SizeOfUndoHeader) - SizeOfZHeapUpdate - SizeOfZHeapHeader;

		zhtup = &tbuf.hdr;
		MemSet((char *) zhtup, 0, SizeofZHeapTupleHeader);
		/* PG73FORMAT: get bitmap [+ padding] [+ oid] + data */
		memcpy((char *) zhtup + SizeofZHeapTupleHeader,
			   data,
			   datalen);
		datalen += SizeofZHeapTupleHeader;
		zhtup->t_infomask2 = xlhdr.t_infomask2;
		zhtup->t_infomask = xlhdr.t_infomask;
		zhtup->t_hoff = xlhdr.t_hoff;

		oldtup.t_data = zhtup;
		oldtup.t_len = datalen;
	}

	/* prepare an undo record */
	undorecord.uur_info = 0;
	undorecord.uur_prevlen = 0;
	undorecord.uur_relfilenode = xlundohdr->relfilenode;
	undorecord.uur_prevxid = xlrec->prevxid;
	undorecord.uur_xid = XLogRecGetXid(record);
	undorecord.uur_cid = FirstCommandId;
	undorecord.uur_tsid = xlundohdr->tsid;
	undorecord.uur_fork = MAIN_FORKNUM;
	undorecord.uur_blkprev = xlundohdr->blkprev;
	undorecord.uur_block = ItemPointerGetBlockNumber(&oldtid);
	undorecord.uur_offset = ItemPointerGetOffsetNumber(&oldtid);
	undorecord.uur_payload.len = 0;

	initStringInfo(&undorecord.uur_tuple);

	appendBinaryStringInfo(&undorecord.uur_tuple,
						   (char *) &oldtup.t_len,
						   sizeof(uint32));
	appendBinaryStringInfo(&undorecord.uur_tuple,
						   (char *) &oldtup.t_self,
						   sizeof(ItemPointerData));
	appendBinaryStringInfo(&undorecord.uur_tuple,
						   (char *) &oldtup.t_tableOid,
						   sizeof(Oid));
	appendBinaryStringInfo(&undorecord.uur_tuple,
						   (char *) oldtup.t_data,
						   oldtup.t_len);

	if (inplace_update)
	{
		undorecord.uur_type =  UNDO_INPLACE_UPDATE;
		urecptr = PrepareUndoInsert(&undorecord, UNDO_PERSISTENT, XLogRecGetXid(record));
	}
	else
	{
		undorecord.uur_type = UNDO_UPDATE;
		undorecord.uur_payload.len = sizeof(ItemPointerData);
		initStringInfo(&undorecord.uur_payload);
		/* update new tuple location in undo record */
		appendBinaryStringInfo(&undorecord.uur_payload,
							   (char *) &newtid,
							   sizeof(ItemPointerData));
		urecptr = PrepareUndoInsert(&undorecord, UNDO_PERSISTENT, XLogRecGetXid(record));

		/* prepare an undo record for new tuple */
		newundorecord.uur_type = UNDO_INSERT;
		newundorecord.uur_info = 0;
		newundorecord.uur_prevlen = 0;
		newundorecord.uur_relfilenode = xlnewundohdr->relfilenode;
		newundorecord.uur_prevxid = XLogRecGetXid(record);
		newundorecord.uur_xid = XLogRecGetXid(record);
		newundorecord.uur_cid = FirstCommandId;
		newundorecord.uur_tsid = xlnewundohdr->tsid;
		newundorecord.uur_fork = MAIN_FORKNUM;
		newundorecord.uur_blkprev = xlnewundohdr->blkprev;
		newundorecord.uur_block = ItemPointerGetBlockNumber(&newtid);
		newundorecord.uur_offset = ItemPointerGetOffsetNumber(&newtid);
		newundorecord.uur_payload.len = 0;
		newundorecord.uur_tuple.len = 0;

		newurecptr = PrepareUndoInsert(&newundorecord, UNDO_PERSISTENT, XLogRecGetXid(record));

		Assert (newurecptr == xlnewundohdr->urec_ptr);
	}

	/*
	 * undo should be inserted at same location as it was during the actual
	 * insert (DO operation).
	 */
	Assert (urecptr == xlundohdr->urec_ptr);

	InsertPreparedUndo();
	SetUndoPageLSNs(lsn);

	/* Ensure old tuple points to the tuple in page. */
	oldtup.t_data = (ZHeapTupleHeader) PageGetItem(oldpage, lp);
	oldtup.t_len = ItemIdGetLength(lp);

	/* First deal with old tuple */
	if (oldaction == BLK_NEEDS_REDO)
	{
		oldtup.t_data->t_infomask &= ~ZHEAP_VIS_STATUS_MASK;
		if (inplace_update)
			oldtup.t_data->t_infomask |= ZHEAP_INPLACE_UPDATED;
		else
			oldtup.t_data->t_infomask |= ZHEAP_UPDATED;
		ZHeapTupleHeaderSetXactSlot(oldtup.t_data, xlrec->old_trans_slot_id);

		if (oldblk != newblk)
			PageSetUNDO(undorecord, oldpage, xlrec->old_trans_slot_id,
						XLogRecGetXid(record), urecptr);

		if (xlrec->flags & XLZ_UPDATE_OLD_ALL_VISIBLE_CLEARED)
			PageClearAllVisible(oldpage);

		PageSetLSN(oldpage, lsn);
		MarkBufferDirty(oldbuffer);
	}

	/*
	 * Read the page the new tuple goes into, if different from old.
	 */
	if (oldblk == newblk)
	{
		newbuffer = oldbuffer;
		newaction = oldaction;
	}
	else if (XLogRecGetInfo(record) & XLOG_ZHEAP_INIT_PAGE)
	{
		newbuffer = XLogInitBufferForRedo(record, 0);
		newpage = (Page) BufferGetPage(newbuffer);
		PageInit(newpage, BufferGetPageSize(newbuffer), 0);
		newaction = BLK_NEEDS_REDO;
	}
	else
		newaction = XLogReadBufferForRedo(record, 0, &newbuffer);

	/*
	 * The visibility map may need to be fixed even if the zheap page is
	 * already up-to-date.
	 */
	if (xlrec->flags & XLZ_UPDATE_NEW_ALL_VISIBLE_CLEARED)
	{
		Buffer		vmbuffer = InvalidBuffer;

		visibilitymap_pin(reln, newblk, &vmbuffer);
		visibilitymap_clear(reln, newblk, vmbuffer, VISIBILITYMAP_VALID_BITS);
		ReleaseBuffer(vmbuffer);
	}

	if (newaction == BLK_NEEDS_REDO)
	{
		uint16		prefixlen = 0,
					suffixlen = 0;
		char	   *newp;
		char	   *recdata;
		char	   *recdata_end;
		Size		datalen;
		Size		tuplen;
		uint32		newlen;

		newpage = BufferGetPage(newbuffer);

		if (PageGetMaxOffsetNumber(newpage) + 1 < xlrec->new_offnum)
			elog(PANIC, "invalid max offset number");

		recdata = XLogRecGetBlockData(record, 0, &datalen);
		recdata_end = recdata + datalen;

		if (xlrec->flags & XLZ_UPDATE_PREFIX_FROM_OLD)
		{
			Assert(newblk == oldblk);
			memcpy(&prefixlen, recdata, sizeof(uint16));
			recdata += sizeof(uint16);
		}
		if (xlrec->flags & XLZ_UPDATE_SUFFIX_FROM_OLD)
		{
			Assert(newblk == oldblk);
			memcpy(&suffixlen, recdata, sizeof(uint16));
			recdata += sizeof(uint16);
		}

		memcpy((char *) &xlhdr, recdata, SizeOfZHeapHeader);
		recdata += SizeOfZHeapHeader;

		tuplen = recdata_end - recdata;
		Assert(tuplen <= MaxZHeapTupleSize);

		newtup = &tbuf.hdr;
		MemSet((char *) newtup, 0, SizeofZHeapTupleHeader);

		/*
		 * Reconstruct the new tuple using the prefix and/or suffix from the
		 * old tuple, and the data stored in the WAL record.
		 */
		newp = (char *) newtup + SizeofZHeapTupleHeader;
		if (prefixlen > 0)
		{
			int			len;

			/* copy bitmap [+ padding] [+ oid] from WAL record */
			len = xlhdr.t_hoff - SizeofZHeapTupleHeader;
			memcpy(newp, recdata, len);
			recdata += len;
			newp += len;

			/* copy prefix from old tuple */
			memcpy(newp, (char *) oldtup.t_data + oldtup.t_data->t_hoff, prefixlen);
			newp += prefixlen;

			/* copy new tuple data from WAL record */
			len = tuplen - (xlhdr.t_hoff - SizeofZHeapTupleHeader);
			memcpy(newp, recdata, len);
			recdata += len;
			newp += len;
		}
		else
		{
			/*
			 * copy bitmap [+ padding] [+ oid] + data from record, all in one
			 * go
			 */
			memcpy(newp, recdata, tuplen);
			recdata += tuplen;
			newp += tuplen;
		}
		Assert(recdata == recdata_end);

		/* copy suffix from old tuple */
		if (suffixlen > 0)
			memcpy(newp, (char *) oldtup.t_data + oldtup.t_len - suffixlen, suffixlen);

		newlen = SizeofZHeapTupleHeader + tuplen + prefixlen + suffixlen;
		newtup->t_infomask2 = xlhdr.t_infomask2;
		newtup->t_infomask = xlhdr.t_infomask;
		newtup->t_hoff = xlhdr.t_hoff;
		trans_slot_id = ZHeapTupleHeaderGetXactSlot(newtup);

		if (inplace_update)
		{
			/*
			 * For inplace updates, we copy the entire data portion including null
			 * bitmap of new tuple.
			 */
			ItemIdChangeLen(lp, newlen);
			memcpy((char *) oldtup.t_data + SizeofZHeapTupleHeader,
				   (char *) newtup + SizeofZHeapTupleHeader,
				   newlen - SizeofZHeapTupleHeader);
			PageSetUNDO(undorecord, newpage, trans_slot_id, XLogRecGetXid(record), urecptr);
		}
		else
		{
			if (ZPageAddItem(newpage, (Item) newtup, newlen, xlrec->new_offnum,
						 true, true) == InvalidOffsetNumber)
				elog(PANIC, "failed to add tuple");
			PageSetUNDO(undorecord, newpage, trans_slot_id, XLogRecGetXid(record), newurecptr);
		}

		if (xlrec->flags & XLZ_UPDATE_NEW_ALL_VISIBLE_CLEARED)
			PageClearAllVisible(newpage);

		freespace = PageGetHeapFreeSpace(newpage); /* needed to update FSM below */

		PageSetLSN(newpage, lsn);
		MarkBufferDirty(newbuffer);
	}

	if (BufferIsValid(newbuffer) && newbuffer != oldbuffer)
		UnlockReleaseBuffer(newbuffer);
	if (BufferIsValid(oldbuffer))
		UnlockReleaseBuffer(oldbuffer);
	UnlockReleaseUndoBuffers();
	FreeFakeRelcacheEntry(reln);

	/*
	 * Update the freespace.  We don't need to update it for inplace updates as
	 * they won't freeup any space or consume any extra space assuming the new
	 * tuple is about the same size as the old one.  See heap_xlog_update.
	 */
	if (newaction == BLK_NEEDS_REDO && !inplace_update && freespace < BLCKSZ / 5)
		XLogRecordPageWithFreeSpace(rnode, newblk, freespace);
}

void
zheap_redo(XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	switch (info & XLOG_ZHEAP_OPMASK)
	{
		case XLOG_ZHEAP_INSERT:
			zheap_xlog_insert(record);
			break;
		case XLOG_ZHEAP_DELETE:
			zheap_xlog_delete(record);
			break;
		case XLOG_ZHEAP_UPDATE:
			zheap_xlog_update(record);
			break;
		default:
			elog(PANIC, "zheap_redo: unknown op code %u", info);
	}
}
