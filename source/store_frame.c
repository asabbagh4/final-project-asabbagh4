/***************************************************
 * Module name: store_frame.c
 *
 * First written in 2026 by Laye Tenumah, adapted from Sam Siewert template.
 *
 * Module Description:
 * Implements the frame-storage service for the V4L2
 * capture pipeline. Provides two low-level disk writers:
 *   dump_ppm() - writes an RGB frame as a Netpbm PPM file.
 *   dump_pgm() - writes a grayscale frame as a Netpbm PGM file.
 *
 * save_image() selects the correct writer based on the
 * negotiated pixel format and the active color-conversion
 * compile flag (COLOR_CONVERT_RGB or COLOR_CONVERT_GRAY).
 *
 * seq_frame_store() is the sequencer-facing entry point
 * that guards against an under-filled motion buffer, samples
 * the current timestamp, and dispatches to save_image().
 * It also logs each saved frame via syslog for traceability.
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
#include "../headers/process_frame.h"

/* COURSE and FRAME_PATH are defined in capturelib.h, included via v4l2_interface.h. */

/* struct buffer is defined in v4l2_interface.h, which is included above.
 * The local redefinition was removed to prevent a conflicting-type error. */

/* PPM (color) file header template.
 * snprintf() patches the timestamp fields in-place before each write.
 * Width and height come from the HRES_STR / VRES_STR macros. */
char ppm_header[] = "P6\n#9999999999 sec 9999999999 msec \n" HRES_STR " " VRES_STR "\n255\n";

/* PGM (grayscale) file header template.
 * Same layout as ppm_header but with the P5 magic number. */
char pgm_header[] = "P5\n#9999999999 sec 9999999999 msec \n" HRES_STR " " VRES_STR "\n255\n";

/* Running count of frames that have been successfully written to disk. */
int save_framecnt = 0;

/**************************************************
 * Function name : static void dump_ppm(const void *p, int size,
 *                                      unsigned int tag,
 *                                      struct timespec *time)
 *    returns    : void
 *    p          : pointer to packed RGB24 pixel data
 *    size       : byte length of the pixel data
 *    tag        : monotonically increasing frame index used in filename
 *    time       : monotonic timestamp embedded in the PPM comment field
 * Created by    : course 4 team
 * Date created  : 2024
 * Description   : Writes a single RGB frame to a PPM image file at
 *                 FRAME_PATH/test<tag>.ppm. The file is opened in
 *                 non-blocking mode to avoid stalling the RT thread.
 *                 The header timestamp is patched in-place before writing.
 *                 Pixel data is written in a retry loop until all bytes
 *                 are transferred.
 * Notes         : sizeof(ppm_header)-1 is used to exclude the null
 *                 terminator from the written header length.
 *                 The in-place snprintf calls assume the header template
 *                 reserves exactly 10 characters for each numeric field.
 **************************************************/
static void dump_ppm(const void *p, int size,
                     unsigned int tag, struct timespec *time)
{
    int written, total, dumpfd;
    char filename[256];

    snprintf(filename, sizeof(filename), "%s/test%04d.ppm", FRAME_PATH, tag);

    dumpfd = open(filename, O_WRONLY | O_NONBLOCK | O_CREAT, 00666);

    /* Patch the timestamp fields in the header template. */
    snprintf(&ppm_header[4], 11, "%010d", (int)time->tv_sec);
    strncat(&ppm_header[14], " sec ", 5);
    snprintf(&ppm_header[19], 11, "%010d", (int)((time->tv_nsec) / 1000000));
    strncat(&ppm_header[29], " msec \n" HRES_STR " " VRES_STR "\n255\n", 19);

    /* Write the header, excluding null terminator. */
    written = write(dumpfd, ppm_header, sizeof(ppm_header) - 1);

    /* Retry pixel data writes until the full frame has been transferred. */
    total = 0;
    do
    {
        written = write(dumpfd, p, size);
        total += written;
    } while (total < size);

    /* Update the global timestamp for FPS logging. */
    clock_gettime(CLOCK_MONOTONIC, &time_now);
    fnow = (double)time_now.tv_sec + (double)time_now.tv_nsec / 1000000000.0;

    close(dumpfd);
}

/**************************************************
 * Function name : static void dump_pgm(const void *p, int size,
 *                                      unsigned int tag,
 *                                      struct timespec *time)
 *    returns    : void
 *    p          : pointer to packed grayscale (Y-plane) pixel data
 *    size       : byte length of the pixel data (HRES*VRES for gray)
 *    tag        : monotonically increasing frame index used in filename
 *    time       : monotonic timestamp embedded in the PGM comment field
 * Created by    : course 4 team
 * Date created  : 2024
 * Description   : Writes a single grayscale frame to a PGM image file at
 *                 FRAME_PATH/test<tag>.pgm. Behavior is identical to
 *                 dump_ppm() except it uses the P5 (PGM) header and
 *                 single-channel pixel data.
 * Notes         : See dump_ppm() notes; same in-place patching applies.
 **************************************************/
static void dump_pgm(const void *p, int size,
                     unsigned int tag, struct timespec *time)
{
    int written, total, dumpfd;
    char filename[256];

    snprintf(filename, sizeof(filename), "%s/test%04d.pgm", FRAME_PATH, tag);

    dumpfd = open(filename, O_WRONLY | O_NONBLOCK | O_CREAT, 00666);

    /* Patch the timestamp fields in the header template. */
    snprintf(&pgm_header[4], 11, "%010d", (int)time->tv_sec);
    strncat(&pgm_header[14], " sec ", 5);
    snprintf(&pgm_header[19], 11, "%010d", (int)((time->tv_nsec) / 1000000));
    strncat(&pgm_header[29], " msec \n" HRES_STR " " VRES_STR "\n255\n", 19);

    /* Write the header, excluding null terminator. */
    written = write(dumpfd, pgm_header, sizeof(pgm_header) - 1);

    /* Retry pixel data writes until the full frame has been transferred. */
    total = 0;
    do
    {
        written = write(dumpfd, p, size);
        total += written;
    } while (total < size);

    /* Update the global timestamp for FPS logging. */
    clock_gettime(CLOCK_MONOTONIC, &time_now);
    fnow = (double)time_now.tv_sec + (double)time_now.tv_nsec / 1000000000.0;

    close(dumpfd);
}

/**************************************************
 * Function name : static int save_image(const void *p, int size,
 *                                       struct timespec *frame_time)
 *    returns    : running save_framecnt after this call
 *    p          : pointer to the frame data (format-dependent)
 *    size       : byte length of the frame data
 *    frame_time : monotonic timestamp to embed in the image file header
 * Created by    : course 4 team
 * Date created  : 2024
 * Description   : Selects the correct Netpbm writer (PPM or PGM) based
 *                 on the negotiated V4L2 pixel format and the active
 *                 color-conversion compile flag. All writes come from
 *                 the motion buffer's most-recent slot so that only
 *                 post-conversion grayscale data is persisted.
 *
 *                 Dispatch logic:
 *                   GREY format          -> dump_pgm (raw sensor gray)
 *                   YUYV + RGB convert   -> dump_ppm (converted RGB)
 *                   YUYV + gray convert  -> dump_pgm (Y-plane only)
 *                   RGB24 format         -> dump_ppm (native RGB)
 *
 * Notes         : The most_recent_idx calculation mirrors the one in
 *                 seq_frame_store() and must stay in sync with the
 *                 motion_buffer ring arithmetic.
 *                 DUMP_FRAMES must be defined in capturelib.h for any
 *                 disk writes to occur; if undefined, this function
 *                 increments save_framecnt but performs no I/O.
 **************************************************/
static int save_image(const void *p, int size, struct timespec *frame_time)
{
    unsigned char *frame_ptr = (unsigned char *)p;

    /* Index of the most recently completed frame in the motion buffer. */
    int most_recent_idx = (motion_buffer.tail_idx - 1 + motion_buffer.ring_size) % motion_buffer.ring_size;

    save_framecnt++;

#ifdef DUMP_FRAMES

    if (fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_GREY)
    {
        /* Camera provides native grayscale; write the motion buffer frame directly. */
        dump_pgm(motion_buffer.frames[most_recent_idx], size, save_framecnt, frame_time);
    }
    else if (fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_YUYV)
    {
#if defined(COLOR_CONVERT_RGB)
        if (save_framecnt > 0)
        {
            /* Write the RGB-converted frame; size expands from YUYV 4-byte
             * macropixels to RGB 6-byte triplet pairs, hence (size*6)/4. */
            dump_ppm(frame_ptr, ((size * 6) / 4), save_framecnt, frame_time);
        }

#elif defined(COLOR_CONVERT_GRAY)
        if (save_framecnt > 0)
        {
            /* Write the Y-plane extracted by process_image(); HRES*VRES bytes. */
            dump_pgm(motion_buffer.frames[most_recent_idx],
                     HRES * VRES, save_framecnt, frame_time);
        }
#endif
    }
    else if (fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_RGB24)
    {
        printf("Dump RGB as-is size %d\n", size);
        dump_ppm(frame_ptr, size, process_framecnt, frame_time);
    }
    else
    {
        printf("ERROR - unknown dump format\n");
    }

#endif /* DUMP_FRAMES */

    return save_framecnt;
}

/**************************************************
 * Function name : int seq_frame_store(void)
 *    returns    : running save_framecnt, or 0 if the motion buffer
 *                 does not yet hold at least 2 frames (insufficient
 *                 data for a valid motion-filtered save)
 * Created by    : course 4 team
 * Date created  : 2024
 * Description   : Sequencer entry point called by Service_3_frame_storage().
 *                 Guards against saving before the motion detector has
 *                 accumulated a baseline frame, then samples the current
 *                 monotonic time, resolves the most-recent motion buffer
 *                 slot, and dispatches to save_image().
 *                 Logs each stored frame to syslog with the frame count
 *                 and elapsed time since capture start.
 * Notes         : CLOCK_MONOTONIC_RAW is used here (not CLOCK_MONOTONIC)
 *                 to match the capture-start timestamp from
 *                 v4l2_frame_acquisition_initialization().
 **************************************************/
int seq_frame_store(void)
{
    int cnt;
    int most_recent_idx;

    /* Require at least 2 frames in the motion buffer so that a valid
     * comparison has been performed before anything is written to disk. */
    if (motion_buffer.count < 2)
    {
        return 0;
    }

    /* Sample current time; use RAW to match the acquisition start reference. */
    clock_gettime(CLOCK_MONOTONIC_RAW, &time_now);

    /* Index of the most recently stored frame in the motion buffer. */
    most_recent_idx = (motion_buffer.tail_idx - 1 + motion_buffer.ring_size) % motion_buffer.ring_size;

    cnt = save_image(motion_buffer.frames[most_recent_idx], HRES * VRES, &time_now);

    if (save_framecnt > 0)
    {
        fnow = (double)time_now.tv_sec + (double)time_now.tv_nsec / 1000000000.0;

        syslog(LOG_CRIT,
               "[COURSE #:%d][Final Project][Frame Count:%d]"
               "[Image Capture Start Time:%.6lf seconds]",
               COURSE, save_framecnt, (fnow - fstart));
    }

    return cnt;
}
