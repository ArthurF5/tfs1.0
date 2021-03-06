/**
 * The Forgotten Server - a free and open-source MMORPG server emulator
 * Copyright (C) 2015  Mark Samman <mark.samman@gmail.com>
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
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "otpch.h"

#include <random>
#include <boost/range/adaptor/reversed.hpp>

#include "protocolgame.h"

#include "networkmessage.h"
#include "outputmessage.h"

#include "items.h"

#include "tile.h"
#include "player.h"
#include "chat.h"

#include "configmanager.h"
#include "actions.h"
#include "game.h"
#include "iologindata.h"
#include "house.h"
#include "waitlist.h"
#include "quests.h"
#include "ban.h"
#include "connection.h"
#include "creatureevent.h"
#include "scheduler.h"

extern Game g_game;
extern ConfigManager g_config;
extern Actions actions;
extern CreatureEvents* g_creatureEvents;
extern Chat* g_chat;

// Helping templates to add dispatcher tasks
template<class FunctionType>
void ProtocolGame::addGameTaskInternal(bool droppable, uint32_t delay, const FunctionType& func)
{
	if (droppable) {
		g_dispatcher.addTask(createTask(delay, func));
	} else {
		g_dispatcher.addTask(createTask(func));
	}
}

ProtocolGame::ProtocolGame(Connection_ptr connection) :
	Protocol(connection),
	player(nullptr),
	eventConnect(0),
	// version(CLIENT_VERSION_MIN),
	m_challengeTimestamp(0),
	m_challengeRandom(0),
	m_debugAssertSent(false),
	m_acceptPackets(false)
{
	//
}

void ProtocolGame::setPlayer(Player* p)
{
	player = p;
}

void ProtocolGame::releaseProtocol()
{
	//dispatcher thread
	if (player && player->client == this) {
		player->client = nullptr;
	}
	Protocol::releaseProtocol();
}

void ProtocolGame::deleteProtocolTask()
{
	//dispatcher thread
	if (player) {
		g_game.ReleaseCreature(player);
		player = nullptr;
	}

	Protocol::deleteProtocolTask();
}

void ProtocolGame::login(const std::string& name, uint32_t accountId, OperatingSystem_t operatingSystem)
{
	//dispatcher thread
	Player* _player = g_game.getPlayerByName(name);
	if (!_player || g_config.getBoolean(ConfigManager::ALLOW_CLONES)) {
		player = new Player(this);
		player->setName(name);

		player->useThing2();
		player->setID();

		if (!IOLoginData::preloadPlayer(player, name)) {
			disconnectClient("Your character could not be loaded.");
			return;
		}

		if (IOBan::isPlayerNamelocked(player->getGUID())) {
			disconnectClient("Your character has been namelocked.");
			return;
		}

		if (g_game.getGameState() == GAME_STATE_CLOSING && !player->hasFlag(PlayerFlag_CanAlwaysLogin)) {
			disconnectClient("The game is just going down.\nPlease try again later.");
			return;
		}

		if (g_game.getGameState() == GAME_STATE_CLOSED && !player->hasFlag(PlayerFlag_CanAlwaysLogin)) {
			disconnectClient("Server is currently closed. Please try again later.");
			return;
		}

		if (g_config.getBoolean(ConfigManager::ONE_PLAYER_ON_ACCOUNT) && player->getAccountType() < ACCOUNT_TYPE_GAMEMASTER && g_game.getPlayerByAccount(player->getAccount())) {
			disconnectClient("You may only login with one character\nof your account at the same time.");
			return;
		}

		if (!player->hasFlag(PlayerFlag_CannotBeBanned)) {
			BanInfo banInfo;
			if (IOBan::isAccountBanned(accountId, banInfo)) {
				if (banInfo.reason.empty()) {
					banInfo.reason = "(none)";
				}

				std::ostringstream ss;
				if (banInfo.expiresAt > 0) {
					ss << "Your account has been banned until " << formatDateShort(banInfo.expiresAt) << " by " << banInfo.bannedBy << ".\n\nReason specified:\n" << banInfo.reason;
				} else {
					ss << "Your account has been permanently banned by " << banInfo.bannedBy << ".\n\nReason specified:\n" << banInfo.reason;
				}
				disconnectClient(ss.str());
				return;
			}
		}

		if (!WaitingList::getInstance()->clientLogin(player)) {
			uint32_t currentSlot = WaitingList::getInstance()->getClientSlot(player);
			uint32_t retryTime = WaitingList::getTime(currentSlot);
			std::ostringstream ss;

			ss << "Too many players online.\nYou are at place "
			   << currentSlot << " on the waiting list.";

			OutputMessage_ptr output = OutputMessagePool::getInstance()->getOutputMessage(this, false);
			if (output) {
				output->AddByte(0x16);
				output->AddString(ss.str());
				output->AddByte(retryTime);
				OutputMessagePool::getInstance()->send(output);
			}

			getConnection()->closeConnection();
			return;
		}

		if (!IOLoginData::loadPlayerByName(player, name)) {
			disconnectClient("Your character could not be loaded.");
			return;
		}

		player->setOperatingSystem(operatingSystem);

		if (!g_game.placeCreature(player, player->getLoginPosition())) {
			if (!g_game.placeCreature(player, player->getTemplePosition(), false, true)) {
				disconnectClient("Temple position is wrong. Contact the administrator.");
				return;
			}
		}

		if (operatingSystem >= CLIENTOS_OTCLIENT_LINUX) {
			player->registerCreatureEvent("ExtendedOpcode");
		}

		player->lastIP = player->getIP();
		player->lastLoginSaved = std::max<time_t>(time(nullptr), player->lastLoginSaved + 1);
		m_acceptPackets = true;
	} else {
		if (eventConnect != 0 || !g_config.getBoolean(ConfigManager::REPLACE_KICK_ON_LOGIN)) {
			//Already trying to connect
			disconnectClient("You are already logged in.");
			return;
		}

		if (_player->client) {
			_player->disconnect();
			_player->isConnecting = true;

			addRef();
			eventConnect = g_scheduler.addEvent(createSchedulerTask(1000, std::bind(&ProtocolGame::connect, this, _player->getID(), operatingSystem)));
			return;
		}

		addRef();
		connect(_player->getID(), operatingSystem);
	}
}

void ProtocolGame::connect(uint32_t playerId, OperatingSystem_t operatingSystem)
{
	unRef();
	eventConnect = 0;

	Player* _player = g_game.getPlayerByID(playerId);
	if (!_player || _player->client) {
		disconnectClient("You are already logged in.");
		return;
	}

	player = _player;
	player->useThing2();

	g_chat->removeUserFromAllChannels(*player);
	player->setOperatingSystem(operatingSystem);
	player->isConnecting = false;

	player->client = this;
	sendAddCreature(player, player->getPosition(), 0, false);
	player->lastIP = player->getIP();
	player->lastLoginSaved = std::max<time_t>(time(nullptr), player->lastLoginSaved + 1);
	m_acceptPackets = true;
}

void ProtocolGame::logout(bool displayEffect, bool forced)
{
	//dispatcher thread
	if (!player) {
		return;
	}

	if (!player->isRemoved()) {
		if (!forced) {
			if (!player->isAccessPlayer()) {
				if (player->getTile()->hasFlag(TILESTATE_NOLOGOUT)) {
					player->sendCancelMessage(RETURNVALUE_YOUCANNOTLOGOUTHERE);
					return;
				}

				if (!player->getTile()->hasFlag(TILESTATE_PROTECTIONZONE) && player->hasCondition(CONDITION_INFIGHT)) {
					player->sendCancelMessage(RETURNVALUE_YOUMAYNOTLOGOUTDURINGAFIGHT);
					return;
				}
			}

			//scripting event - onLogout
			if (!g_creatureEvents->playerLogout(player)) {
				//Let the script handle the error message
				return;
			}
		}

		if (displayEffect && player->getHealth() > 0) {
			g_game.addMagicEffect(player->getPosition(), CONST_ME_POFF);
		}
	}

	if (Connection_ptr connection = getConnection()) {
		connection->closeConnection();
	}

	g_game.removeCreature(player);
}

void ProtocolGame::onRecvFirstMessage(NetworkMessage& msg)
{
	if (g_game.getGameState() == GAME_STATE_SHUTDOWN) {
		getConnection()->closeConnection();
		return;
	}

	OperatingSystem_t operatingSystem = static_cast<OperatingSystem_t>(msg.get<uint16_t>());
	version = msg.get<uint16_t>();

	if (!RSA_decrypt(msg)) {
		getConnection()->closeConnection();
		return;
	}

	uint32_t key[4];
	key[0] = msg.get<uint32_t>();
	key[1] = msg.get<uint32_t>();
	key[2] = msg.get<uint32_t>();
	key[3] = msg.get<uint32_t>();
	enableXTEAEncryption();
	setXTEAKey(key);

	if (operatingSystem >= CLIENTOS_OTCLIENT_LINUX) {
		NetworkMessage opcodeMessage;
		opcodeMessage.AddByte(0x32);
		opcodeMessage.AddByte(0x00);
		opcodeMessage.Add<uint16_t>(0x00);
		writeToOutputBuffer(opcodeMessage);
	}

	msg.SkipBytes(1); // gamemaster flag
	std::string accountName = msg.GetString();
	std::string characterName = msg.GetString();
	std::string password = msg.GetString();

	uint32_t timeStamp = msg.get<uint32_t>();
	uint8_t randNumber = msg.GetByte();
	if (m_challengeTimestamp != timeStamp || m_challengeRandom != randNumber) {
		getConnection()->closeConnection();
		return;
	}

#define dispatchDisconnectClient(err) g_dispatcher.addTask(createTask(std::bind(&ProtocolGame::disconnectClient, this, err)))

	if (version < CLIENT_VERSION_MIN || version > CLIENT_VERSION_MAX) {
		dispatchDisconnectClient("Only clients with protocol " CLIENT_VERSION_STR " allowed!");
		return;
	}

	if (accountName.empty()) {
		dispatchDisconnectClient("You must enter your account name.");
		return;
	}

	if (g_game.getGameState() == GAME_STATE_STARTUP) {
		dispatchDisconnectClient("Gameworld is starting up. Please wait.");
		return;
	}

	if (g_game.getGameState() == GAME_STATE_MAINTAIN) {
		dispatchDisconnectClient("Gameworld is under maintenance. Please re-connect in a while.");
		return;
	}

	BanInfo banInfo;
	if (IOBan::isIpBanned(getIP(), banInfo)) {
		if (banInfo.reason.empty()) {
			banInfo.reason = "(none)";
		}

		std::ostringstream ss;
		ss << "Your IP has been banned until " << formatDateShort(banInfo.expiresAt) << " by " << banInfo.bannedBy << ".\n\nReason specified:\n" << banInfo.reason;
		dispatchDisconnectClient(ss.str());
		return;
	}

	uint32_t accountId = IOLoginData::gameworldAuthentication(accountName, password, characterName);
	if (accountId == 0) {
		dispatchDisconnectClient("Account name or password is not correct.");
		return;
	}

#undef dispatchDisconnectClient

	g_dispatcher.addTask(createTask(std::bind(&ProtocolGame::login, this, characterName, accountId, operatingSystem)));
}

void ProtocolGame::onConnect()
{
	OutputMessage_ptr output = OutputMessagePool::getInstance()->getOutputMessage(this, false);
	if (output) {
		static std::random_device rd;
		static std::ranlux24 generator(rd());
		static std::uniform_int_distribution<uint16_t> randNumber(0x00, 0xFF);

		// Skip checksum
		output->SkipBytes(sizeof(uint32_t));

		// Packet length & type
		output->Add<uint16_t>(0x0006);
		output->AddByte(0x1F);

		// Add timestamp & random number
		m_challengeTimestamp = static_cast<uint32_t>(time(nullptr));
		output->Add<uint32_t>(m_challengeTimestamp);

		m_challengeRandom = randNumber(generator);
		output->AddByte(m_challengeRandom);

		// Go back and write checksum
		output->SkipBytes(-12);
		output->Add<uint32_t>(adlerChecksum(output->getOutputBuffer() + sizeof(uint32_t), 8));

		OutputMessagePool::getInstance()->send(output);
	}
}

void ProtocolGame::disconnectClient(const std::string& message)
{
	OutputMessage_ptr output = OutputMessagePool::getInstance()->getOutputMessage(this, false);
	if (output) {
		output->AddByte(0x14);
		output->AddString(message);
		OutputMessagePool::getInstance()->send(output);
	}
	disconnect();
}

void ProtocolGame::disconnect()
{
	Connection_ptr connection = getConnection();
	if (connection) {
		connection->closeConnection();
	}
}

void ProtocolGame::writeToOutputBuffer(const NetworkMessage& msg)
{
	OutputMessage_ptr out = getOutputBuffer(msg.getLength());
	if (out) {
		out->append(msg);
	}
}

void ProtocolGame::parsePacket(NetworkMessage& msg)
{
	if (!m_acceptPackets || g_game.getGameState() == GAME_STATE_SHUTDOWN || msg.getLength() <= 0) {
		return;
	}

	uint8_t recvbyte = msg.GetByte();

	if (!player) {
		if (recvbyte == 0x0F) {
			disconnect();
		}

		return;
	}

	//a dead player can not performs actions
	if (player->isRemoved() || player->getHealth() <= 0) {
		if (recvbyte == 0x0F) {
			disconnect();
			return;
		}

		if (recvbyte != 0x14) {
			return;
		}
	}

	switch (recvbyte) {
		case 0x14: g_dispatcher.addTask(createTask(std::bind(&ProtocolGame::logout, this, true, false))); break;
		case 0x1E: addGameTask(&Game::playerReceivePing, player->getID()); break;
		case 0x32: parseExtendedOpcode(msg); break; //otclient extended opcode
		case 0x64: parseAutoWalk(msg); break;
		case 0x65: addGameTask(&Game::playerMove, player->getID(), NORTH); break;
		case 0x66: addGameTask(&Game::playerMove, player->getID(), EAST); break;
		case 0x67: addGameTask(&Game::playerMove, player->getID(), SOUTH); break;
		case 0x68: addGameTask(&Game::playerMove, player->getID(), WEST); break;
		case 0x69: addGameTask(&Game::playerStopAutoWalk, player->getID()); break;
		case 0x6A: addGameTask(&Game::playerMove, player->getID(), NORTHEAST); break;
		case 0x6B: addGameTask(&Game::playerMove, player->getID(), SOUTHEAST); break;
		case 0x6C: addGameTask(&Game::playerMove, player->getID(), SOUTHWEST); break;
		case 0x6D: addGameTask(&Game::playerMove, player->getID(), NORTHWEST); break;
		case 0x6F: addGameTaskTimed(DISPATCHER_TASK_EXPIRATION, &Game::playerTurn, player->getID(), NORTH); break;
		case 0x70: addGameTaskTimed(DISPATCHER_TASK_EXPIRATION, &Game::playerTurn, player->getID(), EAST); break;
		case 0x71: addGameTaskTimed(DISPATCHER_TASK_EXPIRATION, &Game::playerTurn, player->getID(), SOUTH); break;
		case 0x72: addGameTaskTimed(DISPATCHER_TASK_EXPIRATION, &Game::playerTurn, player->getID(), WEST); break;
		case 0x78: parseThrow(msg); break;
		case 0x79: parseLookInShop(msg); break;
		case 0x7A: parsePlayerPurchase(msg); break;
		case 0x7B: parsePlayerSale(msg); break;
		case 0x7C: addGameTask(&Game::playerCloseShop, player->getID()); break;
		case 0x7D: parseRequestTrade(msg); break;
		case 0x7E: parseLookInTrade(msg); break;
		case 0x7F: addGameTask(&Game::playerAcceptTrade, player->getID()); break;
		case 0x80: addGameTask(&Game::playerCloseTrade, player->getID()); break;
		case 0x82: parseUseItem(msg); break;
		case 0x83: parseUseItemEx(msg); break;
		case 0x84: parseUseWithCreature(msg); break;
		case 0x85: parseRotateItem(msg); break;
		case 0x87: parseCloseContainer(msg); break;
		case 0x88: parseUpArrowContainer(msg); break;
		case 0x89: parseTextWindow(msg); break;
		case 0x8A: parseHouseWindow(msg); break;
		case 0x8C: parseLookAt(msg); break;
		case 0x8D: parseLookInBattleList(msg); break;
		case 0x8E: /* join aggression */ break;
		case 0x96: parseSay(msg); break;
		case 0x97: addGameTask(&Game::playerRequestChannels, player->getID()); break;
		case 0x98: parseOpenChannel(msg); break;
		case 0x99: parseCloseChannel(msg); break;
		case 0x9A: parseOpenPrivateChannel(msg); break;
		case 0x9B: /* parseProcessRuleViolation */ break;
		case 0x9C: /* parseCloseRuleViolation */ break;
		case 0x9D: /* parseCancelRuleViolation */ break;
		case 0x9E: addGameTask(&Game::playerCloseNpcChannel, player->getID()); break;
		case 0xA0: parseFightModes(msg); break;
		case 0xA1: parseAttack(msg); break;
		case 0xA2: parseFollow(msg); break;
		case 0xA3: parseInviteToParty(msg); break;
		case 0xA4: parseJoinParty(msg); break;
		case 0xA5: parseRevokePartyInvite(msg); break;
		case 0xA6: parsePassPartyLeadership(msg); break;
		case 0xA7: addGameTask(&Game::playerLeaveParty, player->getID()); break;
		case 0xA8: parseEnableSharedPartyExperience(msg); break;
		case 0xAA: addGameTask(&Game::playerCreatePrivateChannel, player->getID()); break;
		case 0xAB: parseChannelInvite(msg); break;
		case 0xAC: parseChannelExclude(msg); break;
		case 0xBE: addGameTask(&Game::playerCancelAttackAndFollow, player->getID()); break;
		case 0xC9: /* update tile */ break;
		case 0xCA: parseUpdateContainer(msg); break;
		case 0xD2: addGameTask(&Game::playerRequestOutfit, player->getID()); break;
		case 0xD3: parseSetOutfit(msg); break;
		case 0xDC: parseAddVip(msg); break;
		case 0xDD: parseRemoveVip(msg); break;
		case 0xE6: parseBugReport(msg); break;
		case 0xE7: /* thank you */ break;
		case 0xE8: parseDebugAssert(msg); break;
		case 0xF0: addGameTaskTimed(DISPATCHER_TASK_EXPIRATION, &Game::playerShowQuestLog, player->getID()); break;
		case 0xF1: parseQuestLine(msg); break;
		case 0xF2: /* rule violation report */ break;
		case 0xF3: /* get object info */ break;

		default:
			// std::cout << "Player: " << player->getName() << " sent an unknown packet header: 0x" << std::hex << static_cast<uint16_t>(recvbyte) << std::dec << "!" << std::endl;
			break;
	}

	if (msg.isOverrun()) {
		disconnect();
	}
}

void ProtocolGame::GetTileDescription(const Tile* tile, NetworkMessage& msg)
{
	int32_t count;
	if (tile->ground) {
		msg.AddItem(tile->ground);
		count = 1;
	} else {
		count = 0;
	}

	const TileItemVector* items = tile->getItemList();
	if (items) {
		for (auto it = items->getBeginTopItem(); it != items->getEndTopItem(); ++it) {
			msg.AddItem(*it);

			if (++count == 10) {
				return;
			}
		}
	}

	const CreatureVector* creatures = tile->getCreatures();
	if (creatures) {
		for (const Creature* creature : boost::adaptors::reverse(*creatures)) {
			if (!player->canSeeCreature(creature)) {
				continue;
			}

			bool known;
			uint32_t removedKnown;
			checkCreatureAsKnown(creature->getID(), known, removedKnown);
			AddCreature(msg, creature, known, removedKnown);

			if (++count == 10) {
				return;
			}
		}
	}

	if (items) {
		for (auto it = items->getBeginDownItem(); it != items->getEndDownItem(); ++it) {
			msg.AddItem(*it);

			if (++count == 10) {
				return;
			}
		}
	}
}

void ProtocolGame::GetMapDescription(int32_t x, int32_t y, int32_t z, int32_t width, int32_t height, NetworkMessage& msg)
{
	int32_t skip = -1;
	int32_t startz, endz, zstep;

	if (z > 7) {
		startz = z - 2;
		endz = std::min<int32_t>(MAP_MAX_LAYERS - 1, z + 2);
		zstep = 1;
	} else {
		startz = 7;
		endz = 0;
		zstep = -1;
	}

	for (int32_t nz = startz; nz != endz + zstep; nz += zstep) {
		GetFloorDescription(msg, x, y, nz, width, height, z - nz, skip);
	}

	if (skip >= 0) {
		msg.AddByte(skip);
		msg.AddByte(0xFF);
	}
}

void ProtocolGame::GetFloorDescription(NetworkMessage& msg, int32_t x, int32_t y, int32_t z, int32_t width, int32_t height, int32_t offset, int32_t& skip)
{
	for (int32_t nx = 0; nx < width; nx++) {
		for (int32_t ny = 0; ny < height; ny++) {
			Tile* tile = g_game.map.getTile(x + nx + offset, y + ny + offset, z);
			if (tile) {
				if (skip >= 0) {
					msg.AddByte(skip);
					msg.AddByte(0xFF);
				}

				skip = 0;
				GetTileDescription(tile, msg);
			} else if (skip == 0xFE) {
				msg.AddByte(0xFF);
				msg.AddByte(0xFF);
				skip = -1;
			} else {
				++skip;
			}
		}
	}
}

void ProtocolGame::checkCreatureAsKnown(uint32_t id, bool& known, uint32_t& removedKnown)
{
	auto result = knownCreatureSet.insert(id);
	if (!result.second) {
		known = true;
		return;
	}

	known = false;

	if (knownCreatureSet.size() > 250) {
		// Look for a creature to remove
		for (std::unordered_set<uint32_t>::iterator it = knownCreatureSet.begin(); it != knownCreatureSet.end(); ++it) {
			Creature* creature = g_game.getCreatureByID(*it);
			if (!canSee(creature)) {
				removedKnown = *it;
				knownCreatureSet.erase(it);
				return;
			}
		}

		// Bad situation. Let's just remove anyone.
		std::unordered_set<uint32_t>::iterator it = knownCreatureSet.begin();
		if (*it == id) {
			++it;
		}

		removedKnown = *it;
		knownCreatureSet.erase(it);
	} else {
		removedKnown = 0;
	}
}

bool ProtocolGame::canSee(const Creature* c) const
{
	if (!c || !player || c->isRemoved()) {
		return false;
	}

	if (!player->canSeeCreature(c)) {
		return false;
	}

	return canSee(c->getPosition());
}

bool ProtocolGame::canSee(const Position& pos) const
{
	return canSee(pos.x, pos.y, pos.z);
}

bool ProtocolGame::canSee(int32_t x, int32_t y, int32_t z) const
{
	if (!player) {
		return false;
	}

	const Position& myPos = player->getPosition();
	if (myPos.z <= 7) {
		//we are on ground level or above (7 -> 0)
		//view is from 7 -> 0
		if (z > 7) {
			return false;
		}
	} else if (myPos.z >= 8) {
		//we are underground (8 -> 15)
		//view is +/- 2 from the floor we stand on
		if (std::abs(myPos.getZ() - z) > 2) {
			return false;
		}
	}

	//negative offset means that the action taken place is on a lower floor than ourself
	int32_t offsetz = myPos.getZ() - z;
	if ((x >= myPos.getX() - 8 + offsetz) && (x <= myPos.getX() + 9 + offsetz) &&
	        (y >= myPos.getY() - 6 + offsetz) && (y <= myPos.getY() + 7 + offsetz)) {
		return true;
	}
	return false;
}

// Parse methods
void ProtocolGame::parseChannelInvite(NetworkMessage& msg)
{
	const std::string name = msg.GetString();
	addGameTask(&Game::playerChannelInvite, player->getID(), name);
}

void ProtocolGame::parseChannelExclude(NetworkMessage& msg)
{
	const std::string name = msg.GetString();
	addGameTask(&Game::playerChannelExclude, player->getID(), name);
}

void ProtocolGame::parseOpenChannel(NetworkMessage& msg)
{
	uint16_t channelId = msg.get<uint16_t>();
	addGameTask(&Game::playerOpenChannel, player->getID(), channelId);
}

void ProtocolGame::parseCloseChannel(NetworkMessage& msg)
{
	uint16_t channelId = msg.get<uint16_t>();
	addGameTask(&Game::playerCloseChannel, player->getID(), channelId);
}

void ProtocolGame::parseOpenPrivateChannel(NetworkMessage& msg)
{
	const std::string receiver = msg.GetString();
	addGameTask(&Game::playerOpenPrivateChannel, player->getID(), receiver);
}

void ProtocolGame::parseAutoWalk(NetworkMessage& msg)
{
	std::list<Direction> path;

	uint8_t numdirs = msg.GetByte();
	for (uint8_t i = 0; i < numdirs; ++i) {
		uint8_t rawdir = msg.GetByte();
		switch (rawdir) {
			case 1: path.push_back(EAST); break;
			case 2: path.push_back(NORTHEAST); break;
			case 3: path.push_back(NORTH); break;
			case 4: path.push_back(NORTHWEST); break;
			case 5: path.push_back(WEST); break;
			case 6: path.push_back(SOUTHWEST); break;
			case 7: path.push_back(SOUTH); break;
			case 8: path.push_back(SOUTHEAST); break;
			default: break;
		}
	}

	if (path.empty()) {
		return;
	}

	addGameTask(&Game::playerAutoWalk, player->getID(), path);
}

void ProtocolGame::parseSetOutfit(NetworkMessage& msg)
{
	Outfit_t newOutfit;
	newOutfit.lookType = msg.get<uint16_t>();
	newOutfit.lookHead = msg.GetByte();
	newOutfit.lookBody = msg.GetByte();
	newOutfit.lookLegs = msg.GetByte();
	newOutfit.lookFeet = msg.GetByte();
	newOutfit.lookAddons = msg.GetByte();
	addGameTask(&Game::playerChangeOutfit, player->getID(), newOutfit);
}

void ProtocolGame::parseUseItem(NetworkMessage& msg)
{
	Position pos = msg.GetPosition();
	uint16_t spriteId = msg.get<uint16_t>();
	uint8_t stackpos = msg.GetByte();
	uint8_t index = msg.GetByte();
	bool isHotkey = (pos.x == 0xFFFF && pos.y == 0 && pos.z == 0);
	addGameTaskTimed(DISPATCHER_TASK_EXPIRATION, &Game::playerUseItem, player->getID(), pos, stackpos, index, spriteId, isHotkey);
}

void ProtocolGame::parseUseItemEx(NetworkMessage& msg)
{
	Position fromPos = msg.GetPosition();
	uint16_t fromSpriteId = msg.get<uint16_t>();
	uint8_t fromStackPos = msg.GetByte();
	Position toPos = msg.GetPosition();
	uint16_t toSpriteId = msg.get<uint16_t>();
	uint8_t toStackPos = msg.GetByte();
	bool isHotkey = (fromPos.x == 0xFFFF && fromPos.y == 0 && fromPos.z == 0);
	addGameTaskTimed(DISPATCHER_TASK_EXPIRATION, &Game::playerUseItemEx, player->getID(), fromPos, fromStackPos, fromSpriteId, toPos, toStackPos, toSpriteId, isHotkey);
}

void ProtocolGame::parseUseWithCreature(NetworkMessage& msg)
{
	Position fromPos = msg.GetPosition();
	uint16_t spriteId = msg.get<uint16_t>();
	uint8_t fromStackPos = msg.GetByte();
	uint32_t creatureId = msg.get<uint32_t>();
	bool isHotkey = (fromPos.x == 0xFFFF && fromPos.y == 0 && fromPos.z == 0);
	addGameTaskTimed(DISPATCHER_TASK_EXPIRATION, &Game::playerUseWithCreature, player->getID(), fromPos, fromStackPos, creatureId, spriteId, isHotkey);
}

void ProtocolGame::parseCloseContainer(NetworkMessage& msg)
{
	uint8_t cid = msg.GetByte();
	addGameTask(&Game::playerCloseContainer, player->getID(), cid);
}

void ProtocolGame::parseUpArrowContainer(NetworkMessage& msg)
{
	uint8_t cid = msg.GetByte();
	addGameTask(&Game::playerMoveUpContainer, player->getID(), cid);
}

void ProtocolGame::parseUpdateContainer(NetworkMessage& msg)
{
	uint8_t cid = msg.GetByte();
	addGameTask(&Game::playerUpdateContainer, player->getID(), cid);
}

void ProtocolGame::parseThrow(NetworkMessage& msg)
{
	Position fromPos = msg.GetPosition();
	uint16_t spriteId = msg.get<uint16_t>();
	uint8_t fromStackpos = msg.GetByte();
	Position toPos = msg.GetPosition();
	uint8_t count = msg.GetByte();

	if (toPos != fromPos) {
		addGameTaskTimed(DISPATCHER_TASK_EXPIRATION, &Game::playerMoveThing, player->getID(), fromPos, spriteId, fromStackpos, toPos, count);
	}
}

void ProtocolGame::parseLookAt(NetworkMessage& msg)
{
	Position pos = msg.GetPosition();
	uint16_t spriteId = msg.get<uint16_t>();
	uint8_t stackpos = msg.GetByte();
	addGameTaskTimed(DISPATCHER_TASK_EXPIRATION, &Game::playerLookAt, player->getID(), pos, spriteId, stackpos);
}

void ProtocolGame::parseLookInBattleList(NetworkMessage& msg)
{
	uint32_t creatureId = msg.get<uint32_t>();
	addGameTaskTimed(DISPATCHER_TASK_EXPIRATION, &Game::playerLookInBattleList, player->getID(), creatureId);
}

void ProtocolGame::parseSay(NetworkMessage& msg)
{
	std::string receiver;
	uint16_t channelId;

	SpeakClasses type = static_cast<SpeakClasses>(msg.GetByte());
	switch (type) {
		case SPEAK_PRIVATE_FROM:
		case SPEAK_PRIVATE_RED_FROM:
		//case SPEAK_RVR_ANSWER
			receiver = msg.GetString();
			break;

		case SPEAK_CHANNEL_Y:
		case SPEAK_CHANNEL_W:
		case SPEAK_CHANNEL_R1:
		case SPEAK_CHANNEL_R2:
		case SPEAK_CHANNEL_O:
			channelId = msg.get<uint16_t>();
			break;

		default:
			channelId = 0;
			break;
	}

	const std::string text = msg.GetString();
	if (text.length() > 255) {
		return;
	}

	addGameTask(&Game::playerSay, player->getID(), channelId, type, receiver, text);
}

void ProtocolGame::parseFightModes(NetworkMessage& msg)
{
	uint8_t rawFightMode = msg.GetByte(); //1 - offensive, 2 - balanced, 3 - defensive
	uint8_t rawChaseMode = msg.GetByte(); // 0 - stand while fightning, 1 - chase opponent
	uint8_t rawSecureMode = msg.GetByte(); // 0 - can't attack unmarked, 1 - can attack unmarked

	chaseMode_t chaseMode;
	if (rawChaseMode == 1) {
		chaseMode = CHASEMODE_FOLLOW;
	} else {
		chaseMode = CHASEMODE_STANDSTILL;
	}

	fightMode_t fightMode;
	if (rawFightMode == 1) {
		fightMode = FIGHTMODE_ATTACK;
	} else if (rawFightMode == 2) {
		fightMode = FIGHTMODE_BALANCED;
	} else {
		fightMode = FIGHTMODE_DEFENSE;
	}

	secureMode_t secureMode;
	if (rawSecureMode == 1) {
		secureMode = SECUREMODE_ON;
	} else {
		secureMode = SECUREMODE_OFF;
	}

	addGameTask(&Game::playerSetFightModes, player->getID(), fightMode, chaseMode, secureMode);
}

void ProtocolGame::parseAttack(NetworkMessage& msg)
{
	uint32_t creatureId = msg.get<uint32_t>();
	// msg.get<uint32_t>(); creatureId (same as above)
	addGameTask(&Game::playerSetAttackedCreature, player->getID(), creatureId);
}

void ProtocolGame::parseFollow(NetworkMessage& msg)
{
	uint32_t creatureId = msg.get<uint32_t>();
	// msg.get<uint32_t>(); creatureId (same as above)
	addGameTask(&Game::playerFollowCreature, player->getID(), creatureId);
}

void ProtocolGame::parseTextWindow(NetworkMessage& msg)
{
	uint32_t windowTextId = msg.get<uint32_t>();
	const std::string newText = msg.GetString();
	addGameTask(&Game::playerWriteItem, player->getID(), windowTextId, newText);
}

void ProtocolGame::parseHouseWindow(NetworkMessage& msg)
{
	uint8_t doorId = msg.GetByte();
	uint32_t id = msg.get<uint32_t>();
	const std::string text = msg.GetString();
	addGameTask(&Game::playerUpdateHouseWindow, player->getID(), doorId, id, text);
}

void ProtocolGame::parseLookInShop(NetworkMessage& msg)
{
	uint16_t id = msg.get<uint16_t>();
	uint8_t count = msg.GetByte();
	addGameTaskTimed(DISPATCHER_TASK_EXPIRATION, &Game::playerLookInShop, player->getID(), id, count);
}

void ProtocolGame::parsePlayerPurchase(NetworkMessage& msg)
{
	uint16_t id = msg.get<uint16_t>();
	uint8_t count = msg.GetByte();
	uint8_t amount = msg.GetByte();
	bool ignoreCap = msg.GetByte() != 0;
	bool inBackpacks = msg.GetByte() != 0;
	addGameTaskTimed(DISPATCHER_TASK_EXPIRATION, &Game::playerPurchaseItem, player->getID(), id, count, amount, ignoreCap, inBackpacks);
}

void ProtocolGame::parsePlayerSale(NetworkMessage& msg)
{
	uint16_t id = msg.get<uint16_t>();
	uint8_t count = msg.GetByte();
	uint8_t amount = msg.GetByte();
	bool ignoreEquipped = msg.GetByte() != 0;
	addGameTaskTimed(DISPATCHER_TASK_EXPIRATION, &Game::playerSellItem, player->getID(), id, count, amount, ignoreEquipped);
}

void ProtocolGame::parseRequestTrade(NetworkMessage& msg)
{
	Position pos = msg.GetPosition();
	uint16_t spriteId = msg.get<uint16_t>();
	uint8_t stackpos = msg.GetByte();
	uint32_t playerId = msg.get<uint32_t>();
	addGameTask(&Game::playerRequestTrade, player->getID(), pos, stackpos, playerId, spriteId);
}

void ProtocolGame::parseLookInTrade(NetworkMessage& msg)
{
	bool counterOffer = (msg.GetByte() == 0x01);
	uint8_t index = msg.GetByte();
	addGameTaskTimed(DISPATCHER_TASK_EXPIRATION, &Game::playerLookInTrade, player->getID(), counterOffer, index);
}

void ProtocolGame::parseAddVip(NetworkMessage& msg)
{
	const std::string name = msg.GetString();
	addGameTask(&Game::playerRequestAddVip, player->getID(), name);
}

void ProtocolGame::parseRemoveVip(NetworkMessage& msg)
{
	uint32_t guid = msg.get<uint32_t>();
	addGameTask(&Game::playerRequestRemoveVip, player->getID(), guid);
}

void ProtocolGame::parseRotateItem(NetworkMessage& msg)
{
	Position pos = msg.GetPosition();
	uint16_t spriteId = msg.get<uint16_t>();
	uint8_t stackpos = msg.GetByte();
	addGameTaskTimed(DISPATCHER_TASK_EXPIRATION, &Game::playerRotateItem, player->getID(), pos, stackpos, spriteId);
}

void ProtocolGame::parseBugReport(NetworkMessage& msg)
{
	std::string bug = msg.GetString();
	addGameTask(&Game::playerReportBug, player->getID(), bug);
}

void ProtocolGame::parseDebugAssert(NetworkMessage& msg)
{
	if (m_debugAssertSent) {
		return;
	}

	m_debugAssertSent = true;

	std::string assertLine = msg.GetString();
	std::string date = msg.GetString();
	std::string description = msg.GetString();
	std::string comment = msg.GetString();
	addGameTask(&Game::playerDebugAssert, player->getID(), assertLine, date, description, comment);
}

void ProtocolGame::parseInviteToParty(NetworkMessage& msg)
{
	uint32_t targetId = msg.get<uint32_t>();
	addGameTask(&Game::playerInviteToParty, player->getID(), targetId);
}

void ProtocolGame::parseJoinParty(NetworkMessage& msg)
{
	uint32_t targetId = msg.get<uint32_t>();
	addGameTask(&Game::playerJoinParty, player->getID(), targetId);
}

void ProtocolGame::parseRevokePartyInvite(NetworkMessage& msg)
{
	uint32_t targetId = msg.get<uint32_t>();
	addGameTask(&Game::playerRevokePartyInvitation, player->getID(), targetId);
}

void ProtocolGame::parsePassPartyLeadership(NetworkMessage& msg)
{
	uint32_t targetId = msg.get<uint32_t>();
	addGameTask(&Game::playerPassPartyLeadership, player->getID(), targetId);
}

void ProtocolGame::parseEnableSharedPartyExperience(NetworkMessage& msg)
{
	bool sharedExpActive = msg.GetByte() == 1;
	addGameTask(&Game::playerEnableSharedPartyExperience, player->getID(), sharedExpActive);
}

void ProtocolGame::parseQuestLine(NetworkMessage& msg)
{
	uint16_t questId = msg.get<uint16_t>();
	addGameTask(&Game::playerShowQuestLine, player->getID(), questId);
}

// Send methods
void ProtocolGame::sendOpenPrivateChannel(const std::string& receiver)
{
	NetworkMessage msg;
	msg.AddByte(0xAD);
	msg.AddString(receiver);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendChannelEvent(uint16_t channelId, const std::string& playerName, ChannelEvent_t channelEvent)
{
	NetworkMessage msg;
	msg.AddByte(0xF3);
	msg.Add<uint16_t>(channelId);
	msg.AddString(playerName);
	msg.AddByte(channelEvent);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendCreatureOutfit(const Creature* creature, const Outfit_t& outfit)
{
	if (!canSee(creature)) {
		return;
	}

	NetworkMessage msg;
	msg.AddByte(0x8E);
	msg.Add<uint32_t>(creature->getID());
	AddOutfit(msg, outfit);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendCreatureLight(const Creature* creature)
{
	if (!canSee(creature)) {
		return;
	}

	NetworkMessage msg;
	AddCreatureLight(msg, creature);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendWorldLight(const LightInfo& lightInfo)
{
	NetworkMessage msg;
	AddWorldLight(msg, lightInfo);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendCreatureWalkthrough(const Creature* creature, bool walkthrough)
{
	if (!canSee(creature)) {
		return;
	}

	NetworkMessage msg;
	msg.AddByte(0x92);
	msg.Add<uint32_t>(creature->getID());
	msg.AddByte(walkthrough ? 0x00 : 0x01);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendCreatureShield(const Creature* creature)
{
	if (!canSee(creature)) {
		return;
	}

	NetworkMessage msg;
	msg.AddByte(0x91);
	msg.Add<uint32_t>(creature->getID());

	PartyShields_t shield = player->getPartyShield(creature->getPlayer());
	if (shield == SHIELD_GRAY) {
		shield = SHIELD_NONE;
	}

	msg.AddByte(shield);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendCreatureSkull(const Creature* creature)
{
	if (g_game.getWorldType() != WORLD_TYPE_PVP) {
		return;
	}

	if (!canSee(creature)) {
		return;
	}

	NetworkMessage msg;
	msg.AddByte(0x90);
	msg.Add<uint32_t>(creature->getID());
	msg.AddByte(player->getSkullClient(creature));
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendCreatureType(uint32_t creatureId, uint8_t creatureType)
{
	return;

	NetworkMessage msg;
	msg.AddByte(0x95);
	msg.Add<uint32_t>(creatureId);
	msg.AddByte(creatureType);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendCreatureHelpers(uint32_t creatureId, uint16_t helpers)
{
	return;

	NetworkMessage msg;
	msg.AddByte(0x94);
	msg.Add<uint32_t>(creatureId);
	msg.Add<uint16_t>(helpers);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendCreatureSquare(const Creature* creature, SquareColor_t color)
{
	if (!canSee(creature)) {
		return;
	}

	NetworkMessage msg;
	msg.AddByte(0x86);
	msg.Add<uint32_t>(creature->getID());
	msg.AddByte(color);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendTutorial(uint8_t tutorialId)
{
	NetworkMessage msg;
	msg.AddByte(0xDC);
	msg.AddByte(tutorialId);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendAddMarker(const Position& pos, uint8_t markType, const std::string& desc)
{
	NetworkMessage msg;
	msg.AddByte(0xDD);
	msg.AddPosition(pos);
	msg.AddByte(markType);
	msg.AddString(desc);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendReLoginWindow(uint8_t unfairFightReduction)
{
	NetworkMessage msg;
	msg.AddByte(0x28);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendStats()
{
	NetworkMessage msg;
	AddPlayerStats(msg);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendTextMessage(MessageClasses mclass, const std::string& message, Position* pos/* = nullptr*/, uint32_t value/* = 0*/, TextColor_t color/* = TEXTCOLOR_NONE*/)
{
	NetworkMessage msg;
	msg.AddByte(0xB4);
	msg.AddByte(mclass);
	msg.AddString(message);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendClosePrivate(uint16_t channelId)
{
	NetworkMessage msg;
	msg.AddByte(0xB3);
	msg.Add<uint16_t>(channelId);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendCreatePrivateChannel(uint16_t channelId, const std::string& channelName)
{
	NetworkMessage msg;
	msg.AddByte(0xB2);
	msg.Add<uint16_t>(channelId);
	msg.AddString(channelName);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendChannelsDialog()
{
	NetworkMessage msg;
	msg.AddByte(0xAB);

	const ChannelList& list = g_chat->getChannelList(*player);
	msg.AddByte(list.size());
	for (ChatChannel* channel : list) {
		msg.Add<uint16_t>(channel->getId());
		msg.AddString(channel->getName());
	}

	writeToOutputBuffer(msg);
}

void ProtocolGame::sendChannel(uint16_t channelId, const std::string& channelName, const UsersMap* channelUsers, const InvitedMap* invitedUsers)
{
	NetworkMessage msg;
	msg.AddByte(0xAC);

	msg.Add<uint16_t>(channelId);
	msg.AddString(channelName);

	/*if (channelUsers) {
		msg.Add<uint16_t>(channelUsers->size());
		for (const auto& it : *channelUsers) {
			msg.AddString(it.second->getName());
		}
	} else {
		msg.Add<uint16_t>(0x00);
	}

	if (invitedUsers) {
		msg.Add<uint16_t>(invitedUsers->size());
		for (const auto& it : *invitedUsers) {
			msg.AddString(it.second->getName());
		}
	} else {
		msg.Add<uint16_t>(0x00);
	}*/
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendChannelMessage(const std::string& author, const std::string& text, SpeakClasses type, uint16_t channel)
{
	NetworkMessage msg;
	msg.AddByte(0xAA);
	msg.Add<uint32_t>(0x00);
	msg.AddString(author);
	msg.Add<uint16_t>(0x00);
	msg.AddByte(type);
	msg.Add<uint16_t>(channel);
	msg.AddString(text);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendIcons(uint16_t icons)
{
	NetworkMessage msg;
	msg.AddByte(0xA2);
	msg.Add<uint16_t>(icons);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendContainer(uint8_t cid, const Container* container, bool hasParent, uint16_t firstIndex)
{
	NetworkMessage msg;
	msg.AddByte(0x6E);

	msg.AddByte(cid);

	msg.AddItem(container);
	msg.AddString(container->getName());

	msg.AddByte(container->capacity());

	msg.AddByte(hasParent ? 0x01 : 0x00);

	uint32_t maxItemsToSend = 0xFF;

	msg.AddByte(std::min<uint32_t>(maxItemsToSend, container->size()));

	uint32_t i = 0;
	const ItemDeque& itemList = container->getItemList();
	for (ItemDeque::const_iterator cit = itemList.begin() + firstIndex, end = itemList.end(); i < maxItemsToSend && cit != end; ++cit, ++i) {
		msg.AddItem(*cit);
	}

	writeToOutputBuffer(msg);
}

void ProtocolGame::sendShop(Npc* npc, const ShopInfoList& itemList)
{
	NetworkMessage msg;
	msg.AddByte(0x7A);
	msg.AddByte(std::min<size_t>(255, itemList.size()));

	uint32_t i = 0;
	for (ShopInfoList::const_iterator it = itemList.begin(), end = itemList.end(); i < 255 && it != end; ++it, ++i) {
		AddShopItem(msg, *it);
	}

	writeToOutputBuffer(msg);
}

void ProtocolGame::sendCloseShop()
{
	NetworkMessage msg;
	msg.AddByte(0x7C);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendSaleItemList(const std::list<ShopInfo>& shop)
{
	NetworkMessage msg;
	msg.AddByte(0x7B);
	msg.Add<uint32_t>((uint32_t)player->getMoney());

	std::map<uint32_t, uint32_t> saleMap;

	if (shop.size() <= 5) {
		// For very small shops it's not worth it to create the complete map
		for (const ShopInfo& shopInfo : shop) {
			if (shopInfo.sellPrice > 0) {
				int8_t subtype = -1;

				const ItemType& itemType = Item::items[shopInfo.itemId];
				if (itemType.hasSubType() && !itemType.stackable) {
					subtype = (shopInfo.subType == 0 ? -1 : shopInfo.subType);
				}

				uint32_t count = player->getItemTypeCount(shopInfo.itemId, subtype);
				if (count > 0) {
					saleMap[shopInfo.itemId] = count;
				}
			}
		}
	} else {
		// Large shop, it's better to get a cached map of all item counts and use it
		// We need a temporary map since the finished map should only contain items
		// available in the shop
		std::map<uint32_t, uint32_t> tempSaleMap;
		player->getAllItemTypeCount(tempSaleMap);

		// We must still check manually for the special items that require subtype matches
		// (That is, fluids such as potions etc., actually these items are very few since
		// health potions now use their own ID)
		for (const ShopInfo& shopInfo : shop) {
			if (shopInfo.sellPrice > 0) {
				int8_t subtype = -1;

				const ItemType& itemType = Item::items[shopInfo.itemId];
				if (itemType.hasSubType() && !itemType.stackable) {
					subtype = (shopInfo.subType == 0 ? -1 : shopInfo.subType);
				}

				if (subtype != -1) {
					uint32_t count;

					if (!itemType.isFluidContainer() && !itemType.isSplash()) {
						count = player->getItemTypeCount(shopInfo.itemId, subtype);    // This shop item requires extra checks
					} else {
						count = subtype;
					}

					if (count > 0) {
						saleMap[shopInfo.itemId] = count;
					}
				} else {
					std::map<uint32_t, uint32_t>::const_iterator findIt = tempSaleMap.find(shopInfo.itemId);
					if (findIt != tempSaleMap.end() && findIt->second > 0) {
						saleMap[shopInfo.itemId] = findIt->second;
					}
				}
			}
		}
	}

	msg.AddByte(std::min<size_t>(255, saleMap.size()));

	uint32_t i = 0;

	for (std::map<uint32_t, uint32_t>::const_iterator it = saleMap.begin(), end = saleMap.end(); i < 255 && it != end; ++it, ++i) {
		msg.AddItemId(it->first);
		msg.AddByte(std::min<uint32_t>(255, it->second));
	}

	writeToOutputBuffer(msg);
}

void ProtocolGame::sendQuestLog()
{
	NetworkMessage msg;
	msg.AddByte(0xF0);
	msg.Add<uint16_t>(g_game.quests.getQuestsCount(player));

	for (const Quest& quest : g_game.quests.getQuests()) {
		if (quest.isStarted(player)) {
			msg.Add<uint16_t>(quest.getID());
			msg.AddString(quest.getName());
			msg.AddByte(quest.isCompleted(player));
		}
	}

	writeToOutputBuffer(msg);
}

void ProtocolGame::sendQuestLine(const Quest* quest)
{
	NetworkMessage msg;
	msg.AddByte(0xF1);
	msg.Add<uint16_t>(quest->getID());
	msg.AddByte(quest->getMissionsCount(player));

	for (const Mission& mission : quest->getMissions()) {
		if (mission.isStarted(player)) {
			msg.AddString(mission.getName(player));
			msg.AddString(mission.getDescription(player));
		}
	}

	writeToOutputBuffer(msg);
}

void ProtocolGame::sendTradeItemRequest(const Player* player, const Item* item, bool ack)
{
	NetworkMessage msg;

	if (ack) {
		msg.AddByte(0x7D);
	} else {
		msg.AddByte(0x7E);
	}

	msg.AddString(player->getName());

	if (const Container* tradeContainer = item->getContainer()) {
		std::list<const Container*> listContainer;
		listContainer.push_back(tradeContainer);

		std::list<const Item*> itemList;
		itemList.push_back(tradeContainer);

		while (!listContainer.empty()) {
			const Container* container = listContainer.front();
			listContainer.pop_front();

			for (Item* containerItem : container->getItemList()) {
				Container* tmpContainer = containerItem->getContainer();
				if (tmpContainer) {
					listContainer.push_back(tmpContainer);
				}
				itemList.push_back(containerItem);
			}
		}

		msg.AddByte(itemList.size());
		for (const Item* listItem : itemList) {
			msg.AddItem(listItem);
		}
	} else {
		msg.AddByte(0x01);
		msg.AddItem(item);
	}
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendCloseTrade()
{
	NetworkMessage msg;
	msg.AddByte(0x7F);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendCloseContainer(uint8_t cid)
{
	NetworkMessage msg;
	msg.AddByte(0x6F);
	msg.AddByte(cid);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendCreatureTurn(const Creature* creature, uint32_t stackPos)
{
	if (!canSee(creature)) {
		return;
	}

	NetworkMessage msg;
	msg.AddByte(0x6B);
	msg.AddPosition(creature->getPosition());
	msg.AddByte(stackPos);
	msg.Add<uint16_t>(0x63);
	msg.Add<uint32_t>(creature->getID());
	msg.AddByte(creature->getDirection());
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendCreatureSay(const Creature* creature, SpeakClasses type, const std::string& text, const Position* pos/* = nullptr*/)
{
	NetworkMessage msg;
	msg.AddByte(0xAA);

	static uint32_t statementId = 0;
	msg.Add<uint32_t>(++statementId);

	msg.AddString(creature->getName());

	//Add level only for players
	if (const Player* speaker = creature->getPlayer()) {
		msg.Add<uint16_t>(speaker->getLevel());
	} else {
		msg.Add<uint16_t>(0x00);
	}

	msg.AddByte(type);
	switch(type){
		case SPEAK_SAY:
		case SPEAK_WHISPER:
		case SPEAK_YELL:
		case SPEAK_MONSTER_SAY:
		case SPEAK_MONSTER_YELL:
		case SPEAK_PRIVATE_NP:
			assert(creature);
			msg.AddPosition(creature->getPosition());
			break;

		default:
			break;
	}

	/*if (pos) {
		msg.AddPosition(*pos);
	} else {
		msg.AddPosition(creature->getPosition());
	}*/

	msg.AddString(text);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendToChannel(const Creature* creature, SpeakClasses type, const std::string& text, uint16_t channelId)
{
	NetworkMessage msg;
	msg.AddByte(0xAA);

	static uint32_t statementId = 0;
	msg.Add<uint32_t>(++statementId);
	if (!creature) {
		msg.Add<uint32_t>(0x00);
	} else if (type == SPEAK_CHANNEL_R2) {
		msg.Add<uint32_t>(0x00);
		type = SPEAK_CHANNEL_R1;
	} else {
		msg.AddString(creature->getName());
		//Add level only for players
		if (const Player* speaker = creature->getPlayer()) {
			msg.Add<uint16_t>(speaker->getLevel());
		} else {
			msg.Add<uint16_t>(0x00);
		}
	}

	msg.AddByte(type);
	msg.Add<uint16_t>(channelId);
	msg.AddString(text);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendPrivateMessage(const Player* speaker, SpeakClasses type, const std::string& text)
{
	NetworkMessage msg;
	msg.AddByte(0xAA);
	static uint32_t statementId = 0;
	msg.Add<uint32_t>(++statementId);
	if (speaker) {
		msg.AddString(speaker->getName());
		msg.Add<uint16_t>(speaker->getLevel());
	} else {
		msg.Add<uint32_t>(0x00);
	}
	msg.AddByte(type);
	msg.AddString(text);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendCancelTarget()
{
	NetworkMessage msg;
	msg.AddByte(0xA3);
	msg.Add<uint32_t>(0x00);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendChangeSpeed(const Creature* creature, uint32_t speed)
{
	NetworkMessage msg;
	msg.AddByte(0x8F);
	msg.Add<uint32_t>(creature->getID());
	msg.Add<uint16_t>(speed);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendCancelWalk()
{
	NetworkMessage msg;
	msg.AddByte(0xB5);
	msg.AddByte(player->getDirection());
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendSkills()
{
	NetworkMessage msg;
	AddPlayerSkills(msg);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendPing()
{
	NetworkMessage msg;
	msg.AddByte(0x1E);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendDistanceShoot(const Position& from, const Position& to, uint8_t type)
{
	NetworkMessage msg;
	msg.AddByte(0x85);
	msg.AddPosition(from);
	msg.AddPosition(to);
	msg.AddByte(type);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendMagicEffect(const Position& pos, uint8_t type)
{
	if (!canSee(pos)) {
		return;
	}

	NetworkMessage msg;
	msg.AddByte(0x83);
	msg.AddPosition(pos);
	msg.AddByte(type);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendCreatureHealth(const Creature* creature)
{
	NetworkMessage msg;
	msg.AddByte(0x8C);
	msg.Add<uint32_t>(creature->getID());

	if (creature->isHealthHidden()) {
		msg.AddByte(0x00);
	} else {
		msg.AddByte(static_cast<int32_t>(std::ceil(static_cast<float>(creature->getHealth() * 100) / std::max<int32_t>(creature->getMaxHealth(), 1))));
	}
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendFYIBox(const std::string& message)
{
	NetworkMessage msg;
	msg.AddByte(0x15);
	msg.AddString(message);
	writeToOutputBuffer(msg);
}

//tile
void ProtocolGame::sendMapDescription(const Position& pos)
{
	NetworkMessage msg;
	msg.AddByte(0x64);
	msg.AddPosition(player->getPosition());
	GetMapDescription(pos.x - 8, pos.y - 6, pos.z, 18, 14, msg);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendAddTileItem(const Position& pos, uint32_t stackpos, const Item* item)
{
	if (!canSee(pos)) {
		return;
	}

	NetworkMessage msg;
	msg.AddByte(0x6A);
	msg.AddPosition(pos);
	msg.AddByte(stackpos);
	msg.AddItem(item);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendUpdateTileItem(const Position& pos, uint32_t stackpos, const Item* item)
{
	if (!canSee(pos)) {
		return;
	}

	NetworkMessage msg;
	msg.AddByte(0x6B);
	msg.AddPosition(pos);
	msg.AddByte(stackpos);
	msg.AddItem(item);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendRemoveTileThing(const Position& pos, uint32_t stackpos)
{
	if (!canSee(pos)) {
		return;
	}

	NetworkMessage msg;
	RemoveTileThing(msg, pos, stackpos);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendUpdateTile(const Tile* tile, const Position& pos)
{
	if (!canSee(pos)) {
		return;
	}

	NetworkMessage msg;
	msg.AddByte(0x69);
	msg.AddPosition(pos);

	if (tile) {
		GetTileDescription(tile, msg);
		msg.AddByte(0x00);
		msg.AddByte(0xFF);
	} else {
		msg.AddByte(0x01);
		msg.AddByte(0xFF);
	}

	writeToOutputBuffer(msg);
}

void ProtocolGame::sendFightModes()
{
	NetworkMessage msg;
	msg.AddByte(0xA7);
	msg.AddByte(player->fightMode);
	msg.AddByte(player->chaseMode);
	msg.AddByte(player->secureMode);
	msg.AddByte(PVP_MODE_DOVE);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendAddCreature(const Creature* creature, const Position& pos, int32_t stackpos, bool isLogin)
{
	if (!canSee(pos)) {
		return;
	}

	if (creature != player) {
		if (stackpos != -1) {
			NetworkMessage msg;
			msg.AddByte(0x6A);
			msg.AddPosition(pos);
			msg.AddByte(stackpos);

			bool known;
			uint32_t removedKnown;
			checkCreatureAsKnown(creature->getID(), known, removedKnown);
			AddCreature(msg, creature, known, removedKnown);
			writeToOutputBuffer(msg);
		}

		if (isLogin) {
			sendMagicEffect(pos, CONST_ME_TELEPORT);
		}
		return;
	}

	NetworkMessage msg;
	msg.AddByte(0x0A);

	msg.Add<uint32_t>(player->getID());
	msg.Add<uint16_t>(0x32); // beat duration (50)

	// can report bugs?
	if (player->getAccountType() >= ACCOUNT_TYPE_TUTOR) {
		msg.AddByte(0x01);
	} else {
		msg.AddByte(0x00);
	}

	writeToOutputBuffer(msg);

	sendMapDescription(pos);

	if (isLogin) {
		sendMagicEffect(pos, CONST_ME_TELEPORT);
	}

	sendInventoryItem(CONST_SLOT_HEAD, player->getInventoryItem(CONST_SLOT_HEAD));
	sendInventoryItem(CONST_SLOT_NECKLACE, player->getInventoryItem(CONST_SLOT_NECKLACE));
	sendInventoryItem(CONST_SLOT_BACKPACK, player->getInventoryItem(CONST_SLOT_BACKPACK));
	sendInventoryItem(CONST_SLOT_ARMOR, player->getInventoryItem(CONST_SLOT_ARMOR));
	sendInventoryItem(CONST_SLOT_RIGHT, player->getInventoryItem(CONST_SLOT_RIGHT));
	sendInventoryItem(CONST_SLOT_LEFT, player->getInventoryItem(CONST_SLOT_LEFT));
	sendInventoryItem(CONST_SLOT_LEGS, player->getInventoryItem(CONST_SLOT_LEGS));
	sendInventoryItem(CONST_SLOT_FEET, player->getInventoryItem(CONST_SLOT_FEET));
	sendInventoryItem(CONST_SLOT_RING, player->getInventoryItem(CONST_SLOT_RING));
	sendInventoryItem(CONST_SLOT_AMMO, player->getInventoryItem(CONST_SLOT_AMMO));

	sendStats();
	sendSkills();

	//gameworld light-settings
	LightInfo lightInfo;
	g_game.getWorldLightInfo(lightInfo);
	sendWorldLight(lightInfo);

	//player light level
	sendCreatureLight(creature);

	const std::forward_list<VIPEntry>& vipEntries = IOLoginData::getVIPEntries(player->getAccount());

	if (player->isAccessPlayer()) {
		for (const VIPEntry& entry : vipEntries) {
			VipStatus_t vipStatus;

			Player* vipPlayer = g_game.getPlayerByGUID(entry.guid);
			if (!vipPlayer) {
				vipStatus = VIPSTATUS_OFFLINE;
			} else {
				vipStatus = VIPSTATUS_ONLINE;
			}

			sendVIP(entry.guid, entry.name, entry.description, entry.icon, entry.notify, vipStatus);
		}
	} else {
		for (const VIPEntry& entry : vipEntries) {
			VipStatus_t vipStatus;

			Player* vipPlayer = g_game.getPlayerByGUID(entry.guid);
			if (!vipPlayer || vipPlayer->isInGhostMode()) {
				vipStatus = VIPSTATUS_OFFLINE;
			} else {
				vipStatus = VIPSTATUS_ONLINE;
			}

			sendVIP(entry.guid, entry.name, entry.description, entry.icon, entry.notify, vipStatus);
		}
	}

	player->sendIcons();
}

void ProtocolGame::sendMoveCreature(const Creature* creature, const Position& newPos, int32_t newStackPos, const Position& oldPos, int32_t oldStackPos, bool teleport)
{
	if (creature == player) {
		if (oldStackPos >= 10) {
			sendMapDescription(newPos);
		} else if (teleport) {
			NetworkMessage msg;
			RemoveTileThing(msg, oldPos, oldStackPos);
			writeToOutputBuffer(msg);
			sendMapDescription(newPos);
		} else {
			NetworkMessage msg;
			if (oldPos.z == 7 && newPos.z >= 8) {
				RemoveTileThing(msg, oldPos, oldStackPos);
			} else {
				msg.AddByte(0x6D);
				msg.AddPosition(oldPos);
				msg.AddByte(oldStackPos);
				msg.AddPosition(newPos);
			}

			if (newPos.z > oldPos.z) {
				MoveDownCreature(msg, creature, newPos, oldPos);
			} else if (newPos.z < oldPos.z) {
				MoveUpCreature(msg, creature, newPos, oldPos);
			}

			if (oldPos.y > newPos.y) { // north, for old x
				msg.AddByte(0x65);
				GetMapDescription(oldPos.x - 8, newPos.y - 6, newPos.z, 18, 1, msg);
			} else if (oldPos.y < newPos.y) { // south, for old x
				msg.AddByte(0x67);
				GetMapDescription(oldPos.x - 8, newPos.y + 7, newPos.z, 18, 1, msg);
			}

			if (oldPos.x < newPos.x) { // east, [with new y]
				msg.AddByte(0x66);
				GetMapDescription(newPos.x + 9, newPos.y - 6, newPos.z, 1, 14, msg);
			} else if (oldPos.x > newPos.x) { // west, [with new y]
				msg.AddByte(0x68);
				GetMapDescription(newPos.x - 8, newPos.y - 6, newPos.z, 1, 14, msg);
			}
			writeToOutputBuffer(msg);
		}
	} else if (canSee(oldPos) && canSee(creature->getPosition())) {
		if (teleport || (oldPos.z == 7 && newPos.z >= 8) || oldStackPos >= 10) {
			sendRemoveTileThing(oldPos, oldStackPos);
			sendAddCreature(creature, newPos, newStackPos, false);
		} else {
			NetworkMessage msg;
			msg.AddByte(0x6D);
			msg.AddPosition(oldPos);
			msg.AddByte(oldStackPos);
			msg.AddPosition(creature->getPosition());
			writeToOutputBuffer(msg);
		}
	} else if (canSee(oldPos)) {
		sendRemoveTileThing(oldPos, oldStackPos);
	} else if (canSee(creature->getPosition())) {
		sendAddCreature(creature, newPos, newStackPos, false);
	}
}

void ProtocolGame::sendInventoryItem(slots_t slot, const Item* item)
{
	NetworkMessage msg;
	if (item) {
		msg.AddByte(0x78);
		msg.AddByte(slot);
		msg.AddItem(item);
	} else {
		msg.AddByte(0x79);
		msg.AddByte(slot);
	}
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendAddContainerItem(uint8_t cid, uint16_t slot, const Item* item)
{
	NetworkMessage msg;
	msg.AddByte(0x70);
	msg.AddByte(cid);
	msg.AddItem(item);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendUpdateContainerItem(uint8_t cid, uint16_t slot, const Item* item)
{
	NetworkMessage msg;
	msg.AddByte(0x71);
	msg.AddByte(cid);
	msg.AddByte(std::min<uint16_t>(0xFF, slot));
	msg.AddItem(item);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendRemoveContainerItem(uint8_t cid, uint16_t slot)
{
	NetworkMessage msg;
	msg.AddByte(0x72);
	msg.AddByte(cid);
	msg.AddByte(std::min<uint16_t>(0xFF, slot));
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendTextWindow(uint32_t windowTextId, Item* item, uint16_t maxlen, bool canWrite)
{
	NetworkMessage msg;
	msg.AddByte(0x96);
	msg.Add<uint32_t>(windowTextId);
	msg.AddItem(item);

	if (canWrite) {
		msg.Add<uint16_t>(maxlen);
		msg.AddString(item->getText());
	} else {
		const std::string& text = item->getText();
		msg.Add<uint16_t>(text.size());
		msg.AddString(text);
	}

	const std::string& writer = item->getWriter();
	if (!writer.empty()) {
		msg.AddString(writer);
	} else {
		msg.Add<uint16_t>(0x00);
	}

	time_t writtenDate = item->getDate();
	if (writtenDate > 0) {
		msg.AddString(formatDateShort(writtenDate));
	} else {
		msg.Add<uint16_t>(0x00);
	}

	writeToOutputBuffer(msg);
}

void ProtocolGame::sendTextWindow(uint32_t windowTextId, uint32_t itemId, const std::string& text)
{
	NetworkMessage msg;
	msg.AddByte(0x96);
	msg.Add<uint32_t>(windowTextId);
	msg.AddItem(itemId, 1);
	msg.Add<uint16_t>(text.size());
	msg.AddString(text);
	msg.Add<uint16_t>(0x00);
	msg.Add<uint16_t>(0x00);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendHouseWindow(uint32_t windowTextId, const std::string& text)
{
	NetworkMessage msg;
	msg.AddByte(0x97);
	msg.AddByte(0x00);
	msg.Add<uint32_t>(windowTextId);
	msg.AddString(text);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendOutfitWindow()
{
	NetworkMessage msg;
	msg.AddByte(0xC8);

	Outfit_t currentOutfit = player->getDefaultOutfit();
	AddOutfit(msg, currentOutfit);

	std::vector<ProtocolOutfit> protocolOutfits;
	if (player->isAccessPlayer()) {
		static const std::string gamemasterOutfitName = "Gamemaster";
		protocolOutfits.emplace_back(
			75,
			&gamemasterOutfitName,
			0
		);
	}

	const auto& outfits = Outfits::getInstance()->getOutfits(player->getSex());
	protocolOutfits.reserve(outfits.size());
	for (const Outfit& outfit : outfits) {
		uint8_t addons;
		if (!player->getOutfitAddons(outfit, addons)) {
			continue;
		}

		protocolOutfits.emplace_back(
			outfit.lookType,
			&outfit.name,
			addons
		);
		if (protocolOutfits.size() == 50) { // Game client doesn't allow more than 50 outfits
			break;
		}
	}

	msg.AddByte(protocolOutfits.size());
	for (const ProtocolOutfit& outfit : protocolOutfits) {
		msg.Add<uint16_t>(outfit.lookType);
		msg.AddString(*outfit.name);
		msg.AddByte(outfit.addons);
	}

	player->hasRequestedOutfit(true);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendUpdatedVIPStatus(uint32_t guid, VipStatus_t newStatus)
{
	NetworkMessage msg;
	msg.AddByte(0xD3);
	msg.Add<uint32_t>(guid);
	msg.AddByte(newStatus);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendVIP(uint32_t guid, const std::string& name, const std::string& description, uint32_t icon, bool notify, VipStatus_t status)
{
	NetworkMessage msg;
	msg.AddByte(0xD2);
	msg.Add<uint32_t>(guid);
	msg.AddString(name);
	msg.AddByte(notify ? 0x01 : 0x00);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendSpellCooldown(uint8_t spellId, uint32_t time)
{
	/*NetworkMessage msg;
	msg.AddByte(0xA4);
	msg.AddByte(spellId);
	msg.Add<uint32_t>(time);
	writeToOutputBuffer(msg);*/
}

void ProtocolGame::sendDamageMessage(MessageClasses mclass, const std::string& message, const Position& pos,
                                     uint32_t primaryDamage/* = 0*/, TextColor_t primaryColor/* = TEXTCOLOR_NONE*/,
                                     uint32_t secondaryDamage/* = 0*/, TextColor_t secondaryColor/* = TEXTCOLOR_NONE*/)
{
	NetworkMessage msg;
	msg.AddByte(0xB4);
	msg.AddByte(mclass);
	msg.AddPosition(pos);
	msg.Add<uint32_t>(primaryDamage);
	msg.AddByte(primaryColor);
	msg.Add<uint32_t>(secondaryDamage);
	msg.AddByte(secondaryColor);
	msg.AddString(message);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendHealMessage(MessageClasses mclass, const std::string& message, const Position& pos, uint32_t heal, TextColor_t color)
{
	NetworkMessage msg;
	msg.AddByte(0xB4);
	msg.AddByte(mclass);
	msg.AddPosition(pos);
	msg.Add<uint32_t>(heal);
	msg.AddByte(color);
	msg.AddString(message);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendExperienceMessage(MessageClasses mclass, const std::string& message, const Position& pos, uint32_t exp, TextColor_t color)
{
	NetworkMessage msg;
	msg.AddByte(0xB4);
	msg.AddByte(mclass);
	msg.AddPosition(pos);
	msg.Add<uint32_t>(exp);
	msg.AddByte(color);
	msg.AddString(message);
	writeToOutputBuffer(msg);
}

////////////// Add common messages
void ProtocolGame::AddCreature(NetworkMessage& msg, const Creature* creature, bool known, uint32_t remove)
{
	//CreatureType_t creatureType = creature->getType();

	const Player* otherPlayer = creature->getPlayer();

	if (known) {
		msg.Add<uint16_t>(0x62);
		msg.Add<uint32_t>(creature->getID());
	} else {
		msg.Add<uint16_t>(0x61);
		msg.Add<uint32_t>(remove);
		msg.Add<uint32_t>(creature->getID());
		msg.AddString(creature->getName());
	}

	if (creature->isHealthHidden()) {
		msg.AddByte(0x00);
	} else {
		msg.AddByte(static_cast<int32_t>(std::ceil(static_cast<float>(creature->getHealth() * 100) / std::max<int32_t>(creature->getMaxHealth(), 1))));
	}

	msg.AddByte(creature->getDirection());

	if (!creature->isInGhostMode() && !creature->isInvisible()) {
		AddOutfit(msg, creature->getCurrentOutfit());
	} else {
		static Outfit_t outfit;
		AddOutfit(msg, outfit);
	}

	LightInfo lightInfo;
	creature->getCreatureLight(lightInfo);
	msg.AddByte(player->isAccessPlayer() ? 0xFF : lightInfo.level);
	msg.AddByte(lightInfo.color);

	msg.Add<uint16_t>(creature->getStepSpeed());
	msg.AddByte(player->getSkullClient(otherPlayer));

	PartyShields_t shield = player->getPartyShield(otherPlayer);

	if (shield == SHIELD_GRAY) {
		shield = SHIELD_NONE;
	}

	msg.AddByte(shield);

	if (!known) {
		uint8_t emblem = player->getGuildEmblem(otherPlayer);

		if (emblem > GUILDEMBLEM_NEUTRAL) {
			emblem = GUILDEMBLEM_NONE;
		}

		msg.AddByte(emblem);
	}

	msg.AddByte(player->canWalkthroughEx(creature) ? 0x00 : 0x01);
}

void ProtocolGame::AddPlayerStats(NetworkMessage& msg)
{
	msg.AddByte(0xA0);

	msg.Add<uint16_t>(std::min<int32_t>(player->getHealth(), std::numeric_limits<uint16_t>::max()));
	msg.Add<uint16_t>(std::min<int32_t>(player->getPlayerInfo(PLAYERINFO_MAXHEALTH), std::numeric_limits<uint16_t>::max()));

	msg.Add<uint32_t>(player->getFreeCapacity());
	//msg.Add<uint32_t>(player->getCapacity());

	uint64_t experience = player->getExperience();
	if(experience > 0x7FFFFFFF)
		msg.Add<uint32_t>(0x7FFFFFFF);
	else
		msg.Add<uint32_t>(experience);

	msg.Add<uint16_t>(player->getLevel());
	msg.AddByte(player->getPlayerInfo(PLAYERINFO_LEVELPERCENT));

	msg.Add<uint16_t>(std::min<int32_t>(player->getMana(), std::numeric_limits<uint16_t>::max()));
	msg.Add<uint16_t>(std::min<int32_t>(player->getPlayerInfo(PLAYERINFO_MAXMANA), std::numeric_limits<uint16_t>::max()));

	msg.AddByte(std::min<uint32_t>(player->getMagicLevel(), std::numeric_limits<uint8_t>::max()));
	msg.AddByte(player->getPlayerInfo(PLAYERINFO_MAGICLEVELPERCENT));

	msg.AddByte(player->getPlayerInfo(PLAYERINFO_SOUL));

	msg.Add<uint16_t>(player->getStaminaMinutes());

	/*msg.Add<uint16_t>(player->getBaseSpeed());

	Condition* condition = player->getCondition(CONDITION_REGENERATION);
	msg.Add<uint16_t>(condition ? condition->getTicks() / 1000 : 0x00);*/
}

void ProtocolGame::AddPlayerSkills(NetworkMessage& msg)
{
	msg.AddByte(0xA1);

	for (uint8_t i = SKILL_FIRST; i <= SKILL_LAST; ++i) {
		msg.AddByte(player->getSkillLevel(i));
		//msg.AddByte(player->getBaseSkill(i));
		msg.AddByte(player->getSkillPercent(i));
	}
}

void ProtocolGame::AddOutfit(NetworkMessage& msg, const Outfit_t& outfit)
{
	msg.Add<uint16_t>(outfit.lookType);

	if (outfit.lookType != 0) {
		msg.AddByte(outfit.lookHead);
		msg.AddByte(outfit.lookBody);
		msg.AddByte(outfit.lookLegs);
		msg.AddByte(outfit.lookFeet);
		msg.AddByte(outfit.lookAddons);
	} else {
		msg.AddItemId(outfit.lookTypeEx);
	}
}

void ProtocolGame::AddWorldLight(NetworkMessage& msg, const LightInfo& lightInfo)
{
	msg.AddByte(0x82);
	msg.AddByte((player->isAccessPlayer() ? 0xFF : lightInfo.level));
	msg.AddByte(lightInfo.color);
}

void ProtocolGame::AddCreatureLight(NetworkMessage& msg, const Creature* creature)
{
	LightInfo lightInfo;
	creature->getCreatureLight(lightInfo);

	msg.AddByte(0x8D);
	msg.Add<uint32_t>(creature->getID());
	msg.AddByte((player->isAccessPlayer() ? 0xFF : lightInfo.level));
	msg.AddByte(lightInfo.color);
}

//tile
void ProtocolGame::RemoveTileThing(NetworkMessage& msg, const Position& pos, uint32_t stackpos)
{
	if (stackpos >= 10) {
		return;
	}

	msg.AddByte(0x6C);
	msg.AddPosition(pos);
	msg.AddByte(stackpos);
}

void ProtocolGame::MoveUpCreature(NetworkMessage& msg, const Creature* creature, const Position& newPos, const Position& oldPos)
{
	if (creature != player) {
		return;
	}

	//floor change up
	msg.AddByte(0xBE);

	//going to surface
	if (newPos.z == 7) {
		int32_t skip = -1;
		GetFloorDescription(msg, oldPos.x - 8, oldPos.y - 6, 5, 18, 14, 3, skip); //(floor 7 and 6 already set)
		GetFloorDescription(msg, oldPos.x - 8, oldPos.y - 6, 4, 18, 14, 4, skip);
		GetFloorDescription(msg, oldPos.x - 8, oldPos.y - 6, 3, 18, 14, 5, skip);
		GetFloorDescription(msg, oldPos.x - 8, oldPos.y - 6, 2, 18, 14, 6, skip);
		GetFloorDescription(msg, oldPos.x - 8, oldPos.y - 6, 1, 18, 14, 7, skip);
		GetFloorDescription(msg, oldPos.x - 8, oldPos.y - 6, 0, 18, 14, 8, skip);

		if (skip >= 0) {
			msg.AddByte(skip);
			msg.AddByte(0xFF);
		}
	}
	//underground, going one floor up (still underground)
	else if (newPos.z > 7) {
		int32_t skip = -1;
		GetFloorDescription(msg, oldPos.x - 8, oldPos.y - 6, oldPos.getZ() - 3, 18, 14, 3, skip);

		if (skip >= 0) {
			msg.AddByte(skip);
			msg.AddByte(0xFF);
		}
	}

	//moving up a floor up makes us out of sync
	//west
	msg.AddByte(0x68);
	GetMapDescription(oldPos.x - 8, oldPos.y - 5, newPos.z, 1, 14, msg);

	//north
	msg.AddByte(0x65);
	GetMapDescription(oldPos.x - 8, oldPos.y - 6, newPos.z, 18, 1, msg);
}

void ProtocolGame::MoveDownCreature(NetworkMessage& msg, const Creature* creature, const Position& newPos, const Position& oldPos)
{
	if (creature != player) {
		return;
	}

	//floor change down
	msg.AddByte(0xBF);

	//going from surface to underground
	if (newPos.z == 8) {
		int32_t skip = -1;

		GetFloorDescription(msg, oldPos.x - 8, oldPos.y - 6, newPos.z, 18, 14, -1, skip);
		GetFloorDescription(msg, oldPos.x - 8, oldPos.y - 6, newPos.z + 1, 18, 14, -2, skip);
		GetFloorDescription(msg, oldPos.x - 8, oldPos.y - 6, newPos.z + 2, 18, 14, -3, skip);

		if (skip >= 0) {
			msg.AddByte(skip);
			msg.AddByte(0xFF);
		}
	}
	//going further down
	else if (newPos.z > oldPos.z && newPos.z > 8 && newPos.z < 14) {
		int32_t skip = -1;
		GetFloorDescription(msg, oldPos.x - 8, oldPos.y - 6, newPos.z + 2, 18, 14, -3, skip);

		if (skip >= 0) {
			msg.AddByte(skip);
			msg.AddByte(0xFF);
		}
	}

	//moving down a floor makes us out of sync
	//east
	msg.AddByte(0x66);
	GetMapDescription(oldPos.x + 9, oldPos.y - 7, newPos.z, 1, 14, msg);

	//south
	msg.AddByte(0x67);
	GetMapDescription(oldPos.x - 8, oldPos.y + 7, newPos.z, 18, 1, msg);
}

void ProtocolGame::AddShopItem(NetworkMessage& msg, const ShopInfo& item)
{
	const ItemType& it = Item::items[item.itemId];
	msg.Add<uint16_t>(it.clientId);

	if (it.isSplash() || it.isFluidContainer()) {
		msg.AddByte(serverFluidToClient(item.subType));
	} else {
		msg.AddByte(0x00);
	}

	msg.AddString(item.realName);
	msg.Add<uint32_t>(it.weight);
	msg.Add<uint32_t>(item.buyPrice);
	msg.Add<uint32_t>(item.sellPrice);
}

void ProtocolGame::parseExtendedOpcode(NetworkMessage& msg)
{
	uint8_t opcode = msg.GetByte();
	const std::string& buffer = msg.GetString();

	// process additional opcodes via lua script event
	addGameTask(&Game::parsePlayerExtendedOpcode, player->getID(), opcode, buffer);
}
