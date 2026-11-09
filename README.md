# deskrec

A small command-line recorder for Linux desktop audio. It can record everything
your computer is playing, or just one application (for example a web browser),
and saves it as a WAV file.

It is written in C++17 and only depends on `libpulse`, so it works on any desktop
running PulseAudio or PipeWire (through `pipewire-pulse`, the default on
Ubuntu, Fedora, Debian, Arch, etc.).

## Build

Install a compiler, CMake and the PulseAudio client headers:

| Distro          | Command                                              |
| --------------- | ---------------------------------------------------- |
| Debian / Ubuntu | `sudo apt install g++ cmake pkg-config libpulse-dev` |
| Fedora          | `sudo dnf install gcc-c++ cmake pulseaudio-libs-devel` |
| Arch            | `sudo pacman -S gcc cmake pkgconf libpulse`          |

Then:

```sh
cmake -S . -B build
cmake --build build
sudo cmake --install build      # optional: installs to /usr/local/bin
```

The binary only needs `libpulse.so.0` at runtime, which every desktop already
has, so a build from one machine also runs on other machines with the same or a
newer distro release.

## Usage

```sh
deskrec list                          # outputs and apps currently playing audio
deskrec record                        # everything on the default output
deskrec record -a firefox             # only Firefox
deskrec record -a chrome -a spotify   # several apps, mixed together
deskrec record -i 57                  # one specific stream from 'list'
deskrec record -s <output-name>       # a specific output (headphones, HDMI, ...)
deskrec record -d 60 -o clip.wav      # stop after 60 seconds
```

Stop a recording with Ctrl+C. Run `deskrec --help` for all options.

Behaviour worth knowing:

- Recording starts when the first audio arrives, so there's no leading silence.
- In app mode (`-a`), deskrec follows the app: streams that start later, move to
  another output or end are picked up and dropped automatically, and multiple
  streams (e.g. several browser tabs) are mixed into one file. While the app is
  silent, silence is recorded.
- In whole-output mode deskrec follows the default output if you switch it
  (e.g. plug in headphones). It never falls back to the microphone.
- Audio is written about 3 seconds behind real time (needed to mix streams
  that arrive with different delays); everything is flushed when you stop.
- The header is updated every second, so the file stays playable even if the
  process gets killed.
- 16-bit WAV files are limited to about 6 hours at 48 kHz stereo.
