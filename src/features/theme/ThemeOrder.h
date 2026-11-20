// ThemeOrder.h -- the mandated theme order, /themes/.order.
//
// One theme id per line, in the order the user arranged them. It is a sidecar
// file rather than a setting because ids are up to 48 chars and MaxThemes is
// 16: ~800 B would go into a settings document that is already ~2.4 KB and
// needs RequestFreeHeap (8192 B) to save on a chip running a theme with ~6.5 KB
// free. The file follows /themes/.index exactly -- written through a `.new`
// sibling and renamed into place (see publishIndex in ThemeWeb.cpp).
//
// Every function here streams: one line of state, never the whole file, for
// the same heap reason as ThemeCycle.h. Each line ends with Source::pause(),
// which is a yield() on the device (CONTRIBUTING: a loop over input-sized data
// yields).
//
// The file is maintained whenever the installed set changes, on the same hooks
// that maintain .index, so it is always complete. That keeps the read path
// free of "theme missing from the order" repair logic: a missing or unreadable
// file simply means the caller falls back to sorted order.
#pragma once
#include "../../JsonStream.h"   // Sink
#include "ThemePackage.h"       // Source
#include <cstdint>
#include <string>

namespace smalltv {

// An id is capped at 48 chars; the slack absorbs trailing whitespace rather
// than discarding an otherwise good line.
constexpr size_t MaxOrderLine = 64;
// A stale file can hold ids no longer installed, so allow well above
// MaxThemes lines before giving up on it entirely.
constexpr uint32_t MaxOrderBytes = 64 * MaxOrderLine;

// Streams /themes/.order one line at a time. A line longer than MaxOrderLine
// is consumed and returned empty, so a corrupt file costs its own length and
// nothing more.
class OrderLines {
 public:
  explicit OrderLines(Source& file)
      : file_(file), size_(file.size() > MaxOrderBytes ? 0 : file.size()) {}

  // False once the file is exhausted. `out` may come back empty (a blank or
  // over-long line); callers skip those.
  bool next(std::string& out) {
    out.clear();
    if (pos_ >= size_) return false;
    bool over = false;
    while (pos_ < size_) {
      char c;
      if (!file_.read(pos_, &c, 1)) {
        pos_ = size_;
        break;
      }
      ++pos_;
      if (c == '\n') break;
      if (c == '\r') continue;
      if (out.size() >= MaxOrderLine) {
        over = true;
        continue;
      }
      out.push_back(c);
    }
    if (over) out.clear();
    file_.pause();
    return true;
  }

 private:
  Source& file_;
  uint32_t size_;
  uint32_t pos_ = 0;
};

// A line is "<id>" or "<id> <seconds>": the id, and optionally how long that
// theme should stay on screen. Returns the id, and through `sec` the dwell
// when one is present and well formed -- 0 meaning "use the global default",
// which is also what a malformed suffix falls back to rather than the line
// being thrown away. A theme is far better off cycling at the default than
// disappearing from the order because someone hand-edited the file.
inline std::string orderLineId(const std::string& line, uint16_t* sec = nullptr) {
  if (sec) *sec = 0;
  const size_t space = line.find(' ');
  if (space == std::string::npos) return line;
  if (sec) {
    uint32_t value = 0;
    bool digits = space + 1 < line.size();
    for (size_t i = space + 1; i < line.size() && digits; ++i) {
      if (line[i] < '0' || line[i] > '9') digits = false;
      else value = value * 10 + static_cast<uint32_t>(line[i] - '0');
      if (value > 0xFFFF) digits = false;
    }
    if (digits) *sec = static_cast<uint16_t>(value);
  }
  return line.substr(0, space);
}

// The line to store for one theme. A dwell of 0 writes the bare id, so a file
// where nothing has been customised reads exactly as it did before dwells
// existed.
inline std::string orderLine(const std::string& id, uint16_t sec) {
  return sec ? id + " " + std::to_string(sec) : id;
}

// How long `id` should stay on screen, or 0 for "use the global default".
inline uint16_t orderDwell(Source& file, const std::string& id) {
  OrderLines lines(file);
  std::string line;
  while (lines.next(line)) {
    if (line.empty()) continue;
    uint16_t sec = 0;
    if (orderLineId(line, &sec) == id) return sec;
  }
  return 0;
}

// Rewrites the order with one theme's dwell set (0 clears it back to the
// default). An id that is not in the file is appended, so a dwell can be set
// before the maintenance pass has ever run.
inline bool setThemeDwell(Source& from, Sink& to, const std::string& id, uint16_t sec) {
  OrderLines lines(from);
  std::string line;
  bool wrote = false;
  auto emit = [&to](const std::string& text) {
    return to.write(text.data(), text.size()) && to.write("\n", 1);
  };
  while (lines.next(line)) {
    if (line.empty()) continue;
    if (!wrote && orderLineId(line) == id) {
      wrote = true;
      if (!emit(orderLine(id, sec))) return false;
      continue;
    }
    if (!emit(line)) return false;
  }
  if (!wrote && !id.empty() && !emit(orderLine(id, sec))) return false;
  return true;
}

// The id after `current`, wrapping to the first. Empty when the file holds no
// usable id, which tells the caller to fall back to sorted order.
//
// `current` not being in the file is not an error -- it happens between an
// install and the next maintenance pass -- and the cycle restarts from the
// first entry rather than stalling.
inline std::string orderSuccessor(Source& file, const std::string& current) {
  OrderLines lines(file);
  std::string line, first, next;
  bool seen = false, haveFirst = false;
  while (lines.next(line)) {
    if (line.empty()) continue;
    const std::string id = orderLineId(line);   // the dwell suffix is not part of it
    if (id.empty()) continue;
    if (!haveFirst) {
      first = id;
      haveFirst = true;
    }
    if (seen) {
      next = id;
      break;
    }
    if (id == current) seen = true;
  }
  if (!next.empty()) return next;
  return first;
}

// Rewrites the order with one id dropped and/or one appended: the install and
// delete maintenance pass. Mirrors copyThemeIndex's shape deliberately.
// Passing an empty string skips that half, so a plain tidy-up is
// copyThemeOrder(from, to, "", "").
//
// `add` is appended only if it is not already present, so running the pass
// twice cannot duplicate a line.
inline bool copyThemeOrder(Source& from, Sink& to, const std::string& remove,
                           const std::string& add) {
  OrderLines lines(from);
  std::string line;
  bool haveAdd = false;
  while (lines.next(line)) {
    if (line.empty()) continue;
    // Match on the id alone, but write the line back whole: that is what keeps
    // a theme's dwell through an install or a delete of some other theme.
    const std::string id = orderLineId(line);
    if (id.empty() || id == remove) continue;
    if (id == add) {
      if (haveAdd) continue;   // a duplicate already in the file
      haveAdd = true;
    }
    if (!to.write(line.data(), line.size()) || !to.write("\n", 1)) return false;
  }
  if (!add.empty() && !haveAdd) {
    if (!to.write(add.data(), add.size()) || !to.write("\n", 1)) return false;
  }
  return true;
}

// Rewrites the order with `id` moved one place toward the front (`up`) or the
// back. A no-op when `id` is absent, or already at that end.
//
// Streaming, so it holds at most two lines: moving up emits the id before the
// line it has been holding back, moving down holds the id until the following
// line has been emitted.
inline bool moveThemeOrder(Source& from, Sink& to, const std::string& id, bool up) {
  OrderLines lines(from);
  std::string line, held;
  bool haveHeld = false, moved = false;
  auto emit = [&to](const std::string& text) {
    return to.write(text.data(), text.size()) && to.write("\n", 1);
  };
  while (lines.next(line)) {
    if (line.empty()) continue;
    if (up) {
      if (orderLineId(line) == id && !moved && haveHeld) {
        // id then the line that was above it
        if (!emit(line) || !emit(held)) return false;
        haveHeld = false;
        moved = true;
        continue;
      }
      if (haveHeld && !emit(held)) return false;
      held = line;
      haveHeld = true;
    } else {
      if (haveHeld) {
        // the line after the held id goes first, then the id
        if (!emit(line) || !emit(held)) return false;
        haveHeld = false;
        moved = true;
        continue;
      }
      if (orderLineId(line) == id && !moved) {
        held = line;
        haveHeld = true;
        continue;
      }
      if (!emit(line)) return false;
    }
  }
  if (haveHeld && !emit(held)) return false;
  return true;
}

}  // namespace smalltv
