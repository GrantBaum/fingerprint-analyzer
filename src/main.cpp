//nessecary libs for ts
#include <filesystem>
#include <iostream>
#include <string>
#include "fingerprint.hpp"
#include "ui.hpp"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: " << argv[0] << " <image>" << std::endl;
        return 1;
    }
    std::string path = argv[1];

    const int window = 15;          // binarize window size, change to test others
    const int maskBlock = 16;       // segmentation block size in pixels
    const float maskVariance = 0.1f; // blocks below this variance are background

    //every print gets its own folder, like out/101_1/
    std::string outDir = "out/" + std::filesystem::path(path).stem().string() + "/";
    std::filesystem::create_directories(outDir);
    ui::banner();

    try {
        // ---- Load ----------------------------------------------------------
        GrayImage img;
        {
            ui::Stage s("Loading");
            img = loadGray(path);
            saveGray(img, outDir + "00_raw.png");
            s.done(std::to_string(img.width) + " x " + std::to_string(img.height) +
                   "  " + std::filesystem::path(path).filename().string());
        }

        // ---- Normalize -----------------------------------------------------
        {
            ui::Stage s("Normalizing");
            NormStats stats = normalize(img);   // mean and std dev of the original
            saveGray(img, outDir + "01_normalized.png");
            s.done("mean " + ui::fixed(stats.mean) + "  std " + ui::fixed(stats.stdDev));
        }

        // ---- Mask ----------------------------------------------------------
        BinaryImage mask;
        {
            ui::Stage s("Masking");
            mask = segmentMask(img, maskBlock, maskVariance);
            saveBinary(mask, outDir + "02_mask.png");

            long long finger = 0;
            for (uint8_t p : mask.pixels) finger += p;
            double percent = 100.0 * finger / mask.pixels.size();
            s.done("block " + std::to_string(maskBlock) + "  " + ui::fixed(percent, 1) + "% finger");
        }

        // ---- Binarize (Checkpoint A) ---------------------------------------
        BinaryImage bin;
        {
            ui::Stage s("Binarizing");
            bin = binarize(img, window);
            applyMask(bin, mask);   //wipe background speckle before thinning
            saveBinary(bin, outDir + "06_binary.png");

            long long ridge = 0;
            for (uint8_t p : bin.pixels) ridge += p;
            double percent = 100.0 * ridge / bin.pixels.size();
            s.done("window " + std::to_string(window) + "  " + ui::fixed(percent, 1) + "% ridge");
        }

        // ---- Thin (Checkpoint B) -------------------------------------------
        {
            ui::Stage s("Thinning");
            long long before = 0;
            for (uint8_t p : bin.pixels) before += p;

            int passes = thin(bin);
            saveBinary(bin, outDir + "07_skeleton.png");

            long long after = 0;
            for (uint8_t p : bin.pixels) after += p;
            s.done(std::to_string(passes) + " passes  " + ui::num(before) + " -> " +
                   ui::num(after) + " px");
        }

        ui::finalScreen(true, "Analysis complete. Images saved to " + outDir);
    } catch (const std::exception& e) {
        ui::finalScreen(false, e.what());
        return 1;
    }

    return 0;
}
