// The face registry.
//
// The only file that knows how many faces exist. Adding one is three lines
// here plus its own .cpp; nothing in main.cpp changes, and nothing in the
// protocol, the NVS layout or the button handling changes either.
//
// Order matters twice: it is the order the button cycles through, and it is
// the fallback order if the compile-time default name is misspelled. It is NOT
// what gets persisted -- the wire name is -- so faces can be reordered without
// stranding a board that has already been set.

#include <stdint.h>
#include <string.h>

#include "Face.hpp"

// Which face a board with empty NVS comes up in. Sam prefers "rounded" over
// the alternatives, so that is what fourteen fresh boards show; this exists so
// changing that choice is a one-line rebuild rather than an edit to the
// renderer. Override with -DDEFAULT_FACE='"bear"'.
#ifndef DEFAULT_FACE
#define DEFAULT_FACE "rounded"
#endif

Face* faceRounded();
Face* faceBear();
Face* faceArc();

namespace {

Face* gFaces[3] = { nullptr, nullptr, nullptr };

// Populated on the first query, which happens in setup() long before the first
// frame. Static initialisation order across translation units is not
// guaranteed, so the table is filled lazily rather than at file scope.
void ensure() {
  if (gFaces[0] != nullptr) return;
  gFaces[0] = faceRounded();
  gFaces[1] = faceBear();
  gFaces[2] = faceArc();
}

int lowerCmp(const char* a, const char* b) {
  for (;; ++a, ++b) {
    char ca = *a, cb = *b;
    if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
    if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
    if (ca != cb) return (int)(unsigned char)ca - (int)(unsigned char)cb;
    if (ca == '\0') return 0;
  }
}

}  // namespace

uint8_t faceCount() {
  ensure();
  return (uint8_t)(sizeof(gFaces) / sizeof(gFaces[0]));
}

Face* faceAt(uint8_t index) {
  ensure();
  if (index >= faceCount()) index = 0;
  return gFaces[index];
}

int faceIndexByName(const char* name) {
  if (name == nullptr || name[0] == '\0') return -1;
  ensure();
  for (uint8_t i = 0; i < faceCount(); ++i) {
    if (lowerCmp(gFaces[i]->name(), name) == 0) return (int)i;
  }
  return -1;
}

uint8_t defaultFaceIndex() {
  int i = faceIndexByName(DEFAULT_FACE);
  // A misspelled -DDEFAULT_FACE must not leave a board with nothing to draw.
  return (i < 0) ? 0 : (uint8_t)i;
}
