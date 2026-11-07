/*
 * Copyright (c) 2002, Papyrus Racing Games, Inc.
 * All rights reserved.
 *
 * Date			Who		What
 * 06-apr-02	RJC 	Created
 *
 * Description:
 *	A small test harness for the Papyrus telemetry system.
 *	It is the intent that you can place this file, along
 *	with papytelemapp.obj and papytelemapp.h in the same
 *	directory, and get a valid executable when you issue
 *		cl /W3 /MT tsample.cpp papytelemapp.obj
 *	(/W3 turns on DevStudio's second-highest warning level,
 *	and /MT says to link with the multi-threaded version
 *	of the C run-time library.  /W4 would work but for
 *	warning 4127 - conditional expression is constant).
 *	If you then run tsample.exe (first) and the sim
 *	(second), this program will read telemetry from the
 *	sim, and write some of it out to its console window.
 *
 *	What this program will output depends on the
 *	"formatForSpreadsheet" variable below.  Change it as
 *	desired.
 *
 */


/*
 * Include files
 */
#include <windows.h>
#include <stdio.h>
#include <math.h>
#include "papytelemapp.h"


/*
 * Miscellaneous definitions local to this file
 */

// So that we don't have to qualify our use of the telem API
// components with "PapyTelem::".  
using namespace PapyTelem;



/*
 * Local/static functions defined in this file (prototype only; implement below)
 */


/*
 * Local/static variables defined in this file
 */

// Set this to true if you want the program to output a tab-delimited
// text file appropriate for importing into a spreadsheet.  You would
// most likely want to redirect this programs output to a text file
// (e.g.  C:\test> tsample > telemetry.txt).
// Set this to false if you want the program to output simple text
// messages that show what kind of telemetry the sim is generating.
const bool formatForSpreadsheet = true;

// Field separator for spreadsheet format.  '\t' and ',' make sense.
const char	S = '\t';

// If you set this to be true, then the telemetry output will be in
// english units.  Otherwise they are output in SI units.
const bool englishUnits = true;

// When false, sampling is done at the game tick rate (36Hz, currently).
// When true, sampling is done at the physics rate (288Hz, currently).
const bool sampleAtPhysicsRate = true;

// Selected field for piped output (0=all, 1=speed, 2=rpm, 3=throttle, 4=brake, 5=gear, 6=trkPct, 7=steerT, 8=longG, 9=latG, 10=normG)
int selectedField = 0;


/*
 * Global/public variables defined
 */




/*
 ***************************************************************************
 * Some utility functions
 ***************************************************************************
 */
static void matrix33FromEuler(
	float		m[3][3],
	float		yaw,
	float		pitch,
	float		roll)
{
	float cy = (float) cos(yaw);
	float cp = (float) cos(pitch);
	float cr = (float) cos(roll);
	float sy = (float) sin(yaw);
	float sp = (float) sin(pitch);
	float sr = (float) sin(roll);
	
	m[0][0] = cp*cy;
	m[0][1] = cy*sp*sr - cr*sy;
	m[0][2] = cr*cy*sp + sr*sy;
	m[1][0] = cp*sy;
	m[1][1] = cr*cy + sp*sr*sy;
	m[1][2] = cr*sp*sy - cy*sr;
	m[2][0] = -sp;
	m[2][1] = cp*sr;
	m[2][2] = cp*cr;
}

// Multiply vector by matrix transpose
static void matrix33MulTVector3(
	float		r[3],
	const float	m[3][3],
	const float	v[3])
{
	float		tmp[3];
	float		*o;

	o = (r == v) ? tmp : r;

	o[0] = (m[0][0]*v[0]) + (m[1][0]*v[1]) + (m[2][0]*v[2]);
	o[1] = (m[0][1]*v[0]) + (m[1][1]*v[1]) + (m[2][1]*v[2]);
	o[2] = (m[0][2]*v[0]) + (m[1][2]*v[1]) + (m[2][2]*v[2]);

	if (o != r) memcpy(r, o, sizeof(tmp));
}

static void vector3Add(
	float		r[3],
	const float	v0[3],
	const float	v1[3])
{
	r[0] = v0[0] + v1[0];
	r[1] = v0[1] + v1[1];
	r[2] = v0[2] + v1[2];
}

static void vector3Sub(
	float		r[3],
	const float	v0[3],
	const float	v1[3])
{
	r[0] = v0[0] - v1[0];
	r[1] = v0[1] - v1[1];
	r[2] = v0[2] - v1[2];
}

static void vector3Scale(
	float		r[3],
	float		s,
	const float	v[3])
{
	r[0] = v[0] * s;
	r[1] = v[1] * s;
	r[2] = v[2] * s;
}

/*
 ***************************************************************************
 * Routines to generate the program's output.
 ***************************************************************************
 */
static const char *sessionType(int t)
{
	switch(t)
	{
		case kSessionTypeInvalid :		return "invalid";
		case kSessionTypeTesting :		return "testing";
		case kSessionTypePractice :		return "practice";
		case kSessionTypeQualifyLone :	return "lone qualifying";
		case kSessionTypeQualifyOpen :	return "open qualifying";
		case kSessionTypeRace :			return "race";
	}
	return "?";
}

static const char *sessionState(int s)
{
	switch(s)
	{
		case kSessionStateInvalid :		return "invalid";
		case kSessionStateGetInCar :	return "get in car";
		case kSessionStateWarmup :		return "warmup";
		case kSessionStateParadeLaps :	return "parade laps";
		case kSessionStateRacing :		return "racing";
		case kSessionStateCheckered :	return "checkered";
		case kSessionStateCoolDown :	return "cool down";
	}
	return "?";
}

static const char *sessionFlag(int f)
{
	switch(f)
	{
		case kFlagGreen :		return "green";
		case kFlagYellow :		return "yellow";
		case kFlagRed :			return "red";
	}
	return "?";
}

static void printStateData(
	eSimDataType	newStateData)

	{
		case kCurrentWeekend :
		{
			const CurrentWeekend *wk = (const CurrentWeekend * )
					AppGetSimData(newStateData);
			if (!formatForSpreadsheet)
			{
				printf("CurrentWeekend: <%d, %s, %dft, <%s>, <%s>>\n",
									wk->atTrack ? 1 : 0,
									wk->track,
									int(wk->trackLength * 3.281f),
									wk->sessions,
									wk->options);
			}
			break;
		}
		case kDriverInCar :
		{
			const DriverInCar *in = (const DriverInCar * )
					AppGetSimData(newStateData);
			if (!formatForSpreadsheet)
			{
				printf("DriverInCar: <%d>\n", in->inCar ? 1 : 0);
			}
			break;
		}
		case kGameIsPaused :
		{
			const GameIsPaused *paused = (const GameIsPaused * )
					AppGetSimData(newStateData);
			if (!formatForSpreadsheet)
			{
				printf("GameIsPaused: <%d>\n", paused->paused ? 1 : 0);
			}
			break;
		}
		case kCarSetup :
		{
			const CarSetup *setup = (const CarSetup * )
					AppGetSimData(newStateData);
			(void) setup;
			if (!formatForSpreadsheet)
			{
				printf("CarSetup: ...\n");
			}

			// It's a good idea to clear out the results of
			// changes made during a prior pit stop since they
			// will no longer be meaningful when a completely
			// new setup is applied.
			AppClearSimData(kPitStop);
			break;
		}

		case kPitStop :
		{
			const PitStop *pitstop = (const PitStop * )
					AppGetSimData(newStateData);
			(void) pitstop;
			if (!formatForSpreadsheet)
			{
				printf("PitStop: ...\n");
			}
			break;
		}

		case kDriverEntry :
		{
			const DriverEntry *entry = (const DriverEntry *)
					AppGetSimData(newStateData);
			if (!formatForSpreadsheet)
			{
				printf("Driver Joins: <%2d, <%02x>, %s>\n",
						entry->carIdx, entry->flags, entry->name);
			}
			break;
		}

		case kDriverWithdrawl :
		{
			const DriverWithdrawl *withdrawl = (const DriverWithdrawl *)
					AppGetSimData(newStateData);
			if (!formatForSpreadsheet)
			{
				printf("Driver Withdraws: <%d>\n", withdrawl->carIdx);
			}
			break;
		}

		case kSessionInfo :
		{
			const SessionInfo *info = (const SessionInfo *)
					AppGetSimData(newStateData);
			if (!formatForSpreadsheet)
			{
				printf("Session: <%d,%d>, <%s,%s,%s>, time<%4d>, tr<%d>, lr<%d>\n",
						info->sessionNum,
						info->sessionCookie,
						sessionType(info->sessionType),
						sessionState(info->sessionState),
						sessionFlag(info->sessionFlag),
						int(info->currentTime),
						int(info->timeRemaining),
						info->lapsRemaining);
			}
			break;
		}

		case kLapCrossing :
		{
			const LapCrossing *crossing = (const LapCrossing *)
					AppGetSimData(newStateData);
			if (!formatForSpreadsheet)
			{
				printf("Lap Crossing: car<%2d>, lap<%3d>, at<%10.3f>, flags<%c%c%c%c>\n",
						crossing->carIdx, crossing->lapNum, crossing->crossedAt,
						crossing->flags & kCrossTimeInvalid	? 'I' : '-',
						crossing->flags & kCrossOffTrack		? 'O' : '-',
						crossing->flags & kCrossPitted		? 'P' : '-',
						crossing->flags & kCrossBlackFlag	? 'B' : '-');
			}
			break;
		}

		case kStandings :
		{
			const Standings *s = (const Standings *)
					AppGetSimData(newStateData);
			if (!formatForSpreadsheet)
			{
				printf("Standings<%d,%sofficial>...\n",
						s->sessionNum, s->flags & kStandingsOfficial ? "" : "un");
			}
			break;
		}
	}


static float TEMP(float c)
{
	if (englishUnits)
		return 9.0f*c/5.0f + 32.0f;		// degrees C to degrees F
	else
		return c;
}

static float PRESSURE(float p)
{
	if (englishUnits)
		return p * 0.1450377f;			// kPa to PSI
	else
		return p;
}

static float FORCE(float f)
{
	if (englishUnits)
		return f * 0.2248f;				// N to lb
	else
		return f;
}

static float DIST1(float d)
{
	if (englishUnits)
		return d * 3.281f;				// M to ft
	else
		return d;
}

static float DIST2(float d)
{
	if (englishUnits)
		return d * 39.372f;				// M to in
	else
		return d;
}

static float VEL(float v)
{
	if (englishUnits)
		return v * 2.237f;				// M/s to MPH
	else
		return v;
}

static float ANGLE(float a)
{
	if (englishUnits)
		return a * 180.0f / 3.14159f;	// rad to deg
	else
		return a;
}

static float TORQUE(float t)
{
	if (englishUnits)
		return FORCE(DIST1(t));			// N*m to Lb*ft
	else
		return t;
}

static float POWER(float t)
{
	if (englishUnits)
		return t/745.7f;				// Watts to hp
	else
		return t;
}






static void printSample()
{
	// The sim has finished writing a new sample.  We can
	// now check to see if each of the types of sample data
	// we have requested has been written.
	const SampleHeader		*sh;
	const ChassisData		*cd;
	const WheelData			*wd;
	const TireData			*td;
	const DrivelineData		*dl;
	const DriverInput		*di;
	float selectedValue = 0.0f;
	if ((sh = (const SampleHeader *) AppGetSimData(kSampleHeader)) != NULL &&
		!formatForSpreadsheet)
	{
		printf("%d, %d, %d, %f, ", sh->sequence, sh->tick, sh->step, sh->time);
	}
	if ((cd = (const ChassisData *) AppGetSimData(kChassisData)) != NULL &&
		!formatForSpreadsheet)
	{
		static const float mt_per_sec_to_MPH = 2.2374145f;
		float v = (float) sqrt(cd->v[0]*cd->v[0] +
								cd->v[1]*cd->v[1] +
								cd->v[2]*cd->v[2]) * mt_per_sec_to_MPH;
		if (selectedField == 1) selectedValue = v; // speed
		if (selectedField == 6) selectedValue = cd->trkPct; // trkPct
		if (selectedField == 7) selectedValue = TORQUE(cd->steerT); // steerT
		printf("%f, %f,", cd->trkPct, VEL(v));
	}
	if ((wd = (const WheelData *) AppGetSimData(kWheelData)) != NULL &&
		!formatForSpreadsheet)
	{
	}
	if ((td = (const TireData *) AppGetSimData(kTireData)) != NULL &&
		!formatForSpreadsheet)
	{
	}
	if ((dl = (const DrivelineData *) AppGetSimData(kDrivelineData)) != NULL &&
		!formatForSpreadsheet)
	{
		if (selectedField == 2) selectedValue = dl->engRPM; // rpm
	}
	if ((di = (const DriverInput *) AppGetSimData(kDriverInput)) != NULL &&
		!formatForSpreadsheet)
	{
		if (selectedField == 3) selectedValue = di->throttle; // throttle
		if (selectedField == 4) selectedValue = di->brake; // brake
		if (selectedField == 5) selectedValue = (float)di->gear; // gear
	}

	if (formatForSpreadsheet)
	{
		if (sh)
		{
			printf("%d%c%d%c%d%c%f",
					sh->sequence, S, sh->tick, S, sh->step, S, sh->time);
		}else printf("%c%c%c", S,S,S);

		if (cd)
		{
			// Compute overall velocity.
			float v = (float) sqrt(cd->v[0]*cd->v[0] +
									cd->v[1]*cd->v[1] +
									cd->v[2]*cd->v[2]);
			printf("%c%f%c%f%c%f%c%f%c%f%c%f%c%f%c%f%c%f%c%f%c%f",
					S, cd->trkPct, S, VEL(v),
					S, DIST1 (cd->r[0]), S, DIST1 (cd->r[1]), S, DIST1 (cd->r[2]),
					S, VEL  (cd->v[0]), S, VEL  (cd->v[1]), S, VEL  (cd->v[2]),
					S, ANGLE(cd->q[0]), S, ANGLE(cd->q[1]), S, ANGLE(cd->q[2]));
			printf("%c%f%c%f%c%f%c%f%c%f%c%f%c%f%c%f",
					S, ANGLE(cd->w[0]), S, ANGLE(cd->w[1]), S, ANGLE(cd->w[2]),
					S, cd->N[0], S, cd->N[1], S, cd->N[2],
					S, cd->H[0], S, cd->H[1]);
			printf("%c%f",
					S, TORQUE(cd->steerT));

			// Compute lateral, longitudinal, and normal G loading
			// on the chassis.
			if (sh)
			{
				static unsigned int	sequence = -1;
				static SampleHeader	sh0;
				static ChassisData	cd0;
				float				ca[3], dt;

				// We do so by computing the change in velocity from one
				// sample to the next, scaling by 1/change in time, adding
				// in gravity, and transforming this into the car's
				// coordinate system.
				if (sh->sequence != sequence ||
					(dt = (float)(sh->time - sh0.time)) <= 0.0f ||
					dt > 0.125f)
				{
					// When the sample header sequence number changes, then
					// this is the first sample since the car was dropped into
					// the world, so we have no prior sample with which to
					// compare.
					// If a data overrun occurred, then we may have missed
					// some samples and so the current and most recent
					// samples could be far apart in time.  Computing
					// G loading the way we do would be terribly inaccurate
					// in such a case.
					ca[0]		= ca[1] = ca[2] = 0.0f;

					// Squirrel away current sample and header so we
					// can compute chassis acceleration at next sample.
					sh0			= *sh;
					cd0			= *cd;
					sequence	= sh->sequence;
				}
				else
				{
					static const float	one_g	= 9.80665f;
					static const float	wg[3]	= { 0.0f, 0.0f, one_g }; // m/sec^2
					float				wa[3];
					float				m[3][3];

					// Compute acceleration (change in velocity per time) of
					// chassis in world frame from change in velocity over
					// this and prior sample.
					vector3Sub(wa, cd->v, cd0.v);			// m/sec
					vector3Scale(wa, 1.0f / dt, wa);		// m/sec^2

					// Add in gravity.
					vector3Add(wa, wa, wg);					// m/sec^2

					// Form 3x3 rotation matrix from car relative
					// coordinates to world coordinates from the
					// yaw, pitch and roll of the chassis.
					matrix33FromEuler(m, cd->q[0], cd->q[1], cd->q[2]);

					// Rotate chassis acceleration vector from world frame
					// into car's frame (note that to do this we're
					// multiplying by the matrix transpose).
					matrix33MulTVector3(ca, m, wa);

					// Scale to convert from m/sec^2 to G's.
					vector3Scale(ca, 1.0f / one_g, ca);		// G's

					// Squirrel away current sample and header so we
					// can compute chassis acceleration at next sample.
					sh0 = *sh;
					cd0 = *cd;
				}

				// The car's reference frame has +x forwards, +y to the
				// left, and +z up.  So longitudinal is ca[0], lateral
				// is ca[1], and normal is ca[2].
				if (selectedField == 8) selectedValue = ca[0]; // longG
				if (selectedField == 9) selectedValue = ca[1]; // latG
				if (selectedField == 10) selectedValue = ca[2]; // normG
				printf("%c%f%c%f%c%f",
						S, ca[0], S, ca[1], S, ca[2]);
			}
			else printf("%c%c%c", S,S,S);
		}
		else printf("%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c",
					  S,S,S,S,S,S,S,S,S,S,S,S,S,S,S,S,S,S,S,S,S,S,S);
		if (di)
		{
			printf("%c%f%c%f%c%f%c%f%c%d",
					S, di->throttle, S, di->brake, S, di->clutch,
					S, ANGLE(di->steer), S, di->gear);
		} else printf("%c%c%c%c%c", S,S,S,S,S);
		if (dl)
		{
			printf("%c%f%c%f%c%f",
					S, dl->engRPM, S, POWER(dl->engPower), S, TORQUE(dl->engT));
		}
		else printf("%c%c%c", S,S,S);
		for(int w = 0; w < 4; ++w)
		{
			if (td && wd)
			{
				printf("%c%f%c%f%c%f%c%f%c%f%c%f%c%f%c%f",
					S, FORCE(wd->w[w].Fz),
					S, ANGLE(wd->w[w].v), S, DIST2(wd->w[w].defl),
					S, ANGLE(wd->w[w].camber),
					S, PRESSURE(td->w[w].pressure), S, TEMP(td->w[w].temp[0]),
					S, TEMP(td->w[w].temp[1]), S, TEMP(td->w[w].temp[2]));
			}
			else printf("%c%c%c%c%c%c%c%c", S,S,S,S,S,S,S,S);
		}
	}

	if (selectedField && sh) {
		printf("%f\t%f\n", sh->time, selectedValue);
	} else {
		printf("\n");
	}
}

/*
 ***************************************************************************
 ***************************************************************************
 */
int main(int argc, char *argv[])
{
	// By default the sim will run as a HIGH_PRIORITY_CLASS process.
	// If we don't do anything special, we'll be run as a
	// NORMAL_PRIORITY_CLASS process.  Since the sim will use
	// as much CPU time as it can, if it is running at a higher
	// priority class than we are, we will only be given the
	// CPU sporadically, and will more than likely not be able
	// to read telemetry quickly enough.  This would be particularly
	// bad if you need to use the telemetry in real-time (for
	// controlling a motion platform, for example).
	// To be serviced by the CPU adequately, we need to run
	// ourself at the same priority class as the sim.  This can
	// be done by either using this line of code (to run ourself
	// as a HIGH_PRIORITY_CLASS process), or by telling the sim
	// to run itself at NORMAL_PRIORITY_CLASS (by passing it
	// -kh0  (that's minus-k-h-zero) on the command line).
	// You probably should not run your telemetry gathering app
	// at a higher priority than the sim, particularly if you're
	// gathering telemetry at the 288Hz physics rate, since this
	// is likely to cause a lot of context switching.
	// So either use this line, or -kh0, but not both.
	SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);

	// Parse command line switches
	for(int i=1; i<argc; i++) {
		if (strcmp(argv[i], "/1") == 0) selectedField = 1; // speed
		else if (strcmp(argv[i], "/2") == 0) selectedField = 2; // rpm
		else if (strcmp(argv[i], "/3") == 0) selectedField = 3; // throttle
		else if (strcmp(argv[i], "/4") == 0) selectedField = 4; // brake
		else if (strcmp(argv[i], "/5") == 0) selectedField = 5; // gear
		else if (strcmp(argv[i], "/6") == 0) selectedField = 6; // trkPct
		else if (strcmp(argv[i], "/7") == 0) selectedField = 7; // steerT
		else if (strcmp(argv[i], "/8") == 0) selectedField = 8; // longG
		else if (strcmp(argv[i], "/9") == 0) selectedField = 9; // latG
		else if (strcmp(argv[i], "/10") == 0) selectedField = 10; // normG
	}

	// Assume that the [Telemetry] id= variable in core.ini is empty.
	// Otherwise, we'd have to pass a second parameter to AppBegin(),
	// indicating to which instance of the sim we're to connect.
	// The only scenario where this would be needed is where multiple
	// instances of a race server were being run on one machine at
	// the same time, and you wanted to gather telemetry from all
	// of them.  You would then need to also run multiple instances
	// of your telemetry gathering application, and would use
	// AppBegin()'s second parameter, along with the core.ini
	// [Telemetry] id= variable, to ensure that each instance of your
	// telemetry gathering app connected to the correct instance of
	// the race server.
	if (AppBegin("NR2003"))
	{
		// Define which types of telemetry we want the sim to
		// write to us.
		const eSimDataType desired[] =
		{
			kSampleHeader,
			kChassisData,
			kWheelData,
			kTireData,
			kDrivelineData,
			kDriverInput,

			kCurrentWeekend,		// Want state information, too
			kDriverInCar,
			kGameIsPaused,
			kCarSetup,
			kPitStop,
			kDriverEntry,
			kDriverWithdrawl,
			kSessionInfo,
			kLapCrossing,
			kStandings
		};

		// Note that, since these requests are written into a queue
		// that was initialized during AppBegin, we do not need to
		// wait for the sim to become active before issuing them.
		// When the sim does its correspondng SimBegin, it will
		// find this queue in memory and read our requests from
		// it.  The sim does check this queue frequently, so we
		// can issue these requests any time. 
		// Also note, however, that the queue into which these
		// requests are written is small, so you wouldn't want
		// to write these things repeatedly without checking the
		// return values to ensure the requests could be queued.
 		(void) AppRequestDataItems(sizeof(desired)/sizeof(desired[0]),
								   desired);
		(void) AppRequestDataAtPhysicsRate(sampleAtPhysicsRate);
		(void) AppEnableSampling(true);

		// All we do is print what telemetry we've read from the
		// sim, so there's nothing for us to do until the sim
		// becomes active.  Just hang around until it does.
		if (!formatForSpreadsheet && !selectedField)
		{
			printf("Waiting for the sim to become active...\n");
		}
		while(!AppCheckIfSimActiveQ())
		{
			Sleep(1000);
		}

		// Ok, the sim is now active.  Once the sim exits, it will
		// indicate so.  Keep processing data from the sim until
		// it goes away.
		if (!formatForSpreadsheet && !selectedField)
		{
			printf("...Sim is now active...\n");
		}
		else
		{
			// We're outputting for import into a spreadsheet.  Print
			// out the column headers.  Note that columns with an
			// asterisk(*) are computed from the telemetry, they are
			// not generated by the simulator.
			printf("Sequence%cTick%cStep%cTime"								// 3 seps
					"%cTrkPct%cSpeed*"										// 2 seps
					"%cx%cy%cz%cVx%cVy%cVz"									// 6 seps
					"%cyaw%cpitch%croll%cVyaw%cVpitch%cVroll"				// 6 seps
					"%cNx%cNy%cNz%cHx%cHy"									// 5 seps
					"%cSteerTorque"											// 1 sep
					"%cLongG*%cLatG*%cNormG*"								// 3 seps
					"%cthrottle%cbrake%cclutch%csteer%cgear"				// 5 seps
					"%cRPM%cpower%cengF"									// 3 seps
					"%clfFz%clfV%clfBump%clfCamb%clfPress%clfI%clfM%clfO"	// 8 seps
					"%crfFz%crfV%crfBump%crfCamb%crfPress%crfI%crfM%crfO"	// 8 seps
					"%clrFz%clrV%clrBump%clrCamb%clrPress%clrI%clrM%clrO"	// 8 seps
					"%crrFz%crrV%crrBump%crrCamb%clrPress%clrI%clrM%clrO",	// 8 seps
					S,S,S,S,S,S,S,S,S,S,S,S,S,S,S,S,S,S,S,S,	// 20
					S,S,S,S,S,S,S,S,S,S,S,S,S,S,S,S,S,S,S,S,	// 40
					S,S,S,S,S,S,S,S,S,S,S,S,S,S,S,S,S,S,S,S,	// 60
					S,S,S,S,S,S);								// 66
			printf("\n");
		}

		// Keep reading from the sim until it goes away, and there's
		// no telemetry left to be read.
		// See if the sim has written anything to us. The timeout
		// used here is fairly arbitrary since this program does
		// nothing but read telemetry from the sim and write it
		// to stdout.  If your program is in its own loop where
		// reading telemetry from the sim is only one of its tasks,
		// you'd probably want to use a timeout of 0.  This tells
		// AppWaitForNewSample to read any telemetry that is
		// queued, but not to wait if there is none available.
		// You should not, however, allow your program to gobble
		// up all available CPU time, since this will adversely
		// affect the sim's performance.  You could, for example
		// call Sleep() with an appropriate number of milliseconds
		// somewhere in your loop, or you could pass AppWaitForNewSample
		// a positive timeout.  Under the covers, it will use Sleep()
		// to ensure at least this amount of time passes before
		// returning.
		const int timeOutMs = 100;
		eSimDataType newStateData;
		bool newSample;
		while((newSample = AppWaitForNewSample(&newStateData, timeOutMs)) == true ||
			  newStateData != kNoStateInfo ||
			  AppCheckIfSimActiveQ())
		{
			// See if any new telemetry from the sim.
			if (newSample || newStateData != kNoStateInfo)
			{
				// Yes, it has either written a piece of state data,
				// or has finished writing a new sample.  Which one?
				// Either newStateData won't equal kNoStateInfo
				// (meaning a piece of state data was read), or
				// AppWaitForNewSample will return true (indicating
				// a complete sample is now available), but never
				// both at once.
				if (newStateData != kNoStateInfo)
				{
					// It was state data.  Process it.
					printStateData(newStateData);
				}
				else if (newSample)
				{
					printSample();

					// While it is not strictly necessary that you call
					// this function, it is a very good idea to do so
					// after processing each sample.
					// The various individual pieces of telemetry that
					// make up this sample are only guaranteed to be
					// self-consistent until the next time you call
					// AppWaitForNewSample.  Once called, it is entirely
					// possible that some parts of the sample will be
					// from the prior sampling, and some parts from the
					// current sampling.  If you need the sample data
					// across calls to AppWaitForNewSample, you should
					// copy it to memory that you control.
					AppClearSample();
				}

				// If, for some reason, our program went away for a while
				// and wasn't reading telemetry from the sim, the buffer
				// into which the sim writes the telemetry might have
				// filled up.  Whenever there is insufficient space
				// available for the sim to write telemetry into the buffer,
				// it simply discards the telemetry, and sets the flag
				// that is checked here.
				// You should carefully consider where you place this test.
				if (AppCheckIfSimDataOverrunQ())
				{
					// Note that this means that some telemetry was lost,
					// If it's important to your application that the
					// sample data be continuous (for example, if you're
					// controlling a motion platform), you would want to
					// deal with that possibility here.
					// If it's important to your application that it deal
					// with samples in real-time (back to the motion platform
					// idea), you should also note that since the telemetry
					// buffer is large enough to hold many samples, and since
					// it has overflowed, there are likely to be many samples
					// in the buffer that have been there for a while, and
					// should probably be considered 'stale'.
					printf("*OVERRUN*\n");

					// The flag will remain set until we (the app) explicitly
					// clear it.  That is, the sim won't reset the overrun
					// flag once it sees that there is space available in
					// the buffer.
					AppClearSimDataOverrun();
				}
			}
		}

		// Since we successfully called AppBegin, we are obliged to
		// call AppEnd.
		if (!formatForSpreadsheet && !selectedField)
		{
			printf("\n...Sim has deactivated.\n");
		}
		AppEnd();
	}
	return 0;
}
