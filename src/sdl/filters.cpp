// VisualBoyAdvance - Nintendo Gameboy/GameboyAdvance (TM) emulator.
// Copyright (C) 1999-2003 Forgotten
// Copyright (C) 2004-2008 Forgotten and the VBA development team

// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2, or(at your option)
// any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software Foundation,
// Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.

#include "filters.h"

      //
      // Screen filters
      //

struct FilterDesc {
	char name[30];
	int enlargeFactor;
	FilterFunc func32;
};

const FilterDesc Filters[] = {
  { "Stretch 1x", 1, sdlStretch1x },
  { "Stretch 2x", 2, sdlStretch2x },
  { "2xSaI", 2, _2xSaI32 },
  { "Super 2xSaI", 2, Super2xSaI32 },
  { "Super Eagle", 2, SuperEagle32 },
  { "Pixelate", 2, Pixelate32 },
  { "AdvanceMAME Scale2x", 2, AdMame2x32 },
  { "Bilinear", 2, Bilinear32 },
  { "Bilinear Plus", 2, BilinearPlus32 },
  { "Scanlines", 2, Scanlines32 },
  { "TV Mode", 2, ScanlinesTV32 },
  { "lq2x", 2, lq2x32 },
  { "hq2x", 2, hq2x32 },
  { "Stretch 3x", 3, sdlStretch3x },
  { "hq3x", 3, hq3x32 },
  { "Stretch 4x", 4, sdlStretch4x },
  { "hq4x", 4, hq4x32 }
};

int getFilterEnlargeFactor(const Filter f)
{
	return Filters[f].enlargeFactor;
}

char* getFilterName(const Filter f)
{
	return (char*)Filters[f].name;
}

FilterFunc initFilter(const Filter f, const int srcWidth)
{
  FilterFunc func = Filters[f].func32;

  if (func)
    switch (f) {
      case kStretch1x:
        sdlStretchInit(0, srcWidth);
        break;
      case kStretch2x:
        sdlStretchInit(1, srcWidth);
        break;
      case kStretch3x:
        sdlStretchInit(2, srcWidth);
        break;
      case kStretch4x:
        sdlStretchInit(3, srcWidth);
        break;
      case k2xSaI:
      case kSuper2xSaI:
      case kSuperEagle:
        Init_2xSaI(32);
        break;
      case khq2x:
      case klq2x:
        hq2x_init(32);
        break;
      default:
        break;
    }

  return func;
}

struct IFBFilterDesc {
	char name[30];
	IFBFilterFunc func16;
	IFBFilterFunc func32;
};

const IFBFilterDesc IFBFilters[] = {
  { "No interframe blending", 0, 0 },
  { "Interframe motion blur", MotionBlurIB, MotionBlurIB32 },
  { "Smart interframe blending", SmartIB, SmartIB32 }
};

IFBFilterFunc initIFBFilter(const IFBFilter f)
{
  IFBFilterFunc func = IFBFilters[f].func32;

  return func;
}

char* getIFBFilterName(const IFBFilter f)
{
	return (char*)IFBFilters[f].name;
}
