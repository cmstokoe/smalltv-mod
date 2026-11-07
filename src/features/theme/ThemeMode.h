#pragma once
#include "config.h"
#if WITH_THEME
#include "Mode.h"
#include "ThemePackage.h"
#include "ThemeData.h"
#include <LittleFS.h>
#include <memory>
#include <vector>

class ThemeFile : public smalltv::Source {
 public:
  bool open(const String& path) {
    file_ = LittleFS.open(path, "r");
    return bool(file_);
  }
  uint32_t size() const override { return file_ ? file_.size() : 0; }
  bool read(uint32_t offset, void* dest, size_t count) override {
    return file_ && file_.seek(offset) && file_.read(static_cast<uint8_t*>(dest), count) == count;
  }
  void pause() override { yield(); }
 private:
  File file_;
};
// Hands every installed theme's id to `fn`. Reads directory entries only:
// openNextFile() opens every file it walks, and on the ESP8266 this runs
// beside a loaded theme, where the heap is shortest (same reason as
// installedCount() in ThemeWeb.cpp). A lap of the directory is input-sized, so
// it yields per entry (CONTRIBUTING).
template <class Fn>
void eachInstalledTheme(Fn fn) {
#if defined(SMALLTV_ESP8266)
  Dir dir = LittleFS.openDir("/themes");
  while (dir.next()) {
    if (!dir.isFile()) continue;
    String name = dir.fileName();
    if (!name.endsWith(".stheme")) continue;
    fn(name.substring(0, name.length() - 7));
    yield();
  }
#else
  File dir = LittleFS.open("/themes", "r");
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    if (f.isDirectory()) continue;
    String name = f.name();
    const int slash = name.lastIndexOf('/');
    if (slash >= 0) name = name.substring(slash + 1);
    if (!name.endsWith(".stheme")) continue;
    fn(name.substring(0, name.length() - 7));
    yield();
  }
  dir.close();
#endif
}
class ThemeMode : public DisplayMode {
 public:
  const char* id() const override { return "theme"; }
  uint8_t modeConst() const override { return MODE_THEME; }
  void begin(const Settings&) override;
  void service(const Settings&) override;
  void invalidate(const Settings&) override;
  void wake(const Settings&) override {
    engine_.invalidate();
    messageDrawn_ = false;
    // Drop the tick origin rather than the elapsed total: the dwell counts
    // time this theme was actually on screen, so the stretch spent away (a
    // carousel slot elsewhere, a notification overlay, the info screen)
    // is not credited to it. Resetting the total instead would mean a theme
    // never advanced whenever carouselSec was shorter than the dwell.
    lastTickMs_ = 0;
  }
  const String& error() const { return error_; }
  // What parsing a package needs: its peak heap (tests/theme/test_memory.cpp
  // holds the examples to 5 KB) plus about 1 KB that the network code may
  // claim meanwhile, and blocks of up to 1 KB (the same test's
  // largest-request limit) with some slack.
  static constexpr uint32_t ParseFreeHeap = 6144;
  static constexpr uint32_t ParseLargestBlock = 1536;
  // Frees the loaded theme when the heap is short of what a web request needs,
  // so the request does not run out of memory next to it; on the ESP8266 a
  // theme and a second package parse do not fit together (issue #30). With
  // enough heap it does nothing. The selection, the fetched values and the
  // fetch schedule stay: the next service() call loads the theme again and
  // repaints it with the same values, and no source is fetched early.
  void release(uint32_t needFree = ParseFreeHeap, uint32_t needLargestBlock = ParseLargestBlock);
  // DisplayMode::freeForRequest(), before a settings read, save or import.
  // A save needs about 7 KB of heap (#35's measurement on a SmallTV-Ultra)
  // plus the same 1 KB of network slack as ParseFreeHeap, and builds a
  // 2.4 KB settings document. On the ESP8266 with a
  // theme running that is not free, so the theme is released; on the ESP32
  // boards it always is, and the theme stays.
  static constexpr uint32_t RequestFreeHeap = 8192;
  static constexpr uint32_t RequestLargestBlock = 3072;
  void freeForRequest() override { release(RequestFreeHeap, RequestLargestBlock); }
 private:
  void unload();
  void refreshData();
  // The theme that should be on screen: the cycle's entry while cycling, else
  // the user's pinned selection. themeId is never written by the cycle.
  String activeId(const Settings& s) const;
  // Accrues on-screen time and advances when the dwell is up.
  void serviceCycle(const Settings& s);
  // Picks the next theme. False when there is nothing to move to (nothing
  // installed, or only the one already showing).
  bool advanceCycle(const Settings& s);

  // A package that will not load is skipped, so one bad theme does not park
  // the cycle on an error screen. Bounded at a full lap's worth, after which
  // the error is reported the usual way rather than spinning silently.
  static constexpr uint8_t MaxCycleSkips = 16;

  smalltv::Engine engine_;
  std::unique_ptr<ThemeFile> file_;
  std::unique_ptr<smalltv::Package> package_;
  String loadedId_, error_;
  std::vector<uint32_t> dataNextMs_;
  std::vector<bool> dataScheduled_;
  std::vector<smalltv::ThemeValue> dataValues_;
  smalltv::DataRequests dataRequests_;
  size_t dataCursor_ = 0;
  // Set by release(): the next load of the same theme keeps the data state.
  bool keepData_ = false;
  bool attempted_ = false, messageDrawn_ = false;

  // --- Cycling. All RAM only: persisting the current entry would rewrite the
  // whole settings document every dwell (~2.4 KB serialize, temp file, rename,
  // and ~7 KB of transient heap), on the same partition as the packages. ---
  String cycleId_;              // entry on screen; empty => fall back to themeId
  String pinnedId_;             // last themeId seen, to spot a new selection
  uint32_t cycleElapsedMs_ = 0; // on-screen time accrued toward the dwell
  uint32_t lastTickMs_ = 0;     // previous service() millis(); 0 = just arrived
  uint32_t lapSeed_ = 0;        // shuffle permutation; rerolled each lap
  uint8_t cycleSkips_ = 0;      // consecutive unloadable packages this lap
};
extern ThemeMode g_themeMode;
#endif
