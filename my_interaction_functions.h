#pragma once


extern "C" {
#include <interface.h>
}

int getBitValue(uInt8 value, uInt8 bit_n);
void setBitValue(uInt8* value, int n_bit, int new_value_bit);


// X axis related functions
void moveXLeft();
void moveXRight();
void stopX();
int  getXPos();
void gotoX(int x);


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


// Higher functions
void putPartInCell();
void takePartFromCell();
void placeItemOnCell(int x, int z);
void takeItemFromCell(int x, int z);


// Utility functions
void stopAll();
void autoCallibrate();


// Other function headers