#include "rendering/rd/infrastructure/resources/terrain_3d_rd_shader.h"
#include "rendering/rd/shaders/terrain_3d_rd_shader_library.h"

#include <EASTL/utility.h>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

RID Terrain3DRdShader::createPhysical(
        RenderingDevice *const pRd, const Ref<RDShaderSPIRV> &rSpirv, const String &rName ) {
	Time *const pTime = Time::get_singleton();
	const uint64_t nStartedMsec = pTime ? pTime->get_ticks_msec() : 0u;
	UtilityFunctions::print( "[Terrain3D][ShaderCompile] DEVICE BEGIN ", rName );

	if ( !pRd || rSpirv.is_null() ) {
		UtilityFunctions::push_error( "[Terrain3D][ShaderCompile] DEVICE FAIL ", rName, " :: invalid RenderingDevice/SPIR-V" );
		return RID();
	}

	RID hShader = pRd->shader_create_from_spirv( rSpirv, rName );
	const uint64_t nFinishedMsec = pTime ? pTime->get_ticks_msec() : nStartedMsec;
	const uint64_t nElapsedMsec = nFinishedMsec >= nStartedMsec ? nFinishedMsec - nStartedMsec : 0u;
	if ( !hShader.is_valid() ) {
		UtilityFunctions::push_error(
		        "[Terrain3D][ShaderCompile] DEVICE FAIL ", rName, " elapsed=", nElapsedMsec, " ms" );
		return RID();
	}

	UtilityFunctions::print( "[Terrain3D][ShaderCompile] DEVICE READY ", rName, " elapsed=", nElapsedMsec, " ms" );
	return hShader;
}

bool Terrain3DRdShader::createTracked( Terrain3DRdContext &rContext, const t3dRdResourceOwner_t owner,
        const String &rName, const t3dRdShaderId_t id ) {
	return createTracked( rContext, owner, rName, id, rName );
}

bool Terrain3DRdShader::createTracked( Terrain3DRdContext &rContext, const t3dRdResourceOwner_t owner,
        const String &rName, const t3dRdShaderId_t id, const String &rRegistryName ) {
	if ( isValid() || rName.is_empty() || rRegistryName.is_empty() || id >= T3D_RD_SHADER_COUNT ) { return false; }
	RenderingDevice *const rd = rContext.getDevice();
	if ( !rd ) { return false; }
	if ( Terrain3DRdShaderLibrary::physicalCacheReady() ) {
		const RID borrowed = Terrain3DRdShaderLibrary::borrowPhysical( id );
		if ( !borrowed.is_valid() ) {
			UtilityFunctions::push_error( "[Terrain3D][ShaderCache] Catalog shader is absent from the ready physical cache: id=", ( int64_t )id, " name=", rName );
			return false;
		}
		m_hRid = borrowed;
		m_owner = owner;
		m_bBorrowed = true;
		m_nGeneration = nextGeneration( m_nGeneration );
		return true;
	}
	const Ref<RDShaderSPIRV> spirv = Terrain3DRdShaderLibrary::borrowSpirv( id );
	if ( spirv.is_null() ) { return false; }
	RID created = createPhysical( rd, spirv, rName );
	if ( !created.is_valid() ) {
		rContext.recordResourceCreationFailure( rRegistryName, T3D_RD_CONTEXT_RESOURCE_SHADER );
		rContext.recordRenderEvent( owner, EVENT_RESOURCE_FAILURE );
		return false;
	}
	if ( !rContext.registerResource( created, rRegistryName, T3D_RD_CONTEXT_RESOURCE_SHADER,
	             T3D_RD_CONTEXT_MEMORY_NONE, 0u, owner ) ) {
		rContext.recordRenderEvent( owner, EVENT_RESOURCE_FAILURE );
		rd->free_rid( created );
		return false;
	}
	m_hRid = created;
	m_owner = owner;
	m_bLocal = false;
	m_bBorrowed = false;
	m_nGeneration = nextGeneration( m_nGeneration );
	return true;
}

bool Terrain3DRdShader::createTracked( Terrain3DRdContext &rContext, const t3dRdResourceOwner_t owner,
        const String &rName, const Ref<RDShaderSPIRV> &rSpirv ) {
	return createTracked( rContext, owner, rName, rSpirv, rName );
}

bool Terrain3DRdShader::createTracked( Terrain3DRdContext &rContext, const t3dRdResourceOwner_t owner,
        const String &rName, const Ref<RDShaderSPIRV> &rSpirv, const String &rRegistryName ) {
	if ( isValid() || rName.is_empty() || rRegistryName.is_empty() || rSpirv.is_null() ) { return false; }
	RenderingDevice *const rd = rContext.getDevice();
	if ( !rd ) { return false; }
	RID created = createPhysical( rd, rSpirv, rName );
	if ( !created.is_valid() ) {
		rContext.recordResourceCreationFailure( rRegistryName, T3D_RD_CONTEXT_RESOURCE_SHADER );
		rContext.recordRenderEvent( owner, EVENT_RESOURCE_FAILURE );
		return false;
	}
	if ( !rContext.registerResource( created, rRegistryName, T3D_RD_CONTEXT_RESOURCE_SHADER,
	             T3D_RD_CONTEXT_MEMORY_NONE, 0u, owner ) ) {
		rContext.recordRenderEvent( owner, EVENT_RESOURCE_FAILURE );
		rd->free_rid( created );
		return false;
	}

	m_hRid = created;
	m_owner = owner;
	m_bLocal = false;
	m_bBorrowed = false;
	m_nGeneration = nextGeneration( m_nGeneration );
	return true;
}

bool Terrain3DRdShader::createLocal(
        RenderingDevice *const pRd, const String &rName, const Ref<RDShaderSPIRV> &rSpirv ) {
	if ( isValid() || !pRd || rName.is_empty() || rSpirv.is_null() ) { return false; }
	RID created = createPhysical( pRd, rSpirv, rName );
	if ( !created.is_valid() ) { return false; }
	m_hRid = created;
	m_owner = T3D_RD_CONTEXT_OWNER_UNSPECIFIED;
	m_bLocal = true;
	m_bBorrowed = false;
	m_nGeneration = nextGeneration( m_nGeneration );
	return true;
}

void Terrain3DRdShader::releaseTracked( Terrain3DRdContext &rContext ) {
	if ( !m_hRid.is_valid() || m_bLocal ) { return; }
	if ( !m_bBorrowed ) rContext.freeResource( m_hRid );
	abandon();
}

void Terrain3DRdShader::releaseLocal( RenderingDevice *const pRd ) {
	if ( !m_hRid.is_valid() || !m_bLocal || !pRd ) { return; }
	pRd->free_rid( m_hRid );
	abandon();
}

void Terrain3DRdShader::abandon( void ) {
	m_hRid = RID();
	m_owner = T3D_RD_CONTEXT_OWNER_UNSPECIFIED;
	m_bLocal = false;
	m_bBorrowed = false;
	m_nGeneration = nextGeneration( m_nGeneration );
}

void Terrain3DRdShader::swap( Terrain3DRdShader &rOther ) {
	using eastl::swap;
	const uint32_t this_generation = nextGeneration( m_nGeneration );
	const uint32_t other_generation = nextGeneration( rOther.m_nGeneration );
	swap( m_hRid, rOther.m_hRid );
	swap( m_owner, rOther.m_owner );
	swap( m_bLocal, rOther.m_bLocal );
	swap( m_bBorrowed, rOther.m_bBorrowed );
	m_nGeneration = this_generation;
	rOther.m_nGeneration = other_generation;
}
