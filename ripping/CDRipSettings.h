#ifndef BETON_CD_RIP_SETTINGS_H
#define BETON_CD_RIP_SETTINGS_H

#include <String.h>
#include <SupportDefs.h>

/** Output formats offered by the CD ripper. Values are stored in settings. */
enum class RipFormat : int32 {
  MP3_320 = 0, ///< MP3, 320 kbps CBR (LAME).
  FLAC = 1,    ///< FLAC, lossless.
  WAV = 2      ///< WAV, 16-bit PCM, uncompressed.
};

/** File name extension for @p format, without the dot. */
const char *RipFormatExtension(RipFormat format);

/**
 * @struct CDRipSettings
 * @brief The ripper's remembered choices, kept in
 *        `~/config/settings/Beton/cd_rip.settings`.
 */
struct CDRipSettings {
  RipFormat format = RipFormat::FLAC;
  BString directory; ///< Absolute folder the pattern is resolved against.
  BString pattern;   ///< Folder/file name pattern, see RipNamePattern.

  /** Loads saved settings, filling anything missing with defaults. */
  void Load();
  /** Writes the settings back to disk. */
  void Save() const;

  /** The pattern used when none was saved. */
  static const char *DefaultPattern();
  /** First library folder, or ~/music when no library folder is set up. */
  static BString DefaultDirectory();
};

#endif // BETON_CD_RIP_SETTINGS_H
