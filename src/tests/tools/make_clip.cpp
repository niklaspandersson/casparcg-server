// caspar_make_clip: writes a test clip like caspar::test::make_clip(), for tests outside C++.
//
//   caspar_make_clip <path> [--format 720p5000] [--frames 50] [--codec ffv1] [--pixel-format yuv420p]
//                    [--audio-codec pcm_s32le] [--audio-channels 2] [--no-audio]
//
// The container follows the path's extension. Frames carry stamped indices and the audio is a ramp
// of sample numbers; see src/tests/support/test_media.h.

#include <test_media.h>

#include <common/utf.h>

#include <iostream>
#include <string>

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::cerr << "usage: caspar_make_clip <path> [--format F] [--frames N] [--codec C] [--pixel-format P] "
                     "[--audio-codec C] [--audio-channels N] [--no-audio]\n";
        return 2;
    }

    caspar::test::clip_options options;
    for (int n = 2; n < argc; ++n) {
        std::string arg   = argv[n];
        auto        value = [&] {
            if (n + 1 >= argc)
                throw std::invalid_argument(arg + " needs a value");
            return std::string(argv[++n]);
        };
        if (arg == "--format")
            options.format = caspar::u16(value());
        else if (arg == "--frames")
            options.frames = std::stoi(value());
        else if (arg == "--codec")
            options.codec = value();
        else if (arg == "--pixel-format")
            options.pixel_format = value();
        else if (arg == "--audio-codec")
            options.audio_codec = value();
        else if (arg == "--audio-channels")
            options.audio_channels = std::stoi(value());
        else if (arg == "--no-audio")
            options.audio = false;
        else {
            std::cerr << "unknown option " << arg << "\n";
            return 2;
        }
    }

    try {
        caspar::test::write_clip(argv[1], options);
    } catch (std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
    return 0;
}
