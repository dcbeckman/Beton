#include "CDRipSettings.h"

#include "MediaLibraryCache.h"

#include <File.h>
#include <FindDirectory.h>
#include <Message.h>
#include <Path.h>

#include <vector>

static const char *kSettingsFile = "Beton/cd_rip.settings";

const char *RipFormatExtension(RipFormat format) {
  switch (format) {
  case RipFormat::MP3_320:
    return "mp3";
  case RipFormat::FLAC:
    return "flac";
  case RipFormat::WAV:
    return "wav";
  }
  return "flac";
}

const char *CDRipSettings::DefaultPattern() {
  return "{albumartist}/{album}/{track} - {title}";
}

BString CDRipSettings::DefaultDirectory() {
  // Ripping into the library means the new files show up there right away.
  std::vector<BString> dirs;
  MediaLibraryCache::LoadDirectories(dirs);
  if (!dirs.empty())
    return dirs.front();

  BPath home;
  if (find_directory(B_USER_DIRECTORY, &home) != B_OK)
    return "/boot/home/music";
  home.Append("music");
  return home.Path();
}

void CDRipSettings::Load() {
  format = RipFormat::FLAC;
  directory.Truncate(0);
  pattern.Truncate(0);

  BPath path;
  if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) == B_OK) {
    path.Append(kSettingsFile);
    BFile file(path.Path(), B_READ_ONLY);
    BMessage archive;
    if (file.InitCheck() == B_OK && archive.Unflatten(&file) == B_OK) {
      int32 value = 0;
      if (archive.FindInt32("format", &value) == B_OK &&
          value >= (int32)RipFormat::MP3_320 && value <= (int32)RipFormat::WAV)
        format = (RipFormat)value;
      archive.FindString("directory", &directory);
      archive.FindString("pattern", &pattern);
    }
  }

  if (directory.IsEmpty())
    directory = DefaultDirectory();
  if (pattern.IsEmpty())
    pattern = DefaultPattern();
}

void CDRipSettings::Save() const {
  BPath path;
  if (find_directory(B_USER_SETTINGS_DIRECTORY, &path, true) != B_OK)
    return;
  path.Append(kSettingsFile);

  BMessage archive;
  archive.AddInt32("format", (int32)format);
  archive.AddString("directory", directory);
  archive.AddString("pattern", pattern);

  BFile file(path.Path(), B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
  if (file.InitCheck() == B_OK)
    archive.Flatten(&file);
}
