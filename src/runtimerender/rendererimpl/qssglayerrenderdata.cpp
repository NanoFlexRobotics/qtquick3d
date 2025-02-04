// Copyright (C) 2008-2012 NVIDIA Corporation.
// Copyright (C) 2022 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only

#include "qssglayerrenderdata_p.h"

#include <QtQuick3DRuntimeRender/private/qssgrenderer_p.h>
#include <QtQuick3DRuntimeRender/private/qssgrenderlight_p.h>
#include <QtQuick3DRuntimeRender/private/qssgrhicustommaterialsystem_p.h>
#include <QtQuick3DRuntimeRender/private/qssgrhiquadrenderer_p.h>
#include <QtQuick3DRuntimeRender/private/qssgrhiparticles_p.h>
#include <QtQuick3DRuntimeRender/private/qssgrenderlayer_p.h>
#include <QtQuick3DRuntimeRender/private/qssgrendereffect_p.h>
#include <QtQuick3DRuntimeRender/private/qssgrendercamera_p.h>
#include <QtQuick3DRuntimeRender/private/qssgrenderskeleton_p.h>
#include <QtQuick3DRuntimeRender/private/qssgrenderjoint_p.h>
#include <QtQuick3DRuntimeRender/private/qssgrendermorphtarget_p.h>
#include <QtQuick3DRuntimeRender/private/qssgrenderparticles_p.h>
#include <QtQuick3DRuntimeRender/private/qssgrendercontextcore_p.h>
#include <QtQuick3DRuntimeRender/private/qssgrenderbuffermanager_p.h>
#include <QtQuick3DRuntimeRender/private/qssgrendershadercache_p.h>
#include <QtQuick3DRuntimeRender/private/qssgperframeallocator_p.h>
#include <QtQuick3DRuntimeRender/private/qssgruntimerenderlogging_p.h>
#include <QtQuick3DRuntimeRender/private/qssglightmapper_p.h>

//#define QT_QUICK3D_MESH_LOD_DEBUG
//#define QT_QUICK3D_MESH_LOD_NORMALS_DEBUG

#if defined(QT_QUICK3D_MESH_LOD_DEBUG) || defined(QT_QUICK3D_MESH_LOD_NORMALS_DEBUG)
#include <QtQuick3DRuntimeRender/private/qssgdebugdrawsystem_p.h>
#endif

#include <QtQuick3DUtils/private/qssgutils_p.h>
#include <QtQuick3DUtils/private/qssgassert_p.h>

#include <QtQuick/private/qsgtexture_p.h>
#include <QtQuick/private/qsgrenderer_p.h>

#include <QtCore/QCoreApplication>
#include <QtCore/QBitArray>
#include <array>

#include "qssgrenderpass_p.h"

QT_BEGIN_NAMESPACE

Q_LOGGING_CATEGORY(lcQuick3DRender, "qt.quick3d.render");

#define POS4BONETRANS(x)    (sizeof(float) * 16 * (x) * 2)
#define POS4BONENORM(x)     (sizeof(float) * 16 * ((x) * 2 + 1))
#define BONEDATASIZE4ID(x)  POS4BONETRANS(x + 1)

// These are meant to be pixel offsets, so you need to divide them by the width/height
// of the layer respectively.
static const QVector2D s_ProgressiveAAVertexOffsets[QSSGLayerRenderData::MAX_AA_LEVELS] = {
    QVector2D(-0.170840f, -0.553840f), // 1x
    QVector2D(0.162960f, -0.319340f), // 2x
    QVector2D(0.360260f, -0.245840f), // 3x
    QVector2D(-0.561340f, -0.149540f), // 4x
    QVector2D(0.249460f, 0.453460f), // 5x
    QVector2D(-0.336340f, 0.378260f), // 6x
    QVector2D(0.340000f, 0.166260f), // 7x
    QVector2D(0.235760f, 0.527760f), // 8x
};

qsizetype QSSGLayerRenderData::frustumCulling(const QSSGClippingFrustum &clipFrustum, const QSSGRenderableObjectList &renderables, QSSGRenderableObjectList &visibleRenderables)
{
    QSSG_ASSERT(visibleRenderables.isEmpty(), visibleRenderables.clear());
    visibleRenderables.reserve(renderables.size());
    for (quint32 end = renderables.size(), idx = quint32(0); idx != end; ++idx) {
        auto handle = renderables.at(idx);
        const auto &b = handle.obj->globalBounds;
        if (clipFrustum.intersectsWith(b))
            visibleRenderables.push_back(handle);
    }

    return visibleRenderables.size();
}

qsizetype QSSGLayerRenderData::frustumCullingInline(const QSSGClippingFrustum &clipFrustum, QSSGRenderableObjectList &renderables)
{
    const qint32 end = renderables.size();
    qint32 front = 0;
    qint32 back = end - 1;

    while (front <= back) {
        const auto &b = renderables.at(front).obj->globalBounds;
        if (clipFrustum.intersectsWith(b))
            ++front;
        else
            renderables.swapItemsAt(front, back--);
    }

    return back + 1;
}

static void collectBoneTransforms(QSSGRenderNode *node, QSSGRenderModel *modelNode, const QVector<QMatrix4x4> &poses)
{
    if (node->type == QSSGRenderGraphObject::Type::Joint) {
        QSSGRenderJoint *jointNode = static_cast<QSSGRenderJoint *>(node);
        jointNode->calculateGlobalVariables();
        QMatrix4x4 globalTrans = jointNode->globalTransform;
        // if user doesn't give the inverseBindPose, identity matrices are used.
        if (poses.size() > jointNode->index)
            globalTrans *= poses[jointNode->index];
        memcpy(modelNode->boneData.data() + POS4BONETRANS(jointNode->index),
               reinterpret_cast<const void *>(globalTrans.constData()),
               sizeof(float) * 16);
        // only upper 3x3 is meaningful
        memcpy(modelNode->boneData.data() + POS4BONENORM(jointNode->index),
               reinterpret_cast<const void *>(QMatrix4x4(globalTrans.normalMatrix()).constData()),
               sizeof(float) * 11);
    } else {
        modelNode->skeletonContainsNonJointNodes = true;
    }
    for (auto &child : node->children)
        collectBoneTransforms(&child, modelNode, poses);
}

static bool hasDirtyNonJointNodes(QSSGRenderNode *node, bool &hasChildJoints)
{
    if (!node)
        return false;
    // we might be non-joint dirty node, but if we do not have child joints we need to return false
    // Note! The frontend clears TransformDirty. Use dirty instead.
    bool dirtyNonJoint = ((node->type != QSSGRenderGraphObject::Type::Joint)
                                              && node->isDirty());

    // Tell our parent we are joint
    if (node->type == QSSGRenderGraphObject::Type::Joint)
        hasChildJoints = true;
    bool nodeHasChildJoints = false;
    for (auto &child : node->children) {
        bool ret = hasDirtyNonJointNodes(&child, nodeHasChildJoints);
        // return if we have child joints and non-joint dirty nodes, else check other children
        hasChildJoints |= nodeHasChildJoints;
        if (ret && nodeHasChildJoints)
            return true;
    }
    // return true if we have child joints and we are dirty non-joint
    hasChildJoints |= nodeHasChildJoints;
    return dirtyNonJoint && nodeHasChildJoints;
}

template<typename T, typename V>
inline void collectNode(V node, QVector<T> &dst, int &dstPos)
{
    if (dstPos < dst.size())
        dst[dstPos] = node;
    else
        dst.push_back(node);

    ++dstPos;
}
template <typename T, typename V>
static inline void collectNodeFront(V node, QVector<T> &dst, int &dstPos)
{
    if (dstPos < dst.size())
        dst[dst.size() - dstPos - 1] = node;
    else
        dst.push_front(node);

    ++dstPos;
}

#define MAX_MORPH_TARGET 8
#define MAX_MORPH_TARGET_INDEX_SUPPORTS_NORMALS 3
#define MAX_MORPH_TARGET_INDEX_SUPPORTS_TANGENTS 1

static bool maybeQueueNodeForRender(QSSGRenderNode &inNode,
                                    QVector<QSSGRenderableNodeEntry> &outRenderableModels,
                                    int &ioRenderableModelsCount,
                                    QVector<QSSGRenderableNodeEntry> &outRenderableParticles,
                                    int &ioRenderableParticlesCount,
                                    QVector<QSSGRenderItem2D *> &outRenderableItem2Ds,
                                    int &ioRenderableItem2DsCount,
                                    QVector<QSSGRenderCamera *> &outCameras,
                                    int &ioCameraCount,
                                    QVector<QSSGRenderLight *> &outLights,
                                    int &ioLightCount,
                                    QVector<QSSGRenderReflectionProbe *> &outReflectionProbes,
                                    int &ioReflectionProbeCount,
                                    quint32 &ioDFSIndex)
{
    bool wasDirty = inNode.isDirty(QSSGRenderNode::DirtyFlag::GlobalValuesDirty) && inNode.calculateGlobalVariables();
    if (inNode.getGlobalState(QSSGRenderNode::GlobalState::Active)) {
        ++ioDFSIndex;
        inNode.dfsIndex = ioDFSIndex;
        if (QSSGRenderGraphObject::isRenderable(inNode.type)) {
            if (inNode.type == QSSGRenderNode::Type::Model)
                collectNode(QSSGRenderableNodeEntry(inNode), outRenderableModels, ioRenderableModelsCount);
            else if (inNode.type == QSSGRenderNode::Type::Particles)
                collectNode(QSSGRenderableNodeEntry(inNode), outRenderableParticles, ioRenderableParticlesCount);
            else if (inNode.type == QSSGRenderNode::Type::Item2D) // Pushing front to keep item order inside QML file
                collectNodeFront(static_cast<QSSGRenderItem2D *>(&inNode), outRenderableItem2Ds, ioRenderableItem2DsCount);
        } else if (QSSGRenderGraphObject::isCamera(inNode.type)) {
            collectNode(static_cast<QSSGRenderCamera *>(&inNode), outCameras, ioCameraCount);
        } else if (QSSGRenderGraphObject::isLight(inNode.type)) {
            if (auto &light = static_cast<QSSGRenderLight &>(inNode); light.isEnabled())
                collectNode(&light, outLights, ioLightCount);
        } else if (inNode.type == QSSGRenderGraphObject::Type::ReflectionProbe) {
            collectNode(static_cast<QSSGRenderReflectionProbe *>(&inNode), outReflectionProbes, ioReflectionProbeCount);
        }

        for (auto &theChild : inNode.children)
            wasDirty |= maybeQueueNodeForRender(theChild,
                                                outRenderableModels,
                                                ioRenderableModelsCount,
                                                outRenderableParticles,
                                                ioRenderableParticlesCount,
                                                outRenderableItem2Ds,
                                                ioRenderableItem2DsCount,
                                                outCameras,
                                                ioCameraCount,
                                                outLights,
                                                ioLightCount,
                                                outReflectionProbes,
                                                ioReflectionProbeCount,
                                                ioDFSIndex);
    }
    return wasDirty;
}

QSSGDefaultMaterialPreparationResult::QSSGDefaultMaterialPreparationResult(QSSGShaderDefaultMaterialKey inKey)
    : firstImage(nullptr), opacity(1.0f), materialKey(inKey), dirty(false)
{
}


QSSGCameraData QSSGLayerRenderData::getCameraDirectionAndPosition()
{
    if (!cameraData.has_value()) {
        if (camera)
            cameraData = QSSGCameraData{ camera->getScalingCorrectDirection(), camera->getGlobalPos() };
        else
            cameraData = QSSGCameraData{ QVector3D(0.0f, 0.0f, -1.0f), QVector3D() };
    }
    return *cameraData;
}

[[nodiscard]] static inline float getCameraDistanceSq(const QSSGRenderableObject &obj,
                                                      const QSSGCameraData &camera) noexcept
{
    const QVector3D difference = obj.worldCenterPoint - camera.position;
    return QVector3D::dotProduct(difference, camera.direction) + obj.depthBiasSq;
}

// Per-frame cache of renderable objects post-sort.
const QVector<QSSGRenderableObjectHandle> &QSSGLayerRenderData::getSortedOpaqueRenderableObjects()
{
    if (!renderedOpaqueObjects.empty() || camera == nullptr)
        return renderedOpaqueObjects;

    if (layer.layerFlags.testFlag(QSSGRenderLayer::LayerFlag::EnableDepthTest) && !opaqueObjects.empty()) {
        renderedOpaqueObjects = opaqueObjects;
        static const auto isRenderObjectPtrLessThan = [](const QSSGRenderableObjectHandle &lhs, const QSSGRenderableObjectHandle &rhs) {
            return lhs.cameraDistanceSq < rhs.cameraDistanceSq;
        };

        // Render nearest to furthest objects
        std::sort(renderedOpaqueObjects.begin(), renderedOpaqueObjects.end(), isRenderObjectPtrLessThan);
    }
    return renderedOpaqueObjects;
}

// If layer depth test is false, this may also contain opaque objects.
const QVector<QSSGRenderableObjectHandle> &QSSGLayerRenderData::getSortedTransparentRenderableObjects()
{
    if (!renderedTransparentObjects.empty() || camera == nullptr)
        return renderedTransparentObjects;

    renderedTransparentObjects = transparentObjects;

    if (!layer.layerFlags.testFlag(QSSGRenderLayer::LayerFlag::EnableDepthTest))
        renderedTransparentObjects.append(opaqueObjects);

    if (!renderedTransparentObjects.empty()) {
        static const auto iSRenderObjectPtrGreatThan = [](const QSSGRenderableObjectHandle &lhs, const QSSGRenderableObjectHandle &rhs) {
            return lhs.cameraDistanceSq > rhs.cameraDistanceSq;
        };
        // render furthest to nearest.
        std::sort(renderedTransparentObjects.begin(), renderedTransparentObjects.end(), iSRenderObjectPtrGreatThan);
    }

    return renderedTransparentObjects;
}

const QVector<QSSGRenderableObjectHandle> &QSSGLayerRenderData::getSortedScreenTextureRenderableObjects()
{
    if (!renderedScreenTextureObjects.empty() || camera == nullptr)
        return renderedScreenTextureObjects;
    renderedScreenTextureObjects = screenTextureObjects;
    if (!renderedScreenTextureObjects.empty()) {
        static const auto iSRenderObjectPtrGreatThan = [](const QSSGRenderableObjectHandle &lhs,
                                                          const QSSGRenderableObjectHandle &rhs) {
            return lhs.cameraDistanceSq > rhs.cameraDistanceSq;
        };
        // render furthest to nearest.
        std::sort(renderedScreenTextureObjects.begin(), renderedScreenTextureObjects.end(), iSRenderObjectPtrGreatThan);
    }
    return renderedScreenTextureObjects;
}

const QVector<QSSGBakedLightingModel> &QSSGLayerRenderData::getSortedBakedLightingModels()
{
    if (!renderedBakedLightingModels.empty() || camera == nullptr)
        return renderedBakedLightingModels;
    if (layer.layerFlags.testFlag(QSSGRenderLayer::LayerFlag::EnableDepthTest) && !bakedLightingModels.empty()) {
        renderedBakedLightingModels = bakedLightingModels;
        for (QSSGBakedLightingModel &lm : renderedBakedLightingModels) {
            static const auto isRenderObjectPtrLessThan = [](const QSSGRenderableObjectHandle &lhs, const QSSGRenderableObjectHandle &rhs) {
                return lhs.cameraDistanceSq < rhs.cameraDistanceSq;
            };
            // sort nearest to furthest (front to back)
            std::sort(lm.renderables.begin(), lm.renderables.end(), isRenderObjectPtrLessThan);
        }
    }
    return renderedBakedLightingModels;
}

const QSSGLayerRenderData::RenderableItem2DEntries &QSSGLayerRenderData::getRenderableItem2Ds()
{
    if (!renderedItem2Ds.isEmpty() || camera == nullptr)
        return renderedItem2Ds;

    renderedItem2Ds = renderableItem2Ds;

    if (!renderedItem2Ds.isEmpty()) {
        const auto cameraDirectionAndPosition = getCameraDirectionAndPosition();
        const QVector3D &cameraDirection = cameraDirectionAndPosition.direction;
        const QVector3D &cameraPosition = cameraDirectionAndPosition.position;

        const auto isItemNodeDistanceGreatThan = [cameraDirection, cameraPosition]
                (const QSSGRenderItem2D *lhs, const QSSGRenderItem2D *rhs) {
            if (!lhs->parent || !rhs->parent)
                return false;
            const QVector3D lhsDifference = lhs->parent->getGlobalPos() - cameraPosition;
            const float lhsCameraDistanceSq = QVector3D::dotProduct(lhsDifference, cameraDirection);
            const QVector3D rhsDifference = rhs->parent->getGlobalPos() - cameraPosition;
            const float rhsCameraDistanceSq = QVector3D::dotProduct(rhsDifference, cameraDirection);
            return lhsCameraDistanceSq > rhsCameraDistanceSq;
        };

        const auto isItemZOrderLessThan = []
                (const QSSGRenderItem2D *lhs, const QSSGRenderItem2D *rhs) {
            if (lhs->parent && rhs->parent && lhs->parent == rhs->parent) {
                // Same parent nodes, so sort with item z-ordering
                return lhs->zOrder < rhs->zOrder;
            }
            return false;
        };

        // Render furthest to nearest items (parent nodes).
        std::stable_sort(renderedItem2Ds.begin(), renderedItem2Ds.end(), isItemNodeDistanceGreatThan);
        // Render items inside same node by item z-order.
        // Note: stable_sort so item order in QML file is respected.
        std::stable_sort(renderedItem2Ds.begin(), renderedItem2Ds.end(), isItemZOrderLessThan);
    }

    return renderedItem2Ds;
}

// Depth Write List
void QSSGLayerRenderData::updateSortedDepthObjectsListImp()
{
    if (!renderedDepthWriteObjects.isEmpty() || !renderedOpaqueDepthPrepassObjects.isEmpty())
        return;

    const auto &sortedOpaqueObjects = getSortedOpaqueRenderableObjects(); // front to back
    const auto &sortedTransparentObjects = getSortedTransparentRenderableObjects(); // back to front
    const auto &sortedScreenTextureObjects = getSortedScreenTextureRenderableObjects(); // back to front
    if (layer.layerFlags.testFlag(QSSGRenderLayer::LayerFlag::EnableDepthTest)) {
        for (const auto &opaqueObject : sortedOpaqueObjects) {
            const auto depthMode = opaqueObject.obj->depthWriteMode;
            if (depthMode == QSSGDepthDrawMode::Always || depthMode == QSSGDepthDrawMode::OpaqueOnly)
                renderedDepthWriteObjects.append(opaqueObject);
            else if (depthMode == QSSGDepthDrawMode::OpaquePrePass)
                renderedOpaqueDepthPrepassObjects.append(opaqueObject);
        }
        for (const auto &transparentObject : sortedTransparentObjects) {
            const auto depthMode = transparentObject.obj->depthWriteMode;
            if (depthMode == QSSGDepthDrawMode::Always)
                renderedDepthWriteObjects.append(transparentObject);
            else if (depthMode == QSSGDepthDrawMode::OpaquePrePass)
                renderedOpaqueDepthPrepassObjects.append(transparentObject);
        }
        for (const auto &screenTextureObject : sortedScreenTextureObjects) {
            const auto depthMode = screenTextureObject.obj->depthWriteMode;
            if (depthMode == QSSGDepthDrawMode::Always || depthMode == QSSGDepthDrawMode::OpaqueOnly)
                renderedDepthWriteObjects.append(screenTextureObject);
            else if (depthMode == QSSGDepthDrawMode::OpaquePrePass)
                renderedOpaqueDepthPrepassObjects.append(screenTextureObject);
        }
    }
}

const QSSGRenderableObjectList &QSSGLayerRenderData::getSortedRenderedDepthWriteObjects()
{
    updateSortedDepthObjectsListImp();
    return renderedDepthWriteObjects;
}

const QSSGRenderableObjectList &QSSGLayerRenderData::getSortedrenderedOpaqueDepthPrepassObjects()
{
    updateSortedDepthObjectsListImp();
    return renderedOpaqueDepthPrepassObjects;
}

/**
 * Usage: T *ptr = RENDER_FRAME_NEW<T>(context, arg0, arg1, ...); is equivalent to: T *ptr = new T(arg0, arg1, ...);
 * so RENDER_FRAME_NEW() takes the RCI + T's arguments
 */
template <typename T, typename... Args>
Q_REQUIRED_RESULT inline T *RENDER_FRAME_NEW(QSSGRenderContextInterface &ctx, Args&&... args)
{
    static_assert(std::is_trivially_destructible_v<T>, "Objects allocated using the per-frame allocator needs to be trivially destructible!");
    return new (ctx.perFrameAllocator().allocate(sizeof(T)))T(std::forward<Args>(args)...);
}

template <typename T, typename... Args>
Q_REQUIRED_RESULT inline T *RENDER_FRAME_NEW(QSSGRenderContextInterface &ctx, size_t asize)
{
    static_assert(std::is_trivially_destructible_v<T>, "Objects allocated using the per-frame allocator needs to be trivially destructible!");
    return reinterpret_cast<T *>(ctx.perFrameAllocator().allocate(asize));
}

QSSGShaderDefaultMaterialKey QSSGLayerRenderData::generateLightingKey(
        QSSGRenderDefaultMaterial::MaterialLighting inLightingType, const QSSGShaderLightListView &lights, bool receivesShadows)
{
    QSSGShaderDefaultMaterialKey theGeneratedKey(qHash(features));
    const bool lighting = inLightingType != QSSGRenderDefaultMaterial::MaterialLighting::NoLighting;
    renderer->defaultMaterialShaderKeyProperties().m_hasLighting.setValue(theGeneratedKey, lighting);
    if (lighting) {
        renderer->defaultMaterialShaderKeyProperties().m_hasIbl.setValue(theGeneratedKey, layer.lightProbe != nullptr);

        quint32 numLights = quint32(lights.size());
        Q_ASSERT(numLights <= QSSGShaderDefaultMaterialKeyProperties::LightCount);
        renderer->defaultMaterialShaderKeyProperties().m_lightCount.setValue(theGeneratedKey, numLights);

        int shadowMapCount = 0;
        for (int lightIdx = 0, lightEnd = lights.size(); lightIdx < lightEnd; ++lightIdx) {
            QSSGRenderLight *theLight(lights[lightIdx].light);
            const bool isDirectional = theLight->type == QSSGRenderLight::Type::DirectionalLight;
            const bool isSpot = theLight->type == QSSGRenderLight::Type::SpotLight;
            const bool castsShadows = theLight->m_castShadow
                    && !theLight->m_fullyBaked
                    && receivesShadows
                    && shadowMapCount < QSSG_MAX_NUM_SHADOW_MAPS;
            if (castsShadows)
                ++shadowMapCount;

            renderer->defaultMaterialShaderKeyProperties().m_lightFlags[lightIdx].setValue(theGeneratedKey, !isDirectional);
            renderer->defaultMaterialShaderKeyProperties().m_lightSpotFlags[lightIdx].setValue(theGeneratedKey, isSpot);
            renderer->defaultMaterialShaderKeyProperties().m_lightShadowFlags[lightIdx].setValue(theGeneratedKey, castsShadows);
        }
    }
    return theGeneratedKey;
}

void QSSGLayerRenderData::prepareImageForRender(QSSGRenderImage &inImage,
                                                           QSSGRenderableImage::Type inMapType,
                                                           QSSGRenderableImage *&ioFirstImage,
                                                           QSSGRenderableImage *&ioNextImage,
                                                           QSSGRenderableObjectFlags &ioFlags,
                                                           QSSGShaderDefaultMaterialKey &inShaderKey,
                                                           quint32 inImageIndex,
                                                           QSSGRenderDefaultMaterial *inMaterial)
{
    QSSGRenderContextInterface &contextInterface = *renderer->contextInterface();
    const QSSGRef<QSSGBufferManager> &bufferManager = contextInterface.bufferManager();

    if (inImage.clearDirty())
        ioFlags |= QSSGRenderableObjectFlag::Dirty;

    // This is where the QRhiTexture gets created, if not already done. Note
    // that the bufferManager is per-QQuickWindow, and so per-render-thread.
    // Hence using the same Texture (backed by inImage as the backend node) in
    // multiple windows will work by each scene in each window getting its own
    // QRhiTexture. And that's why the QSSGRenderImageTexture cannot be a
    // member of the QSSGRenderImage. Conceptually this matches what we do for
    // models (QSSGRenderModel -> QSSGRenderMesh retrieved from the
    // bufferManager in each prepareModelForRender, etc.).

    const QSSGRenderImageTexture texture = bufferManager->loadRenderImage(&inImage);

    if (texture.m_texture) {
        if (texture.m_flags.hasTransparency()
            && (inMapType == QSSGRenderableImage::Type::Diffuse // note: Type::BaseColor is skipped here intentionally
                || inMapType == QSSGRenderableImage::Type::Opacity
                || inMapType == QSSGRenderableImage::Type::Translucency))
        {
            ioFlags |= QSSGRenderableObjectFlag::HasTransparency;
        }

        QSSGRenderableImage *theImage = RENDER_FRAME_NEW<QSSGRenderableImage>(contextInterface, inMapType, inImage, texture);
        QSSGShaderKeyImageMap &theKeyProp = renderer->defaultMaterialShaderKeyProperties().m_imageMaps[inImageIndex];

        theKeyProp.setEnabled(inShaderKey, true);
        switch (inImage.m_mappingMode) {
        case QSSGRenderImage::MappingModes::Normal:
            break;
        case QSSGRenderImage::MappingModes::Environment:
            theKeyProp.setEnvMap(inShaderKey, true);
            break;
        case QSSGRenderImage::MappingModes::LightProbe:
            theKeyProp.setLightProbe(inShaderKey, true);
            break;
        }

        bool hasA = false;
        bool hasG = false;
        bool hasB = false;


        //### TODO: More formats
        switch (texture.m_texture->format()) {
        case QRhiTexture::Format::RED_OR_ALPHA8:
            hasA = !renderer->contextInterface()->rhiContext()->rhi()->isFeatureSupported(QRhi::RedOrAlpha8IsRed);
            break;
        case QRhiTexture::Format::R8:
            // Leave BGA as false
            break;
        default:
            hasA = true;
            hasG = true;
            hasB = true;
            break;
        }

        if (inImage.isImageTransformIdentity())
            theKeyProp.setIdentityTransform(inShaderKey, true);

        if (inImage.m_indexUV == 1)
            theKeyProp.setUsesUV1(inShaderKey, true);

        if (ioFirstImage == nullptr)
            ioFirstImage = theImage;
        else
            ioNextImage->m_nextImage = theImage;

        ioNextImage = theImage;

        if (inMaterial && inImageIndex >= QSSGShaderDefaultMaterialKeyProperties::SingleChannelImagesFirst) {
            QSSGRenderDefaultMaterial::TextureChannelMapping value = QSSGRenderDefaultMaterial::R;

            const quint32 scIndex = inImageIndex - QSSGShaderDefaultMaterialKeyProperties::SingleChannelImagesFirst;
            QSSGShaderKeyTextureChannel &channelKey = renderer->defaultMaterialShaderKeyProperties().m_textureChannels[scIndex];
            switch (inImageIndex) {
            case QSSGShaderDefaultMaterialKeyProperties::OpacityMap:
                value = inMaterial->opacityChannel;
                break;
            case QSSGShaderDefaultMaterialKeyProperties::RoughnessMap:
                value = inMaterial->roughnessChannel;
                break;
            case QSSGShaderDefaultMaterialKeyProperties::MetalnessMap:
                value = inMaterial->metalnessChannel;
                break;
            case QSSGShaderDefaultMaterialKeyProperties::OcclusionMap:
                value = inMaterial->occlusionChannel;
                break;
            case QSSGShaderDefaultMaterialKeyProperties::TranslucencyMap:
                value = inMaterial->translucencyChannel;
                break;
            case QSSGShaderDefaultMaterialKeyProperties::HeightMap:
                value = inMaterial->heightChannel;
                break;
            case QSSGShaderDefaultMaterialKeyProperties::ClearcoatMap:
                value = inMaterial->clearcoatChannel;
                break;
            case QSSGShaderDefaultMaterialKeyProperties::ClearcoatRoughnessMap:
                value = inMaterial->clearcoatRoughnessChannel;
                break;
            case QSSGShaderDefaultMaterialKeyProperties::TransmissionMap:
                value = inMaterial->transmissionChannel;
                break;
            case QSSGShaderDefaultMaterialKeyProperties::ThicknessMap:
                value = inMaterial->thicknessChannel;
                break;
            default:
                break;
            }
            bool useDefault = false;
            switch (value) {
            case QSSGRenderDefaultMaterial::TextureChannelMapping::G:
                useDefault = !hasG;
                break;
            case QSSGRenderDefaultMaterial::TextureChannelMapping::B:
                useDefault = !hasB;
                break;
            case QSSGRenderDefaultMaterial::TextureChannelMapping::A:
                useDefault = !hasA;
                break;
            default:
                break;
            }
            if (useDefault)
                value = QSSGRenderDefaultMaterial::R; // Always Fallback to Red
            channelKey.setTextureChannel(QSSGShaderKeyTextureChannel::TexturChannelBits(value), inShaderKey);
        }
    }
}

void QSSGLayerRenderData::setVertexInputPresence(const QSSGRenderableObjectFlags &renderableFlags,
                                                            QSSGShaderDefaultMaterialKey &key,
                                                            QSSGRenderer *renderer)
{
    quint32 vertexAttribs = 0;
    if (renderableFlags.hasAttributePosition())
        vertexAttribs |= QSSGShaderKeyVertexAttribute::Position;
    if (renderableFlags.hasAttributeNormal())
        vertexAttribs |= QSSGShaderKeyVertexAttribute::Normal;
    if (renderableFlags.hasAttributeTexCoord0())
        vertexAttribs |= QSSGShaderKeyVertexAttribute::TexCoord0;
    if (renderableFlags.hasAttributeTexCoord1())
        vertexAttribs |= QSSGShaderKeyVertexAttribute::TexCoord1;
    if (renderableFlags.hasAttributeTexCoordLightmap())
        vertexAttribs |= QSSGShaderKeyVertexAttribute::TexCoordLightmap;
    if (renderableFlags.hasAttributeTangent())
        vertexAttribs |= QSSGShaderKeyVertexAttribute::Tangent;
    if (renderableFlags.hasAttributeBinormal())
        vertexAttribs |= QSSGShaderKeyVertexAttribute::Binormal;
    if (renderableFlags.hasAttributeColor())
        vertexAttribs |= QSSGShaderKeyVertexAttribute::Color;
    if (renderableFlags.hasAttributeJointAndWeight())
        vertexAttribs |= QSSGShaderKeyVertexAttribute::JointAndWeight;
    renderer->defaultMaterialShaderKeyProperties().m_vertexAttributes.setValue(key, vertexAttribs);
}

QSSGDefaultMaterialPreparationResult QSSGLayerRenderData::prepareDefaultMaterialForRender(
        QSSGRenderDefaultMaterial &inMaterial,
        QSSGRenderableObjectFlags &inExistingFlags,
        float inOpacity,
        const QSSGShaderLightListView &lights,
        QSSGLayerRenderPreparationResultFlags &ioFlags)
{
    QSSGRenderDefaultMaterial *theMaterial = &inMaterial;
    QSSGDefaultMaterialPreparationResult retval(generateLightingKey(theMaterial->lighting, lights, inExistingFlags.receivesShadows()));
    retval.renderableFlags = inExistingFlags;
    QSSGRenderableObjectFlags &renderableFlags(retval.renderableFlags);
    QSSGShaderDefaultMaterialKey &theGeneratedKey(retval.materialKey);
    retval.opacity = inOpacity;
    float &subsetOpacity(retval.opacity);

    if (theMaterial->isDirty())
        renderableFlags |= QSSGRenderableObjectFlag::Dirty;

    subsetOpacity *= theMaterial->opacity;

    QSSGRenderableImage *firstImage = nullptr;

    renderer->defaultMaterialShaderKeyProperties().m_specularAAEnabled.setValue(theGeneratedKey, layer.specularAAEnabled);

    // isDoubleSided
    renderer->defaultMaterialShaderKeyProperties().m_isDoubleSided.setValue(theGeneratedKey, theMaterial->cullMode == QSSGCullFaceMode::Disabled);

    // default materials never define their on position
    renderer->defaultMaterialShaderKeyProperties().m_overridesPosition.setValue(theGeneratedKey, false);

    // default materials dont make use of raw projection or inverse projection matrices
    renderer->defaultMaterialShaderKeyProperties().m_usesProjectionMatrix.setValue(theGeneratedKey, false);
    renderer->defaultMaterialShaderKeyProperties().m_usesInverseProjectionMatrix.setValue(theGeneratedKey, false);
    // nor they do rely on VAR_COLOR
    renderer->defaultMaterialShaderKeyProperties().m_usesVarColor.setValue(theGeneratedKey, false);

    // alpha Mode
    renderer->defaultMaterialShaderKeyProperties().m_alphaMode.setValue(theGeneratedKey, theMaterial->alphaMode);

    // vertex attribute presence flags
    setVertexInputPresence(renderableFlags, theGeneratedKey, renderer.data());

    // set the flag indicating the need for gl_PointSize
    renderer->defaultMaterialShaderKeyProperties().m_usesPointsTopology.setValue(theGeneratedKey, renderableFlags.isPointsTopology());

    // propagate the flag indicating the presence of a lightmap
    renderer->defaultMaterialShaderKeyProperties().m_lightmapEnabled.setValue(theGeneratedKey, renderableFlags.rendersWithLightmap());

    renderer->defaultMaterialShaderKeyProperties().m_specularGlossyEnabled.setValue(theGeneratedKey, theMaterial->type == QSSGRenderGraphObject::Type::SpecularGlossyMaterial);

    // debug modes
    renderer->defaultMaterialShaderKeyProperties().m_debugMode.setValue(theGeneratedKey, int(layer.debugMode));

    // fog
    renderer->defaultMaterialShaderKeyProperties().m_fogEnabled.setValue(theGeneratedKey, layer.fog.enabled);

    if (!renderer->defaultMaterialShaderKeyProperties().m_hasIbl.getValue(theGeneratedKey) && theMaterial->iblProbe) {
        features.set(QSSGShaderFeatures::Feature::LightProbe, true);
        renderer->defaultMaterialShaderKeyProperties().m_hasIbl.setValue(theGeneratedKey, true);
        // features.set(ShaderFeatureDefines::enableIblFov(),
        // m_Renderer.GetLayerRenderData()->m_Layer.m_ProbeFov < 180.0f );
    }

    if (subsetOpacity >= QSSG_RENDER_MINIMUM_RENDER_OPACITY) {

        // Set the semi-transparency flag as specified in PrincipledMaterial's
        // blendMode and alphaMode:
        // - the default SourceOver blendMode does not imply alpha blending on
        //   its own,
        // - but other blendMode values do,
        // - an alphaMode of Blend guarantees blending to be enabled regardless
        //   of anything else.
        // Additionally:
        // - Opacity and texture map alpha are handled elsewhere (that's when a
        //   blendMode of SourceOver or an alphaMode of Default/Opaque can in the
        //   end still result in HasTransparency),
        // - the presence of an opacityMap guarantees alpha blending regardless
        //   of its content.

        if (theMaterial->blendMode != QSSGRenderDefaultMaterial::MaterialBlendMode::SourceOver
                || theMaterial->opacityMap
                || theMaterial->alphaMode == QSSGRenderDefaultMaterial::Blend)
        {
            renderableFlags |= QSSGRenderableObjectFlag::HasTransparency;
        }

        const bool specularEnabled = theMaterial->isSpecularEnabled();
        const bool metalnessEnabled = theMaterial->isMetalnessEnabled();
        renderer->defaultMaterialShaderKeyProperties().m_specularEnabled.setValue(theGeneratedKey, (specularEnabled || metalnessEnabled));
        if (specularEnabled || metalnessEnabled)
            renderer->defaultMaterialShaderKeyProperties().m_specularModel.setSpecularModel(theGeneratedKey, theMaterial->specularModel);

        renderer->defaultMaterialShaderKeyProperties().m_fresnelEnabled.setValue(theGeneratedKey, theMaterial->isFresnelEnabled());

        renderer->defaultMaterialShaderKeyProperties().m_vertexColorsEnabled.setValue(theGeneratedKey,
                                                                                      theMaterial->isVertexColorsEnabled());
        renderer->defaultMaterialShaderKeyProperties().m_clearcoatEnabled.setValue(theGeneratedKey,
                                                                                   theMaterial->isClearcoatEnabled());
        renderer->defaultMaterialShaderKeyProperties().m_transmissionEnabled.setValue(theGeneratedKey,
                                                                                      theMaterial->isTransmissionEnabled());

        // Run through the material's images and prepare them for render.
        // this may in fact set pickable on the renderable flags if one of the images
        // links to a sub presentation or any offscreen rendered object.
        QSSGRenderableImage *nextImage = nullptr;
#define CHECK_IMAGE_AND_PREPARE(img, imgtype, shadercomponent)                          \
    if ((img))                                                                          \
        prepareImageForRender(*(img), imgtype, firstImage, nextImage, renderableFlags,  \
                              theGeneratedKey, shadercomponent, &inMaterial)

        if (theMaterial->type == QSSGRenderGraphObject::Type::PrincipledMaterial ||
            theMaterial->type == QSSGRenderGraphObject::Type::SpecularGlossyMaterial) {
            CHECK_IMAGE_AND_PREPARE(theMaterial->colorMap,
                                    QSSGRenderableImage::Type::BaseColor,
                                    QSSGShaderDefaultMaterialKeyProperties::BaseColorMap);
            CHECK_IMAGE_AND_PREPARE(theMaterial->occlusionMap,
                                    QSSGRenderableImage::Type::Occlusion,
                                    QSSGShaderDefaultMaterialKeyProperties::OcclusionMap);
            CHECK_IMAGE_AND_PREPARE(theMaterial->heightMap,
                                    QSSGRenderableImage::Type::Height,
                                    QSSGShaderDefaultMaterialKeyProperties::HeightMap);
            CHECK_IMAGE_AND_PREPARE(theMaterial->clearcoatMap,
                                    QSSGRenderableImage::Type::Clearcoat,
                                    QSSGShaderDefaultMaterialKeyProperties::ClearcoatMap);
            CHECK_IMAGE_AND_PREPARE(theMaterial->clearcoatRoughnessMap,
                                    QSSGRenderableImage::Type::ClearcoatRoughness,
                                    QSSGShaderDefaultMaterialKeyProperties::ClearcoatRoughnessMap);
            CHECK_IMAGE_AND_PREPARE(theMaterial->clearcoatNormalMap,
                                    QSSGRenderableImage::Type::ClearcoatNormal,
                                    QSSGShaderDefaultMaterialKeyProperties::ClearcoatNormalMap);
            CHECK_IMAGE_AND_PREPARE(theMaterial->transmissionMap,
                                    QSSGRenderableImage::Type::Transmission,
                                    QSSGShaderDefaultMaterialKeyProperties::TransmissionMap);
            CHECK_IMAGE_AND_PREPARE(theMaterial->thicknessMap,
                                    QSSGRenderableImage::Type::Thickness,
                                    QSSGShaderDefaultMaterialKeyProperties::ThicknessMap);
            if (theMaterial->type == QSSGRenderGraphObject::Type::PrincipledMaterial) {
                CHECK_IMAGE_AND_PREPARE(theMaterial->metalnessMap,
                                        QSSGRenderableImage::Type::Metalness,
                                        QSSGShaderDefaultMaterialKeyProperties::MetalnessMap);
            }
        } else {
            CHECK_IMAGE_AND_PREPARE(theMaterial->colorMap,
                                    QSSGRenderableImage::Type::Diffuse,
                                    QSSGShaderDefaultMaterialKeyProperties::DiffuseMap);
        }
        CHECK_IMAGE_AND_PREPARE(theMaterial->emissiveMap, QSSGRenderableImage::Type::Emissive, QSSGShaderDefaultMaterialKeyProperties::EmissiveMap);
        CHECK_IMAGE_AND_PREPARE(theMaterial->specularReflection,
                                QSSGRenderableImage::Type::Specular,
                                QSSGShaderDefaultMaterialKeyProperties::SpecularMap);
        CHECK_IMAGE_AND_PREPARE(theMaterial->roughnessMap,
                                QSSGRenderableImage::Type::Roughness,
                                QSSGShaderDefaultMaterialKeyProperties::RoughnessMap);
        CHECK_IMAGE_AND_PREPARE(theMaterial->opacityMap, QSSGRenderableImage::Type::Opacity, QSSGShaderDefaultMaterialKeyProperties::OpacityMap);
        CHECK_IMAGE_AND_PREPARE(theMaterial->bumpMap, QSSGRenderableImage::Type::Bump, QSSGShaderDefaultMaterialKeyProperties::BumpMap);
        CHECK_IMAGE_AND_PREPARE(theMaterial->specularMap,
                                QSSGRenderableImage::Type::SpecularAmountMap,
                                QSSGShaderDefaultMaterialKeyProperties::SpecularAmountMap);
        CHECK_IMAGE_AND_PREPARE(theMaterial->normalMap, QSSGRenderableImage::Type::Normal, QSSGShaderDefaultMaterialKeyProperties::NormalMap);
        CHECK_IMAGE_AND_PREPARE(theMaterial->translucencyMap,
                                QSSGRenderableImage::Type::Translucency,
                                QSSGShaderDefaultMaterialKeyProperties::TranslucencyMap);
    }
#undef CHECK_IMAGE_AND_PREPARE

    if (subsetOpacity < QSSG_RENDER_MINIMUM_RENDER_OPACITY) {
        subsetOpacity = 0.0f;
        // You can still pick against completely transparent objects(or rather their bounding
        // box)
        // you just don't render them.
        renderableFlags |= QSSGRenderableObjectFlag::HasTransparency;
        renderableFlags |= QSSGRenderableObjectFlag::CompletelyTransparent;
    }

    if (subsetOpacity > 1.f - QSSG_RENDER_MINIMUM_RENDER_OPACITY)
        subsetOpacity = 1.f;
    else
        renderableFlags |= QSSGRenderableObjectFlag::HasTransparency;

    if (inMaterial.isTransmissionEnabled()) {
        ioFlags.setRequiresScreenTexture(true);
        ioFlags.setRequiresMipmapsForScreenTexture(true);
        renderableFlags |= QSSGRenderableObjectFlag::RequiresScreenTexture;
    }

    retval.firstImage = firstImage;
    if (retval.renderableFlags.isDirty())
        retval.dirty = true;
    if (retval.dirty)
        renderer->addMaterialDirtyClear(&inMaterial);
    return retval;
}

QSSGDefaultMaterialPreparationResult QSSGLayerRenderData::prepareCustomMaterialForRender(
        QSSGRenderCustomMaterial &inMaterial, QSSGRenderableObjectFlags &inExistingFlags,
        float inOpacity, bool alreadyDirty, const QSSGShaderLightListView &lights,
        QSSGLayerRenderPreparationResultFlags &ioFlags)
{
    QSSGDefaultMaterialPreparationResult retval(
                generateLightingKey(QSSGRenderDefaultMaterial::MaterialLighting::FragmentLighting,
                                    lights, inExistingFlags.receivesShadows()));
    retval.renderableFlags = inExistingFlags;
    QSSGRenderableObjectFlags &renderableFlags(retval.renderableFlags);
    QSSGShaderDefaultMaterialKey &theGeneratedKey(retval.materialKey);
    retval.opacity = inOpacity;
    float &subsetOpacity(retval.opacity);

    if (subsetOpacity < QSSG_RENDER_MINIMUM_RENDER_OPACITY) {
        subsetOpacity = 0.0f;
        // You can still pick against completely transparent objects(or rather their bounding
        // box)
        // you just don't render them.
        renderableFlags |= QSSGRenderableObjectFlag::HasTransparency;
        renderableFlags |= QSSGRenderableObjectFlag::CompletelyTransparent;
    }

    if (subsetOpacity > 1.f - QSSG_RENDER_MINIMUM_RENDER_OPACITY)
        subsetOpacity = 1.f;
    else
        renderableFlags |= QSSGRenderableObjectFlag::HasTransparency;

    renderer->defaultMaterialShaderKeyProperties().m_specularAAEnabled.setValue(theGeneratedKey, layer.specularAAEnabled);

    // isDoubleSided
    renderer->defaultMaterialShaderKeyProperties().m_isDoubleSided.setValue(theGeneratedKey,
                                                                            inMaterial.m_cullMode == QSSGCullFaceMode::Disabled);

    // Does the material override the position output
    const bool overridesPosition = inMaterial.m_renderFlags.testFlag(QSSGRenderCustomMaterial::RenderFlag::OverridesPosition);
    renderer->defaultMaterialShaderKeyProperties().m_overridesPosition.setValue(theGeneratedKey, overridesPosition);

    // Optional usage of PROJECTION_MATRIX and/or INVERSE_PROJECTION_MATRIX
    const bool usesProjectionMatrix = inMaterial.m_renderFlags.testFlag(QSSGRenderCustomMaterial::RenderFlag::ProjectionMatrix);
    renderer->defaultMaterialShaderKeyProperties().m_usesProjectionMatrix.setValue(theGeneratedKey, usesProjectionMatrix);
    const bool usesInvProjectionMatrix = inMaterial.m_renderFlags.testFlag(QSSGRenderCustomMaterial::RenderFlag::InverseProjectionMatrix);
    renderer->defaultMaterialShaderKeyProperties().m_usesInverseProjectionMatrix.setValue(theGeneratedKey, usesInvProjectionMatrix);

    // vertex attribute presence flags
    setVertexInputPresence(renderableFlags, theGeneratedKey, renderer.data());

    // set the flag indicating the need for gl_PointSize
    renderer->defaultMaterialShaderKeyProperties().m_usesPointsTopology.setValue(theGeneratedKey, renderableFlags.isPointsTopology());

    // propagate the flag indicating the presence of a lightmap
    renderer->defaultMaterialShaderKeyProperties().m_lightmapEnabled.setValue(theGeneratedKey, renderableFlags.rendersWithLightmap());

    // debug modes
    renderer->defaultMaterialShaderKeyProperties().m_debugMode.setValue(theGeneratedKey, int(layer.debugMode));

    // fog
    renderer->defaultMaterialShaderKeyProperties().m_fogEnabled.setValue(theGeneratedKey, layer.fog.enabled);

    // Knowing whether VAR_COLOR is used becomes relevant when there is no
    // custom vertex shader, but VAR_COLOR is present in the custom fragment
    // snippet, because that case needs special care.
    const bool usesVarColor = inMaterial.m_renderFlags.testFlag(QSSGRenderCustomMaterial::RenderFlag::VarColor);
    renderer->defaultMaterialShaderKeyProperties().m_usesVarColor.setValue(theGeneratedKey, usesVarColor);

    if (inMaterial.m_renderFlags.testFlag(QSSGRenderCustomMaterial::RenderFlag::Blending))
        renderableFlags |= QSSGRenderableObjectFlag::HasTransparency;

    if (inMaterial.m_renderFlags.testFlag(QSSGRenderCustomMaterial::RenderFlag::ScreenTexture)) {
        ioFlags.setRequiresScreenTexture(true);
        renderableFlags |= QSSGRenderableObjectFlag::RequiresScreenTexture;
    }

    if (inMaterial.m_renderFlags.testFlag(QSSGRenderCustomMaterial::RenderFlag::ScreenMipTexture)) {
        ioFlags.setRequiresScreenTexture(true);
        ioFlags.setRequiresMipmapsForScreenTexture(true);
        renderableFlags |= QSSGRenderableObjectFlag::RequiresScreenTexture;
    }

    if (inMaterial.m_renderFlags.testFlag(QSSGRenderCustomMaterial::RenderFlag::DepthTexture))
        ioFlags.setRequiresDepthTexture(true);

    if (inMaterial.m_renderFlags.testFlag(QSSGRenderCustomMaterial::RenderFlag::AoTexture)) {
        ioFlags.setRequiresDepthTexture(true);
        ioFlags.setRequiresSsaoPass(true);
    }

    retval.firstImage = nullptr;

    if (retval.dirty || alreadyDirty)
        renderer->addMaterialDirtyClear(&inMaterial);
    return retval;
}

// inModel is const to emphasize the fact that its members cannot be written
// here: in case there is a scene shared between multiple View3Ds in different
// QQuickWindows, each window may run this in their own render thread, while
// inModel is the same.
bool QSSGLayerRenderData::prepareModelForRender(const RenderableNodeEntries &renderableModels,
                                                const QMatrix4x4 &inViewProjection,
                                                QSSGLayerRenderPreparationResultFlags &ioFlags,
                                                const QSSGCameraData &cameraData,
                                                float lodThreshold)
{
    const auto &rhiCtx = renderer->contextInterface()->rhiContext();
    QSSGRenderContextInterface &contextInterface = *renderer->contextInterface();
    const QSSGRef<QSSGBufferManager> &bufferManager = contextInterface.bufferManager();

    bool wasDirty = false;

    bool blendParticlesEnabled = true;
    const bool supportRgba32f = contextInterface.rhiContext()->rhi()->isTextureFormatSupported(QRhiTexture::RGBA32F);
    const bool supportRgba16f = contextInterface.rhiContext()->rhi()->isTextureFormatSupported(QRhiTexture::RGBA16F);
    if (!supportRgba32f && !supportRgba16f) {
        if (!particlesNotSupportedWarningShown)
            qWarning () << "Particles not supported due to missing RGBA32F and RGBA16F texture format support";
        particlesNotSupportedWarningShown = true;
        blendParticlesEnabled = false;
    }

    { // 1. Load meshes as needed
        for (const QSSGRenderableNodeEntry &renderable : renderableModels) {
            // It's up to the BufferManager to employ the appropriate caching mechanisms, so
            // loadMesh() is expected to be fast if already loaded. Note that preparing
            // the same QSSGRenderModel in different QQuickWindows (possible when a
            // scene is shared between View3Ds where the View3Ds belong to different
            // windows) leads to a different QSSGRenderMesh since the BufferManager is,
            // very correctly, per window, and so per scenegraph render thread.

            const QSSGRenderModel &model = *static_cast<QSSGRenderModel *>(renderable.node);
            renderable.mesh = bufferManager->loadMesh(&model);
            if (auto theMesh = renderable.mesh) {
                // Completely transparent models cannot be pickable.  But models with completely
                // transparent materials still are.  This allows the artist to control pickability
                // in a somewhat fine-grained style.
                const bool canModelBePickable = (model.globalOpacity > QSSG_RENDER_MINIMUM_RENDER_OPACITY)
                        && (renderer->isGlobalPickingEnabled()
                            || model.getGlobalState(QSSGRenderModel::GlobalState::Pickable));
                if (canModelBePickable) {
                    // Check if there is BVH data, if not generate it
                    if (!theMesh->bvh) {
                        if (!model.meshPath.isNull())
                            theMesh->bvh = bufferManager->loadMeshBVH(model.meshPath);
                        else if (model.geometry)
                            theMesh->bvh = bufferManager->loadMeshBVH(model.geometry);

                        if (theMesh->bvh) {
                            for (int i = 0; i < theMesh->bvh->roots.size(); ++i)
                                theMesh->subsets[i].bvhRoot = theMesh->bvh->roots.at(i);
                        }
                    }
                }
            }
        }

        // Now is the time to kick off the vertex/index buffer updates for all the
        // new meshes (and their submeshes). This here is the last possible place
        // to kick this off because the rest of the rendering pipeline will only
        // see the individual sub-objects as "renderable objects".
        bufferManager->commitBufferResourceUpdates();
    }

    { // 2. Ensure texture for the bone texture
        for (const QSSGRenderableNodeEntry &renderable : renderableModels) {
            const QSSGRenderModel &model = *static_cast<QSSGRenderModel *>(renderable.node);
            // Prepare boneTexture for skinning
            // NOTE: In the future the boneTexture should not be stored in the render model but in the model context.
            if (!model.boneData.isEmpty()) {
                const int boneTexWidth = qCeil(qSqrt(model.boneCount * 4 * 2));
                const QSize texSize(boneTexWidth, boneTexWidth);
                if (!model.boneTexture) {
                    model.boneTexture = rhiCtx->rhi()->newTexture(QRhiTexture::RGBA32F, texSize);
                    model.boneTexture->setName(QByteArrayLiteral("Bone texture"));
                    model.boneTexture->create();
                    rhiCtx->registerTexture(model.boneTexture);
                } else if (model.boneTexture->pixelSize() != texSize) {
                    model.boneTexture->setPixelSize(texSize);
                    model.boneTexture->create();
                }
                // Make sure boneData is the same size as the destination texture
                const int textureSizeInBytes = boneTexWidth * boneTexWidth * 16; //NB: Assumes RGBA32F set above (16 bytes per color)
                if (textureSizeInBytes != model.boneData.size())
                    const_cast<QSSGRenderModel &>(model).boneData.resize(textureSizeInBytes);
            } else if (model.boneTexture) {
                // This model had a skin but it was removed
                rhiCtx->releaseTexture(model.boneTexture);
                model.boneTexture = nullptr;
            }
        }
    }

    for (const QSSGRenderableNodeEntry &renderable : renderableModels) {
        const QSSGRenderModel &model = *static_cast<QSSGRenderModel *>(renderable.node);
        const auto &lights = renderable.lights;
        QSSGRenderMesh *theMesh = renderable.mesh;

        if (theMesh == nullptr)
            continue;

        QSSGModelContext &theModelContext = *RENDER_FRAME_NEW<QSSGModelContext>(contextInterface, model, inViewProjection);
        modelContexts.push_back(&theModelContext);

        // many renderableFlags are the same for all the subsets
        QSSGRenderableObjectFlags renderableFlagsForModel;

        if (theMesh->subsets.size() > 0) {
            QSSGRenderSubset &theSubset = theMesh->subsets[0];

            renderableFlagsForModel.setCastsShadows(model.castsShadows);
            renderableFlagsForModel.setReceivesShadows(model.receivesShadows);
            renderableFlagsForModel.setReceivesReflections(model.receivesReflections);
            renderableFlagsForModel.setCastsReflections(model.castsReflections);

            renderableFlagsForModel.setUsedInBakedLighting(model.usedInBakedLighting);
            if (model.hasLightmap()) {
                QSSGRenderImageTexture lmImageTexture = bufferManager->loadLightmap(model);
                if (lmImageTexture.m_texture) {
                    renderableFlagsForModel.setRendersWithLightmap(true);
                    theModelContext.lightmapTexture = lmImageTexture.m_texture;
                }
            }

            // TODO: This should be a oneshot thing, move the flags over!
            // With the RHI we need to be able to tell the material shader
            // generator to not generate vertex input attributes that are not
            // provided by the mesh. (because unlike OpenGL, other graphics
            // APIs may treat unbound vertex inputs as a fatal error)
            bool hasJoint = false;
            bool hasWeight = false;
            bool hasMorphTarget = theSubset.rhi.targetsTexture != nullptr;
            for (const QSSGRhiInputAssemblerState::InputSemantic &sem : std::as_const(theSubset.rhi.ia.inputs)) {
                if (sem == QSSGRhiInputAssemblerState::PositionSemantic) {
                    renderableFlagsForModel.setHasAttributePosition(true);
                } else if (sem == QSSGRhiInputAssemblerState::NormalSemantic) {
                    renderableFlagsForModel.setHasAttributeNormal(true);
                } else if (sem == QSSGRhiInputAssemblerState::TexCoord0Semantic) {
                    renderableFlagsForModel.setHasAttributeTexCoord0(true);
                } else if (sem == QSSGRhiInputAssemblerState::TexCoord1Semantic) {
                    renderableFlagsForModel.setHasAttributeTexCoord1(true);
                } else if (sem == QSSGRhiInputAssemblerState::TexCoordLightmapSemantic) {
                    renderableFlagsForModel.setHasAttributeTexCoordLightmap(true);
                } else if (sem == QSSGRhiInputAssemblerState::TangentSemantic) {
                    renderableFlagsForModel.setHasAttributeTangent(true);
                } else if (sem == QSSGRhiInputAssemblerState::BinormalSemantic) {
                    renderableFlagsForModel.setHasAttributeBinormal(true);
                } else if (sem == QSSGRhiInputAssemblerState::ColorSemantic) {
                    renderableFlagsForModel.setHasAttributeColor(true);
                    // For skinning, we will set the HasAttribute only
                    // if the mesh has both joint and weight
                } else if (sem == QSSGRhiInputAssemblerState::JointSemantic) {
                    hasJoint = true;
                } else if (sem == QSSGRhiInputAssemblerState::WeightSemantic) {
                    hasWeight = true;
                }
            }
            renderableFlagsForModel.setHasAttributeJointAndWeight(hasJoint && hasWeight);
            renderableFlagsForModel.setHasAttributeMorphTarget(hasMorphTarget);
        }

        QSSGRenderableObjectList bakedLightingObjects;
        bool usesBlendParticles = blendParticlesEnabled && theModelContext.model.particleBuffer != nullptr
                && model.particleBuffer->particleCount();

        // Subset(s)
        for (int idx = 0; idx < theMesh->subsets.size(); ++idx) {
            // If the materials list < size of subsets, then use the last material for the rest
            QSSGRenderGraphObject *theMaterialObject = nullptr;
            if (model.materials.isEmpty())
                break;
            if (idx + 1 > model.materials.size())
                theMaterialObject = model.materials.last();
            else
                theMaterialObject = model.materials.at(idx);

            if (theMaterialObject == nullptr)
                continue;

            const QSSGRenderSubset &theSubset = theMesh->subsets.at(idx);
            QSSGRenderableObjectFlags renderableFlags = renderableFlagsForModel;
            float subsetOpacity = model.globalOpacity;

            renderableFlags.setPointsTopology(theSubset.rhi.ia.topology == QRhiGraphicsPipeline::Points);
            QSSGRenderableObject *theRenderableObject = nullptr;

            bool usesInstancing = theModelContext.model.instancing()
                    && rhiCtx->rhi()->isFeatureSupported(QRhi::Instancing);
            if (usesInstancing && theModelContext.model.instanceTable->hasTransparency())
                renderableFlags |= QSSGRenderableObjectFlag::HasTransparency;
            if (theModelContext.model.hasTransparency)
                renderableFlags |= QSSGRenderableObjectFlag::HasTransparency;

            // Level Of Detail
            quint32 subsetLevelOfDetail = 0;
            if (!theSubset.lods.isEmpty() && lodThreshold > 0.0f) {
                // Accounts for FOV
                float lodDistanceMultiplier = camera->getLevelOfDetailMultiplier();
                float distanceThreshold = 0.0f;
                const auto scale = mat44::getScale(model.globalTransform);
                float modelScale = qMax(scale.x(), qMax(scale.y(), scale.z()));
                QSSGBounds3 transformedBounds = theSubset.bounds;
                if (camera->type != QSSGRenderGraphObject::Type::OrthographicCamera) {
                    transformedBounds.transform(model.globalTransform);
#ifdef QT_QUICK3D_MESH_LOD_DEBUG
                    renderer->contextInterface()->debugDrawSystem()->drawBounds(transformedBounds, QColor(Qt::red));
#endif
                    const QVector3D cameraNormal = camera->getScalingCorrectDirection();
                    const QVector3D cameraPosition = camera->getGlobalPos();
                    const QSSGPlane cameraPlane = QSSGPlane(cameraPosition, cameraNormal);
                    const QVector3D lodSupportMin = transformedBounds.getSupport(-cameraNormal);
                    const QVector3D lodSupportMax = transformedBounds.getSupport(cameraNormal);
#ifdef QT_QUICK3D_MESH_LOD_DEBUG
                    renderer->contextInterface()->debugDrawSystem()->drawPoint(lodSupportMin, QColor("orange"));
#endif

                    const float distanceMin = cameraPlane.distance(lodSupportMin);
                    const float distanceMax = cameraPlane.distance(lodSupportMax);

                    if (distanceMin * distanceMax < 0.0)
                        distanceThreshold = 0.0;
                    else if (distanceMin >= 0.0)
                        distanceThreshold = distanceMin;
                    else if (distanceMax <= 0.0)
                        distanceThreshold = -distanceMax;

                } else {
                    // Orthographic Projection
                    distanceThreshold = 1.0;
                }

                int currentLod = -1;
                if (model.levelOfDetailBias > 0.0f) {
                    const float threshold = distanceThreshold * lodDistanceMultiplier;
                    const float modelBias = 1 / model.levelOfDetailBias;
                    for (qsizetype i = 0; i < theSubset.lods.count(); ++i) {
                        float subsetDistance = theSubset.lods[i].distance * modelScale * modelBias;
                        float screenSize = subsetDistance / threshold;
                        if (screenSize > lodThreshold)
                            break;
                        currentLod = i;
                    }
                }
                if (currentLod == -1)
                    subsetLevelOfDetail = 0;
                else
                    subsetLevelOfDetail = currentLod + 1;
#ifdef QT_QUICK3D_MESH_LOD_DEBUG
                auto levelOfDetailColor = [](int lod) -> QColor {
                    static QVector<QColor> colors = {
                        QColor(Qt::white),
                        QColor(Qt::red),
                        QColor(Qt::green),
                        QColor(Qt::blue),
                        QColor(Qt::yellow),
                        QColor(Qt::cyan),
                        QColor(Qt::magenta),
                        QColor(Qt::darkRed),
                        QColor(Qt::darkGreen),
                        QColor(Qt::darkBlue),
                        QColor(Qt::darkCyan),
                        QColor(Qt::darkMagenta),
                        QColor(Qt::darkYellow)
                    };

                    if (lod >= colors.count()) {
                        return QColor(Qt::darkGray);
                    }

                    return colors[lod];
                };
                renderer->contextInterface()->debugDrawSystem()->drawBounds(transformedBounds, levelOfDetailColor(subsetLevelOfDetail));
#endif
            }

#ifdef QT_QUICK3D_MESH_LOD_NORMALS_DEBUG
            auto debugNormals = [=](const QSSGRenderModel &model, const QSSGRenderSubset &theSubset, quint32 subsetLevelOfDetail, float lineLength) {
                QSSGMesh::Mesh mesh;
                if (model.geometry)
                    mesh = bufferManager->loadMeshData(model.geometry);
                else
                    mesh = bufferManager->loadMeshData(model.meshPath);

                if (!mesh.isValid())
                    return; // invalid mesh

                QByteArray vertexData = mesh.vertexBuffer().data;
                if (vertexData.isEmpty())
                    return; // no vertex dat
                quint32 vertexStride = mesh.vertexBuffer().stride;
                QByteArray indexData = mesh.indexBuffer().data;
                if (indexData.isEmpty())
                    return; // no index data, not what we're after
                if (mesh.indexBuffer().componentType != QSSGMesh::Mesh::ComponentType::UnsignedInt32)
                    return; // not uint3d, not what we're after either

                quint32 positionOffset = UINT_MAX;
                quint32 normalOffset = UINT_MAX;

                for (const QSSGMesh::Mesh::VertexBufferEntry &vbe : mesh.vertexBuffer().entries) {
                    if (vbe.name == QSSGMesh::MeshInternal::getPositionAttrName()) {
                        positionOffset = vbe.offset;
                        if (vbe.componentType != QSSGMesh::Mesh::ComponentType::Float32 &&
                            vbe.componentCount != 3)
                            return; // not a vec3, some weird stuff
                    } else if (vbe.name == QSSGMesh::MeshInternal::getNormalAttrName()) {
                        normalOffset = vbe.offset;
                        if (vbe.componentType != QSSGMesh::Mesh::ComponentType::Float32 &&
                            vbe.componentCount != 3)
                            return; // not a vec3, really weird normals I guess
                    }
                }

                const auto globalTransform = model.globalTransform;
                // Draw original vertex normals as blue lines
                {
                    // Get Indexes
                    const quint32 *p = reinterpret_cast<const quint32 *>(indexData.constData());
                    const char *vp = vertexData.constData();
                    p += theSubset.offset;
                    for (uint i = 0; i < theSubset.count; ++i) {
                        const quint32 index = *(p + i);
                        const char * posPtr = vp + (index * vertexStride) + positionOffset;
                        const float *fPosPtr = reinterpret_cast<const float *>(posPtr);
                        QVector3D position(fPosPtr[0], fPosPtr[1], fPosPtr[2]);
                        const char * normalPtr = vp + (index * vertexStride) + normalOffset;
                        const float *fNormalPtr = reinterpret_cast<const float *>(normalPtr);
                        QVector3D normal(fNormalPtr[0], fNormalPtr[1], fNormalPtr[2]);
                        position = globalTransform.map(position);
                        normal = mat33::transform(theModelContext.normalMatrix, normal);
                        normal = normal.normalized();
                        renderer->contextInterface()->debugDrawSystem()->drawLine(position, position + (normal * lineLength), QColor(Qt::blue));
                    }
                }

                // Draw lod vertex normals as red lines
                if (subsetLevelOfDetail != 0) {
                    // Get Indexes
                    const quint32 *p = reinterpret_cast<const quint32 *>(indexData.constData());
                    const char *vp = vertexData.constData();
                    p += theSubset.lodOffset(subsetLevelOfDetail);
                    const quint32 indexCount = theSubset.lodCount(subsetLevelOfDetail);
                    for (uint i = 0; i < indexCount; ++i) {
                        const quint32 index = *(p + i);
                        const char * posPtr = vp + (index * vertexStride) + positionOffset;
                        const float *fPosPtr = reinterpret_cast<const float *>(posPtr);
                        QVector3D position(fPosPtr[0], fPosPtr[1], fPosPtr[2]);
                        const char * normalPtr = vp + (index * vertexStride) + normalOffset;
                        const float *fNormalPtr = reinterpret_cast<const float *>(normalPtr);
                        QVector3D normal(fNormalPtr[0], fNormalPtr[1], fNormalPtr[2]);
                        position = globalTransform.map(position);
                        normal = mat33::transform(theModelContext.normalMatrix, normal);
                        normal = normal.normalized();
                        renderer->contextInterface()->debugDrawSystem()->drawLine(position, position + (normal * lineLength), QColor(Qt::red));
                    }
                }
            };
#endif

            QVector3D theModelCenter(theSubset.bounds.center());
            theModelCenter = mat44::transform(model.globalTransform, theModelCenter);
#ifdef QT_QUICK3D_MESH_LOD_NORMALS_DEBUG
            debugNormals(model, theSubset, subsetLevelOfDetail, (theModelCenter - camera->getGlobalPos()).length() * 0.01);
#endif

            if (theMaterialObject->type == QSSGRenderGraphObject::Type::DefaultMaterial ||
                theMaterialObject->type == QSSGRenderGraphObject::Type::PrincipledMaterial ||
                theMaterialObject->type == QSSGRenderGraphObject::Type::SpecularGlossyMaterial) {
                QSSGRenderDefaultMaterial &theMaterial(static_cast<QSSGRenderDefaultMaterial &>(*theMaterialObject));
                QSSGDefaultMaterialPreparationResult theMaterialPrepResult(
                        prepareDefaultMaterialForRender(theMaterial, renderableFlags, subsetOpacity, lights, ioFlags));
                QSSGShaderDefaultMaterialKey &theGeneratedKey(theMaterialPrepResult.materialKey);
                subsetOpacity = theMaterialPrepResult.opacity;
                QSSGRenderableImage *firstImage(theMaterialPrepResult.firstImage);
                wasDirty |= theMaterialPrepResult.dirty;
                renderableFlags = theMaterialPrepResult.renderableFlags;

                // Blend particles
                renderer->defaultMaterialShaderKeyProperties().m_blendParticles.setValue(theGeneratedKey, usesBlendParticles);

                // Skin
                renderer->defaultMaterialShaderKeyProperties().m_boneCount.setValue(theGeneratedKey, model.boneCount);
                renderer->defaultMaterialShaderKeyProperties().m_usesFloatJointIndices.setValue(
                        theGeneratedKey, !rhiCtx->rhi()->isFeatureSupported(QRhi::IntAttributes));
                // Instancing
                renderer->defaultMaterialShaderKeyProperties().m_usesInstancing.setValue(theGeneratedKey, usesInstancing);
                // Morphing
                renderer->defaultMaterialShaderKeyProperties().m_targetCount.setValue(theGeneratedKey,
                                        theSubset.rhi.ia.targetCount);
                renderer->defaultMaterialShaderKeyProperties().m_targetPositionOffset.setValue(theGeneratedKey,
                                        theSubset.rhi.ia.targetOffsets[QSSGRhiInputAssemblerState::PositionSemantic]);
                renderer->defaultMaterialShaderKeyProperties().m_targetNormalOffset.setValue(theGeneratedKey,
                                        theSubset.rhi.ia.targetOffsets[QSSGRhiInputAssemblerState::NormalSemantic]);
                renderer->defaultMaterialShaderKeyProperties().m_targetTangentOffset.setValue(theGeneratedKey,
                                        theSubset.rhi.ia.targetOffsets[QSSGRhiInputAssemblerState::TangentSemantic]);
                renderer->defaultMaterialShaderKeyProperties().m_targetBinormalOffset.setValue(theGeneratedKey,
                                        theSubset.rhi.ia.targetOffsets[QSSGRhiInputAssemblerState::BinormalSemantic]);
                renderer->defaultMaterialShaderKeyProperties().m_targetTexCoord0Offset.setValue(theGeneratedKey,
                                        theSubset.rhi.ia.targetOffsets[QSSGRhiInputAssemblerState::TexCoord0Semantic]);
                renderer->defaultMaterialShaderKeyProperties().m_targetTexCoord1Offset.setValue(theGeneratedKey,
                                        theSubset.rhi.ia.targetOffsets[QSSGRhiInputAssemblerState::TexCoord1Semantic]);
                renderer->defaultMaterialShaderKeyProperties().m_targetColorOffset.setValue(theGeneratedKey,
                                        theSubset.rhi.ia.targetOffsets[QSSGRhiInputAssemblerState::ColorSemantic]);

                theRenderableObject = RENDER_FRAME_NEW<QSSGSubsetRenderable>(contextInterface,
                                                                             QSSGSubsetRenderable::Type::DefaultMaterialMeshSubset,
                                                                             renderableFlags,
                                                                             theModelCenter,
                                                                             renderer,
                                                                             theSubset,
                                                                             theModelContext,
                                                                             subsetOpacity,
                                                                             subsetLevelOfDetail,
                                                                             theMaterial,
                                                                             firstImage,
                                                                             theGeneratedKey,
                                                                             lights);
                wasDirty = wasDirty || renderableFlags.isDirty();
            } else if (theMaterialObject->type == QSSGRenderGraphObject::Type::CustomMaterial) {
                QSSGRenderCustomMaterial &theMaterial(static_cast<QSSGRenderCustomMaterial &>(*theMaterialObject));

                const QSSGRef<QSSGCustomMaterialSystem> &theMaterialSystem(contextInterface.customMaterialSystem());
                wasDirty |= theMaterialSystem->prepareForRender(theModelContext.model, theSubset, theMaterial);

                QSSGDefaultMaterialPreparationResult theMaterialPrepResult(
                        prepareCustomMaterialForRender(theMaterial, renderableFlags, subsetOpacity, wasDirty,
                                                       lights, ioFlags));
                QSSGShaderDefaultMaterialKey &theGeneratedKey(theMaterialPrepResult.materialKey);
                subsetOpacity = theMaterialPrepResult.opacity;
                QSSGRenderableImage *firstImage(theMaterialPrepResult.firstImage);
                renderableFlags = theMaterialPrepResult.renderableFlags;

                if (model.particleBuffer && model.particleBuffer->particleCount())
                    renderer->defaultMaterialShaderKeyProperties().m_blendParticles.setValue(theGeneratedKey, true);
                else
                    renderer->defaultMaterialShaderKeyProperties().m_blendParticles.setValue(theGeneratedKey, false);

                // Skin
                renderer->defaultMaterialShaderKeyProperties().m_boneCount.setValue(theGeneratedKey, model.boneCount);
                renderer->defaultMaterialShaderKeyProperties().m_usesFloatJointIndices.setValue(
                        theGeneratedKey, !rhiCtx->rhi()->isFeatureSupported(QRhi::IntAttributes));

                // Instancing
                bool usesInstancing = theModelContext.model.instancing()
                        && rhiCtx->rhi()->isFeatureSupported(QRhi::Instancing);
                renderer->defaultMaterialShaderKeyProperties().m_usesInstancing.setValue(theGeneratedKey, usesInstancing);
                // Morphing
                renderer->defaultMaterialShaderKeyProperties().m_targetCount.setValue(theGeneratedKey,
                                        theSubset.rhi.ia.targetCount);
                renderer->defaultMaterialShaderKeyProperties().m_targetPositionOffset.setValue(theGeneratedKey,
                                        theSubset.rhi.ia.targetOffsets[QSSGRhiInputAssemblerState::PositionSemantic]);
                renderer->defaultMaterialShaderKeyProperties().m_targetNormalOffset.setValue(theGeneratedKey,
                                        theSubset.rhi.ia.targetOffsets[QSSGRhiInputAssemblerState::NormalSemantic]);
                renderer->defaultMaterialShaderKeyProperties().m_targetTangentOffset.setValue(theGeneratedKey,
                                        theSubset.rhi.ia.targetOffsets[QSSGRhiInputAssemblerState::TangentSemantic]);
                renderer->defaultMaterialShaderKeyProperties().m_targetBinormalOffset.setValue(theGeneratedKey,
                                        theSubset.rhi.ia.targetOffsets[QSSGRhiInputAssemblerState::BinormalSemantic]);
                renderer->defaultMaterialShaderKeyProperties().m_targetTexCoord0Offset.setValue(theGeneratedKey,
                                        theSubset.rhi.ia.targetOffsets[QSSGRhiInputAssemblerState::TexCoord0Semantic]);
                renderer->defaultMaterialShaderKeyProperties().m_targetTexCoord1Offset.setValue(theGeneratedKey,
                                        theSubset.rhi.ia.targetOffsets[QSSGRhiInputAssemblerState::TexCoord1Semantic]);
                renderer->defaultMaterialShaderKeyProperties().m_targetColorOffset.setValue(theGeneratedKey,
                                        theSubset.rhi.ia.targetOffsets[QSSGRhiInputAssemblerState::ColorSemantic]);

                if (theMaterial.m_iblProbe)
                    theMaterial.m_iblProbe->clearDirty();

                theRenderableObject = RENDER_FRAME_NEW<QSSGSubsetRenderable>(contextInterface,
                                                                             QSSGSubsetRenderable::Type::CustomMaterialMeshSubset,
                                                                             renderableFlags,
                                                                             theModelCenter,
                                                                             renderer,
                                                                             theSubset,
                                                                             theModelContext,
                                                                             subsetOpacity,
                                                                             subsetLevelOfDetail,
                                                                             theMaterial,
                                                                             firstImage,
                                                                             theGeneratedKey,
                                                                             lights);
            }
            if (theRenderableObject) {
                if (theRenderableObject->renderableFlags.requiresScreenTexture())
                    screenTextureObjects.push_back({theRenderableObject, getCameraDistanceSq(*theRenderableObject, cameraData)});
                else if (theRenderableObject->renderableFlags.hasTransparency())
                    transparentObjects.push_back({theRenderableObject, getCameraDistanceSq(*theRenderableObject, cameraData)});
                else
                    opaqueObjects.push_back({theRenderableObject, getCameraDistanceSq(*theRenderableObject, cameraData)});

                if (theRenderableObject->renderableFlags.usedInBakedLighting())
                    bakedLightingObjects.push_back({theRenderableObject, getCameraDistanceSq(*theRenderableObject, cameraData)});
            }
        }

        if (!bakedLightingObjects.isEmpty())
            bakedLightingModels.push_back(QSSGBakedLightingModel(&model, bakedLightingObjects));
    }

    return wasDirty;
}

bool QSSGLayerRenderData::prepareParticlesForRender(const RenderableNodeEntries &renderableParticles,
                                                    const QSSGCameraData &cameraData)
{
    QSSGRenderContextInterface &contextInterface = *renderer->contextInterface();
    // TODO/NOTE: We probably want to do this at an earlier stage!
    const bool supportRgba32f = contextInterface.rhiContext()->rhi()->isTextureFormatSupported(QRhiTexture::RGBA32F);
    const bool supportRgba16f = contextInterface.rhiContext()->rhi()->isTextureFormatSupported(QRhiTexture::RGBA16F);
    if (!supportRgba32f && !supportRgba16f) {
        if (!particlesNotSupportedWarningShown)
            qWarning () << "Particles not supported due to missing RGBA32F and RGBA16F texture format support";
        particlesNotSupportedWarningShown = true;
        return false;
    }

    bool dirty = false;

    for (const auto &renderable : renderableParticles) {
        const QSSGRenderParticles &particles = *static_cast<QSSGRenderParticles *>(renderable.node);
        const auto &lights = renderable.lights;

        QSSGRenderableObjectFlags renderableFlags;
        renderableFlags.setCastsShadows(false);
        renderableFlags.setReceivesShadows(false);
        renderableFlags.setHasAttributePosition(true);
        renderableFlags.setHasAttributeNormal(true);
        renderableFlags.setHasAttributeTexCoord0(true);
        renderableFlags.setHasAttributeColor(true);
        renderableFlags.setHasTransparency(particles.m_hasTransparency);
        renderableFlags.setCastsReflections(particles.m_castsReflections);

        float opacity = particles.globalOpacity;
        QVector3D center(particles.m_particleBuffer.bounds().center());
        center = mat44::transform(particles.globalTransform, center);

        QSSGRenderableImage *firstImage = nullptr;
        if (particles.m_sprite) {
            const QSSGRef<QSSGBufferManager> &bufferManager = contextInterface.bufferManager();

            if (particles.m_sprite->clearDirty())
                dirty = true;

            const QSSGRenderImageTexture texture = bufferManager->loadRenderImage(particles.m_sprite);
            QSSGRenderableImage *theImage = RENDER_FRAME_NEW<QSSGRenderableImage>(contextInterface, QSSGRenderableImage::Type::Diffuse, *particles.m_sprite, texture);
            firstImage = theImage;
        }

        QSSGRenderableImage *colorTable = nullptr;
        if (particles.m_colorTable) {
            const QSSGRef<QSSGBufferManager> &bufferManager = contextInterface.bufferManager();

            if (particles.m_colorTable->clearDirty())
                dirty = true;

            const QSSGRenderImageTexture texture = bufferManager->loadRenderImage(particles.m_colorTable);

            QSSGRenderableImage *theImage = RENDER_FRAME_NEW<QSSGRenderableImage>(contextInterface, QSSGRenderableImage::Type::Diffuse, *particles.m_colorTable, texture);
            colorTable = theImage;
        }

        if (opacity > 0.0f && particles.m_particleBuffer.particleCount()) {
            auto *theRenderableObject = RENDER_FRAME_NEW<QSSGParticlesRenderable>(contextInterface,
                                                                                  renderableFlags,
                                                                                  center,
                                                                                  renderer,
                                                                                  particles,
                                                                                  firstImage,
                                                                                  colorTable,
                                                                                  lights,
                                                                                  opacity);
            if (theRenderableObject) {
                if (theRenderableObject->renderableFlags.requiresScreenTexture())
                    screenTextureObjects.push_back({theRenderableObject, getCameraDistanceSq(*theRenderableObject, cameraData)});
                else if (theRenderableObject->renderableFlags.hasTransparency())
                    transparentObjects.push_back({theRenderableObject, getCameraDistanceSq(*theRenderableObject, cameraData)});
                else
                    opaqueObjects.push_back({theRenderableObject, getCameraDistanceSq(*theRenderableObject, cameraData)});
            }
        }
    }

    return dirty;
}

bool QSSGLayerRenderData::prepareItem2DsForRender(const QSSGRenderContextInterface &ctxIfc,
                                                  const RenderableItem2DEntries &renderableItem2Ds,
                                                  const QMatrix4x4 &inViewProjection)
{
    const bool hasItems = (renderableItem2Ds.size() != 0);
    if (hasItems) {
        const auto &clipSpaceCorrMatrix = ctxIfc.rhiContext()->rhi()->clipSpaceCorrMatrix();
        for (const auto &theItem2D : renderableItem2Ds) {
            theItem2D->MVP = inViewProjection * theItem2D->globalTransform;
            static const QMatrix4x4 flipMatrix(1.0f, 0.0f, 0.0f, 0.0f,
                                               0.0f, -1.0f, 0.0f, 0.0f,
                                               0.0f, 0.0f, 1.0f, 0.0f,
                                               0.0f, 0.0f, 0.0f, 1.0f);
            theItem2D->MVP = clipSpaceCorrMatrix * theItem2D->MVP * flipMatrix;
        }
    }

    return hasItems;
}

void QSSGLayerRenderData::prepareResourceLoaders()
{
    QSSGRenderContextInterface &contextInterface = *renderer->contextInterface();
    const QSSGRef<QSSGBufferManager> &bufferManager = contextInterface.bufferManager();

    for (const auto resourceLoader : std::as_const(layer.resourceLoaders))
        bufferManager->processResourceLoader(static_cast<QSSGRenderResourceLoader *>(resourceLoader));
}

void QSSGLayerRenderData::prepareReflectionProbesForRender()
{
    const auto probeCount = reflectionProbes.size();
    if (!reflectionMapManager)
        reflectionMapManager = new QSSGRenderReflectionMap(*renderer->contextInterface());

    for (int i = 0; i < probeCount; i++) {
        QSSGRenderReflectionProbe* probe = reflectionProbes.at(i);

        int reflectionObjectCount = 0;
        QVector3D probeExtent = probe->boxSize / 2;
        QSSGBounds3 probeBound = QSSGBounds3::centerExtents(probe->getGlobalPos() + probe->boxOffset, probeExtent);

        const auto injectProbe = [&](const QSSGRenderableObjectHandle &handle) {
            if (handle.obj->renderableFlags.testFlag(QSSGRenderableObjectFlag::ReceivesReflections)
                && !(handle.obj->type == QSSGRenderableObject::Type::Particles)) {
                QSSGSubsetRenderable* renderableObj = static_cast<QSSGSubsetRenderable*>(handle.obj);
                QSSGBounds3 nodeBound = renderableObj->bounds;
                QVector4D vmin(nodeBound.minimum, 1.0);
                QVector4D vmax(nodeBound.maximum, 1.0);
                vmin = renderableObj->globalTransform * vmin;
                vmax = renderableObj->globalTransform * vmax;
                nodeBound.minimum = vmin.toVector3D();
                nodeBound.maximum = vmax.toVector3D();
                if (probeBound.intersects(nodeBound)) {
                    QVector3D nodeBoundCenter = nodeBound.center();
                    QVector3D probeBoundCenter = probeBound.center();
                    float distance = nodeBoundCenter.distanceToPoint(probeBoundCenter);
                    if (renderableObj->reflectionProbeIndex == -1 || distance < renderableObj->distanceFromReflectionProbe) {
                        renderableObj->reflectionProbeIndex = i;
                        renderableObj->distanceFromReflectionProbe = distance;
                        renderableObj->reflectionProbe.parallaxCorrection = probe->parallaxCorrection;
                        renderableObj->reflectionProbe.probeCubeMapCenter = probe->getGlobalPos();
                        renderableObj->reflectionProbe.probeBoxMax = probeBound.maximum;
                        renderableObj->reflectionProbe.probeBoxMin = probeBound.minimum;
                        renderableObj->reflectionProbe.enabled = true;
                        reflectionObjectCount++;
                    }
                }
            }
        };

        for (const auto &handle : std::as_const(transparentObjects))
            injectProbe(handle);

        for (const auto &handle : std::as_const(opaqueObjects))
            injectProbe(handle);

        if (probe->texture)
            reflectionMapManager->addTexturedReflectionMapEntry(i, *probe);
        else if (reflectionObjectCount > 0)
            reflectionMapManager->addReflectionMapEntry(i, *probe);
    }
}

static bool scopeLight(QSSGRenderNode *node, QSSGRenderNode *lightScope)
{
    // check if the node is parent of the lightScope
    while (node) {
        if (node == lightScope)
            return true;
        node = node->parent;
    }
    return false;
}

static const int REDUCED_MAX_LIGHT_COUNT_THRESHOLD_BYTES = 4096; // 256 vec4

static inline int effectiveMaxLightCount(const QSSGShaderFeatures &features)
{
    if (features.isSet(QSSGShaderFeatures::Feature::ReduceMaxNumLights))
        return QSSG_REDUCED_MAX_NUM_LIGHTS;

    return QSSG_MAX_NUM_LIGHTS;
}

void updateDirtySkeletons(const QVector<QSSGRenderableNodeEntry> &renderableNodes)
{
    // First model using skeleton clears the dirty flag so we need another mechanism
    // to tell to the other models the skeleton is dirty.
    QSet<QSSGRenderSkeleton *> dirtySkeletons;
    for (const auto &node : std::as_const(renderableNodes)) {
        if (node.node->type == QSSGRenderGraphObject::Type::Model) {
            auto modelNode = static_cast<QSSGRenderModel *>(node.node);
            auto skeletonNode = modelNode->skeleton;
            bool hcj = false;
            if (modelNode->skin) {
                modelNode->boneData = modelNode->skin->boneData;
                modelNode->boneCount = modelNode->boneData.size() / 2 / 4 / 16;
            } else if (skeletonNode) {
                const bool dirtySkeleton = dirtySkeletons.contains(skeletonNode);
                const bool hasDirtyNonJoints = (modelNode->skeletonContainsNonJointNodes
                                                && (hasDirtyNonJointNodes(skeletonNode, hcj) || dirtySkeleton));
                const bool dirtyTransform = skeletonNode->isDirty(QSSGRenderNode::DirtyFlag::TransformDirty);
                if (modelNode->skinningDirty || hasDirtyNonJoints || dirtyTransform) {
                    skeletonNode->boneTransformsDirty = false;
                    if (hasDirtyNonJoints && !dirtySkeleton)
                        dirtySkeletons.insert(skeletonNode);
                    modelNode->skinningDirty = false;
                    const qsizetype dataSize = BONEDATASIZE4ID(skeletonNode->maxIndex);
                    if (modelNode->boneData.size() < dataSize)
                        modelNode->boneData.resize(dataSize);
                    skeletonNode->calculateGlobalVariables();
                    modelNode->skeletonContainsNonJointNodes = false;
                    for (auto &child : skeletonNode->children)
                        collectBoneTransforms(&child, modelNode, modelNode->inverseBindPoses);
                }
                modelNode->boneCount = modelNode->boneData.size() / 2 / 4 / 16;
            } else {
                modelNode->boneData.clear();
                modelNode->boneCount = 0;
            }
            const int numMorphTarget = modelNode->morphTargets.size();
            for (int i = 0; i < numMorphTarget; ++i) {
                auto morphTarget = static_cast<const QSSGRenderMorphTarget *>(modelNode->morphTargets.at(i));
                modelNode->morphWeights[i] = morphTarget->weight;
                modelNode->morphAttributes[i] = morphTarget->attributes;
                if (i > MAX_MORPH_TARGET_INDEX_SUPPORTS_NORMALS)
                    modelNode->morphAttributes[i] &= 0x1; // MorphTarget.Position
                else if (i > MAX_MORPH_TARGET_INDEX_SUPPORTS_TANGENTS)
                    modelNode->morphAttributes[i] &= 0x3; // MorphTarget.Position | MorphTarget.Normal
            }
        }
    }

    dirtySkeletons.clear();
}

void QSSGLayerRenderData::prepareForRender()
{
    if (layerPrepResult.has_value())
        return;

    // Verify that the depth write list(s) were cleared between frames
    QSSG_ASSERT(renderedDepthWriteObjects.isEmpty(), renderedDepthWriteObjects.clear());
    QSSG_ASSERT(renderedOpaqueDepthPrepassObjects.isEmpty(), renderedOpaqueDepthPrepassObjects.clear());

    QRect theViewport(renderer->contextInterface()->viewport());

    // Create base pipeline state
    ps = {}; // Reset
    ps.viewport = { float(theViewport.x()), float(theViewport.y()), float(theViewport.width()), float(theViewport.height()), 0.0f, 1.0f };
    if (layer.scissorRect.isValid()) {
        ps.scissorEnable = true;
        ps.scissor = { layer.scissorRect.x(),
                       theViewport.height() - (layer.scissorRect.y() + layer.scissorRect.height()),
                       layer.scissorRect.width(),
                       layer.scissorRect.height() };
    }

    bool wasDirty = false;
    bool wasDataDirty = false;
    wasDirty = layer.isDirty();

    QSSGLayerRenderPreparationResult thePrepResult(theViewport, layer);

    // SSAO
    const bool SSAOEnabled = layer.ssaoEnabled();
    thePrepResult.flags.setRequiresSsaoPass(SSAOEnabled);
    features.set(QSSGShaderFeatures::Feature::Ssao, SSAOEnabled);

    // Effects
    bool requiresDepthTexture = SSAOEnabled;
    for (QSSGRenderEffect *theEffect = layer.firstEffect; theEffect; theEffect = theEffect->m_nextEffect) {
        if (theEffect->isDirty()) {
            wasDirty = true;
            theEffect->clearDirty();
        }
        if (theEffect->requiresDepthTexture)
            requiresDepthTexture = true;
    }
    thePrepResult.flags.setRequiresDepthTexture(requiresDepthTexture);

    // Tonemapping. Except when there are effects, then it is up to the
    // last pass of the last effect to perform tonemapping.
    if (!layer.firstEffect)
        QSSGRenderer::setTonemapFeatures(features, layer.tonemapMode);

    // We may not be able to have an array of 15 light struct elements in
    // the shaders. Switch on the reduced-max-number-of-lights feature
    // if necessary. In practice this is relevant with OpenGL ES 3.0 or
    // 2.0, because there are still implementations in use that only
    // support the spec mandated minimum of 224 vec4s (so 3584 bytes).
    const auto &rhiCtx = renderer->contextInterface()->rhiContext();
    if (rhiCtx->maxUniformBufferRange() < REDUCED_MAX_LIGHT_COUNT_THRESHOLD_BYTES) {
        features.set(QSSGShaderFeatures::Feature::ReduceMaxNumLights, true);
        static bool notified = false;
        if (!notified) {
            notified = true;
            qCDebug(lcQuick3DRender, "Qt Quick 3D maximum number of lights has been reduced from %d to %d due to the graphics driver's limitations",
                    QSSG_MAX_NUM_LIGHTS, QSSG_REDUCED_MAX_NUM_LIGHTS);
        }
    }

    // IBL Lightprobe Image
    QSSGRenderImageTexture lightProbeTexture;
    if (layer.lightProbe) {
        if (layer.lightProbe->m_format == QSSGRenderTextureFormat::Unknown) {
            // Choose on a format that makes sense for a light probe
            // At this point it's just a suggestion
            if (renderer->contextInterface()->rhiContext()->rhi()->isTextureFormatSupported(QRhiTexture::RGBA16F))
                layer.lightProbe->m_format = QSSGRenderTextureFormat::RGBA16F;
            else
                layer.lightProbe->m_format = QSSGRenderTextureFormat::RGBE8;
        }

        if (layer.lightProbe->clearDirty())
            wasDataDirty = true;

        // NOTE: This call can lead to rendering (of envmap) and a texture upload
        lightProbeTexture = renderer->contextInterface()->bufferManager()->loadRenderImage(layer.lightProbe, QSSGBufferManager::MipModeBsdf);
        if (lightProbeTexture.m_texture) {

            features.set(QSSGShaderFeatures::Feature::LightProbe, true);
            features.set(QSSGShaderFeatures::Feature::IblOrientation, !layer.probeOrientation.isIdentity());

            // By this point we will know what the actual texture format of the light probe is
            // Check if using RGBE format light probe texture (the Rhi format will be RGBA8)
            if (lightProbeTexture.m_flags.isRgbe8())
                features.set(QSSGShaderFeatures::Feature::RGBELightProbe, true);
        } else {
            layer.lightProbe = nullptr;
        }
    }

    // Gather Spatial Nodes from Render Tree
    // Do not just clear() renderableNodes and friends. Rather, reuse
    // the space (even if clear does not actually deallocate, it still
    // costs time to run dtors and such). In scenes with a static node
    // count in the range of thousands this may matter.
    int renderableModelsCount = 0;
    int renderableParticlesCount = 0;
    int renderableItem2DsCount = 0;
    int cameraNodeCount = 0;
    int lightNodeCount = 0;
    int reflectionProbeCount = 0;
    quint32 dfsIndex = 0;
    for (auto &theChild : layer.children)
        wasDataDirty |= maybeQueueNodeForRender(theChild,
                                                renderableModels,
                                                renderableModelsCount,
                                                renderableParticles,
                                                renderableParticlesCount,
                                                renderableItem2Ds,
                                                renderableItem2DsCount,
                                                cameras,
                                                cameraNodeCount,
                                                lights,
                                                lightNodeCount,
                                                reflectionProbes,
                                                reflectionProbeCount,
                                                dfsIndex);

    if (renderableModels.size() != renderableModelsCount)
        renderableModels.resize(renderableModelsCount);
    if (renderableParticles.size() != renderableParticlesCount)
        renderableParticles.resize(renderableParticlesCount);
    if (renderableItem2Ds.size() != renderableItem2DsCount)
        renderableItem2Ds.resize(renderableItem2DsCount);

    if (cameras.size() != cameraNodeCount)
        cameras.resize(cameraNodeCount);
    if (lights.size() != lightNodeCount)
        lights.resize(lightNodeCount);
    if (reflectionProbes.size() != reflectionProbeCount)
        reflectionProbes.resize(reflectionProbeCount);

    // Cameras
    // 1. If there's an explicit camera set and it's active (visible) we'll use that.
    // 2. ... if the explicitly set camera is not visible, no further attempts will be done.
    // 3. If no explicit camera is set, we'll search and pick the first active camera.
    camera = layer.explicitCamera;
    if (camera != nullptr) {
        // 1.
        wasDataDirty = wasDataDirty || camera->isDirty();
        QSSGCameraGlobalCalculationResult theResult = thePrepResult.setupCameraForRender(*camera);
        wasDataDirty = wasDataDirty || theResult.m_wasDirty;
        if (!theResult.m_computeFrustumSucceeded)
            qCCritical(INTERNAL_ERROR, "Failed to calculate camera frustum");

        // 2.
        if (!camera->getGlobalState(QSSGRenderCamera::GlobalState::Active))
            camera = nullptr;
    } else {
        // 3.
        for (auto iter = cameras.cbegin();
             (camera == nullptr) && (iter != cameras.cend()); iter++) {
            QSSGRenderCamera *theCamera = *iter;
            wasDataDirty = wasDataDirty
                    || theCamera->isDirty();
            QSSGCameraGlobalCalculationResult theResult = thePrepResult.setupCameraForRender(*theCamera);
            wasDataDirty = wasDataDirty || theResult.m_wasDirty;
            if (!theResult.m_computeFrustumSucceeded)
                qCCritical(INTERNAL_ERROR, "Failed to calculate camera frustum");
            if (theCamera->getGlobalState(QSSGRenderCamera::GlobalState::Active))
                camera = theCamera;
        }
    }
    layer.renderedCamera = camera;

    // ResourceLoaders
    prepareResourceLoaders();

    // Skeletons
    updateDirtySkeletons(renderableModels);

    // Lights
    int shadowMapCount = 0;
    bool hasScopedLights = false;
    // Determine which lights will actually Render
    // Determine how many lights will need shadow maps
    // NOTE: This culling is specific to our Forward renderer
    const int maxLightCount = effectiveMaxLightCount(features);
    const bool showLightCountWarning = !tooManyLightsWarningShown && (lights.size() > maxLightCount);
    if (showLightCountWarning) {
        qWarning("Too many lights in scene, maximum is %d", maxLightCount);
        tooManyLightsWarningShown = true;
    }

    QSSGShaderLightList renderableLights; // All lights (upto 'maxLightCount')

    // List should contain only enabled lights (active && birghtness > 0).
    {
        auto it = lights.crbegin();
        const auto end = it + qMin(maxLightCount, lights.size());

        for (; it != end; ++it) {
            QSSGRenderLight *renderLight = (*it);
            hasScopedLights |= (renderLight->m_scope != nullptr);
            const bool mightCastShadows = renderLight->m_castShadow && !renderLight->m_fullyBaked;
            const bool shadows = mightCastShadows && (shadowMapCount < QSSG_MAX_NUM_SHADOW_MAPS);
            shadowMapCount += int(shadows);
            const auto &direction = renderLight->getScalingCorrectDirection();
            renderableLights.push_back(QSSGShaderLight{ renderLight, shadows, direction });
        }

        if ((shadowMapCount >= QSSG_MAX_NUM_SHADOW_MAPS) && !tooManyShadowLightsWarningShown) {
            qWarning("Too many shadow casting lights in scene, maximum is %d", QSSG_MAX_NUM_SHADOW_MAPS);
            tooManyShadowLightsWarningShown = true;
        }
    }

    if (shadowMapCount > 0) { // Setup Shadow Maps Entries for Lights casting shadows
        if (!shadowMapManager)
            shadowMapManager = new QSSGRenderShadowMap(*renderer->contextInterface());

        for (int i = 0, end = renderableLights.size(); i != end; ++i) {
            const auto &shaderLight = renderableLights.at(i);
            if (shaderLight.shadows) {
                quint32 mapSize = 1 << shaderLight.light->m_shadowMapRes;
                ShadowMapModes mapMode = (shaderLight.light->type != QSSGRenderLight::Type::DirectionalLight)
                        ? ShadowMapModes::CUBE
                        : ShadowMapModes::VSM;
                shadowMapManager->addShadowMapEntry(i,
                                                    mapSize,
                                                    mapSize,
                                                    mapMode,
                                                    shaderLight.light->debugObjectName);
                thePrepResult.flags.setRequiresShadowMapPass(true);
                // Any light with castShadow=true triggers shadow mapping
                // in the generated shaders. The fact that some (or even
                // all) objects may opt out from receiving shadows plays no
                // role here whatsoever.
                features.set(QSSGShaderFeatures::Feature::Ssm, true);
            }
        }
    }

    // Give each renderable a copy of the lights available
    // Also setup scoping for scoped lights

    QSSG_ASSERT(globalLights.isEmpty(), globalLights.clear());
    if (hasScopedLights) { // Filter out scoped lights from the global lights list
        for (const auto &shaderLight : std::as_const(renderableLights)) {
            if (!shaderLight.light->m_scope)
                globalLights.push_back(shaderLight);
        }

        const auto prepareLightsWithScopedLights = [&renderableLights, this](QVector<QSSGRenderableNodeEntry> &renderableNodes) {
            for (qint32 idx = 0, end = renderableNodes.size(); idx < end; ++idx) {
                QSSGRenderableNodeEntry &theNodeEntry(renderableNodes[idx]);
                QSSGShaderLightList filteredLights;
                for (const auto &light : std::as_const(renderableLights)) {
                    if (light.light->m_scope && !scopeLight(theNodeEntry.node, light.light->m_scope))
                        continue;
                    filteredLights.push_back(light);
                }

                if (filteredLights.isEmpty()) { // Node without scoped lights, just reference the global light list.
                    theNodeEntry.lights = QSSGDataView(globalLights);
                } else {
                    // This node has scoped lights, i.e., it's lights differ from the global list
                    // we therefore create a bespoke light list for it. Technically this might be the same for
                    // more then this one node, but the overhead for tracking that is not worth it.
                    using NodeLights = QSSGShaderLight[16]; // Per-node light list
                    QSSGShaderLight *customLightList = RENDER_FRAME_NEW<QSSGShaderLight>(*renderer->contextInterface(), sizeof(NodeLights));
                    std::copy(filteredLights.cbegin(), filteredLights.cend(), customLightList);
                    theNodeEntry.lights = QSSGDataView(customLightList, filteredLights.size());
                }
            }
        };

        prepareLightsWithScopedLights(renderableModels);
        prepareLightsWithScopedLights(renderableParticles);
    } else { // Just a simple copy
        globalLights = renderableLights;
        // No scoped lights, all nodes can just reference the global light list.
        const auto prepareLights = [this](QVector<QSSGRenderableNodeEntry> &renderableNodes) {
            for (qint32 idx = 0, end = renderableNodes.size(); idx < end; ++idx) {
                QSSGRenderableNodeEntry &theNodeEntry(renderableNodes[idx]);
                theNodeEntry.lights = QSSGDataView(globalLights);
            }
        };

        prepareLights(renderableModels);
        prepareLights(renderableParticles);
    }

    // Calculate viewProjection and clippingFrustum for Render Camera
    QMatrix4x4 viewProjection(Qt::Uninitialized);
    float meshLodThreshold = 1.0f;
    if (camera) {
        camera->dpr = renderer->contextInterface()->dpr();
        camera->calculateViewProjectionMatrix(viewProjection);
        if (camera->enableFrustumClipping) {
            QSSGClipPlane nearPlane;
            QMatrix3x3 theUpper33(camera->globalTransform.normalMatrix());
            QVector3D dir(mat33::transform(theUpper33, QVector3D(0, 0, -1)));
            dir.normalize();
            nearPlane.normal = dir;
            QVector3D theGlobalPos = camera->getGlobalPos() + camera->clipNear * dir;
            nearPlane.d = -(QVector3D::dotProduct(dir, theGlobalPos));
            // the near plane's bbox edges are calculated in the clipping frustum's
            // constructor.
            clippingFrustum = QSSGClippingFrustum{viewProjection, nearPlane};
        }
        meshLodThreshold = camera->levelOfDetailPixelThreshold / theViewport.width();
    } else {
        viewProjection = QMatrix4x4(/*identity*/);
    }

    const QSSGCameraData &cameraData = getCameraDirectionAndPosition();

    wasDirty |= prepareModelForRender(renderableModels, viewProjection, thePrepResult.flags, cameraData, meshLodThreshold);
    wasDirty |= prepareParticlesForRender(renderableParticles, cameraData);
    wasDirty |= prepareItem2DsForRender(*renderer->contextInterface(), renderableItem2Ds, viewProjection);

    prepareReflectionProbesForRender();

    wasDirty = wasDirty || wasDataDirty;
    thePrepResult.flags.setWasDirty(wasDirty);
    thePrepResult.flags.setLayerDataDirty(wasDataDirty);

    layerPrepResult = thePrepResult;

    //
    const bool animating = wasDirty;
    if (animating)
        layer.progAAPassIndex = 0;

    const bool progressiveAA = layer.antialiasingMode == QSSGRenderLayer::AAMode::ProgressiveAA && !animating;
    layer.progressiveAAIsActive = progressiveAA;
    const bool temporalAA = layer.temporalAAEnabled && !progressiveAA &&  layer.antialiasingMode != QSSGRenderLayer::AAMode::MSAA;

    layer.temporalAAIsActive = temporalAA;

    QVector2D vertexOffsetsAA;

    if (progressiveAA && layer.progAAPassIndex > 0 && layer.progAAPassIndex < quint32(layer.antialiasingQuality)) {
        int idx = layer.progAAPassIndex - 1;
        vertexOffsetsAA = s_ProgressiveAAVertexOffsets[idx] / QVector2D{ float(theViewport.width()/2.0), float(theViewport.height()/2.0) };
    }

    if (temporalAA) {
        const int t = 1 - 2 * (layer.tempAAPassIndex % 2);
        const float f = t * layer.temporalAAStrength;
        vertexOffsetsAA = { f / float(theViewport.width()/2.0), f / float(theViewport.height()/2.0) };
    }

    if (camera) {
        if (temporalAA || progressiveAA /*&& !vertexOffsetsAA.isNull()*/) {
            QMatrix4x4 offsetProjection = camera->projection;
            QMatrix4x4 invProjection = camera->projection.inverted();
            if (camera->type == QSSGRenderCamera::Type::OrthographicCamera) {
                offsetProjection(0, 3) -= vertexOffsetsAA.x();
                offsetProjection(1, 3) -= vertexOffsetsAA.y();
            } else if (camera->type == QSSGRenderCamera::Type::PerspectiveCamera) {
                offsetProjection(0, 2) += vertexOffsetsAA.x();
                offsetProjection(1, 2) += vertexOffsetsAA.y();
            }
            for (auto &modelContext : std::as_const(modelContexts))
                modelContext->modelViewProjection = offsetProjection * invProjection * modelContext->modelViewProjection;
        }
    }

    // Prepare passes
    QSSG_ASSERT(activePasses.isEmpty(), activePasses.clear());
    // If needed, generate a depth texture with the opaque objects. This
    // and the SSAO texture must come first since other passes may want to
    // expose these textures to their shaders.
    if (thePrepResult.flags.requiresDepthTexture())
        activePasses.push_back(&depthMapPass);

    // Screen space ambient occlusion. Relies on the depth texture and generates an AO map.
    if (thePrepResult.flags.requiresSsaoPass())
        activePasses.push_back(&ssaoMapPass);

    // Shadows. Generates a 2D or cube shadow map. (opaque + pre-pass transparent objects)
    if (thePrepResult.flags.requiresShadowMapPass())
        activePasses.push_back(&shadowMapPass);

    activePasses.push_back(&reflectionMapPass);
    activePasses.push_back(&zPrePassPass);

    // Screen texture with opaque objects.
    if (thePrepResult.flags.requiresScreenTexture())
        activePasses.push_back(&screenMapPass);

    activePasses.push_back(&mainPass);
}

void QSSGLayerRenderData::resetForFrame()
{
    for (const auto &pass : activePasses)
        pass->release();
    activePasses.clear();
    transparentObjects.clear();
    screenTextureObjects.clear();
    opaqueObjects.clear();
    bakedLightingModels.clear();
    layerPrepResult.reset();
    // The check for if the camera is or is not null is used
    // to figure out if this layer was rendered at all.
    camera = nullptr;
    cameraData.reset();
    clippingFrustum.reset();
    renderedOpaqueObjects.clear();
    renderedTransparentObjects.clear();
    renderedScreenTextureObjects.clear();
    renderedItem2Ds.clear();
    renderedOpaqueDepthPrepassObjects.clear();
    renderedDepthWriteObjects.clear();
    renderedBakedLightingModels.clear();
    renderableItem2Ds.clear();
    globalLights.clear();
    modelContexts.clear();
    features = QSSGShaderFeatures();
    plainSkyBoxPrepared = false;
}

QSSGLayerRenderPreparationResult::QSSGLayerRenderPreparationResult(const QRectF &inViewport, QSSGRenderLayer &inLayer)
    : layer(&inLayer)
{
    viewport = inViewport;
}

bool QSSGLayerRenderPreparationResult::isLayerVisible() const
{
    return viewport.height() >= 2.0f && viewport.width() >= 2.0f;
}

QSize QSSGLayerRenderPreparationResult::textureDimensions() const
{
    const auto size = viewport.size().toSize();
    return QSize(QSSGRendererUtil::nextMultipleOf4(size.width()), QSSGRendererUtil::nextMultipleOf4(size.height()));
}

QSSGCameraGlobalCalculationResult QSSGLayerRenderPreparationResult::setupCameraForRender(QSSGRenderCamera &inCamera)
{
    // When using ssaa we need to zoom with the ssaa multiplier since otherwise the
    // orthographic camera will be zoomed out due to the bigger viewport. We therefore
    // scale the magnification before calulating the camera variables and then revert.
    // Since the same camera can be used in several View3Ds with or without ssaa we
    // cannot store the magnification permanently.
    const float horizontalMagnification = inCamera.horizontalMagnification;
    const float verticalMagnification = inCamera.verticalMagnification;
    inCamera.horizontalMagnification *= layer->ssaaEnabled ? layer->ssaaMultiplier : 1.0f;
    inCamera.verticalMagnification *= layer->ssaaEnabled ? layer->ssaaMultiplier : 1.0f;
    const auto result = inCamera.calculateGlobalVariables(viewport);
    inCamera.horizontalMagnification = horizontalMagnification;
    inCamera.verticalMagnification = verticalMagnification;
    return result;
}

QSSGLayerRenderData::QSSGLayerRenderData(QSSGRenderLayer &inLayer, const QSSGRef<QSSGRenderer> &inRenderer)
    : layer(inLayer)
    , renderer(inRenderer)
{
}

QSSGLayerRenderData::~QSSGLayerRenderData()
{
    delete m_lightmapper;
    shadowMapPass.release();
    reflectionMapPass.release();
    zPrePassPass.release();
    ssaoMapPass.release();
    depthMapPass.release();
    screenMapPass.release();
    mainPass.release();
}

static void sortInstances(QByteArray &sortedData, QList<QSSGRhiSortData> &sortData, const void *instances,
                          int stride, int count, const QVector3D &cameraDirection)
{
    sortData.resize(count);
    Q_ASSERT(stride == sizeof(QSSGRenderInstanceTableEntry));
    // create sort data
    {
        const QSSGRenderInstanceTableEntry *instance = reinterpret_cast<const QSSGRenderInstanceTableEntry *>(instances);
        for (int i = 0; i < count; i++) {
            const QVector3D pos = QVector3D(instance->row0.w(), instance->row1.w(), instance->row2.w());
            sortData[i] = {QVector3D::dotProduct(pos, cameraDirection), i};
            instance++;
        }
    }

    // sort
    std::sort(sortData.begin(), sortData.end(), [](const QSSGRhiSortData &a, const QSSGRhiSortData &b){
        return a.d > b.d;
    });

    // copy instances
    {
        const QSSGRenderInstanceTableEntry *instance = reinterpret_cast<const QSSGRenderInstanceTableEntry *>(instances);
        QSSGRenderInstanceTableEntry *dest = reinterpret_cast<QSSGRenderInstanceTableEntry *>(sortedData.data());
        for (auto &s : sortData)
            *dest++ = instance[s.indexOrOffset];
    }
}

static void cullLodInstances(QByteArray &lodData, const void *instances, int count,
                             const QVector3D &cameraPosition, float minThreshold, float maxThreshold)
{
    const QSSGRenderInstanceTableEntry *instance = reinterpret_cast<const QSSGRenderInstanceTableEntry *>(instances);
    QSSGRenderInstanceTableEntry *dest = reinterpret_cast<QSSGRenderInstanceTableEntry *>(lodData.data());
    for (int i = 0; i < count; ++i) {
        const float x = cameraPosition.x() - instance->row0.w();
        const float y = cameraPosition.y() - instance->row1.w();
        const float z = cameraPosition.z() - instance->row2.w();
        const float distanceSq = x * x + y * y + z * z;
        if (distanceSq >= minThreshold * minThreshold && (maxThreshold < 0 || distanceSq < maxThreshold * maxThreshold))
            *dest = *instance;
        else
            *dest= {};
        dest++;
        instance++;
    }
}

bool QSSGSubsetRenderable::prepareInstancing(QSSGRhiContext *rhiCtx, const QVector3D &cameraDirection, const QVector3D &cameraPosition, float minThreshold, float maxThreshold)
{
    if (!modelContext.model.instancing() || instanceBuffer)
        return instanceBuffer;
    auto *table = modelContext.model.instanceTable;
    bool usesLod = minThreshold >= 0 || maxThreshold >= 0;
    QSSGRhiInstanceBufferData &instanceData(usesLod ? rhiCtx->instanceBufferData(&modelContext.model) : rhiCtx->instanceBufferData(table));
    quint32 instanceBufferSize = table->dataSize();
    // Create or resize the instance buffer ### if (instanceData.owned)
    bool sortingChanged = table->isDepthSortingEnabled() != instanceData.sorting;
    bool cameraDirectionChanged = !qFuzzyCompare(instanceData.sortedCameraDirection, cameraDirection);
    bool cameraPositionChanged = !qFuzzyCompare(instanceData.cameraPosition, cameraPosition);
    bool updateInstanceBuffer = table->serial() != instanceData.serial || sortingChanged || (cameraDirectionChanged && table->isDepthSortingEnabled());
    bool updateForLod = cameraPositionChanged && usesLod;
    if (sortingChanged && !table->isDepthSortingEnabled()) {
        instanceData.sortedData.clear();
        instanceData.sortData.clear();
        instanceData.sortedCameraDirection = {};
    }
    instanceData.sorting = table->isDepthSortingEnabled();
    if (instanceData.buffer && instanceData.buffer->size() < instanceBufferSize) {
        updateInstanceBuffer = true;
        //                    qDebug() << "Resizing instance buffer";
        instanceData.buffer->setSize(instanceBufferSize);
        instanceData.buffer->create();
    }
    if (!instanceData.buffer) {
        //                    qDebug() << "Creating instance buffer";
        updateInstanceBuffer = true;
        instanceData.buffer = rhiCtx->rhi()->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer, instanceBufferSize);
        instanceData.buffer->create();
    }
    if (updateInstanceBuffer || updateForLod) {
        const void *data = nullptr;
        if (table->isDepthSortingEnabled()) {
            if (updateInstanceBuffer) {
                QMatrix4x4 invGlobalTransform = modelContext.model.globalTransform.inverted();
                instanceData.sortedData.resize(table->dataSize());
                sortInstances(instanceData.sortedData,
                              instanceData.sortData,
                              table->constData(),
                              table->stride(),
                              table->count(),
                              invGlobalTransform.map(cameraDirection).normalized());
            }
            data = instanceData.sortedData.constData();
            instanceData.sortedCameraDirection = cameraDirection;
        } else {
            data = table->constData();
        }
        if (data) {
            if (updateForLod) {
                if (table->isDepthSortingEnabled()) {
                    instanceData.lodData.resize(table->dataSize());
                    cullLodInstances(instanceData.lodData, instanceData.sortedData.constData(), instanceData.sortedData.size(), cameraPosition, minThreshold, maxThreshold);
                    data = instanceData.lodData.constData();
                } else {
                    instanceData.lodData.resize(table->dataSize());
                    cullLodInstances(instanceData.lodData, table->constData(), table->count(), cameraPosition, minThreshold, maxThreshold);
                    data = instanceData.lodData.constData();
                }
            }
            QRhiResourceUpdateBatch *rub = rhiCtx->rhi()->nextResourceUpdateBatch();
            rub->updateDynamicBuffer(instanceData.buffer, 0, instanceBufferSize, data);
            rhiCtx->commandBuffer()->resourceUpdate(rub);
            //qDebug() << "****** UPDATING INST BUFFER. Size" << instanceBufferSize;
        } else {
            qWarning() << "NO DATA IN INSTANCE TABLE";
        }
        instanceData.serial = table->serial();
        instanceData.cameraPosition = cameraPosition;
    }
    instanceBuffer = instanceData.buffer;
    return instanceBuffer;
}

void QSSGLayerRenderData::maybeBakeLightmap()
{
    if (!interactiveLightmapBakingRequested) {
        static bool bakeRequested = false;
        static bool bakeFlagChecked = false;
        if (!bakeFlagChecked) {
            bakeFlagChecked = true;
            const bool cmdLineReq = QCoreApplication::arguments().contains(QStringLiteral("--bake-lightmaps"));
            const bool envReq = qEnvironmentVariableIntValue("QT_QUICK3D_BAKE_LIGHTMAPS");
            bakeRequested = cmdLineReq || envReq;
        }
        if (!bakeRequested)
            return;
    }

    const auto &sortedBakedLightingModels = getSortedBakedLightingModels(); // front to back
    if (sortedBakedLightingModels.isEmpty())
        return;

    QSSGRhiContext *rhiCtx = renderer->contextInterface()->rhiContext().data();

    if (!m_lightmapper)
        m_lightmapper = new QSSGLightmapper(rhiCtx, renderer.data());

    // sortedBakedLightingModels contains all models with
    // usedInBakedLighting: true. These, together with lights that
    // have a bakeMode set to either Indirect or All, form the
    // lightmapped scene. A lightmap is stored persistently only
    // for models that have their lightmapKey set.

    m_lightmapper->reset();
    m_lightmapper->setOptions(layer.lmOptions);
    m_lightmapper->setOutputCallback(lightmapBakingOutputCallback);

    for (int i = 0, ie = sortedBakedLightingModels.size(); i != ie; ++i)
        m_lightmapper->add(sortedBakedLightingModels[i]);

    QRhiCommandBuffer *cb = rhiCtx->commandBuffer();
    cb->debugMarkBegin("Quick3D lightmap baking");
    m_lightmapper->bake();
    cb->debugMarkEnd();

    if (!interactiveLightmapBakingRequested) {
        qDebug("Lightmap baking done, exiting application");
        QMetaObject::invokeMethod(qApp, "quit");
    }

    interactiveLightmapBakingRequested = false;
}

QT_END_NAMESPACE
