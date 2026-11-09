#pragma once
#include <string>
// Picks the theme that follows another in the rotation. The order is the ids
// sorted as text, which does not depend on the order the filesystem lists
// them in, so the rotation stays the same across reboots and is not disturbed
// by an install or a removal elsewhere in the list.
//
// Ids are offered one at a time rather than collected: the caller walks
// /themes without holding every name at once, and this keeps two ids, not
// sixteen.
namespace smalltv {
class RotationCursor {
 public:
  explicit RotationCursor(const char* after) : after_(after ? after : "") {}
  void offer(const char* id) {
    if (!id || !*id) return;
    if (after_ < id && (successor_.empty() || id < successor_)) successor_ = id;
    if (smallest_.empty() || id < smallest_) smallest_ = id;
  }
  // The smallest id greater than `after`, or the smallest of all once past
  // the end. Empty when nothing was offered; `after` itself when it is the
  // only id, so a caller with one installed theme sees no change.
  const std::string& next() const { return successor_.empty() ? smallest_ : successor_; }

 private:
  std::string after_, successor_, smallest_;
};
}
