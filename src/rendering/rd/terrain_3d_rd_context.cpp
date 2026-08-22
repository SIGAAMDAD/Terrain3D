// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "rendering/rd/terrain_3d_rd_context.h"

#include <godot_cpp/classes/rendering_server.hpp>

Terrain3DRdContext::~Terrain3DRdContext( void ) {
	shutdown();
}

void Terrain3DRdContext::updateBufferPeak( const uint64_t nLiveBytes ) {
	uint64_t peak = m_nPeakBufferBytes.load( std::memory_order_relaxed );
	while ( nLiveBytes > peak &&
			!m_nPeakBufferBytes.compare_exchange_weak( peak, nLiveBytes, std::memory_order_release, std::memory_order_relaxed ) ) {
	}
}

void Terrain3DRdContext::updateTexturePeak( const uint64_t nLiveBytes ) {
	uint64_t peak = m_nPeakTextureBytes.load( std::memory_order_relaxed );
	while ( nLiveBytes > peak &&
			!m_nPeakTextureBytes.compare_exchange_weak( peak, nLiveBytes, std::memory_order_release, std::memory_order_relaxed ) ) {
	}
}

bool Terrain3DRdContext::initialize( void ) {
	if ( m_pRd ) {
		return true;
	}
	RenderingServer *const server = RenderingServer::get_singleton();
	m_pRd = server ? server->get_rendering_device() : nullptr;
	return m_pRd != nullptr;
}

void Terrain3DRdContext::shutdown( void ) {
	m_pRd = nullptr;
}

void Terrain3DRdContext::registerResource( const RID &rRid, const String &rName, const uint64_t nBufferBytes ) {
	if ( !m_pRd || !rRid.is_valid() ) {
		return;
	}
	m_pRd->set_resource_name( rRid, rName );
	m_nCreatedResources.fetch_add( 1u, std::memory_order_relaxed );
	if ( nBufferBytes > 0u ) {
		const uint64_t live = m_nLiveBufferBytes.fetch_add( nBufferBytes, std::memory_order_acq_rel ) + nBufferBytes;
		updateBufferPeak( live );
	}
}


void Terrain3DRdContext::registerTextureResource( const RID &rRid, const String &rName, const uint64_t nTextureBytes ) {
	if ( !m_pRd || !rRid.is_valid() ) return;
	m_pRd->set_resource_name( rRid, rName );
	m_nCreatedResources.fetch_add( 1u, std::memory_order_relaxed );
	if ( nTextureBytes > 0u ) {
		const uint64_t live = m_nLiveTextureBytes.fetch_add( nTextureBytes, std::memory_order_acq_rel ) + nTextureBytes;
		updateTexturePeak( live );
	}
}

void Terrain3DRdContext::freeResource( RID &rRid, const uint64_t nBufferBytes ) {
	if ( !rRid.is_valid() ) {
		return;
	}
	if ( m_pRd ) {
		m_pRd->free_rid( rRid );
	}
	rRid = RID();
	m_nFreedResources.fetch_add( 1u, std::memory_order_relaxed );
	if ( nBufferBytes > 0u ) {
		m_nLiveBufferBytes.fetch_sub( nBufferBytes, std::memory_order_acq_rel );
	}
}


void Terrain3DRdContext::freeTextureResource( RID &rRid, const uint64_t nTextureBytes ) {
	if ( !rRid.is_valid() ) return;
	if ( m_pRd ) m_pRd->free_rid( rRid );
	rRid = RID();
	m_nFreedResources.fetch_add( 1u, std::memory_order_relaxed );
	if ( nTextureBytes > 0u ) m_nLiveTextureBytes.fetch_sub( nTextureBytes, std::memory_order_acq_rel );
}

void Terrain3DRdContext::recordBufferUpdate( const uint64_t nBytes ) {
	m_nBufferUpdates.fetch_add( 1u, std::memory_order_relaxed );
	m_nBytesUploaded.fetch_add( nBytes, std::memory_order_relaxed );
}

Dictionary Terrain3DRdContext::getDiagnostics( void ) const {
	Dictionary result;
	result["resource_creates"] = ( int64_t )( m_nCreatedResources.load( std::memory_order_acquire ) );
	result["resource_frees"] = ( int64_t )( m_nFreedResources.load( std::memory_order_acquire ) );
	result["live_buffer_bytes"] = ( int64_t )( m_nLiveBufferBytes.load( std::memory_order_acquire ) );
	result["peak_buffer_bytes"] = ( int64_t )( m_nPeakBufferBytes.load( std::memory_order_acquire ) );
	result["live_texture_bytes"] = ( int64_t )( m_nLiveTextureBytes.load( std::memory_order_acquire ) );
	result["peak_texture_bytes"] = ( int64_t )( m_nPeakTextureBytes.load( std::memory_order_acquire ) );
	result["buffer_updates"] = ( int64_t )( m_nBufferUpdates.load( std::memory_order_acquire ) );
	result["bytes_uploaded"] = ( int64_t )( m_nBytesUploaded.load( std::memory_order_acquire ) );
	if ( m_pRd ) {
		result["device_name"] = m_pRd->get_device_name();
		result["device_vendor"] = m_pRd->get_device_vendor_name();
		result["device_memory"] = ( int64_t )( m_pRd->get_device_total_memory() );
	}
	return result;
}
