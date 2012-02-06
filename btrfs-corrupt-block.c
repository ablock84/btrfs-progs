/*
 * Copyright (C) 2009 Oracle.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 021110-1307, USA.
 */

#define _XOPEN_SOURCE 500
#define _GNU_SOURCE 1
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <getopt.h>
#include "kerncompat.h"
#include "ctree.h"
#include "volumes.h"
#include "disk-io.h"
#include "print-tree.h"
#include "transaction.h"
#include "list.h"
#include "version.h"

struct extent_buffer *debug_corrupt_block(struct btrfs_root *root, u64 bytenr,
				     u32 blocksize, int copy)
{
	int ret;
	struct extent_buffer *eb;
	u64 length;
	struct btrfs_multi_bio *multi = NULL;
	struct btrfs_device *device;
	int num_copies;
	int mirror_num = 1;

	eb = btrfs_find_create_tree_block(root, bytenr, blocksize);
	if (!eb)
		return NULL;

	length = blocksize;
	while (1) {
		ret = btrfs_map_block(&root->fs_info->mapping_tree, READ,
				      eb->start, &length, &multi, mirror_num);
		BUG_ON(ret);
		device = multi->stripes[0].dev;
		eb->fd = device->fd;
		device->total_ios++;
		eb->dev_bytenr = multi->stripes[0].physical;

		fprintf(stdout, "mirror %d logical %Lu physical %Lu "
			"device %s\n", mirror_num, (unsigned long long)bytenr,
			(unsigned long long)eb->dev_bytenr, device->name);
		kfree(multi);

		if (!copy || mirror_num == copy) {
			ret = read_extent_from_disk(eb);
			printf("corrupting %llu copy %d\n", eb->start,
			       mirror_num);
			memset(eb->data, 0, eb->len);
			write_extent_to_disk(eb);
			fsync(eb->fd);
		}

		num_copies = btrfs_num_copies(&root->fs_info->mapping_tree,
					      eb->start, eb->len);
		if (num_copies == 1)
			break;

		mirror_num++;
		if (mirror_num > num_copies)
			break;
	}
	return eb;
}

static void print_usage(void)
{
	fprintf(stderr, "usage: btrfs-map-logical [options] mount_point\n");
	fprintf(stderr, "\t-l Logical extent to map\n");
	fprintf(stderr, "\t-c Copy of the extent to read (usually 1 or 2)\n");
	fprintf(stderr, "\t-o Output file to hold the extent\n");
	fprintf(stderr, "\t-b Number of bytes to read\n");
	exit(1);
}

static struct option long_options[] = {
	/* { "byte-count", 1, NULL, 'b' }, */
	{ "logical", 1, NULL, 'l' },
	{ "copy", 1, NULL, 'c' },
	{ "bytes", 1, NULL, 'b' },
	{ 0, 0, 0, 0}
};

static int corrupt_extent(struct btrfs_root *root, u64 bytenr, int copy)
{
	struct btrfs_trans_handle *trans;
	struct btrfs_key key;
	struct extent_buffer *leaf;
	u32 item_size;
	unsigned long ptr;
	struct btrfs_path *path;
	int ret;
	int slot;

	trans = btrfs_start_transaction(root, 1);
	path = btrfs_alloc_path();

	key.objectid = bytenr;
	key.type = (u8)-1;
	key.offset = (u64)-1;

	while(1) {
		ret = btrfs_search_slot(trans, root->fs_info->extent_root,
					&key, path, 0, 1);
		if (ret < 0)
			break;

		if (ret > 0) {
			if (path->slots[0] == 0)
				break;
			path->slots[0]--;
		}
		leaf = path->nodes[0];
		slot = path->slots[0];
		btrfs_item_key_to_cpu(leaf, &key, slot);
		if (key.objectid != bytenr)
			break;

		if (key.type != BTRFS_EXTENT_ITEM_KEY &&
		    key.type != BTRFS_TREE_BLOCK_REF_KEY &&
		    key.type != BTRFS_EXTENT_DATA_REF_KEY &&
		    key.type != BTRFS_EXTENT_REF_V0_KEY &&
		    key.type != BTRFS_SHARED_BLOCK_REF_KEY &&
		    key.type != BTRFS_SHARED_DATA_REF_KEY)
			goto next;

		fprintf(stderr, "corrupting extent record: key %Lu %u %Lu\n",
			key.objectid, key.type, key.offset);

		ptr = btrfs_item_ptr_offset(leaf, slot);
		item_size = btrfs_item_size_nr(leaf, slot);
		memset_extent_buffer(leaf, 0, ptr, item_size);
		btrfs_mark_buffer_dirty(leaf);
next:
		btrfs_release_path(NULL, path);

		if (key.offset > 0)
			key.offset--;
		if (key.offset == 0)
			break;
	}

	btrfs_free_path(path);
	btrfs_commit_transaction(trans, root);
	ret = close_ctree(root);
	BUG_ON(ret);
	return 0;
}

int main(int ac, char **av)
{
	struct cache_tree root_cache;
	struct btrfs_root *root;
	struct extent_buffer *eb;
	char *dev;
	u64 logical = 0;
	int ret = 0;
	int option_index = 0;
	int copy = 0;
	u64 bytes = 4096;
	int extent_rec;

	while(1) {
		int c;
		c = getopt_long(ac, av, "l:c:e", long_options,
				&option_index);
		if (c < 0)
			break;
		switch(c) {
			case 'l':
				logical = atoll(optarg);
				if (logical == 0) {
					fprintf(stderr,
						"invalid extent number\n");
					print_usage();
				}
				break;
			case 'c':
				copy = atoi(optarg);
				if (copy == 0) {
					fprintf(stderr,
						"invalid copy number\n");
					print_usage();
				}
				break;
			case 'b':
				bytes = atoll(optarg);
				if (bytes == 0) {
					fprintf(stderr,
						"invalid byte count\n");
					print_usage();
				}
				break;
			case 'e':
				extent_rec = 1;
				break;
			default:
				print_usage();
		}
	}
	ac = ac - optind;
	if (ac == 0)
		print_usage();
	if (logical == 0)
		print_usage();
	if (copy < 0)
		print_usage();

	dev = av[optind];

	radix_tree_init();
	cache_tree_init(&root_cache);

	root = open_ctree(dev, 0, 1);
	if (!root) {
		fprintf(stderr, "Open ctree failed\n");
		exit(1);
	}
	if (extent_rec) {
		ret = corrupt_extent (root, logical, 0);
		goto out;
	}

	if (bytes == 0)
		bytes = root->sectorsize;

	bytes = (bytes + root->sectorsize - 1) / root->sectorsize;
	bytes *= root->sectorsize;

	while (bytes > 0) {
		eb = debug_corrupt_block(root, logical, root->sectorsize, copy);
		free_extent_buffer(eb);
		logical += root->sectorsize;
		bytes -= root->sectorsize;
	}
out:
	return ret;
}
