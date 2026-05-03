#pragma once

#include <clock/clock.h>
#include <stdbool.h>

/* !!! NOTE: Nothing in this file is currently thread safe! !!! */

// Maximum number of timeouts to handle simultaneously
#define CONC_MAX_TIMEOUTS 255

typedef struct id_obj {
    uint32_t id; // Unique reusable id, id 0 is reserved as an error return value from insertion
    struct id_obj *next; // Next id in list.
} id_obj;

typedef struct id {
    id_obj *head; // Head to linked list of ids.
} free_ids;

typedef struct timeout_obj {
    timestamp_t expiry; // TIMER_E timestamp + delay
    uint32_t id; // Unique reusable id, id 0 is reserved as an error return value from insertion.
    timer_callback_t callback; // Callback to call for this timeout.
    void *data; // Passed to callback function for this specific timeout.
    struct timeout_obj *next; // Next timeout in list.
} timeout_obj;

typedef struct timeouts {
    timeout_obj *head; // Head to sorted linked list of timeouts.
    free_ids free_ids; // Id object to use.
    int reset;
    uint64_t num_timeouts;
} timeouts;

/*
 * Initialise timeouts object. O(n).
 */
void *init_timeouts(timeouts *timeouts);

/*
 * Reset timeouts object and free all timeouts contained within it. O(n).
 */
void *destroy_timeouts(timeouts *timeouts);

/*
 * Sorted insert of new timeout_obj into timeouts. O(n).
 *
 * @return              0 on failure, unique ID otherwise.
 */
uint32_t insert_timeout(timeouts *timeouts, timestamp_t expiry, timer_callback_t callback, void *data);

/*
 * Remove and free timeout_obj by id from timeouts. NOTE: O(n).
 *
 * @return             0 on success, 1 otherwise.
 */
int remove_timeout(timeouts *timeouts, uint32_t id, bool signal);

/*
 * Remove and run all timeouts that expire at or before curr_time.
 */
void consume_timeouts(timeouts *timeouts, timestamp_t curr_time);

/*
* Return expiry of head of list or max timestamp if no head.
*/
timestamp_t peek_expiry(timeouts *timeouts);

/*
* Get id of timeout at index i. NULL if no timeout at that index.
* Used for testing.
*/
uint32_t *fetch_timeout(timeouts *timeouts, size_t i);

///////////////////////////////////////////////////////////////////
/*          Interface for Linked List of reusable ID's           */

int init_ids(free_ids *free_ids);

void *destroy_ids(free_ids *timeouts);

/*
 * Inserts "id" to end of linked list once ID is no longer in use, i.e. free
 *
 * @return             0 on success, 1 otherwise.
 */
int insert_id(free_ids *ids, uint32_t id);

uint32_t pop_id(free_ids *ids);