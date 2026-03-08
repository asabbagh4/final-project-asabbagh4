/***************************************************
 * Module name: sequencer.h
 *
 * First written in 2026 by Laye Tenumah, adapted from Sam Siewert template.
 *
 * Module Description:
 * Public interface for the real-time cyclic executive and its
 * supporting pipeline modules. Declares all timing constants,
 * thread and CPU core configuration, and every function
 * implemented across sequencer.c, read_frames.c, and store_frame.c.
 *
 * V4L2 pipeline lifecycle functions (v4l2_frame_acquisition_
 * initialization, _shutdown, _loop) are prototyped exclusively
 * in v4l2_interface.h. They are not repeated here; sequencer.c
 * obtains them by including v4l2_interface.h directly.
 *
 * Function categories declared here:
 *
 *   Sequencer (sequencer.c) - public:
 *       Sequencer(), print_scheduler(), getTimeMsec(), realtime(),
 *       Service_1_frame_acquisition(), Service_2_frame_process(),
 *       Service_3_frame_storage()
 *
 *   Sequencer (sequencer.c) - private static:
 *       log_sysinfo(), program_usage()
 *
 *   Frame acquisition (read_frames.c) - public:
 *       seq_frame_read()
 *
 *   Frame acquisition (read_frames.c) - private static:
 *       read_frame()
 *
 *   Frame storage (store_frame.c) - public:
 *       seq_frame_store()
 *
 *   Frame storage (store_frame.c) - private static:
 *       dump_ppm(), dump_pgm(), save_image()
 *
 ***************************************************/

#ifndef _SEQUENCER_
#define _SEQUENCER_

/* Required for CPU_SET, CPU_ZERO, and related affinity macros on Linux.
 * Must be defined before any system header is pulled in. */
#define _GNU_SOURCE

/* capturelib.h is the single source of truth for all shared constants.
 * Including it here means sequencer.c, read_frames.c, and store_frame.c
 * all obtain COURSE, FRAME_PATH, HRES, VRES, etc. from one location
 * without additional includes. */
#include "capturelib.h"

/*  Defines section - timing constants
***************************************************/

/* Microseconds per millisecond; for unit-conversion documentation. */
#define USEC_PER_MSEC (1000)

/* Nanoseconds per millisecond; used when computing itimerspec fields. */
#define NANOSEC_PER_MSEC (1000000)

/* Nanoseconds per second; used in realtime() and timestamp arithmetic. */
#define NANOSEC_PER_SEC (1000000000)

/*  Defines section - hardware topology
***************************************************/

/* Total logical CPU cores on the target platform.
 * Used to build the allcpuset mask in main(). */
#define NUM_CPU_CORES (4)

/* Boolean shorthands for abort-flag comparisons in service thread loops. */
#define TRUE (1)
#define FALSE (0)

/* Index of the CPU core on which all RT service threads are pinned via
 * pthread_attr_setaffinity_np(). Isolating RT work to one core reduces
 * cache-line contention with OS and non-RT workloads. */
#define RT_CORE (2)

/* Number of POSIX service threads created by main(). Changing this value
 * requires matching updates to the thread-creation loop, the join loop,
 * and the semaphore initialization block in main(). */
#define NUM_THREADS (3)

/*  Defines section - clock selection
 *
 * MY_CLOCK_TYPE selects the POSIX clock used for all latency measurements
 * and syslog timestamps throughout the pipeline.
 *
 * CLOCK_MONOTONIC_RAW is preferred because it is not subject to NTP slew
 * adjustments, giving the most stable inter-sample intervals. Note that
 * CLOCK_MONOTONIC_RAW cannot be used with clock_nanosleep(); use
 * CLOCK_MONOTONIC or CLOCK_REALTIME for any sleep-based timing.
 ***************************************************/
// #define MY_CLOCK_TYPE CLOCK_REALTIME
// #define MY_CLOCK_TYPE CLOCK_MONOTONIC
#define MY_CLOCK_TYPE CLOCK_MONOTONIC_RAW
// #define MY_CLOCK_TYPE CLOCK_REALTIME_COARSE
// #define MY_CLOCK_TYPE CLOCK_MONTONIC_COARSE

/*  Public function prototypes - sequencer.c
***************************************************/

/**************************************************
 * Function name : void Sequencer(int id)
 *    returns    : void
 *    id         : signal number delivered (SIGALRM); received but not used
 * Created by    : course 4 team
 * Date created  : 2024
 * Description   : SIGALRM handler implementing the 100 Hz cyclic executive.
 *                 Fired every 10 ms by the POSIX interval timer timer_1.
 *
 *                 On each invocation:
 *                   1. If abortTest is set, zeroes the timer fields and calls
 *                      timer_settime() to disarm it, then sets all per-thread
 *                      abort flags and posts semS1/semS2/semS3 so blocked
 *                      threads can detect the abort and exit cleanly.
 *                   2. Increments the global seqCnt.
 *                   3. Posts semS1 when (seqCnt % acquisition_frequency == 0)
 *                      to release Service_1 at its configured sub-rate.
 *                   4. Posts semS2 and semS3 when
 *                      (seqCnt % frame_cap_frequency == 0) to release
 *                      Service_2 and Service_3 at their configured sub-rate.
 *
 * Notes         : Executes in ISR context. Must be short and free of blocking
 *                 calls, heap allocation, and non-reentrant library functions.
 *                 The semaphore posts are the only significant work done here.
 **************************************************/
void Sequencer(int id);

/**************************************************
 * Function name : void *Service_1_frame_acquisition(void *threadp)
 *    returns    : NULL via pthread_exit(0)
 *    threadp    : pointer to a threadParams_t struct with this thread's index
 * Created by    : course 4 team
 * Date created  : 2024
 * Description   : RT POSIX thread that acquires one V4L2 frame per release.
 *                 Blocks on semS1, posted by the Sequencer at
 *                 acquisition_frequency sub-rate. On each release calls
 *                 seq_frame_read() to dequeue one driver buffer, copy it
 *                 into the acquisition ring buffer, and re-queue the driver
 *                 buffer. Logs a syslog entry with core, release count, and
 *                 elapsed time.
 * Notes         : Scheduled SCHED_FIFO at RT_MAX-1. Pinned to RT_CORE.
 *                 Must not block, allocate memory, or call non-reentrant
 *                 functions during the work phase.
 **************************************************/
void *Service_1_frame_acquisition(void *threadp);

/**************************************************
 * Function name : void *Service_2_frame_process(void *threadp)
 *    returns    : NULL via pthread_exit(0)
 *    threadp    : pointer to a threadParams_t struct with this thread's index
 * Created by    : course 4 team
 * Date created  : 2024
 * Description   : RT POSIX thread that color-converts and runs motion
 *                 detection on one frame per release. Blocks on semS2,
 *                 posted by the Sequencer at frame_cap_frequency sub-rate.
 *                 On each release calls seq_frame_process(). Logs a syslog
 *                 entry with core, release count, and elapsed time.
 * Notes         : Scheduled SCHED_FIFO at RT_MAX-2. Pinned to RT_CORE.
 *                 Lower priority than Service_1 so acquisition can always
 *                 preempt processing.
 **************************************************/
void *Service_2_frame_process(void *threadp);

/**************************************************
 * Function name : void *Service_3_frame_storage(void *threadp)
 *    returns    : NULL via pthread_exit(0)
 *    threadp    : pointer to a threadParams_t struct with this thread's index
 * Created by    : course 4 team
 * Date created  : 2024
 * Description   : RT POSIX thread that writes processed frames to the
 *                 ramdisk. Blocks on semS3, posted by the Sequencer at
 *                 frame_cap_frequency sub-rate. On each release calls
 *                 seq_frame_store(). When seq_frame_store() returns
 *                 num_frames_to_capture, sets abortTest to trigger a clean
 *                 pipeline shutdown. Logs a syslog entry with core, release
 *                 count, and elapsed time.
 * Notes         : Scheduled SCHED_FIFO at RT_MAX-3. Pinned to RT_CORE.
 *                 Lowest priority so disk I/O never preempts acquisition
 *                 or processing. O_NONBLOCK in dump_pgm/dump_ppm bounds
 *                 the time spent in the storage work phase.
 **************************************************/
void *Service_3_frame_storage(void *threadp);

/**************************************************
 * Function name : double getTimeMsec(void)
 *    returns    : current MY_CLOCK_TYPE time in milliseconds as a double
 * Created by    : course 4 team
 * Date created  : 2024
 * Description   : Reads MY_CLOCK_TYPE and converts seconds and nanoseconds
 *                 to a single millisecond double. Useful for coarse latency
 *                 measurements without manual timespec arithmetic.
 **************************************************/
double getTimeMsec(void);

/**************************************************
 * Function name : double realtime(struct timespec *tsptr)
 *    returns    : time represented by *tsptr as a double-precision second value
 *    tsptr      : pointer to the timespec value to convert
 * Created by    : course 4 team
 * Date created  : 2024
 * Description   : Converts a struct timespec to a single double-precision
 *                 value in seconds by combining tv_sec with the fractional
 *                 nanosecond portion. Used throughout the pipeline for FPS
 *                 and latency log calculations.
 **************************************************/
double realtime(struct timespec *tsptr);

/**************************************************
 * Function name : void print_scheduler(void)
 *    returns    : void; exits with -1 if policy is not SCHED_FIFO
 * Created by    : course 4 team
 * Date created  : 2024
 * Description   : Queries the scheduling policy of the calling process with
 *                 sched_getscheduler() and prints the result to stdout.
 *                 Exits with -1 for any policy other than SCHED_FIFO because
 *                 the pipeline's deadline constraints require real-time
 *                 scheduling. Called from main() after elevating the process
 *                 to SCHED_FIFO to confirm the change succeeded.
 * Notes         : SCHED_DEADLINE support is commented out; re-enable if the
 *                 design migrates to EDF scheduling.
 **************************************************/
void print_scheduler(void);

/*  Public function prototypes - read_frames.c
***************************************************/

/**************************************************
 * Function name : int seq_frame_read(void)
 *    returns    : (int) reserved for future status codes; no explicit return
 *                 value is provided in the current implementation
 * Created by    : course 4 team
 * Date created  : 2024
 * Description   : Entry point called by Service_1_frame_acquisition().
 *                 Uses select(2) with a 2-second timeout to wait for a
 *                 readable frame on camera_device_fd, then calls the
 *                 internal read_frame() to dequeue it via VIDIOC_DQBUF.
 *                 On success, copies the raw pixel data from the mmap region
 *                 into the tail slot of the acquisition ring buffer, advances
 *                 the tail index modulo ring_size, increments ring_buffer.count,
 *                 samples fnow, and re-queues the driver buffer via VIDIOC_QBUF
 *                 so the hardware can reuse it.
 * Notes         : The select() return value is currently unchecked; a timeout
 *                 silently falls through to read_frame(). A future improvement
 *                 should log or handle the timeout to avoid silent frame loss.
 **************************************************/
int seq_frame_read(void);

/*  Public function prototypes - store_frame.c
***************************************************/

/**************************************************
 * Function name : int seq_frame_store(void)
 *    returns    : running save_framecnt after this invocation, or 0 if
 *                 motion_buffer.count < 2 (insufficient baseline data)
 * Created by    : course 4 team
 * Date created  : 2024
 * Description   : Entry point called by Service_3_frame_storage().
 *                 Guards against writing before the motion detector has
 *                 accumulated at least two frames (one baseline and one
 *                 comparison). Samples CLOCK_MONOTONIC_RAW into time_now
 *                 and fnow, resolves the most recently processed frame
 *                 index in motion_buffer, and dispatches to save_image()
 *                 to write the frame to the ramdisk. Logs each stored frame
 *                 to syslog with the frame count and elapsed acquisition time.
 * Notes         : Uses CLOCK_MONOTONIC_RAW to match the baseline clock from
 *                 v4l2_frame_acquisition_initialization(), giving consistent
 *                 elapsed-time values in the syslog output.
 **************************************************/
int seq_frame_store(void);

/*  Private (static) function prototypes - sequencer.c
 *
 * Declared here so the complete function inventory is visible in one place
 * and test harnesses can reference them without modifying the source.
 * Do not call from outside sequencer.c.
 ***************************************************/

/**************************************************
 * Function name : static void log_sysinfo(void)
 *    returns    : void; calls exit(EXIT_FAILURE) if popen() fails
 * Created by    : course 4 team
 * Date created  : 2024
 * Description   : Executes "uname -a" via popen() and writes the output
 *                 to syslog at LOG_INFO. Records the kernel version and
 *                 machine architecture for post-hoc debugging.
 * Notes         : Uses popen(), which is not real-time safe. Call only
 *                 during initialization in main(), before the sequencer
 *                 timer is armed and before RT threads are created.
 **************************************************/
static void log_sysinfo(void);

/**************************************************
 * Function name : static void program_usage(const char *prog)
 *    returns    : void
 *    prog       : argv[0], the executable name as invoked
 * Created by    : course 4 team
 * Date created  : 2024
 * Description   : Prints a usage summary and a concrete invocation example
 *                 to stdout. Called from main() when command-line argument
 *                 parsing encounters an unrecognized value.
 **************************************************/
static void program_usage(const char *prog);

/*  Private (static) function prototypes - read_frames.c
 *
 * Declared here for inventory completeness.
 * Do not call from outside read_frames.c.
 ***************************************************/

/**************************************************
 * Function name : static int read_frame(void)
 *    returns    : 1 on a successful buffer dequeue, 0 if the driver
 *                 returned EAGAIN (no buffer ready) or EIO (I/O error)
 * Created by    : course 4 team
 * Date created  : 2024
 * Description   : Clears frame_buf, sets its type and memory fields, and
 *                 issues VIDIOC_DQBUF to dequeue one completed buffer from
 *                 the V4L2 driver. On the first successful dequeue after
 *                 the warmup period (read_framecnt == 0), records the
 *                 CLOCK_MONOTONIC start timestamp into time_start and fstart.
 *                 Increments read_framecnt and validates frame_buf.index
 *                 against n_buffers with assert().
 * Notes         : EAGAIN is non-fatal in O_NONBLOCK mode; the caller retries
 *                 on the next sequencer release. EIO is also treated as
 *                 non-fatal here, though repeated EIO may warrant reinitialization.
 *                 Calls errno_exit() on all other ioctl errors.
 **************************************************/
static int read_frame(void);

/*  Private (static) function prototypes - store_frame.c
 *
 * Declared here for inventory completeness.
 * Do not call from outside store_frame.c.
 ***************************************************/

/**************************************************
 * Function name : static void dump_ppm(const void *p, int size,
 *                                      unsigned int tag,
 *                                      struct timespec *time)
 *    returns    : void
 *    p          : pointer to packed RGB24 pixel data to write
 *    size       : byte length of the pixel data
 *    tag        : monotonically increasing frame index; used in the filename
 *    time       : monotonic timestamp embedded in the PPM comment field
 * Created by    : course 4 team
 * Date created  : 2024
 * Description   : Creates FRAME_PATH/test<tag>.ppm, writes a P6 Netpbm header
 *                 with the timestamp patched into the comment field, then
 *                 writes all pixel data in a retry loop until every byte has
 *                 been transferred. Opens the file with O_NONBLOCK to avoid
 *                 stalling the RT storage thread. Updates fnow after writing.
 * Notes         : sizeof(ppm_header)-1 excludes the null terminator.
 *                 The in-place snprintf calls assume the header template
 *                 reserves exactly 10 characters per numeric timestamp field.
 *                 Do not change the template string without updating offsets.
 **************************************************/
static void dump_ppm(const void *p, int size,
                     unsigned int tag, struct timespec *time);

/**************************************************
 * Function name : static void dump_pgm(const void *p, int size,
 *                                      unsigned int tag,
 *                                      struct timespec *time)
 *    returns    : void
 *    p          : pointer to packed grayscale (Y-plane) pixel data to write
 *    size       : byte length of the pixel data (HRES * VRES for gray frames)
 *    tag        : monotonically increasing frame index; used in the filename
 *    time       : monotonic timestamp embedded in the PGM comment field
 * Created by    : course 4 team
 * Date created  : 2024
 * Description   : Creates FRAME_PATH/test<tag>.pgm and writes a P5 Netpbm
 *                 header followed by the grayscale pixel data. Behavior and
 *                 implementation are identical to dump_ppm() except for the
 *                 P5 magic number and single-channel pixel data.
 * Notes         : See dump_ppm() notes; the same in-place header patching
 *                 approach and offset assumptions apply here.
 **************************************************/
static void dump_pgm(const void *p, int size,
                     unsigned int tag, struct timespec *time);

/**************************************************
 * Function name : static int save_image(const void *p, int size,
 *                                       struct timespec *frame_time)
 *    returns    : running save_framecnt after this call
 *    p          : pointer to frame data; interpretation is format-dependent
 *    size       : byte length of the frame data
 *    frame_time : monotonic timestamp to embed in the output image header
 * Created by    : course 4 team
 * Date created  : 2024
 * Description   : Increments save_framecnt and, when DUMP_FRAMES is defined,
 *                 selects the correct Netpbm writer based on the V4L2 pixel
 *                 format in fmt and the active color-conversion compile flag:
 *
 *                   GREY format         -> dump_pgm (raw sensor grayscale)
 *                   YUYV + RGB convert  -> dump_ppm (converted RGB24 data)
 *                   YUYV + gray convert -> dump_pgm (Y-plane from motion_buffer)
 *                   RGB24 format        -> dump_ppm (native RGB24 data)
 *
 *                 Always reads pixels from the most recently completed slot
 *                 in motion_buffer so only post-conversion data is persisted.
 * Notes         : If DUMP_FRAMES is not defined, save_framecnt is still
 *                 incremented but no file I/O occurs, allowing the pipeline
 *                 to run without disk writes for profiling purposes.
 *                 The most_recent_idx calculation must stay in sync with the
 *                 identical calculation in seq_frame_store().
 **************************************************/
static int save_image(const void *p, int size, struct timespec *frame_time);

#endif /* _SEQUENCER_ */
