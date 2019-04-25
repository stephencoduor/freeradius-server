/*
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/**
 * $Id$
 *
 * @brief Multi-packet state handling
 * @file src/lib/server/state.c
 *
 * @ingroup AVP
 *
 * For each round of a multi-round authentication method such as EAP,
 * or a 2FA method such as OTP, a state entry will be created.  The state
 * entry holds data that should be available during the complete lifecycle
 * of the authentication attempt.
 *
 * When a request is complete, #fr_request_to_state is called to transfer
 * ownership of the state VALUE_PAIRs and state_ctx (which the VALUE_PAIRs
 * are allocated in) to a #fr_state_entry_t.  This #fr_state_entry_t holds the
 * value of the State attribute, that will be send out in the response.
 *
 * When the next request is received, #fr_state_to_request is called to transfer
 * the VALUE_PAIRs and state ctx to the new request.
 *
 * The ownership of the state_ctx and state VALUE_PAIRs is transferred as below:
 *
 * @verbatim
   request -> state_entry -> request -> state_entry -> request -> free()
          \-> reply                 \-> reply                 \-> access-reject/access-accept
 * @endverbatim
 *
 * @copyright 2014 The FreeRADIUS server project
 */
RCSID("$Id$")

#include <freeradius-devel/server/base.h>
#include <freeradius-devel/server/state.h>
#include <freeradius-devel/server/rad_assert.h>

#include <freeradius-devel/util/dlist.h>
#include <freeradius-devel/util/md5.h>
#include <freeradius-devel/util/misc.h>
#include <freeradius-devel/util/rand.h>

/** Holds a state value, and associated VALUE_PAIRs and data
 *
 */
typedef struct {
	uint64_t		id;				//!< State number within state tree.
	union {
		/** Server ID components
		 *
		 * State values should be unique to a given server
		 */
		struct state_comp {
			uint8_t		tries;			//!< Number of rounds so far in this state sequence.
			uint8_t		tx;			//!< Bits changed in the tries counter for this round.
			uint8_t		r_0;			//!< Random component.
			uint8_t		server_id;		//!< Configured server ID.  Used for debugging
								//!< to locate authentication sessions originating
								//!< from a particular backend authentication server.

			uint32_t	server_hash;		//!< Hash of the current virtual server, xor'd with
								//!< r1, r2, r3, r4 after the original state value
								//!< is sent, but before the state entry is inserted
								//!< into the tree.  The receiving virtual server
								//!< xor's its hash with the received state before
								//!< performing the lookup.  This means one virtual
								//!< server can't act on a state entry generated by
								//!< another, even though the state tree is global
								//!< to all virtual servers.

			uint8_t		vx_0;			//!< Random component.
			uint8_t		r_5;			//!< Random component.
			uint8_t		vx_1;			//!< Random component.
			uint8_t		r_6;			//!< Random component.

			uint8_t		vx_2;			//!< Random component.
			uint8_t		r_7;			//!< Random component.
			uint8_t		r_8;			//!< Random component.
			uint8_t		r_9;			//!< Random component.
		} state_comp;

		uint8_t		state[sizeof(struct state_comp)];	//!< State value in binary.
	};

	uint64_t		seq_start;			//!< Number of first request in this sequence.
	time_t			cleanup;			//!< When this entry should be cleaned up.
	fr_dlist_t		list;				//!< Entry in the list of things to expire.

	int			tries;

	TALLOC_CTX		*ctx;				//!< ctx to parent any data that needs to be
								//!< tied to the lifetime of the request progression.
	VALUE_PAIR		*vps;				//!< session-state VALUE_PAIRs, parented by ctx.

	fr_dlist_head_t		data;				//!< Persistable request data, also parented by ctx.

	REQUEST			*thawed;			//!< The request that thawed this entry.
} fr_state_entry_t;

struct fr_state_tree_t {
	uint64_t		id;				//!< Next ID to assign.
	uint64_t		timed_out;			//!< Number of states that were cleaned up due to
								//!< timeout.
	uint32_t		max_sessions;			//!< Maximum number of sessions we track.
	rbtree_t		*tree;				//!< rbtree used to lookup state value.
	fr_dlist_head_t		to_expire;			//!< Linked list of entries to free.

	uint32_t		timeout;			//!< How long to wait before cleaning up state entires.

	bool			thread_safe;			//!< Whether we lock the tree whilst modifying it.
	pthread_mutex_t		mutex;				//!< Synchronisation mutex.

	uint8_t			server_id;			//!< ID to use for load balancing.

	fr_dict_attr_t const	*da;				//!< State attribute used.
};

#define PTHREAD_MUTEX_LOCK if (state->thread_safe) pthread_mutex_lock
#define PTHREAD_MUTEX_UNLOCK if (state->thread_safe) pthread_mutex_unlock

static void state_entry_unlink(fr_state_tree_t *state, fr_state_entry_t *entry);

/** Compare two fr_state_entry_t based on their state value i.e. the value of the attribute
 *
 */
static int state_entry_cmp(void const *one, void const *two)
{
	fr_state_entry_t const *a = one, *b = two;

	return memcmp(a->state, b->state, sizeof(a->state));
}

/** Free the state tree
 *
 */
static int _state_tree_free(fr_state_tree_t *state)
{
	fr_state_entry_t *entry;

	if (state->thread_safe) pthread_mutex_destroy(&state->mutex);

	DEBUG4("Freeing state tree %p", state);

	while ((entry = fr_dlist_head(&state->to_expire))) {
		DEBUG4("Freeing state entry %p (%"PRIu64")", entry, entry->id);
		state_entry_unlink(state, entry);
		talloc_free(entry);
	}

	/*
	 *	Free the rbtree
	 */
	talloc_free(state->tree);

	return 0;
}

/** Initialise a new state tree
 *
 * @param[in] ctx		to link the lifecycle of the state tree to.
 * @param[in] da		Attribute used to store and retrieve state from.
 * @param[in] thread_safe		Whether we should mutex protect the state tree.
 * @param[in] max_sessions	we track state for.
 * @param[in] timeout		How long to wait before cleaning up entries.
 * @param[in] server_id		ID byte to use in load-balancing operations.
 * @return
 *	- A new state tree.
 *	- NULL on failure.
 */
fr_state_tree_t *fr_state_tree_init(TALLOC_CTX *ctx, fr_dict_attr_t const *da, bool thread_safe,
				    uint32_t max_sessions, uint32_t timeout, uint8_t server_id)
{
	fr_state_tree_t *state;

	state = talloc_zero(NULL, fr_state_tree_t);
	if (!state) return 0;

	state->max_sessions = max_sessions;
	state->timeout = timeout;

	/*
	 *	Create a break in the contexts.
	 *	We still want this to be freed at the same time
	 *	as the parent, but we also need it to be thread
	 *	safe, and multiple threads could be using the
	 *	tree.
	 */
	talloc_link_ctx(ctx, state);

	if (thread_safe && (pthread_mutex_init(&state->mutex, NULL) != 0)) {
		talloc_free(state);
		return NULL;
	}

	fr_dlist_talloc_init(&state->to_expire, fr_state_entry_t, list);

	/*
	 *	We need to do controlled freeing of the
	 *	rbtree, so that all the state entries
	 *	are freed before it's destroyed.  Hence
	 *	it being parented from the NULL ctx.
	 */
	state->tree = rbtree_talloc_create(NULL, state_entry_cmp, fr_state_entry_t, NULL, 0);
	if (!state->tree) {
		talloc_free(state);
		return NULL;
	}
	talloc_set_destructor(state, _state_tree_free);

	state->da = da;		/* Remember which attribute we use to load/store state */
	state->server_id = server_id;
	state->thread_safe = thread_safe;

	return state;
}

/** Unlink an entry and remove if from the tree
 *
 */
static void state_entry_unlink(fr_state_tree_t *state, fr_state_entry_t *entry)
{
	/*
	 *	Check the memory is still valid
	 */
	(void) talloc_get_type_abort(entry, fr_state_entry_t);

	fr_dlist_remove(&state->to_expire, entry);

	rbtree_deletebydata(state->tree, entry);

	DEBUG4("State ID %" PRIu64 " unlinked", entry->id);
}

/** Frees any data associated with a state
 *
 */
static int _state_entry_free(fr_state_entry_t *entry)
{
#ifdef WITH_VERIFY_PTR
	fr_cursor_t cursor;
	VALUE_PAIR *vp;

	/*
	 *	Verify all state attributes are parented
	 *	by the state context.
	 */
	if (entry->ctx) {
		for (vp = fr_cursor_init(&cursor, &entry->vps);
		     vp;
		     vp = fr_cursor_next(&cursor)) {
			rad_assert(entry->ctx == talloc_parent(vp));
		}
	}

	/*
	 *	Ensure any request data is parented by us
	 *	so we know it'll be cleaned up.
	 */
	(void)fr_cond_assert(request_data_verify_parent(entry->ctx, &entry->data));
#endif

	/*
	 *	Should also free any state attributes
	 */
	if (entry->ctx) TALLOC_FREE(entry->ctx);

	DEBUG4("State ID %" PRIu64 " freed", entry->id);

	return 0;
}

/** Create a new state entry
 *
 * @note Called with the mutex held.
 */
static fr_state_entry_t *state_entry_create(fr_state_tree_t *state, REQUEST *request,
					    RADIUS_PACKET *packet, fr_state_entry_t *old)
{
	size_t			i;
	uint32_t		x;
	time_t			now = time(NULL);
	VALUE_PAIR		*vp;
	fr_state_entry_t	*entry, *next;

	uint8_t			old_state[sizeof(old->state)];
	int			old_tries = 0;
	uint64_t		timed_out = 0;
	bool			too_many = false;
	fr_dlist_head_t		to_free;

	fr_dlist_init(&to_free, fr_state_entry_t, list);

	/*
	 *	Clean up old entries.
	 */
	for (entry = fr_dlist_head(&state->to_expire);
	     entry != NULL;
	     entry = next) {
		(void)talloc_get_type_abort(entry, fr_state_entry_t);	/* Allow examination */
		next = fr_dlist_next(&state->to_expire, entry);		/* Advance *before* potential unlinking */

		if (entry == old) continue;

		/*
		 *	Too old, we can delete it.
		 */
		if (entry->cleanup < now) {
			state_entry_unlink(state, entry);
			fr_dlist_insert_tail(&to_free, entry);
			timed_out++;
			continue;
		}

		break;
	}

	state->timed_out += timed_out;

	if (!old && (rbtree_num_elements(state->tree) >= (uint32_t) state->max_sessions)) too_many = true;

	/*
	 *	Record the information from the old state, we may base the
	 *	new state off the old one.
	 *
	 *	Once we release the mutex, the state of old becomes indeterminate
	 *	so we have to grab the values now.
	 */
	if (old) {
		old_tries = old->tries;

		memcpy(old_state, old->state, sizeof(old_state));

		/*
		 *	The old one isn't used any more, so we can free it.
		 */
		if (fr_dlist_empty(&old->data)) {
			state_entry_unlink(state, old);
			fr_dlist_insert_tail(&to_free, old);
		}
	}
	PTHREAD_MUTEX_UNLOCK(&state->mutex);

	if (timed_out > 0) RWDEBUG("Cleaning up %"PRIu64" timed out state entries", timed_out);

	/*
	 *	Now free the unlinked entries.
	 *
	 *	We do it here as freeing may involve significantly more
	 *	work than just freeing the data.
	 *
	 *	If there's request data that was persisted it will now
	 *	be freed also, and it may have complex destructors associated
	 *	with it.
	 */
	while ((entry = fr_dlist_head(&to_free)) != NULL) {
		fr_dlist_remove(&to_free, entry);
		talloc_free(entry);
	}

	/*
	 *	Have to do this post-cleanup, else we end up returning with
	 *	a list full of entries to free with none of them being
	 *	freed which is bad...
	 */
	if (too_many) {
		RERROR("Failed inserting state entry - At maximum ongoing session limit (%u)",
		       state->max_sessions);
		return NULL;
	}

	/*
	 *	Allocation doesn't need to occur inside the critical region
	 *	and would add significantly to contention.
	 */
	entry = talloc_zero(NULL, fr_state_entry_t);
	if (!entry) {
		PTHREAD_MUTEX_LOCK(&state->mutex);	/* Caller expects this to be locked */
		return NULL;
	}

	request_data_list_init(&entry->data);
	talloc_set_destructor(entry, _state_entry_free);
	entry->id = state->id++;

	/*
	 *	Limit the lifetime of this entry based on how long the
	 *	server takes to process a request.  Doing it this way
	 *	isn't perfect, but it's reasonable, and it's one less
	 *	thing for an administrator to configure.
	 */
	entry->cleanup = now + state->timeout;

	/*
	 *	Some modules create their own magic
	 *	state attributes.  If a state value already exists
	 *	int the reply, we use that in preference to the
	 *	old state.
	 */
	vp = fr_pair_find_by_da(packet->vps, state->da, TAG_ANY);
	if (vp) {
		if (DEBUG_ENABLED && (vp->vp_length > sizeof(entry->state))) {
			WARN("State too long, will be truncated.  Expected <= %zd bytes, got %zu bytes",
			     sizeof(entry->state), vp->vp_length);
		}

		/*
		 *	Assume our own State first.
		 */
		if (vp->vp_length == sizeof(entry->state)) {
			memcpy(entry->state, vp->vp_octets, sizeof(entry->state));

		/*
		 *	Too big?  Get the MD5 hash, in order
		 *	to depend on the entire contents of State.
		 */
		} else if (vp->vp_length > sizeof(entry->state)) {
			fr_md5_calc(entry->state, vp->vp_octets, vp->vp_length);

			/*
			 *	Too small?  Use the whole thing, and
			 *	set the rest of my_entry.state to zero.
			 */
		} else {
			memcpy(entry->state, vp->vp_octets, vp->vp_length);
			memset(&entry->state[vp->vp_length], 0, sizeof(entry->state) - vp->vp_length);
		}
	} else {
		/*
		 *	16 octets of randomness should be enough to
		 *	have a globally unique state.
		 */
		if (old) {
			memcpy(entry->state, old_state, sizeof(entry->state));
			entry->tries = old_tries + 1;
		/*
		 *	Base the new state on the old state if we had one.
		 */
		} else {
			for (i = 0; i < sizeof(entry->state) / sizeof(x); i++) {
				x = fr_rand();
				memcpy(entry->state + (i * 4), &x, sizeof(x));
			}
		}

		entry->state_comp.tries = entry->tries + 1;

		entry->state_comp.tx = entry->state_comp.tries ^ entry->tries;

		entry->state_comp.vx_0 = entry->state_comp.r_0 ^
					 ((((uint32_t) HEXIFY(RADIUSD_VERSION)) >> 16) & 0xff);
		entry->state_comp.vx_1 = entry->state_comp.r_0 ^
					 ((((uint32_t) HEXIFY(RADIUSD_VERSION)) >> 8) & 0xff);
		entry->state_comp.vx_2 = entry->state_comp.r_0 ^
					 (((uint32_t) HEXIFY(RADIUSD_VERSION)) & 0xff);

		/*
		 *	Allow a portion of the State attribute to be set,
		 *	this is useful for debugging purposes.
		 */
		entry->state_comp.server_id = state->server_id;

		vp = fr_pair_afrom_da(packet, state->da);
		fr_pair_value_memcpy(vp, entry->state, sizeof(entry->state));
		fr_pair_add(&packet->vps, vp);
	}

	DEBUG4("State ID %" PRIu64 " created, value 0x%pH, expires %" PRIu64 "s",
	       entry->id, fr_box_octets(entry->state, sizeof(entry->state)), (uint64_t)entry->cleanup - now);

	PTHREAD_MUTEX_LOCK(&state->mutex);

	/*
	 *	XOR the server hash with four bytes of random data.
	 *	We XOR is again before resolving, to ensure state lookups
	 *	only succeed in the virtual server that created the state
	 *	value.
	 */
	*((uint32_t *)(&entry->state_comp.server_hash)) ^= fr_hash_string(cf_section_name2(request->server_cs));

	if (!rbtree_insert(state->tree, entry)) {
		RERROR("Failed inserting state entry - Insertion into state tree failed");
		fr_pair_delete_by_da(&packet->vps, state->da);
		talloc_free(entry);
		return NULL;
	}

	/*
	 *	Link it to the end of the list, which is implicitely
	 *	ordered by cleanup time.
	 */
	fr_dlist_insert_tail(&state->to_expire, entry);

	return entry;
}

/** Find the entry, based on the State attribute
 *
 */
static fr_state_entry_t *state_entry_find(fr_state_tree_t *state, REQUEST *request, fr_value_box_t const *vb)
{
	fr_state_entry_t *entry, my_entry;

	/*
	 *	Assume our own State first.
	 */
	if (vb->vb_length == sizeof(my_entry.state)) {
		memcpy(my_entry.state, vb->vb_octets, sizeof(my_entry.state));

		/*
		 *	Too big?  Get the MD5 hash, in order
		 *	to depend on the entire contents of State.
		 */
	} else if (vb->vb_length > sizeof(my_entry.state)) {
		fr_md5_calc(my_entry.state, vb->vb_octets, vb->vb_length);

		/*
		 *	Too small?  Use the whole thing, and
		 *	set the rest of my_entry.state to zero.
		 */
	} else {
		memcpy(my_entry.state, vb->vb_octets, vb->vb_length);
		memset(&my_entry.state[vb->vb_length], 0, sizeof(my_entry.state) - vb->vb_length);
	}

	/*
	 *	Make it unique for different virtual servers handling the same request
	 */
	my_entry.state_comp.server_hash ^= fr_hash_string(cf_section_name2(request->server_cs));

	entry = rbtree_finddata(state->tree, &my_entry);

	if (entry) (void) talloc_get_type_abort(entry, fr_state_entry_t);

	return entry;
}

/** Called when sending an Access-Accept/Access-Reject to discard state information
 *
 */
void fr_state_discard(fr_state_tree_t *state, REQUEST *request)
{
	fr_state_entry_t	*entry;
	VALUE_PAIR		*vp;

	vp = fr_pair_find_by_da(request->packet->vps, state->da, TAG_ANY);
	if (!vp) return;

	PTHREAD_MUTEX_LOCK(&state->mutex);
	entry = state_entry_find(state, request, &vp->data);
	if (!entry) {
		PTHREAD_MUTEX_UNLOCK(&state->mutex);
		return;
	}
	state_entry_unlink(state, entry);
	PTHREAD_MUTEX_UNLOCK(&state->mutex);

	/*
	 *	If fr_state_to_request was never called, this ensures
	 *	the state owned by entry is freed, otherwise this is
	 *	mostly a NOOP, other than freeing the memory held by
	 *	the entry.
	 */
	TALLOC_FREE(entry);

	/*
	 *	If fr_state_to_request was called, then the request
	 *	holds the state data, and we need to destroy it
	 *	and return the request to the state it was in when
	 *	it was first alloced, just in case a user does something
	 *	stupid like add additional session-state attributes
	 *	in  one of the later sections.
	 */
	TALLOC_FREE(request->state_ctx);
	request->state = NULL;

	MEM(request->state_ctx = talloc_init("session-state"));

	RDEBUG3("RADIUS State - discarded");

	return;
}

/** Copy a pointer to the head of the list of state VALUE_PAIRs (and their ctx) into the request
 *
 * @note Does not copy the actual VALUE_PAIRs.  The VALUE_PAIRs and their context
 *	are transferred between state entries as the conversation progresses.
 *
 * @note Called with the mutex free.
 */
void fr_state_to_request(fr_state_tree_t *state, REQUEST *request)
{
	fr_state_entry_t	*entry;
	TALLOC_CTX		*old_ctx = NULL;
	VALUE_PAIR		*vp;

	rad_assert(request->state == NULL);

	/*
	 *	No State, don't do anything.
	 */
	vp = fr_pair_find_by_da(request->packet->vps, state->da, TAG_ANY);
	if (!vp) {
		RDEBUG3("No &request:State attribute, can't restore &session-state");
		if (request->seq_start == 0) request->seq_start = request->number;	/* Need check for fake requests */
		return;
	}

	PTHREAD_MUTEX_LOCK(&state->mutex);
	entry = state_entry_find(state, request, &vp->data);
	if (entry) {
		(void)talloc_get_type_abort(entry, fr_state_entry_t);
		if (entry->thawed) {
			REDEBUG("State entry has already been thawed by a request %"PRIu64, entry->thawed->number);
			PTHREAD_MUTEX_UNLOCK(&state->mutex);
			return;
		}
		if (request->state_ctx) old_ctx = request->state_ctx;	/* Store for later freeing */

		rad_assert(entry->ctx);

		request->seq_start = entry->seq_start;
		request->state_ctx = entry->ctx;
		request->state = entry->vps;
		request_data_restore(request, &entry->data);

		entry->ctx = NULL;
		entry->vps = NULL;
		entry->thawed = request;
	}
	PTHREAD_MUTEX_UNLOCK(&state->mutex);

	if (request->state) {
		RDEBUG2("Restored &session-state");
		log_request_pair_list(L_DBG_LVL_2, request, request->state, "&session-state:");
	}

	/*
	 *	Free this outside of the mutex for less contention.
	 */
	if (old_ctx) talloc_free(old_ctx);

	RDEBUG3("RADIUS State - restored");

	REQUEST_VERIFY(request);
	return;
}


/** Transfer ownership of the state VALUE_PAIRs and ctx, back to a state entry
 *
 * Put request->state into the State attribute.  Put the State attribute
 * into the vps list.  Delete the original entry, if it exists
 *
 * Also creates a new state entry.
 */
int fr_request_to_state(fr_state_tree_t *state, REQUEST *request)
{
	fr_state_entry_t	*entry, *old = NULL;
	fr_dlist_head_t		data;
	VALUE_PAIR		*vp;

	request_data_list_init(&data);
	request_data_by_persistance(&data, request, true);

	if (!request->state && fr_dlist_empty(&data)) return 0;

	if (request->state) {
		RDEBUG2("Saving &session-state");
		log_request_pair_list(L_DBG_LVL_2, request, request->state, "&session-state:");
	}

	vp = fr_pair_find_by_da(request->packet->vps, state->da, TAG_ANY);

	PTHREAD_MUTEX_LOCK(&state->mutex);
	if (vp) old = state_entry_find(state, request, &vp->data);

	entry = state_entry_create(state, request, request->reply, old);
	if (!entry) {
		PTHREAD_MUTEX_UNLOCK(&state->mutex);
		RERROR("Creating state entry failed");
		request_data_restore(request, &data);	/* Put it back again */
		return -1;
	}

	rad_assert(entry->ctx == NULL);
	rad_assert(request->state_ctx);

	entry->seq_start = request->seq_start;
	entry->ctx = request->state_ctx;
	entry->vps = request->state;
	fr_dlist_move(&entry->data, &data);

	request->state_ctx = NULL;
	request->state = NULL;

	PTHREAD_MUTEX_UNLOCK(&state->mutex);

	RDEBUG3("RADIUS State - saved");
	REQUEST_VERIFY(request);

	return 0;
}

/** Store subrequest's session-state list and persistable request data in its parent
 *
 * @param[in] request		The child request to retrieve state from.
 * @param[in] unique_ptr	A parent may have multiple subrequests spawned
 *				by different modules.  This identifies the module
 *      			or other facility that spawned the subrequest.
 * @param[in] unique_int	Further identification.
 */
void fr_state_store_in_parent(REQUEST *request, void *unique_ptr, int unique_int)
{
	if (unlikely(request->parent == NULL)) return;

	RDEBUG3("Subrequest state - saved to %s", request->parent->name);

	/*
	 *	Shove this into the child to make
	 *	it easier to store/restore the
	 *	whole lot...
	 */
	request_data_add(request, (void *)fr_state_store_in_parent, 0, request->state, true, false, true);
	request->state = NULL;

	request_data_store_in_parent(request, unique_ptr, unique_int);
}

/** Restore subrequest data from a parent request
 *
 * @param[in] request		The child request to restore state to.
 * @param[in] unique_ptr	A parent may have multiple subrequests spawned
 *				by different modules.  This identifies the module
 *      			or other facility that spawned the subrequest.
 * @param[in] unique_int	Further identification.
 */
void fr_state_restore_to_child(REQUEST *request, void *unique_ptr, int unique_int)
{
	if (unlikely(request->parent == NULL)) return;

	RDEBUG3("Subrequest state - restored from %s", request->parent->name);

	request_data_restore_to_child(request, unique_ptr, unique_int);

	/*
	 *	Get the state vps back
	 */
	request->state = request_data_get(request, (void *)fr_state_store_in_parent, 0);
}

/** Move all request data and session-state VPs into a new state_ctx
 *
 * If we don't do this on detach, session-state VPs and persistable
 * request data will be freed when the parent's state_ctx is freed.
 * If the parent was freed before the child, we'd get all kinds of
 * use after free nastiness.
 *
 * @param[in] fake		request to detach.
 * @param[in] will_free		Caller super pinky swears to free
 *				the request ASAP, and that it wont
 *				touch persistable request data,
 *				request->state_ctx or request->state.
 */
void fr_state_detach(REQUEST *request, bool will_free)
{
	VALUE_PAIR	*new_state = NULL;
	TALLOC_CTX	*new_state_ctx;

	if (unlikely(request->parent == NULL)) return;

	if (will_free) {
		fr_pair_list_free(&request->state);

		/*
		 *	The non-persistable stuff is
		 *	prented directly by the request
		 */
		request_data_persistable_free(request);

		/*
		 *	Parent will take care of freeing
		 *	honestly this should probably
		 *	be an assert.
		 */
		if (request->state_ctx == request->parent->state_ctx) request->state_ctx = NULL;
		return;
	}

	MEM(new_state_ctx = talloc_init("session-state"));
	request_data_ctx_change(new_state_ctx, request);

	fr_pair_list_copy(new_state_ctx, &new_state, request->state);
	fr_pair_list_free(&request->state);

	request->state = new_state;

	/*
	 *	...again, should probably
	 *	not happen and should probably
	 *	be an assert.
	 */
	if (request->state_ctx != request->parent->state_ctx) talloc_free(request->state_ctx);
	request->state_ctx = new_state;
}

/** Return number of entries created
 *
 */
uint64_t fr_state_entries_created(fr_state_tree_t *state)
{
	return state->id;
}

/** Return number of entries that timed out
 *
 */
uint64_t fr_state_entries_timeout(fr_state_tree_t *state)
{
	return state->timed_out;
}

/** Return number of entries we're currently tracking
 *
 */
uint32_t fr_state_entries_tracked(fr_state_tree_t *state)
{
	return (uint32_t)rbtree_num_elements(state->tree);
}
