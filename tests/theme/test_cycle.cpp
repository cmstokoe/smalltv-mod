// Cursor that picks the next theme in an automatic cycle (ThemeCycle.h).
//
// The property that matters for shuffle is coverage: for one seed, walking
// successors must reach every installed theme exactly once before the scan
// wraps. If it ever repeated or skipped, a user would see one theme twice in a
// lap and another not at all, which is the whole point of shuffling rather
// than picking at random.
#include "ThemeCycle.h"
#include <cassert>
#include <iostream>
#include <set>
#include <string>
#include <vector>
using namespace smalltv;

// The id the cursor picks after `after`, given everything installed.
static std::string pick(uint8_t order, uint32_t seed, const std::vector<std::string>& ids,
                        const std::string& after, const std::string& exclude = std::string()) {
  CycleCursor c(order, seed, after, exclude);
  for (size_t i = 0; i < ids.size(); ++i) c.offer(ids[i]);
  return c.empty() ? std::string() : c.chosen();
}

static bool wraps(uint8_t order, uint32_t seed, const std::vector<std::string>& ids,
                  const std::string& after) {
  CycleCursor c(order, seed, after);
  for (size_t i = 0; i < ids.size(); ++i) c.offer(ids[i]);
  return c.wrapped();
}

// One full lap: start at the smallest key, then follow successors until the
// scan wraps.
static std::vector<std::string> lap(uint8_t order, uint32_t seed,
                                    const std::vector<std::string>& ids) {
  std::vector<std::string> out;
  std::string cur = pick(order, seed, ids, "");
  if (cur.empty()) return out;
  for (;;) {
    out.push_back(cur);
    if (wraps(order, seed, ids, cur)) break;
    cur = pick(order, seed, ids, cur);
    if (out.size() > ids.size() + 2) break;   // runaway guard, asserted below
  }
  return out;
}

static void testSorted() {
  std::vector<std::string> ids;
  ids.push_back("charlie");
  ids.push_back("alpha");
  ids.push_back("bravo");

  // Directory order is irrelevant: the key is the id.
  assert(pick(CycleSorted, 0, ids, "alpha") == "bravo");
  assert(pick(CycleSorted, 0, ids, "bravo") == "charlie");
  // Past the last: wraps to the smallest.
  assert(wraps(CycleSorted, 0, ids, "charlie"));
  assert(pick(CycleSorted, 0, ids, "charlie") == "alpha");
  // Unpinned: start at the smallest.
  assert(pick(CycleSorted, 0, ids, "") == "alpha");

  // A theme deleted mid-cycle still orders against the rest, so the cycle
  // resumes where it was instead of jumping back to the start.
  assert(pick(CycleSorted, 0, ids, "bb-deleted") == "bravo");

  // One installed theme: the lap is that theme, and it wraps to itself.
  std::vector<std::string> one(1, "solo");
  assert(pick(CycleSorted, 0, one, "solo") == "solo");
  assert(wraps(CycleSorted, 0, one, "solo"));

  // Nothing installed: nothing to pick, and the caller keeps what is up.
  std::vector<std::string> none;
  CycleCursor empty(CycleSorted, 0, "anything");
  assert(empty.empty());
  assert(pick(CycleSorted, 0, none, "anything").empty());
}

static void testSortEdges() {
  // '-' (0x2D) < '_' (0x5F), and uppercase sorts before lowercase. Byte order
  // is what the cursor uses; this pins it so a later change is deliberate.
  std::vector<std::string> ids;
  ids.push_back("a_b");
  ids.push_back("a-b");
  ids.push_back("A-b");
  assert(pick(CycleSorted, 0, ids, "") == "A-b");
  assert(pick(CycleSorted, 0, ids, "A-b") == "a-b");
  assert(pick(CycleSorted, 0, ids, "a-b") == "a_b");

  // A prefix sorts before the longer id that extends it.
  std::vector<std::string> pre;
  pre.push_back("theme");
  pre.push_back("theme2");
  assert(pick(CycleSorted, 0, pre, "") == "theme");
  assert(pick(CycleSorted, 0, pre, "theme") == "theme2");
}

static void testExclude() {
  std::vector<std::string> ids;
  ids.push_back("alpha");
  ids.push_back("bravo");
  // Restarting a lap must not open on the theme already on screen.
  assert(pick(CycleSorted, 0, ids, "", "alpha") == "bravo");
  // Unless it is the only one installed, where there is nothing else to show.
  std::vector<std::string> one(1, "solo");
  assert(pick(CycleSorted, 0, one, "", "solo").empty());
}

static void testShuffleCoversEveryTheme() {
  // Up to MaxThemes (16), under several seeds: every lap is a permutation.
  for (size_t n = 1; n <= 16; ++n) {
    std::vector<std::string> ids;
    for (size_t i = 0; i < n; ++i) {
      // Long shared prefix on purpose: plain FNV-1a would leave these nearly
      // adjacent, so this also exercises the avalanche in cycleHash().
      ids.push_back("installed-theme-" + std::to_string(i));
    }
    for (uint32_t seed = 1; seed <= 8; ++seed) {
      std::vector<std::string> walked = lap(CycleShuffle, seed, ids);
      assert(walked.size() == n);                       // no early wrap, no runaway
      std::set<std::string> unique(walked.begin(), walked.end());
      assert(unique.size() == n);                       // no repeat within a lap
      for (size_t i = 0; i < n; ++i) assert(unique.count(ids[i]) == 1);
    }
  }
}

static void testShuffleReseedChangesOrder() {
  std::vector<std::string> ids;
  for (size_t i = 0; i < 8; ++i) ids.push_back("theme-" + std::to_string(i));
  std::vector<std::string> a = lap(CycleShuffle, 1, ids);
  // At least one other seed must give a different lap. Any single seed could
  // coincide with seed 1, so this asserts the family varies, not one pair.
  bool differs = false;
  for (uint32_t seed = 2; seed <= 16 && !differs; ++seed) {
    differs = lap(CycleShuffle, seed, ids) != a;
  }
  assert(differs);
}

static void testShuffleIsStableWithinASeed() {
  // The same seed must give the same successor every time it is asked, or a
  // lap could revisit a theme: nothing stores the permutation, so the hash
  // ordering is the only thing holding a lap together.
  std::vector<std::string> ids;
  for (size_t i = 0; i < 6; ++i) ids.push_back("t" + std::to_string(i));
  for (size_t i = 0; i < ids.size(); ++i) {
    const std::string once = pick(CycleShuffle, 42, ids, ids[i]);
    for (int again = 0; again < 3; ++again) {
      assert(pick(CycleShuffle, 42, ids, ids[i]) == once);
    }
  }
}

static void testShuffleIgnoresOfferOrder() {
  std::vector<std::string> forward;
  for (size_t i = 0; i < 10; ++i) forward.push_back("theme-" + std::to_string(i));
  std::vector<std::string> reversed(forward.rbegin(), forward.rend());
  // The directory hands ids over in whatever order it likes; the lap must not.
  assert(lap(CycleShuffle, 7, forward) == lap(CycleShuffle, 7, reversed));
  assert(lap(CycleSorted, 0, forward) == lap(CycleSorted, 0, reversed));
}

int main() {
  testSorted();
  testSortEdges();
  testExclude();
  testShuffleCoversEveryTheme();
  testShuffleReseedChangesOrder();
  testShuffleIsStableWithinASeed();
  testShuffleIgnoresOfferOrder();
  std::cout << "theme cycle tests passed\n";
  return 0;
}
