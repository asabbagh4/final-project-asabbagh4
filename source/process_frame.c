/***************************************************
 * Module name: process_frame.c
 *
 * First written in 2026 by Laye Tenumah, adapted from Sam Siewert template.
 *
 * Module Description:
 * Implements frame color-space conversion and inter-frame
 * motion detection for the camera capture pipeline.
 *
 * process_image() dispatches incoming raw frames to the
 * correct conversion path (YUYV-to-RGB or YUYV-to-gray)
 * based on the pixel format negotiated during V4L2 init.
 *
 * process_motion_detection() maintains a two-slot ring
 * buffer of grayscale frames and computes the normalized
 * absolute-difference percentage between consecutive frames
 * to classify whether significant motion has occurred.
 *
 * seq_frame_process() is the sequencer-facing entry point
 * that pulls a frame from the acquisition ring buffer and
 * drives the full conversion+detection pipeline.
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

/*  Module-wide variable definitions
***************************************************/

/* Motion detection ring buffer; initialized to an empty two-slot buffer.
 * See process_frame.h for the full field description. */
struct motion_buffer_t motion_buffer = {
    .ring_size = 2,
    .tail_idx = 0,
    .count = 0};

/* Scratch area used during color-space conversion. Sized for the
 * maximum possible frame to avoid reallocation. */
unsigned char scratchpad_buffer[MAX_HRES * MAX_VRES * MAX_PIXEL_SIZE];

/* Running count of frames submitted to process_image(). */
int process_framecnt = 0;

/**************************************************
 * Function name : void yuv2rgb(int y, int u, int v,
 *                              unsigned char *r,
 *                              unsigned char *g,
 *                              unsigned char *b)
 *    returns    : void
 *    y          : luminance sample (0-255)
 *    u          : Cb chrominance sample (0-255, 128 = neutral)
 *    v          : Cr chrominance sample (0-255, 128 = neutral)
 *    r          : output red   channel (0-255, clamped)
 *    g          : output green channel (0-255, clamped)
 *    b          : output blue  channel (0-255, clamped)
 * Created by    : course 4 team
 * Date created  : 2024
 * Description   : Converts a single YUV (YCbCr) sample to RGB using
 *                 integer-only arithmetic. Coefficients are scaled by
 *                 256 and shifted right by 8 bits to approximate the
 *                 BT.601 floating-point matrix without an FPU.
 *                 Output values are clamped to [0, 255].
 * Notes         : Formula: c = y-16, d = u-128, e = v-128
 *                   R = (298c        + 409e + 128) >> 8
 *                   G = (298c - 100d - 208e + 128) >> 8
 *                   B = (298c + 516d        + 128) >> 8
 **************************************************/
void yuv2rgb(int y, int u, int v,
             unsigned char *r, unsigned char *g, unsigned char *b)
{
    int r1, g1, b1;

    /* Offset components per BT.601 specification. */
    int c = y - 16, d = u - 128, e = v - 128;

    /* Integer approximation of BT.601 YCbCr-to-RGB matrix. */
    r1 = (298 * c + 409 * e + 128) >> 8;
    g1 = (298 * c - 100 * d - 208 * e + 128) >> 8;
    b1 = (298 * c + 516 * d + 128) >> 8;

    /* Clamp all channels to valid 8-bit range. */
    if (r1 > 255)
        r1 = 255;
    if (g1 > 255)
        g1 = 255;
    if (b1 > 255)
        b1 = 255;

    if (r1 < 0)
        r1 = 0;
    if (g1 < 0)
        g1 = 0;
    if (b1 < 0)
        b1 = 0;

    *r = r1;
    *g = g1;
    *b = b1;
}

/**************************************************
 * Function name : int process_motion_detection(
 *                     const unsigned char *current_frame,
 *                     int frame_size)
 *    returns    :  0 - first frame stored, no comparison available
 *                  1 - motion detected (diff percentage > 0.4%)
 *                 -1 - no significant motion detected
 *    current_frame : pointer to a grayscale frame (HRES*VRES bytes)
 *    frame_size    : byte length of current_frame
 * Created by    : course 4 team
 * Date created  : 2024
 * Description   : Compares current_frame against the most recently stored
 *                 frame in motion_buffer using a per-pixel absolute
 *                 difference. Normalizes the total difference against the
 *                 theoretical maximum (frame_size * 255) to yield a
 *                 percentage. If the percentage exceeds 0.4%, motion is
 *                 declared. In all cases the current frame is stored into
 *                 motion_buffer for the next comparison.
 * Notes         : The 0.4% threshold was tuned empirically; adjust to
 *                 trade off sensitivity against false positives.
 *                 The ring buffer wraps at ring_size (2) slots, so only
 *                 the immediately preceding frame is compared.
 **************************************************/
int process_motion_detection(const unsigned char *current_frame, int frame_size)
{
    unsigned int diffsum = 0;
    double diff_percentage = 0.0;
    int i;
    int current_idx = motion_buffer.tail_idx;
    int prev_idx;
    unsigned int max_possible_diff;

    /* On the very first call there is no previous frame to compare against;
     * just store the incoming frame and report back. */
    if (motion_buffer.count == 0)
    {
        memcpy(motion_buffer.frames[current_idx], current_frame, frame_size);

        motion_buffer.tail_idx = (motion_buffer.tail_idx + 1) % motion_buffer.ring_size;
        motion_buffer.count++;

        return 0;
    }

    /* Locate the slot holding the most recently stored frame. */
    prev_idx = (current_idx - 1 + motion_buffer.ring_size) % motion_buffer.ring_size;

    /* Sum absolute per-pixel differences between current and previous frame. */
    for (i = 0; i < frame_size; i++)
    {
        int diff = abs((int)current_frame[i] - (int)motion_buffer.frames[prev_idx][i]);
        diffsum += diff;
    }

    /* Normalize: express total difference as a percentage of the worst case. */
    max_possible_diff = frame_size * 255;
    diff_percentage = (double)diffsum / (double)max_possible_diff * 100.0;

    /* Store current frame for use as the previous frame on the next call,
     * regardless of whether motion was detected. */
    memcpy(motion_buffer.frames[current_idx], current_frame, frame_size);
    motion_buffer.tail_idx = (motion_buffer.tail_idx + 1) % motion_buffer.ring_size;
    if (motion_buffer.count < motion_buffer.ring_size)
    {
        motion_buffer.count++;
    }

    if (diff_percentage > 0.4)
    {
        return 1; /* Motion detected. */
    }
    else
    {
        return -1; /* No significant motion. */
    }
}

/**************************************************
 * Function name : void reset_motion_buffer(void)
 *    returns    : void
 * Created by    : course 4 team
 * Date created  : 2024
 * Description   : Zeroes all frame data in motion_buffer and resets
 *                 the tail index and count to their initial values.
 *                 Call when reinitializing the pipeline or after a
 *                 known scene change makes previous frames irrelevant.
 **************************************************/
void reset_motion_buffer(void)
{
    motion_buffer.tail_idx = 0;
    motion_buffer.count = 0;
    memset(motion_buffer.frames, 0, sizeof(motion_buffer.frames));
    printf("Motion buffer reset\n");
}

/**************************************************
 * Function name : static int process_image(const void *p, int size)
 *    returns    : running process_framecnt after this call
 *    p          : pointer to the raw frame data in the ring buffer
 *    size       : byte length of the raw frame (HRES*VRES*PIXEL_SIZE)
 * Created by    : course 4 team
 * Date created  : 2024
 * Description   : Dispatches the raw frame to the appropriate
 *                 color-space conversion path based on the pixel format
 *                 stored in the global 'fmt' structure:
 *
 *                   V4L2_PIX_FMT_GREY  - No conversion; frame is already
 *                                        grayscale and passed through.
 *                   V4L2_PIX_FMT_YUYV  - COLOR_CONVERT_RGB: unpack YUYV
 *                                        macropixels to RGB triplets and
 *                                        store in scratchpad_buffer.
 *                                        COLOR_CONVERT_GRAY: extract only
 *                                        the Y (luma) bytes from YUYV
 *                                        macropixels and store in
 *                                        scratchpad_buffer, then invoke
 *                                        process_motion_detection().
 *                   V4L2_PIX_FMT_RGB24 - No conversion needed; logged only.
 *
 * Notes         : YUYV packs two pixels as [Y0 U Y1 V] in 4 bytes.
 *                 The RGB path expands this to 6 bytes (RGBRGB).
 *                 The gray path extracts bytes 0 and 2 to get Y0 and Y1,
 *                 yielding size/2 luma samples.
 **************************************************/
static int process_image(const void *p, int size)
{
    int i, newi;
    int y_temp, y2_temp, u_temp, v_temp;
    unsigned char *frame_ptr = (unsigned char *)p;

    process_framecnt++;

    if (fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_GREY)
    {
        /* Camera is already delivering raw grayscale; no conversion required. */
    }
    else if (fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_YUYV)
    {
#if defined(COLOR_CONVERT_RGB)
        /* Unpack YUYV macropixels (4 bytes) to RGB triplet pairs (6 bytes).
         * Each macropixel [Y0 U Y1 V] produces two RGB pixels sharing U and V. */
        for (i = 0, newi = 0; i < size; i = i + 4, newi = newi + 6)
        {
            y_temp = (int)frame_ptr[i];
            u_temp = (int)frame_ptr[i + 1];
            y2_temp = (int)frame_ptr[i + 2];
            v_temp = (int)frame_ptr[i + 3];

            yuv2rgb(y_temp, u_temp, v_temp,
                    &scratchpad_buffer[newi],
                    &scratchpad_buffer[newi + 1],
                    &scratchpad_buffer[newi + 2]);

            yuv2rgb(y2_temp, u_temp, v_temp,
                    &scratchpad_buffer[newi + 3],
                    &scratchpad_buffer[newi + 4],
                    &scratchpad_buffer[newi + 5]);
        }

#elif defined(COLOR_CONVERT_GRAY)
        /* Extract only the Y (luma) bytes from each YUYV macropixel.
         * Byte 0 = Y0, byte 2 = Y1; bytes 1 and 3 (U, V) are discarded.
         * Result is size/2 grayscale bytes stored in scratchpad_buffer. */
        for (i = 0, newi = 0; i < size; i = i + 4, newi = newi + 2)
        {
            scratchpad_buffer[newi] = frame_ptr[i];         /* Y0 */
            scratchpad_buffer[newi + 1] = frame_ptr[i + 2]; /* Y1 */
        }

        /* Run motion analysis on the freshly extracted luma plane. */
        process_motion_detection(scratchpad_buffer, size / 2);
#endif
    }
    else if (fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_RGB24)
    {
        /* Frame is already in RGB24; no conversion needed. */
        printf("NO PROCESSING for RGB as-is size %d\n", size);
    }
    else
    {
        printf("NO PROCESSING ERROR - unknown format\n");
    }

    return process_framecnt;
}

/**************************************************
 * Function name : int seq_frame_process(void)
 *    returns    : running process_framecnt after this call,
 *                 or 0 if read_framecnt indicates no valid frames yet
 * Created by    : course 4 team
 * Date created  : 2024
 * Description   : Sequencer entry point called by Service_2_frame_process().
 *                 Advances the ring buffer head index to a frame that has
 *                 been populated by the acquisition service, invokes
 *                 process_image() on it, then adjusts the ring buffer
 *                 count to reflect the frames consumed.
 *                 Updates fnow for downstream FPS logging.
 * Notes         : The head index arithmetic (advance by 2, then by 3)
 *                 and the fixed count decrement (subtract 5) implement
 *                 a specific skip pattern for the current ring geometry;
 *                 review if ring_size or FRAMES_PER_SEC change.
 **************************************************/
int seq_frame_process(void)
{
    int cnt = 0;

    /* Advance head by 2 to skip to the target frame slot. */
    ring_buffer.head_idx = (ring_buffer.head_idx + 2) % ring_buffer.ring_size;

    if (read_framecnt > 0)
    {
        cnt = process_image(
            (void *)&(ring_buffer.save_frame[ring_buffer.head_idx].frame[0]),
            HRES * VRES * PIXEL_SIZE);
    }

    /* Advance head by 3 more for the next call's starting position. */
    ring_buffer.head_idx = (ring_buffer.head_idx + 3) % ring_buffer.ring_size;

    /* Decrement count by the number of slots effectively consumed this cycle. */
    ring_buffer.count = ring_buffer.count - 5;

    /* Sample current time for FPS calculation. */
    if (process_framecnt > 0)
    {
        clock_gettime(CLOCK_MONOTONIC, &time_now);
        fnow = (double)time_now.tv_sec + (double)time_now.tv_nsec / 1000000000.0;
    }

    return cnt;
}
