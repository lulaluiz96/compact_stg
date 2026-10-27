// MY INTERACTION FUNCTIONS

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


  ////////////////////////////////////////////////////////////////////////////////////////
 //							  BIT MANIPULATIONS FUNCTIONS							   //
////////////////////////////////////////////////////////////////////////////////////////


/*
*	Given a byte value, returns the value of its Nth bit (0 if the bit is low, not 0 if it is high).
*/
int getBitValue(uInt8 value, uInt8 bit_n) {
	return (value & (1 << bit_n));
}


/*
*	Given a byte value, sets the Nth bit to new_value_bit (0 or 1).
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




  ////////////////////////////////////////////////////////////////////////////////////////
 //						SWITCHES, BOX SENSOR AND LIGHTS								   //
////////////////////////////////////////////////////////////////////////////////////////


/*
*	Reads the two switches on the left of the kit. true means "pressed".
*	The polarity is set by SWITCH_ACTIVE_HIGH in the header.
*/
void readSwitches(bool* switch1, bool* switch2) {

	taskENTER_CRITICAL();

	uInt8 p1 = readDigitalU8(1);			// read Port 1

	taskEXIT_CRITICAL();

	int level1 = (getBitValue(p1, P1_BIT_SWITCH_1) != 0);		// 1 if the bit is high
	int level2 = (getBitValue(p1, P1_BIT_SWITCH_2) != 0);

	*switch1 = (level1 == SWITCH_ACTIVE_HIGH);
	*switch2 = (level2 == SWITCH_ACTIVE_HIGH);
}


/*
*	Returns true if there is a box in the cage.
*	The polarity is set by BOX_SENSOR_ACTIVE_HIGH in the header.
*/
bool isBoxInCage() {

	taskENTER_CRITICAL();

	uInt8 p1 = readDigitalU8(1);			// read Port 1

	taskEXIT_CRITICAL();

	int level = (getBitValue(p1, P1_BIT_BOX_IN_CAGE) != 0);		// 1 if the bit is high

	return (level == BOX_SENSOR_ACTIVE_HIGH);
}


/*
*	Turns the upper and the lower light on (true) or off (false).
*/
void setLights(bool upper, bool lower) {

	taskENTER_CRITICAL();

	uInt8 p2 = readDigitalU8(2);			// read Port 2

	setBitValue(&p2, P2_BIT_LIGHT_UPPER, upper);
	setBitValue(&p2, P2_BIT_LIGHT_LOWER, lower);

	writeDigitalU8(2, p2);					// update Port 2

	taskEXIT_CRITICAL();
}




  ////////////////////////////////////////////////////////////////////////////////////////
 //								X AXIS RELATED FUNCTIONS							   //
////////////////////////////////////////////////////////////////////////////////////////


/*
*	Moves the X axis to the LEFT: bit 6 high, bit 7 low.
*/
void moveXLeft() {

	taskENTER_CRITICAL();

	uInt8 p2 = readDigitalU8(2);

	setBitValue(&p2, 6, 1);
	setBitValue(&p2, 7, 0);

	writeDigitalU8(2, p2);					// P2 = 01XXXXXX

	taskEXIT_CRITICAL();
}


/*
*	Moves the X axis to the RIGHT: bit 6 low, bit 7 high.
*/
void moveXRight() {

	taskENTER_CRITICAL();

	uInt8 p2 = readDigitalU8(2);

	setBitValue(&p2, 6, 0);
	setBitValue(&p2, 7, 1);

	writeDigitalU8(2, p2);					// P2 = 10XXXXXX

	taskEXIT_CRITICAL();
}


/*
*	Stops the X axis: bits 6 and 7 low.
*/
void stopX() {

	taskENTER_CRITICAL();

	uInt8 p2 = readDigitalU8(2);

	setBitValue(&p2, 6, 0);
	setBitValue(&p2, 7, 0);

	writeDigitalU8(2, p2);					// P2 = 00XXXXXX

	taskEXIT_CRITICAL();
}


/*
*	Returns the current X position: 1, 2, 3 or -1 (the cage is between two sensors).
*/
int getXPos() {

	taskENTER_CRITICAL();

	uInt8 p0 = readDigitalU8(0);

	taskEXIT_CRITICAL();

	if (!getBitValue(p0, 2)) {				// p0.2 = 0 (active low) -> X = 1
		return 1;
	}

	if (!getBitValue(p0, 1)) {				// p0.1 = 0 (active low) -> X = 2
		return 2;
	}

	if (!getBitValue(p0, 0)) {				// p0.0 = 0 (active low) -> X = 3
		return 3;
	}

	return (-1);
}


/*
*	Moves the cage on the X axis to the position x (1, 2 or 3).
*	The loop sleeps between sensor readings so the other tasks (and the Z axis) keep running.
*/
void gotoX(int x) {

	int currentXPos = getXPos();
	int lastMove = 0;						// 0 = stopped, 1 = going right, -1 = going left

	// between two sensors we do not know where we are, so go right until a sensor is found
	if (currentXPos == -1) {
		moveXRight();
		lastMove = 1;
	}

	while (currentXPos != x) {

		// only write to the port when the direction really changes
		if ((currentXPos != -1) && (currentXPos < x) && (lastMove != 1)) {
			moveXRight();
			lastMove = 1;
		}
		else if ((currentXPos > x) && (lastMove != -1)) {
			moveXLeft();
			lastMove = -1;
		}

		vTaskDelay(POLL_DELAY_TICKS);
		currentXPos = getXPos();
	}

	stopX();
}


/*
*	Calibration of the X axis: moves LEFT until X = 1.
*	Going left is always safe because X = 1 is the left end.
*/
void homeX() {

	if (getXPos() != 1) {

		moveXLeft();

		while (getXPos() != 1) {
			vTaskDelay(POLL_DELAY_TICKS);
		}
	}

	stopX();
}




  ////////////////////////////////////////////////////////////////////////////////////////
 //								Y AXIS RELATED FUNCTIONS							   //
////////////////////////////////////////////////////////////////////////////////////////


/*
*	Moves the Y axis INSIDE: bit 4 low, bit 5 high.
*/
void moveYInside() {

	taskENTER_CRITICAL();

	uInt8 p2 = readDigitalU8(2);

	setBitValue(&p2, 4, 0);
	setBitValue(&p2, 5, 1);

	writeDigitalU8(2, p2);					// P2 = XX10XXXX

	taskEXIT_CRITICAL();
}


/*
*	Moves the Y axis OUTSIDE: bit 4 high, bit 5 low.
*/
void moveYOutside() {

	taskENTER_CRITICAL();

	uInt8 p2 = readDigitalU8(2);

	setBitValue(&p2, 5, 0);
	setBitValue(&p2, 4, 1);

	writeDigitalU8(2, p2);					// P2 = XX01XXXX

	taskEXIT_CRITICAL();
}


/*
*	Stops the Y axis: bits 4 and 5 low.
*/
void stopY() {

	taskENTER_CRITICAL();

	uInt8 p2 = readDigitalU8(2);

	setBitValue(&p2, 4, 0);
	setBitValue(&p2, 5, 0);

	writeDigitalU8(2, p2);					// P2 = XX00XXXX

	taskEXIT_CRITICAL();
}


/*
*	Returns the current Y position: 1, 2, 3 or -1 (the cage is between two sensors).
*/
int getYPos() {

	taskENTER_CRITICAL();

	uInt8 p0 = readDigitalU8(0);

	taskEXIT_CRITICAL();

	if (!getBitValue(p0, 5)) {				// p0.5 = 0 (active low) -> Y = 1
		return 1;
	}

	if (!getBitValue(p0, 4)) {				// p0.4 = 0 (active low) -> Y = 2
		return 2;
	}

	if (!getBitValue(p0, 3)) {				// p0.3 = 0 (active low) -> Y = 3
		return 3;
	}

	return (-1);
}


/*
*	Moves the cage on the Y axis to the position y (1, 2 or 3).
*/
void gotoY(int y) {

	int currentYPos = getYPos();
	int lastMove = 0;						// 0 = stopped, 1 = going inside, -1 = going outside

	// between two sensors: go inside until a sensor is found
	if (currentYPos == -1) {
		moveYInside();
		lastMove = 1;
	}

	while (currentYPos != y) {

		if ((currentYPos != -1) && (currentYPos < y) && (lastMove != 1)) {
			moveYInside();
			lastMove = 1;
		}
		else if ((currentYPos > y) && (lastMove != -1)) {
			moveYOutside();
			lastMove = -1;
		}

		vTaskDelay(POLL_DELAY_TICKS);
		currentYPos = getYPos();
	}

	stopY();
}




  ////////////////////////////////////////////////////////////////////////////////////////
 //								Z AXIS RELATED FUNCTIONS							   //
////////////////////////////////////////////////////////////////////////////////////////


/*
*	Moves the Z axis UP: bit 2 low, bit 3 high.
*/
void moveZUp() {

	taskENTER_CRITICAL();

	uInt8 p2 = readDigitalU8(2);

	setBitValue(&p2, 2, 0);
	setBitValue(&p2, 3, 1);

	writeDigitalU8(2, p2);					// P2 = XXXX10XX

	taskEXIT_CRITICAL();
}


/*
*	Moves the Z axis DOWN: bit 2 high, bit 3 low.
*/
void moveZDown() {

	taskENTER_CRITICAL();

	uInt8 p2 = readDigitalU8(2);

	setBitValue(&p2, 2, 1);
	setBitValue(&p2, 3, 0);

	writeDigitalU8(2, p2);					// P2 = XXXX01XX

	taskEXIT_CRITICAL();
}


/*
*	Stops the Z axis: bits 2 and 3 low.
*/
void stopZ() {

	taskENTER_CRITICAL();

	uInt8 p2 = readDigitalU8(2);

	setBitValue(&p2, 2, 0);
	setBitValue(&p2, 3, 0);

	writeDigitalU8(2, p2);					// P2 = XXXX00XX

	taskEXIT_CRITICAL();
}


/*
*	Returns the current Z DOWN position: 1, 2, 3 or -1.
*/
int getZPos() {

	taskENTER_CRITICAL();

	uInt8 p0 = readDigitalU8(0);
	uInt8 p1 = readDigitalU8(1);

	taskEXIT_CRITICAL();

	if (!getBitValue(p1, 3)) {				// p1.3 = 0 (active low) -> Z = 1 Down
		return 1;
	}

	if (!getBitValue(p1, 1)) {				// p1.1 = 0 (active low) -> Z = 2 Down
		return 2;
	}

	if (!getBitValue(p0, 7)) {				// p0.7 = 0 (active low) -> Z = 3 Down
		return 3;
	}

	return (-1);
}


/*
*	Returns the current Z UP position: 1, 2, 3 or -1.
*/
int getZUpPos() {

	taskENTER_CRITICAL();

	uInt8 p0 = readDigitalU8(0);
	uInt8 p1 = readDigitalU8(1);

	taskEXIT_CRITICAL();

	if (!getBitValue(p1, 2)) {				// p1.2 = 0 (active low) -> Z = 1 Up
		return 1;
	}

	if (!getBitValue(p1, 0)) {				// p1.0 = 0 (active low) -> Z = 2 Up
		return 2;
	}

	if (!getBitValue(p0, 6)) {				// p0.6 = 0 (active low) -> Z = 3 Up
		return 3;
	}

	return (-1);
}


/*
*	Moves the cage on the Z axis to the DOWN position z (1, 2 or 3).
*	If the cage starts between two "Down" sensors (for example on an "Up" position) it goes DOWN first.
*	That is always safe, going up from the top position would run past the limit.
*/
void gotoZ(int z) {

	int currentZPos = getZPos();
	int lastMove = 0;						// 0 = stopped, 1 = going up, -1 = going down

	if (currentZPos == -1) {
		moveZDown();
		lastMove = -1;
	}

	while (currentZPos != z) {

		if ((currentZPos != -1) && (currentZPos < z) && (lastMove != 1)) {
			moveZUp();
			lastMove = 1;
		}
		else if ((currentZPos > z) && (lastMove != -1)) {
			moveZDown();
			lastMove = -1;
		}

		vTaskDelay(POLL_DELAY_TICKS);
		currentZPos = getZPos();
	}

	stopZ();
}


/*
*	Moves the cage on the Z axis to the UP position z (1, 2 or 3).
*/
void gotoZUp(int z) {

	int currentZPos = getZUpPos();
	int lastMove = 0;						// 0 = stopped, 1 = going up, -1 = going down

	if (currentZPos == -1) {
		moveZUp();
		lastMove = 1;
	}

	while (currentZPos != z) {

		if ((currentZPos != -1) && (currentZPos < z) && (lastMove != 1)) {
			moveZUp();
			lastMove = 1;
		}
		else if ((currentZPos > z) && (lastMove != -1)) {
			moveZDown();
			lastMove = -1;
		}

		vTaskDelay(POLL_DELAY_TICKS);
		currentZPos = getZUpPos();
	}

	stopZ();
}


/*
*	Moves Z up until it finds any Z UP sensor (does not move if it is already on one).
*/
void movetoZUp() {

	if (getZUpPos() == -1) {

		moveZUp();

		while (getZUpPos() == -1) {
			vTaskDelay(POLL_DELAY_TICKS);
		}
	}

	stopZ();
}


/*
*	Moves Z down until it finds any Z DOWN sensor (does not move if it is already on one).
*/
void movetoZDown() {

	if (getZPos() == -1) {

		moveZDown();

		while (getZPos() == -1) {
			vTaskDelay(POLL_DELAY_TICKS);
		}
	}

	stopZ();
}


/*
*	Calibration of the Z axis: moves DOWN until Z = 1.
*	Going down is always safe because Z = 1 Down is the bottom end.
*/
void homeZ() {

	if (getZPos() != 1) {

		moveZDown();

		while (getZPos() != 1) {
			vTaskDelay(POLL_DELAY_TICKS);
		}
	}

	stopZ();
}




  ////////////////////////////////////////////////////////////////////////////////////////
 //				HIGHER FUNCTIONS (sequential, only for manual tests)				   //
////////////////////////////////////////////////////////////////////////////////////////


/*
*	Given that the CAGE is already in front of the cell (Z Down position), puts the part in the cell.
*/
void putPartInCell() {

	movetoZUp();		// go to the corresponding Z Up position
	gotoY(3);			// move Y to position 3
	movetoZDown();		// go back to the Z Down of that same Z position
	gotoY(2);			// retract Y back to the default 2 position
}


/*
*	Given that the CAGE is already in front of the cell (Z Down position), takes the part from the cell.
*/
void takePartFromCell() {

	gotoY(3);			// from the Z Down position go to Y position 3
	movetoZUp();		// elevate the box by going to the corresponding Z Up position
	gotoY(2);			// bring the box into the cage by going to Y position 2
	movetoZDown();		// go back to the default Z Down position
}


/*
*	Moves the CAGE to the desired cell and puts the part in it.
*/
void placeItemOnCell(int x, int z) {

	gotoX(x);
	gotoZUp(z);
	putPartInCell();
	autoCallibrate();
}


/*
*	Moves the CAGE to the desired cell and takes the part from it.
*/
void takeItemFromCell(int x, int z) {

	gotoX(x);
	gotoZ(z);
	takePartFromCell();
	autoCallibrate();
}




  ////////////////////////////////////////////////////////////////////////////////////////
 //								UTILITY FUNCTIONS									   //
////////////////////////////////////////////////////////////////////////////////////////


/*
*	Stops the movement on all three axis.
*/
void stopAll() {
	stopX();
	stopY();
	stopZ();
}


/*
*	Sequential calibration: Y to the centre first (X and Z can only move with Y = 2),
*	then X = 1 and Z = 1.
*/
void autoCallibrate() {
	gotoY(2);
	homeX();
	homeZ();
}
