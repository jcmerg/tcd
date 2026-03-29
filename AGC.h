#pragma once

// Simple single-band AGC for PCM audio (160 samples / 20ms frames)
// Applied between decode and encode in the transcoder pipeline.

#include <cstdint>
#include <cmath>
#include <unordered_map>

struct AGCState
{
	float gain = 1.0f;       // current linear gain
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
		// Convert time constants to per-frame (20ms) smoothing coefficients
		// alpha = 1 - exp(-20 / T)
		m_attackAlpha  = (attackMs  > 0.0f) ? 1.0f - expf(-20.0f / attackMs)  : 1.0f;
		m_releaseAlpha = (releaseMs > 0.0f) ? 1.0f - expf(-20.0f / releaseMs) : 1.0f;
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

		// Measure RMS
		float sum = 0.0f;
		for (int i = 0; i < count; i++)
		{
			float s = (float)samples[i];
			sum += s * s;
		}
		float rms = sqrtf(sum / count);

		if (rms < 1.0f)
			return; // silence, don't adjust gain

		// Desired gain to reach target level
		float desiredGain = m_targetLinear / rms;
		if (desiredGain > m_maxGain)
			desiredGain = m_maxGain;
		if (desiredGain < 0.01f)
			desiredGain = 0.01f;

		// Smooth gain transition (attack = getting quieter, release = getting louder)
		float alpha = (desiredGain < state.gain) ? m_attackAlpha : m_releaseAlpha;
		state.gain += alpha * (desiredGain - state.gain);

		// Apply gain
		for (int i = 0; i < count; i++)
		{
			float s = samples[i] * state.gain;
			if (s > 32767.0f) s = 32767.0f;
			else if (s < -32767.0f) s = -32767.0f;
			samples[i] = (int16_t)s;
		}
	}

	// Clean up finished streams
	void RemoveStream(uint16_t streamId)
	{
		m_streams.erase(streamId);
	}

	bool IsEnabled() const { return m_enabled; }

private:
	bool  m_enabled = false;
	float m_targetLinear = 3277.0f; // -20 dBFS default
	float m_maxGain = 4.0f;        // +12 dB default
	float m_attackAlpha = 0.33f;
	float m_releaseAlpha = 0.04f;
	std::unordered_map<uint16_t, AGCState> m_streams;
};
