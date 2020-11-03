/* -*-c++-*- */
/* osgEarth - Geospatial SDK for OpenSceneGraph
 * Copyright 2020 Pelican Mapping
 * http://osgearth.org
 *
 * osgEarth is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>
 */

#include <osgEarth/InstanceBuilder>
#include <osgEarth/VirtualProgram>
#include <osgEarth/Shaders>
#include <osgEarth/Registry>
#include <osgEarth/Capabilities>
#include <osg/GL>
#include <osg/VertexAttribDivisor>
#include <osg/Geometry>

using namespace osgEarth;

#define POSITION_ATTRIB 9
#define ROTATION_ATTRIB 10
#define SCALE_ATTRIB 11

// OSG in GLCORE mode doesn't set the vertex attribute divisor
// correctly, so here's a big old hack to get around that.
#if OSG_VERSION_GREATER_OR_EQUAL(3,6,0)
class InstancedGeometry : public osg::Geometry
{
public:
    InstancedGeometry();
    InstancedGeometry(const InstancedGeometry& geometry,const osg::CopyOp& copyop=osg::CopyOp::SHALLOW_COPY);

    META_Node(osgEarth, InstancedGeometry);
    
    virtual void compileGLObjects(osg::RenderInfo& renderInfo) const;
    void setVertexAttribDivisor(unsigned int index, unsigned int divisor)
    {
        if (index >= _divisors.size())
        {
            _divisors.resize(index + 1);
        }
        _divisors[index] = divisor;
        dirtyGLObjects();
    }
    unsigned int getVertexAttribDivisor(unsigned int index)
    {
        if (index < _divisors.size())
        {
            return _divisors[index];
        }
        else
        {
            return 0;
        }
    }
protected:
    std::vector<unsigned int> _divisors;
};
#endif

namespace
{
    // Funky manoeuvres for calculating the bounding box
    struct InstanceBoundingBoxCallback : public osg::Geometry::ComputeBoundingBoxCallback
    {
        osg::BoundingBox computeBound(const osg::Drawable& drawable) const
        {
            const osg::Geometry* geom = static_cast<const osg::Geometry*>(&drawable);
            osg::BoundingBox initBB = geom->computeBoundingBox();
            const osg::Vec3Array* pos = dynamic_cast<const osg::Vec3Array*>(geom->getVertexAttribArray(POSITION_ATTRIB));
            const osg::Vec4Array* rot = dynamic_cast<const osg::Vec4Array*>(geom->getVertexAttribArray(ROTATION_ATTRIB));
            const osg::Vec3Array* scale = dynamic_cast<const osg::Vec3Array*>(geom->getVertexAttribArray(SCALE_ATTRIB));
            osg::BoundingBox resultBB;
            // XXX
            int numInstances = 0;
            if (geom->getNumPrimitiveSets() > 0)
            {
                numInstances = geom->getPrimitiveSet(0)->getNumInstances();
            }
            for (int i = 0; i < numInstances; ++i)
            {
                osg::Matrixf instanceTransform;
                if (pos)
                {
                    instanceTransform.makeTranslate((*pos)[i]);
                }
                if (rot)
                {
                    if (rot->getBinding() == osg::Array::BIND_PER_VERTEX)
                    {
                        instanceTransform = osg::Matrixf::rotate(osg::Quat((*rot)[i])) * instanceTransform;
                    }
                    else
                    {
                        instanceTransform = osg::Matrixf::rotate(osg::Quat((*rot)[0])) * instanceTransform;
                    }
                }
                if (scale)
                {
                    if (scale->getBinding() == osg::Array::BIND_PER_VERTEX)
                    {
                        instanceTransform = osg::Matrixf::scale((*scale)[i]) * instanceTransform;
                    }
                    else
                    {
                        instanceTransform = osg::Matrixf::scale((*scale)[0]) * instanceTransform;
                    }
                }
                for (int j = 0; j < 8; ++j)
                {
                    resultBB.expandBy(initBB.corner(i) * instanceTransform);
                }
                
            }
            return resultBB;
        }
    };

    void setPerVertexOrOverall(osg::Geometry*geom, osg::Array* perVertex, osg::Array* overall, unsigned int index)
    {
        if (perVertex)
        {
            geom->setVertexAttribArray(index, perVertex, osg::Array::BIND_PER_VERTEX);
#if OSG_VERSION_GREATER_OR_EQUAL(3,6,0)
            InstancedGeometry* instancedGeom = dynamic_cast<InstancedGeometry*>(geom);
            if (instancedGeom && instancedGeom->getUseVertexArrayObject())
            {
                instancedGeom->setVertexAttribDivisor(index, 1);
            }
            else
#endif
            {
                osg::StateSet* ss = geom->getOrCreateStateSet();
                ss->setAttribute(new osg::VertexAttribDivisor(index, 1));
            }
        }
        else
        {
            geom->setVertexAttribArray(index, overall, osg::Array::BIND_OVERALL);
        }
    }
}

#if OSG_VERSION_GREATER_OR_EQUAL(3,6,0)
InstancedGeometry::InstancedGeometry()
{
    setUseVertexArrayObject(osgEarth::Registry::capabilities().supportsVertexArrayObjects());
}

InstancedGeometry::InstancedGeometry(const InstancedGeometry& geometry,const osg::CopyOp& copyop)
    : Geometry(geometry, copyop),
      _divisors(geometry._divisors.begin(), geometry._divisors.end())
{
}

void InstancedGeometry::compileGLObjects(osg::RenderInfo& renderInfo) const
{
    Geometry::compileGLObjects(renderInfo);
    osg::State& state = *renderInfo.getState();
    bool useVertexArrayObject = state.useVertexArrayObject(_useVertexArrayObject);
    if (useVertexArrayObject)
    {
        osg::VertexArrayState* vas = _vertexArrayStateList[renderInfo.getContextID()].get();
        if (!vas)
        {
            return;
        }
        osg::State::SetCurrentVertexArrayStateProxy setVASProxy(state, vas);
        state.bindVertexArrayObject(vas);
        const osg::GLExtensions* extensions = state.get<osg::GLExtensions>();
        if (extensions->glVertexAttribDivisor)
        {
            for (int i = 0; i < _divisors.size(); ++i)
            {
                extensions->glVertexAttribDivisor(i, _divisors[i]);
            }
        }
        state.unbindVertexArrayObject();
    }
}
#endif

InstanceBuilder::InstanceBuilder()
{
    osg::Vec3 position(0.0f, 0.0f, 0.0f);
    osg::Vec4 rotation(0.0f, 0.0f, 0.0f, 1.0f);
    osg::Vec3 scale(1.0f, 1.0f, 1.0f);

    _position = new osg::Vec3Array(1,&position);
    _rotation = new osg::Vec4Array(1, &rotation);
    _scale = new osg::Vec3Array(1, &scale);
}

osg::Geometry* InstanceBuilder::createGeometry()
{
#if OSG_VERSION_GREATER_OR_EQUAL(3,6,0)
    return new InstancedGeometry;
#else
    return new osg::Geometry;
#endif
}

void InstanceBuilder::installInstancing(osg::Geometry* geometry) const
{
    // XXX do something more clever
    int numInstances = _positions->getNumElements();
    osg::StateSet* ss = geometry->getOrCreateStateSet();
    // assign the instance parameters
    setPerVertexOrOverall(geometry, _positions.get(), _position.get(), POSITION_ATTRIB);
    setPerVertexOrOverall(geometry, _rotations.get(), _rotation.get(), ROTATION_ATTRIB);
    setPerVertexOrOverall(geometry, _scales.get(), _scale.get(), SCALE_ATTRIB);
    osg::Geometry::PrimitiveSetList& prims = geometry->getPrimitiveSetList();
    for (osg::Geometry::PrimitiveSetList::iterator it = prims.begin(), end = prims.end();
         it != end;
        ++it)
    {
        (*it)->setNumInstances(numInstances);
    }
    VirtualProgram* vp = VirtualProgram::getOrCreate(ss);
    vp->setName("DrawInstancedAttribute");
    osgEarth::Shaders pkg;
    pkg.load(vp, pkg.DrawInstancedAttribute);
    vp->addBindAttribLocation("oe_DrawInstancedAttribute_position", POSITION_ATTRIB);
    vp->addBindAttribLocation("oe_DrawInstancedAttribute_rotation", ROTATION_ATTRIB);
    vp->addBindAttribLocation("oe_DrawInstancedAttribute_scale", SCALE_ATTRIB);
    geometry->setComputeBoundingBoxCallback(new InstanceBoundingBoxCallback);
}
