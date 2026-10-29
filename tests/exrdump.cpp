// Test helper: prints the channels of an EXR file and then every pixel as "X Y Z", top row first.
#include <ImfArray.h>
#include <ImfChannelList.h>
#include <ImfFrameBuffer.h>
#include <ImfHeader.h>
#include <ImfInputFile.h>
#include <ImfStringAttribute.h>

#include <cstdio>
#include <exception>
#include <vector>

int main(int argc, char **argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: exrdump file.exr\n");
        return 2;
    }
    try {
        Imf::InputFile file(argv[1]);
        const Imath::Box2i dw = file.header().dataWindow();
        const int w = dw.max.x - dw.min.x + 1, h = dw.max.y - dw.min.y + 1;
        std::printf("size %d %d\n", w, h);
        for (auto it = file.header().channels().begin(); it != file.header().channels().end(); ++it)
            std::printf("channel %s type %d\n", it.name(), static_cast<int>(it.channel().type));
        std::printf("compression %d\n", static_cast<int>(file.header().compression()));
        if (const auto *units = file.header().findTypedAttribute<Imf::StringAttribute>("units"))
            std::printf("units %s\n", units->value().c_str());
        std::vector<float> px(static_cast<size_t>(w) * h * 3);
        Imf::FrameBuffer fb;
        const char *names[3] = {"X", "Y", "Z"};
        for (int c = 0; c < 3; ++c) {
            char *base = reinterpret_cast<char *>(px.data() + c) -
                (static_cast<size_t>(dw.min.x) + static_cast<size_t>(dw.min.y) * w) * 3 * sizeof(float);
            fb.insert(names[c], Imf::Slice(Imf::FLOAT, base, 3 * sizeof(float), 3 * sizeof(float) * w));
        }
        file.setFrameBuffer(fb);
        file.readPixels(dw.min.y, dw.max.y);
        std::printf("pixels\n");
        for (size_t i = 0; i < px.size(); i += 3)
            std::printf("%.9g %.9g %.9g\n", px[i], px[i + 1], px[i + 2]);
    } catch (const std::exception &e) {
        std::fprintf(stderr, "exrdump: %s\n", e.what());
        return 1;
    }
    return 0;
}
