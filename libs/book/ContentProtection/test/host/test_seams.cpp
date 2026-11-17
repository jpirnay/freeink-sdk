// ProtectedBook's seams: an injected entry index, so nothing per entry is scanned or held, and an
// injected pull inflater, so a firmware runs the read path on the inflate it already links.
//
// Built twice by run.sh: with the default inflater (miniz linked), and with
// CONTENT_NO_DEFAULT_INFLATER and no miniz in the link at all, which proves the read path then
// references none of it.
#define main protectedFixtureMain
#include "test_protected.cpp"
#undef main

#include <EntryIndex.h>
#include <Inflate.h>
#if !defined(CONTENT_NO_DEFAULT_INFLATER)
#include <MinizInflater.h>
#endif

using freeink::content::ByteSource;
using freeink::content::EntryIndex;
using freeink::content::InflateInput;
using freeink::content::Inflater;
using freeink::content::ZipEntryInfo;
using freeink::content::ZipScan;

namespace {

// An index over a scan of the fixture, counting its use: what a firmware's zip reader answers.
struct CountingIndex : EntryIndex {
  ZipScan scan;
  ByteSource* source = nullptr;
  int finds = 0;
  int offsets = 0;
  bool find(const std::string& name, ZipEntryInfo* out) override {
    ++finds;
    const ZipEntryInfo* entry = scan.find(name);
    if (!entry) return false;
    *out = *entry;
    return true;
  }
  bool dataOffset(const ZipEntryInfo& entry, uint64_t* out) override {
    ++offsets;
    return scan.dataOffset(*source, entry, out);
  }
};

// A pull inflater that is not miniz: stored DEFLATE blocks (BTYPE 00) only, taking its input in
// whatever pieces input() hands out, as uzlib does through its read callback.
class StoredBlockInflater : public Inflater {
 public:
  int begins = 0, reads = 0, ends = 0;

  bool begin(InflateInput input, void* context, uint32_t) override {
    ++begins;
    input_ = input;
    context_ = context;
    piece_ = nullptr;
    left_ = 0;
    blockLeft_ = 0;
    final_ = false;
    done_ = false;
    return true;
  }
  int32_t read(uint8_t* out, uint32_t cap) override {
    ++reads;
    uint32_t produced = 0;
    while (produced < cap && !done_) {
      if (blockLeft_ == 0) {
        if (final_) {
          done_ = true;
          break;
        }
        uint8_t header[5];
        for (uint8_t& b : header) {
          const int c = nextByte();
          if (c < 0) return -1;
          b = static_cast<uint8_t>(c);
        }
        if ((header[0] & 0x06) != 0) return -1;  // only stored blocks
        final_ = (header[0] & 1) != 0;
        blockLeft_ = static_cast<uint16_t>(header[1] | header[2] << 8);
        if (static_cast<uint16_t>(~(header[3] | header[4] << 8)) != blockLeft_) return -1;
        continue;
      }
      if (left_ == 0 && !refill()) return -1;
      uint32_t n = cap - produced;
      if (n > left_) n = left_;
      if (n > blockLeft_) n = blockLeft_;
      std::memcpy(out + produced, piece_, n);
      piece_ += n;
      left_ -= n;
      blockLeft_ -= n;
      produced += n;
    }
    return static_cast<int32_t>(produced);
  }
  void end() override { ++ends; }

 private:
  bool refill() {
    const int32_t size = input_(context_, &piece_);
    if (size <= 0) return false;
    left_ = static_cast<uint32_t>(size);
    return true;
  }
  int nextByte() {
    if (left_ == 0 && !refill()) return -1;
    --left_;
    return *piece_++;
  }
  InflateInput input_ = nullptr;
  void* context_ = nullptr;
  const uint8_t* piece_ = nullptr;
  uint32_t left_ = 0;
  uint32_t blockLeft_ = 0;
  bool final_ = false;
  bool done_ = false;
};

// Identity "AES": checks the CBC chaining, not the cipher.
class IdentityCrypto : public freeink::content::Crypto {
 public:
  int calls = 0;
  bool decrypt(const uint8_t* in, size_t len, uint8_t* out) {
    assert(len > 0 && len % 16 == 0);
    ++calls;
    std::memmove(out, in, len);
    return true;
  }
  int32_t rsaPrivateRaw(const uint8_t*, size_t, const uint8_t*, size_t, uint8_t*, size_t) override { return -1; }
  bool aes128CbcDecrypt(const uint8_t*, const uint8_t*, const uint8_t* in, size_t len, uint8_t* out) override {
    return decrypt(in, len, out);
  }
  bool aes256CbcDecrypt(const uint8_t*, const uint8_t*, const uint8_t* in, size_t len, uint8_t* out) override {
    return decrypt(in, len, out);
  }
  void sha1(const uint8_t*, size_t, uint8_t*) override {}
  void sha256(const uint8_t*, size_t, uint8_t*) override {}
  bool rsaGenerate(freeink::content::RsaKeyPairDer*) override { return false; }
  bool rsaPublicEncrypt(const uint8_t*, size_t, const uint8_t*, size_t, uint8_t*, size_t, size_t*) override {
    return false;
  }
  bool rsaPrivateSignRaw(const uint8_t*, size_t, const uint8_t*, uint8_t*) override { return false; }
  bool aes128CbcEncrypt(const uint8_t*, const uint8_t*, const uint8_t*, size_t, uint8_t*) override { return false; }
  bool pkcs12Extract(const uint8_t*, size_t, const std::string&, std::vector<uint8_t>*,
                     std::vector<uint8_t>*) override {
    return false;
  }
  void randomBytes(uint8_t*, size_t) override {}
};

bool append(void* context, const uint8_t* data, size_t size) {
  static_cast<std::string*>(context)->append(reinterpret_cast<const char*>(data), size);
  return true;
}

// An AES-128 (PKCS#7) entry holding `plain` as stored DEFLATE blocks of at most `block` bytes.
std::string deflatedBlob(const std::string& plain, size_t block) {
  std::string payload;
  for (size_t at = 0; at < plain.size(); at += block) {
    const size_t n = std::min(block, plain.size() - at);
    const bool last = at + n == plain.size();
    payload.push_back(last ? 1 : 0);
    payload.push_back(static_cast<char>(n));
    payload.push_back(static_cast<char>(n >> 8));
    payload.push_back(static_cast<char>(~n));
    payload.push_back(static_cast<char>(~n >> 8));
    payload.append(plain, at, n);
  }
  const char pad = static_cast<char>(16 - payload.size() % 16);
  payload.append(static_cast<size_t>(pad), pad);
  return std::string(16, static_cast<char>(0xA5)) + payload;
}

// One cipher per container, as ProtectedBook assumes.
std::string aes128Entry(const char* uri, const char* compression) {
  std::string e = encEntry(uri, compression);
  e.replace(e.find("aes256-cbc"), std::strlen("aes256-cbc"), "aes128-cbc");
  return e;
}

}  // namespace

int main() {
  // 9,000 bytes in 1,000-byte blocks: several CBC chunks and inflate reads, blocks crossing pieces.
  std::string plain;
  for (int i = 0; i < 9000; ++i) plain.push_back(static_cast<char>('a' + i % 26));
  const std::string xml = "<encryption>" + aes128Entry("chapter.xhtml", "Method=\"8\"") +
                          aes128Entry("notes.xhtml", "Method=\"0\" OriginalLength=\"32\"") + "</encryption>";
  auto source = storedZip({{"META-INF/encryption.xml", xml},
                           {"chapter.xhtml", deflatedBlob(plain, 1000)},
                           {"notes.xhtml", std::string(48, 'n')}});
  const uint8_t key[16] = {};

  // The index answers every lookup; no scan of the book's own.
  CountingIndex index;
  index.source = &source;
  assert(index.scan.open(source));
  ProtectedBook book;
  assert(book.openWithIndex(source, index));
  assert(book.isProtected() && book.isEncrypted("chapter.xhtml") && book.isEncrypted("notes.xhtml"));
  assert(index.finds == 1 && index.offsets == 1);  // encryption.xml
  // Without OriginalLength the zip entry's size answers: the encrypted blob's.
  assert(book.decryptedSize("notes.xhtml") == 32 &&
         book.decryptedSize("chapter.xhtml") == deflatedBlob(plain, 1000).size());
  book.setContentKey(key, 16);

  // A STORED entry needs no inflater.
  IdentityCrypto crypto;
  std::string out;
  assert(book.decryptEntryToSink(source, crypto, "notes.xhtml", append, &out));
  assert(out == std::string(32, 'n'));

  // A deflated entry through the injected pull inflater.
  StoredBlockInflater stored;
  book.setInflater(&stored);
  out.clear();
  assert(book.decryptEntryToSink(source, crypto, "chapter.xhtml", append, &out));
  assert(out == plain);
  assert(stored.begins == 1 && stored.ends == 1 && stored.reads > 1);
  assert(index.offsets >= 3);

#if !defined(CONTENT_NO_DEFAULT_INFLATER)
  // miniz behind the same interface gives the same bytes.
  freeink::content::MinizInflater miniz;
  book.setInflater(&miniz);
  out.clear();
  assert(book.decryptEntryToSink(source, crypto, "chapter.xhtml", append, &out));
  assert(out == plain);
  // And it stays the default.
  book.setInflater(nullptr);
  out.clear();
  assert(book.decryptEntryToSink(source, crypto, "chapter.xhtml", append, &out));
  assert(out == plain);
#else
  // Without a default, a deflated item names what is missing.
  book.setInflater(nullptr);
  out.clear();
  assert(!book.decryptEntryToSink(source, crypto, "chapter.xhtml", append, &out));
  assert(book.lastError() == "no inflater for a deflated entry");
#endif

  // A deflated encryption.xml through the injected inflater, before any key.
  auto deflatedManifest = storedZip({{"META-INF/encryption.xml", xml}, {"notes.xhtml", std::string(48, 'n')}}, true);
  ProtectedBook manifestBook;
  StoredBlockInflater manifestInflater;
  manifestBook.setInflater(&manifestInflater);
  assert(manifestBook.open(deflatedManifest));
  assert(manifestBook.isEncrypted("chapter.xhtml") && manifestInflater.begins == 1 && manifestInflater.ends == 1);
#if defined(CONTENT_NO_DEFAULT_INFLATER)
  ProtectedBook bare;
  assert(!bare.open(deflatedManifest));
  assert(bare.lastError() == "no inflater for a deflated encryption.xml");
#endif

#if defined(CONTENT_NO_DEFAULT_INFLATER)
  std::puts("test_seams (no default inflater, no miniz linked): all assertions passed");
#else
  std::puts("test_seams: all assertions passed");
#endif
  return 0;
}
