#include "noise_gen.h"
#include <math.h>

NoiseGenerator noiseGen;

const char* NOISE_NAMES[NUM_NOISE_TYPES] = {
    "White Noise",
    "Pink Noise",
    "Brown Noise",
    "Blue Noise",
    "Violet Noise",
    "Ocean Waves",     // Modulated White
    "Pink Ocean",      // Modulated Pink
    "Deep Ocean",      // Modulated Brown
    "Breathing Pink",  // Slow Modulated Pink
    "Fast Modulated Pink"
};

NoiseGenerator::NoiseGenerator() {
    currentType = 0;
    pink_b0 = pink_b1 = pink_b2 = pink_b3 = pink_b4 = pink_b5 = pink_b6 = 0;
    brown_out = 0;
    blue_last = 0;
    lfo_phase = 0;
    lfo_freq = 0.5f; // 0.5 Hz default
}

void NoiseGenerator::setType(int typeIndex) {
    if (typeIndex >= 0 && typeIndex < NUM_NOISE_TYPES) {
        currentType = typeIndex;
    }
}

int NoiseGenerator::getType() {
    return currentType;
}

const char* NoiseGenerator::getTypeName(int typeIndex) {
    if (typeIndex >= 0 && typeIndex < NUM_NOISE_TYPES) {
        return NOISE_NAMES[typeIndex];
    }
    return "Unknown";
}

int16_t NoiseGenerator::generateWhite() {
    // Generate random number between -32768 and 32767
    return (int16_t)(esp_random() % 65536 - 32768);
}

int16_t NoiseGenerator::generatePink() {
    // Voss-McCartney algorithm approximation or Paul Kellett's method
    float white = ((float)(esp_random() % 65536) - 32768.0f) / 32768.0f;
    pink_b0 = 0.99886f * pink_b0 + white * 0.0555179f;
    pink_b1 = 0.99332f * pink_b1 + white * 0.0750759f;
    pink_b2 = 0.96900f * pink_b2 + white * 0.1538520f;
    pink_b3 = 0.86650f * pink_b3 + white * 0.3104856f;
    pink_b4 = 0.55000f * pink_b4 + white * 0.5329522f;
    pink_b5 = -0.7616f * pink_b5 - white * 0.0168980f;
    float out = pink_b0 + pink_b1 + pink_b2 + pink_b3 + pink_b4 + pink_b5 + pink_b6 + white * 0.5362f;
    pink_b6 = white * 0.115926f;
    
    // Scale back to int16_t, avoiding clipping
    out *= 4000.0f;
    if (out > 32767) out = 32767;
    if (out < -32768) out = -32768;
    return (int16_t)out;
}

int16_t NoiseGenerator::generateBrown() {
    float white = ((float)(esp_random() % 65536) - 32768.0f) / 32768.0f;
    brown_out = (brown_out + (0.02f * white)) / 1.02f;
    float out = brown_out * 3.5f; // Gain adjust
    
    out *= 32767.0f;
    if (out > 32767) out = 32767;
    if (out < -32768) out = -32768;
    return (int16_t)out;
}

int16_t NoiseGenerator::generateBlue() {
    float white = ((float)(esp_random() % 65536) - 32768.0f) / 32768.0f;
    float out = white - blue_last;
    blue_last = white;
    
    out *= 16383.0f; // Scale
    if (out > 32767) out = 32767;
    if (out < -32768) out = -32768;
    return (int16_t)out;
}

int16_t NoiseGenerator::generateViolet() {
    // Differentiation of blue noise or simple highpass
    float white = ((float)(esp_random() % 65536) - 32768.0f) / 32768.0f;
    float blue = white - blue_last;
    blue_last = white;
    static float violet_last = 0;
    float out = blue - violet_last;
    violet_last = blue;
    
    out *= 10000.0f; // Scale
    if (out > 32767) out = 32767;
    if (out < -32768) out = -32768;
    return (int16_t)out;
}

void NoiseGenerator::getFrames(Frame* frames, int32_t frameCount) {
    // 44100 Hz sample rate assumed
    float dt = 1.0f / 44100.0f;
    
    for (int32_t i = 0; i < frameCount; i++) {
        int16_t sample = 0;
        
        switch (currentType) {
            case 0: sample = generateWhite(); break;
            case 1: sample = generatePink(); break;
            case 2: sample = generateBrown(); break;
            case 3: sample = generateBlue(); break;
            case 4: sample = generateViolet(); break;
            
            case 5: lfo_freq = 0.2f; sample = generateWhite(); break; // Ocean Waves
            case 6: lfo_freq = 0.2f; sample = generatePink(); break;  // Pink Ocean
            case 7: lfo_freq = 0.2f; sample = generateBrown(); break; // Deep Ocean
            case 8: lfo_freq = 0.1f; sample = generatePink(); break;  // Breathing Pink
            case 9: lfo_freq = 1.0f; sample = generatePink(); break;  // Fast Mod Pink
        }
        
        // Apply modulation for types >= 5
        if (currentType >= 5) {
            lfo_phase += lfo_freq * dt;
            if (lfo_phase > 1.0f) lfo_phase -= 1.0f;
            
            // LFO shape: Sine wave mapped to 0.2 .. 1.0
            float mod = 0.6f + 0.4f * sinf(lfo_phase * 2.0f * (float)M_PI);
            sample = (int16_t)((float)sample * mod);
        }
        
        // Stereo
        frames[i].channel1 = sample;
        frames[i].channel2 = sample;
    }
}
