#pragma once


extern "C" {
#include <interface.h>
}


/*
		HARDWARE MAP

	Port 0 (inputs):  bit 0 = X is 3     | bit 1 = X is 2      | bit 2 = X is 1       | bit 3 = Y is 3
					  bit 4 = Y is 2     | bit 5 = Y is 1      | bit 6 = Z is 3 Up    | bit 7 = Z is 3 Down

	Port 1 (inputs):  bit 0 = Z is 2 Up  | bit 1 = Z is 2 Down | bit 2 = Z is 1 Up    | bit 3 = Z is 1 Down
					  bit 4 = BOX IN CAGE (active HIGH)        | bit 5 = Switch 1     | bit 6 = Switch 2
					  bit 7 = (unknown)

	Port 2 (outputs): bit 0 = upper light | bit 1 = lower light | bit 2 = Z down      | bit 3 = Z up
					  bit 4 = Y outside   | bit 5 = Y inside    | bit 6 = X left       | bit 7 = X right

	The position sensors are active LOW (0 = the cage is there).
	The actuators are active HIGH (1 = turned on).
*/


// bits that are not position sensors
#define P1_BIT_BOX_IN_CAGE		4
#define P1_BIT_SWITCH_1			5
#define P1_BIT_SWITCH_2			6
#define P2_BIT_LIGHT_UPPER		0
#define P2_BIT_LIGHT_LOWER		1

// polarity of the box sensor and of the switches:
// 1 = the bit is HIGH when it is active | 0 = the bit is LOW when it is active
// (if one of them ever works backwards, change it here and nowhere else)
#define BOX_SENSOR_ACTIVE_HIGH	1
#define SWITCH_ACTIVE_HIGH		1

// port 2 bits that move the axes (bits 2 to 7), the lights are not included
#define MOTION_MASK				0xFC

// ticks that a loop sleeps between two sensor readings.
// Do NOT use taskYIELD() in these loops: it never lets lower priority tasks run
#define POLL_DELAY_TICKS		1


// Bit manipulation functions
int getBitValue(uInt8 value, uInt8 bit_n);
void setBitValue(uInt8* value, int n_bit, int new_value_bit);


// Switches, box sensor and lights
void readSwitches(bool* switch1, bool* switch2);
bool isBoxInCage();
void setLights(bool upper, bool lower);


// X axis related functions
void moveXLeft();
void moveXRight();
void stopX();
int  getXPos();
void gotoX(int x);
void homeX();


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
int getZPos();
int getZUpPos();
void gotoZ(int z);
void gotoZUp(int z);
void movetoZUp();
void movetoZDown();
void homeZ();


// Higher functions (sequential versions, labwork1.cpp does not use them)
void putPartInCell();
void takePartFromCell();
void placeItemOnCell(int x, int z);
void takeItemFromCell(int x, int z);


// Utility functions
void stopAll();
void autoCallibrate();
