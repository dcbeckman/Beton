#ifndef BETON_CDDA_TRACKS_H
#define BETON_CDDA_TRACKS_H

#include "MediaItem.h"

#include <String.h>
#include <vector>

/**
 * @namespace CddaTracks
 * @brief Lists the tracks of a mounted audio CD (Haiku's cdda file system).
 *
 * The cdda file system shows each track as a WAV file and puts the CD-Text or
 * CDDB names into BFS attributes. Both the CD source view and the ripper read
 * the disc through here, so they always agree on titles and track numbers.
 */
namespace CddaTracks {

/**
 * @brief Reads the tracks of the disc mounted at @p cdPath.
 * @param cdPath Mount point of the cdda volume.
 * @return Tracks sorted by track number; empty if the path can't be read.
 */
std::vector<MediaItem> Read(const BString &cdPath);

} // namespace CddaTracks

#endif // BETON_CDDA_TRACKS_H
