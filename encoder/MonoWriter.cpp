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

#include "MonoWriter.hpp"
#include "../decoder/Enforcer.hpp"
#include "FilterScorer.hpp"
#include "EntropyEstimator.hpp"
#include "../decoder/BitMath.hpp"
using namespace cat;


#ifdef CAT_DESYNCH_CHECKS
#define DESYNC_TABLE() writer.writeWord(1234567);
#define DESYNC(x, y) writer.writeBits(x ^ 12345, 16); writer.writeBits(y ^ 54321, 16);
#else
#define DESYNC_TABLE()
#define DESYNC(x, y)
#endif


//// MonoWriter

void MonoWriter::cleanup() {
	if (_filter_encoder) {
		delete _filter_encoder;
		_filter_encoder = 0;
	}
	if (_best_writer) {
		delete _best_writer;
		_best_writer = 0;
	}
}

void MonoWriter::maskTiles() {
	const u16 tile_size_x = _tile_size_x, tile_size_y = _tile_size_y;
	const u16 size_x = _params.size_x, size_y = _params.size_y;
	u8 *p = _tiles.get();

	// For each tile,
	for (u16 y = 0; y < size_y; y += tile_size_y) {
		for (u16 x = 0; x < size_x; x += tile_size_x) {

			// For each element in the tile,
			u16 py = y, cy = tile_size_y;
			while (cy-- > 0 && py < size_y) {
				u16 px = x, cx = tile_size_x;
				while (cx-- > 0 && px < size_x) {
					// If it is not masked,
					if (!_params.mask(px, py)) {
						// We need to do this tile
						*p++ = TODO_TILE;
						goto next_tile;
					}
					++px;
				}
				++py;
			}

			// Tile is masked out entirely
			*p++ = MASK_TILE;
next_tile:;
		}
	}
}

void MonoWriter::designPaletteFilters() {
	CAT_INANE("2D") << "Designing palette filters for " << _tiles_x << "x" << _tiles_y << "...";

	const u16 tile_size_x = _tile_size_x, tile_size_y = _tile_size_y;
	const u16 size_x = _params.size_x, size_y = _params.size_y;
	u8 *p = _tiles.get();

	u32 hist[MAX_SYMS] = { 0 };

	// For each tile,
	const u8 *topleft_row = _params.data;
	for (u16 y = 0; y < size_y; y += tile_size_y) {
		const u8 *topleft = topleft_row;

		for (u16 x = 0; x < size_x; x += tile_size_x, ++p, topleft += tile_size_x) {
			// If tile is masked,
			if (*p == MASK_TILE) {
				continue;
			}

			bool uniform = true;
			bool seen = false;
			u8 uniform_value = 0;

			// For each element in the tile,
			const u8 *row = topleft;
			u16 py = y, cy = tile_size_y;
			while (cy-- > 0 && py < size_y) {
				const u8 *data = row;
				u16 px = x, cx = tile_size_x;
				while (cx-- > 0 && px < size_x) {
					// If element is not masked,
					if (!_params.mask(px, py)) {
						const u8 value = *data;

						if (!seen) {
							uniform_value = value;
							seen = true;
						} else if (value != uniform_value) {
							uniform = false;
							cy = 0;
							break;
						}
					}
					++px;
					++data;
				}
				++py;
				row += size_x;
			}

			// If uniform data,
			if (uniform) {
				hist[uniform_value]++;
			}
		}

		topleft_row += _params.size_x * _tile_size_y;
	}

	// Determine threshold
	u32 sympal_thresh = _params.sympal_thresh * _tiles_count;
	int sympal_count = 0;

	// For each histogram bin,
	for (int sym = 0, num_syms = _params.num_syms; sym < num_syms; ++sym) {
		u32 coverage = hist[sym];

		// If filter is worth adding,
		if (coverage > sympal_thresh) {
			// Add it
			_sympal[sympal_count++] = (u8)sym;

			CAT_INANE("2D") << " - Added symbol palette filter for symbol " << (int)sym;

			// If we ran out of room,
			if (sympal_count >= MAX_PALETTE) {
				break;
			}
		}
	}

	// Initialize filter map
	for (int ii = 0; ii < sympal_count; ++ii) {
		_sympal_filter_map[ii] = UNUSED_SYMPAL;
	}

	_sympal_filter_count = sympal_count;
}

void MonoWriter::designFilters() {
	CAT_INANE("2D") << "Designing filters for " << _tiles_x << "x" << _tiles_y << "...";

	const u16 tile_size_x = _tile_size_x, tile_size_y = _tile_size_y;
	const u16 size_x = _params.size_x, size_y = _params.size_y;
	const u16 num_syms = _params.num_syms;
	u8 *p = _tiles.get();

	FilterScorer scores, awards;
	scores.init(SF_COUNT + _sympal_filter_count);
	awards.init(SF_COUNT + _sympal_filter_count);
	awards.reset();

	// For each tile,
	const u8 *topleft_row = _params.data;
	for (u16 y = 0; y < size_y; y += tile_size_y) {
		const u8 *topleft = topleft_row;

		for (u16 x = 0; x < size_x; x += tile_size_x, ++p, topleft += tile_size_x) {
			// If tile is masked,
			if (*p == MASK_TILE) {
				continue;
			}

			scores.reset();

			bool uniform = true;
			bool seen = false;
			u8 uniform_value = 0;

			// For each element in the tile,
			const u8 *row = topleft;
			u16 py = y, cy = tile_size_y;
			while (cy-- > 0 && py < size_y) {
				const u8 *data = row;
				u16 px = x, cx = tile_size_x;
				while (cx-- > 0 && px < size_x) {
					// If element is not masked,
					if (!_params.mask(px, py)) {
						const u8 value = *data;

						if (!seen) {
							uniform_value = value;
							seen = true;
						} else if (value != uniform_value) {
							uniform = false;
						}

						for (int f = 0; f < SF_COUNT; ++f) {
							u8 prediction = MONO_FILTERS[f].safe(data, num_syms - 1, px, py, size_x);
							int residual = value + num_syms - prediction;
							if (residual >= num_syms) {
								residual -= num_syms;
							}
							u8 score = MonoChaos::ResidualScore(residual, num_syms); // lower = better

							scores.add(f, score);
						}
					}
					++px;
					++data;
				}
				++py;
				row += size_x;
			}

			// If data is uniform,
			int offset = 0;
			if (uniform) {
				// Find the matching filter
				for (int f = 0; f < _sympal_filter_count; ++f) {
					if (_sympal[f] == uniform_value) {
						// Award it top points
						awards.add(SF_COUNT + f, _params.AWARDS[0]);
						offset = 1;

						// Mark it as a palette filter tile so we can find it faster later if this palette filter gets chosen
						*p = SF_COUNT + f;
						break;
					}
				}
			}

			// Sort top few filters for awards
			FilterScorer::Score *top = scores.getTop(_params.award_count, true);
			for (int ii = offset; ii < _params.award_count; ++ii) {
				awards.add(top[ii - offset].index, _params.AWARDS[ii]);
			}
		}

		topleft_row += _params.size_x * _tile_size_y;
	}

	// Copy the first SF_FIXED filters
	for (int f = 0; f < SF_FIXED; ++f) {
		_filters[f] = MONO_FILTERS[f];
		_filter_indices[f] = f;
	}

	// Decide how many filters to sort by score
	int count = _params.max_filters + SF_FIXED;
	if (count > SF_COUNT) {
		count = SF_COUNT;
	}

	// Calculate min coverage threshold
	int coverage_thresh = _params.filter_thresh * _tiles_count;
	int coverage = 0;

	// Prepare to reduce the sympal set size
	int sympal_f = 0;

	// Choose remaining filters until coverage is acceptable
	int normal_f = SF_FIXED; // Next normal filter index
	int filters_set = SF_FIXED; // Total filters
	FilterScorer::Score *top = awards.getTop(count, true);

	u8 palette[MAX_PALETTE];

	// For each of the sorted filter scores,
	for (int ii = 0; ii < count; ++ii) {
		int index = top[ii].index;
		int score = top[ii].score;

		// Calculate approximate bytes covered
		int covered = score / _params.AWARDS[0];

		// NOTE: Interesting interaction with fixed filters that are not chosen
		coverage += covered;

		// If filter is not fixed,
		if (index >= SF_FIXED) {
			// If filter is a sympal,
			if (index >= SF_COUNT) {
				// Map it from sympal filter index to new filter index
				int sympal_filter = index - SF_COUNT;
				_sympal_filter_map[sympal_filter] = sympal_f;

				palette[sympal_f] = index;
				++sympal_f;

				CAT_INANE("2D") << " - Added palette filter " << sympal_f << " for palette index " << sympal_filter;
			} else {
				_filters[normal_f] = MONO_FILTERS[index];
				_filter_indices[normal_f] = index;
				++normal_f;

				CAT_INANE("2D") << " - Added filter " << normal_f << " for filter index " << index;
			}

			++filters_set;
			if (filters_set >= MAX_FILTERS) {
				break;
			}
		}

		// If coverage is satisifed,
		if (coverage >= coverage_thresh) {
			// We're done here
			break;
		}
	}

	// Insert filter indices at the end
	for (int ii = 0; ii < sympal_f; ++ii) {
		_filter_indices[ii + normal_f] = palette[ii];
	}

	// Record counts
	_normal_filter_count = normal_f;
	_sympal_filter_count = sympal_f;
	_filter_count = filters_set;

	CAT_DEBUG_ENFORCE(_filter_count == _normal_filter_count + _sympal_filter_count);

	CAT_INANE("2D") << " + Chose " << _filter_count << " filters : " << _sympal_filter_count << " of which are palettes";
}

void MonoWriter::designPaletteTiles() {
	if (_sympal_filter_count < 0) {
		CAT_INANE("2D") << "No palette filters selected";
		return;
	}

	CAT_INANE("2D") << "Designing palette tiles for " << _tiles_x << "x" << _tiles_y << "...";

	const u16 tile_size_x = _tile_size_x, tile_size_y = _tile_size_y;
	const u16 size_x = _params.size_x, size_y = _params.size_y;
	u8 *p = _tiles.get();

	// For each tile,
	const u8 *topleft_row = _params.data;
	for (u16 y = 0; y < size_y; y += tile_size_y) {
		const u8 *topleft = topleft_row;

		for (u16 x = 0; x < size_x; x += tile_size_x, ++p, topleft += tile_size_x) {
			const u8 value = *p;

			// If tile is masked,
			if (value == MASK_TILE) {
				continue;
			}

			// If this tile was initially paletted,
			if (value >= SF_COUNT) {
				// Look up the new filter value
				u8 filter = _sympal_filter_map[value - SF_COUNT];

				// If it was used,
				if (filter != UNUSED_SYMPAL) {
					// Prefer it over any other filter type
					*p = _normal_filter_count + filter;
				} else {
					// Unlock it for use
					*p = TODO_TILE;
				}
			}
		}

		topleft_row += _params.size_x * _tile_size_y;
	}
}

void MonoWriter::designTiles() {
	CAT_INANE("2D") << "Designing tiles for " << _tiles_x << "x" << _tiles_y << "...";

	const u16 tile_size_x = _tile_size_x, tile_size_y = _tile_size_y;
	const u16 size_x = _params.size_x, size_y = _params.size_y;
	const u16 num_syms = _params.num_syms;
	u8 *p = _tiles.get();

	EntropyEstimator ee;
	ee.init();

	const u32 code_stride = _tile_size_x * _tile_size_y;
	const u32 codes_size = code_stride * _filter_count;
	_ecodes.resize(codes_size);
	u8 *codes = _ecodes.get();

	// Until revisits are done,
	int passes = 0;
	int revisitCount = _params.knobs->mono_revisitCount;
	while (passes < MAX_PASSES) {
		// For each tile,
		const u8 *topleft_row = _params.data;
		int ty = 0;

		for (u16 y = 0; y < size_y; y += tile_size_y, ++ty) {
			const u8 *topleft = topleft_row;
			int tx = 0;

			for (u16 x = 0; x < size_x; x += tile_size_x, ++p, topleft += tile_size_x, ++tx) {
				// If tile is masked or sympal,
				if (*p >= _normal_filter_count) {
					continue;
				}

				// If we are on the second or later pass,
				if (passes > 0) {
					// If just finished revisiting old zones,
					if (--revisitCount < 0) {
						// Done!
						delete []codes;
						return;
					}

					int code_count = 0;
					const int old_filter = *p;

					// If old filter is not a sympal,
					if (_filter_indices[old_filter] < SF_COUNT) {
						// For each element in the tile,
						const u8 *row = topleft;
						u16 py = y, cy = tile_size_y;
						while (cy-- > 0 && py < size_y) {
							const u8 *data = row;
							u16 px = x, cx = tile_size_x;
							while (cx-- > 0 && px < size_x) {
								// If element is not masked,
								if (!_params.mask(px, py)) {
									const u8 value = *data;

									u8 prediction = _filters[old_filter].safe(data, num_syms - 1, px, py, size_x);
									int residual = value + num_syms - prediction;
									if (residual >= num_syms) {
										residual -= num_syms;
									}

									codes[code_count++] = residual;
								}
								++px;
								++data;
							}
							++py;
							row += size_x;
						}

						ee.subtract(codes, code_count);
					}
				}

				int code_count = 0;

				// For each element in the tile,
				const u8 *row = topleft;
				u16 py = y, cy = tile_size_y;
				while (cy-- > 0 && py < size_y) {
					const u8 *data = row;
					u16 px = x, cx = tile_size_x;
					while (cx-- > 0 && px < size_x) {
						// If element is not masked,
						if (!_params.mask(px, py)) {
							const u8 value = *data;

							u8 *dest = codes + code_count;
							for (int f = 0; f < _filter_count; ++f) {
								u8 prediction = _filters[f].safe(data, num_syms - 1, px, py, size_x);
								int residual = value + num_syms - prediction;
								if (residual >= num_syms) {
									residual -= num_syms;
								}

								*dest = residual;
								dest += code_stride;
							}

							++code_count;
						}
						++px;
						++data;
					}
					++py;
					row += size_x;
				}

				// Read neighbor tiles
				int a = MASK_TILE; // left
				int b = MASK_TILE; // up
				int c = MASK_TILE; // up-left
				int d = MASK_TILE; // up-right

				if (ty > 0) {
					b = p[-_tiles_x];

					if (tx > 0) {
						c = p[-_tiles_x - 1];
					}

					if (tx < _tiles_x-1) {
						d = p[-_tiles_x + 1];
					}
				}

				if (tx > 0) {
					a = p[-1];
				}

				// Evaluate entropy of codes
				u8 *src = codes;
				int lowest_entropy = 0x7fffffff;
				int bestFilterIndex = 0;

				const int NEIGHBOR_REWARD = 1;
				for (int f = 0; f < _filter_count; ++f) {
					int entropy = ee.entropy(src, code_count);

					// Nudge scoring based on neighbors
					if (entropy == 0) {
						entropy -= NEIGHBOR_REWARD;
					}
					if (f == a) {
						entropy -= NEIGHBOR_REWARD;
					}
					if (f == b) {
						entropy -= NEIGHBOR_REWARD;
					}
					if (f == c) {
						entropy -= NEIGHBOR_REWARD;
					}
					if (f == d) {
						entropy -= NEIGHBOR_REWARD;
					}

					if (lowest_entropy > entropy) {
						lowest_entropy = entropy;
						bestFilterIndex = f;
					}

					src += code_stride;
				}

				*p = (u8)bestFilterIndex;
			}

			topleft_row += _params.size_x * _tile_size_y;
		}

		CAT_INANE("2D") << "Revisiting filter selections from the top... " << revisitCount << " left";
	}
}

void MonoWriter::computeResiduals() {
	CAT_INANE("2D") << "Executing tiles to generate residual matrix...";

	const u16 tile_size_x = _tile_size_x, tile_size_y = _tile_size_y;
	const u16 size_x = _params.size_x, size_y = _params.size_y;
	const u16 num_syms = _params.num_syms;
	const u8 *p = _tiles.get();

	// For each tile,
	const u8 *topleft_row = _params.data;
	size_t residual_delta = (size_t)(_residuals.get() - topleft_row);
	for (u16 y = 0; y < size_y; y += tile_size_y) {
		const u8 *topleft = topleft_row;

		for (u16 x = 0; x < size_x; x += tile_size_x, ++p, topleft += tile_size_x) {
			const u8 f = *p;

			// If tile is masked or sympal,
			if (*p >= _normal_filter_count) {
				continue;
			}

			// For each element in the tile,
			const u8 *row = topleft;
			u16 py = y, cy = tile_size_y;
			while (cy-- > 0 && py < size_y) {
				const u8 *data = row;
				u16 px = x, cx = tile_size_x;
				while (cx-- > 0 && px < size_x) {
					// If element is not masked,
					if (!_params.mask(px, py)) {
						const u8 value = *data;

						u8 prediction = _filters[f].safe(data, num_syms - 1, px, py, size_x);
						int residual = value + num_syms - prediction;
						if (residual >= num_syms) {
							residual -= num_syms;
						}

						// Convert data pointer to residual pointer
						u8 *residual_data = (u8*)data + residual_delta;

						// Write residual data
						*residual_data = residual;
					}
					++px;
					++data;
				}
				++py;
				row += size_x;
			}
		}

		topleft_row += _params.size_x * _tile_size_y;
	}
}

void MonoWriter::designRowFilters() {
	CAT_INANE("2D") << "Designing row filters for " << _tiles_x << "x" << _tiles_y << "...";

	const int tiles_x = _tiles_x, tiles_y = _tiles_y;
	const u16 num_filters = _filter_count;
	u8 *p = _tiles.get();

	EntropyEstimator ee;
	ee.init();

	u32 total_entropy;

	// Allocate temporary workspace
	const u32 codes_size = MonoReader::RF_COUNT * tiles_x;
	_ecodes.resize(codes_size);
	u8 *codes = _ecodes.get();

	// For each pass through,
	int passes = 0;
	while (passes < MAX_ROW_PASSES) {
		total_entropy = 0;

		// For each tile,
		for (int ty = 0; ty < tiles_y; ++ty) {
			u8 prev = 0;

			for (int tx = 0; tx < tiles_x; ++tx) {
				u8 f = p[0];

				// If tile is not masked,
				if (f != MASK_TILE) {
					// RF_NOOP
					codes[tx] = f;

					// RF_A
					u8 fprev = f + num_filters - prev;
					if (fprev >= num_filters) {
						fprev -= num_filters;
					}
					codes[tx + tiles_x] = fprev;
				}

				++p;
			}

			// If on the second or later pass,
			if (passes > 0) {
				// Subtract out the previous winner
				ee.subtract(codes + tiles_x * _tile_row_filters[ty], tiles_x);
			}

			// Calculate entropy for each of the row filter options
			u32 e0 = ee.entropy(codes, tiles_x);
			u32 e1 = ee.entropy(codes + tiles_x, tiles_x);

			// Find the best one
			u32 best_e = e0;
			u32 best_i = 0;
			if (best_e > e1) {
				best_e = e1;
				best_i = 1;
			}

			_tile_row_filters[ty] = best_i;
			total_entropy += 1 + best_e; // + 1 bit per row for header

			// Add the best option into the running histogram
			ee.add(codes + tiles_x * best_i, tiles_x);
		}

		++passes;
	}

	_row_filter_entropy = total_entropy;
}

bool MonoWriter::IsMasked(u16 x, u16 y) {
	return _tiles[x + y * _tiles_x] == MASK_TILE;
}

void MonoWriter::recurseCompress() {
	if (_tiles_count < RECURSE_THRESH_COUNT) {
		CAT_INANE("2D") << "Stopping below recursive threshold for " << _tiles_x << "x" << _tiles_y << "...";
	} else {
		CAT_INANE("2D") << "Recursively compressing tiles for " << _tiles_x << "x" << _tiles_y << "...";

		_filter_encoder = new MonoWriter;

		Parameters params = _params;
		params.data = _tiles.get();
		params.num_syms = _filter_count;
		params.size_x = _tiles_x;
		params.size_y = _tiles_y;

		// Hook up our mask function
		params.mask.SetMember<MonoWriter, &MonoWriter::IsMasked>(this);

		// Recurse!
		u32 recurse_entropy = _filter_encoder->process(params);

		// If it does not win over row filters,
		if (recurse_entropy > _row_filter_entropy) {
			CAT_INANE("2D") << "Recursive filter did not win over simple row filters: " << recurse_entropy << " > " << _row_filter_entropy;

			delete _filter_encoder;
			_filter_encoder = 0;
		} else {
			CAT_INANE("2D") << "Recursive filter won over simple row filters: " << recurse_entropy << " <= " << _row_filter_entropy;
		}
	}
}

void MonoWriter::designChaos() {
	CAT_INANE("2D") << "Designing chaos...";

	EntropyEstimator ee[MAX_CHAOS_LEVELS];

	u32 best_entropy = 0x7fffffff;
	int best_chaos_levels = 1;

	// For each chaos level,
	for (int chaos_levels = 1; chaos_levels < MAX_CHAOS_LEVELS; ++chaos_levels) {
		_chaos.init(chaos_levels, _params.size_x);

		// Reset entropy estimator
		for (int ii = 0; ii < chaos_levels; ++ii) {
			ee[ii].init();
		}

		_chaos.start();

		// For each row,
		const u8 *residuals = _residuals.get();
		for (int y = 0; y < _params.size_y; ++y) {
			_chaos.startRow();

			// For each column,
			for (int x = 0; x < _params.size_x; ++x) {
				// If it is a palsym tile,
				const u8 f = getTile(x, y);

				// If masked,
				if (f == MASK_TILE || _params.mask(x, y) || f >= _normal_filter_count) {
					_chaos.zero();
				} else {
					// Get residual symbol
					u8 residual = *residuals;

					// Get chaos bin
					int chaos = _chaos.get();
					_chaos.store(residual, _params.num_syms);

					// Add to histogram for this chaos bin
					ee[chaos].addSingle(residual);
				}

				++residuals;
			}
		}

		// For each chaos level,
		u32 entropy = 0;
		for (int ii = 0; ii < chaos_levels; ++ii) {
			entropy += ee[ii].entropyOverall();

			// Approximate cost of adding an entropy level
			entropy += 5 * _params.num_syms;
		}

		// If this is the best chaos levels so far,
		if (best_entropy > entropy) {
			best_entropy = entropy;
			best_chaos_levels = chaos_levels;
		}
	}

	// Record the best option found
	_chaos.init(best_chaos_levels, _params.size_x);
	_chaos_entropy = best_entropy;
}

void MonoWriter::initializeEncoders() {
	_chaos.start();

	// For each row,
	const u8 *residuals = _residuals.get();
	for (int y = 0; y < _params.size_y; ++y) {
		_chaos.startRow();

		// For each column,
		for (int x = 0; x < _params.size_x; ++x) {
			// If it is a palsym tile,
			const u8 f = getTile(x, y);

			// If masked,
			if (f == MASK_TILE || _params.mask(x, y) || f >= _normal_filter_count) {
				_chaos.zero();
			} else {
				// Get residual symbol
				u8 residual = *residuals;

				// Calculate local chaos
				int chaos = _chaos.get();
				_chaos.store(residual, _params.num_syms);

				// Add to histogram for this chaos bin
				_encoder[chaos].add(residual);
			}

			++residuals;
		}
	}

	// For each chaos level,
	for (int ii = 0, iiend = _chaos.getBinCount(); ii < iiend; ++ii) {
		_encoder[ii].finalize();
	}

	// If using row encoders for filters,
	if (!_filter_encoder) {
		// For each filter row,
		const u8 *tile = _tiles.get();
		for (int ty = 0; ty < _tiles_y; ++ty) {
			int tile_row_filter = _tile_row_filters[ty];
			u8 prev = 0;

			// For each column,
			for (int tx = 0; tx < _tiles_x; ++tx) {
				u8 f = *tile++;

				if (f != MASK_TILE) {
					u8 rf = f;

					// If filtering this row,
					if (tile_row_filter == MonoReader::RF_PREV) {
						rf += _filter_count - prev;
						if (rf >= _filter_count) {
							rf -= _filter_count;
						}
						prev = f;
					}

					_row_filter_encoder.add(rf);
				}
			}
		}

		// Finalize the row filter encoder
		_row_filter_encoder.finalize();
	}
}

u32 MonoWriter::simulate() {
	u32 bits = 0;

	// Chaos overhead
	bits += 4 + _chaos.getBinCount() * 5 * _params.num_syms;

	// Tile bits overhead
	u32 range = (_params.max_bits - _params.min_bits);
	if (range > 0) {
		bits += BSR32(range) + 1;
	}

	// Filter choice overhead
	bits += 5 + 7 * _normal_filter_count;

	// Sympal choice overhead
	bits += 4 + 8 * _sympal_filter_count;

	// If not using row encoders for filters,
	++bits;
	if (_filter_encoder) {
		bits += _filter_encoder->simulate();
	} else {
		// For each filter row,
		const u8 *tile = _tiles.get();
		for (int ty = 0; ty < _tiles_y; ++ty) {
			int tile_row_filter = _tile_row_filters[ty];
			u8 prev = 0;

			// For each column,
			for (int tx = 0; tx < _tiles_x; ++tx) {
				u8 f = *tile++;

				if (f != MASK_TILE) {
					u8 rf = f;

					if (tile_row_filter == MonoReader::RF_PREV) {
						rf += _filter_count - prev;
						if (rf >= _filter_count) {
							rf -= _filter_count;
						}
						prev = f;
					}

					bits += _row_filter_encoder.simulate(rf);
				}
			}
		}
	}

	// Simulate residuals
	_chaos.start();

	// For each row,
	const u8 *residuals = _residuals.get();
	for (int y = 0; y < _params.size_y; ++y) {
		_chaos.startRow();

		// For each column,
		for (int x = 0; x < _params.size_x; ++x) {
			// If it is a palsym tile,
			const u8 f = getTile(x, y);

			// If masked,
			if (f == MASK_TILE || _params.mask(x, y) || f >= _normal_filter_count) {
				_chaos.zero();
			} else {
				// Get residual symbol
				u8 residual = *residuals;

				// Calculate local chaos
				int chaos = _chaos.get();
				_chaos.store(residual, _params.num_syms);

				// Add to histogram for this chaos bin
				bits += _encoder[chaos].simulate(residual);
			}

			++residuals;
		}
	}

	return bits;
}

u32 MonoWriter::process(const Parameters &params) {
	cleanup();

	// Initialize
	_params = params;
	_filter_encoder = 0;

	// Calculate bits to represent tile bits field
	u32 range = (_params.max_bits - _params.min_bits);
	int bits_bc = 0;
	if (range > 0) {
		bits_bc = BSR32(range) + 1;
	}
	_tile_bits_field_bc = bits_bc;

	// Determine best tile size to use
	u32 best_entropy = 0x7fffffff;
	int best_bits = params.max_bits;

	CAT_INANE("2D") << "!! Monochrome filter processing started for " << _params.size_x << "x" << _params.size_y << " data matrix...";

	// For each bit count to try,
	for (int bits = params.min_bits; bits <= params.max_bits; ++bits) {
		// Init with bits
		_tile_bits_x = bits;
		_tile_bits_y = bits;
		_tile_size_x = 1 << bits;
		_tile_size_y = 1 << bits;
		_tiles_x = (_params.size_x + _tile_size_x - 1) >> bits;
		_tiles_y = (_params.size_y + _tile_size_y - 1) >> bits;

		CAT_INANE("2D") << " - Trying " << _tile_size_x << "x" << _tile_size_y << " tile size, yielding a subresolution matrix " << _tiles_x << "x" << _tiles_y << " for input " << _params.size_x << "x" << _params.size_y << " data matrix";

		// Allocate tile memory
		_tiles_count = _tiles_x * _tiles_y;
		_tiles.resize(_tiles_count);
		_tile_row_filters.resize(_tiles_y);

		// Allocate residual memory
		const u32 residuals_memory = _params.size_x * _params.size_y;
		_residuals.resize(residuals_memory);

		// Process
		maskTiles();
		designPaletteFilters();
		designFilters();
		designPaletteTiles();
		designTiles();
		computeResiduals();
		designRowFilters();
		recurseCompress();
		designChaos();
		initializeEncoders();

		// Calculate bits required to represent the data with this tile size
		u32 entropy = simulate();
		if (best_entropy > entropy) {
			best_entropy = entropy;
			best_bits = bits;

			// Allocate a best writer if needed
			if (!_best_writer) {
				_best_writer = new MonoWriter;
			}

			// TODO: Store off the best option
		} else {
			// Stop trying options
			break;
		}
	}

	return best_entropy;
}

bool MonoWriter::init(const Parameters &params) {
	process(params);

	return true;
}

int MonoWriter::writeTables(ImageWriter &writer) {
	// Initialize stats
	Stats.basic_overhead_bits = 0;
	Stats.encoder_overhead_bits = 0;
	Stats.filter_overhead_bits = 1;
	Stats.data_bits = 0;

	// Write tile size
	{
		CAT_DEBUG_ENFORCE(_tile_bits_x == _tile_bits_y);	// Square regions only for now

		if (_tile_bits_field_bc > 0) {
			writer.writeBits(_tile_bits_x - _params.min_bits, _tile_bits_field_bc);
			Stats.basic_overhead_bits += _tile_bits_field_bc;
		}
	}

	DESYNC_TABLE();

	// Sympal filters
	{
		CAT_DEBUG_ENFORCE(MAX_PALETTE <= 16);

		writer.writeBits(_sympal_filter_count - 1, 4);
		for (int f = 0; f < _sympal_filter_count; ++f) {
			writer.writeBits(_sympal[f], 8);
			Stats.basic_overhead_bits += 8;
		}
	}

	DESYNC_TABLE();

	// Normal filters
	{
		CAT_DEBUG_ENFORCE(MAX_FILTERS <= 32);
		CAT_DEBUG_ENFORCE(SF_COUNT + MAX_PALETTE <= 128);

		writer.writeBits(_normal_filter_count - SF_FIXED, 5);
		for (int f = SF_FIXED; f < _normal_filter_count; ++f) {
			writer.writeBits(_filter_indices[f], 7);
			Stats.basic_overhead_bits += 7;
		}
	}

	DESYNC_TABLE();

	// Write chaos levels
	{
		CAT_DEBUG_ENFORCE(MAX_CHAOS_LEVELS <= 16);

		writer.writeBits(_chaos.getBinCount() - 1, 4);
		Stats.basic_overhead_bits += 4;
	}

	DESYNC_TABLE();

	// Write encoder tables
	{
		for (int ii = 0, iiend = _chaos.getBinCount(); ii < iiend; ++ii) {
			Stats.encoder_overhead_bits += _encoder[ii].writeTables(writer);
		}
	}

	DESYNC_TABLE();

	// Bit : row filters or recurse write tables
	{
		// If we decided to recurse,
		if (_filter_encoder) {
			writer.writeBit(1);

			// Recurse write tables
			Stats.filter_overhead_bits += _filter_encoder->writeTables(writer);
		} else {
			writer.writeBit(0);

			// Will write row filters at this depth
			Stats.filter_overhead_bits += _row_filter_encoder.writeTables(writer);
		}
	}

	DESYNC_TABLE();

	initializeWriter();

	return Stats.encoder_overhead_bits + Stats.basic_overhead_bits + Stats.filter_overhead_bits;
}

void MonoWriter::initializeWriter() {
	_tile_seen.resize(_tiles_x);

	_chaos.start();

	if (!_filter_encoder) {
		_row_filter_encoder.reset();
	}

	for (int ii = 0, iiend = _chaos.getBinCount(); ii < iiend; ++ii) {
		_encoder[ii].reset();
	}
}

int MonoWriter::writeRowHeader(u16 y, ImageWriter &writer) {
	CAT_DEBUG_ENFORCE(y < _params.size_y);

	int bits = 0;

	// Reset chaos bin subsystem
	_chaos.startRow();

	// If at the start of a tile row,
	if ((y & (_tile_size_y - 1)) == 0) {
		// Clear tile seen
		_tile_seen.fill_00();

		// Calculate tile y-coordinate
		u16 ty = y >> _tile_bits_y;

		// If filter encoder is used instead of row filters,
		if (_filter_encoder) {
			// Recurse start row (they all start at 0)
			bits += _filter_encoder->writeRowHeader(ty, writer);
		} else {
			CAT_DEBUG_ENFORCE(MonoReader::RF_COUNT <= 2);
			CAT_DEBUG_ENFORCE(_tile_row_filters[ty] < 2);

			// Write out chosen row filter
			writer.writeBit(_tile_row_filters[ty]);
			bits++;

			// Clear prev filter
			_prev_filter = 0;
		}
	}

	DESYNC(0, y);

	Stats.filter_overhead_bits += bits;
	return bits;
}

int MonoWriter::write(u16 x, u16 y, ImageWriter &writer) {
	int overhead_bits = 0, data_bits = 0;

	CAT_DEBUG_ENFORCE(x < _params.size_x && y < _params.size_y);

	// If this pixel is masked,
	if (_params.mask(x, y)) {
		_chaos.zero();
	}

	// Calculate tile coordinates
	u16 tx = x >> _tile_bits_x;

	// If tile not seen yet,
	if (!_tile_seen[tx]) {
		_tile_seen[tx] = true;

		// Get tile
		u16 ty = y >> _tile_bits_y;
		u8 *tile = _tiles.get() + tx + ty * _tiles_x;
		u8 f = tile[0];
		_write_filter = f;

		// If tile is not masked,
		if (f != MASK_TILE) {
			// If recursive filter encoded,
			if (_filter_encoder) {
				// Pass filter write down the tree
				overhead_bits += _filter_encoder->write(tx, ty, writer);
			} else {
				// Calculate row filter residual for filter data (filter of filters at tree leaf)
				u8 rf = f;

				if (_tile_row_filters[ty] == MonoReader::RF_PREV) {
					rf += _filter_count - _prev_filter;
					if (rf >= _filter_count) {
						rf -= _filter_count;
					}
					_prev_filter = f;
				}

				overhead_bits += _row_filter_encoder.write(rf, writer);
			}

			Stats.filter_overhead_bits += overhead_bits;
		}

		DESYNC(x, y);
	}

	// If filter is masked,
	u8 f = _write_filter;
	if (f >= _normal_filter_count) {
		_chaos.zero();
	} else {
		// Look up residual sym
		u8 residual = _residuals[x + y * _params.size_x];

		// Calculate local chaos
		int chaos = _chaos.get();
		_chaos.store(residual, _params.num_syms);

		// Write the residual value
		data_bits += _encoder[chaos].write(residual, writer);

		Stats.data_bits += data_bits;
	}

	DESYNC(x, y);

	return overhead_bits + data_bits;
}
