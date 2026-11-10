#ifndef BETON_CD_RIPPER_H
#define BETON_CD_RIPPER_H

#include "CDRipSettings.h"
#include "MediaItem.h"

#include <Messenger.h>
#include <OS.h>
#include <String.h>

#include <atomic>
#include <vector>

/**
 * @struct CDRipJob
 * @brief Everything one rip needs; copied into the worker thread.
 */
struct CDRipJob {
  std::vector<MediaItem> tracks; ///< Tracks to rip, `path` = cdda WAV file.
  RipFormat format = RipFormat::FLAC;
  BString directory; ///< Absolute output folder.
  BString pattern;   ///< RipNamePattern for each track's relative path.
};

/**
 * @class CDRipper
 * @brief Reads tracks off a mounted audio CD and encodes them, on its own
 *        thread, reporting to a messenger.
 *
 * Sends MSG_CD_RIP_PROGRESS (at most ~5 a second), MSG_CD_RIP_TRACK_DONE per
 * track and one MSG_CD_RIP_FINISHED at the end, including after Cancel().
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

  /**
   * @brief The absolute output path (with extension) for @p track.
   */
  static BString DestinationFor(const CDRipJob &job, const MediaItem &track);

private:
  static status_t _ThreadEntry(void *data);
  void _Run();
  status_t _RipTrack(size_t index, const BString &dest, BString &error);
  void _SendProgress(size_t index, off_t trackBytesDone, bool force);
  void _FinishFile(const BString &dest, const MediaItem &track);

  CDRipJob fJob;
  BMessenger fTarget;
  thread_id fThread = -1;
  std::atomic<bool> fCancel{false};
  std::atomic<bool> fRunning{false};

  off_t fTotalBytes = 0;
  off_t fBytesBefore = 0; ///< Bytes of all tracks before the current one.
  bigtime_t fLastProgress = 0;
};

#endif // BETON_CD_RIPPER_H
