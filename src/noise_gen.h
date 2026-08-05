#pragma once

#include <Arduino.h>
#include <stdint.h>

#define NUM_NOISE_TYPES 10

class NoiseGenerator {
public:
    NoiseGenerator();
    void setType(int typeIndex);
    int getType();
    const char* getTypeName(int typeIndex);
    
    // Fills the stereo frame buffer with noise samples
    // A frame is 2x 16-bit integers (Left and Right)
    struct Frame { int16_t channel1; int16_t channel2; };
    void getFrames(Frame* frames, int32_t frameCount);

private:
    int currentType;
    
    // State variables for filters
    float pink_b0, pink_b1, pink_b2, pink_b3, pink_b4, pink_b5, pink_b6;
    float brown_out;
    float blue_last;
    
    // LFO (Low Frequency Oscillator) state for modulation
    float lfo_phase;
    float lfo_freq;

    int16_t generateWhite();
    int16_t generatePink();
    int16_t generateBrown();
    int16_t generateBlue();
    int16_t generateViolet();
};

extern NoiseGenerator noiseGen;
