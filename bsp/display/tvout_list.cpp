// bsp/display/tvout_list.cpp — see tvout_list.h.
#include "tvout_list.h"
#include "rpDVIFrame.h"
#include "rpDVIList.h"
#include "tvout_list_frame.h"

static_assert(sizeof(tvout_list_entry_t) == sizeof(stDVIListEntry), "list entry layouts differ");
static_assert(TVOUT_LIST_BYTES >= TVOUT_LIST_BARS_ENTRIES * sizeof(stDVIListEntry), "the block holds the colour bars");

namespace {

// The frame holds the shared line words the entries point at.
rpDVIFrame s_frame;
rpDVIList s_list;
bool s_audioFraming;

// Each pixel row of the frame of a picture shown at twice its size, as one
// row of the picture: frame row r reads picture row r / 2, width pixels, each
// of which the HSTX encoder sends twice. Every other piece passes through.
class DoubledRows : public rpDVIFrameSink
{
public:
    DoubledRows(rpDVIFrameSink &sink, const uint16_t *pixels, unsigned width)
        : m_sink(sink), m_pixels(pixels), m_width(width)
    {
    }
    bool addWords(const uint32_t *pWords, unsigned int iCount) override { return m_sink.addWords(pWords, iCount); }
    bool addPixels(const uint16_t *, unsigned int) override
    {
        return m_sink.addPixels(m_pixels + (m_frameRows++ / 2u) * m_width, m_width);
    }
    bool addLines(const uint32_t *pWords, unsigned int iFirstWords, unsigned int iLineWords, unsigned int iLines) override
    {
        return m_sink.addLines(pWords, iFirstWords, iLineWords, iLines);
    }
    bool addJoin(const uint32_t *pWords, unsigned int iCount) override { return m_sink.addJoin(pWords, iCount); }

private:
    rpDVIFrameSink &m_sink;
    const uint16_t *m_pixels;
    unsigned m_width;
    unsigned m_frameRows = 0;
};

// The entries, the scan runs and the row table of a frame's list.
unsigned listBytes(unsigned width, unsigned height)
{
    const unsigned entries = rpDVIList::entryCount(DVI_MODE_640x480_60, true, width, height, s_audioFraming);
    if (entries == 0)
        return 0;
    return entries * (unsigned)sizeof(stDVIListEntry) + rpDVIList::SCAN_RUNS * (unsigned)sizeof(stDVIScanRun) +
           rpDVIList::rowEntryCount(true, height) * (unsigned)sizeof(stDVIRowEntry);
}

rpDVIList::stConfig listConfig(const tvout_list_config_t *cfg)
{
    rpDVIList::stConfig stConfig = {};
    stConfig.iCtrlWords = cfg->ctrl_words;
    stConfig.iCtrlPixels = cfg->ctrl_pixels;
    stConfig.iCtrlCopy = cfg->ctrl_copy;
    stConfig.iChainBits = cfg->chain_bits;
    stConfig.iChainShift = cfg->chain_shift;
    stConfig.iJoinChannel = cfg->join_channel;
    stConfig.iRestartChannel = cfg->restart_channel;
    stConfig.iRewindChannel = cfg->rewind_channel;
    stConfig.iFifo = cfg->fifo;
    stConfig.iRowReadAddr = cfg->row_read_addr;
    return stConfig;
}

}

unsigned tvout_list_bars_entries(void)
{
    return rpDVIList::entryCount(DVI_MODE_640x480_60, false, 0, 0, false);
}

bool tvout_list_build_bars(tvout_list_entry_t *list, unsigned entries, const tvout_list_config_t *cfg)
{
    const rpDVIList::stConfig stConfig = listConfig(cfg);
    s_list.begin(reinterpret_cast<stDVIListEntry *>(list), entries, nullptr, nullptr, 0, stConfig, false);
    return s_frame.buildBars(DVI_MODE_640x480_60, s_list) && s_list.finish();
}

unsigned tvout_list_picture_bytes(unsigned width, unsigned height, unsigned scale)
{
    if (scale != 1u && scale != 2u)
        return 0;
    // The border is split evenly, so it has to divide by two (rpDVIFrame::buildPicture).
    const unsigned frameWidth = width * scale;
    if (frameWidth > DVI_MODE_640x480_60.iHActive || (DVI_MODE_640x480_60.iHActive - frameWidth) % 2u != 0)
        return 0;
    return listBytes(frameWidth, height * scale);
}

bool tvoutListBuildPicture(rpDVIFrame &frame, const uint16_t *pixels, unsigned width, unsigned height,
                           unsigned scale, rpDVIFrameSink &sink)
{
    if (scale == 1u)
        return frame.buildPicture(DVI_MODE_640x480_60, pixels, width, height, width, sink);
    if (scale != 2u)
        return false;
    DoubledRows doubled(sink, pixels, width);
    return frame.buildPicture(DVI_MODE_640x480_60, pixels, 2u * width, 2u * height, width, doubled);
}

// The block holds the entries, then the scan runs, then the row table, as the
// stock firmware lays out its list allocation.
bool tvout_list_build_picture(void *block, unsigned bytes, const uint16_t *pixels,
                              unsigned width, unsigned height, unsigned scale,
                              const tvout_list_config_t *cfg, tvout_list_picture_t *out)
{
    const unsigned need = tvout_list_picture_bytes(width, height, scale);
    if (pixels == nullptr || need == 0 || need > bytes)
        return false;
    const unsigned frameHeight = height * scale;
    const unsigned entries = rpDVIList::entryCount(DVI_MODE_640x480_60, true, width * scale, frameHeight, s_audioFraming);
    const unsigned rowEntries = rpDVIList::rowEntryCount(true, frameHeight);
    stDVIListEntry *pList = static_cast<stDVIListEntry *>(block);
    stDVIScanRun *pRuns = reinterpret_cast<stDVIScanRun *>(pList + entries);
    stDVIRowEntry *pRows = reinterpret_cast<stDVIRowEntry *>(pRuns + rpDVIList::SCAN_RUNS);

    const rpDVIList::stConfig stConfig = listConfig(cfg);
    s_list.begin(pList, entries, pRuns, pRows, rowEntries, stConfig, true);
    if (!tvoutListBuildPicture(s_frame, pixels, width, height, scale, s_list) || !s_list.finish())
        return false;

    out->joins = s_list.joins();
    out->join_start = s_list.joinStart();
    out->join_start_word = s_list.joinStartWord();
    out->join_words = s_list.joinWords();
    out->row_start = s_list.rowStart();
    out->rows_loaded_at = s_list.rowsLoadedAt();
    out->first_row = s_list.scanFirstRow();
    out->rows = s_list.rows();
    out->pieces = s_list.scanCount();
    out->row0_read_addr = s_list.rowReadAddress(0);
    out->row_read_addrs = frameHeight > 1 ? s_list.rowReadAddress(1) : nullptr;
    return true;
}

unsigned tvout_list_scan_piece(uint32_t loaded, uint32_t left, uint32_t rows_loaded)
{
    return s_list.scanPiece(loaded, left, rows_loaded);
}

// Row r is piece first_row + 2r, a join between each two. With p the piece
// after the one being sent, the last row has been sent once p reaches
// first_row + 2 * rows.
bool tvout_list_flip_window(unsigned piece, unsigned first_row, unsigned rows)
{
    return piece >= first_row + 2u * rows || piece + TVOUT_LIST_FLIP_MARGIN <= first_row;
}

unsigned tvout_list_pixel_clock_khz(void)
{
    return DVI_MODE_640x480_60.iPixelClockKHz;
}

void tvout_list_set_audio_framing(uint32_t *pool, unsigned words)
{
    s_audioFraming = pool != nullptr;
    s_frame.setAudioFraming(pool, words);
}

rpDVIFrame &tvoutListFrame()
{
    return s_frame;
}

const rpDVIList &tvoutListList()
{
    return s_list;
}
