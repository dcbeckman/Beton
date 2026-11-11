#ifndef BETON_CD_RIPPER_H
#define BETON_CD_RIPPER_H

#include "AccurateRip.h"
#include "CDRipSettings.h"
#include "CdDriveReader.h"
#include "LoudnessMeter.h"
#include "MediaItem.h"

#include <File.h>
#include <Messenger.h>
#include <OS.h>
#include <String.h>

#include <atomic>
#include <memory>
#include <vector>

/** AccurateRip result of one track, sent as "ar" in MSG_CD_RIP_TRACK_DONE. */
enum RipVerdict : int32 {
  kRipNotChecked = 0, ///< Verification off, or the lookup failed.
  kRipAccurate = 1,   ///< Matches the database; "ar_confidence" rips agree.
  kRipNoMatch = 2,    ///< The disc is in the database, this track differs.
  kRipNotInDatabase = 3
};

/**
 * @struct CDRipJob
 * @brief Everything one rip needs; copied into the worker thread.
 */
struct CDRipJob {
  BString discPath;              ///< Mount point of the cdda volume.
  std::vector<MediaItem> tracks; ///< Every track of the disc, in disc order.
  RipFormat format = RipFormat::FLAC;
  BString directory; ///< Absolute output folder.
  BString pattern;   ///< RipNamePattern for each track's relative path.

  bool verify = true;       ///< Look the disc up in AccurateRip.
  bool offsetKnown = false; ///< False: detect it from AccurateRip.
  int32 offset = 0;         ///< Drive read offset in samples.
  /** Per track (same order); false = skip it. Empty = rip every track. */
  std::vector<bool> selected;
};

/**
 * @class CDRipper
 * @brief Reads a mounted audio CD and encodes its tracks on its own thread,
 *        reporting to a messenger.
 *
 * Before ripping it looks the disc up in AccurateRip and, if the drive's read
 * offset isn't known yet, finds it by matching one track against the
 * database (MSG_CD_RIP_OFFSET). Every track is then read shifted by that
 * offset, so the files hold exactly the audio other rippers get, and its
 * AccurateRip checksum is compared as it is written.
 *
 * Sends MSG_CD_RIP_STATUS for the lookup steps, MSG_CD_RIP_PROGRESS (at most
 * ~5 a second), MSG_CD_RIP_TRACK_DONE per track and one MSG_CD_RIP_FINISHED
 * at the end, including after Cancel().
 *
 * When the drive reports C2 errors, the disc is read straight from the drive
 * (CdDriveReader) and every track's read errors are counted, CDEx-style: a
 * sector the drive flags is re-read up to kRetries times and counts as fixed
 * if a re-read comes back clean, as damaged if not. A sector that can't be
 * read at all is filled with silence and the rip goes on. Without C2 support
 * the cdda files are read as before and errors are not checked.
 *
 * Every track is also measured for ReplayGain 2.0 as it is encoded; its
 * track gain is tagged straight away, and the album gain once the whole disc
 * has been ripped in one go.
 *
 * Each track is written to "<name>.part" and renamed once complete, so a
 * cancelled or failed track never leaves a truncated file behind. A file that
 * already exists is left alone and reported as skipped (status
 * B_FILE_EXISTS), which makes re-running a partly failed rip cheap.
 */
class CDRipper {
public:
  CDRipper(const CDRipJob &job, BMessenger target);
  /** Cancels and waits for the thread. */
  ~CDRipper();

  status_t Start();
  /** Asks the thread to stop after the current read; returns at once. */
  void Cancel();
  /** Blocks until the thread has exited. */
  void Wait();
  bool IsRunning() const { return fRunning.load(); }

  /** The absolute output path (with extension) for @p track. */
  static BString DestinationFor(const CDRipJob &job, const MediaItem &track);

  /** Read errors of one track, in sectors (1/75 s each). */
  struct ReadStats {
    int32 flagged = 0;    ///< Flagged by C2 on the first read.
    int32 fixed = 0;      ///< Of those, clean after a re-read.
    int32 damaged = 0;    ///< Still flagged after every re-read.
    int32 unreadable = 0; ///< Could not be read; filled with silence.
  };

private:
  /** Where one track's audio sits on the disc and in its cdda file. */
  struct Span {
    std::unique_ptr<BFile> file;
    off_t dataOffset = 0; ///< Byte offset of the PCM data in the file.
    int64 firstFrame = 0; ///< Disc position of the track's first frame.
    int64 frames = 0;
  };

  static status_t _ThreadEntry(void *data);
  /** Reads a track ahead of the encoder, on its own thread. */
  struct ReadAhead;
  static status_t _ReadAheadEntry(void *data);
  void _Run();
  status_t _BuildLayout(BString &error);
  status_t _ReadDisc(int64 first, size_t count, uint32 *out, BString &error);
  status_t _ReadDiscFromFiles(int64 first, size_t count, uint32 *out,
                              BString &error);
  void _OpenDrive();
  void _ReadDiscFromDrive(int64 first, size_t count, uint32 *out);
  void _ReadDriveChunk(uint32 sector, uint32 count);
  status_t _DriveRead(uint32 lba, uint32 count, uint8 *audio, uint8 *c2,
                      bool firstAttempt);
  void _LookUp();
  void _DetectOffset();
  status_t _RipTrack(size_t index, const BString &dest,
                     AccurateRip::Checksum &checksum, LoudnessMeter &loudness,
                     BString &error);
  void _WriteAlbumGain(const std::vector<BString> &files,
                       const std::vector<double> &blocks, double peak);
  void _FinishFile(const BString &dest, const MediaItem &track);
  void _SendStatus(const char *text);
  void _SendProgress(size_t index, int64 framesDone, bool force);

  CDRipJob fJob;
  BMessenger fTarget;
  thread_id fThread = -1;
  std::atomic<bool> fCancel{false};
  std::atomic<bool> fRunning{false};

  std::vector<Span> fSpans;
  int64 fDiscFrames = 0;

  std::unique_ptr<CdDriveReader> fDrive; ///< Null: read the cdda files.
  BString fDriveNote;   ///< Why errors aren't checked, for the summary.
  uint32 fFirstLba = 0; ///< Sector of the disc's first frame.
  uint32 fLeadOut = 0;
  ReadStats fStats;     ///< Of the track being ripped.
  int32 fTestHook = 0;  ///< BETON_RIP_TEST_TAILS, debug runs only.
  /** The last chunk read from the drive, so sectors shared by neighbouring
   *  reads (the offset makes them straddle sectors) are read once. */
  uint32 fCacheSector = 0;
  uint32 fCacheCount = 0;
  std::vector<uint8> fCacheAudio;

  AccurateRip::Database fDatabase;
  RipVerdict fLookup = kRipNotChecked; ///< kRipAccurate = entry loaded.
  BString fLookupNote; ///< Why verification is off, for the summary.
  bool fOffsetDetected = false;

  int64 fFramesBefore = 0; ///< Frames of the selected tracks before this one.
  int64 fTotalFrames = 0;  ///< Frames of all selected tracks.
  bigtime_t fLastProgress = 0;
};

#endif // BETON_CD_RIPPER_H
