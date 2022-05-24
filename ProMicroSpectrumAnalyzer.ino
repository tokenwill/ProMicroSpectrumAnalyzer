// (Heavily) adapted from https://github.com/G6EJD/ESP32-8266-Audio-Spectrum-Display/blob/master/ESP32_Spectrum_Display_02.ino
// Adjusted to allow brightness changes on press+hold, Auto-cycle for 3 button presses within 2 seconds
// Edited to add Neomatrix support for easier compatibility with different layouts.

#include <FastLED_NeoMatrix.h>
#include <arduinoFFT.h>
#include <EasyButton.h>

#define SAMPLES         64          // Must be a power of 2
#define SAMPLING_FREQ   20000         // Hz, must be 40000 or less due to ADC conversion time. Determines maximum frequency that can be analysed by the FFT Fmax=sampleF/2.
#define AMPLITUDE       16          // Depending on your audio source level, you may need to alter this value. Can be used as a 'sensitivity' control.
//#define AUDIO_IN_PIN    A0            // Signal in on this pin
#define LED_PIN         3             // LED strip data
#define BTN_PIN         2             // Connect a push button to this pin to change patterns
#define LONG_PRESS_MS   200           // Number of ms to count as a long press
#define COLOR_ORDER     GRB           // If colours look wrong, play with this
#define CHIPSET         WS2812B       // LED strip type
//#define MAX_MILLIAMPS   500          // Careful with the amount of power here if running off USB port
//const int BRIGHTNESS_SETTINGS[3] = {25, 25, 35};  // 3 Integer array for 3 brightness settings (based on pressing+holding BTN_PIN)
#define NUM_BANDS       8            // To change this, you will need to change the bunch of if statements describing the mapping from bins to bands
const uint8_t kMatrixWidth = 8;                          // Matrix width
const uint8_t kMatrixHeight = 8;                         // Matrix height
#define NUM_LEDS       (kMatrixWidth * kMatrixHeight)     // Total number of LEDs
#define BAR_WIDTH      (kMatrixWidth  / (NUM_BANDS - 1))  // If width >= 8 light 1 LED width per bar, >= 16 light 2 LEDs width bar etc
#define TOP            (kMatrixHeight - 0)                // Don't allow the bars to go offscreen
#define SERPENTINE     true                               // Set to false if you're LEDS are connected end to end, true if serpentine


//#define MIC_PIN    A0                                        // Microphone, or A0 on a WeMOS D1 Mini.
#define DC_OFFSET  370                                      // DC offset in mic signal. I subtract this value from the raw sample.

int sample;
int sampleAgc, multAgc;
bool samplePeak = 0;                                          // Boolean flag for peak. Responding routine must reset this flag.
float sampleAvg = 0;                                          // Smoothed Average.
//int16_t micLev;                                           // Here, we subtract the offset, so this should be 16 bit signed.
uint8_t maxVol = 11;                                          // Reasonable value for constant volume for 'peak detector', as it won't always trigger.
uint8_t squelch = 50;                                          // Anything below this is background noise, so we'll make it '0'.
uint8_t targetAgc = 60;                                       // This is our setPoint at 20% of max for the adjusted output.
// Params for width and height

// Sampling and FFT stuff
//unsigned int sampling_period_us;
byte peak[] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};              // The length of these arrays must be >= NUM_BANDS
int oldBarHeights[] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
int bandValues[] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
double vReal[SAMPLES];
double vImag[SAMPLES];
double fftBin[SAMPLES];
unsigned long newTime;
arduinoFFT FFT = arduinoFFT(vReal, vImag, SAMPLES, SAMPLING_FREQ);

// Button stuff
int buttonPushCounter = 0;
bool autoChangePatterns = false;
EasyButton modeBtn(BTN_PIN);

// FastLED stuff
CRGB leds[NUM_LEDS];
DEFINE_GRADIENT_PALETTE( purple_gp ) {
  0,   0, 212, 255,   //blue
255, 179,   0, 255 }; //purple
DEFINE_GRADIENT_PALETTE( outrun_gp ) {
  0, 141,   0, 100,   //purple
127, 255, 192,   0,   //yellow
255,   0,   5, 255 };  //blue
DEFINE_GRADIENT_PALETTE( greenblue_gp ) {
  0,   0, 255,  60,   //green
 64,   0, 236, 255,   //cyan
128,   0,   5, 255,   //blue
192,   0, 236, 255,   //cyan
255,   0, 255,  60 }; //green
DEFINE_GRADIENT_PALETTE( redyellow_gp ) {
  0,   200, 200,  200,   //white
 64,   255, 218,    0,   //yellow
128,   231,   0,    0,   //red
192,   255, 218,    0,   //yellow
255,   200, 200,  200 }; //white
CRGBPalette16 purplePal = purple_gp;
CRGBPalette16 outrunPal = outrun_gp;
CRGBPalette16 greenbluePal = greenblue_gp;
CRGBPalette16 heatPal = redyellow_gp;
uint8_t colorTimer = 0;

// FastLED_NeoMaxtrix - see https://github.com/marcmerlin/FastLED_NeoMatrix for Tiled Matrixes, Zig-Zag and so forth
FastLED_NeoMatrix *matrix = new FastLED_NeoMatrix(leds, kMatrixWidth, kMatrixHeight,
  NEO_MATRIX_TOP        + NEO_MATRIX_RIGHT +
  NEO_MATRIX_ROWS       + NEO_MATRIX_ZIGZAG +
  NEO_TILE_TOP + NEO_TILE_LEFT + NEO_TILE_ROWS);

void setup() {
  Serial.begin(115200);
  FastLED.addLeds<CHIPSET, LED_PIN, COLOR_ORDER>(leds, NUM_LEDS).setCorrection(TypicalSMD5050);
  FastLED.setMaxPowerInVoltsAndMilliamps(5, 500);
  FastLED.setBrightness(30);
  FastLED.clear();

  ADCSRA = 0xE5;      // set ADC to free running mode and set pre-scalar to 32 (0xe5)
  ADMUX  = 0x47;;       // use pin A0 and external voltage reference
  DIDR0  = 0x80;       // use pin A0 and external voltage reference
}

void changeMode() {
  Serial.println("Button pressed");
  autoChangePatterns = false;
  buttonPushCounter = (buttonPushCounter + 1) % 6;
}

void startAutoMode() {
  autoChangePatterns = true;
}

void brightnessOff(){
  FastLED.setBrightness(0);  //Lights out
}

void loop() {

  // Don't clear screen if waterfall pattern, be sure to change this is you change the patterns / order
  if (buttonPushCounter != 5) FastLED.clear();
//
//  modeBtn.read();

  getFFT();

  // Analyse FFT results
  for (int i = 1; i < (SAMPLES/2); i++){       // Don't use sample 0 and only first SAMPLES/2 are usable. Each array element represents a frequency bin and its value the amplitude.
    //if (vReal[i] > NOISE) {                    // Add a crude noise filter

/*     //8 bands, 12kHz top band
     if (i<=2 )         bandValues[0]  += (int)vReal[i];
     if (i>2  && i<=3)  bandValues[1]  += (int)vReal[i];
     if (i>3  && i<=4)  bandValues[2]  += (int)vReal[i];
     if (i>4  && i<=5)  bandValues[3]  += (int)vReal[i];
     if (i>5  && i<=6)  bandValues[4]  += (int)vReal[i];
     if (i>6  && i<=8)  bandValues[5]  += (int)vReal[i];
     if (i>8  && i<=15) bandValues[6]  += (int)vReal[i];
     if (i>15         ) bandValues[7]  += (int)vReal[i]; */

         //8 bands, 12kHz top band
     if (i<=2 )         bandValues[0]  += (int)vReal[i];
     if (i>2  && i<=4)  bandValues[1]  += (int)vReal[i];
     if (i>4  && i<=6)  bandValues[2]  += (int)vReal[i];
     if (i>6  && i<=8)  bandValues[3]  += (int)vReal[i];
     if (i>8  && i<=10) bandValues[4]  += (int)vReal[i];
     if (i>10  && i<=14)bandValues[5]  += (int)vReal[i];
     if (i>14  && i<=22)bandValues[6]  += (int)vReal[i];
     if (i>22         ) bandValues[7]  += (int)vReal[i];

//    //16 bands, 12kHz top band
//      if (i<=2 )           bandValues[0]  += (int)vReal[i];
//      if (i>2   && i<=3  ) bandValues[1]  += (int)vReal[i];
//      if (i>3   && i<=5  ) bandValues[2]  += (int)vReal[i];
//      if (i>5   && i<=7  ) bandValues[3]  += (int)vReal[i];
//      if (i>7   && i<=9  ) bandValues[4]  += (int)vReal[i];
//      if (i>9   && i<=13 ) bandValues[5]  += (int)vReal[i];
//      if (i>13  && i<=18 ) bandValues[6]  += (int)vReal[i];
//      if (i>18  && i<=25 ) bandValues[7]  += (int)vReal[i];
//      if (i>25  && i<=36 ) bandValues[8]  += (int)vReal[i];
//      if (i>36  && i<=50 ) bandValues[9]  += (int)vReal[i];
//      if (i>50  && i<=69 ) bandValues[10] += (int)vReal[i];
//      if (i>69  && i<=97 ) bandValues[11] += (int)vReal[i];
//      if (i>97  && i<=135) bandValues[12] += (int)vReal[i];
//      if (i>135 && i<=189) bandValues[13] += (int)vReal[i];
//      if (i>189 && i<=264) bandValues[14] += (int)vReal[i];
//      if (i>264          ) bandValues[15] += (int)vReal[i];
    
  }

  // Process the FFT data into bar heights
  for (byte band = 0; band < NUM_BANDS; band++) {

    // Scale the bars for the display
    int barHeight = bandValues[band] / AMPLITUDE;
    if (barHeight > TOP) barHeight = TOP;

    // Small amount of averaging between frames
    barHeight = ((oldBarHeights[band] * 1) + barHeight) / 2;

    // Move peak up
    if (barHeight > peak[band]) {
      peak[band] = min(TOP, barHeight);
    }

    // Draw bars
    switch (buttonPushCounter) {
      case 0:
        rainbowBars(band, barHeight);
        break;
      case 1:
        // No bars on this one
        break;
      case 2:
        purpleBars(band, barHeight);
        break;
      case 3:
        centerBars(band, barHeight);
        break;
      case 4:
        changingBars(band, barHeight);
        break;
      case 5:
        waterfall(band);
        break;
    }

    // Draw peaks
    switch (buttonPushCounter) {
      case 0:
        whitePeak(band);
        break;
      case 1:
        outrunPeak(band);
        break;
      case 2:
        whitePeak(band);
        break;
      case 3:
        // No peaks
        break;
      case 4:
        // No peaks
        break;
      case 5:
        // No peaks
        break;
    }

    // Save oldBarHeights for averaging later
    oldBarHeights[band] = barHeight;
  }

  // Decay peak
  EVERY_N_MILLISECONDS(60) {
    for (byte band = 0; band < NUM_BANDS; band++)
      if (peak[band] > 0) peak[band] -= 1;
    colorTimer++;
  }

  // Used in some of the patterns
  EVERY_N_MILLISECONDS(10) {
    colorTimer++;
  }

  EVERY_N_SECONDS(10) {
    if (autoChangePatterns) buttonPushCounter = (buttonPushCounter + 1) % 6;
  }

  FastLED.show();
}

// PATTERNS BELOW //

void rainbowBars(int band, int barHeight) {
  int xStart = BAR_WIDTH * band;
  for (int x = xStart; x < xStart + BAR_WIDTH; x++) {
    for (int y = TOP; y >= TOP - barHeight; y--) {
      matrix->drawPixel(x, y, CHSV((x / BAR_WIDTH) * (255 / NUM_BANDS), 255, 155));
    }
  }
}

void purpleBars(int band, int barHeight) {
  int xStart = BAR_WIDTH * band;
  for (int x = xStart; x < xStart + BAR_WIDTH; x++) {
    for (int y = TOP; y >= TOP - barHeight; y--) {
      matrix->drawPixel(x, y, ColorFromPalette(purplePal, y * (255 / (barHeight + 1))));
    }
  }
}

void changingBars(int band, int barHeight) {
  int xStart = BAR_WIDTH * band;
  for (int x = xStart; x < xStart + BAR_WIDTH; x++) {
    for (int y = TOP; y >= TOP - barHeight; y--) {
      matrix->drawPixel(x, y, CHSV(y * (255 / kMatrixHeight) + colorTimer, 255, 255));
    }
  }
}

void centerBars(int band, int barHeight) {
  int xStart = BAR_WIDTH * band;
  for (int x = xStart; x < xStart + BAR_WIDTH; x++) {
    if (barHeight % 2 == 0) barHeight--;
    int yStart = ((kMatrixHeight - barHeight) / 2 );
    for (int y = yStart; y <= (yStart + barHeight); y++) {
      int colorIndex = constrain((y - yStart) * (255 / barHeight), 0, 255);
      matrix->drawPixel(x, y, ColorFromPalette(heatPal, colorIndex));
    }
  }
}

void whitePeak(int band) {
  int xStart = BAR_WIDTH * band;
  int peakHeight = TOP - peak[band] - 1;
  for (int x = xStart; x < xStart + BAR_WIDTH; x++) {
    matrix->drawPixel(x, peakHeight, CHSV(0,0,135));
  }
}

void outrunPeak(int band) {
  int xStart = BAR_WIDTH * band;
  int peakHeight = TOP - peak[band] - 1;
  for (int x = xStart; x < xStart + BAR_WIDTH; x++) {
    matrix->drawPixel(x, peakHeight, ColorFromPalette(outrunPal, peakHeight * (255 / kMatrixHeight)));
  }
}

void waterfall(int band) {
  int xStart = BAR_WIDTH * band;
  double highestBandValue = 60000;        // Set this to calibrate your waterfall

  // Draw bottom line
  for (int x = xStart; x < xStart + BAR_WIDTH; x++) {
    matrix->drawPixel(x, 0, CHSV(constrain(map(bandValues[band],0,highestBandValue,160,0),0,160), 255, 255));
  }

  // Move screen up starting at 2nd row from top
  if (band == NUM_BANDS - 1){
    for (int y = kMatrixHeight - 2; y >= 0; y--) {
      for (int x = 0; x < kMatrixWidth; x++) {
        int pixelIndexY = matrix->XY(x, y + 1);
        int pixelIndex = matrix->XY(x, y);
        leds[pixelIndexY] = leds[pixelIndex];
      }
    }
  }
}

void getSample() {

  int16_t micIn,micLev;                                            // Current sample starts with negative values and large values, which is why it's 16 bit signed.
  static long peakTime;

  while(!(ADCSRA & 0x10))
  ADCSRA = 0xf5;                                            // clear ADIF bit so that ADC can do next operation (0xf5)
  micIn = ADC;                                              // Read in
  micLev = micIn - DC_OFFSET;                               // centering around 0.
  micIn = abs(micLev);
  sample = (micIn <= squelch) ? 0 : (sample + micIn) / 2;
  sampleAvg = ((sampleAvg * 31) + sample) / 32;               // Smooth it out over the last 32 samples.
    
  if (sample > (sampleAvg+maxVol) && millis() > (peakTime + 50)) {    // Poor man's beat detection by seeing if sample > Average + some value.
    samplePeak = 1;                                                   // Then we got a peak, else we don't. Display routines need to reset the samplepeak value in case they miss the trigger.
    peakTime=millis();                
  }
  samplePeak = 0;
}

void getFFT() {
  // Reset bandValues[]
  for (int i = 0; i<NUM_BANDS; i++){
    bandValues[i] = 0;
  }

  // Sample the audio pin
  for (int i = 0; i < SAMPLES; i++) {
    //newTime = micros();
    getSample();
    vReal[i] = sample;
    vImag[i] = 0;
    //while ((micros() - newTime) < sampling_period_us) { /* chill */ }
  }

  // Compute FFT
//  FFT.DCRemoval();
  FFT.Windowing( FFT_WIN_TYP_FLT_TOP, FFT_FORWARD );         // Flat Top Window - better amplitude accuracy
//  FFT.Windowing(FFT_WIN_TYP_HAMMING, FFT_FORWARD);
  FFT.Compute(FFT_FORWARD);
  FFT.ComplexToMagnitude();
}

