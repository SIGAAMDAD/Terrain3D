// P31b experimental: multi-request, single-owner coarse-grid producer.
// This shader REQUIRES a depth-tested R32_UINT texture array whose layers
// contain ALL virtual page IDs contributed by each visible pixel. Missing
// layers must be INVALID_PAGE. A separate per-pixel request-count R32_UINT
// texture is required to detect overflowing source layers.
// Rendering these sources is not yet implemented.
// An overflow or unsupported cell size marks the capture incomplete; the CPU
// must reject it and retain the production raster-feedback result.
#[versions]
reduce_multi = "";

#[compute]
#version 450
#VERSION_DEFINES

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(set = 0, binding = 0) uniform usampler2DArray source_pages;
// R32_UINT request count per pixel. Values > source layer count mark
// the capture incomplete instead of silently ignoring extra materials.
layout(set = 0, binding = 1) uniform usampler2D source_counts;
layout(set = 0, binding = 2, std430) buffer CoarseFeedbackMulti {
    uint words[];
} destination;

layout(push_constant, std430) uniform MultiParams {
    uvec4 dimensions; // grid width, grid height, viewport width, viewport height
    uvec4 sourceInfo;      // slot, slot stride words, global page count, source layers
    uvec4 limits;     // max pixels per cell, record words, slot words, reserved
} params;

const uint INVALID_PAGE = 0xffffffffu;
const uint HEADER_WORDS = 10u;
const uint PAGES_PER_CELL = 4u;
const uint RECORD_WORDS = 9u;
const uint MAX_LAYERS = 8u;
const uint MAX_PIXELS = 256u;

uint inverse_edge(uint cell, uint viewport, uint grid) {
    return (cell * viewport + grid - 1u) / grid;
}

void main() {
    uvec2 cell = gl_GlobalInvocationID.xy;
    uint gw = params.dimensions.x;
    uint gh = params.dimensions.y;
    uint vw = params.dimensions.z;
    uint vh = params.dimensions.w;
    uint slot = params.sourceInfo.x;
    uint stride = params.sourceInfo.y;
    uint page_count = params.sourceInfo.z;
    uint layers = params.sourceInfo.w;
    if (cell.x >= gw || cell.y >= gh) return;
    if (gw == 0u || gh == 0u || gw > 240u || gh > 136u ||
        vw == 0u || vh == 0u || vw > 65535u || vh > 65535u ||
        slot >= 4u || page_count == 0u || layers == 0u || layers > MAX_LAYERS ||
        params.limits.x != MAX_PIXELS || params.limits.y != RECORD_WORDS ||
        stride != HEADER_WORDS + gw * gh * RECORD_WORDS || params.limits.z != stride ||
        textureSize(source_pages, 0) != ivec3(vw, vh, int(layers)) ||
        textureSize(source_counts, 0) != ivec2(vw, vh)) return;

    uint offset = slot * stride + HEADER_WORDS +
        (cell.y * gw + cell.x) * RECORD_WORDS;
    if (offset + RECORD_WORDS > destination.words.length()) return;

    uint x0 = inverse_edge(cell.x, vw, gw);
    uint x1 = min(inverse_edge(cell.x + 1u, vw, gw), vw);
    uint y0 = inverse_edge(cell.y, vh, gh);
    uint y1 = min(inverse_edge(cell.y + 1u, vh, gh), vh);
    uint pages[PAGES_PER_CELL] = uint[PAGES_PER_CELL](INVALID_PAGE, INVALID_PAGE,
        INVALID_PAGE, INVALID_PAGE);
    uint counts[PAGES_PER_CELL] = uint[PAGES_PER_CELL](0u, 0u, 0u, 0u);
    uint overflow = 0u;

    uint pixel_count = (x1 - x0) * (y1 - y0);
    if (pixel_count > MAX_PIXELS) {
        // A sentinel that cannot be confused with a valid completed cell.
        overflow = INVALID_PAGE;
    } else {
        // Every pixel and every request layer, unlike P31a's 4x4 sample.
        for (uint y = y0; y < y1; ++y) {
            for (uint x = x0; x < x1; ++x) {
                uint count = texelFetch(source_counts, ivec2(x, y), 0).r;
                if (count > layers) {
                    overflow = INVALID_PAGE;
                    continue;
                }
                for (uint layer = 0u; layer < count; ++layer) {
                    uint page = texelFetch(source_pages, ivec3(x, y, int(layer)), 0).r;
                    if (page >= page_count) {
                        overflow = INVALID_PAGE;
                        continue;
                    }
                    bool accounted = false;
                    for (uint k = 0u; k < PAGES_PER_CELL; ++k) {
                        if (pages[k] == page) {
                            ++counts[k];
                            accounted = true;
                            break;
                        }
                    }
                    if (accounted) continue;
                    for (uint k = 0u; k < PAGES_PER_CELL; ++k) {
                        if (pages[k] == INVALID_PAGE) {
                            pages[k] = page;
                            counts[k] = 1u;
                            accounted = true;
                            break;
                        }
                    }
                    if (!accounted && overflow != INVALID_PAGE) ++overflow;
                }
            }
        }
    }
    // Deterministic per-cell record. No global atomics or competing writes.
    for (uint k = 0u; k < PAGES_PER_CELL; ++k) {
        destination.words[offset + k] = pages[k];
        destination.words[offset + PAGES_PER_CELL + k] = counts[k];
    }
    destination.words[offset + PAGES_PER_CELL * 2u] = overflow;
}
