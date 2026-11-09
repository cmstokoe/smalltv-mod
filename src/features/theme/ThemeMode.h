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
    // The dwell counts the time a theme is on screen, so a carousel that was
    // away for longer than one does not switch the moment it comes back.
    rotateAt_ = millis();
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
  // The theme to show: the rotation's current entry while it runs, otherwise
  // the one selection picked. Everything past this point loads whatever it
  // names, so the rotation needs no load path of its own.
  const String& activeId(const Settings& s) const;
  // Moves the rotation on, and seeds it from the selection when it starts or
  // when a new theme is selected under it.
  void rotate(const Settings& s);
  void advance();
  // A theme a rotation cannot load is passed over at once rather than holding
  // the screen for a whole dwell. Stopping after a lap (MaxThemes in
  // ThemeWeb.cpp) means a /themes where nothing loads shows its error instead
  // of turning over packages forever.
  static constexpr uint8_t MaxSkips = 16;
  String   rotateId_;          // empty while the rotation is off or unseeded
  String   rotateFrom_;        // the selection it was seeded from
  uint32_t rotateAt_ = 0;      // millis() of the last change
  uint8_t  rotateSkips_ = 0;   // packages passed over since one last loaded
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
};
extern ThemeMode g_themeMode;
#endif
