//nessecary libs for ts
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>
#include "fingerprint.hpp"
#include "ui.hpp"

// ---- pipeline settings ------------------------------------------------------
const int BINARIZE_WINDOW = 15;      // binarize window size, change to test others
const int MASK_BLOCK = 16;           // segmentation block size in pixels
const float MASK_VARIANCE = 0.1f;    // blocks below this variance are background

// outDirFor: the folder a print's results wait in, like out/101_1/
std::string outDirFor(const std::string& path) {
    return "out/" + std::filesystem::path(path).stem().string() + "/";
}

// analyzePrint: runs the whole pipeline on one image, saves every debug image
// in outDir and returns the print's template.
Template analyzePrint(const std::string& path, const std::string& outDir) {
    std::filesystem::create_directories(outDir);

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
        mask = segmentMask(img, MASK_BLOCK, MASK_VARIANCE);
        saveBinary(mask, outDir + "02_mask.png");

        long long finger = 0;
        for (uint8_t p : mask.pixels) finger += p;
        double percent = 100.0 * finger / mask.pixels.size();
        s.done("block " + std::to_string(MASK_BLOCK) + "  " + ui::fixed(percent, 1) + "% finger");
    }

    // ---- Binarize (Checkpoint A) ---------------------------------------
    BinaryImage bin;
    {
        ui::Stage s("Binarizing");
        bin = binarize(img, BINARIZE_WINDOW);
        applyMask(bin, mask);   //wipe background speckle before thinning
        saveBinary(bin, outDir + "06_binary.png");

        long long ridge = 0;
        for (uint8_t p : bin.pixels) ridge += p;
        double percent = 100.0 * ridge / bin.pixels.size();
        s.done("window " + std::to_string(BINARIZE_WINDOW) + "  " + ui::fixed(percent, 1) + "% ridge");
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

    // ---- Minutiae -------------------------------------------------------
    std::vector<Minutia> minutiae;
    {
        ui::Stage s("Minutiae");
        minutiae = findMinutiae(bin);
        saveMinutiae(bin, minutiae, outDir + "08_minutiae.png");

        int endings = 0;
        for (const Minutia& m : minutiae) {
            if (m.type == ENDING) endings++;
        }
        s.done(std::to_string(minutiae.size()) + " found  " + std::to_string(endings) +
               " endings  " + std::to_string(minutiae.size() - endings) + " bifurcations");
    }

    // ---- Filter --------------------------------------------------------
    {
        ui::Stage s("Filtering");
        FilterReport report;
        size_t before = minutiae.size();
        minutiae = filterMinutiae(minutiae, bin, mask, report);
        saveMinutiae(bin, minutiae, outDir + "09_filtered.png");
        s.done(std::to_string(before) + " -> " + std::to_string(minutiae.size()) + " minutiae");
        ui::info("border " + std::to_string(report.border) + "  spurs " + std::to_string(report.spurs) +
                 "  islands " + std::to_string(report.islands) + "  broken " + std::to_string(report.broken) +
                 "  close " + std::to_string(report.close));
    }

    // ---- Template ------------------------------------------------------
    Template t;
    {
        ui::Stage s("Template");
        t.width = bin.width;
        t.height = bin.height;
        t.minutiae = minutiae;
        saveTemplate(t, outDir + "template.tmpl");
        s.done(std::to_string(t.minutiae.size()) + " minutiae  template.tmpl");
    }

    return t;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: " << argv[0] << " <image>" << std::endl;
        return 1;
    }
    std::string path = argv[1];
    std::string outDir = outDirFor(path);
    ui::banner();

    try {
        analyzePrint(path, outDir);
        ui::finalScreen(true, "Analysis complete. Results waiting in " + outDir);
    } catch (const std::exception& e) {
        ui::finalScreen(false, e.what());
        return 1;
    }

    return 0;
}
