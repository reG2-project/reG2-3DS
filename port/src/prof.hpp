// GTA2_3DS_PROF=<frames>: the times of the game's parts, and counts, on stderr every <frames> frames
// (port/3ds/prof_3ds.cpp). 3DS only: elsewhere the macros are empty.
#pragma once

#if defined(__3DS__)

namespace PortProf
{
// The names are in prof_3ds.cpp, in this order. A dot puts a part under the one its name starts with.
enum Part
{
    // Per logic step.
    kStep,
    kControls,
    kPlayers,
    kLights,
    kCranes,
    kCrushers,
    kGenerators,
    kPeds,
    kPedsPed,
    kPedsPedAi,
    kPedsDummies,
    kTransport,
    kGarages,
    kCars,
    kCarsCar,
    kCarsCarAi,
    kCarsCarPhysics,
    kCarsTraffic,
    kTrafficLights,
    kObjects,
    kScripts,
    kCrews,
    kAmbulance,
    kPolice,
    kParticles,
    kTileAnim,
    kHud,
    kDoors,
    kFireTrucks,
    kScores,
    kSound,
    kSoundEntities,
    kSoundQueues,
    kPaused,
    kCollide,
    kStepParts,
    // Per drawn frame.
    kFrame = kStepParts,
    kLoopNext,
    kLoopBeforeStep,
    kLoopAfterStep,
    kLoopBeginDraw,
    kLoopEndDraw,
    kLoopFps,
    kLog,
    kInputs,
    kPump,
    kPumpPad,
    kIdle,
    kDraw,
    kDrawSort,
    kDrawSetup,
    kDrawMap,
    kDrawScores,
    kDrawHud,
    kDrawEnd,
    kPrim,
    kPrimUv,
    kPrimTurn,
    kPrimClip,
    kPrimResolve,
    kPrimSetup,
    kPrimLight,
    kPrimVerts,
    kPrimC3d,
    kFlush,
    kPresent,
    kPresentBottom,
    kPresentOverlay,
    kPresentFrameEnd,
    kTexConvert,
    kTexUpload,
    kWaitVsync,
    kWaitBottom,
    kWaitGpu,
    kGpu,
    kC3dCpu,
    kCalibrate,
    kPartCount
};

enum Count
{
    // Per logic step.
    kDrive,
    kCarsVisible,
    kPedsOnScreen,
    kStepCounts,
    // Per drawn frame.
    kSteps = kStepCounts,
    kPrims,
    kDrawCalls,
    kVertices,
    kStates,
    kBinds,
    kUploads,
    kLightsAdded,
    kLitPrims,
    kLightTests,
    kLitVerts,
    kNearLights,
    kTiles,
    kSprites,
    kAmbient,
    kPumps,
    kEvents,
    kLateFrames,
    kRefreshes,
    kCountCount
};

extern bool gOn;
// The scale of scopes inside a primitive: 0 outside one, kSkip in one that is not sampled.
extern unsigned gPrimScale;
const unsigned kSkip = 0xFFFF;
extern unsigned gCounts[kCountCount];

// A timed entry on the stack, or -1. Scale: what its time and call count are multiplied by.
int Push(Part part, unsigned scale);
// Ends that entry and the sections opened above it.
void Pop(int index);
// Ends the open section of the current scope, and starts one for part. In a primitive, as its scopes.
void Section(Part part);
void EndSection();
// Whether the next primitive is one of those timed.
unsigned SamplePrimitive();
void AddMs(Part part, float ms);
// Reads GTA2_3DS_PROF the first time; drops what was measured since the last frame (the frontend's, a load).
void Reset();
void EndStep();
void EndFrame();

class Scope
{
  public:
    explicit Scope(Part part) : mIndex(gOn ? Push(part, gPrimScale) : -1)
    {
    }
    ~Scope()
    {
        if (mIndex >= 0)
        {
            Pop(mIndex);
        }
    }

  private:
    int mIndex;
};

// A wait or a rare costly job: always timed, and left out of the times of the parts around it.
class ExcludedScope
{
  public:
    explicit ExcludedScope(Part part) : mIndex(gOn ? Push(part, 1) : -1)
    {
    }
    ~ExcludedScope()
    {
        if (mIndex >= 0)
        {
            Pop(mIndex);
        }
    }

  private:
    int mIndex;
};

// One primitive of the gbh_* layer: one in GTA2_3DS_PROF_SAMPLE is timed, with the scopes inside it.
class PrimScope
{
  public:
    explicit PrimScope(Part part) : mIndex(-1), mOwner(gOn && !gPrimScale)
    {
        if (mOwner)
        {
            gPrimScale = SamplePrimitive();
            mIndex = Push(part, gPrimScale);
            gPrimScale = mIndex >= 0 ? gPrimScale : kSkip;
        }
    }
    ~PrimScope()
    {
        if (mIndex >= 0)
        {
            Pop(mIndex);
        }
        if (mOwner)
        {
            gPrimScale = 0;
        }
    }

  private:
    int mIndex;
    bool mOwner;
};
} // namespace PortProf

#define PORT_PROF_CAT2(a, b) a##b
#define PORT_PROF_CAT(a, b) PORT_PROF_CAT2(a, b)
// Times the rest of the enclosing block.
#define PORT_PROF(part) PortProf::Scope PORT_PROF_CAT(portProf, __LINE__)(PortProf::part)
#define PORT_PROF_EXCLUDED(part) PortProf::ExcludedScope PORT_PROF_CAT(portProf, __LINE__)(PortProf::part)
#define PORT_PROF_PRIM(part) PortProf::PrimScope PORT_PROF_CAT(portProf, __LINE__)(PortProf::part)
// Times from here to the next section or PORT_PROF_END in the same scope.
#define PORT_PROF_SECTION(part) (PortProf::gOn && PortProf::gPrimScale != PortProf::kSkip ? PortProf::Section(PortProf::part) : (void)0)
#define PORT_PROF_END() (PortProf::gOn && PortProf::gPrimScale != PortProf::kSkip ? PortProf::EndSection() : (void)0)
#define PORT_PROF_COUNT(count, n) (PortProf::gOn ? (void)(PortProf::gCounts[PortProf::count] += (n)) : (void)0)
#define PORT_PROF_ADD_MS(part, ms) (PortProf::gOn ? PortProf::AddMs(PortProf::part, (ms)) : (void)0)
#define PORT_PROF_RESET() PortProf::Reset()
#define PORT_PROF_END_STEP() (PortProf::gOn ? PortProf::EndStep() : (void)0)
#define PORT_PROF_END_FRAME() (PortProf::gOn ? PortProf::EndFrame() : (void)0)

#else

#define PORT_PROF(part)
#define PORT_PROF_EXCLUDED(part)
#define PORT_PROF_PRIM(part)
#define PORT_PROF_SECTION(part)
#define PORT_PROF_END()
#define PORT_PROF_COUNT(count, n)
#define PORT_PROF_ADD_MS(part, ms)
#define PORT_PROF_RESET()
#define PORT_PROF_END_STEP()
#define PORT_PROF_END_FRAME()

#endif
