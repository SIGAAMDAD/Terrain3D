#ifndef TERRAIN3D_RUNTIME_NODE_DESCRIPTOR_PATH_H
#define TERRAIN3D_RUNTIME_NODE_DESCRIPTOR_PATH_H
#include "data/model/terrain_3d_region.h"
#include <godot_cpp/classes/packed_scene.hpp>
EA_FORCE_INLINE String t3dRuntimeNodeDescriptorScenePath(
	const t3dStreamedNodeRuntimeDescriptor_t &rDescriptor ) {
	if ( !rDescriptor.m_scenePath.is_empty() ) {
		return rDescriptor.m_scenePath;
	}
	return rDescriptor.m_scene.is_valid() ? rDescriptor.m_scene->get_path() : String();
}
EA_FORCE_INLINE String t3dRuntimeNodeDescriptorShadowPath(
	const t3dStreamedNodeRuntimeDescriptor_t &rDescriptor ) {
	if ( !rDescriptor.m_shadowProxyScenePath.is_empty() ) {
		return rDescriptor.m_shadowProxyScenePath;
	}
	return rDescriptor.m_shadowProxyScene.is_valid() ? rDescriptor.m_shadowProxyScene->get_path() : String();
}
#endif // TERRAIN3D_RUNTIME_NODE_DESCRIPTOR_PATH_H
