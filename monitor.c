/*
 * mdmon - monitor external metadata arrays
 *
 * Copyright (C) 2007-2009 Neil Brown <neilb@suse.de>
 * Copyright (C) 2007-2009 Intel Corporation
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "mdadm.h"
#include "mdmon.h"
#include <sys/syscall.h>
#include <sys/select.h>
#include <signal.h>

static char *array_states[] = {
	"clear", "inactive", "suspended", "readonly", "read-auto",
	"clean", "active", "write-pending", "active-idle", NULL };
static char *sync_actions[] = {
	"idle", "reshape", "resync", "recover", "check", "repair", NULL
};

static int write_attr(char *attr, int fd)
{
	return write(fd, attr, strlen(attr));
}

static void add_fd(fd_set *fds, int *maxfd, int fd)
{
	if (fd < 0)
		return;
	if (fd > *maxfd)
		*maxfd = fd;
	FD_SET(fd, fds);
}

static int read_attr(char *buf, int len, int fd)
{
	int n;

	if (fd < 0) {
		buf[0] = 0;
		return 0;
	}
	lseek(fd, 0, 0);
	n = read(fd, buf, len - 1);

	if (n <= 0) {
		buf[0] = 0;
		return 0;
	}
	buf[n] = 0;
	if (buf[n-1] == '\n')
		buf[n-1] = 0;
	return n;
}

static unsigned long long read_resync_start(int fd)
{
	char buf[30];
	int n;

	n = read_attr(buf, 30, fd);
	if (n <= 0)
		return 0;
	if (strncmp(buf, "none", 4) == 0)
		return MaxSector;
	else
		return strtoull(buf, NULL, 10);
}

static unsigned long long read_sync_completed(int fd)
{
	unsigned long long val;
	char buf[50];
	int n;
	char *ep;

	n = read_attr(buf, 50, fd);

	if (n <= 0)
		return 0;
	buf[n] = 0;
	val = strtoull(buf, &ep, 0);
	if (ep == buf || (*ep != 0 && *ep != '\n' && *ep != ' '))
		return 0;
	return val;
}

static enum array_state read_state(int fd)
{
	char buf[20];
	int n = read_attr(buf, 20, fd);

	if (n <= 0)
		return bad_word;
	return (enum array_state) sysfs_match_word(buf, array_states);
}

static enum sync_action read_action( int fd)
{
	char buf[20];
	int n = read_attr(buf, 20, fd);

	if (n <= 0)
		return bad_action;
	return (enum sync_action) sysfs_match_word(buf, sync_actions);
}

int read_dev_state(int fd)
{
	char buf[60];
	int n = read_attr(buf, 60, fd);
	char *cp;
	int rv = 0;

	if (n <= 0)
		return 0;

	cp = buf;
	while (cp) {
		if (sysfs_attr_match(cp, "faulty"))
			rv |= DS_FAULTY;
		if (sysfs_attr_match(cp, "in_sync"))
			rv |= DS_INSYNC;
		if (sysfs_attr_match(cp, "write_mostly"))
			rv |= DS_WRITE_MOSTLY;
		if (sysfs_attr_match(cp, "spare"))
			rv |= DS_SPARE;
		if (sysfs_attr_match(cp, "blocked"))
			rv |= DS_BLOCKED;
		cp = strchr(cp, ',');
		if (cp)
			cp++;
	}
	return rv;
}

static void signal_manager(void)
{
	/* tgkill(getpid(), mon_tid, SIGUSR1); */
	int pid = getpid();
	syscall(SYS_tgkill, pid, mgr_tid, SIGUSR1);
}

/* Monitor a set of active md arrays - all of which share the
 * same metadata - and respond to events that require
 * metadata update.
 *
 * New arrays are detected by another thread which allocates
 * required memory and attaches the data structure to our list.
 *
 * Events:
 *  Array stops.
 *    This is detected by array_state going to 'clear' or 'inactive'.
 *    while we thought it was active.
 *    Response is to mark metadata as clean and 'clear' the array(??)
 *  write-pending
 *    array_state if 'write-pending'
 *    We mark metadata as 'dirty' then set array to 'active'.
 *  active_idle
 *    Either ignore, or mark clean, then mark metadata as clean.
 *
 *  device fails
 *    detected by rd-N/state reporting "faulty"
 *    mark device as 'failed' in metadata, let the kernel release the
 *    device by writing '-blocked' to rd/state, and finally write 'remove' to
 *    rd/state.  Before a disk can be replaced it must be failed and removed
 *    from all container members, this will be preemptive for the other
 *    arrays... safe?
 *
 *  sync completes
 *    sync_action was 'resync' and becomes 'idle' and resync_start becomes
 *    MaxSector
 *    Notify metadata that sync is complete.
 *
 *  recovery completes
 *    sync_action changes from 'recover' to 'idle'
 *    Check each device state and mark metadata if 'faulty' or 'in_sync'.
 *
 *  deal with resync
 *    This only happens on finding a new array... mdadm will have set
 *    'resync_start' to the correct value.  If 'resync_start' indicates that an
 *    resync needs to occur set the array to the 'active' state rather than the
 *    initial read-auto state.
 *
 *
 *
 * We wait for a change (poll/select) on array_state, sync_action, and
 * each rd-X/state file.
 * When we get any change, we check everything.  So read each state file,
 * then decide what to do.
 *
 * The core action is to write new metadata to all devices in the array.
 * This is done at most once on any wakeup.
 * After that we might:
 *   - update the array_state
 *   - set the role of some devices.
 *   - request a sync_action
 *
 */

static int read_and_act(struct active_array *a)
{
	unsigned long long sync_completed;
	int check_degraded = 0;
	int deactivate = 0;
	struct mdinfo *mdi;
	int dirty = 0;

	a->next_state = bad_word;
	a->next_action = bad_action;

	a->curr_state = read_state(a->info.state_fd);
	a->curr_action = read_action(a->action_fd);
	a->info.resync_start = read_resync_start(a->resync_start_fd);
	sync_completed = read_sync_completed(a->sync_completed_fd);
	for (mdi = a->info.devs; mdi ; mdi = mdi->next) {
		mdi->next_state = 0;
		mdi->curr_state = 0;
		if (mdi->state_fd >= 0) {
			mdi->recovery_start = read_resync_start(mdi->recovery_fd);
			mdi->curr_state = read_dev_state(mdi->state_fd);
		}
	}

	if (a->curr_state <= inactive &&
	    a->prev_state > inactive) {
		/* array has been stopped */
		a->container->ss->set_array_state(a, 1);
		a->next_state = clear;
		deactivate = 1;
	}
	if (a->curr_state == write_pending) {
		a->container->ss->set_array_state(a, 0);
		a->next_state = active;
		dirty = 1;
	}
	if (a->curr_state == active_idle) {
		/* Set array to 'clean' FIRST, then mark clean
		 * in the metadata
		 */
		a->next_state = clean;
		dirty = 1;
	}
	if (a->curr_state == clean) {
		a->container->ss->set_array_state(a, 1);
	}
	if (a->curr_state == active ||
	    a->curr_state == suspended ||
	    a->curr_state == bad_word)
		dirty = 1;
	if (a->curr_state == readonly) {
		/* Well, I'm ready to handle things.  If readonly
		 * wasn't requested, transition to read-auto.
		 */
		char buf[64];
		read_attr(buf, sizeof(buf), a->metadata_fd);
		if (strncmp(buf, "external:-", 10) == 0) {
			/* explicit request for readonly array.  Leave it alone */
			;
		} else {
			if (a->container->ss->set_array_state(a, 2))
				a->next_state = read_auto; /* array is clean */
			else {
				a->next_state = active; /* Now active for recovery etc */
				dirty = 1;
			}
		}
	}

	if (!deactivate &&
	    a->curr_action == idle &&
	    a->prev_action == resync) {
		/* A resync has finished.  The endpoint is recorded in
		 * 'sync_start'.  We don't update the metadata
		 * until the array goes inactive or readonly though.
		 * Just check if we need to fiddle spares.
		 */
		a->container->ss->set_array_state(a, a->curr_state <= clean);
		check_degraded = 1;
	}

	if (!deactivate &&
	    a->curr_action == idle &&
	    a->prev_action == recover) {
		/* A recovery has finished.  Some disks may be in sync now,
		 * and the array may no longer be degraded
		 */
		for (mdi = a->info.devs ; mdi ; mdi = mdi->next) {
			a->container->ss->set_disk(a, mdi->disk.raid_disk,
						   mdi->curr_state);
			if (! (mdi->curr_state & DS_INSYNC))
				check_degraded = 1;
		}
	}

	/* Check for failures and if found:
	 * 1/ Record the failure in the metadata and unblock the device.
	 *    FIXME update the kernel to stop notifying on failed drives when
	 *    the array is readonly and we have cleared 'blocked'
	 * 2/ Try to remove the device if the array is writable, or can be
	 *    made writable.
	 */
	for (mdi = a->info.devs ; mdi ; mdi = mdi->next) {
		if (mdi->curr_state & DS_FAULTY) {
			a->container->ss->set_disk(a, mdi->disk.raid_disk,
						   mdi->curr_state);
			check_degraded = 1;
			mdi->next_state |= DS_UNBLOCK;
			if (a->curr_state == read_auto) {
				a->container->ss->set_array_state(a, 0);
				a->next_state = active;
			}
			if (a->curr_state > readonly)
				mdi->next_state |= DS_REMOVE;
		}
	}

	/* Check for recovery checkpoint notifications.  We need to be a
	 * minimum distance away from the last checkpoint to prevent
	 * over checkpointing.  Note reshape checkpointing is not
	 * handled here.
	 */
	if (sync_completed > a->last_checkpoint &&
	    sync_completed - a->last_checkpoint > a->info.component_size >> 4 &&
	    a->curr_action > reshape) {
		/* A (non-reshape) sync_action has reached a checkpoint.
		 * Record the updated position in the metadata
		 */
		a->last_checkpoint = sync_completed;
		a->container->ss->set_array_state(a, a->curr_state <= clean);
	} else if (sync_completed > a->last_checkpoint)
		a->last_checkpoint = sync_completed;

	a->container->ss->sync_metadata(a->container);
	dprintf("%s(%d): state:%s action:%s next(", __func__, a->info.container_member,
		array_states[a->curr_state], sync_actions[a->curr_action]);

	/* Effect state changes in the array */
	if (a->next_state != bad_word) {
		dprintf(" state:%s", array_states[a->next_state]);
		write_attr(array_states[a->next_state], a->info.state_fd);
	}
	if (a->next_action != bad_action) {
		write_attr(sync_actions[a->next_action], a->action_fd);
		dprintf(" action:%s", sync_actions[a->next_action]);
	}
	for (mdi = a->info.devs; mdi ; mdi = mdi->next) {
		if (mdi->next_state & DS_UNBLOCK) {
			dprintf(" %d:-blocked", mdi->disk.raid_disk);
			write_attr("-blocked", mdi->state_fd);
		}

		if ((mdi->next_state & DS_REMOVE) && mdi->state_fd >= 0) {
			int remove_result;

			/* the kernel may not be able to immediately remove the
			 * disk, we can simply wait until the next event to try
			 * again.
			 */
			remove_result = write_attr("remove", mdi->state_fd);
			if (remove_result > 0) {
				dprintf(" %d:removed", mdi->disk.raid_disk);
				close(mdi->state_fd);
				close(mdi->recovery_fd);
				mdi->state_fd = -1;
			}
		}
		if (mdi->next_state & DS_INSYNC) {
			write_attr("+in_sync", mdi->state_fd);
			dprintf(" %d:+in_sync", mdi->disk.raid_disk);
		}
	}
	dprintf(" )\n");

	/* move curr_ to prev_ */
	a->prev_state = a->curr_state;

	a->prev_action = a->curr_action;

	for (mdi = a->info.devs; mdi ; mdi = mdi->next) {
		mdi->prev_state = mdi->curr_state;
		mdi->next_state = 0;
	}

	if (check_degraded) {
		/* manager will do the actual check */
		a->check_degraded = 1;
		signal_manager();
	}

	if (deactivate)
		a->container = NULL;

	return dirty;
}

static struct mdinfo *
find_device(struct active_array *a, int major, int minor)
{
	struct mdinfo *mdi;

	for (mdi = a->info.devs ; mdi ; mdi = mdi->next)
		if (mdi->disk.major == major && mdi->disk.minor == minor)
			return mdi;

	return NULL;
}

static void reconcile_failed(struct active_array *aa, struct mdinfo *failed)
{
	struct active_array *a;
	struct mdinfo *victim;

	for (a = aa; a; a = a->next) {
		if (!a->container)
			continue;
		victim = find_device(a, failed->disk.major, failed->disk.minor);
		if (!victim)
			continue;

		if (!(victim->curr_state & DS_FAULTY))
			write_attr("faulty", victim->state_fd);
	}
}

#ifdef DEBUG
static void dprint_wake_reasons(fd_set *fds)
{
	int i;
	char proc_path[256];
	char link[256];
	char *basename;
	int rv;

	fprintf(stderr, "monitor: wake ( ");
	for (i = 0; i < FD_SETSIZE; i++) {
		if (FD_ISSET(i, fds)) {
			sprintf(proc_path, "/proc/%d/fd/%d",
				(int) getpid(), i);

			rv = readlink(proc_path, link, sizeof(link) - 1);
			if (rv < 0) {
				fprintf(stderr, "%d:unknown ", i);
				continue;
			}
			link[rv] = '\0';
			basename = strrchr(link, '/');
			fprintf(stderr, "%d:%s ",
				i, basename ? ++basename : link);
		}
	}
	fprintf(stderr, ")\n");
}
#endif

int monitor_loop_cnt;

static int wait_and_act(struct supertype *container, int nowait)
{
	fd_set rfds;
	int maxfd = 0;
	struct active_array **aap = &container->arrays;
	struct active_array *a, **ap;
	int rv;
	struct mdinfo *mdi;
	static unsigned int dirty_arrays = ~0; /* start at some non-zero value */

	FD_ZERO(&rfds);

	for (ap = aap ; *ap ;) {
		a = *ap;
		/* once an array has been deactivated we want to
		 * ask the manager to discard it.
		 */
		if (!a->container) {
			if (discard_this) {
				ap = &(*ap)->next;
				continue;
			}
			*ap = a->next;
			a->next = NULL;
			discard_this = a;
			signal_manager();
			continue;
		}

		add_fd(&rfds, &maxfd, a->info.state_fd);
		add_fd(&rfds, &maxfd, a->action_fd);
		add_fd(&rfds, &maxfd, a->sync_completed_fd);
		for (mdi = a->info.devs ; mdi ; mdi = mdi->next)
			add_fd(&rfds, &maxfd, mdi->state_fd);

		ap = &(*ap)->next;
	}

	if (manager_ready && (*aap == NULL || (sigterm && !dirty_arrays))) {
		/* No interesting arrays, or we have been told to
		 * terminate and everything is clean.  Lets see about
		 * exiting.  Note that blocking at this point is not a
		 * problem as there are no active arrays, there is
		 * nothing that we need to be ready to do.
		 */
		int fd = open_dev_excl(container->devnum);
		if (fd >= 0 || errno != EBUSY) {
			/* OK, we are safe to leave */
			if (sigterm && !dirty_arrays)
				dprintf("caught sigterm, all clean... exiting\n");
			else
				dprintf("no arrays to monitor... exiting\n");
			if (!sigterm)
				/* On SIGTERM, someone (the take-over mdmon) will
				 * clean up
				 */
				remove_pidfile(container->devname);
			exit_now = 1;
			signal_manager();
			exit(0);
		}
	}

	if (!nowait) {
		sigset_t set;
		sigprocmask(SIG_UNBLOCK, NULL, &set);
		sigdelset(&set, SIGUSR1);
		monitor_loop_cnt |= 1;
		rv = pselect(maxfd+1, NULL, NULL, &rfds, NULL, &set);
		monitor_loop_cnt += 1;
		if (rv == -1 && errno == EINTR)
			rv = 0;
		#ifdef DEBUG
		dprint_wake_reasons(&rfds);
		#endif

	}

	if (update_queue) {
		struct metadata_update *this;

		for (this = update_queue; this ; this = this->next)
			container->ss->process_update(container, this);

		update_queue_handled = update_queue;
		update_queue = NULL;
		signal_manager();
		container->ss->sync_metadata(container);
	}

	rv = 0;
	dirty_arrays = 0;
	for (a = *aap; a ; a = a->next) {
		int is_dirty;

		if (a->replaces && !discard_this) {
			struct active_array **ap;
			for (ap = &a->next; *ap && *ap != a->replaces;
			     ap = & (*ap)->next)
				;
			if (*ap)
				*ap = (*ap)->next;
			discard_this = a->replaces;
			a->replaces = NULL;
			/* FIXME check if device->state_fd need to be cleared?*/
			signal_manager();
		}
		if (a->container) {
			is_dirty = read_and_act(a);
			rv |= 1;
			dirty_arrays += is_dirty;
			/* when terminating stop manipulating the array after it
			 * is clean, but make sure read_and_act() is given a
			 * chance to handle 'active_idle'
			 */
			if (sigterm && !is_dirty)
				a->container = NULL; /* stop touching this array */
		}
	}

	/* propagate failures across container members */
	for (a = *aap; a ; a = a->next) {
		if (!a->container)
			continue;
		for (mdi = a->info.devs ; mdi ; mdi = mdi->next)
			if (mdi->curr_state & DS_FAULTY)
				reconcile_failed(*aap, mdi);
	}

	return rv;
}

void do_monitor(struct supertype *container)
{
	int rv;
	int first = 1;
	do {
		rv = wait_and_act(container, first);
		first = 0;
	} while (rv >= 0);
}
