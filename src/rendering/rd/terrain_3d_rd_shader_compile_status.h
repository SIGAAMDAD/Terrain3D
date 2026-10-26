// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_RD_SHADER_COMPILE_STATUS_H
#define TERRAIN3D_RD_SHADER_COMPILE_STATUS_H

#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

// Compute-pipeline creation is where backend-native compilation/translation may
// occur after the physical shader object has already been created. Keep this status
// separate so a driver/backend stall can be distinguished from SPIR-V object creation.
inline RID t3dRdCreateComputePipelineWithStatus(
        RenderingDevice *const pRd, const RID &hShader, const String &rName ) {
	Time *const pTime = Time::get_singleton();
	const uint64_t nStartedMsec = pTime ? pTime->get_ticks_msec() : 0u;
	UtilityFunctions::print( "[Terrain3D][ShaderCompile] PIPELINE BEGIN ", rName );

	if ( !pRd || !hShader.is_valid() ) {
		UtilityFunctions::push_error( "[Terrain3D][ShaderCompile] PIPELINE FAIL ", rName, " :: invalid RenderingDevice/shader" );
		return RID();
	}

	RID hPipeline = pRd->compute_pipeline_create( hShader );
	const uint64_t nFinishedMsec = pTime ? pTime->get_ticks_msec() : nStartedMsec;
	const uint64_t nElapsedMsec = nFinishedMsec >= nStartedMsec ? nFinishedMsec - nStartedMsec : 0u;
	if ( !hPipeline.is_valid() ) {
		UtilityFunctions::push_error(
		        "[Terrain3D][ShaderCompile] PIPELINE FAIL ", rName, " elapsed=", nElapsedMsec, " ms" );
		return RID();
	}

	UtilityFunctions::print( "[Terrain3D][ShaderCompile] PIPELINE READY ", rName, " elapsed=", nElapsedMsec, " ms" );
	return hPipeline;
}

#endif // TERRAIN3D_RD_SHADER_COMPILE_STATUS_H
