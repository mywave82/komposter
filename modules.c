/*
 * Komposter
 *
 * Copyright (c) 2010 Noora Halme et al. (see AUTHORS)
 *
 * This code is licensed under the GNU General Public
 * License version 2. See LICENSE for full text.
 *
 * Implementations for the synthesizer modules
 *
 */

#include <stdlib.h>
#include <stdio.h>
#include <math.h>

#define _MODULES_C_
#include "audio.h"
#include "buffermm.h"
#include "modules.h"
#include "synthesizer.h"
#include "sequencer.h"

// macros for typecasting the localdata void ptr
#define mod_fdata  ((float*)data)
#define mod_fpdata ((float**)data)
#define mod_ldata  ((u32*)data)
#define mod_lpdata ((u32**)data)
#define mod_ddata  ((double*)data)

#define clamp(X)   fmax(fmin(X, 1.0f), 0.0f)

int noise_x1=0x67452301, noise_x2=0xefcdab89;


// jp8000 supersaw oscillator offsets and detune curve coefficents (thanks to adam szabo!)
double osc_offset[7]={0, 0.01991221, -0.01952356, 0.06216538, -0.06288439, 0.10745242, -0.11002313};
double coeftable[12]={
  0.0030115596, 0.6717417634, -24.1878824391, 404.2703938388, -3425.0836591318, 17019.9518580080, -53046.9642751875,
  106649.6679158292, -138150.6761080548, 111363.4808729368, -50818.8652045924, 10028.7312891634};

// tables for detune and mix coefficents for modulator values 0..127
float supersaw_detune[128][7];
float supersaw_mix[128][7];


float pitch[MAX_SYNTH];
float accent[MAX_SYNTH];

// these are just 1-bit flags
int gate[MAX_SYNTH];

// this has flags for different restart types
int restart[MAX_SYNTH];

// from sequencer.c
extern int seqch;


////////////////////////////////////////////////
//
// module definitions
//
///////////////////////////////////////////////

// names for the knob scale types
const char *knobScaleNames[KNOBSCALES]=
{
  "Raw float",
  "Frequency (Hz)",
  "Freq from tempo",
  "Duration (sec)",
  "Duration from tempo",
  "Ramp (sec)",
  "mIDI Note",
  "Semitones interval",
  "Percentage"
};
const char *knobScaleUnits[KNOBSCALES]=
{
  "",
  "Hz",
  "x tempo (Hz)",
  "sec",
  "x tempo (sec)",
  "sec",
  "",
  "semitones",
  "%"
};


// Descriptive names for all 16 module types
const char *modTypeNames[MODTYPES]=
{
	"KBD CV",
	"ENV",
	"VCO",
	"LFO",
	"Knob",
	"Amp mod",
	"mixer",
	"VCF",
	"LPF",
	"Delay",
	"Scale",
	"resample",
	"Suprsaw", //Switch",
	"Distort",
	"Accent",
	"Output",
	"Bitcrush",
	"Slew",
	"Mod CV"
};

// Module descriptions
const char *modTypeDescriptions[MODTYPES]=
{
	"Keyboard control voltage (CV) from the sequencer for setting the VCO frequency. The voltage\nis 1 unit/hz, so an A-3 generates a voltage of 440.",
	"ADSR envelope generator with linear attack and decay/release ramps.\nSustain level sets the amplification, A/D/R ramps are set as a duration",
	"Voltage controlled oscillator (VCO). Outputs the chosen waveform at a\nfrequency set by the control voltage. Includes a noise generator\nand -1 octave pulse suboscillator with levels\nselectable using control inputs.",
	"The low frequency oscillator (LFO) can be used to, for example, make\nlong filter- and pulse width sweeps, vibrato or other effects\nlike chorus and flanging.",
	"Knobs output a control voltage which can be used to control the\nother modules. There is no forced voltage range but 0 to 1 is normally used.",
	"Amplitude modulation amplifies or attenuates the input signal\naccording to the control voltage. Since this is done by multiplying,\nit also works as a ring modulator.",
	"Mixes up to four signals together by adding. Works also an an\namplifier or a frequency multiplier by connecting the same signal to\nmultiple inputs",
	"State-variable voltage controlled filter (VCF). Resonant low-,\nband- and highpass filter with 12db/oct attenuation.",
	"Voltage controlled 24db/oct 4-pole resonant lowpass filter (LPF).\nSelf-oscillates when resonance is set to 100%.",
	"Delay module which can operate either as a comb filter or an\nallpass filter. Has control inputs for gain/feedback, loop length and\ndelay time. When feedback is not connected or zero, it functions\nas a normal one-tap variable delay. The loop length is 3 seconds when\nno control input is provided.",
	"Scaler knobs can be used attenuate or amplify an incoming signal\nby multiplying it with the modulator value.",
        "Sample-and-hold resamples the signal down by sampling it at given\nintervals and outputting the sampled value until a new one is\nread. The sample rate input is a frequency in hz",
        "Seven-oscillator supersaw generator with detune and mix controls.\nEmulates closely the output of a Roland JP8000/JP8080 supersaw.",
	"Amplifies and distorts the input signal by a variable amount",
	"The accent modules outputs the user-specified voltage when a sequencer note\nbeing played has the accent mark. It can be used to change the volume, resonance or any other feature of the note.",
	"Outputs the signal to the master mixer at the selected output level",
	"Bitcrusher restricts the sample resolution by a variable amount",
	"Slew limiter is a specialized low-pass filter typically used for\nglide/portamento",
	"Modulator control voltage (CV) allows you to tap KBD CV from\nother channels to use for modulation"
};


// number of dwords a module requires for buffer. ptr goes always to
// first dword of the localdata
const int modDataBufferLength[MODTYPES]={
	0, //CV
	0, //ADSR
	0, //wave
	0, //lfo
	0, //knob
	0, //amp
	0, //mixer
	0, //filter
	0, //lpf24
	5*OUTPUTFREQ, //delay
	0, //scaler
	0, //compressor
	0, //switch
	0, //distort
	0, //accent
	0, //output
	0, //bitcrush
        0, //slew
        0  //modulator
};


// Number of input nodes on modules
const int modInputCount[MODTYPES]={
	0, //CV
	4, //ADSR (attack, decay, sustain, release)
	4, //wave (frequency, pwm, subosc, noise)
	3, //lfo (frequency, ampl, bias)
	0, //knob
	2, //ampmod (input signal, amplification)
	4, //mixer (four input signals)
	3, //filter (input signal, cutoff, resonance)
	3, //lpf24 (input signal, cutoff, resonance)
	4, //delay (input signal, delay, loop)
	1, //scaler (input signal)
	2, //resample (input signal, samplerate)
	3, //supersaw (pitch, detune, mix)      //4, //switch
	2, //distort (input signal, distortion)
	0, //accent
	1, //output (input signal)
	2, //bitcrush (input signal, amount)
	2, //slew (input, amount)
	0  //modulator
};


// Labels for all input nodes on modules
const char* modInputNames[MODTYPES][4]={
{"","","",""},
{"A", "d", "s", "r"},
{"frq", "pwm", "sub", "nse"},
{"frq", "AmpL", "bias", ""},
{"","","",""},
{"in", "Amp", "", ""},
{"in1", "in2", "in3", "in4"},
{"in", "fc", "rEs", ""},
{"in", "fc", "rEs", ""},
{"in", "timE", "loop", "fb"},
{"in", "", "", ""},
{"in", "rate", "", ""},
{"frq", "dtune", "mix", ""},
{"in", "dist", "", ""},
{"","","",""},
{"in", "", "", ""},
{"in", "dep", "", ""},
{"in", "amt", "", ""},
{"", "", "", ""}
};


// expected scale of each input on the modules
const int modInputScale[MODTYPES][4]={
{0, 0, 0, 0},
{SCALE_RAMP, SCALE_RAMP, SCALE_PERCENTAGE, SCALE_RAMP},
{SCALE_FREQUENCY_HZ, SCALE_PERCENTAGE, SCALE_PERCENTAGE, SCALE_PERCENTAGE},
{SCALE_FREQUENCY_HZ, 0, 0, 0},
{0, 0, 0, 0},
{0, 0, 0, 0},
{0, 0, 0, 0},
{0, SCALE_FREQUENCY_HZ, SCALE_PERCENTAGE, 0},
{0, SCALE_FREQUENCY_HZ, SCALE_PERCENTAGE, 0},
{0, SCALE_DURATION, SCALE_DURATION, SCALE_PERCENTAGE},
{0, 0, 0, 0},
{0, SCALE_FREQUENCY_HZ, 0, 0},
{SCALE_FREQUENCY_HZ, SCALE_PERCENTAGE, SCALE_PERCENTAGE, 0},
{0, SCALE_PERCENTAGE, 0, 0},
{0, 0, 0, 0},
{0, 0, 0, 0},
{0, SCALE_PERCENTAGE, 0, 0},
{0, SCALE_PERCENTAGE, 0, 0},
{0, 0, 0, 0}
};


// output signal type of each module type, 0 if use is varying
const int modOutputScale[MODTYPES]={
  SCALE_FREQUENCY_HZ,
  SCALE_PERCENTAGE,
  SCALE_SIGNAL_AUDIO,
  SCALE_PERCENTAGE,
  0, // knob
  0, // amp
  0, // mixer
  SCALE_SIGNAL_AUDIO,
  SCALE_SIGNAL_AUDIO,
  SCALE_SIGNAL_AUDIO,
  0,
  SCALE_SIGNAL_AUDIO,
  SCALE_SIGNAL_AUDIO,
  SCALE_SIGNAL_AUDIO,
  SCALE_PERCENTAGE,
  0,
  0,
  0,
  SCALE_FREQUENCY_HZ
};


// modulator names for each module
const char* modModulatorNames[MODTYPES]={
"pitch",
"",
"waveform",
"waveform",
"mod",
"",
"",
"mode",
"",
"mode",
"LeveL",
"",
"input",
"",
"velocity",
"level",
"",
"lin/log",
"channel"
};

// modulator value type. 0=no modulator, 1=float, 2=integer
const int modModulatorTypes[MODTYPES]={
	1, //CV
	0, //ADSR
	3, //wave
	4, //lfo
	1, //knob
	0, //amp
	0, //mixer
	5, //filter
	0, //lpf24
	6, //delay
	1, //attenuator
	0, //resample
	0, //supersaw
	0, //distort
	1, //accent
	1, //output
	0, // bitcrush
        7, // slew
        8  // modulator
};



 char* modVcoWaveforms[VCO_WAVEFORMS]={"Pulse", "Saw", "Triangle", "Sine"}; //, "Noise"};
 char* modLfoWaveforms[LFO_WAVEFORMS]={"Square", "Saw", "Triangle", "Sine"};
 char* modVcfModes[VCF_MODES]={"Off", "Lowpass", "Highpass", "Bandpass"};
 char* modDelayModes[DELAY_MODES]={"Comb filter", "Allpass filter"};
 char* modSlewModes[SLEW_MODES]={"Linear", "Logarithmic"};

// input node positions [maxnodes][node]
 float node_xoffset[5][4]={
  {0, 0, 0, 0},
  {-(MODULE_HALF+0.5), 0, 0, 0},
  {0, 0, 0, 0},
  {-(MODULE_HALF+0.5), 0, 0, 0},
  {-MODULE_QUARTER, MODULE_QUARTER, -MODULE_QUARTER, MODULE_QUARTER}
};
 float node_yoffset[5][4]={
  {0, 0, 0, 0},
  {0+OUTPUT_OFFSET, 0, 0, 0},
  {-(MODULE_HALF+0.5), (MODULE_HALF+0.5), 0, 0},
  {0+OUTPUT_OFFSET, -(MODULE_HALF+0.5), (MODULE_HALF+0.5), 0},
  {-(MODULE_HALF+0.5), -(MODULE_HALF+0.5), (MODULE_HALF+0.5), (MODULE_HALF+0.5)}
};

// input node label positions
 float node_labelpos[5][4]={
  {0,0,0,0}, {3,0,0,0}, {0,2,0,0}, {3,0,2,0}, {0,0,2,2}
};
                
// output node positions depending on how many input nodes there are
 int node_outputCount[5]={4, 3, 2, 1, 2};
 int node_outputList[5][4]={
  {0, 1, 2, 3}, {0, 2, 3, -1}, {0, 1, -1, -1}, {0, -1, -1, -1}, {0, 1, -1, -1} };
 int node_outputxoffset[4]={ //left, right, top, bottom
  (MODULE_HALF)+0.5, -(MODULE_HALF)+0.5, MODULE_HALF, MODULE_HALF};
 int node_outputyoffset[4]={
  MODULE_HALF-OUTPUT_OFFSET, MODULE_HALF+OUTPUT_OFFSET, -MODULE_HALF, MODULE_HALF};



//
// init for supersaw tables
//

void calc_supersaw_tables() {
  double x, y;
  int mod, osc, e;

  for (mod=0; mod<128; mod++) {
    x=(float)(mod) / 127.0;
    supersaw_mix[mod][0]=-0.55366*x + 0.99785;
    for(y=0,e=0;e<12;e++) y+=coeftable[e]*pow(x, e);
    for(osc=0; osc<7; osc++) {
      supersaw_detune[mod][osc]=1.0+osc_offset[osc]*y;
      if (osc) supersaw_mix[mod][osc]=-0.73764*x*x + 1.2841*x + 0.044372;
    }
  }
}




////////////////////////////////////////////////
//
// module functions
//
///////////////////////////////////////////////


MODULE_FUNC(kbd) { return *mod=pitch[v]/OUTPUTFREQ; }

MODULE_FUNC(modulator) {
  int mod_src=(int)(*mod);
  if (mod_src < 0 || mod_src >= seqch) {
    mod_src=v;
  }
  float cv=pitch[mod_src]/OUTPUTFREQ;
  //printf("modulator: channelnum %d mod source %d cv %f\n", v, mod_src, cv);
  return cv;
}

MODULE_FUNC(output) { return ms[0]*(*mod); }


MODULE_FUNC(accent) {
  return accent[v] ? *mod : 0.0;
}


MODULE_FUNC(vco) // phase-accumulating oscillator w/ suboscillator
{
  float out=0.0;
  
  mod_fdata[0]+=ms[0];
  mod_fdata[0]-=floor(mod_fdata[0]);

  // advance subosc
  mod_fdata[1]+=ms[0]/2;
  mod_fdata[1]-=floor(mod_fdata[1]);

  // hard restart
  if (restart[v]&SEQ_RESTART_VCO) { mod_fdata[0]=0; mod_fdata[1]=0; }

  switch((int)(*mod))
  {
    case VCO_PULSE:    out=(mod_fdata[0] < ms[1]) ? -1.0 : 1.0; break;
    case VCO_SAW:      out=(mod_fdata[0] * 2 - 1.0f); break;
    case VCO_TRIANGLE: out=(mod_fdata[0]<0.75) ? 1-fabs(mod_fdata[0]*4-1) : 1-fabs(mod_fdata[0]*4-5); break;
    case VCO_SINE:     out=sin(2*3.1415926* mod_fdata[0]); break;
    break;
  }

  // suboscillator (pulse at -1 octave)
  out+=ms[2]*((ms[1]<mod_fdata[1])?-1.0:1.0);

  // noise
  noise_x1^=noise_x2;
  out+=ms[3]*(noise_x2*(2.0f/0xffffffff));
  noise_x2+=noise_x1;
  
  return out;
}


MODULE_FUNC(lfo) { // low-frequency oscillator, input is freq in hz, cv output (0 to 1.0)
  float out=0.0;

  // hard restart
  if (restart[v]&SEQ_RESTART_LFO) { mod_fdata[0]=0; mod_fdata[1]=0; }
  
  mod_fdata[0]+=ms[0];
  mod_fdata[0]-=floor(mod_fdata[0]);

  switch((int)(*mod)) {
    case LFO_TRIANGLE: out=2*mod_fdata[0]; if (out>1.0) out=2-out; break;
    case LFO_SINE:     out=-0.5*(cos(2*3.1415926*mod_fdata[0])-1); break;
  }
  out*=ms[1];
  out+=ms[2];
  
  return out;
}


MODULE_FUNC(env) // linear adsr envelope generator
{
  // hard restart
  if (restart[v]&SEQ_RESTART_ENV) { mod_fdata[0]=0; mod_ldata[1]=0; }
  
  if (gate[v]) {
    if (!mod_ldata[1]) mod_ldata[2]=1; // trig if gate went 0->1
  if (mod_ldata[2]) {
      mod_fdata[0]+=ms[0]; if (mod_fdata[0]>=1.0) {
       mod_ldata[2]=0;
       mod_fdata[0]=1.0; } // attack
    } else {
      mod_fdata[0]-=ms[1]; if (mod_fdata[0]<ms[2]) mod_fdata[0]=ms[2]; // decay+sustain
    }
  } else {
    mod_fdata[0]-=ms[3]; if (mod_fdata[0]<0.0) mod_fdata[0]=0.0; // release
  }
  mod_ldata[1]=gate[v]; // save current gate
  return mod_fdata[0];
}


MODULE_FUNC(vcf) // 12db/oct resonant state variable low-/high-/bandpass filter
{
  float f, q, r, out;
  // in1=signal in, in2=cutoff 0.0~1.0 (=0-fs), in3=resonance 0.0~1.0

  // safety nets to keep the filter from going nuts
  if (ms[1]>1.0) ms[1]=1.0;
  if (ms[1]<0.0) ms[1]=0.0;
  if (ms[2]>1.0) ms[2]=1.0;
  if (ms[2]<0.0) ms[2]=0.0;

  // float *data -> 0=lpf, 1=hpf, 2=bpf
  f = 2*sin(3.14159 * ms[1]); // cutoff in [0.0, 1.0]
  q=1.0-ms[2];
  r=sqrt(q);
  mod_fdata[0] = mod_fdata[0] + f * mod_fdata[2];
  mod_fdata[1] = r * ms[0] - mod_fdata[0] - q * mod_fdata[2];
  mod_fdata[2] = f * mod_fdata[1] + mod_fdata[2];

  // generate filter output
  out=0.0;
  switch((int)(*mod)) {
    case VCF_OFF:      out=ms[0]; break;
    case VCF_LOWPASS:  out+=mod_fdata[0]; break;
    case VCF_HIGHPASS: out+=mod_fdata[1]; break;
    case VCF_BANDPASS: out+=mod_fdata[2]; break;
  }
  
  return out;
}




MODULE_FUNC(delay)
{
  float *buffer, out, spfrac;
  s32 writeptr, readptr, loopend, ptrdelta;

  buffer=mod_fpdata[0]; // data[0] is a ptr to a float buffer
  writeptr=mod_ldata[2];

  if (!buffer) return ms[0]; // failsafe - return the input if no buffer

  // delay and loop in samples
  loopend=3*OUTPUTFREQ; // 3sec maximum
  if (ms[2]>1) loopend=ms[2]; // use loop input if greater than 1 sample
  ptrdelta=(s32)(ms[1]); // truncate fractional part
  spfrac=ms[1]-(float)(ptrdelta);

  readptr=(writeptr - ptrdelta);
  while (readptr<0) readptr+=loopend;
  out=buffer[readptr]*spfrac;
  readptr++; readptr%=loopend;
  out+= buffer[readptr]*(1-spfrac);

  if ((int)(*mod)==DELAY_ALLPASS) out+=ms[0]*(-ms[3]); // feedforward for allpass
  buffer[writeptr]=ms[0] + out*ms[3]; 

  mod_ldata[2]=(writeptr+1)%loopend;

  return out;
}


MODULE_FUNC(dist)  { // simple clipping distort

  float out;
  out=ms[0];
  out*=ms[1]; // ampl
  if (fabs(out)>1.0) out = out/fabs(out);
  return out;

// alternative implementations
/*
  float a,k,x,y;
  a=clamp(ms[1]);
  if (a>=1.0f) a=0.9999f;
  k=2*a/(1-a);
  x=ms[0];
  y=(1+k)*x/(1+k*abs(x));
  return y;
*/
/*
  float y,x;
  y=ms[0]*ms[1];
  x=fabs(y);
  if (x>1.0) x=(2.0 - 1.0/x);
  x=(y/fabs(y));
  return y;
*/
}


MODULE_FUNC(resample) { // sample-and-hold
  // ms[0] is input signal
  // ms[1] is sample rate as accumulator delta

  mod_fdata[0]-=ms[1];
  if (mod_fdata[0]<0) {
    mod_fdata[1]=ms[0]; // sample from input
    mod_fdata[0]=1.0f; // reset accumulator
  }
  return mod_fdata[1];
}


// simple basic operators
MODULE_FUNC(cv) { return *mod; }
MODULE_FUNC(amp) { return ms[0]*ms[1]; }
MODULE_FUNC(att) { return ms[0]* *mod; }
MODULE_FUNC(mixer) { return ms[0]+ms[1]+ms[2]+ms[3]; }



MODULE_FUNC(lpf24) { // 24db/oct four-pole low pass
  // ms[0]=signal in, ms[1]=cutoff (0..1), ms[2]=resonance (0..1)
  
  // safety nets to keep the filter from going nuts
  if (ms[1]>1.0) ms[1]=1.0;
  if (ms[1]<0.0) ms[1]=0.0;
  if (ms[2]>1.0) ms[2]=1.0;
  if (ms[2]<0.0) ms[2]=0.0;

  double f = ms[1]*1.16*3;
  double fb = (ms[2]*4.0) * (1.0 - 0.15 * f * f);
  double input = ms[0] - mod_ddata[3] * fb;
  input *= 0.35013 * (f*f)*(f*f);

  mod_ddata[0] = input        + 0.3 * mod_ddata[4] + (1 - f) * mod_ddata[0]; // Pole 1
  mod_ddata[4] = input;
  mod_ddata[1] = mod_ddata[0] + 0.3 * mod_ddata[5] + (1 - f) * mod_ddata[1]; // Pole 2
  mod_ddata[5] = mod_ddata[0];
  mod_ddata[2] = mod_ddata[1] + 0.3 * mod_ddata[6] + (1 - f) * mod_ddata[2]; // Pole 3
  mod_ddata[6] = mod_ddata[1];
  mod_ddata[3] = mod_ddata[2] + 0.3 * mod_ddata[7] + (1 - f) * mod_ddata[3]; // Pole 4
  mod_ddata[7] = mod_ddata[2];
  
  return mod_ddata[3]; //out4;
}


/* waveshaper
float waveshape_distort( float in ) {
  return 1.5f * in - 0.5f * in *in * in;
  }
*/


// bitcrush with variable step size
MODULE_FUNC(bitcrush) {
  // ms[0]=input, ms[1]=depth 0..1 where 0=1bit and 1=16bit
  int rate=ms[1]*32766+1;
  int in=ms[0]*32767;
  int step=in%rate;
  in-=step;
  return (float)(in)/32767;
}


/*
MODULE_FUNC(compressor)
{
}
*/

MODULE_FUNC(slew) { // slew limiter: ms[0] = cv input, ms[1] = rate
  float k;
  float out=ms[0];
  float fp=mod_ddata[0];
  
  // lin/log mode
  k=((int)(*mod)) ? -log2(1-ms[1]) : ms[1];
  
  fp=out*k+fp*(1-k); out=fp;
//  fp=(fp+out)*k;
  mod_ddata[0]=fp; // save fp for next sample
//  out+=fp;
//  out*=(1-k);
  return out;
}


MODULE_FUNC(envdet) { // envelope follower: ms[0] = input, ms[1] = attack, ms[2] = release
//  float attack_coef = exp(log(0.01)/( ms[1] * OUTPUTFREQ * 0.001));
//  float release_coef = exp(log(0.01)/( ms[2] * OUTPUTFREQ * 0.001));

  // attack and release inputs are in duration (sec) scale
  float attack_coef = exp(log(0.01)/ms[1]);
  float release_coef = exp(log(0.01)/ms[2]);

  float tmp=fabs(ms[0]);
  if(tmp > mod_fdata[0])
    mod_fdata[0] = attack_coef * (mod_fdata[0] - tmp) + tmp;
  else
    mod_fdata[0] = release_coef * (mod_fdata[0] - tmp) + tmp;

  return mod_fdata[0];
}


/*
float sawtooth(float ac) {
//  return -1.0 + 2.0*ac;
  return -1.0 + 2.0*sqrt(ac);
}
*/

#define sawtooth(ac)	(1.0+2.0*sqrt(ac))
MODULE_FUNC(supersaw) {
  float f, q, r;
  float out=0;
  int i;
  float m_pitch;
  int m_mix, m_detune;

  m_pitch=ms[0];
  m_detune=(127 * clamp(ms[1]));
  m_mix=(127 * clamp(ms[2]));

  // generate waveform and step accumulators
  out=0;
  for(i=0;i<7;i++) {
    out+=sawtooth(mod_fdata[i])*supersaw_mix[m_mix][i];
    mod_fdata[i]+=supersaw_detune[m_detune][i]*m_pitch;
    mod_fdata[i]-=floor(mod_fdata[i]);
  }

  // highpass
  f = 2*sin(3.14159 * m_pitch); // cutoff in [0.0, 1.0]
  q=1.0 - 0.2; // resonance is 0.2
  r=sqrt(q);
  mod_fdata[8] = mod_fdata[8] + f * mod_fdata[10];
  mod_fdata[9] = r * out - mod_fdata[8] - q * mod_fdata[10];
  mod_fdata[10] = f * mod_fdata[9] + mod_fdata[10];
  out = mod_fdata[9];
  
  return out;
}




// module function call table
float (*mod_functable[MODTYPES])(unsigned char, float*, void*, float*)={
		modfunc_kbd,
		modfunc_env,
		modfunc_vco,
		modfunc_lfo,
		modfunc_cv,
		modfunc_amp,
		modfunc_mixer,
		modfunc_vcf,
		modfunc_lpf24,
		modfunc_delay,
		modfunc_att,
		modfunc_resample,
		modfunc_supersaw,
		modfunc_dist,
		modfunc_accent,
		modfunc_output,
		modfunc_bitcrush,
                modfunc_slew,
                modfunc_modulator
};

