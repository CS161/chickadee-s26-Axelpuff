CS 161 Problem Set 5 Answers
============================
Leave your name out of this file. Put collaboration notes and credit in
`pset5collab.md`.

Answers to written questions
----------------------------
## 1. What was your goal?

My original goal was to port GNU nano to Chickadee. I was motivated by the realization that a teaching operating system is an extraordinarily effortful and beautiful creation completely unusable for anything practical. In practice, this turned into a large terminal and userspace compatibility project. Before nano could plausibly run, Chickadee needed enough of a Unix-style terminal stack for ncurses to work.

The goal therefore became to implement the minimum kernel and userspace infrastructure needed to support ncurses programs, then build a small ncurses-based text editor, as the end-user demonstration. This was meant to preserve the spirit of the original nano plan while keeping the final editor small enough to control and debug.

But as James Hague writes about programming, "If you focus in on one area it expands like a fractal." In the end, I mostly ended up drowning in the complexities of the `tty` interface, which has an entire language of control sequences and signals which I didn't know existed. When I finally got to actually porting ncurses, I didn't have enough time left to debug successfully, so didn't achieve the above goal either.

The end result includes a VT100/ANSI terminal emulator, raw-mode terminal input, termios support, ioctl window-size support, minimal signal support, a Chickadee terminfo entry, and a ported static `libncurses.a` that compiles and links...on my device. There was a demo program, `p-curses.cc`, but I wasn't able to get colors to work.

## 2. What’s your design?

The main feature is kernel-side terminal emulator. Stdout and stderr now go through a VT100 parser before reaching the console framebuffer.

The terminal state is stored in a `tty_state` structure. This keeps track of the cursor position, saved cursor position, screen size, color attributes, scroll region, terminal modes, and input buffering state. The VT100 parser itself is mostly stateless: it consumes a stream of bytes and dispatches actions onto the `tty_state`.

Input is handled by a "line discipline". In canonical mode, it behaves like the old console, but in raw mode, it makes each keypress immediately available to userspace. This is essential because ncurses programs need to receive control keys and ordinary characters without waiting for a newline. The line discipline also respects terminal flags such as `ICANON`, `ECHO`, and `ISIG`.

I also had to add POSIX-based terminal APIs. `tcgetattr` and `tcsetattr` expose the state in the termios struct for configuration. `ioctl` supports `TIOCGWINSZ` and `TIOCSWINSZ`, so ncurses can get the terminal dimensions. There is a minimal signal system supports installing handlers, raising signals, delivering pending signals on return to userspace, and handling terminal-driven signals such as `SIGINT` and `SIGWINCH`.

## 3. What code did you write?

The main kernel-side additions were the VT100 terminal emulator and TTY infrastructure. I added files for the parser and terminal state, including `k-vt100.cc` and `k-vt100.hh`. These files contain the parser, terminal state, and line discipline.

I added a new terminal vnode in `k-vfs.hh`/`k-vfs.cc` so that writes to stdin and stdout go through the terminal parser instead of writing raw characters to the framebuffer. In this vnode, reads come from the line discipline and writes go into the VT100 parser.

There is also the minimal signal system in `kernel.cc` This included per-process signal state, `kill`, `sigaction`, signal delivery on return to userspace, `sigreturn`, default and ignored dispositions, and enough signal behavior for ncurses and editor programs to tolerate `SIGINT` and `SIGWINCH`.

I added a Chickadee terminfo source file, build rules for compiling or embedding the terminfo entry, and Makefile rules for building and linking ncurses into Chickadee userspace programs. However, I didn't give myself enough time to figure out how to make these work on the grading server.

## 4. What challenges did you encounter?

Cross-compiling ncurses was very painful. The build wanted a much more complete Unix environment than Chickadee had. Some headers had to exist for configure and linking, but using Chickadee’s incomplete headers too early could break the ncurses build by shadowing the host system headers. The final approach was to compile ncurses carefully against system headers, then provide the Chickadee implementations and compatibility symbols at final link time.

There were also many small libc and glibc-compatibility traps. Ncurses referenced functions such as `__errno_location`, ctype lookup functions, checked string and printf variants, `poll`, `clock_gettime`, `nanosleep`, locale functions, `setjmp`/`longjmp`, and several file and tree-search functions. Most of these did not need full implementations for this project, but they did need safe enough stubs or minimal versions for ncurses to initialize and run.

## 5. How can we test your work?

The following custom tests should pass:

```sh
p-vttest
p-rawread
p-sigwinch
```
Grading notes
-------------
