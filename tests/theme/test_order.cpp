// The mandated theme order, /themes/.order (ThemeOrder.h).
//
// The file is maintained on install and delete, so the reader is allowed to be
// simple -- but it still has to survive a file that has gone stale or corrupt
// between those passes, because the alternative is a cycle that stalls on a
// device the user cannot easily inspect.
#include "ThemeOrder.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <string>
using namespace smalltv;

struct Memory : Source {
  std::string data;
  unsigned pauses = 0;
  explicit Memory(const std::string& text) : data(text) {}
  void pause() override { ++pauses; }
  uint32_t size() const override { return (uint32_t)data.size(); }
  bool read(uint32_t offset, void* out, size_t n) override {
    if (offset > data.size() || n > data.size() - offset) return false;
    memcpy(out, data.data() + offset, n);
    return true;
  }
};

struct Text : Sink {
  std::string data;
  bool write(const char* p, size_t n) override {
    data.append(p, n);
    return true;
  }
};

static std::string after(const std::string& file, const std::string& current) {
  Memory source(file);
  return orderSuccessor(source, current);
}

static std::string maintain(const std::string& file, const std::string& remove,
                            const std::string& add) {
  Memory source(file);
  Text out;
  assert(copyThemeOrder(source, out, remove, add));
  return out.data;
}

static std::string move(const std::string& file, const std::string& id, bool up) {
  Memory source(file);
  Text out;
  assert(moveThemeOrder(source, out, id, up));
  return out.data;
}

static void testSuccessor() {
  const std::string three = "alpha\nbravo\ncharlie\n";
  assert(after(three, "alpha") == "bravo");
  assert(after(three, "bravo") == "charlie");
  // Past the end: wrap to the first.
  assert(after(three, "charlie") == "alpha");
  // Unpinned, or a theme installed since the last maintenance pass: restart at
  // the first entry rather than stalling.
  assert(after(three, "") == "alpha");
  assert(after(three, "not-in-the-file") == "alpha");

  // One entry wraps to itself.
  assert(after("solo\n", "solo") == "solo");
  // No usable entry: empty, so the caller falls back to sorted order.
  assert(after("", "alpha").empty());
  assert(after("\n\n\n", "alpha").empty());
}

static void testTolerance() {
  // Trailing CR (a file edited on Windows, or a hand-written one) and blank
  // lines must not break the sequence.
  assert(after("alpha\r\nbravo\r\n", "alpha") == "bravo");
  assert(after("alpha\n\n\nbravo\n", "alpha") == "bravo");
  // A missing final newline still yields its line.
  assert(after("alpha\nbravo", "alpha") == "bravo");

  // An over-long line is dropped rather than truncated into a different id --
  // truncation could silently alias one theme onto another.
  const std::string huge(MaxOrderLine + 10, 'x');
  assert(after("alpha\n" + huge + "\nbravo\n", "alpha") == "bravo");

  // A file past MaxOrderBytes is ignored entirely.
  std::string enormous;
  while (enormous.size() <= MaxOrderBytes) enormous += "padding-theme-id\n";
  assert(after(enormous, "").empty());
}

static void testMaintenance() {
  const std::string three = "alpha\nbravo\ncharlie\n";
  // Delete drops the line and keeps the rest in order.
  assert(maintain(three, "bravo", "") == "alpha\ncharlie\n");
  // Install appends, so a new theme joins at the end of the user's sequence
  // rather than jumping into the middle of it.
  assert(maintain(three, "", "delta") == "alpha\nbravo\ncharlie\ndelta\n");
  // Running the pass twice must not duplicate: the second call is a no-op.
  assert(maintain(maintain(three, "", "delta"), "", "delta") ==
         "alpha\nbravo\ncharlie\ndelta\n");
  // A plain tidy-up drops blank lines.
  assert(maintain("alpha\n\nbravo\n", "", "") == "alpha\nbravo\n");
  // Replacing in place: remove and add in one pass.
  assert(maintain(three, "alpha", "delta") == "bravo\ncharlie\ndelta\n");
  // Deleting the only entry leaves an empty file, not a stray newline.
  assert(maintain("solo\n", "solo", "").empty());
}

static void testMove() {
  const std::string four = "alpha\nbravo\ncharlie\ndelta\n";
  assert(move(four, "bravo", true) == "bravo\nalpha\ncharlie\ndelta\n");
  assert(move(four, "charlie", true) == "alpha\ncharlie\nbravo\ndelta\n");
  assert(move(four, "bravo", false) == "alpha\ncharlie\nbravo\ndelta\n");
  assert(move(four, "charlie", false) == "alpha\nbravo\ndelta\ncharlie\n");

  // At the ends, and for an id that is not there, the order is unchanged --
  // the UI leaves the buttons live at the ends, so this is a normal request.
  assert(move(four, "alpha", true) == four);
  assert(move(four, "delta", false) == four);
  assert(move(four, "missing", true) == four);
  assert(move(four, "missing", false) == four);

  // Single entry: both directions are no-ops.
  assert(move("solo\n", "solo", true) == "solo\n");
  assert(move("solo\n", "solo", false) == "solo\n");

  // Moving up then down returns the original sequence.
  assert(move(move(four, "charlie", true), "charlie", false) == four);
}

static std::string dwellSet(const std::string& file, const std::string& id, uint16_t sec) {
  Memory source(file);
  Text out;
  assert(setThemeDwell(source, out, id, sec));
  return out.data;
}

static uint16_t dwellOf(const std::string& file, const std::string& id) {
  Memory source(file);
  return orderDwell(source, id);
}

static void testDwell() {
  // A line is the id alone, or the id and the seconds that theme stays up.
  uint16_t sec = 0;
  assert(orderLineId("alpha", &sec) == "alpha" && sec == 0);
  assert(orderLineId("alpha 300", &sec) == "alpha" && sec == 300);
  // A malformed suffix falls back to the default rather than losing the theme:
  // a hand-edited file must not drop an entry out of the cycle.
  assert(orderLineId("alpha 30x", &sec) == "alpha" && sec == 0);
  assert(orderLineId("alpha ", &sec) == "alpha" && sec == 0);
  assert(orderLineId("alpha 99999", &sec) == "alpha" && sec == 0);   // past 16 bits
  assert(orderLine("alpha", 0) == "alpha" && orderLine("alpha", 300) == "alpha 300");

  const std::string mixed = "alpha 300\nbravo\ncharlie 60\n";
  assert(dwellOf(mixed, "alpha") == 300);
  assert(dwellOf(mixed, "bravo") == 0);
  assert(dwellOf(mixed, "charlie") == 60);
  assert(dwellOf(mixed, "missing") == 0);

  // The sequence still reads as ids, with the dwell invisible to it.
  assert(after(mixed, "alpha") == "bravo");
  assert(after(mixed, "charlie") == "alpha");
  assert(after(mixed, "") == "alpha");

  // Setting, clearing, and setting one that is not in the file yet.
  assert(dwellSet(mixed, "bravo", 120) == "alpha 300\nbravo 120\ncharlie 60\n");
  assert(dwellSet(mixed, "alpha", 0) == "alpha\nbravo\ncharlie 60\n");
  assert(dwellSet(mixed, "delta", 90) == "alpha 300\nbravo\ncharlie 60\ndelta 90\n");

  // The dwell survives the maintenance passes, which is the whole point of
  // keeping it on the same line.
  assert(maintain(mixed, "bravo", "") == "alpha 300\ncharlie 60\n");
  assert(maintain(mixed, "", "delta") == "alpha 300\nbravo\ncharlie 60\ndelta\n");
  assert(move(mixed, "charlie", true) == "alpha 300\ncharlie 60\nbravo\n");
  assert(move(mixed, "alpha", false) == "bravo\nalpha 300\ncharlie 60\n");
}

static void testYields() {
  // CONTRIBUTING: a loop over input-sized data pauses, so the watchdog and
  // WiFi are served while a long file is read.
  Memory source("alpha\nbravo\ncharlie\n");
  orderSuccessor(source, "charlie");
  assert(source.pauses >= 3);
}

int main() {
  testSuccessor();
  testTolerance();
  testMaintenance();
  testMove();
  testDwell();
  testYields();
  std::cout << "theme order tests passed\n";
  return 0;
}
