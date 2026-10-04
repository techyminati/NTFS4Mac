/*
 * DiskArbitration helpers: describe, list, unmount and eject disks.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <DiskArbitration/DiskArbitration.h>
#include <IOKit/IOBSD.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/storage/IOMedia.h>
#include <dispatch/dispatch.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>

#include "disk.h"

#define GPT_BASIC_DATA "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7"

bool disk_normalize_name(const char *in, char *out, size_t outsz)
{
	if (!strncmp(in, "/dev/", 5))
		in += 5;
	if (!strncmp(in, "rdisk", 5))
		in++;
	if (strncmp(in, "disk", 4) || strlen(in) >= outsz || strchr(in, '/'))
		return false;
	snprintf(out, outsz, "%s", in);
	return true;
}

static void cfstr(CFTypeRef v, char *buf, size_t bufsz)
{
	buf[0] = 0;
	if (v && CFGetTypeID(v) == CFStringGetTypeID())
		CFStringGetCString(v, buf, (CFIndex)bufsz,
				kCFStringEncodingUTF8);
}

int disk_describe(const char *bsd, struct diskinfo *info)
{
	DASessionRef session;
	DADiskRef disk;
	CFDictionaryRef d;
	CFTypeRef v;
	int err = 0;

	memset(info, 0, sizeof(*info));
	snprintf(info->bsd, sizeof(info->bsd), "%s", bsd);
	session = DASessionCreate(kCFAllocatorDefault);
	if (!session)
		return ENOMEM;
	disk = DADiskCreateFromBSDName(kCFAllocatorDefault, session, bsd);
	if (!disk) {
		CFRelease(session);
		return ENOENT;
	}
	d = DADiskCopyDescription(disk);
	if (!d) {
		err = ENOENT;
		goto out;
	}
	cfstr(CFDictionaryGetValue(d, kDADiskDescriptionVolumeKindKey),
		info->kind, sizeof(info->kind));
	cfstr(CFDictionaryGetValue(d, kDADiskDescriptionVolumeNameKey),
		info->label, sizeof(info->label));
	cfstr(CFDictionaryGetValue(d, kDADiskDescriptionMediaContentKey),
		info->content, sizeof(info->content));
	v = CFDictionaryGetValue(d, kDADiskDescriptionMediaSizeKey);
	if (v && CFGetTypeID(v) == CFNumberGetTypeID())
		CFNumberGetValue(v, kCFNumberSInt64Type, &info->size);
	v = CFDictionaryGetValue(d, kDADiskDescriptionMediaRemovableKey);
	info->removable = v == kCFBooleanTrue;
	v = CFDictionaryGetValue(d, kDADiskDescriptionDeviceInternalKey);
	info->internal = v == kCFBooleanTrue;
	v = CFDictionaryGetValue(d, kDADiskDescriptionVolumePathKey);
	if (v && CFGetTypeID(v) == CFURLGetTypeID()) {
		struct statfs sf;

		CFURLGetFileSystemRepresentation(v, true,
			(UInt8 *)info->mountpoint, sizeof(info->mountpoint));
		if (!statfs(info->mountpoint, &sf))
			snprintf(info->fstype, sizeof(info->fstype), "%s",
				sf.f_fstypename);
	}
	CFRelease(d);
out:
	CFRelease(disk);
	CFRelease(session);
	return err;
}

int disk_list_ntfs(void (*cb)(const struct diskinfo *info, void *ctx),
		void *ctx)
{
	io_iterator_t it;
	io_object_t media;
	CFMutableDictionaryRef match = IOServiceMatching(kIOMediaClass);

	if (!match)
		return ENOMEM;
	CFDictionarySetValue(match, CFSTR(kIOMediaLeafKey), kCFBooleanTrue);
	if (IOServiceGetMatchingServices(kIOMainPortDefault, match, &it) !=
			KERN_SUCCESS)
		return EIO;
	while ((media = IOIteratorNext(it))) {
		CFTypeRef name = IORegistryEntryCreateCFProperty(media,
				CFSTR(kIOBSDNameKey), kCFAllocatorDefault, 0);
		char bsd[64];
		struct diskinfo info;

		cfstr(name, bsd, sizeof(bsd));
		if (name)
			CFRelease(name);
		IOObjectRelease(media);
		if (!bsd[0] || disk_describe(bsd, &info))
			continue;
		if (!strcmp(info.kind, "ntfs") ||
				!strcmp(info.content, "Windows_NTFS"))
			cb(&info, ctx);
	}
	IOObjectRelease(it);
	return 0;
}

struct da_wait {
	dispatch_semaphore_t sem;
	int err;
	char msg[256];
};

static void da_done(DADiskRef disk, DADissenterRef dissenter, void *ctx)
{
	struct da_wait *w = ctx;

	(void)disk;
	if (dissenter) {
		DAReturn st = DADissenterGetStatus(dissenter);
		CFStringRef s = DADissenterGetStatusString(dissenter);

		w->err = st == kDAReturnBusy ? EBUSY :
			st == kDAReturnNotPermitted ? EPERM : EIO;
		if (s)
			CFStringGetCString(s, w->msg, sizeof(w->msg),
					kCFStringEncodingUTF8);
	}
	dispatch_semaphore_signal(w->sem);
}

static int da_run(const char *bsd, bool eject, bool force)
{
	DASessionRef session = DASessionCreate(kCFAllocatorDefault);
	dispatch_queue_t q;
	struct da_wait w = { 0 };
	DADiskRef disk, whole = NULL;

	if (!session)
		return ENOMEM;
	q = dispatch_queue_create("ntfs4mac.da", DISPATCH_QUEUE_SERIAL);
	DASessionSetDispatchQueue(session, q);
	disk = DADiskCreateFromBSDName(kCFAllocatorDefault, session, bsd);
	if (!disk) {
		w.err = ENOENT;
		goto out;
	}
	w.sem = dispatch_semaphore_create(0);
	if (eject) {
		whole = DADiskCopyWholeDisk(disk);
		if (!whole) {
			w.err = ENOENT;
			goto out;
		}
		DADiskUnmount(whole, kDADiskUnmountOptionWhole |
			(force ? kDADiskUnmountOptionForce : 0), da_done, &w);
		dispatch_semaphore_wait(w.sem, DISPATCH_TIME_FOREVER);
		if (!w.err) {
			DADiskEject(whole, kDADiskEjectOptionDefault, da_done,
				&w);
			dispatch_semaphore_wait(w.sem, DISPATCH_TIME_FOREVER);
		}
	} else {
		DADiskUnmount(disk, force ? kDADiskUnmountOptionForce :
			kDADiskUnmountOptionDefault, da_done, &w);
		dispatch_semaphore_wait(w.sem, DISPATCH_TIME_FOREVER);
	}
	if (w.err && w.msg[0])
		fprintf(stderr, "ntfs4mac: %s\n", w.msg);
out:
	DASessionSetDispatchQueue(session, NULL);
	if (whole)
		CFRelease(whole);
	if (disk)
		CFRelease(disk);
	CFRelease(session);
	dispatch_release(q);
	return w.err;
}

int disk_unmount(const char *bsd, bool force)
{
	return da_run(bsd, false, force);
}

int disk_eject(const char *bsd)
{
	return da_run(bsd, true, false);
}
