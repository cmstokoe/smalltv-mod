#pragma once
// The device's base URL for theme data sources, and the one placeholder a
// theme may write in place of a hardcoded origin.
//
// A package bakes its data URLs into the manifest, so a theme that fetches from
// your own server hardcodes that server's address and cannot follow it between
// a LAN address, a staging host and a public name without being repacked. A
// source may instead write "{base}/path", and the device supplies the origin.
//
// Deliberately free of Arduino, config.h and ThemeEngine.h: Settings.cpp,
// ThemeEngine.cpp and the native tests all include this, and the theme tests
// compile with only -Isrc/features/theme.
#include <string>

namespace smalltv {

// Long enough for a host with a path; short enough to bound what an import can
// put in RAM. A constexpr rather than a config.h macro so this header stays
// includable from the native tests (the same reason MaxDataBody is one).
constexpr size_t MaxThemeBase = 96;
constexpr char ThemeBaseToken[] = "{base}";
constexpr size_t ThemeBaseTokenLen = 6;   // strlen(ThemeBaseToken)

// True when `base` can serve as the origin of a data URL: empty -- the feature
// is simply off, and a theme that asks for it will fail its fetch visibly --
// or an http/https URL with a host. A path is allowed, so
// http://dashboard.example.net/smalltv is a valid base.
//
// These are the rules ThemeEngine.cpp applies to a manifest url, which is why
// both call this rather than keeping two copies that can drift.
inline bool validThemeBase(const char* base) {
  if (!base) return false;
  const std::string s(base);
  if (s.empty()) return true;
  if (s.size() > MaxThemeBase) return false;
  // Printable ASCII only. `char` may be signed, so a byte >= 128 reads as
  // negative here and is rejected with the control characters.
  for (size_t i = 0; i < s.size(); ++i)
    if (s[i] < 32 || s[i] > 126) return false;
  const size_t host = s.rfind("https://", 0) == 0 ? 8 : s.rfind("http://", 0) == 0 ? 7 : std::string::npos;
  if (host == std::string::npos || host == s.size()) return false;
  return s[host] != '/' && s[host] != '?' && s[host] != '#';
}

// Offset just past the "{base}" placeholder, or npos when the url does not use
// it.
//
// Three spellings are accepted: "{base}/x", "http://{base}/x" and
// "https://{base}/x". The bare form is the honest one, but SmallTV Studio --
// the browser editor most themes are authored in -- validates this field with
// /^https?:\/\/[^\/?#]/ (src/core/core.ts) and would refuse to export a package
// containing it. The two scheme-prefixed spellings satisfy that regex while
// meaning exactly the same thing here.
//
// The scheme written before the placeholder is DISCARDED: the whole origin,
// scheme included, comes from the device's base. That keeps one meaning for
// "{base}" rather than two, and it is what lets a server move between http and
// https without any theme being repacked -- the point of the feature.
inline size_t themeBaseEnd(const std::string& url) {
  size_t at = 0;
  if (url.compare(0, 7, "http://") == 0) at = 7;
  else if (url.compare(0, 8, "https://") == 0) at = 8;
  if (url.compare(at, ThemeBaseTokenLen, ThemeBaseToken) != 0) return std::string::npos;
  return at + ThemeBaseTokenLen;
}

// True when a manifest url defers its origin to the device's base.
inline bool usesThemeBase(const std::string& url) {
  return themeBaseEnd(url) != std::string::npos;
}

// Joins a theme's url to the device's base, or returns it untouched when it
// does not open with the placeholder. The base is NOT percent-encoded: it is
// itself a URL, and encoding it would destroy the scheme and host. What makes
// that safe is that it never comes from the package -- only from the device's
// own settings, validated by validThemeBase() before it is stored.
//
// With no base set this yields a path with no scheme, which HTTPClient refuses;
// the source is then reported through the ordinary failure path rather than
// silently never being tried.
inline std::string expandThemeUrl(const std::string& url, const std::string& base) {
  const size_t end = themeBaseEnd(url);
  if (end == std::string::npos) return url;
  return base + url.substr(end);
}

}
