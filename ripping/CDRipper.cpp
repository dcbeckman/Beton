#include "CDRipper.h"

#include "AudioFileEncoder.h"
#include "Debug.h"
#include "Messages.h"
#include "MetadataTagIO.h"
#include "RipNamePattern.h"

#include <ByteOrder.h>
#include <Catalog.h>
#include <Directory.h>
#include <Entry.h>
#include <InterfaceDefs.h>
#include <Mime.h>
#include <Path.h>

#include <algorithm>
#include <errno.h>
#include <set>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "CDRipper"

namespace {

/** Frames read per call: 32 CD sectors, about 0.4 s of audio. */
const size_t kChunkFrames = 32 * 588;

const bigtime_t kProgressInterval = 200000;

/** Offsets tried when detecting the drive's read offset. */
const int32 kOffsetRange = AccurateRip::kSkipFrames - 1;

uint32 ReadLE32(const uint8 *p) {
  return (uint32)p[0] | ((uint32)p[1] << 8) | ((uint32)p[2] << 16) |
         ((uint32)p[3] << 24);
}

uint16 ReadLE16(const uint8 *p) { return (uint16)(p[0] | (p[1] << 8)); }

/**
 * Finds the PCM data in a cdda track. The cdda file system always writes the
 * plain 44-byte header, but walking the chunks costs nothing. Only CD audio
 * (44.1 kHz, 16-bit stereo) is accepted: the disc is read as one continuous
 * stream of 4-byte frames across the track files.
 */
status_t ParseWav(BFile &file, off_t fileSize, off_t &dataOffset,
                  off_t &dataSize, BString &error) {
  uint8 riff[12];
  if (file.ReadAt(0, riff, sizeof(riff)) != (ssize_t)sizeof(riff) ||
      memcmp(riff, "RIFF", 4) != 0 || memcmp(riff + 8, "WAVE", 4) != 0) {
    error = "Not a WAV track";
    return B_BAD_DATA;
  }

  bool haveFormat = false;
  off_t pos = 12;
  while (pos + 8 <= fileSize) {
    uint8 chunk[8];
    if (file.ReadAt(pos, chunk, sizeof(chunk)) != (ssize_t)sizeof(chunk))
      break;
    const uint32 size = ReadLE32(chunk + 4);
    const off_t body = pos + 8;

    if (memcmp(chunk, "fmt ", 4) == 0 && size >= 16) {
      uint8 fmt[16];
      if (file.ReadAt(body, fmt, sizeof(fmt)) != (ssize_t)sizeof(fmt))
        break;
      if (ReadLE16(fmt) != 1 || ReadLE16(fmt + 2) != 2 ||
          ReadLE32(fmt + 4) != 44100 || ReadLE16(fmt + 14) != 16) {
        error = "Not CD audio (44.1 kHz, 16-bit stereo)";
        return B_NOT_SUPPORTED;
      }
      haveFormat = true;
    } else if (memcmp(chunk, "data", 4) == 0) {
      if (!haveFormat)
        break;
      dataOffset = body;
      dataSize = std::min<off_t>((off_t)size, fileSize - body);
      return B_OK;
    }
    pos = body + size + (size & 1);
  }

  error = "Could not find the audio data in the track";
  return B_BAD_DATA;
}

/** "Dir/Name.flac" -> "Dir/Name (2).flac" */
BString NumberedPath(const BString &path, int32 n) {
  BString out(path);
  int32 dot = out.FindLast('.');
  int32 slash = out.FindLast('/');
  BString suffix;
  suffix << " (" << n << ")";
  if (dot > slash)
    out.Insert(suffix, dot);
  else
    out << suffix;
  return out;
}

TagData ToTagData(const MediaItem &t) {
  TagData td;
  td.title = t.title;
  td.artist = t.artist;
  td.album = t.album;
  td.albumArtist = t.albumArtist;
  td.genre = t.genre;
  td.year = t.year > 0 ? (uint32)t.year : 0;
  td.track = t.track > 0 ? (uint32)t.track : 0;
  td.trackTotal = t.trackTotal > 0 ? (uint32)t.trackTotal : 0;
  td.disc = t.disc > 0 ? (uint32)t.disc : 0;
  td.lengthSec = t.duration > 0 ? (uint32)t.duration : 0;
  return td;
}

} // namespace

CDRipper::CDRipper(const CDRipJob &job, BMessenger target)
    : fJob(job), fTarget(target) {}

CDRipper::~CDRipper() {
  Cancel();
  Wait();
}

BString CDRipper::DestinationFor(const CDRipJob &job, const MediaItem &track) {
  BString path(job.directory);
  if (!path.EndsWith("/"))
    path << '/';
  path << RipNamePattern::Expand(job.pattern, track) << '.'
       << RipFormatExtension(job.format);
  return path;
}

status_t CDRipper::Start() {
  if (fThread >= 0)
    return B_BUSY;
  fCancel = false;
  fRunning = true;
  fThread = spawn_thread(_ThreadEntry, "cd_ripper", B_LOW_PRIORITY, this);
  if (fThread < 0) {
    fRunning = false;
    return fThread;
  }
  return resume_thread(fThread);
}

void CDRipper::Cancel() { fCancel = true; }

void CDRipper::Wait() {
  if (fThread < 0)
    return;
  status_t ignored;
  wait_for_thread(fThread, &ignored);
  fThread = -1;
}

status_t CDRipper::_ThreadEntry(void *data) {
  static_cast<CDRipper *>(data)->_Run();
  return B_OK;
}

void CDRipper::_SendStatus(const char *text) {
  BMessage msg(MSG_CD_RIP_STATUS);
  msg.AddString("text", text);
  fTarget.SendMessage(&msg);
}

void CDRipper::_SendProgress(size_t index, int64 framesDone, bool force) {
  const bigtime_t now = system_time();
  if (!force && now - fLastProgress < kProgressInterval)
    return;
  fLastProgress = now;

  const int64 trackFrames = fSpans[index].frames;
  float fraction = 0.0f;
  if (fDiscFrames > 0)
    fraction = (float)(fFramesBefore + framesDone) / (float)fDiscFrames;

  BMessage msg(MSG_CD_RIP_PROGRESS);
  msg.AddInt32("track", (int32)index);
  msg.AddInt32("tracks", (int32)fJob.tracks.size());
  msg.AddFloat("fraction", std::min(fraction, 1.0f));
  msg.AddFloat("track_fraction",
               trackFrames > 0 ? (float)framesDone / (float)trackFrames : 0.0f);
  msg.AddString("title", fJob.tracks[index].title);
  // Progress is best effort: never stall the rip on a busy window.
  fTarget.SendMessage(&msg, (BHandler *)nullptr, 0);
}

status_t CDRipper::_BuildLayout(BString &error) {
  // cdda's track files are contiguous: each runs from its track's start to
  // the next one's, so the disc is simply the files one after another.
  fSpans.clear();
  fDiscFrames = 0;
  for (const MediaItem &track : fJob.tracks) {
    Span span;
    span.file.reset(new BFile(track.path.String(), B_READ_ONLY));
    off_t size = 0;
    if (span.file->InitCheck() != B_OK || span.file->GetSize(&size) != B_OK) {
      error.SetToFormat("Could not open track %ld on the disc", (long)track.track);
      return B_ERROR;
    }
    off_t dataSize = 0;
    status_t status = ParseWav(*span.file, size, span.dataOffset, dataSize, error);
    if (status != B_OK)
      return status;
    span.firstFrame = fDiscFrames;
    span.frames = dataSize / 4;
    fDiscFrames += span.frames;
    fSpans.push_back(std::move(span));
  }
  return fSpans.empty() ? B_ENTRY_NOT_FOUND : B_OK;
}

status_t CDRipper::_ReadDisc(int64 first, size_t count, uint32 *out,
                             BString &error) {
  size_t done = 0;
  size_t span = 0;
  while (done < count) {
    const int64 frame = first + (int64)done;
    if (frame < 0 || frame >= fDiscFrames) {
      // Before the first track or past the lead-out: cdda can't read there.
      // Silence is what other rippers use too, and AccurateRip leaves these
      // frames out of its checksums.
      out[done++] = 0;
      continue;
    }
    while (span + 1 < fSpans.size() && fSpans[span + 1].firstFrame <= frame)
      span++;
    while (span > 0 && fSpans[span].firstFrame > frame)
      span--;

    const Span &s = fSpans[span];
    const size_t n = (size_t)std::min<int64>((int64)(count - done),
                                             s.firstFrame + s.frames - frame);
    const off_t at = s.dataOffset + (frame - s.firstFrame) * 4;
    ssize_t got = s.file->ReadAt(at, out + done, n * 4);
    if (got != (ssize_t)(n * 4)) {
      const status_t status = got < 0 ? (status_t)got : B_IO_ERROR;
      error.SetToFormat("Read error at %ld:%02ld on the disc: %s",
                        (long)(frame / 44100 / 60), (long)(frame / 44100 % 60),
                        got < 0 ? strerror(status) : "unexpected end");
      return status;
    }
    done += n;
  }
  return B_OK;
}

void CDRipper::_LookUp() {
  if (!fJob.verify)
    return;

  _SendStatus(B_TRANSLATE("Looking up the disc in AccurateRip" B_UTF8_ELLIPSIS));

  AccurateRip::DiscId id;
  if (AccurateRip::ReadDiscId(fJob.discPath, id) != B_OK) {
    fLookupNote = B_TRANSLATE("not checked: the disc's table of contents "
                              "could not be read");
    return;
  }
  if (id.audioTracks != (int32)fJob.tracks.size()) {
    fLookupNote = B_TRANSLATE("not checked: the track list doesn't match the "
                              "disc's table of contents");
    return;
  }

  BString error;
  status_t status = AccurateRip::Fetch(id, fDatabase, error);
  if (status == B_OK) {
    fLookup = kRipAccurate;
  } else if (status == B_ENTRY_NOT_FOUND) {
    fLookup = kRipNotInDatabase;
    fLookupNote = B_TRANSLATE("this disc is not in the database");
  } else {
    fLookupNote.SetToFormat(B_TRANSLATE("not checked: %s"), error.String());
  }
}

void CDRipper::_DetectOffset() {
  if (fJob.offsetKnown || fLookup != kRipAccurate || fSpans.size() < 3)
    return;

  _SendStatus(B_TRANSLATE("Finding the drive's read offset" B_UTF8_ELLIPSIS));

  // Use the shortest track that is neither first nor last (those have
  // frames left out of their checksums): least to read again.
  size_t best = 1;
  for (size_t i = 2; i + 1 < fSpans.size(); i++) {
    if (fSpans[i].frames < fSpans[best].frames)
      best = i;
  }

  const Span &s = fSpans[best];
  std::vector<uint32> frames((size_t)s.frames + 2 * kOffsetRange);
  BString error;
  if (_ReadDisc(s.firstFrame - kOffsetRange, frames.size(), frames.data(),
                error) != B_OK) {
    DEBUG_PRINT("CD rip: offset detection read failed: %s\n", error.String());
    return;
  }

  int32 offset = 0, confidence = 0;
  if (!AccurateRip::FindOffset(frames.data(), (uint32)s.frames, kOffsetRange,
                               best, fDatabase, offset, confidence)) {
    DEBUG_PRINT("CD rip: no read offset matched track %zu\n", best + 1);
    return;
  }

  DEBUG_PRINT("CD rip: drive read offset %+ld (track %zu, confidence %ld)\n",
              (long)offset, best + 1, (long)confidence);
  fJob.offset = offset;
  fJob.offsetKnown = true;
  fOffsetDetected = true;

  BMessage msg(MSG_CD_RIP_OFFSET);
  msg.AddInt32("offset", offset);
  msg.AddInt32("confidence", confidence);
  fTarget.SendMessage(&msg);
}

void CDRipper::_Run() {
  int32 ripped = 0, skipped = 0, failed = 0, accurate = 0, noMatch = 0;
  std::set<BString> written;
  BMessage finished(MSG_CD_RIP_FINISHED);

  BString layoutError;
  if (_BuildLayout(layoutError) != B_OK) {
    finished.AddString("error", layoutError);
    fJob.tracks.clear();
  } else {
    _LookUp();
    if (!fCancel)
      _DetectOffset();
  }

  for (size_t i = 0; i < fJob.tracks.size() && !fCancel; i++) {
    const MediaItem &track = fJob.tracks[i];
    BString dest = DestinationFor(fJob, track);

    // Two tracks resolving to one name (a pattern without {track} and two
    // untitled tracks): number the later ones instead of skipping them.
    for (int32 n = 2; written.count(dest) > 0; n++)
      dest = NumberedPath(DestinationFor(fJob, track), n);

    _SendProgress(i, 0, true);

    BString error;
    status_t status;
    RipVerdict verdict = kRipNotChecked;
    int32 confidence = -1;
    AccurateRip::Checksum checksum;

    if (BEntry(dest.String()).Exists()) {
      status = B_FILE_EXISTS;
      skipped++;
    } else {
      status = _RipTrack(i, dest, checksum, error);
      if (status == B_OK) {
        ripped++;
        written.insert(dest);
        finished.AddString("path", dest);

        if (fLookup == kRipAccurate) {
          confidence = std::max(fDatabase.Match(i, checksum.V2()),
                                fDatabase.Match(i, checksum.V1()));
          verdict = confidence >= 0 ? kRipAccurate : kRipNoMatch;
          if (verdict == kRipAccurate)
            accurate++;
          else
            noMatch++;
        } else if (fLookup == kRipNotInDatabase) {
          verdict = kRipNotInDatabase;
        }
        DEBUG_PRINT("CD rip: track %zu v1 %08" B_PRIx32 " v2 %08" B_PRIx32
                    " -> %d (%ld)\n", i + 1, checksum.V1(), checksum.V2(),
                    (int)verdict, (long)confidence);
      } else if (status != B_CANCELED) {
        failed++;
      }
    }
    fFramesBefore += fSpans[i].frames;

    if (status == B_CANCELED)
      break;

    BMessage done(MSG_CD_RIP_TRACK_DONE);
    done.AddInt32("track", (int32)i);
    done.AddInt32("status", status);
    done.AddString("path", dest);
    done.AddString("title", track.title);
    done.AddString("error", error);
    done.AddInt32("ar", verdict);
    done.AddInt32("ar_confidence", confidence);
    fTarget.SendMessage(&done);
  }

  finished.AddInt32("ripped", ripped);
  finished.AddInt32("skipped", skipped);
  finished.AddInt32("failed", failed);
  finished.AddInt32("accurate", accurate);
  finished.AddInt32("no_match", noMatch);
  finished.AddString("ar_note", fLookupNote);
  finished.AddBool("offset_known", fJob.offsetKnown);
  finished.AddBool("offset_detected", fOffsetDetected);
  finished.AddInt32("offset", fJob.offset);
  finished.AddBool("cancelled", fCancel.load());
  fRunning = false;
  fTarget.SendMessage(&finished);
}

status_t CDRipper::_RipTrack(size_t index, const BString &dest,
                             AccurateRip::Checksum &checksum, BString &error) {
  const MediaItem &track = fJob.tracks[index];
  const Span &span = fSpans[index];

  BPath destPath(dest.String());
  BPath parent;
  if (destPath.InitCheck() != B_OK || destPath.GetParent(&parent) != B_OK) {
    error = "Invalid destination path";
    return B_BAD_VALUE;
  }
  status_t status = create_directory(parent.Path(), 0755);
  if (status != B_OK) {
    error.SetToFormat("Could not create folder %s: %s", parent.Path(),
                      strerror(status));
    return status;
  }

  BString partPath(dest);
  partPath << ".part";

  std::unique_ptr<AudioFileEncoder> encoder(CreateAudioFileEncoder(fJob.format));
  status = encoder->Open(partPath.String(), 44100, 2, track);
  if (status != B_OK) {
    error = encoder->ErrorText();
    unlink(partPath.String());
    return status;
  }

  checksum.Begin((uint32)span.frames, index == 0,
                 index + 1 == fSpans.size());

  // The track as other rippers see it: shifted by the drive's read offset,
  // which may reach a little into the neighbouring tracks.
  const int64 first = span.firstFrame + fJob.offset;
  std::vector<uint32> buffer(kChunkFrames);

  int64 done = 0;
  while (done < span.frames) {
    if (fCancel) {
      encoder->Abort();
      unlink(partPath.String());
      return B_CANCELED;
    }

    const size_t n = (size_t)std::min<int64>(kChunkFrames, span.frames - done);
    status = _ReadDisc(first + done, n, buffer.data(), error);
    if (status != B_OK) {
      encoder->Abort();
      unlink(partPath.String());
      return status;
    }
    checksum.Add(buffer.data(), n);

#if B_HOST_IS_BENDIAN
    swap_data(B_INT16_TYPE, buffer.data(), n * 4, B_SWAP_LENDIAN_TO_HOST);
#endif
    status = encoder->Write(reinterpret_cast<const int16 *>(buffer.data()),
                            (int32)n);
    if (status != B_OK) {
      error = encoder->ErrorText();
      encoder->Abort();
      unlink(partPath.String());
      return status;
    }
    done += n;
    _SendProgress(index, done, false);
  }

  status = encoder->Finish();
  if (status != B_OK) {
    error = encoder->ErrorText();
    unlink(partPath.String());
    return status;
  }

  if (rename(partPath.String(), dest.String()) != 0) {
    error.SetToFormat("Could not rename the finished file: %s", strerror(errno));
    unlink(partPath.String());
    return B_ERROR;
  }

  _FinishFile(dest, track);
  return B_OK;
}

void CDRipper::_FinishFile(const BString &dest, const MediaItem &track) {
  BPath path(dest.String());

  // FFmpeg tags a WAV with a RIFF INFO list only. TagLib 1.13 parses that,
  // but its combined tag() -- what Beton's reader uses -- comes back empty
  // for an INFO-only WAV (seen with taglib 1.13 on hrev60368). Writing the
  // tags again through TagLib adds an ID3v2 chunk, which it does read.
  if (fJob.format == RipFormat::WAV) {
    if (!MetadataTagIO::WriteTagsToFile(path, ToTagData(track), nullptr))
      DEBUG_PRINT("CD rip: could not tag %s\n", dest.String());
  }

  // Give the file its audio MIME type so Tracker and the library see it as
  // music straight away.
  update_mime_info(dest.String(), false, true, B_UPDATE_MIME_INFO_NO_FORCE);

  // Mirror the tags into Haiku's Audio:* attributes so Tracker's columns and
  // queries work without waiting for a library scan.
  if (MetadataTagIO::IsBeFsVolume(path)) {
    if (!MetadataTagIO::WriteBfsAttributes(path, ToTagData(track), nullptr))
      DEBUG_PRINT("CD rip: could not write attributes to %s\n", dest.String());
  }
}
