# p1tiler
 <img width="1472" height="1178" alt="Monitor_1_20261105-230442-565" src="https://github.com/user-attachments/assets/51710055-3479-4a1b-adb6-cf3b7fca8663" />

A lightweight, framebuffer-based tiling terminal manager for Linux, written in C.

P1Tiler was developed as a challenge to build a usable graphical terminal environment on an Intel Pentium 120 MHz with only 8 MiB of RAM, without swap, X11, or Wayland.

The project focuses on low memory consumption, efficient framebuffer rendering, and compatibility with older x86 hardware.

Features

Three independent workspaces

Up to four terminal windows per workspace

Binary-tree tiling window management

Compressed, switchable wallpapers

Dirty-cell rendering to minimize framebuffer writes

Configurable window gaps, shadows, and text colors

Live configuration reloading

Automatic framebuffer resolution detection

Support for RGB555 and RGB565 framebuffers

CPU and RAM usage indicators

Requirements

Linux with framebuffer support (/dev/fb0)

Linux virtual terminal (/dev/tty1)

Pseudoterminal (PTY) support

16-bit RGB555 or RGB565 framebuffer

A shell available at /bin/sh

P1Tiler is designed for 32-bit x86 systems. Static builds can be produced using a compatible musl toolchain.

Tested resolutions

640×480

800×600

1024×768

1280×1024

Keyboard shortcuts

Shortcut

Action

Alt + 1–4

Focus or spawn a terminal

Alt + Q/W/E

Switch workspace

Alt + H

Display help

Alt + -

Change wallpaper

Alt + '

Reload configuration

Configuration

The configuration file is located at:

/p1tiler/p1tiler.conf

It allows changing window gaps, shadows, and foreground text colors without restarting P1Tiler.

Wallpapers

Wallpapers are stored in resolution-specific directories:

/p1tiler/backgrounds/<resolution>/    ( currently supports : 640x480 800x600 1024x768 1280x1024) 

For example:

/p1tiler/backgrounds/1024x768/default00.bin

P1Tiler uses a custom compressed wallpaper format with indexed cells and small color palettes.

If a wallpaper cannot be loaded, P1Tiler falls back to a solid-color background.

Limitations

ASCII character support

Partial ANSI terminal escape sequence support

Only 16-bit RGB555/RGB565 framebuffers

Maximum display resolution of 1280×1024

Designed primarily for single-user, resource-constrained Linux systems

P1Tiler can be built with any compatible C toolchain. Static linking is recommended for minimal Linux installations.
note: compile only p1tiler.c the other files are included.


License:

P1Tiler is licensed under the GNU General Public License, version 2.0 only (GPL-2.0-only).

The included bitmap font is derived from the Linux kernel and retains its original GPL-2.0 license.

The included wallpapers were AI-generated using OpenAI's image generation tools specifically for P1Tiler. They were subsequently processed into P1Tiler's custom compressed wallpaper format.
