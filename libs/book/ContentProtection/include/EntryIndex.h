#pragma once

// FreeInk — where ProtectedBook finds a container's entries, injectable.
//
// ProtectedBook::open() scans the central directory itself (ZipScan), which keeps every entry in
// memory, 24 bytes each: a container with thousands of entries costs tens of KB, which a small
// device may not have. A firmware whose own zip reader already finds entries can answer for them
// instead (ProtectedBook::openWithIndex()), and nothing per entry is held here.
//
// Freestanding C++17.

#include <stdint.h>

#include <string>

#include "Zip.h"

namespace freeink {
namespace content {

class EntryIndex {
 public:
  virtual ~EntryIndex() = default;
  // The entry named `name` (a container-relative path): its sizes, method and local-header
  // offset. nameHash may be left 0. False when there is none.
  virtual bool find(const std::string& name, ZipEntryInfo* out) = 0;
  // The offset of the entry's raw data, past its local header.
  virtual bool dataOffset(const ZipEntryInfo& entry, uint64_t* out) = 0;
};

}  // namespace content
}  // namespace freeink
