/***************************************************
 * Module name: process_frame.h
 *
 * First written in 2026 by Laye Tenumah, adapted from Sam Siewert template.
 *
 * Module Description:
 * Public interface for the frame processing module.
 * Declares the motion detection ring buffer type and
 * instance, the shared scratchpad conversion workspace,
 * and every function implemented in process_frame.c.
 *
 * Function categories declared here:
 *   Public  - called by sequencer.c and store_frame.c:
 *               seq_frame_process(), yuv2rgb(),
 *               process_motion_detection(), reset_motion_buffer()
 *   Private (static, forward-declared for ordering clarity):
 *               process_image()
 *
 ***************************************************/

#ifndef _PROCESS_FRAME_
#define _PROCESS_FRAME_

/*  Defines section
***************************************************/

/* Active horizontal resolution in pixels.
 * Must match the HRES value in v4l2_interface.h. */
#define HRES (640)

/* Active vertical resolution in pixels.
 * Must match the VRES value in v4l2_interface.h. */
#define VRES (480)

/*  Type definitions
***************************************************/

/*
 * motion_buffer_t - Minimal two-slot ring buffer for inter-frame motion analysis.
 *
 * Holds the two most recent grayscale frames so that process_motion_detection()
 * can compute a pixel-wise absolute difference without keeping the full
 * acquisition ring buffer in scope. The ring is intentionally small: only
 * the immediately preceding frame is needed for a frame-to-frame comparison.
 *
 * Members:
 *   ring_size  - Fixed capacity of the frames[] array; always 2.
 *   tail_idx   - Index of the slot that will receive the next incoming frame.
 *                Wraps modulo ring_size after each store.
 *   count      - Number of valid frames currently stored (0, 1, or 2).
 *                Used to gate comparisons until a baseline frame exists.
 *   frames[][] - Storage for up to 2 raw grayscale frames.
 *                Each slot holds exactly HRES*VRES bytes (one luma plane).
 */
struct motion_buffer_t
{
    unsigned int ring_size;
    int tail_idx;
    int count;
    unsigned char frames[2][HRES * VRES];
};

/*  External variable declarations
***************************************************/

/* Shared motion detection ring buffer; defined and initialized in
 * process_frame.c. Read by store_frame.c to retrieve the most recently
 * processed grayscale frame for disk output. */
extern struct motion_buffer_t motion_buffer;

/* Scratch area used during color-space conversion inside process_image().
 * Holds the converted output before it is passed to process_motion_detection()
 * or store_frame.c. Sized for the largest possible raw frame to avoid
 * reallocation: MAX_HRES * MAX_VRES * MAX_PIXEL_SIZE bytes. */
extern unsigned char scratchpad_buffer[];

/* Running count of frames submitted to process_image().
 * Incremented on every call regardless of motion detection outcome. */
extern int process_framecnt;

/*  Public function prototypes
***************************************************/

/**************************************************
 * Function name : void yuv2rgb(int y, int u, int v,
 *                              unsigned char *r,
 *                              unsigned char *g,
 *                              unsigned char *b)
 *    returns    : void
 *    y          : luminance sample in range 0-255
 *    u          : Cb chrominance sample in range 0-255 (128 = neutral)
 *    v          : Cr chrominance sample in range 0-255 (128 = neutral)
 *    r          : output red   channel, clamped to 0-255
 *    g          : output green channel, clamped to 0-255
 *    b          : output blue  channel, clamped to 0-255
 * Created by    : course 4 team
 * Date created  : 2024
 * Description   : Converts a single YCbCr sample to RGB using integer-only
 *                 arithmetic. Coefficients are scaled by 256 and right-
 *                 shifted by 8 bits to approximate the BT.601 floating-point
 *                 conversion matrix without requiring an FPU.
 *                 Formula (c = y-16, d = u-128, e = v-128):
 *                   R = (298c        + 409e + 128) >> 8
 *                   G = (298c - 100d - 208e + 128) >> 8
 *                   B = (298c + 516d        + 128) >> 8
 *                 All three output channels are clamped to [0, 255].
 * Notes         : Called in the COLOR_CONVERT_RGB path of process_image()
 *                 for each YUYV macropixel pair (two pixels per call).
 *                 Not used in the COLOR_CONVERT_GRAY path.
 **************************************************/
void yuv2rgb(int y, int u, int v,
             unsigned char *r, unsigned char *g, unsigned char *b);

/**************************************************
 * Function name : int process_motion_detection(
 *                     const unsigned char *current_frame,
 *                     int frame_size)
 *    returns    :  0 - first frame; stored as baseline, no comparison done
 *                  1 - motion detected; absolute-difference percentage > 0.4%
 *                 -1 - no significant motion; difference at or below threshold
 *    current_frame : pointer to the grayscale luma plane to evaluate;
 *                    must be frame_size bytes of contiguous pixel data
 *    frame_size    : byte length of current_frame; expected to be HRES*VRES
 * Created by    : course 4 team
 * Date created  : 2024
 * Description   : Compares current_frame against the most recently stored
 *                 frame in motion_buffer using a per-pixel absolute
 *                 difference sum. Normalizes the total against the
 *                 theoretical maximum (frame_size * 255) to produce a
 *                 percentage. Declares motion if the percentage exceeds
 *                 0.4%. In all cases, stores current_frame into motion_buffer
 *                 for use as the reference on the next call.
 * Notes         : The 0.4% threshold was determined empirically; lower
 *                 values increase sensitivity but also false-positive rate.
 *                 The ring buffer wraps at ring_size (2) slots so only the
 *                 immediately preceding frame is used for comparison.
 *                 Called from process_image() in the COLOR_CONVERT_GRAY path.
 **************************************************/
int process_motion_detection(const unsigned char *current_frame, int frame_size);

/**************************************************
 * Function name : void reset_motion_buffer(void)
 *    returns    : void
 * Created by    : course 4 team
 * Date created  : 2024
 * Description   : Zeroes all frame data in motion_buffer and resets
 *                 tail_idx and count to their initial values, discarding
 *                 any stored baseline frames.
 *                 Call when reinitializing the pipeline, after a known
 *                 abrupt scene change, or whenever the comparison baseline
 *                 should be considered invalid.
 * Notes         : Thread safety: no mutex is held. Call only when the
 *                 processing service thread is not concurrently executing
 *                 process_motion_detection().
 **************************************************/
void reset_motion_buffer(void);

/**************************************************
 * Function name : int seq_frame_process(void)
 *    returns    : running process_framecnt after this invocation, or 0
 *                 if read_framecnt indicates no valid frames have been
 *                 acquired yet
 * Created by    : course 4 team
 * Date created  : 2024
 * Description   : Sequencer-facing entry point called by
 *                 Service_2_frame_process() in sequencer.c. Advances
 *                 the ring buffer head index to the target frame slot,
 *                 calls process_image() on that frame to perform
 *                 color-space conversion and motion detection, then
 *                 advances the head index and decrements the ring buffer
 *                 count to reflect the slots consumed. Updates fnow for
 *                 downstream FPS logging.
 * Notes         : The head index arithmetic (advance by 2, then by 3) and
 *                 the fixed count decrement of 5 implement a specific skip
 *                 pattern for the current ring geometry. Review and adjust
 *                 these offsets if ring_size or FRAMES_PER_SEC changes.
 **************************************************/
int seq_frame_process(void);

/*  Private (static) function prototypes
 *
 * process_image() is file-scoped in process_frame.c. It is declared here
 * so that the complete function inventory of this module is visible in one
 * place and so that future unit-test harnesses can reference it without
 * modifying the source file. Do not call it from outside this module.
 ***************************************************/

/**************************************************
 * Function name : static int process_image(const void *p, int size)
 *    returns    : running process_framecnt after this call
 *    p          : pointer to the raw frame data in the acquisition ring buffer
 *    size       : byte length of the raw frame (HRES * VRES * PIXEL_SIZE)
 * Created by    : course 4 team
 * Date created  : 2024
 * Description   : Dispatches the raw frame to the correct color-space
 *                 conversion path based on the pixel format stored in the
 *                 global fmt struct negotiated during V4L2 initialization:
 *
 *                   V4L2_PIX_FMT_GREY:
 *                     Frame is already single-channel grayscale;
 *                     no conversion performed.
 *
 *                   V4L2_PIX_FMT_YUYV with COLOR_CONVERT_RGB:
 *                     Unpacks each 4-byte YUYV macropixel [Y0 U Y1 V]
 *                     to two RGB triplets (6 bytes) in scratchpad_buffer
 *                     using yuv2rgb().
 *
 *                   V4L2_PIX_FMT_YUYV with COLOR_CONVERT_GRAY:
 *                     Extracts only bytes 0 and 2 of each macropixel
 *                     (Y0 and Y1) into scratchpad_buffer, yielding
 *                     size/2 luma bytes, then calls
 *                     process_motion_detection() on the result.
 *
 *                   V4L2_PIX_FMT_RGB24:
 *                     Frame is already RGB24; no conversion needed.
 *
 * Notes         : Increments process_framecnt unconditionally before
 *                 dispatching, so the count always reflects total frames
 *                 seen regardless of format or conversion outcome.
 *                 Called exclusively from seq_frame_process().
 **************************************************/
static int process_image(const void *p, int size);

#endif /* _PROCESS_FRAME_ */
