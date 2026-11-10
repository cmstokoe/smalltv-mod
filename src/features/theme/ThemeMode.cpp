#include "ThemeMode.h"
#if WITH_THEME
#include "Clock.h"
#include "Gfx.h"
#include "Platform.h"
#include "ThemeCycle.h"
#include "ThemeDataClient.h"
#include "ThemeOrder.h"
#include <Arduino_GFX_Library.h>
#include <font/glcdfont.h>
#include <algorithm>

ThemeMode g_themeMode;
namespace {
// Adapts the portable engine's Display interface to this device's actual
// panel: glyphColumn() reads the built-in font from flash (PROGMEM), and
// row() applies the usual panel color tint before handing the row to the
// GFX driver. yield() every 16 rows keeps a large dirty region from starving
// WiFi/other tasks during a long render.
class Panel : public smalltv::Display {
 public:
  uint8_t glyphColumn(uint8_t c, uint8_t col) const override {
    return pgm_read_byte(font + unsigned(c) * 5 + col);
  }
  void row(int x, int y, const uint16_t* pixels, int count) override {
    uint16_t tinted[240];
    for (int i = 0; i < count; ++i) tinted[i] = gfxTint(pixels[i]);
    gfxDev()->draw16bitRGBBitmap(x, y, tinted, count, 1);
    if ((y & 15) == 15) yield();
  }
};
// What a load needs before it starts: a package parse (ThemeMode.h). A failed
// allocation mid-load restarts the ESP8266, so a load that would not fit
// waits instead.
constexpr uint32_t LoadFreeHeap = ThemeMode::ParseFreeHeap;
constexpr uint32_t LoadLargestBlock = ThemeMode::ParseLargestBlock;
const char* const WaitingForMemory = "Waiting for free memory";
std::string str(const String& s) { return std::string(s.c_str()); }
}
void ThemeMode::begin(const Settings& s) {
  pinnedId_ = s.themeId;
  LittleFS.mkdir("/themes");
  // An interrupted install leaves its staging file behind; boot is the one
  // point it is safe to remove without racing a request in progress.
  LittleFS.remove("/themes/.upload");
}
// Drops every bit of state for the currently loaded theme -- the open
// package/file, any in-flight or fetched data, and the engine itself -- so
// the next service() call starts a clean load from scratch.
void ThemeMode::unload() {
  dataRequests_.cancel();
  dataRequests_.take();
  package_.reset();
  file_.reset();
  dataNextMs_.clear();
  dataScheduled_.clear();
  dataFails_.clear();
  dataValues_.clear();
  dataCursor_ = 0;
  engine_.setValues({});
  engine_.setTheme(smalltv::Theme{}, millis());
  loadedId_ = "";
  attempted_ = messageDrawn_ = keepData_ = false;
  error_ = "";
}
void ThemeMode::release(uint32_t needFree, uint32_t needLargestBlock) {
  if (!package_ || (ESP.getFreeHeap() >= needFree && platformMaxFreeBlock() >= needLargestBlock)) return;
  // A fetch in flight is dropped; its source is still due, so the reload
  // fetches it again.
  dataRequests_.cancel();
  dataRequests_.take();
  package_.reset();
  file_.reset();
  engine_.setValues({});
  engine_.setTheme(smalltv::Theme{}, millis());
  attempted_ = messageDrawn_ = false;
  keepData_ = true;
}
// Collects a finished fetch (if any) and, with at most one request in flight
// at a time, starts the next source whose interval has elapsed, round-robin
// over sources so no single slow/failing one starves the others.
void ThemeMode::refreshData() {
  const auto& sources = engine_.theme().data;
  if (auto result = dataRequests_.take()) {
    const size_t index = result->index;
    if (result->success) {
      // Merge into the running set by key rather than replace it wholesale:
      // a source's fields persist across refreshes of the *other* sources, so
      // a slow source does not blank fields a fast one already delivered.
      for (auto& value : result->values) {
        auto existing = std::find_if(dataValues_.begin(), dataValues_.end(), [&](const smalltv::ThemeValue& old) { return old.key == value.key; });
        if (existing == dataValues_.end()) dataValues_.push_back(std::move(value));
        else existing->value = std::move(value.value);
      }
      engine_.setValues(dataValues_);
      dataFails_[index] = 0;
    } else if (dataFails_[index] < 255) {
      // Only the first failure of a streak, so a source that is down does not
      // fill the console: the ladder below already spaces out the retries.
      // Without this line a refused fetch is invisible -- the field just shows
      // "--", which is also what a null value or a wrong JSON path renders as.
      if (!dataFails_[index]) {
        Serial.printf("theme: source %u (%s) fetch failed, heap %u\n", (unsigned)index,
                      sources[index].id.c_str(), (unsigned)ESP.getFreeHeap());
      }
      // A refused or failed fetch comes back on the short ladder instead of
      // waiting out a full interval. Rescheduling a failure like a success
      // left the fields it feeds at "--" for the whole interval -- 300 s by
      // default -- and, with a theme cycle running on a shorter dwell, for
      // good: the theme is unloaded before the retry falls due, and the
      // reload clears the schedule so the next visit fails the same way.
      ++dataFails_[index];
    }
    // Schedule from completion, preserving the interval even for slow sources.
    dataNextMs_[index] = millis() + smalltv::dataPeriodMs(sources[index].interval, dataFails_[index]);
  }
  if (sources.empty() || dataRequests_.busy() || WiFi.status() != WL_CONNECTED) return;
  // Scan forward from `dataCursor_` for the first source that is due (an
  // unscheduled one always is), start it, and advance the cursor past it --
  // so a source that just fired goes to the back of the round-robin queue.
  for (size_t offset = 0; offset < sources.size(); ++offset) {
    size_t i = (dataCursor_ + offset) % sources.size();
    if (dataScheduled_[i] && static_cast<int32_t>(millis() - dataNextMs_[i]) < 0) continue;
    auto request = dataRequests_.start(sources[i], i);
    if (!startThemeDataFetch(request)) request->finish(false);
    dataScheduled_[i] = true;
    dataCursor_ = (i + 1) % sources.size();
    break;
  }
}
String ThemeMode::activeId(const Settings& s) const {
  return (s.themeCycleSec && cycleId_.length()) ? cycleId_ : s.themeId;
}
// Accrues the time this theme has actually been on screen and advances when
// the dwell is up.
//
// main.cpp calls service() only on the active mode, so the gap between two
// calls IS on-screen time -- no bookkeeping is needed for the carousel, the
// notification overlay, the button info screen or AP mode, which all simply
// stop calling. wake() clears lastTickMs_ so the gap while away is skipped.
void ThemeMode::serviceCycle(const Settings& s) {
  if (!s.themeCycleSec) {
    // Switched off: drop the RAM-only entry so themeId takes the screen back.
    cycleId_ = "";
    cycleElapsedMs_ = lastTickMs_ = 0;
    cycleSkips_ = 0;
    return;
  }
  const uint32_t now = millis();
  if (lastTickMs_) cycleElapsedMs_ += now - lastTickMs_;
  lastTickMs_ = now;
  if (cycleElapsedMs_ < (uint32_t)s.themeCycleSec * 1000UL) return;
  cycleElapsedMs_ = 0;
  cycleSkips_ = 0;   // a dwell ran to completion, so the lap is healthy
  advanceCycle(s);
}
bool ThemeMode::advanceCycle(const Settings& s) {
  const String current = activeId(s);
  String next;

  if (s.themeCycleOrder == THEME_ORDER_MANDATED) {
    // The order file is maintained on install and delete, so it is normally
    // complete. If it is missing or unreadable the cycle falls through to
    // sorted rather than stalling on a device nobody can easily inspect.
    ThemeFile order;
    if (order.open("/themes/.order")) next = smalltv::orderSuccessor(order, str(current)).c_str();
  }

  if (!next.length()) {
    const uint8_t order = (s.themeCycleOrder == THEME_ORDER_SHUFFLE) ? smalltv::CycleShuffle
                                                                    : smalltv::CycleSorted;
    smalltv::CycleCursor cursor(order, lapSeed_, str(current));
    eachInstalledTheme([&cursor](const String& id) { cursor.offer(str(id)); });
    if (cursor.empty()) return false;
    next = cursor.chosen().c_str();
    if (cursor.wrapped() && order == smalltv::CycleShuffle) {
      // The wrap is the end of a lap: a fresh seed is a fresh permutation.
      // Excluding what is on screen stops the new lap opening on the theme
      // that just finished, which with two installed would be half of them.
      lapSeed_ = lapSeed_ * 1664525u + 1013904223u + micros();
      smalltv::CycleCursor restart(order, lapSeed_, std::string(), str(current));
      eachInstalledTheme([&restart](const String& id) { restart.offer(str(id)); });
      if (!restart.empty()) next = restart.chosen().c_str();
    }
  }

  if (!next.length() || next == current) return false;
  cycleId_ = next;
  return true;
}
// Called when the theme should be redrawn from scratch, e.g. when a
// notification overlay just finished. Switches to a different theme by
// unloading first; otherwise leaves the loaded package alone and only marks
// the engine's next update() as a full repaint.
void ThemeMode::invalidate(const Settings& s) {
  // A new pinned selection restarts the cycle from it; any other settings
  // change leaves the cycle where it is, so saving an unrelated field does not
  // jump the display back to themeId.
  if (pinnedId_ != s.themeId) {
    pinnedId_ = s.themeId;
    cycleId_ = "";
    cycleElapsedMs_ = 0;
    cycleSkips_ = 0;
  }
  const bool onScreen = s.mode == MODE_THEME || (s.mode == MODE_CAROUSEL && s.carouselTheme);
  if (loadedId_ != activeId(s) || !onScreen) unload();
  // A failed load may be retried after installation or settings changes.
  if (!package_) attempted_ = false;
  engine_.invalidate();
  messageDrawn_ = false;
}
// Per-frame entry point: loads the selected theme at most once per selection
// (via `attempted_`, so a failed load is not retried every call -- only on
// the next invalidate()/selection change), then renders it. `attempted_` and
// `loadedId_` together are the load state machine; everything past the load
// block runs every call once a package is open.
void ThemeMode::service(const Settings& s) {
  serviceCycle(s);
  if (loadedId_ != activeId(s)) {
    unload();
    loadedId_ = activeId(s);
  }
  // A web request, or the buffers of its reply, can briefly hold the heap a
  // load needs; the load then waits, checked again every frame.
  const bool loadFits = ESP.getFreeHeap() >= LoadFreeHeap && platformMaxFreeBlock() >= LoadLargestBlock;
  if (!attempted_ && !loadFits && error_ != WaitingForMemory) {
    error_ = WaitingForMemory;
    messageDrawn_ = false;
  }
  if (!attempted_ && loadFits) {
    attempted_ = true;
    std::string error;
    smalltv::Theme theme;
    if (!smalltv::validId(loadedId_.c_str())) error = "Select an installed theme";
    else {
      file_.reset(new ThemeFile);
      if (!file_->open("/themes/" + loadedId_ + ".stheme")) error = "Theme file not found";
      else {
        package_.reset(new smalltv::Package(*file_));
        if (!package_->load(theme, error)) package_.reset();
        else if (theme.id != loadedId_.c_str()) {
          error = "Theme ID mismatch";
          package_.reset();
        }
      }
    }
    if (!error.empty()) {
      error_ = error.c_str();
      package_.reset();
      file_.reset();
    } else {
      // After a release the same package loads again, so its sources match.
      if (!keepData_ || dataNextMs_.size() != theme.data.size()) {
        dataNextMs_.assign(theme.data.size(), 0);
        dataScheduled_.assign(theme.data.size(), false);
        dataFails_.assign(theme.data.size(), 0);
        dataValues_.clear();
        dataCursor_ = 0;
      }
      // Hold every source off until the heap has settled after the parse and
      // the repaint below. Only ever pushes an attempt later, so a source
      // already waiting out a longer interval keeps it.
      const uint32_t settleAt = millis() + smalltv::DataSettleMs;
      for (size_t i = 0; i < dataNextMs_.size(); ++i) {
        if (!dataScheduled_[i] || (int32_t)(dataNextMs_[i] - settleAt) < 0) {
          dataNextMs_[i] = settleAt;
          dataScheduled_[i] = true;
        }
      }
      engine_.setTheme(std::move(theme), millis());
      engine_.setValues(dataValues_);
      error_ = "";
      cycleSkips_ = 0;
    }
  }
  if (!package_) {
    // While cycling, a package that will not load is skipped and the cycle
    // moves on: gfxMessage() starts with fillScreen(C_BLACK), so parking on
    // one bad package would put a black frame between every pair of good
    // ones. The heap gate is a stall rather than a failure -- it retries next
    // frame by design -- so it neither counts toward the lap limit nor
    // advances past the theme. After a lap's worth of genuine failures the
    // cycle stops and the error is reported as usual, so a /themes where
    // nothing loads still says so.
    if (s.themeCycleSec && attempted_ && error_ != WaitingForMemory &&
        cycleSkips_ < MaxCycleSkips && advanceCycle(s)) {
      ++cycleSkips_;
      cycleElapsedMs_ = 0;
      return;
    }
    if (!messageDrawn_) {
      gfxMessage("THEMES", error_.c_str(), C_WHITE);
      messageDrawn_ = true;
    }
    return;
  }
  refreshData();
  tm t{};
  bool valid = clockNow(t);
  Panel panel;
  engine_.setLanguage(smalltv::languageFromCode(s.clock.lang.c_str()));
  auto dirty = engine_.update(millis(), valid ? &t : nullptr);
  // A render failure (a corrupt or truncated asset read mid-frame) is treated
  // like a load failure: drop the package and show an error rather than
  // risk repeatedly failing on every subsequent frame.
  if (!smalltv::render(engine_, dirty, *package_, panel)) {
    dataRequests_.cancel();
    dataRequests_.take();
    error_ = "Could not read theme asset";
    package_.reset();
    file_.reset();
    messageDrawn_ = false;
  }
}
#endif
