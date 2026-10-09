// labwork1.cpp : This file contains the 'main' function. Program execution begins and ends there.

#include <iostream>
#include <conio.h>
#include <stdlib.h>
#include <windows.h>
#include "my_interaction_functions.h"

extern "C" {
    #include <interface.h>
}

void printMenu();

int main(int argc, char** argv) {

    // INPUT PORTS
    createDigitalInput(0);
    createDigitalInput(1);

    // OUTPUT PORTS
    createDigitalOutput(2);


    printMenu();

    int tecla = 0;
    while (tecla != 27) {

        tecla = _getch();

        if (tecla == 'd') {
            moveXRight();
            printf("\tmoving right...\n");
        }
        if (tecla == 'a') {
            moveXLeft();
            printf("\tmoving left...\n");
        }
        if (tecla == 'w') {
            moveZUp();
            printf("\tmoving up...\n");
        }
        if (tecla == 's') {
            moveZDown();
            printf("\tmoving down...\n");
        }
        if (tecla == 'b') {
            printf("stopping all movements\n");
            stopAll();
            printf("\tstoped all movements\n");
        }
        if (tecla == 'i') {
            moveYInside();
            printf("\tmoving inside...\n");
        }
        if (tecla == 'o') {
            moveYOutside();
            printf("\tmoving outside...\n");
        }


        if (tecla == 'p') {
            printf("\tPositions {\n\tX\tY\tZ\n\t%d\t%d\t%d\n}\n", getXPos(), getYPos(), getZPos());
        }



        if (tecla == '1') {
            printf("\tmoving X to position 1\n");
            gotoX(1);
            printf("cage at X = 1\n");
        }
        if (tecla == '2') {
            printf("\tmoving X to position 2\n");
            gotoX(2);
            printf("cage at X = 2\n");
        }
        if (tecla == '3') {
            printf("\tmoving X to position 3\n");
            gotoX(3);
            printf("cage at X = 3\n");
        }
        if (tecla == '4') {
            printf("\tmoving Z to position 1\n");
            gotoZ(1);
            printf("cage at Z = 1\n");
        }
        if (tecla == '5') {
            printf("\tmoving Z to position 2\n");
            gotoZ(2);
            printf("cage at Z = 2\n");
        }
        if (tecla == '6') {
            printf("\tmoving Z to position 3\n");
            gotoZ(3);
            printf("cage at Z = 3\n");
        }
        if (tecla == '7') {
            printf("\tmoving Y to position 1\n");
            gotoY(1);
            printf("\tcage at Y = 1\n");
        }
        if (tecla == '8') {
            printf("\tmoving Y to position 2\n");
            gotoY(2);
            printf("cage at Y = 2\n");
        }
        if (tecla == '9') {
            printf("\tmoving Y to position 3\n");
            gotoY(3);
            printf("\tcage at Y = 3\n");
        }


        if (tecla == 'k') {
            printf("\tputting part in cell...\n");
            putPartInCell();
            printf("\tpart placed in cell: (%d, %d)\n", getXPos(), getZPos());
        }
        if (tecla == 'l') {
            printf("\taking part from cell...\n");
            takePartFromCell();
            printf("\tpart taken from cell: (%d, %d)\n", getXPos(), getZPos());
        }


        if (tecla == 'z') {
            printf("\tmoving z to up position...\n");
            movetoZUp();
            printf("\tz at up position on z: %d\n", getZPos());
        }
        if (tecla == 'x') {
            printf("\tmoving z to down position...\n");
            movetoZDown();
            printf("\tz at down position on z: %d\n", getZPos());
        }


        if (tecla == 'c') {
            char choice = ' ';
            printf("\n\tDo you want to take(t) or place(p) the item on the compact storage?\n");

            while (choice != 'p' && choice != 't') {
                choice = _getch();
                if (choice != 'p' && choice != 't') {
                    printf("\tplease select a valid choice (t or p)\n");
                }
            }

            int x, z;

            if (choice == 'p') {

                printf("\tselect the (X, Z) position to place the item: ");
                scanf("%d %d", &x, &z);
                printf("\tplacing item on (%d, %d)...\n", x, z);
                placeItemOnCell(x, z);
                printf("\titem placed.");
            }
            else if (choice == 't') {

                printf("\tselect the (X, Z) position from which to take the item: ");
                scanf("%d %d", &x, &z);
                printf("\ttaking item from (%d, %d)...\n", x, z);
                takeItemFromCell(x, z);
                printf("\titem taken.");
            }
            else {
                printf("\tsomething went wrong, please try again\n");
            }
        }
    }
    closeChannels();

    return 0;

}


void printMenu() {
    //printf("\n\tcallibrate kit manually and press enter...\n\n");

    printf("\n\tauto-callibration in process...\n");
    autoCallibrate();
    printf("\tsystem callibrated. Cage at X = 1; Y = 2; Z = 1\n");

    printf("\tCOMMANDS:\n\n");
    printf("\t\twasd -> move on the X and Z axis\n");
    printf("\t\tb (break) -> stop movement on all axis\n");
    printf("\t\tp (position) -> print the current XYZ positions\n");
    printf("\t\ti -> move Y inside\n");
    printf("\t\to -> move Y outside\n\n");
    printf("\t\t[1, 2, 3] -> [gotoX(1), gotoX(2), gotoX(3)]\n");
    printf("\t\t[4, 5, 6] -> [gotoZ(1), gotoZ(2), gotoZ(3)]\n");
    printf("\t\t[7, 8, 9] -> [gotoY(1), gotoY(2), gotoY(3)]\n\n");

    printf("\t\tk -> put part in cell\n");
    printf("\t\tl -> take part from cell\n");
    printf("\t\tz -> move z to up position of corresponding z down position\n");
    printf("\t\tx -> move z to down position of corresponding z up position\n\n");

    printf("\t\tc -> enters the take/place items menu\n\n");
}





// Run program: Ctrl + F5 or Debug > Start Without Debugging menu
// Debug program: F5 or Debug > Start Debugging menu

// Tips for Getting Started: 
//   1. Use the Solution Explorer window to add/manage files
//   2. Use the Team Explorer window to connect to source control
//   3. Use the Output window to see build output and other messages
//   4. Use the Error List window to view errors
//   5. Go to Project > Add New Item to create new code files, or Project > Add Existing Item to add existing code files to the project
//   6. In the future, to open this project again, go to File > Open > Project and select the .sln file






  ////////////////////////////////////////////////////////////////////////////////////////
 //							ALTERNATIVE MAIN FUNCTIONS	    						   //
////////////////////////////////////////////////////////////////////////////////////////

/*

    Other main code examples: 


    /////////////////////////////////////////////////////////////////////////////////////////
    1. 


    int main(int argc, char **argv) {
        
        createDigitalInput(0);
        createDigitalInput(1);
        createDigitalInput(2);

        uInt8 p0 = 0;
        uInt8 p2 = 0;

        writeDigitalU8(2, 0x80);            // start moving right
        while(!_kbhit()) {                  // do until a key is pressed

            p0 = readDigitalU8(0);          // reading sensor values
            p2 = readDigitalU8(2);          //

            if(!(p0 & (1 << 0))) {          // reached right side?
                
                p2 &= ~(1 << 7);
                p2 |= (1 << 6);

                writeDigitalU8(2, p2);      // move left
            }
            if (!(p0 & (1 << 2))) {
                
                p2 &= ~(1 << 6);
                p2 |= (1 << 7);

                writeDigitalU8(2, p2);
            }
        }
        writeDigitalU8(2, 0);
        closeChannels();

        return 0;
    }



    //////////////////////////////////////////////////////////////////
    2.

    int main(int argc, char **argv) {

        // INPUT PORTS
        createDigitalInput(0);
        createDigitalInput(1);

        // OUTPUT PORTS
        createDigitalOutput(2);

        printf("\ncallibrate kit manually and press enter...");

        int tecla = 0;
        while (tecla != 27) {
            tecla = _getch();
            if (tecla == 'd') moveXRight();
            if (tecla == 'a') moveXLeft();
            if (tecla == 's') stopX();
            if (tecla == 'w') printf("vai mover para cima\n");
            if (tecla == 'x') printf("vai mover para baixo\n");

            // to complete...
        }

        closeChannels();
    }
*/