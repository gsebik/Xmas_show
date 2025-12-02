# Real-Time LED + Music Sequencer

A hard real-time LED and audio sequencer for Raspberry Pi. Synchronizes audio playback (MP3/WAV) with precisely timed LED patterns via GPIO.

## Features

- Real-time operation on Raspberry Pi 1/2/3/4
- Supports MP3 and WAV audio formats
- Dynamic sample rate handling (32kHz, 44.1kHz, 48kHz)
- Direct GPIO register access (memory-mapped)
- Multi-threaded design with SCHED_FIFO real-time scheduling
- LED thread: 10ms period, priority 80
- Audio thread: 30ms period, priority 75
- WAV files: mmap + mlock for hard real-time (no disk I/O during playback)
- MP3 files: ring buffer with ~3 sec pre-buffer for soft real-time
- Graceful shutdown with immediate LED turn-off on SIGTERM/SIGINT
- Optional UDP control mode
- Timing and jitter logging

## Dependencies

```bash
sudo apt-get install libasound2-dev libmpg123-dev
```

## Building

```bash
# For Raspberry Pi 4
make PLATFORM=RPI4

# For Raspberry Pi 1/Zero
make PLATFORM=RPI1

# Set capabilities for non-root execution
make setcap
```

Available platforms: `RPI1`, `RPI2`, `RPI3`, `RPI4`

## Capabilities Setup

The sequencer needs elevated privileges for GPIO access and real-time scheduling. Instead of running as root, use Linux capabilities:

```bash
sudo setcap cap_sys_rawio,cap_sys_nice+ep ./sequencer
```

Or use the Makefile target:
```bash
make setcap
```

This grants:
- `cap_sys_rawio`: GPIO memory mapping
- `cap_sys_nice`: SCHED_FIFO real-time scheduling

Note: Capabilities must be re-applied after each recompile.

## Usage

```bash
# Play a song (loads songname.mp3/.wav and songname.txt from music dir)
./sequencer songname

# Specify custom music directory
./sequencer -m /path/to/music/ songname

# Verbose mode (print timing stats)
./sequencer -v songname

# Turn all LEDs on and exit
./sequencer -s on

# Turn all LEDs off and exit
./sequencer -s off

# Interactive menu mode
./sequencer
```

## Directory Structure

```
/src        - C source files
/include    - Header files
/test       - Test files
```

## LED Pattern Format

Pattern files (`.txt`) must match the audio filename. Each line:

```
TTTT BBBB.BBBB
```

- `TTTT`: Duration in milliseconds (minimum 10ms)
- `BBBB.BBBB`: 8-bit LED pattern (1=on, 0=off), dot is optional separator

Example:
```
0100 1010.1100
0050 0101.0011
0200 1111.1111
```

## Hardware Requirements

- Raspberry Pi (1/2/3/4)
- 8 LEDs connected to GPIO pins
- Audio output (3.5mm jack or HDMI)
- ALSA-compatible audio

## Signal Handling

The sequencer handles SIGTERM and SIGINT for graceful shutdown:

1. Signal handler immediately turns off all LEDs via GPIO
2. Sets `stop_requested` flag for threads to check
3. Threads exit their loops after current sleep cycle
4. Main thread waits for threads to join
5. Final GPIO cleanup and exit

This ensures LEDs are turned off immediately when playback is stopped externally (e.g., via the v43 controller).

## Integration with v43-christmas-lights

This sequencer is designed to work with the v43-christmas-lights Node.js controller. The controller spawns the sequencer as a child process for each song playback and sends SIGTERM to stop playback.

See `/home/linux/v43-christmas-lights/` for the controller setup.
