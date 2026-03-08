/***************************************************
 * Module name: capturelib.h
 *
 * First written in 2026 by Laye Tenumah, adapted from Sam Siewert template.
 *
 * Module Description:
 * Single source of truth for all compile-time constants
 * and cross-module global variable declarations used
 * throughout the V4L2 camera capture pipeline.
 *
 * This is the only header that defines shared macros.
 * v4l2_interface.h, process_frame.h, and sequencer.h
 * all include this file and must NOT redefine any
 * symbol declared here. That discipline eliminates the
 * redefinition warnings that arise when a translation
 * unit includes more than one pipeline header.
 *
 * Constants are organised into five groups:
 *   1. Buffer utility macro (CLEAR).
 *   2. Resolution limits and active geometry.
 *   3. Frame-count parameters controlling acquisition length.
 *   4. Output-mode flags (color conversion, disk I/O).
 *   5. Logging and path constants shared by all modules.
 *
 ***************************************************/

#ifndef _CAPTURELIB_
#define _CAPTURELIB_

/*  Defines section - buffer utility
***************************************************/

/* Zero-initializes any struct or variable by address using memset.
 * Use before filling any V4L2 or timespec struct to avoid stale fields.
 * Defined here once; all other headers must not redefine it. */
#define CLEAR(x) memset(&(x), 0, sizeof(x))

/*  Defines section - resolution limits
***************************************************/

/* Maximum horizontal resolution supported by pipeline buffer sizing.
 * Raise this value if a higher-resolution sensor is introduced. */
#define MAX_HRES (1920)

/* Maximum vertical resolution supported by pipeline buffer sizing. */
#define MAX_VRES (1080)

/* Maximum bytes per pixel across all supported pixel formats.
 * RGB24 requires 3 bytes per pixel, the worst case. */
#define MAX_PIXEL_SIZE (3)

/*  Defines section - active capture geometry
 *
 * These values describe the resolution and pixel packing actually
 * negotiated with the sensor at runtime. Change them here to
 * reconfigure the entire pipeline; do not change them in individual
 * module headers.
 ***************************************************/

/* Active horizontal resolution in pixels. */
#define HRES (640)

/* Active vertical resolution in pixels. */
#define VRES (480)

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

/*  Defines section - frame-count parameters
***************************************************/

/* Number of frames discarded at startup while the camera auto-adjusts
 * exposure, white balance, and focus. read_framecnt is initialized to
 * the negative of this value so it reaches 0 on the first valid frame. */
#define STARTUP_FRAMES (30)

/* Rate at which the sequencer releases the frame-acquisition service (Hz).
 * Also controls the ring buffer depth: 3 * FRAMES_PER_SEC slots. */
#define FRAMES_PER_SEC (1)

/* Number of trailing guard frames appended after the main capture window.
 * Ensures the final valid frame is not lost during pipeline drain. */
#define LAST_FRAMES (1)

/* Total frames in the main acquisition window, including the guard frame.
 * Adjust this value to change the capture duration. */
#define CAPTURE_FRAMES (300 + LAST_FRAMES)

/* Grand total frames the acquisition loop reads before halting, covering
 * warm-up discards, the main capture window, and guard frames. */
#define FRAMES_TO_ACQUIRE (CAPTURE_FRAMES + STARTUP_FRAMES + LAST_FRAMES)

/*  Defines section - output-mode flags
***************************************************/

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

/*  Defines section - logging and path constants
***************************************************/

/* Course number embedded in every syslog message produced by the pipeline.
 * Defined here once so all three modules (sequencer.c, read_frames.c,
 * store_frame.c) use a consistent value without independent local defines. */
#define COURSE (4)

/* Filesystem path to the ramdisk directory where frame image files are
 * written by store_frame.c. Using a ramdisk avoids SD/eMMC write latency
 * in the real-time storage service thread. */
#define FRAME_PATH "/mnt/ramdisk/frames"

/* Number of V4L2 driver mmap buffers to request during initialization.
 * A larger pool reduces frame-drop probability during service latency spikes. */
#define DRIVER_MMAP_BUFFERS (6)

/*  External variable declarations
 *
 * All variables below are defined in v4l2_interface.c and shared
 * read-only (or read-write where noted) by all other modules.
 ***************************************************/

/* Forward declaration of struct timespec; avoids pulling <time.h> into
 * every translation unit that includes only this header. */
struct timespec;

/* File descriptor for the opened V4L2 camera device node.
 * Initialized to -1 in v4l2_interface.c; valid only after
 * v4l2_frame_acquisition_initialization() returns successfully. */
extern int camera_device_fd;

/* Count of V4L2 mmap buffers successfully mapped by init_mmap().
 * Used by read_frames.c to validate frame_buf.index bounds. */
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
