/* This file is part of OpenTTD and is licensed under the GNU General Public License version 2. */
/** @file terrain3d_slots.h Per-tile vertex slots in a retained chunk buffer, so a changed tile rewrites only its own slot. */
#ifndef VIDEO_TERRAIN3D_SLOTS_H
#define VIDEO_TERRAIN3D_SLOTS_H

#include <algorithm>
#include <vector>

namespace Terrain3D {
/** Vertex ranges of each tile inside one buffer, drawn with one multi-draw.
 * A tile that outgrows its slot moves to the free tail with 25% extra room. When the tail is
 * full, every tile is laid out again, keeping the grown slots so repeated animation frames
 * and construction stages settle into place. A buffer that never changed has no spare room. */
class TileSlots {
public:
	std::vector<int> first, count, capacity; ///< Vertices, per tile; \c count is what is drawn.
	size_t used = 0; ///< End of the allocated slots.
	size_t size = 0; ///< Buffer size in vertices.

	/**
	 * Fit the new tile sizes.
	 * @param sizes Vertex count of every tile.
	 * @param changed Tiles whose geometry changed; an empty or mismatched list means all of them.
	 * @param writes Receives the tiles whose slots must be written.
	 * @return True when the buffer must be reallocated at \c size and written whole.
	 */
	bool Update(const std::vector<size_t> &sizes, const std::vector<bool> &changed, std::vector<size_t> &writes)
	{
		writes.clear();
		if (this->first.size() != sizes.size() || changed.size() != sizes.size()) {
			this->Layout(sizes, false);
			return this->WriteAll(writes);
		}
		for (size_t i = 0; i < sizes.size(); ++i) {
			if (!changed[i]) continue;
			int n = static_cast<int>(sizes[i]);
			if (n > this->capacity[i]) {
				int grown = Grow(n);
				if (this->used + grown > this->size) {
					this->Layout(sizes, true, &changed);
					return this->WriteAll(writes);
				}
				this->first[i] = static_cast<int>(this->used);
				this->capacity[i] = grown;
				this->used += grown;
			}
			this->count[i] = n;
			writes.push_back(i);
		}
		return false;
	}

	/**
	 * Merge the drawn ranges of consecutive tiles that sit back to back, keeping tile order, so drawing the runs
	 * is the same as drawing every tile's range. A packed buffer is one run; unused capacity is never drawn.
	 * @param run_first Receives the first vertex of each run.
	 * @param run_count Receives the vertex count of each run.
	 */
	void Runs(std::vector<int> &run_first, std::vector<int> &run_count) const
	{
		run_first.clear();
		run_count.clear();
		for (size_t i = 0; i < this->first.size(); ++i) {
			if (this->count[i] == 0) continue;
			if (!run_first.empty() && run_first.back() + run_count.back() == this->first[i]) {
				run_count.back() += this->count[i];
			} else {
				run_first.push_back(this->first[i]);
				run_count.push_back(this->count[i]);
			}
		}
	}

private:
	static int Grow(int n) { return n + n / 4; }

	bool WriteAll(std::vector<size_t> &writes)
	{
		/* A relayout can follow partial writes collected earlier in Update. */
		writes.clear();
		for (size_t i = 0; i < this->first.size(); ++i) writes.push_back(i);
		return true;
	}

	/** Pack the slots in tile order. Without \c keep every slot fits exactly and there is no tail. */
	void Layout(const std::vector<size_t> &sizes, bool keep, const std::vector<bool> *changed = nullptr)
	{
		if (!keep) this->capacity.assign(sizes.size(), 0);
		this->first.resize(sizes.size());
		this->count.resize(sizes.size());
		this->used = 0;
		for (size_t i = 0; i < sizes.size(); ++i) {
			int n = static_cast<int>(sizes[i]);
			int cap = n;
			if (keep && (*changed)[i]) cap = n > this->capacity[i] ? Grow(n) : this->capacity[i];
			else if (keep) cap = std::max(n, this->capacity[i]);
			this->first[i] = static_cast<int>(this->used);
			this->count[i] = n;
			this->capacity[i] = cap;
			this->used += cap;
		}
		this->size = keep ? this->used + this->used / 32 + 1024 : this->used;
	}
};
}
#endif
