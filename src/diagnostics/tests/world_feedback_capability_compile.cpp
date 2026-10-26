// FIX-01: compile-only WORLD shader capability cache contract.
// The cache must be invalidated when the feedback owner, generation,
// readiness, environment binding or compact shader request changes.
#include "rendering/rd/world/visibility/terrain_3d_world_raster.h"

namespace {
constexpr t3dWorldShaderCapabilityKey_t noFeedback{ nullptr, 3u, false, false, false };
constexpr t3dWorldShaderCapabilityKey_t noChange{ nullptr, 3u, false, false, false };
constexpr t3dWorldShaderCapabilityKey_t feedbackEnabled{ nullptr, 3u, true, false, false };
constexpr t3dWorldShaderCapabilityKey_t newGeneration{ nullptr, 4u, true, false, false };
constexpr t3dWorldShaderCapabilityKey_t feedbackDisabled{ nullptr, 4u, false, false, false };
constexpr t3dWorldShaderCapabilityKey_t environmentReady{ nullptr, 4u, false, true, false };
constexpr t3dWorldShaderCapabilityKey_t compactReady{ nullptr, 4u, false, true, true };

static_assert( noFeedback == noChange, "Steady-state feedback should not rebuild shaders" );
static_assert( !( noFeedback == feedbackEnabled ), "Readiness activation must rebuild shaders" );
static_assert( !( feedbackEnabled == newGeneration ), "Feedback generation must rebuild shaders" );
static_assert( !( newGeneration == feedbackDisabled ), "Readiness loss must rebuild shaders" );
static_assert( !( feedbackDisabled == environmentReady ), "Environment transition must rebuild shaders" );
static_assert( !( environmentReady == compactReady ), "Compact variant change must rebuild shaders" );
}
