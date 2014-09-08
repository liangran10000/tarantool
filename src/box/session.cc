/*
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
#include "session.h"
#include "fiber.h"
#include "memory.h"

#include "assoc.h"
#include "trigger.h"
#include "exception.h"
#include "random.h"
#include <sys/socket.h>
#include "box/txn.h"

static struct mh_i32ptr_t *session_registry;

struct mempool session_pool;

RLIST_HEAD(session_on_connect);
RLIST_HEAD(session_on_disconnect);

static inline  uint32_t
sid_max()
{
	static uint32_t sid_max = 0;
	/* Return the next sid rolling over the reserved value of 0. */
	while (++sid_max == 0)
		;
	return sid_max;
}

static void
session_on_stop(struct trigger * trigger, void *event)
{
	(void) event;
	/* Remove on_stop trigger from fiber */
	trigger_clear(trigger);
	struct session *session = fiber_get_session(fiber());
	if (session == NULL)
		return;
	/* Destroy session */
	session_destroy(session);
	fiber_set_session(fiber(), NULL);
}

struct session *
session_create(int fd, uint64_t cookie)
{
	struct session *session = (struct session *)
		mempool_alloc(&session_pool);
	session->id = sid_max();
	session->fd =  fd;
	session->cookie = cookie;
	session->txn = NULL;
	session->fiber_on_stop = {
		rlist_nil, session_on_stop, NULL, NULL
	};
	session_set_user(session, ADMIN, ADMIN);
	random_bytes(session->salt, SESSION_SEED_SIZE);
	struct mh_i32ptr_node_t node;
	node.key = session->id;
	node.val = session;

	mh_int_t k = mh_i32ptr_put(session_registry, &node, NULL, NULL);

	if (k == mh_end(session_registry)) {
		mempool_free(&session_pool, session);
		tnt_raise(ClientError, ER_MEMORY_ISSUE,
			  "session hash", "new session");
	}
	return session;
}

void
session_run_on_disconnect_triggers(struct session *session)
{
	/* For triggers. */
	session_set_user(session, ADMIN, ADMIN);
	try {
		trigger_run(&session_on_disconnect, NULL);
	} catch (Exception *e) {
		e->log();
	} catch (...) {
		/* catch all. */
	}
	session_storage_cleanup(session->id);
}

void
session_run_on_connect_triggers(struct session *session)
{
	(void) session;
	trigger_run(&session_on_connect, NULL);
}

void
session_destroy(struct session *session)
{
	if (session->txn) {
		assert(session->txn == in_txn());
		txn_rollback();
	}
	assert(session->txn == NULL);
	struct mh_i32ptr_node_t node = { session->id, NULL };
	mh_i32ptr_remove(session_registry, &node, NULL);
	mempool_free(&session_pool, session);
}

struct session *
session_find(uint32_t sid)
{
	mh_int_t k = mh_i32ptr_find(session_registry, sid, NULL);
	if (k == mh_end(session_registry))
		return NULL;
	return (struct session *)
		mh_i32ptr_node(session_registry, k)->val;
}

void
session_init()
{
	session_registry = mh_i32ptr_new();
	if (session_registry == NULL)
		panic("out of memory");
	mempool_create(&session_pool, &cord()->slabc, sizeof(struct session));
}

void
session_free()
{
	if (session_registry)
		mh_i32ptr_delete(session_registry);
}

SessionGuard::SessionGuard(int fd, uint64_t cookie)
{
	session = session_create(fd, cookie);
	fiber_set_session(fiber(), session);
}

SessionGuard::~SessionGuard()
{
	assert(session == fiber_get_session(fiber()));
	session_destroy(session);
	fiber_set_session(fiber(), NULL);
}

SessionGuardWithTriggers::SessionGuardWithTriggers(int fd, uint64_t cookie)
	:SessionGuard(fd, cookie)
{
	session_run_on_connect_triggers(session);
}

SessionGuardWithTriggers::~SessionGuardWithTriggers()
{
	session_run_on_disconnect_triggers(session);
}