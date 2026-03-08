/***************************************************
 * Module name: read_frames.c
 *
 * First written in 2026 by Laye Tenumah, adapted from Sam Siewert template.
 *
 * Module Description:
 * Implements the frame-acquisition layer between the
 * V4L2 kernel driver and the application ring buffer.
 * read_frame() dequeues one buffer from the driver via
 * VIDIOC_DQBUF, and seq_frame_read() wraps it with a
 * select(2) timeout guard, copies the pixel data into
 * the acquisition ring buffer, and immediately re-queues
 * the driver buffer via VIDIOC_QBUF.
 *
 ***************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <syslog.h>

#include <getopt.h> /* getopt_long() */

#include <fcntl.h> /* low-level i/o */
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

#include <linux/videodev2.h>

#include <time.h>
#include "../headers/capturelib.h"
#include "../headers/v4l2_interface.h"

/* COURSE is defined in capturelib.h, which is included via v4l2_interface.h. */

/* struct buffer is defined in v4l2_interface.h, which is included above.
 * The local redefinition was removed to prevent a conflicting-type error. */

/**************************************************
 * Function name : static int read_frame(void)
 *    returns    : 1 on a successful dequeue, 0 if the
 *                 driver returned EAGAIN or EIO
 * Created by    : course 4 team
 * Date created  : 2024
 * Description   : Dequeues one frame buffer from the V4L2
 *                 driver using VIDIOC_DQBUF. Records the
 *                 monotonic start timestamp on the first
 *                 successful dequeue (read_framecnt == 0).
 *                 Exits the process on unrecoverable ioctl errors.
 * Notes         : EAGAIN is a non-fatal condition in O_NONBLOCK mode.
 *                 EIO may indicate a transient driver issue and is
 *                 also treated as non-fatal here.
 **************************************************/
static int read_frame(void)
{
    CLEAR(frame_buf);

    frame_buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    frame_buf.memory = V4L2_MEMORY_MMAP;

    if (-1 == xioctl(camera_device_fd, VIDIOC_DQBUF, &frame_buf))
    {
        switch (errno)
        {
        case EAGAIN:
            /* No buffer available yet in non-blocking mode; caller should retry. */
            return 0;

        case EIO:
            /* Driver-level I/O error; some drivers raise this for non-fatal
             * conditions, so treat as retriable rather than fatal. */
            return 0;

        default:
            printf("mmap failure\n");
            errno_exit("VIDIOC_DQBUF");
        }
    }

    read_framecnt++;

    /* Capture the monotonic start time on the first valid post-warmup frame. */
    if (read_framecnt == 0)
    {
        clock_gettime(CLOCK_MONOTONIC, &time_start);
        fstart = (double)time_start.tv_sec + (double)time_start.tv_nsec / 1000000000.0;
    }

    /* Sanity-check that the returned buffer index is within the allocated pool. */
    assert(frame_buf.index < n_buffers);

    return 1;
}

/**************************************************
 * Function name : int seq_frame_read(void)
 *    returns    : (int) - reserved for future status; currently no explicit return
 * Created by    : course 4 team
 * Date created  : 2024
 * Description   : Entry point called by Service_1_frame_acquisition().
 *                 Uses select(2) with a 2-second timeout to wait for a
 *                 readable frame, then delegates to read_frame() to dequeue
 *                 it. On success, copies the raw pixel data from the mmap
 *                 region into the tail slot of the shared acquisition ring
 *                 buffer, advances the tail index, and re-queues the driver
 *                 buffer so the hardware can reuse it.
 * Notes         : The select() return value (rc) is currently not checked;
 *                 a timeout silently falls through to read_frame(). A future
 *                 improvement should log or handle the timeout explicitly.
 *                 Logs and prints FPS statistics when read_framecnt > 0.
 **************************************************/
int seq_frame_read(void)
{
    fd_set fds;
    struct timeval tv;
    int rc;

    FD_ZERO(&fds);
    FD_SET(camera_device_fd, &fds);

    /* Block up to 2 seconds for a frame to become available. */
    tv.tv_sec = 2;
    tv.tv_usec = 0;

    rc = select(camera_device_fd + 1, &fds, NULL, NULL, &tv);

    read_frame();

    /* Copy raw pixel data from the kernel mmap region into the ring buffer's
     * tail slot. The ring buffer provides a user-space copy so the driver
     * buffer can be re-queued immediately without holding a reference. */
    memcpy(
        (void *)&(ring_buffer.save_frame[ring_buffer.tail_idx].frame[0]),
        buffers[frame_buf.index].start,
        frame_buf.bytesused);

    /* Advance the tail and wrap modulo ring size. */
    ring_buffer.tail_idx = (ring_buffer.tail_idx + 1) % ring_buffer.ring_size;
    ring_buffer.count++;

    /* Sample the current time for FPS calculation. */
    clock_gettime(CLOCK_MONOTONIC, &time_now);
    fnow = (double)time_now.tv_sec + (double)time_now.tv_nsec / 1000000000.0;

    /* Re-queue the driver buffer so the hardware can fill it again. */
    if (-1 == xioctl(camera_device_fd, VIDIOC_QBUF, &frame_buf))
        errno_exit("VIDIOC_QBUF");
}
