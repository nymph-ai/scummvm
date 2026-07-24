/* ScummVM - Graphic Adventure Engine
 *
 * ScummVM is the legal property of its developers, whose names
 * are too numerous to list here. Please refer to the COPYRIGHT
 * file distributed with this source distribution.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "engines/nancy/nancy.h"
#include "engines/nancy/util.h"
#include "engines/nancy/resource.h"
#include "engines/nancy/video.h"
#include "engines/nancy/input.h"

#include "engines/nancy/action/interactivevideo.h"
#include "engines/nancy/action/secondarymovie.h"
#include "engines/nancy/state/scene.h"

namespace Nancy {
namespace Action {

void InteractiveVideo::readData(Common::SeekableReadStream &stream) {
	Common::Path ivFilename;
	readFilename(stream, ivFilename);

	stream.skip(2);

	_flags.resize(15);
	_cursors.resize(5);

	for (uint i = 0; i < 15; ++i) {
		_flags[i].label = stream.readSint16LE();
		_flags[i].flag = stream.readSint16LE();
	}

	for (uint i = 0; i < 5; ++ i) {
		_cursors[i] = stream.readSint16LE();
	}

	Common::SeekableReadStream *ivFile = SearchMan.createReadStreamForMember(ivFilename.append(".iv"));
	assert(ivFile);

	readFilename(*ivFile, _videoName);

	// WORKAROUND: In Nancy 9, the Feeding Frenzy mini-game plays 6 videos (the whales that pop up)
	// with a normal arrow cursor. In such cases, the cursor manager reverts to the default arrow
	// cursor, if an item is held (which, in this case is a fish). We replace these cursors with a
	// normal one, which allows for the held item to be visible. Making this a workaround for that
	// scene, since a change in that part of the cursor code would require a full regression test in
	// all supported games. Fixes bug #16792.
	const uint16 sceneId = NancySceneState.getSceneInfo().sceneID;
	if (g_nancy->getGameType() == kGameTypeNancy9 && (sceneId == 2992 || sceneId == 2995 || sceneId == 2996)) {
		if (_videoName.toString().contains("WhaleFeed") && _cursors[0] == CursorManager::kNormalArrow) {
			_cursors[0] = CursorManager::kNormal;
		}
	}

	uint32 numFrames = ivFile->readUint32LE();
	_frames.resize(numFrames);
	for (uint i = 0; i < numFrames; ++i) {
		InteractiveFrame &frame = _frames[i];
		frame.frameID = ivFile->readUint16LE();
		uint16 numHotspots = ivFile->readUint16LE();
		frame.triggerOnNoHotspot = ivFile->readByte();
		frame.noHSFlagID = ivFile->readSint16LE();
		frame.noHSCursorID = ivFile->readSint16LE();

		frame.hotspots.resize(numHotspots);
		for (uint j = 0; j < numHotspots; ++j) {
			ivFile->skip(4);
			readRect(*ivFile, frame.hotspots[j].hotspot);
			frame.hotspots[j].flagID = ivFile->readSint16LE();
			frame.hotspots[j].cursorID = ivFile->readSint16LE();
		}
	}

	delete ivFile;
}

void InteractiveVideo::execute() {
	switch (_state) {
	case kBegin:
		_movieAR = NancySceneState.getActiveMovie();
		if (!_movieAR || _movieAR->_state == kRun) {
			_state = kRun;
		}

		break;
	case kRun:
		if (_movieAR->_state == kActionTrigger || _movieAR->getIsFinished()) {
			_state = kActionTrigger;
		}

		break;
	case kActionTrigger:
		finishExecution();
		break;
	}
}

void InteractiveVideo::handleInput(NancyInput &input) {
	if (_state != kRun) {
		return;
	}

	int curFrame = _movieAR->_decoder.getCurFrame();
	if (curFrame < 0) {
		return;
	}

	for (auto &frame : _frames) {
		if (frame.frameID == curFrame) {
			// Found data for the current video frame

			// First, look through the hotspots for the frame
			for (auto &hotspot : frame.hotspots) {
				if (NancySceneState.getViewport().convertViewportToScreen(hotspot.hotspot).contains(input.mousePos)) {
					// Mouse is in a hotspot, change cursor and set flag if clicked
					if (hotspot.cursorID >= 0 && _cursors[hotspot.cursorID] >= 0) {
						g_nancy->_cursor->setCursorType((CursorManager::CursorType)_cursors[hotspot.cursorID], true);
					}

					if (input.input & NancyInput::kLeftMouseButtonUp) {
						NancySceneState.setEventFlag(_flags[hotspot.flagID]);
					}

					return;
				}
			}

			// Mouse is not in a hotspot for the frame, check if we have a default action
			if (frame.triggerOnNoHotspot) {
				if (frame.noHSCursorID >= 0 && _cursors[frame.noHSCursorID] >= 0) {
					g_nancy->_cursor->setCursorType((CursorManager::CursorType)_cursors[frame.noHSCursorID], true);
				}

				if (input.input & NancyInput::kLeftMouseButtonUp) {
					NancySceneState.setEventFlag(_flags[frame.noHSFlagID]);
				}
			}

			return;
		}
	}
}

void InteractiveVideo::getAgentControls(Common::Array<AgentControl> &controls) const {
	if (_state != kRun || !_movieAR) {
		return;
	}

	const int curFrame = _movieAR->_decoder.getCurFrame();
	if (curFrame < 0) {
		return;
	}

	for (const InteractiveFrame &frame : _frames) {
		if (frame.frameID != curFrame) {
			continue;
		}

		const Common::String subject = _videoName.empty() ? _description : _videoName.toString();
		for (uint i = 0; i < frame.hotspots.size(); ++i) {
			const Common::String description = frame.hotspots.size() == 1
				? Common::String::format("interact with %s", subject.c_str())
				: Common::String::format("interact with %s region %u", subject.c_str(), i + 1);
			// Keep the id independent of the movie frame. Agent observations and
			// decisions are slower than animation, while activation asks us for
			// the current rectangle again immediately before synthesizing input.
			controls.push_back(AgentControl(
				Common::String::format("hotspot_%u", i),
				description,
				frame.hotspots[i].hotspot));
		}

		if (frame.triggerOnNoHotspot) {
			// The authored human action is a click outside every listed hotspot.
			// Pick a concrete point that is outside every authored rectangle so
			// the synthesized click follows the same default branch.
			const Common::Rect viewport = NancySceneState.getViewport().getBounds();
			Common::Point backgroundPoint;
			bool foundBackground = false;
			for (int16 y = viewport.top; y < viewport.bottom && !foundBackground; ++y) {
				for (int16 x = viewport.left; x < viewport.right; ++x) {
					bool insideHotspot = false;
					for (const InteractiveHotspot &hotspot : frame.hotspots) {
						if (hotspot.hotspot.contains(x, y)) {
							insideHotspot = true;
							break;
						}
					}
					if (!insideHotspot) {
						backgroundPoint = Common::Point(x, y);
						foundBackground = true;
						break;
					}
				}
			}
			if (foundBackground) {
				controls.push_back(AgentControl(
					"background",
					Common::String::format("interact with the background around %s", subject.c_str()),
					Common::Rect(backgroundPoint.x, backgroundPoint.y,
						backgroundPoint.x + 1, backgroundPoint.y + 1)));
			}
		}

		return;
	}
}

Common::String InteractiveVideo::getAgentState() const {
	if (_state != kRun || !_movieAR) {
		return Common::String();
	}

	const Common::String subject = _videoName.empty() ? _description : _videoName.toString();
	return Common::String::format("interactive video %s", subject.c_str());
}

} // End of namespace Action
} // End of namespace Nancy
