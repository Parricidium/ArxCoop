/*
 * Copyright 2014-2022 Arx Libertatis Team (see the AUTHORS file)
 *
 * This file is part of Arx Libertatis.
 *
 * Arx Libertatis is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Arx Libertatis is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Arx Libertatis.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef ARX_GAME_NPC_DISMEMBERMENT_H
#define ARX_GAME_NPC_DISMEMBERMENT_H

#include "math/Types.h"
#include "game/NPC.h"
#include "graphics/BaseGraphicsTypes.h"

class Entity;
struct EERIE_3DOBJ;

MaterialId getGoreMaterial(const EERIE_3DOBJ & object);

void ARX_NPC_RestoreCuts();

/*!
 * \brief Attempt to cut something on NPC
 */
void ARX_NPC_TryToCutSomething(Entity * target, const Vec3f * pos);

//! Co-op client: mirrors the host's cut flags, spawning the members that fell off there.
void ARX_NPC_ApplyRemoteCuts(Entity & npc, DismembermentFlags cuts);

/*!
 * Co-op mod: a severed part is a corpse piece that stays (the engine blew it up in gore after
 * 300 ms): an entity named after its NPC and the cut ("goblin_base_cut_head_0006"), the same on
 * every machine, simulated by the host like any loose object and saved with the level's
 * ragdolls. Spawns (launches) the part of \a npc for \a flag, or returns the one already
 * there; null when the NPC has no such part.
 */
Entity * ARX_NPC_SpawnCutMember(Entity & npc, DismembermentFlag flag);

//! Is \a io such a corpse piece? Then its NPC's id and the cut are filled in.
bool ARX_NPC_IsCutMember(const Entity & io, std::string & npcId, DismembermentFlag & flag);

//! Per frame for a corpse piece (NPC.cpp): a piece asleep in the air is dropped again.
void ARX_NPC_UpdateCutMember(Entity & io);

#endif // ARX_GAME_NPC_DISMEMBERMENT_H
