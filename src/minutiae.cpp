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

// filter: a branch shorter than this that ends is noise (a fake spur)
const int SPUR_LENGTH = 12;

// filter: a whole ridge shorter than this (ending to ending) is noise (a fake island)
const int ISLAND_LENGTH = 15;

// filter: an island this short with no other ridge near it is a real dot, not noise
const int DOT_LENGTH = 6;
const int DOT_CLEARANCE = 3;

// shapes: the longest a short ridge, a spur or each side of a lake can be.
// 40 px is about 4 ridge widths. Anything longer is just ordinary ridges
const int SHORT_RIDGE_LENGTH = 40;
const int SPUR_MAX_LENGTH = 25;
const int LAKE_LENGTH = 40;

// shapes: a crossover is a ridge at most this long joining two ridges, and it
// has to run across them by at least this angle
const int CROSSOVER_LENGTH = 20;
const double CROSSOVER_ANGLE_DEG = 45;

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

// traceBranches: walks every ridge leaving (x, y), up to maxSteps each. The first
// pixel of each group of ridge neighbors starts one branch. A group starts
// wherever the ring goes from 0 to 1, same as countTransitions.
std::vector<RidgeTrace> traceBranches(const BinaryImage& skel, int x, int y, int maxSteps) {
    std::array<uint8_t, 8> p = getNeighbors(skel, x, y);
    std::vector<std::pair<int, int>> starts;
    for(int i = 0; i < 8; i++){
        int next = (i + 1) % 8;
        if(p[i] == 0 && p[next] == 1){
            starts.push_back({x + NEIGHBOR_DX[next], y + NEIGHBOR_DY[next]});
        }
    }

    //the other branches and the center count as visited so no walk can cut
    //across into another branch
    std::vector<RidgeTrace> traces;
    for(size_t b = 0; b < starts.size(); b++){
        std::vector<std::pair<int, int>> visited = {{x, y}};
        for(size_t o = 0; o < starts.size(); o++){
            if(o != b) visited.push_back(starts[o]);
        }
        traces.push_back(traceRidge(skel, visited, starts[b].first, starts[b].second, maxSteps));
    }
    return traces;
}

// bifurcationAngle: walks all 3 branches. The two branches closest in angle are
// the fork, the third one is the trunk, and the angle points away from the trunk.
double bifurcationAngle(const BinaryImage& skel, int x, int y) {
    std::vector<RidgeTrace> traces = traceBranches(skel, x, y, ANGLE_TRACE_STEPS);
    if(traces.size() != 3) return 0; //can't happen for a real bifurcation

    double branch[3];
    for(int b = 0; b < 3; b++){
        auto [ex, ey] = traces[b].path.back();
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

// findMinutiae: finds every dot, ending, bifurcation and crossing on a 1 pixel
// wide skeleton. On a skeleton, countTransitions IS the crossing number: 0 means
// no ridge touches the pixel (a dot), 1 means one ridge does (an ending), 3 means
// three ridges meet (a bifurcation) and 4 means two ridges cross.
std::vector<Minutia> findMinutiae(const BinaryImage& skel) {
    std::vector<Minutia> found;

    for(int Y = 1; Y < skel.height - 1; Y++){
        for(int X = 1; X < skel.width - 1; X++){
            if(skel.at(X, Y) == 0) continue; //only ridge pixels

            std::array<uint8_t, 8> p = getNeighbors(skel, X, Y);
            int cn = countTransitions(p);   //crossing number
            if(cn == 2) continue;           //middle of a ridge
            if(cn == 0 && countNeighbors(p) > 0) continue; //solid blob, thinning missed it

            Minutia m;
            m.x = X;
            m.y = Y;
            if(cn == 0){
                m.type = DOT; //a dot has no direction, angle stays 0
            }
            else if(cn == 1){
                m.type = ENDING;
                m.angle = endingAngle(skel, X, Y);
            }
            else if(cn >= 4){
                //two ridges crossing. the angle runs along one of them, kept
                //from 0 to π since a line looks the same both ways
                m.type = CROSSOVER;
                std::vector<RidgeTrace> traces = traceBranches(skel, X, Y, ANGLE_TRACE_STEPS);
                auto [ex, ey] = traces[0].path.back();
                m.angle = std::fmod(angleBetween(X, Y, ex, ey), PI);
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

// typeColor: the color each type is drawn in. OpenCV colors are blue, green, red.
cv::Scalar typeColor(MinutiaType type) {
    if(type == ENDING) return cv::Scalar(0, 0, 255);           //red
    if(type == BIFURCATION) return cv::Scalar(255, 0, 0);      //blue
    if(type == DOT) return cv::Scalar(255, 0, 255);            //magenta
    if(type == CROSSOVER) return cv::Scalar(0, 140, 255);      //orange
    if(type == SHORT_RIDGE) return cv::Scalar(0, 170, 0);      //green
    if(type == SPUR) return cv::Scalar(170, 170, 0);           //teal
    return cv::Scalar(160, 0, 120);                            //lake, purple
}

// saveMinutiae: draws the skeleton in gray and every minutia in its type's
// color, each with a short line showing its angle. Endings and bifurcations
// get small circles, the bigger shapes get thick ones. Saves it as a PNG.
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

    for(const Minutia& m : minutiae){
        cv::Scalar c = typeColor(m.type);
        cv::Point center(m.x, m.y);
        if(m.type == DOT){
            cv::circle(color, center, 4, c, -1); //filled, a dot has no direction to draw
            continue;
        }

        int radius = 4;
        int thick = 1;
        if(m.type != ENDING && m.type != BIFURCATION){
            radius = 7;
            thick = 2;
        }
        //minus sin because y points down on screen
        int tipX = (int)std::lround(10 * std::cos(m.angle));
        int tipY = -(int)std::lround(10 * std::sin(m.angle));
        cv::circle(color, center, radius, c, thick);
        cv::line(color, center, cv::Point(m.x + tipX, m.y + tipY), c, thick);
        if(isAxis(m.type)){
            cv::line(color, center, cv::Point(m.x - tipX, m.y - tipY), c, thick); //same both ways
        }
    }

    cv::imwrite(path, color); //save image
}

// typeName: the word for a type, as written in templates.
std::string typeName(MinutiaType type) {
    if(type == DOT) return "dot";
    if(type == ENDING) return "ending";
    if(type == BIFURCATION) return "bifurcation";
    if(type == CROSSOVER) return "crossover";
    if(type == SHORT_RIDGE) return "short_ridge";
    if(type == SPUR) return "spur";
    return "lake";
}

// isAxis: true for the types that look the same from both ends, so their angle
// is a line from 0 to π and not an arrow from 0 to 2π.
bool isAxis(MinutiaType type) {
    if(type == CROSSOVER || type == SHORT_RIDGE || type == LAKE) return true;
    return false;
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

// minutiaAngleDiff: how far apart two minutiae of the same type point, 0 to π.
// Lines (isAxis) can only be up to π/2 apart, and dots always count as 0.
double minutiaAngleDiff(const Minutia& a, const Minutia& b) {
    if(a.type == DOT || b.type == DOT) return 0;
    double d = angleDiff(a.angle, b.angle);
    if(isAxis(a.type) && d > PI / 2) d = PI - d; //a line pointing the other way is the same line
    return d;
}

// isolated: true if no skeleton pixel outside `piece` is within `radius` of it.
bool isolated(const BinaryImage& skel, const std::vector<std::pair<int, int>>& piece, int radius) {
    for(const auto& [px, py] : piece){
        for(int Y = py - radius; Y <= py + radius; Y++){
            for(int X = px - radius; X <= px + radius; X++){
                if(X < 0 || Y < 0 || X >= skel.width || Y >= skel.height) continue;
                if(skel.at(X, Y) == 1 && !wasVisited(piece, X, Y)) return false; //some other ridge
            }
        }
    }
    return true;
}

// filterMinutiae: removes minutiae that are probably fake, and counts why.
// Each check only looks at minutiae that survived the checks before it.
std::vector<Minutia> filterMinutiae(const std::vector<Minutia>& found, const BinaryImage& skel,
                                    const BinaryImage& mask, FilterReport& report) {
    std::vector<Minutia> minutiae = found; //a copy, so check 2 can add dots to it
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

    //check 2: spurs, islands and dots. walk from every ending along its ridge.
    //  hit a junction soon   -> spur, a tiny branch sticking off a ridge
    //  hit another end soon  -> island, a tiny piece of ridge on its own
    //  a very tiny island with nothing around it is a real dot, so it stays as one
    for(int i = 0; i < n; i++){
        if(!keep[i]) continue;

        //a one pixel dot from findMinutiae only counts if it sits alone
        if(minutiae[i].type == DOT){
            if(isolated(skel, {{minutiae[i].x, minutiae[i].y}}, DOT_CLEARANCE)){
                report.dots++;
            }
            else{
                keep[i] = false;
                report.islands++;
            }
            continue;
        }

        if(minutiae[i].type != ENDING) continue;
        Minutia m = minutiae[i]; //a copy, the push_back below can move the list

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
            //the ending at the other end of the island goes too
            for(int j = 0; j < n; j++){
                if(j != i && keep[j] && minutiae[j].x == ex && minutiae[j].y == ey){
                    keep[j] = false;
                }
            }

            //the whole piece, ending included. short and alone means a dot
            std::vector<std::pair<int, int>> piece = t.path;
            piece.push_back({m.x, m.y});
            if((int)piece.size() <= DOT_LENGTH && isolated(skel, piece, DOT_CLEARANCE)){
                Minutia dot;
                dot.type = DOT;
                dot.x = piece[piece.size() / 2].first; //the middle pixel
                dot.y = piece[piece.size() / 2].second;
                minutiae.push_back(dot);
                keep.push_back(true);
                report.dots++;
            }
            else report.islands += 2;
        }
    }
    n = minutiae.size(); //now with the new dots

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

// findNear: the index of a minutia of this type within 2 px of (x, y), or -1.
// A junction can light up 2 touching pixels, so the end of a walk lands on
// either one.
int findNear(const std::vector<Minutia>& minutiae, MinutiaType type, int x, int y) {
    for(size_t i = 0; i < minutiae.size(); i++){
        const Minutia& m = minutiae[i];
        if(m.type == type && std::abs(m.x - x) <= 2 && std::abs(m.y - y) <= 2) return i;
    }
    return -1;
}

// crossesAt: true if the ridge from bifurcation i to bifurcation j runs across a
// ridge that goes straight through i, like one side of the letter H. A normal
// fork (a Y) fails this, since two of its branches are close together.
bool crossesAt(const std::vector<Minutia>& minutiae, const BinaryImage& skel, int i, int j) {
    const Minutia& a = minutiae[i];
    const Minutia& b = minutiae[j];
    std::vector<RidgeTrace> longWalk = traceBranches(skel, a.x, a.y, CROSSOVER_LENGTH);
    std::vector<RidgeTrace> shortWalk = traceBranches(skel, a.x, a.y, ANGLE_TRACE_STEPS);
    if(longWalk.size() != 3 || shortWalk.size() != 3) return false;

    //which branch is the bridge to j
    int bridge = -1;
    for(int k = 0; k < 3; k++){
        auto [ex, ey] = longWalk[k].path.back();
        if(longWalk[k].stop == STOP_JUNCTION && std::abs(ex - b.x) <= 2 && std::abs(ey - b.y) <= 2) bridge = k;
    }
    if(bridge < 0) return false;

    //the angle of each branch, measured close to the center
    double branch[3];
    for(int k = 0; k < 3; k++){
        auto [ex, ey] = shortWalk[k].path.back();
        branch[k] = angleBetween(a.x, a.y, ex, ey);
    }

    //the other two branches have to make a straight line, and the bridge has
    //to leave that line at a real angle
    const double crossAngle = CROSSOVER_ANGLE_DEG * PI / 180;
    int p = (bridge + 1) % 3;
    int q = (bridge + 2) % 3;
    if(angleDiff(branch[p], branch[q]) < PI - crossAngle) return false; //bent, so it's a fork
    if(angleDiff(branch[bridge], branch[p]) < crossAngle) return false;
    if(angleDiff(branch[bridge], branch[q]) < crossAngle) return false;
    return true;
}

// makeShape: a new minutia halfway between (x1, y1) and (x2, y2). For a line
// type the angle is kept from 0 to π, since it looks the same both ways.
Minutia makeShape(MinutiaType type, int x1, int y1, int x2, int y2) {
    Minutia s;
    s.type = type;
    s.x = (x1 + x2) / 2;
    s.y = (y1 + y2) / 2;
    s.angle = std::fmod(angleBetween(x1, y1, x2, y2), PI);
    return s;
}

// findShapes: finds the bigger ridge shapes made of two minutiae joined by a
// short piece of ridge, after filtering:
//   short ridge  ending to ending
//   spur         ending to bifurcation, a short branch off a ridge
//   lake         bifurcation to bifurcation along two separate paths
//   crossover    bifurcation to bifurcation along one path, across the ridges
// Returns only the new shapes. The endings and bifurcations they're made of
// stay in the template too.
std::vector<Minutia> findShapes(const std::vector<Minutia>& minutiae, const BinaryImage& skel) {
    std::vector<Minutia> shapes;
    int n = minutiae.size();

    //short ridges and spurs: walk from every ending
    for(int i = 0; i < n; i++){
        const Minutia& m = minutiae[i];
        if(m.type != ENDING) continue;
        std::vector<RidgeTrace> walk = traceBranches(skel, m.x, m.y, SHORT_RIDGE_LENGTH);
        if(walk.size() != 1) continue; //can't happen for a real ending
        auto [ex, ey] = walk[0].path.back();
        int steps = walk[0].path.size();

        if(walk[0].stop == STOP_ENDING){
            int j = findNear(minutiae, ENDING, ex, ey);
            if(j > i) shapes.push_back(makeShape(SHORT_RIDGE, m.x, m.y, ex, ey)); //j > i counts it once
        }
        else if(walk[0].stop == STOP_JUNCTION && steps <= SPUR_MAX_LENGTH){
            int j = findNear(minutiae, BIFURCATION, ex, ey);
            if(j >= 0){
                //a spur sits at its root and points out along the branch
                Minutia s;
                s.type = SPUR;
                s.x = minutiae[j].x;
                s.y = minutiae[j].y;
                s.angle = angleBetween(s.x, s.y, m.x, m.y);
                shapes.push_back(s);
            }
        }
    }

    //lakes and crossovers: walk every branch of every bifurcation and see which
    //other bifurcations they reach
    for(int i = 0; i < n; i++){
        const Minutia& a = minutiae[i];
        if(a.type != BIFURCATION) continue;
        std::vector<RidgeTrace> walk = traceBranches(skel, a.x, a.y, LAKE_LENGTH);

        std::vector<int> reached; //one entry per branch that ends at another bifurcation
        for(const RidgeTrace& t : walk){
            if(t.stop != STOP_JUNCTION) continue;
            auto [ex, ey] = t.path.back();
            int j = findNear(minutiae, BIFURCATION, ex, ey);
            if(j > i) reached.push_back(j); //j > i counts each shape once, and skips i itself
        }

        for(size_t k = 0; k < reached.size(); k++){
            int j = reached[k];
            int paths = 0;
            for(int r : reached){
                if(r == j) paths++;
            }
            const Minutia& b = minutiae[j];

            if(paths >= 2){
                bool first = true; //two paths reach j, only add the lake once
                for(size_t e = 0; e < k; e++){
                    if(reached[e] == j) first = false;
                }
                if(first) shapes.push_back(makeShape(LAKE, a.x, a.y, b.x, b.y));
            }
            else if(crossesAt(minutiae, skel, i, j) && crossesAt(minutiae, skel, j, i)){
                shapes.push_back(makeShape(CROSSOVER, a.x, a.y, b.x, b.y));
            }
        }
    }

    return shapes;
}

// saveTemplate: writes a template as plain text. The first line is a header:
//     fpmatch-template <version> <width> <height> <count>
// then one minutia per line:  x y angle type   (type is a word from typeName)
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
        out << m.x << " " << m.y << " " << m.angle << " " << typeName(m.type) << "\n"; //one minutia per line
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
        bool known = false;
        const MinutiaType types[] = {DOT, ENDING, BIFURCATION, CROSSOVER, SHORT_RIDGE, SPUR, LAKE};
        for(MinutiaType tp : types){
            if(type == typeName(tp)){
                m.type = tp;
                known = true;
            }
        }
        if(!known){
            throw std::runtime_error("loadTemplate: unknown minutia type \"" + type + "\" in " + path);
        }
        t.minutiae.push_back(m);
    }

    return t;
}
