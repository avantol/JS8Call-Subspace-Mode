#ifndef SPOT_MAP_GEO_DATA_HPP__
#define SPOT_MAP_GEO_DATA_HPP__

/**
 * @file SpotMapGeoData.h
 * @brief Embedded world outline data (Natural Earth 110m, public
 * domain), quantized to qint16 degrees x100. See the .cpp for details.
 */

#include <QtGlobal>

extern int const kGeoPolylineCount;
extern int const kGeoPolylineStart[]; // count+1 entries (sentinel end)
extern qint16 const kGeoPoints[][2];  // {lat x100, lon x100}

#endif
