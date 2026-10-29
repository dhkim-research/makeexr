// makeexr: converts a Radiance HDR picture into an OpenEXR file with the channels X, Y and Z,
// CIE 1931 tristimulus values in cd/m^2 (Radiance values x 179, header EXPOSURE undone).
//
//   makeexr input.hdr > output.exr
//   makeexr input.hdr output.exr

#include "radiance.h"

#include <ImfChannelList.h>
#include <ImfCompression.h>
#include <ImfFrameBuffer.h>
#include <ImfHeader.h>
#include <ImfIO.h>
#include <ImfOutputFile.h>
#include <ImfStandardAttributes.h>
#include <ImfStdIO.h>
#include <ImfStringAttribute.h>
#include <ImfThreading.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {

const char *kVersion = "1.0.0";
const double kWhiteEfficacy = 179.0;  // lm/W, Radiance's luminous efficacy of equal-energy white

const std::array<double, 8> kRadiancePrimaries = {0.640, 0.330, 0.290, 0.600, 0.150, 0.060, 1.0 / 3, 1.0 / 3};
const std::array<double, 8> kRec709Primaries = {0.640, 0.330, 0.300, 0.600, 0.150, 0.060, 0.3127, 0.3290};
const std::array<double, 8> kXyzPrimaries = {1.0, 0.0, 0.0, 1.0, 0.0, 0.0, 1.0 / 3, 1.0 / 3};

using Matrix = std::array<std::array<double, 3>, 3>;

Matrix inverse(const Matrix &m) {
    const double det =
        m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
        m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
        m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    if (std::abs(det) < 1e-12)
        throw std::runtime_error("the PRIMARIES in the header do not form a valid colour space");
    Matrix r;
    r[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) / det;
    r[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) / det;
    r[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) / det;
    r[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) / det;
    r[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) / det;
    r[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) / det;
    r[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) / det;
    r[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) / det;
    r[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) / det;
    return r;
}

// RGB -> XYZ for the given chromaticities, scaled so that RGB (1,1,1) is the white point
// with Y = 1 (as Radiance's compxyz2rgbmat). The Y row is the luminance weights of the space.
Matrix rgbToXyz(const std::array<double, 8> &p) {
    const double wx = p[6], wy = p[7];
    if (wy <= 0.0)
        throw std::runtime_error("the white point in PRIMARIES has y = 0");
    Matrix chroma;
    for (int c = 0; c < 3; ++c) {
        const double x = p[2 * c], y = p[2 * c + 1];
        chroma[0][c] = x;
        chroma[1][c] = y;
        chroma[2][c] = 1.0 - x - y;
    }
    const std::array<double, 3> white = {wx / wy, 1.0, (1.0 - wx - wy) / wy};
    const Matrix inv = inverse(chroma);
    Matrix m;
    for (int c = 0; c < 3; ++c) {
        const double s = inv[c][0] * white[0] + inv[c][1] * white[1] + inv[c][2] * white[2];
        for (int r = 0; r < 3; ++r)
            m[r][c] = chroma[r][c] * s;
    }
    return m;
}

bool samePrimaries(const std::array<double, 8> &a, const std::array<double, 8> &b) {
    for (int i = 0; i < 8; ++i)
        if (std::abs(a[i] - b[i]) > 0.002)
            return false;
    return true;
}

std::vector<unsigned char> readFile(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::runtime_error("cannot open " + path.u8string());
    return std::vector<unsigned char>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// OpenEXR needs to seek back to fill in the line offset table, which stdout cannot do,
// so the file is built in memory and written out in one go.
class MemoryOStream : public Imf::OStream {
public:
    MemoryOStream() : Imf::OStream("<stdout>") {}
    void write(const char c[], int n) override {
        if (pos_ + n > data_.size())
            data_.resize(pos_ + n);
        std::memcpy(data_.data() + pos_, c, static_cast<size_t>(n));
        pos_ += static_cast<size_t>(n);
    }
    uint64_t tellp() override { return pos_; }
    void seekp(uint64_t pos) override { pos_ = static_cast<size_t>(pos); }
    const std::vector<char> &data() const { return data_; }

private:
    std::vector<char> data_;
    size_t pos_ = 0;
};

void writeExr(Imf::OStream &stream, const Imf::Header &header, const std::vector<float> &xyz,
        int width, int height) {
    const size_t pixelStride = 3 * sizeof(float);
    const size_t rowStride = pixelStride * static_cast<size_t>(width);
    char *base = reinterpret_cast<char *>(const_cast<float *>(xyz.data()));
    Imf::FrameBuffer frame;
    frame.insert("X", Imf::Slice(Imf::FLOAT, base + 0 * sizeof(float), pixelStride, rowStride));
    frame.insert("Y", Imf::Slice(Imf::FLOAT, base + 1 * sizeof(float), pixelStride, rowStride));
    frame.insert("Z", Imf::Slice(Imf::FLOAT, base + 2 * sizeof(float), pixelStride, rowStride));
    Imf::OutputFile file(stream, header);
    file.setFrameBuffer(frame);
    file.writePixels(height);
}

void usage(std::ostream &out) {
    out << "makeexr " << kVersion << "\n"
        << "Converts a Radiance HDR picture into an OpenEXR file with the channels X, Y and Z:\n"
        << "CIE 1931 XYZ in cd/m^2, 32-bit float, ZIP compression.\n\n"
        << "Usage:\n"
        << "  makeexr input.hdr > output.exr\n"
        << "  makeexr input.hdr output.exr\n\n"
        << "In Windows PowerShell 5 use the second form; its '>' changes binary output.\n\n"
        << "The colour space comes from the PRIMARIES= line (Radiance primaries if there is none),\n"
        << "or FORMAT=32-bit_rle_xyze. Values are multiplied by 179 and divided by the product\n"
        << "of the EXPOSURE= lines, so Y is luminance in cd/m^2. Not multiplied by 179: XYZE\n"
        << "pictures (their Y is already cd/m^2) and files that hdrspace marked as XYZ in cd/m^2\n"
        << "(HDRSPACE_COLOR= xyz cd/m2).\n\n"
        << "MATLAB (R2022b or newer, Image Processing Toolbox):\n"
        << "  xyz = exrread(\"output.exr\", Channels=[\"X\",\"Y\",\"Z\"]);\n";
}

int run(const std::vector<std::string> &args, const std::vector<fs::path> &paths) {
    if (args.size() == 2 && (args[1] == "-h" || args[1] == "--help" || args[1] == "/?")) {
        usage(std::cout);
        return 0;
    }
    if (args.size() == 2 && (args[1] == "-v" || args[1] == "--version")) {
        std::cout << "makeexr " << kVersion << "\n";
        return 0;
    }
    if (args.size() < 2 || args.size() > 3) {
        usage(std::cerr);
        return 2;
    }
    const fs::path inputPath = paths[1];
    const bool toStdout = args.size() == 2;
#if defined(_WIN32)
    const bool stdoutIsTerminal = _isatty(_fileno(stdout)) != 0;
#else
    const bool stdoutIsTerminal = isatty(STDOUT_FILENO) != 0;
#endif
    if (toStdout && stdoutIsTerminal) {
        std::cerr << "makeexr: give an output file, or redirect: makeexr input.hdr > output.exr\n";
        return 2;
    }

    const RadiancePicture pic = readRadiance(readFile(inputPath));

    // Colour space of the stored values.
    std::array<double, 8> primaries = kRadiancePrimaries;
    std::string space = "Radiance RGB";
    bool inCdm2 = false;
    const std::string &assigned = pic.hdrspaceColor;
    if (!assigned.empty()) {
        if (assigned.rfind("raw", 0) == 0 || assigned.rfind("luminance", 0) == 0)
            throw std::runtime_error("the file is marked as \"" + assigned +
                "\" (HDRSPACE_COLOR); only Radiance RGB, sRGB and XYZ pictures can be converted");
        inCdm2 = assigned.find("cd/m2") != std::string::npos;
    }
    if (pic.format == "32-bit_rle_xyze") {
        // Radiance XYZE pictures (ra_xyze) are photometric: Y is already in cd/m^2.
        primaries = kXyzPrimaries;
        inCdm2 = true;
    } else if (pic.hasPrimaries) {
        primaries = pic.primaries;
    } else if (assigned.rfind("srgb", 0) == 0) {
        primaries = kRec709Primaries;
    } else if (assigned.rfind("xyz", 0) == 0) {
        primaries = kXyzPrimaries;
    } else if (assigned.empty()) {
        std::cerr << "makeexr: note: no PRIMARIES in the header; Radiance RGB primaries are assumed.\n";
    }
    if (samePrimaries(primaries, kXyzPrimaries))
        space = "XYZ";
    else if (samePrimaries(primaries, kRec709Primaries))
        space = "sRGB (Rec. 709)";
    else if (samePrimaries(primaries, kRadiancePrimaries))
        space = "Radiance RGB";
    else
        space = "RGB with the header PRIMARIES";
    if (pic.hasColorCorrection)
        std::cerr << "makeexr: note: the COLORCORR= line in the header is ignored.\n";

    const Matrix toXyz = rgbToXyz(primaries);
    const double scale = (inCdm2 ? 1.0 : kWhiteEfficacy) / pic.exposure;

    const size_t pixels = static_cast<size_t>(pic.width) * pic.height;
    std::vector<float> xyz(pixels * 3);
    double maxY = 0.0;
    for (size_t i = 0; i < pixels; ++i) {
        const float *rgb = &pic.values[3 * i];
        for (int r = 0; r < 3; ++r) {
            const double v = scale * (toXyz[r][0] * rgb[0] + toXyz[r][1] * rgb[1] + toXyz[r][2] * rgb[2]);
            xyz[3 * i + r] = static_cast<float>(v);
        }
        maxY = std::max(maxY, static_cast<double>(xyz[3 * i + 1]));
    }

    Imf::Header header(pic.width, pic.height);
    header.compression() = Imf::ZIP_COMPRESSION;
    header.channels().insert("X", Imf::Channel(Imf::FLOAT));
    header.channels().insert("Y", Imf::Channel(Imf::FLOAT));
    header.channels().insert("Z", Imf::Channel(Imf::FLOAT));
    std::ostringstream note;
    note << "CIE 1931 XYZ in cd/m^2 from " << inputPath.filename().u8string() << " (" << space
         << "; x " << (inCdm2 ? 1.0 : kWhiteEfficacy) << " / EXPOSURE " << pic.exposure << ")";
    Imf::addComments(header, note.str());
    header.insert("units", Imf::StringAttribute("cd/m^2"));
    header.insert("software", Imf::StringAttribute(std::string("makeexr ") + kVersion));
    std::string radianceHeader;
    for (const std::string &line : pic.headerLines)
        radianceHeader += line + "\n";
    header.insert("radianceHeader", Imf::StringAttribute(radianceHeader));

    Imf::setGlobalThreadCount(static_cast<int>(std::max(1u, std::thread::hardware_concurrency())));

    if (toStdout) {
        MemoryOStream memory;
        writeExr(memory, header, xyz, pic.width, pic.height);
#if defined(_WIN32)
        _setmode(_fileno(stdout), _O_BINARY);
#endif
        const std::vector<char> &bytes = memory.data();
        if (std::fwrite(bytes.data(), 1, bytes.size(), stdout) != bytes.size() || std::fflush(stdout) != 0)
            throw std::runtime_error("writing to the output failed");
    } else {
        const fs::path outputPath = paths[2];
        std::ofstream out(outputPath, std::ios::binary | std::ios::trunc);
        if (!out)
            throw std::runtime_error("cannot create " + outputPath.u8string());
        {
            Imf::StdOFStream stream(out, outputPath.u8string().c_str());
            writeExr(stream, header, xyz, pic.width, pic.height);
        }
        out.close();
        if (!out)
            throw std::runtime_error("writing " + outputPath.u8string() + " failed");
    }
    std::cerr << inputPath.filename().u8string() << ": " << pic.width << " x " << pic.height << ", "
              << space << ", EXPOSURE " << pic.exposure << " -> X Y Z in cd/m^2 (max Y "
              << maxY << ")\n";
    return 0;
}

}  // namespace

#if defined(_WIN32)
int wmain(int argc, wchar_t **argv) {
    std::vector<std::string> args;
    std::vector<fs::path> paths;
    for (int i = 0; i < argc; ++i) {
        paths.emplace_back(argv[i]);
        args.push_back(paths.back().u8string());
    }
#else
int main(int argc, char **argv) {
    std::vector<std::string> args(argv, argv + argc);
    std::vector<fs::path> paths(argv, argv + argc);
#endif
    try {
        return run(args, paths);
    } catch (const std::exception &e) {
        std::cerr << "makeexr: " << e.what() << "\n";
        return 1;
    }
}
