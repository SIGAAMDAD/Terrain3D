#ifndef TERRAIN3D_RD_BINDING_BUILDER_H
#define TERRAIN3D_RD_BINDING_BUILDER_H

#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/classes/rendering_device.hpp>

class Terrain3DRdBindingBuilder
{
public:
	static godot::Ref<godot::RDUniform> single( int nBinding, godot::RenderingDevice::UniformType type, const godot::RID &rRid ) {
		godot::Ref<godot::RDUniform> uniform;
		uniform.instantiate();
		uniform->set_uniform_type( type );
		uniform->set_binding( nBinding );
		uniform->add_id( rRid );
		return uniform;
	}

	static godot::Ref<godot::RDUniform> samplerTexture( int nBinding,
	        const godot::RID &rSampler,
	        const godot::RID &rTexture ) {
		godot::Ref<godot::RDUniform> uniform = single( nBinding, godot::RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, rSampler );
		uniform->add_id( rTexture );
		return uniform;
	}

	static godot::Ref<godot::RDUniform> storage( int nBinding, const godot::RID &rBuffer ) {
		return single( nBinding, godot::RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, rBuffer );
	}

	static godot::Ref<godot::RDUniform> image( int nBinding, const godot::RID &rTexture ) {
		return single( nBinding, godot::RenderingDevice::UNIFORM_TYPE_IMAGE, rTexture );
	}
};

#endif // TERRAIN3D_RD_BINDING_BUILDER_H
