#pragma once
// fingerprint.hpp: the structs every part of fpmatch shares, plus the
// declarations of the functions in each .cpp file.

//nessecary libs for ts
#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// bump this whenever the pipeline changes, so old templates in db/ can be
// found and re-analyzed later
constexpr const char* PIPELINE_VERSION = "1";

constexpr double PI = 3.14159265358979323846;

//struct that holds one singular image of a print in grayscale
struct GrayImage{
    int width = 0;
    int height = 0;
    std::vector<float> pixels;

    //indexing helpers
    float& at(int x, int y) {
    return pixels[y * width + x];
    }
    float at(int x, int y) const {
        return pixels[y * width + x];
    }
};

struct NormStats{
    //to make my UI cooler
    double mean;
    double stdDev;
};

// BinaryImage: same idea as GrayImage, but every pixel is 1 (ridge) or 0 (valley).
struct BinaryImage {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> pixels;

    //indexing helpers
    uint8_t& at(int x, int y) {
    return pixels[y * width + x];
    }
    uint8_t at(int x, int y) const {
        return pixels[y * width + x];
    }
};

// ---- image_processing.cpp ---------------------------------------------------
GrayImage loadGray(const std::string& path);
void saveGray(const GrayImage& img, const std::string& path);
NormStats normalize(GrayImage& img);
float localMean(const GrayImage& img, int cx, int cy, int window);
BinaryImage binarize(const GrayImage& img, int window);
void saveBinary(const BinaryImage& img, const std::string& path);
std::array<uint8_t, 8> getNeighbors(const BinaryImage& img, int x, int y);
int countNeighbors(const std::array<uint8_t, 8>& p);
int countTransitions(const std::array<uint8_t, 8>& p);
int thin(BinaryImage& img);
long long floodFill(BinaryImage& img, int startX, int startY, uint8_t from, uint8_t to);
BinaryImage segmentMask(const GrayImage& img, int block, float threshold);
void applyMask(BinaryImage& img, const BinaryImage& mask);

// ---- minutiae.cpp -------------------------------------------------------------

// the type numbers match the crossing number that finds them
enum MinutiaType { ENDING = 1, BIFURCATION = 3 };

// Minutia: one ridge ending or bifurcation.
// Angle convention: radians from 0 to 2π, like on a math graph. 0 points right,
// π/2 points UP on screen and angles grow counterclockwise. Image y points down,
// so every atan2 in this project flips dy: atan2(-(y2 - y1), x2 - x1).
// An ending points out of the ridge tip. A bifurcation points away from the
// single ridge (the trunk) toward the gap between the two branches.
struct Minutia {
    int x = 0;
    int y = 0;
    double angle = 0;
    MinutiaType type = ENDING;
};

// why traceRidge stopped walking
enum TraceStop { STOP_LENGTH, STOP_ENDING, STOP_JUNCTION, STOP_BORDER };

// RidgeTrace: the pixels visited while walking along a ridge, and why it stopped.
struct RidgeTrace {
    std::vector<std::pair<int, int>> path;
    TraceStop stop = STOP_LENGTH;
};

double wrapAngle(double a);
double angleDiff(double a, double b);
double angleBetween(int x1, int y1, int x2, int y2);
double distance(const Minutia& a, const Minutia& b);
RidgeTrace traceRidge(const BinaryImage& skel, std::vector<std::pair<int, int>> visited,
                      int x, int y, int maxSteps);
std::vector<Minutia> findMinutiae(const BinaryImage& skel);
void saveMinutiae(const BinaryImage& skel, const std::vector<Minutia>& minutiae, const std::string& path);

// FilterReport: how many minutiae each filter check removed.
struct FilterReport {
    int border = 0;
    int spurs = 0;
    int islands = 0;
    int broken = 0;
    int close = 0;
};

std::vector<Minutia> filterMinutiae(const std::vector<Minutia>& minutiae, const BinaryImage& skel,
                                    const BinaryImage& mask, FilterReport& report);

// Template: everything we keep about one print, the minutiae plus the size of
// the image they came from. This is what gets compared, not the picture.
struct Template {
    int width = 0;
    int height = 0;
    std::vector<Minutia> minutiae;
};

void saveTemplate(const Template& t, const std::string& path);
Template loadTemplate(const std::string& path);

// ---- matching.cpp -------------------------------------------------------------

// Alignment: how to move print A on top of print B. A is turned by `rotation`
// (radians, counterclockwise on screen, same convention as Minutia::angle)
// around its own image center (cx, cy), then shifted by (dx, dy) pixels.
struct Alignment {
    double rotation = 0;
    double dx = 0;
    double dy = 0;
    double cx = 0;
    double cy = 0;
    int votes = 0;   // how many minutia pairs agreed on this alignment
};

// MatchResult: the outcome of comparing two templates.
struct MatchResult {
    double score = 0;                          // 0 = nothing in common, 1 = identical
    int matched = 0;                           // how many minutiae paired up
    Alignment alignment;                       // the alignment that was used
    std::vector<std::pair<int, int>> pairs;    // (index in A, index in B) for each pair
};

extern const double MATCH_THRESHOLD;
Minutia transformMinutia(const Minutia& m, const Alignment& al);
Alignment alignTemplates(const Template& a, const Template& b);
MatchResult matchTemplates(const Template& a, const Template& b);
void saveAligned(const Template& a, const Template& b, const MatchResult& result, const std::string& path);
