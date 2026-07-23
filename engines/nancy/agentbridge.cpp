/* ScummVM - Graphic Adventure Engine
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#define FORBIDDEN_SYMBOL_ALLOW_ALL

#include "common/config-manager.h"
#include "common/base64.h"
#include "common/events.h"
#include "common/formats/json.h"
#include "common/memstream.h"
#include "common/system.h"
#include "common/textconsole.h"

#include "engines/metaengine.h"

#ifdef USE_PNG
#include "image/png.h"
#endif

#include "engines/nancy/agentbridge.h"
#include "engines/nancy/action/actionmanager.h"
#include "engines/nancy/action/actionrecord.h"
#include "engines/nancy/action/conversation.h"
#include "engines/nancy/action/secondarymovie.h"
#include "engines/nancy/cursor.h"
#include "engines/nancy/enginedata.h"
#include "engines/nancy/input.h"
#include "engines/nancy/nancy.h"
#include "engines/nancy/state/scene.h"
#include "engines/nancy/ui/conversationpopup.h"
#include "engines/nancy/ui/cellphonepopup.h"
#include "engines/nancy/ui/inventorypopup.h"
#include "engines/nancy/ui/notebookpopup.h"
#include "engines/nancy/ui/taskbar.h"
#include "engines/nancy/ui/viewport.h"

#ifdef USE_ENET
#include "backends/networking/enet/enet.h"
#include "backends/networking/enet/source/enet.h"
#endif

namespace Nancy {
namespace Agent {

static const uint32 kMaximumMessageSize = 1024 * 1024;

static Common::JSONValue *jsonInteger(int64 value) {
	return new Common::JSONValue((long long int)value);
}

static Common::JSONValue *jsonRect(const Common::Rect &rect) {
	Common::JSONArray values;
	values.push_back(jsonInteger(rect.left));
	values.push_back(jsonInteger(rect.top));
	values.push_back(jsonInteger(rect.right));
	values.push_back(jsonInteger(rect.bottom));
	return new Common::JSONValue(values);
}

static Common::String cursorName(CursorManager::CursorType cursor) {
	switch (cursor) {
	case CursorManager::kHotspotTalk: return "talk";
	case CursorManager::kMoveLeft: return "move_left";
	case CursorManager::kMoveRight: return "move_right";
	case CursorManager::kMoveForward: return "move_forward";
	case CursorManager::kMoveBackward: return "move_backward";
	case CursorManager::kMoveUp: return "move_up";
	case CursorManager::kMoveDown: return "move_down";
	case CursorManager::kRotateCW: return "rotate_clockwise";
	case CursorManager::kRotateCCW: return "rotate_counterclockwise";
	case CursorManager::kExit: return "exit";
	case CursorManager::kHotspot:
	case CursorManager::kHotspotArrow: return "inspect";
	default: return Common::String::format("cursor_%d", (int)cursor);
	}
}

static Common::String jsonString(Common::JSONObject &object, const char *key) {
	Common::JSONValue *value = object.getValOrDefault(key, nullptr);
	return value && value->isString() ? value->asString() : Common::String();
}

static int jsonInt(Common::JSONObject &object, const char *key, int fallback) {
	Common::JSONValue *value = object.getValOrDefault(key, nullptr);
	if (!value)
		return fallback;
	if (value->isIntegerNumber())
		return (int)value->asIntegerNumber();
	if (value->isNumber())
		return (int)value->asNumber();
	return fallback;
}

class AgentTransport {
public:
	AgentTransport();
	~AgentTransport();

	bool start(uint16 port);
	void stop();
	void poll(Common::Array<Common::String> &lines);
	void sendLine(const Common::String &line);
	bool hasClient() const;

private:
#ifdef USE_ENET
	Networking::ENet::ENet *_network;
	ENetSocket _server;
	ENetSocket _client;
#endif
	Common::String _input;
	Common::String _output;
	bool _running;
};

AgentTransport::AgentTransport() :
#ifdef USE_ENET
		_network(nullptr), _server(ENET_SOCKET_NULL), _client(ENET_SOCKET_NULL),
#endif
		_running(false) {}

AgentTransport::~AgentTransport() {
	stop();
}

bool AgentTransport::start(uint16 port) {
#ifndef USE_ENET
	warning("Nancy agent bridge requires a build configured with ENet");
	return false;
#else
	_network = new Networking::ENet::ENet();
	if (!_network->initialize()) {
		stop();
		return false;
	}

	ENetAddress address;
	if (enet_address_set_host(&address, "127.0.0.1") < 0) {
		warning("Nancy agent bridge could not resolve localhost");
		stop();
		return false;
	}
	address.port = port;

	_server = enet_socket_create(ENET_SOCKET_TYPE_STREAM);
	if (_server == ENET_SOCKET_NULL ||
		enet_socket_set_option(_server, ENET_SOCKOPT_REUSEADDR, 1) < 0 ||
		enet_socket_bind(_server, &address) < 0 ||
		enet_socket_listen(_server, 1) < 0 ||
		enet_socket_set_option(_server, ENET_SOCKOPT_NONBLOCK, 1) < 0) {
		warning("Nancy agent bridge could not bind 127.0.0.1:%u", port);
		stop();
		return false;
	}

	_running = true;
	debug(1, "Nancy agent bridge listening on 127.0.0.1:%u", port);
	return true;
#endif
}

void AgentTransport::stop() {
#ifdef USE_ENET
	if (_client != ENET_SOCKET_NULL) {
		enet_socket_destroy(_client);
		_client = ENET_SOCKET_NULL;
	}
	if (_server != ENET_SOCKET_NULL) {
		enet_socket_destroy(_server);
		_server = ENET_SOCKET_NULL;
	}
	delete _network;
	_network = nullptr;
#endif
	_input.clear();
	_output.clear();
	_running = false;
}

bool AgentTransport::hasClient() const {
#ifdef USE_ENET
	return _client != ENET_SOCKET_NULL;
#else
	return false;
#endif
}

void AgentTransport::poll(Common::Array<Common::String> &lines) {
#ifdef USE_ENET
	if (!_running)
		return;

	ENetAddress peerAddress;
	ENetSocket accepted = enet_socket_accept(_server, &peerAddress);
	if (accepted != ENET_SOCKET_NULL) {
		if (_client == ENET_SOCKET_NULL) {
			_client = accepted;
			enet_socket_set_option(_client, ENET_SOCKOPT_NONBLOCK, 1);
			enet_socket_set_option(_client, ENET_SOCKOPT_NODELAY, 1);
		} else {
			enet_socket_destroy(accepted);
		}
	}

	if (_client == ENET_SOCKET_NULL)
		return;

	enet_uint32 receiveCondition = ENET_SOCKET_WAIT_RECEIVE;
	if (enet_socket_wait(_client, &receiveCondition, 0) < 0) {
		enet_socket_destroy(_client);
		_client = ENET_SOCKET_NULL;
		_input.clear();
		_output.clear();
		return;
	}
	if (receiveCondition & ENET_SOCKET_WAIT_RECEIVE) {
		char buffer[4096];
		ENetBuffer receiveBuffer;
		receiveBuffer.data = buffer;
		receiveBuffer.dataLength = sizeof(buffer);
		const int received = enet_socket_receive(_client, nullptr, &receiveBuffer, 1);
		if (received <= 0) {
			enet_socket_destroy(_client);
			_client = ENET_SOCKET_NULL;
			_input.clear();
			_output.clear();
			return;
		}
		_input += Common::String(buffer, received);
		if (_input.size() > kMaximumMessageSize) {
			enet_socket_destroy(_client);
			_client = ENET_SOCKET_NULL;
			_input.clear();
			_output.clear();
			return;
		}

		size_t newline = _input.find('\n');
		while (newline != Common::String::npos) {
			Common::String line = _input.substr(0, newline);
			if (!line.empty() && line.lastChar() == '\r')
				line.deleteLastChar();
			if (!line.empty())
				lines.push_back(line);
			_input = _input.substr(newline + 1);
			newline = _input.find('\n');
		}
	}

	if (!_output.empty()) {
		ENetBuffer sendBuffer;
		sendBuffer.data = const_cast<char *>(_output.c_str());
		sendBuffer.dataLength = _output.size();
		const int sent = enet_socket_send(_client, nullptr, &sendBuffer, 1);
		if (sent > 0)
			_output = _output.substr(sent);
	}
#else
	(void)lines;
#endif
}

void AgentTransport::sendLine(const Common::String &line) {
	if (!hasClient())
		return;
	_output += line;
	_output += '\n';
}

AgentBridge::AgentBridge() :
		_transport(new AgentTransport()), _tick(0), _clientWasConnected(false), _wasObservationPoint(false),
		_actionPending(false), _actionInputQueued(false), _actionWaitForScene(false),
		_actionWaitForPhoneTransition(false), _actionSawPhoneBusy(false), _actionIssuedTick(0),
		_settleFrames(0), _activationRecordIndex(-1), _activationPanRemaining(0) {}

AgentBridge::~AgentBridge() {
	stop();
	delete _transport;
}

bool AgentBridge::start(uint16 port) {
	return _transport->start(port);
}

void AgentBridge::stop() {
	_transport->stop();
}

bool AgentBridge::isRunning() const {
	return _transport->hasClient();
}

bool AgentBridge::StateDigest::operator==(const StateDigest &other) const {
	return sceneID == other.sceneID && frameID == other.frameID &&
		heldItem == other.heldItem && inventory == other.inventory;
}

bool AgentBridge::isStableDecisionPoint() const {
	if (!g_nancy)
		return false;
	if (g_nancy->getState() == NancyState::kMainMenu) {
		const MENU *menu = GetEngineData(MENU);
		return menu != nullptr;
	}
	if (g_nancy->getState() != NancyState::kScene || !State::Scene::hasInstance())
		return false;

	State::Scene &scene = State::Scene::instance();
	Action::PlaySecondaryMovie *activeMovie = scene.getActiveMovie();
	if (scene.getState() != State::Scene::kRun || scene.isUIPrepActive() ||
		scene.isRunningAd() || (activeMovie && activeMovie->isAgentBusy()) ||
		scene.getCellPhonePopup().isAgentBusy()) {
		return false;
	}

	for (Action::ActionRecord *record : scene.getActionManager().getActionRecords()) {
		if (record && record->_isActive && !record->_isDone && record->isAgentBusy())
			return false;
	}

	Action::ConversationSound *conversation = scene.getActiveConversation();
	return !conversation || conversation->isWaitingForResponse();
}

bool AgentBridge::isObservationPoint() const {
	return isStableDecisionPoint() || (g_nancy && g_nancy->getState() == NancyState::kCredits);
}

AgentBridge::StateDigest AgentBridge::buildDigest() const {
	StateDigest digest;
	if (!State::Scene::hasInstance())
		return digest;

	State::Scene &scene = State::Scene::instance();
	digest.sceneID = scene.getSceneInfo().sceneID;
	digest.frameID = scene.getSceneInfo().frameID;
	digest.heldItem = scene.getHeldItem();
	const INV *inventory = GetEngineData(INV);
	if (inventory) {
		for (uint i = 0; i < inventory->itemDescriptions.size(); ++i) {
			if (scene.hasItem(i) == g_nancy->_true)
				digest.inventory += Common::String::format("%u,", i);
		}
	}
	return digest;
}

Common::String AgentBridge::buildObservationJSON() {
	if (g_nancy->getState() == NancyState::kMainMenu)
		return buildMainMenuObservationJSON();
	if (g_nancy->getState() == NancyState::kCredits)
		return buildCreditsObservationJSON();

	State::Scene &scene = State::Scene::instance();
	const SceneChangeDescription &sceneInfo = scene.getSceneInfo();
	Action::ConversationSound *conversation = scene.getActiveConversation();
	Common::Array<Action::ActionRecord *> &records = scene.getActionManager().getActionRecords();
	UI::InventoryPopup &inventoryPopup = scene.getInventoryPopup();
	UI::NotebookPopup &notebookPopup = scene.getNotebookPopup();
	UI::CellPhonePopup &cellPhonePopup = scene.getCellPhonePopup();
	const bool popupOpen = inventoryPopup.isOpen() || notebookPopup.isOpen() || cellPhonePopup.isOpen();
	const char *mode = conversation ? "dialogue" : "exploration";
	if (inventoryPopup.isOpen())
		mode = "inventory";
	else if (notebookPopup.isOpen())
		mode = "notebook";
	else if (cellPhonePopup.isOpen())
		mode = "cell_phone";
	else if (!conversation) {
		for (Action::ActionRecord *record : records) {
			if (!record || !record->_isActive || record->_isDone)
				continue;
			Common::Array<AgentControl> controls;
			record->getAgentControls(controls);
			if (!controls.empty() || !record->getAgentState().empty() || record->getRecordTypeName().contains("Puzzle")) {
				mode = "puzzle";
				break;
			}
		}
	}

	Common::JSONObject root;
	root.setVal("type", new Common::JSONValue("observation"));
	root.setVal("protocol", new Common::JSONValue("nancy-agent-v1"));
	root.setVal("tick", jsonInteger(_tick));
	root.setVal("exposure", new Common::JSONValue("affordance"));
	root.setVal("mode", new Common::JSONValue(mode));
	root.setVal("settled", new Common::JSONValue(true));
	root.setVal("input_locked", new Common::JSONValue(false));

	Common::JSONObject sceneJSON;
	sceneJSON.setVal("id", jsonInteger(sceneInfo.sceneID));
	sceneJSON.setVal("description", new Common::JSONValue(scene.getSceneSummary().description));
	sceneJSON.setVal("pan_frame", jsonInteger(scene.getViewport().getCurFrame()));
	sceneJSON.setVal("pan_frames", jsonInteger(scene.getViewport().getFrameCount()));
	sceneJSON.setVal("panning_type", jsonInteger(scene.getSceneSummary().panningType));
	root.setVal("scene", new Common::JSONValue(sceneJSON));

	Common::JSONArray inventoryJSON;
	const INV *inventory = GetEngineData(INV);
	if (inventory) {
		for (uint i = 0; i < inventory->itemDescriptions.size(); ++i) {
			if (scene.hasItem(i) != g_nancy->_true)
				continue;
			Common::JSONObject item;
			item.setVal("id", jsonInteger(i));
			item.setVal("name", new Common::JSONValue(inventory->itemDescriptions[i].name));
			item.setVal("held", new Common::JSONValue(scene.getHeldItem() == (int16)i));
			inventoryJSON.push_back(new Common::JSONValue(item));
		}
	}
	root.setVal("inventory", new Common::JSONValue(inventoryJSON));
	root.setVal("held_item", jsonInteger(scene.getHeldItem()));
	// Difficulty is a player-selected, currently knowable setting rather than
	// hidden script state. Nancy 3+ uses 0 for Junior and 1 for Senior.
	root.setVal("difficulty", jsonInteger(scene.getDifficulty()));

	Common::JSONArray affordances;
	if (!popupOpen) for (uint index = 0; index < records.size(); ++index) {
		Action::ActionRecord *record = records[index];
		if (!record || !record->_isActive || record->_isDone)
			continue;

		if (record->_hasHotspot && record->_hotspot.isValidRect()) {
			const Common::Rect screenRect = scene.getViewport().convertViewportToScreen(record->_hotspot);
			Common::String description = record->_description;
			// Nancy 11's player challenge buttons are visibly labeled on the
			// rendered book, but their action records contain only generic names.
			// Preserve affordance-mode parity with what a human can read without
			// exposing the flags or scene changes behind the buttons.
			if (g_nancy->getGameType() == kGameTypeNancy11 && sceneInfo.sceneID == 0) {
				if (index == 32)
					description = "Gameplay Overview";
				else if (index == 34)
					description = "Junior Detective";
				else if (index == 35)
					description = "Senior Detective";
			}
			Common::JSONObject affordance;
			affordance.setVal("id", new Common::JSONValue(Common::String::format("hs_%u_%u", sceneInfo.sceneID, index)));
			affordance.setVal("kind", new Common::JSONValue(cursorName(record->getHoverCursor())));
			affordance.setVal("description", new Common::JSONValue(description));
			affordance.setVal("record_type", new Common::JSONValue(record->getRecordTypeName()));
			affordance.setVal("cursor", jsonInteger(record->getHoverCursor()));
			affordance.setVal("hotspot", jsonRect(record->_hotspot));
			affordance.setVal("screen_hotspot", jsonRect(screenRect));
			affordance.setVal("visible", new Common::JSONValue(!screenRect.isEmpty()));
			affordances.push_back(new Common::JSONValue(affordance));
		}

		Common::Array<AgentControl> controls;
		record->getAgentControls(controls);
		for (const AgentControl &control : controls) {
			const Common::Rect screenRect = scene.getViewport().convertViewportToScreen(control.hotspot);
			Common::JSONObject affordance;
			affordance.setVal("id", new Common::JSONValue(Common::String::format("control_%u_%u_%s",
				sceneInfo.sceneID, index, control.id.c_str())));
			affordance.setVal("kind", new Common::JSONValue("puzzle_control"));
			affordance.setVal("description", new Common::JSONValue(control.description));
			affordance.setVal("record_type", new Common::JSONValue(record->getRecordTypeName()));
			affordance.setVal("cursor", jsonInteger(CursorManager::kHotspot));
			affordance.setVal("hotspot", jsonRect(control.hotspot));
			affordance.setVal("screen_hotspot", jsonRect(screenRect));
			affordance.setVal("visible", new Common::JSONValue(!screenRect.isEmpty()));
			affordances.push_back(new Common::JSONValue(affordance));
		}
	}

	Common::Array<AgentControl> uiControls;
	Common::String uiOwner;
	Common::String uiState;
	Common::Array<Common::String> uiText;
	if (inventoryPopup.isOpen()) {
		uiOwner = "inventory";
		inventoryPopup.getAgentControls(uiControls);
	} else if (notebookPopup.isOpen()) {
		uiOwner = "notebook";
		uiState = notebookPopup.getAgentState();
		notebookPopup.getAgentControls(uiControls);
		uiText = notebookPopup.getTextLines();
	} else if (cellPhonePopup.isOpen()) {
		uiOwner = "cell_phone";
		uiState = cellPhonePopup.getAgentState();
		cellPhonePopup.getAgentControls(uiControls);
		cellPhonePopup.getAgentTextLines(uiText);
	} else if (scene.getTaskbar()) {
		uiOwner = "taskbar";
		scene.getTaskbar()->getAgentControls(uiControls);
	}
	for (const AgentControl &control : uiControls) {
		Common::JSONObject affordance;
		affordance.setVal("id", new Common::JSONValue(Common::String::format("ui_%s_%s", uiOwner.c_str(), control.id.c_str())));
		affordance.setVal("kind", new Common::JSONValue("ui_control"));
		affordance.setVal("description", new Common::JSONValue(control.description));
		affordance.setVal("record_type", new Common::JSONValue("PlayerUI"));
		affordance.setVal("cursor", jsonInteger(CursorManager::kHotspotArrow));
		affordance.setVal("hotspot", jsonRect(control.hotspot));
		affordance.setVal("screen_hotspot", jsonRect(control.hotspot));
		affordance.setVal("visible", new Common::JSONValue(!control.hotspot.isEmpty()));
		affordances.push_back(new Common::JSONValue(affordance));
	}
	root.setVal("affordances", new Common::JSONValue(affordances));

	Common::JSONObject ui;
	ui.setVal("type", new Common::JSONValue(uiOwner));
	if (!uiState.empty())
		ui.setVal("state", new Common::JSONValue(uiState));
	Common::JSONArray uiTextJSON;
	for (const Common::String &line : uiText)
		uiTextJSON.push_back(new Common::JSONValue(line));
	ui.setVal("text", new Common::JSONValue(uiTextJSON));
	root.setVal("ui", new Common::JSONValue(ui));

	Common::JSONArray puzzleStates;
	if (!popupOpen) for (uint index = 0; index < records.size(); ++index) {
		Action::ActionRecord *record = records[index];
		if (!record || !record->_isActive || record->_isDone)
			continue;
		const Common::String state = record->getAgentState();
		const Common::String recordType = record->getRecordTypeName();
		if (state.empty() && !recordType.contains("Puzzle"))
			continue;
		Common::JSONObject puzzle;
		puzzle.setVal("id", new Common::JSONValue(Common::String::format("record_%u_%u", sceneInfo.sceneID, index)));
		puzzle.setVal("record_type", new Common::JSONValue(recordType));
		if (!state.empty())
			puzzle.setVal("state", new Common::JSONValue(state));
		puzzleStates.push_back(new Common::JSONValue(puzzle));
	}
	root.setVal("puzzles", new Common::JSONValue(puzzleStates));

	Common::JSONObject dialogue;
	Common::JSONArray choices;
	if (conversation) {
		dialogue.setVal("caption", new Common::JSONValue(conversation->getCaptionText()));
		for (uint i = 0; i < conversation->getVisibleResponseCount(); ++i) {
			Common::JSONObject choice;
			choice.setVal("id", jsonInteger(i));
			choice.setVal("text", new Common::JSONValue(conversation->getVisibleResponseText(i)));
			choices.push_back(new Common::JSONValue(choice));
		}
	}
	dialogue.setVal("choices", new Common::JSONValue(choices));
	root.setVal("dialogue", new Common::JSONValue(dialogue));

	Common::JSONArray textbox;
	for (const Common::String &line : scene.getTextbox().getTextLines())
		textbox.push_back(new Common::JSONValue(line));
	root.setVal("textbox", new Common::JSONValue(textbox));
	root.setVal("screenshot_id", new Common::JSONValue(Common::String::format("frame-%llu",
		(unsigned long long)_tick)));

	Common::JSONValue value(root);
	return value.stringify();
}

Common::String AgentBridge::buildMainMenuObservationJSON() {
	Common::JSONObject root;
	root.setVal("type", new Common::JSONValue("observation"));
	root.setVal("protocol", new Common::JSONValue("nancy-agent-v1"));
	root.setVal("tick", jsonInteger(_tick));
	root.setVal("exposure", new Common::JSONValue("affordance"));
	root.setVal("mode", new Common::JSONValue("main_menu"));
	root.setVal("settled", new Common::JSONValue(true));
	root.setVal("input_locked", new Common::JSONValue(false));

	Common::JSONObject scene;
	scene.setVal("id", jsonInteger(-1));
	scene.setVal("description", new Common::JSONValue("Main menu"));
	scene.setVal("pan_frame", jsonInteger(0));
	scene.setVal("pan_frames", jsonInteger(0));
	scene.setVal("panning_type", jsonInteger(0));
	root.setVal("scene", new Common::JSONValue(scene));

	root.setVal("inventory", new Common::JSONValue(Common::JSONArray()));
	root.setVal("held_item", jsonInteger(-1));
	Common::JSONArray affordances;
	const MENU *menu = GetEngineData(MENU);
	if (menu && menu->_buttonDests.size() > 1) {
		Common::JSONObject affordance;
		affordance.setVal("id", new Common::JSONValue("main_menu:new_game"));
		affordance.setVal("kind", new Common::JSONValue("start"));
		affordance.setVal("description", new Common::JSONValue("Start a new game"));
		affordance.setVal("record_type", new Common::JSONValue("MainMenuButton"));
		affordance.setVal("cursor", jsonInteger(CursorManager::kHotspotArrow));
		affordance.setVal("hotspot", jsonRect(menu->_buttonDests[1]));
		affordance.setVal("screen_hotspot", jsonRect(menu->_buttonDests[1]));
		affordance.setVal("visible", new Common::JSONValue(true));
		affordances.push_back(new Common::JSONValue(affordance));
	}
	root.setVal("affordances", new Common::JSONValue(affordances));

	Common::JSONObject dialogue;
	dialogue.setVal("choices", new Common::JSONValue(Common::JSONArray()));
	root.setVal("dialogue", new Common::JSONValue(dialogue));
	root.setVal("textbox", new Common::JSONValue(Common::JSONArray()));
	root.setVal("screenshot_id", new Common::JSONValue(Common::String::format("frame-%llu",
		(unsigned long long)_tick)));

	Common::JSONValue value(root);
	return value.stringify();
}

Common::String AgentBridge::buildCreditsObservationJSON() {
	Common::JSONObject root;
	root.setVal("type", new Common::JSONValue("observation"));
	root.setVal("protocol", new Common::JSONValue("nancy-agent-v1"));
	root.setVal("tick", jsonInteger(_tick));
	root.setVal("exposure", new Common::JSONValue("affordance"));
	root.setVal("mode", new Common::JSONValue("credits"));
	root.setVal("settled", new Common::JSONValue(true));
	root.setVal("input_locked", new Common::JSONValue(true));
	root.setVal("terminal", new Common::JSONValue(true));

	Common::JSONObject scene;
	const StateDigest digest = buildDigest();
	const uint16 lastSceneID = _beforeAction.sceneID ? _beforeAction.sceneID : digest.sceneID;
	scene.setVal("id", jsonInteger(lastSceneID));
	scene.setVal("description", new Common::JSONValue("Credits"));
	scene.setVal("pan_frame", jsonInteger(0));
	scene.setVal("pan_frames", jsonInteger(0));
	scene.setVal("panning_type", jsonInteger(0));
	root.setVal("scene", new Common::JSONValue(scene));
	root.setVal("inventory", new Common::JSONValue(Common::JSONArray()));
	root.setVal("held_item", jsonInteger(-1));
	root.setVal("affordances", new Common::JSONValue(Common::JSONArray()));
	Common::JSONObject dialogue;
	dialogue.setVal("choices", new Common::JSONValue(Common::JSONArray()));
	root.setVal("dialogue", new Common::JSONValue(dialogue));
	root.setVal("textbox", new Common::JSONValue(Common::JSONArray()));
	root.setVal("screenshot_id", new Common::JSONValue(Common::String::format("frame-%llu",
		(unsigned long long)_tick)));

	Common::JSONValue value(root);
	return value.stringify();
}

void AgentBridge::publishObservation(bool force) {
	if (!isObservationPoint() || !_transport->hasClient())
		return;
	if (g_nancy->getState() == NancyState::kMainMenu) {
		const Common::String fingerprint = "main_menu:new_game";
		if (force || fingerprint != _lastObservationDigest) {
			_transport->sendLine(buildMainMenuObservationJSON());
			_lastObservationDigest = fingerprint;
		}
		return;
	}
	if (g_nancy->getState() == NancyState::kCredits) {
		const Common::String fingerprint = "terminal:credits";
		if (force || fingerprint != _lastObservationDigest) {
			if (fingerprint != _lastObservationDigest)
				sendEvent("credits_entered");
			_transport->sendLine(buildCreditsObservationJSON());
			_lastObservationDigest = fingerprint;
		}
		return;
	}
	const StateDigest digest = buildDigest();
	Common::String fingerprint = Common::String::format("%u:%u:%d:%s", digest.sceneID,
		digest.frameID, digest.heldItem, digest.inventory.c_str());
	Action::ConversationSound *conversation = State::Scene::instance().getActiveConversation();
	if (conversation) {
		fingerprint += conversation->getCaptionText();
		for (uint i = 0; i < conversation->getVisibleResponseCount(); ++i)
			fingerprint += conversation->getVisibleResponseText(i);
	}
	State::Scene &scene = State::Scene::instance();
	Common::Array<Action::ActionRecord *> &records = scene.getActionManager().getActionRecords();
	const bool popupOpen = scene.getInventoryPopup().isOpen() || scene.getNotebookPopup().isOpen() ||
		scene.getCellPhonePopup().isOpen();
	if (!popupOpen) for (uint i = 0; i < records.size(); ++i) {
		Action::ActionRecord *record = records[i];
		if (!record || !record->_isActive || record->_isDone)
			continue;
		if (record->_hasHotspot)
			fingerprint += Common::String::format("|%u:%d:%s", i, (int)record->getHoverCursor(), record->_description.c_str());
		fingerprint += record->getAgentState();
		Common::Array<AgentControl> controls;
		record->getAgentControls(controls);
		for (const AgentControl &control : controls)
			fingerprint += Common::String::format("|%u:%s:%s", i, control.id.c_str(), control.description.c_str());
	}
	Common::Array<AgentControl> uiControls;
	if (scene.getInventoryPopup().isOpen()) {
		fingerprint += "|ui:inventory";
		scene.getInventoryPopup().getAgentControls(uiControls);
	} else if (scene.getNotebookPopup().isOpen()) {
		fingerprint += "|ui:notebook:" + scene.getNotebookPopup().getAgentState();
		scene.getNotebookPopup().getAgentControls(uiControls);
		for (const Common::String &line : scene.getNotebookPopup().getTextLines())
			fingerprint += line;
	} else if (scene.getCellPhonePopup().isOpen()) {
		fingerprint += "|ui:cell_phone:" + scene.getCellPhonePopup().getAgentState();
		scene.getCellPhonePopup().getAgentControls(uiControls);
		Common::Array<Common::String> lines;
		scene.getCellPhonePopup().getAgentTextLines(lines);
		for (const Common::String &line : lines)
			fingerprint += line;
	} else if (scene.getTaskbar()) {
		fingerprint += "|ui:taskbar";
		scene.getTaskbar()->getAgentControls(uiControls);
	}
	for (const AgentControl &control : uiControls)
		fingerprint += Common::String::format("|ui:%s:%s", control.id.c_str(), control.description.c_str());
	for (const Common::String &line : scene.getTextbox().getTextLines())
		fingerprint += line;
	if (force || fingerprint != _lastObservationDigest) {
		_transport->sendLine(buildObservationJSON());
		_lastObservationDigest = fingerprint;
	}
}

void AgentBridge::sendScreenshot(const Common::String &requestID) {
#ifndef USE_PNG
	sendError(requestID, "png_unavailable", "This ScummVM build has no PNG encoder");
#else
	Graphics::Surface *screen = g_system->lockScreen();
	if (!screen) {
		sendError(requestID, "screenshot_failed", "Could not lock the current screen");
		return;
	}

	Common::MemoryWriteStreamDynamic png(DisposeAfterUse::YES);
	const bool encoded = Image::writePNG(png, *screen);
	g_system->unlockScreen();
	if (!encoded) {
		sendError(requestID, "screenshot_failed", "Could not encode the current screen");
		return;
	}

	Common::JSONObject root;
	root.setVal("type", new Common::JSONValue("screenshot"));
	root.setVal("request_id", new Common::JSONValue(requestID));
	root.setVal("screenshot_id", new Common::JSONValue(Common::String::format("frame-%llu",
		(unsigned long long)_tick)));
	root.setVal("mime_type", new Common::JSONValue("image/png"));
	root.setVal("data", new Common::JSONValue(Common::b64EncodeData(png.getData(), png.size())));
	Common::JSONValue value(root);
	_transport->sendLine(value.stringify());
#endif
}

void AgentBridge::sendError(const Common::String &requestID, const Common::String &code,
		const Common::String &message) {
	Common::JSONObject root;
	root.setVal("type", new Common::JSONValue("error"));
	root.setVal("request_id", new Common::JSONValue(requestID));
	root.setVal("code", new Common::JSONValue(code));
	root.setVal("message", new Common::JSONValue(message));
	Common::JSONValue value(root);
	_transport->sendLine(value.stringify());
}

void AgentBridge::sendEvent(const Common::String &eventType, const Common::String &detail) {
	Common::JSONObject root;
	root.setVal("type", new Common::JSONValue("event"));
	root.setVal("tick", jsonInteger(_tick));
	root.setVal("event", new Common::JSONValue(eventType));
	if (!detail.empty())
		root.setVal("detail", new Common::JSONValue(detail));
	Common::JSONValue value(root);
	_transport->sendLine(value.stringify());
}

void AgentBridge::handleLine(const Common::String &line) {
	Common::JSONValue *value = Common::JSON::parse(line);
	if (!value || !value->isObject()) {
		delete value;
		sendError(Common::String(), "invalid_json", "Expected one JSON object per line");
		return;
	}

	Common::JSONObject object = value->asObject();
	const Common::String type = jsonString(object, "type");
	if (type == "observe") {
		if (!isObservationPoint())
			sendError(jsonString(object, "request_id"), "not_settled", "The engine is not at a decision point");
		else
			publishObservation(true);
	} else if (type == "screenshot") {
		if (!isObservationPoint())
			sendError(jsonString(object, "request_id"), "not_settled", "The engine is not at a decision point");
		else
			sendScreenshot(jsonString(object, "request_id"));
	} else if (type == "act" || object.contains("action")) {
		handleAction(value);
		value = nullptr;
	} else {
		sendError(jsonString(object, "request_id"), "unknown_message", "Expected observe or act");
	}
	delete value;
}

bool AgentBridge::queueActivation(uint recordIndex) {
	State::Scene &scene = State::Scene::instance();
	Common::Array<Action::ActionRecord *> &records = scene.getActionManager().getActionRecords();
	if (recordIndex >= records.size())
		return false;
	Action::ActionRecord *record = records[recordIndex];
	if (!record || !record->_isActive || record->_isDone || !record->_hasHotspot || !record->_hotspot.isValidRect())
		return false;

	const Common::Rect hotspot = scene.getViewport().convertViewportToScreen(record->_hotspot);
	if (hotspot.isEmpty())
		return false;

	NancyInput input;
	input.mousePos = Common::Point(hotspot.left + hotspot.width() / 2,
		hotspot.top + hotspot.height() / 2);
	input.input = NancyInput::kLeftMouseButtonUp;
	g_nancy->_input->queueSyntheticInput(input);
	return true;
}

bool AgentBridge::queueAgentControlActivation(uint recordIndex, const Common::String &controlID) {
	State::Scene &scene = State::Scene::instance();
	Common::Array<Action::ActionRecord *> &records = scene.getActionManager().getActionRecords();
	if (recordIndex >= records.size())
		return false;
	Action::ActionRecord *record = records[recordIndex];
	if (!record || !record->_isActive || record->_isDone)
		return false;

	Common::Array<AgentControl> controls;
	record->getAgentControls(controls);
	for (const AgentControl &control : controls) {
		if (control.id != controlID || !control.hotspot.isValidRect())
			continue;
		const Common::Rect hotspot = scene.getViewport().convertViewportToScreen(control.hotspot);
		if (hotspot.isEmpty())
			return false;
		NancyInput input;
		input.mousePos = Common::Point(hotspot.left + hotspot.width() / 2,
			hotspot.top + hotspot.height() / 2);
		input.input = NancyInput::kLeftMouseButtonUp;
		g_nancy->_input->queueSyntheticInput(input);
		return true;
	}
	return false;
}

bool AgentBridge::queueUIActivation(const Common::String &target) {
	State::Scene &scene = State::Scene::instance();
	Common::Array<AgentControl> controls;
	Common::String owner;
	if (scene.getInventoryPopup().isOpen()) {
		owner = "inventory";
		scene.getInventoryPopup().getAgentControls(controls);
	} else if (scene.getNotebookPopup().isOpen()) {
		owner = "notebook";
		scene.getNotebookPopup().getAgentControls(controls);
	} else if (scene.getCellPhonePopup().isOpen()) {
		owner = "cell_phone";
		scene.getCellPhonePopup().getAgentControls(controls);
	} else if (scene.getTaskbar()) {
		owner = "taskbar";
		scene.getTaskbar()->getAgentControls(controls);
	}

	for (const AgentControl &control : controls) {
		if (target != Common::String::format("ui_%s_%s", owner.c_str(), control.id.c_str()) ||
				!control.hotspot.isValidRect())
			continue;
		NancyInput input;
		input.mousePos = Common::Point(control.hotspot.left + control.hotspot.width() / 2,
			control.hotspot.top + control.hotspot.height() / 2);
		input.input = NancyInput::kLeftMouseButtonUp;
		g_nancy->_input->queueSyntheticInput(input);
		return true;
	}
	return false;
}

void AgentBridge::handleAction(Common::JSONValue *rootValue) {
	Common::JSONObject object = rootValue->asObject();
	const Common::String requestID = jsonString(object, "request_id");
	if (_actionPending) {
		sendError(requestID, "action_in_progress", "Wait for action_completed before acting again");
		delete rootValue;
		return;
	}
	if (!isStableDecisionPoint()) {
		sendError(requestID, "not_settled", "The engine is not at a decision point");
		delete rootValue;
		return;
	}

	const Common::String action = jsonString(object, "action");
	_actionID = jsonString(object, "action_id");
	if (_actionID.empty())
		_actionID = requestID.empty() ? Common::String::format("a-%llu", (unsigned long long)_tick) : requestID;
	_beforeAction = buildDigest();
	_actionName = action;
	_actionInputQueued = true;
	_actionWaitForScene = false;
	_actionWaitForPhoneTransition = false;
	_actionSawPhoneBusy = false;
	_activationRecordIndex = -1;
	_activationPanRemaining = 0;
	if (g_nancy->getState() == NancyState::kMainMenu && action != "activate" && action != "load" && action != "wait") {
		sendError(requestID, "unavailable_action", "This action is unavailable at the main menu");
		delete rootValue;
		return;
	}

	if (action == "activate") {
		const Common::String target = jsonString(object, "target");
		if (g_nancy->getState() == NancyState::kMainMenu) {
			const MENU *menu = GetEngineData(MENU);
			if (target != "main_menu:new_game" || !menu || menu->_buttonDests.size() <= 1) {
				sendError(requestID, "invalid_target", "Target is not a current active affordance");
				delete rootValue;
				return;
			}
			const Common::Rect &hotspot = menu->_buttonDests[1];
			NancyInput input;
			input.mousePos = Common::Point(hotspot.left + hotspot.width() / 2,
				hotspot.top + hotspot.height() / 2);
			input.input = NancyInput::kLeftMouseButtonUp;
			g_nancy->_input->queueSyntheticInput(input);
			_actionWaitForScene = true;
		} else if (target.hasPrefix("ui_")) {
			if (!queueUIActivation(target)) {
				sendError(requestID, "invalid_target", "UI control is not currently visible and clickable");
				delete rootValue;
				return;
			}
			if (target == "ui_cell_phone_talk")
				_actionWaitForPhoneTransition = true;
		} else {
			State::Scene &scene = State::Scene::instance();
			Common::Array<Action::ActionRecord *> &records = scene.getActionManager().getActionRecords();
			int recordIndex = -1;
			Common::String controlID;
			for (uint i = 0; i < records.size(); ++i) {
				if (target == Common::String::format("hs_%u_%u", scene.getSceneInfo().sceneID, i)) {
					recordIndex = i;
					break;
				}
				if (!records[i] || !records[i]->_isActive || records[i]->_isDone)
					continue;
				Common::Array<AgentControl> controls;
				records[i]->getAgentControls(controls);
				for (const AgentControl &control : controls) {
					if (target == Common::String::format("control_%u_%u_%s",
						scene.getSceneInfo().sceneID, i, control.id.c_str())) {
						recordIndex = i;
						controlID = control.id;
						break;
					}
				}
				if (!controlID.empty())
					break;
			}
			if (recordIndex < 0 || !records[recordIndex] || !records[recordIndex]->_isActive || records[recordIndex]->_isDone ||
				(controlID.empty() && !records[recordIndex]->_hasHotspot)) {
				sendError(requestID, "invalid_target", "Target is not a current active affordance");
				delete rootValue;
				return;
			}
			if (!controlID.empty()) {
				if (!queueAgentControlActivation(recordIndex, controlID)) {
					sendError(requestID, "invalid_target", "Puzzle control is not currently visible and clickable");
					delete rootValue;
					return;
				}
			} else if (!queueActivation(recordIndex)) {
				_activationRecordIndex = recordIndex;
				_activationPanRemaining = scene.getViewport().getFrameCount();
				_actionInputQueued = false;
			}
		}
	} else if (action == "pan") {
		if (g_nancy->getState() != NancyState::kScene) {
			sendError(requestID, "unavailable_action", "This action is unavailable outside a scene");
			delete rootValue;
			return;
		}
		const Common::String direction = jsonString(object, "direction");
		NancyInput input;
		input.mousePos = g_nancy->getEventManager()->getMousePos();
		input.input = NancyInput::kMoveFastModifier;
		if (direction == "left")
			input.input |= NancyInput::kMoveLeft;
		else if (direction == "right")
			input.input |= NancyInput::kMoveRight;
		else {
			sendError(requestID, "invalid_direction", "Pan direction must be left or right");
			delete rootValue;
			return;
		}
		g_nancy->_input->queueSyntheticInput(input);
	} else if (action == "choose_response") {
		const int choice = jsonInt(object, "choice", -1);
		State::Scene &scene = State::Scene::instance();
		Action::ConversationSound *conversation = scene.getActiveConversation();
		Common::Point point;
		if (!conversation || choice < 0 || (uint)choice >= conversation->getVisibleResponseCount() ||
			!scene.getConversationPopup().prepareResponseClickPoint(choice, point)) {
			sendError(requestID, "invalid_choice", "Response is not currently visible and clickable");
			delete rootValue;
			return;
		}
		NancyInput input;
		input.mousePos = point;
		input.input = NancyInput::kLeftMouseButtonUp;
		g_nancy->_input->queueSyntheticInput(input);
	} else if (action == "set_held_item") {
		const int item = jsonInt(object, "item", -1);
		State::Scene &scene = State::Scene::instance();
		if (item != -1 && scene.hasItem(item) != g_nancy->_true) {
			sendError(requestID, "item_not_owned", "The requested item is not in inventory");
			delete rootValue;
			return;
		}
		if (item == -1) {
			scene.setNoHeldItem();
		} else if (scene.getHeldItem() != item) {
			scene.setNoHeldItem();
			scene.removeItemFromInventory(item, true);
		}
	} else if (action == "save") {
		const int slot = jsonInt(object, "slot", -1);
		const Common::String description = jsonString(object, "description");
		if (!g_nancy->canSaveGameStateCurrently() || slot < 0 || slot > g_nancy->getMetaEngine()->getMaximumSaveSlot() ||
			g_nancy->saveGameState(slot, description.empty() ? "AGENT CHECKPOINT" : description).getCode() != Common::kNoError) {
			sendError(requestID, "save_failed", "Could not write the requested save slot");
			delete rootValue;
			return;
		}
	} else if (action == "load") {
		const int slot = jsonInt(object, "slot", -1);
		const bool loadFromMainMenu = g_nancy->getState() == NancyState::kMainMenu;
		if (!g_nancy->canLoadGameStateCurrently() || slot < 0 || slot > g_nancy->getMetaEngine()->getMaximumSaveSlot() ||
			g_nancy->loadGameState(slot).getCode() != Common::kNoError) {
			sendError(requestID, "load_failed", "Could not load the requested save slot");
			delete rootValue;
			return;
		}
		_actionWaitForScene = loadFromMainMenu;
	} else if (action != "wait") {
		sendError(requestID, "unknown_action", "Unsupported action");
		delete rootValue;
		return;
	}

	_actionPending = true;
	_actionIssuedTick = _tick;
	_settleFrames = 0;

	Common::JSONObject ack;
	ack.setVal("type", new Common::JSONValue("action_accepted"));
	ack.setVal("action_id", new Common::JSONValue(_actionID));
	ack.setVal("action", new Common::JSONValue(action));
	Common::JSONValue ackValue(ack);
	_transport->sendLine(ackValue.stringify());
	delete rootValue;
}

void AgentBridge::advancePendingAction() {
	if (!_actionPending)
		return;
	if (_tick <= _actionIssuedTick)
		return;
	if (g_nancy->getState() == NancyState::kCredits) {
		completePendingAction(buildDigest(), true);
		return;
	}
	if (_actionWaitForScene) {
		if (g_nancy->getState() != NancyState::kScene)
			return;
		_actionWaitForScene = false;
	}
	if (_actionWaitForPhoneTransition) {
		const bool phoneBusy = State::Scene::hasInstance() &&
			State::Scene::instance().getCellPhonePopup().isAgentBusy();
		if (phoneBusy) {
			_actionSawPhoneBusy = true;
			_settleFrames = 0;
			return;
		}
		if (!_actionSawPhoneBusy) {
			// Allow a rejected/no-signal Talk click to settle normally after its
			// short DTMF sound if no call transition begins.
			if (_tick - _actionIssuedTick < 180)
				return;
		} else {
			_actionWaitForPhoneTransition = false;
		}
	}

	if (!_actionInputQueued) {
		if (!isStableDecisionPoint())
			return;
		if (queueActivation(_activationRecordIndex)) {
			_actionInputQueued = true;
			_actionIssuedTick = _tick;
		} else if (_activationPanRemaining > 0) {
			NancyInput input;
			input.mousePos = g_nancy->getEventManager()->getMousePos();
			input.input = NancyInput::kMoveLeft | NancyInput::kMoveFastModifier;
			g_nancy->_input->queueSyntheticInput(input);
			--_activationPanRemaining;
		} else {
			sendError(_actionID, "target_not_visible", "Hotspot did not become visible after a full panorama rotation");
			_actionPending = false;
		}
		return;
	}

	if (!isStableDecisionPoint()) {
		_settleFrames = 0;
		return;
	}

	const StateDigest digest = buildDigest();
	if (_settleFrames == 0 || !(digest == _settleCandidate)) {
		_settleCandidate = digest;
		_settleFrames = 1;
		return;
	}

	if (++_settleFrames >= 2)
		completePendingAction(digest);
}

void AgentBridge::completePendingAction(const StateDigest &after, bool terminal) {
	Common::JSONObject result;
	result.setVal("scene_changed", new Common::JSONValue(_beforeAction.sceneID != after.sceneID));
	result.setVal("pan_changed", new Common::JSONValue(_beforeAction.frameID != after.frameID));
	result.setVal("inventory_changed", new Common::JSONValue(_beforeAction.inventory != after.inventory));
	result.setVal("held_item_changed", new Common::JSONValue(_beforeAction.heldItem != after.heldItem));
	result.setVal("terminal", new Common::JSONValue(terminal));

	Common::JSONObject root;
	root.setVal("type", new Common::JSONValue("action_completed"));
	root.setVal("action_id", new Common::JSONValue(_actionID));
	root.setVal("action", new Common::JSONValue(_actionName));
	root.setVal("settled", new Common::JSONValue(true));
	root.setVal("result", new Common::JSONValue(result));
	Common::JSONValue value(root);
	_transport->sendLine(value.stringify());

	_actionPending = false;
	_actionInputQueued = false;
	_actionWaitForScene = false;
	_actionWaitForPhoneTransition = false;
	_actionSawPhoneBusy = false;
	_settleFrames = 0;
	publishObservation(true);
}

void AgentBridge::poll() {
	++_tick;
	Common::Array<Common::String> lines;
	_transport->poll(lines);

	const bool connected = _transport->hasClient();
	if (connected && !_clientWasConnected) {
		_lastObservationDigest.clear();
		Common::JSONObject hello;
		hello.setVal("type", new Common::JSONValue("hello"));
		hello.setVal("protocol", new Common::JSONValue("nancy-agent-v1"));
		hello.setVal("exposure", new Common::JSONValue("affordance"));
		Common::JSONValue value(hello);
		_transport->sendLine(value.stringify());
	}
	_clientWasConnected = connected;

	for (const Common::String &line : lines)
		handleLine(line);

	advancePendingAction();
	const bool observationPoint = isObservationPoint();
	if (!_actionPending && observationPoint)
		publishObservation(!_wasObservationPoint);
	_wasObservationPoint = observationPoint;
}

void AgentBridge::notifySceneChange(uint16 sceneID) {
	sendEvent("scene_change_requested", Common::String::format("scene:%u", sceneID));
}

void AgentBridge::notifySceneEntered(uint16 sceneID) {
	sendEvent("scene_entered", Common::String::format("scene:%u", sceneID));
}

void AgentBridge::notifyItemChanged(const char *eventType, int16 itemID) {
	const INV *inventory = GetEngineData(INV);
	Common::String detail = Common::String::format("item:%d", itemID);
	if (inventory && itemID >= 0 && (uint)itemID < inventory->itemDescriptions.size())
		detail = inventory->itemDescriptions[itemID].name;
	sendEvent(eventType, detail);
}

void AgentBridge::notifyEventFlagChanged() {
	// Affordance mode deliberately withholds flag identifiers and values.
	sendEvent("state_changed");
}

} // End of namespace Agent
} // End of namespace Nancy
