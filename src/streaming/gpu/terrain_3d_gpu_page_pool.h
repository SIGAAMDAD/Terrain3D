#ifndef TERRAIN3D_GPU_PAGE_POOL_H
#define TERRAIN3D_GPU_PAGE_POOL_H

#include "streaming/residency/terrain_3d_gpu_page_contract.h"
#include "rendering/rd/resources/terrain_3d_rd_texture_manager.h"

// Render-device owner. A slot never becomes reusable based on elapsed frames.
class Terrain3DGpuPagePool
{
public:
	static constexpr uint32_t MAX_LAYERS = T3D_GPU_CACHE_MAX_LAYERS;
	enum State : uint8_t
	{
		FREE = 0u,
		RESERVED,
		PUBLISHED,
		RETIRED,
		DEFAULT_PAGE
	};
	struct Slot
	{
		uint64_t lastReaderEpoch = 0u;
		uint32_t generation = 1u;
		uint16_t references = 0u;
		uint8_t	 state = FREE;
	};
	struct Configuration
	{
		RenderingDevice::DataFormat format = RenderingDevice::DATA_FORMAT_MAX;
		uint32_t					width = 0u, height = 0u, layers = 0u, mipmaps = 1u;
		uint64_t					maximumBytes = 0u;
		bool						storageWrites = true;
	};
	bool configure( const Configuration &config, uint16_t poolIndex );
	bool createCold( Terrain3DRdTextureManager &manager );
	bool stageDefault( Terrain3DRdTextureManager &manager, const PackedByteArray &bytes, uint64_t submissionEpoch );
	bool publishDefault( uint64_t confirmedCompletedEpoch );
	t3dGpuPageHandle_t reserve();
	bool upload( Terrain3DRdTextureManager &manager, const t3dGpuPageHandle_t &handle, const PackedByteArray &bytes );
	bool publish( const t3dGpuPageHandle_t &handle );
	bool addReference( const t3dGpuPageHandle_t &handle );
	bool markReader( const t3dGpuPageHandle_t &handle, uint64_t submissionEpoch );
	bool pinUntil( const t3dGpuPageHandle_t &handle, uint64_t submissionEpoch );
	bool release( const t3dGpuPageHandle_t &handle );
	uint32_t collect( uint64_t confirmedCompletedEpoch );
	void	 destroyCold( Terrain3DRdTextureManager &manager );
	void	 abandonDevice();
	bool	 valid( const t3dGpuPageHandle_t &handle ) const;
	bool	 isPublished( const t3dGpuPageHandle_t &handle ) const;
	uint64_t allocatedBytes() const {
		return m_layout.m_nBytes;
	}
	uint64_t layerBytes() const {
		return m_layout.m_nLayers ? m_layout.m_nBytes / m_layout.m_nLayers : 0u;
	}
	uint32_t available() const;
	uint32_t resident() const;
	uint32_t retired() const;
	uint32_t deviceEpoch() const {
		return m_deviceEpoch;
	}
	const Terrain3DRdTexture &texture() const {
		return m_texture;
	}
	Terrain3DRdTexture &mutableTexture() {
		return m_texture;
	}
	const Configuration &configuration() const {
		return m_config;
	}

private:
	Configuration		 m_config;
	t3dRdTextureLayout_t m_layout;
	Terrain3DRdTexture	 m_texture;
	Slot				 m_slots[MAX_LAYERS]{};
	uint32_t			 m_deviceEpoch = 1u;
	uint16_t			 m_poolIndex = UINT16_MAX;
	bool				 m_configured = false;
	bool				 m_defaultInitialized = false;
	bool				 m_defaultStaged = false;
};
#endif
