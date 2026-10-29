#include "radiance.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <stdexcept>

namespace {

class Reader {
public:
    explicit Reader(const std::vector<unsigned char> &data) : data_(data) {}

    bool atEnd() const { return pos_ >= data_.size(); }

    int get() {
        if (pos_ >= data_.size())
            throw std::runtime_error("the file ends in the middle of the pixel data");
        return data_[pos_++];
    }

    // One header line without the newline; false at the end of the file.
    bool line(std::string &out) {
        out.clear();
        if (atEnd())
            return false;
        while (!atEnd()) {
            const char c = static_cast<char>(data_[pos_++]);
            if (c == '\n')
                return true;
            out.push_back(c);
        }
        return true;
    }

private:
    const std::vector<unsigned char> &data_;
    size_t pos_ = 0;
};

bool startsWith(const std::string &s, const char *prefix) {
    return s.compare(0, std::strlen(prefix), prefix) == 0;
}

std::string trim(const std::string &s) {
    const size_t a = s.find_first_not_of(" \t\r");
    if (a == std::string::npos)
        return std::string();
    const size_t b = s.find_last_not_of(" \t\r");
    return s.substr(a, b - a + 1);
}

// Old-style scanline: flat pixels, where a pixel 1,1,1,n repeats the previous pixel
// n << shift times (shift grows by 8 for consecutive repeat pixels).
void readOldScanline(Reader &in, unsigned char *scan, int length, const unsigned char first[4]) {
    int shift = 0;
    int i = 0;
    bool useFirst = true;
    while (i < length) {
        unsigned char px[4];
        for (int c = 0; c < 4; ++c)
            px[c] = useFirst ? first[c] : static_cast<unsigned char>(in.get());
        useFirst = false;
        if (px[0] == 1 && px[1] == 1 && px[2] == 1) {
            if (i == 0)
                throw std::runtime_error("bad run-length data at the start of a scanline");
            const long count = static_cast<long>(px[3]) << shift;
            for (long k = 0; k < count && i < length; ++k, ++i)
                std::memcpy(scan + 4 * i, scan + 4 * (i - 1), 4);
            shift += 8;
        } else {
            std::memcpy(scan + 4 * i, px, 4);
            ++i;
            shift = 0;
        }
    }
}

void readScanline(Reader &in, unsigned char *scan, int length) {
    unsigned char head[4];
    for (int c = 0; c < 4; ++c)
        head[c] = static_cast<unsigned char>(in.get());
    const bool newFormat = length >= 8 && length <= 0x7fff &&
        head[0] == 2 && head[1] == 2 && (head[2] & 0x80) == 0;
    if (!newFormat) {
        readOldScanline(in, scan, length, head);
        return;
    }
    if (((head[2] << 8) | head[3]) != length)
        throw std::runtime_error("a scanline length does not match the picture width");
    // New format: the four components are stored one after another, each run-length coded.
    for (int c = 0; c < 4; ++c) {
        int i = 0;
        while (i < length) {
            int code = in.get();
            if (code > 128) {
                code &= 127;
                const unsigned char value = static_cast<unsigned char>(in.get());
                if (i + code > length)
                    throw std::runtime_error("a run goes past the end of a scanline");
                for (int k = 0; k < code; ++k)
                    scan[4 * (i++) + c] = value;
            } else {
                if (code == 0 || i + code > length)
                    throw std::runtime_error("bad run-length data in a scanline");
                for (int k = 0; k < code; ++k)
                    scan[4 * (i++) + c] = static_cast<unsigned char>(in.get());
            }
        }
    }
}

}  // namespace

RadiancePicture readRadiance(const std::vector<unsigned char> &file) {
    Reader in(file);
    RadiancePicture pic;

    std::string text;
    if (!in.line(text) || !startsWith(text, "#?"))
        throw std::runtime_error("not a Radiance picture (the file does not start with #?)");

    // Information header, up to the empty line.
    bool headerEnded = false;
    while (in.line(text)) {
        if (!text.empty() && text.back() == '\r')
            text.pop_back();
        if (text.empty()) {
            headerEnded = true;
            break;
        }
        pic.headerLines.push_back(text);
        if (startsWith(text, "FORMAT=")) {
            pic.format = trim(text.substr(7));
        } else if (startsWith(text, "EXPOSURE=")) {
            // Only lines that start with EXPOSURE= count (Radiance's own rule).
            const double e = std::atof(text.c_str() + 9);
            if (e > 0.0)
                pic.exposure *= e;
        } else if (startsWith(text, "PRIMARIES=")) {
            std::istringstream values(text.substr(10));
            for (double &v : pic.primaries)
                values >> v;
            if (!values)
                throw std::runtime_error("PRIMARIES= needs eight numbers");
            pic.hasPrimaries = true;
        } else if (startsWith(text, "HDRSPACE_COLOR=")) {
            std::string v = trim(text.substr(15));
            for (char &c : v)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            pic.hdrspaceColor = v;
        } else if (startsWith(text, "COLORCORR=")) {
            pic.hasColorCorrection = true;
        }
    }
    if (!headerEnded)
        throw std::runtime_error("the header has no end (missing empty line)");
    if (!pic.format.empty() && pic.format != "32-bit_rle_rgbe" && pic.format != "32-bit_rle_xyze")
        throw std::runtime_error("unsupported FORMAT=" + pic.format);

    // Resolution line, e.g. "-Y 512 +X 768": the first axis is the scanline axis.
    if (!in.line(text))
        throw std::runtime_error("the resolution line is missing");
    char s1 = 0, a1 = 0, s2 = 0, a2 = 0;
    int n1 = 0, n2 = 0;
    {
        char axis1[3] = {0}, axis2[3] = {0};
        if (std::sscanf(text.c_str(), "%2s %d %2s %d", axis1, &n1, axis2, &n2) != 4)
            throw std::runtime_error("cannot read the resolution line \"" + text + "\"");
        s1 = axis1[0]; a1 = axis1[1]; s2 = axis2[0]; a2 = axis2[1];
    }
    const bool valid = (s1 == '+' || s1 == '-') && (s2 == '+' || s2 == '-') &&
        ((a1 == 'Y' && a2 == 'X') || (a1 == 'X' && a2 == 'Y')) && n1 > 0 && n2 > 0;
    if (!valid)
        throw std::runtime_error("cannot read the resolution line \"" + text + "\"");

    const bool rowsAreScanlines = a1 == 'Y';
    pic.width = rowsAreScanlines ? n2 : n1;
    pic.height = rowsAreScanlines ? n1 : n2;
    pic.values.assign(static_cast<size_t>(pic.width) * pic.height * 3, 0.0f);

    std::vector<unsigned char> scan(static_cast<size_t>(n2) * 4);
    for (int s = 0; s < n1; ++s) {
        readScanline(in, scan.data(), n2);
        for (int p = 0; p < n2; ++p) {
            // Radiance y points up: "-Y" scanlines run from the top, "+X" pixels from the left.
            int row, col;
            if (rowsAreScanlines) {
                row = s1 == '-' ? s : n1 - 1 - s;
                col = s2 == '+' ? p : n2 - 1 - p;
            } else {
                col = s1 == '+' ? s : n1 - 1 - s;
                row = s2 == '-' ? p : n2 - 1 - p;
            }
            const unsigned char *px = &scan[4 * static_cast<size_t>(p)];
            float *out = &pic.values[(static_cast<size_t>(row) * pic.width + col) * 3];
            if (px[3] == 0) {
                out[0] = out[1] = out[2] = 0.0f;
            } else {
                // Radiance colr_color(): (mantissa + 0.5) * 2^(exponent - 136)
                const double f = std::ldexp(1.0, static_cast<int>(px[3]) - 136);
                for (int c = 0; c < 3; ++c)
                    out[c] = static_cast<float>((px[c] + 0.5) * f);
            }
        }
    }
    return pic;
}
