#include "CDRipper.h"

#include "AudioFileEncoder.h"
#include "Debug.h"
#include "Messages.h"
#include "MetadataTagIO.h"
#include "RipNamePattern.h"

#include <ByteOrder.h>
#include <Directory.h>
#include <Entry.h>
#include <File.h>
#include <Mime.h>
#include <Path.h>

#include <algorithm>
#include <errno.h>
#include <memory>
#include <set>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

namespace {

/** Bytes read per call: 32 CD sectors, about 0.4 s of audio. */
const size_t kReadChunk = 32 * 2352;

const bigtime_t kProgressInterval = 200000;

struct WavLayout {
  off_t dataOffset = 0;
  off_t dataSize = 0;
  int32 sampleRate = 0;
  int32 channels = 0;
};

uint32 ReadLE32(const uint8 *p) {
  return (uint32)p[0] | ((uint32)p[1] << 8) | ((uint32)p[2] << 16) |
         ((uint32)p[3] << 24);
}

uint16 ReadLE16(const uint8 *p) { return (uint16)(p[0] | (p[1] << 8)); }

/**
 * Finds the PCM data in a cdda track. The cdda file system always writes the
 * plain 44-byte header, but walking the chunks costs nothing and copes with
 * any other WAV the job might be handed.
 */
status_t ParseWav(BFile &file, off_t fileSize, WavLayout &out, BString &error) {
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
      const uint16 tag = ReadLE16(fmt);
      out.channels = ReadLE16(fmt + 2);
      out.sampleRate = (int32)ReadLE32(fmt + 4);
      const uint16 bits = ReadLE16(fmt + 14);
      if (tag != 1 || bits != 16 || out.channels < 1 || out.channels > 2 ||
          out.sampleRate <= 0) {
        error = "Unsupported audio layout on the disc";
        return B_NOT_SUPPORTED;
      }
      haveFormat = true;
    } else if (memcmp(chunk, "data", 4) == 0) {
      if (!haveFormat)
        break;
      out.dataOffset = body;
      out.dataSize = std::min<off_t>((off_t)size, fileSize - body);
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
    : fJob(job), fTarget(target) {
  for (const MediaItem &t : fJob.tracks)
    fTotalBytes += t.size;
}

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

void CDRipper::_SendProgress(size_t index, off_t trackBytesDone, bool force) {
  const bigtime_t now = system_time();
  if (!force && now - fLastProgress < kProgressInterval)
    return;
  fLastProgress = now;

  float fraction = 0.0f;
  if (fTotalBytes > 0)
    fraction = (float)(fBytesBefore + trackBytesDone) / (float)fTotalBytes;

  BMessage msg(MSG_CD_RIP_PROGRESS);
  msg.AddInt32("track", (int32)index);
  msg.AddInt32("tracks", (int32)fJob.tracks.size());
  msg.AddFloat("fraction", std::min(fraction, 1.0f));
  msg.AddString("title", fJob.tracks[index].title);
  // Progress is best effort: never stall the rip on a busy window.
  fTarget.SendMessage(&msg, (BHandler *)nullptr, 0);
}

void CDRipper::_Run() {
  int32 ripped = 0, skipped = 0, failed = 0;
  std::set<BString> written;
  BMessage finished(MSG_CD_RIP_FINISHED);

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
    if (BEntry(dest.String()).Exists()) {
      status = B_FILE_EXISTS;
      skipped++;
    } else {
      status = _RipTrack(i, dest, error);
      if (status == B_OK) {
        ripped++;
        written.insert(dest);
        finished.AddString("path", dest);
      } else if (status != B_CANCELED) {
        failed++;
      }
    }
    fBytesBefore += track.size;

    if (status == B_CANCELED)
      break;

    BMessage done(MSG_CD_RIP_TRACK_DONE);
    done.AddInt32("track", (int32)i);
    done.AddInt32("status", status);
    done.AddString("path", dest);
    done.AddString("title", track.title);
    done.AddString("error", error);
    fTarget.SendMessage(&done);
  }

  finished.AddInt32("ripped", ripped);
  finished.AddInt32("skipped", skipped);
  finished.AddInt32("failed", failed);
  finished.AddBool("cancelled", fCancel.load());
  fRunning = false;
  fTarget.SendMessage(&finished);
}

status_t CDRipper::_RipTrack(size_t index, const BString &dest, BString &error) {
  const MediaItem &track = fJob.tracks[index];

  BFile in(track.path.String(), B_READ_ONLY);
  off_t fileSize = 0;
  if (in.InitCheck() != B_OK || in.GetSize(&fileSize) != B_OK) {
    error = "Could not open the track on the disc";
    return B_ERROR;
  }

  WavLayout wav;
  status_t status = ParseWav(in, fileSize, wav, error);
  if (status != B_OK)
    return status;

  BPath destPath(dest.String());
  BPath parent;
  if (destPath.InitCheck() != B_OK || destPath.GetParent(&parent) != B_OK) {
    error = "Invalid destination path";
    return B_BAD_VALUE;
  }
  status = create_directory(parent.Path(), 0755);
  if (status != B_OK) {
    error.SetToFormat("Could not create folder %s: %s", parent.Path(),
                      strerror(status));
    return status;
  }

  BString partPath(dest);
  partPath << ".part";

  std::unique_ptr<AudioFileEncoder> encoder(CreateAudioFileEncoder(fJob.format));
  status = encoder->Open(partPath.String(), wav.sampleRate, wav.channels, track);
  if (status != B_OK) {
    error = encoder->ErrorText();
    unlink(partPath.String());
    return status;
  }

  const size_t frameBytes = (size_t)wav.channels * sizeof(int16);
  const size_t chunk = kReadChunk - (kReadChunk % frameBytes);
  std::unique_ptr<uint8[]> buffer(new uint8[chunk]);
  size_t carry = 0; // Bytes of a partial frame left from the previous read.

  off_t done = 0;
  while (done < wav.dataSize) {
    if (fCancel) {
      encoder->Abort();
      unlink(partPath.String());
      return B_CANCELED;
    }

    size_t want = (size_t)std::min<off_t>((off_t)(chunk - carry), wav.dataSize - done);
    ssize_t got = in.ReadAt(wav.dataOffset + done, buffer.get() + carry, want);
    if (got <= 0) {
      error.SetToFormat("Read error at %.0f%% of the track: %s",
                        100.0 * (double)done / (double)wav.dataSize,
                        got < 0 ? strerror((status_t)got) : "unexpected end");
      encoder->Abort();
      unlink(partPath.String());
      return got < 0 ? (status_t)got : B_IO_ERROR;
    }
    done += got;

    size_t avail = carry + (size_t)got;
    size_t frames = avail / frameBytes;
#if B_HOST_IS_BENDIAN
    swap_data(B_INT16_TYPE, buffer.get(), frames * frameBytes, B_SWAP_LENDIAN_TO_HOST);
#endif
    if (frames > 0) {
      status = encoder->Write(reinterpret_cast<const int16 *>(buffer.get()),
                              (int32)frames);
      if (status != B_OK) {
        error = encoder->ErrorText();
        encoder->Abort();
        unlink(partPath.String());
        return status;
      }
    }
    carry = avail - frames * frameBytes;
    if (carry > 0)
      memmove(buffer.get(), buffer.get() + frames * frameBytes, carry);

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
