#ifndef ACTION_STATUS_H
#define ACTION_STATUS_H

/* should probably be used in other places (decoder/input buffering), too */
enum action_status {
	/* all mutexes for conditions are unlocked and caller signaled */
	AS_COMPLETE = 0,

	/* mutexes are locked and caller has not been signaled, yet */
	AS_INPROGRESS,

	/* mutexes are unlocked and caller has not been signaled */
	AS_DEFERRED
};

#endif /* !ACTION_STATUS_H */
