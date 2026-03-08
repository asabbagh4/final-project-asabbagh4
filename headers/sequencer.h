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
 * Function categories declared here:
 *
 *   Sequencer (sequencer.c) - public:
 *       Sequencer(), print_scheduler(), getTimeMsec(), realtime()
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
 *   V4L2 pipeline lifecycle (v4l2_interface.c, re-declared for convenience):
 *       v4l2_frame_acquisition_initialization(),
 *       v4l2_frame_acquisition_shutdown(),
 *       v4l2_frame_acquisition_loop()
 *
 ***************************************************/

#ifndef _SEQUENCER_
#define _SEQUENCER_

/* Required for CPU_SET, CPU_ZERO, and related affinity macros on Linux. */
#define _GNU_SOURCE

/*  Defines section - timing constants
***************************************************/

/* Microseconds per millisecond; used for unit-conversion documentation. */
#define USEC_PER_MSEC (1000)

/* Nanoseconds per millisecond; used when computing itimerspec field values. */
#define NANOSEC_PER_MSEC (1000000)

/* Nanoseconds per second; used in realtime() and manual timestamp arithmetic. */
#define NANOSEC_PER_SEC (1000000000)

/*  Defines section - hardware topology
***************************************************/

/* Total logical CPU cores present on the target platform.
 * Used when building the allcpuset mask in main(). */
#define NUM_CPU_CORES (4)

/* Boolean shorthands used for abort-flag comparisons throughout
 * the sequencer and service thread loops. */
#define TRUE (1)
#define FALSE (0)

/* Index of the CPU core on which all real-time service threads are pinned
 * via pthread_attr_setaffinity_np(). Isolating RT work to one core
 * reduces cache-line contention with OS and non-RT workloads. */
#define RT_CORE (2)

/* Number of POSIX service threads created by main().
 * Changing this value requires matching updates to the thread-creation
 * loop, the join loop, and the semaphore initialization block. */
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
 * Description   : SIGALRM handler that implements the 100 Hz cyclic executive.
 *                 Fired every 10 ms by the POSIX interval timer timer_1.
 *
 *                 On each invocation the handler:
 *                   1. Checks abortTest; if set, disarms the interval timer
 *                      by zeroing its fields, then sets all per-thread abort
 *                      flags and posts semS1/semS2/semS3 so blocked service
 *                      threads can detect the abort and exit cleanly.
 *                   2. Increments the global seqCnt.
 *                   3. Posts semS1 when (seqCnt % acquisition_frequency == 0),
 *                      releasing Service_1 at its configured sub-rate.
 *                   4. Posts semS2 and semS3 when
 *                      (seqCnt % frame_cap_frequency == 0), releasing
 *                      Service_2 and Service_3 at their configured sub-rate.
 *
 * Notes         : This executes in ISR context. Keep it short and free of
 *                 blocking calls, heap allocation, and non-reentrant library
 *                 functions. The semaphore posts are the only significant
 *                 work performed here.
 **************************************************/
void Sequencer(int id);

/**************************************************
 * Function name : void *Service_1_frame_acquisition(void *threadp)
 *    returns    : NULL via pthread_exit(0)
 *    threadp    : pointer to a threadParams_t struct containing this
 *                 thread's zero-based index
 * Created by    : course 4 team
 * Date created  : 2024
 * Description   : Real-time POSIX thread that acquires one V4L2 frame on
 *                 each release. Blocks on semS1, which is posted by the
 *                 Sequencer at acquisition_frequency sub-rate. On each
 *                 release calls seq_frame_read() to dequeue one driver
 *                 buffer, copy it into the acquisition ring buffer, and
 *                 re-queue the driver buffer. Logs a syslog entry with
 *                 core number, release count, and elapsed time.
 * Notes         : Scheduled at SCHED_FIFO priority RT_MAX-1.
 *                 Pinned to RT_CORE via pthread_attr_setaffinity_np()
 *                 in main(). Must not block, allocate memory, or call
 *                 non-reentrant functions during the work phase.
 **************************************************/
void *Service_1_frame_acquisition(void *threadp);

/**************************************************
 * Function name : void *Service_2_frame_process(void *threadp)
 *    returns    : NULL via pthread_exit(0)
 *    threadp    : pointer to a threadParams_t struct containing this
 *                 thread's zero-based index
 * Created by    : course 4 team
 * Date created  : 2024
 * Description   : Real-time POSIX thread that color-converts and runs
 *                 motion detection on one frame per release. Blocks on
 *                 semS2, posted by the Sequencer at frame_cap_frequency
 *                 sub-rate. On each release calls seq_frame_process().
 *                 Logs a syslog entry with core number, release count,
 *                 and elapsed time.
 * Notes         : Scheduled at SCHED_FIFO priority RT_MAX-2.
 *                 Pinned to RT_CORE. Lower priority than Service_1 so
 *                 frame acquisition can always preempt frame processing.
 **************************************************/
void *Service_2_frame_process(void *threadp);

/**************************************************
 * Function name : void *Service_3_frame_storage(void *threadp)
 *    returns    : NULL via pthread_exit(0)
 *    threadp    : pointer to a threadParams_t struct containing this
 *                 thread's zero-based index
 * Created by    : course 4 team
 * Date created  : 2024
 * Description   : Real-time POSIX thread that writes processed frames to
 *                 the ramdisk. Blocks on semS3, posted by the Sequencer
 *                 at frame_cap_frequency sub-rate. On each release calls
 *                 seq_frame_store(). When seq_frame_store() returns
 *                 num_frames_to_capture, sets abortTest to trigger a
 *                 clean pipeline shutdown. Logs a syslog entry with core
 *                 number, release count, and elapsed time.
 * Notes         : Scheduled at SCHED_FIFO priority RT_MAX-3.
 *                 Pinned to RT_CORE. Lowest priority so disk I/O never
 *                 preempts acquisition or processing.
 *                 O_NONBLOCK in dump_pgm/dump_ppm limits time spent in
 *                 the storage work phase.
 **************************************************/
void *Service_3_frame_storage(void *threadp);

/**************************************************
 * Function name : double getTimeMsec(void)
 *    returns    : current MY_CLOCK_TYPE time in milliseconds as a double
 * Created by    : course 4 team
 * Date created  : 2024
 * Description   : Reads the MY_CLOCK_TYPE clock and converts seconds and
 *                 nanoseconds to a single millisecond double. Useful for
 *                 coarse latency measurements without manual timespec
 *                 arithmetic.
 **************************************************/
double getTimeMsec(void);

/**************************************************
 * Function name : double realtime(struct timespec *tsptr)
 *    returns    : the time represented by *tsptr as a double in seconds
 *    tsptr      : pointer to the timespec value to convert
 * Created by    : course 4 team
 * Date created  : 2024
 * Description   : Converts a struct timespec to a single double-precision
 *                 floating-point value in seconds by combining tv_sec and
 *                 the fractional nanosecond portion. Used throughout the
 *                 pipeline to compute elapsed times for FPS and latency logs.
 **************************************************/
double realtime(struct timespec *tsptr);

/**************************************************
 * Function name : void print_scheduler(void)
 *    returns    : void; exits with -1 if the policy is not SCHED_FIFO
 * Created by    : course 4 team
 * Date created  : 2024
 * Description   : Queries the scheduling policy of the calling process
 *                 with sched_getscheduler() and prints the result. Exits
 *                 with -1 for any policy other than SCHED_FIFO, because
 *                 the pipeline's deadline constraints require real-time
 *                 scheduling. Called from main() after elevating the main
 *                 process to SCHED_FIFO to confirm the change succeeded.
 * Notes         : SCHED_DEADLINE support is commented out; re-enable and
 *                 test if the design migrates to EDF scheduling.
 **************************************************/
void print_scheduler(void);

/*  Public function prototypes - read_frames.c
***************************************************/

/**************************************************
 * Function name : int seq_frame_read(void)
 *    returns    : (int) - reserved for future status codes; no explicit
 *                 return value is provided in the current implementation
 * Created by    : course 4 team
 * Date created  : 2024
 * Description   : Entry point called by Service_1_frame_acquisition().
 *                 Uses select(2) with a 2-second timeout to wait for a
 *                 readable frame on camera_device_fd, then calls the
 *                 internal read_frame() to dequeue it via VIDIOC_DQBUF.
 *                 On success, copies the raw pixel data from the mmap
 *                 region into the tail slot of the acquisition ring buffer,
 *                 advances the tail index modulo ring_size, increments
 *                 ring_buffer.count, samples fnow, and re-queues the
 *                 driver buffer via VIDIOC_QBUF so the hardware can reuse it.
 * Notes         : The select() return value is currently unchecked; a
 *                 timeout silently falls through to read_frame(). A future
 *                 improvement should log or handle the timeout explicitly
 *                 to avoid a silent missed-frame condition.
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
 *                 to write the frame to the ramdisk. Logs each stored
 *                 frame to syslog with the frame count and elapsed
 *                 acquisition time.
 * Notes         : CLOCK_MONOTONIC_RAW is used here to match the baseline
 *                 clock used in v4l2_frame_acquisition_initialization(),
 *                 giving consistent elapsed-time values in the syslog output.
 **************************************************/
int seq_frame_store(void);

/*  V4L2 pipeline lifecycle prototypes (implemented in v4l2_interface.c)
***************************************************/

/**************************************************
 * Function name : int v4l2_frame_acquisition_initialization(char *dev_name)
 *    returns    : 0 on success; exits on V4L2 error
 *    dev_name   : path to the V4L2 device node (e.g. "/dev/video0")
 * Created by    : course 4 team
 * Date created  : 2024
 * Description   : Opens the device, negotiates YUYV format at HRES x VRES,
 *                 allocates and maps DRIVER_MMAP_BUFFERS kernel buffers,
 *                 and starts the DMA stream. Records fstart as the
 *                 acquisition baseline. Call once from main() before
 *                 creating service threads.
 * Notes         : Full documentation in v4l2_interface.h.
 **************************************************/
int v4l2_frame_acquisition_initialization(char *dev_name);

/**************************************************
 * Function name : int v4l2_frame_acquisition_shutdown(void)
 *    returns    : 0 on success
 * Created by    : course 4 team
 * Date created  : 2024
 * Description   : Stops the DMA stream, prints capture statistics,
 *                 unmaps all kernel buffers, and closes the device.
 *                 Call from main() after all service threads have joined.
 * Notes         : Full documentation in v4l2_interface.h.
 **************************************************/
int v4l2_frame_acquisition_shutdown(void);

/**************************************************
 * Function name : int v4l2_frame_acquisition_loop(char *dev_name)
 *    returns    : (int) - reserved for future status codes
 *    dev_name   : path to the V4L2 device node
 * Created by    : course 4 team
 * Date created  : 2024
 * Description   : Legacy combined init/capture/shutdown loop retained
 *                 for reference. Not used in the current sequencer-based
 *                 design; replaced by the three-function init/service/shutdown
 *                 pattern.
 * Notes         : Do not call in production; kept only for historical reference.
 **************************************************/
int v4l2_frame_acquisition_loop(char *dev_name);

/*  Private (static) function prototypes - sequencer.c
 *
 * These functions are file-scoped in sequencer.c. Declared here so the
 * complete inventory is visible and test harnesses can reference them
 * without modifying source. Do not call from outside sequencer.c.
 ***************************************************/

/**************************************************
 * Function name : static void log_sysinfo(void)
 *    returns    : void; calls exit(EXIT_FAILURE) if popen() fails
 * Created by    : course 4 team
 * Date created  : 2024
 * Description   : Executes "uname -a" via popen() and writes the output
 *                 to syslog at LOG_INFO level. Provides a permanent record
 *                 of the kernel version and machine architecture in the
 *                 system log for post-hoc debugging and regression analysis.
 * Notes         : Uses popen(), which is not real-time safe. Call only
 *                 during initialization in main(), before the sequencer
 *                 timer is armed and before RT threads are created.
 **************************************************/
static void log_sysinfo(void);

/**************************************************
 * Function name : static void program_usage(const char *prog)
 *    returns    : void
 *    prog       : argv[0], the name of the executable as invoked
 * Created by    : course 4 team
 * Date created  : 2024
 * Description   : Prints a usage summary and a concrete invocation
 *                 example to stdout. Called from main() when command-line
 *                 argument parsing detects an unrecognized value.
 **************************************************/
static void program_usage(const char *prog);

/*  Private (static) function prototypes - read_frames.c
 *
 * read_frame() is file-scoped in read_frames.c. Declared here for inventory
 * completeness. Do not call from outside read_frames.c.
 ***************************************************/

/**************************************************
 * Function name : static int read_frame(void)
 *    returns    : 1 on a successful buffer dequeue, 0 if the driver
 *                 returned EAGAIN (no buffer available) or EIO (I/O error)
 * Created by    : course 4 team
 * Date created  : 2024
 * Description   : Clears frame_buf, sets its type and memory fields, and
 *                 issues VIDIOC_DQBUF to dequeue one completed buffer from
 *                 the V4L2 driver. On the first successful dequeue after
 *                 the warmup period (read_framecnt == 0), records the
 *                 CLOCK_MONOTONIC start timestamp into time_start and fstart.
 *                 Increments read_framecnt and validates frame_buf.index
 *                 against n_buffers with assert().
 * Notes         : EAGAIN is non-fatal in O_NONBLOCK mode and indicates
 *                 no frame is ready yet; the caller should retry on the
 *                 next sequencer release. EIO may indicate a transient
 *                 driver issue and is also treated as non-fatal here, though
 *                 repeated EIO returns may warrant a reinitialization path.
 *                 Calls errno_exit() on all other ioctl errors.
 **************************************************/
static int read_frame(void);

/*  Private (static) function prototypes - store_frame.c
 *
 * dump_ppm(), dump_pgm(), and save_image() are file-scoped in store_frame.c.
 * Declared here for inventory completeness. Do not call from outside store_frame.c.
 ***************************************************/

/**************************************************
 * Function name : static void dump_ppm(const void *p, int size,
 *                                      unsigned int tag,
 *                                      struct timespec *time)
 *    returns    : void
 *    p          : pointer to packed RGB24 pixel data to write
 *    size       : byte length of the pixel data
 *    tag        : monotonically increasing frame index; embedded in filename
 *    time       : monotonic timestamp to embed in the PPM comment field
 * Created by    : course 4 team
 * Date created  : 2024
 * Description   : Creates FRAME_PATH/test<tag>.ppm, writes a P6 Netpbm
 *                 header with the timestamp patched into the comment field,
 *                 then writes all pixel data in a retry loop until every
 *                 byte has been transferred. Opens the file with O_NONBLOCK
 *                 to avoid stalling the RT storage thread. Updates fnow
 *                 after the write completes.
 * Notes         : sizeof(ppm_header)-1 excludes the null terminator.
 *                 The in-place snprintf calls assume the header template
 *                 reserves exactly 10 characters for each numeric timestamp
 *                 field; do not change the template string without updating
 *                 the offset constants accordingly.
 **************************************************/
static void dump_ppm(const void *p, int size,
                     unsigned int tag, struct timespec *time);

/**************************************************
 * Function name : static void dump_pgm(const void *p, int size,
 *                                      unsigned int tag,
 *                                      struct timespec *time)
 *    returns    : void
 *    p          : pointer to packed grayscale (Y-plane) pixel data to write
 *    size       : byte length of the pixel data (HRES*VRES for gray frames)
 *    tag        : monotonically increasing frame index; embedded in filename
 *    time       : monotonic timestamp to embed in the PGM comment field
 * Created by    : course 4 team
 * Date created  : 2024
 * Description   : Creates FRAME_PATH/test<tag>.pgm and writes a P5 Netpbm
 *                 header followed by the grayscale pixel data. Behavior
 *                 and implementation are identical to dump_ppm() except
 *                 that it uses the P5 magic number and single-channel data.
 * Notes         : See dump_ppm() notes; the same in-place header patching
 *                 approach and offset assumptions apply here.
 **************************************************/
static void dump_pgm(const void *p, int size,
                     unsigned int tag, struct timespec *time);

/**************************************************
 * Function name : static int save_image(const void *p, int size,
 *                                       struct timespec *frame_time)
 *    returns    : running save_framecnt after this call
 *    p          : pointer to the frame data; interpretation is format-dependent
 *    size       : byte length of the frame data
 *    frame_time : monotonic timestamp to embed in the output image header
 * Created by    : course 4 team
 * Date created  : 2024
 * Description   : Increments save_framecnt and, when DUMP_FRAMES is defined,
 *                 selects the correct Netpbm writer based on the V4L2 pixel
 *                 format in fmt and the active compile-time color-conversion
 *                 flag:
 *
 *                   GREY format        -> dump_pgm (raw sensor grayscale)
 *                   YUYV + RGB convert -> dump_ppm (converted RGB24 data)
 *                   YUYV + gray convert-> dump_pgm (Y-plane from motion_buffer)
 *                   RGB24 format       -> dump_ppm (native RGB24 data)
 *
 *                 Always reads the output pixels from the most recently
 *                 completed slot in motion_buffer so that only post-conversion
 *                 grayscale data is persisted.
 * Notes         : If DUMP_FRAMES is not defined, save_framecnt is still
 *                 incremented but no file I/O occurs. This allows the
 *                 pipeline to run without disk writes for profiling purposes.
 *                 The most_recent_idx calculation must stay in sync with the
 *                 identical calculation in seq_frame_store().
 **************************************************/
static int save_image(const void *p, int size, struct timespec *frame_time);

#endif /* _SEQUENCER_ */
