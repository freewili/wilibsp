#pragma once

// Timing of one DVI video mode, in pixels and lines.
struct stDVIMode
{
	unsigned short iHActive;
	unsigned short iHFrontPorch;
	unsigned short iHSync;
	unsigned short iHBackPorch;
	unsigned short iVActive;
	unsigned short iVFrontPorch;
	unsigned short iVSync;
	unsigned short iVBackPorch;
	bool bHSyncActiveLow;
	bool bVSyncActiveLow;
	unsigned int iPixelClockKHz;
	unsigned char iVIC;               // CTA-861 video identification code, 0 = none
};

// 640x480 at 60 Hz: 800 x 525 total, 25.2 MHz pixel clock, both sync pulses low, VIC 1.
inline constexpr stDVIMode DVI_MODE_640x480_60 = { 640, 16, 96, 48, 480, 10, 2, 33, true, true, 25200, 1 };
