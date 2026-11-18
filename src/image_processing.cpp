//nessecary libs for ts
#include <cstdint>
#include <iostream>
#include <vector>
#include <cmath>
#include <string>
#include <stdexcept>
#include <array>
#include <utility>
#include <algorithm>
#include <filesystem>
#include <opencv2/imgcodecs.hpp>
#include "fingerprint.hpp"
#include "ui.hpp"

// Load a file as grayscale into a GrayImage. Report a failed load, set width
// and height, size pixels, copy every pixel in.
GrayImage loadGray(const std::string& path) {
    if(!cv::haveImageReader(path)){
        throw std::runtime_error("loadGray: image file could not be read!");
        //stop
    }
    
    //read image and store in a mat var
    cv::Mat mat = cv::imread(path, cv::IMREAD_GRAYSCALE);
    if(mat.empty()){
        throw std::runtime_error("loadGray: could not load image!");
        //stop
    }
    
    //creating the image object
    GrayImage img;
    img.width = mat.cols;
    img.height = mat.rows;
    img.pixels.resize(img.width * img.height);

    //loops for copying mat array into img object
    for (int Y = 0; Y < img.height; Y++){
        for(int X = 0; X < img.width; X++){
            img.at(X, Y) = mat.at<uchar>(Y, X); //sets one pixel
        }
    }
    
    return img; //returning finished image

}
// saveGray: writes a GrayImage to a PNG so you can look at it.
void saveGray(const GrayImage& img, const std::string& path) {

    //auto iterator for the min and max element in the pixels
    auto [lo, hi] = std::minmax_element(img.pixels.begin(), img.pixels.end());
    float minVal = *lo;
    float maxVal = *hi;

    //making out object
    cv::Mat out(img.height, img.width, CV_8U);

    //error checking
    if(minVal == maxVal){
        std::cerr << "saveGray: every pixel is the same value, saving a flat image" << std::endl;
        out.setTo(0);          //flat black image, no divide by zero
        cv::imwrite(path, out); //save image
        return;
    }
    //stretching and saving loops
    for (int Y = 0; Y < img.height; Y++){
        for(int X = 0; X < img.width; X++){
            out.at<uchar>(Y, X) = (img.at(X, Y) - minVal) / (maxVal - minVal) * 255;
        }
    }

    cv::imwrite(path, out); //save image
}
// normalize: shifts and scales pixels to mean 0, std dev 1. Changes img in place.
NormStats normalize(GrayImage& img) {
    double sum = 0;
    double mean;
    double variance = 0;
    double stdDeviation;

    //first find the mean
    for (int Y = 0; Y < img.height; Y++){
        for(int X = 0; X < img.width; X++){
            sum += img.at(X, Y);
        }
    }
    mean = sum / img.pixels.size();

    //then find the variance
    for (int Y = 0; Y < img.height; Y++){
        for(int X = 0; X < img.width; X++){
            variance += (img.at(X, Y) - mean) * (img.at(X, Y) - mean); //there is no better way to square ts
        }
    }
    variance = variance / img.pixels.size();

    //and std dev is easy now
    stdDeviation = std::sqrt(variance);
    if(stdDeviation == 0){
        throw std::runtime_error("normalize: std dev is 0, image has only one value");
        //stop
    }

    //now replace every pixel with (pixel - mean) / stddev
    for(int Y = 0; Y < img.height; Y++){
        for(int X = 0; X < img.width; X++){
            img.at(X, Y) = (img.at(X, Y) - mean) / stdDeviation;
        }
    }

    //saving and returning the stats
    NormStats stats;
    stats.mean = mean;
    stats.stdDev = stdDeviation;
    return stats;

}

// localMean: average of the window x window square centered on (cx, cy).
float localMean(const GrayImage& img, int cx, int cy, int window) {
    int half = window / 2;   // window 15 -> 7 pixels on each side of center
    double sum = 0.0;        // double so the additions don't lose precision
    int count = 0;           // how many pixels were really inside the image

    for (int dy = -half; dy <= half; dy++) {
        int ny = cy + dy;                              //row we're looking at
        if (ny < 0 || ny >= img.height) continue;      //row is off the image

        for (int dx = -half; dx <= half; dx++) {
            int nx = cx + dx;                          //column we're looking at
            if (nx < 0 || nx >= img.width) continue;   //column is off the image

            sum += img.at(nx, ny);
            count++;
        }
    }

    //count can't be 0, the center pixel is always inside the image
    return static_cast<float>(sum / count);
}
// binarize: decides for every pixel if it's ridge (1) or valley (0).
BinaryImage binarize(const GrayImage& img, int window) {

    //creating binary image object
    BinaryImage image;
    image.height = img.height;
    image.width = img.width;
    image.pixels.resize(image.width * image.height);

    for(int Y = 0; Y < img.height; Y++){
        for(int X = 0; X < img.width; X++){
            //check and binarize
            float avg = localMean(img, X, Y, window);
            if(img.at(X, Y) < avg){
                image.at(X, Y) = 1; //because it is darker than surroundings
            }
            else{
                image.at(X, Y) = 0; //because it is lighter than surroundings
            }
        }
    }

    return image;
}
// saveBinary: writes a BinaryImage to a PNG, ridges black, valleys white.
void saveBinary(const BinaryImage& img, const std::string& path) {

    cv::Mat out(img.height, img.width, CV_8U); //creating the mat object
    
    //iteration loops for creating the png
    for(int Y = 0; Y < img.height; Y++){
        for(int X = 0; X < img.width; X++){
            if(img.at(X, Y) == 1){
                out.at<uchar>(Y, X) = 0; //ridge
            }
            else out.at<uchar>(Y, X) = 255; //valley
        }
    }

    cv::imwrite(path, out); //save image
}
// Reads the 8 neighbors of (x, y) in the order P2, P3, P4, P5, P6, P7, P8, P9.
// So p[0] is P2, p[2] is P4, p[4] is P6 and p[6] is P8.
// (x, y) must not be on the image border, or this reads outside the image.
std::array<uint8_t, 8> getNeighbors(const BinaryImage& img, int x, int y) {
    return {
        img.at(x,     y - 1),   // P2 up
        img.at(x + 1, y - 1),   // P3 up-right
        img.at(x + 1, y    ),   // P4 right
        img.at(x + 1, y + 1),   // P5 down-right
        img.at(x,     y + 1),   // P6 down
        img.at(x - 1, y + 1),   // P7 down-left
        img.at(x - 1, y    ),   // P8 left
        img.at(x - 1, y - 1)    // P9 up-left
    };
}

// B(P1): how many of the 8 neighbors are ridge (0 to 8).
int countNeighbors(const std::array<uint8_t, 8>& p) {
    int count = 0;
    for (uint8_t v : p) count += v;
    return count;
}

// A(P1): walk P2 -> P3 -> ... -> P9 -> back to P2 and count the 0 -> 1 steps.
// That's how many separate groups of ridge pixels touch P1.
int countTransitions(const std::array<uint8_t, 8>& p) {
    int transitions = 0;
    for (int i = 0; i < 8; i++) {
        int next = (i + 1) % 8;   // % 8 makes P9 wrap around to P2
        if (p[i] == 0 && p[next] == 1) transitions++;
    }
    return transitions;
}

// Thins every ridge to 1 pixel wide, in place. Returns how many passes it
// took (the last pass is the one that finds nothing left to remove).
int thin(BinaryImage& img) {
    int passes = 0;
    bool changed;
    std::vector<std::pair<int, int>> marks;   // pixels to delete this sub-step

    do {
        changed = false;
        passes++;
        long long removedThisPass = 0;

        for (int step = 0; step < 2; step++) {
            marks.clear();

            // Skip the outer border: those pixels don't have 8 neighbors.
            for (int y = 1; y < img.height - 1; y++) {
                for (int x = 1; x < img.width - 1; x++) {
                    if (img.at(x, y) == 0) continue;               // only ridge pixels

                    std::array<uint8_t, 8> p = getNeighbors(img, x, y);

                    int b = countNeighbors(p);
                    if (b < 2 || b > 6) continue;                  // tip or buried pixel: keep
                    if (countTransitions(p) != 1) continue;        // bridge pixel: keep

                    int p2 = p[0], p4 = p[2], p6 = p[4], p8 = p[6];
                    if (step == 0) {
                        // Sub-step 1: bottom or right edge, or top-left corner
                        if (p2 * p4 * p6 != 0) continue;
                        if (p4 * p6 * p8 != 0) continue;
                    } else {
                        // Sub-step 2: top or left edge, or bottom-right corner
                        if (p2 * p4 * p8 != 0) continue;
                        if (p2 * p6 * p8 != 0) continue;
                    }

                    marks.push_back({x, y});   // mark now, delete after the scan
                }
            }

            // Delete everything marked in this sub-step at once
            for (const auto& [mx, my] : marks) img.at(mx, my) = 0;

            removedThisPass += static_cast<long long>(marks.size());
            if (!marks.empty()) changed = true;
        }

        ui::status("pass " + std::to_string(passes) + "   removed " + ui::num(removedThisPass) + " px");
    } while (changed);

    return passes;
}
