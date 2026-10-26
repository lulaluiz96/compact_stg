#pragma once

extern "C" {
#include <interface.h>
}

/*
	HARDWARE MAP   (sensors active LOW, actuators active HIGH, switches active HIGH)

	Port 0 : bit0 X=3 | bit1 X=2 | bit2 X=1 | bit3 Y=3 | bit4 Y=2 | bit5 Y=1 | bit6 Z=3 Up | bit7 Z=3 Down
	Port 1 : bit0 Z=2 Up | bit1 Z=2 Down | bit2 Z=1 Up | bit3 Z=1 Down | bit4 BOX IN CAGE
			 bit5 Switch 1 (upper) | bit6 Switch 2 (lower) | bit7 ?
	Port 2 : bit2 Z down | bit3 Z up | bit4 Y outside | bit5 Y inside | bit6 X left | bit7 X right
			 bit0 / bit1 : the two lights (ASSUMED - confirm with the light test key in the menu)
*/
#define P1_BIT_BOX_IN_CAGE   4
#define P1_BIT_SWITCH_1      5
#define P1_BIT_SWITCH_2      6
#define P2_BIT_LIGHT_UPPER   0
#define P2_BIT_LIGHT_LOWER   1
#define P2_MOTION_MASK       0xFC      // port 2 bits 2..7 drive the axes

// Polling loops sleep this many ticks between sensor reads. NEVER busy-wait with taskYIELD():
// it only yields to tasks of equal/higher priority, so lower-priority tasks (and the other
// axis) would be starved and X/Z would no longer move in parallel.
#define POLL_DELAY_TICKS     1


int  getBitValue(uInt8 value, uInt8 bit_n);
void setBitValue(uInt8* value, int n_bit, int new_value_bit);


// Switches, box sensor, lights (FR1.7, FR1.8)
void readSwitches(bool* switch1, bool* switch2);
bool isBoxInCage();
void setLights(bool upper, bool lower);

// Save / restore the axis output bits (used by emergency STOP / RESUME)
uInt8 saveMotionBits();
void  restoreMotionBits(uInt8 bits);


// X axis related functions
void moveXLeft();
void moveXRight();
void stopX();
int  getXPos();
void gotoX(int x);
void homeX();            // move left until X = 1 (always safe, never overshoots the right end)


// Y axis related function
void moveYInside();
void moveYOutside();
void stopY();
int  getYPos();
void gotoY(int y);


// Z axis related functions
void moveZUp();
void moveZDown();
void stopZ();
int  getZPos();          // Z "Down" positions 1..3 or -1
int  getZUpPos();        // Z "Up" positions 1..3 or -1
void gotoZ(int z);
void gotoZUp(int z);
void movetoZUp();
void movetoZDown();
void homeZ();            // move down until Z = 1 (always safe, never overshoots the top)


// Higher functions (blocking, sequential - NOT used by the RTOS version of labwork1.cpp)
void putPartInCell();
void takePartFromCell();
void placeItemOnCell(int x, int z);
void takeItemFromCell(int x, int z);


// Utility functions
void stopAll();
void autoCallibrate();   // sequential: Y -> 2, X -> 1, Z -> 1
bool isHomed();          // X == 1, Y == 2, Z == 1
