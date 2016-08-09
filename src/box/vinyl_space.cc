/*
 * Copyright 2010-2016, Tarantool AUTHORS, please see AUTHORS file.
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include "vinyl_engine.h"
#include "vinyl_index.h"
#include "vinyl_space.h"
#include "xrow.h"
#include "tuple.h"
#include "scoped_guard.h"
#include "txn.h"
#include "index.h"
#include "space.h"
#include "schema.h"
#include "port.h"
#include "request.h"
#include "iproto_constants.h"
#include "vinyl.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

VinylSpace::VinylSpace(Engine *e)
	:Handler(e)
{ }

void
VinylSpace::applySnapshotRow(struct space *space, struct request *request)
{
	assert(request->type == IPROTO_INSERT);
	assert(request->header != NULL);
	struct vy_env *env = ((VinylEngine *)space->handler->engine)->env;
	VinylIndex *index;

	/* Check the tuple fields. */
	tuple_validate_raw(space->format, request->tuple);

	struct vy_tx *tx = vy_begin(env);
	if (tx == NULL)
		diag_raise();

	int64_t signature = request->header->lsn;
	for (uint32_t i = 0; i < space->index_count; ++i) {
		index = (VinylIndex *)space->index[i];
		if (vy_replace(tx, index->db, request->tuple,
			       request->tuple_end))
			diag_raise();
	}


	int rc = vy_prepare(env, tx);
	switch (rc) {
	case 0:
		if (vy_commit(env, tx, signature))
			panic("failed to commit vinyl transaction");
		return;
	case 1: /* rollback */
		vy_rollback(env, tx);
		/* must never happen during JOIN */
		tnt_raise(ClientError, ER_TRANSACTION_CONFLICT);
		return;
	case -1:
		vy_rollback(env, tx);
		diag_raise();
		return;
	default:
		unreachable();
	}
}

/**
 * Delete a tuple from all indexes, primary and secondary.
 */
static void
vinyl_delete_all(struct space *space, struct tuple *tuple,
		 struct request *request, struct vy_tx *tx)
{
	uint32_t key_size;
	VinylIndex *index;
	uint32_t part_count;
	const char *key;
	for (uint32_t iid = 0; iid < space->index_count; ++iid) {
		index = (VinylIndex *) space->index[iid];
		if (request->index_id == iid && request->key != NULL) {
			key = request->key;
		} else {
			key = tuple_extract_key(tuple,
						vy_index_key_def(index->db),
						&key_size);
		}
		part_count = mp_decode_array(&key);
		if (vy_delete(tx, index->db, key, part_count))
			diag_raise();
	}
}

/**
 * Insert a tuple into a single index, be it primary or secondary.
 */
static void
vinyl_insert_one(VinylIndex *index, const char *tuple,
		 const char *tuple_end, uint32_t space_id,
		 struct vy_tx *tx)
{
	/*
	 * If the index is unique then the new tuple must not
	 * conflict with existing tuples. If the index is not
	 * unique a conflict is impossible.
	 */
	if (index->key_def->opts.is_unique) {
		uint32_t key_len;
		struct key_def *def = vy_index_key_def(index->db);
		const char *key;
		key = tuple_extract_key_raw(tuple, tuple_end, def, &key_len);
		mp_decode_array(&key); /* Skip array header. */
		struct tuple *found;
		if (vy_get(tx, index->db, key, def->part_count, &found))
			diag_raise();

		if (found) {
			struct space *space = space_by_id(space_id);
			tnt_raise(ClientError, ER_TUPLE_FOUND,
				  index_name(index), space_name(space));
		}
	}

	if (vy_replace(tx, index->db, tuple, tuple_end))
		diag_raise();
}

/**
 * Insert a tuple into a space.
 */
static void
vinyl_insert_all(struct space *space, struct request *request,
		 struct vy_tx *tx)
{
	assert(request->type == IPROTO_INSERT);
	/* Check if there is at least one index. */
	index_find(space, 0);
	for (uint32_t iid = 0; iid < space->index_count; ++iid) {
		VinylIndex *index = (VinylIndex *)space->index[iid];
		vinyl_insert_one(index, request->tuple, request->tuple_end,
				 request->space_id, tx);
	}
}

static void
vinyl_replace_one(struct space *space, struct request *request,
		  struct vy_tx *tx, struct txn_stmt *stmt)
{
	VinylEngine *engine = (VinylEngine *)space->handler->engine;
	(void) engine;
	assert((request->type == IPROTO_REPLACE) ||
	       (!engine->recovery_complete));
	assert(space->index_count == 1);
	VinylIndex *index = (VinylIndex *) index_find(space, 0);
	/**
	 * If the space has triggers then need to remember old_tuple
	 * for triggers run. For getting the old_tuple need to search it
	 * by vy_get.
	 */
	if (!rlist_empty(&space->on_replace)) {
		VinylIndex *pk = (VinylIndex *)index_find(space, 0);
		const char *key;
		key = tuple_extract_key_raw(request->tuple, request->tuple_end,
					    pk->key_def, NULL);
		uint32_t part_count = mp_decode_array(&key);
		if (vy_get(tx, pk->db, key, part_count, &stmt->old_tuple))
			diag_raise();
		if (stmt->old_tuple)
			tuple_ref(stmt->old_tuple);
	}
	if (vy_replace(tx, index->db, request->tuple, request->tuple_end))
		diag_raise();
}

static void
vinyl_replace_all(struct space *space, struct request *request,
		  struct vy_tx *tx, struct txn_stmt *stmt)
{
	VinylEngine *engine = (VinylEngine *)space->handler->engine;
	(void) engine;
	assert((request->type == IPROTO_REPLACE) ||
	       (!engine->recovery_complete));
	struct tuple *old_tuple;
	VinylIndex *pk = (VinylIndex *)index_find(space, 0);
	const char *key;
	key = tuple_extract_key_raw(request->tuple, request->tuple_end,
				    pk->key_def, NULL);
	uint32_t part_count = mp_decode_array(&key);

	/* If the request type is replace then get full tuple from primary. */
	if (vy_get(tx, pk->db, key, part_count, &old_tuple))
		diag_raise();

	/* Tuple doesn't exist so it can be inserted. */
	if (vy_replace(tx, pk->db, request->tuple, request->tuple_end))
		diag_raise();

	/* Update secondary keys, avoid duplicates. */
	for (uint32_t iid = 1; iid < space->index_count; ++iid) {
		VinylIndex *index = (VinylIndex *) space->index[iid];
		/**
		 * Delete goes first, so if old and new keys
		 * fully match, there is no look up beyond the
		 * transaction index.
		 */
		if (old_tuple) {
			if (request->index_id == iid && request->key != NULL) {
				key = request->key;
			} else {
				key = tuple_extract_key(old_tuple,
							vy_index_key_def(index->db),
							NULL);
			}
			part_count = mp_decode_array(&key);
			if (vy_delete(tx, index->db, key, part_count))
				diag_raise();
		}
		vinyl_insert_one(index, request->tuple, request->tuple_end,
				 request->space_id, tx);
	}
	if (old_tuple)
		tuple_ref(old_tuple);
	stmt->old_tuple = old_tuple;
}

/*
 * Four cases:
 *  - insert in one index
 *  - insert in multiple indexes
 *  - replace in one index
 *  - replace in multiple indexes.
 */
struct tuple *
VinylSpace::executeReplace(struct txn *txn, struct space *space,
			   struct request *request)
{
	assert(request->index_id == 0);

	/* Check the tuple fields. */
	tuple_validate_raw(space->format, request->tuple);
	struct vy_tx *tx = (struct vy_tx *)(txn->engine_tx);
	VinylEngine *engine = (VinylEngine *)space->handler->engine;
	struct txn_stmt *stmt = txn_current_stmt(txn);

	if (request->type == IPROTO_INSERT && engine->recovery_complete) {
		vinyl_insert_all(space, request, tx);
	} else {
		if (space->index_count == 1) {
			/* Replace in a space with a single index. */
			vinyl_replace_one(space, request, tx, stmt);
		} else {
			/* Replace in a space with secondary keys. */
			vinyl_replace_all(space, request, tx, stmt);
		}
	}

	struct tuple *new_tuple = tuple_new(space->format, request->tuple,
					    request->tuple_end);
	/* GC the new tuple if there is an exception below. */
	TupleRef ref(new_tuple);
	tuple_ref(new_tuple);
	stmt->new_tuple = new_tuple;
	return tuple_bless(new_tuple);
}

struct tuple *
VinylSpace::executeDelete(struct txn *txn, struct space *space,
                          struct request *request)
{
	VinylIndex *index = (VinylIndex *)index_find_unique(space,
							    request->index_id);

	/* Find full tuple in the index. */
	const char *key = request->key;
	uint32_t part_count = mp_decode_array(&key);
	primary_key_validate(index->key_def, key, part_count);
	struct txn_stmt *stmt = txn_current_stmt(txn);

	struct tuple *old_tuple = NULL;
	struct vy_tx *tx = (struct vy_tx *)(txn->engine_tx);
	/**
	 * If there are more than one index then need to get old_tuple
	 * for extracting keys from it for all indexes.
	 * Also the old_tuple is need if the space has triggers and need
	 * to pass the old_tuple to this triggers.
	 */
	if (space->index_count > 1 || !rlist_empty(&space->on_replace)) {
		if (vy_get(tx, index->db, key, part_count, &old_tuple))
			diag_raise();
	}
	if (space->index_count > 1) {
		if (old_tuple)
			vinyl_delete_all(space, old_tuple, request, tx);
	} else {
		if (vy_delete(tx, index->db, key, part_count))
			diag_raise();
	}
	if (old_tuple)
		tuple_ref(old_tuple);
	stmt->old_tuple = old_tuple;
	return NULL;
}

struct tuple *
VinylSpace::executeUpdate(struct txn *txn, struct space *space,
                          struct request *request)
{
	uint32_t index_id = request->index_id;
	/* Find full tuple in the index. */
	struct tuple *old_tuple = NULL;
	VinylIndex *index = (VinylIndex *)index_find_unique(space, index_id);
	struct vy_tx *tx = (struct vy_tx *)(txn->engine_tx);
	const char *key = request->key;
	uint32_t part_count = mp_decode_array(&key);
	primary_key_validate(index->key_def, key, part_count);
	struct txn_stmt *stmt = txn_current_stmt(txn);

	if (vy_get(tx, index->db, key, part_count, &old_tuple))
		diag_raise();
	if (old_tuple == NULL)
		return NULL;

	TupleRef old_ref(old_tuple);
	struct tuple *new_tuple;
	new_tuple = tuple_update(space->format, region_aligned_alloc_xc_cb,
				 &fiber()->gc, old_tuple, request->tuple,
				 request->tuple_end, request->index_base);
	TupleRef new_ref(new_tuple);
	space_check_update(space, old_tuple, new_tuple);

	/* Tuple doesn't exist so it can be inserted. */

	index = (VinylIndex *)space->index[0];
	if (vy_replace(tx, index->db, new_tuple->data,
			  new_tuple->data + new_tuple->bsize))
		diag_raise();

	/* Update secondary keys, avoid duplicates. */
	uint32_t key_size;
	for (uint32_t iid = 1; iid < space->index_count; ++iid) {
		index = (VinylIndex *) space->index[iid];
		if (iid != index_id) {
			key = tuple_extract_key(old_tuple, index->key_def,
						&key_size);
		} else {
			key = request->key;
		}
		key = tuple_extract_key(old_tuple, vy_index_key_def(index->db),
					&key_size);
		part_count = mp_decode_array(&key);
		/**
		 * Delete goes first, so if old and new keys
		 * fully match, there is no look up beyond the
		 * transaction index.
		 */
		if (vy_delete(tx, index->db, key, part_count))
			diag_raise();
		vinyl_insert_one(index, new_tuple->data,
				 new_tuple->data + new_tuple->bsize,
				 request->space_id, tx);
	}
	if (old_tuple)
		tuple_ref(old_tuple);
	tuple_ref(new_tuple);
	stmt->old_tuple = old_tuple;
	stmt->new_tuple = new_tuple;
	return tuple_bless(new_tuple);
}

void
VinylSpace::executeUpsert(struct txn *txn, struct space *space,
                           struct request *request)
{
	assert(request->index_id == 0);
	VinylIndex *index;
	index = (VinylIndex *)index_find_unique(space, request->index_id);

	/* Check tuple fields. */
	tuple_validate_raw(space->format, request->tuple);

	struct vy_tx *tx = (struct vy_tx *)(txn->engine_tx);
	tuple_update_check_ops(region_aligned_alloc_xc_cb, &fiber()->gc,
			       request->ops, request->ops_end,
			       request->index_base);
	for (uint32_t i = 0; i < space->index_count; ++i) {
		index = (VinylIndex *)space->index[i];
		if (vy_upsert(tx, index->db, request->tuple,
				 request->tuple_end, request->ops,
				 request->ops_end, request->index_base) < 0) {
			diag_raise();
		}
	}
}
