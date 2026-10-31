/* This file is part of OpenTTD and is licensed under the GNU General Public License version 2. */
/** @file terrain3d_dirty.h Coalesced tile snapshot invalidation inside 16/32-tile chunks. */
#ifndef VIDEO_TERRAIN3D_DIRTY_H
#define VIDEO_TERRAIN3D_DIRTY_H

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cstdint>
#include <vector>

namespace Terrain3D {
class DirtyTileSet {
	std::array<uint64_t, 16> words{};
	bool pending = true, all = true;

public:
	bool Empty() const { return !this->pending; }
	void Mark(size_t index)
	{
		assert(index < this->words.size() * 64);
		this->words[index / 64] |= uint64_t{1} << (index % 64);
		this->pending = true;
	}

	/** First use and reference modes read all tiles. Equality controls mesh rebuilding;
	 * redundant redraw notifications never require rebuilding unchanged geometry. */
	template <typename Snapshot, typename Reader>
	std::vector<bool> Refresh(std::vector<Snapshot> &snapshots, size_t count, bool full, Reader read)
	{
		assert(count <= this->words.size() * 64);
		bool initial = snapshots.size() != count;
		snapshots.resize(count);
		std::vector<bool> changed(count, false);
		auto update = [&](size_t i) {
			auto current = read(i);
			changed[i] = initial || !(current == snapshots[i]);
			if (changed[i]) snapshots[i] = current;
		};
		if (initial || full || this->all) {
			for (size_t i = 0; i < count; ++i) update(i);
		} else {
			for (size_t w = 0; w < (count + 63) / 64; ++w) {
				for (uint64_t bits = this->words[w]; bits != 0; bits &= bits - 1) {
					update(w * 64 + std::countr_zero(bits));
				}
			}
		}
		this->words.fill(0);
		this->pending = this->all = false;
		return changed;
	}
};

/** Include shared-corner neighbors, clip at map edges, and use each chunk's actual
 * row width (also valid for partial edge chunks). Repeated marks coalesce. */
template <typename Marker>
void MarkDirtyTiles(int x, int y, int width, int height, int chunk_size, Marker mark)
{
	if (x < 0 || y < 0 || x >= width || y >= height) return;
	for (int ty = std::max(0, y - 1); ty <= std::min(height - 1, y + 1); ++ty) {
		for (int tx = std::max(0, x - 1); tx <= std::min(width - 1, x + 1); ++tx) {
			int cx = tx / chunk_size, cy = ty / chunk_size;
			int row_width = std::min(chunk_size, width - cx * chunk_size);
			mark(cx, cy, (ty % chunk_size) * row_width + tx % chunk_size);
		}
	}
}
}
#endif
