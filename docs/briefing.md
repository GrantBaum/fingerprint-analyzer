# fpmatch briefing

This document explains how fpmatch works, one stage at a time. It's written to learn from and to teach from. Each stage covers what it does, why it's needed, how the algorithm works, its main tuning numbers and its known weaknesses.

Every algorithm here is classic image processing, signal processing or geometry. There is no machine learning, no trained model and no network connection. Every number the program uses is written in the source code where you can read it.

## The big idea

A fingerprint is a pattern of ridges. Examiners don't compare whole pictures. They compare **minutiae**, the places where a ridge does something special:

- A **ridge ending** is where a ridge stops.
- A **bifurcation** is where one ridge splits into two.

Each minutia has a position (x, y) and a direction (an angle). Two prints from the same finger share many minutiae in the same arrangement. Prints from different fingers don't.

So fpmatch does three jobs:

1. **Enhance** the picture so the ridges are clean.
2. **Extract** the minutiae into a list called a template.
3. **Match** two templates by lining them up and counting how many points agree.

Every stage saves a debug image in `out/<print name>/` so you can see what it did.

## Test data

All numbers here come from FVC2002 DB1_B. That's a public test set from the Fingerprint Verification Competition. It has 10 fingers with 8 impressions each, scanned at 500 dpi. Each image is 388 x 374 pixels. Our copy is missing 4 impressions of finger 104, so it has 76 prints.

At 500 dpi, one ridge plus one valley is about 9 pixels wide. Many tuning numbers below are based on that.

---

## Stage 1: Load

**What it does.** Reads the image file and turns it into a grid of gray values from 0 (black) to 255 (white). Saves `00_raw.png`.

**Why.** Every later stage works on numbers, not on a file.

**How.**
1. Check that OpenCV can read the file type.
2. Read it as grayscale with `cv::imread`.
3. Copy every pixel into a `GrayImage`, which is a flat list of floats with an `at(x, y)` helper.

**Weaknesses.** None to speak of. A file that won't load stops the program with a clear error.

## Stage 2: Normalize

**What it does.** Shifts and scales the gray values so the average is 0 and the spread (standard deviation) is 1. Saves `01_normalized.png`.

**Why.** One scan might be dark and another might be light. After normalizing, both look the same to the rest of the pipeline, so the same settings work on both.

**How.**
1. Add up every pixel and divide by the count to get the mean. The sum uses a `double`, because a `float` loses precision over 145,000 additions.
2. Add up the squared distance of every pixel from the mean and divide by the count. That's the variance. Its square root is the standard deviation.
3. Replace every pixel with (pixel - mean) / standard deviation.

**Weaknesses.** It treats the whole image the same. A print that's dark on one side and light on the other is still uneven after this step. The local threshold in binarize handles that.

## Stage 3: Mask (segmentation)

**What it does.** Decides which parts of the image are finger and which are background. Saves `02_mask.png`, where black is finger.

**Why.** Background has dust and scanner noise. Without a mask, that noise turns into fake ridges and fake minutiae. The mask is also used later to throw out minutiae near the edge of the finger.

**How.**
1. Split the image into 16 x 16 blocks.
2. Work out the variance of each block. Ridges make a block swing between dark and light, so finger blocks have high variance. Background is flat, so its variance is low.
3. Blocks above the cutoff are finger.
4. Clean up with three steps:
   - **Majority vote.** A block is finger only if at least 5 of the 9 blocks around it are. This trims blocks that stick out.
   - **Biggest blob.** Keep only the largest connected group of finger blocks. Specks of dirt disappear.
   - **Fill holes.** Flood fill the background from the image edges. Any background left over is surrounded by finger, so it's a hole. Fill it.
5. After binarize, every pixel outside the mask is set to background.

The flood fill uses a to-do list of pixels instead of recursion. Recursion on a big region would run out of stack memory.

**Tuning.** Block size 16 px. Variance cutoff 0.1 (on normalized pixels).

**Weaknesses.** The edge of the mask is blocky, 16 px at a time. A very faint area of a real finger can be marked as background. A smudge with texture can be marked as finger.

## Stage 4: Orientation field

**What it does.** Works out which way the ridges run in each 16 x 16 block. Saves `03_orientation.png` with a red line per block.

**Why.** The Gabor filter in stage 6 needs to know the ridge direction everywhere.

**How.**
1. For every pixel, measure the **gradient** with the Sobel weights. The gradient points in the direction where brightness changes fastest. On a fingerprint that's across the ridges.
2. The ridge runs at a right angle to the gradient.
3. Average the gradients in a window around each block. There's a catch here. A gradient on one side of a ridge points the opposite way from the gradient on the other side (dark to light versus light to dark). A plain average would cancel them out to nothing.
4. **The fix: double every angle.** 0° and 180° both become 0° and 360°, which is the same direction. Now they add up instead of cancelling. In practice each gradient (gx, gy) becomes (gx² - gy², 2·gx·gy).
5. Smooth the field by adding up each block's 3 x 3 neighbors, twice. One bad block (a scar or a smudge) gets outvoted.
6. Halve the angle to undo the doubling, then turn it 90° to get the ridge direction.

**Tuning.** The window is 2 times the block size. 2 smoothing passes.

**Weaknesses.** Near the **core** (the center of a loop or whorl) and **deltas** (the triangle-shaped areas) the ridge direction changes very fast. One angle per block can't describe that. Expect wrong angles there.

## Stage 5: Ridge frequency

**What it does.** Measures how far apart the ridges are in each block. Saves `04_frequency.png`, where brighter means the ridges are closer together.

**Why.** The Gabor filter has to be tuned to the ridge spacing, just like a radio has to be tuned to a station.

**How.** This is the **x-signature** method from Hong, Wan and Jain (1998).
1. Picture a 32 x 16 pixel window centered on the block, turned so its long side runs across the ridges.
2. Average each row of 16 pixels (along the ridge) into one number. That gives 32 numbers.
3. Going across, those numbers dip at every ridge (dark) and rise at every valley (light). It looks like a wave.
4. Find the dips. The average gap between them is the ridge **period**. Frequency is 1 / period.
5. A period outside 3 to 25 pixels isn't a real ridge pattern, so that block is marked unknown.
6. Unknown blocks take the average of their known neighbors. Repeat until every finger block has a value.
7. Smooth once with a 3 x 3 average.

On print 101_1 the average period comes out at 9.3 px, which is right for 500 dpi.

**Tuning.** Window 32 x 16 px. Valid period 3 to 25 px.

**Weaknesses.** Blocks with heavy noise have no clear wave and have to borrow from their neighbors. Sampling uses the nearest pixel, which is a little rough on diagonal ridges.

## Stage 6: Gabor filter

**What it does.** Cleans the ridges. It joins broken ridges, fills in pores and fades scratches. Saves `05_enhanced.png`.

**Why.** Real prints are messy. Pores, dry skin and creases break ridges into pieces. Every break looks like two ridge endings, which are fake minutiae. Gabor filtering is the single biggest improvement in this project.

**How.**
1. A **Gabor kernel** is a small picture of a wave (a cosine) faded out by a soft round blob (a Gaussian). It looks like a few parallel ridges.
2. Build the kernel so its wave runs across the ridges at this block's angle, with this block's spacing.
3. For each finger pixel, lay the kernel over the image centered on that pixel. Multiply the overlapping values and add them up. This is called **convolution**.
4. If the patch has ridges at the matching angle and spacing, the answer is big. If it's noise, the answer is small. Ridges get stronger and noise fades.
5. Subtract the kernel's own average first, so a flat patch gives exactly 0.
6. Building a new kernel for every pixel would be slow. Instead angles are rounded to 16 steps and periods to whole pixels. Each kernel is built the first time it's needed and then reused. A print typically needs about 40 kernels.

**Tuning.** Sigma 4.0 in both directions (Hong). 16 angles. Kernel radius 3 x sigma = 12 px.

**Weaknesses.** The filter trusts the orientation field. Where the orientation is wrong (the core, deltas, scars), Gabor draws confident ridges in the wrong direction. That can create fake minutiae near the core. It's also the slowest stage, at about 625 multiplications per pixel.

## Stage 7: Binarize

**What it does.** Turns every pixel into ridge (1) or valley (0). Saves `06_binary.png`.

**Why.** Thinning and the crossing number need a clean yes or no for every pixel.

**How.**
1. For each pixel, take the average of the 15 x 15 square around it.
2. If the pixel is darker than that local average, it's ridge. Otherwise it's valley.
3. Wipe everything outside the mask.

A local average works better than one threshold for the whole image, because one part of a print can be darker than another.

**Tuning.** Window 15 px, about 1.5 ridge periods.

**Weaknesses.** It's slow (225 additions per pixel). Without Gabor in front of it, pores and breaks go straight through into the binary image.

## Stage 8: Thin (skeletonize)

**What it does.** Shrinks every ridge to a line 1 pixel wide. Saves `07_skeleton.png`.

**Why.** On a 1 pixel line, endings and bifurcations become easy to spot by looking at a pixel's 8 neighbors.

**How.** This is the Zhang-Suen algorithm (1984).
1. Look at each ridge pixel and its 8 neighbors, named P2 to P9 going clockwise from the top.
2. A pixel can be removed only if all three of these are true:
   - It has 2 to 6 ridge neighbors. So it's not a tip and not buried inside a ridge.
   - Walking around its neighbors finds exactly one 0 to 1 step. So removing it won't split the ridge.
   - It's on the correct edge for this sub-step.
3. Sub-step 1 peels the bottom and right edges. Sub-step 2 peels the top and left edges. Alternating keeps the line centered.
4. Mark all removable pixels first, then delete them all at once. Deleting as you go would change the answer for the next pixel.
5. Repeat until a full pass removes nothing.

**Weaknesses.** Zhang-Suen can leave small stair steps on diagonal lines and short spurs where a ridge edge was bumpy. The filter stage cleans most of these up.

## Stage 9: Minutiae (crossing number)

**What it does.** Finds every ridge ending and bifurcation on the skeleton and measures its angle. Saves `08_minutiae.png` with endings circled in red and bifurcations in blue.

**Why.** Minutiae are what gets compared.

**How.**
1. For each skeleton pixel, walk around its 8 neighbors in a circle and count the steps from 0 to 1. This is the **crossing number**.
   - 1 means one ridge touches the pixel. It's an **ending**.
   - 2 means the pixel is in the middle of a ridge.
   - 3 means three ridges meet. It's a **bifurcation**.
2. A junction can light up two touching pixels. Only the first is kept.
3. **Angle of an ending.** Walk 10 pixels along the ridge, away from the tip. The angle points from there back to the tip, out of the ridge.
4. **Angle of a bifurcation.** Walk 10 pixels along each of the three branches. The two branches closest in angle are the fork. The third is the trunk. The angle points away from the trunk, toward the gap in the fork.

**Angle convention.** Radians from 0 to 2π, like on a math graph. 0 points right, π/2 points up and angles turn counterclockwise. In an image, y points down, so every `atan2` call flips the y difference. This was checked with hand-drawn test shapes: a horizontal line, a vertical line and a Y.

**Tuning.** Angle trace length 10 px.

**Weaknesses.** Every break in the skeleton becomes two fake endings. Every bridge between ridges becomes two fake bifurcations. Without Gabor, print 101_1 gives 374 raw minutiae. With Gabor it gives 94.

## Stage 10: False minutiae filter

**What it does.** Removes minutiae that are probably fake and reports how many each check removed. Saves `09_filtered.png`.

**Why.** Fake minutiae add noise to matching. They make different fingers look alike and the same finger look different.

**How.** Five checks, in order. Each one only looks at minutiae that survived the ones before it.

1. **Mask edge.** If the 25 x 25 square around a minutia isn't all finger, drop it. Ridges get cut off at the edge of the finger, so every ridge there looks like it ends.
2. **Spurs.** Walk from each ending along its ridge. If you hit a junction within 12 px, it's a tiny branch sticking off a ridge. Drop the ending and the bifurcation at its root.
3. **Islands.** If that walk hits another ending within 15 px, it's a tiny ridge piece on its own. Drop both ends.
4. **Broken ridges.** Two endings within 15 px of each other that point toward each other (within 45°) are one ridge with a gap in it. Drop both.
5. **Crowding.** Any two minutiae closer than 6 px are noise. Drop both.

On 101_1 without Gabor this takes 374 minutiae down to 65. With Gabor it takes 94 down to 23.

**Weaknesses.** Real minutiae close to the finger's edge are lost. A real short ridge (a "dot" or "short ridge" in forensic terms) gets removed as an island. The rules are fixed distances, so they can't adapt to unusually wide or narrow ridges.

## Stage 11: Template

**What it does.** Saves the list of minutiae plus the image size in a plain text file, `template.tmpl`.

**Why.** Matching only needs the points, not the picture. A template is small, easy to read and fast to compare.

**How.** The first line is a header: `fpmatch-template <version> <width> <height> <count>`. Then each minutia gets one line: `x y angle type`. Angles are written with 17 significant digits. That's the number a `double` needs so a save and load gives back exactly the same value. A test confirmed the round trip is exact.

## Stage 12: Align (Hough voting)

**What it does.** Finds the rotation and shift that puts print A on top of print B.

**Why.** Nobody presses a finger in exactly the same spot twice. Before we can compare points, we have to undo the difference in position and angle.

**How.** This is the method from Ratha et al. (1996).
1. Take every possible pair: one minutia from A and one from B. With 30 minutiae each, that's 900 pairs.
2. For each pair, ask: if these two are the same point, what rotation and shift would line them up?
   - The rotation is the difference between their angles.
   - Turn A's point by that rotation around A's center. The shift is whatever gap is left between it and B's point.
3. Each pair casts one vote in a 3D grid of (rotation, dx, dy) bins.
4. Wrong pairs vote all over the place. Right pairs all vote for the same answer, so that bin piles up votes.
5. A real match often lands on the line between two bins and splits its votes. So each bin is scored by adding up its 3 x 3 x 3 neighborhood.
6. Take the top 5 peaks. After each pick, wipe the area around it so the next pick is a really different alignment.
7. For each peak, average the exact rotation and shift of every vote near it. That's sharper than the bin center. Rotations are averaged as sine and cosine, so -179° and 179° average to 180°, not 0°.

Only endings vote with endings and bifurcations with bifurcations. This cuts down the wrong votes.

A test aligned the template of print 101_1 with a copy of itself turned by 30° and shifted by (10, -5). The aligner found 29.2° and (11.4, -6.5). That's off by about a pixel, because minutia positions are whole pixels and get rounded after turning. All 23 points still paired up and the score was 1.

**Tuning.** Bins of 5° and 8 px. Rotation up to 60° either way. Shift up to 300 px. 5 peaks tried.

**Weaknesses.** It finds one rigid rotation and shift. Skin stretches, so a print pressed hard is a little distorted. Points far from the center then miss by more than the tolerance. Pressure can also turn an ending into a bifurcation. Same-type voting then misses that pair.

## Stage 13: Score

**What it does.** Pairs up minutiae after alignment and turns the count into a score from 0 to 1. Saves `aligned.png` for a compare.

**How.**
1. Move every minutia of A by the alignment.
2. List every (A, B) pair closer than 12 px whose angles differ by less than 20°.
3. Sort that list by distance. Go down it and accept a pair only if neither point is already taken. This is a **greedy** pairing. The best pairs get first pick.
4. Score = matched² / (count in A x count in B).

Why this formula? matched / A is "how much of A was found in B". matched / B is the other way around. Multiplying them means both prints have to agree. A print with hundreds of noisy minutiae can't score high just by matching lots of points by luck.

5. Steps 1 to 4 run for each of the 5 Hough peaks. The best score wins.
6. A score at or above **0.028** is called a MATCH. That number is the EER point from the evaluation below.

**Weaknesses.** Two impressions that only share a small area get a low score, because all the points outside the shared area count against them. There is an `OVERLAP_ONLY` switch in `matching.cpp` that only counts points in the shared area. It didn't help on this data, so it's off. It's a good thing to experiment with.

## The database

**What it does.** Keeps catalogued prints in `db/` on the local machine.

**How.**
- After analysis, results wait in `out/<print name>/`. Nothing goes into the database until the user picks Catalogue.
- Catalogue gives the next 4-digit ID by reading `db/index.csv`. It copies the **original** file (not a converted copy) to `db/raw/`, the skeleton to `db/skel/` and the template to `db/tmpl/`, all named by ID.
- The order is careful. First copy the files. Then write the index row. Then delete `out/`. If anything fails partway, the print isn't in the index and `out/` is still there, so the user can try again.
- Search loads every template, scores the current print against each one and shows the top 5. It lists the raw and skeleton paths so a person can look at the prints.

**The software suggests. The examiner decides.** The score is a ranking tool, not a verdict. At the threshold, roughly 1 in 7 impostor pairs still scores as a match. A trained examiner has to compare the actual prints.

## Evaluation

`fpmatch eval data` runs the standard FVC test.

- **Genuine pairs** are two impressions of the same finger. Every impression is compared with every other impression of its finger, once. That's 258 pairs here.
- **Impostor pairs** are two different fingers. The first impression of each finger is compared with the first impression of every other finger. That's 45 pairs.
- **FMR (false match rate):** the share of impostor pairs at or above the threshold. These are wrong accusations.
- **FNMR (false non-match rate):** the share of genuine pairs below the threshold. These are missed matches.
- Raising the threshold lowers FMR and raises FNMR. The **EER (equal error rate)** is where they meet. Lower is better.

### Results on DB1_B

| Version | EER |
| --- | --- |
| Baseline: no enhancement | 24.04% |
| With orientation, frequency and Gabor | 15.53% |
| Plus same-type voting and 5 Hough peaks (final) | **13.45%** |

At the final threshold of 0.028, FMR is 13.3% and FNMR is 14.0%.

For comparison, the best commercial systems in FVC2002 got under 1% on the full DB1 set. They used decades of engineering and much more advanced matching. 13.45% is a good result for a from-scratch teaching system.

### A lesson about overfitting

Every tuning number was tried on the same 76 prints that produce the EER. That's risky. With only 45 impostor pairs, each one moves the FMR by 2.2%. A setting can look better just by luck.

To guard against that, every change was also tested against all 2,880 impostor pairs (every print of one finger against every print of another). A change was only kept if it helped on both tests and there was a clear reason why.

One example: a binarize window of 11 px instead of 15 px dropped the EER from 13.45% to 11.18% on the 45 pairs. On the 2,880 pairs it made things slightly worse. It was luck, so it was not kept. The final settings score 13.98% on the 2,880-pair test, close to the official 13.45%. That's a good sign the number is honest.

### Why the remaining errors happen

Looking at the genuine pairs with the lowest scores:

- **Small overlap.** Print 103_4 only shows the top of the finger. Print 103_3 shows the middle. They share very little skin, so there are few points to pair.
- **Big rotation and distortion.** The prints of finger 110 are pressed at very different angles and pressures. The core sits in very different places.
- **Core confusion.** Near the core the orientation is unreliable. Gabor can draw wrong ridges there and create fake minutiae.

## Glossary

- **Bifurcation:** a ridge that splits into two.
- **Convolution:** sliding a small grid of weights over an image, multiplying and adding at every spot.
- **Core:** the center of a loop or whorl.
- **Crossing number:** the number of 0 to 1 steps around a pixel's 8 neighbors.
- **Delta:** a triangle-shaped area where ridges flow in three directions.
- **EER:** equal error rate, where false matches and false non-matches are equally common.
- **Gradient:** the direction and strength of the fastest brightness change.
- **Minutia (plural minutiae):** a ridge ending or bifurcation.
- **Skeleton:** the ridges thinned to 1 pixel wide.
- **Template:** the list of minutiae for one print.

## References

- Hong, Wan and Jain (1998). *Fingerprint Image Enhancement: Algorithm and Performance Evaluation.* IEEE TPAMI.
- Zhang and Suen (1984). *A Fast Parallel Algorithm for Thinning Digital Patterns.* Communications of the ACM.
- Ratha, Karu, Chen and Jain (1996). *A Real-Time Matching System for Large Fingerprint Databases.* IEEE TPAMI.
- Maltoni, Maio, Jain and others. *Handbook of Fingerprint Recognition.* Springer.
- FVC2002: http://bias.csr.unibo.it/fvc2002/
