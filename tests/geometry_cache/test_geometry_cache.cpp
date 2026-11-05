// The runner inserts verbatim production functions. Only engine dependencies and
// the skeleton generator are doubled here; this is not a GPU/platform smoke test.
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

template <typename T> class dmArray
{
    std::vector<T> m_Data;
    uint32_t m_Size = 0;
public:
    uint32_t Size() const { return m_Size; }
    uint32_t Capacity() const { return (uint32_t)m_Data.size(); }
    bool Full() const { return m_Size == Capacity(); }
    void SetCapacity(uint32_t capacity)
    {
        // Like dmArray, only the used prefix survives a reallocation.
        std::vector<T> next(capacity);
        m_Size = std::min(m_Size, capacity);
        std::copy_n(m_Data.begin(), m_Size, next.begin());
        m_Data.swap(next);
    }
    void SetSize(uint32_t size) { assert(size <= Capacity()); m_Size = size; }
    void Push(const T& value) { assert(m_Size < Capacity()); m_Data[m_Size++] = value; }
    T* Begin() { return m_Data.data(); }
    const T* Begin() const { return m_Data.data(); }
    T& operator[](uint32_t i) { assert(i < m_Size); return m_Data[i]; }
    const T& operator[](uint32_t i) const { assert(i < m_Size); return m_Data[i]; }
    void Swap(dmArray& rhs) { m_Data.swap(rhs.m_Data); std::swap(m_Size, rhs.m_Size); }
};
template <typename T> struct dmObjectPool
{
    dmArray<T> m_Objects;
    dmArray<T>& GetRawObjects() { return m_Objects; }
};
namespace dmMath {
template <typename T> T Min(T a, T b) { return std::min(a, b); }
template <typename T> T Max(T a, T b) { return std::max(a, b); }
}
namespace dmVMath { struct Matrix4 {}; struct Vector4 { explicit Vector4(float) {} }; }
namespace dmResource { typedef uintptr_t HFactory; }
namespace dmGraphics {
typedef uintptr_t HVertexDeclaration;
typedef uintptr_t HVertexBuffer;
typedef uintptr_t HIndexBuffer;
typedef uintptr_t HTexture;
enum Type { TYPE_UNSIGNED_SHORT, TYPE_UNSIGNED_INT };
enum BlendFactor { BLEND_FACTOR_ONE, BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, BLEND_FACTOR_DST_COLOR, BLEND_FACTOR_ONE_MINUS_DST_COLOR, BLEND_FACTOR_ONE_MINUS_SRC_COLOR };
enum { BUFFER_USAGE_DYNAMIC_DRAW, PRIMITIVE_TRIANGLES };
static std::vector<uint8_t> g_VertexGPU, g_IndexGPU;
static uint32_t g_VertexUploads, g_IndexUploads;
void SetVertexBufferData(HVertexBuffer, uint32_t size, const void* data, int)
{
    ++g_VertexUploads;
    g_VertexGPU.assign((const uint8_t*)data, (const uint8_t*)data + size);
}
void SetIndexBufferData(HIndexBuffer, uint32_t size, const void* data, int)
{
    ++g_IndexUploads;
    g_IndexGPU.assign((const uint8_t*)data, (const uint8_t*)data + size);
}
}
namespace dmGameSystem { typedef uintptr_t HComponentRenderConstants; }
namespace dmRender {
typedef uintptr_t HRenderContext;
typedef uintptr_t HMaterial;
struct RenderObject
{
    dmGraphics::HVertexDeclaration m_VertexDeclaration;
    dmGraphics::HVertexBuffer m_VertexBuffer;
    dmGraphics::HIndexBuffer m_IndexBuffer;
    uint32_t m_PrimitiveType, m_VertexStart, m_VertexCount;
    dmGraphics::HTexture m_Textures[1];
    HMaterial m_Material;
    bool m_SetBlendFactors;
    dmGraphics::BlendFactor m_SourceBlendFactor, m_DestinationBlendFactor;
    dmGraphics::Type m_IndexType;
    uintptr_t m_ConstantBuffer;
    void Init() { memset(this, 0, sizeof(*this)); }
};
static std::vector<RenderObject*> g_Submitted;
void AddToRender(HRenderContext, RenderObject* ro) { g_Submitted.push_back(ro); }
struct RenderListEntry { uintptr_t m_UserData; };
enum RenderListOperation { RENDER_LIST_OPERATION_BEGIN, RENDER_LIST_OPERATION_BATCH, RENDER_LIST_OPERATION_END };
struct RenderListDispatchParams
{
    void* m_UserData;
    RenderListOperation m_Operation;
    HRenderContext m_Context;
    RenderListEntry* m_Buf;
    uint32_t* m_Begin;
    uint32_t* m_End;
};
}
namespace dmGameSystem {
void EnableRenderObjectConstants(dmRender::RenderObject* ro, HComponentRenderConstants constants) { ro->m_ConstantBuffer = constants; }
}
namespace dmGameSystemDDF { struct SpineModelDesc {
    enum BlendMode { BLEND_MODE_ALPHA, BLEND_MODE_ADD, BLEND_MODE_MULT, BLEND_MODE_SCREEN, BLEND_MODE_INHERIT };
    BlendMode m_BlendMode;
}; }
enum spBlendMode { SP_BLEND_MODE_NORMAL, SP_BLEND_MODE_ADDITIVE, SP_BLEND_MODE_MULTIPLY, SP_BLEND_MODE_SCREEN };
struct spSkeleton { uint32_t slotsCount, vertices, indices; float seed; };
struct spSkeletonClipping {};
#define dmLogInfo(...) ((void)0)
#define dmLogError(...) ((void)0)
#define DM_PROPERTY_ADD_U32(...) ((void)0)

namespace dmSpine {
using namespace dmVMath;
struct SpineModelResource { dmGameSystemDDF::SpineModelDesc* m_Ddf; };
struct SpineModelComponent
{
    uint64_t m_GeometryRevision;
    uint32_t m_ComponentIndex;
    spSkeleton* m_SkeletonInstance;
    Matrix4 m_World;
    SpineModelResource* m_Resource;
    dmGameSystem::HComponentRenderConstants m_RenderConstants;
};
static const uint32_t RENDER_OBJECT_OVERFLOW_BLOCK_SHIFT = 5;
static const uint32_t RENDER_OBJECT_OVERFLOW_BLOCK_SIZE = 1U << 5;
static const uint32_t RENDER_OBJECT_OVERFLOW_BLOCK_MASK = (1U << 5) - 1;
#include "cache_types.inc"
static uint32_t g_GenerationCalls;
static uint32_t GenerateIndexedVertexData(dmArray<SpineVertex>& vertices, dmArray<uint32_t>& indices,
    const spSkeleton* skeleton, spSkeletonClipping*, const Matrix4&, const Vector4&,
    dmArray<SpineIndexedDrawDesc>* descs, dmArray<float>&)
{
    ++g_GenerationCalls;
    uint32_t vb = vertices.Size(), ib = indices.Size();
    if (vertices.Capacity() < vb + skeleton->vertices) vertices.SetCapacity(vb + skeleton->vertices);
    if (indices.Capacity() < ib + skeleton->indices) indices.SetCapacity(ib + skeleton->indices);
    vertices.SetSize(vb + skeleton->vertices);
    indices.SetSize(ib + skeleton->indices);
    for (uint32_t i = 0; i < skeleton->vertices; ++i)
    {
        SpineVertex vertex = {};
        vertex.x = skeleton->seed + i;
        vertex.a = 1;
        vertices[vb + i] = vertex;
    }
    for (uint32_t i = 0; i < skeleton->indices; ++i)
        indices[ib + i] = vb + (i == 2 ? skeleton->vertices - 1 : i % skeleton->vertices);
    if (descs && skeleton->indices)
    {
        if (descs->Capacity() == descs->Size()) descs->SetCapacity(descs->Size() + 1);
        SpineIndexedDrawDesc desc = {ib, skeleton->indices, SP_BLEND_MODE_NORMAL};
        descs->Push(desc);
    }
    return skeleton->vertices;
}
struct Texture { dmGraphics::HTexture m_Texture; };
struct TextureSet { Texture* m_Texture; };
struct Scene { TextureSet* m_TextureSet; };
static Texture g_Texture = {31};
static TextureSet g_TextureSet = {&g_Texture};
static Scene g_Scene = {&g_TextureSet};
static Scene* GetSpineScene(const SpineModelComponent*) { return &g_Scene; }
static dmRender::HMaterial GetMaterial(const SpineModelComponent*) { return 41; }
static dmGameSystemDDF::SpineModelDesc::BlendMode SpineBlendModeToRenderBlendMode(spBlendMode mode)
{
    return (dmGameSystemDDF::SpineModelDesc::BlendMode)mode;
}
static void MergeIndexedDrawDescs(const dmArray<SpineIndexedDrawDesc>& src, dmArray<SpineIndexedDrawDesc>& dst)
{
    if (dst.Capacity() < src.Size()) dst.SetCapacity(src.Size());
    dst.SetSize(src.Size());
    for (uint32_t i = 0; i < src.Size(); ++i) dst[i] = src[i];
}
#include "cache_functions.inc"

static void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
struct Fixture
{
    SpineModelWorld world;
    dmGameSystemDDF::SpineModelDesc desc[3];
    SpineModelResource resources[3];
    spSkeleton skeletons[3];
    SpineModelComponent components[3];
    dmRender::RenderListEntry entries[3];
    explicit Fixture(int mode) : world(), desc(), resources(), skeletons(), components(), entries()
    {
        world.m_GeometryCacheMode = mode;
        world.m_GeometryEpoch = 1;
        world.m_VertexDeclaration = world.m_VertexBuffer = world.m_IndexBuffer = 1;
        world.m_RenderObjects.SetCapacity(1);
        world.m_Components.m_Objects.SetCapacity(3);
        for (uint32_t i = 0; i < 3; ++i)
        {
            desc[i].m_BlendMode = dmGameSystemDDF::SpineModelDesc::BLEND_MODE_ALPHA;
            resources[i].m_Ddf = &desc[i];
            skeletons[i] = {1, 4, 6, (float)(i * 10)};
            components[i].m_GeometryRevision = 1;
            components[i].m_ComponentIndex = i;
            components[i].m_SkeletonInstance = &skeletons[i];
            components[i].m_Resource = &resources[i];
            world.m_Components.m_Objects.Push(&components[i]);
            entries[i].m_UserData = i;
        }
        dmGraphics::g_VertexUploads = dmGraphics::g_IndexUploads = g_GenerationCalls = 0;
        dmGraphics::g_VertexGPU.clear();
        dmGraphics::g_IndexGPU.clear();
    }
    ~Fixture()
    {
        for (uint32_t i = 0; i < world.m_RenderObjectOverflowBlocks.Size(); ++i)
            delete[] world.m_RenderObjectOverflowBlocks[i];
        delete world.m_PackReference;
    }
    void Dispatch(dmRender::RenderListOperation operation, uint32_t* begin = 0, uint32_t* end = 0)
    {
        dmRender::RenderListDispatchParams params = {&world, operation, 1, entries, begin, end};
        RenderListDispatch(params);
    }
    void Begin() { dmRender::g_Submitted.clear(); Dispatch(dmRender::RENDER_LIST_OPERATION_BEGIN); }
    void Batch(std::initializer_list<uint32_t> order)
    {
        std::vector<uint32_t> indices(order);
        if (!indices.empty()) Dispatch(dmRender::RENDER_LIST_OPERATION_BATCH, indices.data(), indices.data() + indices.size());
    }
    void End() { Dispatch(dmRender::RENDER_LIST_OPERATION_END); }
    void Pass(std::initializer_list<uint32_t> order) { Begin(); Batch(order); End(); }
};

static void TestModesAndReuse()
{
    for (int mode = 0; mode <= 2; ++mode)
    {
        Fixture f(mode);
        f.Pass({0, 1});
        const std::vector<uint8_t> expected = dmGraphics::g_VertexGPU;
        uint32_t calls = g_GenerationCalls;
        f.Pass({0, 1});
        Require(dmGraphics::g_VertexGPU == expected, "same-frame geometry changed");
        Require(dmGraphics::g_IndexUploads == (mode ? 1U : 2U), "complete pass upload reuse");
        if (mode == 1) Require(g_GenerationCalls == calls, "cache hit regenerated geometry");
        ++f.world.m_GeometryEpoch;
        f.Pass({0, 1});
        Require(dmGraphics::g_IndexUploads == (mode ? 2U : 3U), "cross-frame reuse");
        Require(!f.world.m_GeometryCacheFailed, "valid pass failed diagnostics");
    }
}
static void TestDivergence()
{
    for (int mode = 1; mode <= 2; ++mode)
    {
        Fixture f(mode);
        f.Pass({0, 1, 2});
        f.Pass({0});
        Require(dmGraphics::g_IndexGPU.size() == 12, "shorter prefix kept old GPU suffix");
        f.Pass({0, 1, 2});
        Require(dmGraphics::g_IndexGPU.size() == 36, "longer pass lost suffix");
        f.Pass({2, 1, 0});
        Require(f.world.m_VertexBufferData[0].x == 20, "reorder reused wrong component");
        f.skeletons[2].seed = 99;
        InvalidateGeometryCache(&f.components[2]);
        f.Pass({2, 1, 0});
        Require(f.world.m_VertexBufferData[0].x == 99, "revision change reused old geometry");
        uint32_t uploads = dmGraphics::g_IndexUploads;
        f.Pass({});
        f.Pass({2, 1, 0});
        Require(dmGraphics::g_IndexUploads == uploads, "empty pass invalidated untouched residency");
        Require(!f.world.m_GeometryCacheFailed, "divergence failed diagnostics");
    }
}
static void TestMixedAndOverflow()
{
    Fixture f(2);
    f.Begin(); f.Batch({0}); f.Batch({1}); f.Batch({2}); f.End();
    Require(f.world.m_RenderObjectsInUse == 3, "overflow objects missing");
    f.Begin(); f.Batch({0}); f.Batch({1}); f.Batch({2}); f.End();
    Require(f.world.m_CacheHits == 3 && dmGraphics::g_IndexUploads == 1, "batch boundaries prevented reuse");
    f.desc[1].m_BlendMode = dmGameSystemDDF::SpineModelDesc::BLEND_MODE_ADD;
    f.Begin(); f.Batch({0}); f.Batch({1}); f.Batch({2}); f.End();
    Require(f.world.m_ResidentEpoch == 0, "mixed blend pass became resident");
    f.desc[1].m_BlendMode = dmGameSystemDDF::SpineModelDesc::BLEND_MODE_INHERIT;
    f.Begin(); f.Batch({0}); f.Batch({1}); f.Batch({2}); f.End();
    Require(!f.world.m_GeometryCacheFailed && f.world.m_ResidentEpoch == 0, "inherit fallback");
}
static void TestIndexWidths()
{
    for (int mode = 0; mode <= 2; ++mode)
    {
        Fixture f(mode);
        const uint32_t counts[] = {65535, 65536, 65537, 4};
        for (uint32_t count : counts)
        {
            f.skeletons[0].vertices = count;
            f.skeletons[0].indices = 3;
            InvalidateGeometryCache(&f.components[0]);
            f.Pass({0}); f.Pass({0});
            Require(f.world.m_Is16BitIndex == (count <= 65536), "wrong index width");
            Require(dmGraphics::g_IndexGPU.size() == (count <= 65536 ? 6U : 12U), "wrong GPU index bytes");
            Require(!f.world.m_GeometryCacheFailed, "width transition failed diagnostics");
        }
    }
}
static void TestEmptyGeometry()
{
    for (int mode = 1; mode <= 2; ++mode)
    {
        Fixture f(mode);
        f.skeletons[0].vertices = f.skeletons[0].indices = 0;
        f.skeletons[2].vertices = f.skeletons[2].indices = 0;
        f.Pass({0, 1});
        const std::vector<uint8_t> expected = dmGraphics::g_IndexGPU;
        f.Pass({0}); // Matching empty prefix must not overwrite GPU residency.
        f.Pass({2}); // Divergent empty component must not discard retained CPU bytes.
        f.Pass({0, 1});
        Require(dmGraphics::g_IndexUploads == 1 && dmGraphics::g_IndexGPU == expected, "empty geometry damaged residency");
        Require(!f.world.m_GeometryCacheFailed, "empty geometry failed diagnostics");
    }
}
static void TestCorruptDirectIndices()
{
    Fixture f(2);
    f.skeletons[0].vertices = 65537;
    f.skeletons[0].indices = 3;
    f.Begin(); f.Batch({0});
    f.world.m_IndexBufferData[0] = 123; // Corrupt the actual upload source after component verification.
    f.End();
    Require(f.world.m_GeometryCacheFailed, "corrupt direct indices went undetected");
    uint32_t first = 1;
    memcpy(&first, dmGraphics::g_IndexGPU.data(), sizeof(first));
    Require(first == 0, "corrupt direct indices reached current GPU pass");
    Require(dmGraphics::g_IndexUploads == 1, "diagnostics ran after the GPU upload");
    Require(f.world.m_ResidentEpoch == 0, "failed cache remained resident");
    f.Pass({0});
    Require(dmGraphics::g_IndexUploads == 2, "failed cache did not use upstream path");
}
static void TestCorruptRenderObject()
{
    for (uint32_t vertices : {4U, 65537U})
    {
        Fixture f(2);
        f.skeletons[0].vertices = vertices;
        f.components[0].m_RenderConstants = 51;
        f.Begin(); f.Batch({0}); f.Batch({1}); f.End();
        f.Begin(); f.Batch({0}); f.Batch({1});
        dmRender::RenderObject& ro = *dmRender::g_Submitted[0];
        ro.m_VertexStart = 1; // Still in bounds, so a self-snapshot cannot catch this.
        ro.m_VertexCount = 3;
        ro.m_Material = 99;
        ro.m_Textures[0] = 99;
        ro.m_ConstantBuffer = 99;
        ro.m_SourceBlendFactor = dmGraphics::BLEND_FACTOR_DST_COLOR;
        f.End();
        Require(f.world.m_GeometryCacheFailed, "corrupt RenderObject went undetected");
        Require(ro.m_VertexStart == 0 && ro.m_VertexCount == 6, "current RenderObject range was not restored");
        Require(ro.m_Material == 41 && ro.m_Textures[0] == 31 && ro.m_ConstantBuffer == 51, "current RenderObject state was not restored");
        Require(ro.m_SourceBlendFactor == dmGraphics::BLEND_FACTOR_ONE, "current blend state was not restored");
        Require(dmGraphics::g_IndexUploads == 2 && dmGraphics::g_IndexGPU.size() == (vertices == 4 ? 24U : 48U), "RO failure did not reupload valid 16/32-bit data");
        Require(f.world.m_ResidentEpoch == 0, "RO failure kept residency");
    }
}
static void TestCorruptRetainedPackedIndices()
{
    Fixture f(2);
    f.Pass({0});
    f.Begin(); f.Batch({0});
    f.world.m_PackedIndexBufferData.Begin()[0] ^= 1;
    f.End();
    Require(f.world.m_GeometryCacheFailed, "corrupt retained packed bytes went undetected");
    Require(dmGraphics::g_IndexUploads == 2 && dmGraphics::g_IndexGPU[0] == 0, "retained packed bytes were not repaired before upload");
}
static void TestCorruptCachedVertices()
{
    Fixture f(2);
    f.Pass({0});
    f.world.m_VertexBufferData[0].x = 999;
    f.Begin(); f.Batch({0}); f.End();
    Require(f.world.m_GeometryCacheFailed, "corrupt cached vertices went undetected");
    float first = 999;
    memcpy(&first, dmGraphics::g_VertexGPU.data(), sizeof(first));
    Require(first == 0 && f.world.m_ResidentEpoch == 0, "component verification did not repair the current pass");
}
} // dmSpine

int main()
{
    using namespace dmSpine;
    struct Test { const char* name; void (*run)(); };
    const Test tests[] = {
        {"modes_and_same_frame_reuse", TestModesAndReuse},
        {"prefix_divergence_revision_and_empty", TestDivergence},
        {"mixed_blend_and_overflow", TestMixedAndOverflow},
        {"16_32_bit_width_transitions", TestIndexWidths},
        {"empty_geometry_preserves_residency", TestEmptyGeometry},
        {"corrupt_direct_indices_recover_current_pass", TestCorruptDirectIndices},
        {"corrupt_render_object_recover_current_pass", TestCorruptRenderObject},
        {"corrupt_retained_packed_indices", TestCorruptRetainedPackedIndices},
        {"corrupt_cached_vertices", TestCorruptCachedVertices},
    };
    int failures = 0;
    for (const Test& test : tests)
    {
        try { test.run(); std::cout << "PASS " << test.name << '\n'; }
        catch (const std::exception& error) { ++failures; std::cout << "FAIL " << test.name << ": " << error.what() << '\n'; }
    }
    return failures ? 1 : 0;
}
