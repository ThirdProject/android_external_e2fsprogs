/*
 * main.c --- ext2 resizer main program
 *
 * Copyright (C) 1997 Theodore Ts'o
 * 
 * %Begin-Header%
 * All rights reserved.
 * %End-Header%
 */

#ifdef HAVE_GETOPT_H
#include <getopt.h>
#endif
#include <fcntl.h>
#include <sys/ioctl.h>

#include "resize2fs.h"

#include "./version.h"

/* #define EXPIRE_TIME 905835600 */

#ifdef EXPIRE_TIME
static void check_expire_time(const char *progname);
#endif

char *program_name, *device_name;

static volatile void usage (char *prog)
{
	fprintf (stderr, "usage: %s [-d debug_flags] [-f] [-F] [-p] device [new-size]\n\n", prog);

#ifdef EXPIRE_TIME
	check_expire_time(program_name);
#endif
	exit (1);
}

static errcode_t resize_progress_func(ext2_resize_t rfs, int pass,
				      unsigned long cur, unsigned long max)
{
	ext2_sim_progmeter progress;
	const char	*label;
	errcode_t	retval;

	progress = (ext2_sim_progmeter) rfs->prog_data;
	if (max == 0)
		return 0;
	if (cur == 0) {
		if (progress)
			ext2fs_progress_close(progress);
		progress = 0;
		switch (pass) {
		case E2_RSZ_EXTEND_ITABLE_PASS:
			label = "Extending the inode table";
			break;
		case E2_RSZ_BLOCK_RELOC_PASS:
			label = "Relocating blocks";
			break;
		case E2_RSZ_INODE_SCAN_PASS:
			label = "Scanning inode table";
			break;
		case E2_RSZ_INODE_REF_UPD_PASS:
			label = "Updating inode references";
			break;
		case E2_RSZ_MOVE_ITABLE_PASS:
			label = "Moving inode table";
			break;
		default:
			label = "Unknown pass?!?";
			break;
		}
		printf("Begin pass %d (max = %lu)\n", pass, max);
		retval = ext2fs_progress_init(&progress, label, 30,
					      40, max, 0);
		if (retval)
			progress = 0;
		rfs->prog_data = (void *) progress;
	}
	if (progress)
		ext2fs_progress_update(progress, cur);
	if (cur >= max) {
		if (progress)
			ext2fs_progress_close(progress);
		progress = 0;
		rfs->prog_data = 0;
	}
	return 0;
}

static void check_mount(char *device)
{
	errcode_t	retval;
	int		mount_flags;

	retval = ext2fs_check_if_mounted(device, &mount_flags);
	if (retval) {
		com_err("ext2fs_check_if_mount", retval,
			"while determining whether %s is mounted.",
			device);
		return;
	}
	if (!(mount_flags & EXT2_MF_MOUNTED))
		return;
	
	fprintf(stderr, "%s is mounted; can't resize a "
		"mounted filesystem!\n\n", device);
	exit(1);
}

#ifdef EXPIRE_TIME
static void check_expire_time(const char *progname)
{
	time_t	timenow;

	timenow = time(0);
	
	if (timenow > EXPIRE_TIME) {
		fprintf(stderr, "This beta-test version of %s is expired.\n"
			"Please contact PowerQuest to get an updated version "
			"of this program.\n\n", progname);
		exit(1);
	} else {
		fprintf(stderr, "Please note this is a beta-test version of "
			"%s which will\nexpire in %d days.\n\n", progname,
			(EXPIRE_TIME - timenow) / (60*60*24));
	}
}		
#endif
	


int main (int argc, char ** argv)
{
	errcode_t	retval;
	ext2_filsys	fs;
	int		c;
	int		flags = 0;
	int		flush = 0;
	int		force = 0;
	int		fd;
	blk_t		new_size = 0;
	blk_t		max_size = 0;
	io_manager	io_ptr;
	char		*tmp;
	struct ext2fs_sb *s;
	
	initialize_ext2_error_table();

	fprintf (stderr, "resize2fs %s (%s)\n",
		 E2FSPROGS_VERSION, E2FSPROGS_DATE);
	fprintf(stderr, "Copyright 1998 by Theodore Ts'o and PowerQuest, Inc.  All Rights Reserved.\n\n");
	if (argc && *argv)
		program_name = *argv;

	while ((c = getopt (argc, argv, "d:fFhp")) != EOF) {
		switch (c) {
		case 'h':
			usage(program_name);
			break;
		case 'f':
			force = 1;
			break;
		case 'F':
			flush = 1;
			break;
		case 'd':
			flags |= atoi(optarg);
			break;
		case 'p':
			flags |= RESIZE_PERCENT_COMPLETE;
			break;
		default:
			usage(program_name);
		}
	}
	if (optind == argc)
		usage(program_name);

#ifdef EXPIRE_TIME
	check_expire_time(program_name);
#endif
	
	device_name = argv[optind++];
	if (optind < argc) {
		new_size = strtoul(argv[optind++], &tmp, 0);
		if (*tmp) {
			com_err(program_name, 0, "bad filesystem size - %s",
				argv[optind - 1]);
			exit(1);
		}
	}
	if (optind < argc)
		usage(program_name);
	
	check_mount(device_name);
	
	if (flush) {
#ifdef BLKFLSBUF
		fd = open(device_name, O_RDONLY, 0);

		if (fd < 0) {
			com_err("open", errno, "while opening %s for flushing",
				device_name);
			exit(1);
		}
		if (ioctl(fd, BLKFLSBUF, 0) < 0) {
			com_err("BLKFLSBUF", errno, "while trying to flush %s",
				device_name);
			exit(1);
		}
		close(fd);
#else
		fprintf(stderr, "BLKFLSBUF not supported");
		exit(1);
#endif /* BLKFLSBUF */
	}

	if (flags & RESIZE_DEBUG_IO) {
		io_ptr = test_io_manager;
		test_io_backing_manager = unix_io_manager;
	} else 
		io_ptr = unix_io_manager;

	retval = ext2fs_open (device_name, EXT2_FLAG_RW, 0, 0,
			      io_ptr, &fs);
	if (retval) {
		com_err (program_name, retval, "while trying to open %s",
			 device_name);
		printf ("Couldn't find valid filesystem superblock.\n");
		exit (1);
	}
	/*
	 * Check for compatibility with the feature sets.  We need to
	 * be more stringent than ext2fs_open().
	 */
	s = (struct ext2fs_sb *) fs->super;
	if ((s->s_feature_compat & ~EXT2_LIB_FEATURE_COMPAT_SUPP) ||
	    (s->s_feature_incompat & ~EXT2_LIB_FEATURE_RO_COMPAT_SUPP)) {
		com_err(program_name, EXT2_ET_UNSUPP_FEATURE,
			"(%s)", device_name);
		exit(1);
	}
	
	/*
	 * Get the size of the containing partition, and use this for
	 * defaults and for making sure the new filesystme doesn't
	 * exceed the partition size.
	 */
	retval = ext2fs_get_device_size(device_name, fs->blocksize,
					&max_size);
	if (retval) {
		com_err(program_name, retval,
			"while trying to determine filesystem size");
		exit(1);
	}
	if (!new_size)
		new_size = max_size;
	if (!force && (new_size > max_size)) {
		fprintf(stderr, "The containing partition (or device)"
			" is only %d blocks.\nYou requested a new size"
			" of %d blocks.\n\n", max_size,
			new_size);
		exit(1);
	}
	if (new_size == fs->super->s_blocks_count) {
		fprintf(stderr, "The filesystem is already %d blocks "
			"long.  Nothing to do!\n\n", new_size);
		exit(0);
	}
	if (!force && (fs->super->s_lastcheck < fs->super->s_mtime)) {
		fprintf(stderr, "Please run 'e2fsck -f %s' first.\n\n",
			device_name);
		exit(1);
	}
	retval = resize_fs(fs, new_size, flags,
			   ((flags & RESIZE_PERCENT_COMPLETE) ?
			    resize_progress_func : 0));
	if (retval) {
		com_err(program_name, retval, "while trying to resize %s",
			device_name);
		ext2fs_close (fs);
	}
	printf("The filesystem on %s is now %d blocks long.\n\n",
	       device_name, new_size);
	return (0);
}
