/*
 * mdadm - manage Linux "md" devices aka RAID arrays.
 *
 * Copyright (C) 2001-2009 Neil Brown <neilb@suse.de>
 *
 *
 *    This program is free software; you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation; either version 2 of the License, or
 *    (at your option) any later version.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with this program; if not, write to the Free Software
 *    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 *    Author: Neil Brown
 *    Email: <neilb@suse.de>
 */

#include	"mdadm.h"
#include	<ctype.h>

static int name_matches(char *found, char *required, char *homehost)
{
	/* See if the name found matches the required name, possibly
	 * prefixed with 'homehost'
	 */
	char fnd[33];

	strncpy(fnd, found, 32);
	fnd[32] = 0;
	if (strcmp(found, required)==0)
		return 1;
	if (homehost) {
		int l = strlen(homehost);
		if (l < 32 && fnd[l] == ':' &&
		    strcmp(fnd+l+1, required)==0)
			return 1;
	}
	return 0;
}

static int is_member_busy(char *metadata_version)
{
	/* check if the given member array is active */
	struct mdstat_ent *mdstat = mdstat_read(1, 0);
	struct mdstat_ent *ent;
	int busy = 0;

	for (ent = mdstat; ent; ent = ent->next) {
		if (ent->metadata_version == NULL)
			continue;
		if (strncmp(ent->metadata_version, "external:", 9) != 0)
			continue;
		if (!is_subarray(&ent->metadata_version[9]))
			continue;
		/* Skip first char - it can be '/' or '-' */
		if (strcmp(&ent->metadata_version[10], metadata_version+1) == 0) {
			busy = 1;
			break;
		}
	}
	free_mdstat(mdstat);

	return busy;
}

int Assemble(struct supertype *st, char *mddev,
	     mddev_ident_t ident,
	     mddev_dev_t devlist, char *backup_file,
	     int readonly, int runstop,
	     char *update, char *homehost, int require_homehost,
	     int verbose, int force)
{
	/*
	 * The task of Assemble is to find a collection of
	 * devices that should (according to their superblocks)
	 * form an array, and to give this collection to the MD driver.
	 * In Linux-2.4 and later, this involves submitting a
	 * SET_ARRAY_INFO ioctl with no arg - to prepare
	 * the array - and then submit a number of
	 * ADD_NEW_DISK ioctls to add disks into
	 * the array.  Finally RUN_ARRAY might
	 * be submitted to start the array.
	 *
	 * Much of the work of Assemble is in finding and/or
	 * checking the disks to make sure they look right.
	 *
	 * If mddev is not set, then scan must be set and we
	 *  read through the config file for dev+uuid mapping
	 *  We recurse, setting mddev, for each device that
	 *    - isn't running
	 *    - has a valid uuid (or any uuid if !uuidset)
	 *
	 * If mddev is set, we try to determine state of md.
	 *   check version - must be at least 0.90.0
	 *   check kernel version.  must be at least 2.4.
	 *    If not, we can possibly fall back on START_ARRAY
	 *   Try to GET_ARRAY_INFO.
	 *     If possible, give up
	 *     If not, try to STOP_ARRAY just to make sure
	 *
	 * If !uuidset and scan, look in conf-file for uuid
	 *       If not found, give up
	 * If !devlist and scan and uuidset, get list of devs from conf-file
	 *
	 * For each device:
	 *   Check superblock - discard if bad
	 *   Check uuid (set if we don't have one) - discard if no match
	 *   Check superblock similarity if we have a superblock - discard if different
	 *   Record events, devicenum
	 * This should give us a list of devices for the array
	 * We should collect the most recent event number
	 *
	 * Count disks with recent enough event count
	 * While force && !enough disks
	 *    Choose newest rejected disks, update event count
	 *     mark clean and rewrite superblock
	 * If recent kernel:
	 *    SET_ARRAY_INFO
	 *    foreach device with recent events : ADD_NEW_DISK
	 *    if runstop == 1 || "enough" disks and runstop==0 -> RUN_ARRAY
	 * If old kernel:
	 *    Check the device numbers in superblock are right
	 *    update superblock if any changes
	 *    START_ARRAY
	 *
	 */
	int mdfd;
	int clean;
	int auto_assem = (mddev == NULL && !ident->uuid_set &&
			  ident->super_minor == UnSet && ident->name[0] == 0
			  && (ident->container == NULL || ident->member == NULL));
	int old_linux = 0;
	int vers = vers; /* Keep gcc quite - it really is initialised */
	struct {
		char *devname;
		int uptodate; /* set once we decide that this device is as
			       * recent as everything else in the array.
			       */
		struct mdinfo i;
	} *devices;
	int *best = NULL; /* indexed by raid_disk */
	int bestcnt = 0;
	int devcnt = 0;
	unsigned int okcnt, sparecnt, rebuilding_cnt;
	unsigned int req_cnt;
	int i;
	int most_recent = 0;
	int chosen_drive;
	int change = 0;
	int inargv = 0;
	int report_missmatch;
	int bitmap_done;
	int start_partial_ok = (runstop >= 0) && 
		(force || devlist==NULL || auto_assem);
	unsigned int num_devs;
	mddev_dev_t tmpdev;
	struct mdinfo info;
	struct mdinfo *content = NULL;
	char *avail;
	int nextspare = 0;
	char *name = NULL;
	int trustworthy;
	char chosen_name[1024];

	if (get_linux_version() < 2004000)
		old_linux = 1;

	/*
	 * If any subdevs are listed, then any that don't
	 * match ident are discarded.  Remainder must all match and
	 * become the array.
	 * If no subdevs, then we scan all devices in the config file, but
	 * there must be something in the identity
	 */

	if (!devlist &&
	    ident->uuid_set == 0 &&
	    (ident->super_minor < 0 || ident->super_minor == UnSet) &&
	    ident->name[0] == 0 &&
	    (ident->container == NULL || ident->member == NULL) &&
	    ident->devices == NULL) {
		fprintf(stderr, Name ": No identity information available for %s - cannot assemble.\n",
			mddev ? mddev : "further assembly");
		return 1;
	}

	if (devlist == NULL)
		devlist = conf_get_devs();
	else if (mddev)
		inargv = 1;

	report_missmatch = ((inargv && verbose >= 0) || verbose > 0);
 try_again:
	/* We come back here when doing auto-assembly and attempting some
	 * set of devices failed.  Those are now marked as ->used==2 and
	 * we ignore them and try again
	 */

	tmpdev = devlist; num_devs = 0;
	while (tmpdev) {
		if (tmpdev->used)
			tmpdev->used = 2;
		else
			num_devs++;
		tmpdev = tmpdev->next;
	}
	devices = malloc(num_devs * sizeof(*devices));

	if (!st && ident->st) st = ident->st;

	if (verbose>0)
	    fprintf(stderr, Name ": looking for devices for %s\n",
		    mddev ? mddev : "further assembly");

	/* first walk the list of devices to find a consistent set
	 * that match the criterea, if that is possible.
	 * We flag the ones we like with 'used'.
	 */
	for (tmpdev = devlist;
	     tmpdev;
	     tmpdev = tmpdev->next) {
		char *devname = tmpdev->devname;
		int dfd;
		struct stat stb;
		struct supertype *tst = dup_super(st);

		if (tmpdev->used > 1) continue;

		if (ident->devices &&
		    !match_oneof(ident->devices, devname)) {
			if (report_missmatch)
				fprintf(stderr, Name ": %s is not one of %s\n", devname, ident->devices);
			continue;
		}

		dfd = dev_open(devname, O_RDONLY|O_EXCL);
		if (dfd < 0) {
			if (report_missmatch)
				fprintf(stderr, Name ": cannot open device %s: %s\n",
					devname, strerror(errno));
			tmpdev->used = 2;
		} else if (fstat(dfd, &stb)< 0) {
			/* Impossible! */
			fprintf(stderr, Name ": fstat failed for %s: %s\n",
				devname, strerror(errno));
			tmpdev->used = 2;
		} else if ((stb.st_mode & S_IFMT) != S_IFBLK) {
			fprintf(stderr, Name ": %s is not a block device.\n",
				devname);
			tmpdev->used = 2;
		} else if (!tst && (tst = guess_super(dfd)) == NULL) {
			if (report_missmatch)
				fprintf(stderr, Name ": no recogniseable superblock on %s\n",
					devname);
			tmpdev->used = 2;
		} else if (tst->ss->load_super(tst,dfd, NULL)) {
			if (report_missmatch)
				fprintf( stderr, Name ": no RAID superblock on %s\n",
					 devname);
		} else if (auto_assem && st == NULL &&
			   !conf_test_metadata(tst->ss->name,
					       tst->ss->match_home(tst, homehost) == 1)) {
			if (report_missmatch)
				fprintf(stderr, Name ": %s has metadata type %s for which "
					"auto-assembly is disabled\n",
					devname, tst->ss->name);
			tst->ss->free_super(tst);
			tmpdev->used = 2;
		} else {
			content = &info;
			memset(content, 0, sizeof(*content));
			tst->ss->getinfo_super(tst, content);
		}
		if (dfd >= 0) close(dfd);

		if (tst && tst->sb && tst->ss->container_content
		    && tst->loaded_container) {
			/* tmpdev is a container.  We need to be either
			 * looking for a member, or auto-assembling
			 */
			if (st) {
				/* already found some components, this cannot
				 * be another one.
				 */
				if (report_missmatch)
					fprintf(stderr, Name ": %s is a container, but we are looking for components\n",
						devname);
				goto loop;
			}

			if (ident->container) {
				if (ident->container[0] == '/' &&
				    !same_dev(ident->container, devname)) {
					if (report_missmatch)
						fprintf(stderr, Name ": %s is not the container required (%s)\n",
							devname, ident->container);
					goto loop;
				}
				if (ident->container[0] != '/') {
					/* we have a uuid */
					int uuid[4];
					if (!parse_uuid(ident->container, uuid) ||
					    !same_uuid(content->uuid, uuid, tst->ss->swapuuid)) {
						if (report_missmatch)
							fprintf(stderr, Name ": %s has wrong UUID to be required container\n",
								devname);
						goto loop;
					}
				}
			}
			/* It is worth looking inside this container.
			 */
			if (verbose > 0)
				fprintf(stderr, Name ": looking in container %s\n",
					devname);
		next_member:
			if (tmpdev->content)
				content = tmpdev->content;
			else
				content = tst->ss->container_content(tst);
			if (!content)
				goto loop; /* empty container */

			tmpdev->content = content->next;
			if (tmpdev->content == NULL)
				tmpdev->used = 2;

		} else if (ident->container || ident->member) {
			/* No chance of this matching if we don't have
			 * a container */
			if (report_missmatch)
				fprintf(stderr, Name "%s is not a container, and one is required.\n",
					devname);
			goto loop;
		}

		if (ident->uuid_set && (!update || strcmp(update, "uuid")!= 0) &&
		    (!tst || !tst->sb ||
		     same_uuid(content->uuid, ident->uuid, tst->ss->swapuuid)==0)) {
			if (report_missmatch)
				fprintf(stderr, Name ": %s has wrong uuid.\n",
					devname);
			goto loop;
		}
		if (ident->name[0] && (!update || strcmp(update, "name")!= 0) &&
		    (!tst || !tst->sb ||
		     name_matches(content->name, ident->name, homehost)==0)) {
			if (report_missmatch)
				fprintf(stderr, Name ": %s has wrong name.\n",
					devname);
			goto loop;
		}
		if (ident->super_minor != UnSet &&
		    (!tst || !tst->sb ||
		     ident->super_minor != content->array.md_minor)) {
			if (report_missmatch)
				fprintf(stderr, Name ": %s has wrong super-minor.\n",
					devname);
			goto loop;
		}
		if (ident->level != UnSet &&
		    (!tst || !tst->sb ||
		     ident->level != content->array.level)) {
			if (report_missmatch)
				fprintf(stderr, Name ": %s has wrong raid level.\n",
					devname);
			goto loop;
		}
		if (ident->raid_disks != UnSet &&
		    (!tst || !tst->sb ||
		     ident->raid_disks!= content->array.raid_disks)) {
			if (report_missmatch)
				fprintf(stderr, Name ": %s requires wrong number of drives.\n",
					devname);
			goto loop;
		}
		if (auto_assem) {
			if (tst == NULL || tst->sb == NULL)
				continue;
		}
		/* If we are this far, then we are nearly commited to this device.
		 * If the super_block doesn't exist, or doesn't match others,
		 * then we probably cannot continue
		 * However if one of the arrays is for the homehost, and
		 * the other isn't that can disambiguate.
		 */

		if (!tst || !tst->sb) {
			fprintf(stderr, Name ": %s has no superblock - assembly aborted\n",
				devname);
			if (st)
				st->ss->free_super(st);
			return 1;
		}

		if (tst && tst->sb && tst->ss->container_content
		    && tst->loaded_container) {
			/* we have the one container we need, don't keep
			 * looking.  If the chosen member is active, skip.
			 */
			if (is_member_busy(content->text_version)) {
				if (report_missmatch)
					fprintf(stderr, Name ": member %s in %s is already assembled\n",
						content->text_version,
						devname);
			skip:
				if (tmpdev->content)
					goto next_member;
				tst->ss->free_super(tst);
				tst = NULL;
				content = NULL;
				if (auto_assem)
					goto loop;
				return 1;
			}
			if (ident->member && ident->member[0]) {
				char *s = strchr(content->text_version+1, '/');
				if (s == NULL) {
					fprintf(stderr, Name ": badly formatted version: %s\n",
						content->text_version);
					goto skip;
				}
				if (strcmp(ident->member, s+1) != 0) {
					if (report_missmatch)
						fprintf(stderr,
							Name ": skipping wrong member %s\n",
							content->text_version);
					goto skip;
				}
			}
			st = tst; tst = NULL;
			if (!auto_assem && inargv && tmpdev->next != NULL) {
				fprintf(stderr, Name ": %s is a container, but is not "
					"only device given: confused and aborting\n",
					devname);
				st->ss->free_super(st);
				return 1;
			}
			if (verbose > 0)
				fprintf(stderr, Name ": found match on member %s in %s\n",
					content->text_version, devname);
			break;
		}
		if (st == NULL)
			st = dup_super(tst);
		if (st->minor_version == -1)
			st->minor_version = tst->minor_version;
		if (st->ss != tst->ss ||
		    st->minor_version != tst->minor_version ||
		    st->ss->compare_super(st, tst) != 0) {
			/* Some mismatch. If exactly one array matches this host,
			 * we can resolve on that one.
			 * Or, if we are auto assembling, we just ignore the second
			 * for now.
			 */
			if (auto_assem)
				goto loop;
			if (homehost) {
				int first = st->ss->match_home(st, homehost);
				int last = tst->ss->match_home(tst, homehost);
				if (first != last &&
				    (first == 1 || last == 1)) {
					/* We can do something */
					if (first) {/* just ignore this one */
						if (report_missmatch)
							fprintf(stderr, Name ": %s misses out due to wrong homehost\n",
								devname);
						goto loop;
					} else { /* reject all those sofar */
						mddev_dev_t td;
						if (report_missmatch)
							fprintf(stderr, Name ": %s overrides previous devices due to good homehost\n",
								devname);
						for (td=devlist; td != tmpdev; td=td->next)
							if (td->used == 1)
								td->used = 0;
						tmpdev->used = 1;
						goto loop;
					}
				}
			}
			fprintf(stderr, Name ": superblock on %s doesn't match others - assembly aborted\n",
				devname);
			tst->ss->free_super(tst);
			st->ss->free_super(st);
			return 1;
		}

		tmpdev->used = 1;

	loop:
		if (tmpdev->content)
			goto next_member;
		if (tst)
			tst->ss->free_super(tst);
	}

	if (!st || !st->sb || !content)
		return 2;

	/* Now need to open the array device.  Use create_mddev */
	if (content == &info)
		st->ss->getinfo_super(st, content);

	trustworthy = FOREIGN;
	name = content->name;
	switch (st->ss->match_home(st, homehost)
		?: st->ss->match_home(st, "any")) {
	case 1:
		trustworthy = LOCAL;
		name = strchr(content->name, ':');
		if (name)
			name++;
		else
			name = content->name;
		break;
	}
	if (!auto_assem)
		/* If the array is listed in mdadm.conf or on
		 * command line, then we trust the name
		 * even if the array doesn't look local
		 */
		trustworthy = LOCAL;

	if (name[0] == 0 &&
	    content->array.level == LEVEL_CONTAINER) {
		name = content->text_version;
		trustworthy = METADATA;
	}

	if (name[0] && trustworthy != LOCAL &&
	    ! require_homehost &&
	    conf_name_is_free(name))
		trustworthy = LOCAL;

	if (trustworthy == LOCAL &&
	    strchr(name, ':'))
		/* Ignore 'host:' prefix of name */
		name = strchr(name, ':')+1;

	mdfd = create_mddev(mddev, name, ident->autof, trustworthy,
			    chosen_name);
	if (mdfd < 0) {
		st->ss->free_super(st);
		free(devices);
		if (auto_assem)
			goto try_again;
		return 1;
	}
	mddev = chosen_name;
	vers = md_get_version(mdfd);
	if (vers < 9000) {
		fprintf(stderr, Name ": Assemble requires driver version 0.90.0 or later.\n"
			"    Upgrade your kernel or try --build\n");
		close(mdfd);
		return 1;
	}
	if (mddev_busy(fd2devnum(mdfd))) {
		fprintf(stderr, Name ": %s already active, cannot restart it!\n",
			mddev);
		for (tmpdev = devlist ;
		     tmpdev && tmpdev->used != 1;
		     tmpdev = tmpdev->next)
			;
		if (tmpdev && auto_assem)
			fprintf(stderr, Name ":   %s needed for %s...\n",
				mddev, tmpdev->devname);
		close(mdfd);
		mdfd = -3;
		st->ss->free_super(st);
		free(devices);
		if (auto_assem)
			goto try_again;
		return 1;
	}
	ioctl(mdfd, STOP_ARRAY, NULL); /* just incase it was started but has no content */

#ifndef MDASSEMBLE
	if (content != &info) {
		/* This is a member of a container.  Try starting the array. */
		return assemble_container_content(st, mdfd, content, runstop,
					   chosen_name, verbose);
	}
#endif
	/* Ok, no bad inconsistancy, we can try updating etc */
	bitmap_done = 0;
	content->update_private = NULL;
	for (tmpdev = devlist; tmpdev; tmpdev=tmpdev->next) if (tmpdev->used == 1) {
		char *devname = tmpdev->devname;
		struct stat stb;
		/* looks like a good enough match to update the super block if needed */
#ifndef MDASSEMBLE
		if (update) {
			int dfd;
			/* prepare useful information in info structures */
			struct stat stb2;
			struct supertype *tst;
			fstat(mdfd, &stb2);

			if (strcmp(update, "uuid")==0 &&
			    !ident->uuid_set) {
				int rfd;
				if ((rfd = open("/dev/urandom", O_RDONLY)) < 0 ||
				    read(rfd, ident->uuid, 16) != 16) {
					*(__u32*)(ident->uuid) = random();
					*(__u32*)(ident->uuid+1) = random();
					*(__u32*)(ident->uuid+2) = random();
					*(__u32*)(ident->uuid+3) = random();
				}
				if (rfd >= 0) close(rfd);
			}
			dfd = dev_open(devname, O_RDWR|O_EXCL);

			tst = dup_super(st);
			if (dfd < 0 || tst->ss->load_super(tst, dfd, NULL) != 0) {
				fprintf(stderr, Name ": cannot re-read metadata from %s - aborting\n",
					devname);
				if (dfd >= 0)
					close(dfd);
				close(mdfd);
				return 1;
			}
			tst->ss->getinfo_super(tst, content);

			memcpy(content->uuid, ident->uuid, 16);
			strcpy(content->name, ident->name);
			content->array.md_minor = minor(stb2.st_rdev);

			tst->ss->update_super(tst, content, update,
					      devname, verbose,
					      ident->uuid_set, homehost);
			if (strcmp(update, "uuid")==0 &&
			    !ident->uuid_set) {
				ident->uuid_set = 1;
				memcpy(ident->uuid, content->uuid, 16);
			}
			if (dfd < 0)
				fprintf(stderr, Name ": Cannot open %s for superblock update\n",
					devname);
			else if (tst->ss->store_super(tst, dfd))
				fprintf(stderr, Name ": Could not re-write superblock on %s.\n",
					devname);
			if (dfd >= 0)
				close(dfd);

			if (strcmp(update, "uuid")==0 &&
			    ident->bitmap_fd >= 0 && !bitmap_done) {
				if (bitmap_update_uuid(ident->bitmap_fd,
						       content->uuid,
						       tst->ss->swapuuid) != 0)
					fprintf(stderr, Name ": Could not update uuid on external bitmap.\n");
				else
					bitmap_done = 1;
			}
			tst->ss->free_super(tst);
		} else
#endif
		{
			struct supertype *tst = dup_super(st);
			int dfd;
			dfd = dev_open(devname, O_RDWR|O_EXCL);

			if (dfd < 0 || tst->ss->load_super(tst, dfd, NULL) != 0) {
				fprintf(stderr, Name ": cannot re-read metadata from %s - aborting\n",
					devname);
				if (dfd >= 0)
					close(dfd);
				close(mdfd);
				return 1;
			}
			tst->ss->getinfo_super(tst, content);
			tst->ss->free_super(tst);
			close(dfd);
		}

		stat(devname, &stb);

		if (verbose > 0)
			fprintf(stderr, Name ": %s is identified as a member of %s, slot %d.\n",
				devname, mddev, content->disk.raid_disk);
		devices[devcnt].devname = devname;
		devices[devcnt].uptodate = 0;
		devices[devcnt].i = *content;
		devices[devcnt].i.disk.major = major(stb.st_rdev);
		devices[devcnt].i.disk.minor = minor(stb.st_rdev);
		if (most_recent < devcnt) {
			if (devices[devcnt].i.events
			    > devices[most_recent].i.events)
				most_recent = devcnt;
		}
		if (content->array.level == LEVEL_MULTIPATH)
			/* with multipath, the raid_disk from the superblock is meaningless */
			i = devcnt;
		else
			i = devices[devcnt].i.disk.raid_disk;
		if (i+1 == 0) {
			if (nextspare < content->array.raid_disks)
				nextspare = content->array.raid_disks;
			i = nextspare++;
		} else {
			if (i >= content->array.raid_disks &&
			    i >= nextspare)
				nextspare = i+1;
		}
		if (i < 10000) {
			if (i >= bestcnt) {
				int newbestcnt = i+10;
				int *newbest = malloc(sizeof(int)*newbestcnt);
				int c;
				for (c=0; c < newbestcnt; c++)
					if (c < bestcnt)
						newbest[c] = best[c];
					else
						newbest[c] = -1;
				if (best)free(best);
				best = newbest;
				bestcnt = newbestcnt;
			}
			if (best[i] >=0 &&
			    devices[best[i]].i.events
			    == devices[devcnt].i.events
			    && (devices[best[i]].i.disk.minor
				!= devices[devcnt].i.disk.minor)
			    && st->ss == &super0
			    && content->array.level != LEVEL_MULTIPATH) {
				/* two different devices with identical superblock.
				 * Could be a mis-detection caused by overlapping
				 * partitions.  fail-safe.
				 */
				fprintf(stderr, Name ": WARNING %s and %s appear"
					" to have very similar superblocks.\n"
					"      If they are really different, "
					"please --zero the superblock on one\n"
					"      If they are the same or overlap,"
					" please remove one from %s.\n",
					devices[best[i]].devname, devname,
					inargv ? "the list" :
					   "the\n      DEVICE list in mdadm.conf"
					);
				close(mdfd);
				return 1;
			}
			if (best[i] == -1
			    || (devices[best[i]].i.events
				< devices[devcnt].i.events))
				best[i] = devcnt;
		}
		devcnt++;
	}
	free(content->update_private);
	content->update_private = NULL;

	if (devcnt == 0) {
		fprintf(stderr, Name ": no devices found for %s\n",
			mddev);
		if (st)
			st->ss->free_super(st);
		close(mdfd);
		return 1;
	}

	if (update && strcmp(update, "byteorder")==0)
		st->minor_version = 90;

	st->ss->getinfo_super(st, content);
	clean = content->array.state & 1;

	/* now we have some devices that might be suitable.
	 * I wonder how many
	 */
	avail = malloc(content->array.raid_disks);
	memset(avail, 0, content->array.raid_disks);
	okcnt = 0;
	sparecnt=0;
	rebuilding_cnt=0;
	for (i=0; i< bestcnt; i++) {
		int j = best[i];
		int event_margin = 1; /* always allow a difference of '1'
				       * like the kernel does
				       */
		if (j < 0) continue;
		/* note: we ignore error flags in multipath arrays
		 * as they don't make sense
		 */
		if (content->array.level != LEVEL_MULTIPATH)
			if (!(devices[j].i.disk.state & (1<<MD_DISK_ACTIVE))) {
				if (!(devices[j].i.disk.state
				      & (1<<MD_DISK_FAULTY)))
					sparecnt++;
				continue;
			}
		if (devices[j].i.events+event_margin >=
		    devices[most_recent].i.events) {
			devices[j].uptodate = 1;
			if (i < content->array.raid_disks) {
				if (devices[j].i.recovery_start == MaxSector) {
					okcnt++;
					avail[i]=1;
				} else
					rebuilding_cnt++;
			} else
				sparecnt++;
		}
	}
	while (force && !enough(content->array.level, content->array.raid_disks,
				content->array.layout, 1,
				avail, okcnt)) {
		/* Choose the newest best drive which is
		 * not up-to-date, update the superblock
		 * and add it.
		 */
		int fd;
		struct supertype *tst;
		unsigned long long current_events;
		chosen_drive = -1;
		for (i = 0; i < content->array.raid_disks && i < bestcnt; i++) {
			int j = best[i];
			if (j>=0 &&
			    !devices[j].uptodate &&
			    devices[j].i.recovery_start == MaxSector &&
			    (chosen_drive < 0 ||
			     devices[j].i.events
			     > devices[chosen_drive].i.events))
				chosen_drive = j;
		}
		if (chosen_drive < 0)
			break;
		current_events = devices[chosen_drive].i.events;
	add_another:
		if (verbose >= 0)
			fprintf(stderr, Name ": forcing event count in %s(%d) from %d upto %d\n",
				devices[chosen_drive].devname,
				devices[chosen_drive].i.disk.raid_disk,
				(int)(devices[chosen_drive].i.events),
				(int)(devices[most_recent].i.events));
		fd = dev_open(devices[chosen_drive].devname, O_RDWR|O_EXCL);
		if (fd < 0) {
			fprintf(stderr, Name ": Couldn't open %s for write - not updating\n",
				devices[chosen_drive].devname);
			devices[chosen_drive].i.events = 0;
			continue;
		}
		tst = dup_super(st);
		if (tst->ss->load_super(tst,fd, NULL)) {
			close(fd);
			fprintf(stderr, Name ": RAID superblock disappeared from %s - not updating.\n",
				devices[chosen_drive].devname);
			devices[chosen_drive].i.events = 0;
			continue;
		}
		content->events = devices[most_recent].i.events;
		tst->ss->update_super(tst, content, "force-one",
				     devices[chosen_drive].devname, verbose,
				     0, NULL);

		if (tst->ss->store_super(tst, fd)) {
			close(fd);
			fprintf(stderr, Name ": Could not re-write superblock on %s\n",
				devices[chosen_drive].devname);
			devices[chosen_drive].i.events = 0;
			tst->ss->free_super(tst);
			continue;
		}
		close(fd);
		devices[chosen_drive].i.events = devices[most_recent].i.events;
		devices[chosen_drive].uptodate = 1;
		avail[chosen_drive] = 1;
		okcnt++;
		tst->ss->free_super(tst);

		/* If there are any other drives of the same vintage,
		 * add them in as well.  We can't lose and we might gain
		 */
		for (i = 0; i < content->array.raid_disks && i < bestcnt ; i++) {
			int j = best[i];
			if (j >= 0 &&
			    !devices[j].uptodate &&
			    devices[j].i.events == current_events) {
				chosen_drive = j;
				goto add_another;
			}
		}
	}

	/* Now we want to look at the superblock which the kernel will base things on
	 * and compare the devices that we think are working with the devices that the
	 * superblock thinks are working.
	 * If there are differences and --force is given, then update this chosen
	 * superblock.
	 */
	chosen_drive = -1;
	st->ss->free_super(st);
	for (i=0; chosen_drive < 0 && i<bestcnt; i++) {
		int j = best[i];
		int fd;

		if (j<0)
			continue;
		if (!devices[j].uptodate)
			continue;
		chosen_drive = j;
		if ((fd=dev_open(devices[j].devname, O_RDONLY|O_EXCL))< 0) {
			fprintf(stderr, Name ": Cannot open %s: %s\n",
				devices[j].devname, strerror(errno));
			close(mdfd);
			return 1;
		}
		if (st->ss->load_super(st,fd, NULL)) {
			close(fd);
			fprintf(stderr, Name ": RAID superblock has disappeared from %s\n",
				devices[j].devname);
			close(mdfd);
			return 1;
		}
		close(fd);
	}
	if (st->sb == NULL) {
		fprintf(stderr, Name ": No suitable drives found for %s\n", mddev);
		close(mdfd);
		return 1;
	}
	st->ss->getinfo_super(st, content);
#ifndef MDASSEMBLE
	sysfs_init(content, mdfd, 0);
#endif
	for (i=0; i<bestcnt; i++) {
		int j = best[i];
		unsigned int desired_state;

		if (i < content->array.raid_disks)
			desired_state = (1<<MD_DISK_ACTIVE) | (1<<MD_DISK_SYNC);
		else
			desired_state = 0;

		if (j<0)
			continue;
		if (!devices[j].uptodate)
			continue;

		devices[j].i.disk.state = desired_state;
		if (!(devices[j].i.array.state & 1))
			clean = 0;

		if (st->ss->update_super(st, &devices[j].i, "assemble", NULL,
					 verbose, 0, NULL)) {
			if (force) {
				if (verbose >= 0)
					fprintf(stderr, Name ": "
						"clearing FAULTY flag for device %d in %s for %s\n",
						j, mddev, devices[j].devname);
				change = 1;
			} else {
				if (verbose >= -1)
					fprintf(stderr, Name ": "
						"device %d in %s has wrong state in superblock, but %s seems ok\n",
						i, mddev, devices[j].devname);
			}
		}
#if 0
		if (!(super.disks[i].i.disk.state & (1 << MD_DISK_FAULTY))) {
			fprintf(stderr, Name ": devices %d of %s is not marked FAULTY in superblock, but cannot be found\n",
				i, mddev);
		}
#endif
	}
	if (force && !clean &&
	    !enough(content->array.level, content->array.raid_disks,
		    content->array.layout, clean,
		    avail, okcnt)) {
		change += st->ss->update_super(st, content, "force-array",
					devices[chosen_drive].devname, verbose,
					       0, NULL);
		clean = 1;
	}

	if (change) {
		int fd;
		fd = dev_open(devices[chosen_drive].devname, O_RDWR|O_EXCL);
		if (fd < 0) {
			fprintf(stderr, Name ": Could not open %s for write - cannot Assemble array.\n",
				devices[chosen_drive].devname);
			close(mdfd);
			return 1;
		}
		if (st->ss->store_super(st, fd)) {
			close(fd);
			fprintf(stderr, Name ": Could not re-write superblock on %s\n",
				devices[chosen_drive].devname);
			close(mdfd);
			return 1;
		}
		close(fd);
	}

	/* If we are in the middle of a reshape we may need to restore saved data
	 * that was moved aside due to the reshape overwriting live data
	 * The code of doing this lives in Grow.c
	 */
#ifndef MDASSEMBLE
	if (content->reshape_active) {
		int err = 0;
		int *fdlist = malloc(sizeof(int)* bestcnt);
		if (verbose > 0)
			fprintf(stderr, Name ":%s has an active reshape - checking "
				"if critical section needs to be restored\n",
				chosen_name);
		for (i=0; i<bestcnt; i++) {
			int j = best[i];
			if (j >= 0) {
				fdlist[i] = dev_open(devices[j].devname, O_RDWR|O_EXCL);
				if (fdlist[i] < 0) {
					fprintf(stderr, Name ": Could not open %s for write - cannot Assemble array.\n",
						devices[j].devname);
					err = 1;
					break;
				}
			} else
				fdlist[i] = -1;
		}
		if (!err)
			err = Grow_restart(st, content, fdlist, bestcnt, backup_file, verbose > 0);
		while (i>0) {
			i--;
			if (fdlist[i]>=0) close(fdlist[i]);
		}
		if (err) {
			fprintf(stderr, Name ": Failed to restore critical section for reshape, sorry.\n");
			if (backup_file == NULL)
				fprintf(stderr,"      Possibly you needed to specify the --backup-file\n");
			close(mdfd);
			return err;
		}
	}
#endif
	/* count number of in-sync devices according to the superblock.
	 * We must have this number to start the array without -s or -R
	 */
	req_cnt = content->array.working_disks;

	/* Almost ready to actually *do* something */
	if (!old_linux) {
		int rv;

		/* First, fill in the map, so that udev can find our name
		 * as soon as we become active.
		 */
		map_update(NULL, fd2devnum(mdfd), content->text_version,
			   content->uuid, chosen_name);

		rv = set_array_info(mdfd, st, content);
		if (rv) {
			fprintf(stderr, Name ": failed to set array info for %s: %s\n",
				mddev, strerror(errno));
			ioctl(mdfd, STOP_ARRAY, NULL);
			close(mdfd);
			return 1;
		}
		if (ident->bitmap_fd >= 0) {
			if (ioctl(mdfd, SET_BITMAP_FILE, ident->bitmap_fd) != 0) {
				fprintf(stderr, Name ": SET_BITMAP_FILE failed.\n");
				ioctl(mdfd, STOP_ARRAY, NULL);
				close(mdfd);
				return 1;
			}
		} else if (ident->bitmap_file) {
			/* From config file */
			int bmfd = open(ident->bitmap_file, O_RDWR);
			if (bmfd < 0) {
				fprintf(stderr, Name ": Could not open bitmap file %s\n",
					ident->bitmap_file);
				ioctl(mdfd, STOP_ARRAY, NULL);
				close(mdfd);
				return 1;
			}
			if (ioctl(mdfd, SET_BITMAP_FILE, bmfd) != 0) {
				fprintf(stderr, Name ": Failed to set bitmapfile for %s\n", mddev);
				close(bmfd);
				ioctl(mdfd, STOP_ARRAY, NULL);
				close(mdfd);
				return 1;
			}
			close(bmfd);
		}

		/* First, add the raid disks, but add the chosen one last */
		for (i=0; i<= bestcnt; i++) {
			int j;
			if (i < bestcnt) {
				j = best[i];
				if (j == chosen_drive)
					continue;
			} else
				j = chosen_drive;

			if (j >= 0 /* && devices[j].uptodate */) {
				int dfd = dev_open(devices[j].devname,
						   O_RDWR|O_EXCL);
				if (dfd >= 0) {
					remove_partitions(dfd);
					close(dfd);
				}
				rv = add_disk(mdfd, st, content, &devices[j].i);

				if (rv) {
					fprintf(stderr, Name ": failed to add "
						        "%s to %s: %s\n",
						devices[j].devname,
						mddev,
						strerror(errno));
					if (i < content->array.raid_disks
					    || i == bestcnt)
						okcnt--;
					else
						sparecnt--;
				} else if (verbose > 0)
					fprintf(stderr, Name ": added %s "
						        "to %s as %d\n",
						devices[j].devname, mddev,
						devices[j].i.disk.raid_disk);
			} else if (verbose > 0 && i < content->array.raid_disks)
				fprintf(stderr, Name ": no uptodate device for "
					        "slot %d of %s\n",
					i, mddev);
		}

		if (content->array.level == LEVEL_CONTAINER) {
			if (verbose >= 0) {
				fprintf(stderr, Name ": Container %s has been "
					"assembled with %d drive%s",
					mddev, okcnt+sparecnt, okcnt+sparecnt==1?"":"s");
				if (okcnt < (unsigned)content->array.raid_disks)
					fprintf(stderr, " (out of %d)",
						content->array.raid_disks);
				fprintf(stderr, "\n");
			}
			sysfs_uevent(content, "change");
			wait_for(chosen_name, mdfd);
			close(mdfd);
			return 0;
		}

		if (runstop == 1 ||
		    (runstop <= 0 &&
		     ( enough(content->array.level, content->array.raid_disks,
			      content->array.layout, clean, avail, okcnt) &&
		       (okcnt + rebuilding_cnt >= req_cnt || start_partial_ok)
			     ))) {
			/* This array is good-to-go.
			 * If a reshape is in progress then we might need to
			 * continue monitoring it.  In that case we start
			 * it read-only and let the grow code make it writable.
			 */
			int rv;
#ifndef MDASSEMBLE
			if (content->reshape_active &&
			    content->delta_disks <= 0)
				rv = Grow_continue(mdfd, st, content, backup_file);
			else
#endif
				rv = ioctl(mdfd, RUN_ARRAY, NULL);
			if (rv == 0) {
				if (verbose >= 0) {
					fprintf(stderr, Name ": %s has been started with %d drive%s",
						mddev, okcnt, okcnt==1?"":"s");
					if (okcnt < (unsigned)content->array.raid_disks)
						fprintf(stderr, " (out of %d)", content->array.raid_disks);
					if (rebuilding_cnt)
						fprintf(stderr, "%s %d rebuilding", sparecnt?",":" and", rebuilding_cnt);
					if (sparecnt)
						fprintf(stderr, " and %d spare%s", sparecnt, sparecnt==1?"":"s");
					fprintf(stderr, ".\n");
				}
				if (content->reshape_active &&
				    content->array.level >= 4 &&
				    content->array.level <= 6) {
					/* might need to increase the size
					 * of the stripe cache - default is 256
					 */
					if (256 < 4 * (content->array.chunk_size/4096)) {
						struct mdinfo *sra = sysfs_read(mdfd, 0, 0);
						if (sra)
							sysfs_set_num(sra, NULL,
								      "stripe_cache_size",
								      (4 * content->array.chunk_size / 4096) + 1);
					}
				}
				wait_for(mddev, mdfd);
				close(mdfd);
				if (auto_assem) {
					int usecs = 1;
					/* There is a nasty race with 'mdadm --monitor'.
					 * If it opens this device before we close it,
					 * it gets an incomplete open on which IO
					 * doesn't work and the capacity is
					 * wrong.
					 * If we reopen (to check for layered devices)
					 * before --monitor closes, we loose.
					 *
					 * So: wait upto 1 second for there to be
					 * a non-zero capacity.
					 */
					while (usecs < 1000) {
						mdfd = open(mddev, O_RDONLY);
						if (mdfd >= 0) {
							unsigned long long size;
							if (get_dev_size(mdfd, NULL, &size) &&
							    size > 0)
								break;
							close(mdfd);
						}
						usleep(usecs);
						usecs <<= 1;
					}
				}
				return 0;
			}
			fprintf(stderr, Name ": failed to RUN_ARRAY %s: %s\n",
				mddev, strerror(errno));

			if (!enough(content->array.level, content->array.raid_disks,
				    content->array.layout, 1, avail, okcnt))
				fprintf(stderr, Name ": Not enough devices to "
					"start the array.\n");
			else if (!enough(content->array.level,
					 content->array.raid_disks,
					 content->array.layout, clean,
					 avail, okcnt))
				fprintf(stderr, Name ": Not enough devices to "
					"start the array while not clean "
					"- consider --force.\n");

			if (auto_assem)
				ioctl(mdfd, STOP_ARRAY, NULL);
			close(mdfd);
			return 1;
		}
		if (runstop == -1) {
			fprintf(stderr, Name ": %s assembled from %d drive%s",
				mddev, okcnt, okcnt==1?"":"s");
			if (okcnt != (unsigned)content->array.raid_disks)
				fprintf(stderr, " (out of %d)", content->array.raid_disks);
			fprintf(stderr, ", but not started.\n");
			close(mdfd);
			return 0;
		}
		if (verbose >= -1) {
			fprintf(stderr, Name ": %s assembled from %d drive%s", mddev, okcnt, okcnt==1?"":"s");
			if (rebuilding_cnt)
				fprintf(stderr, "%s %d rebuilding", sparecnt?", ":" and ", rebuilding_cnt);
			if (sparecnt)
				fprintf(stderr, " and %d spare%s", sparecnt, sparecnt==1?"":"s");
			if (!enough(content->array.level, content->array.raid_disks,
				    content->array.layout, 1, avail, okcnt))
				fprintf(stderr, " - not enough to start the array.\n");
			else if (!enough(content->array.level,
					 content->array.raid_disks,
					 content->array.layout, clean,
					 avail, okcnt))
				fprintf(stderr, " - not enough to start the "
					"array while not clean - consider "
					"--force.\n");
			else {
				if (req_cnt == (unsigned)content->array.raid_disks)
					fprintf(stderr, " - need all %d to start it", req_cnt);
				else
					fprintf(stderr, " - need %d of %d to start", req_cnt, content->array.raid_disks);
				fprintf(stderr, " (use --run to insist).\n");
			}
		}
		if (auto_assem)
			ioctl(mdfd, STOP_ARRAY, NULL);
		close(mdfd);
		return 1;
	} else {
		/* The "chosen_drive" is a good choice, and if necessary, the superblock has
		 * been updated to point to the current locations of devices.
		 * so we can just start the array
		 */
		unsigned long dev;
		dev = makedev(devices[chosen_drive].i.disk.major,
			    devices[chosen_drive].i.disk.minor);
		if (ioctl(mdfd, START_ARRAY, dev)) {
		    fprintf(stderr, Name ": Cannot start array: %s\n",
			    strerror(errno));
		}

	}
	close(mdfd);
	return 0;
}

#ifndef MDASSEMBLE
int assemble_container_content(struct supertype *st, int mdfd,
			       struct mdinfo *content, int runstop,
			       char *chosen_name, int verbose)
{
	struct mdinfo *dev, *sra;
	int working = 0, preexist = 0;
	struct map_ent *map = NULL;

	sysfs_init(content, mdfd, 0);

	sra = sysfs_read(mdfd, 0, GET_VERSION);
	if (sra == NULL || strcmp(sra->text_version, content->text_version) != 0)
		if (sysfs_set_array(content, md_get_version(mdfd)) != 0) {
			close(mdfd);
			return 1;
		}
	if (sra)
		sysfs_free(sra);

	for (dev = content->devs; dev; dev = dev->next)
		if (sysfs_add_disk(content, dev, 1) == 0)
			working++;
		else if (errno == EEXIST)
			preexist++;
	if (working == 0) {
		close(mdfd);
		return 1;/* Nothing new, don't try to start */
	}
	
	map_update(&map, fd2devnum(mdfd),
		   content->text_version,
		   content->uuid, chosen_name);

	if (runstop > 0 ||
		 (working + preexist) >= content->array.working_disks) {
		int err;

		switch(content->array.level) {
		case LEVEL_LINEAR:
		case LEVEL_MULTIPATH:
		case 0:
			err = sysfs_set_str(content, NULL, "array_state",
					    "active");
			break;
		default:
			err = sysfs_set_str(content, NULL, "array_state",
				      "readonly");
			/* start mdmon if needed. */
			if (!err) {
				if (!mdmon_running(st->container_dev))
					start_mdmon(st->container_dev);
				ping_monitor(devnum2devname(st->container_dev));
			}
			break;
		}
		if (!err)
			sysfs_set_safemode(content, content->safe_mode_delay);
		if (verbose >= 0) {
			if (err)
				fprintf(stderr, Name
					": array %s now has %d devices",
					chosen_name, working + preexist);
			else
				fprintf(stderr, Name
					": Started %s with %d devices",
					chosen_name, working + preexist);
			if (preexist)
				fprintf(stderr, " (%d new)", working);
			fprintf(stderr, "\n");
		}
		if (!err)
			wait_for(chosen_name, mdfd);
		close(mdfd);
		return 0;
		/* FIXME should have an O_EXCL and wait for read-auto */
	} else {
		if (verbose >= 0)
			fprintf(stderr, Name
				": %s assembled with %d devices but "
				"not started\n",
				chosen_name, working);
		close(mdfd);
		return 1;
	}
}
#endif

