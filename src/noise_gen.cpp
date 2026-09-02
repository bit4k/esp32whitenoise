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

// 64-bit XorShift* PRNG: Period is 2^64 - 1 (~1.84 x 10^19 samples)
// At 44.1 kHz, this will not repeat for over 13 million years!
// Eliminates the 65,536-sample (1.48s / ~2Hz) cyclical repetition of the old LCG generator.
static uint64_t s_rng_state = 0x853c49e6748fea9bULL;

static inline uint32_t xorshift64star() {
    s_rng_state ^= s_rng_state >> 12;
    s_rng_state ^= s_rng_state << 25;
    s_rng_state ^= s_rng_state >> 27;
    return (uint32_t)((s_rng_state * 0x2545F4914F6CDD1DULL) >> 32);
}

int16_t NoiseGenerator::generateWhite() {
    // Use upper 16 bits of 64-bit XorShift* for maximum entropy and zero low-bit correlation
    return (int16_t)(xorshift64star() >> 16);
}

int16_t NoiseGenerator::generatePink() {
    // Voss-McCartney algorithm approximation / Paul Kellett's method
    float white = ((float)(int16_t)(xorshift64star() >> 16)) / 32768.0f;
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
    float white = ((float)(int16_t)(xorshift64star() >> 16)) / 32768.0f;
    brown_out = (brown_out + (0.02f * white)) / 1.02f;
    float out = brown_out * 3.5f; // Gain adjust
    
    out *= 32767.0f;
    if (out > 32767) out = 32767;
    if (out < -32768) out = -32768;
    return (int16_t)out;
}

int16_t NoiseGenerator::generateBlue() {
    float white = ((float)(int16_t)(xorshift64star() >> 16)) / 32768.0f;
    float out = white - blue_last;
    blue_last = white;
    
    out *= 16383.0f; // Scale
    if (out > 32767) out = 32767;
    if (out < -32768) out = -32768;
    return (int16_t)out;
}

int16_t NoiseGenerator::generateViolet() {
    float white = ((float)(int16_t)(xorshift64star() >> 16)) / 32768.0f;
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
    float dt = 1.0f / 44100.0f;
    
    for (int32_t i = 0; i < frameCount; i++) {
        int16_t sampleL = 0;
        int16_t sampleR = 0;
        
        if (currentType == 0) {
            // Pure White Noise: Independent random samples for Left & Right (wide, organic spatial sound)
            sampleL = generateWhite();
            sampleR = generateWhite();
        } else {
            switch (currentType) {
                case 1: sampleL = generatePink(); break;
                case 2: sampleL = generateBrown(); break;
                case 3: sampleL = generateBlue(); break;
                case 4: sampleL = generateViolet(); break;
                
                case 5: lfo_freq = 0.2f; sampleL = generateWhite(); break; // Ocean Waves
                case 6: lfo_freq = 0.2f; sampleL = generatePink(); break;  // Pink Ocean
                case 7: lfo_freq = 0.2f; sampleL = generateBrown(); break; // Deep Ocean
                case 8: lfo_freq = 0.1f; sampleL = generatePink(); break;  // Breathing Pink
                case 9: lfo_freq = 1.0f; sampleL = generatePink(); break;  // Fast Mod Pink
            }
            
            if (currentType >= 5) {
                lfo_phase += lfo_freq * dt;
                if (lfo_phase > 1.0f) lfo_phase -= 1.0f;
                
                // LFO shape: Sine wave mapped to 0.2 .. 1.0
                float mod = 0.6f + 0.4f * sinf(lfo_phase * 2.0f * (float)M_PI);
                sampleL = (int16_t)((float)sampleL * mod);
            }
            sampleR = sampleL;
        }
        
        frames[i].channel1 = sampleL;
        frames[i].channel2 = sampleR;
    }
}
