// MY INTERACTION FUNCTIONS (v2)

#include "my_interaction_functions.h"
#include <stdio.h>

extern "C" {
#include <FreeRTOS.h>
#include <task.h>
#include <timers.h>
#include <semphr.h>
#include <interrupts.h>
#include <interface.h>
}

// Sleep between two sensor reads. This is what lets the other axis task, the lights task,
// the menu... run while this loop waits for a sensor (real concurrency).
static inline void pollDelay() { vTaskDelay(POLL_DELAY_TICKS); }


////////////////////////////////////////////////////////////////////////////////////////
//							  BIT MANIPULATIONS FUNCTIONS							   //
////////////////////////////////////////////////////////////////////////////////////////

/*
*	Given a byte value, returns the (masked) value of its Nth bit: 0 if low, non-zero if high.
*/
int getBitValue(uInt8 value, uInt8 bit_n) {
	return (value & (1 << bit_n));
}

/*
*	Set the Nth bit of *variable to new_value_bit
*/
void setBitValue(uInt8* variable, int n_bit, int new_value_bit) {

	uInt8 mask_on = (uInt8)(1 << n_bit);
	uInt8 mask_off = (uInt8)~mask_on;

	if (new_value_bit) {
		*variable |= mask_on;
	}
	else {
		*variable &= mask_off;
	}
}

/*
*	Atomic read-modify-write of two bits of port 2 (shared by X, Y, Z and the lights).
*	The critical section is tiny: one read, two bit operations, one write.
*/
static void setPort2Bits(int bitA, int valA, int bitB, int valB) {

	taskENTER_CRITICAL();

	uInt8 p2 = readDigitalU8(2);
	setBitValue(&p2, bitA, valA);
	setBitValue(&p2, bitB, valB);
	writeDigitalU8(2, p2);

	taskEXIT_CRITICAL();
}

static uInt8 readPort(uInt32 port) {

	taskENTER_CRITICAL();
	uInt8 value = readDigitalU8(port);
	taskEXIT_CRITICAL();

	return value;
}


////////////////////////////////////////////////////////////////////////////////////////
//					SWITCHES, BOX SENSOR, LIGHTS, MOTION BITS						   //
////////////////////////////////////////////////////////////////////////////////////////

/*
*	Reads the two switches on the left of the kit (ACTIVE HIGH): true = pressed.
*/
void readSwitches(bool* switch1, bool* switch2) {

	uInt8 p1 = readPort(1);

	*switch1 = getBitValue(p1, P1_BIT_SWITCH_1) != 0;
	*switch2 = getBitValue(p1, P1_BIT_SWITCH_2) != 0;
}

/*
*	true if there is a box in the cage (sensor ACTIVE LOW).
*/
bool isBoxInCage() {

	uInt8 p1 = readPort(1);

	return getBitValue(p1, P1_BIT_BOX_IN_CAGE) != 0;
}

/*
*	Turns the two lights on the left on/off (actuators ACTIVE HIGH).
*/
void setLights(bool upper, bool lower) {
	setPort2Bits(P2_BIT_LIGHT_UPPER, upper, P2_BIT_LIGHT_LOWER, lower);
}

/*
*	Returns the current state of the axis output bits (port 2, bits 2..7).
*/
uInt8 saveMotionBits() {
	return (uInt8)(readPort(2) & P2_MOTION_MASK);
}

/*
*	Writes back axis output bits previously returned by saveMotionBits(), keeping the lights untouched.
*/
void restoreMotionBits(uInt8 bits) {

	taskENTER_CRITICAL();

	uInt8 p2 = readDigitalU8(2);
	p2 = (uInt8)((p2 & ~P2_MOTION_MASK) | (bits & P2_MOTION_MASK));
	writeDigitalU8(2, p2);

	taskEXIT_CRITICAL();
}


////////////////////////////////////////////////////////////////////////////////////////
//							GENERIC AXIS MOVEMENT HELPERS							   //
////////////////////////////////////////////////////////////////////////////////////////

typedef int  (*PosFn)();
typedef void (*ActFn)();

/*
*	Moves one axis to 'target' (sensor position). Direction is only written to the port when it
*	changes, the loop sleeps between reads (pollDelay) and the motor is stopped the instant the
*	target sensor is seen.
*
*	whenUnknown: action used if the axis starts BETWEEN sensors (-1).
*/
static void gotoAxis(int target, PosFn getPos, ActFn forward, ActFn backward, ActFn stop, ActFn whenUnknown) {

	int dir = 0;			// +1 forward, -1 backward, 0 stopped
	int cur = getPos();

	if (cur == -1) {
		whenUnknown();
		dir = (whenUnknown == forward) ? +1 : -1;
	}

	while ((cur = getPos()) != target) {

		if (cur != -1) {
			int want = (cur < target) ? +1 : -1;

			if (want != dir) {
				dir = want;
				if (want > 0) forward(); else backward();
			}
		}

		pollDelay();
	}

	stop();
}

/*
*	Moves one axis towards its home end until position 1 is seen.
*/
static void homeAxis(PosFn getPos, ActFn towardsHome, ActFn stop) {

	if (getPos() != 1) {
		towardsHome();

		while (getPos() != 1) {
			pollDelay();
		}
	}

	stop();
}


////////////////////////////////////////////////////////////////////////////////////////
//								X AXIS RELATED FUNCTIONS							   //
////////////////////////////////////////////////////////////////////////////////////////

void moveXLeft() { setPort2Bits(6, 1, 7, 0); }		// P2 = 01XXXXXX
void moveXRight() { setPort2Bits(6, 0, 7, 1); }		// P2 = 10XXXXXX
void stopX() { setPort2Bits(6, 0, 7, 0); }		// P2 = 00XXXXXX

/*
*	Current X position: 1, 2, 3 or -1 (between sensors)
*/
int getXPos() {

	uInt8 p0 = readPort(0);

	if (!getBitValue(p0, 2)) return 1;
	if (!getBitValue(p0, 1)) return 2;
	if (!getBitValue(p0, 0)) return 3;

	return (-1);
}

void gotoX(int x) {
	gotoAxis(x, getXPos, moveXRight, moveXLeft, stopX, moveXRight);
}

void homeX() {
	homeAxis(getXPos, moveXLeft, stopX);
}


////////////////////////////////////////////////////////////////////////////////////////
//								Y AXIS RELATED FUNCTIONS							   //
////////////////////////////////////////////////////////////////////////////////////////

void moveYInside() { setPort2Bits(4, 0, 5, 1); }		// P2 = XX10XXXX
void moveYOutside() { setPort2Bits(4, 1, 5, 0); }		// P2 = XX01XXXX
void stopY() { setPort2Bits(4, 0, 5, 0); }		// P2 = XX00XXXX

/*
*	Current Y position: 1, 2, 3 or -1 (between sensors)
*/
int getYPos() {

	uInt8 p0 = readPort(0);

	if (!getBitValue(p0, 5)) return 1;
	if (!getBitValue(p0, 4)) return 2;
	if (!getBitValue(p0, 3)) return 3;

	return (-1);
}

void gotoY(int y) {
	gotoAxis(y, getYPos, moveYInside, moveYOutside, stopY, moveYInside);
}


////////////////////////////////////////////////////////////////////////////////////////
//								Z AXIS RELATED FUNCTIONS							   //
////////////////////////////////////////////////////////////////////////////////////////

void moveZUp() { setPort2Bits(2, 0, 3, 1); }			// P2 = XXXX10XX
void moveZDown() { setPort2Bits(2, 1, 3, 0); }			// P2 = XXXX01XX
void stopZ() { setPort2Bits(2, 0, 3, 0); }			// P2 = XXXX00XX

/*
*	Current Z DOWN position: 1, 2, 3 or -1
*/
int getZPos() {

	uInt8 p0 = readPort(0);
	uInt8 p1 = readPort(1);

	if (!getBitValue(p1, 3)) return 1;		// Z=1 Down
	if (!getBitValue(p1, 1)) return 2;		// Z=2 Down
	if (!getBitValue(p0, 7)) return 3;		// Z=3 Down

	return (-1);
}

/*
*	Current Z UP position: 1, 2, 3 or -1
*/
int getZUpPos() {

	uInt8 p0 = readPort(0);
	uInt8 p1 = readPort(1);

	if (!getBitValue(p1, 2)) return 1;		// Z=1 Up
	if (!getBitValue(p1, 0)) return 2;		// Z=2 Up
	if (!getBitValue(p0, 6)) return 3;		// Z=3 Up

	return (-1);
}

/*
*	Goto the Z DOWN position z. If the cage starts between "Down" sensors (for example at an
*	"Up" position) it goes DOWN first: that is always safe because a sensor exists at both
*	ends, whereas going up from the top would run past the limit.
*/
void gotoZ(int z) {
	gotoAxis(z, getZPos, moveZUp, moveZDown, stopZ, moveZDown);
}

/*
*	Goto the Z UP position z.
*/
void gotoZUp(int z) {
	gotoAxis(z, getZUpPos, moveZUp, moveZDown, stopZ, moveZUp);
}

/*
*	Move Z up until some Z UP sensor is found (no movement if already on one).
*/
void movetoZUp() {

	if (getZUpPos() == -1) {
		moveZUp();

		while (getZUpPos() == -1) {
			pollDelay();
		}
	}

	stopZ();
}

/*
*	Move Z down until some Z DOWN sensor is found (no movement if already on one).
*/
void movetoZDown() {

	if (getZPos() == -1) {
		moveZDown();

		while (getZPos() == -1) {
			pollDelay();
		}
	}

	stopZ();
}

void homeZ() {
	homeAxis(getZPos, moveZDown, stopZ);
}


////////////////////////////////////////////////////////////////////////////////////////
//			HIGHER FUNCTIONS (sequential versions, kept for manual tests)			   //
////////////////////////////////////////////////////////////////////////////////////////

void putPartInCell() {

	movetoZUp();
	gotoY(3);
	movetoZDown();
	gotoY(2);
}

void takePartFromCell() {

	gotoY(3);
	movetoZUp();
	gotoY(2);
	movetoZDown();
}

void placeItemOnCell(int x, int z) {
	gotoX(x);
	gotoZUp(z);
	putPartInCell();
	autoCallibrate();
}

void takeItemFromCell(int x, int z) {
	gotoX(x);
	gotoZ(z);
	takePartFromCell();
	autoCallibrate();
}


////////////////////////////////////////////////////////////////////////////////////////
//								UTILITY FUNCTIONS									   //
////////////////////////////////////////////////////////////////////////////////////////

void stopAll() {
	stopX();
	stopY();
	stopZ();
}

/*
*	Sequential calibration: Y to the centre first (X/Z may only move with Y == 2),
*	then X -> 1 and Z -> 1.
*/
void autoCallibrate() {
	gotoY(2);
	homeX();
	homeZ();
}

bool isHomed() {
	return getXPos() == 1 && getYPos() == 2 && getZPos() == 1;
}
