/*
* This file is part of the CMaNGOS Project. See AUTHORS file for Copyright information
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
*/

#include "Globals/Locales.h"
#include "Models/M2Structure.h"
#include "Server/DBCStores.h"
#include "M2Structure.h"
#include "M2Stores.h"
#include "Common.h"
#include "Log.h"
#include "Server/DBCStructure.h"
#include "World/World.h"
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <unordered_map>

std::unordered_map<uint32, FlyByCameraCollection> sFlyByCameraStore;
std::unordered_map<uint32, AttachmentCollection> sModelAttachmentStore;

// Convert the geomoetry from a spline value, to an actual world XYZ
G3D::Vector3 TranslateLocation(G3D::Vector4 const* DBCPosition, G3D::Vector3 const* basePosition, G3D::Vector3 const* splineVector)
{
    G3D::Vector3 work;
    float x = basePosition->x + splineVector->x;
    float y = basePosition->y + splineVector->y;
    float z = basePosition->z + splineVector->z;
    float const distance = sqrt((x * x) + (y * y));
    float angle = std::atan2(x, y) - DBCPosition->w;

    if (angle < 0)
        angle += 2 * float(M_PI);

    work.x = DBCPosition->x + (distance * sin(angle));
    work.y = DBCPosition->y + (distance * cos(angle));
    work.z = DBCPosition->z + z;
    return work;
}

// Number of cameras not used. Multiple cameras never used in 3.3.5
bool readCamera(M2Camera const* cam, uint32 buffSize, M2Header const* header, CinematicCameraEntry const* dbcentry)
{
    char const* buffer = reinterpret_cast<char const*>(header);

    FlyByCameraCollection cameras;
    FlyByCameraCollection targetcam;

    G3D::Vector4 DBCData;
    DBCData.x = dbcentry->Origin.X;
    DBCData.y = dbcentry->Origin.Y;
    DBCData.z = dbcentry->Origin.Z;
    DBCData.w = dbcentry->OriginFacing;

    // Read target locations, only so that we can calculate orientation
    for (uint32 k = 0; k < cam->target_positions.timestamps.size; ++k)
    {
        // Extract Target positions
        if (cam->target_positions.timestamps.offset + sizeof(M2Array<uint32_t>) > buffSize)
            return false;
        M2Array<uint32_t> const* targTsArray = reinterpret_cast<M2Array<uint32_t> const*>(buffer + cam->target_positions.timestamps.offset);
        if (targTsArray->offset + sizeof(uint32) > buffSize || cam->target_positions.values.offset + sizeof(M2Array<uint32_t>) > buffSize)
            return false;
        uint32 const* targTimestamps = reinterpret_cast<uint32 const*>(buffer + targTsArray->offset);
        M2Array<uint32_t> const* targArray = reinterpret_cast<M2Array<uint32_t> const*>(buffer + cam->target_positions.values.offset);

        if (targArray->offset + sizeof(M2SplineKey<G3D::Vector3>) > buffSize)
            return false;
        M2SplineKey<G3D::Vector3> const* targPositions = reinterpret_cast<M2SplineKey<G3D::Vector3> const*>(buffer + targArray->offset);

        // Read the data for this set
        uint32 currPos = targArray->offset;
        for (uint32 i = 0; i < targTsArray->size; ++i)
        {
            if (currPos + sizeof(M2SplineKey<G3D::Vector3>) > buffSize)
                return false;
            // Translate co-ordinates
            G3D::Vector3 newPos = TranslateLocation(&DBCData, &cam->target_position_base, &targPositions->p0);

            // Add to vector
            FlyByCamera thisCam;
            thisCam.timeStamp = targTimestamps[i];
            thisCam.locations.x = newPos.x;
            thisCam.locations.y = newPos.y;
            thisCam.locations.z = newPos.z;
            thisCam.locations.w = 0.0f;
            targetcam.push_back(thisCam);
            targPositions++;
            currPos += sizeof(M2SplineKey<G3D::Vector3>);
        }
    }

    // Read camera positions and timestamps (translating first position of 3 only, we don't need to translate the whole spline)
    for (uint32 k = 0; k < cam->positions.timestamps.size; ++k)
    {
        // Extract Camera positions for this set
        if (cam->positions.timestamps.offset + sizeof(M2Array<uint32_t>) > buffSize)
            return false;
        M2Array<uint32_t> const* posTsArray = reinterpret_cast<M2Array<uint32_t> const*>(buffer + cam->positions.timestamps.offset);
        if (posTsArray->offset + sizeof(uint32) > buffSize || cam->positions.values.offset + sizeof(M2Array<uint32_t>) > buffSize)
            return false;
        uint32 const* posTimestamps = reinterpret_cast<uint32 const*>(buffer + posTsArray->offset);
        M2Array<uint32_t> const* posArray = reinterpret_cast<M2Array<uint32_t> const*>(buffer + cam->positions.values.offset);
        if (posArray->offset + sizeof(M2SplineKey<G3D::Vector3>) > buffSize)
            return false;
        M2SplineKey<G3D::Vector3> const* positions = reinterpret_cast<M2SplineKey<G3D::Vector3> const*>(buffer + posArray->offset);

        // Read the data for this set
        uint32 currPos = posArray->offset;
        for (uint32 i = 0; i < posTsArray->size; ++i)
        {
            if (currPos + sizeof(M2SplineKey<G3D::Vector3>) > buffSize)
                return false;
            // Translate co-ordinates
            G3D::Vector3 newPos = TranslateLocation(&DBCData, &cam->position_base, &positions->p0);

            // Add to vector
            FlyByCamera thisCam;
            thisCam.timeStamp = posTimestamps[i];
            thisCam.locations.x = newPos.x;
            thisCam.locations.y = newPos.y;
            thisCam.locations.z = newPos.z;

            if (targetcam.size() > 0)
            {
                // Find the target camera before and after this camera
                FlyByCamera lastTarget;
                FlyByCamera nextTarget;

                // Pre-load first item
                lastTarget = targetcam[0];
                nextTarget = targetcam[0];
                for (uint32 j = 0; j < targetcam.size(); ++j)
                {
                    nextTarget = targetcam[j];
                    if (targetcam[j].timeStamp > posTimestamps[i])
                        break;

                    lastTarget = targetcam[j];
                }

                float x = lastTarget.locations.x;
                float y = lastTarget.locations.y;
                float z = lastTarget.locations.z;

                // Now, the timestamps for target cam and position can be different. So, if they differ we interpolate
                if (lastTarget.timeStamp != posTimestamps[i])
                {
                    uint32 timeDiffTarget = nextTarget.timeStamp - lastTarget.timeStamp;
                    uint32 timeDiffThis = posTimestamps[i] - lastTarget.timeStamp;
                    float xDiff = nextTarget.locations.x - lastTarget.locations.x;
                    float yDiff = nextTarget.locations.y - lastTarget.locations.y;
                    float zDiff = nextTarget.locations.z - lastTarget.locations.z;
                    x = lastTarget.locations.x + (xDiff * (float(timeDiffThis) / float(timeDiffTarget)));
                    y = lastTarget.locations.y + (yDiff * (float(timeDiffThis) / float(timeDiffTarget)));
                    z = lastTarget.locations.z + (zDiff * (float(timeDiffThis) / float(timeDiffTarget)));
                }
                float xDiff = x - thisCam.locations.x;
                float yDiff = y - thisCam.locations.y;
                thisCam.locations.w = std::atan2(yDiff, xDiff);
            }

            cameras.push_back(thisCam);
            positions++;
            currPos += sizeof(M2SplineKey<G3D::Vector3>);
        }
    }

    sFlyByCameraStore[dbcentry->ID] = cameras;
    return true;
}

void LoadM2Cameras(std::string const& dataPath)
{
    sFlyByCameraStore.clear();

    uint32 oldMSTime = WorldTimer::getMSTime();
    for (uint32 i = 0; i < sCinematicCameraStore.GetNumRows(); ++i)
    {
        if (CinematicCameraEntry const* dbcentry = sCinematicCameraStore.LookupEntry(i))
        {
            std::string filename = dataPath;
            filename.append(dbcentry->Model);

            // Replace slashes
            std::replace(filename.begin(), filename.end(), '\\', '/');

            // Replace mdx to .m2
            size_t loc = filename.find(".mdx");
            if (loc != std::string::npos)
                filename.replace(loc, 4, ".m2");

            std::ifstream m2file(filename.c_str(), std::ios::in | std::ios::binary);
            if (!m2file.is_open())
                continue;

            // Get file size
            m2file.seekg(0, std::ios::end);
            std::streamoff const fileSize = m2file.tellg();

            // Reject if not at least the size of the header
            if (static_cast<uint32 const>(fileSize) < sizeof(M2Header))
            {
                sLog.outError("Camera file %s is damaged. File is smaller than header size", filename.c_str());
                m2file.close();
                continue;
            }

            // Read 4 bytes (signature)
            m2file.seekg(0, std::ios::beg);
            char fileCheck[5];
            m2file.read(fileCheck, 4);
            fileCheck[4] = 0;

            // Check file has correct magic (MD20)
            if (strcmp(fileCheck, "MD20"))
            {
                sLog.outError("Camera file %s is damaged. File identifier not found", filename.c_str());
                m2file.close();
                continue;
            }

            // Now we have a good file, read it all into a vector of char's, then close the file.
            std::vector<char> buffer(fileSize);
            m2file.seekg(0, std::ios::beg);
            if (!m2file.read(buffer.data(), fileSize))
            {
                m2file.close();
                continue;
            }
            m2file.close();

            // Read header
            M2Header const* header = reinterpret_cast<M2Header const*>(buffer.data());

            if (header->ofsCameras + sizeof(M2Camera) > static_cast<uint32 const>(fileSize))
            {
                sLog.outError("Camera file %s is damaged. Camera references position beyond file end (header)", filename.c_str());
                continue;
            }

            // Get camera(s) - Main header, then dump them.
            M2Camera const* cam = reinterpret_cast<M2Camera const*>(buffer.data() + header->ofsCameras);
            if (!readCamera(cam, fileSize, header, dbcentry))
                sLog.outError("Camera file %s is damaged. Camera references position beyond file end (camera)", filename.c_str());
        }
    }
    sLog.outString(">> Loaded %u cinematic waypoint sets in %u ms", static_cast<uint32 const>(sFlyByCameraStore.size()), WorldTimer::getMSTimeDiff(oldMSTime, WorldTimer::getMSTime()));
    sLog.outString();
}

void LoadM2Attachments(std::string const& dataPath)
{
    sModelAttachmentStore.clear();

    for (uint32 i = 0; i < sCreatureModelDataStore.GetNumRows(); ++i)
    {
        if (CreatureModelDataEntry const* dbcentry = sCreatureModelDataStore.LookupEntry(i))
        {
            std::string filename = dataPath;
            filename.append("CreatureModels/");
            std::string modelPath = dbcentry->ModelPath;
            std::replace(modelPath.begin(), modelPath.end(), '\\', '_');
            filename.append(modelPath);

            // Replace mdx to .m2
            size_t loc = filename.find(".mdx");
            if (loc != std::string::npos)
                filename.replace(loc, 4, ".m2");

            std::ifstream m2file(filename.c_str(), std::ios::in | std::ios::binary);
            if (!m2file.is_open())
                continue;

            // Get file size
            m2file.seekg(0, std::ios::end);
            std::streamoff const fileSize = m2file.tellg();
            if (fileSize == 0 || fileSize % sizeof(MiniM2Attachment))
            {
                sLog.outDetail("M2 Attachment File %s invalid", filename.c_str());
                m2file.close();
                continue;
            }

            m2file.seekg(std::ios::beg);
            uint32 numEles = fileSize / sizeof(MiniM2Attachment);
            std::vector<MiniM2Attachment> attachmentData(numEles);
            m2file.read(reinterpret_cast<char*>(attachmentData.data()), numEles * sizeof(MiniM2Attachment));

            sModelAttachmentStore[dbcentry->Id] = attachmentData;

            m2file.close();
        }
    }
}
