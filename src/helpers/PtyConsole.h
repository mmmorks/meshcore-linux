#pragma once

// Also built for the host test env (PlatformIO defines PIO_UNIT_TESTING there),
// where the PTY mechanics are exercised end to end against real /dev/pts nodes.
#if defined(ARDULINUX_PLATFORM) || defined(LINUX_PLATFORM) || defined(PIO_UNIT_TESTING)

#include <stddef.h>
#include <stdint.h>
#include <string>

// A local console carried over a pseudo-terminal (PTY), for the Linux repeater's
// text CLI.
//
// meshcored opens a PTY master and publishes a stable symlink to the slave
// device (e.g. /run/meshcored/console -> /dev/pts/N) so a serial client can
// attach to it directly:  meshcore-cli -r -s /run/meshcored/console
// (meshcore-cli's repeater mode drives a raw-text serial CLI via pyserial, which
// needs a tty — hence a PTY rather than a socket).
//
// Pure POSIX (no Arduino dependency) so the read/write/newline logic is
// unit-testable on the host; LinuxConsole wraps it in an Arduino Stream.
//
// The PTY master persists for the daemon's life; a client just opens/closes the
// slave, so there is no accept/reap. The slave device is chmod'd 0600 -- the
// unauthenticated local CLI's access gate -- while the pts is still locked, so
// there is no instant at which it is open to group tty.
//
// The daemon also keeps one descriptor of its own on the slave open. Without it
// the master reports POLLHUP and read() fails with EIO from the moment the last
// client closes until the next one opens -- a level condition that would wake a
// poll()-based main loop continuously. With it, a detached console is simply
// idle: poll() sleeps, read() says "nothing yet", and the next client attaches
// as if the previous one had never left.
//
// Every descriptor is close-on-exec. LinuxBoard::reboot() re-execs this process
// image, and a master that reached the new image would keep the old /dev/pts/N
// alive with nobody reading it.
class PtyConsole {
    int master_fd = -1;      // PTY master; -1 when closed
    int holder_fd = -1;      // our own descriptor on the slave (see above)
    int peeked = -1;         // one-byte pushback for peek(); -1 when empty
    std::string link_path;   // published symlink to the slave (unlinked on end())
    std::string pts_path;    // the slave device path (/dev/pts/N)

    bool publish(const char* link);

public:
    PtyConsole() = default;
    ~PtyConsole();

    PtyConsole(const PtyConsole&) = delete;
    PtyConsole& operator=(const PtyConsole&) = delete;

    // Open the PTY and publish a symlink to it. `link` non-empty is tried first;
    // then, in order, /run/meshcored/console (when that directory exists and is
    // writable -- the systemd unit's RuntimeDirectory), $XDG_RUNTIME_DIR/
    // meshcore/console, and /tmp/meshcore-<uid>/console. A candidate is skipped,
    // with a line on stderr, if it is held by another live console, is something
    // other than a symlink, or sits in a directory that is not ours and private
    // (whoever can write that directory can substitute their own PTY for the
    // console, which is the unauthenticated admin CLI). Returns true on success;
    // on failure logs to stderr and returns false. The PTY itself is always
    // created even when no symlink could be published; path() then names the raw
    // pts device.
    bool begin(const char* link);
    void end();

    int available();         // bytes available from the client (0 if none)
    int peek();              // peek one byte without consuming (-1 if none)
    int read();              // read one byte (-1 if none); maps '\n' -> '\r'
    size_t write(uint8_t c); // write one byte to the client; -> 1
    void flush() {}

    bool isOpen() const { return master_fd != -1; }
    // The master descriptor, for poll(): readable exactly when a client has sent
    // bytes that read() has not yet consumed. -1 when closed.
    int fd() const { return master_fd; }
    // Path a client should open: the published symlink, else the raw pts device.
    const char* path() const {
        return link_path.empty() ? pts_path.c_str() : link_path.c_str();
    }
};

#endif // ARDULINUX_PLATFORM || LINUX_PLATFORM || PIO_UNIT_TESTING
