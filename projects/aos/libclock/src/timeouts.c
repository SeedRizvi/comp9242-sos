#include <stdlib.h>
#include <stdint.h>
#include <clock/timeouts.h>
#include <clock/timestamp.h>
#include <clock/device.h>

#include "../../sos/src/clock_sync.h"

static int num_timeouts = 0;

///////////////////////////////////////////////////////////////////
/*       Implementation for Sorted Linked List of timeouts              */

void *init_timeouts(timeouts *timeouts) {
    timeouts->head = NULL;
    init_ids(&(timeouts->free_ids));
    timeouts->reset = 0;
    timeouts->num_timeouts = 0;
}

void *destroy_timeouts(timeouts *timeouts) {
    clock_wait();
    timeout_obj *curr = NULL;
    for (curr = timeouts->head; curr != NULL;) {
        timeout_obj *next = curr->next;
        free(curr);
        curr = next;
    }
    timeouts->head = NULL;
    timeouts->reset = 1;
    timeouts->num_timeouts = 0;
    destroy_ids(&(timeouts->free_ids));
    clock_signal();
}

uint32_t insert_timeout(timeouts *timeouts, timestamp_t expiry, timer_callback_t callback, void *data) {
    clock_wait();

    num_timeouts++;
    //dprintf(2, "======== NUM TIMEOUTS ADDITION = %d ========\n", num_timeouts);

    timeout_obj *to = malloc(sizeof(timeout_obj));
    if (to == NULL || timeouts->num_timeouts == CONC_MAX_TIMEOUTS) {
        clock_signal();
        return 0;
    }
    to->expiry = expiry;
    to->callback = callback;
    to->data = data;
    to->next = NULL;
    to->id = pop_id(&(timeouts->free_ids));

    timeouts->num_timeouts++;

    if (timeouts->head == NULL) {
        timeouts->head = to;
        clock_signal();
        return to->id;
    }

    if (timeouts->head->expiry >= expiry) {
        to->next = timeouts->head;
        timeouts->head = to;
        clock_signal();
        return to->id;
    }

    timeout_obj *prev = timeouts->head;
    for (timeout_obj *curr = timeouts->head->next; curr != NULL; curr = curr->next) {
        if (curr->expiry >= expiry) {
            prev->next = to;
            to->next = curr;
            clock_signal();
            return to->id;
        }
        prev = curr;
    }

    prev->next = to;

    clock_signal();
    return to->id;
}

int remove_timeout(timeouts *timeouts, uint32_t id, bool block) {
    if (block) clock_wait();

    num_timeouts--;
    //dprintf(2, "======== NUM TIMEOUTS REMOVAL = %d ========\n", num_timeouts);

    if (timeouts->head == NULL) {
        if (block) clock_signal();
        return 1;
    }

    // Remove head special case.
    if (timeouts->head->id == id) {
        // Insert ID of removed timeout back into list of free_ids
        insert_id(&(timeouts->free_ids), timeouts->head->id);

        timeout_obj *rem = timeouts->head;
        timeouts->head = timeouts->head->next;
        free(rem);

        timeouts->num_timeouts--;
        if (block) clock_signal();
        return 0;
    }

    timeout_obj *prev = timeouts->head;
    for (timeout_obj *curr = timeouts->head->next; curr != NULL; curr = curr->next) {
        if (curr->id == id) {
            // Insert ID of removed timeout back into list of free_ids
            insert_id(&(timeouts->free_ids), curr->id);

            prev->next = curr->next;
            free(curr);

            timeouts->num_timeouts--;
            if (block) clock_signal();
            return 0;
        }
        prev = curr;
    }

    if (block) clock_signal();
    return 1;
}

/* NOTE: Cannot reset the timer within a callback. */
void consume_timeouts(timeouts *timeouts, timestamp_t curr_time) {
    clock_wait();

    timeouts->reset = 0;
    for (timeout_obj *curr = timeouts->head; curr != NULL && curr->expiry <= curr_time;) {
        timer_callback_t callback = curr->callback;
        uint32_t id = curr->id;
        void *data = curr->data;

        /* THIS WILL BE O(1) SPECIAL CASE SINCE ALWAYS FIRST ENTRY */
        curr = curr->next;
        remove_timeout(timeouts, id, false);
        (callback)(id, data);

        // If timer has been stopped in a callback.
        if (timeouts->reset == 1) {
            timeouts->reset = 0;
            clock_signal();
            return; 
        }
    }

    clock_signal();
}

timestamp_t peek_expiry(timeouts *timeouts) {
    clock_wait();

    timestamp_t min_tstamp = 0;
    if (timeouts->head != NULL) {
        clock_signal();
        return timeouts->head->expiry;
    }
    clock_signal();
    return ~min_tstamp;
}

uint32_t *fetch_timeout(timeouts *timeouts, size_t i) {
    clock_wait();
    timeout_obj *curr = timeouts->head;
    size_t curr_index = 0;
    while (curr != NULL) {
        if (curr_index == i) {
            clock_signal();
            return &(curr->id);
        }
        curr = curr->next;
        curr_index += 1;
    }

    clock_signal();
    return NULL;
}

///////////////////////////////////////////////////////////////////
/*       Implementation for Linked List of reusable ID's         */

int init_ids(free_ids *free_ids) {
    free_ids->head = NULL;

    // Initialise ids in range [1,255)
    for (uint32_t i = 1; i < CONC_MAX_TIMEOUTS; i++) {
        if (insert_id(free_ids, i) == 1) {
            return 1;
        }
    }
    return 0;
}

void *destroy_ids(free_ids *ids) {
    id_obj *curr = NULL;
    for (curr = ids->head; curr != NULL;) {
        id_obj *next = curr->next;
        free(curr);
        curr = next;
    }
    ids->head = NULL;
}

int insert_id(free_ids *ids, uint32_t id) {
    id_obj *new_id = malloc(sizeof(id_obj));
    if (new_id == NULL) {
        return 1;
    }
    new_id->id = id;
    new_id->next = NULL;

    // Insert head
    if (ids->head == NULL) {
        ids->head = new_id;
        return 0;
    }
    // free_ids *copy = ids;
    for (id_obj *curr = ids->head; curr != NULL; curr = curr->next) {
        // Insert at tail
        if (curr->next == NULL) {
            curr->next = new_id;
            break;
        }
    }
    
    return 0;
}

uint32_t pop_id(free_ids *ids) {
    // Pop from HEAD
    uint32_t ret_id = 0;
    if (ids->head != NULL) {
        ret_id = ids->head->id;
        id_obj *to_destroy = ids->head;
        ids->head = ids->head->next;
        free(to_destroy);
    }
    return ret_id;
}