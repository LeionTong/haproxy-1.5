/*
 * FD polling functions for generic poll()
 *
 * Copyright 2000-2012 Willy Tarreau <w@1wt.eu>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */

#include <unistd.h>
#include <sys/poll.h>
#include <sys/time.h>
#include <sys/types.h>

#include <common/compat.h>
#include <common/config.h>
#include <common/ticks.h>
#include <common/time.h>

#include <types/global.h>

#include <proto/fd.h>
#include <proto/signal.h>
#include <proto/task.h>


static fd_set *fd_evts[2];

/* private data */
static struct pollfd *poll_events = NULL;


REGPRM1 static void __fd_clo(const int fd)
{
	FD_CLR(fd, fd_evts[DIR_RD]);
	FD_CLR(fd, fd_evts[DIR_WR]);
}

/*
 * Poll() poller
 */
REGPRM2 static void _do_poll(struct poller *p, int exp)
{
	int status;
	int fd, nbfd;
	int wait_time;
	int updt_idx, en, eo;
	int fds, count;
	int sr, sw;
	unsigned rn, wn; /* read new, write new */

	/* first, scan the update list to find changes */
	for (updt_idx = 0; updt_idx < fd_nbupdt; updt_idx++) {
		fd = fd_updt[updt_idx];
		en = fdtab[fd].spec_e & 15;  /* new events */
		eo = fdtab[fd].spec_e >> 4;  /* previous events */

		if (fdtab[fd].owner && (eo ^ en)) {
			if ((eo ^ en) & FD_EV_POLLED_RW) {
				/* poll status changed, update the lists */
				if ((eo & ~en) & FD_EV_POLLED_R)
					FD_CLR(fd, fd_evts[DIR_RD]);
				else if ((en & ~eo) & FD_EV_POLLED_R)
					FD_SET(fd, fd_evts[DIR_RD]);

				if ((eo & ~en) & FD_EV_POLLED_W)
					FD_CLR(fd, fd_evts[DIR_WR]);
				else if ((en & ~eo) & FD_EV_POLLED_W)
					FD_SET(fd, fd_evts[DIR_WR]);
			}

			fdtab[fd].spec_e = (en << 4) + en;  /* save new events */

			if (!(en & FD_EV_ACTIVE_RW)) {
				/* This fd doesn't use any active entry anymore, we can
				 * kill its entry.
				 */
				release_spec_entry(fd);
			}
			else if ((en & ~eo) & FD_EV_ACTIVE_RW) {
				/* we need a new spec entry now */
				alloc_spec_entry(fd);
			}

		}
		fdtab[fd].updated = 0;
		fdtab[fd].new = 0;
	}
	fd_nbupdt = 0;

	nbfd = 0;
	for (fds = 0; (fds * BITS_PER_INT) < maxfd; fds++) {

		rn = ((int*)fd_evts[DIR_RD])[fds];
		wn = ((int*)fd_evts[DIR_WR])[fds];
	  
		if ((rn|wn)) {
			for (count = 0, fd = fds * BITS_PER_INT; count < BITS_PER_INT && fd < maxfd; count++, fd++) {
#define FDSETS_ARE_INT_ALIGNED
#ifdef FDSETS_ARE_INT_ALIGNED

#define WE_REALLY_KNOW_THAT_FDSETS_ARE_INTS
#ifdef WE_REALLY_KNOW_THAT_FDSETS_ARE_INTS
				sr = (rn >> count) & 1;
				sw = (wn >> count) & 1;
#else
				sr = FD_ISSET(fd&(BITS_PER_INT-1), (typeof(fd_set*))&rn);
				sw = FD_ISSET(fd&(BITS_PER_INT-1), (typeof(fd_set*))&wn);
#endif
#else
				sr = FD_ISSET(fd, fd_evts[DIR_RD]);
				sw = FD_ISSET(fd, fd_evts[DIR_WR]);
#endif
				if ((sr|sw)) {
					poll_events[nbfd].fd = fd;
					poll_events[nbfd].events = (sr ? POLLIN : 0) | (sw ? POLLOUT : 0);
					nbfd++;
				}
			}
		}		  
	}
      
	/* now let's wait for events */
	if (fd_nbspec || run_queue || signal_queue_len)
		wait_time = 0;
	else if (!exp)
		wait_time = MAX_DELAY_MS;
	else if (tick_is_expired(exp, now_ms))
		wait_time = 0;
	else {
		wait_time = TICKS_TO_MS(tick_remain(now_ms, exp)) + 1;
		if (wait_time > MAX_DELAY_MS)
			wait_time = MAX_DELAY_MS;
	}

	gettimeofday(&before_poll, NULL);
	status = poll(poll_events, nbfd, wait_time);
	tv_update_date(wait_time, status);
	measure_idle();

	for (count = 0; status > 0 && count < nbfd; count++) {
		int e = poll_events[count].revents;
		fd = poll_events[count].fd;
	  
		if (!(e & ( POLLOUT | POLLIN | POLLERR | POLLHUP )))
			continue;

		/* ok, we found one active fd */
		status--;

		if (!fdtab[fd].owner)
			continue;

		/* it looks complicated but gcc can optimize it away when constants
		 * have same values... In fact it depends on gcc :-(
		 */
		fdtab[fd].ev &= FD_POLL_STICKY;
		if (POLLIN == FD_POLL_IN && POLLOUT == FD_POLL_OUT &&
		    POLLERR == FD_POLL_ERR && POLLHUP == FD_POLL_HUP) {
			fdtab[fd].ev |= e & (POLLIN|POLLOUT|POLLERR|POLLHUP);
		}
		else {
			fdtab[fd].ev |=
				((e & POLLIN ) ? FD_POLL_IN  : 0) |
				((e & POLLOUT) ? FD_POLL_OUT : 0) |
				((e & POLLERR) ? FD_POLL_ERR : 0) |
				((e & POLLHUP) ? FD_POLL_HUP : 0);
		}

		if (fdtab[fd].iocb && fdtab[fd].owner && fdtab[fd].ev) {
			/* Mark the events as speculative before processing
			 * them so that if nothing can be done we don't need
			 * to poll again.
			 */
			if (fdtab[fd].ev & FD_POLL_IN)
				fd_ev_set(fd, DIR_RD);

			if (fdtab[fd].ev & FD_POLL_OUT)
				fd_ev_set(fd, DIR_WR);

			fdtab[fd].iocb(fd);
		}
	}

}

/*
 * Initialization of the poll() poller.
 * Returns 0 in case of failure, non-zero in case of success. If it fails, it
 * disables the poller by setting its pref to 0.
 */
REGPRM1 static int _do_init(struct poller *p)
{
	__label__ fail_swevt, fail_srevt, fail_pe;
	int fd_set_bytes;

	p->private = NULL;
	fd_set_bytes = sizeof(fd_set) * (global.maxsock + FD_SETSIZE - 1) / FD_SETSIZE;

	poll_events = (struct pollfd*)
		calloc(1, sizeof(struct pollfd) * global.maxsock);

	if (poll_events == NULL)
		goto fail_pe;
		
	if ((fd_evts[DIR_RD] = (fd_set *)calloc(1, fd_set_bytes)) == NULL)
		goto fail_srevt;

	if ((fd_evts[DIR_WR] = (fd_set *)calloc(1, fd_set_bytes)) == NULL)
		goto fail_swevt;

	return 1;

 fail_swevt:
	free(fd_evts[DIR_RD]);
 fail_srevt:
	free(poll_events);
 fail_pe:
	p->pref = 0;
	return 0;
}

/*
 * Termination of the poll() poller.
 * Memory is released and the poller is marked as unselectable.
 */
REGPRM1 static void _do_term(struct poller *p)
{
	free(fd_evts[DIR_WR]);
	free(fd_evts[DIR_RD]);
	free(poll_events);
	p->private = NULL;
	p->pref = 0;
}

/*
 * Check that the poller works.
 * Returns 1 if OK, otherwise 0.
 */
REGPRM1 static int _do_test(struct poller *p)
{
	return 1;
}

/*
 * It is a constructor, which means that it will automatically be called before
 * main(). This is GCC-specific but it works at least since 2.95.
 * Special care must be taken so that it does not need any uninitialized data.
 */
__attribute__((constructor))
static void _do_register(void)
{
	struct poller *p;

	if (nbpollers >= MAX_POLLERS)
		return;
	p = &pollers[nbpollers++];

	p->name = "poll";
	p->pref = 200;
	p->private = NULL;

	p->clo  = __fd_clo;
	p->test = _do_test;
	p->init = _do_init;
	p->term = _do_term;
	p->poll = _do_poll;
}


/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 * End:
 */
