//
// Copyright (c) 2008-2019 the Urho3D project.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//

// Embree includes must be first
#include <embree3/rtcore.h>
#include <embree3/rtcore_ray.h>
#define _SSIZE_T_DEFINED

#include "../Glow/LightmapBaker.h"
#include "../Glow/LightmapUVGenerator.h"
#include "../Graphics/Camera.h"
#include "../Graphics/Graphics.h"
#include "../Graphics/Material.h"
#include "../Graphics/Model.h"
#include "../Graphics/Octree.h"
#include "../Graphics/StaticModel.h"
#include "../Graphics/RenderPath.h"
#include "../Graphics/RenderSurface.h"
#include "../Graphics/Texture2D.h"
#include "../Graphics/View.h"
#include "../Graphics/Viewport.h"
#include "../Math/AreaAllocator.h"
#include "../Resource/ResourceCache.h"
#include "../Resource/XMLFile.h"

// TODO: Use thread pool?
#include <future>

namespace Urho3D
{

/// Size of Embree ray packed.
static const unsigned RayPacketSize = 16;

/// Description of lightmap region.
struct LightmapRegion
{
    /// Construct default.
    LightmapRegion() = default;
    /// Construct actual region.
    LightmapRegion(unsigned index, const IntVector2& position, const IntVector2& size, unsigned maxSize)
        : lightmapIndex_(index)
        , lightmapTexelRect_(position, position + size)
    {
        lightmapUVRect_.min_ = static_cast<Vector2>(lightmapTexelRect_.Min()) / static_cast<float>(maxSize);
        lightmapUVRect_.max_ = static_cast<Vector2>(lightmapTexelRect_.Max()) / static_cast<float>(maxSize);
    }
    /// Get lightmap offset vector.
    Vector4 GetScaleOffset() const
    {
        const Vector2 offset = lightmapUVRect_.Min();
        const Vector2 size = lightmapUVRect_.Size();
        return { size.x_, size.y_, offset.x_, offset.y_ };
    }

    /// Lightmap index.
    unsigned lightmapIndex_;
    /// Lightmap rectangle (in texels).
    IntRect lightmapTexelRect_;
    /// Lightmap rectangle (UV).
    Rect lightmapUVRect_;
};

/// Description of lightmap receiver.
struct LightReceiver
{
    /// Node.
    Node* node_;
    /// Static model.
    StaticModel* staticModel_{};
    /// Lightmap region.
    LightmapRegion region_;
};

/// Lightmap description.
struct LightmapDesc
{
    /// Area allocator for lightmap texture.
    AreaAllocator allocator_;
    /// Baking bakingScene.
    SharedPtr<Scene> bakingScene_;
    /// Baking camera.
    Camera* bakingCamera_{};
    /// Render texture placeholder.
    SharedPtr<Texture2D> renderTexturePlaceholder_;
    /// Render surface placeholder.
    SharedPtr<RenderSurface> renderSurfacePlaceholder_;
};

struct LightmapBakerImpl
{
    /// Construct.
    LightmapBakerImpl(Context* context, const LightmapBakingSettings& settings, Scene* scene,
        const ea::vector<Node*>& lightReceivers, const ea::vector<Node*>& obstacles, const ea::vector<Node*>& lights)
        : context_(context)
        , settings_(settings)
        , lightReceivers_(lightReceivers.size())
        , obstacles_(obstacles)
        , lights_(lights)
    {
        for (unsigned i = 0; i < lightReceivers.size(); ++i)
            lightReceivers_[i].node_ = lightReceivers[i];
    }
    /// Destruct.
    ~LightmapBakerImpl()
    {
        if (embreeScene_)
            rtcReleaseScene(embreeScene_);
        if (embreeDevice_)
            rtcReleaseDevice(embreeDevice_);
    }
    /// Validate settings and whatever.
    bool Validate() const
    {
        if (settings_.lightmapSize_ % settings_.numParallelChunks_ != 0)
            return false;
        if (settings_.lightmapSize_ % RayPacketSize != 0)
            return false;
        return true;
    }

    /// Context.
    Context* context_{};

    /// Settings.
    const LightmapBakingSettings settings_;
    /// Scene.
    Scene* scene_{};
    /// Light receivers.
    ea::vector<LightReceiver> lightReceivers_;
    /// Light obstacles.
    ea::vector<Node*> obstacles_;
    /// Lights.
    ea::vector<Node*> lights_;

    /// Max length of the ray.
    float maxRayLength_{};
    /// Lightmaps.
    ea::vector<LightmapDesc> lightmaps_;
    /// Baking render path.
    SharedPtr<RenderPath> bakingRenderPath_;
    /// Embree device.
    RTCDevice embreeDevice_{};
    /// Embree scene.
    RTCScene embreeScene_{};
    /// Render texture placeholder.
    SharedPtr<Texture2D> renderTexturePlaceholder_;
    /// Render surface placeholder.
    SharedPtr<RenderSurface> renderSurfacePlaceholder_;

    /// Calculation: current lightmap index.
    unsigned currentLightmapIndex_{ M_MAX_UNSIGNED };
    /// Calculation: texel positions
    ea::vector<Vector4> positionBuffer_;
    /// Calculation: texel smooth positions
    ea::vector<Vector4> smoothPositionBuffer_;
    /// Calculation: texel face normals
    ea::vector<Vector4> faceNormalBuffer_;
    /// Calculation: texel smooth normals
    ea::vector<Vector4> smoothNormalBuffer_;
};

/// Calculate model lightmap size.
static IntVector2 CalculateModelLightmapSize(const LightmapBakingSettings& settings,
    Model* model, const Vector3& scale)
{
    const Variant& lightmapSizeVar = model->GetMetadata(LightmapUVGenerationSettings::LightmapSizeKey);
    const Variant& lightmapDensityVar = model->GetMetadata(LightmapUVGenerationSettings::LightmapDensityKey);

    const auto modelLightmapSize = static_cast<Vector2>(lightmapSizeVar.GetIntVector2());
    const unsigned modelLightmapDensity = lightmapDensityVar.GetUInt();

    const float nodeScale = scale.DotProduct(DOT_SCALE);
    const float rescaleFactor = nodeScale * static_cast<float>(settings.texelDensity_) / modelLightmapDensity;
    const float clampedRescaleFactor = ea::max(settings.minLightmapScale_, rescaleFactor);

    return VectorCeilToInt(modelLightmapSize * clampedRescaleFactor);
}

/// Allocate lightmap region.
static LightmapRegion AllocateLightmapRegion(const LightmapBakingSettings& settings,
    ea::vector<LightmapDesc>& lightmaps, const IntVector2& size)
{
    const int padding = static_cast<int>(settings.lightmapPadding_);
    const IntVector2 paddedSize = size + 2 * padding * IntVector2::ONE;

    // Try existing maps
    unsigned lightmapIndex = 0;
    for (LightmapDesc& lightmapDesc : lightmaps)
    {
        IntVector2 paddedPosition;
        if (lightmapDesc.allocator_.Allocate(paddedSize.x_, paddedSize.y_, paddedPosition.x_, paddedPosition.y_))
        {
            const IntVector2 position = paddedPosition + padding * IntVector2::ONE;
            return { lightmapIndex, position, size, settings.lightmapSize_ };
        }
        ++lightmapIndex;
    }

    // Create new map
    const int lightmapSize = static_cast<int>(settings.lightmapSize_);
    LightmapDesc& lightmapDesc = lightmaps.push_back();

    // Allocate dedicated map for this specific region
    if (size.x_ > lightmapSize || size.y_ > lightmapSize)
    {
        const int sizeX = (size.x_ + RayPacketSize - 1) / RayPacketSize * RayPacketSize;

        lightmapDesc.allocator_.Reset(sizeX, size.y_, 0, 0, false);

        IntVector2 position;
        const bool success = lightmapDesc.allocator_.Allocate(sizeX, size.y_, position.x_, position.y_);

        assert(success);
        assert(position == IntVector2::ZERO);

        return { lightmapIndex, IntVector2::ZERO, size, settings.lightmapSize_ };
    }

    // Allocate chunk from new map
    lightmapDesc.allocator_.Reset(lightmapSize, lightmapSize, 0, 0, false);

    IntVector2 paddedPosition;
    const bool success = lightmapDesc.allocator_.Allocate(paddedSize.x_, paddedSize.y_, paddedPosition.x_, paddedPosition.y_);

    assert(success);
    assert(paddedPosition == IntVector2::ZERO);

    const IntVector2 position = paddedPosition + padding * IntVector2::ONE;
    return { lightmapIndex, position, size, settings.lightmapSize_ };
}

/// Allocate lightmap regions for light receivers.
static void AllocateLightmapRegions(const LightmapBakingSettings& settings,
    ea::vector<LightReceiver>& lightReceivers, ea::vector<LightmapDesc>& lightmaps)
{
    for (LightReceiver& lightReceiver : lightReceivers)
    {
        Node* node = lightReceiver.node_;

        if (auto staticModel = node->GetComponent<StaticModel>())
        {
            Model* model = staticModel->GetModel();
            const IntVector2 nodeLightmapSize = CalculateModelLightmapSize(settings, model, node->GetWorldScale());

            lightReceiver.staticModel_ = staticModel;
            lightReceiver.region_ = AllocateLightmapRegion(settings, lightmaps, nodeLightmapSize);
        }
    }
}

/// Calculate bounding box of all light receivers.
BoundingBox CalculateReceiversBoundingBox(const ea::vector<LightReceiver>& lightReceivers)
{
    BoundingBox boundingBox;
    for (const LightReceiver& lightReceiver : lightReceivers)
    {
        if (lightReceiver.staticModel_)
            boundingBox.Merge(lightReceiver.staticModel_->GetWorldBoundingBox());
    }
    return boundingBox;
}

/// Load render path.
SharedPtr<RenderPath> LoadRenderPath(Context* context, const ea::string& renderPathName)
{
    auto renderPath = MakeShared<RenderPath>();
    auto renderPathXml = context->GetCache()->GetResource<XMLFile>(renderPathName);
    if (!renderPath->Load(renderPathXml))
        return nullptr;
    return renderPath;
}

/// Initialize camera from bounding box.
void InitializeCameraBoundingBox(Camera* camera, const BoundingBox& boundingBox)
{
    Node* node = camera->GetNode();

    const float zNear = 1.0f;
    const float zFar = boundingBox.Size().z_ + zNear;
    Vector3 position = boundingBox.Center();
    position.z_ = boundingBox.min_.z_ - zNear;

    node->SetPosition(position);
    node->SetDirection(Vector3::FORWARD);

    camera->SetOrthographic(true);
    camera->SetOrthoSize(Vector2(boundingBox.Size().x_, boundingBox.Size().y_));
    camera->SetNearClip(zNear);
    camera->SetFarClip(zFar);
}

/// Initialize lightmap baking scenes.
void InitializeLightmapBakingScenes(Context* context, Material* bakingMaterial,
    ea::vector<LightmapDesc>& lightmaps, ea::vector<LightReceiver>& lightReceivers)
{
    const BoundingBox lightReceiversBoundingBox = CalculateReceiversBoundingBox(lightReceivers);

    // Allocate lightmap baking scenes
    for (LightmapDesc& lightmapDesc : lightmaps)
    {
        auto bakingScene = MakeShared<Scene>(context);
        bakingScene->CreateComponent<Octree>();

        auto camera = bakingScene->CreateComponent<Camera>();
        InitializeCameraBoundingBox(camera, lightReceiversBoundingBox);

        lightmapDesc.bakingCamera_ = camera;
        lightmapDesc.bakingScene_ = bakingScene;
    }

    // Prepare baking scenes
    for (const LightReceiver& receiver : lightReceivers)
    {
        LightmapDesc& lightmapDesc = lightmaps[receiver.region_.lightmapIndex_];
        Scene* bakingScene = lightmapDesc.bakingScene_;

        if (receiver.staticModel_)
        {
            auto material = bakingMaterial->Clone();
            material->SetShaderParameter("LMOffset", receiver.region_.GetScaleOffset());

            Node* node = bakingScene->CreateChild();
            node->SetPosition(receiver.node_->GetWorldPosition());
            node->SetRotation(receiver.node_->GetWorldRotation());
            node->SetScale(receiver.node_->GetWorldScale());

            StaticModel* staticModel = node->CreateComponent<StaticModel>();
            staticModel->SetModel(receiver.staticModel_->GetModel());
            staticModel->SetMaterial(material);
        }
    }
}
/// Parsed model key and value.
struct ParsedModelKeyValue
{
    Model* model_{};
    SharedPtr<ModelView> parsedModel_;
};

/// Parse model data.
ParsedModelKeyValue ParseModelForEmbree(Model* model)
{
    NativeModelView nativeModelView(model->GetContext());
    nativeModelView.ImportModel(model);

    auto modelView = MakeShared<ModelView>(model->GetContext());
    modelView->ImportModel(nativeModelView);

    return { model, modelView };
}

/// Embree geometry desc.
struct EmbreeGeometry
{
    /// Node.
    Node* node_{};
    /// Geometry index.
    unsigned geometryIndex_{};
    /// Geometry LOD.
    unsigned geometryLOD_{};
    /// Embree geometry.
    RTCGeometry embreeGeometry_;
};

/// Create Embree geometry from geometry view.
RTCGeometry CreateEmbreeGeometry(RTCDevice embreeDevice, const GeometryLODView& geometryLODView, Node* node)
{
    const Matrix3x4 worldTransform = node->GetWorldTransform();
    RTCGeometry embreeGeometry = rtcNewGeometry(embreeDevice, RTC_GEOMETRY_TYPE_TRIANGLE);

    float* vertices = reinterpret_cast<float*>(rtcSetNewGeometryBuffer(embreeGeometry, RTC_BUFFER_TYPE_VERTEX,
        0, RTC_FORMAT_FLOAT3, sizeof(Vector3), geometryLODView.vertices_.size()));

    for (unsigned i = 0; i < geometryLODView.vertices_.size(); ++i)
    {
        const Vector3 localPosition = static_cast<Vector3>(geometryLODView.vertices_[i].position_);
        const Vector3 worldPosition = worldTransform * localPosition;
        vertices[i * 3 + 0] = worldPosition.x_;
        vertices[i * 3 + 1] = worldPosition.y_;
        vertices[i * 3 + 2] = worldPosition.z_;
    }

    unsigned* indices = reinterpret_cast<unsigned*>(rtcSetNewGeometryBuffer(embreeGeometry, RTC_BUFFER_TYPE_INDEX,
        0, RTC_FORMAT_UINT3, sizeof(unsigned) * 3, geometryLODView.faces_.size()));

    for (unsigned i = 0; i < geometryLODView.faces_.size(); ++i)
    {
        indices[i * 3 + 0] = geometryLODView.faces_[i].indices_[0];
        indices[i * 3 + 1] = geometryLODView.faces_[i].indices_[1];
        indices[i * 3 + 2] = geometryLODView.faces_[i].indices_[2];
    }

    rtcCommitGeometry(embreeGeometry);
    return embreeGeometry;
}

/// Create Embree geometry from parsed model.
ea::vector<EmbreeGeometry> CreateEmbreeGeometryArray(RTCDevice embreeDevice, ModelView* modelView, Node* node)
{
    ea::vector<EmbreeGeometry> result;

    unsigned geometryIndex = 0;
    for (const GeometryView& geometryView : modelView->GetGeometries())
    {
        unsigned geometryLod = 0;
        for (const GeometryLODView& geometryLODView : geometryView.lods_)
        {
            const RTCGeometry embreeGeometry = CreateEmbreeGeometry(embreeDevice, geometryLODView, node);
            result.push_back(EmbreeGeometry{ node, geometryIndex, geometryLod, embreeGeometry });
            ++geometryLod;
        }
        ++geometryIndex;
    }
    return result;
}

/// Create render surface texture for lightmap.
SharedPtr<Texture2D> CreateRenderTextureForLightmap(Context* context, int width, int height)
{
    auto texture = MakeShared<Texture2D>(context);
    texture->SetSize(width, height, Graphics::GetRGBAFormat(), TEXTURE_RENDERTARGET);
    return texture;
}

/// Read RGBA32 float texture to vector.
void ReadTextureRGBA32Float(Texture* texture, ea::vector<Vector4>& dest)
{
    auto texture2D = dynamic_cast<Texture2D*>(texture);
    const unsigned numElements = texture->GetDataSize(texture->GetWidth(), texture->GetHeight()) / sizeof(Vector4);
    dest.resize(numElements);
    texture2D->GetData(0, dest.data());
}

LightmapBaker::LightmapBaker(Context* context)
    : Object(context)
{
}

LightmapBaker::~LightmapBaker()
{
}

bool LightmapBaker::Initialize(const LightmapBakingSettings& settings, Scene* scene,
    const ea::vector<Node*>& lightReceivers, const ea::vector<Node*>& obstacles, const ea::vector<Node*>& lights)
{
    impl_ = ea::make_unique<LightmapBakerImpl>(context_, settings, scene, lightReceivers, obstacles, lights);
    if (!impl_->Validate())
        return false;

    // Prepare metadata and baking scenes
    AllocateLightmapRegions(impl_->settings_, impl_->lightReceivers_, impl_->lightmaps_);

    const BoundingBox lightReceiversBoundingBox = CalculateReceiversBoundingBox(impl_->lightReceivers_);
    impl_->maxRayLength_ = lightReceiversBoundingBox.Size().Length();

    impl_->bakingRenderPath_ = LoadRenderPath(context_, impl_->settings_.bakingRenderPath_);

    Material* bakingMaterial = context_->GetCache()->GetResource<Material>(settings.bakingMaterial_);
    InitializeLightmapBakingScenes(context_, bakingMaterial, impl_->lightmaps_, impl_->lightReceivers_);

    // Create render surfaces
    const int lightmapSize = impl_->settings_.lightmapSize_;
    impl_->renderTexturePlaceholder_ = CreateRenderTextureForLightmap(context_, lightmapSize, lightmapSize);
    impl_->renderSurfacePlaceholder_ = impl_->renderTexturePlaceholder_->GetRenderSurface();

    for (LightmapDesc& lightmapDesc : impl_->lightmaps_)
    {
        const int width = lightmapDesc.allocator_.GetWidth();
        const int height = lightmapDesc.allocator_.GetHeight();
        if (width != lightmapSize || height != lightmapSize)
        {
            lightmapDesc.renderTexturePlaceholder_ = CreateRenderTextureForLightmap(context_, width, height);
            lightmapDesc.renderSurfacePlaceholder_ = lightmapDesc.renderTexturePlaceholder_->GetRenderSurface();
        }
        else
        {
            lightmapDesc.renderTexturePlaceholder_ = impl_->renderTexturePlaceholder_;
            lightmapDesc.renderSurfacePlaceholder_ = impl_->renderSurfacePlaceholder_;
        }
    }

    return true;
}

void LightmapBaker::CookRaytracingScene()
{
    // Load models
    ea::vector<std::future<ParsedModelKeyValue>> asyncParsedModels;
    for (Node* node : impl_->obstacles_)
    {
        if (auto staticModel = node->GetComponent<StaticModel>())
            asyncParsedModels.push_back(std::async(ParseModelForEmbree, staticModel->GetModel()));
    }

    // Prepare model cache
    ea::unordered_map<Model*, SharedPtr<ModelView>> parsedModelCache;
    for (auto& asyncModel : asyncParsedModels)
    {
        const ParsedModelKeyValue& parsedModel = asyncModel.get();
        parsedModelCache.emplace(parsedModel.model_, parsedModel.parsedModel_);
    }

    // Prepare Embree scene
    impl_->embreeDevice_ = rtcNewDevice("");
    impl_->embreeScene_ = rtcNewScene(impl_->embreeDevice_);

    ea::vector<std::future<ea::vector<EmbreeGeometry>>> asyncEmbreeGeometries;
    for (Node* node : impl_->obstacles_)
    {
        if (auto staticModel = node->GetComponent<StaticModel>())
        {
            ModelView* parsedModel = parsedModelCache[staticModel->GetModel()];
            asyncEmbreeGeometries.push_back(std::async(CreateEmbreeGeometryArray, impl_->embreeDevice_, parsedModel, node));
        }
    }

    // Collect and attach Embree geometries
    for (auto& asyncGeometry : asyncEmbreeGeometries)
    {
        const ea::vector<EmbreeGeometry> embreeGeometriesArray = asyncGeometry.get();
        for (const EmbreeGeometry& embreeGeometry : embreeGeometriesArray)
        {
            rtcAttachGeometry(impl_->embreeScene_, embreeGeometry.embreeGeometry_);
            rtcReleaseGeometry(embreeGeometry.embreeGeometry_);
        }
    }

    rtcCommitScene(impl_->embreeScene_);
}

unsigned LightmapBaker::GetNumLightmaps() const
{
    return impl_->lightmaps_.size();
}

bool LightmapBaker::RenderLightmapGBuffer(unsigned index)
{
    if (index >= GetNumLightmaps())
        return false;

    Graphics* graphics = GetGraphics();
    ResourceCache* cache = GetCache();
    const LightmapDesc& lightmapDesc = impl_->lightmaps_[index];

    if (!graphics->BeginFrame())
        return false;

    // Setup viewport
    Viewport viewport(context_);
    viewport.SetCamera(lightmapDesc.bakingCamera_);
    viewport.SetRect(IntRect::ZERO);
    viewport.SetRenderPath(impl_->bakingRenderPath_);
    viewport.SetScene(lightmapDesc.bakingScene_);

    // Render bakingScene
    View view(context_);
    view.Define(lightmapDesc.renderSurfacePlaceholder_, &viewport);
    view.Update(FrameInfo());
    view.Render();

    graphics->EndFrame();

    // Fill temporary buffers
    impl_->currentLightmapIndex_ = index;

    ReadTextureRGBA32Float(view.GetExtraRenderTarget("position"), impl_->positionBuffer_);
    ReadTextureRGBA32Float(view.GetExtraRenderTarget("smoothposition"), impl_->smoothPositionBuffer_);
    ReadTextureRGBA32Float(view.GetExtraRenderTarget("facenormal"), impl_->faceNormalBuffer_);
    ReadTextureRGBA32Float(view.GetExtraRenderTarget("smoothnormal"), impl_->smoothNormalBuffer_);

    return true;
}

bool LightmapBaker::BakeLightmap(LightmapBakedData& data)
{
    const LightmapDesc& lightmapDesc = impl_->lightmaps_[impl_->currentLightmapIndex_];
    const int lightmapWidth = lightmapDesc.allocator_.GetWidth();
    const int lightmapHeight = lightmapDesc.allocator_.GetHeight();

    // Prepare output buffers
    data.lightmapSize_ = { lightmapWidth, lightmapHeight };
    data.backedLighting_.resize(lightmapWidth * lightmapHeight);
    ea::fill(data.backedLighting_.begin(), data.backedLighting_.end(), Color::WHITE);

    Vector3 lightDirection;
    for (Node* lightNode : impl_->lights_)
    {
        if (auto light = lightNode->GetComponent<Light>())
        {
            if (light->GetLightType() == LIGHT_DIRECTIONAL)
            {
                lightDirection = lightNode->GetWorldDirection();
                break;
            }
        }
    }
    const Vector3 rayDirection = -lightDirection.Normalized();

    // Process rows in multiple threads
    ea::vector<std::future<void>> tasks;
    const unsigned numRayPackets = static_cast<unsigned>(lightmapWidth / RayPacketSize);
    const unsigned chunkHeight = static_cast<unsigned>(lightmapHeight / impl_->settings_.numParallelChunks_);
    for (unsigned parallelChunkIndex = 0; parallelChunkIndex < impl_->settings_.numParallelChunks_; ++parallelChunkIndex)
    {
        tasks.push_back(std::async([=, &data]()
        {
            alignas(64) RTCRayHit16 rayHit16;
            alignas(64) int rayValid[RayPacketSize];
            float diffuseLight[RayPacketSize];

            const unsigned fromY = parallelChunkIndex * chunkHeight;
            const unsigned toY = ea::min((parallelChunkIndex + 1) * chunkHeight, static_cast<unsigned>(lightmapHeight));
            for (unsigned y = fromY; y < toY; ++y)
            {
                for (unsigned rayPacketIndex = 0; rayPacketIndex < numRayPackets; ++rayPacketIndex)
                {
                    const unsigned fromX = rayPacketIndex * RayPacketSize;
                    const unsigned baseIndex = y * lightmapWidth + fromX;

                    unsigned numValidRays = 0;
                    for (unsigned i = 0; i < RayPacketSize; ++i)
                    {
                        const unsigned index = baseIndex + i;

                        const unsigned geometryId = static_cast<unsigned>(impl_->positionBuffer_[index].w_);
                        if (!geometryId)
                        {
                            rayValid[i] = 0;
                            rayHit16.ray.tnear[i] = 0.0f;
                            rayHit16.ray.tfar[i] = -1.0f;
                            rayHit16.hit.geomID[i] = RTC_INVALID_GEOMETRY_ID;
                            continue;
                        }

                        const Vector3 position = static_cast<Vector3>(impl_->positionBuffer_[index]);
                        const Vector3 smoothNormal = static_cast<Vector3>(impl_->smoothNormalBuffer_[index]);

                        diffuseLight[i] = ea::max(0.0f, smoothNormal.DotProduct(rayDirection));

                        const Vector3 rayOrigin = position + rayDirection * 0.001f;

                        rayValid[i] = -1;
                        rayHit16.ray.org_x[i] = rayOrigin.x_;
                        rayHit16.ray.org_y[i] = rayOrigin.y_;
                        rayHit16.ray.org_z[i] = rayOrigin.z_;
                        rayHit16.ray.dir_x[i] = rayDirection.x_;
                        rayHit16.ray.dir_y[i] = rayDirection.y_;
                        rayHit16.ray.dir_z[i] = rayDirection.z_;
                        rayHit16.ray.tnear[i] = 0.0f;
                        rayHit16.ray.tfar[i] = impl_->maxRayLength_;
                        rayHit16.ray.time[i] = 0.0f;
                        rayHit16.ray.id[i] = 0;
                        rayHit16.ray.mask[i] = 0xffffffff;
                        rayHit16.ray.flags[i] = 0xffffffff;
                        rayHit16.hit.geomID[i] = RTC_INVALID_GEOMETRY_ID;

                        ++numValidRays;
                    }

                    if (numValidRays == 0)
                        continue;

                    RTCIntersectContext rayContext;
                    rtcInitIntersectContext(&rayContext);
                    rtcIntersect16(rayValid, impl_->embreeScene_, &rayContext, &rayHit16);

                    for (unsigned i = 0; i < RayPacketSize; ++i)
                    {
                        if (rayValid[i])
                        {
                            const unsigned index = baseIndex + i;
                            const float shadow = rayHit16.hit.geomID[i] == RTC_INVALID_GEOMETRY_ID ? 1.0f : 0.0f;
                            data.backedLighting_[index] = Color::WHITE * diffuseLight[i] * shadow;
                        }
                    }
                }
            }
        }));
    }

    // Wait for async tasks
    for (auto& task : tasks)
        task.wait();

    return true;
}

void LightmapBaker::ApplyLightmapsToScene(unsigned baseLightmapIndex)
{
    for (const LightReceiver& receiver : impl_->lightReceivers_)
    {
        if (receiver.staticModel_)
        {
            receiver.staticModel_->SetLightmap(true);
            receiver.staticModel_->SetLightmapIndex(baseLightmapIndex + receiver.region_.lightmapIndex_);
            receiver.staticModel_->SetLightmapScaleOffset(receiver.region_.GetScaleOffset());
        }
    }
}

}