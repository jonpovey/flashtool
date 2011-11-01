/*
 * flashtool - erase/write MTD NAND flash
 * with various options for bad block handling, ranges and OOB layout.
 *
 * Inspired by mtd-utils nandwrite, flash_eraseall
 *
 * Copyright (C) 2011 Racelogic Limited
 * Written by Jon Povey <jon.povey@racelogic.co.uk>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <mtd/mtd-user.h>

#include "debug.h"

#include "genecc.h"

enum exit_codes {
	EXIT_OK			= 0,
	EXIT_FAIL		= 1,	// general fatal error
	EXIT_BADBLOCK	= 2,	// --failbad and found a bad block
	EXIT_NOSPACE	= 3,	// not enough space (maybe due to bad blocks)
};

// using ints, can handle up to 2GiB MTD partitions.
static char			*image_path;
static char			*mtd_path;
static int			image_fd = -1;
static int			mtd_fd;
static int			max_off = -1;		// excluding OOB
static int			start_off = -1;		// excluding OOB
static int			req_length = -1;	// excluding OOB
static int 			req_pages;
static int			input_size;
static int			failbad;
static int			write_mode;
static int			erase_mode;
static int			legacy;
static int			dm365_rbl;
static int			ubi = 0;
static int			genecc;
static int			quiet;
static int			block_pages;
static struct mtd_info_user mi;
static unsigned char *page_buf, *block_buf;
static int			block_off;
static int			bytes_done;			// data bytes successfuly (written)
static int			block_bytes_done;

void usage(void)
{
	fprintf(stderr, "\nflashtool - erase/write MTD NAND flash\n\n"
"Usage:\n"
"  flashtool [OPTIONS] mtd-device [image-file]\n\n"
"  mtd-device       Target MTD partition in mtdX or /dev/mtdX format\n"
"  image-file       Source data if writing\n"
"OPTIONS:\n"
"  -w, --write      Write image-file\n"
"  -e, --erase      Erase blocks; with -w, erase-before-write\n"
"  -s, --start x    Offset from partition start, in bytes\n"
"  -l, --length x   In bytes, else input file length is used\n"
"      --failbad    Fail if any bad block is found\n"
"      --maxoff x   Do not go above this absolute offset\n"
"      --legacy     Write legacy infix OOB layout\n"
"      --dm365-rbl  Write DM365 RBL compatible OOB layout\n"
"      --ubi        UBI writing: per block, skip trailing all-FF pages\n"
"  -q, --quiet\n"
"\n"
	);
}

long long int llarg(void)
{
	long long int x;
	char *tmpstr;

	errno = 0;
	x = strtoll(optarg, &tmpstr, 0);
	if (errno != 0 || *tmpstr != 0) {
		fprintf(stderr, "Bad (long long) integer argument %s\n", optarg);
		usage();
		exit(EXIT_FAIL);
	}
	return x;
}

void handle_options(int argc, char *argv[])
{
	int error = 0;

	for (;;) {
		int option_index = 0;
		static const char *short_options = "wes:l:q";
		static const struct option long_options[] = {
			{"failbad",		no_argument,		0, 0},
			{"maxoff",		required_argument,	0, 0},
			{"legacy",		no_argument,		0, 0},
			{"ubi",			no_argument,		0, 0},
			{"dm365-rbl",	no_argument,		0, 0},
			{"write",		no_argument,		0, 'w'},
			{"erase",		no_argument,		0, 'e'},
			{"start",		required_argument,	0, 's'},
			{"length",		required_argument,	0, 'l'},
			{"quiet",		no_argument,		0, 'q'},
			{},
		};

		int c = getopt_long(argc, argv, short_options,
				long_options, &option_index);
		if (c == -1) {
			break;
		}

		switch (c) {
		case 0:
			switch (option_index) {
			case 0:
				failbad = 1;
				break;
			case 1:
				max_off = llarg();
				break;
			case 2:
				legacy = 1;
				break;
			case 3:
				ubi = 1;
				break;
			case 4:
				dm365_rbl = 1;
				break;
			}
			break;
		case 'w':
			write_mode = 1;
			break;
		case 'e':
			erase_mode = 1;
			break;
		case 's':
			start_off = llarg();
			break;
		case 'l':
			req_length = llarg();
			break;
		case 'q':
			quiet = 1;
			break;
		case '?':
			error = 1;
			break;
		}
	}

	if (optind < argc) {
		if (0 == strncmp(argv[optind], "mtd", 3)) {
			mtd_path = malloc(strlen(argv[optind]) + 4);
			sprintf(mtd_path, "/dev/%s", argv[optind]);
		} else {
			mtd_path = strdup(argv[optind]);
		}
		optind++;
	} else {
		fprintf(stderr, "Must supply mtd device name\n");
		error = 1;
	}

	if (write_mode) {
		if (optind < argc) {
			image_path = strdup(argv[optind]);
			optind++;
		} else {
			fprintf(stderr, "Must supply input filename with -w\n");
			error = 1;
		}
	} else if (req_length < 0) {
		fprintf(stderr, "Must supply length if not writing\n");
	}

	if (!write_mode && !erase_mode) {
		fprintf(stderr, "Must set either -w or -e.\n");
		error = 1;
	}

	if (start_off < 0) {
		fprintf(stderr, "Must supply start offset\n");
		error = 1;
	}

	if (legacy && dm365_rbl) {
		fprintf(stderr, "legacy and dm365_rbl modes are mutually exclusive\n");
		error = 1;
	}

	if (optind < argc) {
		if (!write_mode) {
			fprintf(stderr, "Input file without -w ?\n");
		}
		fprintf(stderr, "Too many commandline arguments\n");
		error = 1;
	}

	if (error) {
		usage();
		exit(EXIT_FAIL);
	}
}

void dump_stats(void)
{
	fprintf(stderr, "MTD device size:  0x%-8x bytes\n", mi.size);
	fprintf(stderr, "Max offset:       0x%-8x\n", max_off);
	fprintf(stderr, "Requested length: 0x%-8x bytes\n", req_length);
	fprintf(stderr, "Page size:        0x%-8x bytes\n", mi.writesize);
	fprintf(stderr, "Pages needed:     %-6d\n", req_pages);
	if (write_mode)
		fprintf(stderr, "Input file:       0x%-8x bytes\n", input_size);
	fprintf(stderr, "Start offset:     0x%x\n", start_off);
	fprintf(stderr, "This block start: 0x%x\n", block_off);
	fprintf(stderr, "Bytes done ok:    0x%x\n", bytes_done);
	
}

int erase_block(int offset)
{
	struct erase_info_user ei;

	DBG("erase block at 0x%x\n", offset);

	ei.start = offset;
	ei.length = mi.erasesize;

	return ioctl(mtd_fd, MEMERASE, &ei);
}

int mark_block_bad(loff_t offset)
{
	fprintf(stderr, "mark block bad at 0x%llx\n", offset);

	/*
	 * This ioctl may only set the BBT (not sure).
	 * We should possibly try to write the manufacturer bad block markers
	 * in the block itself, for the UBL.
	 */
	return ioctl(mtd_fd, MEMSETBADBLOCK, &offset);

	printf("DISABLED: mark block bad at 0x%llx\n", offset);
	return 0;
}

int write_page(int blockoff, int pagenum)
{
	unsigned char *writeme, *srcdata;
	off_t pageoff;
	int ret;

	pageoff = blockoff + pagenum * mi.writesize;
	srcdata = &block_buf[pagenum * mi.writesize];
	if (genecc) {
		int layout = 0;

		if (legacy) {
			layout = GENECC_LAYOUT_LEGACY;
		} else if (dm365_rbl) {
			layout = GENECC_LAYOUT_DM365_RBL;
		} else {
			ERR("genecc with unknown layout!\n");
			return -EINVAL;
		}
		writeme = do_genecc(srcdata, layout);	// page data + OOB
	} else {
		writeme = srcdata;				// page data only
	}

	DBG("0x%lx (#%-2d of block)\n", pageoff, pagenum);

	if (lseek(mtd_fd, pageoff, SEEK_SET) != pageoff) {
		perror("Write seek");
		return -errno;
	}

	ret = write(mtd_fd, writeme, mi.writesize);
	if (ret != mi.writesize) {
		perror("Write page");
		return -errno;
	}

	if (genecc) {
		struct mtd_oob_buf oob;

		oob.start = pageoff;
		oob.length = mi.oobsize;
		oob.ptr = writeme + mi.writesize;

		DBG("OOB\n");
		if (ioctl(mtd_fd, MEMWRITEOOB, &oob) != 0) {
			perror("Write OOB");
			return -errno;
		}
	}
	return 0;
}

/*
 * Ready to write next block (for writing) or have read a block (reading).
 * Do needed image file i/o
 */
int next_image_block(void)
{
	int buf_start;	// index of first image data
	int buf_end;	// index of last image data + 1
	int want_sz, read_sz;
	int ret;

	if (write_mode) {
		// first block, non-first page?
		if (start_off > block_off) {
			buf_start = start_off - block_off;
			memset(block_buf, 0xFF, buf_start);
		} else {
			buf_start = 0;
		}
		// last block, not full?
		if (bytes_done + mi.erasesize > req_length) {
			buf_end = req_length - bytes_done + buf_start;
			memset(&block_buf[buf_end], 0xff, mi.erasesize - buf_end);
		} else {
			buf_end = mi.erasesize;
		}

		want_sz = buf_end - buf_start;
		read_sz = 0;
		DBG("want %d bytes\n", want_sz);
		while (read_sz < want_sz) {
			ret = read(image_fd, &block_buf[buf_start + read_sz],
					want_sz - read_sz);
			if (ret == 0) {
				fprintf(stderr, "Unexpected EOF reading input file\n");
				exit(EXIT_FAIL);
			} else if (ret < 0) {
				perror("Reading image file");
				exit(EXIT_FAIL);
			}
			DBG("read 0x%x (%d) bytes\n", ret, ret);
			read_sz += ret;
		}
	}
	return 0;
}

/* Return number of pages at the end of block_buf which are all FFs */
int count_trailing_ff_pages(void)
{
	int ffs, ffpages;

	ffs = 0;
	while (ffs < mi.erasesize && block_buf[(mi.erasesize - ffs) - 1] == 0xff) {
		++ffs;
	}
	ffpages = ffs / mi.writesize;

	DBG("0x%x FFs = %d trailing pages\n", ffs, ffpages);

	return ffpages;
}

int main(int argc, char *argv[])
{
	int ret;
	int rewind;		// bad block, write the same data in next block

	handle_options(argc, argv);

	if (legacy || dm365_rbl)
		genecc = 1;
	else
		genecc = 0;

	if ((mtd_fd = open(mtd_path, O_RDWR)) == -1) {
		perror(mtd_path);
		exit(EXIT_FAIL);
	}

	if (ioctl(mtd_fd, MEMGETINFO, &mi) != 0) {
		perror("MEMGETINFO");
		close(mtd_fd);
		exit(EXIT_FAIL);
	}

	// Right now, only support one expected NAND size
	if (mi.oobsize != 64) {
		fprintf(stderr, "oobsize %d not supported\n", mi.oobsize);
		exit(EXIT_FAIL);
	}
	if (mi.writesize != 2048) {
		fprintf(stderr, "writesize %d not supported\n", mi.writesize);
		exit(EXIT_FAIL);
	}

	if (start_off & (mi.writesize - 1)) {
		fprintf(stderr, "Start offset must be aligned to page size 0x%x\n",
				mi.writesize);
		close(mtd_fd);
		exit(EXIT_FAIL);
	}

	if (write_mode) {
		image_fd = open(image_path, O_RDONLY);
		if (image_fd == -1) {
			perror(image_path);
			exit(EXIT_FAIL);
		}
		input_size = lseek(image_fd, 0, SEEK_END);
		lseek(image_fd, 0, SEEK_SET);

		if (req_length < 0) {
			req_length = input_size;
		} else if (req_length > input_size) {
			fprintf(stderr, "File smaller (%d) than requested length (%d)\n",
					(int)input_size, req_length);
			exit(EXIT_FAIL);
		}
		DBG("input_size: %d\n", (int)input_size);
	}

	if (req_length < 0) {
		fprintf(stderr, "Must specify length or supply an input file\n");
		exit(EXIT_FAIL);
	}

	req_pages = (((req_length - 1) / mi.writesize) + 1);
	block_pages = mi.erasesize / mi.writesize;

	if (max_off < 0) {
		max_off = mi.size;
	} else {
		if (max_off > mi.size) {
			max_off = mi.size;
			fprintf(stderr, "Max offset truncated to device size: 0x%x\n",
					max_off);
		}
	}

	if (req_pages * mi.writesize > mi.size - start_off) {
		dump_stats();
		fprintf(stderr, "Request would pass the end of device\n");
		exit(EXIT_NOSPACE);
	}

	if (req_pages * mi.writesize > max_off - start_off) {
		dump_stats();
		fprintf(stderr, "Request would exceed max offset limit\n");
		exit(EXIT_NOSPACE);
	}

	if (write_mode)  {
		if (genecc) {
			ret = ioctl(mtd_fd, MTDFILEMODE, (void *) MTD_MODE_RAW);
			if (ret != 0) {
				perror ("MTDFILEMODE");
				close (mtd_fd);
				exit (EXIT_FAIL);
			} else {
				DBG("Set MTD_MODE_RAW\n");
			}
			genecc_init();
		}
		// allocate block buffer: in-band page size * pages per block
		block_buf = malloc(mi.writesize * block_pages);
		if (!block_buf) {
			fprintf(stderr, "block_buf malloc failed\n");
			exit(EXIT_FAIL);
		}
	}

	/*
	 * Main write loop:
	 */

	rewind = 0;
	// start at beginning of block containing start_off
	for (block_off = start_off & ~(mi.erasesize - 1);
		bytes_done < req_length;
		block_off += mi.erasesize
	) {
		int start_page_num, page_num;
		int write_pages;
		loff_t ll_off;

		block_bytes_done = 0;

		//dump_stats();

		// check bad block. Have to pass a long long to ioctl.
		ll_off = block_off;
		ret = ioctl(mtd_fd, MEMGETBADBLOCK, &ll_off);
		if (ret < 0) {
			perror("MEMGETBADBLOCK");
			exit(EXIT_FAIL);
		}
		if (ret == 1) {
			fprintf(stderr, "Bad block at 0x%x : ", block_off);
			if (failbad) {
				fprintf(stderr, "ABORT\n");
				exit(EXIT_BADBLOCK);
			} else {
				fprintf(stderr, "skip\n");
				continue;
			}
		}

		if (!quiet) {
			if (erase_mode && write_mode)
				printf("Erase + write");
			else if (erase_mode)
				printf("Erase");
			else if (write_mode)
				printf("Write");
			else
				exit(EXIT_FAIL);	// bug
			printf(" block at 0x%x\n", block_off);
		}

		if (erase_mode) {
			if (block_off + mi.erasesize > max_off) {
				fprintf(stderr, "Erasing next block would exceed max offset\n");
				dump_stats();
				exit(EXIT_NOSPACE);
			}

			ret = erase_block(block_off);
			if (ret < 0) {
				fprintf(stderr, "Erase block at 0x%x failed\n", block_off);
				if (mark_block_bad(block_off) < 0) {
					fprintf(stderr, "Marking block bad failed\n");
					// If not marked bad it would be misread, so this is fatal
					exit(EXIT_FAIL);
				} else {
					continue;	// try next block
				}
			}
		}

		if (start_off > block_off) {
			// first block, starting later than page 0
			start_page_num = (start_off - block_off) / mi.writesize;
		} else {
			start_page_num = 0;
		}

		if (write_mode) {
			// don't read more image file if skipping a bad block
			if (!rewind)
				next_image_block();
		} else {
			// erasing, update count now
			bytes_done += mi.erasesize - start_page_num * mi.writesize;
			continue;
		}

		/*
		 * UBI assumes it can write to any pages at the end of a PEB which
		 * are all FFs in the in-band data area, so we must not write those
		 * pages as we write (non-FF) ECC and UBI's later write would end up
		 * with corrupt ECC (bitwise ANDed with ours).
		 *
		 * http://www.linux-mtd.infradead.org/doc/ubi.html#L_flasher_algo
		 */
		if (ubi)
			write_pages = block_pages - count_trailing_ff_pages();
		else
			write_pages = block_pages;

		if (!quiet && write_pages != block_pages)
			printf("Skip last %d pages of block\n", block_pages - write_pages);

		rewind = 0;
		// foreach page in this block, until done
		for (page_num = start_page_num; page_num < block_pages;	++page_num) {

			if (block_off + (page_num + 1) * mi.writesize > max_off) {
				fprintf(stderr, "Writing this page would exceed max offset\n");
				dump_stats();
				exit(EXIT_NOSPACE);
			}

			if (page_num >= write_pages) {
				/*
				 * Do the UBI mode skip here but stay in the loop so our "done"
				 * counters get incremented appropriately
				 */
				DBG("Skipping page %d\n", page_num);
				ret = 0;
			} else {
				ret = write_page(block_off, page_num);
			}

			if (ret < 0) {
				fprintf(stderr, "Write block at 0x%x, page %d failed: ",
						block_off, page_num);
				if (failbad) {
					fprintf(stderr, "ABORT\n");
					exit(EXIT_BADBLOCK);
				} else {
					fprintf(stderr, "Mark bad and skip\n");
				}

				if (erase_block(block_off) < 0) {
					fprintf(stderr, "Erase block at 0x%x failed\n", block_off);
					// This isn't so important as we are about to mark bad
				}
				if (mark_block_bad(block_off) < 0) {
					fprintf(stderr, "Marking block bad at 0x%x failed\n",
							block_off);
					// If not marked bad it would be misread, so this is fatal
					exit(EXIT_FAIL);
				}
				rewind = 1;
				break;
			}

			block_bytes_done += mi.writesize;
			if (bytes_done + block_bytes_done >= req_length)
				break;
		}
		if (!rewind) {
			bytes_done += block_bytes_done;
		}
	}

	if (image_fd != -1)
		close(image_fd);
	close(mtd_fd);

	if (mtd_path)
		free(mtd_path);
	if (image_path)
		free(image_path);
	if (page_buf)
		free(page_buf);
	if (block_buf)
		free(block_buf);

	return 0;
}
