#pragma once

#if defined(ARDULINUX_PLATFORM) || defined(LINUX_PLATFORM) || defined(PIO_UNIT_TESTING)

#include <Arduino.h>
#include "PtyConsole.h"

// Arduino Stream adapter over PtyConsole, plus stdin when that is a terminal.
//
// The repeater's text CLI reads from / writes to this instead of Serial, so the
// PTY carries only the CLI while Serial (stdout) keeps the debug logs -- the
// clean log/CLI split. On Linux Serial's read() is a stub that never returns a
// byte, so this is the only way a command reaches the daemon at all.
//
// Two doors onto one CLI:
//
//   * the PTY, published at a stable path (see PtyConsole) for
//     meshcore-cli -r -s <path> or any serial tool; and
//   * stdin, when meshcored runs in a foreground terminal, so a developer can
//     type at it without attaching anything. Only in the foreground: driving
//     the terminal of a backgrounded `meshcored &` would stop the job (see
//     begin()), so there stdin is left alone and the PTY is the only door.
//
// Both are drained on every read, so both can be watched by the event loop
// permanently. Output follows the input: every byte is written to stdout (when
// the command came over the PTY that is the copy journald keeps, alongside the
// debug log), and to the PTY only when the current command arrived there -- a
// foreground session's echo must not sit in the PTY's buffer waiting to greet
// the next client.
class LinuxConsole : public Stream {
    enum Source { FROM_PTY, FROM_STDIN };

    PtyConsole _pty;
    bool       _stdin_tty = false;     // stdin is a terminal, in raw mode, ours to read
    int        _peeked    = -1;        // one-byte lookahead over both sources
    Source     _src       = FROM_PTY;  // where the byte last read came from

    int fetch();

public:
    LinuxConsole() = default;
    ~LinuxConsole();

    // The descriptors below are owned, not shared, and stdin's terminal state is
    // process-wide. There is exactly one console.
    LinuxConsole(const LinuxConsole&) = delete;
    LinuxConsole& operator=(const LinuxConsole&) = delete;

    // Publish the PTY console (see PtyConsole::begin for how `link` and the
    // default search order are used) and, when stdin is a terminal this process
    // may drive, put it in raw mode for keystrokes. Never blocks. Returns true
    // if at least one input is usable; a failure is reported on stderr either
    // way.
    //
    // "may drive" is the foreground check: tcsetattr() and read() from a
    // background process group of the controlling terminal raise SIGTTOU /
    // SIGTTIN, which by default *stop* the process -- `meshcored &` would print
    // "Stopped" in setup() and never boot. Both signals are also set to SIG_IGN
    // here so that a session backgrounded later (Ctrl-Z, bg) degrades to a read
    // error rather than a stopped daemon.
    //
    // Also arranges for the terminal to be handed back on the way out: a
    // SIGINT / SIGTERM / SIGHUP handler restores it and re-raises. loop() never
    // returns and nothing calls exit(), so those signals -- not the destructor
    // -- are how this process actually ends, and a raw terminal survives it.
    bool begin(const char* link);

    // Unpublish and close the PTY and restore stdin's terminal mode. Idempotent.
    //
    // Exists for LinuxBoard::reboot(), which re-execs this process image: the
    // descriptors are close-on-exec, but the symlink is on disk, and a symlink
    // left pointing at a /dev/pts/N that the exec has just freed would name
    // whatever terminal next reuses that number. Removing it here first makes
    // the new image's begin() find nothing to reclaim rather than something to
    // decline.
    void end();

    bool        hasPty() const { return _pty.isOpen(); }
    const char* path()   const { return _pty.path(); }

    // Descriptors to watch for readability: the PTY master (-1 when there is
    // none) and stdin (-1 unless it is a terminal we are driving, and again
    // once that terminal hangs up). Every byte either delivers is consumed by
    // read(), so registering both permanently cannot leave a level-triggered
    // POLLIN that nothing drains.
    int ptyFd()   const { return _pty.fd(); }
    int stdinFd() const;

    int available() override;
    int peek() override;
    int read() override;
    size_t write(uint8_t c) override;
    void flush() override;
    using Print::write;  // pull in write(str) / write(buf, size)
};

extern LinuxConsole Console;

#endif // ARDULINUX_PLATFORM || LINUX_PLATFORM || PIO_UNIT_TESTING
