/***************************************************
 * Module name: sequencer.c
 *
 * First written in 2026 by Laye Tenumah, adapted from Sam Siewert template.
 *
 * Module Description:
 * Implements the real-time cyclic executive and its three
 * POSIX service threads for the V4L2 camera capture pipeline.
 *
 * Architecture overview:
 *   main() initializes V4L2, sets SCHED_FIFO priorities, pins all
 *   threads to RT_CORE, arms a POSIX interval timer at 100 Hz, and
 *   joins the three service threads.
 *
 *   Sequencer() fires on each SIGALRM (every 10 ms) and releases
 *   service threads at their configured sub-rates via semaphores:
 *     Service_1 (frame acquisition) @ acquisition_frequency sub-rate
 *     Service_2 (frame processing)  @ frame_cap_frequency sub-rate
 *     Service_3 (frame storage)     @ frame_cap_frequency sub-rate
 *
 *   Each service thread blocks on its semaphore, performs its work,
 *   logs its completion time, and loops. Service_3 sets abortTest
 *   after num_frames_to_capture frames have been saved.
 *
 ***************************************************/

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <semaphore.h>

#include <syslog.h>
#include <sys/time.h>
#include <sys/sysinfo.h>
#include <errno.h>

#include <signal.h>

#include "../headers/sequencer.h"
#include "../headers/process_frame.h"
#include "../headers/capturelib.h"

/* COURSE is defined in capturelib.h, which is included via sequencer.h. */

/*  Module-wide variable definitions
***************************************************/

/* Set to TRUE by Service_3 (or externally) to request a clean shutdown of
 * the sequencer and all service threads. */
int abortTest = FALSE;

/* Per-thread abort flags; set TRUE by the Sequencer before posting the
 * corresponding semaphore during shutdown to unblock waiting threads. */
int abortS1 = FALSE, abortS2 = FALSE, abortS3 = FALSE;

/* Binary semaphores used to release each service thread from the sequencer.
 * Initialized to 0 (blocked) so threads wait immediately after creation. */
sem_t semS1, semS2, semS3;

/* Monotonic start time captured in main() before the sequencer is armed.
 * Used as the reference epoch for all elapsed-time log messages. */
struct timespec start_time_val;
double start_realtime;

/* Sub-rate divisors applied to the 100 Hz sequencer tick:
 *   acquisition_frequency: every N ticks -> release Service_1
 *   frame_cap_frequency:   every N ticks -> release Service_2 and Service_3
 *   num_frames_to_capture: total frames Service_3 writes before aborting */
int acquisition_frequency = 4;
int frame_cap_frequency = 100;
int num_frames_to_capture = 1800;

/* POSIX interval timer handle; created in main() and armed to fire every 10 ms. */
static timer_t timer_1;

/* Timer spec used to arm and disarm timer_1.
 * Initial values (1s interval) are overridden in main() to 10 ms. */
static struct itimerspec itime = {{1, 0}, {1, 0}};
static struct itimerspec last_itime; /* Receives the previous timer setting on arm/disarm. */

/* Monotonically incrementing count of Sequencer invocations since startup. */
static unsigned long long seqCnt = 0;

/*
 * threadParams_t - Per-thread initialization data passed at creation time.
 *
 * Members:
 *   threadIdx - Zero-based index identifying the thread (0=S1, 1=S2, 2=S3).
 */
typedef struct
{
    int threadIdx;
} threadParams_t;

/**************************************************
 * Function name : void Sequencer(int id)
 *    returns    : void
 *    id         : signal number (SIGALRM); unused but required by signal API
 * Created by    : course 4 team
 * Date created  : 2024
 * Description   : SIGALRM handler that implements the cyclic executive.
 *                 Fires at 100 Hz (every 10 ms) driven by timer_1.
 *
 *                 On each invocation:
 *                   1. If abortTest is set, disarms the interval timer,
 *                      sets all per-thread abort flags, and posts all
 *                      semaphores so blocked threads can exit cleanly.
 *                   2. Increments seqCnt.
 *                   3. Releases Service_1 when (seqCnt % acquisition_frequency == 0).
 *                   4. Releases Service_2 and Service_3 when
 *                      (seqCnt % frame_cap_frequency == 0).
 *
 * Notes         : This is an ISR context; keep it short. No heap allocation,
 *                 no blocking calls, no non-reentrant library functions.
 *                 The semaphore posts are the only significant work here.
 **************************************************/
void Sequencer(int id)
{
    struct timespec current_time_val;
    double current_realtime;
    int rc, flags = 0;

    if (abortTest)
    {
        /* Disarm the interval timer to stop future SIGALRM deliveries. */
        itime.it_interval.tv_sec = 0;
        itime.it_interval.tv_nsec = 0;
        itime.it_value.tv_sec = 0;
        itime.it_value.tv_nsec = 0;
        timer_settime(timer_1, flags, &itime, &last_itime);
        printf("Disabling sequencer interval timer with abort=%d and %llu\n",
               abortTest, seqCnt);

        /* Signal all service threads to exit their work loops. */
        abortS1 = TRUE;
        abortS2 = TRUE;
        abortS3 = TRUE;
        sem_post(&semS1);
        sem_post(&semS2);
        sem_post(&semS3);
    }

    seqCnt++;

    /* Release Service_1 (frame acquisition) at the acquisition sub-rate. */
    if ((seqCnt % acquisition_frequency) == 0)
        sem_post(&semS1);

    /* Release Service_2 (frame processing) at the capture sub-rate. */
    if ((seqCnt % frame_cap_frequency) == 0)
        sem_post(&semS2);

    /* Release Service_3 (frame storage) at the same capture sub-rate. */
    if ((seqCnt % frame_cap_frequency) == 0)
        sem_post(&semS3);
}

/**************************************************
 * Function name : void *Service_1_frame_acquisition(void *threadp)
 *    returns    : NULL (via pthread_exit)
 *    threadp    : pointer to threadParams_t with this thread's index
 * Created by    : course 4 team
 * Date created  : 2024
 * Description   : Real-time thread that acquires one V4L2 frame per
 *                 release. Blocks on semS1 (posted by Sequencer at
 *                 acquisition_frequency sub-rate), then calls
 *                 seq_frame_read() to dequeue a driver buffer, copy it
 *                 into the ring buffer, and re-queue the driver buffer.
 *                 Logs start and per-release timestamps to syslog.
 * Notes         : Scheduled at SCHED_FIFO priority RT_MAX-1.
 *                 Pinned to RT_CORE via pthread_attr_setaffinity_np() in main().
 *                 Must not block, allocate memory, or call non-reentrant functions
 *                 during the work phase to preserve timing determinism.
 **************************************************/
void *Service_1_frame_acquisition(void *threadp)
{
    struct timespec current_time_val;
    double current_realtime;
    unsigned long long S1Cnt = 0;
    threadParams_t *threadParams = (threadParams_t *)threadp;

    clock_gettime(MY_CLOCK_TYPE, &current_time_val);
    current_realtime = realtime(&current_time_val);
    syslog(LOG_CRIT, "S1 thread @ sec=%6.9lf\n", current_realtime - start_realtime);
    printf("S1 thread @ sec=%6.9lf\n", current_realtime - start_realtime);

    while (!abortS1)
    {
        /* Block until the Sequencer posts semS1. */
        sem_wait(&semS1);

        if (abortS1)
            break;

        S1Cnt++;

        /* Dequeue one V4L2 frame and place it in the acquisition ring buffer. */
        seq_frame_read();

        clock_gettime(MY_CLOCK_TYPE, &current_time_val);
        current_realtime = realtime(&current_time_val);
        syslog(LOG_CRIT, "S1 at 25 Hz on core %d for release %llu @ sec=%6.9lf\n",
               sched_getcpu(), S1Cnt, current_realtime - start_realtime);
    }

    pthread_exit((void *)0);
}

/**************************************************
 * Function name : void *Service_2_frame_process(void *threadp)
 *    returns    : NULL (via pthread_exit)
 *    threadp    : pointer to threadParams_t with this thread's index
 * Created by    : course 4 team
 * Date created  : 2024
 * Description   : Real-time thread that color-converts and motion-analyzes
 *                 one frame per release. Blocks on semS2 (posted by
 *                 Sequencer at frame_cap_frequency sub-rate), then calls
 *                 seq_frame_process(). Logs per-release timestamps to syslog.
 * Notes         : Scheduled at SCHED_FIFO priority RT_MAX-2.
 *                 Pinned to RT_CORE. Lower priority than Service_1 so that
 *                 the acquisition service can always preempt it.
 **************************************************/
void *Service_2_frame_process(void *threadp)
{
    struct timespec current_time_val;
    double current_realtime;
    unsigned long long S2Cnt = 0;
    int process_cnt;
    threadParams_t *threadParams = (threadParams_t *)threadp;

    clock_gettime(MY_CLOCK_TYPE, &current_time_val);
    current_realtime = realtime(&current_time_val);
    syslog(LOG_CRIT, "S2 thread @ sec=%6.9lf\n", current_realtime - start_realtime);
    printf("S2 thread @ sec=%6.9lf\n", current_realtime - start_realtime);

    while (!abortS2)
    {
        /* Block until the Sequencer posts semS2. */
        sem_wait(&semS2);

        if (abortS2)
            break;

        S2Cnt++;

        /* Color-convert the next ring-buffer frame and run motion detection. */
        process_cnt = seq_frame_process();

        clock_gettime(MY_CLOCK_TYPE, &current_time_val);
        current_realtime = realtime(&current_time_val);
        syslog(LOG_CRIT, "S2 at 1 Hz on core %d for release %llu @ sec=%6.9lf\n",
               sched_getcpu(), S2Cnt, current_realtime - start_realtime);
    }

    pthread_exit((void *)0);
}

/**************************************************
 * Function name : void *Service_3_frame_storage(void *threadp)
 *    returns    : NULL (via pthread_exit)
 *    threadp    : pointer to threadParams_t with this thread's index
 * Created by    : course 4 team
 * Date created  : 2024
 * Description   : Real-time thread that persists processed frames to the
 *                 ramdisk. Blocks on semS3 (posted by Sequencer at
 *                 frame_cap_frequency sub-rate), then calls seq_frame_store().
 *                 When seq_frame_store() returns num_frames_to_capture,
 *                 sets abortTest to initiate a clean pipeline shutdown.
 *                 Logs per-release timestamps to syslog.
 * Notes         : Scheduled at SCHED_FIFO priority RT_MAX-3.
 *                 Pinned to RT_CORE. Lowest priority of the three services
 *                 so storage I/O never preempts acquisition or processing.
 *                 The O_NONBLOCK flag in dump_pgm/dump_ppm minimizes the
 *                 time spent in the storage work phase.
 **************************************************/
void *Service_3_frame_storage(void *threadp)
{
    struct timespec current_time_val;
    double current_realtime;
    unsigned long long S3Cnt = 0;
    int store_cnt;
    threadParams_t *threadParams = (threadParams_t *)threadp;

    clock_gettime(MY_CLOCK_TYPE, &current_time_val);
    current_realtime = realtime(&current_time_val);
    syslog(LOG_CRIT, "S3 thread @ sec=%6.9lf\n", current_realtime - start_realtime);
    printf("S3 thread @ sec=%6.9lf\n", current_realtime - start_realtime);

    while (!abortS3)
    {
        /* Block until the Sequencer posts semS3. */
        sem_wait(&semS3);

        if (abortS3)
            break;

        S3Cnt++;

        /* Write the most recent processed frame to the ramdisk. */
        store_cnt = seq_frame_store();

        clock_gettime(MY_CLOCK_TYPE, &current_time_val);
        current_realtime = realtime(&current_time_val);
        syslog(LOG_CRIT, "S3 at 1 Hz on core %d for release %llu @ sec=%6.9lf\n",
               sched_getcpu(), S3Cnt, current_realtime - start_realtime);

        /* Signal a clean shutdown after the target number of frames is reached. */
        if (store_cnt == num_frames_to_capture)
        {
            abortTest = TRUE;
        }
    }

    pthread_exit((void *)0);
}

/**************************************************
 * Function name : double getTimeMsec(void)
 *    returns    : current monotonic time in milliseconds as a double
 * Created by    : course 4 team
 * Date created  : 2024
 * Description   : Reads the MY_CLOCK_TYPE clock and converts the result
 *                 to milliseconds. Useful for coarse latency measurements
 *                 without dealing with timespec arithmetic directly.
 **************************************************/
double getTimeMsec(void)
{
    struct timespec event_ts = {0, 0};

    clock_gettime(MY_CLOCK_TYPE, &event_ts);
    return ((event_ts.tv_sec) * 1000.0) + ((event_ts.tv_nsec) / 1000000.0);
}

/**************************************************
 * Function name : double realtime(struct timespec *tsptr)
 *    returns    : time represented by *tsptr as a double-precision second value
 *    tsptr      : pointer to the timespec to convert
 * Created by    : course 4 team
 * Date created  : 2024
 * Description   : Converts a struct timespec to a single double-precision
 *                 floating-point value in seconds. Used throughout the
 *                 pipeline to compute elapsed times for FPS and latency logs.
 **************************************************/
double realtime(struct timespec *tsptr)
{
    return ((double)(tsptr->tv_sec) + (((double)tsptr->tv_nsec) / 1000000000.0));
}

/**************************************************
 * Function name : void print_scheduler(void)
 *    returns    : void
 * Created by    : course 4 team
 * Date created  : 2024
 * Description   : Queries and prints the scheduling policy of the calling
 *                 process. Exits with -1 if the policy is not SCHED_FIFO,
 *                 because the pipeline requires real-time scheduling to
 *                 meet its deadline constraints.
 * Notes         : SCHED_DEADLINE is commented out; re-enable if the target
 *                 kernel supports it and the design migrates to EDF.
 **************************************************/
void print_scheduler(void)
{
    int schedType;

    schedType = sched_getscheduler(getpid());

    switch (schedType)
    {
    case SCHED_FIFO:
        printf("Pthread Policy is SCHED_FIFO\n");
        break;
    case SCHED_OTHER:
        printf("Pthread Policy is SCHED_OTHER\n");
        exit(-1);
        break;
    case SCHED_RR:
        printf("Pthread Policy is SCHED_RR\n");
        exit(-1);
        break;
    default:
        printf("Pthread Policy is UNKNOWN\n");
        exit(-1);
    }
}

/**************************************************
 * Function name : void log_sysinfo(void)
 *    returns    : void
 * Created by    : course 4 team
 * Date created  : 2024
 * Description   : Executes "uname -a" via popen() and logs the output to
 *                 syslog at LOG_INFO. Provides a permanent record of the
 *                 kernel version and machine architecture in the system log,
 *                 which is useful for post-hoc debugging and regression analysis.
 * Notes         : popen() is used here during initialization only, so
 *                 its non-real-time behavior is acceptable.
 *                 Exits with EXIT_FAILURE if popen() or fgets() fails.
 **************************************************/
void log_sysinfo(void)
{
    char buffer[256];

    /* Execute uname and read its output into buffer. */
    FILE *fp = popen("uname -a", "r");
    if (fp == NULL)
    {
        perror("popen failed");
        syslog(LOG_ERR, "[COURSE:%d] Failed to execute uname -a", COURSE);
        exit(EXIT_FAILURE);
    }

    if (fgets(buffer, sizeof(buffer), fp) != NULL)
    {
        syslog(LOG_INFO, "[COURSE #:%d][Final Project] %s", COURSE, buffer);
    }
    pclose(fp);
}

/**************************************************
 * Function name : void program_usage(const char *prog)
 *    returns    : void
 *    prog       : argv[0] - the name of the executable
 * Created by    : course 4 team
 * Date created  : 2024
 * Description   : Prints a usage summary and an invocation example to
 *                 stdout. Call when command-line argument parsing fails.
 **************************************************/
void program_usage(const char *prog)
{
    printf("Usage: %s [option] | <FRAME SAVE FREQUENCY>\n"
           "\n"
           "Options:\n"
           "  -h, --help     Show this help and exit\n"
           "\n"
           "<FRAME SAVE FREQUENCY> is the rate at which frames are saved and processed.\n"
           "\n"
           "Example:\n"
           "  %s 100 1 1800\n",
           prog, prog);
}

/**************************************************
 * Function name : void main(int argc, char *argv[])
 *    returns    : void
 *    argc       : count of command-line arguments
 *    argv       : array of command-line argument strings
 * Created by    : course 4 team
 * Date created  : 2024
 * Description   : Program entry point. Performs the following in order:
 *
 *   1. Logs system information via log_sysinfo().
 *   2. Parses argv[1] to optionally override acquisition_frequency and
 *      frame_cap_frequency (only the value 10 is currently recognized).
 *   3. Initializes the V4L2 pipeline and primes the ring buffer with
 *      one warm-up frame read.
 *   4. Sets the main process to SCHED_FIFO at RT_MAX priority.
 *   5. Creates three POSIX service threads, all pinned to RT_CORE with
 *      descending SCHED_FIFO priorities (RT_MAX-1, RT_MAX-2, RT_MAX-3).
 *   6. Arms the 100 Hz POSIX interval timer tied to the Sequencer handler.
 *   7. Joins all service threads (blocks until abortTest causes them to exit).
 *   8. Shuts down the V4L2 pipeline and prints final statistics.
 *
 * Notes         : The main thread's SCHED_FIFO priority is set above all
 *                 service threads so it can preempt them during setup and
 *                 teardown. Once the timer is armed and join() is called,
 *                 the main thread blocks and the service threads run freely.
 *                 argv[1] parsing only recognizes "10" to switch to a
 *                 10 Hz capture mode; extend the switch block for other rates.
 **************************************************/
void main(int argc, char *argv[])
{
    log_sysinfo();

    char *next;
    long val;

    /* Parse optional rate override from the first argument. */
    for (int i = 1; i < argc; i++)
    {
        val = strtol(argv[i], &next, 10);
        switch (i)
        {
        case 1:
            if ((next != argv[i]) || (*next == '\0'))
            {
                if (val == 10)
                {
                    /* Switch to 10 Hz capture mode. */
                    acquisition_frequency = 5;
                    frame_cap_frequency = 10;
                }
                else
                {
                    continue;
                }
            }
            else
            {
                program_usage(argv[0]);
            }
            break;
        default:
            break;
        }
    }

    struct timespec current_time_val, current_time_res;
    double current_realtime, current_realtime_res;

    /* Default V4L2 device node; override via argv if needed in future. */
    char *dev_name = "/dev/video0";

    int i, rc, scope, flags = 0;

    cpu_set_t threadcpu;
    cpu_set_t allcpuset;

    pthread_t threads[NUM_THREADS];
    threadParams_t threadParams[NUM_THREADS];
    pthread_attr_t rt_sched_attr[NUM_THREADS];
    int rt_max_prio, rt_min_prio, cpuidx;

    struct sched_param rt_param[NUM_THREADS];
    struct sched_param main_param;

    pthread_attr_t main_attr;
    pid_t mainpid;

    /* Initialize V4L2 hardware and start the sensor stream. */
    v4l2_frame_acquisition_initialization(dev_name);

    /* Prime the pipeline with one frame so the ring buffer is non-empty
     * when the service threads first run. */
    seq_frame_read();

    printf("Starting High Rate Sequencer Demo\n");

    /* Record the reference start time used by all log messages. */
    clock_gettime(MY_CLOCK_TYPE, &start_time_val);
    start_realtime = realtime(&start_time_val);

    clock_gettime(MY_CLOCK_TYPE, &current_time_val);
    current_realtime = realtime(&current_time_val);

    clock_getres(MY_CLOCK_TYPE, &current_time_res);
    current_realtime_res = realtime(&current_time_res);

    printf("START High Rate Sequencer @ sec=%6.9lf with resolution %6.9lf\n",
           (current_realtime - start_realtime), current_realtime_res);
    syslog(LOG_CRIT, "START High Rate Sequencer @ sec=%6.9lf with resolution %6.9lf\n",
           (current_realtime - start_realtime), current_realtime_res);

    printf("System has %d processors configured and %d available.\n",
           get_nprocs_conf(), get_nprocs());

    /* Build the full CPU set for logging purposes. */
    CPU_ZERO(&allcpuset);
    for (i = 0; i < NUM_CPU_CORES; i++)
        CPU_SET(i, &allcpuset);
    printf("Using CPUS=%d from total available.\n", CPU_COUNT(&allcpuset));

    /* Initialize binary semaphores to 0 (threads block immediately). */
    if (sem_init(&semS1, 0, 0))
    {
        printf("Failed to initialize S1 semaphore\n");
        exit(-1);
    }
    if (sem_init(&semS2, 0, 0))
    {
        printf("Failed to initialize S2 semaphore\n");
        exit(-1);
    }
    if (sem_init(&semS3, 0, 0))
    {
        printf("Failed to initialize S3 semaphore\n");
        exit(-1);
    }

    mainpid = getpid();

    /* Elevate the main process to SCHED_FIFO RT_MAX so it can set up
     * service threads without being preempted. */
    rt_max_prio = sched_get_priority_max(SCHED_FIFO);
    rt_min_prio = sched_get_priority_min(SCHED_FIFO);

    rc = sched_getparam(mainpid, &main_param);
    main_param.sched_priority = rt_max_prio;
    rc = sched_setscheduler(getpid(), SCHED_FIFO, &main_param);
    if (rc < 0)
        perror("main_param");
    print_scheduler();

    pthread_attr_getscope(&main_attr, &scope);

    if (scope == PTHREAD_SCOPE_SYSTEM)
        printf("PTHREAD SCOPE SYSTEM\n");
    else if (scope == PTHREAD_SCOPE_PROCESS)
        printf("PTHREAD SCOPE PROCESS\n");
    else
        printf("PTHREAD SCOPE UNKNOWN\n");

    printf("rt_max_prio=%d\n", rt_max_prio);
    printf("rt_min_prio=%d\n", rt_min_prio);

    /* Configure common RT attributes for all threads: SCHED_FIFO, RT_CORE affinity. */
    for (i = 0; i < NUM_THREADS; i++)
    {
        CPU_ZERO(&threadcpu);
        cpuidx = RT_CORE;
        CPU_SET(cpuidx, &threadcpu);

        rc = pthread_attr_init(&rt_sched_attr[i]);
        rc = pthread_attr_setinheritsched(&rt_sched_attr[i], PTHREAD_EXPLICIT_SCHED);
        rc = pthread_attr_setschedpolicy(&rt_sched_attr[i], SCHED_FIFO);
        rc = pthread_attr_setaffinity_np(&rt_sched_attr[i], sizeof(cpu_set_t), &threadcpu);

        /* Assign descending priorities; overridden per-thread below. */
        rt_param[i].sched_priority = rt_max_prio - i;
        pthread_attr_setschedparam(&rt_sched_attr[i], &rt_param[i]);

        threadParams[i].threadIdx = i;
    }

    printf("Service threads will run on %d CPU cores\n", CPU_COUNT(&threadcpu));

    /* Service_1: frame acquisition at RT_MAX-1 (highest service priority). */
    rt_param[0].sched_priority = rt_max_prio - 1;
    pthread_attr_setschedparam(&rt_sched_attr[0], &rt_param[0]);
    rc = pthread_create(&threads[0], &rt_sched_attr[0],
                        Service_1_frame_acquisition, (void *)&(threadParams[0]));
    if (rc < 0)
        perror("pthread_create for service 1 - V4L2 video frame acquisition");
    else
        printf("pthread_create successful for service 1\n");

    /* Service_2: frame processing at RT_MAX-2. */
    rt_param[1].sched_priority = rt_max_prio - 2;
    pthread_attr_setschedparam(&rt_sched_attr[1], &rt_param[1]);
    rc = pthread_create(&threads[1], &rt_sched_attr[1],
                        Service_2_frame_process, (void *)&(threadParams[1]));
    if (rc < 0)
        perror("pthread_create for service 2 - frame processing");
    else
        printf("pthread_create successful for service 2\n");

    /* Service_3: frame storage at RT_MAX-3 (lowest service priority). */
    rt_param[2].sched_priority = rt_max_prio - 3;
    pthread_attr_setschedparam(&rt_sched_attr[2], &rt_param[2]);
    rc = pthread_create(&threads[2], &rt_sched_attr[2],
                        Service_3_frame_storage, (void *)&(threadParams[2]));
    if (rc < 0)
        perror("pthread_create for service 3 - frame storage");
    else
        printf("pthread_create successful for service 3\n");

    /* Arm the 100 Hz interval timer that drives the Sequencer SIGALRM handler. */
    printf("Start sequencer\n");
    timer_create(CLOCK_REALTIME, NULL, &timer_1);
    signal(SIGALRM, (void (*)())Sequencer);

    /* 10 ms period: both interval and initial expiration set to 10,000,000 ns. */
    itime.it_interval.tv_sec = 0;
    itime.it_interval.tv_nsec = 10000000;
    itime.it_value.tv_sec = 0;
    itime.it_value.tv_nsec = 10000000;

    timer_settime(timer_1, flags, &itime, &last_itime);

    /* Block main() until all service threads exit following an abort. */
    for (i = 0; i < NUM_THREADS; i++)
    {
        if ((rc = pthread_join(threads[i], NULL)) < 0)
            perror("main pthread_join");
        else
            printf("joined thread %d\n", i);
    }

    /* Stop the V4L2 stream and release all resources. */
    v4l2_frame_acquisition_shutdown();

    printf("\nTEST COMPLETE\n");
}
