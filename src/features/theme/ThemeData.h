#pragma once
#include "ThemeEngine.h"
#include <algorithm>
#include <atomic>
#include <memory>

namespace smalltv {
constexpr size_t MaxDataBody = 2048;
constexpr uint32_t DataTimeoutMs = 3000;

// How long until a source is fetched again. A good fetch waits its declared
// interval; a failed one comes back on a short, doubling backoff instead.
// Without this, one refused fetch costs a source its whole interval -- 300 s
// by default -- and the fields it feeds sit at "--" until then. The common
// refusal is transient and clears in seconds: both fetch paths give up when
// the heap is momentarily too low (ThemeDataClient.cpp wants 18000 B free,
// and 8192 B more for the task's stack off the ESP8266), and the moment that
// is most likely is the tick a theme is loaded in, just after the package
// parse has peaked the heap.
//
// Same shape and cap as the ticker's ladder (symbolPeriodMs in
// ticker/StockClient.cpp): once the ladder is spent a source settles into its
// normal cadence and keeps retrying there, so a genuinely dead endpoint is
// never given up on. The cap is a floor on frequency -- a failure never makes
// a source wait longer than its own interval -- so a source polled faster than
// a ladder step simply keeps its own cadence.
// The first retry is quick, because the overwhelmingly common failure is the
// heap dip around a load and it has usually passed by the time the repaint
// finishes. From the second it backs off properly, so a source that is really
// unreachable is not hammered.
constexpr uint32_t DataRetryMs = 2000;      // first retry after a failure
constexpr uint32_t DataRetryStepMs = 5000;  // second retry; doubles from there
constexpr uint8_t DataRetryMax = 5;         // ladder: 2 s, 5, 10, 20, 40

inline uint32_t dataRetryMs(uint32_t intervalSec, uint8_t fails) {
  const uint32_t interval = intervalSec * 1000UL;
  if (!fails || fails > DataRetryMax) return interval;   // ladder spent -> normal cadence
  uint32_t back = DataRetryMs;
  if (fails > 1) {
    back = DataRetryStepMs;
    for (uint8_t i = 2; i < fails; ++i) back <<= 1;
  }
  return back < interval ? back : interval;              // never slower than a good fetch
}

// Stream supplies a monotonic clock and nonblocking socket reads. Limit total
// elapsed time, including slow trickles; a socket's idle timeout is insufficient.
// `length` is the declared Content-Length, or -1 for chunked/unknown length
// (HTTPClient already de-chunks the stream; here that just means "read until
// the connection closes" instead of "read until N bytes"), capped either way
// at MaxDataBody so a source cannot force an unbounded allocation.
template <class Stream>
bool readDataBody(Stream& stream, int length, std::string& body) {
  body.clear();
  if (length == 0 || length < -1 || length > static_cast<int>(MaxDataBody)) return false;
  body.reserve(length > 0 ? static_cast<size_t>(length) : MaxDataBody);
  const uint32_t started = stream.now();
  uint8_t buffer[128];
  for (;;) {
    if (stream.cancelled() || uint32_t(stream.now() - started) >= DataTimeoutMs) return false;
    if (length > 0 && body.size() == static_cast<size_t>(length)) return true;
    int available = stream.available();
    if (available > 0) {
      const size_t limit = length > 0 ? static_cast<size_t>(length) : MaxDataBody;
      if (body.size() == limit) return false;
      size_t count = std::min(sizeof(buffer), std::min(static_cast<size_t>(available), limit - body.size()));
      int read = stream.read(buffer, count);
      if (read > 0) {
        body.append(reinterpret_cast<const char*>(buffer), read);
        continue;
      }
    } else if (!stream.connected()) {
      // On the ESP8266, available() lets the network stack run when it finds
      // nothing and still returns 0, even if the rest of the reply arrived
      // with the server's close meanwhile. Those bytes are waiting: read them.
      if (stream.available() > 0) continue;
      return length < 0 && !body.empty();
    }
    stream.idle();
  }
}

// One in-flight (or just-completed) fetch, shared between the display loop
// and whatever actually performs the request -- a FreeRTOS task on ESP32, or
// a direct synchronous call on ESP8266 (see startThemeDataFetch in
// ThemeDataClient.cpp for both). `cancelled_`/`finished_` are the only fields
// either side may touch after construction, so they are atomic; `source`,
// `index`, and `values` are written once by their respective owner and never
// contended. `finish()`'s release / `finished()`'s acquire pairing is what
// makes `values` (written just before finish()) visible to the reader once
// finished() observes true.
struct DataFetch {
  ThemeDataSource source;
  size_t index;
  std::vector<ThemeValue> values;
  bool success = false;
  DataFetch(const ThemeDataSource& s, size_t i) : source(s), index(i) {}
  void cancel() { cancelled_.store(true); }
  bool cancelled() const { return cancelled_.load(); }
  void finish(bool ok) {
    success = ok;
    finished_.store(true, std::memory_order_release);
  }
  bool finished() const { return finished_.load(std::memory_order_acquire); }
 private:
  std::atomic<bool> cancelled_{false}, finished_{false};
};

// The worker owns a shared reference, so changing/unloading a theme never frees
// its request underneath it. Only the display loop consumes completed values.
class DataRequests {
 public:
  ~DataRequests() { cancel(); }
  bool busy() const { return bool(pending_); }
  // Refuses a second request while one is already pending -- callers must
  // check busy() first; this only guards against a caller that does not.
  std::shared_ptr<DataFetch> start(const ThemeDataSource& source, size_t index) {
    if (pending_) return {};
    pending_ = std::make_shared<DataFetch>(source, index);
    return pending_;
  }
  void cancel() { if (pending_) pending_->cancel(); }
  // Returns the finished result once, or nothing if still pending or if it
  // was cancelled (a cancelled fetch's values are stale by definition, e.g.
  // from a theme that has since been switched away from).
  std::shared_ptr<DataFetch> take() {
    if (!pending_ || !pending_->finished()) return {};
    auto result = std::move(pending_);
    return result->cancelled() ? std::shared_ptr<DataFetch>{} : result;
  }
 private:
  std::shared_ptr<DataFetch> pending_;
};
}
