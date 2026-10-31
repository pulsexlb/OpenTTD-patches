/* This file is part of OpenTTD and is licensed under the GNU General Public License version 2. */
/** @file terrain3d_mesh.h Packed tile geometry updates, preserving tile and face order. */
#ifndef VIDEO_TERRAIN3D_MESH_H
#define VIDEO_TERRAIN3D_MESH_H

#include "terrain3d_math.h"
#include <algorithm>
#include <vector>

namespace Terrain3D {
struct TileMeshSpan {
	size_t end = 0;
	Vec low{}, high{};
};

/** Reuse unchanged face spans; rebuild variable-sized changed spans into shared scratch storage.
 * Bounds are cached per tile so removal/shrinking also tightens the chunk's culling bounds.
 * When every changed tile keeps its face count, the new faces are written in place.
 * The caller must mark every tile changed on first use or after resetting the cache.
 */
template <typename Face, typename Builder>
void PatchTileMesh(std::vector<Face> &faces, std::vector<TileMeshSpan> &spans,
		const std::vector<bool> &changed, std::vector<Face> &scratch, Builder build)
{
	bool fresh = spans.size() != changed.size();
	spans.resize(changed.size());
	scratch.clear();
	/* Build the changed tiles first; \c built holds each one's end in scratch. */
	std::vector<size_t> built;
	bool same_size = !fresh;
	size_t begin = 0;
	for (size_t i = 0; i < changed.size(); ++i) {
		size_t end = spans[i].end;
		if (changed[i]) {
			auto &span = spans[i];
			size_t first = scratch.size();
			build(scratch, i);
			built.push_back(scratch.size());
			span.low = {1e30f, 1e30f, 1e30f};
			span.high = {-1e30f, -1e30f, -1e30f};
			for (size_t f = first; f < scratch.size(); ++f) for (auto p : scratch[f].p) {
				span.low = {std::min(span.low.x, p.x), std::min(span.low.y, p.y), std::min(span.low.z, p.z)};
				span.high = {std::max(span.high.x, p.x), std::max(span.high.y, p.y), std::max(span.high.z, p.z)};
			}
			same_size = same_size && scratch.size() - first == end - begin;
		}
		begin = end;
	}
	if (same_size) {
		size_t next = 0, from = 0;
		for (size_t i = 0; i < changed.size(); ++i) {
			if (!changed[i]) continue;
			size_t to = built[next++];
			std::copy(scratch.begin() + from, scratch.begin() + to, faces.begin() + (i == 0 ? 0 : spans[i - 1].end));
			from = to;
		}
		return;
	}
	/* Keep each chunk's capacity tied to its own geometry. Swapping a shared
	 * scratch buffer would propagate a large chunk's allocation to small chunks. */
	size_t total = scratch.size();
	begin = 0;
	for (size_t i = 0; i < changed.size(); ++i) {
		if (!changed[i] && !fresh) total += spans[i].end - begin;
		begin = spans[i].end;
	}
	std::vector<Face> result;
	result.reserve(total);
	size_t next = 0, from = 0;
	begin = 0;
	for (size_t i = 0; i < changed.size(); ++i) {
		size_t end = spans[i].end;
		if (changed[i]) {
			size_t to = built[next++];
			result.insert(result.end(), scratch.begin() + from, scratch.begin() + to);
			from = to;
		} else {
			result.insert(result.end(), faces.begin() + begin, faces.begin() + end);
		}
		begin = end;
		spans[i].end = result.size();
	}
	faces = std::move(result);
}
}
#endif
