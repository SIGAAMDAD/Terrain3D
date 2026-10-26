#ifndef TERRAIN3D_GENERATION_H
#define TERRAIN3D_GENERATION_H
#include <stdint.h>
// Zero is the unpublished sentinel. Revisions wrap to the first published value.
inline constexpr uint64_t t3dNextGeneration(
	const uint64_t nGeneration ) {
	return nGeneration == UINT64_MAX ? 1u : nGeneration + 1u;
}
inline constexpr uint32_t t3dNextGeneration(
	const uint32_t nGeneration ) {
	return nGeneration == UINT32_MAX ? 1u : nGeneration + 1u;
}
#endif // TERRAIN3D_GENERATION_H
