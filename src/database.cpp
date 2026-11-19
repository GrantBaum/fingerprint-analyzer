//nessecary libs for ts
#include <algorithm>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include "fingerprint.hpp"
#include "ui.hpp"

namespace fs = std::filesystem; //shorter name, it gets used a lot here

// where everything lives
const std::string DB_DIR = "db/";
const std::string INDEX_PATH = "db/index.csv";
const std::string INDEX_HEADER = "id,name,finger,raw,skeleton,template,added,version";

// splitCsv: splits one line of index.csv at the commas.
// Names can't contain commas (catalogue refuses them), so no quoting is needed.
std::vector<std::string> splitCsv(const std::string& line) {
    std::vector<std::string> cells;
    std::string cell;
    std::stringstream in(line);
    while(std::getline(in, cell, ',')) cells.push_back(cell);
    if(!line.empty() && line.back() == ',') cells.push_back(""); //getline drops an empty last cell
    return cells;
}

// readIndex: reads every row of db/index.csv. A missing file just means an empty database.
std::vector<DbEntry> readIndex() {
    std::vector<DbEntry> entries;
    std::ifstream in(INDEX_PATH);
    if(!in) return entries; //no index yet

    std::string line;
    std::getline(in, line); //skip the header
    while(std::getline(in, line)){
        if(!line.empty() && line.back() == '\r') line.pop_back(); //in case it was edited on Windows
        if(line.empty()) continue;

        std::vector<std::string> c = splitCsv(line);
        if(c.size() != 8){
            ui::warn("index.csv: skipping a broken row: " + line);
            continue;
        }
        DbEntry e;
        e.id = c[0];
        e.name = c[1];
        e.finger = c[2];
        e.raw = c[3];
        e.skeleton = c[4];
        e.tmpl = c[5];
        e.added = c[6];
        e.version = c[7];
        entries.push_back(e);
    }
    return entries;
}

// nextId: one more than the biggest ID in the index, as 4 digits (0001, 0002 ...).
std::string nextId(const std::vector<DbEntry>& entries) {
    int biggest = 0;
    for(const DbEntry& e : entries){
        try {
            biggest = std::max(biggest, std::stoi(e.id));
        } catch (const std::exception&) {
            //not a number, ignore it
        }
    }
    std::ostringstream id;
    id << std::setw(4) << std::setfill('0') << biggest + 1; //pads 7 to 0007
    return id.str();
}

// nowStamp: the current local time as YYYY-MM-DD HH:MM.
std::string nowStamp() {
    std::time_t now = std::time(nullptr);
    std::ostringstream out;
    out << std::put_time(std::localtime(&now), "%Y-%m-%d %H:%M");
    return out.str();
}

// catalogue: saves a print from out/<name>/ into the database and returns its new ID.
// The order matters. Files get copied first, the index row is written second
// to last and out/ is deleted last. If anything fails part way, out/ is still
// there and the print isn't in the index, so the user can just try again.
// (Any files that did get copied will be overwritten by the retry, because the
// retry gets the same ID.)
std::string catalogue(const std::string& rawPath, const std::string& outDir,
                      const std::string& name, const std::string& finger, const std::string& version) {

    //commas or new lines would break the csv
    if(name.empty()){
        throw std::runtime_error("catalogue: name can't be empty");
    }
    if(name.find_first_of(",\n\r") != std::string::npos || finger.find_first_of(",\n\r") != std::string::npos){
        throw std::runtime_error("catalogue: name and finger can't contain commas");
    }

    //make sure everything we need exists before touching the database
    std::string skelPath = outDir + "07_skeleton.png";
    std::string tmplPath = outDir + "template.tmpl";
    if(!fs::exists(rawPath)) throw std::runtime_error("catalogue: original image is gone: " + rawPath);
    if(!fs::exists(skelPath)) throw std::runtime_error("catalogue: no skeleton in " + outDir);
    if(!fs::exists(tmplPath)) throw std::runtime_error("catalogue: no template in " + outDir);

    //create the folders, and the index with its header if it's new (or empty)
    fs::create_directories(DB_DIR + "raw");
    fs::create_directories(DB_DIR + "skel");
    fs::create_directories(DB_DIR + "tmpl");
    if(!fs::exists(INDEX_PATH) || fs::file_size(INDEX_PATH) == 0){
        std::ofstream make(INDEX_PATH);
        make << INDEX_HEADER << "\n";
        if(!make) throw std::runtime_error("catalogue: could not create " + INDEX_PATH);
    }

    std::string id = nextId(readIndex());

    //where each file goes. the raw keeps its original extension (.tif, .png ...)
    DbEntry e;
    e.id = id;
    e.name = name;
    e.finger = finger;
    e.raw = DB_DIR + "raw/" + id + fs::path(rawPath).extension().string();
    e.skeleton = DB_DIR + "skel/" + id + ".png";
    e.tmpl = DB_DIR + "tmpl/" + id + ".tmpl";
    e.added = nowStamp();
    e.version = version;

    //step 1: copy the files in. the ORIGINAL file, not 00_raw.png, so nothing
    //about the evidence is changed
    fs::copy_file(rawPath, e.raw, fs::copy_options::overwrite_existing);
    fs::copy_file(skelPath, e.skeleton, fs::copy_options::overwrite_existing);
    fs::copy_file(tmplPath, e.tmpl, fs::copy_options::overwrite_existing);

    //step 2: the index row, now that every file it points to is in place
    std::ofstream index(INDEX_PATH, std::ios::app); //app = add to the end
    index << e.id << "," << e.name << "," << e.finger << "," << e.raw << "," << e.skeleton << ","
          << e.tmpl << "," << e.added << "," << e.version << "\n";
    index.close();
    if(!index) throw std::runtime_error("catalogue: could not write to " + INDEX_PATH);

    //step 3: the print is safe in db/, so clear out/. together with the copy
    //this works like a move
    fs::remove_all(outDir);
    return id;
}

// searchDatabase: scores the probe against every print in the database and
// returns the best topN, highest score first.
std::vector<SearchHit> searchDatabase(const Template& probe, int topN) {
    std::vector<DbEntry> entries = readIndex();
    std::vector<SearchHit> hits;

    for(size_t i = 0; i < entries.size(); i++){
        try {
            Template t = loadTemplate(entries[i].tmpl);
            SearchHit h;
            h.entry = entries[i];
            h.result = matchTemplates(probe, t);
            hits.push_back(h);
        } catch (const std::exception& e) {
            ui::warn(std::string("skipping ") + entries[i].id + ": " + e.what()); //one bad file shouldn't stop the search
        }
        ui::progress(i + 1, entries.size());
    }

    //best first. stable_sort keeps ties in ID order
    std::stable_sort(hits.begin(), hits.end(), [](const SearchHit& a, const SearchHit& b) {
        return a.result.score > b.result.score;
    });
    if((int)hits.size() > topN) hits.resize(topN);
    return hits;
}

// discard: deletes a print's waiting results in out/.
void discard(const std::string& outDir) {
    fs::remove_all(outDir);
}
