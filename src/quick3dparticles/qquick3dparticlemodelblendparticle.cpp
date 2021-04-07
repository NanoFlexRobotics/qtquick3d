/****************************************************************************
**
** Copyright (C) 2021 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of Qt Quick 3D.
**
** $QT_BEGIN_LICENSE:GPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3 or (at your option) any later version
** approved by the KDE Free Qt Foundation. The licenses are as published by
** the Free Software Foundation and appearing in the file LICENSE.GPL3
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-3.0.html.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "qquick3dparticlemodelblendparticle_p.h"
#include "qquick3dparticleemitter_p.h"

#include <QtCore/qdir.h>
#include <QtQml/qqmlfile.h>

#include <QtQuick3D/private/qquick3dobject_p.h>
#include <QtQuick3D/private/qquick3dgeometry_p.h>

#include <QtQuick3DUtils/private/qssgutils_p.h>
#include <QtQuick3DRuntimeRender/private/qssgrenderparticles_p.h>
#include <QtQuick3DRuntimeRender/private/qssgrendergeometry_p.h>
#include <QtQuick3DRuntimeRender/private/qssgrendermodel_p.h>
#include <QtQuick3DUtils/private/qssgmesh_p.h>

QT_BEGIN_NAMESPACE

/*!
    \qmltype ModelBlendParticle3D
    \inherits Particle3D
    \inqmlmodule QtQuick3D.Particles3D
    \brief Blends particle effect with a 3D model
    \preliminary

    \note This type is in tech preview in 6.2. \b {The API is under development and subject to change.}

    The type provides a way to blend particle effect with a 3D model. The provided model needs to be
    triangle-based. Each triangle in the model is converted into a particle, which are then used by
    the emitter. Instead of particle shader, the model is shaded using the \l {Model::materials}{material}
    specified in the model. The way the effect is blended is determined by the \l modelBlendMode.

    The possible modes are:
    \list
    \li \b Construct, where the model gets constructed from the particles.
    \li \b Explode, where the model gets converted into particles.
    \li \b Transfer, where Construct and Explode are combined to create an effect where the model is
    transferred from one place to another.
    \endlist

    Some features defined in base class and emitters are not functional with this type:
    \list
    \li \l Particle3D::alignMode is not functional since the particles can be in arbitrary orientation
    in the model.
     \li\l Particle3D::sortMode is not functional since the particles are always rendered in the order
    the primitives are specified in the model.
    \li \l ParticleEmitter3D::depthBias is not functional since the model depth bias is used instead.
    \endlist
*/

QQuick3DParticleModelBlendParticle::QQuick3DParticleModelBlendParticle(QQuick3DNode *parent)
    : QQuick3DParticle(*new QQuick3DObjectPrivate(QQuick3DObjectPrivate::Type::ModelBlendParticle), parent)
{

}

QQuick3DParticleModelBlendParticle::~QQuick3DParticleModelBlendParticle()
{
    delete m_model;
    delete m_modelGeometry;
}

/*!
    \qmlproperty Component ModelBlendParticle3D::delegate
    \preliminary
    The delegate provides a template defining the model for the ModelBlendParticle3D.

    For example, using the default sphere model with default material

    \qml
    Component {
        id: modelComponent
        Model {
            source: "#Sphere"
            scale: Qt.vector3d(0.5, 0.5, 0.5)
            materials: DefaultMaterial { diffuseColor: "red" }
        }
    }

    ModelBlendParticle3D {
        id: particleRedSphere
        delegate: modelComponent
    }
    \endqml
*/
QQmlComponent *QQuick3DParticleModelBlendParticle::delegate() const
{
    return m_delegate;
}

void QQuick3DParticleModelBlendParticle::setDelegate(QQmlComponent *delegate)
{
    if (delegate == m_delegate)
        return;
    m_delegate = delegate;

    reset();
    regenerate();
    Q_EMIT delegateChanged();
}

/*!
    \qmlproperty Node ModelBlendParticle3D::endNode
    \preliminary
    This property holds the node that specifies the transformation for the model at the end
    of particle effect. It defines the size, position and rotation where the model is constructed
    when using the \c ModelBlendParticle3D.Construct and \c ModelBlendParticle3D.Transfer blend modes.
*/
QQuick3DNode *QQuick3DParticleModelBlendParticle::endNode() const
{
    return m_endNode;
}

/*!
    \qmlproperty enumeration ModelBlendParticle3D::ModelBlendMode
    \preliminary
    Defines the blending mode for the particle effect.

    \value ModelBlendParticle3D.Explode
        The model gets exploded i.e. the particles are emitted from the position of the model.
    \value ModelBlendParticle3D.Construct
        The model gets constructed i.e the particles fly from the emitter and construct the model at the end.
    \value ModelBlendParticle3D.Transfer
        Combines Explode and Transfer for the same model.
*/
/*!
    \qmlproperty ModelBlendMode ModelBlendParticle3D::modelBlendMode
    \preliminary
    This property holds blending mode for the particle effect.
*/
QQuick3DParticleModelBlendParticle::ModelBlendMode QQuick3DParticleModelBlendParticle::modelBlendMode() const
{
    return m_modelBlendMode;
}

/*!
    \qmlproperty int ModelBlendParticle3D::endTime
    \preliminary
    This property holds the end time of the particle in milliseconds. The end time is used during construction
    and defines duration from particle lifetime at the end where the effect is blended with
    the model positions. Before the end time the particles positions are defined only by the
    particle effect, but during end time the particle position is blended linearly with the model
    end position.
*/
int QQuick3DParticleModelBlendParticle::endTime() const
{
    return m_endTime;
}

void QQuick3DParticleModelBlendParticle::setEndNode(QQuick3DNode *node)
{
    if (m_endNode == node)
        return;

    m_endNode = node;
    if (node) {
        m_endNodePosition = m_endNode->position();
        m_endNodeRotation = m_endNode->rotation().toEulerAngles();
        m_endNodeScale = m_endNode->scale();
    }
    Q_EMIT endNodeChanged();
}

void QQuick3DParticleModelBlendParticle::setModelBlendMode(ModelBlendMode mode)
{
    if (m_modelBlendMode == mode)
        return;
    m_modelBlendMode = mode;
    reset();
    Q_EMIT modelBlendModeChanged();
}

void QQuick3DParticleModelBlendParticle::setEndTime(int endTime)
{
    if (endTime == m_endTime)
        return;
    m_endTime = endTime;
    Q_EMIT endTimeChanged();
}

void QQuick3DParticleModelBlendParticle::regenerate()
{
    delete m_model;
    m_model = nullptr;

    if (!isComponentComplete())
        return;

    if (!m_delegate)
        return;

    auto *obj = m_delegate->create(m_delegate->creationContext());

    m_model = qobject_cast<QQuick3DModel *>(obj);
    if (m_model) {
        updateParticles();
        auto *psystem = QQuick3DParticle::system();
        m_model->setParent(psystem);
        m_model->setParentItem(psystem);
    } else {
        delete obj;
    }
    if (m_endNode) {
        m_endNodePosition = m_endNode->position();
        m_endNodeRotation = m_endNode->rotation().toEulerAngles();
        m_endNodeScale = m_endNode->scale();
    }
}

static QSSGMesh::Mesh loadMesh(const QString &source)
{
    QString src = source;
    if (source.startsWith(QLatin1Char('#'))) {
        src = QSSGBufferManager::primitivePath(source);
        src.prepend(QLatin1String(":/"));
    }
    src = QDir::cleanPath(src);
    if (src.startsWith(QLatin1String("qrc:/")))
        src = src.mid(3);
    QSSGMesh::Mesh mesh;
    QFileInfo fileInfo = QFileInfo(src);
    if (fileInfo.exists()) {
        QFile file(fileInfo.absoluteFilePath());
        if (!file.open(QFile::ReadOnly))
            return {};
        mesh = QSSGMesh::Mesh::loadMesh(&file);
    }
    return mesh;
}

static QVector3D getPosition(const quint8 *srcVertices, quint32 idx, quint32 vertexStride, quint32 posOffset)
{
    const quint8 *vertex = srcVertices + idx * vertexStride;
    return *reinterpret_cast<const QVector3D *>(vertex + posOffset);
}

static void copyToUnindexedVertices(QByteArray &unindexedVertexData,
                                    QVector<QVector3D> &centerData,
                                    const QByteArray &vertexBufferData,
                                    quint32 vertexStride,
                                    quint32 posOffset,
                                    const QByteArray &indexBufferData,
                                    QSSGMesh::Mesh::ComponentType indexBufferComponentType,
                                    quint32 primitiveCount)
{
    const quint8 *srcVertices = reinterpret_cast<const quint8 *>(vertexBufferData.data());
    quint8 *dst = reinterpret_cast<quint8 *>(unindexedVertexData.data());
    const quint16 *indexData16 = reinterpret_cast<const quint16 *>(indexBufferData.begin());
    const quint32 *indexData32 = reinterpret_cast<const quint32 *>(indexBufferData.begin());
    const float c_div3 = 1.0f / 3.0f;
    for (quint32 i = 0; i < primitiveCount; i++) {
        quint32 i0, i1, i2;
        if (indexBufferComponentType == QSSGMesh::Mesh::ComponentType::UnsignedInt16) {
            i0 = indexData16[3 * i];
            i1 = indexData16[3 * i + 1];
            i2 = indexData16[3 * i + 2];
        } else {
            i0 = indexData32[3 * i];
            i1 = indexData32[3 * i + 1];
            i2 = indexData32[3 * i + 2];
        }
        QVector3D p0 = getPosition(srcVertices, i0, vertexStride, posOffset);
        QVector3D p1 = getPosition(srcVertices, i1, vertexStride, posOffset);
        QVector3D p2 = getPosition(srcVertices, i2, vertexStride, posOffset);
        QVector3D center = (p0 + p1 + p2) * c_div3;
        centerData[i] = center;
        memcpy(dst, srcVertices + i0 * vertexStride, vertexStride);
        dst += vertexStride;
        memcpy(dst, srcVertices + i1 * vertexStride, vertexStride);
        dst += vertexStride;
        memcpy(dst, srcVertices + i2 * vertexStride, vertexStride);
        dst += vertexStride;
    }
}

static void getVertexCenterData(QVector<QVector3D> &centerData,
                                const QByteArray &vertexBufferData,
                                quint32 vertexStride,
                                quint32 posOffset,
                                quint32 primitiveCount)
{
    const quint8 *srcVertices = reinterpret_cast<const quint8 *>(vertexBufferData.data());
    const float c_div3 = 1.0f / 3.0f;
    for (quint32 i = 0; i < primitiveCount; i++) {
        QVector3D p0 = getPosition(srcVertices, 3 * i, vertexStride, posOffset);
        QVector3D p1 = getPosition(srcVertices, 3 * i + 1, vertexStride, posOffset);
        QVector3D p2 = getPosition(srcVertices, 3 * i + 2, vertexStride, posOffset);
        QVector3D center = (p0 + p1 + p2) * c_div3;
        centerData[i] = center;
    }
}

void QQuick3DParticleModelBlendParticle::updateParticles()
{
    // The primitives needs to be triangle list without indexing, because each triangle
    // needs to be it's own primitive and we need vertex index to get the particle index,
    // which we don't get with indexed primitives
    if (!m_model->geometry()) {
        const QQmlContext *context = qmlContext(this);
        QString src = m_model->source().toString();
        if (context && !src.startsWith(QLatin1Char('#')))
            src = QQmlFile::urlToLocalFileOrQrc(context->resolvedUrl(m_model->source()));
        QSSGMesh::Mesh mesh = loadMesh(src);
        if (!mesh.isValid())
            return;
        if (mesh.drawMode() != QSSGMesh::Mesh::DrawMode::Triangles)
            return;

        m_modelGeometry = new QQuick3DGeometry;

        const auto vertexBuffer = mesh.vertexBuffer();
        const auto indexBuffer = mesh.indexBuffer();

        const auto entryOffset = [&](const QSSGMesh::Mesh::VertexBuffer &vb, const QByteArray &name) -> int {
            for (const auto &e : vb.entries) {
                if (e.name == name) {
                    Q_ASSERT(e.componentType == QSSGMesh::Mesh::ComponentType::Float32);
                    return e.offset;
                }
            }
            Q_ASSERT(0);
            return -1;
        };
        const auto toAttribute = [&](const QSSGMesh::Mesh::VertexBufferEntry &e) -> QQuick3DGeometry::Attribute {
            QQuick3DGeometry::Attribute a;
            a.componentType = QQuick3DGeometryPrivate::toComponentType(e.componentType);
            a.offset = e.offset;
            a.semantic = QQuick3DGeometryPrivate::semanticFromName(e.name);
            return a;
        };

        const auto indexedPrimitiveCount = [&](const QSSGMesh::Mesh::IndexBuffer &indexBuffer) -> quint32 {
            if (indexBuffer.componentType == QSSGMesh::Mesh::ComponentType::UnsignedInt16)
                return quint32(indexBuffer.data.size() / sizeof(quint16) / 3);
            return quint32(indexBuffer.data.size() / sizeof(quint32) / 3);
        };

        if (indexBuffer.data.size()) {
            // deindex data
            QByteArray unindexedVertexData;
            quint32 primitiveCount = indexedPrimitiveCount(indexBuffer);
            unindexedVertexData.resize(vertexBuffer.stride * primitiveCount * 3);
            m_centerData.resize(primitiveCount);
            m_particleCount = primitiveCount;
            copyToUnindexedVertices(unindexedVertexData, m_centerData, vertexBuffer.data, vertexBuffer.stride, entryOffset(vertexBuffer, QByteArray(QSSGMesh::MeshInternal::getPositionAttrName())), indexBuffer.data, indexBuffer.componentType, primitiveCount);
            m_modelGeometry->setBounds(mesh.subsets().first().bounds.min, mesh.subsets().first().bounds.max);
            m_modelGeometry->setStride(vertexBuffer.stride);
            m_modelGeometry->setVertexData(unindexedVertexData);
            m_modelGeometry->setPrimitiveType(QQuick3DGeometry::PrimitiveType::Triangles);
        } else {
            // can use vertexbuffer directly
            quint32 primitiveCount = vertexBuffer.data.size() / vertexBuffer.stride / 3;
            m_centerData.resize(primitiveCount);
            m_particleCount = primitiveCount;
            getVertexCenterData(m_centerData, vertexBuffer.data, vertexBuffer.stride, entryOffset(vertexBuffer, QByteArray(QSSGMesh::MeshInternal::getPositionAttrName())), primitiveCount);
            m_modelGeometry->setBounds(mesh.subsets().first().bounds.min, mesh.subsets().first().bounds.max);
            m_modelGeometry->setStride(vertexBuffer.stride);
            m_modelGeometry->setVertexData(vertexBuffer.data);
            m_modelGeometry->setPrimitiveType(QQuick3DGeometry::PrimitiveType::Triangles);
        }

        QMatrix4x4 transform = m_model->sceneTransform();
        if (m_model->parentNode())
            transform = m_model->parentNode()->sceneTransform().inverted() * transform;
        m_triangleParticleData.resize(m_particleCount);
        m_particleData.resize(m_particleCount);
        m_particleData.fill({});
        for (int i = 0; i < m_particleCount; i++) {
            m_triangleParticleData[i].center = m_centerData[i];
            m_centerData[i] = transform * m_centerData[i];
            if (m_modelBlendMode == Construct) {
                m_triangleParticleData[i].size = 0.0f;
            } else {
                m_triangleParticleData[i].size = 1.0f;
                m_triangleParticleData[i].position = m_centerData[i];
            }
        }
        QQuick3DParticle::doSetMaxAmount(m_particleCount);

        for (auto &e : vertexBuffer.entries)
            m_modelGeometry->addAttribute(toAttribute(e));

        m_model->setSource({});
        m_model->setGeometry(m_modelGeometry);
    }
}

QSSGRenderGraphObject *QQuick3DParticleModelBlendParticle::updateSpatialNode(QSSGRenderGraphObject *node)
{
    auto *spatialNode = QQuick3DObjectPrivate::get(m_model)->spatialNode;
    if (spatialNode) {
        QSSGRenderModel *model = static_cast<QSSGRenderModel *>(spatialNode);

        if (!model->particleBuffer) {
            QSSGParticleBuffer *buffer = model->particleBuffer = new QSSGParticleBuffer;
            buffer->resize(m_particleCount, sizeof(QSSGTriangleParticle));
        }
        QQuick3DParticleSystem *psystem = QQuick3DParticle::system();
        QMatrix4x4 particleMatrix = psystem->sceneTransform().inverted() * m_model->sceneTransform();
        model->particleMatrix = particleMatrix.inverted();
        updateParticleBuffer(model->particleBuffer);
    }
    return node;
}

void QQuick3DParticleModelBlendParticle::componentComplete()
{
    if (!system() && qobject_cast<QQuick3DParticleSystem *>(parentItem()))
        setSystem(qobject_cast<QQuick3DParticleSystem *>(parentItem()));

    // don't call particles componentComplete, we don't wan't to emit maxAmountChanged yet
    QQuick3DObject::componentComplete();
    regenerate();
}

void QQuick3DParticleModelBlendParticle::doSetMaxAmount(int)
{
    qWarning() << "ModelBlendParticle3D.maxAmount: Unable to set maximum amount, because it is set from the model.";
    return;
}

int QQuick3DParticleModelBlendParticle::nextCurrentIndex(const QQuick3DParticleEmitter *emitter)
{
    if (!m_perEmitterData.contains(emitter)) {
        m_perEmitterData.insert(emitter, PerEmitterData());
        auto &perEmitter = m_perEmitterData[emitter];
        perEmitter.emitter = emitter;
        perEmitter.emitterIndex = m_nextEmitterIndex++;
    }
    auto &perEmitter = m_perEmitterData[emitter];
    int index = QQuick3DParticle::nextCurrentIndex(emitter);
    if (m_triangleParticleData[index].emitterIndex != perEmitter.emitterIndex) {
        if (m_triangleParticleData[index].emitterIndex >= 0)
            perEmitterData(m_triangleParticleData[index].emitterIndex).particleCount--;
        perEmitter.particleCount++;
    }
    m_triangleParticleData[index].emitterIndex = perEmitter.emitterIndex;
    return index;
}


void QQuick3DParticleModelBlendParticle::setParticleData(int particleIndex,
                                                         const QVector3D &position,
                                                         const QVector3D &rotation,
                                                         const QVector4D &color,
                                                         float size, float age)
{
    auto &dst = m_triangleParticleData[particleIndex];
    dst = {position, rotation, dst.center, color, age, size, dst.emitterIndex};
}

QQuick3DParticleModelBlendParticle::PerEmitterData &QQuick3DParticleModelBlendParticle::perEmitterData(int emitterIndex)
{
    for (auto &perEmitter : m_perEmitterData) {
        if (perEmitter.emitterIndex == emitterIndex)
            return perEmitter;
    }
    return n_noPerEmitterData;
}

void QQuick3DParticleModelBlendParticle::updateParticleBuffer(QSSGParticleBuffer *buffer)
{
    const auto &particles = m_triangleParticleData;

    if (!buffer)
        return;

    const int particleCount = m_particleCount;

    char *dest = buffer->pointer();
    const TriangleParticleData *src = particles.data();
    const int pps = buffer->particlesPerSlice();
    const int ss = buffer->sliceStride();
    const int slices = buffer->sliceCount();
    const float c_degToRad = float(M_PI / 180.0f);
    int i = 0;
    QSSGBounds3 bounds;
    for (int s = 0; s < slices; s++) {
        QSSGTriangleParticle *dp = reinterpret_cast<QSSGTriangleParticle *>(dest);
        for (int p = 0; p < pps && i < particleCount; ) {
            if (src->size > 0.0f)
                bounds.include(src->position);
            dp->position = src->position;
            dp->rotation = src->rotation * c_degToRad;
            dp->color = src->color;
            dp->age = src->age;
            dp->center = src->center;
            dp->size = src->size;
            dp++;
            p++;
            i++;
            src++;
        }
        dest += ss;
    }
    buffer->setBounds(bounds);
}

void QQuick3DParticleModelBlendParticle::itemChange(QQuick3DObject::ItemChange change,
                                                    const QQuick3DObject::ItemChangeData &value)
{
    QQuick3DObject::itemChange(change, value);
    if (change == ItemParentHasChanged && value.sceneManager)
        regenerate();
}

void QQuick3DParticleModelBlendParticle::reset()
{
    QQuick3DParticle::reset();
    if (m_particleCount) {
        for (int i = 0; i < m_particleCount; i++) {
            if (m_modelBlendMode == Construct) {
                m_triangleParticleData[i].size = 0.0f;
            } else {
                m_triangleParticleData[i].size = 1.0f;
                m_triangleParticleData[i].position = m_triangleParticleData[i].center;
            }
        }
    }
}

QVector3D QQuick3DParticleModelBlendParticle::particleCenter(int particleIndex) const
{
    return m_centerData[particleIndex];
}

bool QQuick3DParticleModelBlendParticle::lastParticle() const
{
    return m_currentIndex >= m_maxAmount - 1;
}

QVector3D QQuick3DParticleModelBlendParticle::particleEndPosition(int idx) const
{
    return m_endNodeScale * m_centerData[idx] + m_endNodePosition;
}

QVector3D QQuick3DParticleModelBlendParticle::particleEndRotation(int) const
{
    return m_endNodeRotation;
}

QT_END_NAMESPACE
