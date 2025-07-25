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

#ifndef GITREVISION_H
#define GITREVISION_H

#include "Define.h"

#include "revision_data.h"

namespace GitRevision
{
    // github data
    char const* GetHash();
    char const* GetDate();
    char const* GetBranch();
    char const* GetDepElunaHash();
    char const* GetDepElunaDate();
    char const* GetDepElunaBranch();
    char const* GetDepSD3Hash();
    char const* GetDepSD3Date();
    char const* GetDepSD3Branch();

    // system data
    char const* GetCMakeVersion();
    char const* GetHostOSVersion();
    char const* GetRunningSystem();

    // database data
    char const* GetProjectRevision();
    char const* GetRealmDBVersion();
    char const* GetRealmDBStructure();
    char const* GetRealmDBContent();
    char const* GetRealmDBUpdateDescription();

    char const* GetCharDBVersion();
    char const* GetCharDBStructure();
    char const* GetCharDBContent();
    char const* GetCharDBUpdateDescription();

    char const* GetWorldDBVersion();
    char const* GetWorldDBStructure();
    char const* GetWorldDBContent();
    char const* GetWorldDBUpdateDescription();

    // application data
    char const* GetFullRevision();
    char const* GetDepElunaFullRevisionStr();
    char const* GetDepElunaFullRevision();
    char const* GetDepSD3FullRevisionStr();
    char const* GetDepSD3FullRevision();
    char const* GetCompanyNameStr();
    char const* GetLegalCopyrightStr();
    char const* GetFileVersionStr();
    char const* GetProductVersionStr();
}

#endif
