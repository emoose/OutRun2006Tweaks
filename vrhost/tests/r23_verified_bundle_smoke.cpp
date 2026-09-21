#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include "runtime/r23_verified_bundle.hpp"

#include <cassert>

int main()
{
    using namespace OutRunVrR23VerifiedBundle;

    Invalidate();
    Snapshot snapshot{};
    assert(Read(snapshot));
    assert(!IsFresh(snapshot));

    OutRunVR::SharedRenderFrameState frame{};
    frame.magic = OutRunVR::RenderFrameMagic;
    frame.protocolVersion = OutRunVR::RenderFrameProtocolVersion;
    frame.structSize = sizeof(frame);
    frame.state = OutRunVR::StereoSbsActive;
    frame.frameId = 42;
    frame.sourcePoseSequence = 77;
    frame.presentQpc = 123456;

    Publish(frame, SourceKind::DirectGpu, 0);
    assert(ReadFresh(snapshot));
    assert(snapshot.kind == SourceKind::DirectGpu);
    assert(snapshot.frameId == 42);
    assert(snapshot.poseSequence == 77);
    assert(snapshot.presentQpc == 123456);
    assert(Matches(frame, SourceKind::DirectGpu));
    assert(!Matches(frame, SourceKind::ClassicSbs));

    // Identity fields inside the copied Frame.v2 must agree with the atomic
    // bundle identity. A mixed texture/pose publication must fail freshness.
    snapshot.frame.frameId = 43;
    assert(!IsFresh(snapshot, snapshot.publishedAtMs));

    assert(Read(snapshot));
    assert(IsFresh(snapshot, snapshot.publishedAtMs));
    assert(!IsFresh(snapshot,
        snapshot.publishedAtMs + MaxPresentationAgeMs + 1));

    Invalidate();
    assert(!ReadFresh(snapshot));
    return 0;
}
