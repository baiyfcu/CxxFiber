/* A simple event-driven programming library. Originally I wrote this code
 * for the Jim's event-loop (Jim is a Tcl interpreter) but later translated
 * it in form of a library for easy reuse.
 *
 * Copyright (c) 2006-2010, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>
#include <poll.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#include "eco_ae.h"

#define HAVE_POLL

#define aeEventLoop co_poll_t

/* Include the best multiplexing layer supported by this system.
 * The following should be ordered by performances, descending. */
#ifdef HAVE_EPOLL
#include "eco_ae_epoll.c"
#else
	#ifdef HAVE_KQUEUE
	#include "eco_ae_kqueue.c"
	#else //win etc.
    #include "eco_ae_select.c"
	#endif
#endif

co_poll_t *eco_poll_create(int setsize) {
	co_poll_t *poll;
    int i;

    if ((poll = malloc(sizeof(*poll))) == NULL) goto err;
    poll->events = malloc(sizeof(coFileEvent)*setsize);
    poll->fired = malloc(sizeof(coFiredEvent)*setsize);
    if (poll->events == NULL || poll->fired == NULL) goto err;
    poll->setsize = setsize;
    poll->lastTime = time(NULL);
    poll->timeEventHead = NULL;
    poll->timeEventNextId = 0;
    poll->maxfd = -1;
    if (aeApiCreate(poll) == -1) goto err;
    /* Events with mask == ECO_POLL_NONE are not set. So let's initialize the
     * vector with it. */
    for (i = 0; i < setsize; i++)
    	poll->events[i].mask = ECO_POLL_NONE;
    return poll;

err:
    if (poll) {
    	free(poll->events);
    	free(poll->fired);
    	free(poll);
    }
    return NULL;
}

void eco_poll_destroy(co_poll_t** poll) {
	if (!poll || !*poll) {
		return;
	}
    aeApiFree(*poll);
    free((*poll)->events);
    free((*poll)->fired);
    free(*poll);
    *poll = NULL;
}

/* Return the current set size. */
int eco_poll_get_size(co_poll_t *poll) {
    return poll->setsize;
}

/* Resize the maximum set size of the event loop.
 * If the requested set size is smaller than the current set size, but
 * there is already a file descriptor in use that is >= the requested
 * set size minus one, -1 is returned and the operation is not
 * performed at all.
 *
 * Otherwise 0 is returned and the operation is successful. */
int eco_poll_resize(co_poll_t *poll, int setsize) {
    int i;

    if (setsize == poll->setsize) return 0;
    if (poll->maxfd >= setsize) return -1;
    if (aeApiResize(poll,setsize) == -1) return -1;

    poll->events = realloc(poll->events,sizeof(coFileEvent)*setsize);
    poll->fired = realloc(poll->fired,sizeof(coFiredEvent)*setsize);
    poll->setsize = setsize;

    /* Make sure that if we created new slots, they are initialized with
     * an ECO_POLL_NONE mask. */
    for (i = poll->maxfd+1; i < setsize; i++)
    	poll->events[i].mask = ECO_POLL_NONE;
    return 0;
}

es_status_t eco_poll_file_event_create(co_poll_t *poll, int fd, int mask,
        coFileProc *proc, void *clientData)
{
	coFileEvent *fe = NULL;

    if (fd >= poll->setsize) {
        errno = ERANGE;
        return ES_FAILURE;
    }
    fe = &poll->events[fd];

    if (aeApiAddEvent(poll, fd, mask) == -1)
        return ES_FAILURE;
    fe->mask |= mask;
    if (mask & ECO_POLL_READABLE) fe->rfileProc = proc;
    if (mask & ECO_POLL_WRITABLE) fe->wfileProc = proc;
    fe->clientData = clientData;
    if (fd > poll->maxfd)
    	poll->maxfd = fd;
    return ES_SUCCESS;
}

es_status_t eco_poll_file_event_update(co_poll_t *poll, int fd, int mask,
        coFileProc *proc, void *clientData)
{
	coFileEvent *fe = NULL;

	if (fd >= poll->setsize) {
		errno = ERANGE;
		return ES_FAILURE;
	}
	fe = &poll->events[fd];

	if (aeApiUpdEvent(poll, fd, mask) == -1)
		return ES_FAILURE;
	fe->mask |= mask;
	if (mask & ECO_POLL_READABLE)
		fe->rfileProc = proc;
	if (mask & ECO_POLL_WRITABLE)
		fe->wfileProc = proc;
	fe->clientData = clientData;
	if (fd > poll->maxfd)
		poll->maxfd = fd;
	return ES_SUCCESS;
}

void eco_poll_file_event_delete(co_poll_t *poll, int fd, int mask)
{
	coFileEvent *fe = NULL;

    if (fd >= poll->setsize) return;
    fe = &poll->events[fd];

    if (fe->mask == ECO_POLL_NONE) return;
    fe->mask = fe->mask & (~mask);
    if (fd == poll->maxfd && fe->mask == ECO_POLL_NONE) {
        /* Update the max fd */
        int j;

        for (j = poll->maxfd-1; j >= 0; j--)
            if (poll->events[j].mask != ECO_POLL_NONE) break;
        poll->maxfd = j;
    }
    aeApiDelEvent(poll, fd, mask);
}

int eco_poll_get_file_events(co_poll_t *poll, int fd) {
	coFileEvent *fe = NULL;

    if (fd >= poll->setsize) return 0;
    fe = &poll->events[fd];

    return fe->mask;
}

static void aeGetTime(long *seconds, long *milliseconds)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    *seconds = tv.tv_sec;
    *milliseconds = tv.tv_usec/1000;
}

static void aeAddMillisecondsToNow(es_int64_t milliseconds, long *sec, long *ms) {
    long cur_sec, cur_ms, when_sec, when_ms;

    aeGetTime(&cur_sec, &cur_ms);
    when_sec = (long)(cur_sec + milliseconds/1000);
    when_ms = (long)(cur_ms + milliseconds%1000);
    if (when_ms >= 1000) {
        when_sec ++;
        when_ms -= 1000;
    }
    *sec = when_sec;
    *ms = when_ms;
}

es_int64_t eco_poll_time_event_create(co_poll_t *poll, es_int64_t milliseconds,
        coTimeProc *proc, void *clientData,
        coEventFinalizerProc *finalizerProc)
{
    es_int64_t id = poll->timeEventNextId++;
    coTimeEvent *te;

    te = malloc(sizeof(*te));
    if (te == NULL) return ES_FAILURE;
    te->id = id;
    aeAddMillisecondsToNow(milliseconds,&te->when_sec,&te->when_ms);
    te->timeProc = proc;
    te->finalizerProc = finalizerProc;
    te->clientData = clientData;
    te->next = poll->timeEventHead;
    poll->timeEventHead = te;
    return id;
}

es_status_t eco_poll_time_event_delete(co_poll_t *poll, es_int64_t id)
{
	coTimeEvent *te = poll->timeEventHead;
	while(te) {
		if (te->id == id) {
			te->id = ECO_POLL_DELETED_EVENT_ID;
			return ES_SUCCESS;
		}
		te = te->next;
	}
	return ES_FAILURE; /* NO event with the specified ID found */
}

es_status_t eco_poll_time_fire_all(co_poll_t* poll) {
	long cur_sec, cur_ms;
	coTimeEvent *te;

    aeGetTime(&cur_sec, &cur_ms);

	te = poll->timeEventHead;
	while(te) {
		if (te->id != ECO_POLL_DELETED_EVENT_ID) {
			te->when_sec = cur_sec;
			te->when_ms = cur_ms;
		}
		te = te->next;
	}
	return ES_SUCCESS;
}

/* Search the first timer to fire.
 * This operation is useful to know how many time the select can be
 * put in sleep without to delay any event.
 * If there are no timers NULL is returned.
 *
 * Note that's O(N) since time events are unsorted.
 * Possible optimizations (not needed by Redis so far, but...):
 * 1) Insert the event in order, so that the nearest is just the head.
 *    Much better but still insertion or deletion of timers is O(N).
 * 2) Use a skiplist to have this operation as O(1) and insertion as O(log(N)).
 */
static coTimeEvent *aeSearchNearestTimer(co_poll_t *poll)
{
    coTimeEvent *te = poll->timeEventHead;
    coTimeEvent *nearest = NULL;

    while(te) {
        if (!nearest || te->when_sec < nearest->when_sec ||
                (te->when_sec == nearest->when_sec &&
                 te->when_ms < nearest->when_ms))
            nearest = te;
        te = te->next;
    }
    return nearest;
}

/* Process time events */
static int processTimeEvents(co_poll_t *poll) {
    int processed = 0;
    coTimeEvent *te, *prev;
    es_int64_t maxId;
    time_t now = time(NULL);

    /* If the system clock is moved to the future, and then set back to the
     * right value, time events may be delayed in a random way. Often this
     * means that scheduled operations will not be performed soon enough.
     *
     * Here we try to detect system clock skews, and force all the time
     * events to be processed ASAP when this happens: the idea is that
     * processing events earlier is less dangerous than delaying them
     * indefinitely, and practice suggests it is. */
    if (now < poll->lastTime) {
        te = poll->timeEventHead;
        while(te) {
            te->when_sec = 0;
            te = te->next;
        }
    }
    poll->lastTime = now;

    prev = NULL;
    te = poll->timeEventHead;
    maxId = poll->timeEventNextId-1;
    while(te) {
        long now_sec, now_ms;
        es_int64_t id;

        /* Remove events scheduled for deletion. */
		if (te->id == ECO_POLL_DELETED_EVENT_ID) {
			coTimeEvent *next = te->next;
			if (prev == NULL)
				poll->timeEventHead = te->next;
			else
				prev->next = te->next;
			if (te->finalizerProc)
				te->finalizerProc(poll, te->clientData);
			free(te);
			te = next;
			continue;
		}

		/* Make sure we don't process time events created by time events in
		 * this iteration. Note that this check is currently useless: we always
		 * add new timers on the head, however if we change the implementation
		 * detail, this check may be useful again: we keep it here for future
		 * defense. */
        if (te->id > maxId) {
            te = te->next;
            continue;
        }
        aeGetTime(&now_sec, &now_ms);
        if (now_sec > te->when_sec ||
            (now_sec == te->when_sec && now_ms >= te->when_ms))
        {
            int retval;

            id = te->id;
            retval = te->timeProc(poll, id, te->clientData);
            processed++;
            if (retval != ECO_POLL_NOMORE) {
                aeAddMillisecondsToNow(retval,&te->when_sec,&te->when_ms);
            } else {
            	te->id = ECO_POLL_DELETED_EVENT_ID;
            }
        }
        prev = te;
		te = te->next;
    }
    return processed;
}

int eco_poll_process_events(co_poll_t *poll, int flags, es_int64_t timeout)
{
    int processed = 0, numevents;

    /* Nothing to do? return ASAP */
    if (!(flags & ECO_POLL_TIME_EVENTS) && !(flags & ECO_POLL_FILE_EVENTS)) return 0;

    /* Note that we want call select() even if there are no
     * file events to process as long as we want to process time
     * events, in order to sleep until the next time event is ready
     * to fire. */
    if (poll->maxfd != -1 ||
        ((flags & ECO_POLL_TIME_EVENTS) && !(timeout == 0))) {
        int j;
        coTimeEvent *shortest = NULL;
        struct timeval tv, *tvp;

        if ((flags & ECO_POLL_TIME_EVENTS) && !(timeout == 0))
            shortest = aeSearchNearestTimer(poll);
        if (shortest) {
            long now_sec, now_ms;

            /* Calculate the time missing for the nearest
             * timer to fire. */
            aeGetTime(&now_sec, &now_ms);
            tvp = &tv;
            tvp->tv_sec = shortest->when_sec - now_sec;
            if (shortest->when_ms < now_ms) {
                tvp->tv_usec = ((shortest->when_ms+1000) - now_ms)*1000;
                tvp->tv_sec --;
            } else {
                tvp->tv_usec = (shortest->when_ms - now_ms)*1000;
            }
            if (tvp->tv_sec < 0) tvp->tv_sec = 0;
            if (tvp->tv_usec < 0) tvp->tv_usec = 0;
        } else {
        	if (timeout > 0) {
        		tv.tv_sec = timeout / 1000;
        		tv.tv_usec = (timeout % 1000) * 1000;
        		tvp = &tv;
        	} else if (timeout == 0) {
                /* If we have to check for events but need to return
                 * ASAP because of timeout == 0 we need to set the timeout
                 * to zero */
                tv.tv_sec = tv.tv_usec = 0;
                tvp = &tv;
            } else {
                /* Otherwise we can block */
                tvp = NULL; /* wait forever */
            }
        }

        numevents = aeApiPoll(poll, tvp);
        for (j = 0; j < numevents; j++) {
            coFileEvent *fe = &poll->events[poll->fired[j].fd];
            int mask = poll->fired[j].mask;
            int fd = poll->fired[j].fd;
            int rfired = 0;

            /* note the fe->mask & mask & ... code: maybe an already processed
             * event removed an element that fired and we still didn't
             * processed, so we check if the event is still valid. */
            if (fe->mask & mask & ECO_POLL_READABLE) {
                rfired = 1;
                if (fe->rfileProc) fe->rfileProc(poll,fd,fe->clientData,mask);
            }
            if (fe->mask & mask & ECO_POLL_WRITABLE) {
                if (fe->wfileProc && (!rfired || fe->wfileProc != fe->rfileProc))
                    fe->wfileProc(poll,fd,fe->clientData,mask);
            }
            processed++;
        }
    }
    /* Check time events */
    if (flags & ECO_POLL_TIME_EVENTS)
        processed += processTimeEvents(poll);

    return processed; /* return the number of processed file/time events */
}

int eco_poll_wait(int fd, int mask, es_int64_t milliseconds) {
#ifdef HAVE_POLL
    struct pollfd pfd;
    int retmask = 0, retval;

    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = fd;
    if (mask & ECO_POLL_READABLE) pfd.events |= POLLIN;
    if (mask & ECO_POLL_WRITABLE) pfd.events |= POLLOUT;

	if ((retval = poll(&pfd, 1, milliseconds)) == 1) {
		if (pfd.revents & POLLIN)
			retmask |= ECO_POLL_READABLE;
		if (pfd.revents & POLLOUT)
			retmask |= ECO_POLL_WRITABLE;
		if (pfd.revents & POLLERR)
			retmask |= ECO_POLL_WRITABLE;
		if (pfd.revents & POLLHUP)
			retmask |= ECO_POLL_WRITABLE;
		return retmask;
	} else {
        return retval;
    }
#else //!
	fd_set rd, wr, ex;
	struct timeval t;

	t.tv_sec = milliseconds / 1000;
	t.tv_usec = (milliseconds % 1000) * 1000;

	FD_ZERO(&rd);
	FD_ZERO(&wr);
	FD_ZERO(&ex);

	if (mask & ECO_POLL_READABLE) {
		FD_SET(fd, &rd);
	}
	if (mask & ECO_POLL_WRITABLE) {
		FD_SET(fd, &wr);
	}

	errno = 0;
	return select(fd + 1, &rd, &wr, &ex, (milliseconds >= 0) ? &t : NULL);
#endif
}

char *eco_poll_api_name(void) {
    return aeApiName();
}

int eco_poll_getfd(co_poll_t* poll) {
	return aeApiHandle(poll);
}
