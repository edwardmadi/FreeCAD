// SPDX-License-Identifier: LGPL-2.1-or-later
/****************************************************************************
 *                                                                          *
 *   Copyright (c) 2022 Zheng, Lei <realthunder.dev@gmail.com>              *
 *   Copyright (c) 2023 FreeCAD Project Association                         *
 *                                                                          *
 *   This file is part of FreeCAD.                                          *
 *                                                                          *
 *   FreeCAD is free software: you can redistribute it and/or modify it     *
 *   under the terms of the GNU Lesser General Public License as            *
 *   published by the Free Software Foundation, either version 2.1 of the   *
 *   License, or (at your option) any later version.                        *
 *                                                                          *
 *   FreeCAD is distributed in the hope that it will be useful, but         *
 *   WITHOUT ANY WARRANTY; without even the implied warranty of             *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU       *
 *   Lesser General Public License for more details.                        *
 *                                                                          *
 *   You should have received a copy of the GNU Lesser General Public       *
 *   License along with FreeCAD. If not, see                                *
 *   <https://www.gnu.org/licenses/>.                                       *
 *                                                                          *
 ***************************************************************************/


#include "PreCompiled.h"
#ifndef _PreComp_

#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepFill_Generator.hxx>
#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <Precision.hxx>
#include <ShapeFix_Shape.hxx>
#include <ShapeFix_ShapeTolerance.hxx>
#include <ShapeUpgrade_ShellSewing.hxx>
#include <gp_Pln.hxx>

#endif

#include "TopoShape.h"
#include "TopoShapeCache.h"
#include "TopoShapeOpCode.h"
#include "FaceMaker.h"


FC_LOG_LEVEL_INIT("TopoShape", true, true)  // NOLINT

namespace Part
{

void TopoShape::initCache(int reset) const
{
    if (reset > 0 || !_cache || _cache->isTouched(_Shape)) {
        if (_parentCache) {
            _parentCache.reset();
            _subLocation.Identity();
        }
        _cache = std::make_shared<TopoShapeCache>(_Shape);
    }
}

void TopoShape::setShape(const TopoDS_Shape& shape, bool resetElementMap)
{
    if (resetElementMap) {
        this->resetElementMap();
    }
    else if (_cache && _cache->isTouched(shape)) {
        this->flushElementMap();
    }
    //_Shape._Shape = shape; // TODO: Replace the next line with this once ShapeProtector is
    // available.
    _Shape = shape;
    if (_cache) {
        initCache();
    }
}


TopoDS_Shape& TopoShape::move(TopoDS_Shape& tds, const TopLoc_Location& location)
{
#if OCC_VERSION_HEX < 0x070600
    tds.Move(location);
#else
    tds.Move(location, false);
#endif
    return tds;
}

TopoDS_Shape TopoShape::moved(const TopoDS_Shape& tds, const TopLoc_Location& location)
{
#if OCC_VERSION_HEX < 0x070600
    return tds.Moved(location);
#else
    return tds.Moved(location, false);
#endif
}

TopoDS_Shape& TopoShape::move(TopoDS_Shape& tds, const gp_Trsf& transfer)
{
#if OCC_VERSION_HEX < 0x070600
    static constexpr double scalePrecision {1e-14};
    if (std::abs(transfer.ScaleFactor()) > scalePrecision)
#else
    if (std::abs(transfer.ScaleFactor()) > TopLoc_Location::ScalePrec())
#endif
    {
        auto transferCopy(transfer);
        transferCopy.SetScaleFactor(1.0);
        tds.Move(transferCopy);
    }
    else {
        tds.Move(transfer);
    }
    return tds;
}

TopoDS_Shape TopoShape::moved(const TopoDS_Shape& tds, const gp_Trsf& transfer)
{
    TopoDS_Shape sCopy(tds);
    return move(sCopy, transfer);
}

TopoDS_Shape& TopoShape::locate(TopoDS_Shape& tds, const TopLoc_Location& loc)
{
    tds.Location(TopLoc_Location());
    return move(tds, loc);
}

TopoDS_Shape TopoShape::located(const TopoDS_Shape& tds, const TopLoc_Location& loc)
{
    auto sCopy(tds);
    sCopy.Location(TopLoc_Location());
    return moved(sCopy, loc);
}

TopoDS_Shape& TopoShape::locate(TopoDS_Shape& tds, const gp_Trsf& transfer)
{
    tds.Location(TopLoc_Location());
    return move(tds, transfer);
}

TopoDS_Shape TopoShape::located(const TopoDS_Shape& tds, const gp_Trsf& transfer)
{
    auto sCopy(tds);
    sCopy.Location(TopLoc_Location());
    return moved(sCopy, transfer);
}


int TopoShape::findShape(const TopoDS_Shape& subshape) const
{
    initCache();
    return _cache->findShape(_Shape, subshape);
}


TopoDS_Shape TopoShape::findShape(const char* name) const
{
    if (!name) {
        return {};
    }

    Data::MappedElement res = getElementName(name);
    if (!res.index) {
        return {};
    }

    auto idx = shapeTypeAndIndex(name);
    if (idx.second == 0) {
        return {};
    }
    initCache();
    return _cache->findShape(_Shape, idx.first, idx.second);
}

TopoDS_Shape TopoShape::findShape(TopAbs_ShapeEnum type, int idx) const
{
    initCache();
    return _cache->findShape(_Shape, type, idx);
}

int TopoShape::findAncestor(const TopoDS_Shape& subshape, TopAbs_ShapeEnum type) const
{
    initCache();
    return _cache->findShape(_Shape, _cache->findAncestor(_Shape, subshape, type));
}

TopoDS_Shape TopoShape::findAncestorShape(const TopoDS_Shape& subshape, TopAbs_ShapeEnum type) const
{
    initCache();
    return _cache->findAncestor(_Shape, subshape, type);
}

std::vector<int> TopoShape::findAncestors(const TopoDS_Shape& subshape, TopAbs_ShapeEnum type) const
{
    const auto& shapes = findAncestorsShapes(subshape, type);
    std::vector<int> ret;
    ret.reserve(shapes.size());
    for (const auto& shape : shapes) {
        ret.push_back(findShape(shape));
    }
    return ret;
}

std::vector<TopoDS_Shape> TopoShape::findAncestorsShapes(const TopoDS_Shape& subshape,
                                                         TopAbs_ShapeEnum type) const
{
    initCache();
    std::vector<TopoDS_Shape> shapes;
    _cache->findAncestor(_Shape, subshape, type, &shapes);
    return shapes;
}

// The following lines should be used for now to replace the original macros (in the future we can
// refactor to use std::source_location and eliminate the use of the macros entirely).
//     FC_THROWM(NullShapeException, "Null shape");
//     FC_THROWM(NullShapeException, "Null input shape");
//     FC_WARN("Null input shape");  // NOLINT
//
// The original macros:
// #define HANDLE_NULL_SHAPE _HANDLE_NULL_SHAPE("Null shape",true)
// #define HANDLE_NULL_INPUT _HANDLE_NULL_SHAPE("Null input shape",true)
// #define WARN_NULL_INPUT _HANDLE_NULL_SHAPE("Null input shape",false)

bool TopoShape::hasPendingElementMap() const
{
    return !elementMap(false) && this->_cache
        && (this->_parentCache || this->_cache->cachedElementMap);
}

bool TopoShape::canMapElement(const TopoShape& other) const
{
    if (isNull() || other.isNull() || this == &other || other.Tag == -1 || Tag == -1) {
        return false;
    }
    if ((other.Tag == 0) && !other.elementMap(false) && !other.hasPendingElementMap()) {
        return false;
    }
    initCache();
    other.initCache();
    _cache->relations.clear();
    return true;
}

namespace
{
size_t checkSubshapeCount(const TopoShape& topoShape1,
                          const TopoShape& topoShape2,
                          TopAbs_ShapeEnum elementType)
{
    auto count = topoShape1.countSubShapes(elementType);
    auto other = topoShape2.countSubShapes(elementType);
    if (count != other) {
        FC_WARN("sub shape mismatch");  // NOLINT
        if (count > other) {
            count = other;
        }
    }
    return count;
}
}  // namespace

void TopoShape::setupChild(Data::ElementMap::MappedChildElements& child,
                           TopAbs_ShapeEnum elementType,
                           const TopoShape& topoShape,
                           size_t shapeCount,
                           const char* op)
{
    child.indexedName = Data::IndexedName::fromConst(TopoShape::shapeName(elementType).c_str(), 1);
    child.offset = 0;
    child.count = static_cast<int>(shapeCount);
    child.elementMap = topoShape.elementMap();
    if (this->Tag != topoShape.Tag) {
        child.tag = topoShape.Tag;
    }
    else {
        child.tag = 0;
    }
    if (op) {
        child.postfix = op;
    }
}

void TopoShape::copyElementMap(const TopoShape& topoShape, const char* op)
{
    if (topoShape.isNull() || isNull()) {
        return;
    }
    std::vector<Data::ElementMap::MappedChildElements> children;
    std::array<TopAbs_ShapeEnum, 3> elementTypes = {TopAbs_VERTEX, TopAbs_EDGE, TopAbs_FACE};
    for (const auto elementType : elementTypes) {
        auto count = checkSubshapeCount(*this, topoShape, elementType);
        if (count == 0) {
            continue;
        }
        children.emplace_back();
        auto& child = children.back();
        setupChild(child, elementType, topoShape, count, op);
    }
    resetElementMap();
    if (!Hasher) {
        Hasher = topoShape.Hasher;
    }
    setMappedChildElements(children);
}

namespace
{
void warnIfLogging()
{
    if (FC_LOG_INSTANCE.isEnabled(FC_LOGLEVEL_LOG)) {
        FC_WARN("hasher mismatch");  // NOLINT
    }
};

void hasherMismatchError()
{
    FC_ERR("hasher mismatch");  // NOLINT
}


void checkAndMatchHasher(TopoShape& topoShape1, const TopoShape& topoShape2)
{
    if (topoShape1.Hasher) {
        if (topoShape2.Hasher != topoShape1.Hasher) {
            if (topoShape1.getElementMapSize(false) == 0U) {
                warnIfLogging();
            }
            else {
                hasherMismatchError();
            }
            topoShape1.Hasher = topoShape2.Hasher;
        }
    }
    else {
        topoShape1.Hasher = topoShape2.Hasher;
    }
}
}  // namespace


// TODO: Refactor mapSubElementTypeForShape to reduce complexity
void TopoShape::mapSubElementTypeForShape(const TopoShape& other,
                                          TopAbs_ShapeEnum type,
                                          const char* op,
                                          int count,
                                          bool forward,
                                          bool& warned)
{
    auto& shapeMap = _cache->getAncestry(type);
    auto& otherMap = other._cache->getAncestry(type);
    const char* shapeType = shapeName(type).c_str();

    // 1-indexed for readability (e.g. there is no "Edge0", we started at "Edge1", etc.)
    for (int outerCounter = 1; outerCounter <= count; ++outerCounter) {
        int innerCounter {0};
        int index {0};
        if (forward) {
            innerCounter = outerCounter;
            index = shapeMap.find(_Shape, otherMap.find(other._Shape, outerCounter));
            if (index == 0) {
                continue;
            }
        }
        else {
            index = outerCounter;
            innerCounter = otherMap.find(other._Shape, shapeMap.find(_Shape, outerCounter));
            if (innerCounter == 0) {
                continue;
            }
        }
        Data::IndexedName element = Data::IndexedName::fromConst(shapeType, index);
        for (auto& mappedName :
             other.getElementMappedNames(Data::IndexedName::fromConst(shapeType, innerCounter),
                                         true)) {
            auto& name = mappedName.first;
            auto& sids = mappedName.second;
            if (!sids.empty()) {
                if (!Hasher) {
                    Hasher = sids[0].getHasher();
                }
                else if (!sids[0].isFromSameHasher(Hasher)) {
                    if (!warned) {
                        warned = true;
                        FC_WARN("hasher mismatch");  // NOLINT
                    }
                    sids.clear();
                }
            }
            std::ostringstream ss;
            char elementType {shapeName(type)[0]};
            if (!elementMap()) {
                // NOLINTNEXTLINE
                FC_THROWM(NullShapeException, "No element map");
            }
            elementMap()->encodeElementName(elementType, name, ss, &sids, Tag, op, other.Tag);
            elementMap()->setElementName(element, name, Tag, &sids);
        }
    }
}

void TopoShape::mapSubElementForShape(const TopoShape& other, const char* op)
{
    bool warned = false;
    static const std::array<TopAbs_ShapeEnum, 3> types = {TopAbs_VERTEX, TopAbs_EDGE, TopAbs_FACE};

    for (auto type : types) {
        auto& shapeMap = _cache->getAncestry(type);
        auto& otherMap = other._cache->getAncestry(type);
        if ((shapeMap.count() == 0) || (otherMap.count() == 0)) {
            continue;
        }

        bool forward {false};
        int count {0};
        if (otherMap.count() <= shapeMap.count()) {
            forward = true;
            count = otherMap.count();
        }
        else {
            forward = false;
            count = shapeMap.count();
        }
        mapSubElementTypeForShape(other, type, op, count, forward, warned);
    }
}

void TopoShape::mapSubElement(const TopoShape& other, const char* op, bool forceHasher)
{
    if (!canMapElement(other)) {
        return;
    }

    if ((getElementMapSize(false) == 0U) && this->_Shape.IsPartner(other._Shape)) {
        if (!this->Hasher) {
            this->Hasher = other.Hasher;
        }
        copyElementMap(other, op);
        return;
    }

    if (!forceHasher && other.Hasher) {
        checkAndMatchHasher(*this, other);
    }

    mapSubElementForShape(other, op);
}

std::vector<Data::ElementMap::MappedChildElements>
TopoShape::createChildMap(size_t count, const std::vector<TopoShape>& shapes, const char* op)
{
    std::vector<Data::ElementMap::MappedChildElements> children;
    children.reserve(count * (size_t)3);
    std::array<TopAbs_ShapeEnum, 3> types = {TopAbs_VERTEX, TopAbs_EDGE, TopAbs_FACE};
    for (const auto topAbsType : types) {
        size_t offset = 0;
        for (auto& topoShape : shapes) {
            if (topoShape.isNull()) {
                continue;
            }
            auto subShapeCount = topoShape.countSubShapes(topAbsType);
            if (subShapeCount == 0) {
                continue;
            }
            children.emplace_back();
            auto& child = children.back();
            child.indexedName =
                Data::IndexedName::fromConst(TopoShape::shapeName(topAbsType).c_str(), 1);
            child.offset = static_cast<int>(offset);
            offset += subShapeCount;
            child.count = static_cast<int>(subShapeCount);
            child.elementMap = topoShape.elementMap();
            child.tag = topoShape.Tag;
            if (op) {
                child.postfix = op;
            }
        }
    }
    return children;
}

void TopoShape::mapCompoundSubElements(const std::vector<TopoShape>& shapes, const char* op)
{
    int count = 0;
    for (auto& topoShape : shapes) {
        if (topoShape.isNull()) {
            continue;
        }
        ++count;
        auto subshape = getSubShape(TopAbs_SHAPE, count, /*silent = */ true);
        if (!subshape.IsPartner(topoShape._Shape)) {
            return;  // Not a partner shape, don't do any mapping at all
        }
    }
    auto children {createChildMap(count, shapes, op)};
    setMappedChildElements(children);
}

void TopoShape::mapSubElement(const std::vector<TopoShape>& shapes, const char* op)
{
    if (shapes.empty()) {
        return;
    }

    if (shapeType(true) == TopAbs_COMPOUND) {
        mapCompoundSubElements(shapes, op);
    }
    else {
        for (auto& shape : shapes) {
            mapSubElement(shape, op);
        }
    }
}

struct ShapeInfo
{
    const TopoDS_Shape& shape;
    TopoShapeCache::Ancestry& cache;
    TopAbs_ShapeEnum type;
    const char* shapetype;

    ShapeInfo(const TopoDS_Shape& shape, TopAbs_ShapeEnum type, TopoShapeCache::Ancestry& cache)
        : shape(shape)
        , cache(cache)
        , type(type)
        , shapetype(TopoShape::shapeName(type).c_str())
    {}

    int count() const
    {
        return cache.count();
    }

    TopoDS_Shape find(int index)
    {
        return cache.find(shape, index);
    }

    int find(const TopoDS_Shape& subshape)
    {
        return cache.find(shape, subshape);
    }
};

////////////////////////////////////////
// makESHAPE -> makeShapeWithElementMap
///////////////////////////////////////

struct NameKey
{
    Data::MappedName name;
    long tag = 0;
    int shapetype = 0;

    NameKey()
    = default;
    explicit NameKey(const Data::MappedName& n)
        : name(n)
    {}
    NameKey(int type, Data::MappedName  n)
        : name(std::move(n))
    {
        // Order the shape type from vertex < edge < face < other.  We'll rely
        // on this for sorting when we name the geometry element.
        switch (type) {
            case TopAbs_VERTEX:
                shapetype = 0;
                break;
            case TopAbs_EDGE:
                shapetype = 1;
                break;
            case TopAbs_FACE:
                shapetype = 2;
                break;
            default:
                shapetype = 3;
        }
    }
    bool operator<(const NameKey& other) const
    {
        if (shapetype < other.shapetype) {
            return true;
        }
        if (shapetype > other.shapetype) {
            return false;
        }
        if (tag < other.tag) {
            return true;
        }
        if (tag > other.tag) {
            return false;
        }
        return name < other.name;
    }
};

struct NameInfo
{
    int index{};
    Data::ElementIDRefs sids;
    const char* shapetype{};
};


const std::string& modPostfix()
{
    static std::string postfix(TopoShape::elementMapPrefix() + ":M");
    return postfix;
}

const std::string& modgenPostfix()
{
    static std::string postfix(modPostfix() + "G");
    return postfix;
}

const std::string& genPostfix()
{
    static std::string postfix(TopoShape::elementMapPrefix() + ":G");
    return postfix;
}

const std::string& upperPostfix()
{
    static std::string postfix(TopoShape::elementMapPrefix() + ":U");
    return postfix;
}

const std::string& lowerPostfix()
{
    static std::string postfix(TopoShape::elementMapPrefix() + ":L");
    return postfix;
}

// TODO: Refactor makeShapeWithElementMap to reduce complexity
TopoShape& TopoShape::makeShapeWithElementMap(const TopoDS_Shape& shape,
                                              const Mapper& mapper,
                                              const std::vector<TopoShape>& shapes,
                                              const char* op)
{
    setShape(shape);
    if (shape.IsNull()) {
        FC_THROWM(NullShapeException, "Null shape");
    }

    if (shapes.empty()) {
        return *this;
    }

    size_t canMap = 0;
    for (auto& incomingShape : shapes) {
        if (canMapElement(incomingShape)) {
            ++canMap;
        }
    }
    if (canMap == 0U) {
        return *this;
    }
    if (canMap != shapes.size() && FC_LOG_INSTANCE.isEnabled(FC_LOGLEVEL_LOG)) {
        FC_WARN("Not all input shapes are mappable");  // NOLINT
    }

    if (!op) {
        op = Part::OpCodes::Maker;
    }
    std::string _op = op;
    _op += '_';

    initCache();
    ShapeInfo vinfo(_Shape, TopAbs_VERTEX, _cache->getAncestry(TopAbs_VERTEX));
    ShapeInfo einfo(_Shape, TopAbs_EDGE, _cache->getAncestry(TopAbs_EDGE));
    ShapeInfo finfo(_Shape, TopAbs_FACE, _cache->getAncestry(TopAbs_FACE));
    mapSubElement(shapes, op);

    std::array<ShapeInfo*, 3> infos = {&vinfo, &einfo, &finfo};

    std::array<ShapeInfo*, TopAbs_SHAPE> infoMap{};
    infoMap[TopAbs_VERTEX] = &vinfo;
    infoMap[TopAbs_EDGE] = &einfo;
    infoMap[TopAbs_WIRE] = &einfo;
    infoMap[TopAbs_FACE] = &finfo;
    infoMap[TopAbs_SHELL] = &finfo;
    infoMap[TopAbs_SOLID] = &finfo;
    infoMap[TopAbs_COMPOUND] = &finfo;
    infoMap[TopAbs_COMPSOLID] = &finfo;

    std::ostringstream ss;
    std::string postfix;
    Data::MappedName newName;

    std::map<Data::IndexedName, std::map<NameKey, NameInfo>> newNames;

    // First, collect names from other shapes that generates or modifies the
    // new shape
    for (auto& pinfo : infos) {
        auto& info = *pinfo;
        for (const auto & incomingShape : shapes) {
            if (!canMapElement(incomingShape)) {
                continue;
            }
            auto& otherMap = incomingShape._cache->getAncestry(info.type);
            if (otherMap.count() == 0) {
                continue;
            }

            for (int i = 1; i <= otherMap.count(); i++) {
                const auto& otherElement = otherMap.find(incomingShape._Shape, i);
                // Find all new objects that are a modification of the old object
                Data::ElementIDRefs sids;
                NameKey key(info.type,
                    incomingShape.getMappedName(Data::IndexedName::fromConst(info.shapetype, i),
                                                true,
                                                &sids));

                int newShapeCounter = 0;
                for (auto& newShape : mapper.modified(otherElement)) {
                    ++newShapeCounter;
                    if (newShape.ShapeType() >= TopAbs_SHAPE) {
                        // NOLINTNEXTLINE
                        FC_ERR("unknown modified shape type " << newShape.ShapeType() << " from "
                                                              << info.shapetype << i);
                        continue;
                    }
                    auto& newInfo = *infoMap[newShape.ShapeType()];
                    if (newInfo.type != newShape.ShapeType()) {
                        if (FC_LOG_INSTANCE.isEnabled(FC_LOGLEVEL_LOG)) {
                            // TODO: it seems modified shape may report higher
                            // level shape type just like generated shape below.
                            // Maybe we shall do the same for name construction.
                            // NOLINTNEXTLINE
                            FC_WARN("modified shape type " << shapeName(newShape.ShapeType())
                                                           << " mismatch with " << info.shapetype
                                                           << i);
                        }
                        continue;
                    }
                    int newShapeIndex = newInfo.find(newShape);
                    if (newShapeIndex == 0) {
                        // This warning occurs in makERevolve. It generates
                        // some shape from a vertex that never made into the
                        // final shape. There may be incomingShape cases there.
                        if (FC_LOG_INSTANCE.isEnabled(FC_LOGLEVEL_LOG)) {
                            // NOLINTNEXTLINE
                            FC_WARN("Cannot find " << op << " modified " << newInfo.shapetype
                                                   << " from " << info.shapetype << i);
                        }
                        continue;
                    }

                    Data::IndexedName element = Data::IndexedName::fromConst(newInfo.shapetype, newShapeIndex);
                    if (getMappedName(element)) {
                        continue;
                    }

                    key.tag = incomingShape.Tag;
                    auto& name_info = newNames[element][key];
                    name_info.sids = sids;
                    name_info.index = newShapeCounter;
                    name_info.shapetype = info.shapetype;
                }

                int checkParallel = -1;
                gp_Pln pln;

                // Find all new objects that were generated from an old object
                // (e.g. a face generated from an edge)
                newShapeCounter = 0;
                for (auto& newShape : mapper.generated(otherElement)) {
                    if (newShape.ShapeType() >= TopAbs_SHAPE) {
                        // NOLINTNEXTLINE
                        FC_ERR("unknown generated shape type " << newShape.ShapeType() << " from "
                                                               << info.shapetype << i);
                        continue;
                    }

                    int parallelFace = -1;
                    int coplanarFace = -1;
                    auto& newInfo = *infoMap[newShape.ShapeType()];
                    std::vector<TopoDS_Shape> newShapes;
                    int shapeOffset = 0;
                    if (newInfo.type == newShape.ShapeType()) {
                        newShapes.push_back(newShape);
                    }
                    else {
                        // It is possible for the maker to report generating a
                        // higher level shape, such as shell or solid. For
                        // example, when extruding, OCC will report the
                        // extruding face generating the entire solid. However,
                        // it will also report the edges of the extruding face
                        // generating the side faces. In this case, too much
                        // information is bad for us. We don't want the name of
                        // the side face (and its edges) to be coupled with
                        // incomingShape (unrelated) edges in the extruding face.
                        //
                        // shapeOffset below is used to make sure the higher
                        // level mapped names comes late after sorting. We'll
                        // ignore those names if there are more precise mapping
                        // available.
                        shapeOffset = 3;

                        if (info.type == TopAbs_FACE && checkParallel < 0) {
                            if (!TopoShape(otherElement).findPlane(pln)) {
                                checkParallel = 0;
                            }
                            else {
                                checkParallel = 1;
                            }
                        }
                        for (TopExp_Explorer xp(newShape, newInfo.type); xp.More(); xp.Next()) {
                            newShapes.push_back(xp.Current());

                            if ((parallelFace < 0 || coplanarFace < 0) && checkParallel > 0) {
                                // Specialized checking for high level mapped
                                // face that are either coplanar or parallel
                                // with the source face, which are common in
                                // operations like extrusion. Once found, the
                                // first coplanar face will assign an index of
                                // INT_MIN+1, and the first parallel face
                                // INT_MIN. The purpose of these special
                                // indexing is to make the name more stable for
                                // those generated faces.
                                //
                                // For example, the top or bottom face of an
                                // extrusion will be named using the extruding
                                // face. With a fixed index, the name is no
                                // longer affected by adding/removing of holes
                                // inside the extruding face/sketch.
                                gp_Pln plnOther;
                                if (TopoShape(newShapes.back()).findPlane(plnOther)) {
                                    if (pln.Axis().IsParallel(plnOther.Axis(),
                                                              Precision::Angular())) {
                                        if (coplanarFace < 0) {
                                            gp_Vec vec(pln.Axis().Location(),
                                                       plnOther.Axis().Location());
                                            Standard_Real D1 =
                                                gp_Vec(pln.Axis().Direction()).Dot(vec);
                                            if (D1 < 0) {
                                                D1 = -D1;
                                            }
                                            Standard_Real D2 =
                                                gp_Vec(plnOther.Axis().Direction()).Dot(vec);
                                            if (D2 < 0) {
                                                D2 = -D2;
                                            }
                                            if (D1 <= Precision::Confusion()
                                                && D2 <= Precision::Confusion()) {
                                                coplanarFace = (int)newShapes.size();
                                                continue;
                                            }
                                        }
                                        if (parallelFace < 0) {
                                            parallelFace = (int)newShapes.size();
                                        }
                                    }
                                }
                            }
                        }
                    }
                    key.shapetype += shapeOffset;
                    for (auto& workingShape : newShapes) {
                        ++newShapeCounter;
                        int workingShapeIndex = newInfo.find(workingShape);
                        if (!workingShapeIndex) {
                            if (FC_LOG_INSTANCE.isEnabled(FC_LOGLEVEL_LOG)) {
                                FC_WARN("Cannot find " << op << " generated " << newInfo.shapetype
                                                       << " from " << info.shapetype << i);
                            }
                            continue;
                        }

                        Data::IndexedName element =
                            Data::IndexedName::fromConst(newInfo.shapetype, workingShapeIndex);
                        auto mapped = getMappedName(element);
                        if (mapped) {
                            continue;
                        }

                        key.tag = incomingShape.Tag;
                        auto& name_info = newNames[element][key];
                        name_info.sids = sids;
                        if (newShapeCounter == parallelFace) {
                            name_info.index = std::numeric_limits<int>::min();
                        }
                        else if (newShapeCounter == coplanarFace) {
                            name_info.index = std::numeric_limits<int>::min() + 1;
                        }
                        else {
                            name_info.index = -newShapeCounter;
                        }
                        name_info.shapetype = info.shapetype;
                    }
                    key.shapetype -= shapeOffset;
                }
            }
        }
    }

    // We shall first exclude those names generated from high level mapping. If
    // there are still any unnamed elements left after we go through the process
    // below, we set delayed=true, and start using those excluded names.
    bool delayed = false;

    while (true) {

        // Construct the names for modification/generation info collected in
        // the previous step
        for (auto itName = newNames.begin(), itNext = itName; itNext != newNames.end();
             itName = itNext) {
            // We treat the first modified/generated source shape name specially.
            // If case there are more than one source shape. We hash the first
            // source name separately, and then obtain the second string id by
            // hashing all the source names together.  We then use the second
            // string id as the postfix for our name.
            //
            // In this way, we can associate the same source that are modified by
            // multiple other shapes.

            ++itNext;

            auto& element = itName->first;
            auto& names = itName->second;
            const auto& first_key = names.begin()->first;
            auto& first_info = names.begin()->second;

            if (!delayed && first_key.shapetype >= 3 && first_info.index > INT_MIN + 1) {
                // This name is mapped from high level (shell, solid, etc.)
                // Delay till next round.
                //
                // index>INT_MAX+1 is for checking generated coplanar and
                // parallel face mapping, which has special fixed index to make
                // name stable.  These names are not delayed.
                continue;
            }
            if (!delayed && getMappedName(element)) {
                newNames.erase(itName);
                continue;
            }

            int name_type =
                first_info.index > 0 ? 1 : 2;  // index>0 means modified, or else generated
            Data::MappedName first_name = first_key.name;

            Data::ElementIDRefs sids(first_info.sids);

            postfix.clear();
            if (names.size() > 1) {
                ss.str("");
                ss << '(';
                bool first = true;
                auto it = names.begin();
                int count = 0;
                for (++it; it != names.end(); ++it) {
                    auto& other_key = it->first;
                    if (other_key.shapetype >= 3 && first_key.shapetype < 3) {
                        // shapetype>=3 means it's a high level mapping (e.g. a face
                        // generates a solid). We don't want that if there are more
                        // precise low level mapping available. See comments above
                        // for more details.
                        break;
                    }
                    if (first) {
                        first = false;
                    }
                    else {
                        ss << '|';
                    }
                    auto& other_info = it->second;
                    std::ostringstream ss2;
                    if (other_info.index != 1) {
                        // 'K' marks the additional source shape of this
                        // generate (or modified) shape.
                        ss2 << elementMapPrefix() << 'K';
                        if (other_info.index == INT_MIN) {
                            ss2 << '0';
                        }
                        else if (other_info.index == INT_MIN + 1) {
                            ss2 << "00";
                        }
                        else {
                            // The same source shape may generate or modify
                            // more than one shape. The index here marks the
                            // position it is reported by OCC. Including the
                            // index here is likely to degrade name stablilty,
                            // but is unfortunately a necessity to avoid
                            // duplicate names.
                            ss2 << other_info.index;
                        }
                    }
                    Data::MappedName other_name = other_key.name;
                    elementMap()->encodeElementName(other_info.shapetype[0],
                                                    other_name,
                                                    ss2,
                                                    &sids,
                                                    Tag,
                                                    0,
                                                    other_key.tag);
                    ss << other_name;
                    if ((name_type == 1 && other_info.index < 0)
                        || (name_type == 2 && other_info.index > 0)) {
                        if (FC_LOG_INSTANCE.isEnabled(FC_LOGLEVEL_LOG)) {
                            // NOLINTNEXTLINE
                            FC_WARN("element is both generated and modified");
                        }
                        name_type = 0;
                    }
                    sids += other_info.sids;
                    // To avoid the name becoming to long, just put some limit here
                    if (++count == 4) {
                        break;
                    }
                }
                if (!first) {
                    ss << ')';
                    if (Hasher) {
                        sids.push_back(Hasher->getID(ss.str().c_str()));
                        ss.str("");
                        ss << sids.back().toString();
                    }
                    postfix = ss.str();
                }
            }

            ss.str("");
            if (name_type == 2) {
                ss << genPostfix();
            }
            else if (name_type == 1) {
                ss << modPostfix();
            }
            else {
                ss << modgenPostfix();
            }
            if (first_info.index == INT_MIN) {
                ss << '0';
            }
            else if (first_info.index == INT_MIN + 1) {
                ss << "00";
            }
            else if (abs(first_info.index) > 1) {
                ss << abs(first_info.index);
            }
            ss << postfix;
            elementMap()
                ->encodeElementName(element[0], first_name, ss, &sids, Tag, op, first_key.tag);
            elementMap()->setElementName(element, first_name, Tag, &sids);

            if (!delayed && first_key.shapetype < 3) {
                newNames.erase(itName);
            }
        }

        // The reverse pass. Starting from the highest level element, i.e.
        // Face, for any element that are named, assign names for its lower unnamed
        // elements. For example, if Edge1 is named E1, and its vertexes are not
        // named, then name them as E1;U1, E1;U2, etc.
        //
        // In order to make the name as stable as possible, we may assign multiple
        // names (which must be sorted, because we may use the first one to name
        // upper element in the final pass) to lower element if it appears in
        // multiple higher elements, e.g. same edge in multiple faces.

        for (size_t infoIndex = infos.size() - 1; infoIndex != 0; --infoIndex) {
            std::map<Data::IndexedName,
                     std::map<Data::MappedName, NameInfo, Data::ElementNameComparator>>
                names;
            auto& info = *infos[infoIndex];
            auto& next = *infos[infoIndex - 1];
            int elementCounter = 1;
            auto it = newNames.end();
            if (delayed) {
                it = newNames.upper_bound(Data::IndexedName::fromConst(info.shapetype, 0));
            }
            for (;; ++elementCounter) {
                Data::IndexedName element;
                if (!delayed) {
                    if (elementCounter > info.count()) {
                        break;
                    }
                    element = Data::IndexedName::fromConst(info.shapetype, elementCounter);
                    if (newNames.count(element) != 0U) {
                        continue;
                    }
                }
                else if (it == newNames.end()
                         || !boost::starts_with(it->first.getType(), info.shapetype)) {
                    break;
                }
                else {
                    element = it->first;
                    ++it;
                    elementCounter = element.getIndex();
                    if (elementCounter == 0 || elementCounter > info.count()) {
                        continue;
                    }
                }
                Data::ElementIDRefs sids;
                Data::MappedName mapped = getMappedName(element, false, &sids);
                if (!mapped) {
                    continue;
                }

                TopTools_IndexedMapOfShape submap;
                TopExp::MapShapes(info.find(elementCounter), next.type, submap);
                for (int submapIndex = 1, infoCounter = 1; submapIndex <= submap.Extent(); ++submapIndex) {
                    ss.str("");
                    int elementIndex = next.find(submap(submapIndex));
                    assert(elementIndex);
                    Data::IndexedName indexedName = Data::IndexedName::fromConst(next.shapetype, elementIndex);
                    if (getMappedName(indexedName)) {
                        continue;
                    }
                    auto& infoRef = names[indexedName][mapped];
                    infoRef.index = infoCounter++;
                    infoRef.sids = sids;
                }
            }
            // Assign the actual names
            for (auto& actualName : names) {
#ifndef FC_ELEMENT_MAP_ALL
                // Do we really want multiple names for an element in this case?
                // If not, we just pick the name in the first sorting order here.
                auto& name = *actualName.second.begin();
#else
                for (auto& name : actualName.second)
#endif
                {
                    auto& infoRef = name.second;
                    auto& sids = infoRef.sids;
                    newName = name.first;
                    ss.str("");
                    ss << upperPostfix();
                    if (infoRef.index > 1) {
                        ss << infoRef.index;
                    }
                    elementMap()->encodeElementName(actualName.first[0], newName, ss, &sids, Tag, op);
                    elementMap()->setElementName(actualName.first, newName, Tag, &sids);
                }
            }
        }

        // The forward pass. For any elements that are not named, try construct its
        // name from the lower elements
        bool hasUnnamed = false;
        for (size_t ifo = 1; ifo < infos.size(); ++ifo) {
            auto& info = *infos[ifo];
            auto& prev = *infos[ifo - 1];
            for (int i = 1; i <= info.count(); ++i) {
                Data::IndexedName element = Data::IndexedName::fromConst(info.shapetype, i);
                if (getMappedName(element)) {
                    continue;
                }

                Data::ElementIDRefs sids;
                std::map<Data::MappedName, Data::IndexedName, Data::ElementNameComparator> names;
                TopExp_Explorer xp;
                if (info.type == TopAbs_FACE) {
                    xp.Init(BRepTools::OuterWire(TopoDS::Face(info.find(i))), TopAbs_EDGE);
                }
                else {
                    xp.Init(info.find(i), prev.type);
                }
                for (; xp.More(); xp.Next()) {
                    int j = prev.find(xp.Current());
                    assert(j);
                    Data::IndexedName prevElement = Data::IndexedName::fromConst(prev.shapetype, j);
                    if (!delayed && (newNames.count(prevElement) != 0U)) {
                        names.clear();
                        break;
                    }
                    Data::ElementIDRefs sid;
                    Data::MappedName name = getMappedName(prevElement, false, &sid);
                    if (!name) {
                        // only assign name if all lower elements are named
                        if (FC_LOG_INSTANCE.isEnabled(FC_LOGLEVEL_LOG)) {
                            // NOLINTNEXTLINE
                            FC_WARN("unnamed lower element " << prevElement);
                        }
                        names.clear();
                        break;
                    }
                    auto res = names.emplace(name, prevElement);
                    if (res.second) {
                        sids += sid;
                    }
                    else if (prevElement != res.first->second) {
                        // The seam edge will appear twice, which is normal. We
                        // only warn if the mapped element names are different.
                        // NOLINTNEXTLINE
                        FC_WARN("lower element " << prevElement << " and " << res.first->second
                                                 << " has duplicated name " << name << " for "
                                                 << info.shapetype << i);
                    }
                }
                if (names.empty()) {
                    hasUnnamed = true;
                    continue;
                }
                auto it = names.begin();
                newName = it->first;
                if (names.size() == 1) {
                    ss << lowerPostfix();
                }
                else {
                    bool first = true;
                    ss.str("");
                    if (!Hasher) {
                        ss << lowerPostfix();
                    }
                    ss << '(';
                    int count = 0;
                    for (++it; it != names.end(); ++it) {
                        if (first) {
                            first = false;
                        }
                        else {
                            ss << '|';
                        }
                        ss << it->first;

                        // To avoid the name becoming to long, just put some limit here
                        if (++count == 4) {
                            break;
                        }
                    }
                    ss << ')';
                    if (Hasher) {
                        sids.push_back(Hasher->getID(ss.str().c_str()));
                        ss.str("");
                        ss << lowerPostfix() << sids.back().toString();
                    }
                }
                elementMap()->encodeElementName(element[0], newName, ss, &sids, Tag, op);
                elementMap()->setElementName(element, newName, Tag, &sids);
            }
        }
        if (!hasUnnamed || delayed || newNames.empty()) {
            break;
        }
        delayed = true;
    }
    return *this;
}

namespace
{
void addShapesToBuilder(const std::vector<TopoShape>& shapes,
                        BRep_Builder& builder,
                        TopoDS_Compound& comp)
{
    int count = 0;
    for (auto& topoShape : shapes) {
        if (topoShape.isNull()) {
            FC_WARN("Null input shape");  // NOLINT
            continue;
        }
        builder.Add(comp, topoShape.getShape());
        ++count;
    }
    if (count == 0) {
        FC_THROWM(NullShapeException, "Null shape");
    }
}
}  // namespace

TopoShape&
TopoShape::makeElementCompound(const std::vector<TopoShape>& shapes, const char* op, bool force)
{
    if (!force && shapes.size() == 1) {
        *this = shapes[0];
        return *this;
    }

    BRep_Builder builder;
    TopoDS_Compound comp;
    builder.MakeCompound(comp);

    if (shapes.empty()) {
        setShape(comp);
        return *this;
    }
    addShapesToBuilder(shapes, builder, comp);
    setShape(comp);
    initCache();

    mapSubElement(shapes, op);
    return *this;
}


TopoShape& TopoShape::makeElementFace(const TopoShape& shape,
                                      const char* op,
                                      const char* maker,
                                      const gp_Pln* plane)
{
    std::vector<TopoShape> shapes;
    if (shape.isNull()) {
        FC_THROWM(NullShapeException, "Null shape");
    }
    if (shape.getShape().ShapeType() == TopAbs_COMPOUND) {
        shapes = shape.getSubTopoShapes();
    }
    else {
        shapes.push_back(shape);
    }
    return makeElementFace(shapes, op, maker, plane);
}

TopoShape& TopoShape::makeElementFace(const std::vector<TopoShape>& shapes,
                                      const char* op,
                                      const char* maker,
                                      const gp_Pln* plane)
{
    if (!maker || !maker[0]) {
        maker = "Part::FaceMakerBullseye";
    }
    std::unique_ptr<FaceMaker> mkFace = FaceMaker::ConstructFromType(maker);
    mkFace->MyHasher = Hasher;
    mkFace->MyOp = op;
    if (plane) {
        mkFace->setPlane(*plane);
    }

    for (auto& shape : shapes) {
        if (shape.getShape().ShapeType() == TopAbs_COMPOUND) {
            mkFace->useTopoCompound(shape);
        }
        else {
            mkFace->addTopoShape(shape);
        }
    }
    mkFace->Build();

    const auto& ret = mkFace->getTopoShape();
    setShape(ret._Shape);
    Hasher = ret.Hasher;
    resetElementMap(ret.elementMap());
    if (!isValid()) {
        ShapeFix_ShapeTolerance aSFT;
        aSFT.LimitTolerance(getShape(),
                            Precision::Confusion(),
                            Precision::Confusion(),
                            TopAbs_SHAPE);

        // In some cases, the OCC reports the returned shape having invalid
        // tolerance. Not sure about the real cause.
        //
        // Update: one of the cause is related to OCC bug in
        // BRepBuilder_FindPlane, A possible call sequence is,
        //
        //      makEOffset2D() -> TopoShape::findPlane() -> BRepLib_FindSurface
        //
        // See code comments in findPlane() for the description of the bug and
        // work around.

        ShapeFix_Shape fixer(getShape());
        fixer.Perform();
        setShape(fixer.Shape(), false);

        if (!isValid()) {
            FC_WARN("makeElementFace: resulting face is invalid");
        }
    }
    return *this;
}

/**
 *  Encode and set an element name in the elementMap.  If a hasher is defined, apply it to the name.
 *
 * @param element   The element name(type) that provides 1 one character suffix to the name IF
 * <conditions>.
 * @param names     The subnames to build the name from.  If empty, return the TopoShape MappedName.
 * @param marker    The elementMap name or suffix to start the name with.  If null, use the
 *                  elementMapPrefix.
 * @param op        The op text passed to the element name encoder along with the TopoShape Tag
 * @param _sids     If defined, records the sub ids processed.
 *
 * @return          The encoded, possibly hashed name.
 */
Data::MappedName TopoShape::setElementComboName(const Data::IndexedName& element,
                                                const std::vector<Data::MappedName>& names,
                                                const char* marker,
                                                const char* op,
                                                const Data::ElementIDRefs* _sids)
{
    if (names.empty()) {
        return Data::MappedName();
    }
    std::string _marker;
    if (!marker) {
        marker = elementMapPrefix().c_str();
    }
    else if (!boost::starts_with(marker, elementMapPrefix())) {
        _marker = elementMapPrefix() + marker;
        marker = _marker.c_str();
    }
    auto it = names.begin();
    Data::MappedName newName = *it;
    std::ostringstream ss;
    Data::ElementIDRefs sids;
    if (_sids) {
        sids = *_sids;
    }
    if (names.size() == 1) {
        ss << marker;
    }
    else {
        bool first = true;
        ss.str("");
        if (!Hasher) {
            ss << marker;
        }
        ss << '(';
        for (++it; it != names.end(); ++it) {
            if (first) {
                first = false;
            }
            else {
                ss << '|';
            }
            ss << *it;
        }
        ss << ')';
        if (Hasher) {
            sids.push_back(Hasher->getID(ss.str().c_str()));
            ss.str("");
            ss << marker << sids.back().toString();
        }
    }
    elementMap()->encodeElementName(element[0], newName, ss, &sids, Tag, op);
    return elementMap()->setElementName(element, newName, Tag, &sids);
}

/**
 * Reorient the outer and inner wires of the TopoShape
 *
 * @param inner If this is not a nullptr, then any inner wires processed will be returned in this
 * vector.
 * @param reorient  One of NoReorient, Reorient ( Outer forward, inner reversed ),
 *                  ReorientForward ( all forward ), or ReorientReversed ( all reversed )
 * @return The outer wire, or an empty TopoShape if this isn't a Face, has no Face subShapes, or the
 *         outer wire isn't found.
 */
TopoShape TopoShape::splitWires(std::vector<TopoShape>* inner, SplitWireReorient reorient) const
{
    // ShapeAnalysis::OuterWire() is un-reliable for some reason. OCC source
    // code shows it works by creating face using each wire, and then test using
    // BRepTopAdaptor_FClass2d::PerformInfinitePoint() to check if it is an out
    // bound wire. And practice shows it sometimes returns the incorrect
    // result. Need more investigation. Note that this may be related to
    // unreliable solid face orientation
    // (https://forum.freecadweb.org/viewtopic.php?p=446006#p445674)
    //
    // Use BrepTools::OuterWire() instead. OCC source code shows it is
    // implemented using simple bound box checking. This should be a
    // reliable method, especially so for a planar face.

    TopoDS_Shape tmp;
    if (shapeType(true) == TopAbs_FACE) {
        tmp = BRepTools::OuterWire(TopoDS::Face(_Shape));
    }
    else if (countSubShapes(TopAbs_FACE) == 1) {
        tmp = BRepTools::OuterWire(TopoDS::Face(getSubShape(TopAbs_FACE, 1)));
    }
    if (tmp.IsNull()) {
        return TopoShape();
    }
    const auto& wires = getSubTopoShapes(TopAbs_WIRE);
    auto it = wires.begin();

    TopAbs_Orientation orientOuter, orientInner;
    switch (reorient) {
        case ReorientReversed:
            orientOuter = orientInner = TopAbs_REVERSED;
            break;
        case ReorientForward:
            orientOuter = orientInner = TopAbs_FORWARD;
            break;
        default:
            orientOuter = TopAbs_FORWARD;
            orientInner = TopAbs_REVERSED;
            break;
    }

    auto doReorient = [](TopoShape& s, TopAbs_Orientation orient) {
        // Special case of single edge wire. Make sure the edge is in the
        // required orientation. This is necessary because BRepFill_OffsetWire
        // has special handling of circular edge offset, which seem to only
        // respect the edge orientation and disregard the wire orientation. The
        // orientation is used to determine whether to shrink or expand.
        if (s.countSubShapes(TopAbs_EDGE) == 1) {
            TopoDS_Shape e = s.getSubShape(TopAbs_EDGE, 1);
            if (e.Orientation() == orient) {
                if (s._Shape.Orientation() == orient) {
                    return;
                }
            }
            else {
                e = e.Oriented(orient);
            }
            BRepBuilderAPI_MakeWire mkWire(TopoDS::Edge(e));
            s.setShape(mkWire.Shape(), false);
        }
        else if (s._Shape.Orientation() != orient) {
            s.setShape(s._Shape.Oriented(orient), false);
        }
    };

    for (; it != wires.end(); ++it) {
        auto& wire = *it;
        if (wire.getShape().IsSame(tmp)) {
            if (inner) {
                for (++it; it != wires.end(); ++it) {
                    inner->push_back(*it);
                    if (reorient) {
                        doReorient(inner->back(), orientInner);
                    }
                }
            }
            auto res = wire;
            if (reorient) {
                doReorient(res, orientOuter);
            }
            return res;
        }
        if (inner) {
            inner->push_back(wire);
            if (reorient) {
                doReorient(inner->back(), orientInner);
            }
        }
    }
    return TopoShape();
}

struct MapperFill: Part::TopoShape::Mapper
{
    BRepFill_Generator& maker;
    explicit MapperFill(BRepFill_Generator& maker)
        : maker(maker)
    {}
    const std::vector<TopoDS_Shape>& generated(const TopoDS_Shape& s) const override
    {
        _res.clear();
        try {
            TopTools_ListIteratorOfListOfShape it;
            for (it.Initialize(maker.GeneratedShapes(s)); it.More(); it.Next()) {
                _res.push_back(it.Value());
            }
        }
        catch (const Standard_Failure& e) {
            if (FC_LOG_INSTANCE.isEnabled(FC_LOGLEVEL_LOG)) {
                FC_WARN("Exception on shape mapper: " << e.GetMessageString());
            }
        }
        return _res;
    }
};

// topo naming counterpart of TopoShape::makeShell()
TopoShape& TopoShape::makeElementShell(bool silent, const char* op)
{
    if (silent) {
        if (isNull()) {
            return *this;
        }

        if (shapeType(true) != TopAbs_COMPOUND) {
            return *this;
        }

        // we need a compound that consists of only faces
        TopExp_Explorer it;
        // no shells
        if (hasSubShape(TopAbs_SHELL)) {
            return *this;
        }

        // no wires outside a face
        it.Init(_Shape, TopAbs_WIRE, TopAbs_FACE);
        if (it.More()) {
            return *this;
        }

        // no edges outside a wire
        it.Init(_Shape, TopAbs_EDGE, TopAbs_WIRE);
        if (it.More()) {
            return *this;
        }

        // no vertexes outside an edge
        it.Init(_Shape, TopAbs_VERTEX, TopAbs_EDGE);
        if (it.More()) {
            return *this;
        }
    }
    else if (!hasSubShape(TopAbs_FACE)) {
        FC_THROWM(Base::CADKernelError, "Cannot make shell without face");
    }

    BRep_Builder builder;
    TopoDS_Shape shape;
    TopoDS_Shell shell;
    builder.MakeShell(shell);

    try {
        for (const auto& face : getSubShapes(TopAbs_FACE)) {
            builder.Add(shell, face);
        }

        TopoShape tmp(Tag, Hasher, shell);
        tmp.resetElementMap();
        tmp.mapSubElement(*this, op);

        shape = shell;
        BRepCheck_Analyzer check(shell);
        if (!check.IsValid()) {
            ShapeUpgrade_ShellSewing sewShell;
            shape = sewShell.ApplySewing(shell);
            // TODO confirm the above won't change OCCT topological naming
        }

        if (shape.IsNull()) {
            if (silent) {
                return *this;
            }
            FC_THROWM(NullShapeException, "Failed to make shell");
        }

        if (shape.ShapeType() != TopAbs_SHELL) {
            if (silent) {
                return *this;
            }
            FC_THROWM(Base::CADKernelError,
                      "Failed to make shell: unexpected output shape type "
                          << shapeType(shape.ShapeType(), true));
        }

        setShape(shape);
        resetElementMap(tmp.elementMap());
    }
    catch (Standard_Failure& e) {
        if (!silent) {
            FC_THROWM(Base::CADKernelError, "Failed to make shell: " << e.GetMessageString());
        }
    }

    return *this;
}

// TopoShape& TopoShape::makeElementShellFromWires(const std::vector<TopoShape>& wires,
//                                                 bool silent,
//                                                 const char* op)
// {
//     BRepFill_Generator maker;
//     for (auto& w : wires) {
//         if (w.shapeType(silent) == TopAbs_WIRE) {
//             maker.AddWire(TopoDS::Wire(w.getShape()));
//         }
//     }
//     if (wires.empty()) {
//         if (silent) {
//             _Shape.Nullify();
//             return *this;
//         }
//         FC_THROWM(NullShapeException, "No input shapes");
//     }
//     maker.Perform();
//     this->makeShapeWithElementMap(maker.Shell(), MapperFill(maker), wires, op);
//     return *this;
// }

}  // namespace Part
