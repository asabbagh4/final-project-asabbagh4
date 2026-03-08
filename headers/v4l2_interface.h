/***************************************************
 * Module name: v4l2_interface.h
 *
 * First written in 2026 by Laye Tenumah, adapted from Sam Siewert template.
 *
 * Module Description:
 * Public interface for the V4L2 camera driver abstraction
 * layer. Declares the acquisition ring buffer and frame-save
 * structures, all shared V4L2 kernel-API objects, and every
 * function implemented in v4l2_interface.c.
 *
 * All shared compile-time constants (CLEAR, HRES, VRES,
 * PIXEL_SIZE, HRES_STR, VRES_STR, MAX_HRES, MAX_VRES,
 * MAX_PIXEL_SIZE, STARTUP_FRAMES, FRAMES_PER_SEC,
 * DRIVER_MMAP_BUFFERS, COURSE, FRAME_PATH) are defined
 * in capturelib.h, which this header includes. They must
 * not be redefined here.
 *
 * Function categories declared here:
 *   Public  - called by sequencer.c and read_frames.c:
 *               errno_exit(), xioctl(),
 *               v4l2_frame_acquisition_initialization(),
 *               v4l2_frame_acquisition_shutdown()
 *   Private (static, forward-declared for ordering clarity):
 *               open_device(), close_device(),
 *               init_device(), init_mmap(),
 *               start_capturing(), stop_capturing(),
 *               uninit_device()
 *
 ***************************************************/

#ifndef _V4LT_INTERFACE_
#define _V4LT_INTERFACE_

/* capturelib.h is the single source of truth for all shared constants.
 * Including it here means any translation unit that includes only
 * v4l2_interface.h still has access to CLEAR, HRES, VRES, etc. */
#include "capturelib.h"

/*  Type definitions
***************************************************/

/* Forward declarations for kernel V4L2 types used in extern variable
 * declarations below. The full definitions come from <linux/videodev2.h>,
 * which is included only in the .c files. */
struct v4l2_format;
struct v4l2_buffer;

/*
 * buffer - Tracks a single kernel mmap region mapped into user space.
 *
 * Defined here once. All .c files that previously redefined this struct
 * locally have had those definitions removed; they rely on this header.
 *
 * Members:
 *   start  - User-space virtual address returned by mmap().
 *   length - Byte length of the region, from VIDIOC_QUERYBUF.length.
 */
struct buffer
{
    void *start;
    size_t length;
};

/*
 * save_frame_t - One complete captured frame with its acquisition metadata.
 *
 * Members:
 *   frame[]        - Raw pixel data; HRES * VRES * PIXEL_SIZE bytes.
 *   time_stamp     - CLOCK_MONOTONIC value recorded when the frame was dequeued.
 *   identifier_str - Human-readable label (e.g. a frame-index string).
 */
struct save_frame_t
{
    unsigned char frame[HRES * VRES * PIXEL_SIZE];
    struct timespec time_stamp;
    char identifier_str[80];
};

/*
 * ring_buffer_t - Lock-free circular buffer connecting the acquisition
 *                 and processing services.
 *
 * Holds 3 * FRAMES_PER_SEC slots so that at 1 Hz the most recent three
 * seconds of raw frames are always available without overwriting data
 * that the processing service has not yet consumed.
 *
 * Members:
 *   ring_size    - Total allocated slots (3 * FRAMES_PER_SEC).
 *   tail_idx     - Next slot to be written by the acquisition service.
 *   head_idx     - Next slot to be consumed by the processing service.
 *   count        - Number of unread frames currently held in the buffer.
 *   save_frame[] - Array of frame-storage structs, one per slot.
 */
struct ring_buffer_t
{
    unsigned int ring_size;
    int tail_idx;
    int head_idx;
    int count;
    struct save_frame_t save_frame[3 * FRAMES_PER_SEC];
};

/*  External variable declarations
***************************************************/

/* Active V4L2 pixel format and geometry; negotiated once in init_device()
 * and read by all downstream color-conversion and storage functions. */
extern struct v4l2_format fmt;

/* Reused V4L2 buffer descriptor; populated by VIDIOC_DQBUF on every
 * frame dequeue in read_frame(). */
extern struct v4l2_buffer frame_buf;

/* Running frame counter. Starts at -STARTUP_FRAMES so it reaches 0
 * on the first post-warmup frame, enabling timestamp and FPS logic. */
extern int read_framecnt;

/* Array of user-space mmap region descriptors; one entry per driver buffer.
 * Allocated in init_mmap() and freed in uninit_device(). */
extern struct buffer *buffers;

/* Shared inter-service ring buffer connecting acquisition to processing.
 * Tail is advanced by seq_frame_read(); head is advanced by seq_frame_process(). */
extern struct ring_buffer_t ring_buffer;

/*  Public function prototypes
***************************************************/

/**************************************************
 * Function name : void errno_exit(const char *s)
 *    returns    : void (does not return)
 *    s          : prefix string printed before the errno description
 * Created by    : course 4 team
 * Date created  : 2024
 * Description   : Prints a formatted error message containing s, the
 *                 numeric errno value, and its human-readable strerror
 *                 description, then terminates the process with EXIT_FAILURE.
 * Notes         : Use only for unrecoverable V4L2 errors. Retriable
 *                 conditions such as EAGAIN and EINTR must be handled
 *                 by the caller before reaching this function.
 **************************************************/
void errno_exit(const char *s);

/**************************************************
 * Function name : int xioctl(int fh, int request, void *arg)
 *    returns    : ioctl return code (0 on success, -1 on error with errno set)
 *    fh         : open file descriptor for the V4L2 device node
 *    request    : V4L2 ioctl command code (VIDIOC_*)
 *    arg        : pointer to the command-specific argument structure
 * Created by    : course 4 team
 * Date created  : 2024
 * Description   : Wrapper around ioctl(2) that automatically retries the
 *                 call whenever it is interrupted by a signal (EINTR).
 *                 All other return codes are passed through to the caller.
 * Notes         : EINTR occurs frequently in signal-driven RT systems.
 *                 The retry loop guarantees the ioctl eventually completes
 *                 or returns a genuine error code.
 **************************************************/
int xioctl(int fh, int request, void *arg);

/**************************************************
 * Function name : int v4l2_frame_acquisition_initialization(char *dev_name)
 *    returns    : 0 on success; calls errno_exit() and does not return
 *                 on any V4L2 initialization failure
 *    dev_name   : path to the V4L2 character device node (e.g. "/dev/video0")
 * Created by    : course 4 team
 * Date created  : 2024
 * Description   : Executes the complete V4L2 pipeline startup sequence:
 *                   1. open_device()     - validates and opens the device node
 *                   2. init_device()     - queries capabilities, negotiates
 *                                         YUYV format at HRES x VRES, and
 *                                         calls init_mmap()
 *                   3. start_capturing() - enqueues all mmap buffers and
 *                                         issues VIDIOC_STREAMON
 *                   4. Records fstart using CLOCK_MONOTONIC_RAW as the
 *                      FPS and elapsed-time reference epoch.
 * Notes         : Call exactly once from main() before creating service
 *                 threads or arming the sequencer timer.
 *                 Uses CLOCK_MONOTONIC_RAW to match MY_CLOCK_TYPE in
 *                 sequencer.h and avoid NTP slew affecting the baseline.
 **************************************************/
int v4l2_frame_acquisition_initialization(char *dev_name);

/**************************************************
 * Function name : int v4l2_frame_acquisition_shutdown(void)
 *    returns    : 0 on success
 * Created by    : course 4 team
 * Date created  : 2024
 * Description   : Executes the complete V4L2 pipeline shutdown sequence:
 *                   1. stop_capturing() - issues VIDIOC_STREAMOFF and
 *                                         records the stop timestamp (fstop)
 *                   2. Prints total capture time and achieved frame rate
 *                   3. uninit_device()  - unmaps all mmap regions and
 *                                         frees the buffers[] array
 *                   4. close_device()   - closes camera_device_fd
 * Notes         : Call from main() only after all service threads have
 *                 been joined. The read_framecnt + 1 adjustment in the
 *                 FPS calculation corrects for the -STARTUP_FRAMES
 *                 initialization value.
 **************************************************/
int v4l2_frame_acquisition_shutdown(void);

/*  Private (static) function prototypes
 *
 * These functions are file-scoped in v4l2_interface.c. They are declared
 * here so the full call graph is visible at a glance and to allow future
 * unit-test harnesses to reference them without modifying the source file.
 * Do not call them from outside this module.
 ***************************************************/

/**************************************************
 * Function name : static void open_device(char *dev_name)
 *    returns    : void; calls errno_exit() on any failure
 *    dev_name   : path to the V4L2 character device node
 * Created by    : course 4 team
 * Date created  : 2024
 * Description   : Verifies that dev_name refers to a character device via
 *                 stat(2), then opens it in read-write non-blocking mode
 *                 and stores the result in camera_device_fd.
 * Notes         : O_NONBLOCK is required so that seq_frame_read() can
 *                 use select(2) for timeout-guarded dequeue rather than
 *                 blocking indefinitely inside read_frame().
 **************************************************/
static void open_device(char *dev_name);

/**************************************************
 * Function name : static void close_device(void)
 *    returns    : void; calls errno_exit() on failure
 * Created by    : course 4 team
 * Date created  : 2024
 * Description   : Closes camera_device_fd and resets it to -1 so that
 *                 any subsequent accidental use is detectable. Called
 *                 only from v4l2_frame_acquisition_shutdown().
 **************************************************/
static void close_device(void);

/**************************************************
 * Function name : static void init_device(char *dev_name)
 *    returns    : void; calls errno_exit() on V4L2 errors
 *    dev_name   : device node path, used in capability error messages
 * Created by    : course 4 team
 * Date created  : 2024
 * Description   : Queries V4L2 capability flags to confirm the device
 *                 supports video capture and streaming I/O. Attempts to
 *                 reset the crop rectangle to the driver default (errors
 *                 silently ignored). When force_format is set, negotiates
 *                 YUYV progressive format at HRES x VRES via VIDIOC_S_FMT;
 *                 otherwise preserves the existing driver format.
 *                 Applies paranoia corrections to bytesperline and
 *                 sizeimage, then calls init_mmap().
 * Notes         : VIDIOC_S_FMT may silently adjust width or height to
 *                 the nearest hardware-supported value. Verify fmt fields
 *                 after the call if exact resolution is critical.
 **************************************************/
static void init_device(char *dev_name);

/**************************************************
 * Function name : static void init_mmap(char *dev_name)
 *    returns    : void; calls errno_exit() on V4L2 or mmap errors
 *    dev_name   : device node path, used only in error messages
 * Created by    : course 4 team
 * Date created  : 2024
 * Description   : Requests DRIVER_MMAP_BUFFERS kernel buffers via
 *                 VIDIOC_REQBUFS, queries each buffer's size with
 *                 VIDIOC_QUERYBUF, and maps it into user space with
 *                 mmap(). Stores each mapped region in buffers[].
 *                 Also zeroes and sizes ring_buffer to
 *                 3 * FRAMES_PER_SEC slots.
 * Notes         : Exits if the driver allocates fewer than 2 buffers.
 *                 MAP_FAILED from mmap() is treated as a fatal error.
 *                 ring_buffer must be initialized here, before the first
 *                 call to seq_frame_read(), so tail and head start at 0.
 **************************************************/
static void init_mmap(char *dev_name);

/**************************************************
 * Function name : static void start_capturing(void)
 *    returns    : void; calls errno_exit() on V4L2 errors
 * Created by    : course 4 team
 * Date created  : 2024
 * Description   : Enqueues every allocated mmap buffer into the driver's
 *                 incoming queue via VIDIOC_QBUF, then issues
 *                 VIDIOC_STREAMON to start the DMA transfer engine.
 *                 After this call the sensor begins filling buffers in
 *                 round-robin order as frames arrive.
 * Notes         : All n_buffers buffers must be enqueued before STREAMON;
 *                 leaving any buffer outside the queue reduces pipeline
 *                 depth and increases frame-drop risk.
 **************************************************/
static void start_capturing(void);

/**************************************************
 * Function name : static void stop_capturing(void)
 *    returns    : void; calls errno_exit() on V4L2 errors
 * Created by    : course 4 team
 * Date created  : 2024
 * Description   : Samples the CLOCK_MONOTONIC stop timestamp into
 *                 time_stop and fstop immediately before issuing
 *                 VIDIOC_STREAMOFF to halt the DMA transfer engine.
 *                 After this call no further frames are delivered.
 * Notes         : The timestamp is recorded before STREAMOFF rather than
 *                 after to minimize latency error in the final FPS
 *                 calculation printed by v4l2_frame_acquisition_shutdown().
 **************************************************/
static void stop_capturing(void);

/**************************************************
 * Function name : static void uninit_device(void)
 *    returns    : void; calls errno_exit() if munmap() fails
 * Created by    : course 4 team
 * Date created  : 2024
 * Description   : Iterates over buffers[0..n_buffers-1], unmaps each
 *                 kernel region with munmap(), then frees the buffers[]
 *                 tracking array. Must be called only after stop_capturing()
 *                 has issued VIDIOC_STREAMOFF, so the driver is no longer
 *                 writing into the mapped regions.
 **************************************************/
static void uninit_device(void);

#endif /* _V4LT_INTERFACE_ */
