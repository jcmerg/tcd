#pragma once

// Single-band AGC with noise gate and peak limiter for PCM audio
// (160 samples / 20ms frames). Applied between decode and encode
// in the transcoder pipeline.
//
// Gain limits are asymmetric: amplification (up) is limited more
// conservatively than attenuation (down), because boosting amplifies
// noise while cutting is always safe.

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
	void Configure(bool enable, float targetDbfs, float attackMs, float releaseMs,
	               float maxGainUpDb, float maxGainDownDb, float noiseGateDbfs = -55.0f)
	{
		m_enabled = enable;
		m_targetLinear = powf(10.0f, targetDbfs / 20.0f) * 32767.0f;
		m_maxGain = powf(10.0f, maxGainUpDb / 20.0f);     // max amplification
		m_minGain = powf(10.0f, -maxGainDownDb / 20.0f);  // max attenuation
		m_attackAlpha  = (attackMs  > 0.0f) ? 1.0f - expf(-20.0f / attackMs)  : 1.0f;
		m_releaseAlpha = (releaseMs > 0.0f) ? 1.0f - expf(-20.0f / releaseMs) : 1.0f;
		m_noiseGateLinear = powf(10.0f, noiseGateDbfs / 20.0f) * 32767.0f;
	}

	// Backwards-compatible: single maxGainDb uses asymmetric defaults
	void Configure(bool enable, float targetDbfs, float attackMs, float releaseMs, float maxGainDb)
	{
		Configure(enable, targetDbfs, attackMs, releaseMs, maxGainDb, maxGainDb);
	}

	// Process 160 PCM samples in-place
	void Process(int16_t *samples, int count, uint16_t streamId)
	{
		if (!m_enabled) return;

		auto &state = m_streams[streamId];
		if (state.streamId != streamId)
		{
			state.streamId = streamId;
			state.gain = 1.0f;
		}

		float sumSq = 0.0f, peak = 0.0f;
		for (int i = 0; i < count; i++)
		{
			float s = (float)samples[i];
			sumSq += s * s;
			float a = fabsf(s);
			if (a > peak) peak = a;
		}
		float rms = sqrtf(sumSq / count);

		if (rms < m_noiseGateLinear)
			return;

		float desiredGain = std::clamp(m_targetLinear / rms, m_minGain, m_maxGain);

		// Peak limiter: never clip
		float peakLimit = 32000.0f / std::max(peak, 1.0f);
		if (desiredGain > peakLimit)
			desiredGain = peakLimit;

		float alpha = (desiredGain < state.gain) ? m_attackAlpha : m_releaseAlpha;
		state.gain += alpha * (desiredGain - state.gain);

		for (int i = 0; i < count; i++)
		{
			float s = samples[i] * state.gain;
			samples[i] = (int16_t)std::clamp(s, -32767.0f, 32767.0f);
		}
	}

	// Process with monitoring output (gain_db, gate status)
	void Process(int16_t *samples, int count, uint16_t streamId, float &gain_db_out, bool &gate_out)
	{
		gate_out = false;
		gain_db_out = 0.0f;
		if (!m_enabled) return;

		auto &state = m_streams[streamId];
		if (state.streamId != streamId)
		{
			state.streamId = streamId;
			state.gain = 1.0f;
		}

		float sumSq = 0.0f, peak = 0.0f;
		for (int i = 0; i < count; i++)
		{
			float s = (float)samples[i];
			sumSq += s * s;
			float a = fabsf(s);
			if (a > peak) peak = a;
		}
		float rms = sqrtf(sumSq / count);

		if (rms < m_noiseGateLinear)
		{
			gate_out = true;
			gain_db_out = 20.0f * log10f(std::max(state.gain, 0.001f));
			return;
		}

		float desiredGain = std::clamp(m_targetLinear / rms, m_minGain, m_maxGain);

		float peakLimit = 32000.0f / std::max(peak, 1.0f);
		if (desiredGain > peakLimit)
			desiredGain = peakLimit;

		float alpha = (desiredGain < state.gain) ? m_attackAlpha : m_releaseAlpha;
		state.gain += alpha * (desiredGain - state.gain);

		for (int i = 0; i < count; i++)
		{
			float s = samples[i] * state.gain;
			samples[i] = (int16_t)std::clamp(s, -32767.0f, 32767.0f);
		}

		gain_db_out = 20.0f * log10f(std::max(state.gain, 0.001f));
		gate_out = false;
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
	float m_maxGain = 10.0f;          // +20 dB up (amplification)
	float m_minGain = 0.063f;         // -24 dB down (attenuation)
	float m_attackAlpha = 0.33f;
	float m_releaseAlpha = 0.04f;
	float m_noiseGateLinear = 58.0f;   // -55 dBFS
	std::unordered_map<uint16_t, AGCState> m_streams;
};
