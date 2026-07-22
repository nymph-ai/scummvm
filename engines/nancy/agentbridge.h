/* ScummVM - Graphic Adventure Engine
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef NANCY_AGENTBRIDGE_H
#define NANCY_AGENTBRIDGE_H

#include "common/array.h"
#include "common/scummsys.h"
#include "common/str.h"

namespace Common {
class JSONValue;
}

namespace Nancy {
namespace Agent {

class AgentTransport;

class AgentBridge {
public:
	AgentBridge();
	~AgentBridge();

	bool start(uint16 port);
	void stop();
	void poll();
	bool isRunning() const;

	void notifySceneChange(uint16 sceneID);
	void notifySceneEntered(uint16 sceneID);
	void notifyItemChanged(const char *eventType, int16 itemID);
	void notifyEventFlagChanged();

private:
	struct StateDigest {
		uint16 sceneID = 0;
		uint16 frameID = 0;
		int16 heldItem = -1;
		Common::String inventory;

		bool operator==(const StateDigest &other) const;
	};

	bool isStableDecisionPoint() const;
	bool isObservationPoint() const;
	StateDigest buildDigest() const;
	Common::String buildObservationJSON();
	Common::String buildMainMenuObservationJSON();
	Common::String buildCreditsObservationJSON();
	void publishObservation(bool force = false);
	void sendScreenshot(const Common::String &requestID);
	void handleLine(const Common::String &line);
	void handleAction(Common::JSONValue *root);
	void advancePendingAction();
	bool queueActivation(uint recordIndex);
	bool queueAgentControlActivation(uint recordIndex, const Common::String &controlID);
	bool queueUIActivation(const Common::String &target);
	void completePendingAction(const StateDigest &after, bool terminal = false);
	void sendError(const Common::String &requestID, const Common::String &code,
		const Common::String &message);
	void sendEvent(const Common::String &eventType, const Common::String &detail = Common::String());

	AgentTransport *_transport;
	uint64 _tick;
	bool _clientWasConnected;
	bool _wasObservationPoint;
	Common::String _lastObservationDigest;

	bool _actionPending;
	bool _actionInputQueued;
	bool _actionWaitForScene;
	uint64 _actionIssuedTick;
	Common::String _actionID;
	Common::String _actionName;
	StateDigest _beforeAction;
	StateDigest _settleCandidate;
	uint _settleFrames;

	int _activationRecordIndex;
	uint _activationPanRemaining;
};

} // End of namespace Agent
} // End of namespace Nancy

#endif // NANCY_AGENTBRIDGE_H
