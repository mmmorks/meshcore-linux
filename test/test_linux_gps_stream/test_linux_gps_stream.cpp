#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>
#include <string>

// openpty(): macOS declares it in <util.h>; glibc in <pty.h>.
#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

#include <Arduino.h>
#include <MicroNMEA.h>

#include "LinuxGpsStream.h"

namespace {

using Target = LinuxGpsStream::Target;

// The GPS descriptor is protected so the event loop cannot register it (see
// LinuxBoard::idleUntilEvent). Tests still have to see it, to check that it
// cannot survive the execv() in LinuxBoard::reboot().
class TestGpsStream : public LinuxGpsStream {
public:
  using LinuxGpsStream::fd;
};

TEST(ParseDevice, EmptyIsNoTransportButValid) {
  Target t = LinuxGpsStream::parseDevice("");
  EXPECT_TRUE(t.valid);
  EXPECT_EQ(t.transport, LinuxGpsStream::NO_SOURCE);
}

TEST(ParseDevice, NullIsNoTransportButValid) {
  Target t = LinuxGpsStream::parseDevice(nullptr);
  EXPECT_TRUE(t.valid);
  EXPECT_EQ(t.transport, LinuxGpsStream::NO_SOURCE);
}

TEST(ParseDevice, DevicePathIsSerial) {
  Target t = LinuxGpsStream::parseDevice("/dev/ttyS0");
  EXPECT_TRUE(t.valid);
  EXPECT_EQ(t.transport, LinuxGpsStream::SERIAL_DEVICE);
}

TEST(ParseDevice, BareSchemeUsesDefaults) {
  Target t = LinuxGpsStream::parseDevice("gpsd://");
  ASSERT_TRUE(t.valid);
  EXPECT_EQ(t.transport, LinuxGpsStream::GPSD_SOCKET);
  EXPECT_STREQ(t.host, "127.0.0.1");
  EXPECT_EQ(t.port, 2947);
}

TEST(ParseDevice, HostOnlyUsesDefaultPort) {
  Target t = LinuxGpsStream::parseDevice("gpsd://gpsbox.local");
  ASSERT_TRUE(t.valid);
  EXPECT_STREQ(t.host, "gpsbox.local");
  EXPECT_EQ(t.port, 2947);
}

TEST(ParseDevice, HostAndPort) {
  Target t = LinuxGpsStream::parseDevice("gpsd://10.0.0.5:3000");
  ASSERT_TRUE(t.valid);
  EXPECT_STREQ(t.host, "10.0.0.5");
  EXPECT_EQ(t.port, 3000);
}

TEST(ParseDevice, EmptyHostWithPortUsesDefaultHost) {
  Target t = LinuxGpsStream::parseDevice("gpsd://:3000");
  ASSERT_TRUE(t.valid);
  EXPECT_STREQ(t.host, "127.0.0.1");
  EXPECT_EQ(t.port, 3000);
}

TEST(ParseDevice, RejectsMissingPortAfterColon) {
  EXPECT_FALSE(LinuxGpsStream::parseDevice("gpsd://host:").valid);
}

TEST(ParseDevice, RejectsNonNumericPort) {
  EXPECT_FALSE(LinuxGpsStream::parseDevice("gpsd://host:abc").valid);
}

TEST(ParseDevice, RejectsZeroPort) {
  EXPECT_FALSE(LinuxGpsStream::parseDevice("gpsd://host:0").valid);
}

TEST(ParseDevice, RejectsOutOfRangePort) {
  EXPECT_FALSE(LinuxGpsStream::parseDevice("gpsd://host:70000").valid);
}

// Only hostnames and IPv4 are supported. A bare IPv6 literal is ambiguous
// against the host:port split, so it is rejected loudly rather than
// misparsed -- bad_values makes LinuxBoard::begin() refuse to start.
TEST(ParseDevice, RejectsMultipleColons) {
  EXPECT_FALSE(LinuxGpsStream::parseDevice("gpsd://::1:2947").valid);
}

TEST(ParseDevice, RejectsOverlongHost) {
  std::string url = "gpsd://";
  url.append(80, 'a');
  EXPECT_FALSE(LinuxGpsStream::parseDevice(url.c_str()).valid);
}

// Minimal stand-in for gpsd: listens on an ephemeral loopback port, accepts one
// client, and lets the test push bytes at it. Enough to exercise the handshake,
// the read path and a mid-stream disconnect.
class FakeGpsd {
public:
  FakeGpsd() {
    _listen = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(_listen, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;                       // ephemeral
    bind(_listen, (sockaddr*)&addr, sizeof addr);
    listen(_listen, 4);
    // Non-blocking: acceptClient() is polled from loops that also have to make
    // progress when the stream has not (re)connected yet. A blocking accept()
    // deadlocks those.
    fcntl(_listen, F_SETFL, fcntl(_listen, F_GETFL, 0) | O_NONBLOCK);
    socklen_t len = sizeof addr;
    getsockname(_listen, (sockaddr*)&addr, &len);
    _port = ntohs(addr.sin_port);
  }
  ~FakeGpsd() { closeClient(); if (_listen >= 0) close(_listen); }

  int port() const { return _port; }

  // Accept a pending connection. Returns false if none arrived.
  // accept() + fcntl() rather than accept4(): these tests build on the host,
  // which may be macOS, where accept4()/SOCK_NONBLOCK do not exist.
  // Always tries to accept, so a reconnect is picked up even while an older
  // client fd is still held. acceptCount() is then how a test distinguishes
  // "same connection" from "connected again".
  bool acceptClient() {
    sockaddr_in from{};
    socklen_t len = sizeof from;
    int fd = accept(_listen, (sockaddr*)&from, &len);
    if (fd < 0) return _client >= 0;
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    if (_client >= 0) close(_client);
    _client = fd;
    _accepts++;
    return true;
  }

  int acceptCount() const { return _accepts; }

  void send(const std::string& s) { if (_client >= 0) ::write(_client, s.data(), s.size()); }

  // Accumulate whatever the client has written (the WATCH command).
  std::string received() {
    char buf[512];
    while (_client >= 0) {
      ssize_t n = ::recv(_client, buf, sizeof buf, MSG_DONTWAIT);
      if (n <= 0) break;
      _rx.append(buf, n);
    }
    return _rx;
  }

  void closeClient() { if (_client >= 0) { close(_client); _client = -1; } }
  void forgetRx() { _rx.clear(); }

private:
  int _listen  = -1;
  int _client  = -1;
  int _port    = 0;
  int _accepts = 0;
  std::string _rx;
};

std::string gpsdUrl(int port) {
  return "gpsd://127.0.0.1:" + std::to_string(port);
}

// Drive the non-blocking connect state machine to completion. The first reads
// legitimately return -1 while connect() is still in flight.
void settle(LinuxGpsStream& s, FakeGpsd& fake, int rounds = 200) {
  for (int i = 0; i < rounds; i++) {
    fake.acceptClient();
    s.read();
    usleep(500);
  }
}

// Pump the stream until it yields a byte or the budget runs out.
int readWithin(LinuxGpsStream& s, FakeGpsd& fake, int attempts = 400) {
  for (int i = 0; i < attempts; i++) {
    fake.acceptClient();
    int c = s.read();
    if (c >= 0) return c;
    usleep(500);
  }
  return -1;
}

TEST(GpsdTransport, ConnectsAndSendsWatch) {
  FakeGpsd fake;
  LinuxGpsStream s;
  ASSERT_TRUE(s.begin(gpsdUrl(fake.port()).c_str(), 9600));
  EXPECT_EQ(s.transport(), LinuxGpsStream::GPSD_SOCKET);

  settle(s, fake);

  std::string got = fake.received();
  EXPECT_NE(got.find("?WATCH="), std::string::npos) << "got: " << got;
  EXPECT_NE(got.find("\"nmea\":true"), std::string::npos) << "got: " << got;
}

TEST(GpsdTransport, ReadsNmeaBytes) {
  FakeGpsd fake;
  LinuxGpsStream s;
  ASSERT_TRUE(s.begin(gpsdUrl(fake.port()).c_str(), 9600));
  settle(s, fake);
  fake.send("$GNGGA,x\r\n");
  EXPECT_EQ(readWithin(s, fake), '$');
}

// The point of the whole design: gpsd interleaves JSON control lines with the
// NMEA, and MicroNMEA must ignore them rather than be corrupted by them.
TEST(GpsdTransport, JsonBannerDoesNotCorruptFixParsing) {
  FakeGpsd fake;
  LinuxGpsStream s;
  ASSERT_TRUE(s.begin(gpsdUrl(fake.port()).c_str(), 9600));
  settle(s, fake);

  fake.send("{\"class\":\"VERSION\",\"release\":\"3.22\",\"rev\":\"3.22\"}\r\n");
  fake.send("{\"class\":\"DEVICES\",\"devices\":[{\"path\":\"/dev/ttyS0\"}]}\r\n");
  fake.send("$GNGGA,214011.000,4739.71889,N,12219.58334,W,1,18,0.8,80.5,M,-21.6,M,,*45\r\n");

  char buf[100];
  MicroNMEA nmea(buf, sizeof buf);
  for (int i = 0; i < 6000; i++) {
    fake.acceptClient();
    int c = s.read();
    if (c >= 0) nmea.process((char)c);
    else usleep(200);
  }

  EXPECT_TRUE(nmea.isValid());
  EXPECT_EQ(nmea.getNumSatellites(), 18);
}

// A gpsd that has not started yet must not disable GPS for the whole boot:
// initBasicGPS() asks isPresent() exactly once.
TEST(GpsdTransport, IsPresentWhenConfiguredButUnreachable) {
  LinuxGpsStream s;
  ASSERT_TRUE(s.begin("gpsd://127.0.0.1:1", 9600));   // nothing listens on :1
  EXPECT_TRUE(s.isPresent());
}

TEST(GpsdTransport, NotPresentWhenUnconfigured) {
  LinuxGpsStream s;
  s.begin("", 9600);
  EXPECT_FALSE(s.isPresent());
}

TEST(GpsdTransport, WriteIsNoOp) {
  FakeGpsd fake;
  LinuxGpsStream s;
  ASSERT_TRUE(s.begin(gpsdUrl(fake.port()).c_str(), 9600));
  settle(s, fake);
  EXPECT_EQ(s.write((uint8_t)'$'), 0u);
}

TEST(GpsdTransport, InvalidUrlFailsBegin) {
  LinuxGpsStream s;
  EXPECT_FALSE(s.begin("gpsd://host:abc", 9600));
  EXPECT_FALSE(s.isPresent());
}

// LinuxBoard::reboot() re-execs this process image, and execv() keeps every
// descriptor that is not close-on-exec. Neither of these is reachable from the
// new image, so each reboot would otherwise leak one for the life of the
// rebooted daemon -- and hold gpsd's client slot open with nobody reading it.
TEST(GpsdTransport, SocketIsCloseOnExec) {
  FakeGpsd fake;
  TestGpsStream s;
  ASSERT_TRUE(s.begin(gpsdUrl(fake.port()).c_str(), 9600));
  settle(s, fake);

  ASSERT_GE(s.fd(), 0);
  EXPECT_TRUE(fcntl(s.fd(), F_GETFD) & FD_CLOEXEC);
}

TEST(GpsdReconnect, ReconnectsAfterServerDrop) {
  FakeGpsd fake;
  LinuxGpsStream s;
  ASSERT_TRUE(s.begin(gpsdUrl(fake.port()).c_str(), 9600));
  settle(s, fake);
  fake.send("$A\r\n");
  ASSERT_EQ(readWithin(s, fake), '$');

  fake.closeClient();
  for (int i = 0; i < 50; i++) { s.read(); usleep(500); }   // observe the hangup

  g_mock_millis += 60000;                                   // past the backoff
  settle(s, fake);
  fake.send("$B\r\n");
  EXPECT_EQ(readWithin(s, fake), '$');
}

// gps off stops the drain. An undrained socket fills its receive window and
// gpsd drops clients it cannot write to, and on gps on the backlog would be
// parsed as if current. Reconnecting after a gap avoids both.
TEST(GpsdReconnect, ReconnectsAfterReadGap) {
  FakeGpsd fake;
  LinuxGpsStream s;
  ASSERT_TRUE(s.begin(gpsdUrl(fake.port()).c_str(), 9600));
  settle(s, fake);
  fake.send("$A\r\n");
  ASSERT_EQ(readWithin(s, fake), '$');

  ASSERT_EQ(fake.acceptCount(), 1);

  // The server stays up and the connection stays healthy. A gap in reads alone
  // must force a fresh connection -- otherwise this passes for the wrong
  // reason, via the server-drop path rather than the gap.
  // One time step only: a gap-drop reconnects immediately rather than backing
  // off, so advancing again here would trip the gap a second time and the
  // count would be ambiguous.
  g_mock_millis += 10000;      // longer than the 5 s gap threshold
  settle(s, fake);

  EXPECT_EQ(fake.acceptCount(), 2) << "a read gap should have reconnected";
}

// The negative case for the test above: a pause shorter than the threshold
// is normal poll-loop jitter, not a `gps off`, and must not pay the cost of
// a reconnect.
TEST(GpsdReconnect, ShortReadGapDoesNotReconnect) {
  FakeGpsd fake;
  LinuxGpsStream s;
  ASSERT_TRUE(s.begin(gpsdUrl(fake.port()).c_str(), 9600));
  settle(s, fake);
  fake.send("$A\r\n");
  ASSERT_EQ(readWithin(s, fake), '$');

  ASSERT_EQ(fake.acceptCount(), 1);

  g_mock_millis += 2000;      // well under the 5 s gap threshold
  settle(s, fake);

  EXPECT_EQ(fake.acceptCount(), 1) << "a short gap must not force a reconnect";
}

TEST(GpsdReconnect, UnreachableGpsdKeepsRetryingWithoutSpinning) {
  LinuxGpsStream s;
  ASSERT_TRUE(s.begin("gpsd://127.0.0.1:1", 9600));   // nothing listens
  for (int i = 0; i < 50; i++) s.read();
  // The point is that it neither crashes nor gives up: reads keep returning -1
  // and the stream stays present for a gpsd that may yet start.
  EXPECT_EQ(s.read(), -1);
  EXPECT_TRUE(s.isPresent());
}

// A short-write test for finishConnect()'s partial-WATCH-write path was
// attempted and dropped: the command is ~38 bytes, and forcing write() to
// return short/EAGAIN for a payload that small requires shrinking the
// *client* socket's own SO_SNDBUF below ~38 bytes. Shrinking the fake
// server's SO_RCVBUF (the only buffer a test harness can reach here) does
// not do it -- write() succeeds once data fits in the local kernel send
// buffer, regardless of the peer's advertised window, and LinuxGpsStream
// exposes no hook to shrink the client fd's own SO_SNDBUF. The dropConnection()
// call on the short-write path is exercised by code review and by the
// existing error-path tests (invalid URL, unreachable host) taking the same
// dropConnection() branch instead.

// Minimal pty-backed stand-in for a real /dev/tty* GPS receiver. openpty()
// hands back a connected master/slave pair; the slave is closed immediately
// so LinuxGpsStream can open the path itself (mirroring how it opens a real
// device node), and the test drives bytes in through the master side.
class FakePtyGps {
public:
  FakePtyGps() {
    int master = -1, slave = -1;
    char name[256] = {};
    if (openpty(&master, &slave, name, nullptr, nullptr) != 0) return;
    close(slave);   // LinuxGpsStream::openSerial() opens its own fd on `name`
    fcntl(master, F_SETFL, fcntl(master, F_GETFL, 0) | O_NONBLOCK);
    _master = master;
    _path = name;
    _ok = true;
  }
  ~FakePtyGps() { if (_master >= 0) close(_master); }

  bool ok() const { return _ok; }
  const char* path() const { return _path.c_str(); }
  int masterFd() const { return _master; }
  void send(const std::string& s) { ::write(_master, s.data(), s.size()); }

private:
  bool _ok = false;
  int _master = -1;
  std::string _path;
};

// Pump the stream until it yields a byte or the budget runs out. No accept()
// step needed (unlike the gpsd readWithin()): a pty has no listen/accept
// phase, so LinuxGpsStream::begin() has the device open by the time this runs.
int readSerialWithin(LinuxGpsStream& s, int attempts = 400) {
  for (int i = 0; i < attempts; i++) {
    int c = s.read();
    if (c >= 0) return c;
    usleep(500);
  }
  return -1;
}

TEST(SerialGpsStream, OpensDeviceAndReadsBytes) {
  FakePtyGps pty;
  ASSERT_TRUE(pty.ok());

  LinuxGpsStream s;
  ASSERT_TRUE(s.begin(pty.path(), 9600));
  EXPECT_EQ(s.transport(), LinuxGpsStream::SERIAL_DEVICE);
  EXPECT_TRUE(s.isPresent());

  pty.send("$GPGGA,x\r\n");
  EXPECT_EQ(readSerialWithin(s), '$');
}

// The serial-path counterpart of GpsdTransport.SocketIsCloseOnExec.
TEST(SerialGpsStream, DeviceIsCloseOnExec) {
  FakePtyGps pty;
  ASSERT_TRUE(pty.ok());

  TestGpsStream s;
  ASSERT_TRUE(s.begin(pty.path(), 9600));

  ASSERT_GE(s.fd(), 0);
  EXPECT_TRUE(fcntl(s.fd(), F_GETFD) & FD_CLOEXEC);
}

// baud_to_speed() is a private implementation detail; verify indirectly by
// reading back the termios settings the tty ends up with. A pty's line
// discipline state is shared by both ends, so what LinuxGpsStream configured
// through the slave path is visible via the master fd this test still holds.
TEST(SerialGpsStream, BaudIsAppliedToTheDevice) {
  FakePtyGps pty;
  ASSERT_TRUE(pty.ok());

  LinuxGpsStream s;
  ASSERT_TRUE(s.begin(pty.path(), 19200));

  struct termios tio;
  ASSERT_EQ(tcgetattr(pty.masterFd(), &tio), 0);
  EXPECT_EQ(cfgetispeed(&tio), (speed_t)B19200);
  EXPECT_EQ(cfgetospeed(&tio), (speed_t)B19200);
}

// The serial-path analogue of GpsdReconnect.ReconnectsAfterReadGap: `gps off`
// stops EnvironmentSensorManager from draining the stream, so bytes queue in
// the tty's kernel input buffer while nothing reads them. `gps on` must not
// replay that backlog as though it were current (finding 4).
TEST(SerialGpsStream, StaleBacklogFlushedAfterReadGap) {
  FakePtyGps pty;
  ASSERT_TRUE(pty.ok());

  LinuxGpsStream s;
  ASSERT_TRUE(s.begin(pty.path(), 9600));

  // Establish a first successful read so _last_read_ms is seeded -- the gap
  // check is a no-op until then.
  pty.send("$A\r\n");
  ASSERT_EQ(readSerialWithin(s), '$');
  for (int i = 0; i < 3; i++) readSerialWithin(s);   // drain "A\r\n"

  // Simulate the backlog that piles up while `gps off` stops the drain.
  pty.send("$STALE,should,not,appear\r\n");
  usleep(20000);   // let the kernel queue it on the slave side before the gap

  g_mock_millis += 10000;   // longer than the 5 s gap threshold

  // The read that resumes after the gap must flush the backlog rather than
  // hand any of it back.
  EXPECT_EQ(s.read(), -1);

  // Fresh data written after the flush is still delivered normally.
  pty.send("$FRESH\r\n");
  EXPECT_EQ(readSerialWithin(s), '$');
}

// Negative case: a pause shorter than the threshold is normal poll-loop
// jitter, not a `gps off`, and must not discard data that is simply waiting
// to be read.
TEST(SerialGpsStream, ShortReadGapDoesNotFlush) {
  FakePtyGps pty;
  ASSERT_TRUE(pty.ok());

  LinuxGpsStream s;
  ASSERT_TRUE(s.begin(pty.path(), 9600));

  pty.send("$A\r\n");
  ASSERT_EQ(readSerialWithin(s), '$');
  for (int i = 0; i < 3; i++) readSerialWithin(s);   // drain "A\r\n"

  pty.send("$B\r\n");
  usleep(20000);

  g_mock_millis += 2000;   // well under the 5 s gap threshold

  // Retried rather than read once: 20 ms is not a guarantee that the pty has
  // handed the bytes over on a contended runner. The assertion still tells the
  // two cases apart, because the flush case above asserts -1 on the *first*
  // read -- readSerialWithin() would never turn a flushed queue into a '$'.
  EXPECT_EQ(readSerialWithin(s), '$') << "a short gap must not flush pending data";
}

}  // namespace

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
