// Copyright ? 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_RD_CONTEXT_ACCESS_H
#define TERRAIN3D_RD_CONTEXT_ACCESS_H
#include "Terrain3DTypes.h"
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/string.hpp>

// Resource clients depend on the registry contract, not on the context's
// lifetime state, telemetry storage, or concrete object layout.
class Terrain3DRdContextAccess
{
protected:
	Terrain3DRdContextAccess( void ) = default;
	~Terrain3DRdContextAccess( void ) = default;

public:
	virtual godot::RenderingDevice *getDevice( void ) const = 0;
	virtual bool					registerResource( const godot::RID	   &rRid,
													  const godot::String  &rName,
													  t3dRdResourceType_t	type,
													  t3dRdMemoryCategory_t memoryCategory,
													  uint64_t				nBytes,
													  t3dRdResourceOwner_t	owner ) = 0;
	virtual void					freeResource( godot::RID &rRid, uint64_t nBufferBytes = 0u ) = 0;
	virtual void recordBufferUpdate( uint64_t nBytes, t3dRdResourceOwner_t owner, Terrain3DGpuBufferPolicy policy ) = 0;
	virtual void recordRenderEvent( t3dRdResourceOwner_t owner, t3dRenderTelemetryEvent_t event ) = 0;
	virtual void
	recordRenderCapacity( t3dRdResourceOwner_t owner, Terrain3DGpuBufferPolicy policy, uint64_t nBytes ) = 0;
};
#endif // TERRAIN3D_RD_CONTEXT_ACCESS_H
