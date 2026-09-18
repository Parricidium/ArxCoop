/*
 * Copyright 2026 ArxCoop contributors
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

#ifndef ARX_COOP_REPLICATION_H
#define ARX_COOP_REPLICATION_H

#include <string>
#include <string_view>
#include <vector>

#include "coop/Protocol.h"
#include "game/EntityId.h"
#include "game/Item.h"
#include "script/Script.h"

class Entity;
namespace script { class Context; }

/*!
 * World state synchronization: the host is authoritative for everything that is not a
 * player's own character.
 *
 * - Clients only run scripts for their player and the entities it owns. Events their player
 *   sends to world entities are forwarded to the host, which runs them with an "acting player".
 * - On the host, script commands executed in the context of world entities are recorded with
 *   their parameters already resolved (see script::Context::setTranscript) and replayed on the
 *   clients. Commands that act on the player are instead redirected to the acting player.
 * - Global script variables, quests, keys, runes and experience are shared by everyone.
 */
namespace coop {

void replicationInit();

//! Per in-game frame: level state requests/answers between host and clients.
void replicationUpdate();

//! Called when our own character enters a (new or loaded) playthrough: the world can now be synced.
void playthroughStarted();

//! Host: nothing is replicated while a level is loading (clients get the full state afterwards).
void levelLoadBegin();
void levelLoadEnd();

//! Base for instance numbers of dynamically created entities, so ids never clash between machines.
EntityInstance instanceBase();

/*!
 * Script execution policy, called before an event is executed.
 * \return true if the event must not be executed locally (\a result is then set).
 */
bool interceptScriptEvent(Entity * sender, Entity * entity, const ScriptEventName & event,
                          const ScriptParameters & parameters, bool baseScript, ScriptResult & result);

enum class CommandSync {
	Local,     //!< Execute normally, nothing to send
	Replicate, //!< Execute, record the parameters and send them to the other players
	Redirect,  //!< Do not execute here: send the command to the acting player instead
	Mirror,    //!< Execute here (a client's item stand-in keeps up with the real item) and send it to the acting player
};

//! Decides what to do with a script command about to be executed.
CommandSync commandSync(std::string_view command, const script::Context & context);

/*!
 * While an event sent by the local player to a world entity is processed on the host, the
 * player-directed effects belong to the host, not to everyone.
 */
class ActorScope {
	bool m_active = false;
	unsigned m_previous = 0;
public:
	ActorScope(const Entity * sender, const Entity * entity);
	~ActorScope();
};

/*!
 * While an event caused by another player's puppet is processed on the host (walking into a
 * guarded zone...), the player-directed effects go to that player.
 */
class PuppetActorScope {
	bool m_active = false;
	unsigned m_previous = 0;
public:
	explicit PuppetActorScope(const Entity * io);
	~PuppetActorScope();
};

/*!
 * The player whose action is being processed right now (0xFF if none), so that a script that
 * resumes later (after a speech, from a timer) can act for the same player.
 */
unsigned currentActor();
class PlayerActorScope {
	bool m_active = false;
	unsigned m_previous = 0;
public:
	explicit PlayerActorScope(unsigned actor);
	~PlayerActorScope();
};

/*!
 * The stats of a player as the scripts read them (^player_skill_mecanism, ^player_attribute_strength,
 * ^player_life...). Each client sends its own to the host, which answers those variables for the
 * client whose action a script is processing: lockpicking, alchemy, "not skilled enough" checks
 * are made with the stats of the player who acts, not the host's.
 */
struct PlayerStats {
	float strength = 0.f, dexterity = 0.f, constitution = 0.f, mind = 0.f;
	float stealth = 0.f, mecanism = 0.f, intuition = 0.f, etheralLink = 0.f, objectKnowledge = 0.f;
	float casting = 0.f, projectile = 0.f, closeCombat = 0.f, defense = 0.f;
	float life = 0.f, maxLife = 0.f, mana = 0.f, maxMana = 0.f, hunger = 0.f, poison = 0.f;
	long gold = 0;
	long level = 0;
	bool operator==(const PlayerStats & o) const;
	bool operator!=(const PlayerStats & o) const { return !(*this == o); }
};

//! Host: the stats of the client whose action is being processed, or null (use the local player's).
const PlayerStats * actingPlayerStats();

//! Host: a persistent magic field cast by a world entity ended (someone stood on it...): the clients end theirs too.
void fieldSpellEnded(const Entity * caster);

/*!
 * What makes an item this particular item beyond its class: the script variables its scripts
 * wrote (a chest scroll's spell name / circle set by the chest's TRANSMUTE, "enchanted"...), its
 * instance script, enchantments (IO_EQUIPITEM), price, name, halo and wear. Appended to
 * GiveItem, DropItem and StoreItem so that a copy made on another machine from class + instance,
 * or the stale hidden copy of a world item that spent time in a player's inventory, gets them.
 */
struct ItemState {
	bool present = false;         //!< false: the message ended before it, or not an item
	EntityInstance instance = -1; //!< the original's instance (its instance script directory)
	bool hasInstanceScript = false;
	SCRIPT_VARIABLES variables;
	bool hasEquip = false;
	IO_EQUIPITEM equip;
	s32 price = 0;
	std::string locname;
	u32 haloFlags = 0;            //!< Entity::halo_native (the offset is never set by scripts, not carried)
	float haloColor[3] = { 0.f, 0.f, 0.f };
	float haloRadius = 0.f;
	float durability = 0.f;
	float maxDurability = 0.f;
	s16 poisonous = 0;
	s16 poisonousCount = 0;
};
ItemState captureItemState(const Entity & item);
void writeItemState(Writer & writer, const ItemState & state);
//! Reads the trailing item state; present = false when nothing is left in the message.
ItemState readItemState(Reader & reader);
//! Existing entity (our stale copy of it): enchantments, price, name, halo, wear, variables.
void applyItemState(const ItemState & state, Entity & item);
/*!
 * Fresh AddItem() copy of another player's item: its instance script (looked up by the
 * original's instance), INIT, the original's variables, INITEND (which derives the name, icon
 * and price from them), then applyItemState(). Runs as "applying remote" so that the host does
 * not replicate the INIT of an entity the clients do not have. The item may be destroyed by its
 * own INIT: check ValidIOAddress() afterwards.
 */
void initItemCopy(Entity & item, const ItemState & state);

//! Sends a recorded (executed) command to the players that need it.
void commandReplicated(std::string_view command, const std::vector<std::string> & words,
                       const script::Context & context);

//! Parses the parameters of a command without executing it and sends it to the acting player.
void commandRedirected(std::string_view command, script::Context & context);

//! Sends an executed command (see CommandSync::Mirror) to the acting player.
void commandMirrored(std::string_view command, const std::vector<std::string> & words, const script::Context & context);

//! Called by the script variable setters when a global variable changes.
void globalVariableChanged(std::string_view name, const SCRIPT_VAR & var);

// Called by the player functions when something shared changes (no-ops while applying remote changes)
void sharedQuestAdded(std::string_view quest);
void sharedKeyAdded(std::string_view key);
void sharedRuneAdded(unsigned rune);
void sharedExperience(long amount);
void sharedGold(long amount);

//! The local player used a backpack: the others get the extra inventory too.
void sharedBag();

//! True while applying something received from the network (prevents echoing it back).
bool applyingRemote();

//! Client: true once the host's level state has been loaded (the world is the host's).
bool levelSynced();

//! Client: true once our own character has entered the playthrough.
bool isPlaythroughStarted();

//! Host: a puppet was hit; the damage is sent to the player it represents.
float damagePuppet(const Entity & puppet, float damage, unsigned type);

//! Client: a local hit on an NPC is applied by the host.
void forwardNpcDamage(const Entity & npc, float damage, unsigned type, const Vec3f * pos);

//! Host: the game was saved under this name; clients save their character under the same name.
void gameSaved(std::string_view name);

//! Host: a save was loaded; clients load their matching character save (if any) and resync.
void gameLoaded(std::string_view name);

//! Client: true while a save/load requested by the host is being performed.
bool hostDrivenSaveLoad();

//! Client: saves our own character as "coop: <hostName>" (what a SaveRequest does).
void saveMyCharacter(std::string_view hostName);

/*!
 * Host: a positional sound effect was played by the engine (not by a replicated script
 * command, clients replay those themselves): let the clients hear it too.
 */
void soundPlayed(std::string_view sample, const Vec3f & pos, float pitch);

//! Host: sends a teammate next to us (slot spreads several players sideways).
bool teleportPlayerToMe(PlayerId id, size_t slot);
void collisionSoundPlayed(int mat1, int mat2, float volume, const Vec3f & pos);
//! While a replicated script command executes on the host (its sounds are not broadcast).
void replicatedCommandBegin();
void replicatedCommandEnd();
//! Client: plays a sound the host's world made.
void applySound(Reader & reader);

//! Client joining a running game: loads the newest "coop: ..." character save, if any.
bool loadSavedCoopCharacter();

//! Host: true when a cinematic speech being executed belongs to another player (no camera here).
bool cinematicSpeechIsSomeoneElses();

//! Called when the local player skips a speech / cutscene: the others skip it too.
void speechSkipped();

//! A world item went into the local player's hands: it leaves the shared world everywhere else.
void itemTaken(const Entity & item);
/*!
 * We handed this item to another player: kept hidden here (like a world item another player
 * took) rather than destroyed, so that its instance number is not given to a new item while the
 * receiver's copy still carries it - and found again under that id if the receiver drops it.
 */
void itemHandedOver(Entity & item);

//! Client: an entity of ours (inventory, equipment) that a level state from the host must not replace.
bool keptOverLevelState(const Entity & io);

//! Co-op console commands ("tp p2": teleport player 2 to me). Returns true if the line was one.
bool consoleCommand(std::string_view line);

//! The local player is carrying a world item around with the mouse: the others see it move.
void itemDragged(const Entity & item);

//! The local player put an item on the floor (or threw it): it (re)enters the shared world everywhere.
void itemDropped(const Entity & item, bool thrown = false, const Vec3f & direction = Vec3f(0.f));

/*!
 * The local player drops the item it carries into a world container (chest, merchant, corpse):
 * whatever changed in there when the scope ends (item stored, stack grown) reaches the others.
 */
class ContainerDropScope {
public:
	ContainerDropScope(const Entity & container, const Entity * item);
	~ContainerDropScope();
	ContainerDropScope(const ContainerDropScope &) = delete;
	ContainerDropScope & operator=(const ContainerDropScope &) = delete;
};

//! The local player changed the size of a stack in a world container (bought one, took one).
void itemCountChanged(const Entity & item);

//! The local player's character said a line: the others hear it from the puppet.
void playerSpoke(const std::string & sample);

//! Host: is this entity within \a limit of a teammate (so that it gets updated like the ones around us)?
bool nearTeammate(const Entity & entity, float limit);

//! Aim pitch of another player's puppet (its owner's look pitch), for projectile spells it casts.
bool puppetAimPitch(const Entity & caster, float & pitch);
//! Same, with the owner's yaw (their player.angle, what their own spells used; the puppet's entity yaw is the NPC one).
bool puppetAim(const Entity & caster, float & pitch, float & yaw);

/*!
 * A bone of a puppet in world space, or \a fallback above its feet when the skeleton is stale
 * (a puppet out of view is not animated, its vertices stay where they were - or at the origin).
 */
Vec3f puppetBone(const Entity & puppet, const char * group, const Vec3f & fallback);

//! Diagnostic: where a spell was placed / aimed (logged in co-op, compared between machines)
void spellPlaced(unsigned spell, const Entity * caster, const Vec3f & pos, float yaw);

} // namespace coop

#endif // ARX_COOP_REPLICATION_H
