//nessecary libs for ts
#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>
#include "fingerprint.hpp"
#include "ui.hpp"

const int BINARIZE_WINDOW = 15;      // binarize window size, change to test others
const int MASK_BLOCK = 16;           // segmentation block size in pixels
const float MASK_VARIANCE = 0.1f;    // blocks below this variance are background
bool useEnhance = true;

// pipelineVersion: what goes in the version column of index.csv. Templates made
// without enhancement get "-plain" so they're never mixed up with normal ones.
std::string pipelineVersion() {
    if (useEnhance) return PIPELINE_VERSION;
    return std::string(PIPELINE_VERSION) + "-plain";
}

// outDirFor: the folder a print's results wait in, like out/101_1/
std::string outDirFor(const std::string& path) {
    return "out/" + std::filesystem::path(path).stem().string() + "/";
}

// analyzePrint: runs the whole pipeline on one image, saves every debug image
// in outDir and returns the print's template.
Template analyzePrint(const std::string& path, const std::string& outDir) {

    // ---- Load ----------------------------------------------------------
    GrayImage img;
    {
        ui::Stage s("Loading");
        img = loadGray(path);
        std::filesystem::create_directories(outDir); //only once we know the image is good
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

    // ---- Enhance (optional) --------------------------------------------
    if (useEnhance) {
        BlockField orient;
        {
            ui::Stage s("Orientation");
            orient = orientationField(img, MASK_BLOCK);
            saveOrientation(img, orient, mask, outDir + "03_orientation.png");
            s.done(std::to_string(orient.width) + " x " + std::to_string(orient.height) + " blocks");
        }

        BlockField freq;
        {
            ui::Stage s("Frequency");
            freq = frequencyField(img, orient, mask);
            saveFrequency(freq, mask, outDir + "04_frequency.png");

            //average ridge period over the finger, as a sanity check (about 9 px at 500 dpi)
            double sum = 0;
            int count = 0;
            for (float f : freq.values) {
                if (f > 0) {
                    sum += 1.0 / f;
                    count++;
                }
            }
            s.done("mean ridge period " + ui::fixed(sum / count, 1) + " px");
        }

        {
            ui::Stage s("Gabor");
            img = gaborFilter(img, orient, freq, mask);
            saveGray(img, outDir + "05_enhanced.png");
            s.done("ridges cleaned");
        }
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

        int endings = 0, forks = 0, dots = 0, crossings = 0;
        for (const Minutia& m : minutiae) {
            if (m.type == ENDING) endings++;
            if (m.type == BIFURCATION) forks++;
            if (m.type == DOT) dots++;
            if (m.type == CROSSOVER) crossings++;
        }
        s.done(std::to_string(minutiae.size()) + " found  " + std::to_string(endings) + " endings  " +
               std::to_string(forks) + " bifurcations");
        ui::info("dots " + std::to_string(dots) + "  crossings " + std::to_string(crossings));
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
                 "  close " + std::to_string(report.close) + "  kept as dots " + std::to_string(report.dots));
    }

    // ---- Shapes --------------------------------------------------------
    {
        ui::Stage s("Shapes");
        std::vector<Minutia> shapes = findShapes(minutiae, bin);
        int shortRidges = 0, spurs = 0, lakes = 0, crossovers = 0;
        for (const Minutia& m : shapes) {
            if (m.type == SHORT_RIDGE) shortRidges++;
            if (m.type == SPUR) spurs++;
            if (m.type == LAKE) lakes++;
            if (m.type == CROSSOVER) crossovers++;
        }
        minutiae.insert(minutiae.end(), shapes.begin(), shapes.end());
        saveMinutiae(bin, minutiae, outDir + "10_shapes.png");
        s.done(std::to_string(shapes.size()) + " found  " + std::to_string(shortRidges) + " short ridges  " +
               std::to_string(spurs) + " spurs");
        ui::info("lakes " + std::to_string(lakes) + "  crossovers " + std::to_string(crossovers));
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

    ui::scan("Comparing");
    ui::verdict(result.score >= MATCH_THRESHOLD,
                "score " + ui::fixed(result.score, 3) + "   " + std::to_string(result.matched) + " points line up");
    ui::info("threshold " + ui::fixed(MATCH_THRESHOLD, 3) + "   overlay saved to " + pairDir + "aligned.png");
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

    //one row per hit, the paths are there so the examiner can open them.
    //the bar shows each score next to the threshold: a full bar is twice the
    //threshold, so a half full bar is right on the line
    ui::scan("Ranking");
    ui::header("CANDIDATE LIST");
    const int widths[8] = {3, 4, 14, 12, 6, 10, 16, 16};
    const std::string header[8] = {"#", "ID", "Name", "Finger", "Score", "", "Raw", "Skeleton"};
    ui::tableRow(header, widths, 8, true);
    for(size_t i = 0; i < hits.size(); i++){
        const SearchHit& h = hits[i];
        std::string score = ui::fixed(h.result.score, 3);
        if(h.result.score >= MATCH_THRESHOLD) score += "*"; //above the match threshold
        const std::string row[8] = {std::to_string(i + 1), h.entry.id, h.entry.name, h.entry.finger,
                                    score, "", h.entry.raw, h.entry.skeleton};
        //print the row with the bar spliced into the empty column
        std::string before, after;
        for(int c = 0; c < 5; c++) before += ui::pad(row[c], widths[c]) + "  ";
        for(int c = 6; c < 8; c++) after += ui::pad(row[c], widths[c]) + "  ";
        std::cout << "  ";
        ui::typeOut(before, 4);
        std::cout << ui::scoreBar(h.result.score, 2 * MATCH_THRESHOLD, widths[5]) << "  ";
        ui::typeOut(after, 4);
        std::cout << "\n";
    }
    std::cout << "\n";
    ui::info("* = at or above the match threshold (" + ui::fixed(MATCH_THRESHOLD, 3) + ")");
    ui::warn("THE SOFTWARE SUGGESTS. THE EXAMINER DECIDES.");
    ui::info("open the raw and skeleton files and compare them by eye");
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
        char choice = ui::menu("CASE OPTIONS", "abcdq", labels);

        if(choice == 'a'){
            std::string name, finger;
            askNameAndFinger(name, finger);
            try {
                ui::Stage s("Cataloguing");
                std::string id = catalogue(path, outDir, name, finger, pipelineVersion());
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

// listImages: every .tif, .png, .bmp and .jpg in a folder, sorted by name.
std::vector<std::string> listImages(const std::string& folder) {
    if(!std::filesystem::is_directory(folder)){
        throw std::runtime_error("listImages: " + folder + " is not a folder");
    }

    std::vector<std::string> images;
    for(const auto& file : std::filesystem::directory_iterator(folder)){
        if(!file.is_regular_file()) continue;

        //lowercase the extension so .TIF counts too
        std::string ext = file.path().extension().string();
        for(char& c : ext) c = std::tolower(static_cast<unsigned char>(c));
        if(ext == ".tif" || ext == ".png" || ext == ".bmp" || ext == ".jpg"){
            images.push_back(file.path().string());
        }
    }
    std::sort(images.begin(), images.end());
    return images;
}

// runBatch: fast mode. Analyzes and catalogues every image in a folder, one
// after another, with no pauses. Each print still shows its stages. The name
// comes from the file name and the finger is "unknown". This is how a pile of
// unknown prints goes into the database.
std::string runBatch(const std::string& folder) {
    std::vector<std::string> images = listImages(folder);
    if(images.empty()){
        throw std::runtime_error("runBatch: no .tif, .png, .bmp or .jpg files in " + folder);
    }

    ui::fast = true; //no pauses and no typing effects until the batch is done
    ui::warn("FAST MODE  " + std::to_string(images.size()) + " prints from " + folder + ", pauses off");
    auto t0 = std::chrono::steady_clock::now();

    std::vector<std::string> failed;   // "file: reason" for each one that broke
    std::string firstId, lastId;
    for(size_t i = 0; i < images.size(); i++){
        std::string file = std::filesystem::path(images[i]).filename().string();
        std::string name = std::filesystem::path(images[i]).stem().string();
        ui::header("EXHIBIT " + std::to_string(i + 1) + " OF " + std::to_string(images.size()) + "  " + file);
        try {
            std::string outDir = outDirFor(images[i]);
            analyzePrint(images[i], outDir);
            ui::Stage s("Cataloguing");
            lastId = catalogue(images[i], outDir, name, "unknown", pipelineVersion());
            s.done("ID " + lastId + "  " + name + "  unknown");
            if(firstId.empty()) firstId = lastId;
        } catch (const std::exception& e) {
            failed.push_back(file + ": " + e.what()); //keep going, one bad print shouldn't stop the rest
            ui::warn(e.what());
        }
    }

    ui::fast = false;
    double seconds = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - t0).count() / 1000.0;
    int good = images.size() - failed.size();

    //the report. a box on screen, plain lines when piped to a file
    std::string ids = "none";
    if(!firstId.empty()) ids = firstId + " to " + lastId;
    std::string failText = std::to_string(failed.size());
    if(!failed.empty()) failText = ui::paint(ui::RED, failText + "   see the warnings above");
    const std::string report[5] = {
        "folder       " + folder,
        "catalogued   " + ui::paint(ui::BOLD, std::to_string(good) + " of " + std::to_string(images.size())) + " prints",
        "IDs          " + ids,
        "failed       " + failText,
        "time         " + ui::fixed(seconds, 1) + " s"
    };
    if(ui::enabled){
        ui::scan("Tallying");
        ui::box("BATCH REPORT", report, 5);
    }
    else{
        for(const std::string& r : report) ui::line(r);
    }
    for(const std::string& f : failed) ui::warn(f);

    if(firstId.empty()) return "Nothing was catalogued";
    return "Catalogued " + std::to_string(good) + " prints as IDs " + ids;
}

// runSingle: the normal one print analysis. Runs the pipeline, then the case
// menu. Returns the text for the final screen.
std::string runSingle(const std::string& path) {
    std::string outDir = outDirFor(path);
    Template t = analyzePrint(path, outDir);
    ui::success("Analysis complete. Results waiting in " + outDir);
    return runMenu(path, outDir, t);
}

// DataEntry: one thing in data/ that the start menu can pick, a print or a
// folder of prints.
struct DataEntry {
    std::string path;
    bool folder = false;
    int prints = 0;     // how many prints a folder holds
};

// scanData: every print in data/, then every folder in data/ that has prints
// in it, each sorted by name.
std::vector<DataEntry> scanData() {
    std::vector<DataEntry> entries;
    if(!std::filesystem::is_directory("data")) return entries; //nothing there yet

    for(const std::string& path : listImages("data")){
        DataEntry e;
        e.path = path;
        entries.push_back(e);
    }

    std::vector<std::string> folders;
    for(const auto& item : std::filesystem::directory_iterator("data")){
        if(item.is_directory()) folders.push_back(item.path().string());
    }
    std::sort(folders.begin(), folders.end());
    for(const std::string& path : folders){
        DataEntry e;
        e.path = path;
        e.folder = true;
        e.prints = listImages(path).size();
        if(e.prints > 0) entries.push_back(e); //skip empty folders
    }
    return entries;
}

// confirmBatch: asks before a batch writes a whole folder into the database.
bool confirmBatch(const std::string& folder, int prints) {
    std::string answer = ui::ask("Catalogue all " + std::to_string(prints) + " prints in " + folder +
                                 " as unknown? (y/n)");
    if(answer == "y" || answer == "Y") return true;
    ui::info("cancelled, nothing was catalogued");
    return false;
}

// runStart: the start menu, shown when fpmatch runs with no arguments. Lists
// everything in data/ so a print can be dragged in there and picked by number.
// Returns the text for the final screen, or "" to quit without one.
std::string runStart() {
    while(true){
        std::vector<DataEntry> entries = scanData();
        int images = 0, folders = 0;
        std::vector<std::string> names;
        for(const DataEntry& e : entries){
            std::string name = std::filesystem::path(e.path).filename().string();
            if(e.folder){
                folders++;
                name += "/ (" + std::to_string(e.prints) + ")";
            }
            else images++;
            names.push_back(name);
        }

        ui::header("EVIDENCE LOCKER");
        ui::info("data/   " + std::to_string(images) + " prints   " + std::to_string(folders) + " folders");
        if(entries.empty()){
            ui::warn("data/ is empty. drag prints into it, then pick [r]");
        }
        ui::numberedList(names);
        std::cout << "\n";
        if(!entries.empty()){
            std::cout << "  " << ui::paint(ui::CYAN, "[#]") << " Analyze a print, or catalogue a whole folder (fast mode)\n";
        }
        if(images > 0){
            std::cout << "  " << ui::paint(ui::CYAN, "[a]") << " Catalogue every print in data/ (fast mode)\n";
        }
        std::cout << "  " << ui::paint(ui::CYAN, "[r]") << " Rescan data/\n";
        std::cout << "  " << ui::paint(ui::CYAN, "[q]") << " Quit\n";

        //keep asking until the answer makes sense
        while(true){
            std::cout << " " << ui::paint(ui::CYAN, ">") << " " << std::flush;
            std::string answer;
            if(!std::getline(std::cin, answer)) return ""; //input ran out
            if(answer == "q") return "";
            if(answer == "r") break; //back to the top, which scans again
            if(answer == "a" && images > 0){
                if(confirmBatch("data/", images)) return runBatch("data");
                break;
            }

            //a number picks an entry
            bool digits = !answer.empty();
            for(char c : answer){
                if(!std::isdigit(static_cast<unsigned char>(c))) digits = false;
            }
            if(digits && answer.size() < 6){
                int pick = std::stoi(answer);
                if(pick >= 1 && pick <= (int)entries.size()){
                    const DataEntry& e = entries[pick - 1];
                    if(!e.folder) return runSingle(e.path);
                    if(confirmBatch(e.path + "/", e.prints)) return runBatch(e.path);
                    break;
                }
            }
            if(entries.empty()){
                std::cout << " " << ui::paint(ui::YELLOW, "[!] pick r or q") << "\n";
            }
            else{
                std::cout << " " << ui::paint(ui::YELLOW, "[!] pick a number from 1 to " + std::to_string(entries.size()) +
                                              ", or a, r or q") << "\n";
            }
        }
    }
}

// runEval: the FVC test on an FVC "B" set, where files are named
// <finger>_<impression>, like 101_1.tif.
//   genuine pairs:  every impression of a finger against every other impression
//                   of the same finger (each pair once)
//   impostor pairs: the first impression of every finger against the first
//                   impression of every other finger (each pair once)
// Writes every score to out/eval/scores.csv and prints FMR, FNMR and EER.
std::string runEval(const std::string& folder) {
    ui::minStageMs = 0; //hundreds of steps, no time for pauses

    //group the files by finger, using the part of the name before the _
    std::vector<std::string> images = listImages(folder);
    std::map<std::string, std::vector<std::string>> fingers; // finger -> its impressions, in order
    for(const std::string& path : images){
        std::string stem = std::filesystem::path(path).stem().string();
        size_t underscore = stem.find('_');
        if(underscore == std::string::npos) continue; //not an FVC name, skip it
        fingers[stem.substr(0, underscore)].push_back(path);
    }
    if(fingers.size() < 2){
        throw std::runtime_error("runEval: need at least 2 fingers named like 101_1.tif in " + folder);
    }

    //analyze every print once, its debug images go in out/eval/<name>/
    std::map<std::string, Template> templates; // path -> template
    {
        ui::Stage s("Analyzing");
        int done = 0;
        for(const std::string& path : images){
            {
                ui::Mute quiet;
                std::string name = std::filesystem::path(path).stem().string();
                templates[path] = analyzePrint(path, "out/eval/" + name + "/");
            }
            done++;
            s.progress(done, images.size());
        }
        s.done(std::to_string(images.size()) + " prints  " + std::to_string(fingers.size()) + " fingers");
    }

    //build the list of pairs to compare
    struct Pair {
        std::string a;
        std::string b;
        bool genuine;
    };
    std::vector<Pair> pairs;
    for(const auto& [finger, prints] : fingers){
        for(size_t i = 0; i < prints.size(); i++){
            for(size_t j = i + 1; j < prints.size(); j++){
                pairs.push_back({prints[i], prints[j], true}); //same finger
            }
        }
    }
    for(auto f1 = fingers.begin(); f1 != fingers.end(); f1++){
        for(auto f2 = std::next(f1); f2 != fingers.end(); f2++){
            pairs.push_back({f1->second[0], f2->second[0], false}); //first of one finger vs first of another
        }
    }

    //score every pair and write them all to scores.csv
    std::vector<double> genuine;
    std::vector<double> impostor;
    std::filesystem::create_directories("out/eval");
    std::ofstream csv("out/eval/scores.csv");
    csv << "a,b,genuine,score\n";
    {
        ui::Stage s("Matching");
        for(size_t i = 0; i < pairs.size(); i++){
            const Pair& p = pairs[i];
            double score = matchTemplates(templates[p.a], templates[p.b]).score;
            if(p.genuine){
                genuine.push_back(score);
            }
            else{
                impostor.push_back(score);
            }
            csv << std::filesystem::path(p.a).stem().string() << "," << std::filesystem::path(p.b).stem().string()
                << "," << (p.genuine ? "yes" : "no") << "," << score << "\n";
            s.progress(i + 1, pairs.size());
        }
        s.done(std::to_string(genuine.size()) + " genuine  " + std::to_string(impostor.size()) + " impostor");
    }

    //FMR(t) = share of impostor pairs scoring at or above t (wrongly accepted)
    //FNMR(t) = share of genuine pairs scoring below t (wrongly rejected)
    auto fmr = [&](double t) {
        int count = 0;
        for(double v : impostor) if(v >= t) count++;
        return (double)count / impostor.size();
    };
    auto fnmr = [&](double t) {
        int count = 0;
        for(double v : genuine) if(v < t) count++;
        return (double)count / genuine.size();
    };

    //EER: raising t lowers FMR and raises FNMR. try every score that showed up
    //as the threshold and keep the one where the two rates are closest. the EER
    //is the average of the two rates there
    double eer = 1;
    double eerThreshold = 0;
    double bestGap = 2;
    std::vector<double> all = genuine;
    all.insert(all.end(), impostor.begin(), impostor.end());
    all.push_back(1.0001); //a threshold above every score, so "accept nothing" is tried too
    for(double t : all){
        double gap = std::fabs(fmr(t) - fnmr(t));
        if(gap < bestGap){
            bestGap = gap;
            eer = (fmr(t) + fnmr(t)) / 2;
            eerThreshold = t;
        }
    }

    //averages, to see how far apart the two piles are
    double gMean = 0, iMean = 0;
    for(double v : genuine) gMean += v;
    for(double v : impostor) iMean += v;
    gMean /= genuine.size();
    iMean /= impostor.size();

    //the report. a box on screen, plain lines when piped to a file
    std::string mode = "enhancement on";
    if (!useEnhance) mode = "enhancement off (--no-enhance)";
    const std::string report[7] = {
        "set        " + folder + "   " + mode,
        "genuine    " + std::to_string(genuine.size()) + " pairs   mean score " + ui::fixed(gMean, 3),
        "impostor   " + std::to_string(impostor.size()) + " pairs   mean score " + ui::fixed(iMean, 3),
        "",
        "set threshold " + ui::fixed(MATCH_THRESHOLD, 4) + "   FMR " + ui::fixed(100 * fmr(MATCH_THRESHOLD), 1) +
            "%   FNMR " + ui::fixed(100 * fnmr(MATCH_THRESHOLD), 1) + "%",
        "EER point     " + ui::fixed(eerThreshold, 4) + "   FMR " + ui::fixed(100 * fmr(eerThreshold), 1) +
            "%   FNMR " + ui::fixed(100 * fnmr(eerThreshold), 1) + "%",
        ui::paint(ui::BOLD, "EER " + ui::fixed(100 * eer, 2) + "%") + "   (lower is better)"
    };
    if (ui::enabled) {
        ui::scan("Tallying");
        ui::box("EVALUATION REPORT", report, 7);
    }
    else {
        for (const std::string& r : report) {
            if (!r.empty()) ui::line(r);
        }
    }
    return "Scores written to out/eval/scores.csv";
}

// printUsage: the list of commands.
void printUsage(const char* program) {
    std::cerr << "usage:\n"
              << "  " << program << "                    start menu: pick a print or folder from data/\n"
              << "  " << program << " <image>            analyze one print, then show the menu\n"
              << "  " << program << " import <folder>    catalogue every image in a folder (fast mode)\n"
              << "  " << program << " eval <folder>      run the FVC test on an FVC B set\n"
              << "add --no-enhance to any of them to skip the orientation, frequency and Gabor stages\n";
}

int main(int argc, char** argv) {
    //pull out the --no-enhance flag wherever it is, keep everything else in order
    std::vector<std::string> args;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--no-enhance") useEnhance = false;
        else args.push_back(a);
    }
    std::string command;
    if (!args.empty()) command = args[0];
    if (command == "help" || command == "--help" || command == "-h") {
        printUsage(argv[0]);
        return 0;
    }

    //import and eval need a folder after them
    if ((command == "import" || command == "eval") && args.size() < 2) {
        printUsage(argv[0]);
        return 1;
    }

    ui::banner();
    try {
        if (command.empty()) {
            std::string result = runStart();
            if (result.empty()) ui::info("case closed");
            else ui::finalScreen(true, result);
        }
        else if (command == "import") {
            ui::finalScreen(true, runBatch(args[1]));
        }
        else if (command == "eval") {
            ui::finalScreen(true, runEval(args[1]));
        }
        else {
            ui::finalScreen(true, runSingle(command));   //no command word, so it's an image
        }
    } catch (const std::exception& e) {
        ui::fast = false; //a batch that blew up still gets a proper ending
        ui::finalScreen(false, e.what());
        return 1;
    }

    return 0;
}
