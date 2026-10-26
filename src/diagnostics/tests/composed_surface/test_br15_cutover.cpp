#include "rendering/rd/vt/core/terrain_3d_composed_surface_cutover.h"
#include <assert.h>
#include <stdio.h>

int main() {
    t3dComposedSurfaceCutoverProof_t ref{};
    ref.m_nSourceFingerprint = 0xc0302u;
    ref.m_nControlsRevision = 2u;
    ref.m_nMaterialRevision = 3u;
    ref.m_nTopologyRevision = 4u;
    ref.m_nFilterIdentity = 5u;
    ref.m_nDeviceGeneration = 8u;
    ref.m_bStaticMaterialParity = true;
    ref.m_bDnrParity = true;
    ref.m_bAlphaParity = true;
    ref.m_bNoPendingEdits = true;
    assert( t3d_cs_cutover_proof_current( ref, ref, ref.m_nSourceFingerprint, 8u ) );
    for ( int i = 0; i < 10; ++i ) {
        t3dComposedSurfaceCutoverProof_t dirty = ref;
        switch ( i ) {
            case 0: ++dirty.m_nSourceFingerprint; break;
            case 1: ++dirty.m_nControlsRevision; break;
            case 2: ++dirty.m_nMaterialRevision; break;
            case 3: ++dirty.m_nTopologyRevision; break;
            case 4: ++dirty.m_nFilterIdentity; break;
            case 5: ++dirty.m_nDeviceGeneration; break;
            case 6: dirty.m_bStaticMaterialParity = false; break;
            case 7: dirty.m_bDnrParity = false; break;
            case 8: dirty.m_bAlphaParity = false; break;
            case 9: dirty.m_bNoPendingEdits = false; break;
        }
        assert( !t3d_cs_cutover_proof_current( ref, dirty, ref.m_nSourceFingerprint, 8u ) );
    }
    assert( !t3d_cs_cutover_proof_current( ref, ref, ref.m_nSourceFingerprint, 9u ) );
    assert( !t3d_cs_cutover_proof_current( ref, ref, 0u, 8u ) );
    puts("BR15 fail-closed material/source parity proof PASS");
}
