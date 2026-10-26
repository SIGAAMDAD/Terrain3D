#ifndef TERRAIN3D_CLIPMAP_SOURCE_H
#define TERRAIN3D_CLIPMAP_SOURCE_H
#include "Terrain3DTypes.h"
#include "rendering/geometry/terrain_3d_clipmap_strip_contract.h"
#include "rendering/rd/core/terrain_3d_rd_types.h"
class Terrain3DClipmapSource {
protected:
	~Terrain3DClipmapSource( void ) = default;
public:
	virtual int getMeshSize( void ) const = 0;
	virtual int getTessellationLevel( void ) const = 0;
	virtual const t3dClipmapStripPart_t *getClipmapStripParts( t3dMeshType_t type ) const = 0;
	virtual uint16_t getClipmapStripPartCount( t3dMeshType_t type ) const = 0;
	virtual int fillRdPatches( t3dRdPatch_t *pPatches, int nCapacity ) const = 0;
};
#endif // TERRAIN3D_CLIPMAP_SOURCE_H
