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

#include <zlib.h>
#include "Common.h"
#include "Language.h"
#include "Database/DatabaseEnv.h"
#include "Database/DatabaseImpl.h"
#include "WorldPacket.h"
#include "Opcodes.h"
#include "Log.h"
#include "Player.h"
#include "World.h"
#include "GuildMgr.h"
#include "ObjectMgr.h"
#include "WorldSession.h"
#include "Auth/BigNumber.h"
#include "Auth/Sha1.h"
#include "UpdateData.h"
#include "LootMgr.h"
#include "Chat.h"
#include "ScriptMgr.h"
#include "ObjectAccessor.h"
#include "Object.h"
#include "BattleGround/BattleGround.h"
#include "OutdoorPvP/OutdoorPvP.h"
#include "Pet.h"
#include "SocialMgr.h"
#include "DBCEnums.h"
#ifdef ENABLE_ELUNA
#include "LuaEngine.h"
#endif /* ENABLE_ELUNA */

void WorldSession::HandleRepopRequestOpcode(WorldPacket& recv_data)
{
    DEBUG_LOG("WORLD: Received opcode CMSG_REPOP_REQUEST");

    recv_data.read_skip<uint8>();

    if (GetPlayer()->IsAlive() || GetPlayer()->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST))
    {
        return;
    }

    if (GetPlayer()->HasAuraType(SPELL_AURA_PREVENT_RESURRECTION))
    {
        return;
    }

    // the world update order is sessions, players, creatures
    // the netcode runs in parallel with all of these
    // creatures can kill players
    // so if the server is lagging enough the player can
    // release spirit after he's killed but before he is updated
    if (GetPlayer()->GetDeathState() == JUST_DIED)
    {
        DEBUG_LOG("HandleRepopRequestOpcode: got request after player %s(%d) was killed and before he was updated", GetPlayer()->GetName(), GetPlayer()->GetGUIDLow());
        GetPlayer()->KillPlayer();
    }

    // Used by Eluna
#ifdef ENABLE_ELUNA
    if (Eluna* e = GetPlayer()->GetEluna())
    {
        e->OnRepop(GetPlayer());
    }
#endif /* ENABLE_ELUNA */

    // this is spirit release confirm?
    GetPlayer()->RemovePet(PET_SAVE_REAGENTS);
    GetPlayer()->BuildPlayerRepop();
    GetPlayer()->RepopAtGraveyard();
}

void WorldSession::HandleWhoOpcode(WorldPacket& recv_data)
{
    DEBUG_LOG("WORLD: Received opcode CMSG_WHO");
    // recv_data.hexlike();

    uint32 clientcount = 0;

    uint32 level_min, level_max, racemask, classmask, zones_count, str_count;
    uint32 zoneids[10];                                     // 10 is client limit
    std::string player_name, guild_name;

    recv_data >> level_min;                                 // maximal player level, default 0
    recv_data >> level_max;                                 // minimal player level, default 100 (MAX_LEVEL)
    recv_data >> player_name;                               // player name, case sensitive...

    recv_data >> guild_name;                                // guild name, case sensitive...

    recv_data >> racemask;                                  // race mask
    recv_data >> classmask;                                 // class mask
    recv_data >> zones_count;                               // zones count, client limit=10 (2.0.10)

    if (zones_count > 10)
    {
        return;                                              // can't be received from real client or broken packet
    }

    for (uint32 i = 0; i < zones_count; ++i)
    {
        uint32 temp;
        recv_data >> temp;                                  // zone id, 0 if zone is unknown...
        zoneids[i] = temp;
        DEBUG_LOG("Zone %u: %u", i, zoneids[i]);
    }

    recv_data >> str_count;                                 // user entered strings count, client limit=4 (checked on 2.0.10)

    if (str_count > 4)
    {
        return;                                              // can't be received from real client or broken packet
    }

    DEBUG_LOG("Minlvl %u, maxlvl %u, name %s, guild %s, racemask %u, classmask %u, zones %u, strings %u", level_min, level_max, player_name.c_str(), guild_name.c_str(), racemask, classmask, zones_count, str_count);

    std::wstring str[4];                                    // 4 is client limit
    for (uint32 i = 0; i < str_count; ++i)
    {
        std::string temp;
        recv_data >> temp;                                  // user entered string, it used as universal search pattern(guild+player name)?

        if (!Utf8toWStr(temp, str[i]))
        {
            continue;
        }

        wstrToLower(str[i]);

        DEBUG_LOG("String %u: %s", i, temp.c_str());
    }

    std::wstring wplayer_name;
    std::wstring wguild_name;
    if (!(Utf8toWStr(player_name, wplayer_name) && Utf8toWStr(guild_name, wguild_name)))
    {
        return;
    }
    wstrToLower(wplayer_name);
    wstrToLower(wguild_name);

    // client send in case not set max level value 100 but mangos support 255 max level,
    // update it to show GMs with characters after 100 level
    if (level_max >= MAX_LEVEL)
    {
        level_max = STRONG_MAX_LEVEL;
    }

    Team team = _player->GetTeam();
    uint32 security = GetSecurity();
    bool allowTwoSideWhoList = sWorld.getConfig(CONFIG_BOOL_ALLOW_TWO_SIDE_WHO_LIST);
    AccountTypes gmLevelInWhoList = (AccountTypes)sWorld.getConfig(CONFIG_UINT32_GM_LEVEL_IN_WHO_LIST);

    WorldPacket data(SMSG_WHO, 50);                         // guess size
    data << uint32(clientcount);                            // clientcount place holder, listed count
    data << uint32(clientcount);                            // clientcount place holder, online count

    uint32 count = 0;
    sObjectAccessor.DoForAllPlayers([&](Player* pl)->void
    {
        ++count;

        if (clientcount == 50)
        {
            return;
        }

        if (security == SEC_PLAYER)
        {
            // player can see member of other team only if CONFIG_BOOL_ALLOW_TWO_SIDE_WHO_LIST
            if (pl->GetTeam() != team && !allowTwoSideWhoList)
            {
                return;
            }

            // player can see MODERATOR, GAME MASTER, ADMINISTRATOR only if CONFIG_GM_IN_WHO_LIST
            if (pl->GetSession()->GetSecurity() > gmLevelInWhoList)
            {
                return;
            }
        }

        // do not process players which are not in world
        if (!pl->IsInWorld())
        {
            return;
        }

        // check if target is globally visible for player
        if (!pl->IsVisibleGloballyFor(_player))
        {
            return;
        }

        // check if target's level is in level range
        uint32 lvl = pl->getLevel();
        if (lvl < level_min || lvl > level_max)
        {
            return;
        }

        // check if class matches classmask
        uint32 class_ = pl->getClass();
        if (!(classmask & (1 << class_)))
        {
            return;
        }

        // check if race matches racemask
        uint32 race = pl->getRace();
        if (!(racemask & (1 << race)))
        {
            return;
        }

        uint32 pzoneid = pl->GetZoneId();
        uint8 gender = pl->getGender();

        bool z_show = true;
        for (uint32 i = 0; i < zones_count; ++i)
        {
            if (zoneids[i] == pzoneid)
            {
                z_show = true;
                break;
            }

            z_show = false;
        }
        if (!z_show)
        {
            return;
        }

        std::string pname = pl->GetName();
        std::wstring wpname;
        if (!Utf8toWStr(pname, wpname))
        {
            return;
        }
        wstrToLower(wpname);

        if (!(wplayer_name.empty() || wpname.find(wplayer_name) != std::wstring::npos))
        {
            return;
        }

        std::string gname = sGuildMgr.GetGuildNameById(pl->GetGuildId());
        std::wstring wgname;
        if (!Utf8toWStr(gname, wgname))
        {
            return;
        }
        wstrToLower(wgname);

        if (!(wguild_name.empty() || wgname.find(wguild_name) != std::wstring::npos))
        {
            return;
        }

        std::string aname;
        if (AreaTableEntry const* areaEntry = GetAreaEntryByAreaID(pzoneid))
        {
            aname = areaEntry->area_name[GetSessionDbcLocale()];
        }

        bool s_show = true;
        for (uint32 i = 0; i < str_count; ++i)
        {
            if (!str[i].empty())
            {
                if (wgname.find(str[i]) != std::wstring::npos ||
                    wpname.find(str[i]) != std::wstring::npos ||
                    Utf8FitTo(aname, str[i]))
                {
                    s_show = true;
                    break;
                }
                s_show = false;
            }
        }
        if (!s_show)
        {
            return;
        }

        data << pname;                                      // player name
        data << gname;                                      // guild name
        data << uint32(lvl);                                // player level
        data << uint32(class_);                             // player class
        data << uint32(race);                               // player race
        data << uint8(gender);                              // player gender
        data << uint32(pzoneid);                            // player zone id

        ++clientcount;
    });

    data.put(0, clientcount);                               // insert right count, listed count
    data.put(4, count > 50 ? count : clientcount);          // insert right count, online count

    SendPacket(&data);
    DEBUG_LOG("WORLD: Send SMSG_WHO Message");
}

void WorldSession::HandleLogoutRequestOpcode(WorldPacket& /*recv_data*/)
{
    DEBUG_LOG("WORLD: Received opcode CMSG_LOGOUT_REQUEST, security %u", GetSecurity());

    if (ObjectGuid lootGuid = GetPlayer()->GetLootGuid())
    {
        DoLootRelease(lootGuid);
    }

    // Can not logout if...
    if (GetPlayer()->IsInCombat() ||                        //...is in combat
            //...is jumping ...is falling
            GetPlayer()->m_movementInfo.HasMovementFlag(MovementFlags(MOVEFLAG_FALLING | MOVEFLAG_FALLINGFAR)))
    {
        WorldPacket data(SMSG_LOGOUT_RESPONSE, 5);
        data << uint32(1);
        data << uint8(0);
        SendPacket(&data);
        LogoutRequest(0);
        return;
    }

    // instant logout in taverns/cities or on taxi or for admins, gm's, mod's if its enabled in mangosd.conf
    if (GetPlayer()->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_RESTING) || GetPlayer()->IsTaxiFlying() ||
        GetSecurity() >= (AccountTypes)sWorld.getConfig(CONFIG_UINT32_INSTANT_LOGOUT))
    {
        LogoutPlayer(true);
        return;
    }

    // not set flags if player can't free move to prevent lost state at logout cancel
    if (GetPlayer()->CanFreeMove())
    {
        float height = GetPlayer()->GetMap()->GetHeight(GetPlayer()->GetPhaseMask(), GetPlayer()->GetPositionX(), GetPlayer()->GetPositionY(), GetPlayer()->GetPositionZ());
        if ((GetPlayer()->GetPositionZ() < height + 0.1f) && !(GetPlayer()->IsInWater()))
        {
            GetPlayer()->SetStandState(UNIT_STAND_STATE_SIT);
        }

        GetPlayer()->SetRoot(true);
        GetPlayer()->SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_STUNNED);
    }

    WorldPacket data(SMSG_LOGOUT_RESPONSE, 5);
    data << uint32(0);
    data << uint8(0);
    SendPacket(&data);
    LogoutRequest(time(NULL));
}

void WorldSession::HandlePlayerLogoutOpcode(WorldPacket& /*recv_data*/)
{
    DEBUG_LOG("WORLD: Received opcode CMSG_PLAYER_LOGOUT Message");
}

void WorldSession::HandleLogoutCancelOpcode(WorldPacket& /*recv_data*/)
{
    DEBUG_LOG("WORLD: Received opcode CMSG_LOGOUT_CANCEL Message");

    LogoutRequest(0);

    WorldPacket data(SMSG_LOGOUT_CANCEL_ACK, 0);
    SendPacket(&data);

    // not remove flags if can't free move - its not set in Logout request code.
    if (GetPlayer()->CanFreeMove())
    {
        //!we can move again
        GetPlayer()->SetRoot(false);

        //! Stand Up
        GetPlayer()->SetStandState(UNIT_STAND_STATE_STAND);

        //! DISABLE_ROTATE
        GetPlayer()->RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_STUNNED);
    }

    DEBUG_LOG("WORLD: sent SMSG_LOGOUT_CANCEL_ACK Message");
}

void WorldSession::HandleTogglePvP(WorldPacket& recv_data)
{
    // this opcode can be used in two ways: Either set explicit new status or toggle old status
    if (recv_data.size() == 1)
    {
        bool newPvPStatus;
        recv_data >> newPvPStatus;
        GetPlayer()->ApplyModFlag(PLAYER_FLAGS, PLAYER_FLAGS_IN_PVP, newPvPStatus);
        GetPlayer()->ApplyModFlag(PLAYER_FLAGS, PLAYER_FLAGS_PVP_TIMER, !newPvPStatus);
    }
    else
    {
        GetPlayer()->ToggleFlag(PLAYER_FLAGS, PLAYER_FLAGS_IN_PVP);
        GetPlayer()->ToggleFlag(PLAYER_FLAGS, PLAYER_FLAGS_PVP_TIMER);
    }

    if (GetPlayer()->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_IN_PVP))
    {
        if (!GetPlayer()->IsPvP() || GetPlayer()->pvpInfo.endTimer != 0)
        {
            GetPlayer()->UpdatePvP(true, true);
        }
    }
    else
    {
        if (!GetPlayer()->pvpInfo.inHostileArea && GetPlayer()->IsPvP())
        {
            GetPlayer()->pvpInfo.endTimer = time(NULL);      // start toggle-off
        }
    }
}

void WorldSession::HandleZoneUpdateOpcode(WorldPacket& recv_data)
{
    uint32 newZone;
    recv_data >> newZone;

    DETAIL_LOG("WORLD: Received opcode CMSG_ZONEUPDATE: newzone is %u", newZone);

    // use server side data
    uint32 newzone, newarea;
    GetPlayer()->GetZoneAndAreaId(newzone, newarea);
    GetPlayer()->UpdateZone(newzone, newarea);
}

void WorldSession::HandleSetTargetOpcode(WorldPacket& recv_data)
{
    // When this packet send?
    ObjectGuid guid ;
    recv_data >> guid;

    _player->SetTargetGuid(guid);

    // update reputation list if need
    Unit* unit = sObjectAccessor.GetUnit(*_player, guid);   // can select group members at diff maps
    if (!unit)
    {
        return;
    }

    if (FactionTemplateEntry const* factionTemplateEntry = sFactionTemplateStore.LookupEntry(unit->getFaction()))
    {
        _player->GetReputationMgr().SetVisible(factionTemplateEntry);
    }
}

void WorldSession::HandleSetSelectionOpcode(WorldPacket& recv_data)
{
    ObjectGuid guid;
    recv_data >> guid;

    _player->SetSelectionGuid(guid);

    // update reputation list if need
    Unit* unit = sObjectAccessor.GetUnit(*_player, guid);   // can select group members at diff maps
    if (!unit)
    {
        return;
    }

    if (FactionTemplateEntry const* factionTemplateEntry = sFactionTemplateStore.LookupEntry(unit->getFaction()))
    {
        _player->GetReputationMgr().SetVisible(factionTemplateEntry);
    }
}

void WorldSession::HandleStandStateChangeOpcode(WorldPacket& recv_data)
{
    // DEBUG_LOG("WORLD: Received opcode CMSG_STANDSTATECHANGE"); -- too many spam in log at lags/debug stop
    uint32 animstate;
    recv_data >> animstate;

    _player->SetStandState(animstate);
}

void WorldSession::HandleContactListOpcode(WorldPacket& recv_data)
{
    DEBUG_LOG("WORLD: Received opcode CMSG_CONTACT_LIST");
    uint32 unk;
    recv_data >> unk;
    DEBUG_LOG("unk value is %u", unk);
    _player->GetSocial()->SendSocialList();
}

void WorldSession::HandleAddFriendOpcode(WorldPacket& recv_data)
{
    DEBUG_LOG("WORLD: Received opcode CMSG_ADD_FRIEND");

    std::string friendName = GetMangosString(LANG_FRIEND_IGNORE_UNKNOWN);
    std::string friendNote;

    recv_data >> friendName;

    recv_data >> friendNote;

    if (!normalizePlayerName(friendName))
    {
        return;
    }

    CharacterDatabase.escape_string(friendName);            // prevent SQL injection - normal name don't must changed by this call

    DEBUG_LOG("WORLD: %s asked to add friend : '%s'",
              GetPlayer()->GetName(), friendName.c_str());

    CharacterDatabase.AsyncPQuery(&WorldSession::HandleAddFriendOpcodeCallBack, GetAccountId(), friendNote, "SELECT `guid`, `race` FROM `characters` WHERE `name` = '%s'", friendName.c_str());
}

void WorldSession::HandleAddFriendOpcodeCallBack(QueryResult* result, uint32 accountId, std::string friendNote)
{
    if (!result)
    {
        return;
    }

    uint32 friendLowGuid = (*result)[0].GetUInt32();
    ObjectGuid friendGuid = ObjectGuid(HIGHGUID_PLAYER, friendLowGuid);
    Team team = Player::TeamForRace((*result)[1].GetUInt8());

    delete result;

    WorldSession* session = sWorld.FindSession(accountId);
    if (!session)
    {
        return;
    }

    Player* player = session->GetPlayer();
    if (!player)
    {
        return;
    }

    FriendsResult friendResult = FRIEND_NOT_FOUND;
    if (friendGuid)
    {
        if (friendGuid == player->GetObjectGuid())
        {
            friendResult = FRIEND_SELF;
        }
        else if (player->GetTeam() != team && !sWorld.getConfig(CONFIG_BOOL_ALLOW_TWO_SIDE_ADD_FRIEND) && session->GetSecurity() < SEC_MODERATOR)
        {
            friendResult = FRIEND_ENEMY;
        }
        else if (player->GetSocial()->HasFriend(friendGuid))
        {
            friendResult = FRIEND_ALREADY;
        }
        else
        {
            Player* pFriend = sObjectAccessor.FindPlayer(friendGuid);
            if (pFriend && pFriend->IsInWorld() && pFriend->IsVisibleGloballyFor(player))
            {
                friendResult = FRIEND_ADDED_ONLINE;
            }
            else
            {
                friendResult = FRIEND_ADDED_OFFLINE;
            }

            if (!player->GetSocial()->AddToSocialList(friendGuid, false))
            {
                friendResult = FRIEND_LIST_FULL;
                DEBUG_LOG("WORLD: %s's friend list is full.", player->GetName());
            }

            player->GetSocial()->SetFriendNote(friendGuid, friendNote);
        }
    }

    sSocialMgr.SendFriendStatus(player, friendResult, friendGuid, false);

    DEBUG_LOG("WORLD: Sent (SMSG_FRIEND_STATUS)");
}

void WorldSession::HandleDelFriendOpcode(WorldPacket& recv_data)
{
    ObjectGuid friendGuid;

    DEBUG_LOG("WORLD: Received opcode CMSG_DEL_FRIEND");

    recv_data >> friendGuid;

    _player->GetSocial()->RemoveFromSocialList(friendGuid, false);

    sSocialMgr.SendFriendStatus(GetPlayer(), FRIEND_REMOVED, friendGuid, false);

    DEBUG_LOG("WORLD: Sent motd (SMSG_FRIEND_STATUS)");
}

void WorldSession::HandleAddIgnoreOpcode(WorldPacket& recv_data)
{
    DEBUG_LOG("WORLD: Received opcode CMSG_ADD_IGNORE");

    std::string IgnoreName = GetMangosString(LANG_FRIEND_IGNORE_UNKNOWN);

    recv_data >> IgnoreName;

    if (!normalizePlayerName(IgnoreName))
    {
        return;
    }

    CharacterDatabase.escape_string(IgnoreName);            // prevent SQL injection - normal name don't must changed by this call

    DEBUG_LOG("WORLD: %s asked to Ignore: '%s'",
              GetPlayer()->GetName(), IgnoreName.c_str());

    CharacterDatabase.AsyncPQuery(&WorldSession::HandleAddIgnoreOpcodeCallBack, GetAccountId(), "SELECT `guid` FROM `characters` WHERE `name` = '%s'", IgnoreName.c_str());
}

void WorldSession::HandleAddIgnoreOpcodeCallBack(QueryResult* result, uint32 accountId)
{
    if (!result)
    {
        return;
    }

    uint32 ignoreLowGuid = (*result)[0].GetUInt32();
    ObjectGuid ignoreGuid = ObjectGuid(HIGHGUID_PLAYER, ignoreLowGuid);

    delete result;

    WorldSession* session = sWorld.FindSession(accountId);
    if (!session)
    {
        return;
    }

    Player* player = session->GetPlayer();
    if (!player)
    {
        return;
    }

    FriendsResult ignoreResult = FRIEND_IGNORE_NOT_FOUND;
    if (ignoreGuid)
    {
        if (ignoreGuid == player->GetObjectGuid())
        {
            ignoreResult = FRIEND_IGNORE_SELF;
        }
        else if (player->GetSocial()->HasIgnore(ignoreGuid))
        {
            ignoreResult = FRIEND_IGNORE_ALREADY;
        }
        else
        {
            ignoreResult = FRIEND_IGNORE_ADDED;

            // ignore list full
            if (!player->GetSocial()->AddToSocialList(ignoreGuid, true))
            {
                ignoreResult = FRIEND_IGNORE_FULL;
            }
        }
    }

    sSocialMgr.SendFriendStatus(player, ignoreResult, ignoreGuid, false);

    DEBUG_LOG("WORLD: Sent (SMSG_FRIEND_STATUS)");
}

void WorldSession::HandleDelIgnoreOpcode(WorldPacket& recv_data)
{
    ObjectGuid ignoreGuid;

    DEBUG_LOG("WORLD: Received opcode CMSG_DEL_IGNORE");

    recv_data >> ignoreGuid;

    _player->GetSocial()->RemoveFromSocialList(ignoreGuid, true);

    sSocialMgr.SendFriendStatus(GetPlayer(), FRIEND_IGNORE_REMOVED, ignoreGuid, false);

    DEBUG_LOG("WORLD: Sent motd (SMSG_FRIEND_STATUS)");
}

void WorldSession::HandleSetContactNotesOpcode(WorldPacket& recv_data)
{
    DEBUG_LOG("WORLD: Received opcode CMSG_SET_CONTACT_NOTES");
    ObjectGuid guid;
    std::string note;
    recv_data >> guid >> note;
    _player->GetSocial()->SetFriendNote(guid, note);
}

void WorldSession::HandleBugOpcode(WorldPacket& recv_data)
{
    uint32 suggestion, contentlen, typelen;
    std::string content, type;

    recv_data >> suggestion >> contentlen >> content;

    recv_data >> typelen >> type;

    if (suggestion == 0)
    {
        DEBUG_LOG("WORLD: Received opcode CMSG_BUG [Bug Report]");
    }
    else
    {
        DEBUG_LOG("WORLD: Received opcode CMSG_BUG [Suggestion]");
    }

    DEBUG_LOG("%s", type.c_str());
    DEBUG_LOG("%s", content.c_str());

    CharacterDatabase.escape_string(type);
    CharacterDatabase.escape_string(content);
    CharacterDatabase.PExecute("INSERT INTO `bugreport` (`type`,`content`) VALUES('%s', '%s')", type.c_str(), content.c_str());
}

void WorldSession::HandleReclaimCorpseOpcode(WorldPacket& recv_data)
{
    DETAIL_LOG("WORLD: Received opcode CMSG_RECLAIM_CORPSE");

    ObjectGuid guid;
    recv_data >> guid;

    if (GetPlayer()->IsAlive())
    {
        return;
    }

    // do not allow corpse reclaim in arena
    if (GetPlayer()->InArena())
    {
        return;
    }

    // body not released yet
    if (!GetPlayer()->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST))
    {
        return;
    }

    Corpse* corpse = GetPlayer()->GetCorpse();

    if (!corpse)
    {
        return;
    }

    // prevent resurrect before 30-sec delay after body release not finished
    if (corpse->GetGhostTime() + GetPlayer()->GetCorpseReclaimDelay(corpse->GetType() == CORPSE_RESURRECTABLE_PVP) > time(NULL))
    {
        return;
    }

    if (!corpse->IsWithinDistInMap(GetPlayer(), CORPSE_RECLAIM_RADIUS, true))
    {
        return;
    }

    // resurrect
    GetPlayer()->ResurrectPlayer(GetPlayer()->InBattleGround() ? 1.0f : 0.5f);

    // spawn bones
    GetPlayer()->SpawnCorpseBones();
}

void WorldSession::HandleResurrectResponseOpcode(WorldPacket& recv_data)
{
    DETAIL_LOG("WORLD: Received opcode CMSG_RESURRECT_RESPONSE");

    ObjectGuid guid;
    uint8 status;
    recv_data >> guid;
    recv_data >> status;

    if (GetPlayer()->IsAlive())
    {
        return;
    }

    if (status == 0)
    {
        GetPlayer()->clearResurrectRequestData();           // reject
        return;
    }

    if (!GetPlayer()->isRessurectRequestedBy(guid))
    {
        return;
    }

    GetPlayer()->ResurectUsingRequestData();                // will call spawncorpsebones
}

void WorldSession::HandleAreaTriggerOpcode(WorldPacket& recv_data)
{
    DEBUG_LOG("WORLD: Received opcode CMSG_AREATRIGGER");

    uint32 Trigger_ID;

    recv_data >> Trigger_ID;
    DEBUG_LOG("Trigger ID: %u", Trigger_ID);
    Player* player = GetPlayer();

    if (player->IsTaxiFlying())
    {
        DEBUG_LOG("Player '%s' (GUID: %u) in flight, ignore Area Trigger ID: %u", player->GetName(), player->GetGUIDLow(), Trigger_ID);
        return;
    }

    AreaTriggerEntry const* atEntry = sAreaTriggerStore.LookupEntry(Trigger_ID);
    if (!atEntry)
    {
        DEBUG_LOG("Player '%s' (GUID: %u) send unknown (by DBC) Area Trigger ID: %u", player->GetName(), player->GetGUIDLow(), Trigger_ID);
        return;
    }

    // delta is safe radius
    const float delta = 5.0f;

    // check if player in the range of areatrigger
    if (!IsPointInAreaTriggerZone(atEntry, player->GetMapId(), player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), delta))
    {
        DEBUG_LOG("Player '%s' (GUID: %u) too far, ignore Area Trigger ID: %u", player->GetName(), player->GetGUIDLow(), Trigger_ID);
        return;
    }

    if (sScriptMgr.OnAreaTrigger(player, atEntry))
    {
        return;
    }

    uint32 quest_id = sObjectMgr.GetQuestForAreaTrigger(Trigger_ID);
    if (quest_id && player->IsAlive() && player->IsActiveQuest(quest_id))
    {
        Quest const* pQuest = sObjectMgr.GetQuestTemplate(quest_id);
        if (pQuest)
        {
            if (player->GetQuestStatus(quest_id) == QUEST_STATUS_INCOMPLETE)
            {
                player->AreaExploredOrEventHappens(quest_id);
            }
        }
    }

    // enter to tavern, not overwrite city rest
    if (sObjectMgr.IsTavernAreaTrigger(Trigger_ID))
    {
        // set resting flag we are in the inn
        if (player->GetRestType() != REST_TYPE_IN_CITY)
        {
            player->SetRestType(REST_TYPE_IN_TAVERN, Trigger_ID);
        }
        return;
    }

    if (BattleGround* bg = player->GetBattleGround())
    {
        if (bg->HandleAreaTrigger(player, Trigger_ID))
        {
            return;
        }
    }
    else if (OutdoorPvP* outdoorPvP = sOutdoorPvPMgr.GetScript(player->GetCachedZoneId()))
    {
        if (outdoorPvP->HandleAreaTrigger(player, Trigger_ID))
        {
            return;
        }
    }

    // NULL if all values default (non teleport trigger)
    AreaTrigger const* at = sObjectMgr.GetAreaTrigger(Trigger_ID);
    if (!at)
    {
        return;
    }

    MapEntry const* targetMapEntry = sMapStore.LookupEntry(at->target_mapId);
    if (!targetMapEntry)
    {
        return;
    }

    // ghost resurrected at enter attempt to dungeon with corpse (including fail enter cases)
    if (!player->IsAlive() && targetMapEntry->IsDungeon())
    {
        int32 corpseMapId = 0;
        if (Corpse* corpse = player->GetCorpse())
        {
            corpseMapId = corpse->GetMapId();
        }

        // check back way from corpse to entrance
        uint32 instance_map = corpseMapId;
        do
        {
            // most often fast case
            if (instance_map == targetMapEntry->MapID)
            {
                break;
            }

            InstanceTemplate const* instance = ObjectMgr::GetInstanceTemplate(instance_map);
            instance_map = instance ? instance->parent : 0;
        }
        while (instance_map);

        // corpse not in dungeon or some linked deep dungeons
        if (!instance_map)
        {
            WorldPacket data(SMSG_AREA_TRIGGER_NO_CORPSE);
            player->GetSession()->SendPacket(&data);
            return;
        }

        // need find areatrigger to inner dungeon for landing point
        if (at->target_mapId != corpseMapId)
        {
            if (AreaTrigger const* corpseAt = sObjectMgr.GetMapEntranceTrigger(corpseMapId))
            {
                at = corpseAt;
                targetMapEntry = sMapStore.LookupEntry(at->target_mapId);
                if (!targetMapEntry)
                {
                    return;
                }
            }
        }

        // now we can resurrect player, and then check teleport requirements
        player->ResurrectPlayer(0.5f);
        player->SpawnCorpseBones();
    }

    // teleport player (trigger requirement will be checked on TeleportTo)
    player->TeleportTo(at->target_mapId, at->target_X, at->target_Y, at->target_Z, at->target_Orientation, TELE_TO_NOT_LEAVE_TRANSPORT, at);
}

void WorldSession::HandleUpdateAccountData(WorldPacket& recv_data)
{
    DETAIL_LOG("WORLD: Received opcode CMSG_UPDATE_ACCOUNT_DATA");

    uint32 type, timestamp, decompressedSize;
    recv_data >> type >> timestamp >> decompressedSize;

    DEBUG_LOG("UAD: type %u, time %u, decompressedSize %u", type, timestamp, decompressedSize);

    if (type > NUM_ACCOUNT_DATA_TYPES)
    {
        return;
    }

    if (decompressedSize == 0)                              // erase
    {
        SetAccountData(AccountDataType(type), 0, "");

        WorldPacket data(SMSG_UPDATE_ACCOUNT_DATA_COMPLETE, 4 + 4);
        data << uint32(type);
        data << uint32(0);
        SendPacket(&data);

        return;
    }

    if (decompressedSize > 0xFFFF)
    {
        recv_data.rpos(recv_data.wpos());                   // unnneded warning spam in this case
        sLog.outError("UAD: Account data packet too big, size %u", decompressedSize);
        return;
    }

    ByteBuffer dest;
    dest.resize(decompressedSize);

    uLongf realSize = decompressedSize;
    if (uncompress(const_cast<uint8*>(dest.contents()), &realSize, const_cast<uint8*>(recv_data.contents() + recv_data.rpos()), recv_data.size() - recv_data.rpos()) != Z_OK)
    {
        recv_data.rpos(recv_data.wpos());                   // unneded warning spam in this case
        sLog.outError("UAD: Failed to decompress account data");
        return;
    }

    recv_data.rpos(recv_data.wpos());                       // uncompress read (recv_data.size() - recv_data.rpos())

    std::string adata;
    dest >> adata;

    SetAccountData(AccountDataType(type), timestamp, adata);

    WorldPacket data(SMSG_UPDATE_ACCOUNT_DATA_COMPLETE, 4 + 4);
    data << uint32(type);
    data << uint32(0);
    SendPacket(&data);
}

void WorldSession::HandleRequestAccountData(WorldPacket& recv_data)
{
    DETAIL_LOG("WORLD: Received opcode CMSG_REQUEST_ACCOUNT_DATA");

    uint32 type;
    recv_data >> type;

    DEBUG_LOG("RAD: type %u", type);

    if (type > NUM_ACCOUNT_DATA_TYPES)
    {
        return;
    }

    AccountData* adata = GetAccountData(AccountDataType(type));

    uint32 size = adata->Data.size();

    uLongf destSize = compressBound(size);

    ByteBuffer dest;
    dest.resize(destSize);

    if (size && compress(const_cast<uint8*>(dest.contents()), &destSize, (uint8*)adata->Data.c_str(), size) != Z_OK)
    {
        DEBUG_LOG("RAD: Failed to compress account data");
        return;
    }

    dest.resize(destSize);

    WorldPacket data(SMSG_UPDATE_ACCOUNT_DATA, 8 + 4 + 4 + 4 + destSize);
    data << (_player ? _player->GetObjectGuid() : ObjectGuid());// player guid
    data << uint32(type);                                   // type (0-7)
    data << uint32(adata->Time);                            // unix time
    data << uint32(size);                                   // decompressed length
    data.append(dest);                                      // compressed data
    SendPacket(&data);
}

void WorldSession::HandleSetActionButtonOpcode(WorldPacket& recv_data)
{
    DEBUG_LOG("WORLD: Received opcode CMSG_SET_ACTION_BUTTON");
    uint8 button;
    uint32 packetData;
    recv_data >> button >> packetData;

    uint32 action = ACTION_BUTTON_ACTION(packetData);
    uint8  type   = ACTION_BUTTON_TYPE(packetData);

    DETAIL_LOG("BUTTON: %u ACTION: %u TYPE: %u", button, action, type);
    if (!packetData)
    {
        DETAIL_LOG("MISC: Remove action from button %u", button);
        GetPlayer()->removeActionButton(GetPlayer()->GetActiveSpec(), button);
    }
    else
    {
        switch (type)
        {
            case ACTION_BUTTON_MACRO:
            case ACTION_BUTTON_CMACRO:
                DETAIL_LOG("MISC: Added Macro %u into button %u", action, button);
                break;
            case ACTION_BUTTON_EQSET:
                DETAIL_LOG("MISC: Added EquipmentSet %u into button %u", action, button);
                break;
            case ACTION_BUTTON_SPELL:
                DETAIL_LOG("MISC: Added Spell %u into button %u", action, button);
                break;
            case ACTION_BUTTON_ITEM:
                DETAIL_LOG("MISC: Added Item %u into button %u", action, button);
                break;
            default:
                sLog.outError("MISC: Unknown action button type %u for action %u into button %u", type, action, button);
                return;
        }
        GetPlayer()->addActionButton(GetPlayer()->m_activeSpec, button, action, type);
    }
}

void WorldSession::HandleCompleteCinematic(WorldPacket& /*recv_data*/)
{
    DEBUG_LOG("WORLD: Received opcode CMSG_COMPLETE_CINEMATIC");
}

void WorldSession::HandleNextCinematicCamera(WorldPacket& /*recv_data*/)
{
    DEBUG_LOG("WORLD: Received opcode CMSG_NEXT_CINEMATIC_CAMERA");
}

void WorldSession::HandleMoveTimeSkippedOpcode(WorldPacket& recv_data)
{
    /*  WorldSession::Update( WorldTimer::getMSTime() );*/
    DEBUG_LOG("WORLD: Received opcode CMSG_MOVE_TIME_SKIPPED");

    ObjectGuid guid;

    recv_data >> guid.ReadAsPacked();
    recv_data >> Unused<uint32>();

    /*
    ObjectGuid guid;
    uint32 time_skipped;
    recv_data >> guid;
    recv_data >> time_skipped;
    DEBUG_LOG("WORLD: Received opcode CMSG_MOVE_TIME_SKIPPED");

    /// TODO
    must be need use in mangos
    We substract server Lags to move time ( AntiLags )
    for exmaple
    {
        GetPlayer()->ModifyLastMoveTime( -int32(time_skipped) );
    }
    */
}

void WorldSession::HandleFeatherFallAck(WorldPacket& recv_data)
{
    DEBUG_LOG("WORLD: Received opcode CMSG_MOVE_FEATHER_FALL_ACK");

    // no used
    recv_data.rpos(recv_data.wpos());                       // prevent warnings spam
}

void WorldSession::HandleMoveUnRootAck(WorldPacket& recv_data)
{
    // no used
    recv_data.rpos(recv_data.wpos());                       // prevent warnings spam
    /*
        ObjectGuid guid;
        recv_data >> guid;

        // now can skip not our packet
        if (_player->GetGUID() != guid)
        {
            recv_data.rpos(recv_data.wpos());               // prevent warnings spam
            return;
        }

        DEBUG_LOG("WORLD: Received opcode CMSG_FORCE_MOVE_UNROOT_ACK");

        recv_data.read_skip<uint32>();                      // unk

        MovementInfo movementInfo;
        ReadMovementInfo(recv_data, &movementInfo);
    */
}

void WorldSession::HandleMoveRootAck(WorldPacket& recv_data)
{
    // no used
    recv_data.rpos(recv_data.wpos());                       // prevent warnings spam
    /*
        ObjectGuid guid;
        recv_data >> guid;

        // now can skip not our packet
        if (_player->GetObjectGuid() != guid)
        {
            recv_data.rpos(recv_data.wpos());               // prevent warnings spam
            return;
        }

        DEBUG_LOG("WORLD: Received opcode CMSG_FORCE_MOVE_ROOT_ACK");

        recv_data.read_skip<uint32>();                      // unk

        MovementInfo movementInfo;
        ReadMovementInfo(recv_data, &movementInfo);
    */
}

void WorldSession::HandleSetActionBarTogglesOpcode(WorldPacket& recv_data)
{
    uint8 ActionBar;

    recv_data >> ActionBar;

    if (!GetPlayer())                                       // ignore until not logged (check needed because STATUS_AUTHED)
    {
        if (ActionBar != 0)
        {
            sLog.outError("WorldSession::HandleSetActionBarToggles in not logged state with value: %u, ignored", uint32(ActionBar));
        }
        return;
    }

    GetPlayer()->SetByteValue(PLAYER_FIELD_BYTES, 2, ActionBar);
}

void WorldSession::HandlePlayedTime(WorldPacket& recv_data)
{
    uint8 unk1;
    recv_data >> unk1;                                      // 0 or 1 expected

    WorldPacket data(SMSG_PLAYED_TIME, 4 + 4 + 1);
    data << uint32(_player->GetTotalPlayedTime());
    data << uint32(_player->GetLevelPlayedTime());
    data << uint8(unk1);                                    // 0 - will not show in chat frame
    SendPacket(&data);
}

void WorldSession::HandleInspectOpcode(WorldPacket& recv_data)
{
    ObjectGuid guid;
    recv_data >> guid;
    DEBUG_LOG("Inspected guid is %s", guid.GetString().c_str());

    _player->SetSelectionGuid(guid);

    Player* plr = sObjectMgr.GetPlayer(guid);
    if (!plr)                                               // wrong player
    {
        return;
    }

    WorldPacket data(SMSG_INSPECT_RESULTS, 50);
    data << plr->GetPackGUID();

    if (sWorld.getConfig(CONFIG_BOOL_TALENTS_INSPECTING) || _player->isGameMaster())
    {
        plr->BuildPlayerTalentsInfoData(&data);
    }
    else
    {
        data << uint32(0);                                  // unspentTalentPoints
        data << uint8(0);                                   // talentGroupCount
        data << uint8(0);                                   // talentGroupIndex
    }

    plr->BuildEnchantmentsInfoData(&data);

    SendPacket(&data);
}

void WorldSession::HandleInspectHonorStatsOpcode(WorldPacket& recv_data)
{
    ObjectGuid guid;
    recv_data >> guid;

    Player* player = sObjectMgr.GetPlayer(guid);

    if (!player)
    {
        sLog.outError("InspectHonorStats: WTF, player not found...");
        return;
    }

    WorldPacket data(MSG_INSPECT_HONOR_STATS, 8 + 1 + 4 * 4);
    data << player->GetObjectGuid();
    data << uint8(player->GetUInt32Value(PLAYER_FIELD_HONOR_CURRENCY));
    data << uint32(player->GetUInt32Value(PLAYER_FIELD_KILLS));
    data << uint32(player->GetUInt32Value(PLAYER_FIELD_TODAY_CONTRIBUTION));
    data << uint32(player->GetUInt32Value(PLAYER_FIELD_YESTERDAY_CONTRIBUTION));
    data << uint32(player->GetUInt32Value(PLAYER_FIELD_LIFETIME_HONORABLE_KILLS));
    SendPacket(&data);
}


/****************************************/
/* This function handles the 'worldport' and 'movecharacter' client commands. */
/* Usage: worldport <continentID> [x y z] [facing] */
/****************************************/
void WorldSession::WorldTeleportHandler(WorldPacket& recv_data)
{
    const char *commandName = (recv_data.GetOpcode() == CMSG_WORLD_TELEPORT) ? "worldport" : "movecharacter";
    DEBUG_LOG("WORLD: Received %s command from account %d:", commandName, GetAccountId());

    /* Check that we have permission to perform the function */
    if (GetSecurity() != SEC_PLAYER)
    {
        uint32 timeMs = time(NULL);  /* Truncated time stamp: Used for time value ONLY. */
        uint32 continentID = NULL;
        uint64 characterGUID = NULL;    /* This is used ONLY for CMSG_MOVE_CHARACTER_CHEAT. Hardcoded to 0 for other cases. */
        Position position = Position();

        recv_data >> timeMs;
        recv_data >> continentID;
        recv_data >> characterGUID;
        recv_data >> position.x;
        recv_data >> position.y;
        recv_data >> position.z;
        recv_data >> position.o;

        DEBUG_LOG("Porting %s(%s): continentID=%u, x=%f, y=%f, z=%f, facing=%f...", GetPlayerName(), GetPlayer()->GetGuidStr().c_str(), continentID, position.x, position.y, position.z, position.o);
        GetPlayer()->TeleportTo(continentID, position.x, position.y, position.z, position.o, TELE_TO_GM_MODE, NULL);
    }
    else
    {
        DEBUG_LOG("Permission denied.");
        SendNotification(LANG_YOU_NOT_HAVE_PERMISSION);
    }
}

/****************************************/
/* This function handles the 'resurrect' client command. */
/* Usage: resurrect <player name> */
/****************************************/
void WorldSession::GmResurrectHandler(WorldPacket &msg)
{
    DEBUG_LOG("WORLD: received resurrect command from account %d:", GetAccountId());

    /* Check that we have permission to perform the function */
    if (GetSecurity() != SEC_PLAYER)
    {
        std::string name = "";
        Player *pPlayer = 0;

        msg >> name;
        normalizePlayerName(name);

        pPlayer = sObjectMgr.GetPlayer(name.c_str());
        if (pPlayer)
        {
            if (pPlayer->IsAlive())
            {
                /* Player is already alive: Sending down failure response to client... */
                DEBUG_LOG("resurrect failed");
                SendGmResurrectFailureResponse();
            }
            else
            {
                /* Resurrect the player with full health and power... */
                pPlayer->ResurrectPlayer(1.0f, false);
                Powers power = pPlayer->GetPowerType();
                uint32 maxPower = pPlayer->GetMaxPower(power);
                pPlayer->SetPower(power, maxPower);
                SendGmResurrectSuccessResponse();
                DEBUG_LOG("Player resurrected");
            }
        }
        else
        {
            /* Player not found */
            DEBUG_LOG("Player not found");
            SendPlayerNotFoundFailureResponse();
        }
    }
    else
    {
        DEBUG_LOG("Permission denied.");
        SendNotification(LANG_YOU_NOT_HAVE_PERMISSION);
    }
}

/****************************************/
/* This function handles the 'level' client command. */
/****************************************/
void WorldSession::LevelCheatHandler(WorldPacket &msg)
{
    DEBUG_LOG("WORLD: Received %s message from account %d:", msg.GetOpcodeName(), GetAccountId());

    /* Check that we have permission to perform the function */
    if (GetSecurity() > SEC_PLAYER)
    {
        uint32 curLevel = GetPlayer()->getLevel();
        uint32 newLevel = 0;

        msg >> newLevel;

        /* Check that the level value is greater than 0 and smaller than the server's max player level for its expansion level */
        /* and that we have a different level than the one requested... */
        if ((newLevel != 0 && newLevel <= DEFAULT_MAX_LEVEL) && newLevel != curLevel)
        {
            DEBUG_LOG("Leveling player %s from level %d to %d", GetPlayerName(), curLevel, newLevel);
            GetPlayer()->SetLevel(newLevel);
        }
    }
    else
    {
        DEBUG_LOG("Permission denied.");
        SendNotification(LANG_YOU_NOT_HAVE_PERMISSION);
    }
}

/****************************************/
/* Forcefully removes us from the server */
/****************************************/
void WorldSession::BootMeHandler(WorldPacket& msg)
{
    DEBUG_LOG("Received CMSG_BOOTME from account %d:", GetAccountId());
    if (GetSecurity() > SEC_PLAYER)
    {
        KickPlayer();
        DEBUG_LOG("Account disconnected, reason = \"User sent BOOTME command\"");
    }
    else
    {
        DEBUG_LOG("Permission denied.");
        SendNotification(LANG_YOU_NOT_HAVE_PERMISSION);
    }
}

/****************************************/
/* Creates a game object by ID */
/* at the player's current location */
/****************************************/
void WorldSession::CreateGameObjectHandler(WorldPacket &msg)
{
    DEBUG_LOG("WORLD: Received %s message from account %d:", msg.GetOpcodeName(), GetAccountId());

    /* Check that we have permission to perform the function */
    if (GetSecurity() > SEC_PLAYER)
    {
        uint32 gameObjectId = 0;
        Player *pPlayer = GetPlayer();
        Position position = pPlayer->GetPosition();
        Map *pMap = pPlayer->GetMap();

        msg >> gameObjectId;
        if (gameObjectId)
        {
            const GameObjectInfo *gInfo = ObjectMgr::GetGameObjectInfo(gameObjectId);
            if (!gInfo)
            {
                SendNotification("Game object not found");
                return;
            }

            /* Is this check really necessary? */
            if (gInfo->displayId && !sGameObjectDisplayInfoStore.LookupEntry(gInfo->displayId))
            {
                sLog.outErrorDb("Game object %u has invalid display ID %u", gameObjectId, gInfo->displayId);
                return;
            }

            /* GUID can be zero if the server limit has been reached */
            uint32 db_lowGUID = sObjectMgr.GenerateStaticGameObjectLowGuid();
            if (!db_lowGUID)
            {
                SendNotification(LANG_NO_FREE_STATIC_GUID_FOR_SPAWN);
                return;
            }

            GameObject *pGameObj = new GameObject;  /* Object is freed from memory in Map::Remove */
            if (!pGameObj->Create(db_lowGUID, gInfo->id, pMap, pPlayer->GetPhaseMaskForSpawn(), position.x, position.y, position.z, position.o))
            {
                delete pGameObj;
                SendNotification("Failed to create game object");
                return;
            }

            pGameObj->SaveToDB(pMap->GetId(), (1 << pMap->GetSpawnMode()), pPlayer->GetPhaseMaskForSpawn()); /* Call this first or the server shits a brick */
            pMap->Add(pGameObj);
            pGameObj->AIM_Initialize();
            sObjectMgr.AddGameobjectToGrid(db_lowGUID, sObjectMgr.GetGOData(db_lowGUID));
            DEBUG_LOG("Created game object %d", gameObjectId);
        }
    }
    else
    {
        DEBUG_LOG("Permission denied.");
        SendNotification(LANG_YOU_NOT_HAVE_PERMISSION);
    }
}

/****************************************/
/* Sets the money value for the active player */
/****************************************/
void WorldSession::SetMoneyHandler(WorldPacket &msg)
{
    DEBUG_LOG("WORLD: Received %s message from account %d:", msg.GetOpcodeName(), GetAccountId());
    if (GetSecurity() > SEC_PLAYER)
    {
        Player *pPlayer = GetPlayer();
        int32 money = 0;

        msg >> money;
        if (money < 0)
        {
            money = 0x7FFFFFFF;    /* Money limit */
        }

        DEBUG_LOG("Setting money on %s from %d to %d", pPlayer->GetName(), pPlayer->GetMoney(), money); /*TODO: Log this appropriately*/
        GetPlayer()->SetMoney(money);
    }
    else
    {
        DEBUG_LOG("Permission denied.");
        SendNotification(LANG_YOU_NOT_HAVE_PERMISSION);
    }
}

void WorldSession::HandleWhoisOpcode(WorldPacket& recv_data)
{
    DEBUG_LOG("WORLD: Received opcode CMSG_WHOIS");
    std::string charname;
    recv_data >> charname;

    if (GetSecurity() < SEC_ADMINISTRATOR)
    {
        SendNotification(LANG_YOU_NOT_HAVE_PERMISSION);
        return;
    }

    if (charname.empty() || !normalizePlayerName(charname))
    {
        SendNotification(LANG_NEED_CHARACTER_NAME);
        return;
    }

    Player* plr = sObjectMgr.GetPlayer(charname.c_str());

    if (!plr)
    {
        SendNotification(LANG_PLAYER_NOT_EXIST_OR_OFFLINE, charname.c_str());
        return;
    }

    uint32 accid = plr->GetSession()->GetAccountId();

    QueryResult* result = LoginDatabase.PQuery("SELECT `username`,`email`,`last_ip` FROM `account` WHERE `id`=%u", accid);
    if (!result)
    {
        SendNotification(LANG_ACCOUNT_FOR_PLAYER_NOT_FOUND, charname.c_str());
        return;
    }

    Field* fields = result->Fetch();
    std::string acc = fields[0].GetCppString();
    if (acc.empty())
    {
        acc = "Unknown";
    }
    std::string email = fields[1].GetCppString();
    if (email.empty())
    {
        email = "Unknown";
    }
    std::string lastip = fields[2].GetCppString();
    if (lastip.empty())
    {
        lastip = "Unknown";
    }

    std::string msg = charname + "'s " + "account is " + acc + ", e-mail: " + email + ", last ip: " + lastip;

    WorldPacket data(SMSG_WHOIS, msg.size() + 1);
    data << msg;
    _player->GetSession()->SendPacket(&data);

    delete result;

    DEBUG_LOG("Received whois command from player %s for character %s", GetPlayer()->GetName(), charname.c_str());
}

void WorldSession::HandleComplainOpcode(WorldPacket& recv_data)
{
    DEBUG_LOG("WORLD: Received opcode CMSG_COMPLAIN");
    recv_data.hexlike();

    uint8 spam_type;                                        // 0 - mail, 1 - chat
    ObjectGuid spammerGuid;
    uint32 unk1 = 0;
    uint32 unk2 = 0;
    uint32 unk3 = 0;
    uint32 unk4 = 0;
    std::string description = "";
    recv_data >> spam_type;                                 // unk 0x01 const, may be spam type (mail/chat)
    recv_data >> spammerGuid;                               // player guid
    switch (spam_type)
    {
        case 0:
            recv_data >> unk1;                              // const 0
            recv_data >> unk2;                              // probably mail id
            recv_data >> unk3;                              // const 0
            break;
        case 1:
            recv_data >> unk1;                              // probably language
            recv_data >> unk2;                              // message type?
            recv_data >> unk3;                              // probably channel id
            recv_data >> unk4;                              // unk random value
            recv_data >> description;                       // spam description string (messagetype, channel name, player name, message)
            break;
    }

    // NOTE: all chat messages from this spammer automatically ignored by spam reporter until logout in case chat spam.
    // if it's mail spam - ALL mails from this spammer automatically removed by client

    // Complaint Received message
    WorldPacket data(SMSG_COMPLAIN_RESULT, 1);
    data << uint8(0);
    SendPacket(&data);

    DEBUG_LOG("REPORT SPAM: type %u, spammer %s, unk1 %u, unk2 %u, unk3 %u, unk4 %u, message %s", spam_type, spammerGuid.GetString().c_str(), unk1, unk2, unk3, unk4, description.c_str());
}

void WorldSession::HandleRealmSplitOpcode(WorldPacket& recv_data)
{
    DEBUG_LOG("WORLD: Received opcode CMSG_REALM_SPLIT");

    uint32 unk;
    std::string split_date = "01/01/01";
    recv_data >> unk;

    WorldPacket data(SMSG_REALM_SPLIT, 4 + 4 + split_date.size() + 1);
    data << unk;
    data << uint32(0x00000000);                             // realm split state
    // split states:
    // 0x0 realm normal
    // 0x1 realm split
    // 0x2 realm split pending
    data << split_date;
    SendPacket(&data);
    // DEBUG_LOG("response sent %u", unk);
}

void WorldSession::HandleFarSightOpcode(WorldPacket& recv_data)
{
    DEBUG_LOG("WORLD: Received opcode CMSG_FAR_SIGHT");
    // recv_data.hexlike();

    uint8 op;
    recv_data >> op;

    WorldObject* obj = _player->GetMap()->GetWorldObject(_player->GetFarSightGuid());
    if (!obj)
    {
        return;
    }

    switch (op)
    {
        case 0:
            DEBUG_LOG("Removed FarSight from %s", _player->GetGuidStr().c_str());
            _player->GetCamera().ResetView(false);
            break;
        case 1:
            DEBUG_LOG("Added FarSight %s to %s", _player->GetFarSightGuid().GetString().c_str(), _player->GetGuidStr().c_str());
            _player->GetCamera().SetView(obj, false);
            break;
    }
}

void WorldSession::HandleSetTitleOpcode(WorldPacket& recv_data)
{
    DEBUG_LOG("WORLD: Received opcode CMSG_SET_TITLE");

    int32 title;
    recv_data >> title;

    // -1 at none
    if (title > 0 && title < MAX_TITLE_INDEX)
    {
        if (!GetPlayer()->HasTitle(title))
        {
            return;
        }
    }
    else
    {
        title = 0;
    }

    GetPlayer()->SetUInt32Value(PLAYER_CHOSEN_TITLE, title);
}

void WorldSession::HandleTimeSyncResp(WorldPacket& recv_data)
{
    uint32 counter, clientTicks;
    recv_data >> counter >> clientTicks;

    DEBUG_LOG("WORLD: Received opcode CMSG_TIME_SYNC_RESP: counter %u, client ticks %u, time since last sync %u", counter, clientTicks, clientTicks - _player->m_timeSyncClient);

    if (counter != _player->m_timeSyncCounter - 1)
    {
        DEBUG_LOG(" WORLD: Opcode CMSG_TIME_SYNC_RESP -- Wrong time sync counter from %s (cheater?)", _player->GetGuidStr().c_str());
    }

    uint32 ourTicks = clientTicks + (GameTime::GetGameTimeMS() - _player->m_timeSyncServer);

    // diff should be small
    DEBUG_LOG(" WORLD: Opcode CMSG_TIME_SYNC_RESP -- Our ticks: %u, diff %u, latency %u", ourTicks, ourTicks - clientTicks, GetLatency());

    _player->m_timeSyncClient = clientTicks;
}

void WorldSession::HandleResetInstancesOpcode(WorldPacket& /*recv_data*/)
{
    DEBUG_LOG("WORLD: Received opcode CMSG_RESET_INSTANCES");

    if (Group* pGroup = _player->GetGroup())
    {
        if (pGroup->IsLeader(_player->GetObjectGuid()))
        {
            pGroup->ResetInstances(INSTANCE_RESET_ALL, false, _player);
        }
    }
    else
    {
        _player->ResetInstances(INSTANCE_RESET_ALL, false);
    }
}

void WorldSession::HandleSetDungeonDifficultyOpcode(WorldPacket& recv_data)
{
    DEBUG_LOG("WORLD: Received opcode MSG_SET_DUNGEON_DIFFICULTY");

    uint32 mode;
    recv_data >> mode;

    if (mode >= MAX_DUNGEON_DIFFICULTY)
    {
        sLog.outError("WorldSession::HandleSetDungeonDifficultyOpcode: player %d sent an invalid instance mode %d!", _player->GetGUIDLow(), mode);
        return;
    }

    if (Difficulty(mode) == _player->GetDungeonDifficulty())
    {
        return;
    }

    // cannot reset while in an instance
    Map* map = _player->GetMap();
    if (map && map->IsDungeon())
    {
        sLog.outError("WorldSession::HandleSetDungeonDifficultyOpcode: player %d tried to reset the instance while inside!", _player->GetGUIDLow());
        return;
    }

    // Exception to set mode to normal for low-level players
    if (_player->getLevel() < LEVELREQUIREMENT_HEROIC && mode > REGULAR_DIFFICULTY)
    {
        return;
    }

    if (Group* pGroup = _player->GetGroup())
    {
        if (pGroup->IsLeader(_player->GetObjectGuid()))
        {
            // the difficulty is set even if the instances can't be reset
            //_player->SendDungeonDifficulty(true);
            pGroup->ResetInstances(INSTANCE_RESET_CHANGE_DIFFICULTY, false, _player);
            pGroup->SetDungeonDifficulty(Difficulty(mode));
        }
    }
    else
    {
        _player->ResetInstances(INSTANCE_RESET_CHANGE_DIFFICULTY, false);
        _player->SetDungeonDifficulty(Difficulty(mode));
    }
}

void WorldSession::HandleSetRaidDifficultyOpcode(WorldPacket& recv_data)
{
    DEBUG_LOG("WORLD: Received opcode MSG_SET_RAID_DIFFICULTY");

    uint32 mode;
    recv_data >> mode;

    if (mode >= MAX_RAID_DIFFICULTY)
    {
        sLog.outError("WorldSession::HandleSetRaidDifficultyOpcode: player %d sent an invalid instance mode %d!", _player->GetGUIDLow(), mode);
        return;
    }

    if (Difficulty(mode) == _player->GetRaidDifficulty())
    {
        return;
    }

    // cannot reset while in an instance
    Map* map = _player->GetMap();
    if (map && map->IsDungeon())
    {
        sLog.outError("WorldSession::HandleSetRaidDifficultyOpcode: player %d tried to reset the instance while inside!", _player->GetGUIDLow());
        return;
    }

    // Exception to set mode to normal for low-level players
    if (_player->getLevel() < LEVELREQUIREMENT_HEROIC && mode > REGULAR_DIFFICULTY)
    {
        return;
    }

    if (Group* pGroup = _player->GetGroup())
    {
        if (pGroup->IsLeader(_player->GetObjectGuid()))
        {
            // the difficulty is set even if the instances can't be reset
            //_player->SendDungeonDifficulty(true);
            pGroup->ResetInstances(INSTANCE_RESET_CHANGE_DIFFICULTY, true, _player);
            pGroup->SetRaidDifficulty(Difficulty(mode));
        }
    }
    else
    {
        _player->ResetInstances(INSTANCE_RESET_CHANGE_DIFFICULTY, true);
        _player->SetRaidDifficulty(Difficulty(mode));
    }
}

void WorldSession::HandleCancelMountAuraOpcode(WorldPacket& /*recv_data*/)
{
    DEBUG_LOG("WORLD: Received opcode  CMSG_CANCEL_MOUNT_AURA");

    // If player is not mounted, so go out :)
    if (!_player->IsMounted())                              // not blizz like; no any messages on blizz
    {
        ChatHandler(this).SendSysMessage(LANG_CHAR_NON_MOUNTED);
        return;
    }

    if (_player->IsTaxiFlying())                            // not blizz like; no any messages on blizz
    {
        ChatHandler(this).SendSysMessage(LANG_YOU_IN_FLIGHT);
        return;
    }

    _player->Unmount(_player->HasAuraType(SPELL_AURA_MOUNTED));
    _player->RemoveSpellsCausingAura(SPELL_AURA_MOUNTED);
}

void WorldSession::HandleMoveSetCanFlyAckOpcode(WorldPacket& recv_data)
{
    // fly mode on/off
    DEBUG_LOG("WORLD: Received opcode CMSG_MOVE_SET_CAN_FLY_ACK");
    // recv_data.hexlike();

    ObjectGuid guid;
    MovementInfo movementInfo;

    recv_data >> guid.ReadAsPacked();
    recv_data >> Unused<uint32>();                          // unk
    recv_data >> movementInfo;
    recv_data >> Unused<float>();                           // unk2

    if (_player->GetMover()->GetObjectGuid() != guid)
    {
        DEBUG_LOG("WorldSession::HandleMoveSetCanFlyAckOpcode: player %s, mover %s, received %s, ignored",
                  _player->GetGuidStr().c_str(), _player->GetMover()->GetGuidStr().c_str(), guid.GetString().c_str());
        return;
    }

    _player->GetMover()->m_movementInfo.SetMovementFlags(movementInfo.GetMovementFlags());
}

void WorldSession::HandleRequestPetInfoOpcode(WorldPacket& /*recv_data */)
{
    /*
        DEBUG_LOG("WORLD: Received opcode CMSG_REQUEST_PET_INFO");
        recv_data.hexlike();
    */
}

void WorldSession::HandleSetTaxiBenchmarkOpcode(WorldPacket& recv_data)
{
    uint8 mode;
    recv_data >> mode;

    DEBUG_LOG("Client used \"/timetest %d\" command", mode);
}

void WorldSession::HandleQueryInspectAchievementsOpcode(WorldPacket& recv_data)
{
    ObjectGuid guid;

    recv_data >> guid.ReadAsPacked();

    if (Player* player = sObjectMgr.GetPlayer(guid))
    {
        player->GetAchievementMgr().SendRespondInspectAchievements(_player);
    }
}

void WorldSession::HandleUITimeRequestOpcode(WorldPacket& /*recv_data*/)
{
    // empty opcode
    DEBUG_LOG("WORLD: Received opcode SMSG_UI_TIME");

    WorldPacket data(SMSG_UI_TIME, 4);
    data << uint32(time(NULL));
    SendPacket(&data);
}

void WorldSession::HandleReadyForAccountDataTimesOpcode(WorldPacket& /*recv_data*/)
{
    // empty opcode
    DEBUG_LOG("WORLD: Received opcode CMSG_READY_FOR_ACCOUNT_DATA_TIMES");

    SendAccountDataTimes(GLOBAL_CACHE_MASK);
}

void WorldSession::HandleHearthandResurrect(WorldPacket& /*recv_data*/)
{
    DEBUG_LOG("WORLD: Received opcode CMSG_HEARTH_AND_RESURRECT");

    AreaTableEntry const* atEntry = sAreaStore.LookupEntry(_player->GetAreaId());
    if (!atEntry || !(atEntry->flags & AREA_FLAG_CAN_HEARTH_AND_RES))
    {
        return;
    }

    // Can't use in flight
    if (_player->IsTaxiFlying())
    {
        return;
    }

    // Send Everytime
    _player->BuildPlayerRepop();
    _player->ResurrectPlayer(100);
    _player->TeleportToHomebind();
}
