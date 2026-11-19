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
// Pressing harder can turn an ending into a bifurcation, so this was off at
// first. With Gabor enhancement the types are reliable enough that it helps:
// EER 15.5% -> 15.3% and far fewer impostor votes.
const bool SAME_TYPE_ONLY = true;

// How many of the best Hough peaks to try. Each one gets paired up and scored,
// and the best score wins. A noisy bin can beat the true one by a vote or two,
// so trying a few makes sure the real alignment gets a turn.
const int HOUGH_PEAKS = 5;

// Pairing: after alignment, two minutiae pair up when they are this close.
// Skin stretches, so these need some slack.
const double MATCH_DISTANCE_PX = 12;
const double MATCH_ANGLE_DEG = 20;

// Use the bigger ridge shapes (dots, short ridges, spurs, lakes, crossovers)
// when matching, not just endings and bifurcations. A shape only pairs with
// the same kind of shape. Off, because they made it worse: EER 13.5% -> 15.3%.
// A small change in pressure turns a lake into two bifurcations or a short
// ridge into a broken one, so the shapes rarely show up the same way twice.
// They still go in the template and the pictures for the examiner to look at.
const bool USE_SHAPES = false;

// A print with fewer minutiae than this can't be matched with any confidence.
const int MIN_MINUTIAE = 5;

// Overlap: two impressions of a finger rarely cover the same skin. A minutia
// only counts toward the score if it landed within this distance of some
// minutia of the other print, meaning it's inside the area both prints share.
const bool OVERLAP_ONLY = false;
const double OVERLAP_RADIUS_PX = 40;

// Overlap: never divide by fewer than this many minutiae, so a tiny overlap
// with 3 lucky pairs can't score like a full match.
const int MIN_OVERLAP = 10;

// Scores at or above this are called a MATCH. Set from the EER of "fpmatch eval".
extern const double MATCH_THRESHOLD = 0.028;

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

// alignTemplates: finds the rotations and shifts that put the most minutiae of
// A on top of minutiae of B, using Hough voting. Returns the HOUGH_PEAKS best.
//
// The idea: if a minutia in A really is the same point as a minutia in B, then
// the difference between their angles is the rotation, and after turning A by
// that much, the gap between them is the shift. Every possible pair (one from
// A, one from B) works out that rotation and shift and casts one vote for it.
// Wrong pairs vote all over the place. Right pairs all vote for the same
// answer, so that bin piles up votes and wins.
std::vector<Alignment> alignTemplates(const Template& a, const Template& b) {
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
        if(ma.type == DOT || mb.type == DOT) return false; //a dot has no angle to vote with

        //rotation from -180 to 180 degrees
        rot = wrapAngle(mb.angle - ma.angle);
        if(rot > PI) rot -= 2 * PI;

        //a line can be read either way round, so take the smaller turn
        if(isAxis(ma.type) && rot > PI / 2) rot -= PI;
        if(isAxis(ma.type) && rot < -PI / 2) rot += PI;
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

    //score every bin by adding up its 3 x 3 x 3 neighborhood. a real match often
    //lands right on the line between two bins and splits its votes, so looking
    //at a bin alone would miss it
    std::vector<int> totals(votes.size(), 0);
    for(int r = 0; r < rotBins; r++){
        for(int Y = 0; Y < shiftBins; Y++){
            for(int X = 0; X < shiftBins; X++){
                if(bin(r, X, Y) == 0) continue; //only bins near votes can win, the rest stay 0
                for(int nr = r - 1; nr <= r + 1; nr++){
                    for(int ny = Y - 1; ny <= Y + 1; ny++){
                        for(int nx = X - 1; nx <= X + 1; nx++){
                            if(nr < 0 || nr >= rotBins || ny < 0 || ny >= shiftBins || nx < 0 || nx >= shiftBins) continue;
                            //this bin's votes count for every neighbor around it
                            totals[(nr * shiftBins + ny) * shiftBins + nx] += bin(r, X, Y);
                        }
                    }
                }
            }
        }
    }

    //pick the top peaks. after each pick, wipe out the totals right around it
    //so the next pick is a really different alignment, not the same one again
    std::vector<Alignment> peaks;
    for(int k = 0; k < HOUGH_PEAKS; k++){
        int best = -1;
        for(size_t i = 0; i < totals.size(); i++){
            if(totals[i] > 0 && (best < 0 || totals[i] > totals[best])) best = i;
        }
        if(best < 0) break; //no votes left

        //turn the flat index back into (r, x, y)
        int bestX = best % shiftBins;
        int bestY = (best / shiftBins) % shiftBins;
        int bestR = best / (shiftBins * shiftBins);
        Alignment peak = al;
        peak.votes = totals[best];

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
                if(std::abs(r - bestR) > 1 || std::abs(x - bestX) > 1 || std::abs(y - bestY) > 1) continue; //not in this neighborhood
                sumSin += std::sin(rot);
                sumCos += std::cos(rot);
                sumDx += dx;
                sumDy += dy;
                count++;
            }
        }
        peak.rotation = std::atan2(sumSin, sumCos);
        peak.dx = sumDx / count;
        peak.dy = sumDy / count;
        peaks.push_back(peak);

        //wipe a 5 x 5 x 5 area around this peak
        for(int nr = bestR - 2; nr <= bestR + 2; nr++){
            for(int ny = bestY - 2; ny <= bestY + 2; ny++){
                for(int nx = bestX - 2; nx <= bestX + 2; nx++){
                    if(nr < 0 || nr >= rotBins || ny < 0 || ny >= shiftBins || nx < 0 || nx >= shiftBins) continue;
                    totals[(nr * shiftBins + ny) * shiftBins + nx] = 0;
                }
            }
        }
    }

    if(peaks.empty()) peaks.push_back(al); //no votes at all, leave it unrotated and unshifted
    return peaks;
}

// countOverlap: how many minutiae in `points` have some minutia of `other`
// within OVERLAP_RADIUS_PX, meaning they sit in the area both prints cover.
int countOverlap(const std::vector<Minutia>& points, const std::vector<Minutia>& other) {
    int count = 0;
    for(const Minutia& p : points){
        for(const Minutia& o : other){
            if(distance(p, o) <= OVERLAP_RADIUS_PX){
                count++;
                break; //one neighbor is enough
            }
        }
    }
    return count;
}

// pairAndScore: moves A by one alignment, pairs up minutiae and scores it.
MatchResult pairAndScore(const Template& a, const Template& b, const Alignment& al) {
    MatchResult result;
    result.alignment = al;
    int nA = a.minutiae.size();
    int nB = b.minutiae.size();

    //move every minutia of A into B's position
    std::vector<Minutia> moved;
    for(const Minutia& m : a.minutiae) moved.push_back(transformMinutia(m, al));

    //list every pair that is close enough in both distance and angle
    struct Candidate {
        double dist;
        int i;
        int j;
    };
    std::vector<Candidate> candidates;
    for(int i = 0; i < nA; i++){
        for(int j = 0; j < nB; j++){
            const Minutia& ma = moved[i];
            const Minutia& mb = b.minutiae[j];
            double d = distance(ma, mb);
            if(d > MATCH_DISTANCE_PX) continue;                                 //too far apart
            if(minutiaAngleDiff(ma, mb) > MATCH_ANGLE_DEG * DEG) continue;      //pointing different ways

            //endings and bifurcations can pair with each other, since pressing
            //harder turns one into the other. every other shape needs its own kind
            bool aBasic = (ma.type == ENDING || ma.type == BIFURCATION);
            bool bBasic = (mb.type == ENDING || mb.type == BIFURCATION);
            if(!(aBasic && bBasic) && ma.type != mb.type) continue;
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
    result.matched = result.pairs.size();

    //how many minutiae each print brings to the comparison. normally all of them,
    //but with OVERLAP_ONLY just the ones inside the shared area
    double countA = nA;
    double countB = nB;
    if(OVERLAP_ONLY){
        countA = std::max(countOverlap(moved, b.minutiae), MIN_OVERLAP);
        countB = std::max(countOverlap(b.minutiae, moved), MIN_OVERLAP);
        countA = std::max(countA, (double)result.matched); //can't match more than we counted
        countB = std::max(countB, (double)result.matched);
    }

    //score: matched / countA is "how much of A was found in B" and
    //matched / countB is the other way around. multiplying them means both
    //prints have to agree, so a print with hundreds of noisy minutiae can't
    //match everything by luck
    result.score = result.matched * result.matched / (countA * countB);
    return result;
}

// matchTemplates: aligns A onto B, pairs up minutiae and scores the result.
// Tries each of the top Hough peaks and keeps the best score.
MatchResult matchTemplates(const Template& fullA, const Template& fullB) {
    MatchResult best;

    //without USE_SHAPES, match on endings and bifurcations only. indexA and
    //indexB remember where each one sat in the full template
    Template a = fullA;
    Template b = fullB;
    std::vector<int> indexA, indexB;
    a.minutiae.clear();
    b.minutiae.clear();
    for(size_t i = 0; i < fullA.minutiae.size(); i++){
        const Minutia& m = fullA.minutiae[i];
        if(USE_SHAPES || m.type == ENDING || m.type == BIFURCATION){
            a.minutiae.push_back(m);
            indexA.push_back(i);
        }
    }
    for(size_t j = 0; j < fullB.minutiae.size(); j++){
        const Minutia& m = fullB.minutiae[j];
        if(USE_SHAPES || m.type == ENDING || m.type == BIFURCATION){
            b.minutiae.push_back(m);
            indexB.push_back(j);
        }
    }

    if((int)a.minutiae.size() < MIN_MINUTIAE || (int)b.minutiae.size() < MIN_MINUTIAE) return best; //not enough to go on, score 0

    std::vector<Alignment> peaks = alignTemplates(a, b);
    best = pairAndScore(a, b, peaks[0]); //the strongest peak wins ties
    for(size_t k = 1; k < peaks.size(); k++){
        MatchResult r = pairAndScore(a, b, peaks[k]);
        if(r.score > best.score) best = r;
    }

    //point the pairs back at the full templates
    for(auto& [i, j] : best.pairs){
        i = indexA[i];
        j = indexB[j];
    }
    return best;
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
        if(m.type == DOT){
            cv::circle(img, center, 3, c, -1); //filled, a dot has no direction
            return;
        }
        int radius = 4;
        if(m.type != ENDING && m.type != BIFURCATION) radius = 7; //the bigger shapes
        cv::Point tip(m.x + (int)std::lround(10 * std::cos(m.angle)),
                      m.y - (int)std::lround(10 * std::sin(m.angle))); //minus because y points down
        cv::circle(img, center, radius, c, 1);
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
