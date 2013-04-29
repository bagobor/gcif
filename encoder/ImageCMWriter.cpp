/*
	Copyright (c) 2013 Game Closure.  All rights reserved.

	Redistribution and use in source and binary forms, with or without
	modification, are permitted provided that the following conditions are met:

	* Redistributions of source code must retain the above copyright notice,
	  this list of conditions and the following disclaimer.
	* Redistributions in binary form must reproduce the above copyright notice,
	  this list of conditions and the following disclaimer in the documentation
	  and/or other materials provided with the distribution.
	* Neither the name of GCIF nor the names of its contributors may be used
	  to endorse or promote products derived from this software without
	  specific prior written permission.

	THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
	AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
	IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
	ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
	LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
	CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
	SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
	INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
	CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
	ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
	POSSIBILITY OF SUCH DAMAGE.
*/

#include "ImageCMWriter.hpp"
#include "../decoder/BitMath.hpp"
#include "../decoder/Filters.hpp"
#include "EntropyEstimator.hpp"
#include "Log.hpp"
#include "ImageLZWriter.hpp"
using namespace cat;

#include <vector>
using namespace std;

#include "../decoder/lz4.h"
#include "lz4hc.h"
#include "Log.hpp"
#include "HuffmanEncoder.hpp"
#include "lodepng.h"

#ifdef CAT_DESYNCH_CHECKS
#define DESYNC(x, y) writer.writeBits(x ^ 12345, 16); writer.writeBits(y ^ 54321, 16);
#define DESYNC_FILTER(x, y) writer.writeBits(x ^ 31337, 16); writer.writeBits(y ^ 31415, 16);
#else
#define DESYNC(x, y)
#define DESYNC_FILTER(x, y)
#endif


static CAT_INLINE int scoreYUV(u8 *yuv) {
	return CHAOS_SCORE[yuv[0]] + CHAOS_SCORE[yuv[1]] + CHAOS_SCORE[yuv[2]];
}

static CAT_INLINE int wrapNeg(u8 p) {
	if (p == 0) {
		return 0;
	} else if (p < 128) {
		return ((p - 1) << 1) | 1;
	} else {
		return (256 - p) << 1;
	}
}


//// ImageCMWriter

void ImageCMWriter::clear() {
	if (_filters) {
		delete []_filters;
		_filters = 0;
	}
	if (_seen_filter) {
		delete []_seen_filter;
		_seen_filter = 0;
	}
	if (_chaos) {
		delete []_chaos;
		_chaos = 0;
	}

	_filter_replacements.clear();
}

int ImageCMWriter::init(int width, int height) {
	if (width < 0 || height < 0) {
		return GCIF_WE_BAD_DIMS;
	}

	_width = width;
	_height = height;

	const int fw = (width + FILTER_ZONE_SIZE_MASK) >> FILTER_ZONE_SIZE_SHIFT;
	const int fh = (height + FILTER_ZONE_SIZE_MASK) >> FILTER_ZONE_SIZE_SHIFT;

	const int filters_size = fw * fh;
	if (!_filters || filters_size > _filters_alloc) {
		if (_filters) {
			delete []_filters;
		}
		_filters = new u16[filters_size];
		_filters_alloc = filters_size;
	}

	_filter_stride = fw;

	if (!_seen_filter || fw > _seen_filter_alloc) {
		if (!_seen_filter) {
			delete []_seen_filter;
		}
		_seen_filter = new u8[fw];
		_seen_filter_alloc = fw;
	}

	// And last row of chaos data
	_chaos_size = (width + 1) * COLOR_PLANES;

	if (!_chaos || _chaos_size > _chaos_alloc) {
		if (!_chaos) {
			delete []_chaos;
		}
		_chaos = new u8[_chaos_size];
		_chaos_alloc = _chaos_size;
	}

	return GCIF_WE_OK;
}

void ImageCMWriter::designFilters() {
	// If disabled,
	if (!_knobs->cm_designFilters) {
		CAT_INANE("CM") << "Skipping filter design";
		return;
	}

	/* Inputs: A, B, C, D same as described in Filters.hpp
	 *
	 * PRED = (a*A + b*B + c*C + d*D) / 2
	 * a,b,c,d = {-4, -3, -2, -1, 0, 1, 2, 3, 4}
	 */

	const int width = _width;

	FilterScorer scores;
	const int TAPPED_COUNT = SpatialFilterSet::TAPPED_COUNT;
	scores.init(SF_COUNT + TAPPED_COUNT);

	int bestHist[SF_COUNT + TAPPED_COUNT] = {0};
	u8 FPT[3];

	CAT_INANE("CM") << "Designing filters...";

	for (int y = 0; y < _height; y += FILTER_ZONE_SIZE) {
		for (int x = 0; x < width; x += FILTER_ZONE_SIZE) {
			// If this zone is skipped,
			if (getFilter(x, y) == UNUSED_FILTER) {
				continue;
			}

			scores.reset();

			// For each pixel in the 8x8 zone,
			for (int yy = 0; yy < FILTER_ZONE_SIZE; ++yy) {
				for (int xx = 0; xx < FILTER_ZONE_SIZE; ++xx) {
					int px = x + xx, py = y + yy;
					if (px >= _width || py >= _height) {
						continue;
					}
					if (_mask->masked(px, py)) {
						continue;
					}
					if (_lz->visited(px, py)) {
						continue;
					}

					const u8 *p = _rgba + (px + py * width) * 4;

					int A[3] = {0};
					int B[3] = {0};
					int C[3] = {0};
					int D[3] = {0};

					for (int cc = 0; cc < 3; ++cc) {
						if (px > 0) {
							A[cc] = p[(-1) * 4 + cc];
						}
						if (py > 0 ) {
							B[cc] = p[(-width) * 4 + cc];
							if (px > 0) {
								C[cc] = p[(-width - 1) * 4 + cc];
							}
							if (px < width - 1) {
								D[cc] = p[(-width + 1) * 4 + cc];
							}
						}
					}

					for (int ii = 0; ii < SF_COUNT; ++ii) {
						const u8 *pred = FPT;
						_sf_set.get(ii).safe(p, &pred, px, py, width);

						int sum = 0;

						for (int cc = 0; cc < 3; ++cc) {
							int err = p[cc] - (int)pred[cc];
							if (err < 0) err = -err;
							sum += err;
						}
						scores.add(ii, sum);
					}

					for (int ii = 0; ii < TAPPED_COUNT; ++ii) {
						const int a = SpatialFilterSet::FILTER_TAPS[ii][0];
						const int b = SpatialFilterSet::FILTER_TAPS[ii][1];
						const int c = SpatialFilterSet::FILTER_TAPS[ii][2];
						const int d = SpatialFilterSet::FILTER_TAPS[ii][3];

						int sum = 0;
						for (int cc = 0; cc < 3; ++cc) {
							const int pred = (u8)((a * A[cc] + b * B[cc] + c * C[cc] + d * D[cc]) / 2);
							int err = p[cc] - pred;
							if (err < 0) err = -err;
							sum += err;
						}

						scores.add(ii + SF_COUNT, sum);
					}
				}
			}

			// Super Mario Kart scoring
			FilterScorer::Score *top = scores.getLowest();
			bestHist[top[0].index] += 4;

			top = scores.getTop(4, false);
			bestHist[top[0].index] += 1;
			bestHist[top[1].index] += 1;
			bestHist[top[2].index] += 1;
			bestHist[top[3].index] += 1;
		}
	}

	// Replace filters
	for (int jj = 0; jj < SF_COUNT; ++jj) {
		// Find worst default filter
		int lowest_sf = 0x7fffffffUL, lowest_index = 0;

		for (int ii = 0; ii < SF_COUNT; ++ii) {
			if (bestHist[ii] < lowest_sf) {
				lowest_sf = bestHist[ii];
				lowest_index = ii;
			}
		}

		// Find best custom filter
		int best_tap = -1, highest_index = -1;

		for (int ii = 0; ii < TAPPED_COUNT; ++ii) {
			int score = bestHist[ii + SF_COUNT];

			if (score > best_tap) {
				best_tap = score;
				highest_index = ii;
			}
		}

		// If it not an improvement,
		if (best_tap <= lowest_sf) {
			break;
		}

		// Verify it is good enough to bother with
		double ratio = best_tap / (double)lowest_sf;
		if (ratio < _knobs->cm_minTapQuality) {
			break;
		}

		// Insert it at this location
		const int a = SpatialFilterSet::FILTER_TAPS[highest_index][0];
		const int b = SpatialFilterSet::FILTER_TAPS[highest_index][1];
		const int c = SpatialFilterSet::FILTER_TAPS[highest_index][2];
		const int d = SpatialFilterSet::FILTER_TAPS[highest_index][3];

		CAT_INANE("CM") << "Replacing default filter " << lowest_index << " with tapped filter " << highest_index << " that is " << ratio << "x more preferable : PRED = (" << a << "A + " << b << "B + " << c << "C + " << d << "D) / 2";

		_filter_replacements.push_back((lowest_index << 16) | highest_index);

		_sf_set.replace(lowest_index, highest_index);

		// Install grave markers
		bestHist[lowest_index] = 0x7fffffffUL;
		bestHist[highest_index + SF_COUNT] = 0;
	}
}

void ImageCMWriter::decideFilters() {
	EntropyEstimator ee[3];
	ee[0].init();
	ee[1].init();
	ee[2].init();

	FilterScorer scores;
	scores.init(SF_COUNT * CF_COUNT);

	const int width = _width;

	if (!_knobs->cm_disableEntropy) {
		CAT_INANE("CM") << "Scoring filters using " << _knobs->cm_filterSelectFuzz << " entropy-based trials...";
	} else {
		CAT_INANE("CM") << "Scoring filters using L1-norm...";
	}

	int passes = 0;
	int revisitCount = _knobs->cm_revisitCount;
	u8 FPT[3];

	for (;;) {
		for (int y = 0; y < _height; y += FILTER_ZONE_SIZE) {
			for (int x = 0; x < width; x += FILTER_ZONE_SIZE) {
				// If this zone is skipped,
				const u16 filter = getFilter(x, y);
				if (filter == UNUSED_FILTER) {
					continue;
				}

				// Determine best filter combination to use
				int bestSF = 0, bestCF = 0;

				// If we are on the second or later pass,
				if (passes > 0) {
					// If just finished revisiting old zones,
					if (--revisitCount < 0) {
						// Done!
						return;
					}

					bestSF = (u8)(filter >> 8);
					bestCF = (u8)filter;

					u8 codes[3][16];
					int count = 0;

					// For each pixel in the 8x8 zone,
					for (int yy = 0; yy < FILTER_ZONE_SIZE; ++yy) {
						for (int xx = 0; xx < FILTER_ZONE_SIZE; ++xx) {
							int px = x + xx, py = y + yy;
							if (px >= _width || py >= _height) {
								continue;
							}
							if (_mask->masked(px, py)) {
								continue;
							}
							if (_lz->visited(px, py)) {
								continue;
							}

							const u8 *p = _rgba + (px + py * width) * 4;

							const u8 *pred = FPT;
							_sf_set.get(bestSF).safe(p, &pred, px, py, width);
							u8 temp[3];
							for (int jj = 0; jj < 3; ++jj) {
								temp[jj] = p[jj] - pred[jj];
							}

							u8 yuv[3];
							RGB2YUV_FILTERS[bestCF](temp, yuv);

							codes[0][count] = yuv[0];
							codes[1][count] = yuv[1];
							codes[2][count] = yuv[2];
							++count;
						}
					}

					// Subtract old choice back out
					ee[0].subtract(codes[0], count);
					ee[1].subtract(codes[1], count);
					ee[2].subtract(codes[2], count);
				}

				scores.reset();

				// For each pixel in the 8x8 zone,
				for (int yy = 0; yy < FILTER_ZONE_SIZE; ++yy) {
					for (int xx = 0; xx < FILTER_ZONE_SIZE; ++xx) {
						int px = x + xx, py = y + yy;
						if (px >= _width || py >= _height) {
							continue;
						}
						if (_mask->masked(px, py)) {
							continue;
						}
						if (_lz->visited(px, py)) {
							continue;
						}

						const u8 *p = _rgba + (px + py * width) * 4;

						for (int ii = 0; ii < SF_COUNT; ++ii) {
							const u8 *pred = FPT;
							_sf_set.get(ii).safe(p, &pred, px, py, width);
							u8 temp[3];
							for (int jj = 0; jj < 3; ++jj) {
								temp[jj] = p[jj] - pred[jj];
							}

							for (int jj = 0; jj < CF_COUNT; ++jj) {
								u8 yuv[3];
								RGB2YUV_FILTERS[jj](temp, yuv);

								int error = scoreYUV(yuv);

								scores.add(ii + SF_COUNT*jj, error);
							}
						}
					}
				}

				FilterScorer::Score *lowest = scores.getLowest();

				if (_knobs->cm_disableEntropy ||
						lowest->score <= _knobs->cm_maxEntropySkip) {
					bestSF = lowest->index % SF_COUNT;
					bestCF = lowest->index / SF_COUNT;

					if (!_knobs->cm_disableEntropy) {
						u8 codes[3][16];
						int count = 0;

						// Record this choice
						for (int yy = 0; yy < FILTER_ZONE_SIZE; ++yy) {
							for (int xx = 0; xx < FILTER_ZONE_SIZE; ++xx) {
								int px = x + xx, py = y + yy;
								if (px >= _width || py >= _height) {
									continue;
								}
								if (_mask->masked(px, py)) {
									continue;
								}
								if (_lz->visited(px, py)) {
									continue;
								}

								const u8 *p = _rgba + (px + py * width) * 4;
								const u8 *pred = FPT;
								_sf_set.get(bestSF).safe(p, &pred, px, py, width);
								u8 temp[3];
								for (int jj = 0; jj < 3; ++jj) {
									temp[jj] = p[jj] - pred[jj];
								}

								u8 yuv[3];
								RGB2YUV_FILTERS[bestCF](temp, yuv);

								codes[0][count] = yuv[0];
								codes[1][count] = yuv[1];
								codes[2][count] = yuv[2];
								++count;
							}
						}

						ee[0].add(codes[0], count);
						ee[1].add(codes[1], count);
						ee[2].add(codes[2], count);
					}
				} else {
					const int TOP_COUNT = _knobs->cm_filterSelectFuzz;

					FilterScorer::Score *top = scores.getTop(TOP_COUNT, _knobs->cm_sortFilters);

					u32 best_entropy = 0x7fffffff; // lower = better
					u8 best_codes[3][16];
					int best_count;

					for (int ii = 0; ii < TOP_COUNT; ++ii) {
						const int index = top[ii].index;
						u8 sf = index % SF_COUNT;
						u8 cf = index / SF_COUNT;

						u8 codes[3][16];
						int count = 0;

						for (int yy = 0; yy < FILTER_ZONE_SIZE; ++yy) {
							for (int xx = 0; xx < FILTER_ZONE_SIZE; ++xx) {
								int px = x + xx, py = y + yy;
								if (px >= _width || py >= _height) {
									continue;
								}
								if (_mask->masked(px, py)) {
									continue;
								}
								if (_lz->visited(px, py)) {
									continue;
								}

								const u8 *p = _rgba + (px + py * width) * 4;
								const u8 *pred = FPT;
								_sf_set.get(sf).safe(p, &pred, px, py, width);
								u8 temp[3];
								for (int jj = 0; jj < 3; ++jj) {
									temp[jj] = p[jj] - pred[jj];
								}

								u8 yuv[3];
								RGB2YUV_FILTERS[cf](temp, yuv);

								codes[0][count] = yuv[0];
								codes[1][count] = yuv[1];
								codes[2][count] = yuv[2];
								++count;
							}
						}

						u32 entropy = ee[0].entropy(codes[0], count)
							+ ee[1].entropy(codes[1], count)
							+ ee[2].entropy(codes[2], count);

						if (best_entropy > entropy) {
							best_entropy = entropy;
							memcpy(best_codes, codes, sizeof(best_codes));
							best_count = count;

							bestSF = sf;
							bestCF = cf;
						}
					}

					ee[0].add(best_codes[0], best_count);
					ee[1].add(best_codes[1], best_count);
					ee[2].add(best_codes[2], best_count);
				}

				// Set filter for this zone
				setFilter(x, y, ((u16)bestSF << 8) | bestCF);
			}
		}

		// After good statistics are collected, revisit the first few zones
		if (revisitCount <= 0) {
			// Exit now!
			return;
		}

		if (passes < 4) {
			CAT_INANE("CM") << "Revisiting filter selections from the top... " << revisitCount << " left";
		}
		++passes;
	}
}

void ImageCMWriter::scanlineLZ() {

	//CAT_INANE("CM") << "Comparing performance with ScanlineLZ...";

	/*
	 * For each filter zone set of scanlines there are two options:
	 * + Use chosen zone filters as normal (good for natural images)
	 * + Choose a new filter for each scanline + do LZ (synthetic images)
	 *
	 * ScanlineLZ is coded differently:
	 *
	 * An escape code is used in place of the first nonzero CF selection
	 * to indicate that the following four symbols are the filters for the
	 * next 4 scanlines.
	 *
	 * During these scanlines, the encoding is changed.  This encoding
	 * is expected to be used only when LZ is applicable to the data after
	 * filtering, so the LZ field sizes are heuristic to work even better
	 * when it works well.
	 *
	 * Based on the LZ4 frame, the ScanlineLZ frame is:
	 *
	 * <literal count(4)>
	 * <match count(4)>
	 * [extended literal count (8+)]
	 * [literal pixels]
	 * [extended match count (8+)]
	 * <match offset(16)>
	 *
	 * (counts and offsets are in pixels)
	 *
	 * Since the decoder needs to write out the post-filter values to a
	 * small circular buffer anyway to calculate the chaos metric, this
	 * same circular buffer can be easily adapted as the history used for
	 * LZ matches.
	 *
	 * This format gets RLE for free.
	 */
/*
	CAT_CLR(_chaos, _chaos_size);

	for (int fy = 0; fy < _height; fy += FILTER_ZONE_SIZE) {
		u8 *last = lastStart;
		int base_bits = 0;

		// Simulate writes to get a baseline for what we want to improve on
		for (int iy = 0; iy < FILTER_ZONE_SIZE; ++iy) {
			const int y = fy + iy;
			last = lastStart;

			// Zero left
			last[0 - 4] = 0;
			last[1 - 4] = 0;
			last[2 - 4] = 0;
			last[3 - 4] = 0;

			// For each pixel,
			for (int x = 0; x < width; ++x) {
				// If not masked out,
				if (!_lz->visited(x, y) && !_mask->masked(x, y)) {
					// Get filter for this pixel
					const u16 filter = getFilter(x, y);
					const u8 cf = (u8)filter;
					const u8 sf = (u8)(filter >> 8);

					// Apply spatial filter
					const u8 *pred = SPATIAL_FILTERS[sf](p, x, y, width);
					u8 temp[3];
					for (int jj = 0; jj < 3; ++jj) {
						temp[jj] = p[jj] - pred[jj];
					}

					// Apply color filter
					u8 yuv[COLOR_PLANES];
					RGB2YUV_FILTERS[cf](temp, yuv);
					if (x > 0) {
						yuv[3] = p[-1] - p[3];
					} else {
						yuv[3] = 255 - p[3];
					}

					u8 chaos = CHAOS_TABLE[chaosScore(last[0 - 4]) + chaosScore(last[0])];
					base_bits += _y_encoder[chaos].simulate(yuv[0]);
					chaos = CHAOS_TABLE[chaosScore(last[1 - 4]) + chaosScore(last[1])];
					base_bits += _u_encoder[chaos].simulate(yuv[1]);
					chaos = CHAOS_TABLE[chaosScore(last[2 - 4]) + chaosScore(last[2])];
					base_bits += _v_encoder[chaos].simulate(yuv[2]);
					chaos = CHAOS_TABLE[chaosScore(last[3 - 4]) + chaosScore(last[3])];
					base_bits += _a_encoder[chaos].simulate(yuv[3]);

					for (int c = 0; c < COLOR_PLANES; ++c) {
						last[c] = yuv[c];
					}
				} else {
					for (int c = 0; c < COLOR_PLANES; ++c) {
						last[c] = 0;
					}
				}

				// Next pixel
				last += COLOR_PLANES;
				p += 4;
			}
		}

		// base_bits is now the target we need to beat

		// For each scanline to encode,
		for (int iy = 0; iy < FILTER_ZONE_SIZE; ++iy) {
			const int y = fy + iy;

			// For each filter option,
			for (int sf = 0; sf < SF_COUNT; ++sf) {
				for (int cf = 0; cf < CF_COUNT; ++cf) {
				}
			}
		}
	}*/
}


void ImageCMWriter::maskFilters() {
	// For each zone,
	for (int y = 0, height = _height; y < height; y += FILTER_ZONE_SIZE) {
		for (int x = 0, width = _width; x < width; x += FILTER_ZONE_SIZE) {
			bool on = true;

			for (int ii = 0; ii < FILTER_ZONE_SIZE; ++ii) {
				for (int jj = 0; jj < FILTER_ZONE_SIZE; ++jj) {
					int px = x + ii, py = y + jj;
					if (px >= _width || py >= _height) {
						continue;
					}
					if (!_mask->masked(px, py) &&
						!_lz->visited(px, py)) {
						on = false;
						ii = FILTER_ZONE_SIZE;
						break;
					}
				}
			}

			if (on) {
				setFilter(x, y, UNUSED_FILTER);
			} else {
				setFilter(x, y, TODO_FILTER);
			}
		}
	}
}

bool ImageCMWriter::applyFilters() {
	FreqHistogram<SF_COUNT> sf_hist;
	FreqHistogram<CF_COUNT> cf_hist;

	// For each zone,
	for (int y = 0, height = _height; y < height; y += FILTER_ZONE_SIZE) {
		for (int x = 0, width = _width; x < width; x += FILTER_ZONE_SIZE) {
			// Read filter
			u16 filter = getFilter(x, y);
			if (filter != UNUSED_FILTER) {
				u8 cf = (u8)filter;
				u8 sf = (u8)(filter >> 8);

				sf_hist.add(sf);
				cf_hist.add(cf);
			}
		}
	}

	// Geneerate huffman codes from final histogram
	if (!_cf_encoder.init(cf_hist)) {
		return false;
	}
	if (!_sf_encoder.init(sf_hist)) {
		return false;
	}

	return true;
}



//#define TEST_COLOR_FILTERS

#ifdef TEST_COLOR_FILTERS

#include <iostream>
using namespace std;

const char *cat::GetColorFilterString(int cf) {
	switch (cf) {
		case CF_YUVr:	// YUVr from JPEG2000
			return "YUVr";

		case CF_E2_R:	// derived from E2 and YCgCo-R
			 return "E2-R";

		case CF_D8:		// from the Strutz paper
			 return "D8";
		case CF_D9:		// from the Strutz paper
			 return "D9";
		case CF_D14:		// from the Strutz paper
			 return "D14";

		case CF_D10:		// from the Strutz paper
			 return "D10";
		case CF_D11:		// from the Strutz paper
			 return "D11";
		case CF_D12:		// from the Strutz paper
			 return "D12";
		case CF_D18:		// from the Strutz paper
			 return "D18";

		case CF_YCgCo_R:	// Malvar's YCgCo-R
			 return "YCgCo-R";

		case CF_GB_RG:	// from BCIF
			 return "BCIF-GB-RG";
		case CF_GB_RB:	// from BCIF
			 return "BCIF-GB-RB";
		case CF_GR_BR:	// from BCIF
			 return "BCIF-GR-BR";
		case CF_GR_BG:	// from BCIF
			 return "BCIF-GR-BG";
		case CF_BG_RG:	// from BCIF (recommendation from LOCO-I paper)
			 return "BCIF-BG-RG";

		case CF_B_GR_R:		// RGB -> B, G-R, R
			 return "B_GR_R";
	}

	return "Unknown";
}

void cat::testColorFilters() {
	for (int cf = 0; cf < CF_COUNT; ++cf) {
		for (int r = 0; r < 256; ++r) {
			for (int g = 0; g < 256; ++g) {
				for (int b = 0; b < 256; ++b) {
					u8 yuv[3];
					u8 rgb[3] = {r, g, b};
					RGB2YUV_FILTERS[cf](rgb, yuv);
					u8 rgb2[3];
					YUV2RGB_FILTERS[cf](yuv, rgb2);

					if (rgb2[0] != r || rgb2[1] != g || rgb2[2] != b) {
						cout << "Color filter " << GetColorFilterString(cf) << " is lossy for " << r << "," << g << "," << b << " -> " << (int)rgb2[0] << "," << (int)rgb2[1] << "," << (int)rgb2[2] << endl;
						goto nextcf;
					}
				}
			}
		}

		cout << "Color filter " << GetColorFilterString(cf) << " is reversible with YUV888!" << endl;
nextcf:
		;
	}
}

#endif // TEST_COLOR_FILTERS



//#define GENERATE_CHAOS_TABLE

#ifdef GENERATE_CHAOS_TABLE

static int CalculateChaos(int sum) {
	if (sum <= 0) {
		return 0;
	} else {
		int chaos = BSR32(sum - 1) + 1;
		if (chaos > 7) {
			chaos = 7;
		}
		return chaos;
	}
}

#include <iostream>
#include <iomanip>
using namespace std;

void GenerateChaosTable() {
	cout << "const u8 cat::CHAOS_TABLE[512] = {";

	for (int sum = 0; sum < 256*2; ++sum) {
		if ((sum & 31) == 0) {
			cout << endl << '\t';
		}
		cout << CalculateChaos(sum) << ",";
	}

	cout << endl << "};" << endl;

	cout << "const u8 cat::CHAOS_SCORE[256] = {";

	for (int sum = 0; sum < 256; ++sum) {
		if ((sum & 15) == 0) {
			cout << endl << '\t';
		}
		int score = sum;
		if (score >= 128) {
			score = 256 - score;
		}
		cout << "0x" << setw(2) << setfill('0') << hex << score << ",";
	}

	cout << endl << "};" << endl;
}

#endif // GENERATE_CHAOS_TABLE

void ImageCMWriter::chaosStats() {
#ifdef GENERATE_CHAOS_TABLE
	GenerateChaosTable();
#endif

	// Find number of pixels to encode
	int chaos_count = 0;
	for (int y = 0; y < _height; ++y) {
		for (int x = 0; x < _width; ++x) {
			if (!_lz->visited(x, y) && !_mask->masked(x, y)) {
				++chaos_count;
			}
		}
	}

#ifdef CAT_COLLECT_STATS
	Stats.chaos_count = chaos_count;
#endif

	// If it is above a threshold,
	if (chaos_count >= _knobs->cm_chaosThresh) {
		CAT_DEBUG_ENFORCE(CHAOS_LEVELS_MAX == 8);

		// Use more chaos levels for better compression
		_chaos_levels = CHAOS_LEVELS_MAX;
		_chaos_table = CHAOS_TABLE_8;
	} else {
		_chaos_levels = 1;
		_chaos_table = CHAOS_TABLE_1;
	}

	const int width = _width;

	// For each scanline,
	const u8 *p = _rgba;
	u8 *lastStart = _chaos + COLOR_PLANES;
	CAT_CLR(_chaos, _chaos_size);

	const u8 *CHAOS_TABLE = _chaos_table;
	u8 FPT[3];

	for (int y = 0; y < _height; ++y) {
		u8 *last = lastStart;

		// Zero left
		last[0 - 4] = 0;
		last[1 - 4] = 0;
		last[2 - 4] = 0;
		last[3 - 4] = 0;

		// For each pixel,
		for (int x = 0; x < width; ++x) {
			// If not masked out,
			if (!_lz->visited(x, y) && !_mask->masked(x, y)) {
				// Get filter for this pixel
				const u16 filter = getFilter(x, y);
				const u8 cf = (u8)filter;
				const u8 sf = (u8)(filter >> 8);

				// Apply spatial filter
				const u8 *pred = FPT;
				_sf_set.get(sf).safe(p, &pred, x, y, width);
				u8 temp[3];
				for (int jj = 0; jj < 3; ++jj) {
					temp[jj] = p[jj] - pred[jj];
				}

				// Apply color filter
				u8 yuv[COLOR_PLANES];
				RGB2YUV_FILTERS[cf](temp, yuv);
				if (x > 0) {
					yuv[3] = p[-1] - p[3];
				} else {
					yuv[3] = 255 - p[3];
				}

				u8 chaos = CHAOS_TABLE[CHAOS_SCORE[last[0 - 4]] + CHAOS_SCORE[last[0]]];
				_y_encoder[chaos].add(yuv[0]);
				chaos = CHAOS_TABLE[CHAOS_SCORE[last[1 - 4]] + CHAOS_SCORE[last[1]]];
				_u_encoder[chaos].add(yuv[1]);
				chaos = CHAOS_TABLE[CHAOS_SCORE[last[2 - 4]] + CHAOS_SCORE[last[2]]];
				_v_encoder[chaos].add(yuv[2]);
				chaos = CHAOS_TABLE[CHAOS_SCORE[last[3 - 4]] + CHAOS_SCORE[last[3]]];
				_a_encoder[chaos].add(yuv[3]);

				for (int c = 0; c < COLOR_PLANES; ++c) {
					last[c] = yuv[c];
				}
			} else {
				for (int c = 0; c < COLOR_PLANES; ++c) {
					last[c] = 0;
				}
			}

			// Next pixel
			last += COLOR_PLANES;
			p += 4;
		}
	}

	// Finalize
	for (int jj = 0; jj < _chaos_levels; ++jj) {
		_y_encoder[jj].finalize();
		_u_encoder[jj].finalize();
		_v_encoder[jj].finalize();
		_a_encoder[jj].finalize();
	}
}

int ImageCMWriter::initFromRGBA(const u8 *rgba, int width, int height, ImageMaskWriter &mask, ImageLZWriter &lz, const GCIFKnobs *knobs) {
	int err;

	if ((err = init(width, height))) {
		return err;
	}

	if ((!knobs->cm_disableEntropy && knobs->cm_filterSelectFuzz <= 0)) {
		return GCIF_WE_BAD_PARAMS;
	}

	_knobs = knobs;
	_rgba = rgba;
	_mask = &mask;
	_lz = &lz;

#ifdef TEST_COLOR_FILTERS
	testColorFilters();
	return -1;
#endif

	maskFilters();

	designFilters();

	decideFilters();

	if (_knobs->cm_scanlineFilters) {
		scanlineLZ();
	}

	if (!applyFilters()) {
		return GCIF_WE_BUG;
	}

	chaosStats();

	return GCIF_WE_OK;
}

bool ImageCMWriter::writeFilters(ImageWriter &writer) {
	const int rep_count = static_cast<int>( _filter_replacements.size() );

	CAT_DEBUG_ENFORCE(SF_COUNT < 32);
	CAT_DEBUG_ENFORCE(SpatialFilterSet::TAPPED_COUNT < 128);

	writer.writeBits(rep_count, 5);
	int bits = 5;

	for (int ii = 0; ii < rep_count; ++ii) {
		u32 filter = _filter_replacements[ii];

		u16 def = (u16)(filter >> 16);
		u16 cust = (u16)filter;

		writer.writeBits(def, 5);
		writer.writeBits(cust, 7);
		bits += 12;
	}

	// Write out filter huffman tables
	int cf_table_bits = _cf_encoder.writeTable(writer);
	int sf_table_bits = _sf_encoder.writeTable(writer);

#ifdef CAT_COLLECT_STATS
	Stats.filter_table_bits[0] = sf_table_bits + bits;
	Stats.filter_table_bits[1] = cf_table_bits;
#endif // CAT_COLLECT_STATS

	return true;
}

bool ImageCMWriter::writeChaos(ImageWriter &writer) {
#ifdef CAT_COLLECT_STATS
	int overhead_bits = 0;
	int bitcount[COLOR_PLANES] = {0};
	int filter_table_bits[2] = {0};
#endif

	CAT_DEBUG_ENFORCE(_chaos_levels <= 8);

	writer.writeBits(_chaos_levels - 1, 3);

	int bits = 3;

	for (int jj = 0; jj < _chaos_levels; ++jj) {
		bits += _y_encoder[jj].writeTables(writer);
		bits += _u_encoder[jj].writeTables(writer);
		bits += _v_encoder[jj].writeTables(writer);
		bits += _a_encoder[jj].writeTables(writer);
	}
#ifdef CAT_COLLECT_STATS
	overhead_bits += bits;
#endif

	const int width = _width;

	// For each scanline,
	const u8 *p = _rgba;
	u8 *lastStart = _chaos + COLOR_PLANES;
	CAT_CLR(_chaos, _chaos_size);

	const u8 *CHAOS_TABLE = _chaos_table;
	u8 FPT[3];

	for (int y = 0; y < _height; ++y) {
		u8 *last = lastStart;

		// Zero left
		last[0 - 4] = 0;
		last[1 - 4] = 0;
		last[2 - 4] = 0;
		last[3 - 4] = 0;

		// If it is time to clear the seen filters,
		if ((y & FILTER_ZONE_SIZE_MASK) == 0) {
			CAT_CLR(_seen_filter, _filter_stride);
		}

		// For each pixel,
		for (int x = 0; x < width; ++x) {
			DESYNC(x, y);

			// If not masked out,
			if (!_lz->visited(x, y) && !_mask->masked(x, y)) {
				// Get filter for this pixel
				u16 filter = getFilter(x, y);
				CAT_DEBUG_ENFORCE(filter != UNUSED_FILTER);
				u8 cf = (u8)filter;
				u8 sf = (u8)(filter >> 8);

				// If it is time to write out the filter information,
				if (!_seen_filter[x >> FILTER_ZONE_SIZE_SHIFT]) {
					_seen_filter[x >> FILTER_ZONE_SIZE_SHIFT] = true;

					int cf_bits = _cf_encoder.writeSymbol(cf, writer);
					DESYNC_FILTER(x, y);
					int sf_bits = _sf_encoder.writeSymbol(sf, writer);
					DESYNC_FILTER(x, y);

#ifdef CAT_COLLECT_STATS
					filter_table_bits[0] += sf_bits;
					filter_table_bits[1] += cf_bits;
#endif
				}

				// Apply spatial filter
				const u8 *pred = FPT;
				_sf_set.get(sf).safe(p, &pred, x, y, width);
				u8 temp[3];
				for (int jj = 0; jj < 3; ++jj) {
					temp[jj] = p[jj] - pred[jj];
				}

				// Apply color filter
				u8 YUVA[COLOR_PLANES];
				RGB2YUV_FILTERS[cf](temp, YUVA);
				if (x > 0) {
					YUVA[3] = p[-1] - p[3];
				} else {
					YUVA[3] = 255 - p[3];
				}

				u8 chaos = CHAOS_TABLE[CHAOS_SCORE[last[0 - 4]] + CHAOS_SCORE[last[0]]];

				int bits = _y_encoder[chaos].write(YUVA[0], writer);
				DESYNC(x, y);
#ifdef CAT_COLLECT_STATS
				bitcount[0] += bits;
#endif
				chaos = CHAOS_TABLE[CHAOS_SCORE[last[1 - 4]] + CHAOS_SCORE[last[1]]];
				bits = _u_encoder[chaos].write(YUVA[1], writer);
				DESYNC(x, y);
#ifdef CAT_COLLECT_STATS
				bitcount[1] += bits;
#endif
				chaos = CHAOS_TABLE[CHAOS_SCORE[last[2 - 4]] + CHAOS_SCORE[last[2]]];
				bits = _v_encoder[chaos].write(YUVA[2], writer);
				DESYNC(x, y);
#ifdef CAT_COLLECT_STATS
				bitcount[2] += bits;
#endif
				chaos = CHAOS_TABLE[CHAOS_SCORE[last[3 - 4]] + CHAOS_SCORE[last[3]]];
				bits = _a_encoder[chaos].write(YUVA[3], writer);
				DESYNC(x, y);
#ifdef CAT_COLLECT_STATS
				bitcount[3] += bits;
#endif

				for (int c = 0; c < COLOR_PLANES; ++c) {
					last[c] = YUVA[c];
				}
			} else {
				for (int c = 0; c < COLOR_PLANES; ++c) {
					last[c] = 0;
				}
			}

			// Next pixel
			last += COLOR_PLANES;
			p += 4;
		}
	}

#ifdef CAT_COLLECT_STATS
	for (int ii = 0; ii < COLOR_PLANES; ++ii) {
		Stats.rgb_bits[ii] = bitcount[ii];
	}
	Stats.chaos_overhead_bits = overhead_bits;
	Stats.filter_compressed_bits[0] = filter_table_bits[0];
	Stats.filter_compressed_bits[1] = filter_table_bits[1];
#endif

	return true;
}

void ImageCMWriter::write(ImageWriter &writer) {
	CAT_INANE("CM") << "Writing encoded pixel data...";

	writeFilters(writer);

	writeChaos(writer);

#ifdef CAT_COLLECT_STATS
	int total = 0;
	for (int ii = 0; ii < 2; ++ii) {
		total += Stats.filter_table_bits[ii];
		total += Stats.filter_compressed_bits[ii];
	}
	for (int ii = 0; ii < COLOR_PLANES; ++ii) {
		total += Stats.rgb_bits[ii];
	}
	total += Stats.chaos_overhead_bits;
	Stats.chaos_bits = total;
	total += _lz->Stats.huff_bits;
	total += _mask->Stats.compressedDataBits;
	Stats.total_bits = total;

	Stats.overall_compression_ratio = _width * _height * 4 * 8 / (double)Stats.total_bits;

	Stats.chaos_compression_ratio = Stats.chaos_count * COLOR_PLANES * 8 / (double)Stats.chaos_bits;
#endif
}

#ifdef CAT_COLLECT_STATS

bool ImageCMWriter::dumpStats() {
	CAT_INANE("stats") << "(CM Compress) Spatial Filter Table Size : " <<  Stats.filter_table_bits[0] << " bits (" << Stats.filter_table_bits[0]/8 << " bytes)";
	CAT_INANE("stats") << "(CM Compress) Spatial Filter Compressed Size : " <<  Stats.filter_compressed_bits[0] << " bits (" << Stats.filter_compressed_bits[0]/8 << " bytes)";

	CAT_INANE("stats") << "(CM Compress) Color Filter Table Size : " <<  Stats.filter_table_bits[1] << " bits (" << Stats.filter_table_bits[1]/8 << " bytes)";
	CAT_INANE("stats") << "(CM Compress) Color Filter Compressed Size : " <<  Stats.filter_compressed_bits[1] << " bits (" << Stats.filter_compressed_bits[1]/8 << " bytes)";

	CAT_INANE("stats") << "(CM Compress) Y-Channel Compressed Size : " <<  Stats.rgb_bits[0] << " bits (" << Stats.rgb_bits[0]/8 << " bytes)";
	CAT_INANE("stats") << "(CM Compress) U-Channel Compressed Size : " <<  Stats.rgb_bits[1] << " bits (" << Stats.rgb_bits[1]/8 << " bytes)";
	CAT_INANE("stats") << "(CM Compress) V-Channel Compressed Size : " <<  Stats.rgb_bits[2] << " bits (" << Stats.rgb_bits[2]/8 << " bytes)";
	CAT_INANE("stats") << "(CM Compress) A-Channel Compressed Size : " <<  Stats.rgb_bits[3] << " bits (" << Stats.rgb_bits[3]/8 << " bytes)";

	CAT_INANE("stats") << "(CM Compress) YUVA Overhead Size : " << Stats.chaos_overhead_bits << " bits (" << Stats.chaos_overhead_bits/8 << " bytes)";
	CAT_INANE("stats") << "(CM Compress) Chaos pixel count : " << Stats.chaos_count << " pixels";
	CAT_INANE("stats") << "(CM Compress) Chaos compression ratio : " << Stats.chaos_compression_ratio << ":1";
	CAT_INANE("stats") << "(CM Compress) Overall size : " << Stats.total_bits << " bits (" << Stats.total_bits/8 << " bytes)";
	CAT_INANE("stats") << "(CM Compress) Overall compression ratio : " << Stats.overall_compression_ratio << ":1";

	return true;
}

#endif
