#pragma once

#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include "WString.h"

using oflag_t = int;
constexpr oflag_t O_RDONLY = 0, O_RDWR = 1, O_CREAT = 2, O_TRUNC = 4;
using Sector_t = uint32_t;
class FsBlockDeviceInterface {
 public:
  virtual ~FsBlockDeviceInterface() = default;
  virtual void end() {}
  virtual bool isBusy() { return false; }
  virtual bool readSector(Sector_t, uint8_t*) { return false; }
  virtual bool readSectors(Sector_t, uint8_t*, size_t) { return false; }
  virtual bool writeSector(Sector_t, const uint8_t*) { return false; }
  virtual bool writeSectors(Sector_t, const uint8_t*, size_t) { return false; }
  virtual Sector_t sectorCount() { return 1024; }
  virtual bool syncDevice() { return true; }
};

namespace fakeSd {
inline std::vector<uint8_t> data;
inline uint64_t advertisedSize = 0;
inline bool openFails = false;
inline size_t failAt = std::numeric_limits<size_t>::max();
inline size_t maxRead = std::numeric_limits<size_t>::max();
inline size_t largestRequest = 0;
inline unsigned reads = 0, closes = 0;
inline uint32_t readMs = 0;
inline int32_t freeClusters = 1;
inline uint32_t clusters = 1;
inline unsigned clusterScans = 0;
inline bool rootOpenFails = false, rootSyncFails = false, rootCloseFails = false;
inline unsigned rootSyncs = 0, volumeEnds = 0;
// A directory tree for removeDir(): each directory lists its entries in order, keyed by its path without a trailing
// slash. Removals are only recorded, so iteration positions stay where SdFat would keep them.
struct Entry {
  std::string name;
  bool directory;
};
inline std::map<std::string, std::vector<Entry>> directories;
inline std::vector<std::string> openedDirectories, removedFiles, removedDirectories;
inline size_t directoryOpenLimit = std::numeric_limits<size_t>::max();
inline std::string withoutTrailingSlash(const char* path) {
  std::string key(path);
  while (key.size() > 1 && key.back() == '/') key.pop_back();
  return key;
}
}  // namespace fakeSd

class FsVolume;
class FsFile {
 public:
  explicit FsFile(bool open = false) : open_(open) {}
  explicit operator bool() const { return open_; }
  bool openRoot(FsVolume*) {
    root_ = true;
    open_ = !fakeSd::rootOpenFails;
    return open_;
  }
  bool sync() {
    ++fakeSd::rootSyncs;
    return open_ && !fakeSd::rootSyncFails;
  }
  uint64_t fileSize() const { return fakeSd::advertisedSize; }
  bool isDirectory() const { return directory_; }
  bool close() {
    if (open_) ++fakeSd::closes;
    open_ = false;
    return !(root_ && fakeSd::rootCloseFails);
  }
  int available() const { return cursor_ < fakeSd::data.size(); }
  int read(void* destination, size_t count) {
    ++fakeSd::reads;
    nowMs += fakeSd::readMs;
    fakeSd::largestRequest = std::max(fakeSd::largestRequest, count);
    if (cursor_ >= fakeSd::failAt) return -1;
    const size_t accepted = std::min({count, fakeSd::data.size() - cursor_, fakeSd::maxRead});
    if (accepted != 0) std::memcpy(destination, fakeSd::data.data() + cursor_, accepted);
    cursor_ += accepted;
    return static_cast<int>(accepted);
  }
  int read() {
    uint8_t byte;
    return read(&byte, 1) == 1 ? byte : -1;
  }
  FsFile openNextFile() {
    if (!directory_) return FsFile();
    const auto& entries = fakeSd::directories[path_];
    if (next_ >= entries.size()) return FsFile();
    const auto& entry = entries[next_++];
    FsFile file(true);
    file.name_ = entry.name;
    file.directory_ = entry.directory;
    return file;
  }
  // SdFat's contract: a name that does not fit (with its NUL) comes back as "" and 0.
  size_t getName(char* name, size_t size) const {
    if (name_.size() + 1 > size) {
      if (size) name[0] = '\0';
      return 0;
    }
    std::memcpy(name, name_.c_str(), name_.size() + 1);
    return name_.size();
  }
  size_t print(const String& text) { return text.size(); }

 private:
  friend class FsVolume;
  bool open_;
  size_t cursor_ = 0;
  bool root_ = false;
  bool directory_ = false;
  std::string path_, name_;
  size_t next_ = 0;
};

class FsVolume {
 public:
  bool begin(FsBlockDeviceInterface*) { return true; }
  void end() { ++fakeSd::volumeEnds; }
  FsFile open(const char* path, oflag_t = O_RDONLY) {
    if (fakeSd::openFails) return FsFile(false);
    const auto key = fakeSd::withoutTrailingSlash(path);
    if (fakeSd::directories.count(key) == 0) return FsFile(true);
    fakeSd::openedDirectories.push_back(key);
    // Bounds a runaway recursion so a test can fail on it instead of overflowing the stack.
    if (fakeSd::openedDirectories.size() > fakeSd::directoryOpenLimit) return FsFile(false);
    FsFile dir(true);
    dir.directory_ = true;
    dir.path_ = key;
    return dir;
  }
  bool exists(const char*) const { return false; }
  bool mkdir(const char*, bool = true) { return true; }
  bool remove(const char* path) {
    if (fakeSd::directories.count(fakeSd::withoutTrailingSlash(path))) return false;  // SdFat removes files only
    fakeSd::removedFiles.emplace_back(path);
    return true;
  }
  bool rmdir(const char* path) {
    fakeSd::removedDirectories.emplace_back(path);
    return true;
  }
  bool rename(const char*, const char*) { return true; }
  uint32_t clusterCount() const { return fakeSd::clusters; }
  uint32_t bytesPerCluster() const { return 512; }
  int32_t freeClusterCount() const {
    ++fakeSd::clusterScans;
    return fakeSd::freeClusters;
  }
};

class SdFat : public FsVolume {
 public:
  FsBlockDeviceInterface* card() { return &card_; }
  bool begin(uint8_t, uint32_t) { return true; }
  void end() {}
  int sdErrorCode() const { return 0; }
  int sdErrorData() const { return 0; }

 private:
  FsBlockDeviceInterface card_;
};
