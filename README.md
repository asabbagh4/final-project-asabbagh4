# machine-vision-synchronome

## Background

The inspiration for this project is the Shortt-Synchronome free pendulum clock is a complex precision electromechanical clock invented in 1921 by William Hamilton Shortt. It was designed to be a more accurate clock than the earth itself.
It works by using an electrically-impulsed pendulum to control a series of slave clocks, using a feedback loop; the pendulum swing moves a count wheel, which every half-minute, releases a gravity arm to give the pendulum a small, precise push (free of interference), as the gravity arm falls, it closes an electrical contact that sends a pulse to reset the arm and trigger the slave clocks to ensure synchronicity.

The aim of this project is to create a machine vision synchronome with accuracies of 1Hz and 10Hz in clock tick detection. The key issue to resolve in tackling this undertaking is that of multiple clock domains such as the processor clock, the physical analog clock which the camera apparatus is taking photos of, and likewise the digital stopwatch.
The successful outcome of this project will be realized in the following:

- To have developed a match between the internal processor clock used in the program to an external clock tick or increment.
- Computer to real-world sync over time.
- Tracing for verification.

## Hardware Used

- Raspberry Pi 4 8GB RAM
- 32GB Samsung EVO+ Micro SD Card
- Logitech C270 Webcam (Any webcam will do so far as it is 30FPS)

## Real-Time Requirements

A sequencer is used as an orchestrator for all real time services required by this program. The sequencer is driven by a hardware oscillator that is set to run at a rate of 100Hz.
The services run by this sequencer orchestrator are:

- Frame acquisition service that captures new frames to the camera device’s internal ring-buffer at a rate of 20Hz. (Hard real time service)
- Frame processor service that compares the saved frame in the FLASH ring buffer to images in the camera device’s internal ring-buffer. If the difference passes a threshold, it is passed to the service responsible for frame saving, and replaces the image in the FLASH ring buffer at a rate of 2Hz. (Hard real time service)
- Frame saving service is responsible for saving a frame to a specified FLASH folder. (Best effort service)
- Logging service used for tracing and verification that writes to syslog after selecting and saving a stable frame at a given interval at a 1Hz rate. (Hard real time service)

## Usage

After cloning the project, run

```sh
make

 # Create mount point in RAM for frame storage
sudo mkdir -p /mnt/ramdisk
sudo mount -t tmpfs -o size=1024M tmpfs /mnt/ramdisk
sudo mkdir /mnt/ramdisk/frames

# The two options for sequencer are a 1Hz capture/storage rate or 10Hz. It is a 1Hz capture rate if no args are provided.
sudo ./sequencer # OR sudo ./sequencer 10
```

`sudo` permissions are required to run the program because we need to adjust the scheduling policy to `SCHED_FIFO` which provides real-time scheduling from the POSIX standard.

## References

- https://en.wikipedia.org/wiki/Shortt%E2%80%93Synchronome_clock#:~:text=The%20Shortt%E2%80%93Synchronome%20free%20pendulum,produced%20between%201922%20and%201956.
- https://github.com/sbsiewertcsu/computer-vision-starter-code
