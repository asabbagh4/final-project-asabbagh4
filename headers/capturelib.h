/***************************************************
 * Module name: capturelib.h
 *
 * First written in 2026 by Laye Tenumah, adapted from Sam Siewert template.
 *
 * Module Description:
 * Shared compile-time constants and cross-module global
 * variable declarations used throughout the V4L2 camera
 * capture pipeline. This header is included by every
 * translation unit; it intentionally declares no functions
 * of its own. All function prototypes belong to the header
 * of the module that implements them.
 *
 * Constants defined here fall into four groups:
 *   1. Buffer utility macro (CLEAR).
 *   2. Resolution and pixel-size limits.
 *   3. Frame-count parameters controlling acquisition length.
 *   4. Output-mode flags selecting color conversion and disk I/O.
 *
 ***************************************************/

#ifndef _CAPTURELIB_
#define _CAPTURELIB_

/*  Defines section
***************************************************/

/* Zero-initializes any struct or variable by address using memset.
 * Use before filling any V4L2 or timespec struct to avoid stale fields. */
#define CLEAR(x) memset(&(x), 0, sizeof(x))

/* Maximum horizontal resolution supported by the pipeline buffer sizing.
 * Increase this value if a higher-resolution sensor is introduced. */
#define MAX_HRES (1920)

/* Maximum vertical resolution supported by the pipeline buffer sizing. */
#define MAX_VRES (1080)

/* Maximum bytes per pixel across all supported pixel formats.
 * RGB24 requires 3 bytes per pixel, which is the worst case. */
#define MAX_PIXEL_SIZE (3)

/* Active bytes per pixel for the YUYV packed 4:2:2 capture format.
 * Two pixels share one U and one V sample, giving an average of
 * 2 bytes per pixel over any even-width row. */
#define PIXEL_SIZE (2)

/* Active horizontal resolution as a string literal.
 * Embedded directly into PPM and PGM file headers by store_frame.c. */
#define HRES_STR "640"

/* Active vertical resolution as a string literal.
 * Embedded directly into PPM and PGM file headers by store_frame.c. */
#define VRES_STR "480"

/* Number of trailing guard frames appended after the main capture window.
 * Ensures the final valid frame is not lost during pipeline drain. */
#define LAST_FRAMES (1)

/* Total frames in the main acquisition window, including the trailing
 * guard frame. Adjust to change the capture duration. */
#define CAPTURE_FRAMES (300 + LAST_FRAMES)

/* Grand total frames the acquisition loop reads before the pipeline halts,
 * covering warm-up discards, the main capture window, and guard frames. */
#define FRAMES_TO_ACQUIRE (CAPTURE_FRAMES + STARTUP_FRAMES + LAST_FRAMES)

/* Uncomment to override the sequencer-driven rate with a fixed 1 Hz rate.
 * Left disabled so v4l2_interface.h retains control of FRAMES_PER_SEC. */
// #define FRAMES_PER_SEC (1)

/* Uncomment to convert YUYV frames to packed RGB24 before storage.
 * Mutually exclusive with COLOR_CONVERT_GRAY; define exactly one. */
// #define COLOR_CONVERT_RGB

/* Convert YUYV frames to grayscale (Y-channel only) before storage.
 * Mutually exclusive with COLOR_CONVERT_RGB; define exactly one. */
#define COLOR_CONVERT_GRAY

/* When defined, each processed frame is written to the ramdisk as a
 * PGM or PPM image file by store_frame.c. Undefine to disable all
 * disk I/O while keeping the rest of the pipeline active. */
#define DUMP_FRAMES

/*  External variable declarations
 *
 * These variables are defined in v4l2_interface.c and shared
 * across all pipeline modules via this header.
 ***************************************************/

/* Forward declaration of struct timespec to avoid pulling <time.h>
 * into every translation unit that includes this header. */
struct timespec;

/* File descriptor for the opened V4L2 camera device node.
 * Initialized to -1 in v4l2_interface.c; becomes valid only after
 * v4l2_frame_acquisition_initialization() returns successfully. */
extern int camera_device_fd;

/* Count of V4L2 mmap buffers successfully mapped by init_mmap().
 * Used by read_frames.c to validate frame_buf.index against the
 * allocated buffer pool size. */
extern unsigned int n_buffers;

/* Floating-point timestamps in seconds (CLOCK_MONOTONIC or
 * CLOCK_MONOTONIC_RAW where noted):
 *   fnow   - updated after every frame dequeue, process, or store call
 *   fstart - set on the first valid post-warmup frame dequeue
 *   fstop  - set when VIDIOC_STREAMOFF is issued during shutdown */
extern double fnow, fstart, fstop;

/* High-resolution timespec mirrors of fnow, fstart, and fstop.
 * Written by the same code sites that update the double timestamps. */
extern struct timespec time_now, time_start, time_stop;

#endif /* _CAPTURELIB_ */
