# fpmatch briefing

How fpmatch works, one stage at a time. It's written to learn from and to teach from.

Everything here is classic image processing, signal processing or geometry. There is no machine learning, no trained model and no network connection. Every number the program uses is written in the source code.

## The big idea

A fingerprint is a pattern of ridges. Examiners don't compare whole pictures. They compare **minutiae**, the places where a ridge does something special. Each minutia has a position (x, y), a direction (an angle) and a type. Two prints of the same finger share many minutiae in the same layout. Prints of different fingers don't.

fpmatch does three jobs:

1. **Enhance** the picture so the ridges are clean.
2. **Extract** the minutiae into a list called a template.
3. **Match** two templates by lining them up and counting the points that agree.

Every stage saves a picture in `out/<print name>/` so you can see what it did.

## Minutia types

| Type | What it is | How it's found |
| --- | --- | --- |
| Ending | a ridge stops | crossing number 1 |
| Bifurcation | a ridge splits in two | crossing number 3 |
| Dot | a ridge so short it's just a spot | crossing number 0, or a tiny island alone in a valley |
| Crossover | a short ridge joins two ridges, or two ridges cross | crossing number 4, or two bifurcations joined across the ridges |
| Short ridge | a short piece of ridge on its own (an island) | two endings joined by 16 to 40 px of ridge |
| Spur | a short branch off a ridge (a hook) | an ending joined to a bifurcation by 13 to 25 px |
| Lake | a ridge splits and joins back up (an enclosure) | two bifurcations joined by two separate paths |

Endings and bifurcations are the basic types. The rest are made of them, so a lake is also two bifurcations and a short ridge is also two endings. Both the shape and its parts go in the template.

Only endings and bifurcations are used for matching. The section on overfitting below explains why.

## Test data

All numbers come from FVC2002 DB1_B, a public set from the Fingerprint Verification Competition. It has 10 fingers with 8 impressions each, 388 x 374 pixels at 500 dpi. Our copy is missing 4 impressions of finger 104, so it has 76 prints.

At 500 dpi one ridge plus one valley is about 9 pixels wide. Many tuning numbers are based on that.

---

## Part 1: Enhance

### Load and normalize

Reads the image as gray values from 0 to 255 (`00_raw.png`). Then shifts and scales them so the average is 0 and the spread (standard deviation) is 1 (`01_normalized.png`). A dark scan and a light scan look the same after this, so one set of settings works on both.

The sums use a `double`, because a `float` loses precision over 145,000 additions.

**Weak spot.** It treats the whole image the same. A print that's dark on one side stays uneven. Binarize handles that later with a local threshold.

### Mask

Decides which parts are finger and which are background (`02_mask.png`, black is finger). Background noise would turn into fake minutiae.

1. Split the image into 16 x 16 blocks.
2. Ridges make a block swing between dark and light, so finger blocks have high variance. Background is flat. Blocks above 0.1 are finger.
3. Clean up: a block stays finger only if 5 of the 9 blocks around it are, then keep only the biggest blob, then fill any holes.

The flood fill uses a to-do list of pixels, not recursion. Recursion on a big area would run out of stack memory.

**Weak spot.** The edge is blocky, 16 px at a time. A faint part of a real finger can be marked as background.

### Orientation

Works out which way the ridges run in each block (`03_orientation.png`, red lines). Gabor needs this.

1. At every pixel, measure the **gradient** with Sobel weights. It points across the ridge, where brightness changes fastest.
2. Average the gradients over each block. The catch: gradients on the two sides of a ridge point opposite ways and would cancel out.
3. **The fix is to double every angle.** Then 0° and 180° both become 0°, so they add up instead of cancelling.
4. Smooth with the 3 x 3 neighbors, twice, so one bad block gets outvoted.
5. Halve the angle and turn it 90° to get the ridge direction.

**Weak spot.** Near the **core** (the center of a loop or whorl) and **deltas** the direction changes too fast for one angle per block.

### Frequency

Measures how far apart the ridges are in each block (`04_frequency.png`, brighter means closer). Gabor has to be tuned to the spacing, like tuning a radio. This is the x-signature method from Hong, Wan and Jain (1998).

1. Take a 32 x 16 window turned so its long side runs across the ridges.
2. Average each row along the ridge. The 32 numbers make a wave that dips at every ridge.
3. The average gap between dips is the ridge **period**. Frequency is 1 / period.
4. Periods outside 3 to 25 px aren't real. Those blocks borrow the average of their neighbors.

On 101_1 the average period is 9.3 px, which is right for 500 dpi.

### Gabor filter

Cleans the ridges: joins breaks, fills pores and fades scratches (`05_enhanced.png`). Every break would otherwise become two fake endings. This is the biggest single improvement in the project.

1. A **Gabor kernel** is a small picture of a few parallel ridges: a wave faded out by a soft round blob.
2. Turn it to the block's angle and stretch it to the block's spacing.
3. Lay it over each pixel, multiply and add (**convolution**). Real ridges at that angle and spacing give a big answer. Noise gives a small one.
4. Angles are rounded to 16 steps and periods to whole pixels, so about 40 kernels get built once and reused.

**Tuning.** Sigma 4.0 (Hong). 16 angles.

**Weak spot.** It trusts the orientation. Where that's wrong (the core, scars) it draws confident ridges in the wrong direction. It's also the slowest stage.

---

## Part 2: Extract

### Binarize

Every pixel becomes ridge (1) or valley (0) (`06_binary.png`). A pixel darker than the average of the 15 x 15 square around it is ridge. Then everything outside the mask is wiped.

### Thin

Shrinks every ridge to a line 1 pixel wide (`07_skeleton.png`), using Zhang and Suen (1984).

1. A ridge pixel can be peeled off only if it has 2 to 6 ridge neighbors (not a tip and not buried) and removing it won't split the ridge.
2. Two sub-steps take turns peeling opposite edges, so the line stays centered.
3. Mark first, then delete all at once. Repeat until nothing changes.

**Weak spot.** Bumpy ridge edges leave short fake spurs. The filter cleans most of them up.

### Find minutiae

Finds every point on the skeleton where something happens (`08_minutiae.png`).

1. Walk around a pixel's 8 neighbors and count the steps from 0 to 1. That's the **crossing number**. 0 is a dot, 1 is an ending, 2 is the middle of a ridge, 3 is a bifurcation and 4 is a crossing.
2. **Ending angle.** Walk 10 px along the ridge. The angle points from there back out of the tip.
3. **Bifurcation angle.** Walk 10 px along each branch. The two closest branches are the fork. The angle points away from the third one, the trunk.

Angles are in radians, 0 to 2π. 0 points right and π/2 points up. Image y points down, so every `atan2` flips the y difference. Hand-drawn test shapes confirmed it.

On 101_1 this finds 95 points: 82 endings, 12 bifurcations and 1 dot.

### Filter

Removes points that are probably fake (`09_filtered.png`). Each check only looks at what survived the one before.

1. **Mask edge.** Ridges get cut off at the edge of the finger, so drop anything within 12 px of it.
2. **Spurs, islands and dots.** Walk from each ending. A junction within 12 px is a fake spur. Another ending within 15 px is a fake island. A piece 6 px or less with no other ridge within 3 px is a real **dot**, so it's kept as one.
3. **Broken ridges.** Two endings within 15 px that point at each other are one ridge with a gap. Drop both.
4. **Crowding.** Any two points closer than 6 px are noise. Drop both. If a dot or crossing is crowded against an ending or bifurcation, drop only the dot or crossing. That way the new types never change which endings and bifurcations survive.

On 101_1 this takes 95 points down to 23.

**Weak spot.** Real points near the edge are lost. The distances are fixed, so they don't adapt to unusually wide or narrow ridges.

### Shapes

Finds the bigger shapes made of two filtered points joined by a short ridge (`10_shapes.png`, thick circles).

- **Short ridge.** Walk from an ending. Reach another ending within 40 px.
- **Spur.** Walk from an ending. Reach a bifurcation within 25 px. The fake spurs under 12 px are already gone.
- **Lake.** Walk every branch of a bifurcation. Two branches reach the same other bifurcation within 40 px.
- **Crossover.** One branch reaches another bifurcation within 20 px. At both ends the other two branches have to make a straight line and the bridge has to leave it at 45° or more. That's an H shape. A normal fork (a Y) fails this test.

Short ridges, lakes and crossovers look the same from both ends. Their angle is a line from 0 to π, not an arrow.

On 101_1 this finds 1 spur. Print 108_6 has 12 shapes, including 5 lakes and a crossover. Zooming in on its skeleton showed each one really is that shape.

### Template

Saves the points and the image size as plain text (`template.tmpl`). The header is `fpmatch-template <version> <width> <height> <count>`, then one `x y angle type` line per point. Angles get 17 digits so a save and load gives back exactly the same numbers. A test confirmed it.

---

## Part 3: Match

### Align (Hough voting)

Finds the rotation and shift that puts print A on top of print B, using the method from Ratha et al. (1996).

1. Take every pair: one point from A, one from B.
2. If they were the same point, the rotation is the difference in their angles. Turn A's point by that much. The leftover gap is the shift.
3. Each pair casts one vote in a 3D grid of (rotation, dx, dy) bins. Wrong pairs scatter. Right pairs pile up in one bin.
4. Score each bin with its 3 x 3 x 3 neighborhood, since a real answer can land on a bin edge.
5. Take the top 5 peaks. For each one, average the exact votes near it for a sharper answer.

Only endings vote with endings and bifurcations with bifurcations.

A test turned 101_1 by 30° and shifted it by (10, -5). The aligner found 29.2° and (11.4, -6.5), and every point paired up.

**Weak spot.** It finds one rigid move. Skin stretches, so on a hard press the far points miss.

### Score

1. Move every point of A by the alignment.
2. A and B points pair up if they're within 12 px and 20°. Closest pairs pick first, and each point is used once.
3. Score = matched² / (points in A x points in B). Both prints have to agree, so a print full of noise can't win by luck.
4. Do this for all 5 peaks and keep the best. A score of **0.028** or more is a MATCH.

**Weak spot.** Two prints that share only a small area score low, because the points outside it count against them.

---

## The database

Results wait in `out/<print name>/` until the user picks Catalogue. Catalogue copies the original file, the skeleton and the template into `db/`, adds a row to `db/index.csv` and only then deletes `out/`. If anything fails, `out/` is still there to try again.

Search scores the print against every template and shows the top 5 with the paths to their pictures.

**The software suggests. The examiner decides.** At the threshold, about 1 in 7 impostor pairs still scores as a match.

## Evaluation

`fpmatch eval data` runs the FVC test.

- **Genuine pairs:** every impression against every other impression of the same finger. 258 pairs.
- **Impostor pairs:** the first impression of each finger against the first of every other finger. 45 pairs.
- **FMR:** impostor pairs at or above the threshold. Wrong accusations.
- **FNMR:** genuine pairs below the threshold. Missed matches.
- **EER:** where FMR and FNMR are equal. Lower is better.

| Version | EER |
| --- | --- |
| No enhancement, first matcher | 24.04% |
| With orientation, frequency and Gabor | 15.53% |
| Plus same-type voting and 5 Hough peaks (final) | **13.45%** |

The final matcher without enhancement scores 28.98%. Same-type voting needs the clean types that Gabor gives.

At 0.028, FMR is 13.3% and FNMR is 14.0%. The best systems in FVC2002 got under 1% on the full DB1. 13.45% is good for a from-scratch teaching system.

### A lesson about overfitting

Every setting was tuned on the same 76 prints that give the EER. With only 45 impostor pairs, one pair moves the FMR by 2.2%, so a change can look good by luck. Every change was also tested on all 2,880 impostor pairs. It was only kept if it helped on both and there was a clear reason.

- **Binarize window 11 px.** EER dropped from 13.45% to 11.18% on the 45 pairs but got worse on the 2,880. It was luck, so it wasn't kept.
- **Matching with the shapes.** EER rose from 13.45% to 15.34% (15.10% on the 2,880). A little more pressure turns a lake into two bifurcations or a short ridge into a broken one, so the shapes rarely show up the same way twice. They stay in the template for the examiner but aren't scored. `USE_SHAPES` in `matching.cpp` turns them on to try it.

The final settings score 13.98% on the 2,880-pair test, close to the official number. That's a good sign it's honest.

### Why the remaining errors happen

- **Small overlap.** 103_4 shows only the top of the finger and 103_3 the middle. They share little skin.
- **Rotation and pressure.** The prints of finger 110 are pressed at very different angles.
- **The core.** Orientation is unreliable there, so Gabor can draw wrong ridges and create fake minutiae.

## Glossary

- **Convolution:** sliding a small grid of weights over an image, multiplying and adding at every spot.
- **Core:** the center of a loop or whorl.
- **Crossing number:** the number of 0 to 1 steps around a pixel's 8 neighbors.
- **Delta:** a triangle-shaped area where ridges flow three ways.
- **EER:** equal error rate, where false matches and missed matches are equally common.
- **Gradient:** the direction and strength of the fastest brightness change.
- **Minutia (plural minutiae):** a point where a ridge does something special. See the types table.
- **Skeleton:** the ridges thinned to 1 pixel wide.
- **Template:** the list of minutiae for one print.

## References

- Hong, Wan and Jain (1998). *Fingerprint Image Enhancement: Algorithm and Performance Evaluation.* IEEE TPAMI.
- Zhang and Suen (1984). *A Fast Parallel Algorithm for Thinning Digital Patterns.* Communications of the ACM.
- Ratha, Karu, Chen and Jain (1996). *A Real-Time Matching System for Large Fingerprint Databases.* IEEE TPAMI.
- Maltoni, Maio, Jain and others. *Handbook of Fingerprint Recognition.* Springer.
- FVC2002: http://bias.csr.unibo.it/fvc2002/
