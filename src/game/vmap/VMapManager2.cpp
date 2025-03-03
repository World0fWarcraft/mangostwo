/**
 * MaNGOS is a full featured server for World of Warcraft, supporting
 * the following clients: 1.12.x, 2.4.3, 3.3.5a, 4.3.4a and 5.4.8
 *
 * Copyright (C) 2005-2025 MaNGOS <https://www.getmangos.eu>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * World of Warcraft, and all World of Warcraft or Warcraft art, images,
 * and lore are copyrighted by Blizzard Entertainment, Inc.
 */

#include <iomanip>
#include <string>
#include <sstream>
#include "VMapManager2.h"
#include "MapTree.h"
#include "ModelInstance.h"
#include "WorldModel.h"
#include "VMapDefinitions.h"

using G3D::Vector3;

namespace VMAP
{
    //=========================================================

    VMapManager2::VMapManager2()
    {
    }

    //=========================================================

    VMapManager2::~VMapManager2(void)
    {
        for (InstanceTreeMap::iterator i = iInstanceMapTrees.begin(); i != iInstanceMapTrees.end(); ++i)
        {
            delete i->second;
        }
        for (ModelFileMap::iterator i = iLoadedModelFiles.begin(); i != iLoadedModelFiles.end(); ++i)
        {
            delete i->second.getModel();
        }
    }

    //=========================================================

    Vector3 VMapManager2::convertPositionToInternalRep(float x, float y, float z) const
    {
        Vector3 pos;
        const float mid = 0.5f * 64.0f * 533.33333333f;
        pos.x = mid - x;
        pos.y = mid - y;
        pos.z = z;

        return pos;
    }

    // move to MapTree too?
    std::string VMapManager2::getMapFileName(unsigned int pMapId)
    {
        std::stringstream fname;
        fname.width(3);
        fname << std::setfill('0') << pMapId << std::string(MAP_FILENAME_EXTENSION2);
        return fname.str();
    }

    //=========================================================

    VMAPLoadResult VMapManager2::loadMap(const char* pBasePath, unsigned int pMapId, int x, int y)
    {
        VMAPLoadResult result = VMAP_LOAD_RESULT_IGNORED;
        if (isMapLoadingEnabled())
        {
            if (_loadMap(pMapId, pBasePath, x, y))
            {
                result = VMAP_LOAD_RESULT_OK;
            }
            else
            {
                result = VMAP_LOAD_RESULT_ERROR;
            }
        }
        return result;
    }

    //=========================================================
    // load one tile (internal use only)

    bool VMapManager2::_loadMap(unsigned int pMapId, const std::string& basePath, uint32 tileX, uint32 tileY)
    {
        InstanceTreeMap::iterator instanceTree = iInstanceMapTrees.find(pMapId);
        if (instanceTree == iInstanceMapTrees.end())
        {
            std::string mapFileName = getMapFileName(pMapId);
            StaticMapTree* newTree = new StaticMapTree(pMapId, basePath);
            if (!newTree->InitMap(mapFileName, this))
            {
                return false;
            }
            instanceTree = iInstanceMapTrees.insert(InstanceTreeMap::value_type(pMapId, newTree)).first;
        }
        return instanceTree->second->LoadMapTile(tileX, tileY, this);
    }

    //=========================================================

    void VMapManager2::unloadMap(unsigned int pMapId)
    {
        InstanceTreeMap::iterator instanceTree = iInstanceMapTrees.find(pMapId);
        if (instanceTree != iInstanceMapTrees.end())
        {
            instanceTree->second->UnloadMap(this);
            if (instanceTree->second->numLoadedTiles() == 0)
            {
                delete instanceTree->second;
                iInstanceMapTrees.erase(pMapId);
            }
        }
    }

    //=========================================================

    void VMapManager2::unloadMap(unsigned int  pMapId, int x, int y)
    {
        InstanceTreeMap::iterator instanceTree = iInstanceMapTrees.find(pMapId);
        if (instanceTree != iInstanceMapTrees.end())
        {
            instanceTree->second->UnloadMapTile(x, y, this);
            if (instanceTree->second->numLoadedTiles() == 0)
            {
                delete instanceTree->second;
                iInstanceMapTrees.erase(pMapId);
            }
        }
    }

    //==========================================================

    bool VMapManager2::isInLineOfSight(unsigned int pMapId, float x1, float y1, float z1, float x2, float y2, float z2)
    {
        if (!isLineOfSightCalcEnabled() || IsVMAPDisabledForPtr(pMapId, VMAP_DISABLE_LOS))
        {
            return true;
        }

        bool result = true;
        InstanceTreeMap::iterator instanceTree = iInstanceMapTrees.find(pMapId);
        if (instanceTree != iInstanceMapTrees.end())
        {
            Vector3 pos1 = convertPositionToInternalRep(x1, y1, z1);
            Vector3 pos2 = convertPositionToInternalRep(x2, y2, z2);
            if (pos1 != pos2)
            {
                result = instanceTree->second->isInLineOfSight(pos1, pos2);
            }
        }
        return result;
    }
    //=========================================================
    /**
    get the hit position and return true if we hit something
    otherwise the result pos will be the dest pos
    */
    bool VMapManager2::getObjectHitPos(unsigned int pMapId, float x1, float y1, float z1, float x2, float y2, float z2, float& rx, float& ry, float& rz, float pModifyDist)
    {
        bool result = false;
        rx = x2;
        ry = y2;
        rz = z2;
        if (isLineOfSightCalcEnabled() && !IsVMAPDisabledForPtr(pMapId, VMAP_DISABLE_LOS))
        {
            InstanceTreeMap::iterator instanceTree = iInstanceMapTrees.find(pMapId);
            if (instanceTree != iInstanceMapTrees.end())
            {
                Vector3 pos1 = convertPositionToInternalRep(x1, y1, z1);
                Vector3 pos2 = convertPositionToInternalRep(x2, y2, z2);
                Vector3 resultPos;
                result = instanceTree->second->getObjectHitPos(pos1, pos2, resultPos, pModifyDist);
                resultPos = convertPositionToInternalRep(resultPos.x, resultPos.y, resultPos.z);
                rx = resultPos.x;
                ry = resultPos.y;
                rz = resultPos.z;
            }
        }
        return result;
    }

    //=========================================================
    /**
    get height or INVALID_HEIGHT if no height available
    */

    float VMapManager2::getHeight(unsigned int pMapId, float x, float y, float z, float maxSearchDist)
    {
        float height = VMAP_INVALID_HEIGHT_VALUE;           // no height
        if (isHeightCalcEnabled() && !IsVMAPDisabledForPtr(pMapId, VMAP_DISABLE_HEIGHT))
        {
            InstanceTreeMap::iterator instanceTree = iInstanceMapTrees.find(pMapId);
            if (instanceTree != iInstanceMapTrees.end())
            {
                Vector3 pos = convertPositionToInternalRep(x, y, z);
                height = instanceTree->second->getHeight(pos, maxSearchDist);
                if (!(height < G3D::inf()))
                {
                    height = VMAP_INVALID_HEIGHT_VALUE;     // no height
                }
            }
        }
        return height;
    }

    //=========================================================

    bool VMapManager2::getAreaInfo(unsigned int pMapId, float x, float y, float& z, uint32& flags, int32& adtId, int32& rootId, int32& groupId) const
    {
        bool result = false;
        if (!IsVMAPDisabledForPtr(pMapId, VMAP_DISABLE_AREAFLAG))
        {
            InstanceTreeMap::const_iterator instanceTree = iInstanceMapTrees.find(pMapId);
            if (instanceTree != iInstanceMapTrees.end())
            {
                Vector3 pos = convertPositionToInternalRep(x, y, z);
                result = instanceTree->second->getAreaInfo(pos, flags, adtId, rootId, groupId);
                // z is not touched by convertPositionToMangosRep(), so just copy
                z = pos.z;
            }
        }
        return result;
    }

    bool VMapManager2::GetLiquidLevel(uint32 pMapId, float x, float y, float z, uint8 ReqLiquidType, float& level, float& floor, uint32& type) const
    {
        if (!IsVMAPDisabledForPtr(pMapId, VMAP_DISABLE_LIQUIDSTATUS))
        {
            InstanceTreeMap::const_iterator instanceTree = iInstanceMapTrees.find(pMapId);
            if (instanceTree != iInstanceMapTrees.end())
            {
                LocationInfo info;
                Vector3 pos = convertPositionToInternalRep(x, y, z);
                if (instanceTree->second->GetLocationInfo(pos, info))
                {
                    floor = info.ground_Z;
                    type = info.hitModel->GetLiquidType();
                    if (ReqLiquidType && !(type & ReqLiquidType))
                    {
                        return false;
                    }
                    if (info.hitInstance->GetLiquidLevel(pos, info, level))
                    {
                        return true;
                    }
                }
            }
        }
        return false;
    }

    //=========================================================

    WorldModel* VMapManager2::acquireModelInstance(const std::string& basepath, const std::string& filename, uint32 flags/* Only used when creating the model */)
    {
        ModelFileMap::iterator model = iLoadedModelFiles.find(filename);
        if (model == iLoadedModelFiles.end())
        {
            WorldModel* worldmodel = new WorldModel();
            if (!worldmodel->ReadFile(basepath + filename + ".vmo"))
            {
                ERROR_LOG("VMapManager2: could not load '%s%s.vmo'!", basepath.c_str(), filename.c_str());
                delete worldmodel;
                return NULL;
            }
            DEBUG_FILTER_LOG(LOG_FILTER_MAP_LOADING, "VMapManager2: loading file '%s%s'.", basepath.c_str(), filename.c_str());
            worldmodel->Flags = flags;
            model = iLoadedModelFiles.insert(std::pair<std::string, ManagedModel>(filename, ManagedModel())).first;
            model->second.setModel(worldmodel);
        }
        model->second.incRefCount();
        return model->second.getModel();
    }

    void VMapManager2::releaseModelInstance(const std::string& filename)
    {
        ModelFileMap::iterator model = iLoadedModelFiles.find(filename);
        if (model == iLoadedModelFiles.end())
        {
            ERROR_LOG("VMapManager2: trying to unload non-loaded file '%s'!", filename.c_str());
            return;
        }
        if (model->second.decRefCount() == 0)
        {
            DEBUG_FILTER_LOG(LOG_FILTER_MAP_LOADING, "VMapManager2: unloading file '%s'", filename.c_str());
            delete model->second.getModel();
            iLoadedModelFiles.erase(model);
        }
    }
    //=========================================================

    bool VMapManager2::existsMap(const char* pBasePath, unsigned int pMapId, int x, int y)
    {
        return StaticMapTree::CanLoadMap(std::string(pBasePath), pMapId, x, y);
    }
} // namespace VMAP
