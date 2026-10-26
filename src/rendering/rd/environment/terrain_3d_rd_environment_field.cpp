#include "rendering/rd/environment/terrain_3d_rd_environment_field.h"
#include "rendering/rd/shaders/terrain_3d_rd_shader_library.h"
#include "rendering/rd/infrastructure/descriptors/terrain_3d_rd_binding_builder.h"

#include <godot_cpp/classes/rd_sampler_state.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <math.h>
#include <string.h>

namespace
{
    static const uint32_t s_nDimensions[3] = { 512u, 256u, 256u };
    static const uint32_t s_nBytesPerTexel[3] = { 4u, 8u, 8u };

    // Matches the 3-vec4 GLSL push constant block.
    struct t3dEnvironmentPush_t
    {
        float m_fCurrentX;
        float m_fCurrentZ;
        float m_fPreviousX;
        float m_fPreviousZ;
        float m_fRain;
        float m_fSnow;
        float m_fDry;
        float m_fDt;
        uint32_t m_nReadPrevious;
        uint32_t m_nImpulseCount;
        uint32_t m_nEnabled;
        uint32_t m_nReserved;
    };
    static_assert( sizeof( t3dEnvironmentPush_t ) == 48u, "Environment compute push ABI" );
}

Terrain3DRdEnvironmentField::Terrain3DRdEnvironmentField() :
    m_context( Terrain3DRdContext::getShared() ),
    m_computeSets( m_context, T3D_RD_CONTEXT_OWNER_TERRAIN ) {
}

bool Terrain3DRdEnvironmentField::createFieldCold( const uint32_t nStage, const uint32_t nIndex ) {
    const uint32_t dimension = s_nDimensions[nStage];
    t3dRdTextureSpec_t spec{};
    spec.m_name = String( "T3D.Environment." ) + String( nStage == 0u ? "State" : ( nStage == 1u ? "Forces" : "Height" ) ) +
                  String::num_uint64( nIndex );
    spec.m_owner = T3D_RD_CONTEXT_OWNER_TERRAIN;
    spec.m_memory = T3D_RD_CONTEXT_MEMORY_TERRAIN_TEXTURE;
    spec.m_bFailureFatal = false; // Optional renderer extension.
    spec.m_layout.m_format = nStage == 0u ? RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM :
                              RenderingDevice::DATA_FORMAT_R16G16B16A16_SFLOAT;
    spec.m_layout.m_type = nStage == 2u ? RenderingDevice::TEXTURE_TYPE_2D_ARRAY : RenderingDevice::TEXTURE_TYPE_2D;
    spec.m_layout.m_nWidth = dimension;
    spec.m_layout.m_nHeight = dimension;
    spec.m_layout.m_nDepth = 1u;
    spec.m_layout.m_nLayers = nStage == 2u ? 2u : 1u;
    spec.m_layout.m_nMipmaps = 1u;
    spec.m_layout.m_nUsageBits = RenderingDevice::TEXTURE_USAGE_STORAGE_BIT |
                                   RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT;
    spec.m_layout.m_nBytes = ( uint64_t ) dimension * dimension * s_nBytesPerTexel[nStage] * spec.m_layout.m_nLayers;
    return m_szField[nStage][nIndex].createTracked( m_context, spec );
}

bool Terrain3DRdEnvironmentField::createComputeSetCold( const uint32_t nStage, const uint32_t nIndex ) {
    const uint32_t other = nIndex ^ 1u;
    const RID shader = m_szShader[nStage].rid();
    const RID source = m_szField[nStage][nIndex].rid();
    const RID destination = m_szField[nStage][other].rid();
    TypedArray< Ref< RDUniform > > uniforms;
    uniforms.push_back( Terrain3DRdBindingBuilder::image( 0, source ) );
    uniforms.push_back( Terrain3DRdBindingBuilder::image( 1, destination ) );
    if ( nStage == 0u ) {
        uniforms.push_back( Terrain3DRdBindingBuilder::storage( 2, m_regionClimate.rid() ) );
    } else {
        uniforms.push_back( Terrain3DRdBindingBuilder::storage( 2, m_impulses.rid() ) );
    }
    const t3dComputeKey_t key{ shader, source, destination };
    const String name = String( "T3D.Environment.Compute.Set." ) + String::num_uint64( nStage * 2u + nIndex );
    m_szComputeSet[nStage][nIndex] = m_computeSets.getOrCreate( key, name, uniforms, shader, 0u );
    return m_szComputeSet[nStage][nIndex].is_valid();
}

bool Terrain3DRdEnvironmentField::initializeCold() {
    if ( m_bInitialized ) {
        return !m_bFaulted;
    }
    if ( m_bAttempted ) {
        return false;
    }
    m_bAttempted = true;
    RenderingDevice *const rd = m_context.getDevice();
    if ( !rd ) {
        return false;
    }
    // Optional environment resources cannot poison the shared WORLD RD context.
    struct t3dEnvironmentRecoverableScope_t {
        Terrain3DRdContext &m_context;
        explicit t3dEnvironmentRecoverableScope_t( Terrain3DRdContext &rContext ) : m_context( rContext ) {
            m_context.beginRecoverableFailureScope();
        }
        ~t3dEnvironmentRecoverableScope_t() { m_context.endRecoverableFailureScope(); }
    } recoverable( m_context );
    for ( uint32_t stage = 0u; stage < 3u; ++stage ) {
        const t3dRdShaderId_t id = stage == 0u ? T3D_RD_SHADER_ENVIRONMENT_STATE :
            ( stage == 1u ? T3D_RD_SHADER_ENVIRONMENT_FORCE : T3D_RD_SHADER_ENVIRONMENT_HEIGHT );
        if ( !m_szShader[stage].createTracked( m_context, T3D_RD_CONTEXT_OWNER_TERRAIN,
                                               stage == 0u ? "T3D.Environment.State.Shader" : ( stage == 1u ? "T3D.Environment.Force.Shader" : "T3D.Environment.Height.Shader" ), id ) ||
             !m_szPipeline[stage].createTrackedCompute( m_context, T3D_RD_CONTEXT_OWNER_TERRAIN,
                 stage == 0u ? "T3D.Environment.State.Pipeline" : ( stage == 1u ? "T3D.Environment.Force.Pipeline" : "T3D.Environment.Height.Pipeline" ),
                 m_szShader[stage].rid() ) ) {
            shutdownCold();
            return false;
        }
        if ( !createFieldCold( stage, 0u ) || !createFieldCold( stage, 1u ) ) {
            shutdownCold();
            return false;
        }
    }
    Ref< RDSamplerState > sampler;
    sampler.instantiate();
    sampler->set_mag_filter( RenderingDevice::SAMPLER_FILTER_LINEAR );
    sampler->set_min_filter( RenderingDevice::SAMPLER_FILTER_LINEAR );
    sampler->set_repeat_u( RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE );
    sampler->set_repeat_v( RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE );
    if ( !m_sampler.createTracked( m_context, T3D_RD_CONTEXT_OWNER_TERRAIN,
                                   "T3D.Environment.Sampler", sampler ) ) {
        shutdownCold();
        return false;
    }
    Ref< RDSamplerState > heightSampler;
    heightSampler.instantiate();
    heightSampler->set_mag_filter( RenderingDevice::SAMPLER_FILTER_NEAREST );
    heightSampler->set_min_filter( RenderingDevice::SAMPLER_FILTER_NEAREST );
    heightSampler->set_repeat_u( RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE );
    heightSampler->set_repeat_v( RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE );
    if ( !m_heightSampler.createTracked( m_context, T3D_RD_CONTEXT_OWNER_TERRAIN,
                                         "T3D.Environment.HeightSampler", heightSampler ) ) {
        shutdownCold();
        return false;
    }
    t3dRdGpuBufferSpec_t bufferSpec{};
    bufferSpec.m_pName = "T3D.Environment.Impulses";
    bufferSpec.m_owner = T3D_RD_CONTEXT_OWNER_TERRAIN;
    bufferSpec.m_type = T3D_RD_CONTEXT_RESOURCE_STORAGE_BUFFER;
    bufferSpec.m_memory = T3D_RD_CONTEXT_MEMORY_STORAGE;
    bufferSpec.m_growth = T3D_GPU_FIXED;
    if ( !m_impulses.configure( bufferSpec ) ||
         !m_impulses.createTracked( m_context, T3D_ENVIRONMENT_MAX_IMPULSES * sizeof( t3dEnvironmentImpulse_t ) ) ) {
        shutdownCold();
        return false;
    }
    bufferSpec.m_pName = "T3D.Environment.RegionClimate";
    if ( !m_regionClimate.configure( bufferSpec ) ||
         !m_regionClimate.createTracked( m_context, T3D_REGION_SLOTS * sizeof( uint32_t ) ) ) {
        shutdownCold();
        return false;
    }
    bufferSpec.m_pName = "T3D.Environment.Params";
    bufferSpec.m_type = T3D_RD_CONTEXT_RESOURCE_UNIFORM_BUFFER;
    if ( !m_params.configure( bufferSpec ) || !m_params.createTracked( m_context, sizeof( m_paramsData ) ) ) {
        shutdownCold();
        return false;
    }
    m_pushBytes.resize( sizeof( t3dEnvironmentPush_t ) );
    m_uploadFallback.resize( T3D_ENVIRONMENT_MAX_IMPULSES * sizeof( t3dEnvironmentImpulse_t ) );
    for ( uint32_t stage = 0u; stage < 3u; ++stage ) {
        for ( uint32_t ping = 0u; ping < 2u; ++ping ) {
            if ( !createComputeSetCold( stage, ping ) ) {
                shutdownCold();
                return false;
            }
        }
    }
    m_bFaulted = false;
    m_bInitialized = true;
    return true;
}

void Terrain3DRdEnvironmentField::shutdownCold() {
    m_computeSets.clear();
    for ( uint32_t stage = 0u; stage < 3u; ++stage ) {
        for ( uint32_t index = 0u; index < 2u; ++index ) {
            m_szComputeSet[stage][index] = RID();
            m_szField[stage][index].releaseTracked( m_context );
        }
        m_szPipeline[stage].releaseTracked( m_context );
        m_szShader[stage].releaseTracked( m_context );
    }
    m_params.releaseTracked( m_context );
    m_regionClimate.releaseTracked( m_context );
    m_impulses.releaseTracked( m_context );
    m_heightSampler.releaseTracked( m_context );
    m_sampler.releaseTracked( m_context );
    m_bInitialized = false;
    m_bPreviousValid = false;
    m_bFaulted = false;
    m_nLastRenderFrame = UINT64_MAX;
    m_nLastImpulseEpoch = 0u;
    m_nLastRegionClimateEpoch = UINT64_MAX;
    m_nLastRegionOverrideEpoch = 0u;
    m_nFieldGeneration = 0u;
    m_nPing = 0u;
    m_paramsData = t3dRdEnvironmentParams_t{};
    // Keep a failed initialization latched until a full renderer lifecycle reset.
    // Avoid attempting expensive resource creation each frame on an unsupported device.
}

void Terrain3DRdEnvironmentField::resetAfterRendererTeardownCold() {
    shutdownCold();
    m_bAttempted = false;
}

bool Terrain3DRdEnvironmentField::dispatchStageHot(
    const uint32_t nStage, const uint32_t nSource, const uint32_t nDestination,
    const t3dEnvironmentForcing_t &rForcing, const uint32_t nImpulseCount,
    const bool bClimateOverride, const float fOriginX, const float fOriginZ, const float fPreviousX, const float fPreviousZ ) {
    RenderingDevice *const rd = m_context.getDevice();
    const RID descriptor = m_computeSets.resolve( m_szComputeSet[nStage][nSource] );
    if ( !rd || !descriptor.is_valid() || !m_szPipeline[nStage].isValid() ) {
        return false;
    }
    t3dEnvironmentPush_t push{};
    push.m_fCurrentX = fOriginX;
    push.m_fCurrentZ = fOriginZ;
    push.m_fPreviousX = fPreviousX;
    push.m_fPreviousZ = fPreviousZ;
    push.m_fRain = rForcing.m_fRainRate;
    push.m_fSnow = rForcing.m_fSnowRate;
    push.m_fDry = rForcing.m_fDryRate;
    push.m_fDt = CLAMP( rForcing.m_fDeltaTime, 0.0f, 0.1f );
    push.m_nReadPrevious = m_bPreviousValid ? 1u : 0u;
    push.m_nImpulseCount = nImpulseCount;
    push.m_nEnabled = ( rForcing.m_nFlags & 1u ) != 0u ? 1u : 0u;
    push.m_nReserved = nStage == 0u && bClimateOverride ? 1u : 0u;
    memcpy( m_pushBytes.ptrw(), &push, sizeof( push ) );
    const int64_t list = rd->compute_list_begin();
    if ( list < 0 ) {
        return false;
    }
    rd->compute_list_bind_compute_pipeline( list, m_szPipeline[nStage].rid() );
    rd->compute_list_bind_uniform_set( list, descriptor, 0u );
    rd->compute_list_set_push_constant( list, m_pushBytes, sizeof( push ) );
    const uint32_t workgroups = ( s_nDimensions[nStage] + 7u ) / 8u;
    rd->compute_list_dispatch( list, workgroups, workgroups, 1u );
    rd->compute_list_end();
    return true;
}

void Terrain3DRdEnvironmentField::advanceFrameHot(
    const t3dRdFrameSnapshot_t &rSnapshot, const t3dRdForwardPassContext_t &rContext ) {
    if ( m_nLastRenderFrame == rContext.m_nRenderFrame ) {
        return;
    }
    // Cold materialization is performed by the forward preparer before hot prepare.
    if ( !m_bInitialized || m_bFaulted ) {
        return;
    }
    const bool inject = rSnapshot.m_nEnvironmentImpulseCount > 0u &&
                        rSnapshot.m_nEnvironmentImpulseEpoch != m_nLastImpulseEpoch;
    const uint32_t impulseCount = inject ? rSnapshot.m_nEnvironmentImpulseCount : 0u;
    if ( impulseCount > 0u && !m_impulses.writeTrackedForOwner( m_context, 0u,
            rSnapshot.m_szEnvironmentImpulses, impulseCount * sizeof( t3dEnvironmentImpulse_t ),
            m_uploadFallback, T3D_RD_CONTEXT_OWNER_TERRAIN, T3D_GPU_HOT_FRAME ) ) {
        // Preserve the last complete generation for already-cached WORLD shaders.
        // A failed first update publishes nothing and uses the legacy terrain path.
        ++m_nIntegrationFailures;
        m_bFaulted = true;
        return;
    }
    if ( rSnapshot.m_nEnvironmentRegionClimateEpoch != m_nLastRegionClimateEpoch ) {
        if ( !m_regionClimate.writeTrackedForOwner( m_context, 0u,
                rSnapshot.m_szEnvironmentRegionClimate,
                T3D_REGION_SLOTS * sizeof( uint32_t ), m_uploadFallback,
                T3D_RD_CONTEXT_OWNER_TERRAIN, T3D_GPU_HOT_FRAME ) ) {
            ++m_nIntegrationFailures;
            m_bFaulted = true;
            return;
        }
        m_nLastRegionClimateEpoch = rSnapshot.m_nEnvironmentRegionClimateEpoch;
        ++m_nClimateUploads;
    }
    const bool climateOverride = rSnapshot.m_nEnvironmentRegionOverrideEpoch != m_nLastRegionOverrideEpoch;
    const uint32_t source = m_nPing;
    const uint32_t destination = source ^ 1u;
    t3dRdEnvironmentParams_t next = m_paramsData;
    next.m_fStateOriginX = floorf( ( float ) rContext.m_cameraTransform.origin.x - 256.0f );
    next.m_fStateOriginZ = floorf( ( float ) rContext.m_cameraTransform.origin.z - 256.0f );
    next.m_fStateInvSpanX = 1.0f / 512.0f;
    next.m_fStateInvSpanZ = 1.0f / 512.0f;
    next.m_fForceOriginX = floorf( ( float ) rContext.m_cameraTransform.origin.x * 2.0f ) * 0.5f - 64.0f;
    next.m_fForceOriginZ = floorf( ( float ) rContext.m_cameraTransform.origin.z * 2.0f ) * 0.5f - 64.0f;
    next.m_fForceInvSpanX = 1.0f / 128.0f;
    next.m_fForceInvSpanZ = 1.0f / 128.0f;
    const t3dEnvironmentForcing_t &forcing = rSnapshot.m_environmentForcing;
    const bool oldValid = m_bPreviousValid;
    const bool historyValid = m_bPreviousValid &&
        fabsf( next.m_fStateOriginX - m_paramsData.m_fStateOriginX ) < 512.0f &&
        fabsf( next.m_fStateOriginZ - m_paramsData.m_fStateOriginZ ) < 512.0f &&
        fabsf( next.m_fForceOriginX - m_paramsData.m_fForceOriginX ) < 128.0f &&
        fabsf( next.m_fForceOriginZ - m_paramsData.m_fForceOriginZ ) < 128.0f;
    if ( m_bPreviousValid && !historyValid ) {
        ++m_nTeleportsReset;
    }
    m_bPreviousValid = historyValid;
    next.m_fPreviousStateOriginX = m_paramsData.m_fStateOriginX;
    next.m_fPreviousStateOriginZ = m_paramsData.m_fStateOriginZ;
    next.m_fPreviousForceOriginX = m_paramsData.m_fForceOriginX;
    next.m_fPreviousForceOriginZ = m_paramsData.m_fForceOriginZ;
    next.m_fPreviousWindX = m_paramsData.m_fWindX;
    next.m_fPreviousWindZ = m_paramsData.m_fWindZ;
    next.m_fPreviousGust = m_paramsData.m_fGust;
    next.m_fPreviousWindTime = m_paramsData.m_fWindTime;
    next.m_fWindX = CLAMP( forcing.m_fWindX, -1.0f, 1.0f );
    next.m_fWindZ = CLAMP( forcing.m_fWindZ, -1.0f, 1.0f );
    next.m_fGust = CLAMP( forcing.m_fGustStrength, 0.0f, 1.0f );
    next.m_fWindTime = fmodf( next.m_fPreviousWindTime + CLAMP( forcing.m_fDeltaTime, 0.0f, 0.1f ), 4096.0f );
    next.m_fHistoryValid = historyValid ? 1.0f : 0.0f;
    next.m_fSeaLevel = rSnapshot.m_ocean.m_fSeaLevel;
    next.m_fPad = rSnapshot.m_ocean.m_nEnabled != 0u ? 1.0f : 0.0f;
    next.m_fEnabled = ( forcing.m_nFlags & 1u ) != 0u ? 1.0f : 0.0f;
    // Reuse the renderer's existing opt-in GPU marker policy. No GPU readback.
    RenderingDevice *const rd = m_context.getDevice();
    const bool captureTiming = rd && Terrain3DRdShaderLibrary::isTerrainGpuTimestampEnabledCold();
    if ( captureTiming ) {
        rd->capture_timestamp( "T3D.Environment.Compute.Begin" );
    }
    const bool stateOk = dispatchStageHot( 0u, source, destination, forcing, impulseCount, climateOverride,
                                           next.m_fStateOriginX, next.m_fStateOriginZ,
                                           m_paramsData.m_fStateOriginX, m_paramsData.m_fStateOriginZ );
    const bool forceOk = stateOk && dispatchStageHot( 1u, source, destination, forcing, impulseCount, false,
                                                     next.m_fForceOriginX, next.m_fForceOriginZ,
                                                     m_paramsData.m_fForceOriginX, m_paramsData.m_fForceOriginZ );
    const bool heightOk = forceOk && dispatchStageHot( 2u, source, destination, forcing, impulseCount, false,
                                                       next.m_fForceOriginX, next.m_fForceOriginZ,
                                                       m_paramsData.m_fForceOriginX, m_paramsData.m_fForceOriginZ );
    if ( captureTiming ) {
        rd->capture_timestamp( "T3D.Environment.Compute.End" );
        ++m_nTimestampPairs;
    }
    if ( !heightOk ||
         !m_params.writeTrackedForOwner( m_context, 0u, &next, sizeof( next ),
                                         m_uploadFallback, T3D_RD_CONTEXT_OWNER_TERRAIN, T3D_GPU_HOT_FRAME ) ) {
        // Prevent a later pass from observing a half-published field generation.
        // Cold teardown belongs to renderer shutdown/recovery, not this hot callback.
        // Roll back availability to the last fully published ping on failed updates.
        // The abandoned destination ping cannot be observed by any raster consumer.
        m_bPreviousValid = oldValid;
        ++m_nIntegrationFailures;
        m_bFaulted = true;
        return;
    }
    m_paramsData = next;
    m_nPing = destination;
    m_nLastRenderFrame = rContext.m_nRenderFrame;
    m_bPreviousValid = true;
    ++m_nFieldGeneration;
    ++m_nFramesIntegrated;
    m_nImpulsesAccepted += impulseCount;
    m_nLastRegionOverrideEpoch = rSnapshot.m_nEnvironmentRegionOverrideEpoch;
    if ( inject ) {
        m_nLastImpulseEpoch = rSnapshot.m_nEnvironmentImpulseEpoch;
    }
}

bool Terrain3DRdEnvironmentField::acquireBindingsHot( t3dRdEnvironmentBindings_t &rBindings ) const {
    rBindings = t3dRdEnvironmentBindings_t{};
    if ( !isReady() ) {
        return false;
    }
    rBindings.m_hState = m_szField[0][m_nPing].rid();
    rBindings.m_hStatePair[0] = m_szField[0][0].rid();
    rBindings.m_hStatePair[1] = m_szField[0][1].rid();
    rBindings.m_nPing = m_nPing;
    rBindings.m_hInteraction = m_szField[1][m_nPing].rid();
    rBindings.m_hInteractionPair[0] = m_szField[1][0].rid();
    rBindings.m_hInteractionPair[1] = m_szField[1][1].rid();
    rBindings.m_hHeightPair[0] = m_szField[2][0].rid();
    rBindings.m_hHeightPair[1] = m_szField[2][1].rid();
    rBindings.m_hSampler = m_sampler.rid();
    rBindings.m_hHeightSampler = m_heightSampler.rid();
    rBindings.m_hParams = m_params.rid();
    rBindings.m_nGeneration = m_nFieldGeneration;
    rBindings.m_bAvailable = rBindings.m_hState.is_valid() && rBindings.m_hParams.is_valid() &&
                             rBindings.m_hSampler.is_valid() && rBindings.m_hHeightSampler.is_valid() &&
                             rBindings.m_hInteraction.is_valid() &&
                             rBindings.m_hInteractionPair[0].is_valid() && rBindings.m_hInteractionPair[1].is_valid() &&
                             rBindings.m_hHeightPair[0].is_valid() && rBindings.m_hHeightPair[1].is_valid();
    return rBindings.m_bAvailable;
}
