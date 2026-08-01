#include <gtest/gtest.h>

#include <string>

#include "LinuxGpsStream.h"

namespace {

using Target = LinuxGpsStream::Target;

TEST(ParseDevice, EmptyIsNoTransportButValid) {
  Target t = LinuxGpsStream::parseDevice("");
  EXPECT_TRUE(t.valid);
  EXPECT_EQ(t.transport, LinuxGpsStream::NONE);
}

TEST(ParseDevice, NullIsNoTransportButValid) {
  Target t = LinuxGpsStream::parseDevice(nullptr);
  EXPECT_TRUE(t.valid);
  EXPECT_EQ(t.transport, LinuxGpsStream::NONE);
}

TEST(ParseDevice, DevicePathIsSerial) {
  Target t = LinuxGpsStream::parseDevice("/dev/ttyS0");
  EXPECT_TRUE(t.valid);
  EXPECT_EQ(t.transport, LinuxGpsStream::SERIAL);
}

TEST(ParseDevice, BareSchemeUsesDefaults) {
  Target t = LinuxGpsStream::parseDevice("gpsd://");
  ASSERT_TRUE(t.valid);
  EXPECT_EQ(t.transport, LinuxGpsStream::GPSD);
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

}  // namespace

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
