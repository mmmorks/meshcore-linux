#pragma once
#include <Stream.h>

// Arduino Stream over a non-blocking byte source.
//
// available(), peek() and read() all have to answer "is there a byte?" without
// consuming it, but a non-blocking descriptor only answers that by reading. One
// byte of lookahead bridges the two, and it is the same bridge for every such
// source -- so subclasses supply only rawReadByte() and keep the buffering,
// which is easy to get subtly inconsistent, in one place.
class PeekableStream : public Stream {
public:
  int available() override { return fill() >= 0 ? 1 : 0; }
  int peek() override      { return fill(); }

  int read() override {
    int c = fill();
    _peek = -1;
    return c;
  }

  using Print::write;

protected:
  // The next byte from the underlying source, or -1 if none is available right
  // now. Must not block.
  virtual int rawReadByte() = 0;

  // Discard any buffered lookahead. Call when the source is closed or replaced,
  // so a byte read from the old one cannot surface from the new.
  void clearPeek() { _peek = -1; }

private:
  int fill() {
    if (_peek < 0) _peek = rawReadByte();
    return _peek;
  }

  int _peek = -1;   // one-byte lookahead, or -1
};
