//nessecary libs for ts
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include "fingerprint.hpp"
#include "ui.hpp"

// ---- tuning numbers -----------------------------------------------------------
// all in pixels at 500 dpi, where one ridge plus one valley is about 9 px

// how far to walk along the ridge to measure a minutia's angle
const int ANGLE_TRACE_STEPS = 10;

// filter: a minutia must have finger (mask) all around it this far out
const int BORDER_MARGIN = 12;

// filter: a branch shorter than this that ends is a spur
const int SPUR_LENGTH = 12;

// filter: a whole ridge shorter than this (ending to ending) is an island
const int ISLAND_LENGTH = 15;

// filter: two endings closer than this that face each other are a broken ridge
const int BREAK_DISTANCE = 15;

// filter: how close (in degrees) "facing each other" has to be
const double BREAK_ANGLE_DEG = 45;

// filter: any two minutiae closer than this are both noise
const int MIN_DISTANCE = 6;

// x and y offsets of the 8 neighbors, in the same order as getNeighbors:
// P2 up, P3 up-right, P4 right, P5 down-right, P6 down, P7 down-left, P8 left, P9 up-left
const int NEIGHBOR_DX[8] = {0, 1, 1, 1, 0, -1, -1, -1};
const int NEIGHBOR_DY[8] = {-1, -1, 0, 1, 1, 1, 0, -1};

// wrapAngle: moves any angle into the range 0 to 2π.
double wrapAngle(double a) {
    while(a < 0) a += 2 * PI;
    while(a >= 2 * PI) a -= 2 * PI;
    return a + 0.0; //adding 0.0 turns -0 into plain 0
}

// angleBetween: the angle from point 1 to point 2, with y flipped so up is positive.
double angleBetween(int x1, int y1, int x2, int y2) {
    return wrapAngle(std::atan2(-(double)(y2 - y1), (double)(x2 - x1))); //minus because y points down
}

// wasVisited: true if (x, y) is already in the list.
bool wasVisited(const std::vector<std::pair<int, int>>& visited, int x, int y) {
    for(const auto& [vx, vy] : visited){
        if(vx == x && vy == y) return true;
    }
    return false;
}

// traceRidge: walks along the skeleton starting at (x, y), never going back to a
// pixel in `visited`. Stops after maxSteps, at a ridge end, at a junction or at
// the image border. The lists stay short (about 10 to 20 pixels), so a plain
// search through them is fast enough.
RidgeTrace traceRidge(const BinaryImage& skel, std::vector<std::pair<int, int>> visited,
                      int x, int y, int maxSteps) {
    RidgeTrace trace;
    trace.path.push_back({x, y});
    visited.push_back({x, y});

    for(int step = 0; step < maxSteps; step++){
        //border pixels don't have 8 neighbors, so we can't look around
        if(x <= 0 || y <= 0 || x >= skel.width - 1 || y >= skel.height - 1){
            trace.stop = STOP_BORDER;
            return trace;
        }

        //a junction has 3 or more separate ridge groups around it
        std::array<uint8_t, 8> p = getNeighbors(skel, x, y);
        if(countTransitions(p) >= 3){
            trace.stop = STOP_JUNCTION;
            return trace;
        }

        //collect the ridge neighbors we haven't been to yet
        std::vector<std::pair<int, int>> next;
        for(int i = 0; i < 8; i++){
            int nx = x + NEIGHBOR_DX[i];
            int ny = y + NEIGHBOR_DY[i];
            if(p[i] == 1 && !wasVisited(visited, nx, ny)) next.push_back({nx, ny});
        }
        if(next.empty()){
            trace.stop = STOP_ENDING; //nowhere left to go, so the ridge ends here
            return trace;
        }

        //on a staircase there can be 2 choices that touch each other. prefer the
        //straight one (up, down, left or right): the diagonal one is still next
        //to it, so the walk just picks it up on the following step
        std::pair<int, int> pick = next[0];
        for(const auto& [nx, ny] : next){
            if(nx == x || ny == y){
                pick = {nx, ny};
                break;
            }
        }
        visited.push_back(pick);

        x = pick.first;
        y = pick.second;
        trace.path.push_back({x, y});
    }

    trace.stop = STOP_LENGTH; //walked the full distance without a stop
    return trace;
}

// endingAngle: direction an ending points, from the ridge body out to the tip.
double endingAngle(const BinaryImage& skel, int x, int y) {
    std::array<uint8_t, 8> p = getNeighbors(skel, x, y);

    //an ending has exactly one group of neighbors, start walking into it
    for(int i = 0; i < 8; i++){
        if(p[i] == 1){
            RidgeTrace t = traceRidge(skel, {{x, y}}, x + NEIGHBOR_DX[i], y + NEIGHBOR_DY[i], ANGLE_TRACE_STEPS);
            auto [ex, ey] = t.path.back();      //how far along the ridge we got
            return angleBetween(ex, ey, x, y);  //from the ridge body back to the tip
        }
    }
    return 0; //can't happen for a real ending
}

// bifurcationAngle: walks all 3 branches. The two branches closest in angle are
// the fork, the third one is the trunk, and the angle points away from the trunk.
double bifurcationAngle(const BinaryImage& skel, int x, int y) {
    std::array<uint8_t, 8> p = getNeighbors(skel, x, y);

    //the first pixel of each group of ridge neighbors starts one branch.
    //a group starts wherever the ring goes from 0 to 1, same as countTransitions
    std::vector<std::pair<int, int>> starts;
    for(int i = 0; i < 8; i++){
        int next = (i + 1) % 8;
        if(p[i] == 0 && p[next] == 1){
            starts.push_back({x + NEIGHBOR_DX[next], y + NEIGHBOR_DY[next]});
        }
    }
    if(starts.size() != 3) return 0; //can't happen for a real bifurcation

    //walk each branch. the other branches and the center count as visited so
    //no walk can cut across into another branch
    double branch[3];
    for(int b = 0; b < 3; b++){
        std::vector<std::pair<int, int>> visited = {{x, y}};
        for(int o = 0; o < 3; o++){
            if(o != b) visited.push_back(starts[o]);
        }
        RidgeTrace t = traceRidge(skel, visited, starts[b].first, starts[b].second, ANGLE_TRACE_STEPS);
        auto [ex, ey] = t.path.back();
        branch[b] = angleBetween(x, y, ex, ey); //from the center out along the branch
    }

    //find the pair of branches with the smallest angle between them (the fork)
    int trunk = 0;
    double bestGap = 10; //bigger than any real gap
    for(int a = 0; a < 3; a++){
        for(int b = a + 1; b < 3; b++){
            double gap = std::fabs(branch[a] - branch[b]);
            if(gap > PI) gap = 2 * PI - gap; //go the short way around the circle
            if(gap < bestGap){
                bestGap = gap;
                trunk = 3 - a - b; //0 + 1 + 2 = 3, so this is the one left over
            }
        }
    }

    return wrapAngle(branch[trunk] + PI); //turn around: point away from the trunk
}

// findMinutiae: finds every ending and bifurcation on a 1 pixel wide skeleton.
// On a skeleton, countTransitions IS the crossing number: 1 means one ridge
// touches the pixel (an ending) and 3 means three ridges meet (a bifurcation).
std::vector<Minutia> findMinutiae(const BinaryImage& skel) {
    std::vector<Minutia> found;

    for(int Y = 1; Y < skel.height - 1; Y++){
        for(int X = 1; X < skel.width - 1; X++){
            if(skel.at(X, Y) == 0) continue; //only ridge pixels

            std::array<uint8_t, 8> p = getNeighbors(skel, X, Y);
            int cn = countTransitions(p);   //crossing number
            if(cn != 1 && cn != 3) continue; //middle of a ridge, or something odd

            Minutia m;
            m.x = X;
            m.y = Y;
            if(cn == 1){
                m.type = ENDING;
                m.angle = endingAngle(skel, X, Y);
            }
            else{
                m.type = BIFURCATION;

                //a junction can light up 2 touching pixels. keep only the first
                bool duplicate = false;
                for(const Minutia& f : found){
                    if(f.type == BIFURCATION && std::abs(f.x - X) <= 1 && std::abs(f.y - Y) <= 1) duplicate = true;
                }
                if(duplicate) continue;

                m.angle = bifurcationAngle(skel, X, Y);
            }
            found.push_back(m);
        }
        ui::progress(Y, skel.height - 2);
    }

    return found;
}

// saveMinutiae: draws the skeleton in gray, endings in red and bifurcations in
// blue, each with a short line showing its angle. Saves it as a PNG.
void saveMinutiae(const BinaryImage& skel, const std::vector<Minutia>& minutiae, const std::string& path) {

    //skeleton in light gray so the colored marks stand out
    cv::Mat gray(skel.height, skel.width, CV_8U);
    for(int Y = 0; Y < skel.height; Y++){
        for(int X = 0; X < skel.width; X++){
            if(skel.at(X, Y) == 1){
                gray.at<uchar>(Y, X) = 150; //ridge
            }
            else gray.at<uchar>(Y, X) = 255; //valley
        }
    }
    cv::Mat color;
    cv::cvtColor(gray, color, cv::COLOR_GRAY2BGR);

    //OpenCV colors are blue, green, red
    const cv::Scalar red(0, 0, 255);
    const cv::Scalar blue(255, 0, 0);
    for(const Minutia& m : minutiae){
        cv::Scalar c = blue;
        if(m.type == ENDING) c = red;

        cv::Point center(m.x, m.y);
        //minus sin because y points down on screen
        cv::Point tip(m.x + (int)std::lround(10 * std::cos(m.angle)),
                      m.y - (int)std::lround(10 * std::sin(m.angle)));
        cv::circle(color, center, 4, c, 1);
        cv::line(color, center, tip, c, 1);
    }

    cv::imwrite(path, color); //save image
}

// angleDiff: smallest difference between two angles, 0 to π.
double angleDiff(double a, double b) {
    double d = std::fabs(wrapAngle(a) - wrapAngle(b));
    if(d > PI) d = 2 * PI - d; //go the short way around the circle
    return d;
}

// distance: straight line distance between two minutiae in pixels.
double distance(const Minutia& a, const Minutia& b) {
    double dx = a.x - b.x;
    double dy = a.y - b.y;
    return std::sqrt(dx * dx + dy * dy);
}

// filterMinutiae: removes minutiae that are probably fake, and counts why.
// Each check only looks at minutiae that survived the checks before it.
std::vector<Minutia> filterMinutiae(const std::vector<Minutia>& minutiae, const BinaryImage& skel,
                                    const BinaryImage& mask, FilterReport& report) {
    int n = minutiae.size();
    std::vector<bool> keep(n, true);
    report = FilterReport();

    //check 1: near the mask edge. ridges get cut off at the edge of the finger,
    //so every ridge there looks like it ends
    for(int i = 0; i < n; i++){
        const Minutia& m = minutiae[i];
        bool inside = true;
        for(int Y = m.y - BORDER_MARGIN; Y <= m.y + BORDER_MARGIN && inside; Y++){
            for(int X = m.x - BORDER_MARGIN; X <= m.x + BORDER_MARGIN; X++){
                if(X < 0 || Y < 0 || X >= mask.width || Y >= mask.height || mask.at(X, Y) == 0){
                    inside = false; //off the image or off the finger
                    break;
                }
            }
        }
        if(!inside){
            keep[i] = false;
            report.border++;
        }
    }

    //check 2: spurs and islands. walk from every ending along its ridge.
    //  hit a junction soon   -> spur, a tiny branch sticking off a ridge
    //  hit another end soon  -> island, a tiny piece of ridge on its own
    for(int i = 0; i < n; i++){
        if(!keep[i] || minutiae[i].type != ENDING) continue;
        const Minutia& m = minutiae[i];

        //step onto the ridge, the ending only has one neighbor group
        std::array<uint8_t, 8> p = getNeighbors(skel, m.x, m.y);
        int first = 0;
        while(p[first] == 0) first++;
        RidgeTrace t = traceRidge(skel, {{m.x, m.y}}, m.x + NEIGHBOR_DX[first], m.y + NEIGHBOR_DY[first], ISLAND_LENGTH);
        auto [ex, ey] = t.path.back(); //where the walk stopped
        int steps = t.path.size();

        if(t.stop == STOP_JUNCTION && steps <= SPUR_LENGTH){
            keep[i] = false;
            report.spurs++;
            //the bifurcation at the root of the spur is fake too
            for(int j = 0; j < n; j++){
                if(keep[j] && minutiae[j].type == BIFURCATION &&
                   std::abs(minutiae[j].x - ex) <= 2 && std::abs(minutiae[j].y - ey) <= 2){
                    keep[j] = false;
                    report.spurs++;
                }
            }
        }
        else if(t.stop == STOP_ENDING){
            keep[i] = false;
            report.islands++;
            //the ending at the other end of the island is fake too
            for(int j = 0; j < n; j++){
                if(j != i && keep[j] && minutiae[j].x == ex && minutiae[j].y == ey){
                    keep[j] = false;
                    report.islands++;
                }
            }
        }
    }

    //check 3: broken ridges. two endings close together that point at each
    //other are one ridge with a gap in it (a pore, a crease or a dry spot)
    const double breakAngle = BREAK_ANGLE_DEG * PI / 180;
    for(int i = 0; i < n; i++){
        if(!keep[i] || minutiae[i].type != ENDING) continue;
        for(int j = i + 1; j < n; j++){
            if(!keep[j] || minutiae[j].type != ENDING) continue;
            const Minutia& a = minutiae[i];
            const Minutia& b = minutiae[j];
            if(distance(a, b) > BREAK_DISTANCE) continue; //too far apart

            //does a point toward b, and does b point toward a?
            bool aFacesB = angleDiff(a.angle, angleBetween(a.x, a.y, b.x, b.y)) < breakAngle;
            bool bFacesA = angleDiff(b.angle, angleBetween(b.x, b.y, a.x, a.y)) < breakAngle;
            if(aFacesB && bFacesA){
                keep[i] = false;
                keep[j] = false;
                report.broken += 2;
                break; //a is gone, move on to the next i
            }
        }
    }

    //check 4: anything still crowded together is noise, so drop both
    std::vector<bool> crowded(n, false);
    for(int i = 0; i < n; i++){
        if(!keep[i]) continue;
        for(int j = i + 1; j < n; j++){
            if(!keep[j]) continue;
            if(distance(minutiae[i], minutiae[j]) < MIN_DISTANCE){
                crowded[i] = true;
                crowded[j] = true;
            }
        }
    }
    for(int i = 0; i < n; i++){
        if(crowded[i]){
            keep[i] = false;
            report.close++;
        }
    }

    //copy out the survivors
    std::vector<Minutia> kept;
    for(int i = 0; i < n; i++){
        if(keep[i]) kept.push_back(minutiae[i]);
    }
    return kept;
}

// saveTemplate: writes a template as plain text. The first line is a header:
//     fpmatch-template <version> <width> <height> <count>
// then one minutia per line:  x y angle type   (type is "ending" or "bifurcation")
void saveTemplate(const Template& t, const std::string& path) {
    std::ofstream out(path);
    if(!out){
        throw std::runtime_error("saveTemplate: could not open " + path);
    }

    //max_digits10 (17 for a double) is the number of digits that always reads
    //back as the exact same double, so a save and load gives identical angles
    out << std::setprecision(std::numeric_limits<double>::max_digits10);

    out << "fpmatch-template " << PIPELINE_VERSION << " " << t.width << " " << t.height
        << " " << t.minutiae.size() << "\n";
    for(const Minutia& m : t.minutiae){
        std::string type = "bifurcation";
        if(m.type == ENDING) type = "ending";
        out << m.x << " " << m.y << " " << m.angle << " " << type << "\n"; //one minutia per line
    }

    if(!out){
        throw std::runtime_error("saveTemplate: could not write " + path);
    }
}

// loadTemplate: reads a template written by saveTemplate.
Template loadTemplate(const std::string& path) {
    std::ifstream in(path);
    if(!in){
        throw std::runtime_error("loadTemplate: could not open " + path);
    }

    //check the header line
    std::string magic;
    std::string version;
    size_t count = 0;
    Template t;
    in >> magic >> version >> t.width >> t.height >> count;
    if(!in || magic != "fpmatch-template"){
        throw std::runtime_error("loadTemplate: " + path + " is not a template file");
    }

    //read one minutia per line
    for(size_t i = 0; i < count; i++){
        Minutia m;
        std::string type;
        in >> m.x >> m.y >> m.angle >> type;
        if(!in){
            throw std::runtime_error("loadTemplate: " + path + " ends early at minutia " + std::to_string(i));
        }
        if(type == "ending"){
            m.type = ENDING;
        }
        else if(type == "bifurcation"){
            m.type = BIFURCATION;
        }
        else{
            throw std::runtime_error("loadTemplate: unknown minutia type \"" + type + "\" in " + path);
        }
        t.minutiae.push_back(m);
    }

    return t;
}
