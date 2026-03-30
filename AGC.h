#pragma once

// Single-band AGC with noise gate and peak limiter for PCM audio
// (160 samples / 20ms frames). Applied between decode and encode
// in the transcoder pipeline.

#include <cstdint>
#include <cmath>
#include <algorithm>
#include <unordered_map>

struct AGCState
{
	float gain = 1.0f;
	uint16_t streamId = 0;
};

class CAGC
{
public:
	void Configure(bool enable, float targetDbfs, float attackMs, float releaseMs, float maxGainDb)
	{
		m_enabled = enable;
		m_targetLinear = powf(10.0f, targetDbfs / 20.0f) * 32767.0f;
		m_maxGain = powf(10.0f, maxGainDb / 20.0f);
		m_minGain = powf(10.0f, -maxGainDb / 20.0f);  // symmetric limit
		// Convert time constants to per-frame (20ms) smoothing coefficients
		// alpha = 1 - exp(-frameDuration / timeConstant)
		m_attackAlpha  = (attackMs  > 0.0f) ? 1.0f - expf(-20.0f / attackMs)  : 1.0f;
		m_releaseAlpha = (releaseMs > 0.0f) ? 1.0f - expf(-20.0f / releaseMs) : 1.0f;
		// Noise gate threshold: -50 dBFS
		m_noiseGateLinear = powf(10.0f, -50.0f / 20.0f) * 32767.0f;
	}

	// Process 160 PCM samples in-place. streamId for per-stream gain tracking.
	void Process(int16_t *samples, int count, uint16_t streamId)
	{
		if (!m_enabled)
			return;

		auto &state = m_streams[streamId];
		if (state.streamId != streamId)
		{
			state.streamId = streamId;
			state.gain = 1.0f;
		}

		// Measure RMS and peak
		float sumSq = 0.0f;
		float peak = 0.0f;
		for (int i = 0; i < count; i++)
		{
			float s = (float)samples[i];
			sumSq += s * s;
			float a = fabsf(s);
			if (a > peak) peak = a;
		}
		float rms = sqrtf(sumSq / count);

		// Noise gate: don't adjust gain below threshold (avoids amplifying noise/silence)
		if (rms < m_noiseGateLinear)
			return;

		// Desired gain to reach target RMS level
		float desiredGain = std::clamp(m_targetLinear / rms, m_minGain, m_maxGain);

		// Peak limiter: reduce gain if it would clip the loudest sample
		float peakLimit = 32000.0f / std::max(peak, 1.0f);
		if (desiredGain > peakLimit)
			desiredGain = peakLimit;

		// Smooth gain transition
		// Attack (gain decreasing → loud input): fast response to prevent clipping
		// Release (gain increasing → quiet input): slow recovery for natural sound
		float alpha = (desiredGain < state.gain) ? m_attackAlpha : m_releaseAlpha;
		state.gain += alpha * (desiredGain - state.gain);

		// Apply gain with hard clip protection
		for (int i = 0; i < count; i++)
		{
			float s = samples[i] * state.gain;
			samples[i] = (int16_t)std::clamp(s, -32767.0f, 32767.0f);
		}
	}

	// Clean up a specific stream (e.g. on stream close)
	void RemoveStream(uint16_t streamId)
	{
		m_streams.erase(streamId);
	}

	// Remove streams not seen for a while (call periodically to prevent memory growth)
	void Cleanup()
	{
		// Simple approach: if map grows beyond reasonable size, clear all
		// (streams re-create their state on next Process call)
		if (m_streams.size() > 100)
			m_streams.clear();
	}

	bool IsEnabled() const { return m_enabled; }

private:
	bool  m_enabled = false;
	float m_targetLinear = 3277.0f;    // -20 dBFS
	float m_maxGain = 4.0f;           // +12 dB
	float m_minGain = 0.25f;          // -12 dB (symmetric)
	float m_attackAlpha = 0.33f;
	float m_releaseAlpha = 0.04f;
	float m_noiseGateLinear = 100.0f;  // -50 dBFS
	std::unordered_map<uint16_t, AGCState> m_streams;
};
