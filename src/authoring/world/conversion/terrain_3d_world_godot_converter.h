// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_WORLD_GODOT_CONVERTER_H
#define TERRAIN3D_WORLD_GODOT_CONVERTER_H

#include "Terrain3DTypes.h"
#include "Terrain3DConstants.h"

#include "instances/runtime/terrain_3d_world_instance_3d.h"
#include "authoring/world/conversion/terrain_3d_world_scene_builder.h"

#include <EASTL/type_traits.h>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/variant/rid.hpp>

typedef bool ( *t3dWorldResolveMaterialVTColdFn_t )( void	  *pUser,
													 uint64_t  nStableMaterialKey,
													 uint32_t &rAllocationId );

static_assert( eastl::is_trivially_destructible< t3dWorldNativeVisualEntry_t >::value,
			   "World native visual records must remain POD-like" );

class Terrain3DWorldGodotConverter
{
public:
	Terrain3DWorldGodotConverter();

	bool initializeCold( const t3dWorldConversionPolicy_t	   &pPolicy,
						 const t3dWorldGodotMaterialResolver_t &pMaterialResolver,
						 t3dWorldSceneBuildBuffers_t		   *pBuffers );
	void setPolicyCold( const t3dWorldConversionPolicy_t &pPolicy );
	bool rebuildSceneCold( godot::Node *pRoot, t3dWorldSceneBuildStats_t &rStats );
	void releaseNativeOwnershipCold();
	bool applyNativeOwnershipCold( bool pWorldReady );

	bool							   isInitialized() const;
	const t3dWorldConversionPolicy_t  &getPolicy() const;
	const t3dWorldSceneBuildBuffers_t *getBuildBuffersCold() const;

private:
	bool	 visitNodeCold( godot::Node *pNode, t3dWorldSceneBuildStats_t &rStats );
	bool	 convertMeshInstanceCold( godot::MeshInstance3D *pMeshInstance, t3dWorldSceneBuildStats_t &rStats );
	bool	 buildCandidateCold( godot::MeshInstance3D					   *pMeshInstance,
								 t3dWorldCandidateDesc_t				   &rCandidate,
								 godot::Ref< Terrain3DWorldImportProfile > &rProfile,
								 godot::Ref< Terrain3DWorldMaterial >	   &rExplicitMaterial ) const;
	bool	 analyzeMaterialsCold( godot::MeshInstance3D					  *pMeshInstance,
								   uint32_t									   pTier,
								   const godot::Ref< Terrain3DWorldMaterial > &pExplicitMaterial,
								   bool										  &rSupported,
								   bool										  &rSortedTransparency,
								   bool										  &rTwoSided ) const;
	bool	 findOrCookPrototypeCold( godot::MeshInstance3D						 *pMeshInstance,
									  uint32_t									  pTier,
									  const godot::Ref< Terrain3DWorldMaterial > &pExplicitMaterial,
									  uint32_t									 &rMeshId,
									  t3dWorldConversionReason_t				 &rReason );
	bool	 flattenMeshCold( godot::MeshInstance3D						 *pMeshInstance,
							  uint32_t									  pTier,
							  const godot::Ref< Terrain3DWorldMaterial > &pExplicitMaterial,
							  t3dWorldGeometrySourceMesh_t				 &rSource,
							  t3dWorldGeometrySourceLod_t				 &rLod,
                              const godot::Ref< godot::ArrayMesh > &rOverrideMesh,
							  uint64_t									 &rPrototypeKey,
							  t3dWorldConversionReason_t				 &rReason );
	bool	 findOrCompileMaterialCold( const godot::Ref< godot::Material >		   &pMaterial,
										const godot::Ref< Terrain3DWorldMaterial > &pExplicitMaterial,
										uint32_t									pTier,
										uint32_t								   &rMaterialId,
										t3dWorldConversionReason_t				   &rReason );
	bool	 compileStandardMaterialCold( const godot::Ref< godot::Material > &pMaterial,
										  uint32_t							   pTier,
										  t3dRenderingMaterialGpu_t			  &rRecord,
										  bool								  &rSortedTransparency,
										  bool								  &rTwoSided ) const;
	uint64_t computePrototypeKeyCold( godot::MeshInstance3D						 *pMeshInstance,
									  uint32_t									  pTier,
									  const godot::Ref< Terrain3DWorldMaterial > &pExplicitMaterial ) const;
	bool	 appendInstanceCold( godot::MeshInstance3D *pMeshInstance,
								 uint32_t				pMeshId,
								 uint32_t				pMaterialOverride,
								 uint32_t			   &rInstanceId );
	void	 publishOwnershipCold( godot::MeshInstance3D	 *pNode,
								   t3dWorldOwnership_t		  pOwnership,
								   t3dWorldConversionReason_t pReason,
								   uint32_t					  pPrototypeId,
								   uint32_t					  pInstanceId );

	t3dWorldConversionPolicy_t		m_policy;
	t3dWorldGodotMaterialResolver_t m_materialResolver;
	t3dWorldSceneBuildBuffers_t	   *m_pBuffers;
	bool							m_bInitialized;
	bool							m_bNativeOwnershipApplied;
};

#endif // TERRAIN3D_WORLD_GODOT_CONVERTER_H
