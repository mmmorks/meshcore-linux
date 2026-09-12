#include "LinuxConsole.h"

#if defined(ARDULINUX_PLATFORM) || defined(LINUX_PLATFORM) || defined(PIO_UNIT_TESTING)

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <termios.h>
#include <unistd.h>

LinuxConsole Console;

namespace {

// stdin's terminal mode as we found it. This lives here rather than in the
// object because the signal handler below has to reach it and a handler is
// given no context of its own; there is exactly one console (see the header),
// so there is exactly one terminal to remember.
struct termios        g_orig_tty;
volatile sig_atomic_t g_raw_active = 0;  // g_orig_tty holds something to restore

// Async-signal-safe: tcsetattr() is on POSIX's list and nothing else here does
// anything. Shared by end() and by the handler.
void restore_stdin() {
    if (g_raw_active) {
        g_raw_active = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &g_orig_tty);
    }
}

// loop() never returns and nothing calls exit(), so the destructor is not how
// this process ends -- SIGINT (Ctrl-C) and SIGTERM (systemctl stop) are. Left
// to their default action they kill the daemon with the terminal still in raw
// mode, and the operator is left typing blind at a shell with no echo until
// they think to run `reset`. So: put the terminal back, then die exactly as we
// would have -- same signal, default disposition, so the exit status still
// reports it, and a shell still sees "terminated by SIGINT".
void restore_tty_and_reraise(int sig) {
    restore_stdin();
    signal(sig, SIG_DFL);
    raise(sig);
}

void install_restore_handler(int sig) {
    struct sigaction old;
    // Only take a signal nothing else is handling: replacing another handler
    // would silently disable whatever it was for.
    if (sigaction(sig, nullptr, &old) != 0 || old.sa_handler != SIG_DFL) return;
    struct sigaction sa;
    sa.sa_handler = restore_tty_and_reraise;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;  // no SA_RESTART: the main loop's poll() should see EINTR
    sigaction(sig, &sa, nullptr);
}

// True if putting stdin in raw mode and reading it cannot stop this process.
//
// isatty() alone is not enough, because it is just as true for `meshcored &`:
// tcsetattr() from a process in a background process group of its *controlling*
// terminal sends SIGTTOU to that whole group, and read() sends SIGTTIN, both of
// which stop it by default. begin() runs inside setup(), so a backgrounded
// daemon would report "Stopped" and never boot. tcgetpgrp() answers the
// question directly. Failing with ENOTTY answers it too: the terminal on stdin
// is not this process's controlling terminal, and job control does not apply to
// it at all (stdin redirected from an unrelated tty, and the test suite's
// pseudo-terminal, both land here).
bool stdin_is_drivable() {
    if (!isatty(STDIN_FILENO)) return false;
    pid_t fg = tcgetpgrp(STDIN_FILENO);
    return fg < 0 ? errno == ENOTTY : fg == getpgrp();
}

// A terminal in the VMIN=0 mode begin() sets returns 0 from read() both when
// nothing has been typed and when it has gone away, so poll() is what tells the
// two apart. POLLIN has to be asked for even though POLLHUP is reported
// regardless: with an empty event mask macOS reports nothing at all.
bool stdin_hung_up() {
    struct pollfd p = { STDIN_FILENO, POLLIN, 0 };
    return poll(&p, 1, 0) > 0 && (p.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0;
}

}  // namespace

LinuxConsole::~LinuxConsole() { end(); }

bool LinuxConsole::begin(const char* link) {
    bool pty_ok = _pty.begin(link);

    // Job control must never stop the daemon. Ignoring these does not replace
    // the check below -- with SIGTTOU ignored tcsetattr() is *allowed* to
    // proceed, which is exactly the wrong outcome for a background job -- but
    // it does mean that a session backgrounded after startup (Ctrl-Z, bg), or
    // one writing to a terminal under `stty tostop`, gets an error return
    // instead of a stopped process.
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);

    // Foreground use: read keystrokes one at a time, no kernel echo (we echo
    // ourselves, exactly as the PTY path does), Enter delivered as the '\r' the
    // CLI terminates on, and never block the mesh loop waiting for input --
    // VMIN=0/VTIME=0 returns 0 from read() the moment there is nothing to read,
    // which is why the descriptor is left alone: O_NONBLOCK belongs to the open
    // file description, which fd 0 shares with the shell that started us, and
    // it would still be set there after this process exits. ISIG stays on so
    // Ctrl-C still stops the daemon.
    bool drivable = stdin_is_drivable();
    if (drivable && !g_raw_active && tcgetattr(STDIN_FILENO, &g_orig_tty) == 0) {
        struct termios raw = g_orig_tty;
        raw.c_lflag &= ~(ICANON | ECHO);
        raw.c_iflag &= ~(ICRNL | INLCR);
        raw.c_cc[VMIN]  = 0;
        raw.c_cc[VTIME] = 0;
        if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0) {
            g_raw_active = 1;
            install_restore_handler(SIGINT);
            install_restore_handler(SIGTERM);
            install_restore_handler(SIGHUP);
        }
    }
    // Only a terminal that is actually in that mode may be read: a canonical
    // one would block the entire daemon until somebody pressed Enter.
    _stdin_tty = drivable && g_raw_active;
    return pty_ok || _stdin_tty;
}

void LinuxConsole::end() {
    _pty.end();
    restore_stdin();
    _stdin_tty = false;
    _peeked    = -1;
}

int LinuxConsole::stdinFd() const {
    return _stdin_tty ? STDIN_FILENO : -1;
}

// The next byte from either source, or -1. The PTY is asked first, but nothing
// waits on it: whichever has a byte delivers it, so neither can starve the
// other or leave itself readable and unread.
int LinuxConsole::fetch() {
    int c = _pty.read();  // already maps '\n' -> '\r'
    if (c >= 0) { _src = FROM_PTY; return c; }
    if (_stdin_tty) {
        unsigned char b;
        ssize_t       n = ::read(STDIN_FILENO, &b, 1);
        if (n == 1) {
            _src = FROM_STDIN;
            return (b == '\n') ? '\r' : b;  // same mapping, same reason
        }
        // Stop watching a terminal that has gone away: `nohup meshcored &` over
        // an ssh session that then drops (nohup does not redirect stdin). fd 0
        // stays open and stays hung up forever, and a level condition is the
        // one thing the event loop can only throttle, not clear -- the busy
        // loop this console exists to remove, at a thousand wake-ups a second.
        // idleUntilEvent() re-registers from stdinFd() on every iteration, so
        // clearing the flag is what unregisters it.
        if (n == 0 ? stdin_hung_up() : (errno != EINTR && errno != EAGAIN)) _stdin_tty = false;
    }
    return -1;
}

int LinuxConsole::available() {
    if (_peeked < 0) _peeked = fetch();
    return _peeked >= 0 ? 1 : 0;
}

int LinuxConsole::peek() {
    if (_peeked < 0) _peeked = fetch();
    return _peeked;
}

int LinuxConsole::read() {
    int c = peek();
    _peeked = -1;
    return c;
}

size_t LinuxConsole::write(uint8_t c) {
    putchar(c);                           // journald's copy, or the terminal
    if (_src == FROM_PTY) _pty.write(c);  // and the attached console client
    // Flushed per byte, unconditionally. At a terminal, a command being typed
    // has no newline yet and its echo would sit in the buffer until Enter --
    // the user types blind. Under systemd there is no line buffering to rely on
    // at all: stdout is a socket, which the C library block-buffers (it
    // line-buffers only a terminal), so a reply would wait behind 4 KB of log
    // before journald saw it. One write() per CLI byte, and only on CLI traffic
    // -- the debug log takes the buffered path set up in setup().
    fflush(stdout);
    return 1;
}

void LinuxConsole::flush() {
    fflush(stdout);
    _pty.flush();
}

#endif // ARDULINUX_PLATFORM || LINUX_PLATFORM || PIO_UNIT_TESTING
