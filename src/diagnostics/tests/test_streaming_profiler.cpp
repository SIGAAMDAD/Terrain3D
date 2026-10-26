#include "diagnostics/core/terrain_3d_streaming_profiler.h"
#include <thread>
#include <vector>
#include <cassert>
int main() {
	auto &p = Terrain3DStreamingProfiler::get();
	assert( !p.enabled() );
	{
		T3D_STREAM_PROFILE_SCOPE( T3D_SP_VT_IO_READ_DECODE );
	}
	assert( p.snapshot( T3D_SP_VT_IO_READ_DECODE ).calls == 0 );
	p.setEnabled( true, 4 );
	std::vector< std::thread > threads;
	for ( int i = 0; i < 4; i++ )
		threads.emplace_back( [&] {
			for ( int j = 0; j < 10000; j++ ) {
				T3D_STREAM_PROFILE_SCOPE( T3D_SP_VT_IO_READ_DECODE );
				T3D_STREAM_PROFILE_BYTES( T3D_SP_VT_IO_READ_DECODE, 16 );
			}
		} );
	for ( auto &t : threads )
		t.join();
	auto s = p.snapshot( T3D_SP_VT_IO_READ_DECODE );
	assert( s.calls == 40000 && s.samples == 10000 && s.bytes == 640000 );
	assert( Terrain3DStreamingProfiler::workUnitName( T3D_SP_H01_PACK_RESOLVE ) != nullptr );
	assert( Terrain3DStreamingProfiler::get().snapshot( T3D_SP_H01_PACK_RESOLVE ).workUnits == 0u );
	{
		T3D_STREAM_PROFILE_SCOPE( T3D_SP_H01_PACK_RESOLVE );
		Terrain3DStreamingProfileWorkScope comparisons( T3D_SP_H01_PACK_RESOLVE );
		comparisons.add( 3u );
		comparisons.add( 5u );
	}
	assert( p.snapshot( T3D_SP_H01_PACK_RESOLVE ).workUnits == 8u );
	// One atomic publication for multiple channel/event contributions.
	{
		Terrain3DStreamingProfileBytesScope upload( T3D_SP_REGION_TEXTURE_UPLOAD );
		upload.add( 512u );
		upload.add( 256u );
		upload.add( 128u );
		Terrain3DStreamingProfileEventScope events( T3D_SP_H01_PACK_RESOLVE );
		events.add();
		events.add();
	}
	assert( p.snapshot( T3D_SP_REGION_TEXTURE_UPLOAD ).bytes == 896u );

	assert( p.snapshot( T3D_SP_H01_PACK_RESOLVE ).events == 2u );
	for ( uint32_t i = ( uint32_t ) T3D_SP_H01_PACK_RESOLVE; i < ( uint32_t ) T3D_SP_COUNT; ++i ) {
		assert( Terrain3DStreamingProfiler::name( ( t3dStreamProfileStage_t ) i ) != nullptr );
		assert( Terrain3DStreamingProfiler::name( ( t3dStreamProfileStage_t ) i )[0] != 'i' );
	}
	uint64_t sum = 0;
	for ( auto &h : s.histogram )
		sum += h;
	assert( sum == s.samples );
	p.setEnabled( false );
	p.reset();
	assert( p.snapshot( T3D_SP_VT_IO_READ_DECODE ).calls == 0 );
	assert( p.snapshot( T3D_SP_H01_PACK_RESOLVE ).workUnits == 0u );
	assert( p.snapshot( T3D_SP_H01_PACK_RESOLVE ).events == 0u );
	return 0;
}
