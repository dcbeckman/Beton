#ifndef BETON_ACCURATE_RIP_H
#define BETON_ACCURATE_RIP_H

#include <String.h>
#include <SupportDefs.h>

#include <vector>

/**
 * @namespace AccurateRip
 * @brief Checks ripped tracks against the AccurateRip database.
 *
 * AccurateRip stores, per pressing of a disc, a checksum of every track as
 * other people ripped it and how many rips agreed ("confidence"). A rip whose
 * checksum matches is bit-identical to theirs.
 *
 * Formats follow whipper's implementation (accuraterip-checksum.c, table.py,
 * accurip.py). Beton can look rips up but not submit them: the database only
 * accepts submissions from approved rippers.
 *
 * A drive reads a fixed number of samples early or late (its read offset),
 * which shifts every checksum. FindOffset() works the offset out from the
 * database itself, so no table of drive offsets is needed.
 */
namespace AccurateRip {

/** Frames (stereo samples) AccurateRip leaves out at the start and end of
 *  the disc; any read offset below this never needs data off the disc. */
const int32 kSkipFrames = 2940;

/** The identifiers of one disc, from its table of contents. */
struct DiscId {
  uint32 id1 = 0;
  uint32 id2 = 0;
  uint32 cddb = 0;
  int32 audioTracks = 0;

  bool IsValid() const { return audioTracks > 0; }
  /** The database URL for this disc. */
  BString Url() const;
};

/**
 * @brief Builds the disc ID from the `CD:toc` (and `CD:cddbid`) attributes
 *        Haiku's cdda file system puts on the disc's root folder.
 */
status_t ReadDiscId(const BString &cdPath, DiscId &out);

/** One pressing's checksums as stored in the database. */
struct Response {
  std::vector<uint8> confidence; ///< Per audio track.
  std::vector<uint32> crc;       ///< Per audio track; v1 or v2.
};

/** Everything the database knows about one disc. */
struct Database {
  std::vector<Response> responses;

  bool IsEmpty() const { return responses.empty(); }

  /**
   * @brief Confidence of the best entry matching @p crc for @p track
   *        (0-based), or -1 when nothing matches.
   */
  int32 Match(size_t track, uint32 crc) const;
};

/** Parses a downloaded dBAR file. */
status_t Parse(const void *data, size_t length, const DiscId &id, Database &out);

/**
 * @brief Downloads the entry for @p id.
 * @return B_OK, B_ENTRY_NOT_FOUND if the disc isn't in the database, or an
 *         error with @p error set.
 */
status_t Fetch(const DiscId &id, Database &out, BString &error);

/**
 * @class Checksum
 * @brief Accumulates the v1 and v2 checksums of one track as its frames are
 *        fed in order.
 */
class Checksum {
public:
  /**
   * @param frames Length of the track in frames.
   * @param firstTrack Skip the disc's first kSkipFrames - 1 frames.
   * @param lastTrack Skip the disc's last kSkipFrames frames.
   */
  void Begin(uint32 frames, bool firstTrack, bool lastTrack);
  /** @param frames Little-endian 16-bit stereo frames as 32-bit words. */
  void Add(const uint32 *frames, size_t count);

  uint32 V1() const { return fLow; }
  uint32 V2() const { return fLow + fHigh; }

private:
  uint32 fPosition = 1; ///< 1-based frame number of the next frame.
  uint32 fFrom = 1;
  uint32 fTo = 0;
  uint32 fLow = 0;
  uint32 fHigh = 0;
};

/**
 * @brief Finds the read offset at which a track matches the database.
 *
 * Slides the v1 checksum over every offset in [-range, +range]; v1 is a
 * linear sum, so each step costs O(1).
 *
 * @param frames The track's frames with @p range extra frames on each side:
 *        frames[range] is the track's first frame at offset 0.
 * @param trackFrames Length of the track.
 * @param range Largest offset tried, at most kSkipFrames - 1.
 * @param track 0-based track index (must not be the first or last track).
 * @param db The disc's database entry.
 * @param offset Receives the offset with the most confident match.
 * @param confidence Receives that match's confidence.
 * @return True if any offset matched.
 */
bool FindOffset(const uint32 *frames, uint32 trackFrames, int32 range,
                size_t track, const Database &db, int32 &offset,
                int32 &confidence);

/** The drive's vendor and model ("hp CDDVDW TS-H653R"), or empty. */
BString DriveName(const BString &cdPath);

} // namespace AccurateRip

#endif // BETON_ACCURATE_RIP_H
