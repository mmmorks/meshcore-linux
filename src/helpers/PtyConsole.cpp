#ifndef _GNU_SOURCE
#define _GNU_SOURCE  // ptsname_r()
#endif

#include "PtyConsole.h"

#if defined(ARDULINUX_PLATFORM) || defined(LINUX_PLATFORM) || defined(PIO_UNIT_TESTING)

#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

namespace {

// The systemd unit's RuntimeDirectory. It exists only while the unit runs and
// is owned by the service user, so "present and writable" identifies the
// packaged-service case without any configuration.
const char* const RUNTIME_DIR = "/run/meshcored";

// fcntl() rather than O_CLOEXEC at open time: posix_openpt() takes only O_RDWR
// and O_NOCTTY portably, and this file also compiles for the host test build on
// macOS. The window between creating a descriptor and marking it does not
// matter here -- the only exec is this process's own reboot(), which cannot run
// part-way through begin().
void set_cloexec(int fd) {
    int fl = fcntl(fd, F_GETFD, 0);
    if (fl != -1) fcntl(fd, F_SETFD, fl | FD_CLOEXEC);
}

void set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl != -1) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

// True if `link` is a symlink whose target still exists as a character device:
// a console that some other process is holding open right now. A symlink left
// by a daemon that has since exited points at a /dev/pts/N that no longer
// exists (the kernel removes the node when the master closes), so it is stale
// and safe to replace. The one thing this cannot tell apart is a stale symlink
// whose pts number an unrelated terminal has since reused; that case declines
// a path it could have taken, and says so, which is the cheap direction to be
// wrong in.
bool held_by_live_console(const char* link) {
    struct stat st;
    return stat(link, &st) == 0 && S_ISCHR(st.st_mode);
}

}  // namespace

PtyConsole::~PtyConsole() { end(); }

bool PtyConsole::begin(const char* link) {
    if (master_fd != -1) return true;  // already open

    int fd = posix_openpt(O_RDWR | O_NOCTTY);
    if (fd < 0) {
        fprintf(stderr, "meshcore: console posix_openpt() failed: %s\n", strerror(errno));
        return false;
    }
    set_cloexec(fd);
    set_nonblock(fd);

    if (grantpt(fd) != 0) {
        fprintf(stderr, "meshcore: console grantpt() failed: %s\n", strerror(errno));
        close(fd);
        return false;
    }

    char buf[128];
    if (ptsname_r(fd, buf, sizeof(buf)) != 0) {
        fprintf(stderr, "meshcore: console ptsname_r failed: %s\n", strerror(errno));
        close(fd);
        return false;
    }
    pts_path = buf;

    // Owner-only: attaching to the console grants the privileged local CLI, so
    // the mode is the whole access gate. Set before unlockpt(), not after: Linux
    // devpts creates the slave node 0620 root:tty and grantpt() keeps that, so
    // every instant between the two is one in which anybody in group tty could
    // open the console. open() on a pts that is still locked fails, so doing it
    // in this order leaves no window at all -- and ptsname_r() and chmod() both
    // work on a locked pts (verified on Linux and macOS). A mode we could not
    // set is fatal for the same reason it is the gate.
    if (chmod(pts_path.c_str(), 0600) != 0) {
        fprintf(stderr, "meshcore: console chmod(%s, 0600) failed: %s\n", pts_path.c_str(),
                strerror(errno));
        close(fd);
        pts_path.clear();
        return false;
    }

    if (unlockpt(fd) != 0) {
        fprintf(stderr, "meshcore: console unlockpt() failed: %s\n", strerror(errno));
        close(fd);
        pts_path.clear();
        return false;
    }

    // Raw line discipline: no echo / canonical / CR-NL translation, so bytes
    // pass through unchanged (our read() does the '\n'->'\r' mapping itself).
    struct termios t;
    if (tcgetattr(fd, &t) == 0) {
        cfmakeraw(&t);
        tcsetattr(fd, TCSANOW, &t);
    }

    // Our own descriptor on the slave, so a detached console reads as idle
    // rather than hung up (see the class comment). O_NOCTTY: this must not
    // become the daemon's controlling terminal.
    int holder = open(pts_path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (holder < 0) {
        fprintf(stderr, "meshcore: console open(%s) failed: %s\n", pts_path.c_str(), strerror(errno));
        close(fd);
        pts_path.clear();
        return false;
    }
    set_cloexec(holder);

    master_fd = fd;
    holder_fd = holder;
    peeked    = -1;

    // Publish a stable symlink so clients have a fixed path across restarts
    // (the /dev/pts/N number varies). Every candidate can fail -- held by
    // another instance, occupied by something that is not ours to remove, or
    // simply unwritable -- and each failure is reported, because a console that
    // silently came up somewhere else is the least diagnosable outcome. If none
    // can be made, clients can still use the raw pts path (path() falls back to
    // it).
    const char* xdg = getenv("XDG_RUNTIME_DIR");
    std::string candidates[4];
    int n = 0;
    if (link && *link) candidates[n++] = link;
    if (access(RUNTIME_DIR, W_OK) == 0) candidates[n++] = std::string(RUNTIME_DIR) + "/console";
    if (xdg && *xdg) candidates[n++] = std::string(xdg) + "/meshcore/console";
    candidates[n++] = std::string("/tmp/meshcore-") + std::to_string((unsigned)getuid()) + "/console";

    for (int i = 0; i < n && link_path.empty(); i++) publish(candidates[i].c_str());
    if (link_path.empty())
        fprintf(stderr, "meshcore: no console symlink could be published; use %s\n", pts_path.c_str());
    return true;
}

// Point `link` at the slave device. Returns false, with the reason on stderr,
// if that cannot be done safely.
bool PtyConsole::publish(const char* link) {
    if (held_by_live_console(link)) {
        fprintf(stderr, "meshcore: console %s is in use by another instance; not taking it over\n", link);
        return false;
    }

    // The parent directory is created only for the per-user defaults; the
    // runtime directory belongs to systemd and a configured path to the
    // operator. mkdir() on an existing directory is harmless -- and is not the
    // check, because a directory that already exists is the interesting case:
    // whoever owns it owns the console. On a shared machine an unprivileged
    // user can pre-create /tmp/meshcore-0 as a symlink to /etc and aim a root
    // daemon's symlink() and unlink() at it, or leave it 0777 and swap our
    // console symlink for a PTY of their own, at which point the operator's
    // next `get prv.key` is typed into their terminal. So: take a directory
    // only if it is really a directory, is ours, and is closed to everyone
    // else.
    std::string dir(link);
    size_t slash = dir.rfind('/');
    if (slash != std::string::npos && slash > 0) {
        dir.erase(slash);
        mkdir(dir.c_str(), 0700);
        struct stat dst;
        if (lstat(dir.c_str(), &dst) != 0) {
            fprintf(stderr, "meshcore: console directory %s is unusable: %s\n", dir.c_str(),
                    strerror(errno));
            return false;
        }
        if (!S_ISDIR(dst.st_mode) || dst.st_uid != geteuid() || (dst.st_mode & 0077) != 0) {
            fprintf(stderr,
                    "meshcore: console directory %s is not a private directory owned by uid %u; "
                    "not using it\n",
                    dir.c_str(), (unsigned)geteuid());
            return false;
        }
    }

    // Replace only what we could have created. The path is operator input, and
    // unlink() does not care what it removes. lstat(), not stat(): the symlink
    // itself is the question, not what it points at.
    struct stat st;
    if (lstat(link, &st) == 0) {
        if (!S_ISLNK(st.st_mode)) {
            fprintf(stderr, "meshcore: console path %s exists and is not a symlink; refusing to remove it\n", link);
            return false;
        }
        unlink(link);
    }

    if (symlink(pts_path.c_str(), link) != 0) {
        fprintf(stderr, "meshcore: console symlink(%s) failed: %s\n", link, strerror(errno));
        return false;
    }
    link_path = link;
    return true;
}

void PtyConsole::end() {
    if (!link_path.empty()) { unlink(link_path.c_str()); link_path.clear(); }
    if (holder_fd != -1) { close(holder_fd); holder_fd = -1; }
    if (master_fd != -1) { close(master_fd); master_fd = -1; }
    pts_path.clear();
    peeked = -1;
}

int PtyConsole::available() {
    int n = (peeked >= 0) ? 1 : 0;
    if (master_fd != -1) {
        int q = 0;
        if (ioctl(master_fd, FIONREAD, &q) == 0 && q > 0) n += q;
    }
    return n;
}

int PtyConsole::peek() {
    if (peeked < 0) peeked = read();
    return peeked;
}

int PtyConsole::read() {
    if (peeked >= 0) { int c = peeked; peeked = -1; return c; }
    if (master_fd == -1) return -1;
    unsigned char b;
    ssize_t n = ::read(master_fd, &b, 1);
    // Map '\n' -> '\r' (1:1) so line-oriented CLIs that terminate on '\r' work
    // with tools that send '\n'. Kept 1:1 so available()/read() stay consistent
    // (the repeater's read() is unchecked).
    if (n == 1) return (b == '\n') ? '\r' : b;
    // EAGAIN: no data right now. (EIO cannot happen while holder_fd is open.)
    return -1;
}

size_t PtyConsole::write(uint8_t c) {
    if (master_fd != -1) {
        // A PTY master write with no reader just buffers (or EAGAIN under
        // O_NONBLOCK once the slave's input queue is full) -- no SIGPIPE -- so
        // unwritten console output is simply dropped, never fatal.
        ssize_t r = ::write(master_fd, &c, 1);
        (void)r;
    }
    return 1;
}

#endif // ARDULINUX_PLATFORM || LINUX_PLATFORM || PIO_UNIT_TESTING
