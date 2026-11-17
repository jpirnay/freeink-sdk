#include "ProtectedBook.h"

#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <memory>
#include <new>
#include <utility>

#include "Util.h"

#if !defined(CONTENT_NO_DEFAULT_INFLATER)
#include "MinizInflater.h"
#endif

#if defined(ESP_PLATFORM) || defined(ARDUINO_ARCH_ESP32)
#include <sdkconfig.h>
#endif

namespace freeink {
namespace content {

namespace {
constexpr const char* kEncryptionXml = "META-INF/encryption.xml";

bool tagHas(const char* tag, size_t len, const char* token) {
  const size_t tlen = strlen(token);
  if (tlen > len) return false;
  for (size_t i = 0; i + tlen <= len; i++) {
    if (memcmp(tag + i, token, tlen) == 0) return true;
  }
  return false;
}

// Extract one attribute value from a raw tag slice ('<' .. '>'). Requires
// whitespace before the name so URI= cannot match inside another attribute.
// These manifests are machine-generated with quoted attributes; both quote
// styles accepted.
bool tagAttr(const char* tag, size_t len, const char* attr, std::string* out) {
  const size_t alen = strlen(attr);
  for (size_t i = 1; i + alen + 2 < len; i++) {
    if (memcmp(tag + i, attr, alen) != 0) continue;
    const char before = tag[i - 1];
    if (before != ' ' && before != '\t' && before != '\r' && before != '\n') continue;
    size_t pos = i + alen;
    const auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (pos < len && space(tag[pos])) ++pos;
    if (pos == len || tag[pos++] != '=') continue;
    while (pos < len && space(tag[pos])) ++pos;
    if (pos == len) return false;
    const char quote = tag[pos++];
    if (quote != '"' && quote != '\'') continue;
    const char* start = tag + pos;
    const char* end = static_cast<const char*>(memchr(start, quote, len - (start - tag)));
    if (!end) return false;
    out->assign(start, static_cast<size_t>(end - start));
    return true;
  }
  return false;
}

// An encrypted entry's plaintext, decrypted one CBC chunk at a time into the read path's own
// buffers: the input of the inflate stage, or what a STORED entry hands to the sink as it is.
struct EntryDecryptor {
  ByteSource& source;
  Crypto& crypto;
  const uint8_t* key;
  bool aes256;
  size_t chunk;
  uint64_t offset;
  uint32_t remaining;
  uint8_t iv[16];
  uint8_t* cipher = nullptr;
  uint8_t* plain = nullptr;
  const char* error = nullptr;

  // Decrypts the next chunk into `plain`: returns its plaintext size (the last one without its
  // padding), 0 at the end, or -1 with `error` set.
  int32_t next() {
    if (remaining == 0) return 0;
    const size_t amount = remaining < chunk ? remaining : chunk;
    if (source.readAt(offset, cipher, amount) != static_cast<int32_t>(amount)) {
      error = "entry read failed";
      return -1;
    }
    uint8_t nextIv[16];
    memcpy(nextIv, cipher + amount - sizeof(nextIv), sizeof(nextIv));
    const bool decrypted = aes256 ? crypto.aes256CbcDecrypt(key, iv, cipher, amount, plain)
                                  : crypto.aes128CbcDecrypt(key, iv, cipher, amount, plain);
    if (!decrypted) {
      error = "content read failed";
      return -1;
    }
    memcpy(iv, nextIv, sizeof(iv));

    size_t plainSize = amount;
    if (amount == remaining) {
      const uint8_t pad = plain[plainSize - 1];
      if (pad >= 1 && pad <= 16 && pad <= plainSize) {
        if (aes256) {
          // W3C xmlenc padding (LCP): the fill bytes are random; only the
          // count byte is meaningful.
          plainSize -= pad;
        } else {
          bool valid = true;
          for (size_t i = plainSize - pad; i < plainSize; i++) valid = valid && plain[i] == pad;
          if (valid) plainSize -= pad;
        }
      }
    }
    offset += amount;
    remaining -= static_cast<uint32_t>(amount);
    return static_cast<int32_t>(plainSize);
  }

  static int32_t input(void* context, const uint8_t** data) {
    auto& self = *static_cast<EntryDecryptor*>(context);
    *data = self.plain;
    return self.next();
  }
};

// A zip entry's raw bytes, read a chunk at a time into the caller's buffer.
struct RawEntryInput {
  ByteSource& source;
  uint8_t* buffer;
  size_t chunk;
  uint64_t at;
  uint32_t remaining;

  static int32_t input(void* context, const uint8_t** data) {
    auto& self = *static_cast<RawEntryInput*>(context);
    if (self.remaining == 0) return 0;
    const size_t amount = self.remaining < self.chunk ? self.remaining : self.chunk;
    if (self.source.readAt(self.at, self.buffer, amount) != static_cast<int32_t>(amount)) return -1;
    self.at += amount;
    self.remaining -= static_cast<uint32_t>(amount);
    *data = self.buffer;
    return static_cast<int32_t>(amount);
  }
};

// Ends an inflate stream on every way out of a read.
struct InflateGuard {
  Inflater* inflater = nullptr;
  ~InflateGuard() {
    if (inflater) inflater->end();
  }
};
}  // namespace

void ProtectedBook::reserveErrorBuffer() {
  if (heapProbe(96)) lastError_.reserve(64);
  lastError_.clear();
}

bool ProtectedBook::open(ByteSource& source) {
  reserveErrorBuffer();
  protected_ = false;
  index_ = nullptr;

  if (!zip_.open(source)) {
    lastError_ = "not a zip container";
    return false;
  }

  return finishOpen(source);
}

bool ProtectedBook::openFromScan(ByteSource& source, ZipScan&& scan) {
  reserveErrorBuffer();
  protected_ = false;
  index_ = nullptr;
  zip_ = std::move(scan);
  return finishOpen(source);
}

bool ProtectedBook::openWithIndex(ByteSource& source, EntryIndex& index) {
  reserveErrorBuffer();
  protected_ = false;
  zip_ = ZipScan{};  // nothing per entry held here
  index_ = &index;
  return finishOpen(source);
}

bool ProtectedBook::findEntry(const std::string& name, ZipEntryInfo* out) const {
  if (index_) return index_->find(name, out);
  const ZipEntryInfo* entry = zip_.find(name);
  if (!entry) return false;
  *out = *entry;
  return true;
}

bool ProtectedBook::entryDataOffset(ByteSource& source, const ZipEntryInfo& entry, uint64_t* out) const {
  return index_ ? index_->dataOffset(entry, out) : zip_.dataOffset(source, entry, out);
}

bool ProtectedBook::finishOpen(ByteSource& source) {
  ZipEntryInfo encEntry;
  if (!findEntry(kEncryptionXml, &encEntry)) {
    return true;  // plain EPUB; nothing to do
  }

  // Stream-parse the manifest straight out of the zip: it enumerates every
  // encrypted resource, so its size scales with the container's file count
  // (measured ~60KB for a many-hundred-entry book) and materializing it
  // whole was OOM-aborting small-heap devices at open.
  if (!scanEncryptionXml(source, encEntry)) {
    if (lastError_.empty()) lastError_ = "failed to read encryption.xml";
    return false;
  }

  // A manifest containing only embedded-font obfuscation needs no content
  // key or alternate read path.
  protected_ = !encryptedUriHashes_.empty();
  return true;
}

void ProtectedBook::setContentKey(const uint8_t* key, size_t len) {
  if (len != 16 && len != 32) return;
  memcpy(bookKey_, key, len);
  keyLen_ = len;
  hasKey_ = true;
}

bool ProtectedBook::isEncrypted(const std::string& name) const {
  return std::binary_search(encryptedUriHashes_.begin(), encryptedUriHashes_.end(), fnv1a64(name.data(), name.size()));
}

size_t ProtectedBook::decryptedSize(const std::string& name) const {
  // The zip records the stored blob (LCP: IV + ciphertext + padding), not the
  // plaintext; encryption.xml's OriginalLength is the real size when given.
  const uint64_t hash = fnv1a64(name.data(), name.size());
  const auto it = std::lower_bound(originalSizes_.begin(), originalSizes_.end(), std::make_pair(hash, uint32_t{0}));
  if (it != originalSizes_.end() && it->first == hash) return it->second;
  ZipEntryInfo entry;
  return findEntry(name, &entry) ? entry.uncompressedSize : 0;
}

bool ProtectedBook::decryptEntryToSink(ByteSource& source, Crypto& crypto, const std::string& name,
                                       ContentChunkSink sink, void* context) {
  if (!hasKey_ || keyLen_ != (aes256_ ? 32u : 16u)) {
    lastError_ = "no content key";
    return false;
  }
  ZipEntryInfo entry;
  if (!findEntry(name, &entry)) {
    // The concat allocates a temporary; fall back to the SSO-sized literal
    // (never allocates) when the heap cannot take it.
    if (heapProbe(name.size() + 48)) {
      lastError_ = "entry not found: " + name;
    } else {
      lastError_ = "entry not found";
    }
    return false;
  }
  if (entry.compressedSize < 32 || (entry.compressedSize - 16) % 16 != 0) {
    lastError_ = "entry size not AES-shaped";
    return false;
  }

  uint64_t offset = 0;
  if (!entryDataOffset(source, entry, &offset)) {
    lastError_ = "entry offset unavailable";
    return false;
  }

#if defined(CONFIG_IDF_TARGET_ESP32C3) && CONFIG_IDF_TARGET_ESP32C3
  constexpr size_t kCipherChunk = 512;
  constexpr size_t kOutputChunk = 1024;
#else
  constexpr size_t kCipherChunk = 2048;
  constexpr size_t kOutputChunk = 4096;
#endif
  static_assert(kCipherChunk % 16 == 0, "CBC chunks must contain whole AES blocks");

  EntryDecryptor decryptor{source, crypto, bookKey_, aes256_, kCipherChunk, offset + 16, entry.compressedSize - 16, {}};
  if (source.readAt(offset, decryptor.iv, sizeof(decryptor.iv)) != sizeof(decryptor.iv)) {
    lastError_ = "entry IV read failed";
    return false;
  }

  // LCP marks already-uncompressed resources Compression Method="0":
  // decrypt-only, no inflate stage.
  const bool stored =
      std::binary_search(storedUriHashes_.begin(), storedUriHashes_.end(), fnv1a64(name.data(), name.size()));

#if !defined(CONTENT_NO_DEFAULT_INFLATER)
  MinizInflater fallback;
  Inflater* inflater = inflater_ ? inflater_ : &fallback;
#else
  Inflater* inflater = inflater_;
#endif
  InflateGuard guard;
  if (!stored) {
    if (!inflater) {
      lastError_ = "no inflater for a deflated entry";
      return false;
    }
    // Start the inflate stage before the smaller streaming buffers: its state is the largest
    // contiguous block.
    guard.inflater = inflater;
    if (!inflater->begin(&EntryDecryptor::input, &decryptor, static_cast<uint32_t>(decryptedSize(name)))) {
      lastError_ = "inflate setup failed";
      return false;
    }
  }

  auto* buffers = static_cast<uint8_t*>(malloc(kCipherChunk * 2 + kOutputChunk));
  if (!buffers) {
    lastError_ = "insufficient memory for content stream";
    return false;
  }
  decryptor.cipher = buffers;
  decryptor.plain = buffers + kCipherChunk;
  uint8_t* output = decryptor.plain + kCipherChunk;

  bool ok = true;
  for (;;) {
    int32_t size;
    const uint8_t* data;
    if (stored) {
      size = decryptor.next();
      data = decryptor.plain;
    } else {
      size = inflater->read(output, kOutputChunk);
      data = output;
    }
    if (size == 0) break;
    if (size < 0) {
      ok = false;
      lastError_ = decryptor.error ? decryptor.error : "inflate failed";
      break;
    }
    if (!sink || !sink(context, data, static_cast<size_t>(size))) {
      ok = false;
      lastError_ = stored ? "content sink failed" : "inflate failed";
      break;
    }
  }
  free(buffers);
  return ok;
}

bool ProtectedBook::scanEncryptionXml(ByteSource& source, const ZipEntryInfo& entry) {
  encryptedUriHashes_.clear();
  storedUriHashes_.clear();
  originalSizes_.clear();
  aes256_ = false;

  uint64_t at = 0;
  if (!entryDataOffset(source, entry, &at)) {
    lastError_ = "entry offset unavailable";
    return false;
  }
  if (entry.method != 0 && entry.method != 8) return false;

#if defined(CONFIG_IDF_TARGET_ESP32C3) && CONFIG_IDF_TARGET_ESP32C3
  constexpr size_t kChunk = 512;
#else
  constexpr size_t kChunk = 2048;
#endif
  RawEntryInput raw{source, nullptr, kChunk, at, entry.compressedSize};

#if !defined(CONTENT_NO_DEFAULT_INFLATER)
  MinizInflater fallback;
  Inflater* inflater = inflater_ ? inflater_ : &fallback;
#else
  Inflater* inflater = inflater_;
#endif
  InflateGuard guard;
  // Start the inflate stage before smaller parser allocations: its state is the largest
  // contiguous block.
  if (entry.method == 8) {
    if (!inflater) {
      lastError_ = "no inflater for a deflated encryption.xml";
      return false;
    }
    guard.inflater = inflater;
    if (!inflater->begin(&RawEntryInput::input, &raw, entry.uncompressedSize)) {
      lastError_ = "manifest inflate init failed";
      return false;
    }
  }
  auto* bufs = static_cast<uint8_t*>(malloc(kChunk * 2));  // freed below; kept raw for the single free
  if (!bufs) {
    lastError_ = "manifest scratch out of memory";
    return false;
  }
  raw.buffer = bufs;
  uint8_t* outBuf = bufs + kChunk;

  // Tag-level scan. Inter-tag text is discarded; only element attributes
  // matter here. `carry` holds an unterminated tag across chunk boundaries —
  // manifest tags run ~200 bytes, so a tag that never closes within the cap
  // is a malformed document, not a real split.
  std::string carry;
  int pendingCipher = 0;  // bits of the EncryptionMethod just seen; 0 = unsupported
  uint64_t lastHash = 0;
  bool haveLast = false;
  bool malformed = false;
  std::string value;
  // Probe each reserve separately: these buffers need not share a contiguous block.
  {
    constexpr size_t kCarryReserve = 4096 + kChunk;
    constexpr size_t kValueReserve = 1024;  // attribute scratch (URIs)
    constexpr size_t kUriReserve = 256;     // encrypted-entry hashes (8B each)
    const auto canReserve = [this](size_t bytes, const char* error) {
      constexpr size_t kAllocationHeadroom = 64;
      if (heapProbe(bytes + kAllocationHeadroom)) return true;
      lastError_ = error;
      return false;
    };
    if (carry.capacity() < kCarryReserve && !canReserve(kCarryReserve + 1, "manifest carry out of memory")) {
      free(bufs);
      return false;
    }
    carry.reserve(kCarryReserve);
    if (value.capacity() < kValueReserve && !canReserve(kValueReserve + 1, "manifest attribute out of memory")) {
      free(bufs);
      return false;
    }
    value.reserve(kValueReserve);
    if (encryptedUriHashes_.capacity() < kUriReserve &&
        !canReserve(kUriReserve * sizeof(uint64_t), "manifest hashes out of memory")) {
      free(bufs);
      return false;
    }
    encryptedUriHashes_.reserve(kUriReserve);
    if (originalSizes_.capacity() < kUriReserve &&
        !canReserve(kUriReserve * sizeof(std::pair<uint64_t, uint32_t>), "manifest sizes out of memory")) {
      free(bufs);
      return false;
    }
    originalSizes_.reserve(kUriReserve);
    constexpr size_t kStoredReserve = 64;
    if (storedUriHashes_.capacity() < kStoredReserve &&
        !canReserve(kStoredReserve * sizeof(uint64_t), "manifest stored hashes out of memory")) {
      free(bufs);
      return false;
    }
    storedUriHashes_.reserve(kStoredReserve);
  }
  auto handleTag = [&](const char* tag, size_t len) {
    if (len < 3 || tag[1] == '/' || tag[1] == '?' || tag[1] == '!') return;
    if (tagHas(tag, len, "EncryptionMethod")) {
      pendingCipher = 0;
      if (tagAttr(tag, len, "Algorithm", &value)) {
        if (value.find("aes128-cbc") != std::string::npos) pendingCipher = 128;
        if (value.find("aes256-cbc") != std::string::npos) pendingCipher = 256;
      }
    } else if (tagHas(tag, len, "CipherReference")) {
      if (pendingCipher != 0 && tagAttr(tag, len, "URI", &value) && !value.empty()) {
        lastHash = fnv1a64(value.data(), value.size());
        haveLast = true;
        encryptedUriHashes_.push_back(lastHash);
        if (pendingCipher == 256) aes256_ = true;
      }
      pendingCipher = 0;
    } else if (tagHas(tag, len, "Compression")) {
      // Per-entry compression property (LCP): Method="0" means the plaintext
      // is not deflated. Follows the entry's CipherReference in the manifest.
      if (haveLast && tagAttr(tag, len, "Method", &value) && value == "0") {
        storedUriHashes_.push_back(lastHash);
      }
      if (haveLast && tagAttr(tag, len, "OriginalLength", &value)) {
        char* end = nullptr;
        const unsigned long n = strtoul(value.c_str(), &end, 10);
        if (end != value.c_str() && *end == '\0' && n <= UINT32_MAX) {
          originalSizes_.emplace_back(lastHash, static_cast<uint32_t>(n));
        }
      }
    }
  };
  auto feed = [&](const uint8_t* data, size_t size) -> bool {
    carry.append(reinterpret_cast<const char*>(data), size);
    size_t pos = 0;
    for (;;) {
      const size_t lt = carry.find('<', pos);
      if (lt == std::string::npos) {
        carry.clear();
        return true;
      }
      const size_t gt = carry.find('>', lt);
      if (gt == std::string::npos) {
        carry.erase(0, lt);
        if (carry.size() > 4096) {
          malformed = true;
          return false;
        }
        return true;
      }
      handleTag(carry.data() + lt, gt - lt + 1);
      pos = gt + 1;
    }
  };

  bool ok = true;
  for (;;) {
    const uint8_t* data = outBuf;
    const int32_t size = entry.method == 0 ? RawEntryInput::input(&raw, &data) : inflater->read(outBuf, kChunk);
    if (size == 0) break;
    if (size < 0 || !feed(data, static_cast<size_t>(size))) {
      ok = false;
      break;
    }
  }
  free(bufs);

  if (!ok || malformed) {
    if (lastError_.empty()) lastError_ = "encryption manifest unreadable";
    return false;
  }
  std::sort(encryptedUriHashes_.begin(), encryptedUriHashes_.end());
  std::sort(storedUriHashes_.begin(), storedUriHashes_.end());
  std::sort(originalSizes_.begin(), originalSizes_.end());
  return true;
}

}  // namespace content
}  // namespace freeink
