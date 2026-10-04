/*
 * Volume life cycle: mount, unmount, statistics, label.
 */
#include "internal.h"

static s64 free_mft_records(ntfs_volume *vol)
{
	ntfs_attr *na = vol->mftbmp_na;
	s64 nr_free = ntfs_attr_get_free_bits(na);

	if (nr_free >= 0)
		nr_free += (na->allocated_size - na->data_size) << 3;
	return nr_free;
}

/* Deletes hiberfil.sys from the root so the volume can be written. */
static int remove_hiberfile(struct n4m_volume *v)
{
	static const char name[] = "hiberfil.sys";
	ntfs_inode *dir_ni, *ni;
	ntfschar *uname = NULL;
	int ulen, err;
	u64 mref;

	err = n4m_utf8_to_utf16(name, sizeof(name) - 1, &uname, &ulen);
	if (err)
		return err;
	dir_ni = ntfs_inode_open(v->vol, FILE_root);
	if (!dir_ni) {
		free(uname);
		return n4m_errno();
	}
	mref = ntfs_inode_lookup_by_name(dir_ni, uname, ulen);
	if (mref == (u64)-1) {
		err = n4m_errno();
		ntfs_inode_close(dir_ni);
		free(uname);
		return err == ENOENT ? 0 : err;
	}
	ni = ntfs_inode_open(v->vol, MREF(mref));
	if (!ni) {
		err = n4m_errno();
		ntfs_inode_close(dir_ni);
		free(uname);
		return err;
	}
	/* ntfs_delete closes both inodes */
	err = ntfs_delete(v->vol, NULL, ni, dir_ni, uname, (u8)ulen) ?
			n4m_errno() : 0;
	free(uname);
	return err;
}

static ntfs_volume *mount_dev(struct n4m_volume *v, ntfs_mount_flags flags,
		int *err)
{
	struct ntfs_device *dev = n4m_device_new(v);
	ntfs_volume *vol;

	if (!dev) {
		*err = ENOMEM;
		return NULL;
	}
	errno = 0;
	vol = ntfs_device_mount(dev, flags);
	if (!vol) {
		*err = n4m_errno();
		free(dev->d_private);
		ntfs_device_free(dev);
	}
	return vol;
}

static int umount_dev(ntfs_volume *vol)
{
	void *priv = vol->dev->d_private;
	int err = ntfs_umount(vol, FALSE) ? n4m_errno() : 0;

	free(priv);
	return err;
}

int n4m_mount(n4m_blockdev *bdev, const n4m_mount_opts *opts,
		n4m_volume **out)
{
	struct n4m_volume *v;
	ntfs_mount_flags flags = 0;
	int err;

	n4m_log_init();
	*out = NULL;
	v = calloc(1, sizeof(*v));
	if (!v)
		return ENOMEM;
	pthread_mutex_init(&v->lock, NULL);
	v->bdev = *bdev;
	v->uid = opts->uid;
	v->gid = opts->gid;

	/*
	 * Unlike ntfs-3g we do not wipe a dirty journal by default. A dirty
	 * $LogFile means Windows crashed or the drive was pulled with changes
	 * in flight; only Windows can replay those, so we mount read only and
	 * ask the user to let Windows fix it (or to override explicitly).
	 */
	if (opts->readonly || bdev->readonly) {
		flags |= NTFS_MNT_RDONLY;
	} else {
		flags |= NTFS_MNT_MAY_RDONLY;
		if (opts->remove_hiberfile)
			flags |= NTFS_MNT_IGNORE_HIBERFILE | NTFS_MNT_RECOVER;
		if (opts->reset_journal)
			flags |= NTFS_MNT_RECOVER;
	}

	v->vol = mount_dev(v, flags, &err);
	if (!v->vol)
		goto fail;
	v->was_dirty = (v->vol->flags & VOLUME_IS_DIRTY) != 0;

	/*
	 * The NTFS dirty flag means Windows wants to check the volume (or a
	 * previous session did not end cleanly). Do not write to it then,
	 * unless the user explicitly chose to.
	 */
	if (!NVolReadOnly(v->vol) && v->was_dirty && !opts->reset_journal &&
			!opts->remove_hiberfile) {
		n4m_log(0, "volume is marked dirty (Windows wants to check it),"
			" mounting read only");
		umount_dev(v->vol);
		v->vol = mount_dev(v, NTFS_MNT_RDONLY, &err);
		if (!v->vol)
			goto fail;
		v->unclean = true;
	}

	/*
	 * Our own name lookups bypass libntfs-3g's lookup cache (see
	 * inode.c), the library only uses it for its own $Extend files.
	 */
	ntfs_create_lru_caches(v->vol);
	{
		NTFS_BOOT_SECTOR bs;

		if (ntfs_pread(v->vol->dev, 0, sizeof(bs), &bs) == sizeof(bs))
			v->serial = le64_to_cpu(bs.volume_serial_number);
	}

	/* Compress new files created in folders marked compressed */
	NVolSetCompression(v->vol);
	/*
	 * Hide $MFT and friends, show hidden files (we report them with
	 * UF_HIDDEN instead), and mark files starting with a dot as hidden
	 * so .DS_Store and friends do not clutter Windows Explorer.
	 */
	ntfs_set_shown_files(v->vol, FALSE, TRUE, TRUE);

	if (ntfs_volume_get_free_space(v->vol)) {
		err = n4m_errno();
		goto fail_umount;
	}
	v->vol->free_mft_records = free_mft_records(v->vol);

	if (!NVolReadOnly(v->vol) && opts->remove_hiberfile &&
			ntfs_volume_check_hiberfile(v->vol, 0) < 0 &&
			errno == EPERM) {
		err = remove_hiberfile(v);
		if (err)
			goto fail_umount;
		n4m_log(1, "removed hiberfil.sys, volume is now writable");
	}

	if (NVolReadOnly(v->vol) && !(flags & NTFS_MNT_RDONLY) && !v->unclean) {
		/* ntfs-3g fell back to read only, find out why */
		if (ntfs_volume_check_hiberfile(v->vol, 0) < 0)
			v->was_hibernated = true;
		n4m_log(0, "volume is hibernated or was not shut down "
			"cleanly by Windows, mounted read only. Boot Windows "
			"and shut it down fully (or disable Fast Startup) to "
			"write to it.");
	}
	v->readonly = NVolReadOnly(v->vol);
	/*
	 * Like Windows: mark the volume dirty while we may write to it and
	 * clear it again on a clean unmount. If the drive gets pulled out
	 * or we crash, Windows sees the flag and checks the volume.
	 */
	if (!v->readonly && !v->was_dirty &&
			ntfs_volume_write_flags(v->vol,
				v->vol->flags | VOLUME_IS_DIRTY)) {
		err = n4m_errno();
		goto fail_umount;
	}
	*out = v;
	return 0;

fail_umount:
	umount_dev(v->vol);
	v->vol = NULL;
fail:
	n4m_device_free_cache(v);
	pthread_mutex_destroy(&v->lock);
	free(v);
	return err;
}

int n4m_unmount(n4m_volume *v)
{
	int err = 0;

	if (!v)
		return EINVAL;
	LOCK(v);
	/*
	 * Everything goes to the disk first, then the volume is marked
	 * clean. A flag we found set at mount time is left alone, Windows
	 * still has to check that volume.
	 */
	if (!v->readonly && !v->was_dirty) {
		if (ntfs_device_sync(v->vol->dev) == 0)
			ntfs_volume_write_flags(v->vol,
				v->vol->flags & ~VOLUME_IS_DIRTY);
		else
			n4m_log(0, "flushing the drive failed, leaving the "
				"volume marked dirty so Windows checks it");
	}
	err = umount_dev(v->vol);
	v->vol = NULL;
	UNLOCK(v);
	n4m_device_free_cache(v);
	if (v->bdev.close)
		v->bdev.close(v->bdev.ctx);
	pthread_mutex_destroy(&v->lock);
	free(v);
	return err;
}

bool n4m_is_readonly(n4m_volume *v)
{
	return v->readonly;
}

int n4m_volinfo_get(n4m_volume *v, n4m_volinfo *info)
{
	ntfs_volume *vol;
	s64 size;
	int delta;

	memset(info, 0, sizeof(*info));
	LOCK(v);
	vol = v->vol;
	if (vol->vol_name)
		snprintf(info->label, sizeof(info->label), "%s",
			vol->vol_name);
	info->serial = v->serial;
	info->cluster_size = vol->cluster_size;
	info->total_clusters = (uint64_t)vol->nr_clusters;
	size = vol->free_clusters < 0 ? 0 : vol->free_clusters;
	info->free_clusters = (uint64_t)size;
	/* same accounting as ntfs-3g's statfs */
	delta = vol->cluster_size_bits - vol->mft_record_size_bits;
	size = delta >= 0 ? size << delta : size >> -delta;
	info->total_inodes = (uint64_t)((vol->mftbmp_na->allocated_size << 3)
			+ size);
	size += vol->free_mft_records;
	info->free_inodes = size < 0 ? 0 : (uint64_t)size;
	info->major_ver = vol->major_ver;
	info->minor_ver = vol->minor_ver;
	info->readonly = v->readonly;
	info->was_hibernated = v->was_hibernated;
	info->was_dirty = v->was_dirty;
	UNLOCK(v);
	return 0;
}

int n4m_sync(n4m_volume *v)
{
	int err = 0;

	if (v->readonly)
		return 0;
	LOCK(v);
	if (ntfs_device_sync(v->vol->dev))
		err = n4m_errno();
	UNLOCK(v);
	return err;
}

int n4m_set_label(n4m_volume *v, const char *label)
{
	ntfschar *ulabel = NULL;
	int ulen, err;

	if (v->readonly)
		return EROFS;
	err = n4m_utf8_to_utf16(label, strlen(label), &ulabel, &ulen);
	if (err)
		return err;
	if (ulen > 128) {
		free(ulabel);
		return ENAMETOOLONG;
	}
	LOCK(v);
	if (ntfs_volume_rename(v->vol, ulabel, ulen))
		err = n4m_errno();
	UNLOCK(v);
	free(ulabel);
	return err;
}

int n4m_probe(n4m_blockdev *bdev, char *label, size_t labelsz,
		uint64_t *serial)
{
	n4m_blockdev ro = *bdev;
	n4m_mount_opts opts = { .readonly = true };
	n4m_volinfo info;
	n4m_volume *v;
	uint8_t *bs;
	int err;

	bs = malloc(bdev->sector_size > 512 ? bdev->sector_size : 512);
	if (!bs)
		return ENOMEM;
	err = bdev->read(bdev->ctx, bs, 0,
			bdev->sector_size > 512 ? bdev->sector_size : 512);
	if (!err && memcmp(bs + 3, "NTFS    ", 8))
		err = EINVAL;
	free(bs);
	if (err)
		return err;

	/* Mount read only to get the label, but keep the device open */
	ro.readonly = true;
	ro.close = NULL;
	err = n4m_mount(&ro, &opts, &v);
	if (err)
		return err;
	n4m_volinfo_get(v, &info);
	n4m_unmount(v);
	if (label)
		snprintf(label, labelsz, "%s", info.label);
	if (serial)
		*serial = info.serial;
	return 0;
}
