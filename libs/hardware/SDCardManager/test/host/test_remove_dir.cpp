#include <SDCardManager.h>

#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

using Strings = std::vector<std::string>;

static void reset() {
  fakeSd::directories.clear();
  fakeSd::openedDirectories.clear();
  fakeSd::removedFiles.clear();
  fakeSd::removedDirectories.clear();
  fakeSd::directoryOpenLimit = 64;
}

int main() {
  SDCardManager card;

  // A tree goes depth first: the files and the subfolder, then the folder itself.
  reset();
  fakeSd::directories["/books"] = {{"a.epub", false}, {"series", true}};
  fakeSd::directories["/books/series"] = {{"b.epub", false}};
  assert(card.removeDir("/books"));
  assert((fakeSd::removedFiles == Strings{"/books/a.epub", "/books/series/b.epub"}));
  assert((fakeSd::removedDirectories == Strings{"/books/series", "/books"}));

  // The longest name the 128-byte buffer holds.
  reset();
  const std::string fits(127, 'x');
  fakeSd::directories["/books"] = {{fits, true}};
  fakeSd::directories["/books/" + fits] = {};
  assert(card.removeDir("/books"));
  assert((fakeSd::removedDirectories == Strings{"/books/" + fits, "/books"}));

  // One byte more and SdFat hands back "": "/books/" + "" names the folder being removed, and the walk reopened it
  // until the stack ran out (43 or more CJK characters in a folder name are enough). Refuse instead.
  for (const bool directory : {true, false}) {
    reset();
    const std::string tooLong(128, 'y');
    fakeSd::directories["/books"] = {{"a.epub", false}, {tooLong, directory}, {"z.epub", false}};
    fakeSd::directories["/books/" + tooLong] = {};
    assert(!card.removeDir("/books"));
    assert((fakeSd::openedDirectories == Strings{"/books"}));
    assert((fakeSd::removedFiles == Strings{"/books/a.epub"}));
    assert(fakeSd::removedDirectories.empty());
  }

  std::puts("SDCardManager removeDir: depth-first removal and over-long names passed");
  return 0;
}
