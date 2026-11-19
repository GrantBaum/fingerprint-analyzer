//nessecary libs for ts
#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <utility>
#include <vector>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include "fingerprint.hpp"
#include "ui.hpp"

// ---- tuning numbers -----------------------------------------------------------
// Values marked Hong come from Hong, Wan and Jain (1998), tested at 500 dpi.

// orientation: the gradients for one block are summed over a window this many
// times wider than the block, so each angle is based on more ridges
const int ORIENT_WINDOW_SCALE = 2;

// orientation: how many times to smooth the field with a 3 x 3 block average
const int ORIENT_SMOOTH_PASSES = 2;

// frequency: the oriented window used for the x-signature (Hong)
const int FREQ_WINDOW_ACROSS = 32;   // pixels across the ridges
const int FREQ_WINDOW_ALONG = 16;    // pixels along the ridges

// frequency: real ridges are between 3 and 25 pixels apart (Hong)
const float MIN_PERIOD = 3;
const float MAX_PERIOD = 25;

// gabor: how soft the blob around the wave is, in pixels (Hong uses 4 for both)
const double GABOR_SIGMA_ACROSS = 4.0;
const double GABOR_SIGMA_ALONG = 4.0;

// gabor: how many angles the filter bank has, spread over 180 degrees
const int GABOR_ANGLES = 16;

// ---------------------------------------------------------------------------------

// sobel: the brightness gradient at (x, y) using the 3 x 3 Sobel weights.
// gx is how fast it gets brighter going right, gy going DOWN (image y).
//      -1 0 1           -1 -2 -1
//  gx: -2 0 2       gy:  0  0  0
//      -1 0 1            1  2  1
void sobel(const GrayImage& img, int x, int y, float& gx, float& gy) {
    //edge pixels copy their nearest neighbor inside the image
    auto px = [&](int X, int Y) {
        X = std::clamp(X, 0, img.width - 1);
        Y = std::clamp(Y, 0, img.height - 1);
        return img.at(X, Y);
    };
    gx = (px(x + 1, y - 1) + 2 * px(x + 1, y) + px(x + 1, y + 1))
       - (px(x - 1, y - 1) + 2 * px(x - 1, y) + px(x - 1, y + 1));
    gy = (px(x - 1, y + 1) + 2 * px(x, y + 1) + px(x + 1, y + 1))
       - (px(x - 1, y - 1) + 2 * px(x, y - 1) + px(x + 1, y - 1));
}

// orientationField: the direction the ridges run in each block, 0 to π, using
// the same convention as minutiae (0 = right, π/2 = up on screen).
//
// Brightness changes fastest ACROSS a ridge, so the gradient points across the
// ridges and the ridge runs at a right angle to it. The catch: a gradient on one
// side of a ridge points the opposite way from the other side (dark to light vs
// light to dark), so plain averaging would cancel them out. The fix is to
// double every angle first. 0 and 180 degrees both become 0 and 360, which is
// the same direction, so they add up instead of cancelling. Halve at the end.
BlockField orientationField(const GrayImage& img, int block) {

    //every pixel's gradient, worked out once
    std::vector<float> gxs(img.width * img.height);
    std::vector<float> gys(img.width * img.height);
    for(int Y = 0; Y < img.height; Y++){
        for(int X = 0; X < img.width; X++){
            sobel(img, X, Y, gxs[Y * img.width + X], gys[Y * img.width + X]);
        }
    }

    //the doubled angle of each block, stored as a (cos, sin) pair so it can be smoothed
    int bw = (img.width + block - 1) / block;
    int bh = (img.height + block - 1) / block;
    std::vector<double> c2(bw * bh);
    std::vector<double> s2(bw * bh);
    int half = block * ORIENT_WINDOW_SCALE / 2; //window reaches this far from the block center

    for(int BY = 0; BY < bh; BY++){
        for(int BX = 0; BX < bw; BX++){
            int cx = BX * block + block / 2;
            int cy = BY * block + block / 2;

            //the doubled angle of a gradient (gx, gy) is (gx² - gy², 2 gx gy).
            //that's just the formulas for cos 2a and sin 2a, scaled by the
            //gradient's strength squared, so strong edges count for more
            double vx = 0;
            double vy = 0;
            for(int Y = std::max(0, cy - half); Y < std::min(img.height, cy + half); Y++){
                for(int X = std::max(0, cx - half); X < std::min(img.width, cx + half); X++){
                    float gx = gxs[Y * img.width + X];
                    float gy = -gys[Y * img.width + X]; //flip so up is positive, like everything else
                    vx += gx * gx - gy * gy;
                    vy += 2 * gx * gy;
                }
            }
            c2[BY * bw + BX] = vx;
            s2[BY * bw + BX] = vy;
        }
    }

    //smooth: replace each block by the sum of its 3 x 3 neighbors, a few times.
    //one bad block (a smudge, a scar) gets outvoted by the blocks around it
    for(int pass = 0; pass < ORIENT_SMOOTH_PASSES; pass++){
        std::vector<double> nc(bw * bh, 0);
        std::vector<double> ns(bw * bh, 0);
        for(int BY = 0; BY < bh; BY++){
            for(int BX = 0; BX < bw; BX++){
                for(int dy = -1; dy <= 1; dy++){
                    for(int dx = -1; dx <= 1; dx++){
                        int nx = BX + dx;
                        int ny = BY + dy;
                        if(nx < 0 || nx >= bw || ny < 0 || ny >= bh) continue; //off the grid
                        nc[BY * bw + BX] += c2[ny * bw + nx];
                        ns[BY * bw + BX] += s2[ny * bw + nx];
                    }
                }
            }
        }
        c2 = nc;
        s2 = ns;
    }

    //halve the doubled angle to get the gradient angle, then turn it 90 degrees
    //to get the ridge angle
    BlockField orient;
    orient.block = block;
    orient.width = bw;
    orient.height = bh;
    orient.values.resize(bw * bh);
    for(int i = 0; i < bw * bh; i++){
        double gradient = 0.5 * std::atan2(s2[i], c2[i]);
        double ridge = gradient + PI / 2;
        while(ridge < 0) ridge += PI;   //keep it in 0 to π, a ridge has no front or back
        while(ridge >= PI) ridge -= PI;
        orient.values[i] = (float)ridge;
    }
    return orient;
}

// grayToColor: stretches a GrayImage to 0 to 255 and makes it a color picture
// we can draw on (same stretch as saveGray).
cv::Mat grayToColor(const GrayImage& img) {
    auto [lo, hi] = std::minmax_element(img.pixels.begin(), img.pixels.end());
    float range = *hi - *lo;
    if(range == 0) range = 1; //flat image, avoid dividing by zero

    cv::Mat gray(img.height, img.width, CV_8U);
    for(int Y = 0; Y < img.height; Y++){
        for(int X = 0; X < img.width; X++){
            gray.at<uchar>(Y, X) = (uchar)((img.at(X, Y) - *lo) / range * 255);
        }
    }
    cv::Mat color;
    cv::cvtColor(gray, color, cv::COLOR_GRAY2BGR);
    return color;
}

// saveOrientation: draws one short red line per finger block, along the ridges.
void saveOrientation(const GrayImage& img, const BlockField& orient, const BinaryImage& mask, const std::string& path) {
    cv::Mat out = grayToColor(img);
    for(int BY = 0; BY < orient.height; BY++){
        for(int BX = 0; BX < orient.width; BX++){
            int cx = BX * orient.block + orient.block / 2;
            int cy = BY * orient.block + orient.block / 2;
            if(cx >= mask.width || cy >= mask.height || mask.at(cx, cy) == 0) continue; //background

            double a = orient.at(BX, BY);
            int dx = (int)std::lround(6 * std::cos(a));
            int dy = (int)std::lround(-6 * std::sin(a)); //minus because y points down
            cv::line(out, cv::Point(cx - dx, cy - dy), cv::Point(cx + dx, cy + dy), cv::Scalar(0, 0, 255), 2);
        }
    }
    cv::imwrite(path, out); //save image
}

// ridgePeriod: measures the ridge spacing in one block with an x-signature.
// Picture a 32 x 16 window centered on the block, turned so its long side runs
// ACROSS the ridges. Average each row of 16 pixels (along the ridge) into one
// number. Going across, those numbers dip at every ridge and rise at every
// valley, like a wave. The average gap between dips is the ridge period.
// Returns 0 if there's no clear wave.
float ridgePeriod(const GrayImage& img, int cx, int cy, double ridgeAngle) {
    //unit steps along the ridge and across it, in image coordinates (y down)
    double alongX = std::cos(ridgeAngle);
    double alongY = -std::sin(ridgeAngle);
    double acrossX = std::sin(ridgeAngle);
    double acrossY = std::cos(ridgeAngle);

    //build the x-signature
    std::vector<float> signature(FREQ_WINDOW_ACROSS);
    for(int k = 0; k < FREQ_WINDOW_ACROSS; k++){
        double sum = 0;
        int count = 0;
        for(int d = 0; d < FREQ_WINDOW_ALONG; d++){
            double u = k - FREQ_WINDOW_ACROSS / 2.0; //steps across
            double v = d - FREQ_WINDOW_ALONG / 2.0;  //steps along
            int X = (int)std::lround(cx + u * acrossX + v * alongX);
            int Y = (int)std::lround(cy + u * acrossY + v * alongY);
            if(X < 0 || X >= img.width || Y < 0 || Y >= img.height) continue; //off the image
            sum += img.at(X, Y);
            count++;
        }
        if(count == 0) return 0; //window hangs off the image
        signature[k] = sum / count;
    }

    //find the dips (ridges are dark, so they are the low points)
    std::vector<int> dips;
    for(int k = 1; k < FREQ_WINDOW_ACROSS - 1; k++){
        if(signature[k] < signature[k - 1] && signature[k] <= signature[k + 1]) dips.push_back(k);
    }
    if(dips.size() < 2) return 0; //need at least two dips to measure a gap

    //average gap = distance from the first dip to the last / number of gaps
    float period = (float)(dips.back() - dips.front()) / (dips.size() - 1);
    if(period < MIN_PERIOD || period > MAX_PERIOD) return 0; //not a real ridge pattern
    return period;
}

// frequencyField: ridge frequency (1 / period) for every block, 0 for background.
// Blocks where the wave was unclear borrow the average of their neighbors.
BlockField frequencyField(const GrayImage& img, const BlockField& orient, const BinaryImage& mask) {
    BlockField freq;
    freq.block = orient.block;
    freq.width = orient.width;
    freq.height = orient.height;
    freq.values.assign(freq.width * freq.height, 0);

    //which blocks are finger, judged at the block center
    std::vector<bool> finger(freq.width * freq.height, false);
    for(int BY = 0; BY < freq.height; BY++){
        for(int BX = 0; BX < freq.width; BX++){
            int cx = std::min(BX * freq.block + freq.block / 2, mask.width - 1);
            int cy = std::min(BY * freq.block + freq.block / 2, mask.height - 1);
            finger[BY * freq.width + BX] = mask.at(cx, cy) == 1;
        }
    }

    //measure every finger block
    int measured = 0;
    for(int BY = 0; BY < freq.height; BY++){
        for(int BX = 0; BX < freq.width; BX++){
            if(!finger[BY * freq.width + BX]) continue;
            int cx = BX * freq.block + freq.block / 2;
            int cy = BY * freq.block + freq.block / 2;
            float period = ridgePeriod(img, cx, cy, orient.at(BX, BY));
            if(period > 0){
                freq.at(BX, BY) = 1.0f / period;
                measured++;
            }
        }
    }
    if(measured == 0){
        throw std::runtime_error("frequencyField: no block had a clear ridge pattern");
    }

    //fill the gaps: an unknown finger block takes the average of its known
    //neighbors. repeat until nothing changes, so big gaps fill in from the edges
    bool changed = true;
    while(changed){
        changed = false;
        BlockField next = freq;
        for(int BY = 0; BY < freq.height; BY++){
            for(int BX = 0; BX < freq.width; BX++){
                if(!finger[BY * freq.width + BX] || freq.at(BX, BY) > 0) continue; //background or already known
                double sum = 0;
                int count = 0;
                for(int dy = -1; dy <= 1; dy++){
                    for(int dx = -1; dx <= 1; dx++){
                        int nx = BX + dx;
                        int ny = BY + dy;
                        if(nx < 0 || nx >= freq.width || ny < 0 || ny >= freq.height) continue;
                        if(freq.at(nx, ny) > 0){
                            sum += freq.at(nx, ny);
                            count++;
                        }
                    }
                }
                if(count > 0){
                    next.at(BX, BY) = sum / count;
                    changed = true;
                }
            }
        }
        freq = next;
    }

    //smooth once with a 3 x 3 average of known blocks, to calm down odd measurements
    BlockField smooth = freq;
    for(int BY = 0; BY < freq.height; BY++){
        for(int BX = 0; BX < freq.width; BX++){
            if(freq.at(BX, BY) == 0) continue;
            double sum = 0;
            int count = 0;
            for(int dy = -1; dy <= 1; dy++){
                for(int dx = -1; dx <= 1; dx++){
                    int nx = BX + dx;
                    int ny = BY + dy;
                    if(nx < 0 || nx >= freq.width || ny < 0 || ny >= freq.height) continue;
                    if(freq.at(nx, ny) > 0){
                        sum += freq.at(nx, ny);
                        count++;
                    }
                }
            }
            smooth.at(BX, BY) = sum / count;
        }
    }
    return smooth;
}

// saveFrequency: one gray square per block, brighter = ridges closer together.
// Background is black.
void saveFrequency(const BlockField& freq, const BinaryImage& mask, const std::string& path) {
    GrayImage out;
    out.width = mask.width;
    out.height = mask.height;
    out.pixels.resize(out.width * out.height);
    for(int Y = 0; Y < out.height; Y++){
        for(int X = 0; X < out.width; X++){
            out.at(X, Y) = freq.at(X / freq.block, Y / freq.block); //copies the block's value
        }
    }
    saveGray(out, path);
}

// makeGaborKernel: builds one Gabor filter: a wave running across the ridges at
// the given spacing, faded out by a soft blob (a Gaussian) so it only looks
// nearby. Held up against a patch of print, it gives a big answer when the
// patch has ridges at that angle and spacing, and a small one for noise.
std::vector<float> makeGaborKernel(double ridgeAngle, double period, int radius) {
    int size = 2 * radius + 1;
    std::vector<float> kernel(size * size);

    //unit steps across the ridge, in image coordinates (y down)
    double acrossX = std::sin(ridgeAngle);
    double acrossY = std::cos(ridgeAngle);
    double alongX = std::cos(ridgeAngle);
    double alongY = -std::sin(ridgeAngle);

    double sum = 0;
    for(int Y = -radius; Y <= radius; Y++){
        for(int X = -radius; X <= radius; X++){
            double u = X * acrossX + Y * acrossY;   //distance across the ridges
            double v = X * alongX + Y * alongY;     //distance along the ridges
            double blob = std::exp(-0.5 * (u * u / (GABOR_SIGMA_ACROSS * GABOR_SIGMA_ACROSS) +
                                           v * v / (GABOR_SIGMA_ALONG * GABOR_SIGMA_ALONG)));
            double wave = std::cos(2 * PI * u / period);
            kernel[(Y + radius) * size + (X + radius)] = (float)(blob * wave);
            sum += blob * wave;
        }
    }

    //take out the average so a flat patch gives 0. without this, plain bright
    //or dark areas would leak through as if they were ridges
    for(float& k : kernel) k -= (float)(sum / kernel.size());
    return kernel;
}

// gaborFilter: runs a Gabor filter over every finger pixel, tuned to that
// block's ridge angle and spacing. Ridges that follow the local flow get
// stronger, and pores, scratches and smudges fade. Building a kernel for every
// block is slow, so angles are rounded to one of GABOR_ANGLES steps and periods
// to whole pixels, and each kernel is built once and reused.
GrayImage gaborFilter(const GrayImage& img, const BlockField& orient, const BlockField& freq, const BinaryImage& mask) {
    int radius = (int)std::ceil(3 * std::max(GABOR_SIGMA_ACROSS, GABOR_SIGMA_ALONG)); //the blob is basically 0 past 3 sigma
    int size = 2 * radius + 1;
    std::map<std::pair<int, int>, std::vector<float>> bank; // (angle step, period) -> kernel

    GrayImage out;
    out.width = img.width;
    out.height = img.height;
    out.pixels.assign(img.width * img.height, 0); //background stays 0

    for(int Y = 0; Y < img.height; Y++){
        for(int X = 0; X < img.width; X++){
            if(mask.at(X, Y) == 0) continue; //only filter the finger

            int BX = X / orient.block;
            int BY = Y / orient.block;
            float f = freq.at(BX, BY);
            if(f <= 0) continue; //no spacing known here

            //round to the nearest kernel in the bank, build it the first time
            int step = (int)std::lround(orient.at(BX, BY) / PI * GABOR_ANGLES) % GABOR_ANGLES;
            int period = (int)std::lround(1.0f / f);
            std::pair<int, int> key = {step, period};
            if(bank.find(key) == bank.end()){
                bank[key] = makeGaborKernel(step * PI / GABOR_ANGLES, period, radius);
            }
            const std::vector<float>& kernel = bank[key];

            //slide the kernel over this pixel: multiply and add up
            double sum = 0;
            for(int ky = -radius; ky <= radius; ky++){
                int sy = std::clamp(Y + ky, 0, img.height - 1); //edge pixels repeat
                for(int kx = -radius; kx <= radius; kx++){
                    int sx = std::clamp(X + kx, 0, img.width - 1);
                    sum += kernel[(ky + radius) * size + (kx + radius)] * img.at(sx, sy);
                }
            }
            out.at(X, Y) = (float)sum;
        }
        ui::progress(Y + 1, img.height);
    }

    ui::status(std::to_string(bank.size()) + " kernels");
    return out;
}
