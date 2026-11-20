// ThemeCycle.h -- picking the next theme in an automatic cycle.
//
// The cycle never holds a list of installed ids. The ESP8266 runs a theme with
// about 6.5 KB of heap free (CONTRIBUTING) and a failed allocation reboots the
// device, so an array of up to MaxThemes ids is not affordable. Instead the
// caller walks /themes once and offers each id to a CycleCursor, which keeps
// only two candidates: the smallest key above the current theme's (the
// successor) and the smallest key overall (where a lap wraps to). Two strings,
// whatever the theme count.
//
// All three orders are the same scan under a different key:
//
//   sorted    the id itself, as text
//   shuffle   hash(id, seed), tie-broken by id so the order stays total
//   mandated  not here -- a position in /themes/.order, see ThemeOrder.h
//
// Shuffle gets "every theme once per lap" out of that, with no permutation
// stored anywhere: a hash under a fixed seed is a total order, so walking its
// successors reaches every id before wrapping. The wrap IS the end of the lap;
// the caller rerolls the seed and the next lap is a different permutation.
#pragma once
#include <cstdint>
#include <string>

namespace smalltv {

// Mirrors Settings.themeCycleOrder. Values are persisted in config.json, so
// they are fixed.
enum ThemeCycleOrder : uint8_t {
  CycleSorted   = 0,
  CycleMandated = 1,
  CycleShuffle  = 2,
};

// FNV-1a salted with the lap seed, then avalanched. The mix matters: theme ids
// in a set often share a long prefix ("pixel-room", "pixel-clock"), and plain
// FNV-1a leaves those adjacent, which would make a "random" lap walk related
// themes back to back.
inline uint32_t cycleHash(const std::string& id, uint32_t seed) {
  uint32_t h = 2166136261u ^ seed;
  for (unsigned char c : id) {
    h ^= c;
    h *= 16777619u;
  }
  h ^= h >> 16;
  h *= 2246822507u;
  h ^= h >> 13;
  h *= 3266489909u;
  h ^= h >> 16;
  return h;
}

class CycleCursor {
 public:
  // `after`   find the smallest key strictly above this id's; empty means no
  //           successor constraint, so chosen() is simply the smallest.
  // `exclude` never pick this id; empty means no exclusion. Used when a
  //           shuffle lap restarts, to stop the new lap opening on the theme
  //           that is already on screen.
  //
  // `after` does not have to be installed. A theme deleted mid-cycle still
  // orders correctly against the rest, so the cycle resumes from where it was
  // rather than jumping back to the start.
  CycleCursor(uint8_t order, uint32_t seed, const std::string& after,
              const std::string& exclude = std::string())
      : order_(order), seed_(seed), after_(after), exclude_(exclude),
        haveAfter_(!after.empty()) {}

  void offer(const std::string& id) {
    if (id.empty() || (!exclude_.empty() && id == exclude_)) return;
    if (!haveFirst_ || before(id, first_)) {
      first_ = id;
      haveFirst_ = true;
    }
    if (!haveAfter_ || !before(after_, id)) return;
    if (!haveNext_ || before(id, next_)) {
      next_ = id;
      haveNext_ = true;
    }
  }

  // No id was offered (or every one was excluded): the caller keeps whatever
  // is on screen.
  bool empty() const { return !haveFirst_; }
  // True when the scan ran off the end and chosen() is the wrap target. In
  // shuffle that is the end of a lap.
  bool wrapped() const { return !haveNext_; }
  const std::string& chosen() const { return haveNext_ ? next_ : first_; }

 private:
  bool before(const std::string& a, const std::string& b) const {
    if (order_ == CycleShuffle) {
      const uint32_t ha = cycleHash(a, seed_), hb = cycleHash(b, seed_);
      if (ha != hb) return ha < hb;
      // Equal hashes would otherwise make the order partial, and a theme could
      // be skipped for a whole lap or repeated inside one.
    }
    return a < b;
  }

  uint8_t order_;
  uint32_t seed_;
  std::string after_, exclude_, first_, next_;
  bool haveAfter_ = false, haveFirst_ = false, haveNext_ = false;
};

}  // namespace smalltv
