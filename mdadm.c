/*
 * mdadm - manage Linux "md" devices aka RAID arrays.
 *
 * Copyright (C) 2001-2002 Neil Brown <neilb@cse.unsw.edu.au>
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
 *    Email: <neilb@cse.unsw.edu.au>
 *    Paper: Neil Brown
 *           School of Computer Science and Engineering
 *           The University of New South Wales
 *           Sydney, 2052
 *           Australia
 */

#include "mdadm.h"
#include "md_p.h"

int open_mddev(char *dev)
{
	int mdfd = open(dev, O_RDWR, 0);
	if (mdfd < 0)
		fprintf(stderr, Name ": error opening %s: %s\n",
			dev, strerror(errno));
	else if (md_get_version(mdfd) <= 0) {
		fprintf(stderr, Name ": %s does not appear to be an md device\n",
			dev);
		close(mdfd);
		mdfd = -1;
	}
	return mdfd;
}



int main(int argc, char *argv[])
{
	int mode = 0;
	int opt;
	int option_index;
	char *help_text;
	char *c;
	int rv;

	int chunk = 0;
	int size = 0;
	int level = -10;
	int layout = -1;
	int raiddisks = 0;
	int sparedisks = 0;
	struct mddev_ident_s ident;
	char *configfile = NULL;
	char *cp;
	char *update = NULL;
	int scan = 0;
	char devmode = 0;
	int runstop = 0;
	int readonly = 0;
	int SparcAdjust = 0;
	mddev_dev_t devlist = NULL;
	mddev_dev_t *devlistend = & devlist;
	mddev_dev_t dv;
	int devs_found = 0;
	int verbose = 0;
	int brief = 0;
	int force = 0;

	char *mailaddr = NULL;
	char *program = NULL;
	int delay = 0;

	int mdfd = -1;

	ident.uuid_set=0;
	ident.level = -10;
	ident.raid_disks = -1;
	ident.super_minor= -1;
	ident.devices=0;

	while ((option_index = -1) ,
	       (opt=getopt_long(argc, argv,
				short_options, long_options,
				&option_index)) != -1) {
		int newmode = mode;
		/* firstly, so mode-independant options */
		switch(opt) {
		case 'h':
			help_text = Help;
			switch (mode) {
			case ASSEMBLE : help_text = Help_assemble; break;
			case BUILD    : help_text = Help_build; break;
			case CREATE   : help_text = Help_create; break;
			case MANAGE   : help_text = Help_manage; break;
			case MISC     : help_text = Help_misc; break;
			case MONITOR  : help_text = Help_monitor; break;
			}
			fputs(help_text,stderr);
			exit(0);

		case 'V':
			fputs(Version, stderr);
			exit(0);

		case 'v': verbose = 1;
			continue;

		case 'b': brief = 1;
			continue;

		case ':':
		case '?':
			fputs(Usage, stderr);
			exit(2);
		}
		/* second, figure out the mode.
		 * Some options force the mode.  Others
		 * set the mode if it isn't already 
		 */

		switch(opt) {
		case '@': /* just incase they say --manage */
			newmode = MANAGE; break;
		case 'a':
		case 'r':
		case 'f':
		case 1 : if (!mode) newmode = MANAGE; break;

		case 'A': newmode = ASSEMBLE; break;
		case 'B': newmode = BUILD; break;
		case 'C': newmode = CREATE; break;
		case 'F': newmode = MONITOR;break;

		case '#':
		case 'D':
		case 'E':
		case 'Q': newmode = MISC; break;
		case 'R':
		case 'S':
		case 'o':
		case 'w':
		case 'K': if (!mode) newmode = MISC; break;
		}
		if (mode && newmode == mode) {
			/* everybody happy ! */
		} else if (mode && newmode != mode) {
			/* not allowed.. */
			fprintf(stderr, Name ": ");
			if (option_index >= 0)
				fprintf(stderr, "--%s", long_options[option_index].name);
			else
				fprintf(stderr, "-%c", opt);
			fprintf(stderr, " would set mode to %s, but it is already %s.\n",
				map_num(modes, newmode),
				map_num(modes, mode));
			exit(2);
		} else if (!mode && newmode) {
			mode = newmode;
		} else {
			/* special case of -c --help */
			if (opt == 'c' && 
			    ( strncmp(optarg, "--h", 3)==0 ||
			      strncmp(optarg, "-h", 2)==0)) {
				fputs(Help_config, stderr);
				exit(0);
			}
			if (option_index >= 0)
				fprintf(stderr, "--%s", long_options[option_index].name);
			else
				fprintf(stderr, "-%c", opt);
			fprintf(stderr, " does not set the mode, and so cannot be first.\n");
			exit(2);
		}

		/* if we just set the mode, then done */
		switch(opt) {
		case '@':
		case '#':
		case 'A':
		case 'B':
		case 'C':
		case 'F':
			continue;
		}
		if (opt == 1) {
		        /* an undecorated option - must be a device name.
			 */
			if (devs_found > 0 && mode == '@' && !devmode) {
				fprintf(stderr, Name ": Must give on of -a/-r/-f for subsequent devices at %s\n", optarg);
				exit(2);
			}
			dv = malloc(sizeof(*dv));
			if (dv == NULL) {
				fprintf(stderr, Name ": malloc failed\n");
				exit(3);
			}
			dv->devname = optarg;
			dv->disposition = devmode;
			dv->next = NULL;
			*devlistend = dv;
			devlistend = &dv->next;
			
			devs_found++;
			continue;
		}

		/* We've got a mode, and opt is now something else which
		 * could depend on the mode */
#define O(a,b) ((a<<8)|b)
		switch (O(mode,opt)) {
		case O(CREATE,'c'):
		case O(BUILD,'c'): /* chunk or rounding */
			if (chunk) {
				fprintf(stderr, Name ": chunk/rounding may only be specified once. "
					"Second value is %s.\n", optarg);
				exit(2);
			}
			chunk = strtol(optarg, &c, 10);
			if (!optarg[0] || *c || chunk<4 || ((chunk-1)&chunk)) {
				fprintf(stderr, Name ": invalid chunk/rounding value: %s\n",
					optarg);
				exit(2);
			}
			continue;

		case O(CREATE,'z'): /* size */
			if (size) {
				fprintf(stderr, Name ": size may only be specified once. "
					"Second value is %s.\n", optarg);
				exit(2);
			}
			size = strtol(optarg, &c, 10);
			if (!optarg[0] || *c || size < 4) {
				fprintf(stderr, Name ": invalid size: %s\n",
					optarg);
				exit(2);
			}
			continue;

		case O(CREATE,'l'):
		case O(BUILD,'l'): /* set raid level*/
			if (level != -10) {
				fprintf(stderr, Name ": raid level may only be set once.  "
					"Second value is %s.\n", optarg);
				exit(2);
			}
			level = map_name(pers, optarg);
			if (level == -10) {
				fprintf(stderr, Name ": invalid raid level: %s\n",
					optarg);
				exit(2);
			}
			if (level != 0 && level != -1 && mode == BUILD) {
				fprintf(stderr, Name ": Raid level %s not permitted with --build.\n",
					optarg);
				exit(2);
			}
			if (sparedisks > 0 && level < 1 && level >= -1) {
				fprintf(stderr, Name ": raid level %s is incompatible with spare-devices setting.\n",
					optarg);
				exit(2);
			}
			ident.level = level;
			continue;

		case O(CREATE,'p'): /* raid5 layout */
			if (layout >= 0) {
				fprintf(stderr,Name ": layout may only be sent once.  "
					"Second value was %s\n", optarg);
				exit(2);
			}
			switch(level) {
			default:
				fprintf(stderr, Name ": layout not meaningful for %s arrays.\n",
					map_num(pers, level));
				exit(2);
			case -10:
				fprintf(stderr, Name ": raid level must be given before layout.\n");
				exit(2);

			case 5:
				layout = map_name(r5layout, optarg);
				if (layout==-10) {
					fprintf(stderr, Name ": layout %s not understood for raid5.\n",
						optarg);
					exit(2);
				}
				break;
			}
			continue;

		case O(CREATE,'n'):
		case O(BUILD,'n'): /* number of raid disks */
			if (raiddisks) {
				fprintf(stderr, Name ": raid-devices set twice: %d and %s\n",
					raiddisks, optarg);
				exit(2);
			}
			raiddisks = strtol(optarg, &c, 10);
			if (!optarg[0] || *c || raiddisks<=0 || raiddisks > MD_SB_DISKS) {
				fprintf(stderr, Name ": invalid number of raid devices: %s\n",
					optarg);
				exit(2);
			}
			ident.raid_disks = raiddisks;
			continue;

		case O(CREATE,'x'): /* number of spare (eXtra) discs */
			if (sparedisks) {
				fprintf(stderr,Name ": spare-devices set twice: %d and %s\n",
					sparedisks, optarg);
				exit(2);
			}
			if (level > -10 && level <= 0 && level >= -1) {
				fprintf(stderr, Name ": spare-devices setting is incompatible with raid level %d\n",
					level);
				exit(2);
			}
			sparedisks = strtol(optarg, &c, 10);
			if (!optarg[0] || *c || sparedisks < 0 || sparedisks > MD_SB_DISKS - raiddisks) {
				fprintf(stderr, Name ": invalid number of spare-devices: %s\n",
					optarg);
				exit(2);
			}
			continue;
		case O(CREATE,'f'): /* force honouring of device list */
		case O(ASSEMBLE,'f'): /* force assembly */
		case O(MISC,'f'): /* force zero */
			force=1;
			continue;

			/* now for the Assemble options */
		case O(ASSEMBLE,'u'): /* uuid of array */
			if (ident.uuid_set) {
				fprintf(stderr, Name ": uuid cannot be set twice.  "
					"Second value %s.\n", optarg);
				exit(2);
			}
			if (parse_uuid(optarg, ident.uuid))
				ident.uuid_set = 1;
			else {
				fprintf(stderr,Name ": Bad uuid: %s\n", optarg);
				exit(2);
			}
			continue;

		case O(ASSEMBLE,'m'): /* super-minor for array */
			if (ident.super_minor >= 0) {
				fprintf(stderr, Name ": super-minor cannot be set twice.  "
					"Second value: %s.\n", optarg);
				exit(2);
			}
			ident.super_minor = strtoul(optarg, &cp, 10);
			if (!optarg[0] || *cp) {
				fprintf(stderr, Name ": Bad super-minor number: %s.\n", optarg);
				exit(2);
			}
			continue;

		case O(ASSEMBLE,'U'): /* update the superblock */
			if (update) {
				fprintf(stderr, Name ": Can only update one aspect of superblock, both %s and %s given.\n",
					update, optarg);
				exit(2);
			}
			update = optarg;
			if (strcmp(update, "sparc2.2")==0) continue;
			if (strcmp(update, "super-minor") == 0)
				continue;
			fprintf(stderr, Name ": '--update %s' invalid.  Only 'sparc2.2' or 'super-minor' supported\n",update);
			exit(2);

		case O(ASSEMBLE,'c'): /* config file */
		case O(MISC, 'c'):
		case O(MONITOR,'c'):
			if (configfile) {
				fprintf(stderr, Name ": configfile cannot be set twice.  "
					"Second value is %s.\n", optarg);
				exit(2);
			}
			configfile = optarg;
			/* FIXME possibly check that config file exists.  Even parse it */
			continue;
		case O(ASSEMBLE,'s'): /* scan */
		case O(MISC,'s'):
		case O(MONITOR,'s'):
			scan = 1;
			continue;

		case O(MONITOR,'m'): /* mail address */
			if (mailaddr)
				fprintf(stderr, Name ": only specify one mailaddress. %s ignored.\n",
					optarg);
			else
				mailaddr = optarg;
			continue;

		case O(MONITOR,'p'): /* alert program */
			if (program)
				fprintf(stderr, Name ": only specify one alter program. %s ignored.\n",
					optarg);
			else
				program = optarg;
			continue;

		case O(MONITOR,'d'): /* delay in seconds */
			if (delay)
				fprintf(stderr, Name ": only specify delay once. %s ignored.\n",
					optarg);
			else {
				delay = strtol(optarg, &c, 10);
				if (!optarg[0] || *c || delay<1) {
					fprintf(stderr, Name ": invalid delay: %s\n",
						optarg);
					exit(2);
				}
			}
			continue;
			

			/* now the general management options.  Some are applicable
			 * to other modes. None have arguments.
			 */
		case O(MANAGE,'a'):
		case O(CREATE,'a'):
		case O(BUILD,'a'):
		case O(ASSEMBLE,'a'): /* add a drive */
			devmode = 'a';
			continue;
		case O(MANAGE,'r'): /* remove a drive */
			devmode = 'r';
			continue;
		case O(MANAGE,'f'): /* set faulty */
			devmode = 'f';
			continue;
		case O(MANAGE,'R'):
		case O(ASSEMBLE,'R'):
		case O(BUILD,'R'):
		case O(CREATE,'R'): /* Run the array */
			if (runstop < 0) {
				fprintf(stderr, Name ": Cannot both Stop and Run an array\n");
				exit(2);
			}
			runstop = 1;
			continue;
		case O(MANAGE,'S'):
			if (runstop > 0) {
				fprintf(stderr, Name ": Cannot both Run and Stop an array\n");
				exit(2);
			}
			runstop = -1;
			continue;

		case O(MANAGE,'o'):
			if (readonly < 0) {
				fprintf(stderr, Name ": Cannot have both readonly and readwrite\n");
				exit(2);
			}
			readonly = 1;
			continue;
		case O(MANAGE,'w'):
			if (readonly > 0) {
				fprintf(stderr, Name ": Cannot have both readwrite and readonly.\n");
				exit(2);
			}
			readonly = -1;
			continue;

		case O(MISC,'Q'):
		case O(MISC,'D'):
		case O(MISC,'E'):
		case O(MISC,'K'):
		case O(MISC,'R'):
		case O(MISC,'S'):
		case O(MISC,'o'):
		case O(MISC,'w'):
			if (devmode && devmode != opt &&
			    (devmode == 'E' || (opt == 'E' && devmode != 'Q'))) {
				fprintf(stderr, Name ": --examine/-E cannot be given with -%c\n",
					devmode =='E'?opt:devmode);
				exit(2);
			}
			devmode = opt;
			continue;

		case O(MISC, 22):
			if (devmode != 'E') {
				fprintf(stderr, Name ": --sparc2.2 only allowed with --examine\n");
				exit(2);
			}
			SparcAdjust = 1;
			continue;
		}
		/* We have now processed all the valid options. Anything else is
		 * an error
		 */
		fprintf(stderr, Name ": option %c not valid in %s mode\n",
			opt, map_num(modes, mode));
		exit(2);

	}

	if (!mode) {
		fputs(Usage, stderr);
		exit(2);
	}
	/* Ok, got the option parsing out of the way
	 * hopefully it's mostly right but there might be some stuff
	 * missing
	 *
	 * That is mosty checked in the per-mode stuff but...
	 *
	 * For @,B,C  and A without -s, the first device listed must be an md device
	 * we check that here and open it.
	 */

	if (mode==MANAGE || mode == BUILD || mode == CREATE || (mode == ASSEMBLE && ! scan)) {
		if (devs_found < 1) {
			fprintf(stderr, Name ": an md device must be given in this mode\n");
			exit(2);
		}
		mdfd = open_mddev(devlist->devname);
		if (mdfd < 0)
			exit(1);
	}

	rv = 0;
	switch(mode) {
	case MANAGE:
		/* readonly, add/remove, readwrite, runstop */
		if (readonly>0)
			rv = Manage_ro(devlist->devname, mdfd, readonly);
		if (!rv && devs_found>1)
			rv = Manage_subdevs(devlist->devname, mdfd,
					    devlist->next);
		if (!rv && readonly < 0)
			rv = Manage_ro(devlist->devname, mdfd, readonly);
		if (!rv && runstop)
			rv = Manage_runstop(devlist->devname, mdfd, runstop);
		break;
	case ASSEMBLE:
		if (!scan)
			rv = Assemble(devlist->devname, mdfd, &ident, configfile,
				      devlist->next,
				      readonly, runstop, update, verbose, force);
		else if (devs_found>0) {
			if (update && devs_found > 1) {
				fprintf(stderr, Name ": can only update a single array at a time\n");
				exit(1);
			}
			for (dv = devlist ; dv ; dv=dv->next) {
				mddev_ident_t array_ident = conf_get_ident(configfile, dv->devname);
				mdfd = open_mddev(dv->devname);
				if (mdfd < 0) {
					rv |= 1;
					continue;
				}
				if (array_ident == NULL) {
					fprintf(stderr, Name ": %s not identified in config file.\n",
						dv->devname);
					rv |= 1;
					continue;
				}
				rv |= Assemble(dv->devname, mdfd, array_ident, configfile,
					       NULL,
					       readonly, runstop, update, verbose, force);
			}
		} else {
			mddev_ident_t array_list =  conf_get_ident(configfile, NULL);
			if (!array_list) {
				fprintf(stderr, Name ": No arrays found in config file\n");
				rv = 1;
			} else
				for (; array_list; array_list = array_list->next) {
					mdu_array_info_t array;
					mdfd = open_mddev(array_list->devname);
					if (mdfd < 0) {
						rv |= 1;
						continue;
					}
					if (ioctl(mdfd, GET_ARRAY_INFO, &array)>=0)
						/* already assembled, skip */
						continue;
					rv |= Assemble(array_list->devname, mdfd,
						       array_list, configfile,
						       NULL,
						       readonly, runstop, NULL, verbose, force);
				}
		}
		break;
	case BUILD:
		rv = Build(devlist->devname, mdfd, chunk, level, raiddisks, devlist->next);
		break;
	case CREATE:
		rv = Create(devlist->devname, mdfd, chunk, level, layout, size,
			    raiddisks, sparedisks,
			    devs_found-1, devlist->next, runstop, verbose, force);
		break;
	case MISC:

		if (devmode == 'E') {
			if (devlist == NULL && !scan) {
				fprintf(stderr, Name ": No devices to examine\n");
				exit(2);
			}
			if (devlist == NULL)
				devlist = conf_get_devs(configfile);
			if (devlist == NULL) {
				fprintf(stderr, Name ": No devices listed in %s\n", configfile?configfile:DefaultConfFile);
				exit(1);
			}
			rv = Examine(devlist, scan?!verbose:brief, scan, SparcAdjust);
		} else {
			if (devlist == NULL) {
				if ((devmode == 'S' ||devmode=='D') && scan) {
					/* apply to all devices in /proc/mdstat */
					struct mdstat_ent *ms = mdstat_read();
					struct mdstat_ent *e;
					for (e=ms ; e ; e=e->next) {
						char *name = get_md_name(e->devnum);

						if (!name) {
							fprintf(stderr, Name ": cannot find device file for %s\n",
								e->dev);
							continue;
						}
						if (devmode == 'D')
							rv |= Detail(name, !verbose);
						else if (devmode=='S') {
							mdfd = open_mddev(name);
							if (mdfd >= 0)
								rv |= Manage_runstop(name, mdfd, -1);
						}
						put_md_name(name);
					}
				} else {						
					fprintf(stderr, Name ": No devices given.\n");
					exit(2);
				}
			}
			for (dv=devlist ; dv; dv=dv->next) {
				switch(dv->disposition) {
				case 'D':
					rv |= Detail(dv->devname, brief); continue;
				case 'K': /* Zero superblock */
					rv |= Kill(dv->devname, force); continue;
				case 'Q':
					rv |= Query(dv->devname); continue;
				}
				mdfd = open_mddev(dv->devname);
				if (mdfd>=0)
					switch(dv->disposition) {
					case 'R':
						rv |= Manage_runstop(dv->devname, mdfd, 1); break;
					case 'S':
						rv |= Manage_runstop(dv->devname, mdfd, -1); break;
					case 'o':
						rv |= Manage_ro(dv->devname, mdfd, 1); break;
					case 'w':
						rv |= Manage_ro(dv->devname, mdfd, -1); break;
					}
			}
		}
		break;
	case MONITOR:
/*
		if (!devlist && !scan) {
			fprintf(stderr, Name ": Cannot monitor: need --scan or at least one device\n");
			rv = 1;
			break;
		}
*/		rv= Monitor(devlist, mailaddr, program,
			    delay?delay:60, scan, configfile);
		break;
	}
	exit(rv);
}