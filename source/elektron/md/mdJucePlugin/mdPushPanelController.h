#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <vector>

#include "juce_audio_devices/juce_audio_devices.h"
#include "juce_events/juce_events.h"

#include "mdLib/mdpanel.h"

namespace mdJucePlugin
{
	class Controller;

	// Drives the emulated front panel (trigs, Function, Record, arrows, Enter/Exit,
	// Synthesis/Effects/Routing, encoders A-H, track dial) from an
	// Ableton Push 3 connected in Push's own User Mode.
	//
	// This is independent of the plugin's Editor/UI (it talks to the device the
	// same way Editor::sendPanelEvent does, via Controller -> Plugin -> Device)
	// and independent of the DAW's MIDI routing: it opens Push's dedicated
	// "Ableton Push 3 (User Port)" MIDI input directly. Ableton Live keeps its
	// own separate connection to Push's "Live Port" the whole time, so pressing
	// Push's own User button is the entire "switch to Machinedrum" gesture -
	// nothing needs to be re-routed in either app.
	//
	// Pad/encoder note and CC numbers (in mdPushPanelController.cpp) follow
	// Ableton's published Push 2 MIDI interface; verified against a Push 3 in
	// User Mode (pads 36+, Shift CC49 and encoders CC71-78 on channel 1, pads
	// as MPE on member channels).
	class PushPanelController final : private juce::MidiInputCallback, private juce::Timer
	{
	public:
		explicit PushPanelController(Controller& _controller);
		~PushPanelController() override;

		PushPanelController(const PushPanelController&) = delete;
		PushPanelController(PushPanelController&&) = delete;
		PushPanelController& operator=(const PushPanelController&) = delete;
		PushPanelController& operator=(PushPanelController&&) = delete;

		bool isConnected() const { return m_midiInput != nullptr; }
		juce::String getConnectedDeviceName() const;

	private:
		// juce::MidiInputCallback
		void handleIncomingMidiMessage(juce::MidiInput* _source, const juce::MidiMessage& _message) override;

		// juce::Timer - refreshes the Push LEDs and periodically retries the
		// connection while Push is absent (not yet plugged in).
		void timerCallback() override;

		void tryConnect();
		void disconnect();

		// Mirrors the MD's trig/track/bank LEDs onto Push's pads and buttons.
		void updateLeds();
		void clearLeds();
		void sendLed(bool _isButton, int _number, int _color);

		// _modifier + _control held while the Push button is down.
		void applyMuteToggles();
		void updatePendingPattern(bool _extendedBanks);
		void handleChord(int _cc, md::PanelControl _modifier, md::PanelControl _control, bool _showsTrigs, bool _isDown);
		void handleTrackPad(int _track, bool _isOn);
		void handleTouchStrip(int _pitchWheelValue);
		static int relativeDelta(int _ccValue);
		void sendEncoderSteps(md::PanelEncoder _encoder, int _delta, int _burstCap);

		bool press(md::PanelControl _control);
		bool release(md::PanelControl _control);
		void releaseAllHeld();
		bool sendPanelEvent(uint8_t _command, uint8_t _argument);

		Controller& m_controller;
		std::unique_ptr<juce::AudioDeviceManager> m_deviceManager;
		std::unique_ptr<juce::MidiInput> m_midiInput;
		std::unique_ptr<juce::MidiOutput> m_midiOutput;

		// Last colour sent per pad note (0-127) and button CC (128-255), -1 = unknown.
		std::array<int, 256> m_sentLeds{};
		int m_ledRefreshTicks = 0;

		// Written on the MIDI thread, read by the LED timer on the message thread.
		std::array<std::atomic<bool>, 128> m_buttonDown{};

		// CC of the Mute/Accent/Swing button whose trig-LED mode is active, -1 = none.
		// Tracked here because those MD screens can't be recognised from the LEDs.
		std::atomic<int> m_trigModeCc{-1};

		// Set by Capture, cleared by Stop/Record/Exit.
		std::atomic<bool> m_liveRecording{false};

		// Mute pad taps (bit per track) from the MIDI thread, applied by the timer.
		std::atomic<uint16_t> m_pendingMuteToggles{0};

		// Pattern picked on Push (bank A-D * 16 + trig) from the MIDI thread, -1 = none.
		std::atomic<int> m_patternRequest{-1};
		// LED-timer side: pattern (0-127) waiting for its countdown, -1 = none.
		int m_pendingPattern = -1;
		int m_pendingPatternTicks = 0;
		int m_reconnectTicks = 0;

		md::PanelRowState m_panelRows;

		// Panel controls currently held down, in press order. Push can hold any
		// combination physically, so unlike the Editor there is no Shift latch.
		std::vector<md::PanelControl> m_heldControls;

		// Touch strip gesture: last position (0..127, -1 = no gesture) and time.
		int m_stripPosition = -1;
		uint32_t m_stripLastMs = 0;
	};
}
