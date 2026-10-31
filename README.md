# Chaos Popup

A harmless popup prank for Windows, written in C. Realistic error popups appear at random places on the screen, slowly at first and then faster, each with its own Windows error sound. They stay open until the sequence ends, then all close together when the program exits after about 40 seconds.

**Use it only on your own computer or with the consent of the person using it.** The popups look like real system errors and can alarm someone who does not know what they are. Do not use this to scare, mislead or pressure anyone. This repository contains only the harmless version.

## What It Does

- Shows 200 popups, with the gap between them shrinking from 500 ms to 40 ms.
- Closes all of them when it exits, 3 seconds after the last popup, even if nobody touches them.
- Plays each sound on its own audio output, so sounds that overlap can still be heard.
- Stops at once when you press Esc, and always exits within 90 seconds.

## What It Does Not Do

- It does not change files or settings. It only reads the sound files that come with Windows.
- It does not start with Windows, copy itself or use the network.
- It does not ask for input, passwords or payment. The error messages are fixed text and nothing they say is real.
- It takes no command line options, and the limits are constants in the source.

## How It Works

`MessageBoxTimeoutA` is an undocumented function in `user32.dll` that closes a message box after a set time. Here it is a backup that closes a popup after 60 seconds. The program finds it at run time and exits if it is missing.

Each popup runs in its own thread with a CBT hook, which moves its window to a random position as it opens, using the real window size so that it can reach every edge. Popups are spread across all connected displays by screen area. This was only run on a computer with one display, so the multi-display part is untested. A shared `PlaySound` call would cut off the previous sound, so each sound gets its own `waveOut` device, with at most 24 playing together.

## Building

No binary is included. Build it with MinGW-w64:

```
gcc chaos_popup.c -o chaos_popup.exe -mwindows -luser32 -lwinmm
```

## License

Apache License 2.0. See [LICENSE](LICENSE). Copyright 2026 1337leets.
