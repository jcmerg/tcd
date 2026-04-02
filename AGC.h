#pragma once

// Single-band AGC with noise gate and peak limiter for PCM audio
// (160 samples / 20ms frames). Applied between decode and encode
// in the transcoder pipeline.
//
// Design improvements over simple RMS-tracking AGC:
// 1. Sliding RMS window (3 frames / 60ms) — smooths syllable-level variation
// 2. Gate with gain decay — during silence, gain drifts toward unity (0dB)
//    instead of freezing, so next speech onset starts near neutral
// 3. Fast release after gate — first frames after gate opening use 2x release
//    speed for quicker recovery from pauses
// 4. Asymmetric gain limits — amplification limited more than attenuation

#include <cstdint>
#include <cmath>
#include <algorithm>
#include <unordered_map>

struct AGCState
{
	float gain = 1.0f;
	uint16_t streamId = 0;

	// Sliding RMS window (3 frames)
	float rmsHistory[3] = {0.0f, 0.0f, 0.0f};
	int rmsIdx = 0;
	int rmsCount = 0;  // frames seen (ramps up to 3)

	// Long-term average gain for this stream (exponential moving average)
	// Used as decay target during gate — maintains speaker-appropriate gain
	float avgGain = 1.0f;

	// Gate tracking
	int gateFrames = 0;    // consecutive frames in gate
	bool wasGated = false; // previous frame was gated
	int postGateFrames = 0; // frames since gate opened (for fast release)
};

class CAGC
{
public:
	void Configure(bool enable, float targetDbfs, float attackMs, float releaseMs,
	               float maxGainUpDb, float maxGainDownDb, float noiseGateDbfs = -55.0f)
	{
		m_enabled = enable;
		m_targetLinear = powf(10.0f, targetDbfs / 20.0f) * 32767.0f;
		m_maxGain = powf(10.0f, maxGainUpDb / 20.0f);
		m_minGain = powf(10.0f, -maxGainDownDb / 20.0f);
		m_attackAlpha  = (attackMs  > 0.0f) ? 1.0f - expf(-20.0f / attackMs)  : 1.0f;
		m_releaseAlpha = (releaseMs > 0.0f) ? 1.0f - expf(-20.0f / releaseMs) : 1.0f;
		m_noiseGateLinear = powf(10.0f, noiseGateDbfs / 20.0f) * 32767.0f;
	}

	void Configure(bool enable, float targetDbfs, float attackMs, float releaseMs, float maxGainDb)
	{
		Configure(enable, targetDbfs, attackMs, releaseMs, maxGainDb, maxGainDb);
	}

	// Core AGC processing — shared between both Process() variants
	void ProcessCore(int16_t *samples, int count, AGCState &state, float &gain_db_out, bool &gate_out)
	{
		// Measure current frame
		float sumSq = 0.0f, peak = 0.0f;
		for (int i = 0; i < count; i++)
		{
			float s = (float)samples[i];
			sumSq += s * s;
			float a = fabsf(s);
			if (a > peak) peak = a;
		}
		float frameRms = sqrtf(sumSq / count);

		// Update sliding RMS window (3 frames = 60ms)
		state.rmsHistory[state.rmsIdx] = frameRms;
		state.rmsIdx = (state.rmsIdx + 1) % 3;
		if (state.rmsCount < 3) state.rmsCount++;

		// Compute windowed RMS (average of available frames)
		float rmsSum = 0.0f;
		for (int i = 0; i < state.rmsCount; i++)
			rmsSum += state.rmsHistory[i];
		float rms = rmsSum / state.rmsCount;

		// Noise gate check
		if (rms < m_noiseGateLinear)
		{
			state.gateFrames++;
			state.wasGated = true;
			state.postGateFrames = 0;

			// Gate with gain decay: slowly drift toward the long-term average gain
			// for this stream. This keeps the gain at the right level for this speaker
			// instead of drifting to unity (which would be wrong for quiet speakers).
			if (state.gateFrames > 5)  // start decay after ~100ms of gate
			{
				float decayAlpha = 0.02f;
				state.gain += decayAlpha * (state.avgGain - state.gain);
			}

			gate_out = true;
			gain_db_out = 20.0f * log10f(std::max(state.gain, 0.001f));
			return;
		}

		// Speech detected — compute desired gain from windowed RMS
		float desiredGain = std::clamp(m_targetLinear / rms, m_minGain, m_maxGain);

		// Peak limiter: never clip
		float peakLimit = 32000.0f / std::max(peak, 1.0f);
		if (desiredGain > peakLimit)
			desiredGain = peakLimit;

		// Determine smoothing alpha
		float alpha;
		if (desiredGain < state.gain)
		{
			// Attack (gain decreasing = loud input)
			alpha = m_attackAlpha;
		}
		else
		{
			// Release (gain increasing = quiet input)
			alpha = m_releaseAlpha;

			// Fast release after gate: 5x speed for first 5 frames (~100ms)
			// This prevents the first syllable after a pause from being too quiet
			if (state.wasGated && state.postGateFrames < 5)
				alpha = std::min(alpha * 5.0f, 0.5f);  // cap at 0.5 to prevent overshoot
		}

		state.gain += alpha * (desiredGain - state.gain);

		// Update long-term average gain (slow EMA, ~2s time constant)
		// Only during speech — gives a stable "typical gain" for this speaker
		float avgAlpha = 0.01f;  // ~2s at 20ms frames
		state.avgGain += avgAlpha * (state.gain - state.avgGain);

		// Track post-gate state
		if (state.wasGated)
		{
			state.postGateFrames++;
			if (state.postGateFrames >= 5)
				state.wasGated = false;  // back to normal tracking
		}
		state.gateFrames = 0;

		// Apply gain with hard clip protection
		for (int i = 0; i < count; i++)
		{
			float s = samples[i] * state.gain;
			samples[i] = (int16_t)std::clamp(s, -32767.0f, 32767.0f);
		}

		gain_db_out = 20.0f * log10f(std::max(state.gain, 0.001f));
		gate_out = false;
	}

	// Process without monitoring output
	void Process(int16_t *samples, int count, uint16_t streamId)
	{
		if (!m_enabled) return;

		auto &state = m_streams[streamId];
		if (state.streamId != streamId)
		{
			state = AGCState{};
			state.streamId = streamId;
		}

		float gain_db;
		bool gate;
		ProcessCore(samples, count, state, gain_db, gate);
	}

	// Process with monitoring output
	void Process(int16_t *samples, int count, uint16_t streamId, float &gain_db_out, bool &gate_out)
	{
		gate_out = false;
		gain_db_out = 0.0f;
		if (!m_enabled) return;

		auto &state = m_streams[streamId];
		if (state.streamId != streamId)
		{
			state = AGCState{};
			state.streamId = streamId;
		}

		ProcessCore(samples, count, state, gain_db_out, gate_out);
	}

	void RemoveStream(uint16_t streamId) { m_streams.erase(streamId); }

	void Cleanup()
	{
		if (m_streams.size() > 100)
			m_streams.clear();
	}

	bool IsEnabled() const { return m_enabled; }

	float GetGainDb(uint16_t streamId) const
	{
		auto it = m_streams.find(streamId);
		if (it == m_streams.end()) return 0.0f;
		return 20.0f * log10f(std::max(it->second.gain, 0.001f));
	}

private:
	bool  m_enabled = false;
	float m_targetLinear = 3277.0f;    // -20 dBFS default
	float m_maxGain = 10.0f;          // +20 dB up
	float m_minGain = 0.063f;         // -24 dB down
	float m_attackAlpha = 0.33f;
	float m_releaseAlpha = 0.04f;
	float m_noiseGateLinear = 58.0f;   // -55 dBFS
	std::unordered_map<uint16_t, AGCState> m_streams;
};
