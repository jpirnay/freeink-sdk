#include "MinizInflater.h"

#include <string.h>

#include <new>

#include "ContentMinizConfig.h"

namespace freeink {
namespace content {

namespace {
mz_stream* asStream(unsigned char* storage) { return reinterpret_cast<mz_stream*>(storage); }
}  // namespace

bool MinizInflater::begin(InflateInput input, void* context, uint32_t /*expectedSize*/) {
  end();
  static_assert(sizeof(mz_stream) <= sizeof(stream_), "MinizInflater::stream_ is too small for mz_stream");
  mz_stream* stream = new (stream_) mz_stream;
  memset(stream, 0, sizeof(*stream));
  if (mz_inflateInit2(stream, -15) != MZ_OK) return false;
  open_ = true;
  input_ = input;
  context_ = context;
  inputEnded_ = false;
  flushed_ = false;
  ended_ = false;
  return true;
}

int32_t MinizInflater::read(uint8_t* out, uint32_t cap) {
  if (!open_) return -1;
  if (ended_ || cap == 0) return 0;
  mz_stream* stream = asStream(stream_);
  stream->next_out = out;
  stream->avail_out = cap;
  for (;;) {
    if (stream->avail_in == 0 && !inputEnded_) {
      const uint8_t* data = nullptr;
      const int32_t size = input_(context_, &data);
      if (size < 0) return -1;
      if (size == 0) {
        inputEnded_ = true;
      } else {
        stream->next_in = data;
        stream->avail_in = static_cast<unsigned int>(size);
      }
    }
    if (stream->avail_in == 0 && inputEnded_) {
      const uint32_t produced = cap - stream->avail_out;
      if (flushed_) return produced > 0 ? static_cast<int32_t>(produced) : -1;  // the stream never ended
      // miniz reports the end of a stream whose last block ends exactly at the input's end only
      // once it is handed one more byte. The read path has always fed it a dummy one.
      static const uint8_t kTrailing = 'Z';
      stream->next_in = &kTrailing;
      stream->avail_in = 1;
      flushed_ = true;
    }
    const unsigned int inBefore = stream->avail_in;
    const unsigned int outBefore = stream->avail_out;
    const int status = mz_inflate(stream, MZ_NO_FLUSH);
    const uint32_t produced = cap - stream->avail_out;
    if (status == MZ_STREAM_END) {
      ended_ = true;
      return static_cast<int32_t>(produced);
    }
    if (status != MZ_OK && status != MZ_BUF_ERROR) return -1;
    if (produced == cap) return static_cast<int32_t>(produced);
    // No progress with input left would loop for ever: corrupt data.
    if (stream->avail_in == inBefore && stream->avail_out == outBefore && stream->avail_in > 0) return -1;
  }
}

void MinizInflater::end() {
  if (!open_) return;
  mz_inflateEnd(asStream(stream_));
  open_ = false;
}

}  // namespace content
}  // namespace freeink
