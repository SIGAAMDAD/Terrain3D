// Copyright 2026 Terrain3D contributors.
#include "rendering/rd/forward/terrain_3d_rd_cluster_compact.h"
#include "rendering/rd/infrastructure/descriptors/terrain_3d_rd_binding_builder.h"
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/classes/rd_uniform.hpp>
#include <string.h>

using namespace godot;

Terrain3DRdClusterCompact::Terrain3DRdClusterCompact( Terrain3DRdContext &context ) :
	m_context( context ),
	m_computeSets( context, T3D_RD_CONTEXT_OWNER_FORWARD_LIGHTING ),
	m_rasterSets( context, T3D_RD_CONTEXT_OWNER_FORWARD_LIGHTING ) {
	m_pushBytes.resize( sizeof( t3dBr21Push_t ) );
}

Terrain3DRdClusterCompact::~Terrain3DRdClusterCompact() { shutdown(); }

Terrain3DRdClusterCompact::DescriptorKey Terrain3DRdClusterCompact::makeKey(
	const RID &shader, const RID &borrowed ) const {
	DescriptorKey key{};
	key.m_hShader = shader;
	key.m_hSource = borrowed;
	key.m_nGeneration = m_context.getDeviceGeneration();
	for ( uint32_t i = 0u; i < 6u; ++i ) key.m_hBuffers[i] = m_buffers[i].rid();
	return key;
}

bool Terrain3DRdClusterCompact::ensureResources( uint32_t lists ) {
	RenderingDevice *const rd = m_context.getDevice();
	if ( !rd || !lists ) return false;
	if ( !m_bConfigured ) {
		static const char *const names[6] = {
			"T3D.BR21.Counts", "T3D.BR21.Prefix", "T3D.BR21.GroupSums",
			"T3D.BR21.GroupBases", "T3D.BR21.Headers", "T3D.BR21.Indices" };
		for ( uint32_t i = 0u; i < 6u; ++i ) {
			t3dRdGpuBufferSpec_t spec{};
			spec.m_pName = names[i];
			spec.m_owner = T3D_RD_CONTEXT_OWNER_FORWARD_LIGHTING;
			spec.m_type = T3D_RD_CONTEXT_RESOURCE_STORAGE_BUFFER;
			spec.m_memory = T3D_RD_CONTEXT_MEMORY_STORAGE;
			if ( !m_buffers[i].configure( spec ) ) return false;
		}
		m_bConfigured = true;
	}
	if ( !m_hShader.isValid() ) {
		if ( !m_hShader.createTracked( m_context, T3D_RD_CONTEXT_OWNER_FORWARD_LIGHTING,
			"T3D.BR21.CompactShader", T3D_RD_SHADER_CLUSTER_COMPACT ) ||
			 !m_hPipeline.createTrackedCompute( m_context, T3D_RD_CONTEXT_OWNER_FORWARD_LIGHTING,
			"T3D.BR21.CompactPipeline", m_hShader.rid() ) ) return false;
	}
	const uint32_t groups = ( lists + T3D_BR21_SCAN_GROUP_SIZE - 1u ) / T3D_BR21_SCAN_GROUP_SIZE;
	const uint32_t bytes[6] = { lists * 4u, lists * 4u, groups * 4u,
		groups * 4u, lists * ( uint32_t ) sizeof( t3dBr21Header_t ), T3D_BR21_INDEX_CAPACITY * 4u };
	bool replaced = false;
	for ( uint32_t i = 0u; i < 6u; ++i ) {
		if ( m_buffers[i].needsCapacity( bytes[i] ) ) {
			if ( !m_buffers[i].ensureTracked( m_context, bytes[i] ) ) return false;
			replaced = true;
		}
	}
	if ( replaced ) {
		m_computeSets.clear();
		m_rasterSets.clear();
	}
	return true;
}

bool Terrain3DRdClusterCompact::ensureFallbackResources() {
	if ( !m_context.getDevice() ) return false;
	if ( !m_bFallbackConfigured ) {
		t3dRdGpuBufferSpec_t spec{};
		spec.m_owner = T3D_RD_CONTEXT_OWNER_FORWARD_LIGHTING;
		spec.m_type = T3D_RD_CONTEXT_RESOURCE_STORAGE_BUFFER;
		spec.m_memory = T3D_RD_CONTEXT_MEMORY_STORAGE;
		spec.m_pName = "T3D.BR21.FallbackHeaders";
		if ( !m_hFallbackHeaders.configure( spec ) ) return false;
		spec.m_pName = "T3D.BR21.FallbackIndices";
		if ( !m_hFallbackIndices.configure( spec ) ) return false;
		m_bFallbackConfigured = true;
	}
	PackedByteArray zeros;
	zeros.resize( sizeof( t3dBr21Header_t ) );
	memset( zeros.ptrw(), 0, zeros.size() );
	if ( !m_hFallbackHeaders.isValid() && !m_hFallbackHeaders.createTracked( m_context, zeros.size(), zeros ) )
		return false;
	if ( !m_hFallbackIndices.isValid() ) {
		zeros.resize( sizeof( uint32_t ) );
		memset( zeros.ptrw(), 0, zeros.size() );
		if ( !m_hFallbackIndices.createTracked( m_context, zeros.size(), zeros ) ) return false;
	}
	return true;
}

RID Terrain3DRdClusterCompact::ensureComputeSet( const RID &borrowed ) {
	const DescriptorKey key = makeKey( m_hShader.rid(), borrowed );
	TypedArray< Ref< RDUniform > > uniforms;
	uniforms.push_back( Terrain3DRdBindingBuilder::storage( 0u, borrowed ) );
	for ( uint32_t i = 0u; i < 6u; ++i )
		uniforms.push_back( Terrain3DRdBindingBuilder::storage( i + 1u, m_buffers[i].rid() ) );
	return m_computeSets.getOrCreate( key, "T3D.BR21.ComputeSet", uniforms, m_hShader.rid(), 0u );
}

bool Terrain3DRdClusterCompact::prepare( const t3dRdForwardPassContext_t &pass ) {
	m_bReady = false;
	if ( pass.m_phase != T3D_RD_FORWARD_PHASE_PREPARE ||
		 ( pass.m_passKind != T3D_RD_FORWARD_PASS_OPAQUE && pass.m_passKind != T3D_RD_FORWARD_PASS_MOTION ) )
		return false;
	RenderingDevice *const rd = m_context.getDevice();
	if ( !rd || !pass.m_hClusterBuffer.is_valid() ) return false;
	const uint64_t generation = m_context.getDeviceGeneration();
	if ( m_nDeviceGeneration && m_nDeviceGeneration != generation ) {
		m_computeSets.abandon(); m_rasterSets.abandon();
		m_hShader.abandon(); m_hPipeline.abandon();
		for ( uint32_t i = 0u; i < 6u; ++i ) m_buffers[i].abandon();
		m_hFallbackHeaders.abandon();
		m_hFallbackIndices.abandon();
		m_nPreparedFrame = 0u;
	}
	m_nDeviceGeneration = generation;
	const uint32_t words = pass.m_nClusterMaxElementsDiv32;
	const uint32_t stride = words + T3D_BR21_DEPTH_SLICES;
	if ( words == 0u || words > 64u || pass.m_nClusterTypeSize < stride ||
		 pass.m_nClusterTypeSize % stride || pass.m_nClusterTypeSize > UINT32_MAX / T3D_BR21_KIND_COUNT ) return false;
	const uint32_t clusters = pass.m_nClusterTypeSize / stride;
	if ( clusters == 0u || clusters > T3D_BR21_MAX_CLUSTERS ||
		 pass.m_nClusterWidth == 0u || clusters % pass.m_nClusterWidth ) return false;
	const uint32_t lists = clusters * T3D_BR21_LISTS_PER_CLUSTER;
	m_context.beginResourceRebuild();
	const bool resources_ok = ensureResources( lists );
	m_context.endResourceRebuild();
	if ( !resources_ok ) return false;
	if ( m_nPreparedFrame == pass.m_nFrameGeneration &&
		m_hLastBorrowed == pass.m_hClusterBuffer && m_nBorrowedGeneration == pass.m_nResourceGeneration &&
		m_nListCount == lists && m_nLastTypeStride == pass.m_nClusterTypeSize &&
		m_nLastMaxWords == words ) {
		m_bReady = true;
		return true;
	}
	m_context.beginResourceRebuild();
	const RID set = ensureComputeSet( pass.m_hClusterBuffer );
	m_context.endResourceRebuild();
	if ( !set.is_valid() ) return false;
	t3dBr21Push_t push{};
	push.m_nListCount = lists;
	push.m_nClusterCount = clusters;
	push.m_nIndexCapacity = T3D_BR21_INDEX_CAPACITY;
	push.m_nMaxWords = words;
	push.m_nTypeStride = pass.m_nClusterTypeSize;
	push.m_nClusterWidth = pass.m_nClusterWidth;
	push.m_nNativeWordCount = pass.m_nClusterTypeSize * T3D_BR21_KIND_COUNT;
	const int64_t compute = rd->compute_list_begin();
	if ( compute == RenderingDevice::INVALID_ID ) return false;
	rd->compute_list_bind_compute_pipeline( compute, m_hPipeline.rid() );
	rd->compute_list_bind_uniform_set( compute, set, 0u );
	for ( uint32_t phase = 0u; phase < 4u; ++phase ) {
		push.m_nPhase = phase;
		memcpy( m_pushBytes.ptrw(), &push, sizeof( push ) );
		rd->compute_list_set_push_constant( compute, m_pushBytes, sizeof( push ) );
		rd->compute_list_dispatch( compute,
			phase == 2u ? 1u : ( lists + T3D_BR21_SCAN_GROUP_SIZE - 1u ) / T3D_BR21_SCAN_GROUP_SIZE,
			1u, 1u );
		rd->compute_list_add_barrier( compute );
	}
	rd->compute_list_end();
	m_hLastBorrowed = pass.m_hClusterBuffer;
	m_nBorrowedGeneration = pass.m_nResourceGeneration;
	m_nPreparedFrame = pass.m_nFrameGeneration;
	m_nClusterCount = clusters;
	m_nListCount = lists;
	m_nLastTypeStride = pass.m_nClusterTypeSize;
	m_nLastMaxWords = words;
	m_bReady = true;
	return true;
}

RID Terrain3DRdClusterCompact::ensureRasterSet( const RID &shader ) {
	if ( !shader.is_valid() ) return RID();
	m_context.beginResourceRebuild();
	const bool fallback_ready = ensureFallbackResources();
	if ( !fallback_ready ) {
		m_context.endResourceRebuild();
		return RID();
	}
	const RID headers = m_bReady ? m_buffers[4].rid() : m_hFallbackHeaders.rid();
	const RID indices = m_bReady ? m_buffers[5].rid() : m_hFallbackIndices.rid();
	DescriptorKey key = makeKey( shader, RID() );
	key.m_hBuffers[4] = headers;
	key.m_hBuffers[5] = indices;
	TypedArray< Ref< RDUniform > > uniforms;
	uniforms.push_back( Terrain3DRdBindingBuilder::storage( 0u, headers ) );
	uniforms.push_back( Terrain3DRdBindingBuilder::storage( 1u, indices ) );
	const RID set = m_rasterSets.getOrCreate( key, "T3D.BR21.RasterSet", uniforms, shader, 4u );
	m_context.endResourceRebuild();
	return set;
}

void Terrain3DRdClusterCompact::invalidate() {
	m_bReady = false;
	m_nPreparedFrame = 0u;
}

void Terrain3DRdClusterCompact::shutdown() {
	m_computeSets.clear(); m_rasterSets.clear();
	m_hPipeline.releaseTracked( m_context );
	m_hShader.releaseTracked( m_context );
	for ( uint32_t i = 0u; i < 6u; ++i ) m_buffers[i].releaseTracked( m_context );
	m_hFallbackHeaders.releaseTracked( m_context );
	m_hFallbackIndices.releaseTracked( m_context );
	m_bReady = false;
	m_nPreparedFrame = 0u;
	m_nDeviceGeneration = 0u;
}
