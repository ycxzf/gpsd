/****************************************************************************

NAME
   shmexport.c - shared-memory export from the daemon

DESCRIPTION
   This is a very lightweight alternative to JSON-over-sockets.  Clients
won't be able to filter by device, and won't get device activation/deactivation
notifications.  But both client and daemon will avoid all the marshalling and
unmarshalling overhead.

PERMISSIONS
   This file is Copyright (c) 2010-2018 by the GPSD project
   SPDX-License-Identifier: BSD-2-clause

***************************************************************************/

#include "gpsd_config.h"

#ifdef SHM_EXPORT_ENABLE

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/time.h>

#include "gpsd.h"
#include "libgps.h" /* for SHM_PSEUDO_FD */


/**
 * If there's an existing segment with the given key and of at least the given
 * size, return its associated ID.
 * If there's an existing segment with the given key, but with size less than
 * the given size, and there are no processes attached to the existing segment,
 * then delete the segment, create a new segment of the given size, then
 * return the new segment's associated ID.
 * Otherwise, return -1, with errno set meaningfully.
 */
static int recreate_segment(key_t shmkey, struct gps_context_t *context, size_t desired_size, int mode)
{
    struct shmid_ds segment;
    int shmid;
    int saved_errno;

    shmid = shmget(shmkey, desired_size, mode | IPC_CREAT);
    if (shmid != -1) {
	return shmid; /* Segment successfully created/retrieved */
    }
    /* shmget failed to create/retrieve segment of given size */
    saved_errno = errno;
    shmid = shmget(shmkey, 0, 0);
    if (-1 == shmid) {
	errno = saved_errno;
	return -1; /* No existing segment. Unhandled error. */
    }
    if (-1 == shmctl(shmid, IPC_STAT, &segment)) {
	/* Failed to stat segment. Unhandled error. */
	errno = saved_errno;
	return -1;
    }
    if (segment.shm_segsz >= desired_size) {
	/* Segment is already big enough. Unhandled error */
	return -1;
    }
    /* shmget likely failed because the existing segment is too small */
    if (segment.shm_nattch > 0) {
	/* Other process(es) attached. Cannot resize. */
	return -1;
    }
    /* No processes attached to segment. Ok to delete and recreate */
    if (-1 == shmctl(shmid, IPC_RMID, NULL)) {
	/* Cannot delete existing segment */
	errno = saved_errno;
	return -1;
    }
    shmid = shmget(shmkey, desired_size, mode | IPC_CREAT);
    return shmid;
}

bool shm_acquire(struct gps_context_t *context)
/* initialize the shared-memory segment to be used for export */
{
    long shmkey = getenv("GPSD_SHM_KEY") ? strtol(getenv("GPSD_SHM_KEY"), NULL, 0) : GPSD_SHM_KEY;
    int mode = 0666;
    size_t segment_size = sizeof(struct shmexport_t);

    int shmid = recreate_segment((key_t)shmkey, context, segment_size, mode);
    if (shmid == -1) {
	GPSD_LOG(LOG_ERROR, &context->errout,
		 "shmget(0x%lx, %zd, 0666) for SHM export failed: %s\n",
		 shmkey,
		 segment_size,
		 strerror(errno));
	return false;
    } else
	GPSD_LOG(LOG_PROG, &context->errout,
		 "shmget(0x%lx, %zd, 0666) for SHM export succeeded\n",
		 shmkey,
		 segment_size);

    context->shmexport = (void *)shmat(shmid, 0, 0);
    if ((int)(long)context->shmexport == -1) {
	GPSD_LOG(LOG_ERROR, &context->errout,
		 "shmat failed: %s\n", strerror(errno));
	context->shmexport = NULL;
	return false;
    }
    context->shmid = shmid;

    GPSD_LOG(LOG_PROG, &context->errout,
	     "shmat() for SHM export succeeded, segment %d\n", shmid);
    return true;
}

void shm_release(struct gps_context_t *context)
/* release the shared-memory segment used for export */
{
    if (context->shmexport == NULL)
	return;

    /* Mark shmid to go away when no longer used
     * Having it linger forever is bad, and when the size enlarges
     * it can no longer be opened
     */
    if (shmctl(context->shmid, IPC_RMID, NULL) == -1) {
	GPSD_LOG(LOG_WARN, &context->errout,
		 "shmctl for IPC_RMID failed, errno = %d (%s)\n",
		 errno, strerror(errno));
    }
    (void)shmdt((const void *)context->shmexport);
}

void shm_update(struct gps_context_t *context, struct gps_data_t *gpsdata)
/* export an update to all listeners */
{
    if (context->shmexport != NULL)
    {
	static int tick;
	volatile struct shmexport_t *shared = (struct shmexport_t *)context->shmexport;

	++tick;
	/*
	 * Following block of instructions must not be reordered, otherwise
	 * havoc will ensue.
	 *
	 * This is a simple optimistic-concurrency technique.  We write
	 * the second bookend first, then the data, then the first bookend.
	 * Reader copies what it sees in normal order; that way, if we
	 * start to write the segment during the read, the second bookend will
	 * get clobbered first and the data can be detected as bad.
	 *
	 * Of course many architectures, like Intel, make no guarantees
	 * about the actual memory read or write order into RAM, so this
         * is partly wishful thinking.  Thus the need for the memory_barriers()
         * to enforce the required order.
	 */
	shared->bookend2 = tick;
	memory_barrier();
	shared->gpsdata = *gpsdata;
	memory_barrier();
#ifndef USE_QT
	shared->gpsdata.gps_fd = SHM_PSEUDO_FD;
#else
	shared->gpsdata.gps_fd = (void *)(intptr_t)SHM_PSEUDO_FD;
#endif /* USE_QT */
	memory_barrier();
	shared->bookend1 = tick;
    }
}


#endif /* SHM_EXPORT_ENABLE */

/* end */
