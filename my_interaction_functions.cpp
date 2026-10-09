// MY INTERATION FUNCTIONS

#include "my_interaction_functions.h"
#include <stdio.h>




  ////////////////////////////////////////////////////////////////////////////////////////
 //							  BIT MANIPULATIONS FUNCTIONS							   //
////////////////////////////////////////////////////////////////////////////////////////


/*
*	Given a byte value, returns the value of it's Nth bit. 
*/
int getBitValue(uInt8 value, uInt8 bit_n) {
	return (value & (1 << bit_n));
}

 
/*
*	Given a byte value, set the Nth bit to value (new_value_bit)
*/
void setBitValue(uInt8 *variable, int n_bit, int new_value_bit) {

	uInt8 mask_on = (uInt8)(1 << n_bit);
	uInt8 mask_off = ~mask_on;

	if (new_value_bit) {
		*variable |= mask_on;
	}
	else {
		*variable &= mask_off;
	}
}




  ////////////////////////////////////////////////////////////////////////////////////////
 //								X AXIS RELATED FUNCTIONS							   //
////////////////////////////////////////////////////////////////////////////////////////


/*
* Set the bits equivalent to move the X axis to the LEFT to high and RIGHT to low
*/
void moveXLeft() {

	uInt8 p2 = readDigitalU8(2);		// read Port 2

	setBitValue(&p2, 6, 1);			// set bit 6 to high level
	setBitValue(&p2, 7, 0);			// set bit 7 to low level

	writeDigitalU8(2, p2);			// update Port 2
	

	/*
			Schematics

		before:

			P2 = XXXXXXXX

		after:

			P2 = 01XXXXXX
	*/
}


/*
* Set the bits equivalent to move the X axis to the RIGHT to high and LEFT to low
*/
void moveXRight() {

	uInt8 p2 = readDigitalU8(2);		// read Port 2

	setBitValue(&p2, 6, 0);			// set bit 6 to low level
	setBitValue(&p2, 7, 1);			// set bit 7 to high level

	writeDigitalU8(2, p2);			// update Port 2

	/*
 			Schematics
	
	before:

		P2 = XXXXXXXX

	after:

		P2 = 10XXXXXX
	*/
}


/*
*	Set the bits of LEFT and RIGHT to low
*/
void stopX() {

	uInt8 p2 = readDigitalU8(2);		// read Port 2

	setBitValue(&p2, 6, 0);			// set bit 6 to low level
	setBitValue(&p2, 7, 0);			// set bit 7 to low level

	writeDigitalU8(2, p2);			// update Port 2

	/*
			Schematics
	
	before:

		P2 = XXXXXXXX

	after:

		P2 = 00XXXXXX
	*/

}


/*
*	Get current X position. One of the four possible values: 1, 2, 3 or -1
*/
int getXPos() {

	uInt8 p0 = readDigitalU8(0);			// read Port 0

	if (!getBitValue(p0, 2)) return 1;	// p0.2 = 0 (active low) -> X position = PosX_1

	if (!getBitValue(p0, 1)) return 2;	// p0.1 = 0 (active low) -> X position = PosX_2

	if (!getBitValue(p0, 0)) return 3;	// p0.0 = 0 (active low) -> X position = PosX_3

	return (-1);						// else X position is in between sensors
}


/*
*	Move the CAGE on the X axis to the desired x position (1, 2 or 3)
*/
void gotoX(int x) {

	int currentXPos = getXPos();

	if (currentXPos == -1) moveXRight();

	do {
		currentXPos = getXPos();
		
		if (currentXPos < x && currentXPos != -1) {
			moveXRight();
		}
		else if (currentXPos > x) {
			moveXLeft();
		}

	} while (currentXPos != x);
	
	stopX();
}




  ////////////////////////////////////////////////////////////////////////////////////////
 //								Y AXIS RELATED FUNCTIONS							   //
////////////////////////////////////////////////////////////////////////////////////////


/*
*	Set the bit for moving Y axis INSIDE to high and the bit for OUTSIDE to low
*/
void moveYInside() {

	uInt8 p2 = readDigitalU8(2);		// read Port 2

	setBitValue(&p2, 4, 0);			// set bit 4 to low level
	setBitValue(&p2, 5, 1);			// set bit 5 to high level

	writeDigitalU8(2, p2);			// update Port 2

	/*
			Schematics

	before:

		P2 = XXXXXXXX

	after:

		P2 = XX10XXXX
	*/
}


/*
*	Set the bit for moving Y axis OUTSIDE to hight and the bit for INSIDE to low
*/
void moveYOutside() {

	uInt8 p2 = readDigitalU8(2);	// read Port 2

	setBitValue(&p2, 5, 0);			// set bit 5 to low level
	setBitValue(&p2, 4, 1);			// set bit 4 to high level

	writeDigitalU8(2, p2);			// update Port 2

	/*
			Schematics

	before:

		P2 = XXXXXXXX

	after:

		P2 = XX01XXXX
	*/
}


/*
*	Set the bits for Y axis INSIDE and OUTSIDE both to low
*/
void stopY() {
	
	uInt8 p2 = readDigitalU8(2);	// read Port 2

	setBitValue(&p2, 4, 0);			// set bit 2 to low level
	setBitValue(&p2, 5, 0);			// set bit 3 to high level

	writeDigitalU8(2, p2);			// update Port 2

	/*
			Schematics

	before:

		P2 = XXXXXXXX

	after:

		P2 = XX00XXXX
	*/
}


/*
*	Get current Y position. One of the four possible values: 1, 2, 3 or -1
*/
int getYPos() {

	uInt8 p0 = readDigitalU8(0);			// read Port 0

	if (!getBitValue(p0, 5)) return 1;	// p0.5 = 0 (active low) -> Y position = PosY_1
	
	if (!getBitValue(p0, 4)) return 2;	// p0.4 = 0 (active low) -> Y position = PosY_2

	if (!getBitValue(p0, 3)) return 3;	// p0.3 = 0 (active low) -> Y position = PosY_3

	return (-1);						// else Y position is in between sensors
}


/*
*	Move the CAGE on the Y axis to the desired x position (1, 2 or 3)
*/
void gotoY(int y) {

	int currentYPos = getYPos();

	if (currentYPos == -1) moveYInside();

	do {
		currentYPos = getYPos();

		if (currentYPos < y && currentYPos != -1) {
			moveYInside();
		}
		else if (currentYPos > y) {
			moveYOutside();
		}

	} while (currentYPos != y);

	stopY();
}




  ////////////////////////////////////////////////////////////////////////////////////////
 //								Z AXIS RELATED FUNCTIONS							   //
////////////////////////////////////////////////////////////////////////////////////////


/*
*	Set the bit for moving Z axis UP to hight and the bit for DOWN to low
*/
void moveZUp() {

	uInt8 p2 = readDigitalU8(2);	// read Port 2

	setBitValue(&p2, 2, 0);			// set bit 2 to low level
	setBitValue(&p2, 3, 1);			// set bit 3 to high level

	writeDigitalU8(2, p2);			// update Port 2

	/*
			Schematics

	before:

		P2 = XXXXXXXX

	after:

		P2 = XXXX10XX
	*/
}


/*
*	 Set the bit for moving Z axis DOWN to hight and the bit for UP to low
*/
void moveZDown() {

	uInt8 p2 = readDigitalU8(2);	// read Port 2

	setBitValue(&p2, 2, 1);			// set bit 2 to high level
	setBitValue(&p2, 3, 0);			// set bit 3 to low level

	writeDigitalU8(2, p2);			// update Port 2

	/*
			Schematics

	before:

		P2 = XXXXXXXX

	after:

		P2 = XXXX01XX
	*/
}


/*
*	Set the bits for Z axis UP and DOWN both to low
*/
void stopZ() {

	uInt8 p2 = readDigitalU8(2);	// read Port 2

	setBitValue(&p2, 2, 0);			// set bit 2 to low level
	setBitValue(&p2, 3, 0);			// set bit 3 to low level

	writeDigitalU8(2, p2);			// update Port 2

	/*
			Schematics

	before:

		P2 = XXXXXXXX

	after:

		P2 = XXXX00XX
	*/
}


/*
*	Get current Z position. One of the four possible values: 1, 2, 3 or -1
*/
int getZPos() {

	uInt8 p0 = readDigitalU8(0);
	uInt8 p1 = readDigitalU8(1);

	if (!getBitValue(p1, 3)) return 1;	// p1.3 = 0 (active low) -> Z = 1 Down 

	if (!getBitValue(p1, 1)) return 2;	// p1.1 = 0 (active low) -> Z = 2 Down

	if (!getBitValue(p0, 7)) return 3;	// p0.7 = 0 (active low) -> Z = 3 Down

	return (-1);
}


/*
*	Move the CAGE on the Z axis to the desired Z Up position (1, 2, 3)
*/
int getZUpPos() {

	uInt8 p0 = readDigitalU8(0);
	uInt8 p1 = readDigitalU8(1);

	if (!getBitValue(p1, 2)) return 1;	// p1.2 = 0 (active low) -> Z = 1 Up 

	if (!getBitValue(p1, 0)) return 2;	// p1.0 = 0 (active low) -> Z = 2 Up

	if (!getBitValue(p0, 6)) return 3;	// p0.6 = 0 (active low) -> Z = 3 Up

	return (-1);
}


/*
*  Move the CAGE on the Y axis to the desired x position (1, 2 or 3)
*/
void gotoZ(int z) {

	int currentZPos = getZPos();

	if (currentZPos == -1) moveZUp();

	do {
		currentZPos = getZPos();

		if (currentZPos < z && currentZPos != -1) {
			moveZUp();
		}
		else if (currentZPos > z) {
			moveZDown();
		}

	} while (currentZPos != z);

	stopZ();
}

void gotoZUp(int z) {

	int currentZPos = getZUpPos();

	if (currentZPos == -1) moveZUp();

	do {
		currentZPos = getZUpPos();

		if (currentZPos < z && currentZPos != -1) {
			moveZUp();
		}
		else if (currentZPos > z) {
			moveZDown();
		}

	} while (currentZPos != z);

	stopZ();
}


/*
*	Move Z axis in the UP direction until finding the corresponding Z UP position
*/
void movetoZUp() {

	int currentZPos = getZUpPos();

	do {
		moveZUp();
		currentZPos = getZUpPos();
	} while (currentZPos == -1);

	stopZ();
}


/*
*	Move the Z axis in the DOWN direction until finding the corresponding Z DOWN position
*/
void movetoZDown() {

	int currentZPos = -1;
	
	do {
		moveZDown();
		currentZPos = getZPos();
	} while (currentZPos == -1);

	stopZ();
}



  ////////////////////////////////////////////////////////////////////////////////////////
 //								HIGHER FUNCTIONS									   //
////////////////////////////////////////////////////////////////////////////////////////


/*
*	Givem that the CAGE is already in the desired position, executes the sequence of movements
*	to place the part in the corresponding cell
*/
void putPartInCell() {

	movetoZUp();		// go to corresponding z Up position
	gotoY(3);		// move y to position 3
	movetoZDown();	// go back to Z Down of that same Z position
	gotoY(2);		// retract y back to the default 2 position
}


/*
*	Givem that the CAGE is already in the desired position, executes the sequence of movements
*	to place the part in the corresponding cell
*/
void takePartFromCell() {

	gotoY(3);		// from the z down position go to y position 3
	movetoZUp();		// elevate box by going to corresponding z up position
	gotoY(2);		// bring the box into the cage by going to y position 2
	movetoZDown();	// go back to default z down position
}


/*
*	Moves CAGE to the desired cell and calls putPartInCell()
*/
void placeItemOnCell(int x, int z) {
	gotoX(x);			// go to desired X position
	gotoZUp(z);			// go to desired Z position
	putPartInCell();	// place the item in the cell
	autoCallibrate();	// calibrate the cage
}


/*
*	Moves CAGE to the desired cell and calls takePartFromCell()
*/
void takeItemFromCell(int x, int z) {
	gotoX(x);				// go to desired X position
	gotoZ(z);				// go to desired Z position
	takePartFromCell();		// take the item from the cell
	autoCallibrate();		// calibrate the cage
}




  ////////////////////////////////////////////////////////////////////////////////////////
 //								UTILLITY FUNCTIONS									   //
////////////////////////////////////////////////////////////////////////////////////////


/*
*	Stops movement on all three axis.
*/
void stopAll() {
	stopX();
	stopY();
	stopZ();
}


/*
*	Positions the cage at X = 1 and Z = 1 at the start of the program
*/
void autoCallibrate() {
	gotoX(1);
	gotoZ(1);
}