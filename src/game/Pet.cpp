/*
 * Copyright (C) 2005-2012 MaNGOS <http://getmangos.com/>
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

#include "Common.h"
#include "Database/DatabaseEnv.h"
#include "Log.h"
#include "WorldPacket.h"
#include "ObjectMgr.h"
#include "SpellMgr.h"
#include "Pet.h"
#include "Formulas.h"
#include "SpellAuras.h"
#include "CreatureAI.h"
#include "Unit.h"
#include "Util.h"

Pet::Pet(PetType type) :
Creature(CREATURE_SUBTYPE_PET),
m_resetTalentsCost(0), m_resetTalentsTime(0), m_usedTalentCount(0),
m_removed(false), m_petType(type), m_duration(0),
m_bonusdamage(0), m_auraUpdateMask(0), m_loading(false),
m_declinedname(NULL)
{
    m_name = "Pet";
    m_regenTimer = REGEN_TIME_FULL;
    m_holyPowerRegenTimer = REGEN_TIME_HOLY_POWER;
    m_focusRegenTimer = REGEN_TIME_PET_FOCUS;

    // pets always have a charminfo, even if they are not actually charmed
    CharmInfo* charmInfo = InitCharmInfo(this);

    if(type == MINI_PET)                                    // always passive
        charmInfo->SetState(CHARM_STATE_REACT, REACT_PASSIVE);
    else if(type == PROTECTOR_PET)                          // always defensive
        charmInfo->SetState(CHARM_STATE_REACT, REACT_DEFENSIVE);
    else if(type == GUARDIAN_PET)                           // always aggressive
        charmInfo->SetState(CHARM_STATE_REACT, REACT_AGGRESSIVE);

    m_actualSlot = PET_SAVE_NOT_IN_SLOT;
}

Pet::~Pet()
{
    m_spells.clear();

    delete m_declinedname;
}

void Pet::AddToWorld()
{
    ///- Register the pet for guid lookup
    if(!IsInWorld())
        GetMap()->GetObjectsStore().insert<Pet>(GetObjectGuid(), (Pet*)this);

    Unit::AddToWorld();
}

void Pet::RemoveFromWorld()
{
    ///- Remove the pet from the accessor
    if(IsInWorld())
        GetMap()->GetObjectsStore().erase<Pet>(GetObjectGuid(), (Pet*)NULL);

    ///- Don't call the function for Creature, normal mobs + totems go in a different storage
    Unit::RemoveFromWorld();
}

bool Pet::LoadPetFromDB(Player* owner, uint32 petentry, uint32 petnumber, bool current, PetSaveMode slot)
{
    m_loading = true;

    uint32 ownerid = owner->GetGUIDLow();

    QueryResult *result;

    if (petnumber)
        // known petnumber entry                  0   1      2(?)   3        4      5    6           7     8     9        10         11       12      13        14                 15                 16              17       18
        result = CharacterDatabase.PQuery("SELECT id, entry, owner, modelid, level, exp, Reactstate, slot, name, renamed, curhealth, curmana, abdata, savetime, resettalents_cost, resettalents_time, CreatedBySpell, PetType, actual_slot "
            "FROM character_pet WHERE owner = '%u' AND id = '%u'",
            ownerid, petnumber);
    else if (current)
        // current pet (slot 0)                   0   1      2(?)   3        4      5    6           7     8     9        10         11       12      13        14                 15                 16              17       18
        result = CharacterDatabase.PQuery("SELECT id, entry, owner, modelid, level, exp, Reactstate, slot, name, renamed, curhealth, curmana, abdata, savetime, resettalents_cost, resettalents_time, CreatedBySpell, PetType, actual_slot "
            "FROM character_pet WHERE owner = '%u' AND slot = '%u'",
            ownerid, PET_SAVE_AS_CURRENT);
    else if (slot != PET_SAVE_NOT_IN_SLOT)
        // pet by slot                            0   1      2(?)   3        4      5    6           7     8     9        10         11       12      13        14                 15                 16              17       18
        result = CharacterDatabase.PQuery("SELECT id, entry, owner, modelid, level, exp, Reactstate, slot, name, renamed, curhealth, curmana, abdata, savetime, resettalents_cost, resettalents_time, CreatedBySpell, PetType, actual_slot "
            "FROM character_pet WHERE owner = '%u' AND actual_slot = '%u'",
            ownerid, uint32(slot));
    else if (petentry)
        // known petentry entry (unique for summoned pet, but non unique for hunter pet (only from current or not stabled pets)
        //                                        0   1      2(?)   3        4      5    6           7     8     9        10         11       12      13        14                 15                 16              17       18
        result = CharacterDatabase.PQuery("SELECT id, entry, owner, modelid, level, exp, Reactstate, slot, name, renamed, curhealth, curmana, abdata, savetime, resettalents_cost, resettalents_time, CreatedBySpell, PetType, actual_slot "
            "FROM character_pet WHERE owner = '%u' AND entry = '%u' AND (slot = '%u' OR slot > '%u') ",
            ownerid, petentry,PET_SAVE_AS_CURRENT,PET_SAVE_LAST_STABLE_SLOT);
    else
        // any current or other non-stabled pet (for hunter "call pet")
        //                                        0   1      2(?)   3        4      5    6           7     8     9        10         11       12      13        14                 15                 16              17       18
        result = CharacterDatabase.PQuery("SELECT id, entry, owner, modelid, level, exp, Reactstate, slot, name, renamed, curhealth, curmana, abdata, savetime, resettalents_cost, resettalents_time, CreatedBySpell, PetType, actual_slot "
            "FROM character_pet WHERE owner = '%u' AND (slot = '%u' OR slot > '%u') ",
            ownerid,PET_SAVE_AS_CURRENT,PET_SAVE_LAST_STABLE_SLOT);

    if(!result)
        return false;

    Field *fields = result->Fetch();

    // update for case of current pet "slot = 0"
    petentry = fields[1].GetUInt32();
    if (!petentry)
    {
        delete result;
        m_loading = false;
        return false;
    }

    CreatureInfo const *creatureInfo = ObjectMgr::GetCreatureTemplate(petentry);
    if (!creatureInfo)
    {
        sLog.outError("Pet entry %u does not exist but used at pet load (owner: %s).", petentry, owner->GetGuidStr().c_str());
        delete result;
        m_loading = false;
        return false;
    }


    uint32 summon_spell_id = fields[16].GetUInt32();
    SpellEntry const* spellInfo = sSpellStore.LookupEntry(summon_spell_id);

    bool is_temporary_summoned = spellInfo && GetSpellDuration(spellInfo) > 0;

    // check temporary summoned pets like mage water elemental
    if (current && is_temporary_summoned)
    {
        delete result;
        return false;
    }

    PetType pet_type = PetType(fields[17].GetUInt8());
    if (pet_type == HUNTER_PET)
    {
        if (!creatureInfo->isTameable(owner->CanTameExoticPets()))
        {
            delete result;
            m_loading = false;
            return false;
        }
    }

    uint32 pet_number = fields[0].GetUInt32();

    if (current && owner->IsPetNeedBeTemporaryUnsummoned())
    {
        owner->SetTemporaryUnsummonedPetNumber(pet_number);
        delete result;
        m_loading = false;
        return false;
    }

    Map *map = owner->GetMap();

    CreatureCreatePos pos(owner, owner->GetOrientation(), PET_FOLLOW_DIST, PET_FOLLOW_ANGLE);

    uint32 guid = pos.GetMap()->GenerateLocalLowGuid(HIGHGUID_PET);
    if (!Create(guid, pos, creatureInfo, pet_number))
    {
        delete result;
        m_loading = false;
        return false;
    }

    setPetType(pet_type);
    setFaction(owner->getFaction());
    SetUInt32Value(UNIT_CREATED_BY_SPELL, summon_spell_id);

    m_actualSlot = PetSaveMode(fields[18].GetUInt32());

    // reget for sure use real creature info selected for Pet at load/creating
    CreatureInfo const *cinfo = GetCreatureInfo();
    if (cinfo->type == CREATURE_TYPE_CRITTER)
    {
        AIM_Initialize();
        pos.GetMap()->Add((Creature*)this);
        delete result;
        m_loading = false;
        return true;
    }

    m_charmInfo->SetPetNumber(pet_number, IsPermanentPetFor(owner));

    SetOwnerGuid(owner->GetObjectGuid());
    SetDisplayId(fields[3].GetUInt32());
    SetNativeDisplayId(fields[3].GetUInt32());
    uint32 petlevel = fields[4].GetUInt32();
    SetUInt32Value(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_NONE);
    SetName(fields[8].GetString());

    switch (getPetType())
    {
        case SUMMON_PET:
            petlevel=owner->getLevel();
            break;
        case HUNTER_PET:
            SetByteFlag(UNIT_FIELD_BYTES_2, 2, fields[9].GetBool() ? UNIT_CAN_BE_ABANDONED : UNIT_CAN_BE_RENAMED | UNIT_CAN_BE_ABANDONED);
            setPowerType(POWER_FOCUS);
            break;
        default:
            ERROR_LOG("Pet have incorrect type (%u) for pet loading.", getPetType());
    }

    if(owner->IsPvP())
        SetPvP(true);

    if(owner->IsFFAPvP())
        SetFFAPvP(true);

    SetCanModifyStats(true);
    InitStatsForLevel(petlevel);
    InitTalentForLevel();                                   // set original talents points before spell loading

    SetUInt32Value(UNIT_FIELD_PET_NAME_TIMESTAMP, uint32(time(NULL)));
    SetUInt32Value(UNIT_FIELD_PETEXPERIENCE, fields[5].GetUInt32());
    SetCreatorGuid(owner->GetObjectGuid());

    m_charmInfo->SetState(fields[6].GetUInt32());
    if (owner->IsMounted())
        m_charmInfo->SetState(CHARM_STATE_ACTION, ACTIONS_DISABLE);
    else
        m_charmInfo->SetState(CHARM_STATE_ACTION, ACTIONS_ENABLE);

    uint32 savedhealth = fields[10].GetUInt32();
    uint32 savedmana = fields[11].GetUInt32();

    // set current pet as current
    // 0=current
    // 1..MAX_PET_STABLES in stable slot
    // PET_SAVE_NOT_IN_SLOT(100) = not stable slot (summoning))
    if (fields[7].GetUInt32() != 0)
    {
        CharacterDatabase.BeginTransaction();

        static SqlStatementID id_1;
        static SqlStatementID id_2;

        SqlStatement stmt = CharacterDatabase.CreateStatement(id_1, "UPDATE character_pet SET slot = ? WHERE owner = ? AND slot = ? AND id <> ?");
        stmt.PExecute(uint32(PET_SAVE_NOT_IN_SLOT), ownerid, uint32(PET_SAVE_AS_CURRENT), m_charmInfo->GetPetNumber());

        stmt = CharacterDatabase.CreateStatement(id_2, "UPDATE character_pet SET slot = ? WHERE owner = ? AND id = ?");
        stmt.PExecute(uint32(PET_SAVE_AS_CURRENT), ownerid, m_charmInfo->GetPetNumber());

        CharacterDatabase.CommitTransaction();
    }

    // load action bar, if data broken will fill later by default spells.
    if (!is_temporary_summoned)
        m_charmInfo->LoadPetActionBar(fields[12].GetCppString());

    // since last save (in seconds)
    uint32 timediff = uint32(time(NULL) - fields[13].GetUInt64());

    m_resetTalentsCost = fields[14].GetUInt32();
    m_resetTalentsTime = fields[15].GetUInt64();

    delete result;

    //load spells/cooldowns/auras
    _LoadAuras(timediff);

    //init AB
    if (is_temporary_summoned)
    {
        // Temporary summoned pets always have initial spell list at load
        InitPetCreateSpells();
    }
    else
    {
        LearnPetPassives();
    }

    if (getPetType() == SUMMON_PET && !current)             //all (?) summon pets come with full health when called, but not when they are current
    {
        SetHealth(GetMaxHealth());
        SetPower(getPowerType(), GetMaxPower(getPowerType()));
    }
    else
    {
        SetHealth(savedhealth > GetMaxHealth() ? GetMaxHealth() : savedhealth);
        SetPower(getPowerType(), savedmana > GetMaxPower(getPowerType()) ? GetMaxPower(getPowerType()) : savedmana);
    }

    AIM_Initialize();
    map->Add((Creature*)this);

    // Spells should be loaded after pet is added to map, because in CheckCast is check on it
    _LoadSpells();
    InitLevelupSpellsForLevel();

    CleanupActionBar();                                     // remove unknown spells from action bar after load

    _LoadSpellCooldowns();

    owner->SetPet(this);                                    // in DB stored only full controlled creature
    DEBUG_LOG("New Pet has guid %u", GetGUIDLow());

    if (owner->GetTypeId() == TYPEID_PLAYER)
    {
        ((Player*)owner)->PetSpellInitialize();
        if(((Player*)owner)->GetGroup())
            ((Player*)owner)->SetGroupUpdateFlag(GROUP_UPDATE_PET);

        ((Player*)owner)->SendTalentsInfoData(true);
    }

    if (owner->GetTypeId() == TYPEID_PLAYER && getPetType() == HUNTER_PET)
    {
        result = CharacterDatabase.PQuery("SELECT genitive, dative, accusative, instrumental, prepositional FROM character_pet_declinedname WHERE owner = '%u' AND id = '%u'", owner->GetGUIDLow(), GetCharmInfo()->GetPetNumber());

        if(result)
        {
            delete m_declinedname;

            m_declinedname = new DeclinedName;
            Field *fields2 = result->Fetch();
            for(int i = 0; i < MAX_DECLINED_NAME_CASES; ++i)
                m_declinedname->name[i] = fields2[i].GetCppString();

            delete result;
        }
    }

    m_loading = false;

    SynchronizeLevelWithOwner();

    if (!is_temporary_summoned)
        CastPetAuras(current);

    // Ghoul emote
    if (GetEntry() == 24207 || GetEntry() == 26125 || GetEntry() == 28528)
        HandleEmote(EMOTE_ONESHOT_EMERGE);

    if (owner->GetTypeId() == TYPEID_PLAYER)
        if (BattleGround* bg = owner->GetBattleGround())
            if (bg->isArena() && bg->GetStatus() == STATUS_WAIT_JOIN)
                RemoveSpellCooldowns();

    return true;
}

void Pet::SavePetToDB(PetSaveMode mode)
{
    if (!GetEntry())
        return;

    // save only fully controlled creature
    if (!isControlled())
        return;

    // not save not player pets
    if (!GetOwnerGuid().IsPlayer())
        return;

    Player* pOwner = (Player*)GetOwner();
    if (!pOwner)
        return;

    // current/stable/not_in_slot
    if (mode >= PET_SAVE_AS_CURRENT)
    {
        // reagents must be returned before save call
        if (mode == PET_SAVE_REAGENTS)
            mode = PET_SAVE_NOT_IN_SLOT;
        // not save pet as current if another pet temporary unsummoned
        else if (mode == PET_SAVE_AS_CURRENT && pOwner->GetTemporaryUnsummonedPetNumber() &&
            pOwner->GetTemporaryUnsummonedPetNumber() != m_charmInfo->GetPetNumber())
        {
            // pet will lost anyway at restore temporary unsummoned
            if(getPetType()==HUNTER_PET)
                return;

            // for warlock case
            mode = PET_SAVE_NOT_IN_SLOT;
        }

        uint32 curhealth = GetHealth();
        uint32 curmana = GetPower(POWER_MANA);

        // stable and not in slot saves
        if (mode != PET_SAVE_AS_CURRENT)
            RemoveAllAuras();

        //save pet's data as one single transaction
        CharacterDatabase.BeginTransaction();
        _SaveSpells();
        _SaveSpellCooldowns();
        _SaveAuras();

        uint32 ownerLow = GetOwnerGuid().GetCounter();
        // remove current data
        static SqlStatementID delPet ;
        static SqlStatementID insPet ;

        SqlStatement stmt = CharacterDatabase.CreateStatement(delPet, "DELETE FROM character_pet WHERE owner = ? AND id = ?");
        stmt.PExecute(ownerLow, m_charmInfo->GetPetNumber());

        // prevent duplicate using slot (except PET_SAVE_NOT_IN_SLOT)
        if (mode <= PET_SAVE_LAST_STABLE_SLOT)
        {
            static SqlStatementID updPet ;

            stmt = CharacterDatabase.CreateStatement(updPet, "UPDATE character_pet SET slot = ? WHERE owner = ? AND slot = ?");
            stmt.PExecute(uint32(PET_SAVE_NOT_IN_SLOT), ownerLow, uint32(mode));
        }

        // prevent existence another hunter pet in PET_SAVE_AS_CURRENT and PET_SAVE_NOT_IN_SLOT
        if (getPetType() == HUNTER_PET && mode > PET_SAVE_LAST_STABLE_SLOT)
        {
            static SqlStatementID del ;

            stmt = CharacterDatabase.CreateStatement(del, "UPDATE character_pet SET slot = actual_slot WHERE owner = ? AND slot > ?");
            stmt.PExecute(ownerLow, uint32(PET_SAVE_LAST_STABLE_SLOT));
        }

        // save pet
        SqlStatement savePet = CharacterDatabase.CreateStatement(insPet, "INSERT INTO character_pet ( id, entry,  owner, modelid, level, exp, Reactstate, slot, name, renamed, curhealth, "
            "curmana, abdata, savetime, resettalents_cost, resettalents_time, CreatedBySpell, PetType, actual_slot) "
             "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");

        savePet.addUInt32(m_charmInfo->GetPetNumber());
        savePet.addUInt32(GetEntry());
        savePet.addUInt32(ownerLow);
        savePet.addUInt32(GetNativeDisplayId());
        savePet.addUInt32(getLevel());
        savePet.addUInt32(GetUInt32Value(UNIT_FIELD_PETEXPERIENCE));
        savePet.addUInt32(uint32(m_charmInfo->GetState()));
        savePet.addUInt32(uint32(mode));
        savePet.addString(m_name);
        savePet.addUInt32(uint32(HasByteFlag(UNIT_FIELD_BYTES_2, 2, UNIT_CAN_BE_RENAMED) ? 0 : 1));
        savePet.addUInt32((curhealth < 1 ? 1 : curhealth));
        savePet.addUInt32(curmana);

        std::ostringstream ss;
        for(uint32 i = ACTION_BAR_INDEX_START; i < ACTION_BAR_INDEX_END; ++i)
        {
            ss << uint32(m_charmInfo->GetActionBarEntry(i)->GetType()) << " "
               << uint32(m_charmInfo->GetActionBarEntry(i)->GetAction()) << " ";
        };
        savePet.addString(ss);

        savePet.addUInt64(uint64(time(NULL)));
        savePet.addUInt32(uint32(m_resetTalentsCost));
        savePet.addUInt64(uint64(m_resetTalentsTime));
        savePet.addUInt32(GetUInt32Value(UNIT_CREATED_BY_SPELL));
        savePet.addUInt32(uint32(getPetType()));
        savePet.addUInt32(uint32(m_actualSlot));

        savePet.Execute();
        CharacterDatabase.CommitTransaction();
    }
    else
    {
        RemoveAllAuras(AURA_REMOVE_BY_DELETE);
        DeleteFromDB(m_charmInfo->GetPetNumber());
    }
}

void Pet::DeleteFromDB(uint32 guidlow, bool separate_transaction)
{
    if(separate_transaction)
        CharacterDatabase.BeginTransaction();

    static SqlStatementID delPet ;
    static SqlStatementID delDeclName ;
    static SqlStatementID delAuras ;
    static SqlStatementID delSpells ;
    static SqlStatementID delSpellCD ;

    SqlStatement stmt = CharacterDatabase.CreateStatement(delPet, "DELETE FROM character_pet WHERE id = ?");
    stmt.PExecute(guidlow);

    stmt = CharacterDatabase.CreateStatement(delDeclName, "DELETE FROM character_pet_declinedname WHERE id = ?");
    stmt.PExecute(guidlow);

    stmt = CharacterDatabase.CreateStatement(delAuras, "DELETE FROM pet_aura WHERE guid = ?");
    stmt.PExecute(guidlow);

    stmt = CharacterDatabase.CreateStatement(delSpells, "DELETE FROM pet_spell WHERE guid = ?");
    stmt.PExecute(guidlow);

    stmt = CharacterDatabase.CreateStatement(delSpellCD, "DELETE FROM pet_spell_cooldown WHERE guid = ?");
    stmt.PExecute(guidlow);

    if(separate_transaction)
        CharacterDatabase.CommitTransaction();
}

void Pet::SetDeathState(DeathState s)                       // overwrite virtual Creature::SetDeathState and Unit::SetDeathState
{
    Creature::SetDeathState(s);
    if(getDeathState()==CORPSE)
    {
        //remove summoned pet (no corpse)
        if(getPetType()==SUMMON_PET)
            Unsummon(PET_SAVE_NOT_IN_SLOT);
        // other will despawn at corpse desppawning (Pet::Update code)
        else
        {
            // pet corpse non lootable and non skinnable
            SetUInt32Value(UNIT_DYNAMIC_FLAGS, UNIT_DYNFLAG_NONE);
            RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_SKINNABLE);

            //SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_STUNNED);
        }
        // send cooldown for summon spell if necessary
        if (Player* p_owner = GetCharmerOrOwnerPlayerOrPlayerItself())
        {
            SpellEntry const *spellInfo = sSpellStore.LookupEntry(GetUInt32Value(UNIT_CREATED_BY_SPELL));
            if (spellInfo && (spellInfo->Attributes & SPELL_ATTR_DISABLED_WHILE_ACTIVE))
            {
                p_owner->SendCooldownEvent(spellInfo);
                // Raise Dead hack
                if (SpellClassOptionsEntry const * opt = spellInfo->GetSpellClassOptions())
                    if (opt->SpellFamilyName == SPELLFAMILY_DEATHKNIGHT && opt->SpellFamilyFlags & 0x1000)
                        if (spellInfo = sSpellStore.LookupEntry(46584))
                            p_owner->SendCooldownEvent(spellInfo);
            }
        }
    }
    else if(getDeathState()==ALIVE)
    {
        //RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_STUNNED);
        CastPetAuras(true);
    }
}

void Pet::Update(uint32 update_diff, uint32 diff)
{
    if(m_removed)                                           // pet already removed, just wait in remove queue, no updates
        return;

    switch( m_deathState )
    {
        case CORPSE:
        {
            if (m_corpseDecayTimer <= update_diff)
            {
                MANGOS_ASSERT(getPetType()!=SUMMON_PET && "Must be already removed.");
                Unsummon(PET_SAVE_NOT_IN_SLOT);             //hunters' pets never get removed because of death, NEVER!
                return;
            }
            break;
        }
        case ALIVE:
        case GHOULED:
        {
            // unsummon pet that lost owner
            Unit* owner = GetOwner();
            if (!owner ||
                (!IsWithinDistInMap(owner, GetMap()->GetVisibilityDistance()) && (owner->GetCharmGuid() && (owner->GetCharmGuid() != GetObjectGuid()))) ||
                (isControlled() && !owner->GetPetGuid()))
            {
                Unsummon(PET_SAVE_REAGENTS);
                return;
            }

            if (isControlled())
            {
                if( owner->GetPetGuid() != GetObjectGuid() && GetEntry() != 29264 && GetEntry() != 31216) //want 2 wolves ;)
                {
                    Unsummon(getPetType() == HUNTER_PET ? PET_SAVE_AS_DELETED : PET_SAVE_NOT_IN_SLOT, owner);
                    return;
                }
            }

            if (m_duration > 0)
            {
                if(m_duration > (int32)update_diff)
                    m_duration -= (int32)update_diff;
                else
                {
                    Unsummon(getPetType() != SUMMON_PET ? PET_SAVE_AS_DELETED : PET_SAVE_NOT_IN_SLOT, owner);
                    return;
                }
            }

            //regenerate focus for hunter pets or energy for deathknight's ghoul
            if(m_regenTimer <= update_diff)
            {
                switch (getPowerType())
                {
                    case POWER_FOCUS:
                    case POWER_ENERGY:
                        Regenerate(getPowerType());
                        break;
                    default:
                        break;
                }
                m_regenTimer = 4000;
            }
            else
                m_regenTimer -= update_diff;

            if(getPetType() != HUNTER_PET)
                break;
            break;
        }
        default:
            break;
    }

    Creature::Update(update_diff, diff);
}

bool Pet::CanTakeMoreActiveSpells(uint32 spellid)
{
    uint8  activecount = 1;
    uint32 chainstartstore[ACTIVE_SPELLS_MAX];

    if(IsPassiveSpell(spellid))
        return true;

    chainstartstore[0] = sSpellMgr.GetFirstSpellInChain(spellid);

    for (PetSpellMap::const_iterator itr = m_spells.begin(); itr != m_spells.end(); ++itr)
    {
        if(itr->second.state == PETSPELL_REMOVED)
            continue;

        if(IsPassiveSpell(itr->first))
            continue;

        uint32 chainstart = sSpellMgr.GetFirstSpellInChain(itr->first);

        uint8 x;

        for (x = 0; x < activecount; ++x)
        {
            if (chainstart == chainstartstore[x])
                break;
        }

        if (x == activecount)                                //spellchain not yet saved -> add active count
        {
            ++activecount;
            if (activecount > ACTIVE_SPELLS_MAX)
                return false;
            chainstartstore[x] = chainstart;
        }
    }
    return true;
}

void Pet::Unsummon(PetSaveMode mode, Unit* owner /*= NULL*/)
{
    if (!owner)
        owner = GetOwner();

    CombatStop();

    if(owner)
    {
        if (GetOwnerGuid() != owner->GetObjectGuid())
            return;

        Player* p_owner = owner->GetTypeId()==TYPEID_PLAYER ? (Player*)owner : NULL;

        if (p_owner)
        {

            // not save secondary permanent pet as current
            if (mode == PET_SAVE_AS_CURRENT && p_owner->GetTemporaryUnsummonedPetNumber() &&
                p_owner->GetTemporaryUnsummonedPetNumber() != GetCharmInfo()->GetPetNumber())
                mode = PET_SAVE_NOT_IN_SLOT;

            //returning of reagents only for players, so best done here
            uint32 spellId = GetUInt32Value(UNIT_CREATED_BY_SPELL);
            SpellEntry const *spellInfo = sSpellStore.LookupEntry(spellId);

            if (mode == PET_SAVE_REAGENTS)
            {
                SpellReagentsEntry const* spellReagents = spellInfo ? spellInfo->GetSpellReagents() : NULL;

                if (spellReagents)
                {
                    for(uint32 i = 0; i < MAX_SPELL_REAGENTS; ++i)
                    {
                        if (spellReagents->Reagent[i] > 0)
                        {
                            ItemPosCountVec dest;           //for succubus, voidwalker, felhunter and felguard credit soulshard when despawn reason other than death (out of range, logout)
                            uint8 msg = p_owner->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, spellReagents->Reagent[i], spellReagents->ReagentCount[i]);
                            if (msg == EQUIP_ERR_OK)
                            {
                                Item* item = p_owner->StoreNewItem(dest, spellReagents->Reagent[i], true);
                                if (p_owner->IsInWorld())
                                    p_owner->SendNewItem(item, spellReagents->ReagentCount[i], true, false);
                            }
                        }
                    }
                }
            }
            if (mode != PET_SAVE_AS_CURRENT && spellInfo)
                // cooldown, only if pet is not death already (corpse)
                if (spellInfo->Attributes & SPELL_ATTR_DISABLED_WHILE_ACTIVE && getDeathState() != CORPSE)
                {
                    p_owner->SendCooldownEvent(spellInfo);
                    // Raise Dead hack
                    if (SpellClassOptionsEntry const * opt = spellInfo->GetSpellClassOptions())
                        if (opt->SpellFamilyName == SPELLFAMILY_DEATHKNIGHT && opt->SpellFamilyFlags & 0x1000)
                            if (spellInfo = sSpellStore.LookupEntry(46584))
                                p_owner->SendCooldownEvent(spellInfo);
                }

            if (isControlled())
            {
                p_owner->RemovePetActionBar();

                if (p_owner->GetGroup())
                    p_owner->SetGroupUpdateFlag(GROUP_UPDATE_PET);
            }
        }

        // only if current pet in slot
        switch(getPetType())
        {
            case MINI_PET:
                if (p_owner)
                    p_owner->SetMiniPet(NULL);
                break;
            case PROTECTOR_PET:
            case GUARDIAN_PET:
                owner->RemoveGuardian(this);
                break;
            default:
                if (owner->GetPetGuid() == GetObjectGuid())
                    owner->SetPet(NULL);
                break;
        }
    }

    SavePetToDB(mode);
    AddObjectToRemoveList();
    m_removed = true;
}

void Pet::GivePetXP(uint32 xp)
{
    if(getPetType() != HUNTER_PET)
        return;

    if ( xp < 1 )
        return;

    if(!isAlive())
        return;

    uint32 level = getLevel();
    uint32 maxlevel = std::min(sWorld.getConfig(CONFIG_UINT32_MAX_PLAYER_LEVEL), GetOwner()->getLevel());

    // pet not receive xp for level equal to owner level
    if (level >= maxlevel)
        return;

    uint32 nextLvlXP = GetUInt32Value(UNIT_FIELD_PETNEXTLEVELEXP);
    uint32 curXP = GetUInt32Value(UNIT_FIELD_PETEXPERIENCE);
    uint32 newXP = curXP + xp;

    while( newXP >= nextLvlXP && level < maxlevel)
    {
        newXP -= nextLvlXP;
        ++level;

        GivePetLevel(level);                              // also update UNIT_FIELD_PETNEXTLEVELEXP and UNIT_FIELD_PETEXPERIENCE to level start

        nextLvlXP = GetUInt32Value(UNIT_FIELD_PETNEXTLEVELEXP);
    }

    SetUInt32Value(UNIT_FIELD_PETEXPERIENCE, level < maxlevel ? newXP : 0);
}

void Pet::GivePetLevel(uint32 level)
{
    if (!level || level == getLevel())
        return;

    if (getPetType()==HUNTER_PET)
    {
        SetUInt32Value(UNIT_FIELD_PETEXPERIENCE, 0);
        SetUInt32Value(UNIT_FIELD_PETNEXTLEVELEXP, sObjectMgr.GetXPForPetLevel(level));
    }

    InitStatsForLevel(level);
    InitLevelupSpellsForLevel();
    InitTalentForLevel();
}

bool Pet::CreateBaseAtCreature(Creature* creature)
{
    if(!creature)
    {
        ERROR_LOG("CRITICAL: NULL pointer passed into CreateBaseAtCreature()");
        return false;
    }

    CreatureCreatePos pos(creature, creature->GetOrientation());

    uint32 guid = creature->GetMap()->GenerateLocalLowGuid(HIGHGUID_PET);

    BASIC_LOG("Create pet");
    uint32 pet_number = sObjectMgr.GeneratePetNumber();
    if (!Create(guid, pos, creature->GetCreatureInfo(), pet_number))
        return false;

    CreatureInfo const *cinfo = GetCreatureInfo();
    if(!cinfo)
    {
        ERROR_LOG("CreateBaseAtCreature() failed, creatureInfo is missing!");
        return false;
    }

    if(cinfo->type == CREATURE_TYPE_CRITTER)
    {
        setPetType(MINI_PET);
        return true;
    }
    SetDisplayId(creature->GetDisplayId());
    SetNativeDisplayId(creature->GetNativeDisplayId());
    setPowerType(POWER_FOCUS);
    SetUInt32Value(UNIT_FIELD_PET_NAME_TIMESTAMP, 0);
    SetUInt32Value(UNIT_FIELD_PETEXPERIENCE, 0);
    SetUInt32Value(UNIT_FIELD_PETNEXTLEVELEXP, sObjectMgr.GetXPForPetLevel(creature->getLevel()));
    SetUInt32Value(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_NONE);

    if(CreatureFamilyEntry const* cFamily = sCreatureFamilyStore.LookupEntry(cinfo->family))
        SetName(cFamily->Name[sWorld.GetDefaultDbcLocale()]);
    else
        SetName(creature->GetNameForLocaleIdx(sObjectMgr.GetDBCLocaleIndex()));

    if(cinfo->type == CREATURE_TYPE_BEAST)
    {
        SetByteValue(UNIT_FIELD_BYTES_0, 1, CLASS_WARRIOR);
        SetByteValue(UNIT_FIELD_BYTES_0, 2, GENDER_NONE);
        SetByteValue(UNIT_FIELD_BYTES_0, 3, POWER_FOCUS);
        SetSheath(SHEATH_STATE_MELEE);
        SetByteFlag(UNIT_FIELD_BYTES_2, 2, UNIT_CAN_BE_RENAMED | UNIT_CAN_BE_ABANDONED);
        SetUInt32Value(UNIT_MOD_CAST_SPEED, creature->GetUInt32Value(UNIT_MOD_CAST_SPEED));
    }
    return true;
}

bool Pet::InitStatsForLevel(uint32 petlevel, Unit* owner)
{
    CreatureInfo const *cinfo = GetCreatureInfo();
    MANGOS_ASSERT(cinfo);

    if (!owner)
    {
        // Do not remove *owner as function argument, GetOwner() won't work on summoning pet if the owner
        // is no player, because pet is not yet in world; see ObjectAccessor::GetUnit
        owner = GetOwner();
        if(!owner)
        {
            ERROR_LOG("attempt to summon pet (Entry %u) without owner! Attempt terminated.", cinfo->Entry);
            return false;
        }
    }

    if (!petlevel)
        petlevel = owner->getLevel();

    SetLevel(petlevel);

    PetLevelInfo const* pInfo = sObjectMgr.GetPetLevelInfo(cinfo->Entry, petlevel);

    SetMeleeDamageSchool(SpellSchools(cinfo->dmgschool));

    int32 createStats[MAX_STATS+7] =  {22,     // STAT_STRENGTH
                                       22,     // STAT_AGILITY
                                       25,     // STAT_STAMINA
                                       28,     // STAT_INTELLECT
                                       27,     // STAT_SPIRIT
                                       42,     // Base HEALTH
                                       20,     // Base POWER/MANA
                                       10,     // Base AttackPower
                                       5,      // Base MinDamage
                                       10,     // Base MaxDamage
                                       1,      // Base MinRangeDamage
                                       3};     // Base MaxRangeDamage

    uint32 createResistance[MAX_SPELL_SCHOOL] = {0,0,0,0,0,0,0};

    if(cinfo) // Default create values (from creature_template)
    {
        // Resistances
        createResistance[SPELL_SCHOOL_HOLY]   = cinfo->resistance1;
        createResistance[SPELL_SCHOOL_FIRE]   = cinfo->resistance2;
        createResistance[SPELL_SCHOOL_NATURE] = cinfo->resistance3;
        createResistance[SPELL_SCHOOL_FROST]  = cinfo->resistance4;
        createResistance[SPELL_SCHOOL_SHADOW] = cinfo->resistance5;
        createResistance[SPELL_SCHOOL_ARCANE] = cinfo->resistance6;
        // Armor
        createResistance[SPELL_SCHOOL_NORMAL] = int32(cinfo->armor  * petlevel / cinfo->maxlevel / (1 +  cinfo->rank));

        for (int i = 0; i < MAX_STATS; ++i)
            createStats[i] *= petlevel/10;

        createStats[MAX_STATS]    = int32(cinfo->maxhealth * petlevel / cinfo->maxlevel / (1 +  cinfo->rank));
        createStats[MAX_STATS+1]  = int32(cinfo->maxmana * petlevel / cinfo->maxlevel / (1 +  cinfo->rank));
        createStats[MAX_STATS+2]  = int32(cinfo->attackpower * petlevel / cinfo->maxlevel / (1 +  cinfo->rank));
        createStats[MAX_STATS+3]  = int32(cinfo->mindmg * petlevel / cinfo->maxlevel / (1 + cinfo->rank));
        createStats[MAX_STATS+4]  = int32(cinfo->maxdmg * petlevel / cinfo->maxlevel / (1 + cinfo->rank));
        createStats[MAX_STATS+5]  = int32(cinfo->minrangedmg * petlevel / cinfo->maxlevel/ (1 + cinfo->rank));
        createStats[MAX_STATS+6]  = int32(cinfo->maxrangedmg * petlevel / cinfo->maxlevel/ (1 + cinfo->rank));
        SetFloatValue(UNIT_FIELD_MAXRANGEDDAMAGE, float(cinfo->maxrangedmg * petlevel / cinfo->maxlevel));
        setPowerType(Powers(cinfo->powerType));
        SetAttackTime(BASE_ATTACK, cinfo->baseattacktime);
        SetAttackTime(RANGED_ATTACK, cinfo->rangeattacktime);
    }
    else
    {
        SetAttackTime(BASE_ATTACK, BASE_ATTACK_TIME);
        SetAttackTime(RANGED_ATTACK, BASE_ATTACK_TIME);

        for (int i = 0; i < MAX_STATS+7; ++i)
            createStats[i] *= petlevel/10;
        // Armor
        createResistance[SPELL_SCHOOL_NORMAL] = petlevel*50;
    }

    switch(getPetType())
    {
        case SUMMON_PET:
        {
            if (cinfo->family == CREATURE_FAMILY_GHOUL)
            {
                setPowerType(POWER_ENERGY);
                SetByteValue(UNIT_FIELD_BYTES_0, 1, CLASS_ROGUE);
            }
            else
                SetByteValue(UNIT_FIELD_BYTES_0, 1, CLASS_MAGE);

            // this enables popup window (pet dismiss, cancel)
            SetUInt32Value(UNIT_FIELD_FLAGS, UNIT_FLAG_PVP_ATTACKABLE);
            break;
        }
        case HUNTER_PET:
        {
            if (!pInfo)         //If no pet levelstats in DB - use 1 for default hunter pet
                pInfo = sObjectMgr.GetPetLevelInfo(1, petlevel);

            SetByteValue(UNIT_FIELD_BYTES_0, 1, CLASS_WARRIOR);
            SetByteValue(UNIT_FIELD_BYTES_0, 2, GENDER_NONE);
            SetSheath(SHEATH_STATE_MELEE);

            // this enables popup window (pet abandon, cancel)
            SetUInt32Value(UNIT_FIELD_FLAGS, UNIT_FLAG_PVP_ATTACKABLE);

            CreatureFamilyEntry const* cFamily = sCreatureFamilyStore.LookupEntry(cinfo->family);
            if(cFamily && cFamily->minScale > 0.0f)
            {
                float scale;
                if (getLevel() >= cFamily->maxScaleLevel)
                    scale = cFamily->maxScale;
                else if (getLevel() <= cFamily->minScaleLevel)
                    scale = cFamily->minScale;
                else
                    scale = cFamily->minScale + float(getLevel() - cFamily->minScaleLevel) / cFamily->maxScaleLevel * (cFamily->maxScale - cFamily->minScale);

                SetObjectScale(scale);
                UpdateModelData();
            }

            SetUInt32Value(UNIT_FIELD_PETNEXTLEVELEXP, sObjectMgr.GetXPForPetLevel(petlevel));
            setPowerType(POWER_FOCUS);
            break;
        }
        case GUARDIAN_PET:
        case PROTECTOR_PET:
        {
            SetUInt32Value(UNIT_FIELD_PETEXPERIENCE, 0);
            SetUInt32Value(UNIT_FIELD_PETNEXTLEVELEXP, 1000);
            // DK ghouls have energy
            if (cinfo->family == CREATURE_FAMILY_GHOUL)
                setPowerType(POWER_ENERGY);
            break;
        }
        case MINI_PET:
        default:
            return true;
    }

    if(pInfo)                                       // exist in DB
    {
        if (pInfo->health)
            SetCreateHealth(pInfo->health);
        else
            SetCreateHealth(createStats[MAX_STATS]);

        if (pInfo->mana)
            SetCreateMana(pInfo->mana);
        else
            SetCreateMana(createStats[MAX_STATS+1]);

        if (pInfo->armor)
            SetModifierValue(UNIT_MOD_ARMOR, BASE_VALUE, float(pInfo->armor));
        else
            SetModifierValue(UNIT_MOD_ARMOR, BASE_VALUE,  float(createResistance[SPELL_SCHOOL_NORMAL]));

        for( int i = STAT_STRENGTH; i < MAX_STATS; ++i)
            if (pInfo->stats[i])
               SetCreateStat(Stats(i), float(pInfo->stats[i]));
            else
               SetCreateStat(Stats(i), float(createStats[i]));

        if (pInfo->attackpower)
            SetModifierValue(UNIT_MOD_ATTACK_POWER, BASE_VALUE, float(pInfo->attackpower));
        else
            SetModifierValue(UNIT_MOD_ATTACK_POWER, BASE_VALUE, float(createStats[MAX_STATS+2]));

        if (pInfo->mindmg)
            SetBaseWeaponDamage(BASE_ATTACK, MINDAMAGE, float(pInfo->mindmg));
        else
            SetBaseWeaponDamage(BASE_ATTACK, MINDAMAGE, float(createStats[MAX_STATS+3]));

        if (pInfo->maxdmg)
            SetBaseWeaponDamage(BASE_ATTACK, MAXDAMAGE, float(pInfo->maxdmg));
        else
            SetBaseWeaponDamage(BASE_ATTACK, MAXDAMAGE, float(createStats[MAX_STATS+4]));

        SetFloatValue(UNIT_FIELD_MINRANGEDDAMAGE,float(createStats[MAX_STATS+5]));
        SetFloatValue(UNIT_FIELD_MAXRANGEDDAMAGE,float(createStats[MAX_STATS+6]));

        DEBUG_LOG("Pet %u stats for level initialized (from pet_levelstat values)", cinfo->Entry);
    }
    else                                            // not exist in DB, use some default fake data
    {
        DEBUG_LOG("Summoned pet (Entry: %u) not have pet stats data in DB. Use hardcoded values.",cinfo->Entry);
        for (int i = STAT_STRENGTH; i < MAX_STATS; ++i)
            SetCreateStat(Stats(i),float(createStats[i]));

        SetCreateHealth(createStats[MAX_STATS]);
        SetCreateMana(createStats[MAX_STATS+1]);
        SetModifierValue(UNIT_MOD_ARMOR, BASE_VALUE,  float(createResistance[SPELL_SCHOOL_NORMAL]));

        SetModifierValue(UNIT_MOD_ATTACK_POWER, BASE_VALUE, float(createStats[MAX_STATS+2]));

        SetBaseWeaponDamage(BASE_ATTACK, MINDAMAGE, float(createStats[MAX_STATS+3]));
        SetBaseWeaponDamage(BASE_ATTACK, MAXDAMAGE, float(createStats[MAX_STATS+4]));

        SetFloatValue(UNIT_FIELD_MINRANGEDDAMAGE, float(createStats[MAX_STATS+5]));
        SetFloatValue(UNIT_FIELD_MAXRANGEDDAMAGE, float(createStats[MAX_STATS+6]));

        DEBUG_LOG("Pet %u stats for level initialized (from creature_template values)", cinfo->Entry);
    }

    for (int i = SPELL_SCHOOL_HOLY; i < MAX_SPELL_SCHOOL; ++i)
            SetModifierValue(UnitMods(UNIT_MOD_RESISTANCE_START + i), BASE_VALUE, float(createResistance[i]));

    SetModifierValue(UNIT_MOD_ATTACK_POWER, BASE_PCT, 1.0f);
    SetAttackTime(OFF_ATTACK, BASE_ATTACK_TIME);

    // custom stat calculation
    switch (getPetType())
    {
        case SUMMON_PET:
        case GUARDIAN_PET:
        case PROTECTOR_PET:
        {
            float statBonus[MAX_STATS] = {0, 0, 0, 0, 0},
                  armorBonus = 0,
                  apBonus = 0,
                  bonusDamage = 0;

            // some pets scale not dynamically with master's stats (don't have scaling auras),
            // we just add the bonus here as unit mods
            switch (cinfo->Entry)
            {
                // mage's Water Elemental
                case 510:
                case 37994:
                {
                    statBonus[STAT_STAMINA] = owner->GetStat(STAT_STAMINA) * 0.45f;
                    statBonus[STAT_INTELLECT] = owner->GetStat(STAT_INTELLECT) * 0.3f;
                    armorBonus = owner->GetArmor() * 0.35f;
                    bonusDamage = owner->SpellBaseDamageBonusDone(SPELL_SCHOOL_MASK_FROST) * 0.4f;
                    break;
                }
                // Treants
                case 1964:
                {
                    float dmgBonus = owner->GetUInt32Value(PLAYER_FIELD_MOD_DAMAGE_DONE_POS + SPELL_SCHOOL_NATURE) * 0.12f;
                    SetBaseWeaponDamage(BASE_ATTACK, MINDAMAGE, uint32(cinfo->mindmg + dmgBonus));
                    SetBaseWeaponDamage(BASE_ATTACK, MAXDAMAGE, uint32(cinfo->maxdmg + dmgBonus));
                    break;
                }
                // priest's Shadowfiend
                case 19668:
                {
                    statBonus[STAT_STAMINA] = owner->GetStat(STAT_STAMINA) * 0.45f;
                    float dmgBonus = owner->GetUInt32Value(PLAYER_FIELD_MOD_DAMAGE_DONE_POS + SPELL_SCHOOL_SHADOW);
                    SetBaseWeaponDamage(BASE_ATTACK, MINDAMAGE, uint32(cinfo->mindmg + dmgBonus));
                    SetBaseWeaponDamage(BASE_ATTACK, MAXDAMAGE, uint32(cinfo->maxdmg + dmgBonus));
                    break;
                }
                // Death Knight Gargoyle
                case 27829:
                {
                    statBonus[STAT_STAMINA] = owner->GetStat(STAT_STAMINA) * 0.45f;
                    bonusDamage = owner->GetTotalAttackPowerValue(BASE_ATTACK) * 0.4f;
                    break;
                }
                // Feral Spirit Wolves
                case 29264:
                {
                    // 30% of owners stamina, 35% of owners armor
                    statBonus[STAT_STAMINA] = owner->GetStat(STAT_STAMINA) * 0.3f;
                    armorBonus = owner->GetArmor() * 0.35f;

                    // 30% of masters attack power, modified by dummy aura
                    apBonus = 30;
                    if (Aura* pDummy = owner->GetDummyAura(63271))
                        apBonus += pDummy->GetModifier()->m_miscvalue;
                    apBonus = apBonus * owner->GetTotalAttackPowerValue(BASE_ATTACK) / 100;

                    // TODO: hitchance with spell 61783 (cast manually...? not in petspell dbc's :-/)
                    break;
                }
                // Mirror Image
                case 31216:
                {
                    bonusDamage = owner->GetMaxSpellBaseDamageBonusDone(SPELL_SCHOOL_MASK_MAGIC);
                    break;
                }
                default:
                    break;
            }

            for (uint8 i = 0; i < MAX_STATS; i++)
                HandleStatModifier(UnitMods(UNIT_MOD_STAT_START + i), TOTAL_VALUE, statBonus[i] > 0 ? statBonus[i] : 0, true);

            HandleStatModifier(UNIT_MOD_ARMOR, TOTAL_VALUE, armorBonus > 0 ? armorBonus : 0, true);
            HandleStatModifier(UNIT_MOD_ATTACK_POWER, TOTAL_VALUE, apBonus > 0 ? apBonus : 0, true);
            SetBonusDamage(bonusDamage > 0 ? bonusDamage : 0);

            break;
        }
        default:
             break;
    }

    SetFloatValue(UNIT_MOD_CAST_SPEED, 1.0f);

    UpdateAllStats();

    SetHealth(GetMaxHealth());
    SetPower(getPowerType(), GetMaxPower(getPowerType()));

    return true;
}

bool Pet::HaveInDiet(ItemPrototype const* item) const
{
    if (!item->FoodType)
        return false;

    CreatureInfo const* cInfo = GetCreatureInfo();
    if(!cInfo)
        return false;

    CreatureFamilyEntry const* cFamily = sCreatureFamilyStore.LookupEntry(cInfo->family);
    if(!cFamily)
        return false;

    uint32 diet = cFamily->petFoodMask;
    uint32 FoodMask = 1 << (item->FoodType-1);
    return diet & FoodMask;
}

uint32 Pet::GetCurrentFoodBenefitLevel(uint32 itemlevel)
{
    // -5 or greater food level
    if(getLevel() <= itemlevel + 5)                         //possible to feed level 60 pet with level 55 level food for full effect
        return 35000;
    // -10..-6
    else if(getLevel() <= itemlevel + 10)                   //pure guess, but sounds good
        return 17000;
    // -14..-11
    else if(getLevel() <= itemlevel + 14)                   //level 55 food gets green on 70, makes sense to me
        return 8000;
    // -15 or less
    else
        return 0;                                           //food too low level
}

void Pet::_LoadSpellCooldowns()
{
    m_CreatureSpellCooldowns.clear();
    m_CreatureCategoryCooldowns.clear();

    QueryResult *result = CharacterDatabase.PQuery("SELECT spell,time FROM pet_spell_cooldown WHERE guid = '%u'",m_charmInfo->GetPetNumber());

    if(result)
    {
        time_t curTime = time(NULL);

        WorldPacket data(SMSG_SPELL_COOLDOWN, (8+1+size_t(result->GetRowCount())*8));
        data << ObjectGuid(GetObjectGuid());
        data << uint8(0x0);                                 // flags (0x1, 0x2)

        do
        {
            Field *fields = result->Fetch();

            uint32 spell_id = fields[0].GetUInt32();
            time_t db_time  = (time_t)fields[1].GetUInt64();

            if(!sSpellStore.LookupEntry(spell_id))
            {
                ERROR_LOG("Pet %u have unknown spell %u in `pet_spell_cooldown`, skipping.",m_charmInfo->GetPetNumber(),spell_id);
                continue;
            }

            // skip outdated cooldown
            if(db_time <= curTime)
                continue;

            data << uint32(spell_id);
            data << uint32(uint32(db_time-curTime)*IN_MILLISECONDS);

            _AddCreatureSpellCooldown(spell_id,db_time);

            DEBUG_LOG("Pet (Number: %u) spell %u cooldown loaded (%u secs).", m_charmInfo->GetPetNumber(), spell_id, uint32(db_time-curTime));
        }
        while( result->NextRow() );

        delete result;

        if(!m_CreatureSpellCooldowns.empty() && GetOwner())
        {
            ((Player*)GetOwner())->GetSession()->SendPacket(&data);
        }
    }
}

void Pet::_SaveSpellCooldowns()
{
    static SqlStatementID delSpellCD ;
    static SqlStatementID insSpellCD ;

    SqlStatement stmt = CharacterDatabase.CreateStatement(delSpellCD, "DELETE FROM pet_spell_cooldown WHERE guid = ?");
    stmt.PExecute(m_charmInfo->GetPetNumber());

    time_t curTime = time(NULL);

    // remove oudated and save active
    for(CreatureSpellCooldowns::iterator itr = m_CreatureSpellCooldowns.begin();itr != m_CreatureSpellCooldowns.end();)
    {
        if(itr->second <= curTime)
            m_CreatureSpellCooldowns.erase(itr++);
        else
        {
            stmt = CharacterDatabase.CreateStatement(insSpellCD, "INSERT INTO pet_spell_cooldown (guid,spell,time) VALUES (?, ?, ?)");
            stmt.PExecute(m_charmInfo->GetPetNumber(), itr->first, uint64(itr->second));
            ++itr;
        }
    }
}

void Pet::_LoadSpells()
{
    QueryResult *result = CharacterDatabase.PQuery("SELECT spell,active FROM pet_spell WHERE guid = '%u'",m_charmInfo->GetPetNumber());

    if(result)
    {
        do
        {
            Field *fields = result->Fetch();

            addSpell(fields[0].GetUInt32(), ActiveStates(fields[1].GetUInt8()), PETSPELL_UNCHANGED);
        }
        while( result->NextRow() );

        delete result;
    }
}

void Pet::_SaveSpells()
{
    static SqlStatementID delSpell ;
    static SqlStatementID insSpell ;

    for (PetSpellMap::iterator itr = m_spells.begin(), next = m_spells.begin(); itr != m_spells.end(); itr = next)
    {
        ++next;

        // prevent saving family passives to DB
        if (itr->second.type == PETSPELL_FAMILY)
            continue;

        switch(itr->second.state)
        {
            case PETSPELL_REMOVED:
                {
                    SqlStatement stmt = CharacterDatabase.CreateStatement(delSpell, "DELETE FROM pet_spell WHERE guid = ? and spell = ?");
                    stmt.PExecute(m_charmInfo->GetPetNumber(), itr->first);
                    m_spells.erase(itr);
                }
                continue;
            case PETSPELL_CHANGED:
                {
                    SqlStatement stmt = CharacterDatabase.CreateStatement(delSpell, "DELETE FROM pet_spell WHERE guid = ? and spell = ?");
                    stmt.PExecute(m_charmInfo->GetPetNumber(), itr->first);

                    stmt = CharacterDatabase.CreateStatement(insSpell, "INSERT INTO pet_spell (guid,spell,active) VALUES (?, ?, ?)");
                    stmt.PExecute(m_charmInfo->GetPetNumber(), itr->first, uint32(itr->second.active));
                }
                break;
            case PETSPELL_NEW:
                {
                    SqlStatement stmt = CharacterDatabase.CreateStatement(insSpell, "INSERT INTO pet_spell (guid,spell,active) VALUES (?, ?, ?)");
                    stmt.PExecute(m_charmInfo->GetPetNumber(), itr->first, uint32(itr->second.active));
                }
                break;
            case PETSPELL_UNCHANGED:
                continue;
        }

        itr->second.state = PETSPELL_UNCHANGED;
    }
}

void Pet::_LoadAuras(uint32 timediff)
{
    RemoveAllAuras();

    QueryResult *result = CharacterDatabase.PQuery("SELECT caster_guid,item_guid,spell,stackcount,remaincharges,basepoints0,basepoints1,basepoints2,periodictime0,periodictime1,periodictime2,maxduration,remaintime,effIndexMask FROM pet_aura WHERE guid = '%u'", m_charmInfo->GetPetNumber());

    if(result)
    {
        do
        {
            Field *fields = result->Fetch();
            ObjectGuid casterGuid = ObjectGuid(fields[0].GetUInt64());
            uint32 item_lowguid = fields[1].GetUInt32();
            uint32 spellid = fields[2].GetUInt32();
            uint32 stackcount = fields[3].GetUInt32();
            uint32 remaincharges = fields[4].GetUInt32();
            int32  damage[MAX_EFFECT_INDEX];
            uint32 periodicTime[MAX_EFFECT_INDEX];

            for (int32 i = 0; i < MAX_EFFECT_INDEX; ++i)
            {
                damage[i] = fields[i+5].GetInt32();
                periodicTime[i] = fields[i+8].GetUInt32();
            }

            int32 maxduration = fields[11].GetInt32();
            int32 remaintime = fields[12].GetInt32();
            uint32 effIndexMask = fields[13].GetUInt32();

            SpellEntry const* spellproto = sSpellStore.LookupEntry(spellid);
            if (!spellproto)
            {
                ERROR_LOG("Unknown spell (spellid %u), ignore.",spellid);
                continue;
            }

            // do not load single target auras (unless they were cast by the player)
            if (casterGuid != GetObjectGuid() && IsSingleTargetSpell(spellproto))
                continue;

            if (remaintime != -1 && !IsPositiveSpell(spellproto))
            {
                if (remaintime/IN_MILLISECONDS <= int32(timediff))
                    continue;

                remaintime -= timediff*IN_MILLISECONDS;
            }

            // prevent wrong values of remaincharges
            uint32 procCharges = spellproto->GetProcCharges();
            if (procCharges)
            {
                if (remaincharges <= 0 || remaincharges > procCharges)
                    remaincharges = procCharges;
            }
            else
                remaincharges = 0;

            uint32 defstackamount = spellproto->GetStackAmount();
            if (!defstackamount)
                stackcount = 1;
            else if (defstackamount < stackcount)
                stackcount = defstackamount;
            else if (!stackcount)
                stackcount = 1;

            SpellAuraHolder *holder = CreateSpellAuraHolder(spellproto, this, NULL);
            holder->SetLoadedState(casterGuid, ObjectGuid(HIGHGUID_ITEM, item_lowguid), stackcount, remaincharges, maxduration, remaintime);

            for (int32 i = 0; i < MAX_EFFECT_INDEX; ++i)
            {
                if ((effIndexMask & (1 << i)) == 0)
                    continue;

                Aura* aura = CreateAura(spellproto, SpellEffectIndex(i), NULL, holder, this);
                if (!damage[i])
                    damage[i] = aura->GetModifier()->m_amount;

                aura->SetLoadedState(damage[i], periodicTime[i]);
                holder->AddAura(aura, SpellEffectIndex(i));
            }

            if (!holder->IsEmptyHolder())
                AddSpellAuraHolder(holder);
            else
                delete holder;
        }
        while( result->NextRow() );

        delete result;
    }
}

void Pet::_SaveAuras()
{
    static SqlStatementID delAuras ;
    static SqlStatementID insAuras ;

    SqlStatement stmt = CharacterDatabase.CreateStatement(delAuras, "DELETE FROM pet_aura WHERE guid = ?");
    stmt.PExecute(m_charmInfo->GetPetNumber());

    SpellAuraHolderMap const& auraHolders = GetSpellAuraHolderMap();

    if (auraHolders.empty())
        return;

    stmt = CharacterDatabase.CreateStatement(insAuras, "INSERT INTO pet_aura (guid, caster_guid, item_guid, spell, stackcount, remaincharges, "
        "basepoints0, basepoints1, basepoints2, periodictime0, periodictime1, periodictime2, maxduration, remaintime, effIndexMask) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");

    for(SpellAuraHolderMap::const_iterator itr = auraHolders.begin(); itr != auraHolders.end(); ++itr)
    {
        SpellAuraHolder *holder = itr->second;

        bool save = true;
        for (int32 j = 0; j < MAX_EFFECT_INDEX; ++j)
        {
            SpellEntry const* spellInfo = holder->GetSpellProto();
            SpellEffectEntry const* effectEntry = spellInfo->GetSpellEffect(SpellEffectIndex(j));
            if(!effectEntry)
                continue;

            if (effectEntry->EffectApplyAuraName == SPELL_AURA_MOD_STEALTH ||
                effectEntry->Effect == SPELL_EFFECT_APPLY_AREA_AURA_OWNER ||
                effectEntry->Effect == SPELL_EFFECT_APPLY_AREA_AURA_PET )
            {
                save = false;
                break;
            }
        }

        //skip all holders from spells that are passive or channeled
        //do not save single target holders (unless they were cast by the player)
        if (save && !holder->IsPassive() && !IsChanneledSpell(holder->GetSpellProto()) && (holder->GetCasterGuid() == GetObjectGuid() || !holder->IsSingleTarget()))
        {
            int32  damage[MAX_EFFECT_INDEX];
            uint32 periodicTime[MAX_EFFECT_INDEX];
            uint32 effIndexMask = 0;

            for (uint32 i = 0; i < MAX_EFFECT_INDEX; ++i)
            {
                damage[i] = 0;
                periodicTime[i] = 0;

                if (Aura *aur = holder->GetAuraByEffectIndex(SpellEffectIndex(i)))
                {
                    // don't save not own area auras
                    if (aur->IsAreaAura() && holder->GetCasterGuid() != GetObjectGuid())
                        continue;

                    damage[i] = aur->GetModifier()->m_amount;
                    periodicTime[i] = aur->GetModifier()->periodictime;
                    effIndexMask |= (1 << i);
                }
            }

            if (!effIndexMask)
                continue;

            stmt.addUInt32(m_charmInfo->GetPetNumber());
            stmt.addUInt64(holder->GetCasterGuid().GetRawValue());
            stmt.addUInt32(holder->GetCastItemGuid().GetCounter());
            stmt.addUInt32(holder->GetId());
            stmt.addUInt32(holder->GetStackAmount());
            stmt.addUInt8(holder->GetAuraCharges());

            for (uint32 i = 0; i < MAX_EFFECT_INDEX; ++i)
                stmt.addInt32(damage[i]);

            for (uint32 i = 0; i < MAX_EFFECT_INDEX; ++i)
                stmt.addUInt32(periodicTime[i]);

            stmt.addInt32(holder->GetAuraMaxDuration());
            stmt.addInt32(holder->GetAuraDuration());
            stmt.addUInt32(effIndexMask);
            stmt.Execute();
        }
    }
}

bool Pet::addSpell(uint32 spell_id,ActiveStates active /*= ACT_DECIDE*/, PetSpellState state /*= PETSPELL_NEW*/, PetSpellType type /*= PETSPELL_NORMAL*/)
{
    SpellEntry const *spellInfo = sSpellStore.LookupEntry(spell_id);
    if (!spellInfo)
    {
        // do pet spell book cleanup
        if(state == PETSPELL_UNCHANGED)                     // spell load case
        {
            ERROR_LOG("Pet::addSpell: nonexistent in SpellStore spell #%u request, deleting for all pets in `pet_spell`.",spell_id);
            CharacterDatabase.PExecute("DELETE FROM pet_spell WHERE spell = '%u'",spell_id);
        }
        else
            ERROR_LOG("Pet::addSpell: nonexistent in SpellStore spell #%u request.",spell_id);

        return false;
    }

    PetSpellMap::iterator itr = m_spells.find(spell_id);
    if (itr != m_spells.end())
    {
        if (itr->second.state == PETSPELL_REMOVED)
        {
            m_spells.erase(itr);
            state = PETSPELL_CHANGED;
        }
        else if (state == PETSPELL_UNCHANGED && itr->second.state != PETSPELL_UNCHANGED)
        {
            // can be in case spell loading but learned at some previous spell loading
            itr->second.state = PETSPELL_UNCHANGED;

            if(active == ACT_ENABLED)
                ToggleAutocast(spell_id, true);
            else if(active == ACT_DISABLED)
                ToggleAutocast(spell_id, false);

            return false;
        }
        else
            return false;
    }

    uint32 oldspell_id = 0;

    PetSpell newspell;
    newspell.state = state;
    newspell.type = type;

    if(active == ACT_DECIDE)                                //active was not used before, so we save it's autocast/passive state here
    {
        if(IsPassiveSpell(spellInfo))
            newspell.active = ACT_PASSIVE;
        else
            newspell.active = ACT_DISABLED;
    }
    else
        newspell.active = active;

    // talent: unlearn all other talent ranks (high and low)
    if(TalentSpellPos const* talentPos = GetTalentSpellPos(spell_id))
    {
        if(TalentEntry const *talentInfo = sTalentStore.LookupEntry( talentPos->talent_id ))
        {
            for(int i=0; i < MAX_TALENT_RANK; ++i)
            {
                // skip learning spell and no rank spell case
                uint32 rankSpellId = talentInfo->RankID[i];
                if(!rankSpellId || rankSpellId==spell_id)
                    continue;

                // skip unknown ranks
                if(!HasSpell(rankSpellId))
                    continue;
                removeSpell(rankSpellId,false,false);
            }
        }
    }
    else if(sSpellMgr.GetSpellRank(spell_id)!=0)
    {
        for (PetSpellMap::const_iterator itr2 = m_spells.begin(); itr2 != m_spells.end(); ++itr2)
        {
            if(itr2->second.state == PETSPELL_REMOVED) continue;

            if( sSpellMgr.IsRankSpellDueToSpell(spellInfo,itr2->first) )
            {
                // replace by new high rank
                if(sSpellMgr.IsHighRankOfSpell(spell_id,itr2->first))
                {
                    newspell.active = itr2->second.active;

                    if(newspell.active == ACT_ENABLED)
                        ToggleAutocast(itr2->first, false);

                    oldspell_id = itr2->first;
                    unlearnSpell(itr2->first,false,false);
                    break;
                }
                // ignore new lesser rank
                else if(sSpellMgr.IsHighRankOfSpell(itr2->first,spell_id))
                    return false;
            }
        }
    }

    m_spells[spell_id] = newspell;

    if (IsPassiveSpell(spellInfo))
        CastSpell(this, spell_id, true);
    else
        m_charmInfo->AddSpellToActionBar(spell_id, ActiveStates(newspell.active));

    if(newspell.active == ACT_ENABLED)
        ToggleAutocast(spell_id, true);

    uint32 talentCost = GetTalentSpellCost(spell_id);
    if (talentCost)
    {
        m_usedTalentCount+=talentCost;
        UpdateFreeTalentPoints(false);
    }
    return true;
}

bool Pet::learnSpell(uint32 spell_id)
{
    // prevent duplicated entires in spell book
    if (!addSpell(spell_id))
        return false;

    if(!m_loading)
    {
        Unit* owner = GetOwner();
        // send only non-temporary petspells
        if(owner && owner->GetTypeId() == TYPEID_PLAYER && m_duration == 0)
        {
            WorldPacket data(SMSG_PET_LEARNED_SPELL, 4);
            data << uint32(spell_id);
            ((Player*)owner)->GetSession()->SendPacket(&data);

            ((Player*)owner)->PetSpellInitialize();
        }
    }
    return true;
}

void Pet::InitLevelupSpellsForLevel()
{
    uint32 level = getLevel();

    if(PetLevelupSpellSet const *levelupSpells = GetCreatureInfo()->family ? sSpellMgr.GetPetLevelupSpellList(GetCreatureInfo()->family) : NULL)
    {
        // PetLevelupSpellSet ordered by levels, process in reversed order
        for(PetLevelupSpellSet::const_reverse_iterator itr = levelupSpells->rbegin(); itr != levelupSpells->rend(); ++itr)
        {
            // will called first if level down
            if(itr->first > level)
                unlearnSpell(itr->second,true);                 // will learn prev rank if any
            // will called if level up
            else
                learnSpell(itr->second);                        // will unlearn prev rank if any
        }
    }

    int32 petSpellsId = GetCreatureInfo()->PetSpellDataId ? -(int32)GetCreatureInfo()->PetSpellDataId : GetEntry();

    // default spells (can be not learned if pet level (as owner level decrease result for example) less first possible in normal game)
    if(PetDefaultSpellsEntry const *defSpells = sSpellMgr.GetPetDefaultSpellsEntry(petSpellsId))
    {
        for(int i = 0; i < MAX_CREATURE_SPELL_DATA_SLOT; ++i)
        {
            SpellEntry const* spellEntry = sSpellStore.LookupEntry(defSpells->spellid[i]);
            if(!spellEntry)
                continue;

            // will called first if level down
            if(spellEntry->GetSpellLevel() > level)
                unlearnSpell(spellEntry->Id,true);
            // will called if level up
            else
                learnSpell(spellEntry->Id);
        }
    }
}

bool Pet::unlearnSpell(uint32 spell_id, bool learn_prev, bool clear_ab)
{
    if(removeSpell(spell_id,learn_prev,clear_ab))
    {
        if(!m_loading)
        {
            if (Unit* owner = GetOwner())
            {
                if(owner->GetTypeId() == TYPEID_PLAYER)
                {
                    WorldPacket data(SMSG_PET_REMOVED_SPELL, 4);
                    data << uint32(spell_id);
                    ((Player*)owner)->GetSession()->SendPacket(&data);
                }
            }
        }
        return true;
    }
    return false;
}

bool Pet::removeSpell(uint32 spell_id, bool learn_prev, bool clear_ab)
{
    PetSpellMap::iterator itr = m_spells.find(spell_id);
    if (itr == m_spells.end())
        return false;

    if(itr->second.state == PETSPELL_REMOVED)
        return false;

    if(itr->second.state == PETSPELL_NEW)
        m_spells.erase(itr);
    else
        itr->second.state = PETSPELL_REMOVED;

    RemoveAurasDueToSpell(spell_id);

    uint32 talentCost = GetTalentSpellCost(spell_id);
    if (talentCost > 0)
    {
        if (m_usedTalentCount > talentCost)
            m_usedTalentCount-=talentCost;
        else
            m_usedTalentCount = 0;

        UpdateFreeTalentPoints(false);
    }

    if (learn_prev)
    {
        if (uint32 prev_id = sSpellMgr.GetPrevSpellInChain (spell_id))
            learnSpell(prev_id);
        else
            learn_prev = false;
    }

    // if remove last rank or non-ranked then update action bar at server and client if need
    if (clear_ab && !learn_prev && m_charmInfo->RemoveSpellFromActionBar(spell_id))
    {
        if(!m_loading)
        {
            // need update action bar for last removed rank
            if (Unit* owner = GetOwner())
                if (owner->GetTypeId() == TYPEID_PLAYER)
                    ((Player*)owner)->PetSpellInitialize();
        }
    }

    return true;
}

void Pet::CleanupActionBar()
{
    for(int i = 0; i < MAX_UNIT_ACTION_BAR_INDEX; ++i)
        if(UnitActionBarEntry const* ab = m_charmInfo->GetActionBarEntry(i))
            if(uint32 action = ab->GetAction())
                if(ab->IsActionBarForSpell() && !HasSpell(action))
                    m_charmInfo->SetActionBar(i,0,ACT_DISABLED);
}

void Pet::InitPetCreateSpells()
{
    m_charmInfo->InitPetActionBar();
    m_spells.clear();

    LearnPetPassives();

    CastPetAuras(false);
}

bool Pet::resetTalents(bool no_cost)
{
    Unit *owner = GetOwner();
    if (!owner || owner->GetTypeId()!=TYPEID_PLAYER)
        return false;

    // not need after this call
    if(((Player*)owner)->HasAtLoginFlag(AT_LOGIN_RESET_PET_TALENTS))
        ((Player*)owner)->RemoveAtLoginFlag(AT_LOGIN_RESET_PET_TALENTS,true);

    CreatureInfo const * ci = GetCreatureInfo();
    if(!ci)
        return false;
    // Check pet talent type
    CreatureFamilyEntry const *pet_family = sCreatureFamilyStore.LookupEntry(ci->family);
    if(!pet_family || pet_family->petTalentType < 0)
        return false;

    Player *player = (Player *)owner;

    if (m_usedTalentCount == 0)
    {
        UpdateFreeTalentPoints(false);                      // for fix if need counter
        return false;
    }

    uint32 cost = 0;

    if(!no_cost)
    {
        cost = resetTalentsCost();

        if (player->GetMoney() < cost)
        {
            player->SendBuyError( BUY_ERR_NOT_ENOUGHT_MONEY, 0, 0, 0);
            return false;
        }
    }

    for (unsigned int i = 0; i < sTalentStore.GetNumRows(); ++i)
    {
        TalentEntry const *talentInfo = sTalentStore.LookupEntry(i);

        if (!talentInfo) continue;

        TalentTabEntry const *talentTabInfo = sTalentTabStore.LookupEntry( talentInfo->TalentTab );

        if(!talentTabInfo)
            continue;

        // unlearn only talents for pets family talent type
        if(!((1 << pet_family->petTalentType) & talentTabInfo->petTalentMask))
            continue;

        for (int j = 0; j < MAX_TALENT_RANK; ++j)
            if (talentInfo->RankID[j])
            {
                removeSpell(talentInfo->RankID[j],!IsPassiveSpell(talentInfo->RankID[j]), false);

                SpellEntry const *spellInfo = sSpellStore.LookupEntry(talentInfo->RankID[j]);
                for (int k = 0; k < MAX_EFFECT_INDEX; ++k)
                    if (SpellEffectEntry const * effect = spellInfo->GetSpellEffect(SpellEffectIndex(k)))
                        if (effect->EffectTriggerSpell)
                            removeSpell(effect->EffectTriggerSpell, false);
            }
    }

    UpdateFreeTalentPoints(false);

    if(!no_cost)
    {
        player->ModifyMoney(-(int64)cost);

        m_resetTalentsCost = cost;
        m_resetTalentsTime = time(NULL);
    }
    player->PetSpellInitialize();
    return true;
}

void Pet::resetTalentsForAllPetsOf(Player* owner, Pet* online_pet /*= NULL*/)
{
    // not need after this call
    if(((Player*)owner)->HasAtLoginFlag(AT_LOGIN_RESET_PET_TALENTS))
        ((Player*)owner)->RemoveAtLoginFlag(AT_LOGIN_RESET_PET_TALENTS,true);

    // reset for online
    if(online_pet)
        online_pet->resetTalents(true);

    // now need only reset for offline pets (all pets except online case)
    uint32 except_petnumber = online_pet ? online_pet->GetCharmInfo()->GetPetNumber() : 0;

    QueryResult *resultPets = CharacterDatabase.PQuery(
        "SELECT id FROM character_pet WHERE owner = '%u' AND id <> '%u'",
        owner->GetGUIDLow(),except_petnumber);

    // no offline pets
    if(!resultPets)
        return;

    QueryResult *result = CharacterDatabase.PQuery(
        "SELECT DISTINCT pet_spell.spell FROM pet_spell, character_pet "
        "WHERE character_pet.owner = '%u' AND character_pet.id = pet_spell.guid AND character_pet.id <> %u",
        owner->GetGUIDLow(),except_petnumber);

    if(!result)
    {
        delete resultPets;
        return;
    }

    bool need_comma = false;
    std::ostringstream ss;
    ss << "DELETE FROM pet_spell WHERE guid IN (";

    do
    {
        Field *fields = resultPets->Fetch();

        uint32 id = fields[0].GetUInt32();

        if(need_comma)
            ss << ",";

        ss << id;

        need_comma = true;
    }
    while( resultPets->NextRow() );

    delete resultPets;

    ss << ") AND spell IN (";

    bool need_execute = false;
    do
    {
        Field *fields = result->Fetch();

        uint32 spell = fields[0].GetUInt32();

        if(!GetTalentSpellCost(spell))
            continue;

        if(need_execute)
            ss << ",";

        ss << spell;

        need_execute = true;
    }
    while( result->NextRow() );

    delete result;

    if(!need_execute)
        return;

    ss << ")";

    CharacterDatabase.Execute(ss.str().c_str());
}

void Pet::UpdateFreeTalentPoints(bool resetIfNeed)
{
    uint32 level = getLevel();
    uint32 talentPointsForLevel = GetMaxTalentPointsForLevel(level);
    // Reset talents in case low level (on level down) or wrong points for level (hunter can unlearn TP increase talent)
    if (talentPointsForLevel == 0 || m_usedTalentCount > talentPointsForLevel)
    {
        // Remove all talent points (except for admin pets)
        if (resetIfNeed)
        {
            Unit *owner = GetOwner();
            if (!owner || owner->GetTypeId() != TYPEID_PLAYER || ((Player*)owner)->GetSession()->GetSecurity() < SEC_ADMINISTRATOR)
                resetTalents(true);
            else
                SetFreeTalentPoints(0);
        }
        else
            SetFreeTalentPoints(0);
    }
    else
        SetFreeTalentPoints(talentPointsForLevel - m_usedTalentCount);
}


void Pet::InitTalentForLevel()
{
    UpdateFreeTalentPoints();

    Unit *owner = GetOwner();
    if (!owner || owner->GetTypeId() != TYPEID_PLAYER)
        return;

    if(!m_loading)
        ((Player*)owner)->SendTalentsInfoData(true);
}

uint32 Pet::resetTalentsCost() const
{
    uint32 days = uint32(sWorld.GetGameTime() - m_resetTalentsTime)/DAY;

    // The first time reset costs 10 silver; after 1 day cost is reset to 10 silver
    if(m_resetTalentsCost < 10*SILVER || days > 0)
        return 10*SILVER;
    // then 50 silver
    else if(m_resetTalentsCost < 50*SILVER)
        return 50*SILVER;
    // then 1 gold
    else if(m_resetTalentsCost < 1*GOLD)
        return 1*GOLD;
    // then increasing at a rate of 1 gold; cap 10 gold
    else
        return (m_resetTalentsCost + 1*GOLD > 10*GOLD ? 10*GOLD : m_resetTalentsCost + 1*GOLD);
}

uint8 Pet::GetMaxTalentPointsForLevel(uint32 level)
{
    uint8 points = (level >= 20) ? ((level - 16) / 4) : 0;
    // Mod points from owner SPELL_AURA_MOD_PET_TALENT_POINTS
    if (Unit *owner = GetOwner())
        points+=owner->GetTotalAuraModifier(SPELL_AURA_MOD_PET_TALENT_POINTS);
    return points;
}

void Pet::ToggleAutocast(uint32 spellid, bool apply)
{
    if(IsPassiveSpell(spellid))
        return;

    PetSpellMap::iterator itr = m_spells.find(spellid);

    uint32 i;

    if(apply)
    {
        for (i = 0; i < m_autospells.size() && m_autospells[i] != spellid; ++i)
            ;                                               // just search

        if (i == m_autospells.size())
        {
            m_autospells.push_back(spellid);

            if(itr->second.active != ACT_ENABLED)
            {
                itr->second.active = ACT_ENABLED;
                if(itr->second.state != PETSPELL_NEW)
                    itr->second.state = PETSPELL_CHANGED;
            }
        }
    }
    else
    {
        AutoSpellList::iterator itr2 = m_autospells.begin();
        for (i = 0; i < m_autospells.size() && m_autospells[i] != spellid; ++i, ++itr2)
            ;                                               // just search

        if (i < m_autospells.size())
        {
            m_autospells.erase(itr2);
            if(itr->second.active != ACT_DISABLED)
            {
                itr->second.active = ACT_DISABLED;
                if(itr->second.state != PETSPELL_NEW)
                    itr->second.state = PETSPELL_CHANGED;
            }
        }
    }
}

bool Pet::IsPermanentPetFor(Player* owner)
{
    switch(getPetType())
    {
        case SUMMON_PET:
            switch(owner->getClass())
            {
                // oddly enough, Mage's Water Elemental is still treated as temporary pet with Glyph of Eternal Water
                // i.e. does not unsummon at mounting, gets dismissed at teleport etc.
                case CLASS_WARLOCK:
                    return GetCreatureInfo()->type == CREATURE_TYPE_DEMON;
                case CLASS_DEATH_KNIGHT:
                    return GetCreatureInfo()->type == CREATURE_TYPE_UNDEAD;
                case CLASS_MAGE:
                    return GetCreatureInfo()->Entry == 37994;
                default:
                    return false;
            }
        case HUNTER_PET:
            return true;
        default:
            return false;
    }
}

bool Pet::Create(uint32 guidlow, CreatureCreatePos& cPos, CreatureInfo const* cinfo, uint32 pet_number)
{
    SetMap(cPos.GetMap());
    SetPhaseMask(cPos.GetPhaseMask(), false);

    Object::_Create(guidlow, pet_number, HIGHGUID_PET);

    m_originalEntry = cinfo->Entry;

    if (!InitEntry(cinfo->Entry))
        return false;

    cPos.SelectFinalPoint(this);

    if (!cPos.Relocate(this))
        return false;

    SetSheath(SHEATH_STATE_MELEE);

    if (getPetType() == MINI_PET)                           // always non-attackable
        SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NON_ATTACKABLE);

    if (getPetType() == GUARDIAN_PET || getPetType() == MINI_PET || getPetType() == PROTECTOR_PET)
        LoadCreatureAddon(false);

    return true;
}

bool Pet::HasSpell(uint32 spell) const
{
    PetSpellMap::const_iterator itr = m_spells.find(spell);
    return (itr != m_spells.end() && itr->second.state != PETSPELL_REMOVED );
}

// Get all passive spells in our skill line
void Pet::LearnPetPassives()
{
    CreatureInfo const* cInfo = GetCreatureInfo();
    if(!cInfo)
        return;

    CreatureFamilyEntry const* cFamily = sCreatureFamilyStore.LookupEntry(cInfo->family);
    if(!cFamily)
        return;

    PetFamilySpellsStore::const_iterator petStore = sPetFamilySpellsStore.find(cFamily->ID);
    if(petStore != sPetFamilySpellsStore.end())
    {
        for(PetFamilySpellsSet::const_iterator petSet = petStore->second.begin(); petSet != petStore->second.end(); ++petSet)
            addSpell(*petSet, ACT_DECIDE, PETSPELL_NEW, PETSPELL_FAMILY);
    }
}

void Pet::CastPetAuras(bool current)
{
    Unit* owner = GetOwner();
    if(!owner || owner->GetTypeId()!=TYPEID_PLAYER)
        return;

    if (getPetType() != HUNTER_PET && getPetType() != SUMMON_PET)
        return;

    for(PetAuraSet::const_iterator itr = owner->m_petAuras.begin(); itr != owner->m_petAuras.end();)
    {
        PetAura const* pa = *itr;
        ++itr;

        if(!current && pa->IsRemovedOnChangePet())
            owner->RemovePetAura(pa);
        else
            CastPetAura(pa);
    }
}

void Pet::CastPetAura(PetAura const* aura)
{
    uint32 auraId = aura->GetAura(GetEntry());
    if(!auraId)
        return;

    if (getPetType() != HUNTER_PET && getPetType() != SUMMON_PET)
        return;

    switch(auraId)
    {
        case 35696:         // Demonic Knowledge
        {
            int32 basePoints = int32(aura->GetDamage() * (GetStat(STAT_STAMINA) + GetStat(STAT_INTELLECT)) / 100);
            CastCustomSpell(this, auraId, &basePoints, NULL, NULL, true);
            break;
        }
        case 68361:         // Animal Handler
        {
            Unit* owner = GetOwner();
            if (!owner)
                return;

            int32 basePoints = 0;
            if (Aura* aura = owner->GetDummyAura(34453))        // Animal Handler (Rank 1)
                basePoints = aura->GetModifier()->m_amount;
            else if (Aura* aura = owner->GetDummyAura(34454))   // Animal Handler (Rank 2)
                basePoints = aura->GetModifier()->m_amount;
            else
                return;

            CastCustomSpell(this, auraId, &basePoints, NULL, NULL, true);
            break;
        }
        default:
            CastSpell(this, auraId, true);
            break;
    }
}

void Pet::UpdateScalingAuras()
{
    // there exists temp. summoned pets with scaling auras
    // they should not scale dynamically
    if (isTemporarySummoned())
        return;

    for (AuraList::const_iterator itr = m_scalingauras.begin(); itr != m_scalingauras.end(); ++itr)
    {
        SpellEntry const* spellInfo = (*itr)->GetSpellProto();
        // check if we need to update aura
        int32 amount = CalculateSpellDamage(this, spellInfo, (*itr)->GetEffIndex());
        if ((*itr)->GetModifier()->m_amount == amount)
            continue;

        // update aura amount
        (*itr)->UpdateModifierAmount(amount);
    }
}

void Pet::CalcScalingAuraBonus(int32* value, SpellEntry const* spellInfo, SpellEffectIndex effect_index)
{
    Player* owner = GetCharmerOrOwnerPlayerOrPlayerItself();
    SpellEffectEntry const * effect = spellInfo->GetSpellEffect(effect_index);
    if (!owner || !effect || effect->Effect != SPELL_EFFECT_APPLY_AURA)
        return;

    uint32 ownerValue = 0;
    uint32 bonusValue = 0;
    float scale = 0;

    switch (effect->EffectApplyAuraName)
    {
        case SPELL_AURA_MOD_DAMAGE_DONE:
        {
            switch (spellInfo->Id)
            {
                // hunter pet scaling aura
                case 34902:
                {
                    ownerValue = owner->GetTotalAttackPowerValue(RANGED_ATTACK);
                    scale = 0.1287f;

                    // search for "Wild Hunt"
                    for (PetSpellMap::const_iterator itr = m_spells.begin(); itr != m_spells.end(); ++itr)
                        if (itr->second.state != PETSPELL_REMOVED)
                        {
                            SpellEntry const* spellInfo = sSpellStore.LookupEntry(itr->first);
                            if (!spellInfo || spellInfo->SpellIconID == 3748)
                                continue;
                            scale *= float(spellInfo->CalculateSimpleValue(EFFECT_INDEX_1) + 100) / 100.0f;
                            break;
                         }
                    break;
                }
                // warlock pet scaling aura
                case 34947:
                {
                    ownerValue = owner->GetMaxSpellBaseDamageBonusDone(SpellSchoolMask(SPELL_SCHOOL_MASK_FIRE | SPELL_SCHOOL_MASK_SHADOW));
                    scale = 0.15f;
                    break;
                }
                // dk pet scaling aura, unknown for now
                case 54566:
                    break;
            }
            break;
        }
        case SPELL_AURA_MOD_STAT:
        {
            // only single stats in scaling auras (otherwise scaling not possible)
            if (effect->EffectMiscValue < 0 || effect->EffectMiscValue > 4)
                return;

            ownerValue = uint32(owner->GetTotalStatValue(Stats(effect->EffectMiscValue)));

            switch(effect->EffectMiscValue)
            {
                case STAT_STRENGTH:
                {
                    // just dk scaling aura here
                    scale = 0.678f;

                    // "Glyph of Ghoul"
                    if (Aura* glyph = owner->GetDummyAura(58686))
                        scale *= float(glyph->GetModifier()->m_amount + 100) / 100.0f;
                    break;
                }
                case STAT_STAMINA:
                {
                    scale = 0.3f;

                    switch (spellInfo->Id)
                    {
                        // hunter pet scaling aura
                        case 34902:
                        {
                            scale = 0.4493f;

                            // search for "Wild Hunt"
                            for (PetSpellMap::const_iterator itr = m_spells.begin(); itr != m_spells.end(); ++itr)
                                if (itr->second.state != PETSPELL_REMOVED)
                                {
                                    SpellEntry const* spellInfo = sSpellStore.LookupEntry(itr->first);
                                    if (!spellInfo || spellInfo->SpellIconID != 3748)
                                        continue;
                                    scale *= float(spellInfo->CalculateSimpleValue(EFFECT_INDEX_0) + 100) / 100.0f;
                                    break;
                                 }

                            float baseHP = GetModifierValue(UNIT_MOD_HEALTH, BASE_VALUE) + GetCreateHealth();
                            baseHP *= GetModifierValue(UNIT_MOD_HEALTH, BASE_PCT);
                            // Endurance Training
                            Unit::AuraList const& flatmodauras = owner->GetAurasByType(SPELL_AURA_ADD_FLAT_MODIFIER);
                            for (Unit::AuraList::const_iterator i = flatmodauras.begin(); i != flatmodauras.end(); ++i)
                            {
                                SpellClassOptionsEntry const * opt = (*i)->GetSpellProto()->GetSpellClassOptions();
                                if (opt && opt->SpellFamilyName == SPELLFAMILY_HUNTER && (*i)->GetSpellProto()->SpellIconID == 24 && (*i)->GetEffIndex() == EFFECT_INDEX_0)
                                {
                                    bonusValue += float(baseHP * (*i)->GetModifier()->m_amount + 100) / 100.0f;
                                    scale *= ((*i)->GetModifier()->m_amount + 100) / 100.0f;
                                    break;
                                }
                            }
                            break;
                        }
                        // wl scaling aura
                        case 34947:
                        {
                            scale = 0.75f;
                            break;
                        }
                        // dk pet scaling aura
                        case 54566:
                        {
                            scale = 0.3928f;

                            // skip gargoyle
                            if (GetEntry() == 27829)
                                break;

                            // "Glyph of Ghoul"
                            if (Aura* glyph = owner->GetDummyAura(58686))
                                scale *= float(glyph->GetModifier()->m_amount + 100) / 100.0f;
                            break;
                        }
                    }
                    break;
                }
                case STAT_INTELLECT:
                {
                    // just warlock pet aura here
                    scale = 0.3f;
                    break;
                }
            }
            break;
        }
        case SPELL_AURA_MOD_ATTACK_POWER:
        {
            switch (spellInfo->Id)
            {
                // hunter pet scaling aura
                case 34902:
                {
                    ownerValue = uint32(owner->GetTotalAttackPowerValue(RANGED_ATTACK));
                    scale = 0.22f;

                    // search for "Wild Hunt"
                    for (PetSpellMap::const_iterator itr = m_spells.begin(); itr != m_spells.end(); ++itr)
                        if (itr->second.state != PETSPELL_REMOVED)
                        {
                            SpellEntry const* spellInfo = sSpellStore.LookupEntry(itr->first);
                            if (!spellInfo || spellInfo->SpellIconID != 3748)
                                continue;
                            scale *= float(spellInfo->CalculateSimpleValue(EFFECT_INDEX_1) + 101) / 100.0f;
                            break;
                         }
                    break;
                }
                // warlock pet scaling aura
                case 34947:
                {
                    ownerValue = owner->GetMaxSpellBaseDamageBonusDone(SpellSchoolMask(SPELL_SCHOOL_MASK_FIRE | SPELL_SCHOOL_MASK_SHADOW));
                    scale = 0.57f;
                    break;
                }
            }
            break;
        }
        case SPELL_AURA_MOD_RESISTANCE:
        {
            // only single schools in scaling auras (otherwise scaling not possible)
            SpellSchools school = GetFirstSchoolInMask(SpellSchoolMask(effect->EffectMiscValue));
            ownerValue = owner->GetResistance(school);

            switch(school)
            {
                // armor
                case SPELL_SCHOOL_NORMAL:
                {
                    scale = 0.35f;

                    // hunter pet scaling aura
                    if (spellInfo->Id == 34904)
                        scale = 0.4497f;
                    break;
                }
                // all other resistances
                default:
                {
                    scale = 0.4f;
                    break;
                }
            }
            break;
        }
        case SPELL_AURA_MOD_DAMAGE_PERCENT_DONE:
        case SPELL_AURA_MOD_POWER_REGEN:
            // no information here for now
            break;
        case SPELL_AURA_MOD_SPELL_HIT_CHANCE:
        case SPELL_AURA_MOD_HIT_CHANCE:
        {
            // find maximum of owners hit chances (fractions dropped)
            float hit[3] = {owner->m_modMeleeHitChance, owner->m_modRangedHitChance, owner->m_modSpellHitChance};
            for (uint8 i = 0; i<3; i++)
                if (ownerValue < uint32(hit[i]))
                    ownerValue = uint32(hit[i]);
            // all known auras scale 1:1
            scale = 1.0f;
            break;
        }
        case SPELL_AURA_MOD_MELEE_RANGED_HASTE:
        {
            // find owners maximum haste (== minimum speedPct factor)
            float cur = 1.0f, factor[3] = {owner->m_modAttackSpeedPct[BASE_ATTACK], m_modAttackSpeedPct[RANGED_ATTACK], owner->GetFloatValue(UNIT_MOD_CAST_SPEED)};
            for (uint8 i = 0; i<3; i++)
                if (cur > factor[i])
                    cur = factor[i];
            // get percent haste out of factor
            ownerValue = uint32(100.0f/cur -100);

            // just death knight ghoul aura here for now, 1:1
            scale = 1.0f;
        }
        case SPELL_AURA_MOD_EXPERTISE:
        {
            // expertise scales proportional to owners hitchance (fractions dropped)
            // (e.g. 5% hitchance owner -> 5% less chance, that the attack ist dodged/parried)
            // note: 1 point expertise gives 0.25% less chance to fail
            float cur = 0, hit[3] = {owner->m_modMeleeHitChance, owner->m_modRangedHitChance, owner->m_modSpellHitChance};
            for (uint8 i = 0; i<3; i++)
                if (cur < hit[i])
                    cur = hit[i];
            bonusValue = uint32(4*cur);
            break;
        }
        default:
            return;
    }

    *value += int32(ownerValue * scale + bonusValue);
}

struct DoPetLearnSpell
{
    DoPetLearnSpell(Pet& _pet) : pet(_pet) {}
    void operator() (uint32 spell_id) { pet.learnSpell(spell_id); }
    Pet& pet;
};

void Pet::learnSpellHighRank(uint32 spellid)
{
    learnSpell(spellid);

    DoPetLearnSpell worker(*this);
    sSpellMgr.doForHighRanks(spellid,worker);
}

void Pet::SynchronizeLevelWithOwner()
{
    Unit* owner = GetOwner();
    if (!owner || owner->GetTypeId() != TYPEID_PLAYER)
        return;

    switch(getPetType())
    {
        // always same level
        case SUMMON_PET:
            GivePetLevel(owner->getLevel());
            break;
        // can't be greater owner level
        case HUNTER_PET:
            if (getLevel() > owner->getLevel())
                GivePetLevel(owner->getLevel());
            else if (getLevel() + 5 < owner->getLevel())
                GivePetLevel(owner->getLevel() - 5);
            break;
        default:
            break;
    }
}

void Pet::RemoveSpellCooldowns()
{
    if (Player* owner = GetCharmerOrOwnerPlayerOrPlayerItself())
    {
        for (CreatureSpellCooldowns::const_iterator itr = m_CreatureSpellCooldowns.begin(); itr != m_CreatureSpellCooldowns.end(); ++itr)
            owner->SendClearCooldown(itr->first, this);
    }

    m_CreatureSpellCooldowns.clear();
}
