/* This file is part of OpenTTD and is licensed under the GNU General Public License version 2. */
/** @file terrain3d_cache.h Deferred mesh changes and bounded presentation cache policy. */
#ifndef VIDEO_TERRAIN3D_CACHE_H
#define VIDEO_TERRAIN3D_CACHE_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace Terrain3D {
/** Snapshot refreshes may happen many times before a mesh is needed. */
class MeshChanges {
	std::vector<bool> pending;
public:
	void Merge(const std::vector<bool> &changed)
	{
		this->pending.resize(changed.size(), false);
		for (size_t i = 0; i < changed.size(); ++i) this->pending[i] = this->pending[i] || changed[i];
	}
	bool Empty() const { return std::none_of(this->pending.begin(), this->pending.end(), [](bool b) { return b; }); }
	std::vector<bool> Take(size_t count, bool rebuild_all)
	{
		std::vector<bool> result(count, rebuild_all);
		if (!rebuild_all) result.swap(this->pending);
		this->pending.clear();
		return result;
	}
};

struct MeshCacheEntry {
	size_t bytes;
	uint64_t used;
	bool pinned;
};

/** Mesh memory budget for the pinned (visible) bytes: room for twice as much again off screen, so a zoomed-out
 * pan revisits chunks instead of rebuilding them, within a floor (all of a 2048x512 map) and a hard cap. */
inline size_t MeshBudget(size_t pinned)
{
	constexpr size_t MB = 1024 * 1024;
	return std::clamp(pinned * 3, 1536 * MB, 2048 * MB);
}

/** The screen cells of a kept view image that need drawing again, in image pixels with rows from the top.
 * A partial render clears and redraws only these cells and leaves the rest of the image as it was. */
class DirtyCells {
	int width = 0, height = 0, columns = 0, rows = 0;
	std::vector<bool> cells;
	size_t marked = 0;

	/** The cells [c0, c1) x [r0, r1) a pixel rectangle touches, widened by a margin for rasterisation; false when it misses the image. */
	bool Range(float x0, float y0, float x1, float y1, int &c0, int &r0, int &c1, int &r1) const
	{
		if (!(x1 > -MARGIN && y1 > -MARGIN && x0 < this->width + MARGIN && y0 < this->height + MARGIN)) return false; // Also false for NaN.
		c0 = std::clamp(int(std::floor((x0 - MARGIN) / CELL)), 0, this->columns - 1);
		r0 = std::clamp(int(std::floor((y0 - MARGIN) / CELL)), 0, this->rows - 1);
		c1 = std::clamp(int(std::floor((x1 + MARGIN) / CELL)) + 1, c0 + 1, this->columns);
		r1 = std::clamp(int(std::floor((y1 + MARGIN) / CELL)) + 1, r0 + 1, this->rows);
		return true;
	}

public:
	static constexpr int CELL = 32; ///< Cell size in pixels.
	static constexpr float MARGIN = 2; ///< Pixels added around marked and tested rectangles.

	DirtyCells() = default;
	DirtyCells(int width, int height) : width(width), height(height), columns((width + CELL - 1) / CELL), rows((height + CELL - 1) / CELL),
		cells(static_cast<size_t>(columns) * rows, false) {}

	bool Empty() const { return this->marked == 0; }
	/** Share of the image's cells marked. */
	float Fraction() const { return this->cells.empty() ? 0.0f : float(this->marked) / float(this->cells.size()); }

	void Mark(float x0, float y0, float x1, float y1)
	{
		int c0, r0, c1, r1;
		if (!this->Range(x0, y0, x1, y1, c0, r0, c1, r1)) return;
		for (int r = r0; r < r1; ++r) for (int c = c0; c < c1; ++c) {
			size_t i = static_cast<size_t>(r) * this->columns + c;
			if (!this->cells[i]) { this->cells[i] = true; ++this->marked; }
		}
	}

	/** Whether anything drawn within the pixel rectangle can reach a marked cell. */
	bool Overlaps(float x0, float y0, float x1, float y1) const
	{
		int c0, r0, c1, r1;
		if (this->marked == 0 || !this->Range(x0, y0, x1, y1, c0, r0, c1, r1)) return false;
		for (int r = r0; r < r1; ++r) for (int c = c0; c < c1; ++c) {
			if (this->cells[static_cast<size_t>(r) * this->columns + c]) return true;
		}
		return false;
	}

	/** Marked cells merged into runs along each row of cells, as x, y, width and height clipped to the image. */
	std::vector<std::array<int, 4>> Rects() const
	{
		std::vector<std::array<int, 4>> result;
		for (int r = 0; r < this->rows; ++r) {
			for (int c = 0; c < this->columns;) {
				if (!this->cells[static_cast<size_t>(r) * this->columns + c]) { ++c; continue; }
				int end = c;
				while (end < this->columns && this->cells[static_cast<size_t>(r) * this->columns + end]) ++end;
				int x = c * CELL, y = r * CELL;
				result.push_back({x, y, std::min(end * CELL, this->width) - x, std::min(y + CELL, this->height) - y});
				c = end;
			}
		}
		return result;
	}
};

/** Evict oldest unpinned allocations. Active views are a soft lower bound on memory;
 * never evict them merely to meet the budget, which would cause rebuild thrashing. */
inline std::vector<size_t> MeshEvictions(const std::vector<MeshCacheEntry> &entries, size_t budget)
{
	size_t bytes = 0;
	std::vector<size_t> candidates, result;
	for (size_t i = 0; i < entries.size(); ++i) {
		bytes += entries[i].bytes;
		if (!entries[i].pinned && entries[i].bytes != 0) candidates.push_back(i);
	}
	std::stable_sort(candidates.begin(), candidates.end(), [&](size_t a, size_t b) { return entries[a].used < entries[b].used; });
	for (size_t i : candidates) {
		if (bytes <= budget) break;
		bytes -= entries[i].bytes;
		result.push_back(i);
	}
	return result;
}
}
#endif
