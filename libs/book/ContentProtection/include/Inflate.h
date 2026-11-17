#pragma once

// FreeInk — the inflate stage of the encrypted-entry read path, injectable.
//
// ProtectedBook inflates two things: an encrypted entry's plaintext when it is deflated, and a
// deflated META-INF/encryption.xml. It asks an Inflater for that instead of calling a library
// itself, so a firmware can run it on the inflate it already links: a second deflate library in
// the image costs flash.
//
// The interface pulls. The inflater asks for the next piece of compressed input when it needs
// one, which suits a decompressor that cannot stop in the middle of a block and wait to be fed
// (uzlib), and miniz just as well. The pieces are handed out, not copied: the read path's own
// buffers are all the memory input needs.
//
// MinizInflater (MinizInflater.h) binds miniz. ProtectedBook uses it when no inflater is set,
// unless the embedding build defines CONTENT_NO_DEFAULT_INFLATER, which keeps miniz out of the
// image.
//
// Freestanding C++17.

#include <stddef.h>
#include <stdint.h>

namespace freeink {
namespace content {

// The next piece of compressed input: points *data at it and returns its size, 0 at the end of
// the input, or a negative value on error. The piece stays valid until the next call.
using InflateInput = int32_t (*)(void* context, const uint8_t** data);

class Inflater {
 public:
  virtual ~Inflater() = default;
  // Starts a raw DEFLATE stream (no zlib header) whose input comes from input(context).
  // expectedSize is the plaintext size when known, 0 when not: an inflater may size its window by
  // it, since no back-reference reaches further than the bytes produced so far. False when its
  // state cannot be allocated.
  virtual bool begin(InflateInput input, void* context, uint32_t expectedSize) = 0;
  // Writes up to cap plaintext bytes to out: returns the count, 0 once the stream has ended, or a
  // negative value on corrupt data or an input error.
  virtual int32_t read(uint8_t* out, uint32_t cap) = 0;
  // Frees what begin() took. Safe after a failed begin(), and more than once.
  virtual void end() = 0;
};

}  // namespace content
}  // namespace freeink
