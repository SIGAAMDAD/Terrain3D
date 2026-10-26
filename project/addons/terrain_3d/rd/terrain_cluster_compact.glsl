#[versions]
compact = "";

#[compute]
#version 450
#VERSION_DEFINES
// BR10-BR11: optional exact Godot Forward+ cluster-mask compaction.
// One compute program, four ordered dispatches and explicit barriers.
// The borrowed buffer is read-only; all outputs are Terrain3D-owned.
layout(local_size_x = 128, local_size_y = 1, local_size_z = 1) in;
layout(set = 0, binding = 0, std430) readonly buffer NativeClusters { uint words[]; } native_clusters;
layout(set = 0, binding = 1, std430) buffer Counts { uint counts[]; } output_counts;
layout(set = 0, binding = 2, std430) buffer Prefix { uint prefix[]; } output_prefix;
layout(set = 0, binding = 3, std430) buffer GroupSums { uint sums[]; } group_sums;
layout(set = 0, binding = 4, std430) buffer GroupBases { uint bases[]; } group_bases;
layout(set = 0, binding = 5, std430) buffer Headers { uvec4 entries[]; } output_headers;
layout(set = 0, binding = 6, std430) buffer Indices { uint indices[]; } output_indices;
layout(push_constant, std430) uniform CompactPush {
    uvec4 control; // phase, list count, native cluster count, output index capacity
    uvec4 layout_info; // max words, cluster type stride, cluster width, borrowed word count
} push_data;
#define BR21_LIST_KIND_COUNT 5u
#define BR21_SLICE_COUNT 32u
#define BR21_LISTS_PER_CLUSTER (BR21_LIST_KIND_COUNT * BR21_SLICE_COUNT)
#define BR21_SCAN_WIDTH 128u
#define BR21_HEADER_VALID 0x42523131u
shared uint scan_scratch[128];

uint br21_range_mask(uint word_index, uint item_min, uint item_max) {
    // Same bitfieldInsert bounds and signed-clamp arithmetic as the native raster path.
    int local_min = clamp(int(item_min) - int(word_index) * 32, 0, 31);
    int mask_width = min(int(item_max) - int(item_min), 32 - local_min);
    return mask_width > 0 ? bitfieldInsert(0u, 0xffffffffu, local_min, mask_width) : 0u;
}

bool br21_list_bounds(uint list_id, out uint type_base,
                      out uint min_item, out uint max_item,
                      out uint first_word, out uint last_word) {
    uint cluster_index = list_id / BR21_LISTS_PER_CLUSTER;
    uint kind = (list_id / BR21_SLICE_COUNT) % BR21_LIST_KIND_COUNT;
    uint slice = list_id % BR21_SLICE_COUNT;
    uint max_words = push_data.layout_info.x;
    uint stride = max_words + BR21_SLICE_COUNT;
    if (max_words == 0u || cluster_index >= push_data.control.z ||
        push_data.layout_info.y < stride ||
        cluster_index >= push_data.layout_info.y / stride) {
        return false;
    }
    type_base = cluster_index * stride + kind * push_data.layout_info.y;
    uint metadata_index = type_base + max_words + slice;
    if (metadata_index >= push_data.layout_info.w) return false;
    uint bounds = native_clusters.words[metadata_index];
    min_item = bounds & 0xffffu;
    max_item = bounds >> 16u;
    first_word = min(min_item >> 5u, max_words);
    last_word = min(max_item == 0u ? 0u : ((max_item - 1u) >> 5u) + 1u, max_words);
    return last_word > first_word && type_base <= push_data.layout_info.w &&
           max_words <= push_data.layout_info.w - type_base;
}

void br21_count(uint list_id) {
    uint base, minimum, maximum, first_word, last_word;
    uint count = 0u;
    if (br21_list_bounds(list_id, base, minimum, maximum, first_word, last_word)) {
        for (uint word = first_word; word < last_word; ++word) {
            count += bitCount(native_clusters.words[base + word] &
                              br21_range_mask(word, minimum, maximum));
        }
    }
    output_counts.counts[list_id] = count;
}

void br21_scan_local(uint lane, uint list_id, uint group_id) {
    uint value = list_id < push_data.control.y ? output_counts.counts[list_id] : 0u;
    scan_scratch[lane] = value;
    barrier();
    for (uint offset = 1u; offset < BR21_SCAN_WIDTH; offset <<= 1u) {
        uint prev = lane >= offset ? scan_scratch[lane - offset] : 0u;
        barrier();
        scan_scratch[lane] += prev;
        barrier();
    }
    if (list_id < push_data.control.y) {
        output_prefix.prefix[list_id] = scan_scratch[lane] - value;
    }
    if (lane == BR21_SCAN_WIDTH - 1u) group_sums.sums[group_id] = scan_scratch[lane];
}

void br21_scan_groups(uint lane) {
    uint groups = (push_data.control.y + BR21_SCAN_WIDTH - 1u) / BR21_SCAN_WIDTH;
    uint chunks = (groups + BR21_SCAN_WIDTH - 1u) / BR21_SCAN_WIDTH;
    uint running = 0u;
    for (uint chunk = 0u; chunk < chunks; ++chunk) {
        uint index = chunk * BR21_SCAN_WIDTH + lane;
        uint value = index < groups ? group_sums.sums[index] : 0u;
        scan_scratch[lane] = value;
        barrier();
        for (uint shift = 1u; shift < BR21_SCAN_WIDTH; shift <<= 1u) {
            uint prev = lane >= shift ? scan_scratch[lane - shift] : 0u;
            barrier();
            scan_scratch[lane] += prev;
            barrier();
        }
        if (index < groups) group_bases.bases[index] = running + scan_scratch[lane] - value;
        uint chunk_sum = scan_scratch[BR21_SCAN_WIDTH - 1u];
        barrier();
        running += chunk_sum;
    }
}

void br21_scatter(uint list_id) {
    uint count = output_counts.counts[list_id];
    uint offset = output_prefix.prefix[list_id] + group_bases.bases[list_id / BR21_SCAN_WIDTH];
    // Invalid/overflowed slices always fall back to the borrowed mask in raster.
    if (offset > push_data.control.w || count > push_data.control.w - offset) {
        output_headers.entries[list_id] = uvec4(0u);
        return;
    }
    if (count > 0u) {
        uint base, minimum, maximum, first_word, last_word;
        if (!br21_list_bounds(list_id, base, minimum, maximum, first_word, last_word)) {
            output_headers.entries[list_id] = uvec4(0u);
            return;
        }
        uint emitted = 0u;
        for (uint word = first_word; word < last_word; ++word) {
            uint mask = native_clusters.words[base + word] & br21_range_mask(word, minimum, maximum);
            // Preserve native order: ascending mask word, descending set bit.
            while (mask != 0u) {
                uint bit = uint(findMSB(mask));
                mask &= ~(1u << bit);
                output_indices.indices[offset + emitted++] = word * 32u + bit;
            }
        }
        if (emitted != count) {
            output_headers.entries[list_id] = uvec4(0u);
            return;
        }
    }
    output_headers.entries[list_id] = uvec4(offset, count, BR21_HEADER_VALID, 0u);
}

void main() {
    uint lane = gl_LocalInvocationIndex;
    uint list_id = gl_GlobalInvocationID.x;
    uint phase = push_data.control.x;
    if (phase == 0u) {
        if (list_id < push_data.control.y) br21_count(list_id);
    } else if (phase == 1u) {
        br21_scan_local(lane, list_id, gl_WorkGroupID.x);
    } else if (phase == 2u) {
        if (gl_WorkGroupID.x == 0u) br21_scan_groups(lane);
    } else if (phase == 3u) {
        if (list_id < push_data.control.y) br21_scatter(list_id);
    }
}
