// Reading Radiance HDR pictures (RGBE and XYZE, flat or run-length encoded, any orientation).
#pragma once

#include <array>
#include <string>
#include <vector>

struct RadiancePicture {
    int width = 0;
    int height = 0;
    std::vector<float> values;             // width * height * 3, row 0 = top, as stored (no scaling)
    std::vector<std::string> headerLines;  // information header, without the "#?" line
    std::string format;                    // FORMAT= value, e.g. "32-bit_rle_rgbe"
    double exposure = 1.0;                 // product of all EXPOSURE= lines
    bool hasPrimaries = false;
    std::array<double, 8> primaries{};     // xr yr xg yg xb yb xw yw
    std::string hdrspaceColor;             // HDRSPACE_COLOR= value (written by hdrspace), lower case
    bool hasColorCorrection = false;       // a COLORCORR= line was found
};

// Throws std::runtime_error with a readable message if the data is not a Radiance picture.
RadiancePicture readRadiance(const std::vector<unsigned char> &file);
