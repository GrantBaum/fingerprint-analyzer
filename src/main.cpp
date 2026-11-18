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

// askNameAndFinger: asks who the print belongs to. Keeps asking until the name
// is not empty and has no commas, since a comma would break index.csv.
void askNameAndFinger(std::string& name, std::string& finger) {
    while(true){
        name = ui::ask("Name:");
        if(std::cin.eof()) throw std::runtime_error("askNameAndFinger: input ended");
        if(name.empty()){
            ui::warn("name can't be empty");
        }
        else if(name.find(',') != std::string::npos){
            ui::warn("name can't contain commas");
        }
        else break;
    }
    while(true){
        finger = ui::ask("Finger (like \"right index\", blank for unknown):");
        if(finger.empty()) finger = "unknown";
        if(finger.find(',') != std::string::npos){
            ui::warn("finger can't contain commas");
        }
        else break;
    }
}

// runCompare: analyzes a second print, matches it against this one and shows
// MATCH or NO MATCH. aligned.png goes in out/<this>_vs_<other>/.
void runCompare(const std::string& path, const Template& t) {
    std::string otherPath = ui::ask("Path to the other print:");
    if(otherPath.empty()) return; //changed their mind
    if(!std::filesystem::exists(otherPath)){
        ui::warn("no file at " + otherPath);
        return;
    }

    ui::info("analyzing " + otherPath);
    Template other = analyzePrint(otherPath, outDirFor(otherPath));

    MatchResult result;
    std::string pairDir = "out/" + std::filesystem::path(path).stem().string() + "_vs_" +
                          std::filesystem::path(otherPath).stem().string() + "/";
    {
        ui::Stage s("Matching");
        result = matchTemplates(t, other);
        std::filesystem::create_directories(pairDir);
        saveAligned(t, other, result, pairDir + "aligned.png");
        s.done(std::to_string(result.matched) + " pairs  " + std::to_string(result.alignment.votes) + " votes  " +
               "rotation " + ui::fixed(result.alignment.rotation * 180 / PI, 1) + " deg");
    }

    ui::verdict(result.score >= MATCH_THRESHOLD,
                "score " + ui::fixed(result.score, 3) + "  (threshold " + ui::fixed(MATCH_THRESHOLD, 3) + ")");
    ui::info("overlay saved to " + pairDir + "aligned.png");
}

// runSearch: scores this print against the whole database and shows the top 5.
// The software only suggests. A person has to compare the prints by eye.
void runSearch(const Template& t) {
    std::vector<SearchHit> hits;
    {
        ui::Stage s("Searching");
        hits = searchDatabase(t, 5);
        s.done(std::to_string(readIndex().size()) + " prints in database");
    }
    if(hits.empty()){
        ui::warn("the database is empty, catalogue some prints first");
        return;
    }

    //one row per hit, the paths are there so the examiner can open them
    const int widths[7] = {4, 4, 16, 14, 6, 16, 16};
    const std::string header[7] = {"#", "ID", "Name", "Finger", "Score", "Raw", "Skeleton"};
    std::cout << "\n";
    ui::tableRow(header, widths, 7, true);
    for(size_t i = 0; i < hits.size(); i++){
        const SearchHit& h = hits[i];
        std::string score = ui::fixed(h.result.score, 3);
        if(h.result.score >= MATCH_THRESHOLD) score += "*"; //above the match threshold
        const std::string row[7] = {std::to_string(i + 1), h.entry.id, h.entry.name, h.entry.finger,
                                    score, h.entry.raw, h.entry.skeleton};
        ui::tableRow(row, widths, 7);
    }
    std::cout << "\n";
    ui::info("* = at or above the match threshold (" + ui::fixed(MATCH_THRESHOLD, 3) + ")");
    ui::info("these are suggestions. compare the prints by eye before deciding anything");
}

// runMenu: the menu shown after analyzing a print. Results wait in outDir until
// the user catalogues or discards them. Returns the text for the final screen.
std::string runMenu(const std::string& path, const std::string& outDir, const Template& t) {
    const std::string labels[5] = {
        "Catalogue this print",
        "Compare against another print",
        "Search the database",
        "Discard results",
        "Quit (leave results in " + outDir + ")"
    };

    while(true){
        char choice = ui::menu("What next?", "abcdq", labels);

        if(choice == 'a'){
            std::string name, finger;
            askNameAndFinger(name, finger);
            try {
                ui::Stage s("Cataloguing");
                std::string id = catalogue(path, outDir, name, finger);
                s.done("ID " + id + "  " + name + "  " + finger);
                return "Catalogued as " + id + ". Files are in db/";
            } catch (const std::exception& e) {
                //out/ is untouched, so the user can fix the problem and try again
                ui::warn(std::string(e.what()) + ". Results are still in " + outDir);
            }
        }
        else if(choice == 'b'){
            runCompare(path, t);
        }
        else if(choice == 'c'){
            runSearch(t);
        }
        else if(choice == 'd'){
            discard(outDir);
            return "Results discarded";
        }
        else{
            return "Results left in " + outDir;
        }
    }
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
        Template t = analyzePrint(path, outDir);
        ui::success("Analysis complete. Results waiting in " + outDir);
        ui::finalScreen(true, runMenu(path, outDir, t));
    } catch (const std::exception& e) {
        ui::finalScreen(false, e.what());
        return 1;
    }

    return 0;
}
