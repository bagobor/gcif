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

#include "ImagePaletteWriter.hpp"
#include "../decoder/EndianNeutral.hpp"
#include "Log.hpp"
#include "../decoder/Filters.hpp"
#include "EntropyEstimator.hpp"
#include "EntropyEncoder.hpp"
using namespace cat;

#include <algorithm> // std::sort


//// ImagePaletteWriter

void ImagePaletteWriter::clear() {
	if (_image) {
		delete []_image;
		_image = 0;
	}
}

bool ImagePaletteWriter::generatePalette() {
	const u32 *color = reinterpret_cast<const u32 *>( _rgba );

	int colorIndex = 0;

	for (int y = 0; y < _height; ++y) {
		for (int x = 0, xend = _width; x < xend; ++x) {
			u32 c = *color++;

			if (_mask->masked(x, y) || _lz->visited(x, y)) {
				continue;
			}

			// If not seen,
			if (_map.find(c) == _map.end()) {
				// If ran out of palette slots,
				if (colorIndex >= PALETTE_MAX) {
					_enabled = false;
					return false;
				}

				_map[c] = colorIndex++;
				_palette.push_back(c);
			}
		}
	}

	if (colorIndex == 0) {
		_enabled = false;
		return false;
	}

	_enabled = true;
	return true;
}

bool compareColors(u32 a, u32 b) {
	swapLE(a);
	swapLE(b);

	u8 a_a = (u8)(a >> 24);
	u8 b_a = (u8)(b >> 24);

	if (a_a < b_a) {
		return true;
	}

	u8 a_r = (u8)a;
	u8 a_g = (u8)(a >> 8);
	u8 a_b = (u8)(a >> 16);

	u8 b_r = (u8)b;
	u8 b_g = (u8)(b >> 8);
	u8 b_b = (u8)(b >> 16);

	// Sort by luminance
	float y_a = (0.2126*a_r) + (0.7152*a_g) + (0.0722*a_b);
	float y_b = (0.2126*b_r) + (0.7152*b_g) + (0.0722*b_b);

	if (y_a < y_b) {
		return true;
	}

	return false;
}

void ImagePaletteWriter::sortPalette() {
	std::sort(_palette.begin(), _palette.end(), compareColors);

	// Fix map
	_map.clear();

	for (int ii = 0, iiend = (int)_palette.size(); ii < iiend; ++ii) {
		u32 color = _palette[ii];
		_map[color] = (u8)ii;
	}
}

void ImagePaletteWriter::generateImage() {
	const int image_size = _width * _height;
	if (!_image || _image_alloc < image_size) {
		if (_image) {
			delete []_image;
		}

		_image = new u8[image_size];
		_image_alloc = image_size;
	}

	const u32 *color = reinterpret_cast<const u32 *>( _rgba );
	u8 *image = _image;

	u16 masked_palette = 0;
	if (_mask->enabled()) {
		u32 maskColor = _mask->getColor();

		if (_map.find(maskColor) != _map.end()) {
			masked_palette = _map[maskColor];
		}
	}

	for (int y = 0; y < _height; ++y) {
		for (int x = 0, xend = _width; x < xend; ++x) {
			u32 c = *color++;

			if (_mask->masked(x, y)) {
				*image++ = masked_palette;
			} else {
				*image++ = _map[c];
			}
		}
	}

	_masked_palette = masked_palette;
}

int ImagePaletteWriter::initFromRGBA(const u8 *rgba, int width, int height, const GCIFKnobs *knobs, ImageMaskWriter &mask, ImageLZWriter &lz) {
	_knobs = knobs;
	_rgba = rgba;
	_width = width;
	_height = height;
	_mask = &mask;
	_lz = &lz;

	// If palette was generated,
	if (generatePalette()) {
		// Sort the palette to improve compression
		sortPalette();

		// Generate palette raster
		generateImage();
	}

	return GCIF_WE_OK;
}



void ImagePaletteWriter::write(ImageWriter &writer) {
	int bits = 1;
	writer.writeBit(_enabled);

	if (_enabled) {
		CAT_DEBUG_ENFORCE(PALETTE_MAX <= 256);

		const int palette_size = (int)_palette.size();
		writer.writeBits(palette_size - 1, 8);
		bits += 8;

		// Write palette index for mask
		writer.writeBits(_masked_palette, 8);
		bits += 8;

		// If palette is small,
		if (palette_size < 40) {
			writer.writeBit(0);
			++bits;

			for (int ii = 0; ii < palette_size; ++ii) {
				u32 color = getLE(_palette[ii]);

				writer.writeWord(color);
				bits += 32;
			}
		} else {
			writer.writeBit(1);
			++bits;

			// Find best color filter
			int bestCF = 0;
			u32 bestScore = 0x7fffffff;

			for (int cf = 0; cf < CF_COUNT; ++cf) {
				RGB2YUVFilterFunction filter = RGB2YUV_FILTERS[cf];

				EntropyEstimator ee;
				ee.init();

				for (int ii = 0; ii < palette_size; ++ii) {
					u32 color = getLE(_palette[ii]);

					u8 rgb[3] = {
						(u8)color,
						(u8)(color >> 8),
						(u8)(color >> 16)
					};

					u8 yuva[4];
					filter(rgb, yuva);
					yuva[3] = (u8)(color >> 24);

					ee.add(yuva, 4);
				}

				int entropy = 0;

				for (int ii = 0; ii < palette_size; ++ii) {
					u32 color = getLE(_palette[ii]);

					u8 rgb[3] = {
						(u8)color,
						(u8)(color >> 8),
						(u8)(color >> 16)
					};

					u8 yuva[4];
					filter(rgb, yuva);
					yuva[3] = (u8)(color >> 24);

					entropy += ee.entropy(yuva, 4);
				}

				if (entropy < bestScore) {
					bestCF = cf;
					bestScore = entropy;
				}
			}

			CAT_DEBUG_ENFORCE(CF_COUNT <= 16);

			writer.writeBits(bestCF, 4);
			bits += 4;

			RGB2YUVFilterFunction bestFilter = RGB2YUV_FILTERS[bestCF];

			EntropyEncoder<PALETTE_MAX, ENCODER_ZRLE_SYMS> encoder;

			// Train
			for (int ii = 0; ii < palette_size; ++ii) {
				u32 color = getLE(_palette[ii]);

				u8 rgb[3] = {
					(u8)color,
					(u8)(color >> 8),
					(u8)(color >> 16)
				};

				u8 yuva[4];
				bestFilter(rgb, yuva);
				yuva[3] = (u8)(color >> 24);

				encoder.add(yuva[0]);
				encoder.add(yuva[1]);
				encoder.add(yuva[2]);
				encoder.add(yuva[3]);
			}

			encoder.finalize();

			bits += encoder.writeTables(writer);

			// Fire
			for (int ii = 0; ii < palette_size; ++ii) {
				u32 color = getLE(_palette[ii]);

				u8 rgb[3] = {
					(u8)color,
					(u8)(color >> 8),
					(u8)(color >> 16)
				};

				u8 yuva[4];
				bestFilter(rgb, yuva);
				yuva[3] = (u8)(color >> 24);

				bits += encoder.write(yuva[0], writer);
				bits += encoder.write(yuva[1], writer);
				bits += encoder.write(yuva[2], writer);
				bits += encoder.write(yuva[3], writer);
			}
		}
	}

#ifdef CAT_COLLECT_STATS
	Stats.palette_size = (int)_palette.size();
	Stats.overhead_bits = bits;
#endif
}


#ifdef CAT_COLLECT_STATS

bool ImagePaletteWriter::dumpStats() {
	if (!_enabled) {
		CAT_INANE("stats") << "(Palette)   Disabled.";
	} else {
		CAT_INANE("stats") << "(Palette)   Palette size : " << Stats.palette_size << " colors";
		CAT_INANE("stats") << "(Palette)       Overhead : " << Stats.overhead_bits / 8 << " bytes";
	}

	return true;
}

#endif
