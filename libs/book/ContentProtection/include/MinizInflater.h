#pragma once

// FreeInk — the Inflater on miniz (ContentMinizConfig.h: the host application's miniz, or the
// vendored copy). ProtectedBook's default unless CONTENT_NO_DEFAULT_INFLATER is defined.
//
// Freestanding C++17.

#include "Inflate.h"

namespace freeink {
namespace content {

class MinizInflater : public Inflater {
 public:
  MinizInflater() = default;
  ~MinizInflater() override { end(); }
  MinizInflater(const MinizInflater&) = delete;
  MinizInflater& operator=(const MinizInflater&) = delete;

  bool begin(InflateInput input, void* context, uint32_t expectedSize) override;
  int32_t read(uint8_t* out, uint32_t cap) override;
  void end() override;

 private:
  // The mz_stream, in place (no allocation beyond miniz's own state) and opaque, so this header
  // does not pull miniz into every includer. MinizInflater.cpp checks that it fits.
  alignas(8) unsigned char stream_[160] = {};
  bool open_ = false;
  InflateInput input_ = nullptr;
  void* context_ = nullptr;
  bool inputEnded_ = false;
  bool flushed_ = false;
  bool ended_ = false;
};

}  // namespace content
}  // namespace freeink
