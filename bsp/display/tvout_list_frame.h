// bsp/display/tvout_list_frame.h — the frame and list tvout_list built last,
// for the TV sound feeder (tvout_audio_core.cpp), and the picture build at
// either scale, for the host tests. C++ only.
#ifndef TVOUT_LIST_FRAME_H
#define TVOUT_LIST_FRAME_H
#include "rpDVIFrame.h"
#include "rpDVIList.h"

// The frame's audio slots and line map are those of the last build.
rpDVIFrame &tvoutListFrame();
// Scan pieces per frame, the first picture row's piece and the picture rows
// of the last build.
const rpDVIList &tvoutListList();

// Builds a width x height picture (row y at pixels + y * width) into sink:
// scale 1 as it is, scale 2 at twice its size, each frame row r reading
// picture row r / 2 with width pixels, for an HSTX encoder that sends each
// pixel twice. False for another scale or a picture that does not fit.
bool tvoutListBuildPicture(rpDVIFrame &frame, const uint16_t *pixels, unsigned width, unsigned height,
                           unsigned scale, rpDVIFrameSink &sink);

#endif
