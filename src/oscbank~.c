// ------------------------ oscbank~ 0.2 -----------------------------
// oscillator bank using 3 seperate float inlets with interpolation
// - Please see the included README and LICENSE for more info
// - reakinator@gmail.com
// -------------------------------------------------------------------

#include "m_pd.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define DEFAULT_WAVETABLESIZE 65536 // 2^16
#define DEFAULT_NPARTIALS 100
#define DEFAULT_SAMPLERATE 44100
#define DEFAULT_INTERP_MS 20.0f
#define NEAR_ZERO 0.0000001f

// t_partial represents one oscillator (partial) in the bank
typedef struct _partial
{
	int   index;
	float fCurr; // current freq / amp (sample level)
	float aCurr;
	float freq;  // target freq / amp
	float amp;
	float fIncr; // amount freq / amp to increment per sample
	float aIncr;
	float phase; // stored phase
	unsigned long  nInterp; // number of samples left for interpolation (0 = target reached)
} t_partial;

typedef struct _oscbank
{
	t_object x_obj;
	t_outlet *list_outlet;
	t_partial **pBank;
	t_atom   *outv;
	float    *wavetable;
	int      wavetablesize;
	int      got_a_table;
	float    infreq;
	float    inamp;
	float    sampleRate;
	float    sampleperiod;
	float    interp_incr;
	long     interpSamples;
	int      nPartials;
} t_oscbank;

static t_class *oscbank_class;
static void *oscbank_new(void);
static void oscbank_free(t_oscbank *x);
static void oscbank_interpMs(t_oscbank *x, t_floatarg n);
static void oscbank_nPartials(t_oscbank *x, t_floatarg n);
static void oscbank_index(t_oscbank *x, t_floatarg in);
static void oscbank_delta(t_oscbank *x, t_floatarg index, t_floatarg freq, t_floatarg amp);
static void oscbank_table(t_oscbank *x, t_symbol *tablename);
static void oscbank_print(t_oscbank *x);
static void oscbank_outlist(t_oscbank *x);
static void oscbank_dsp(t_oscbank *x, t_signal **sp);
static void oscbank_reset(t_oscbank *x);

static void resize_partials(t_oscbank *x, int old, int new);

inline static float wavetable_read(float *wavetable, float lookup)
{
#if defined(WAVETABLE_INTERP)
	float frac, integral, f1, f2;
	frac = modff(lookup, &integral);
	wavetable += (int)integral;
	f1 = wavetable[0];
	f2 = wavetable[1];
	return f1 + frac * (f2 - f1);
#else
	return wavetable[(int)lookup];
#endif
}

// ------------------- Setup / Teardown ------------------------------

void oscbank_tilde_setup(void)
{
	oscbank_class = class_new(gensym("oscbank~"), (t_newmethod)oscbank_new, (t_method)oscbank_free,
			sizeof(t_oscbank), 0, A_DEFFLOAT, 0);
	class_addfloat(oscbank_class,  oscbank_index);
	class_addmethod(oscbank_class, (t_method)oscbank_delta, gensym("delta"), A_FLOAT, A_FLOAT, A_FLOAT, 0 );
	class_addmethod(oscbank_class, (t_method)oscbank_table, gensym("table"), A_SYMBOL);
	class_addmethod(oscbank_class, (t_method)oscbank_interpMs, gensym("interp"), A_FLOAT, 0);
	class_addmethod(oscbank_class, (t_method)oscbank_dsp, gensym("dsp"), (t_atomtype)0);
	class_addmethod(oscbank_class, (t_method)oscbank_print, gensym("print"), 0);
	class_addmethod(oscbank_class, (t_method)oscbank_outlist, gensym("sendout"), 0);
	class_addmethod(oscbank_class, (t_method)oscbank_reset, gensym("reset"), 0);
	class_addmethod(oscbank_class, (t_method)oscbank_nPartials, gensym("partials"), A_FLOAT, 0);
}

static void *oscbank_new(void)
{
	t_oscbank *x = (t_oscbank *)pd_new(oscbank_class);

	float twopi;
	int i;

	outlet_new(&x->x_obj, gensym("signal"));
	x->list_outlet = outlet_new(&x->x_obj, gensym("list"));

	floatinlet_new(&x->x_obj, &x->infreq);
	floatinlet_new(&x->x_obj, &x->inamp);
	inlet_new(&x->x_obj, &x->x_obj.ob_pd, gensym("float"), gensym("interp"));

	// hardcoded samplerate prevents divide by zero in oscbank_index(), but will be updated when dsp is switched on
	x->sampleRate = DEFAULT_SAMPLERATE;
	x->sampleperiod = 1.0f / x->sampleRate;
	oscbank_interpMs(x, DEFAULT_INTERP_MS);

	x->got_a_table = 0;
	x->nPartials = DEFAULT_NPARTIALS;
	x->pBank = NULL;
	x->outv = NULL;
	resize_partials(x, 0, x->nPartials);

	twopi = 8.0f * atan(1.0f);
	x->wavetablesize = DEFAULT_WAVETABLESIZE;
	float *sinewave;
	sinewave = (t_float *)malloc(x->wavetablesize * sizeof(t_float));
	for(i = 0; i < x->wavetablesize; i++) {
		sinewave[i] = sin(twopi * (float)i/ x->wavetablesize);
	}
	x->wavetable = &sinewave[0];
	return (x);
}

static void oscbank_free(t_oscbank *x)
{
	int i;
	for (i = 0; i < x->nPartials; i++) {
		free(x->pBank[i]);
	}
	free(x->pBank);
	
	if (x->outv) {
		free(x->outv);
	}
	
	if(!x->got_a_table) {
		free(x->wavetable);
	}
}

// ------------------- External Message Routines --------------------

/*
 * Interpolation Time:
 * milleseconds to interpolate over; so samples = (n*SR)/1000
 * divide only when converting the interp time to samples(here),since it 
 * is only used as a denominator to find the increment proportion:
 * SP= 1/SR, 1/(n*SR/1000) = (1000*SP)/n
 */
static void oscbank_interpMs(t_oscbank *x, t_floatarg n)
{
	if(n > 0) {
		x->interp_incr = (1000 * x->sampleperiod) / n ;	
	} else {
		x->interp_incr = x->sampleperiod; 	
	}
	x->interpSamples = (unsigned long)((n *.001) * x->sampleRate);
}

static void oscbank_nPartials(t_oscbank *x, t_floatarg n)
{
	if (n <= 0) {
		pd_error(x, "number of partials must be greater than zero.");
		return;
	}
	resize_partials(x, x->nPartials, n);
	x->nPartials = n;
	post("max partials: %d", x->nPartials);
}

static void oscbank_index(t_oscbank *x, t_floatarg in)
{
	int i, iindex;
	iindex = (int)in;
	int empty_index = -1;
	int quietest_index = 0;
	float freq = fabsf(x->infreq);

	if( iindex < 0)	{
		error("oscbank~ needs a positive index.");
		return;
	}

	//check if it is continuing partial
	//recaluclate increment slope from current interpolated positions and update goal
	for(i = 0; i < x->nPartials; i++) {
		t_partial *partial = x->pBank[i];
		if(partial->index == iindex) {
			if(partial->aCurr <= NEAR_ZERO) {
				partial->aCurr = NEAR_ZERO; // enables synthesis if amp was zeroed out
			}
			partial->fIncr = (freq - partial->fCurr) * x->interp_incr;
			partial->aIncr = (x->inamp - partial->aCurr) * x->interp_incr;
			partial->freq = freq;
			partial->amp = x->inamp;
			partial->nInterp = x->interpSamples;
			return;
		}
		// store index for first empty
		if(empty_index < 0 && partial->aCurr < NEAR_ZERO) {
			empty_index = i;
		}
		// store index for quietest partial
		if(partial->amp < x->pBank[quietest_index]->amp) {
			quietest_index = i;
		}
	}

	if(empty_index >= 0) {
		//ramp amp from zero
		t_partial *partial = x->pBank[empty_index];
		partial->index = iindex;
		partial->nInterp = x->interpSamples;
		partial->fIncr = 0;
		partial->freq = freq;
		partial->fCurr = freq;
		partial->amp = x->inamp;
		partial->aCurr = NEAR_ZERO;
		partial->aIncr = x->inamp * x->interp_incr;
		return;
	}

	//oscbank is full, steal quietest partial and ramp amp from zero
	t_partial *partial = x->pBank[quietest_index];
	partial->index = iindex;
	partial->nInterp = x->interpSamples;
	partial->fIncr = 0;
	partial->freq = freq;
	partial->fCurr = freq;
	partial->amp = x->inamp;
	partial->aCurr = NEAR_ZERO;
	partial->aIncr = x->inamp * x->interp_incr;
}

static void oscbank_delta(t_oscbank *x, t_floatarg index, t_floatarg freq, t_floatarg amp)
{
	int i, iindex;
	iindex = (int)index;

	for(i = 0; i < x->nPartials; i++) {
		t_partial *partial = x->pBank[i];
		if(partial->index == iindex) {
			float newfreq = fabsf(partial->freq + freq);
			float newamp = fabsf(partial->amp + amp);
			partial->fIncr = (newfreq - partial->fCurr) * x->interp_incr;
			partial->aIncr = (newamp - partial->aCurr) * x->interp_incr;
			partial->freq = newfreq;
			partial->amp = newamp;
			partial->nInterp = x->interpSamples;
			return;
		}
	}
}

static void oscbank_table(t_oscbank *x, t_symbol *tablename)
{
	if(!x->got_a_table) {
		free(x->wavetable);
		x->got_a_table = 0;
	}

	t_garray *a;
	if (!(a = (t_garray *)pd_findbyclass(tablename, garray_class))) {
		pd_error(x, "%s: no such array", tablename->s_name);
	} else if (!garray_getfloatarray(a, &x->wavetablesize, &x->wavetable)) {
		pd_error(x, "%s: bad template for tabread", tablename->s_name);
	} else {
		post("wavetablesize: %d", x->wavetablesize );
		x->got_a_table = 1;
	}
}

static void oscbank_print(t_oscbank *x)
{
	post("#:  Index,  Freq,  Amp");

	int i;
	for(i = 0; i < x->nPartials; i++) {
		t_partial *partial = x->pBank[i];
		if(partial->aCurr) {
			post("%d: %d, %.2f, %.2f", i, partial->index, partial->fCurr,  partial->aCurr);
		}
	}
}

static void oscbank_outlist(t_oscbank *x)
{
	int audiblePartials = 0;
	if (!x->outv) {
		x->outv = (t_atom *)malloc(x->nPartials * 3 * sizeof(t_atom));
	}
	if(!x->outv) {
		pd_error(x, "could not allocate memory for t_atom list");
		return;
	}

	int i, offset;
	for(i = 0; i < x->nPartials; i++) {
		t_partial *partial = x->pBank[i];
		if(partial->aCurr) {
			offset = audiblePartials++ * 3;
			SETFLOAT(x->outv + offset, (float)partial->index);
			SETFLOAT(x->outv + offset + 1, partial->fCurr);
			SETFLOAT(x->outv + offset + 2, partial->aCurr);
		}
	}
	outlet_list(x->list_outlet, &s_list, audiblePartials * 3, x->outv); // only send out atoms that are filled
}

static void oscbank_reset(t_oscbank *x)
{
	int i;
	for (i = 0; i < x->nPartials; i++) {
		memset(x->pBank[i], 0, sizeof(t_partial));
		x->pBank[i]->index = -1;
	}
}

//------------------------- DSP routines ---------------------------------------

static t_int *oscbank_perform(t_int *w)
{
	t_oscbank *x = (t_oscbank *)(w[1]);
	t_float *out = (t_float *)(w[2]);
	t_int n = (t_int)(w[3]);
	t_int i, sample;
	t_float phaseincrement;
	
	t_int wavetable_mul = x->wavetablesize - 1;
	t_float *wavetable = x->wavetable;
	
	memset(out, 0, n * sizeof(t_float));

	for(i = 0; i < x->nPartials; i++) {
		t_partial *partial = x->pBank[i];
		if(partial->amp > NEAR_ZERO || partial->aCurr > 0.0f) {
			for(sample = 0; sample < n; sample++) {
				if(partial->nInterp > 0) {
					partial->fCurr += partial->fIncr;
					partial->aCurr += partial->aIncr;
					--partial->nInterp;
				} else {
					partial->fCurr = partial->freq;
					partial->aCurr = partial->amp;
				}

				// get the phase increment freq = cyc/sec,
				// sr = samp/sec, phaseinc = cyc/samp = freq/sr = freq * sampleperiod
				phaseincrement = partial->fCurr * x->sampleperiod;
				partial->phase += phaseincrement;
				if(partial->phase >= 1.0f) {
					partial->phase = fmodf(partial->phase, 1.0f);
				}
				*(out+sample) += wavetable_read(wavetable, wavetable_mul * partial->phase) * partial->aCurr;
			}
		}
	}
	return (w+4);
}

static void oscbank_dsp(t_oscbank *x, t_signal **sp)
{
	x->sampleRate =  sp[0]->s_sr;
	x->sampleperiod = 1 / x->sampleRate;
	dsp_add(oscbank_perform, 3, x, sp[0]->s_vec, sp[0]->s_n);
}

//------------------------- Private routines ---------------------------------------

static void resize_partials(t_oscbank *x, int old, int new)
{
	int i;
	if (new > old) {
		x->pBank = realloc(x->pBank, new * sizeof(t_partial));
		for (i = old; i < new; i++) {
			x->pBank[i] = (t_partial *)calloc(1, sizeof(t_partial));
			x->pBank[i]->index = -1; // invalidate
		}
	} else if (new < old) {
		for (i = new; i < old; i++) {
			free(x->pBank[i]);
			// extra memory in x->pBank will be freed in oscbank_free()
		}
	}
	
	if (x->outv) {
		free(x->outv);
		x->outv = NULL;
	}
}

