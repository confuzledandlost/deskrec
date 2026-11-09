# deskrec

A small recorder for Linux desktop audio. It can record everything your
computer is playing, or just one application (for example a web browser), and
saves it as WAV, FLAC or MP3. It comes as a command-line tool (`deskrec`) and a
simple GTK 4 window (`deskrec-gui`).

It is written in C++17 on top of `libpulse`, so it works on any desktop running
PulseAudio or PipeWire (through `pipewire-pulse`, the default on Ubuntu, Fedora,
Debian, Arch, etc.). FLAC, MP3 and the GUI are optional at build time: if their
libraries aren't installed, the rest still builds.

## Build

Install a compiler, CMake and the development headers:

| Distro          | Command                                                                     |
| --------------- | --------------------------------------------------------------------------- |
| Debian / Ubuntu | `sudo apt install g++ cmake pkg-config libpulse-dev libflac-dev libmp3lame-dev libgtk-4-dev` |
| Fedora          | `sudo dnf install gcc-c++ cmake pulseaudio-libs-devel flac-devel lame-devel gtk4-devel` |
| Arch            | `sudo pacman -S gcc cmake pkgconf libpulse flac lame gtk4`                  |

Then:

```sh
cmake -S . -B build
cmake --build build
sudo cmake --install build      # optional: installs to /usr/local
```

CMake prints what it found (`deskrec output formats: wav flac mp3`,
`deskrec GUI: enabled`). Installing puts both programs on your PATH and adds
deskrec to your desktop's app launcher. At runtime they need `libpulse`,
`libFLAC`, `libmp3lame` and GTK 4, which almost every desktop already has, so a
build from one machine also runs on other machines with the same or a newer
distro release.

## GUI

Run `deskrec-gui` or open "deskrec" from your app launcher. Pick what to record
(all desktop audio, a specific output, or an app) and press Record. The app list
updates live, and apps stay listed after they go quiet, so you can select one
and press Record before it starts playing. System sounds (notification and
volume-change sounds, speech-dispatcher's silent placeholder stream) aren't
listed, but "All desktop audio" still includes them.

Below the list you can choose the format and its quality, the folder (default:
WAV in your Music folder), and what to do on silence:

- **New track after N seconds of silence** saves each song or clip as its own
  numbered file (`... - 01.flac`, `... - 02.flac`), with the gaps left out.
- **Stop after N seconds of silence** ends the recording on its own once
  playback has finished.

All of these are remembered in `~/.config/deskrec/settings.ini`. Files are
named after the source and the time, e.g. `Firefox - 2026-11-08 23-40.flac`; a
number is added if that name is taken. FLAC and MP3 files are also tagged with
a title, the source as artist, the date, and the track number when splitting.

While recording, a level meter shows how loud the captured audio is, and
Pause/Resume skips over parts you don't want. The result is one file with no
gap where the pause was. If a recording finishes while the window isn't
focused (e.g. auto-stop), you get a desktop notification. Closing the window
while recording saves what you have so far.

## Command line

```sh
deskrec list                          # outputs and apps currently playing audio
deskrec record                        # everything on the default output
deskrec record -a firefox             # only Firefox
deskrec record -a chrome -a spotify   # several apps, mixed together
deskrec record -i 57                  # one specific stream from 'list'
deskrec record -s <output-name>       # a specific output (headphones, HDMI, ...)
deskrec record -d 60 -o clip.wav      # stop after 60 seconds
deskrec record -o song.flac           # format follows the file extension
deskrec record -F mp3 -b 320          # or pick it explicitly
deskrec record -a firefox -F flac --split 2 --auto-stop 30
                                      # one file per song; stop when playback ends
deskrec record --title "Interview" --artist "Podcast"   # FLAC/MP3 tags
```

| Format | Option    | Quality                                    | Size (stereo, 48 kHz) |
| ------ | --------- | ------------------------------------------ | --------------------- |
| WAV    | `-F wav`  | Lossless, uncompressed (16-bit, `-f` = 32-bit float) | ~11 MB/min |
| FLAC   | `-F flac` | Lossless, compressed (16-bit, `-f` = 24-bit) | ~5 MB/min, varies with content |
| MP3    | `-F mp3`  | Lossy; high-quality VBR by default, `-b KBPS` for constant bitrate | ~1.5 MB/min |

Without `-o`, the file goes in the current directory and is named after what
you record, e.g. `Firefox - 2026-11-08 23-40.wav` or
`Desktop audio - 2026-11-08 23-40.wav`. Run `deskrec --help` for all options.

While recording in a terminal, press **space** (or `p`) to pause and resume and
**q** (or Ctrl+C) to stop. From another terminal or a keyboard shortcut,
`pkill -USR1 -x deskrec` toggles pause (`-x` keeps it from matching
`deskrec-gui`).

Behaviour worth knowing:

- Recording starts when the first audio arrives, so there's no leading silence.
- Silence means below -60 dBFS. With `--split` or `--auto-stop`, silence at the
  start is skipped and each file keeps only half a second of trailing silence.
  `--split` accepts up to 30 seconds.
- In app mode (`-a`), deskrec follows the app: streams that start later, move to
  another output or end are picked up and dropped automatically, and multiple
  streams (e.g. several browser tabs) are mixed into one file. While the app is
  silent, silence is recorded.
- In whole-output mode deskrec follows the default output if you switch it
  (e.g. plug in headphones). It never falls back to the microphone.
- Audio is written about 3 seconds behind real time (needed to mix streams
  that arrive with different delays); everything is flushed when you stop.
- Data is flushed to disk every second, so the file stays playable even if the
  process gets killed (FLAC and MP3 players may show a wrong duration then).
- WAV files are limited to 4 GB, about 6 hours at 48 kHz stereo 16-bit.
  FLAC and MP3 have no practical limit.
- WAV files have no tags; FLAC uses Vorbis comments and MP3 uses ID3v2.

## Code layout

- `src/core/` – the recording engine (`Recorder`), the live source list
  (`SourceWatcher`), the stream mixer and the WAV/FLAC/MP3 writers. It runs on
  any PulseAudio main loop and reports through callbacks.
- `src/cli/` – `deskrec`, on PulseAudio's own main loop.
- `src/gui/` – `deskrec-gui`, on GLib's main loop (single-threaded, no locking).
