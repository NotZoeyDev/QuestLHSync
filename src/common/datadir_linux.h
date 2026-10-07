// The driver's and the dashboard app's data folder on Linux: $XDG_DATA_HOME/QuestLHSync (~/.local/share/QuestLHSync),
// the counterpart of %LOCALAPPDATA%\QuestLHSync.
#pragma once
#ifndef _WIN32
#include <pwd.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <string>

static inline std::string QlhsDataDir() {
  std::filesystem::path d;
  const char *x = getenv("XDG_DATA_HOME"), *h = getenv("HOME");
  if (x && *x) d = std::filesystem::path(x) / "QuestLHSync";
  else if (h && *h) d = std::filesystem::path(h) / ".local" / "share" / "QuestLHSync";
  else if (passwd *pw = getpwuid(getuid())) d = std::filesystem::path(pw->pw_dir) / ".local" / "share" / "QuestLHSync";
  std::error_code ec;
  std::filesystem::create_directories(d, ec);
  return d.string();
}
#endif
