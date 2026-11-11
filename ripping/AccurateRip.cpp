#include "AccurateRip.h"

#include "CddaTracks.h"
#include "Debug.h"

#include <ByteOrder.h>
#include <DataIO.h>
#include <Drivers.h>
#include <Node.h>
#include <Url.h>
#include <UrlProtocolRoster.h>
#include <fs_info.h>
#include <private/netservices/HttpRequest.h>
#include <private/netservices/HttpResult.h>

#include <fcntl.h>
#include <memory>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

using namespace BPrivate::Network;

namespace {

uint32 ReadLE32(const uint8 *p) {
  return (uint32)p[0] | ((uint32)p[1] << 8) | ((uint32)p[2] << 16) |
         ((uint32)p[3] << 24);
}

/** The standard freedb disc ID, for discs without a CD:cddbid attribute. */
uint32 CddbId(const std::vector<CddaTracks::TocTrack> &tracks, uint32 leadOut) {
  auto digitSum = [](uint32 n) {
    uint32 s = 0;
    for (; n > 0; n /= 10)
      s += n % 10;
    return s;
  };
  uint32 n = 0;
  for (const CddaTracks::TocTrack &t : tracks)
    n += digitSum((t.lba + 150) / 75);
  uint32 total = (leadOut + 150) / 75 - (tracks.front().lba + 150) / 75;
  return ((n % 255) << 24) | (total << 8) | (uint32)tracks.size();
}

} // namespace

BString AccurateRip::DiscId::Url() const {
  BString hex1;
  hex1.SetToFormat("%08" B_PRIx32, id1);
  BString url;
  url.SetToFormat("http://www.accuraterip.com/accuraterip/%c/%c/%c/"
                  "dBAR-%03" B_PRId32 "-%08" B_PRIx32 "-%08" B_PRIx32
                  "-%08" B_PRIx32 ".bin",
                  hex1[7], hex1[6], hex1[5], audioTracks, id1, id2, cddb);
  return url;
}

status_t AccurateRip::ReadDiscId(const BString &cdPath, DiscId &out) {
  out = DiscId();
  CddaTracks::Toc toc;
  status_t status = CddaTracks::ReadToc(cdPath, toc);
  if (status != B_OK)
    return status;
  const std::vector<CddaTracks::TocTrack> &tracks = toc.tracks;
  const uint32 leadOut = toc.leadOut;

  for (const CddaTracks::TocTrack &t : tracks) {
    if (!t.audio)
      continue;
    out.id1 += t.lba;
    out.id2 += (t.lba > 0 ? t.lba : 1) * t.number;
    out.audioTracks++;
  }
  out.id1 += leadOut;
  out.id2 += leadOut * (uint32)(out.audioTracks + 1);

  BNode root(cdPath.String());
  uint32 cddb = 0;
  if (root.ReadAttr("CD:cddbid", B_UINT32_TYPE, 0, &cddb, sizeof(cddb)) ==
      (ssize_t)sizeof(cddb))
    out.cddb = cddb;
  else
    out.cddb = CddbId(tracks, leadOut);

  return out.IsValid() ? B_OK : B_BAD_DATA;
}

int32 AccurateRip::Database::Match(size_t track, uint32 crc) const {
  int32 best = -1;
  for (const Response &r : responses) {
    if (track < r.crc.size() && r.crc[track] == crc &&
        (int32)r.confidence[track] > best)
      best = r.confidence[track];
  }
  return best;
}

status_t AccurateRip::Parse(const void *data, size_t length, const DiscId &id,
                            Database &out) {
  out.responses.clear();
  const uint8 *p = static_cast<const uint8 *>(data);
  size_t pos = 0;
  // Responses follow one another: a 13-byte header (track count, id1, id2,
  // cddb id) and 9 bytes per track (confidence, CRC, CRC of frame 450).
  while (pos + 13 <= length) {
    const size_t tracks = p[pos];
    const size_t next = pos + 13 + tracks * 9;
    if (next > length)
      return B_BAD_DATA;
    if (ReadLE32(p + pos + 1) != id.id1 || ReadLE32(p + pos + 5) != id.id2)
      return B_BAD_DATA;

    Response r;
    for (size_t t = 0; t < tracks; t++) {
      const uint8 *e = p + pos + 13 + t * 9;
      r.confidence.push_back(e[0]);
      r.crc.push_back(ReadLE32(e + 1));
    }
    out.responses.push_back(r);
    pos = next;
  }
  return out.IsEmpty() ? B_ENTRY_NOT_FOUND : B_OK;
}

status_t AccurateRip::Fetch(const DiscId &id, Database &out, BString &error) {
  out.responses.clear();
  if (!id.IsValid()) {
    error = "This disc has no readable table of contents";
    return B_BAD_VALUE;
  }

  try {
    BMallocIO sink;
#if B_HAIKU_VERSION <= B_HAIKU_VERSION_1_BETA_5
    BUrl url(id.Url().String());
#else
    BUrl url(id.Url().String(), true);
#endif
    std::unique_ptr<BUrlRequest> request(
        BUrlProtocolRoster::MakeRequest(url, &sink));
    if (!request) {
      error = "Could not create the request";
      return B_ERROR;
    }
    if (auto http = dynamic_cast<BHttpRequest *>(request.get())) {
      http->SetUserAgent("Beton");
      http->SetFollowLocation(true);
    }

    thread_id thread = request->Run();
    if (thread < 0) {
      error = "Could not start the request";
      return thread;
    }
    status_t exit;
    if (wait_for_thread_etc(thread, B_RELATIVE_TIMEOUT, 20000000, &exit) !=
        B_OK) {
      request->Stop();
      wait_for_thread_etc(thread, B_RELATIVE_TIMEOUT, 2000000, &exit);
      error = "The AccurateRip server did not answer";
      return B_TIMED_OUT;
    }

    const BHttpResult *result =
        dynamic_cast<const BHttpResult *>(&request->Result());
    const int32 status = result ? result->StatusCode() : 0;
    DEBUG_PRINT("AccurateRip: %s -> HTTP %ld, %zu bytes\n", id.Url().String(),
                (long)status, (size_t)sink.BufferLength());
    if (status == 404)
      return B_ENTRY_NOT_FOUND;
    if (status != 200) {
      if (status == 0)
        error = "Could not reach the AccurateRip server";
      else
        error.SetToFormat("The AccurateRip server answered HTTP %ld",
                          (long)status);
      return B_ERROR;
    }

    status_t parsed = Parse(sink.Buffer(), sink.BufferLength(), id, out);
    if (parsed == B_BAD_DATA)
      error = "The AccurateRip answer could not be read";
    return parsed;
  } catch (...) {
    error = "The AccurateRip lookup failed";
    return B_ERROR;
  }
}

void AccurateRip::Checksum::Begin(uint32 frames, bool firstTrack,
                                  bool lastTrack) {
  fPosition = 1;
  fLow = fHigh = 0;
  // whipper: "MulBy >= 2940" on the first track, "MulBy <= N - 2940" on the
  // last; frame numbers are 1-based.
  fFrom = firstTrack ? (uint32)kSkipFrames : 1;
  fTo = lastTrack ? (frames > (uint32)kSkipFrames ? frames - kSkipFrames : 0)
                  : frames;
}

void AccurateRip::Checksum::Add(const uint32 *frames, size_t count) {
  for (size_t i = 0; i < count; i++, fPosition++) {
    if (fPosition < fFrom || fPosition > fTo)
      continue;
    const uint64 product = (uint64)B_LENDIAN_TO_HOST_INT32(frames[i]) * fPosition;
    fLow += (uint32)product;
    fHigh += (uint32)(product >> 32);
  }
}

bool AccurateRip::FindOffset(const uint32 *x, uint32 n, int32 range,
                             size_t track, const Database &db, int32 &offset,
                             int32 &confidence) {
  // S(o) = sum_{k=1..n} k * f[o+k-1] and T(o) = sum f[o+k-1], with f the
  // track's frames at offset o (f[0] is x[range + o]). Then
  // S(o+1) = S(o) - T(o) + n * f[o+n] and T(o+1) = T(o) - f[o] + f[o+n].
  auto frame = [&](int64 i) { return B_LENDIAN_TO_HOST_INT32(x[i]); };

  uint32 s = 0, t = 0;
  for (uint32 k = 1; k <= n; k++) {
    s += k * frame(k - 1);
    t += frame(k - 1);
  }

  bool found = false;
  confidence = -1;
  for (int32 o = -range; o <= range; o++) {
    const int32 c = db.Match(track, s);
    // Prefer the most confident match; on a tie, the smaller offset.
    if (c > confidence ||
        (c == confidence && found && abs(o) < abs(offset))) {
      if (c >= 0) {
        confidence = c;
        offset = o;
        found = true;
      }
    }
    if (o == range)
      break;
    const uint32 in = frame((int64)(o + range) + n);
    const uint32 out = frame(o + range);
    s = s - t + n * in;
    t = t - out + in;
  }
  return found;
}

BString AccurateRip::DriveName(const BString &cdPath) {
  fs_info info;
  if (fs_stat_dev(dev_for_path(cdPath.String()), &info) != B_OK)
    return BString();
  int fd = open(info.device_name, O_RDONLY);
  if (fd < 0)
    return BString();
  char name[B_FILE_NAME_LENGTH] = {0};
  const bool ok = ioctl(fd, B_GET_DEVICE_NAME, name, sizeof(name)) == 0;
  close(fd);
  BString result(ok ? name : "");
  result.Trim();
  return result;
}
