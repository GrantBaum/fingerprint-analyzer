//nessecary libs for ts
#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include "fingerprint.hpp"

// =============================================================================
//  TUNING NUMBERS. Every number that changes how matching behaves is here.
//  Change one at a time and rerun "fpmatch eval" to see what it does.
// =============================================================================

// Hough voting bins. Too fine and real matches split across many bins, too
// coarse and wrong alignments win by accident.
const double ROTATION_BIN_DEG = 5;     // width of one rotation bin
const double SHIFT_BIN_PX = 8;         // width of one dx or dy bin

// The biggest rotation and shift we search. Scanners hold the finger roughly
// upright, so huge rotations are almost always wrong answers.
const double MAX_ROTATION_DEG = 60;
const double MAX_SHIFT_PX = 300;

// Only let an ending vote with an ending and a bifurcation with a bifurcation.
// Off by default, because pressing harder can turn an ending into a bifurcation.
const bool SAME_TYPE_ONLY = false;

// Pairing: after alignment, two minutiae pair up when they are this close.
// Skin stretches, so these need some slack.
const double MATCH_DISTANCE_PX = 12;
const double MATCH_ANGLE_DEG = 20;

// A print with fewer minutiae than this can't be matched with any confidence.
const int MIN_MINUTIAE = 5;

// Scores at or above this are called a MATCH. Set from the EER of "fpmatch eval".
extern const double MATCH_THRESHOLD = 0.030;

// =============================================================================

const double DEG = PI / 180; //multiply degrees by this to get radians

// transformMinutia: turns a minutia from print A by the alignment and shifts it,
// so it lands where it should be in print B.
Minutia transformMinutia(const Minutia& m, const Alignment& al) {
    double c = std::cos(al.rotation);
    double s = std::sin(al.rotation);

    //position relative to the center of A, since that's what we turn around
    double x = m.x - al.cx;
    double y = m.y - al.cy;

    //rotation counterclockwise on screen. the usual math formula is
    //  x' = x cos - y sin,  y' = x sin + y cos
    //but our y points down, so both sin terms flip sign
    double rx = x * c + y * s;
    double ry = -x * s + y * c;

    Minutia out = m;
    out.x = (int)std::lround(rx + al.cx + al.dx);
    out.y = (int)std::lround(ry + al.cy + al.dy);
    out.angle = wrapAngle(m.angle + al.rotation); //the direction turns too
    return out;
}

// alignTemplates: finds the rotation and shift that puts the most minutiae of
// A on top of minutiae of B, using Hough voting.
//
// The idea: if a minutia in A really is the same point as a minutia in B, then
// the difference between their angles is the rotation, and after turning A by
// that much, the gap between them is the shift. Every possible pair (one from
// A, one from B) works out that rotation and shift and casts one vote for it.
// Wrong pairs vote all over the place. Right pairs all vote for the same
// answer, so that bin piles up votes and wins.
Alignment alignTemplates(const Template& a, const Template& b) {
    Alignment al;
    al.cx = a.width / 2.0;
    al.cy = a.height / 2.0;

    //size of the voting grid
    int rotBins = (int)(2 * MAX_ROTATION_DEG / ROTATION_BIN_DEG) + 1;
    int shiftBins = (int)(2 * MAX_SHIFT_PX / SHIFT_BIN_PX) + 1;
    std::vector<int> votes(rotBins * shiftBins * shiftBins, 0);

    //indexing helper for the 3D grid stored as one flat list
    auto bin = [&](int r, int x, int y) -> int& {
        return votes[(r * shiftBins + y) * shiftBins + x];
    };

    //works out which bin a pair (i from A, j from B) votes for.
    //returns false if the vote is outside the grid. also hands back the exact
    //rotation and shift so we can average them later
    auto voteFor = [&](int i, int j, int& r, int& x, int& y, double& rot, double& dx, double& dy) {
        const Minutia& ma = a.minutiae[i];
        const Minutia& mb = b.minutiae[j];
        if(SAME_TYPE_ONLY && ma.type != mb.type) return false;

        //rotation from -180 to 180 degrees
        rot = wrapAngle(mb.angle - ma.angle);
        if(rot > PI) rot -= 2 * PI;
        if(std::fabs(rot) > MAX_ROTATION_DEG * DEG) return false; //too much rotation

        //turn A's point with no shift, then the shift is whatever is left over
        Alignment trial = al;
        trial.rotation = rot;
        Minutia moved = transformMinutia(ma, trial);
        dx = mb.x - moved.x;
        dy = mb.y - moved.y;

        //which bin those numbers fall in. lround snaps to the nearest bin center
        r = (int)std::lround((rot / DEG + MAX_ROTATION_DEG) / ROTATION_BIN_DEG);
        x = (int)std::lround((dx + MAX_SHIFT_PX) / SHIFT_BIN_PX);
        y = (int)std::lround((dy + MAX_SHIFT_PX) / SHIFT_BIN_PX);
        if(r < 0 || r >= rotBins || x < 0 || x >= shiftBins || y < 0 || y >= shiftBins) return false;
        return true;
    };

    //every pair votes
    for(size_t i = 0; i < a.minutiae.size(); i++){
        for(size_t j = 0; j < b.minutiae.size(); j++){
            int r, x, y;
            double rot, dx, dy;
            if(voteFor(i, j, r, x, y, rot, dx, dy)) bin(r, x, y)++;
        }
    }

    //find the winner. a real match often lands right on the line between two
    //bins and splits its votes, so we score each bin by adding up its 3 x 3 x 3
    //neighborhood instead of looking at it alone
    int bestR = 0, bestX = 0, bestY = 0;
    int bestVotes = 0;
    for(int r = 0; r < rotBins; r++){
        for(int Y = 0; Y < shiftBins; Y++){
            for(int X = 0; X < shiftBins; X++){
                if(bin(r, X, Y) == 0) continue; //empty bins can't win, skip them fast

                int total = 0;
                for(int nr = r - 1; nr <= r + 1; nr++){
                    for(int ny = Y - 1; ny <= Y + 1; ny++){
                        for(int nx = X - 1; nx <= X + 1; nx++){
                            if(nr < 0 || nr >= rotBins || ny < 0 || ny >= shiftBins || nx < 0 || nx >= shiftBins) continue;
                            total += bin(nr, nx, ny);
                        }
                    }
                }
                if(total > bestVotes){
                    bestVotes = total;
                    bestR = r;
                    bestX = X;
                    bestY = Y;
                }
            }
        }
    }
    al.votes = bestVotes;
    if(bestVotes == 0) return al; //no votes at all, leave it unrotated and unshifted

    //the bin center is only accurate to half a bin. average the exact numbers
    //of every vote in the winning neighborhood to get a sharper answer.
    //rotations are averaged as sin and cos so -179 and 179 average to 180, not 0
    double sumSin = 0, sumCos = 0, sumDx = 0, sumDy = 0;
    int count = 0;
    for(size_t i = 0; i < a.minutiae.size(); i++){
        for(size_t j = 0; j < b.minutiae.size(); j++){
            int r, x, y;
            double rot, dx, dy;
            if(!voteFor(i, j, r, x, y, rot, dx, dy)) continue;
            if(std::abs(r - bestR) > 1 || std::abs(x - bestX) > 1 || std::abs(y - bestY) > 1) continue; //not in the winning neighborhood
            sumSin += std::sin(rot);
            sumCos += std::cos(rot);
            sumDx += dx;
            sumDy += dy;
            count++;
        }
    }
    al.rotation = std::atan2(sumSin, sumCos);
    al.dx = sumDx / count;
    al.dy = sumDy / count;
    return al;
}

// matchTemplates: aligns A onto B, pairs up minutiae and scores the result.
MatchResult matchTemplates(const Template& a, const Template& b) {
    MatchResult result;
    int nA = a.minutiae.size();
    int nB = b.minutiae.size();
    if(nA < MIN_MINUTIAE || nB < MIN_MINUTIAE) return result; //not enough to go on, score 0

    result.alignment = alignTemplates(a, b);

    //move every minutia of A into B's position
    std::vector<Minutia> moved;
    for(const Minutia& m : a.minutiae) moved.push_back(transformMinutia(m, result.alignment));

    //list every pair that is close enough in both distance and angle
    struct Candidate {
        double dist;
        int i;
        int j;
    };
    std::vector<Candidate> candidates;
    for(int i = 0; i < nA; i++){
        for(int j = 0; j < nB; j++){
            double d = distance(moved[i], b.minutiae[j]);
            if(d > MATCH_DISTANCE_PX) continue;                                                 //too far apart
            if(angleDiff(moved[i].angle, b.minutiae[j].angle) > MATCH_ANGLE_DEG * DEG) continue; //pointing different ways
            candidates.push_back({d, i, j});
        }
    }

    //closest pairs first. each minutia can only be used once, so going in order
    //of distance means the best pairs get first pick
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& p, const Candidate& q) { return p.dist < q.dist; });
    std::vector<bool> usedA(nA, false);
    std::vector<bool> usedB(nB, false);
    for(const Candidate& c : candidates){
        if(usedA[c.i] || usedB[c.j]) continue; //one of them already has a partner
        usedA[c.i] = true;
        usedB[c.j] = true;
        result.pairs.push_back({c.i, c.j});
    }

    //score: matched / nA is "how much of A was found in B" and matched / nB is
    //the other way around. multiplying them means both prints have to agree,
    //so a print with hundreds of noisy minutiae can't match everything by luck
    result.matched = result.pairs.size();
    result.score = (double)result.matched * result.matched / ((double)nA * nB);
    return result;
}

// saveAligned: draws B's minutiae in blue and A's moved minutiae in red, with a
// green line joining every matched pair.
void saveAligned(const Template& a, const Template& b, const MatchResult& result, const std::string& path) {
    cv::Mat img(b.height, b.width, CV_8UC3, cv::Scalar(255, 255, 255)); //white canvas the size of B

    //OpenCV colors are blue, green, red
    const cv::Scalar red(0, 0, 255);
    const cv::Scalar blue(255, 0, 0);
    const cv::Scalar green(0, 170, 0);

    //draws one minutia as a circle with a short angle line
    auto draw = [&](const Minutia& m, const cv::Scalar& c) {
        cv::Point center(m.x, m.y);
        cv::Point tip(m.x + (int)std::lround(10 * std::cos(m.angle)),
                      m.y - (int)std::lround(10 * std::sin(m.angle))); //minus because y points down
        cv::circle(img, center, 4, c, 1);
        cv::line(img, center, tip, c, 1);
    };

    for(const Minutia& m : b.minutiae) draw(m, blue);
    for(const Minutia& m : a.minutiae) draw(transformMinutia(m, result.alignment), red);
    for(const auto& [i, j] : result.pairs){
        Minutia ma = transformMinutia(a.minutiae[i], result.alignment);
        const Minutia& mb = b.minutiae[j];
        cv::line(img, cv::Point(ma.x, ma.y), cv::Point(mb.x, mb.y), green, 2);
    }

    cv::imwrite(path, img); //save image
}
