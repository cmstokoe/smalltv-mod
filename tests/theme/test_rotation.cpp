#include "ThemeRotation.h"
#include <cassert>
#include <iostream>
#include <string>
#include <vector>
using namespace smalltv;
// The id that follows `after` when `installed` is what /themes holds. The ids
// are offered in the given order, which is the order the filesystem happened
// to list them in: the answer must not depend on it.
static std::string next(const char* after, const std::vector<std::string>& installed) {
  RotationCursor cursor(after);
  for (const std::string& id : installed) cursor.offer(id.c_str());
  return cursor.next();
}
// One full lap from `after`, stopping when it comes back round.
static std::string lap(const char* after, const std::vector<std::string>& installed) {
  std::string at = after, out;
  for (size_t i = 0; i < installed.size(); ++i) {
    at = next(at.c_str(), installed);
    out += at + " ";
  }
  return out;
}
int main() {
  const std::vector<std::string> three = {"clock", "pixel-room", "aurora"};

  // The next id in text order, and a wrap from the last back to the first.
  assert(next("aurora", three) == "clock");
  assert(next("clock", three) == "pixel-room");
  assert(next("pixel-room", three) == "aurora");

  // Directory order does not reach the result.
  const std::vector<std::string> shuffled = {"pixel-room", "aurora", "clock"};
  for (const std::string& id : three) assert(next(id.c_str(), three) == next(id.c_str(), shuffled));

  // Every theme appears once per lap, and the lap returns to its start.
  assert(lap("aurora", three) == "clock pixel-room aurora ");

  // Nothing installed: no selection to move to, so the caller keeps its own.
  assert(next("clock", {}).empty());

  // One theme installed is a rotation that never changes what is on screen,
  // whether or not it is the one already showing.
  assert(next("clock", {"clock"}) == "clock");
  assert(next("aurora", {"clock"}) == "clock");
  assert(next("zephyr", {"clock"}) == "clock");

  // The current theme was removed while it was showing: the rotation carries
  // on from where it was rather than starting over.
  assert(next("clock", {"aurora", "pixel-room"}) == "pixel-room");
  // ...including when it was the last id, which wraps as usual.
  assert(next("zephyr", {"aurora", "clock"}) == "aurora");

  // No theme pinned yet (themeId is empty until the first selection): start
  // at the first id rather than at nothing.
  assert(next("", three) == "aurora");

  // Ids differing only in length and in case sort as text. '-' (0x2d) and '_'
  // (0x5f) sit either side of the digits and letters, and uppercase sorts
  // before lowercase, which is what validId() allows.
  const std::vector<std::string> edges = {"a", "a-b", "a_b", "ab", "A"};
  assert(lap("", edges) == "A a a-b a_b ab ");

  // An empty name is not an id and is passed over.
  assert(next("", {"", "clock"}) == "clock");

  std::cout << "rotation tests passed\n";
  return 0;
}
