#ifndef TERRAIN3D_RD_RENDER_TARGET_POOL_H
#define TERRAIN3D_RD_RENDER_TARGET_POOL_H

#include "rendering/rd/infrastructure/terrain_3d_rd_resource_service.h"
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/variant/typed_array.hpp>

// Fixed-capacity pool for temporary 2D textures and single-attachment targets.
// Callers release borrowed RIDs before resizing or shutting down the domain.
template <uint32_t Capacity>
class Terrain3DRdRenderTargetPool
{
public:
	typedef struct Key_s {
		uint32_t width = 0u;
		uint32_t height = 0u;
		RenderingDevice::DataFormat format = RenderingDevice::DATA_FORMAT_MAX;
		uint32_t usage = 0u;
		uint32_t bytesPerPixel = 0u;
		uint32_t historyClass = 0u;
		bool framebuffer = false;
		bool operator==( const Key_s &rOther ) const {
			return width == rOther.width && height == rOther.height && format == rOther.format && usage == rOther.usage
			        && bytesPerPixel == rOther.bytesPerPixel && historyClass == rOther.historyClass && framebuffer == rOther.framebuffer;
		}
	} Key;

private:
	typedef struct Entry_s {
		Key key;
		RID texture;
		RID framebuffer;
		bool inUse = false;
	} Entry;
	static_assert( Capacity > 0u, "Render target pool needs at least one slot" );
	Terrain3DRdContext &m_context;
	Terrain3DRdContext::ResourceOwner m_owner;
	Terrain3DRdContext::MemoryCategory m_memory;
	Entry m_entries[Capacity]{};
	uint32_t m_nEvictCursor = 0u;
	uint64_t m_nDeviceGeneration = 0u;

	static uint64_t bytes( const Key &rKey ) {
		return ( uint64_t )rKey.width * rKey.height * rKey.bytesPerPixel;
	}
	void freeEntry( Entry &rEntry ) {
		m_context.freeResource( rEntry.framebuffer );
		m_context.freeResource( rEntry.texture, bytes( rEntry.key ) );
		rEntry = Entry{};
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
	        Terrain3DRdContext::ResourceOwner owner,
	        Terrain3DRdContext::MemoryCategory memory ) :
	        m_context( rContext ), m_owner( owner ), m_memory( memory ) {}

	bool acquire( const Key &rKey, const String &rName, RID &rTexture, RID &rFramebuffer ) {
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
			if ( !entry.inUse && entry.texture.is_valid() && entry.key == rKey ) {
				entry.inUse = true;
				m_context.recordRenderEvent( m_owner, Terrain3DRdContext::RenderTelemetry::EVENT_TARGET_REUSE );
				rTexture = entry.texture;
				rFramebuffer = entry.framebuffer;
				return true;
			}
		}
		Entry *slot = nullptr;
		for ( Entry &entry : m_entries ) {
			if ( !entry.texture.is_valid() ) {
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
		if ( slot->texture.is_valid() ) {
			freeEntry( *slot );
		}
		Ref<RDTextureFormat> format;
		format.instantiate();
		format->set_width( rKey.width );
		format->set_height( rKey.height );
		format->set_depth( 1u );
		format->set_array_layers( 1u );
		format->set_mipmaps( 1u );
		format->set_texture_type( RenderingDevice::TEXTURE_TYPE_2D );
		format->set_format( rKey.format );
		format->set_usage_bits( BitField<RenderingDevice::TextureUsageBits>( rKey.usage ) );
		Ref<RDTextureView> view;
		view.instantiate();
		RID texture;
		if ( !Terrain3DRdResourceService( m_context, m_owner )
		                    .create( texture, rName + String( ".Texture" ), Terrain3DRdContext::RESOURCE_TEXTURE, m_memory,
		                            bytes( rKey ), [&]( RenderingDevice *pRd ) { return pRd->texture_create( format, view ); } ) ) {
			return false;
		}
		RID framebuffer;
		if ( rKey.framebuffer ) {
			TypedArray<RID> attachments;
			attachments.push_back( texture );
			if ( !Terrain3DRdResourceService( m_context, m_owner )
			                    .create( framebuffer, rName + String( ".Framebuffer" ), Terrain3DRdContext::RESOURCE_FRAMEBUFFER,
		                            Terrain3DRdContext::MEMORY_NONE, 0u,
		                            [&]( RenderingDevice *pRd ) { return pRd->framebuffer_create( attachments, -1, 1u ); } ) ) {
				m_context.freeResource( texture, bytes( rKey ) );
				return false;
			}
		}
		*slot = { rKey, texture, framebuffer, true };
		m_context.recordRenderEvent( m_owner, Terrain3DRdContext::RenderTelemetry::EVENT_TARGET_CREATE );
		rTexture = texture;
		rFramebuffer = framebuffer;
		return true;
	}

	void release( RID &rTexture, RID &rFramebuffer ) {
		for ( Entry &entry : m_entries ) {
			if ( entry.inUse && entry.texture == rTexture ) {
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
			entry = Entry{};
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
