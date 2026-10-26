// CS14: editor-owner publication transaction for a validated V3 composed cook.
#ifndef TERRAIN3D_COMPOSED_SURFACE_EDITOR_COOK_H
#define TERRAIN3D_COMPOSED_SURFACE_EDITOR_COOK_H

#include "authoring/vt/build/terrain_3d_composed_surface_v3_cold_pipeline.h"
#include "authoring/vt/integration/terrain_3d_composed_surface_edit_journal.h"

// The supplied lock must serialize terrain edits, material/source replacement,
// the final sidecar rename and journal acceptance on the editor owner thread.
// Begin/End do not run during composition; only during the short commit phase.
typedef bool ( *t3dCs14BeginCommitFn )( void *pUser );
typedef void ( *t3dCs14EndCommitFn )( void *pUser );

struct t3dCs14EditorCookRequest_t
{
    t3dCs13ColdCookRequest_t m_cook;
    uint32_t m_nRegionIndex;
    uint64_t m_nJournalFingerprint;
    uint64_t m_nJournalTopologyGeneration;
    uint64_t m_nRegionKey;
    uint32_t m_nMip0Width;
    uint32_t m_nMip0Height;
    uint8_t *m_pDirtyFlags;
    uint32_t m_nDirtyCapacity;
    t3dCs14BeginCommitFn m_pBeginCommit;
    t3dCs14EndCommitFn m_pEndCommit;
    void *m_pCommitUser;
};

enum t3dCs14Status_t
{
    T3D_CS14_SUCCESS = 0,
    T3D_CS14_INVALID_INPUT,
    T3D_CS14_NOT_DIRTY,
    T3D_CS14_STALE,
    T3D_CS14_COOK_FAILED,
    T3D_CS14_COMMIT_FAILED
};

struct t3dCs14EditorCookResult_t
{
    t3dCs14Status_t m_status;
    t3dCs13ColdCookResult_t m_cook;
    uint64_t m_nClaimRevision;
};

// Caller retains source/previous files, page footprints, authored layers,
// decoder/encoder and bounded scratch for the entire synchronous cold call.
// Never invoked from an edit callback, render thread or frame service.
// On failure, dirty journal state remains eligible for retry.
bool t3d_cs14_editor_cook_cold( Terrain3DComposedSurfaceEditJournal &rJournal,
                                const t3dCs14EditorCookRequest_t &rRequest,
                                t3dCs14EditorCookResult_t &rResult );
#endif
