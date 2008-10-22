/*
 * Squashfs - a compressed read only filesystem for Linux
 *
 * Copyright (c) 2002, 2003, 2004, 2005, 2006, 2007, 2008
 * Phillip Lougher <phillip@lougher.demon.co.uk>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2,
 * or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * inode.c
 */

/*
 * This file implements code to create and read inodes from disk.
 *
 * Inodes in Squashfs are identified by a 48-bit inode which encodes the
 * location of the compressed metadata block containing the inode, and the byte
 * offset into that block where the inode is placed (<block, offset>).
 *
 * To maximise compression there are different inodes for each file type
 * (regular file, directory, device, etc.), the inode contents and length
 * varying with the type.
 *
 * To further maximise compression, two types of regular file inode and
 * directory inode are defined: inodes optimised for frequently occurring
 * regular files and directories, and extended types where extra
 * information has to be stored.
 */

#include <linux/fs.h>
#include <linux/vfs.h>
#include <linux/zlib.h>
#include <linux/squashfs_fs.h>
#include <linux/squashfs_fs_sb.h>
#include <linux/squashfs_fs_i.h>

#include "squashfs.h"

/*
 * Initialise VFS inode with the base inode information common to all
 * Squashfs inode types.  Inodeb contains the unswapped base inode
 * off disk.
 */
static int squashfs_new_inode(struct super_block *s, struct inode *i,
				struct squashfs_base_inode *inodeb)
{
	int err;

	err = squashfs_get_id(s, le16_to_cpu(inodeb->uid), &i->i_uid);
	if (err)
		goto out;

	err = squashfs_get_id(s, le16_to_cpu(inodeb->guid), &i->i_gid);
	if (err)
		goto out;

	i->i_ino = le32_to_cpu(inodeb->inode_number);
	i->i_mtime.tv_sec = le32_to_cpu(inodeb->mtime);
	i->i_atime.tv_sec = i->i_mtime.tv_sec;
	i->i_ctime.tv_sec = i->i_mtime.tv_sec;
	i->i_mode = le16_to_cpu(inodeb->mode);
	i->i_size = 0;

out:
	return err;
}


struct inode *squashfs_iget(struct super_block *s, long long inode,
				unsigned int inode_number)
{
	struct inode *i = iget_locked(s, inode_number);
	int err;

	TRACE("Entered squashfs_iget\n");

	if (!i)
		return ERR_PTR(-ENOMEM);
	if (!(i->i_state & I_NEW))
		return i;

	err = squashfs_read_inode(i, inode);
	if (err) {
		iget_failed(i);
		return ERR_PTR(err);
	}

	unlock_new_inode(i);
	return i;
}


/*
 * Initialise VFS inode by reading inode from inode table (compressed
 * metadata).  The format and amount of data read depends on type.
 */
int squashfs_read_inode(struct inode *i, long long inode)
{
	struct super_block *s = i->i_sb;
	struct squashfs_sb_info *msblk = s->s_fs_info;
	long long block = SQUASHFS_INODE_BLK(inode) + msblk->inode_table_start;
	unsigned int offset = SQUASHFS_INODE_OFFSET(inode);
	int err, type;
	union squashfs_inode id;
	struct squashfs_base_inode *inodeb = &id.base;

	TRACE("Entered squashfs_read_inode\n");

	/*
	 * Read inode base common to all inode types.
	 */
	err = squashfs_read_metadata(s, inodeb, &block, &offset, sizeof(*inodeb));
	if (err < 0)
		goto failed_read;

	err = squashfs_new_inode(s, i, inodeb);
	if (err)
		goto failed_read;

	block = SQUASHFS_INODE_BLK(inode) + msblk->inode_table_start;
	offset = SQUASHFS_INODE_OFFSET(inode);

	type = le16_to_cpu(inodeb->inode_type);
	switch (type) {
	case SQUASHFS_FILE_TYPE: {
		unsigned int frag_offset, frag_size, frag;
		long long frag_blk;
		struct squashfs_reg_inode *inodep = &id.reg;

		err = squashfs_read_metadata(s, inodep, &block, &offset,
							sizeof(*inodep));
		if (err < 0)
			goto failed_read;

		frag = le32_to_cpu(inodep->fragment);
		if (frag != SQUASHFS_INVALID_FRAG) {
			frag_offset = le32_to_cpu(inodep->offset);
			frag_size = get_fragment_location(s, frag, &frag_blk);
			if (frag_size < 0) {
				err = frag_size;
				goto failed_read;
			}
		} else {
			frag_blk = SQUASHFS_INVALID_BLK;
			frag_size = 0;
			frag_offset = 0;
		}

		i->i_nlink = 1;
		i->i_size = le32_to_cpu(inodep->file_size);
		i->i_fop = &generic_ro_fops;
		i->i_mode |= S_IFREG;
		i->i_blocks = ((i->i_size - 1) >> 9) + 1;
		SQUASHFS_I(i)->fragment_block = frag_blk;
		SQUASHFS_I(i)->fragment_size = frag_size;
		SQUASHFS_I(i)->fragment_offset = frag_offset;
		SQUASHFS_I(i)->start_block = le32_to_cpu(inodep->start_block);
		SQUASHFS_I(i)->block_list_start = block;
		SQUASHFS_I(i)->offset = offset;
		i->i_data.a_ops = &squashfs_aops;

		TRACE("File inode %x:%x, start_block %llx, block_list_start "
			"%llx, offset %x\n", SQUASHFS_INODE_BLK(inode),
			offset, SQUASHFS_I(i)->start_block, block, offset);
		break;
	}
	case SQUASHFS_LREG_TYPE: {
		unsigned int frag_offset, frag_size, frag;
		long long frag_blk;
		struct squashfs_lreg_inode *inodep = &id.lreg;

		err = squashfs_read_metadata(s, inodep, &block, &offset,
							sizeof(*inodep));
		if (err < 0)
			goto failed_read;

		frag = le32_to_cpu(inodep->fragment);
		if (frag != SQUASHFS_INVALID_FRAG) {
			frag_offset = le32_to_cpu(inodep->offset);
			frag_size = get_fragment_location(s, frag, &frag_blk);
			if (frag_size < 0) {
				err = frag_size;
				goto failed_read;
			}
		} else {
			frag_blk = SQUASHFS_INVALID_BLK;
			frag_size = 0;
			frag_offset = 0;
		}

		i->i_nlink = le32_to_cpu(inodep->nlink);
		i->i_size = le64_to_cpu(inodep->file_size);
		i->i_fop = &generic_ro_fops;
		i->i_mode |= S_IFREG;
		i->i_blocks = ((i->i_size - le64_to_cpu(inodep->sparse) - 1)
				>> 9) + 1;

		SQUASHFS_I(i)->fragment_block = frag_blk;
		SQUASHFS_I(i)->fragment_size = frag_size;
		SQUASHFS_I(i)->fragment_offset = frag_offset;
		SQUASHFS_I(i)->start_block = le64_to_cpu(inodep->start_block);
		SQUASHFS_I(i)->block_list_start = block;
		SQUASHFS_I(i)->offset = offset;
		i->i_data.a_ops = &squashfs_aops;

		TRACE("File inode %x:%x, start_block %llx, block_list_start "
			"%llx, offset %x\n", SQUASHFS_INODE_BLK(inode),
			offset, SQUASHFS_I(i)->start_block, block, offset);
		break;
	}
	case SQUASHFS_DIR_TYPE: {
		struct squashfs_dir_inode *inodep = &id.dir;

		err = squashfs_read_metadata(s, inodep, &block, &offset,
				sizeof(*inodep));
		if (err < 0)
			goto failed_read;

		i->i_nlink = le32_to_cpu(inodep->nlink);
		i->i_size = le16_to_cpu(inodep->file_size);
		i->i_op = &squashfs_dir_inode_ops;
		i->i_fop = &squashfs_dir_ops;
		i->i_mode |= S_IFDIR;
		SQUASHFS_I(i)->start_block = le32_to_cpu(inodep->start_block);
		SQUASHFS_I(i)->offset = le16_to_cpu(inodep->offset);
		SQUASHFS_I(i)->dir_index_count = 0;
		SQUASHFS_I(i)->parent_inode = le32_to_cpu(inodep->parent_inode);

		TRACE("Directory inode %x:%x, start_block %llx, offset %x\n",
				SQUASHFS_INODE_BLK(inode), offset,
				SQUASHFS_I(i)->start_block,
				le16_to_cpu(inodep->offset));
		break;
	}
	case SQUASHFS_LDIR_TYPE: {
		struct squashfs_ldir_inode *inodep = &id.ldir;

		err = squashfs_read_metadata(s, inodep, &block, &offset,
				sizeof(*inodep));
		if (err < 0)
			goto failed_read;

		i->i_nlink = le32_to_cpu(inodep->nlink);
		i->i_size = le32_to_cpu(inodep->file_size);
		i->i_op = &squashfs_dir_inode_ops;
		i->i_fop = &squashfs_dir_ops;
		i->i_mode |= S_IFDIR;
		SQUASHFS_I(i)->start_block = le32_to_cpu(inodep->start_block);
		SQUASHFS_I(i)->offset = le16_to_cpu(inodep->offset);
		SQUASHFS_I(i)->dir_index_start = block;
		SQUASHFS_I(i)->dir_index_offset = offset;
		SQUASHFS_I(i)->dir_index_count = le16_to_cpu(inodep->i_count);
		SQUASHFS_I(i)->parent_inode = le32_to_cpu(inodep->parent_inode);

		TRACE("Long directory inode %x:%x, start_block %llx, offset "
				"%x\n", SQUASHFS_INODE_BLK(inode), offset,
				SQUASHFS_I(i)->start_block,
				le16_to_cpu(inodep->offset));
		break;
	}
	case SQUASHFS_SYMLINK_TYPE: {
		struct squashfs_symlink_inode *inodep = &id.symlink;

		err = squashfs_read_metadata(s, inodep, &block, &offset,
				sizeof(*inodep));
		if (err < 0)
			goto failed_read;

		i->i_nlink = le32_to_cpu(inodep->nlink);
		i->i_size = le32_to_cpu(inodep->symlink_size);
		i->i_op = &page_symlink_inode_operations;
		i->i_data.a_ops = &squashfs_symlink_aops;
		i->i_mode |= S_IFLNK;
		SQUASHFS_I(i)->start_block = block;
		SQUASHFS_I(i)->offset = offset;

		TRACE("Symbolic link inode %x:%x, start_block %llx, offset "
				"%x\n", SQUASHFS_INODE_BLK(inode), offset,
				block, offset);
		break;
	}
	case SQUASHFS_BLKDEV_TYPE:
	case SQUASHFS_CHRDEV_TYPE: {
		struct squashfs_dev_inode *inodep = &id.dev;
		unsigned int rdev;

		err = squashfs_read_metadata(s, inodep, &block, &offset,
				sizeof(*inodep));
		if (err < 0)
			goto failed_read;

		i->i_nlink = le32_to_cpu(inodep->nlink);
		i->i_mode |= (type == SQUASHFS_CHRDEV_TYPE) ? S_IFCHR : S_IFBLK;
		rdev = le32_to_cpu(inodep->rdev);
		init_special_inode(i, le16_to_cpu(i->i_mode),
					new_decode_dev(rdev));

		TRACE("Device inode %x:%x, rdev %x\n",
				SQUASHFS_INODE_BLK(inode), offset, rdev);
		break;
	}
	case SQUASHFS_FIFO_TYPE:
	case SQUASHFS_SOCKET_TYPE: {
		struct squashfs_ipc_inode *inodep = &id.ipc;

		err = squashfs_read_metadata(s, inodep, &block, &offset,
				sizeof(*inodep));
		if (err < 0)
			goto failed_read;

		i->i_nlink = le32_to_cpu(inodep->nlink);
		i->i_mode |= (type == SQUASHFS_FIFO_TYPE) ? S_IFIFO : S_IFSOCK;
		init_special_inode(i, le16_to_cpu(i->i_mode), 0);
		break;
	}
	default:
		ERROR("Unknown inode type %d in squashfs_iget!\n", type);
		err = -EINVAL;
		goto failed_read1;
	}

	return 0;

failed_read:
	ERROR("Unable to read inode 0x%llx\n", inode);

failed_read1:
	return err;
}
