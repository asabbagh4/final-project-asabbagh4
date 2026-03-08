/***************************************************
 * Module name: v4l2_interface.c
 *
 * First written in 2026 by Laye Tenumah, adapted from Sam Siewert template.
 *
 * Module Description:
 * Implements the complete V4L2 camera driver abstraction
 * layer. Encapsulates device open/close, capability query,
 * format negotiation, mmap buffer allocation, stream
 * start/stop, and the shared global state that downstream
 * modules (read_frames, process_frame, store_frame) depend on.
 *
 * Public entry points:
 *   v4l2_frame_acquisition_initialization() - opens device and starts stream
 *   v4l2_frame_acquisition_shutdown()       - stops stream, frees resources
 *
 * Internal helpers (file-scope static):
 *   open_device()    - opens and validates the character device node
 *   init_device()    - queries caps, sets format, triggers init_mmap()
 *   init_mmap()      - allocates and maps driver buffers, inits ring buffer
 *   start_capturing()- enqueues all mmap buffers and issues VIDIOC_STREAMON
 *   stop_capturing() - issues VIDIOC_STREAMOFF and records stop timestamp
 *   uninit_device()  - unmaps all mmap buffers and frees the tracking array
 *   close_device()   - closes the device file descriptor
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
#include "../headers/v4l2_interface.h"
#include "../headers/capturelib.h"

/*  Module-wide variable definitions
***************************************************/

/* File descriptor for the opened V4L2 camera device node.
 * Initialized to -1 so callers can detect an uninitialized state. */
int camera_device_fd = -1;

/* Number of mmap buffers successfully allocated by init_mmap(). */
unsigned int n_buffers;

/* When non-zero, init_device() forces YUYV format at HRES x VRES.
 * When zero, the existing driver format is preserved. */
int force_format = 1;

/*
 * buffer - User-space tracking record for a single kernel mmap region.
 *
 * Members:
 *   start  - Virtual address returned by mmap().
 *   length - Byte length of the mapped region (from VIDIOC_QUERYBUF).
 */
struct buffer
{
    void *start;
    size_t length;
};

/* Floating-point timestamps (seconds since epoch on CLOCK_MONOTONIC):
 *   fnow   - most recently sampled time
 *   fstart - monotonic time when the first valid frame was dequeued
 *   fstop  - monotonic time when VIDIOC_STREAMOFF was issued */
double fnow = 0.0, fstart = 0.0, fstop = 0.0;

/* High-resolution timespec mirrors of the floating-point timestamps above. */
struct timespec time_now, time_start, time_stop;

/* Active V4L2 pixel format and geometry; set once in init_device() and
 * referenced by all color-conversion and storage functions. */
struct v4l2_format fmt;

/* Reused V4L2 buffer descriptor; populated by VIDIOC_DQBUF each frame. */
struct v4l2_buffer frame_buf;

/* Array of mmap region descriptors; allocated in init_mmap(). */
struct buffer *buffers;

/* Shared inter-service ring buffer connecting acquisition to processing.
 * Allocated as 3*FRAMES_PER_SEC slots in init_mmap(). */
struct ring_buffer_t ring_buffer;

/* Running frame counter; initialized to -STARTUP_FRAMES so it reaches 0
 * on the first post-warmup frame, enabling timestamp and FPS logic. */
int read_framecnt = -STARTUP_FRAMES;

/**************************************************
 * Function name : void errno_exit(const char *s)
 *    returns    : void (does not return)
 *    s          : prefix string printed before the errno description
 * Created by    : course 4 team
 * Date created  : 2024
 * Description   : Prints a formatted error message including the string s,
 *                 the numeric errno value, and its human-readable description,
 *                 then terminates the process with EXIT_FAILURE.
 * Notes         : Call only on unrecoverable errors; normal retriable
 *                 conditions (EAGAIN, EINTR) should be handled by the caller.
 **************************************************/
void errno_exit(const char *s)
{
    fprintf(stderr, "%s error %d, %s\n", s, errno, strerror(errno));
    exit(EXIT_FAILURE);
}

/**************************************************
 * Function name : int xioctl(int fh, int request, void *arg)
 *    returns    : ioctl return code (0 on success, -1 on error with errno set)
 *    fh         : file descriptor for the V4L2 device
 *    request    : V4L2 ioctl request code (VIDIOC_*)
 *    arg        : pointer to the request-specific argument structure
 * Created by    : course 4 team
 * Date created  : 2024
 * Description   : Wrapper around ioctl(2) that automatically retries when
 *                 the call is interrupted by a signal (EINTR). All other
 *                 error codes are returned to the caller unchanged.
 * Notes         : EINTR can occur legitimately in a signal-heavy RT system;
 *                 the retry loop ensures the ioctl eventually completes.
 **************************************************/
int xioctl(int fh, int request, void *arg)
{
    int rc;

    do
    {
        rc = ioctl(fh, request, arg);
    } while (-1 == rc && EINTR == errno);

    return rc;
}

/**************************************************
 * Function name : static void uninit_device(void)
 *    returns    : void
 * Created by    : course 4 team
 * Date created  : 2024
 * Description   : Unmaps all kernel mmap regions tracked in the buffers[]
 *                 array, then frees the array itself. Call only after
 *                 stop_capturing() has issued VIDIOC_STREAMOFF.
 **************************************************/
static void uninit_device(void)
{
    unsigned int i;

    for (i = 0; i < n_buffers; ++i)
    {
        if (-1 == munmap(buffers[i].start, buffers[i].length))
            errno_exit("munmap");
    }

    free(buffers);
}

/**************************************************
 * Function name : static void init_mmap(char *dev_name)
 *    returns    : void
 *    dev_name   : device node path (used only in error messages)
 * Created by    : course 4 team
 * Date created  : 2024
 * Description   : Requests DRIVER_MMAP_BUFFERS kernel mmap buffers via
 *                 VIDIOC_REQBUFS, then queries and maps each buffer with
 *                 VIDIOC_QUERYBUF and mmap(). Also initializes the
 *                 ring_buffer struct to an empty state sized for
 *                 3*FRAMES_PER_SEC slots.
 * Notes         : Exits if the driver cannot allocate at least 2 buffers.
 *                 MAP_FAILED from mmap() is treated as fatal.
 **************************************************/
static void init_mmap(char *dev_name)
{
    struct v4l2_requestbuffers req;

    CLEAR(req);
    req.count = DRIVER_MMAP_BUFFERS;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    printf("init_mmap req.count=%d\n", req.count);

    /* Initialize ring buffer metadata before any frames can be written. */
    ring_buffer.tail_idx = 0;
    ring_buffer.head_idx = 0;
    ring_buffer.count = 0;
    ring_buffer.ring_size = 3 * FRAMES_PER_SEC;

    if (-1 == xioctl(camera_device_fd, VIDIOC_REQBUFS, &req))
    {
        if (EINVAL == errno)
        {
            fprintf(stderr, "%s does not support memory mapping\n", dev_name);
            exit(EXIT_FAILURE);
        }
        else
        {
            errno_exit("VIDIOC_REQBUFS");
        }
    }

    if (req.count < 2)
    {
        fprintf(stderr, "Insufficient buffer memory on %s\n", dev_name);
        exit(EXIT_FAILURE);
    }
    else
    {
        printf("Device supports %d mmap buffers\n", req.count);
        buffers = calloc(req.count, sizeof(*buffers));
    }

    if (!buffers)
    {
        fprintf(stderr, "Out of memory\n");
        exit(EXIT_FAILURE);
    }

    /* Query each buffer's size and offset, then map it into user space. */
    for (n_buffers = 0; n_buffers < req.count; ++n_buffers)
    {
        CLEAR(frame_buf);
        frame_buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        frame_buf.memory = V4L2_MEMORY_MMAP;
        frame_buf.index = n_buffers;

        if (-1 == xioctl(camera_device_fd, VIDIOC_QUERYBUF, &frame_buf))
            errno_exit("VIDIOC_QUERYBUF");

        buffers[n_buffers].length = frame_buf.length;
        buffers[n_buffers].start = mmap(
            NULL, /* Let the kernel choose the virtual address. */
            frame_buf.length,
            PROT_READ | PROT_WRITE, /* Required for V4L2 mmap. */
            MAP_SHARED,             /* Recommended by V4L2 spec. */
            camera_device_fd,
            frame_buf.m.offset);

        if (MAP_FAILED == buffers[n_buffers].start)
            errno_exit("mmap");

        printf("mapped buffer %d\n", n_buffers);
    }
}

/**************************************************
 * Function name : static void init_device(char *dev_name)
 *    returns    : void
 *    dev_name   : device node path (used in capability and error messages)
 * Created by    : course 4 team
 * Date created  : 2024
 * Description   : Validates V4L2 capability flags (capture + streaming),
 *                 attempts to reset the crop rectangle to the driver default,
 *                 negotiates the pixel format and resolution (YUYV at
 *                 HRES x VRES when force_format is set), applies paranoia
 *                 corrections to bytesperline and sizeimage, then calls
 *                 init_mmap() to allocate buffers.
 * Notes         : Cropping errors are silently ignored per V4L2 spec
 *                 recommendations; not all cameras support crop control.
 *                 force_format=0 preserves whatever format v4l2-ctl set.
 **************************************************/
static void init_device(char *dev_name)
{
    struct v4l2_capability cap;
    struct v4l2_cropcap cropcap;
    struct v4l2_crop crop;
    unsigned int min;

    if (-1 == xioctl(camera_device_fd, VIDIOC_QUERYCAP, &cap))
    {
        if (EINVAL == errno)
        {
            fprintf(stderr, "%s is no V4L2 device\n", dev_name);
            exit(EXIT_FAILURE);
        }
        else
        {
            errno_exit("VIDIOC_QUERYCAP");
        }
    }

    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE))
    {
        fprintf(stderr, "%s is no video capture device\n", dev_name);
        exit(EXIT_FAILURE);
    }

    if (!(cap.capabilities & V4L2_CAP_STREAMING))
    {
        fprintf(stderr, "%s does not support streaming i/o\n", dev_name);
        exit(EXIT_FAILURE);
    }

    /* Attempt to reset crop to the driver default; ignore unsupported errors. */
    CLEAR(cropcap);
    cropcap.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (0 == xioctl(camera_device_fd, VIDIOC_CROPCAP, &cropcap))
    {
        crop.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        crop.c = cropcap.defrect;

        if (-1 == xioctl(camera_device_fd, VIDIOC_S_CROP, &crop))
        {
            switch (errno)
            {
            case EINVAL:
                /* Cropping not supported by this device; non-fatal. */
                break;
            default:
                /* Other crop errors also ignored per V4L2 recommendation. */
                break;
            }
        }
    }

    CLEAR(fmt);
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (force_format)
    {
        printf("FORCING FORMAT\n");
        fmt.fmt.pix.width = HRES;
        fmt.fmt.pix.height = VRES;

        /* YUYV is the packed 4:2:2 format supported by the Logitech C200
         * and many other UVC cameras. Change this line to select a different
         * pixel format (e.g. V4L2_PIX_FMT_GREY, V4L2_PIX_FMT_RGB24). */
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;

        /* V4L2_FIELD_NONE requests progressive (non-interlaced) frames. */
        fmt.fmt.pix.field = V4L2_FIELD_NONE;

        if (-1 == xioctl(camera_device_fd, VIDIOC_S_FMT, &fmt))
            errno_exit("VIDIOC_S_FMT");

        /* Note: VIDIOC_S_FMT may adjust width and height to the nearest
         * supported value; log or assert here if exact resolution matters. */
    }
    else
    {
        printf("ASSUMING FORMAT\n");
        /* Preserve the format already configured by an external tool such as v4l2-ctl. */
        if (-1 == xioctl(camera_device_fd, VIDIOC_G_FMT, &fmt))
            errno_exit("VIDIOC_G_FMT");
    }

    /* Paranoia corrections for buggy drivers that under-report stride/size. */
    min = fmt.fmt.pix.width * 2;
    if (fmt.fmt.pix.bytesperline < min)
        fmt.fmt.pix.bytesperline = min;

    min = fmt.fmt.pix.bytesperline * fmt.fmt.pix.height;
    if (fmt.fmt.pix.sizeimage < min)
        fmt.fmt.pix.sizeimage = min;

    init_mmap(dev_name);
}

/**************************************************
 * Function name : static void close_device(void)
 *    returns    : void
 * Created by    : course 4 team
 * Date created  : 2024
 * Description   : Closes the V4L2 device file descriptor and resets it
 *                 to -1 so callers can detect the closed state.
 *                 Exits on failure via errno_exit().
 **************************************************/
static void close_device(void)
{
    if (-1 == close(camera_device_fd))
        errno_exit("close");

    camera_device_fd = -1;
}

/**************************************************
 * Function name : static void open_device(char *dev_name)
 *    returns    : void
 *    dev_name   : path to the V4L2 character device node
 * Created by    : course 4 team
 * Date created  : 2024
 * Description   : Validates that dev_name exists and is a character
 *                 device, then opens it in read-write non-blocking mode.
 *                 Stores the resulting file descriptor in camera_device_fd.
 *                 Exits on any failure.
 * Notes         : O_NONBLOCK allows read_frame() to use select(2) for
 *                 timeout-guarded dequeue rather than blocking indefinitely.
 **************************************************/
static void open_device(char *dev_name)
{
    struct stat st;

    if (-1 == stat(dev_name, &st))
    {
        fprintf(stderr, "Cannot identify '%s': %d, %s\n",
                dev_name, errno, strerror(errno));
        exit(EXIT_FAILURE);
    }

    if (!S_ISCHR(st.st_mode))
    {
        fprintf(stderr, "%s is no device\n", dev_name);
        exit(EXIT_FAILURE);
    }

    /* O_RDWR required by V4L2; O_NONBLOCK enables select()-based waiting. */
    camera_device_fd = open(dev_name, O_RDWR | O_NONBLOCK, 0);

    if (-1 == camera_device_fd)
    {
        fprintf(stderr, "Cannot open '%s': %d, %s\n",
                dev_name, errno, strerror(errno));
        exit(EXIT_FAILURE);
    }
}

/**************************************************
 * Function name : static void stop_capturing(void)
 *    returns    : void
 * Created by    : course 4 team
 * Date created  : 2024
 * Description   : Records the monotonic stop timestamp (fstop), then
 *                 issues VIDIOC_STREAMOFF to halt the DMA transfer engine.
 *                 After this call no further frames will be delivered by
 *                 the driver.
 **************************************************/
static void stop_capturing(void)
{
    enum v4l2_buf_type type;

    /* Record the stop time before issuing STREAMOFF to minimize latency error. */
    clock_gettime(CLOCK_MONOTONIC, &time_stop);
    fstop = (double)time_stop.tv_sec + (double)time_stop.tv_nsec / 1000000000.0;

    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (-1 == xioctl(camera_device_fd, VIDIOC_STREAMOFF, &type))
        errno_exit("VIDIOC_STREAMOFF");

    printf("capture stopped\n");
}

/**************************************************
 * Function name : static void start_capturing(void)
 *    returns    : void
 * Created by    : course 4 team
 * Date created  : 2024
 * Description   : Enqueues all allocated mmap buffers into the driver's
 *                 incoming queue via VIDIOC_QBUF, then issues
 *                 VIDIOC_STREAMON to begin the DMA transfer. After this
 *                 call the driver will fill the queued buffers in order
 *                 as frames arrive from the sensor.
 **************************************************/
static void start_capturing(void)
{
    unsigned int i;
    enum v4l2_buf_type type;

    printf("will capture to %d buffers\n", n_buffers);

    for (i = 0; i < n_buffers; ++i)
    {
        printf("allocated buffer %d\n", i);

        CLEAR(frame_buf);
        frame_buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        frame_buf.memory = V4L2_MEMORY_MMAP;
        frame_buf.index = i;

        if (-1 == xioctl(camera_device_fd, VIDIOC_QBUF, &frame_buf))
            errno_exit("VIDIOC_QBUF");
    }

    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (-1 == xioctl(camera_device_fd, VIDIOC_STREAMON, &type))
        errno_exit("VIDIOC_STREAMON");
}

/**************************************************
 * Function name : int v4l2_frame_acquisition_initialization(char *dev_name)
 *    returns    : 0 on success; exits on any V4L2 initialization error
 *    dev_name   : path to the V4L2 device node (e.g. "/dev/video0")
 * Created by    : course 4 team
 * Date created  : 2024
 * Description   : Full initialization sequence for the V4L2 capture pipeline:
 *                   1. open_device()    - validates and opens the device node
 *                   2. init_device()    - negotiates format and allocates buffers
 *                   3. start_capturing()- enqueues buffers and starts the stream
 *                   4. Records fstart via CLOCK_MONOTONIC_RAW for FPS baseline.
 * Notes         : Call once from main() before starting the sequencer threads.
 *                 CLOCK_MONOTONIC_RAW is used to match the precision clock
 *                 selected via MY_CLOCK_TYPE in sequencer.h.
 **************************************************/
int v4l2_frame_acquisition_initialization(char *dev_name)
{
    open_device(dev_name);
    init_device(dev_name);
    start_capturing();

    /* Capture the stream-start baseline for elapsed-time and FPS calculations. */
    clock_gettime(CLOCK_MONOTONIC_RAW, &time_start);
    fstart = (double)time_start.tv_sec + (double)time_start.tv_nsec / 1000000000.0;

    return 0;
}

/**************************************************
 * Function name : int v4l2_frame_acquisition_shutdown(void)
 *    returns    : 0 on success
 * Created by    : course 4 team
 * Date created  : 2024
 * Description   : Orderly shutdown sequence for the V4L2 capture pipeline:
 *                   1. stop_capturing() - halts DMA and records fstop
 *                   2. Prints total capture time and achieved frame rate
 *                   3. uninit_device()  - unmaps all buffers and frees array
 *                   4. close_device()   - closes the device file descriptor
 * Notes         : Call from main() after all service threads have joined.
 *                 The +1 in the frame count corrects for the -STARTUP_FRAMES
 *                 initialization of read_framecnt.
 **************************************************/
int v4l2_frame_acquisition_shutdown(void)
{
    stop_capturing();

    printf("Total capture time=%lf, for %d frames, %lf FPS\n",
           (fstop - fstart),
           read_framecnt + 1,
           ((double)read_framecnt / (fstop - fstart)));

    uninit_device();
    close_device();
    fprintf(stderr, "\n");

    return 0;
}
