#ifndef TERRAIN3D_RD_RENDER_TARGET_POOL_H
#define TERRAIN3D_RD_RENDER_TARGET_POOL_H

#include "rendering/rd/infrastructure/targets/terrain_3d_rd_framebuffer.h"
#include "rendering/rd/infrastructure/resources/terrain_3d_rd_texture.h"
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/variant/typed_array.hpp>

// Fixed-capacity pool for temporary 2D textures and single-attachment targets.
// Callers release borrowed RIDs before resizing or shutting down the domain.
typedef struct t3dRdRenderTargetKey_s {

		uint32_t width = 0u;
		uint32_t height = 0u;
		RenderingDevice::DataFormat format = RenderingDevice::DATA_FORMAT_MAX;
		uint32_t usage = 0u;
		uint32_t bytesPerPixel = 0u;
		uint32_t historyClass = 0u;
		bool framebuffer = false;
		bool operator==( const t3dRdRenderTargetKey_s &rOther ) const {
			return width == rOther.width && height == rOther.height && format == rOther.format && usage == rOther.usage
			        && bytesPerPixel == rOther.bytesPerPixel && historyClass == rOther.historyClass && framebuffer == rOther.framebuffer;
		}
} t3dRdRenderTargetKey_t;


template <uint32_t Capacity>
class Terrain3DRdRenderTargetPool
{
private:
	typedef struct Entry_s {

		t3dRdRenderTargetKey_t key;
		Terrain3DRdTexture texture;
		Terrain3DRdFramebuffer framebuffer;
		bool inUse = false;
	} Entry;

	static_assert( Capacity > 0u, "Render target pool needs at least one slot" );
	Terrain3DRdContext &m_context;
	t3dRdResourceOwner_t m_owner;
	t3dRdMemoryCategory_t m_memory;
	Entry m_entries[Capacity]{};
	uint32_t m_nEvictCursor = 0u;
	uint64_t m_nDeviceGeneration = 0u;

	static uint64_t bytes( const t3dRdRenderTargetKey_t &rKey ) {
		return ( uint64_t )rKey.width * rKey.height * rKey.bytesPerPixel;
	}
	void freeEntry( Entry &rEntry ) {
		rEntry.framebuffer.releaseTracked( m_context );
		rEntry.texture.releaseTracked( m_context );
		rEntry.key = t3dRdRenderTargetKey_t{};
		rEntry.inUse = false;
	}
	void synchronizeDevice() {
		const uint64_t generation = m_context.getDeviceGeneration();
		if ( m_nDeviceGeneration != 0u && m_nDeviceGeneration != generation ) {
			abandon();
		}
		m_nDeviceGeneration = generation;
	}

public:
	Terrain3DRdRenderTargetPool( Terrain3DRdContext &rContext,
	        t3dRdResourceOwner_t owner,
	        t3dRdMemoryCategory_t memory ) :
	        m_context( rContext ), m_owner( owner ), m_memory( memory ) {}

	bool acquire( const t3dRdRenderTargetKey_t &rKey, const String &rName, RID &rTexture, RID &rFramebuffer ) {
		rTexture = RID();
		rFramebuffer = RID();
		if ( rKey.width == 0u || rKey.height == 0u || rKey.bytesPerPixel == 0u || rKey.usage == 0u ) {
			return false;
		}
		synchronizeDevice();
		RenderingDevice *const rd = m_context.getDevice();
		if ( !rd ) {
			return false;
		}
		for ( Entry &entry : m_entries ) {
			if ( !entry.inUse && entry.texture.isValid() && entry.key == rKey ) {
				entry.inUse = true;
				m_context.recordRenderEvent( m_owner, EVENT_TARGET_REUSE );
				rTexture = entry.texture.rid();
				rFramebuffer = entry.framebuffer.rid();
				return true;
			}
		}
		Entry *slot = NULL;
		for ( Entry &entry : m_entries ) {
			if ( !entry.texture.isValid() ) {
				slot = &entry;
				break;
			}
		}
		if ( !slot ) {
			for ( uint32_t i = 0u; i < Capacity; ++i ) {
				Entry &candidate = m_entries[( m_nEvictCursor + i ) % Capacity];
				if ( !candidate.inUse ) {
					slot = &candidate;
					m_nEvictCursor = ( m_nEvictCursor + i + 1u ) % Capacity;
					break;
				}
			}
		}
		if ( !slot ) {
			return false;
		}
		if ( slot->texture.isValid() ) {
			freeEntry( *slot );
		}
		Terrain3DRdTexture texture;
		t3dRdTextureSpec_t spec;
		spec.m_name = rName + String( ".Texture" ); spec.m_owner = m_owner; spec.m_memory = m_memory;
		spec.m_layout.m_format = rKey.format; spec.m_layout.m_nWidth = rKey.width; spec.m_layout.m_nHeight = rKey.height;
		spec.m_layout.m_nLayers = 1u; spec.m_layout.m_nMipmaps = 1u; spec.m_layout.m_nUsageBits = rKey.usage; spec.m_layout.m_nBytes = bytes( rKey );
		if ( !texture.createTracked( m_context, spec ) ) { return false; }
		Terrain3DRdFramebuffer framebuffer;
		if ( rKey.framebuffer ) {
			TypedArray<RID> attachments; attachments.push_back( texture.rid() );
			if ( !framebuffer.createTracked( m_context, m_owner, rName + String( ".Framebuffer" ), attachments ) ) {
				texture.releaseTracked( m_context ); return false;
			}
		}
		slot->key = rKey; slot->inUse = true; slot->texture.swap( texture );
		if ( framebuffer.isValid() ) slot->framebuffer.swap( framebuffer );
		m_context.recordRenderEvent( m_owner, EVENT_TARGET_CREATE );
		rTexture = slot->texture.rid();
		rFramebuffer = slot->framebuffer.rid();
		return true;
	}

	int64_t framebufferFormat( const RID &rFramebuffer ) const {
		for ( const Entry &entry : m_entries ) {
			if ( entry.framebuffer.rid() == rFramebuffer ) { return entry.framebuffer.format( m_context.getDevice() ); }
		}
		return RenderingDevice::INVALID_ID;
	}

	PackedByteArray readback( const RID &rTexture, uint32_t nLayer = 0u ) {
		for ( Entry &entry : m_entries ) { if ( entry.texture.rid() == rTexture ) { return entry.texture.readbackTracked( m_context, nLayer ); } }
		return PackedByteArray();
	}
	Error readbackAsync( const RID &rTexture, uint32_t nLayer, const Callable &rCallback ) {
		for ( Entry &entry : m_entries ) { if ( entry.texture.rid() == rTexture ) { return entry.texture.readbackAsyncTracked( m_context, nLayer, rCallback ); } }
		return ERR_INVALID_PARAMETER;
	}
	void release( RID &rTexture, RID &rFramebuffer ) {
		for ( Entry &entry : m_entries ) {
			if ( entry.inUse && entry.texture.rid() == rTexture ) {
				entry.inUse = false;
				break;
			}
		}
		rTexture = RID();
		rFramebuffer = RID();
	}
	void release( RID &rTexture ) {
		RID framebuffer;
		release( rTexture, framebuffer );
	}
	void abandon() {
		for ( Entry &entry : m_entries ) {
			entry.texture.abandon(); entry.framebuffer.abandon(); entry.key = t3dRdRenderTargetKey_t{}; entry.inUse = false;
		}
		m_nEvictCursor = 0u;
	}
	void clear() {
		if ( m_nDeviceGeneration != 0u && m_nDeviceGeneration != m_context.getDeviceGeneration() ) {
			abandon();
		} else {
			for ( Entry &entry : m_entries ) {
				freeEntry( entry );
			}
		}
		m_nDeviceGeneration = 0u;
	}
};

#endif // TERRAIN3D_RD_RENDER_TARGET_POOL_H
