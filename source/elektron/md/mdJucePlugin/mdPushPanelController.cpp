#include "mdPushPanelController.h"

#include "mdController.h"
#include "mdPluginProcessor.h"

#include "jucePluginEditorLib/pluginProcessor.h"

#include "mdLib/mddevice.h"
#include "mdLib/mdmidiprotocol.h"

#include "synthLib/plugin.h"

#include "baseLib/logging.h"

#include "jucePluginLib/parameter.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <optional>

namespace mdJucePlugin
{
	namespace
	{
		// Ableton's published Push 2 MIDI interface, verified on Push 3 User Mode.
		constexpr int g_padBaseNote = 36;	// bottom-left pad of the 8x8 grid
		constexpr int g_padColumns = 8;
		constexpr int g_padTrigRows = 2;	// bottom two rows -> the 16 trigs

		constexpr int g_encoderBaseCc = 71;	// encoders 1-8, consecutive CC numbers
		constexpr int g_jogWheelCc = 70;		// right jog wheel -> track dial, relative
		constexpr int g_levelWheelCc = 79;	// left jog wheel -> Level, relative

		// Touch strip -> Level. It sends absolute 14-bit pitch bend on channel 1 and
		// a touch note on/off. The selected track's level isn't known here, so the
		// strip is used relatively: movement since touch-down becomes Level steps,
		// a full-length swipe covering the full 0-127 range.
		constexpr int g_stripTouchNote = 12;
		constexpr uint32_t g_stripIdleResetMs = 250;	// new gesture if no touch note arrives
		constexpr int g_stripBurstCap = 32;

		constexpr int g_controlChannel = 1;	// buttons and encoders (JUCE 1-based)

		constexpr int g_encoderBurstCap = 8;	// safety cap per incoming CC message

		// LED feedback, sent back to Push on its User Port as note-on (pads) and CC
		// (buttons) on channel 1. RGB pads/buttons take an index into Push's default
		// colour palette, white-only buttons a brightness.
		constexpr int g_colorOff = 0;
		constexpr int g_colorDim = 124;		// dark grey: playable but unlit
		constexpr int g_colorWhite = 122;
		constexpr int g_colorGreen = 126;
		constexpr int g_colorRed = 127;
		constexpr int g_whiteDim = 16;
		constexpr int g_whiteBright = 127;

		enum class ButtonLed : uint8_t { White, Rgb };

		// Momentary buttons (127 = down, 0 = up) mapped 1:1 onto MD panel buttons.
		// Touching the d-pad/jog wheel also sends notes 13/11, which fall below the pads.
		struct ButtonMapping
		{
			int cc;
			md::PanelControl control;
			ButtonLed led;
		};

		constexpr std::array<ButtonMapping, 19> g_buttons
		{{
			{49, md::PanelControl::Function, ButtonLed::White},					// Shift
			{48, md::PanelControl::SynthesisEffectsRouting, ButtonLed::White},	// Select
			{86, md::PanelControl::Record, ButtonLed::Rgb},						// Record
			{85, md::PanelControl::Play, ButtonLed::Rgb},						// Play
			{29, md::PanelControl::Stop, ButtonLed::White},						// Stop
			{28, md::PanelControl::Exit, ButtonLed::White},						// Master (left of the d-pad)
			{46, md::PanelControl::Up, ButtonLed::White},						// d-pad
			{47, md::PanelControl::Down, ButtonLed::White},
			{44, md::PanelControl::Left, ButtonLed::White},
			{45, md::PanelControl::Right, ButtonLed::White},
			{91, md::PanelControl::Enter, ButtonLed::White},					// d-pad centre
			{50, md::PanelControl::Kit, ButtonLed::White},						// Note (below the d-pad)
			{117, md::PanelControl::Scale, ButtonLed::White},					// Double Loop -> trig page
			{33, md::PanelControl::BankGroup, ButtonLed::White},					// Hot Swap
			{3, md::PanelControl::Tempo, ButtonLed::White},						// Tap Tempo
			{20, md::PanelControl::BankA, ButtonLed::Rgb},						// row below the display, 1-4
			{21, md::PanelControl::BankB, ButtonLed::Rgb},
			{22, md::PanelControl::BankC, ButtonLed::Rgb},
			{23, md::PanelControl::BankD, ButtonLed::Rgb},
		}};

		// Two-button shortcuts on their own Push buttons, held like the Editor's
		// chord labels: modifier first, released last. The FUNCTION ones are
		// panelAffordances::g_machinedrumShortcuts. showsTrigs: the mode's screen
		// uses the trig LEDs.
		constexpr int g_captureCc = 65;

		struct ChordMapping
		{
			int cc;
			md::PanelControl modifier;
			md::PanelControl control;
			bool showsTrigs;
		};

		constexpr std::array<ChordMapping, 5> g_machinedrumChords
		{{
			{60, md::PanelControl::Function, md::PanelControl::BankA, false},			// Mute
			{57, md::PanelControl::Function, md::PanelControl::BankB, true},			// Accent
			{116, md::PanelControl::Function, md::PanelControl::BankC, true},			// Quantize -> Swing
			{30, md::PanelControl::Function, md::PanelControl::PatternSong, false},	// Setup (gear) -> Global
			{g_captureCc, md::PanelControl::Record, md::PanelControl::Play, false},	// Capture -> live record
		}};

		constexpr int g_shiftCc = 49;
		constexpr int g_recordCc = 86;

		// Buttons that leave a trig-LED mode (Mute/Accent/Swing) on the MD.
		constexpr std::array<md::PanelControl, 5> g_trigModeExits
		{{
			md::PanelControl::Exit, md::PanelControl::Kit, md::PanelControl::Record,
			md::PanelControl::SynthesisEffectsRouting, md::PanelControl::Scale,
		}};

		// Row above the display, buttons 1-4: display-only trig page indicator.
		constexpr int g_pageLedBaseCc = 102;
		constexpr int g_pageCount = 4;

		// Two-row blocks with an empty row between them: trigs (rows 1-2), tracks
		// (rows 4-5), per-track mute toggles (rows 7-8).
		constexpr int g_trackRowBaseNote = g_padBaseNote + g_padColumns * 3;
		constexpr int g_muteRowBaseNote = g_trackRowBaseNote + g_padColumns * 3;

		constexpr int g_ledIntervalMs = 40;

		// A pattern picked with bank + trig blinks until the firmware reports the
		// switch, i.e. for as long as the MD's countdown runs.
		constexpr int g_patternPollEveryTicks = 200 / g_ledIntervalMs;
		constexpr int g_patternBlinkTicks = 240 / g_ledIntervalMs;
		constexpr int g_patternTimeoutTicks = 60000 / g_ledIntervalMs;
		constexpr int g_bankACc = 20;	// Bank A-D on consecutive CCs
		constexpr int g_reconnectEveryTicks = 1500 / g_ledIntervalMs;
		constexpr int g_ledFullRefreshEveryTicks = 1000 / g_ledIntervalMs;	// Push drops them on mode switches

		// Pad note for a zero-based trig index, the inverse of padControlForNote.
		int padNoteForTrig(const int _trigIndex)
		{
			const auto row = g_padTrigRows - 1 - _trigIndex / g_padColumns;
			return g_padBaseNote + row * g_padColumns + _trigIndex % g_padColumns;
		}

		// Two-row track blocks put tracks 1-8 on the upper row and 9-16 below, so
		// the grid reads top-down: mutes 1-8, 9-16, tracks 1-8, 9-16, trigs 1-8, 9-16.
		int padNoteForTrack(const int _trackIndex, const int _baseNote = g_trackRowBaseNote)
		{
			const auto row = 1 - _trackIndex / g_padColumns;
			return _baseNote + row * g_padColumns + _trackIndex % g_padColumns;
		}

		// Inverse of padNoteForTrack; nullopt for pads outside that two-row block.
		std::optional<int> trackForNote(const int _note, const int _baseNote = g_trackRowBaseNote)
		{
			const auto offset = _note - _baseNote;
			if(offset < 0 || offset >= g_padColumns * 2)
				return std::nullopt;
			return (1 - offset / g_padColumns) * g_padColumns + offset % g_padColumns;
		}

		// MM has six bicolour track LEDs (green/red bit pairs), as in Editor::updateLeds.
		bool monomachineTrackLedLit(const md::FrontPanel& _panel, const int _track)
		{
			constexpr struct { uint8_t greenBank, greenBit, redBank, redBit; } tracks[] =
			{
				{ 0x25, 0, 0x25, 1 }, { 0x25, 2, 0x25, 3 },
				{ 0x24, 0, 0x24, 1 }, { 0x24, 2, 0x24, 3 },
				{ 0x24, 4, 0x24, 5 }, { 0x24, 6, 0x24, 7 },
			};
			const auto& t = tracks[_track];
			const auto lit = [&](const uint8_t _bank, const uint8_t _bit)
			{
				return (_panel.getLedBankRaw(_bank) & (1u << _bit)) == 0;
			};
			return lit(t.greenBank, t.greenBit) || lit(t.redBank, t.redBit);
		}

		constexpr int g_monomachineTrackCount = 6;

		int stepLedColor(const md::FrontPanel::LedColor _color)
		{
			switch(_color)
			{
			case md::FrontPanel::LedColor::Green:	return g_colorGreen;
			case md::FrontPanel::LedColor::Red:		return g_colorRed;
			case md::FrontPanel::LedColor::Yellow:	return g_colorWhite;
			default:								return g_colorDim;
			}
		}

		std::optional<md::PanelControl> padControlForNote(const int _note)
		{
			const auto offset = _note - g_padBaseNote;
			if(offset < 0 || offset >= g_padColumns * g_padTrigRows)
				return std::nullopt;

			// Bottom row = Trigger9-16, row above = Trigger1-8 (reads top-down like
			// the sequencer), left to right within each row.
			const auto row = offset / g_padColumns;
			const auto col = offset % g_padColumns;
			const auto trigIndex = (g_padTrigRows - 1 - row) * g_padColumns + col;

			return static_cast<md::PanelControl>(static_cast<int>(md::PanelControl::Trigger1) + trigIndex);
		}

		const ChordMapping* chordForCc(const int _cc)
		{
			for(const auto& chord : g_machinedrumChords)
			{
				if(chord.cc == _cc)
					return &chord;
			}
			return nullptr;
		}

		std::optional<md::PanelControl> buttonControlForCc(const int _cc)
		{
			for(const auto& button : g_buttons)
			{
				if(button.cc == _cc)
					return button.control;
			}
			return std::nullopt;
		}
	}

	PushPanelController::PushPanelController(Controller& _controller) : m_controller(_controller)
	{
		m_sentLeds.fill(-1);
		tryConnect();
		startTimer(g_ledIntervalMs);
	}

	PushPanelController::~PushPanelController()
	{
		stopTimer();
		disconnect();
	}

	juce::String PushPanelController::getConnectedDeviceName() const
	{
		return m_midiInput != nullptr ? m_midiInput->getName() : juce::String();
	}

	void PushPanelController::timerCallback()
	{
		applyMuteToggles();

		if(m_midiOutput != nullptr)
		{
			if(++m_ledRefreshTicks >= g_ledFullRefreshEveryTicks)
			{
				m_ledRefreshTicks = 0;
				m_sentLeds.fill(-1);
			}
			updateLeds();
		}

		if(++m_reconnectTicks < g_reconnectEveryTicks)
			return;
		m_reconnectTicks = 0;

		if(m_midiInput != nullptr)
		{
			// Detect the device disappearing (unplugged, or Push switched back to
			// Live mode and out of User Mode on some platforms drops the port).
			const auto id = m_midiInput->getIdentifier();
			const auto devices = juce::MidiInput::getAvailableDevices();
			const auto stillPresent = std::any_of(devices.begin(), devices.end(),
				[&](const juce::MidiDeviceInfo& _d) { return _d.identifier == id; });
			if(!stillPresent)
				disconnect();
		}

		if(m_midiInput == nullptr)
			tryConnect();
	}

	void PushPanelController::tryConnect()
	{
		if(m_midiInput != nullptr)
			return;

		const auto devices = juce::MidiInput::getAvailableDevices();
		for(const auto& device : devices)
		{
			if(!device.name.containsIgnoreCase("Push 3") || !device.name.containsIgnoreCase("User"))
				continue;

			if(!m_deviceManager)
				m_deviceManager.reset(new juce::AudioDeviceManager());
			if(!m_deviceManager->isMidiInputDeviceEnabled(device.identifier))
				m_deviceManager->setMidiInputDeviceEnabled(device.identifier, true);

			m_midiInput = juce::MidiInput::openDevice(device.identifier, this);
			if(m_midiInput != nullptr)
			{
				m_midiInput->start();
				LOG("Push panel controller connected to " << device.name.toStdString());
			}
			break;
		}

		if(m_midiInput == nullptr)
			return;

		for(const auto& device : juce::MidiOutput::getAvailableDevices())
		{
			if(!device.name.containsIgnoreCase("Push 3") || !device.name.containsIgnoreCase("User"))
				continue;
			m_midiOutput = juce::MidiOutput::openDevice(device.identifier);
			m_sentLeds.fill(-1);

			// Start from a dark grid: pads outside the current layout (e.g. left lit
			// by an earlier session) are never touched again otherwise.
			for(int note = g_padBaseNote; note < g_padBaseNote + g_padColumns * 8; ++note)
				sendLed(false, note, g_colorOff);
			break;
		}
	}

	void PushPanelController::disconnect()
	{
		if(m_midiInput == nullptr)
			return;

		m_midiInput->stop();
		m_midiInput.reset();
		releaseAllHeld();

		if(m_midiOutput != nullptr)
		{
			clearLeds();
			m_midiOutput.reset();
		}
	}

	void PushPanelController::updateLeds()
	{
		auto publisher = m_controller.getProcessor().getPlugin().withDeviceLocked(
			[](synthLib::Device* const _device)
			{
				auto* const device = dynamic_cast<md::Device*>(_device);
				return device ? device->getFrontPanelPublisher()
					: std::shared_ptr<md::FrontPanelPublisher>{};
			});
		if(!publisher)
			return;

		// Whole-panel snapshots only: the transition stream is single-consumer and
		// belongs to the Editor. Blinks shorter than the poll interval are missed.
		const auto panel = publisher->readPublishedState().panel;
		const auto isMonomachine = m_controller.getModel() == md::MachineModel::Monomachine;

		// Outside grid record the MD's trig LEDs just echo the playing tracks, which
		// the track rows already show. The trig rows only mirror them while they mean
		// something: grid record (steps) or a held bank button (pattern selection).
		const auto bankHeld = std::any_of(g_buttons.begin(), g_buttons.end(), [&](const ButtonMapping& _b)
		{
			return _b.control >= md::PanelControl::BankGroup && _b.control <= md::PanelControl::BankD
				&& m_buttonDown[static_cast<size_t>(_b.cc)].load(std::memory_order_relaxed);
		});
		// Live recording's trig LEDs only echo playback (and the MD's Record LED
		// blinks meanwhile), so it is tracked from Capture until Stop/Record/Exit.
		const auto recordLed = panel.getModeLed(md::FrontPanel::ModeLed::Record);
		const auto liveRecording = m_liveRecording.load(std::memory_order_relaxed);
		const auto showTrigs = bankHeld || (recordLed && !liveRecording)
			|| m_trigModeCc.load(std::memory_order_relaxed) >= 0;

		updatePendingPattern(panel.getModeLed(md::FrontPanel::ModeLed::BankGroupEH));
		const auto pendingTrig = m_pendingPattern >= 0 ? m_pendingPattern % 16 : -1;
		const auto blinkOn = (m_pendingPatternTicks / g_patternBlinkTicks) % 2 == 0;

		for(int i = 0; i < 16; ++i)
		{
			auto color = g_colorDim;
			if(i == pendingTrig)
			{
				color = blinkOn ? g_colorRed : g_colorDim;
			}
			else if(showTrigs)
			{
				color = isMonomachine
					? stepLedColor(panel.getMonomachineStepLedColor(i))
					: (panel.getStepLed(i) ? g_colorRed : g_colorDim);
			}
			sendLed(false, padNoteForTrig(i), color);
		}

		// Mute rows show the plugin's own per-track Mute parameters, not LEDs.
		const auto trackCount = isMonomachine ? g_monomachineTrackCount : 16;
		for(int i = 0; i < 16; ++i)
		{
			auto color = g_colorOff;
			if(i < trackCount)
			{
				if(auto* const mute = m_controller.getParameter("Mute", static_cast<uint8_t>(i)))
					color = mute->getUnnormalizedValue() != 0 ? g_colorDim : g_colorRed;
			}
			sendLed(false, padNoteForTrack(i, g_muteRowBaseNote), color);
		}

		for(int i = 0; i < 16; ++i)
		{
			auto color = g_colorOff;
			if(!isMonomachine)
				color = panel.getDrumLed(i) ? g_colorRed : g_colorDim;
			else if(i < g_monomachineTrackCount)
				color = monomachineTrackLedLit(panel, i) ? g_colorRed : g_colorDim;
			sendLed(false, padNoteForTrack(i), color);
		}

		// Every mapped button glows dim and lights up while held; a few mirror MD state.
		for(const auto& button : g_buttons)
		{
			const auto held = m_buttonDown[static_cast<size_t>(button.cc)].load(std::memory_order_relaxed);
			auto lit = held;
			auto color = -1;

			if(button.control == md::PanelControl::Record && panel.getModeLed(md::FrontPanel::ModeLed::Record))
				color = g_colorRed;
			else if(button.control == md::PanelControl::BankGroup)
				lit = lit || panel.getModeLed(md::FrontPanel::ModeLed::BankGroupEH);

			if(color < 0)
			{
				color = button.led == ButtonLed::Rgb
					? (lit ? g_colorWhite : g_colorDim)
					: (lit ? g_whiteBright : g_whiteDim);
			}
			sendLed(true, button.cc, color);
		}

		if(!isMonomachine)
		{
			const auto trigMode = m_trigModeCc.load(std::memory_order_relaxed);
			for(const auto& chord : g_machinedrumChords)
			{
				const auto lit = chord.cc == trigMode
					|| m_buttonDown[static_cast<size_t>(chord.cc)].load(std::memory_order_relaxed);
				sendLed(true, chord.cc, lit ? g_whiteBright : g_whiteDim);
			}
		}

		const std::array<bool, g_pageCount> pages
		{{
			panel.getStatusLed(md::FrontPanel::StatusLed::Page1),
			panel.getStatusLed(md::FrontPanel::StatusLed::Page2),
			panel.getStatusLed(md::FrontPanel::StatusLed::Page3),
			panel.getModeLed(md::FrontPanel::ModeLed::Page4),
		}};
		for(int i = 0; i < g_pageCount; ++i)
			sendLed(true, g_pageLedBaseCc + i, pages[static_cast<size_t>(i)] ? g_colorWhite : g_colorOff);
	}

	void PushPanelController::clearLeds()
	{
		for(int i = 0; i < 16; ++i)
		{
			sendLed(false, padNoteForTrig(i), g_colorOff);
			sendLed(false, padNoteForTrack(i), g_colorOff);
			sendLed(false, padNoteForTrack(i, g_muteRowBaseNote), g_colorOff);
		}
		for(const auto& button : g_buttons)
			sendLed(true, button.cc, g_colorOff);
		for(const auto& chord : g_machinedrumChords)
			sendLed(true, chord.cc, g_colorOff);
		for(int i = 0; i < g_pageCount; ++i)
			sendLed(true, g_pageLedBaseCc + i, g_colorOff);
	}

	void PushPanelController::sendLed(const bool _isButton, const int _number, const int _color)
	{
		auto& sent = m_sentLeds[static_cast<size_t>((_isButton ? 128 : 0) + _number)];
		if(sent == _color || m_midiOutput == nullptr)
			return;
		sent = _color;

		const auto value = static_cast<juce::uint8>(_color);
		m_midiOutput->sendMessageNow(_isButton
			? juce::MidiMessage::controllerEvent(g_controlChannel, _number, value)
			: juce::MidiMessage::noteOn(g_controlChannel, _number, value));
	}

	void PushPanelController::handleIncomingMidiMessage(
		juce::MidiInput* /*_source*/, const juce::MidiMessage& _message)
	{
		if(_message.isNoteOnOrOff() && _message.getChannel() == g_controlChannel
			&& _message.getNoteNumber() == g_stripTouchNote)
		{
			m_stripPosition = -1;	// touch down or up both start a fresh gesture
			return;
		}

		if(_message.isPitchWheel())
		{
			// Pads bend on MPE member channels; only channel 1 is the strip.
			if(_message.getChannel() == g_controlChannel)
				handleTouchStrip(_message.getPitchWheelValue());
			return;
		}

		if(_message.isNoteOnOrOff())
		{
			const auto isOn = _message.isNoteOn() && _message.getVelocity() > 0;
			if(const auto track = trackForNote(_message.getNoteNumber()))
			{
				handleTrackPad(*track, isOn);
				return;
			}

			if(const auto track = trackForNote(_message.getNoteNumber(), g_muteRowBaseNote))
			{
				// Parameters belong to the message thread; the LED timer applies it.
				if(isOn)
					m_pendingMuteToggles.fetch_or(static_cast<uint16_t>(1u << *track), std::memory_order_relaxed);
				return;
			}

			const auto control = padControlForNote(_message.getNoteNumber());
			if(!control)
				return;

			// Bank A-D held + trig selects a pattern; remember it for the countdown.
			if(isOn)
			{
				for(int bank = 0; bank < 4; ++bank)
				{
					if(!m_buttonDown[static_cast<size_t>(g_bankACc + bank)].load(std::memory_order_relaxed))
						continue;
					const auto trig = static_cast<int>(*control) - static_cast<int>(md::PanelControl::Trigger1);
					m_patternRequest.store(bank * 16 + trig, std::memory_order_relaxed);
					break;
				}
			}

			if(isOn)
				press(*control);
			else
				release(*control);
			return;
		}

		// Push 3's User Mode pads are MPE: each held pad sends pressure, pitch
		// bend and CC74 (slide) on its own member channel (2-16). CC74 collides
		// with the encoder range, so only the buttons/encoders on channel 1 count.
		if(!_message.isController() || _message.getChannel() != g_controlChannel)
			return;

		const auto cc = _message.getControllerNumber();
		const auto value = _message.getControllerValue();

		if(const auto control = buttonControlForCc(cc))
		{
			m_buttonDown[static_cast<size_t>(cc)].store(value != 0, std::memory_order_relaxed);
			if(value != 0)
			{
				if(std::find(g_trigModeExits.begin(), g_trigModeExits.end(), *control) != g_trigModeExits.end())
					m_trigModeCc.store(-1, std::memory_order_relaxed);
				if(*control == md::PanelControl::Stop || *control == md::PanelControl::Record || *control == md::PanelControl::Exit)
					m_liveRecording.store(false, std::memory_order_relaxed);
				press(*control);
			}
			else
			{
				release(*control);
			}
			return;
		}

		if(m_controller.getModel() == md::MachineModel::Machinedrum)
		{
			if(const auto* chord = chordForCc(cc))
			{
				handleChord(chord->cc, chord->modifier, chord->control, chord->showsTrigs, value != 0);
				return;
			}
		}

		if(cc >= g_encoderBaseCc && cc < g_encoderBaseCc + 8)
		{
			sendEncoderSteps(static_cast<md::PanelEncoder>(
				static_cast<int>(md::PanelEncoder::DataEntryA) + (cc - g_encoderBaseCc)), relativeDelta(value), g_encoderBurstCap);
			return;
		}

		if(cc == g_jogWheelCc)
			sendEncoderSteps(md::PanelEncoder::SoundSelection, relativeDelta(value), g_encoderBurstCap);
		else if(cc == g_levelWheelCc)
			sendEncoderSteps(md::PanelEncoder::Level, relativeDelta(value), g_encoderBurstCap);
	}

	void PushPanelController::updatePendingPattern(const bool _extendedBanks)
	{
		const auto request = m_patternRequest.exchange(-1, std::memory_order_relaxed);
		if(request >= 0)
		{
			// Bank group E-H shifts A-D by four banks (64 patterns).
			m_pendingPattern = (_extendedBanks ? 64 : 0) + request;
			m_pendingPatternTicks = 0;
			m_controller.requestPatternStatus();
			return;
		}

		if(m_pendingPattern < 0)
			return;

		++m_pendingPatternTicks;
		if(m_controller.getPatternStatus() == m_pendingPattern || m_pendingPatternTicks > g_patternTimeoutTicks)
		{
			m_pendingPattern = -1;
			return;
		}

		if(m_pendingPatternTicks % g_patternPollEveryTicks == 0)
			m_controller.requestPatternStatus();
	}

	void PushPanelController::applyMuteToggles()
	{
		const auto toggles = m_pendingMuteToggles.exchange(0, std::memory_order_relaxed);
		for(int i = 0; i < 16; ++i)
		{
			if((toggles & (1u << i)) == 0)
				continue;
			auto* const mute = m_controller.getParameter("Mute", static_cast<uint8_t>(i));
			if(!mute)
				continue;
			mute->setUnnormalizedValueNotifyingHost(mute->getUnnormalizedValue() != 0 ? 0 : 1,
				pluginLib::Parameter::Origin::Ui);
		}
	}

	void PushPanelController::handleChord(const int _cc, const md::PanelControl _modifier, const md::PanelControl _control,
		const bool _showsTrigs, const bool _isDown)
	{
		m_buttonDown[static_cast<size_t>(_cc)].store(_isDown, std::memory_order_relaxed);

		if(_isDown)
		{
			// Pressing the same mode button again leaves it, like on the MD.
			const auto previous = m_trigModeCc.load(std::memory_order_relaxed);
			m_trigModeCc.store(_showsTrigs && previous != _cc ? _cc : -1, std::memory_order_relaxed);

			if(_cc == g_captureCc)
				m_liveRecording.store(true, std::memory_order_relaxed);

			press(_modifier);
			press(_control);
			return;
		}

		release(_control);
		// A physically held Shift/Record keeps its modifier down for further combinations.
		const auto modifierCc = _modifier == md::PanelControl::Function ? g_shiftCc : g_recordCc;
		if(!m_buttonDown[static_cast<size_t>(modifierCc)].load(std::memory_order_relaxed))
			release(_modifier);
	}

	void PushPanelController::handleTrackPad(const int _track, const bool _isOn)
	{
		if(m_controller.getModel() == md::MachineModel::Monomachine)
		{
			// MM has dedicated track buttons; hold them like any other panel button.
			if(_track >= g_monomachineTrackCount)
				return;
			const auto control = static_cast<md::PanelControl>(static_cast<int>(md::PanelControl::Track1) + _track);
			if(_isOn)
				press(control);
			else
				release(control);
			return;
		}

		if(!_isOn)
			return;

		// Same SysEx the Editor's clickable track labels send (Editor::selectMachinedrumTrack).
		// Plugin::addMidiEvent is locked, so this is safe from the MIDI input thread.
		const auto body = md::midiProtocol::selectTrack(_track);
		synthLib::SMidiEvent event(synthLib::MidiEventSource::Editor);
		event.sysex.reserve(body.size() + 2);
		event.sysex.push_back(0xf0);
		event.sysex.insert(event.sysex.end(), body.begin(), body.end());
		event.sysex.push_back(0xf7);
		m_controller.getProcessor().getPlugin().addMidiEvent(event);
	}

	void PushPanelController::handleTouchStrip(const int _pitchWheelValue)
	{
		const auto position = _pitchWheelValue >> 7;	// 0..127
		const auto now = juce::Time::getMillisecondCounter();

		if(m_stripPosition < 0 || now - m_stripLastMs > g_stripIdleResetMs)
		{
			m_stripPosition = position;
			m_stripLastMs = now;
			return;
		}

		m_stripLastMs = now;
		const auto delta = position - m_stripPosition;
		if(delta == 0)
			return;

		m_stripPosition = position;
		sendEncoderSteps(md::PanelEncoder::Level, delta, g_stripBurstCap);
	}

	int PushPanelController::relativeDelta(const int _ccValue)
	{
		// Relative (2's complement) encoding: 1..63 = positive steps, 65..127 =
		// negative steps, matching Live's "Relative (lin 2's Comp.)" mapping mode.
		return _ccValue < 64 ? _ccValue : _ccValue - 128;
	}

	void PushPanelController::sendEncoderSteps(const md::PanelEncoder _encoder, const int _delta, const int _burstCap)
	{
		if(_delta == 0)
			return;

		const auto command = md::panelEncoderCommand(m_controller.getModel(), _encoder);
		if(!command)
			return;

		const auto argument = static_cast<uint8_t>(_delta > 0 ? 0x01 : 0xff);
		const auto steps = std::min(std::abs(_delta), _burstCap);
		for(int i = 0; i < steps; ++i)
			sendPanelEvent(*command, argument);
	}

	bool PushPanelController::press(const md::PanelControl _control)
	{
		// A repeated press without a release in between (lost message) must not
		// add a second hold that a single release could never clear.
		if(std::find(m_heldControls.begin(), m_heldControls.end(), _control) != m_heldControls.end())
			return false;

		const auto packet = md::panelPacket(m_controller.getModel(), _control);
		if(!packet)
			return false;

		m_heldControls.push_back(_control);

		const auto combined = m_panelRows.press(*packet);
		return sendPanelEvent(combined.row, combined.mask);
	}

	bool PushPanelController::release(const md::PanelControl _control)
	{
		const auto it = std::find(m_heldControls.begin(), m_heldControls.end(), _control);
		if(it == m_heldControls.end())
			return false;
		m_heldControls.erase(it);

		const auto packet = md::panelPacket(m_controller.getModel(), _control);
		if(!packet)
			return false;

		const auto combined = m_panelRows.release(*packet);
		return sendPanelEvent(combined.row, combined.mask);
	}

	void PushPanelController::releaseAllHeld()
	{
		while(!m_heldControls.empty())
			release(m_heldControls.back());

		// Defensive final sweep in case a row bit survived the above.
		for(uint8_t row = 0x20; row <= 0x26; ++row)
		{
			if(m_panelRows.mask(row) != 0)
				sendPanelEvent(row, 0);
		}
		m_panelRows.reset();
	}

	bool PushPanelController::sendPanelEvent(const uint8_t _command, const uint8_t _argument)
	{
		auto& plugin = m_controller.getProcessor().getPlugin();
		auto& diagnostics = plugin.getRealtimeInstrumentation();
		const auto model = static_cast<uint32_t>(m_controller.getModel());
		const auto token = diagnostics.beginPanelInput(model, _command, _argument);
		const auto accepted = plugin.withDeviceLocked(
			[&](synthLib::Device* const _device)
			{
				auto* const device = dynamic_cast<md::Device*>(_device);
				if(!device)
					return false;
				return device->sendPanelEvent(_command, _argument);
			});
		diagnostics.endPanelInput(token, model, _command, _argument, accepted);
		return accepted;
	}
}
