// VT ancestor hints: standalone POD oracle, without loading the Godot runtime.
// Mirrors the fixed structs in Terrain3DTypes.h. The production header is
// also compiled with the actual Godot structs by the native syntax gate.
#include <cstdint>
#include <cstddef>
#include <cstdio>
#define TERRAIN3D_TYPES_H
#define TERRAIN3D_VT_CONTRACT_H
#define T3D_VT_MAX_MIPS 16u
#define T3D_VT_ANCESTOR_PROBE_CAP 16u
#define T3D_VT_INVALID_PAGE 0xffffffffu
#define T3D_VT_INVALID_SLOT 0xffffffffu
#define T3D_VT_PAGE_VALID (1u << 0u)
#define T3D_VT_PAGE_RESIDENT (1u << 1u)
#define T3D_VT_PAGE_TAIL_FALLBACK (1u << 5u)
#define T3D_VT_ALLOCATION_QUALITY_DISABLED (1u << 13u)
typedef struct t3dVTAllocation_s {
    uint32_t m_nBaseVirtualPage;
    uint32_t m_nPageCount;
    uint16_t m_nMip0WidthPages;
    uint16_t m_nMip0HeightPages;
    uint16_t m_nMipCount;
    uint16_t m_nFlags;
    uint32_t m_nTailIndex;
    uint32_t m_nGeneration;
    uint32_t m_nContentId;
    uint32_t m_nReserved;
} t3dVTAllocation_t;
typedef struct t3dVTPageTableEntry_s {
    uint32_t nPhysicalSlot;
    uint32_t nParentPage;
    uint32_t nGeneration;
    uint16_t nResolvedMip;
    uint16_t nFlags;
} t3dVTPageTableEntry_t;
static_assert(sizeof(t3dVTAllocation_t) == 32u, "allocation ABI");
static_assert(sizeof(t3dVTPageTableEntry_t) == 16u, "page ABI");
#include "rendering/rd/vt/core/terrain_3d_vt_ancestor_hint.h"

static uint32_t s_rng = 0x48ab4c16u;
static uint32_t next_random()
{
	s_rng ^= s_rng << 13;
	s_rng ^= s_rng >> 17;
	s_rng ^= s_rng << 5;
	return s_rng;
}

static bool reference_walk( const t3dVTAllocation_t &allocation,
	const t3dVTPageTableEntry_t *pages, uint32_t count, uint32_t start,
	uint32_t mip, uint32_t &resolved, uint32_t &depth )
{
	resolved = T3D_VT_INVALID_PAGE;
	depth = 0u;
	if ( !allocation.m_nPageCount || !allocation.m_nMipCount ||
		allocation.m_nMipCount > T3D_VT_MAX_MIPS || mip >= allocation.m_nMipCount ||
		allocation.m_nBaseVirtualPage >= count ||
		allocation.m_nPageCount > count - allocation.m_nBaseVirtualPage ||
		( allocation.m_nFlags & T3D_VT_ALLOCATION_QUALITY_DISABLED ) ||
		start < allocation.m_nBaseVirtualPage ||
		start >= allocation.m_nBaseVirtualPage + allocation.m_nPageCount ) return false;
	uint32_t probe = start;
	uint32_t cap = allocation.m_nMipCount - mip;
	if ( cap > T3D_VT_ANCESTOR_PROBE_CAP ) cap = T3D_VT_ANCESTOR_PROBE_CAP;
	for ( uint32_t index = 0u; index < cap; ++index ) {
		if ( probe < allocation.m_nBaseVirtualPage ||
			probe >= allocation.m_nBaseVirtualPage + allocation.m_nPageCount ) return false;
		const t3dVTPageTableEntry_t &current = pages[probe];
		if ( !( current.nFlags & T3D_VT_PAGE_VALID ) ||
			current.nGeneration != allocation.m_nGeneration ) return false;
		if ( ( current.nFlags & T3D_VT_PAGE_RESIDENT ) &&
			current.nPhysicalSlot != T3D_VT_INVALID_SLOT ) {
			resolved = probe;
			depth = index;
			return true;
		}
		if ( ( current.nFlags & T3D_VT_PAGE_TAIL_FALLBACK ) ||
			current.nParentPage == T3D_VT_INVALID_PAGE ) return false;
		probe = current.nParentPage;
	}
	return false;
}

int main()
{
	const uint32_t count = 16u;
	t3dVTAllocation_t allocation{};
	allocation.m_nBaseVirtualPage = 0u;
	allocation.m_nPageCount = count;
	allocation.m_nMipCount = count;
	allocation.m_nGeneration = 11u;
	t3dVTPageTableEntry_t pages[count]{};
	uint32_t checks = 0u;
	for ( uint32_t test = 0u; test < 4000u; ++test ) {
		allocation.m_nGeneration = 1u + next_random() % 30000u;
		allocation.m_nFlags = ( next_random() % 17u == 0u )
			? T3D_VT_ALLOCATION_QUALITY_DISABLED : 0u;
		for ( uint32_t i = 0u; i < count; ++i ) {
			pages[i] = {};
			pages[i].nFlags = ( next_random() & 31u ) != 0u ? T3D_VT_PAGE_VALID : 0u;
			if ( ( next_random() & 3u ) == 0u ) pages[i].nFlags |= T3D_VT_PAGE_RESIDENT;
			if ( ( next_random() & 31u ) == 0u ) pages[i].nFlags |= T3D_VT_PAGE_TAIL_FALLBACK;
			pages[i].nParentPage = i + 1u < count && ( next_random() & 7u ) != 0u
				? i + 1u + next_random() % ( count - i - 1u ) : T3D_VT_INVALID_PAGE;
			pages[i].nPhysicalSlot = ( next_random() & 7u ) == 0u
				? T3D_VT_INVALID_SLOT : next_random() % 256u;
			pages[i].nGeneration = ( next_random() & 31u ) != 0u
				? allocation.m_nGeneration : allocation.m_nGeneration - 1u;
			pages[i].nResolvedMip = ( uint16_t ) i;
		}
		for ( uint32_t start = 0u; start < count; ++start ) {
			uint32_t page = T3D_VT_INVALID_PAGE;
			uint32_t depth = 0u;
			const bool expected = reference_walk( allocation, pages, count,
				start, start, page, depth );
			t3dVTAncestorHint_t hint;
			const bool found = t3d_vt_build_ancestor_hint_reference(
				allocation, pages, count, start, start, hint );
			if ( expected != found || ( found &&
				( hint.nResolvedPage != page ||
				hint.nPhysicalSlot != pages[page].nPhysicalSlot ||
				( ( hint.nPackedMipDepthFlags >> 16u ) & 0xffu ) != depth ||
				( ( hint.nPackedMipDepthFlags >> 8u ) & 0xffu ) != pages[page].nResolvedMip ||
				hint.nAllocationGeneration != allocation.m_nGeneration ) ) ) {
				std::fprintf( stderr, "FAIL test=%u page=%u expected=%u found=%u\n", test, start, expected, found );
				return 1;
			}
			++checks;
		}
	}
	std::printf( "PASS BR07 CPU reference parity %u cases\n", checks );
	return 0;
}
