// labwork1.cpp : Automated Component Store with FIFO dispensing (FreeRTOS)
//
//	TASKS
//		supervisorTask   reads the two switches: pulses = requests, both = STOP, then RESUME / RESET
//		warehouseTask    does the requests one at a time (store / dispense)
//		xTask, yTask, zTask   the only tasks that move the cage (X and Z move at the same time)
//		lightsTask       flashes the two lights
//		menuTask         keyboard menu and information queries
//
//	MAILBOXES (queues)
//		requestQueue     menu / supervisor -> warehouseTask   (up to 10 requests waiting)
//		mbx_X, mbx_Y, mbx_Z   warehouseTask -> axis task      (1 command at a time)
//
//	SEMAPHORES
//		sem_X_finished, sem_Y_finished, sem_Z_finished   axis task -> warehouseTask ("I arrived")
//
//	MUTEXES
//		inventoryMutex   protects the cells, the history and the counters
//		printMutex       keeps two tasks from mixing the colors of the console



#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <iostream>
#include <conio.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <windows.h>
#include "my_interaction_functions.h"

extern "C" {
#include <FreeRTOS.h>
#include <task.h>
#include <queue.h>
#include <timers.h>
#include <semphr.h>
#include <interrupts.h>
#include <interface.h>
}

#define mainREGION_1_SIZE 8201
#define mainREGION_2_SIZE 29905
#define mainREGION_3_SIZE 7607


  ////////////////////////////////////////////////////////////////////////////////////////
 //								CONSTANTS (change them here)						   //
////////////////////////////////////////////////////////////////////////////////////////

// warehouse size
#define NUM_X                   3
#define NUM_Z                   3
#define MAX_HISTORY             100

// times in milliseconds
#define SUPERVISOR_PERIOD_MS    10          // how often the switches are read
#define DEBOUNCE_SAMPLES        2           // equal readings in a row to accept a switch change
#define PULSE_TIMEOUT_MS        1500        // no new pulse for this time = the number is complete
#define LIGHTS_PERIOD_MS        50
#define BLINK_SLOW_TICKS        10          // half period of the slow blink  (10 * 50 ms)
#define BLINK_FAST_TICKS        2           // half period of the fast blink  (2 * 50 ms)
#define CAGE_CLEAR_TIMEOUT_MS   30000       // time given to take the box out of the cage before a dispense
#define CALIBRATION_RETRIES     3

// milliseconds -> ticks (never 0)
#define MS_TO_TICKS(ms)         ((pdMS_TO_TICKS(ms) > 0) ? pdMS_TO_TICKS(ms) : 1)

// task priorities. The warehouse is above the axis tasks so it sends the X and Z commands
// BEFORE any of them starts, that is what makes X and Z move together
#define PRIO_MENU               (tskIDLE_PRIORITY + 1)
#define PRIO_LIGHTS             (tskIDLE_PRIORITY + 2)
#define PRIO_AXIS               (tskIDLE_PRIORITY + 3)
#define PRIO_WAREHOUSE          (tskIDLE_PRIORITY + 4)
#define PRIO_SUPERVISOR         (tskIDLE_PRIORITY + 5)

#define STACK_SMALL             (configMINIMAL_STACK_SIZE * 4)
#define STACK_BIG               (configMINIMAL_STACK_SIZE * 8)

// system states
#define STATE_IDLE              0
#define STATE_STORING           1
#define STATE_DISPENSING        2
#define STATE_STOPPED           3

// request types
#define REQ_STORE               0
#define REQ_DISPENSE            1

// commands for the axis tasks
#define X_GOTO                  0
#define X_HOME                  1
#define Z_GOTO_DOWN             0
#define Z_GOTO_UP               1
#define Z_FIND_UP               2
#define Z_FIND_DOWN             3
#define Z_HOME                  4

// state of each cell
#define CELL_EMPTY              0
#define CELL_RESERVED           1           // a component is on its way to this cell
#define CELL_STORED             2
#define CELL_DISPENSING         3           // the component is being taken out

// menu states
#define MENU_MAIN               0
#define MENU_ASK_STORE_ID       1
#define MENU_ASK_DISPENSE       2
#define MENU_ASK_FAMILY_INFO    3
#define MENU_ASK_CELL_INFO      4

// console colors: errors red, warnings yellow, success green, everything else white
#define COLOR_WHITE             (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE)
#define COLOR_RED               (FOREGROUND_RED | FOREGROUND_INTENSITY)
#define COLOR_YELLOW            (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY)
#define COLOR_GREEN             (FOREGROUND_GREEN | FOREGROUND_INTENSITY)

// extended ASCII characters (code page 437) to draw the menu table
#define BOX_TL                  "\xDA"      // top left corner
#define BOX_TR                  "\xBF"      // top right corner
#define BOX_BL                  "\xC0"      // bottom left corner
#define BOX_BR                  "\xD9"      // bottom right corner
#define BOX_V                   "\xB3"      // vertical line
#define BOX_TM                  "\xC2"      // top middle
#define BOX_BM                  "\xC1"      // bottom middle
#define BOX_LM                  "\xC3"      // left middle
#define BOX_RM                  "\xB4"      // right middle
#define BOX_CR                  "\xC5"      // cross
#define BOX_H                   "\xC4"      // horizontal line
#define BOX_H2                  BOX_H BOX_H
#define BOX_H4                  BOX_H2 BOX_H2
#define BOX_H8                  BOX_H4 BOX_H4
#define BOX_H16                 BOX_H8 BOX_H8
#define BOX_H32                 BOX_H16 BOX_H16


  ////////////////////////////////////////////////////////////////////////////////////////
 //								DATA, MAILBOXES, SEMAPHORES							   //
////////////////////////////////////////////////////////////////////////////////////////

// a request to store a component (id) or to dispense a family
struct Request {
    int type;
    int id;
    int family;
};

// command for the X task
struct XCommand {
    int mode;
    int x;
};

// command for the Z task
struct ZCommand {
    int mode;
    int z;
};

// one cell of the warehouse
struct Cell {
    int state;
    int id;
    time_t storedTime;      // when the storage was completed
    int order;              // storage completion order (this is what the FIFO uses)
};

// one completed dispensing service
struct HistoryRecord {
    int id;
    int family;
    time_t time;
    long duration;          // seconds in the warehouse
};

// mailboxes
QueueHandle_t requestQueue;
QueueHandle_t mbx_X;
QueueHandle_t mbx_Y;
QueueHandle_t mbx_Z;

// semaphores and mutexes
SemaphoreHandle_t sem_X_finished;
SemaphoreHandle_t sem_Y_finished;
SemaphoreHandle_t sem_Z_finished;
SemaphoreHandle_t inventoryMutex;
SemaphoreHandle_t printMutex = NULL;

// task handles (needed to suspend, resume and delete tasks)
TaskHandle_t hMenu, hSupervisor, hLights, hWarehouse, hX, hY, hZ;

// system state
volatile int systemState = STATE_IDLE;
volatile int stateBeforeStop = STATE_IDLE;
volatile bool warehouseBusy = false;        // the warehouse is doing a request
volatile bool manualMoving = false;         // a manual key started a movement
volatile bool resetPending = false;         // the warehouse task was restarted by a RESET

// what the supervisor saves when the emergency STOP happens
uInt8 savedMotionBits = 0;                  // the axis outputs that were on
int savedX = 0;                             // X and Z positions (-1 = between two sensors)
int savedZ = 0;

// warehouse data
Cell cells[NUM_X][NUM_Z];                   // cells[x - 1][z - 1]
int orderCounter = 0;
HistoryRecord history[MAX_HISTORY];
int historyCount = 0;
int countFamily[4] = { 0, 0, 0, 0 };        // index 1 to 3
int countTotal = 0;


  ////////////////////////////////////////////////////////////////////////////////////////
 //						HEAP AND ASSERT (same as the template)						   //
////////////////////////////////////////////////////////////////////////////////////////

void initialiseHeap(void)
{
    static uint8_t ucHeap[configTOTAL_HEAP_SIZE];
    /* Just to prevent 'condition is always true' warnings in
    configASSERT(). */
    volatile uint32_t ulAdditionalOffset = 19;
    const HeapRegion_t xHeapRegions[] =
    {
        /* Start address with dummy offsetsSize */
        { ucHeap + 1,mainREGION_1_SIZE },
        { ucHeap + 15 + mainREGION_1_SIZE,mainREGION_2_SIZE },
        { ucHeap + 19 + mainREGION_1_SIZE +
        mainREGION_2_SIZE,mainREGION_3_SIZE },
        { NULL, 0 }
    };
    configASSERT((ulAdditionalOffset +
        mainREGION_1_SIZE +
        mainREGION_2_SIZE +
        mainREGION_3_SIZE) < configTOTAL_HEAP_SIZE);
    /* Prevent compiler warnings when configASSERT() is not defined. */
    (void)ulAdditionalOffset;
    vPortDefineHeapRegions(xHeapRegions);
}


void vAssertCalled(unsigned long ulLine, const char* const pcFileName)
{
    static BaseType_t xPrinted = pdFALSE;
    volatile uint32_t ulSetToNonZeroInDebuggerToContinue = 0;

    /* Called if an assertion passed to configASSERT() fails. See
    http://www.freertos.org/a00110.html#configASSERT for more information. */
    /* Parameters are not used. */

    (void)ulLine;
    (void)pcFileName;

    printf("ASSERT! Line %ld, file %s, GetLastError() %ld\r\n", ulLine, pcFileName,
        GetLastError());
    taskENTER_CRITICAL();
    {
        /* Cause debugger break point if being debugged. */
        __debugbreak();
        /* You can step out of this function to debug the assertion by using
        the debugger to set ulSetToNonZeroInDebuggerToContinue to a non-zero
        value. */
        while (ulSetToNonZeroInDebuggerToContinue == 0)
        {
            __asm { NOP };
            __asm { NOP };
        }
    }
    taskEXIT_CRITICAL();
}




  ////////////////////////////////////////////////////////////////////////////////////////
 //								SMALL USEFUL FUNCTIONS								   //
////////////////////////////////////////////////////////////////////////////////////////


/*
*   Same as printf but with a color (COLOR_WHITE, COLOR_RED, COLOR_YELLOW or COLOR_GREEN).
*   The mutex makes sure two tasks do not print at the same time and mix the colors.
*/
void printColor(WORD color, const char* format, ...) {

    char text[512];
    va_list arguments;

    va_start(arguments, format);
    vsnprintf(text, sizeof(text), format, arguments);
    va_end(arguments);

    if (printMutex != NULL) {
        xSemaphoreTake(printMutex, portMAX_DELAY);
    }

    HANDLE console = GetStdHandle(STD_OUTPUT_HANDLE);

    SetConsoleTextAttribute(console, color);
    printf("%s", text);
    fflush(stdout);
    SetConsoleTextAttribute(console, COLOR_WHITE);

    if (printMutex != NULL) {
        xSemaphoreGive(printMutex);
    }
}


/*
*   Writes a date and time like "27/10/2026 14:30:05" in text (text needs at least 32 characters).
*/
void formatTime(time_t time, char* text) {

    struct tm timeInfo;

    localtime_s(&timeInfo, &time);
    strftime(text, 32, "%d/%m/%Y %H:%M:%S", &timeInfo);
}


/*
*   Writes a duration in seconds like "3m 05s" in text (text needs at least 32 characters).
*/
void formatDuration(long seconds, char* text) {
    snprintf(text, 32, "%ldm %02lds", (seconds / 60), (seconds % 60));
}


/*
*   Returns the family (1, 2 or 3) of a component ID: 1,4,7 -> 1 | 2,5,8 -> 2 | 3,6,9 -> 3
*/
int getFamily(int id) {
    return (((id - 1) % 3) + 1);
}


/*
*   Changes the system state. If an emergency STOP is active, only the state to go back to is changed,
*   so the warehouse can never overwrite STATE_STOPPED by accident.
*/
void setSystemState(int newState) {

    taskENTER_CRITICAL();

    if (systemState == STATE_STOPPED) {
        stateBeforeStop = newState;
    }
    else {
        systemState = newState;
    }

    taskEXIT_CRITICAL();
}


/*
*   Puts a request in the request mailbox. Used by the menu and by the switches.
*   Never blocks. Returns false if the mailbox is full.
*/
bool submitRequest(int type, int id, int family) {

    Request request = { type, id, family };

    return (xQueueSend(requestQueue, &request, 0) == pdTRUE);
}




  ////////////////////////////////////////////////////////////////////////////////////////
 //							MOVEMENT (used by the warehouse task)					   //
////////////////////////////////////////////////////////////////////////////////////////


/*
*   Sends the Y axis to position y and waits until it arrives.
*/
void moveY(int y) {

    xQueueSend(mbx_Y, &y, portMAX_DELAY);
    xSemaphoreTake(sem_Y_finished, portMAX_DELAY);
}


/*
*   Sends a command to the Z axis (mode = Z_GOTO_DOWN, Z_GOTO_UP, Z_FIND_UP, Z_FIND_DOWN) and waits until it finishes.
*/
void moveZ(int mode, int z) {

    ZCommand command = { mode, z };

    xQueueSend(mbx_Z, &command, portMAX_DELAY);
    xSemaphoreTake(sem_Z_finished, portMAX_DELAY);
}


/*
*   Moves the cage to X = x and Z = z. Both axes move AT THE SAME TIME: we send the two commands first
*   and only then wait for the two semaphores. Y goes to the centre first (X and Z only move with Y = 2).
*/
void moveXZ(int x, int z, int zMode) {

    if (getYPos() != 2) {
        moveY(2);
    }

    XCommand xCommand = { X_GOTO, x };
    ZCommand zCommand = { zMode, z };

    xQueueSend(mbx_X, &xCommand, portMAX_DELAY);
    xQueueSend(mbx_Z, &zCommand, portMAX_DELAY);

    xSemaphoreTake(sem_X_finished, portMAX_DELAY);
    xSemaphoreTake(sem_Z_finished, portMAX_DELAY);
}




  ////////////////////////////////////////////////////////////////////////////////////////
 //									AXIS TASKS										   //
////////////////////////////////////////////////////////////////////////////////////////


/*
*   X task: waits for a command, moves the X axis and signals that it finished.
*/
void xTask(void* pvParameters) {

    XCommand command;

    while (true) {

        xQueueReceive(mbx_X, &command, portMAX_DELAY);

        if (getYPos() != 2) {
            printColor(COLOR_RED, "\t[X] move refused: Y is not at the centre\n");
        }
        else if (command.mode == X_HOME) {
            homeX();
        }
        else if ((command.x >= 1) && (command.x <= 3)) {
            gotoX(command.x);
        }
        else {
            printColor(COLOR_RED, "\t[X] move refused: invalid position %d\n", command.x);
        }

        xSemaphoreGive(sem_X_finished);
    }
}


/*
*   Y task: waits for a position, moves the Y axis and signals that it finished.
*/
void yTask(void* pvParameters) {

    int y;

    while (true) {

        xQueueReceive(mbx_Y, &y, portMAX_DELAY);

        if ((y >= 1) && (y <= 3)) {
            gotoY(y);
        }
        else {
            printColor(COLOR_RED, "\t[Y] move refused: invalid position %d\n", y);
        }

        xSemaphoreGive(sem_Y_finished);
    }
}


/*
*   Z task: waits for a command, moves the Z axis and signals that it finished.
*   The travel commands need Y at the centre. FIND_UP / FIND_DOWN lift or lower a pallet with
*   Y extended, so they are allowed with Y outside the centre.
*/
void zTask(void* pvParameters) {

    ZCommand command;

    while (true) {

        xQueueReceive(mbx_Z, &command, portMAX_DELAY);

        if (command.mode == Z_FIND_UP) {
            movetoZUp();
        }
        else if (command.mode == Z_FIND_DOWN) {
            movetoZDown();
        }
        else if (getYPos() != 2) {
            printColor(COLOR_RED, "\t[Z] move refused: Y is not at the centre\n");
        }
        else if (command.mode == Z_HOME) {
            homeZ();
        }
        else if ((command.z < 1) || (command.z > 3)) {
            printColor(COLOR_RED, "\t[Z] move refused: invalid position %d\n", command.z);
        }
        else if (command.mode == Z_GOTO_DOWN) {
            gotoZ(command.z);
        }
        else {
            gotoZUp(command.z);
        }

        xSemaphoreGive(sem_Z_finished);
    }
}




  ////////////////////////////////////////////////////////////////////////////////////////
 //								WAREHOUSE TASK										   //
////////////////////////////////////////////////////////////////////////////////////////


/*
*   Stores the component with this ID. The checks are made NOW (when the request starts):
*   a rejected request never moves the cage.
*/
void handleStore(int id) {

    int x = 0;
    int z = 0;
    bool alreadyStored = false;
    bool cellFound = false;

    // there must be a box in the cage
    if (isBoxInCage() == false) {
        printColor(COLOR_YELLOW, "\n\tstore %d rejected: there is no component in the cage\n", id);
        return;
    }

    // look for the ID in the warehouse and for the first empty cell (the mutex keeps the menu out meanwhile)
    xSemaphoreTake(inventoryMutex, portMAX_DELAY);

    for (int cx = 0; cx < NUM_X; cx++) {
        for (int cz = 0; cz < NUM_Z; cz++) {
            if ((cells[cx][cz].state != CELL_EMPTY) && (cells[cx][cz].id == id)) {
                alreadyStored = true;
            }
        }
    }

    if (alreadyStored == false) {
        for (int cz = 0; cz < NUM_Z; cz++) {
            for (int cx = 0; cx < NUM_X; cx++) {
                if ((cellFound == false) && (cells[cx][cz].state == CELL_EMPTY)) {
                    cells[cx][cz].state = CELL_RESERVED;        // nobody else can use this cell now
                    cells[cx][cz].id = id;
                    x = (cx + 1);
                    z = (cz + 1);
                    cellFound = true;
                }
            }
        }
    }

    xSemaphoreGive(inventoryMutex);

    if (alreadyStored == true) {
        printColor(COLOR_YELLOW, "\n\tstore %d rejected: this ID is already stored or in transit\n", id);
        return;
    }

    if (cellFound == false) {
        printColor(COLOR_YELLOW, "\n\tstore %d rejected: the warehouse is full\n", id);
        return;
    }

    printColor(COLOR_WHITE, "\n\tstoring ID %d (family %d) in cell (%d,%d)...\n", id, getFamily(id), x, z);
    setSystemState(STATE_STORING);              // the upper light starts to flash

    moveXZ(x, z, Z_GOTO_UP);                    // X and Z move together, Z goes to the "up" position of the cell
    moveY(3);                                   // Y goes into the cell
    moveZ(Z_FIND_DOWN, 0);                      // Z goes down, the pallet stays on the cell

    // the pallet is in the cell: this is the storage completion, so save the time and the FIFO order
    time_t storedTime = time(NULL);

    xSemaphoreTake(inventoryMutex, portMAX_DELAY);

    orderCounter++;
    cells[x - 1][z - 1].state = CELL_STORED;
    cells[x - 1][z - 1].storedTime = storedTime;
    cells[x - 1][z - 1].order = orderCounter;

    xSemaphoreGive(inventoryMutex);

    moveY(2);                                   // Y comes back to the centre
    moveXZ(1, 1, Z_GOTO_DOWN);                  // back to the (1,1) position

    char timeText[32];
    formatTime(storedTime, timeText);

    printColor(COLOR_GREEN, "\tstored: ID %d | family %d | cell (%d,%d) | completed at %s\n", id, getFamily(id), x, z, timeText);
    setSystemState(STATE_IDLE);
}


/*
*   Dispenses the oldest component of a family (FIFO). The component is chosen NOW, when the request starts,
*   and it is marked as "being dispensed" so no other request can choose it.
*/
void handleDispense(int family) {

    bool found = false;
    int itemId = 0;
    int itemX = 0;
    int itemZ = 0;
    int itemOrder = 0;
    time_t itemTime = 0;

    // look for the component of this family that was stored first
    xSemaphoreTake(inventoryMutex, portMAX_DELAY);

    for (int cx = 0; cx < NUM_X; cx++) {
        for (int cz = 0; cz < NUM_Z; cz++) {
            if ((cells[cx][cz].state == CELL_STORED) && (getFamily(cells[cx][cz].id) == family)) {
                if ((found == false) || (cells[cx][cz].order < itemOrder)) {
                    found = true;
                    itemId = cells[cx][cz].id;
                    itemX = (cx + 1);
                    itemZ = (cz + 1);
                    itemOrder = cells[cx][cz].order;
                    itemTime = cells[cx][cz].storedTime;
                }
            }
        }
    }

    if (found == true) {
        cells[itemX - 1][itemZ - 1].state = CELL_DISPENSING;
    }

    xSemaphoreGive(inventoryMutex);

    if (found == false) {
        printColor(COLOR_YELLOW, "\n\tfamily %d is out of stock (the cage does not move)\n", family);
        return;
    }

    // the cage must be empty to receive the component, so wait for the user to take the box out
    if (isBoxInCage() == true) {
        printColor(COLOR_YELLOW, "\n\ttake the component out of the cage to continue (dispense family %d)...\n", family);
    }

    TickType_t startTick = xTaskGetTickCount();

    while ((isBoxInCage() == true) && ((xTaskGetTickCount() - startTick) < MS_TO_TICKS(CAGE_CLEAR_TIMEOUT_MS))) {
        vTaskDelay(MS_TO_TICKS(50));
    }

    if (isBoxInCage() == true) {

        // give the component back to its cell, it keeps its place in the FIFO order
        xSemaphoreTake(inventoryMutex, portMAX_DELAY);
        cells[itemX - 1][itemZ - 1].state = CELL_STORED;
        xSemaphoreGive(inventoryMutex);

        printColor(COLOR_YELLOW, "\tdispense of family %d cancelled: the cage was not emptied\n", family);
        return;
    }

    printColor(COLOR_WHITE, "\n\tdispensing family %d: the oldest is ID %d in cell (%d,%d)...\n", family, itemId, itemX, itemZ);
    setSystemState(STATE_DISPENSING);           // the lower light starts to flash

    moveXZ(itemX, itemZ, Z_GOTO_DOWN);          // X and Z move together
    moveY(3);                                   // Y goes under the pallet
    moveZ(Z_FIND_UP, 0);                        // Z goes up and lifts the pallet
    moveY(2);                                   // Y brings the pallet into the cage
    moveZ(Z_FIND_DOWN, 0);                      // Z goes down with the pallet
    moveXZ(1, 1, Z_GOTO_DOWN);                  // deliver at (1,1)

    // delivered: now it counts. Free the cell, write the history and count the service
    time_t deliveryTime = time(NULL);
    long duration = (long)difftime(deliveryTime, itemTime);

    xSemaphoreTake(inventoryMutex, portMAX_DELAY);

    cells[itemX - 1][itemZ - 1].state = CELL_EMPTY;

    if (historyCount == MAX_HISTORY) {                  // history is full: forget the oldest record
        memmove(&history[0], &history[1], (sizeof(HistoryRecord) * (MAX_HISTORY - 1)));
        historyCount--;
    }

    history[historyCount].id = itemId;
    history[historyCount].family = family;
    history[historyCount].time = deliveryTime;
    history[historyCount].duration = duration;
    historyCount++;

    countFamily[family]++;
    countTotal++;

    xSemaphoreGive(inventoryMutex);

    char timeText[32];
    char durationText[32];
    formatTime(deliveryTime, timeText);
    formatDuration(duration, durationText);

    printColor(COLOR_GREEN, "\tdelivered: ID %d | family %d | storage duration %s | at %s\n", itemId, family, durationText, timeText);
    setSystemState(STATE_IDLE);
}


/*
*   Warehouse task. First it calibrates the kit (cage at X = 1, Z = 1), then it takes the requests
*   from the request mailbox in order of arrival and does them one by one.
*/
void warehouseTask(void* pvParameters) {

    Request request;
    bool calibrated = false;
    int attempt = 1;

    // ---- calibration: Y to the centre, then X and Z go home at the same time ----
    while ((calibrated == false) && (attempt <= CALIBRATION_RETRIES)) {

        printColor(COLOR_WHITE, "\n\tcalibrating (attempt %d): Y to 2, then X to 1 and Z to 1...\n", attempt);

        if (getYPos() != 2) {
            moveY(2);
        }

        XCommand xCommand = { X_HOME, 1 };
        ZCommand zCommand = { Z_HOME, 1 };

        xQueueSend(mbx_X, &xCommand, portMAX_DELAY);
        xQueueSend(mbx_Z, &zCommand, portMAX_DELAY);

        xSemaphoreTake(sem_X_finished, portMAX_DELAY);
        xSemaphoreTake(sem_Z_finished, portMAX_DELAY);

        // check with the sensors that the cage really is where it should be
        if ((getXPos() == 1) && (getYPos() == 2) && (getZPos() == 1)) {
            calibrated = true;
            printColor(COLOR_GREEN, "\tcalibrated: cage at X = %d, Y = %d, Z = %d\n", getXPos(), getYPos(), getZPos());
        }
        else {
            printColor(COLOR_RED, "\tcalibration check failed: X = %d, Y = %d, Z = %d\n", getXPos(), getYPos(), getZPos());
        }

        attempt++;
    }

    if (calibrated == false) {
        printColor(COLOR_RED, "\tCALIBRATION FAILED, check the kit or the simulator\n");
    }

    // ---- after a RESET: if there is a component in the cage, the user has to take it out ----
    if (resetPending == true) {

        resetPending = false;

        if (isBoxInCage() == true) {

            printColor(COLOR_YELLOW, "\n\tRESET: take the component out of the cage to continue...\n");

            while (isBoxInCage() == true) {
                vTaskDelay(MS_TO_TICKS(100));
            }

            printColor(COLOR_WHITE, "\tcage is clear\n");
        }

        printColor(COLOR_GREEN, "\tRESET complete, the system is ready\n");
    }

    // ---- requests, in order of arrival ----
    while (true) {

        xQueueReceive(requestQueue, &request, portMAX_DELAY);

        // if a manual key left an axis moving, stop it before starting
        if (manualMoving == true) {
            stopAll();
            manualMoving = false;
        }

        warehouseBusy = true;

        if (request.type == REQ_STORE) {
            handleStore(request.id);
        }
        else {
            handleDispense(request.family);
        }

        warehouseBusy = false;
    }
}




  ////////////////////////////////////////////////////////////////////////////////////////
 //						SUPERVISOR TASK (switches, STOP, RESUME, RESET)				   //
////////////////////////////////////////////////////////////////////////////////////////


/*
*   Creates the three axis tasks and the warehouse task. Used at start-up and after a RESET.
*/
void createWorkerTasks() {

    xTaskCreate(xTask, "X", STACK_SMALL, NULL, PRIO_AXIS, &hX);
    xTaskCreate(yTask, "Y", STACK_SMALL, NULL, PRIO_AXIS, &hY);
    xTaskCreate(zTask, "Z", STACK_SMALL, NULL, PRIO_AXIS, &hZ);
    xTaskCreate(warehouseTask, "Warehouse", STACK_BIG, NULL, PRIO_WAREHOUSE, &hWarehouse);
}


/*
*   RESET: kills the warehouse and the axis tasks, undoes the interrupted request (the stored components
*   keep their order and times), and starts the tasks again. The new warehouse task calibrates the kit,
*   waits for the cage to be emptied and goes back to the requests that were still waiting.
*   The interrupted request is discarded.
*/
void resetSystem() {

    printColor(COLOR_YELLOW, "\n\tRESET...\n");

    bool boxInCage = isBoxInCage();

    // take the mutexes first so no task is killed while it is using one of them
    xSemaphoreTake(inventoryMutex, portMAX_DELAY);
    xSemaphoreTake(printMutex, portMAX_DELAY);

    vTaskDelete(hWarehouse);
    vTaskDelete(hX);
    vTaskDelete(hY);
    vTaskDelete(hZ);

    // undo what was interrupted
    for (int cx = 0; cx < NUM_X; cx++) {
        for (int cz = 0; cz < NUM_Z; cz++) {

            if (cells[cx][cz].state == CELL_RESERVED) {
                cells[cx][cz].state = CELL_EMPTY;               // the store did not finish
            }
            else if (cells[cx][cz].state == CELL_DISPENSING) {
                if (boxInCage == true) {
                    cells[cx][cz].state = CELL_EMPTY;           // the component is in the cage and will be taken out
                }
                else {
                    cells[cx][cz].state = CELL_STORED;          // it is still in its cell
                }
            }
        }
    }

    xSemaphoreGive(printMutex);
    xSemaphoreGive(inventoryMutex);

    stopAll();

    // clean the mailboxes and the semaphores of the dead tasks
    xQueueReset(mbx_X);
    xQueueReset(mbx_Y);
    xQueueReset(mbx_Z);

    while (xSemaphoreTake(sem_X_finished, 0) == pdTRUE) {
    }
    while (xSemaphoreTake(sem_Y_finished, 0) == pdTRUE) {
    }
    while (xSemaphoreTake(sem_Z_finished, 0) == pdTRUE) {
    }

    taskENTER_CRITICAL();
    systemState = STATE_IDLE;
    stateBeforeStop = STATE_IDLE;
    taskEXIT_CRITICAL();

    warehouseBusy = false;
    manualMoving = false;
    resetPending = true;

    createWorkerTasks();
}


/*
*   Supervisor task. Reads the two switches every few milliseconds:
*   - while the system is working: pulses on switch 1 = store that ID, pulses on switch 2 = dispense that family
*   - both switches together while moving = emergency STOP
*   - during the STOP: switch 1 = RESUME, switch 2 = RESET
*/
void supervisorTask(void* pvParameters) {

    bool rawSwitch1 = false;                // what the hardware says
    bool rawSwitch2 = false;
    bool switch1 = false;                   // after the debounce
    bool switch2 = false;
    bool lastSwitch1 = false;
    bool lastSwitch2 = false;
    int debounceCount1 = 0;
    int debounceCount2 = 0;
    bool bothPressed = false;               // true from the moment both are pressed until both are released
    int pulses1 = 0;
    int pulses2 = 0;
    TickType_t lastPulseTime1 = 0;
    TickType_t lastPulseTime2 = 0;
    TickType_t lastWakeTime = xTaskGetTickCount();

    while (true) {

        vTaskDelayUntil(&lastWakeTime, MS_TO_TICKS(SUPERVISOR_PERIOD_MS));

        readSwitches(&rawSwitch1, &rawSwitch2);

        // debounce: a switch only changes after DEBOUNCE_SAMPLES equal readings in a row
        if (rawSwitch1 != switch1) {
            debounceCount1++;
            if (debounceCount1 >= DEBOUNCE_SAMPLES) {
                switch1 = rawSwitch1;
                debounceCount1 = 0;
            }
        }
        else {
            debounceCount1 = 0;
        }

        if (rawSwitch2 != switch2) {
            debounceCount2++;
            if (debounceCount2 >= DEBOUNCE_SAMPLES) {
                switch2 = rawSwitch2;
                debounceCount2 = 0;
            }
        }
        else {
            debounceCount2 = 0;
        }

        TickType_t now = xTaskGetTickCount();
        int state = systemState;

        // ---- both switches while moving = EMERGENCY STOP ----
        if ((switch1 == true) && (switch2 == true) && (bothPressed == false)) {

            bothPressed = true;
            pulses1 = 0;                    // forget numbers that were half typed
            pulses2 = 0;

            if ((state == STATE_STORING) || (state == STATE_DISPENSING)) {

                // freeze the axis tasks first, and only then stop the motors
                vTaskSuspend(hX);
                vTaskSuspend(hY);
                vTaskSuspend(hZ);

                // save which motors were on and where the cage is, to resume later
                taskENTER_CRITICAL();
                savedMotionBits = (uInt8)(readDigitalU8(2) & MOTION_MASK);
                taskEXIT_CRITICAL();

                savedX = getXPos();
                savedZ = getZPos();

                stopAll();

                taskENTER_CRITICAL();
                stateBeforeStop = systemState;
                systemState = STATE_STOPPED;        // the lights now flash fast
                taskEXIT_CRITICAL();

                printColor(COLOR_YELLOW, "\n\t*** EMERGENCY STOP *** cage at X = %d, Z = %d (-1 = between two sensors)\n", savedX, savedZ);
                printColor(COLOR_YELLOW, "\tswitch 1 = RESUME | switch 2 = RESET\n");
            }
        }

        bool released1 = ((lastSwitch1 == true) && (switch1 == false));
        bool released2 = ((lastSwitch2 == true) && (switch2 == false));

        if (bothPressed == false) {

            if (state == STATE_STOPPED) {

                // ---- RESUME ----
                if (released1 == true) {

                    if ((getXPos() != savedX) || (getZPos() != savedZ)) {
                        printColor(COLOR_YELLOW, "\n\twarning: the cage moved while it was stopped (X = %d, Z = %d now)\n", getXPos(), getZPos());
                    }

                    // turn on again the motors that were on (needed when the cage stopped between two sensors)
                    taskENTER_CRITICAL();
                    uInt8 p2 = readDigitalU8(2);
                    p2 = (uInt8)((p2 & ~MOTION_MASK) | savedMotionBits);
                    writeDigitalU8(2, p2);
                    systemState = stateBeforeStop;
                    taskEXIT_CRITICAL();

                    vTaskResume(hX);
                    vTaskResume(hY);
                    vTaskResume(hZ);

                    printColor(COLOR_GREEN, "\n\tRESUME: the interrupted operation continues\n");
                }
                // ---- RESET ----
                else if (released2 == true) {
                    resetSystem();
                }
            }
            else {

                // normal operation: every release of a switch is one pulse
                if (released1 == true) {
                    pulses1++;
                    lastPulseTime1 = now;
                }
                if (released2 == true) {
                    pulses2++;
                    lastPulseTime2 = now;
                }
            }
        }

        // ---- no new pulse for a while: the number is complete ----
        if ((pulses1 > 0) && ((now - lastPulseTime1) >= MS_TO_TICKS(PULSE_TIMEOUT_MS))) {

            if (pulses1 <= 9) {
                printColor(COLOR_WHITE, "\n\tswitches: STORE ID %d (family %d)\n", pulses1, getFamily(pulses1));

                if (submitRequest(REQ_STORE, pulses1, getFamily(pulses1)) == false) {
                    printColor(COLOR_RED, "\trequest mailbox is full\n");
                }
            }
            else {
                printColor(COLOR_RED, "\n\tswitches: ID %d is out of range (1 to 9), request rejected\n", pulses1);
            }

            pulses1 = 0;
        }

        if ((pulses2 > 0) && ((now - lastPulseTime2) >= MS_TO_TICKS(PULSE_TIMEOUT_MS))) {

            if (pulses2 <= 3) {
                printColor(COLOR_WHITE, "\n\tswitches: DISPENSE family %d\n", pulses2);

                if (submitRequest(REQ_DISPENSE, 0, pulses2) == false) {
                    printColor(COLOR_RED, "\trequest mailbox is full\n");
                }
            }
            else {
                printColor(COLOR_RED, "\n\tswitches: family %d is out of range (1 to 3), request rejected\n", pulses2);
            }

            pulses2 = 0;
        }

        if ((switch1 == false) && (switch2 == false)) {
            bothPressed = false;
        }

        lastSwitch1 = switch1;
        lastSwitch2 = switch2;
    }
}




  ////////////////////////////////////////////////////////////////////////////////////////
 //									LIGHTS TASK										   //
////////////////////////////////////////////////////////////////////////////////////////


/*
*   Lights task: upper light flashes while storing, lower light flashes while dispensing,
*   both flash faster during the emergency STOP.
*/
void lightsTask(void* pvParameters) {

    TickType_t lastWakeTime = xTaskGetTickCount();
    int ticks = 0;
    bool phase = false;                 // true = lights on in this half period

    while (true) {

        vTaskDelayUntil(&lastWakeTime, MS_TO_TICKS(LIGHTS_PERIOD_MS));

        int state = systemState;
        int halfPeriod = BLINK_SLOW_TICKS;

        if (state == STATE_STOPPED) {
            halfPeriod = BLINK_FAST_TICKS;
        }

        ticks++;
        if (ticks >= halfPeriod) {
            ticks = 0;
            phase = !phase;
        }

        bool upper = false;
        bool lower = false;

        if (state == STATE_STORING) {
            upper = phase;
        }
        else if (state == STATE_DISPENSING) {
            lower = phase;
        }
        else if (state == STATE_STOPPED) {
            upper = phase;
            lower = phase;
        }

        setLights(upper, lower);
    }
}




  ////////////////////////////////////////////////////////////////////////////////////////
 //								INFORMATION FUNCTIONS								   //
////////////////////////////////////////////////////////////////////////////////////////


/*
*   FR11: lists the stored components with family, cell, storage date/time and elapsed time.
*/
void printInventory() {

    Cell snapshot[NUM_X][NUM_Z];
    int shown = 0;

    // copy the cells inside the mutex and print outside of it
    xSemaphoreTake(inventoryMutex, portMAX_DELAY);
    memcpy(snapshot, cells, sizeof(cells));
    xSemaphoreGive(inventoryMutex);

    time_t now = time(NULL);

    printColor(COLOR_WHITE, "\n\tID  FAM  CELL   STORED AT            ELAPSED\n");

    for (int cx = 0; cx < NUM_X; cx++) {
        for (int cz = 0; cz < NUM_Z; cz++) {

            if ((snapshot[cx][cz].state == CELL_STORED) || (snapshot[cx][cz].state == CELL_DISPENSING)) {

                char timeText[32];
                char durationText[32];
                formatTime(snapshot[cx][cz].storedTime, timeText);
                formatDuration((long)difftime(now, snapshot[cx][cz].storedTime), durationText);

                printColor(COLOR_WHITE, "\t%-3d %-4d (%d,%d)  %s  %s", snapshot[cx][cz].id, getFamily(snapshot[cx][cz].id), (cx + 1), (cz + 1), timeText, durationText);

                if (snapshot[cx][cz].state == CELL_DISPENSING) {
                    printColor(COLOR_YELLOW, "  [being dispensed]");
                }

                printColor(COLOR_WHITE, "\n");
                shown++;
            }
        }
    }

    if (shown == 0) {
        printColor(COLOR_WHITE, "\t(the warehouse is empty)\n");
    }
}


/*
*   FR12: for a family, shows how many components are available and which one leaves next.
*/
void printFamilyInfo(int family) {

    Cell snapshot[NUM_X][NUM_Z];
    int available = 0;
    bool found = false;
    int nextId = 0;
    int nextX = 0;
    int nextZ = 0;
    int nextOrder = 0;
    time_t nextTime = 0;

    xSemaphoreTake(inventoryMutex, portMAX_DELAY);
    memcpy(snapshot, cells, sizeof(cells));
    xSemaphoreGive(inventoryMutex);

    for (int cx = 0; cx < NUM_X; cx++) {
        for (int cz = 0; cz < NUM_Z; cz++) {

            if ((snapshot[cx][cz].state == CELL_STORED) && (getFamily(snapshot[cx][cz].id) == family)) {

                available++;

                if ((found == false) || (snapshot[cx][cz].order < nextOrder)) {
                    found = true;
                    nextId = snapshot[cx][cz].id;
                    nextX = (cx + 1);
                    nextZ = (cz + 1);
                    nextOrder = snapshot[cx][cz].order;
                    nextTime = snapshot[cx][cz].storedTime;
                }
            }
        }
    }

    if (found == false) {
        printColor(COLOR_YELLOW, "\n\tfamily %d: OUT OF STOCK\n", family);
        return;
    }

    char timeText[32];
    formatTime(nextTime, timeText);

    printColor(COLOR_WHITE, "\n\tfamily %d: %d available | next: ID %d, cell (%d,%d), stored at %s\n", family, available, nextId, nextX, nextZ, timeText);
}


/*
*   FR13: shows what is in a cell (X, Z).
*/
void printCellInfo(int x, int z) {

    if ((x < 1) || (x > NUM_X) || (z < 1) || (z > NUM_Z)) {
        printColor(COLOR_RED, "\n\tinvalid cell (X and Z go from 1 to 3)\n");
        return;
    }

    Cell cell;

    xSemaphoreTake(inventoryMutex, portMAX_DELAY);
    cell = cells[x - 1][z - 1];
    xSemaphoreGive(inventoryMutex);

    if ((cell.state == CELL_EMPTY) || (cell.state == CELL_RESERVED)) {
        printColor(COLOR_WHITE, "\n\tcell (%d,%d): EMPTY\n", x, z);
        return;
    }

    char timeText[32];
    char durationText[32];
    formatTime(cell.storedTime, timeText);
    formatDuration((long)difftime(time(NULL), cell.storedTime), durationText);

    printColor(COLOR_WHITE, "\n\tcell (%d,%d): ID %d | family %d | stored at %s | elapsed %s\n", x, z, cell.id, getFamily(cell.id), timeText, durationText);
}


/*
*   FR14: shows the history of all completed dispensing services.
*/
void printHistory() {

    static HistoryRecord snapshot[MAX_HISTORY];         // static so it is not on the task stack
    int count;

    xSemaphoreTake(inventoryMutex, portMAX_DELAY);
    count = historyCount;
    memcpy(snapshot, history, (sizeof(HistoryRecord) * count));
    xSemaphoreGive(inventoryMutex);

    printColor(COLOR_WHITE, "\n\tID  FAM  DISPENSED AT         STORAGE DURATION\n");

    for (int i = 0; i < count; i++) {

        char timeText[32];
        char durationText[32];
        formatTime(snapshot[i].time, timeText);
        formatDuration(snapshot[i].duration, durationText);

        printColor(COLOR_WHITE, "\t%-3d %-4d %s  %s\n", snapshot[i].id, snapshot[i].family, timeText, durationText);
    }

    if (count == 0) {
        printColor(COLOR_WHITE, "\t(no completed services yet)\n");
    }
}


/*
*   FR15: shows how many services were completed for each family and the total.
*/
void printCounts() {

    int family1;
    int family2;
    int family3;
    int total;

    xSemaphoreTake(inventoryMutex, portMAX_DELAY);
    family1 = countFamily[1];
    family2 = countFamily[2];
    family3 = countFamily[3];
    total = countTotal;
    xSemaphoreGive(inventoryMutex);

    printColor(COLOR_WHITE, "\n\tcompleted services: family 1 = %d | family 2 = %d | family 3 = %d | total = %d\n", family1, family2, family3, total);
}




  ////////////////////////////////////////////////////////////////////////////////////////
 //									MENU TASK										   //
////////////////////////////////////////////////////////////////////////////////////////


/*
*   Prints the menu: a table with the commands, and below it the movement keys, the switches and the warnings.
*/
void printMenu() {

    const char* keys[] = { "t", "r", "i", "f", "c", "h", "n", "p", "m", "ESC" };

    const char* commands[] = { "store component", "dispense component", "stored components", "family info", "cell info",
                               "history", "dispense counts", "positions", "menu", "exit" };

    const char* descriptions[] = { "asks for the component ID (1 to 9)",
                                   "asks for the family (1 to 3)",
                                   "list with cell, date and elapsed time",
                                   "stock and next component to leave",
                                   "asks for the cell (X Z)",
                                   "completed dispensing services",
                                   "services per family and total",
                                   "X, Y, Z and the box sensor",
                                   "shows this menu again",
                                   "stops the axes and closes the program" };

    // widths of the columns: 8, 24 and 40 characters
    printColor(COLOR_WHITE, "\n\t" BOX_TL BOX_H8 BOX_TM BOX_H16 BOX_H8 BOX_TM BOX_H32 BOX_H8 BOX_TR "\n");
    printColor(COLOR_WHITE, "\t" BOX_V " %-6s " BOX_V " %-22s " BOX_V " %-38s " BOX_V "\n", "Key", "Command", "Description");
    printColor(COLOR_WHITE, "\t" BOX_LM BOX_H8 BOX_CR BOX_H16 BOX_H8 BOX_CR BOX_H32 BOX_H8 BOX_RM "\n");

    for (int i = 0; i < 10; i++) {
        printColor(COLOR_WHITE, "\t" BOX_V " %-6s " BOX_V " %-22s " BOX_V " %-38s " BOX_V "\n", keys[i], commands[i], descriptions[i]);
    }

    printColor(COLOR_WHITE, "\t" BOX_BL BOX_H8 BOX_BM BOX_H16 BOX_H8 BOX_BM BOX_H32 BOX_H8 BOX_BR "\n");

    printColor(COLOR_WHITE, "\n\tMANUAL MOVEMENTS (only when the system is idle)\n");
    printColor(COLOR_WHITE, "\t  a / d  ->  X left / right        w / s  ->  Z up / down\n");
    printColor(COLOR_WHITE, "\t  y / u  ->  Y inside / outside     b      ->  stop all axes\n");

    printColor(COLOR_WHITE, "\n\tSWITCHES\n");
    printColor(COLOR_WHITE, "\t  switch 1 pulses  ->  store the component with that ID (1 to 9)\n");
    printColor(COLOR_WHITE, "\t  switch 2 pulses  ->  dispense that family (1 to 3)\n");
    printColor(COLOR_WHITE, "\t  both switches    ->  emergency STOP (then switch 1 = RESUME, switch 2 = RESET)\n");

    printColor(COLOR_YELLOW, "\n\tWARNINGS\n");
    printColor(COLOR_YELLOW, "\t  X and Z only move when Y is at the centre (position 2)\n");
    printColor(COLOR_YELLOW, "\t  a store needs a component in the cage, a dispense needs the cage empty\n");
    printColor(COLOR_YELLOW, "\t  after a dispense, take the component out of the cage before the next request\n\n");
}


/*
*   Menu task. It never blocks: it only reads the keyboard when a key is waiting (_kbhit),
*   and it sleeps a little in every loop so the other tasks can run.
*   Keys that need more typing (ID, family, cell) are read a few characters at a time.
*/
void menuTask(void* pvParameters) {

    int menuState = MENU_MAIN;
    char line[32];
    int lineLength = 0;
    int key = 0;

    printMenu();

    while (true) {

        // ---------- waiting for a command key ----------
        if (menuState == MENU_MAIN) {

            if (_kbhit()) {

                key = _getch();

                // the manual movements only work when nothing else is moving the cage
                bool idle = ((systemState == STATE_IDLE) && (warehouseBusy == false) && (uxQueueMessagesWaiting(requestQueue) == 0));

                if (key == 't') {
                    printColor(COLOR_WHITE, "\tstore - component ID (1 to 9): ");
                    menuState = MENU_ASK_STORE_ID;
                }
                else if (key == 'r') {
                    printColor(COLOR_WHITE, "\tdispense - family (1 to 3): ");
                    menuState = MENU_ASK_DISPENSE;
                }
                else if (key == 'f') {
                    printColor(COLOR_WHITE, "\tfamily info - family (1 to 3): ");
                    menuState = MENU_ASK_FAMILY_INFO;
                }
                else if (key == 'c') {
                    printColor(COLOR_WHITE, "\tcell info - cell (X Z): ");
                    menuState = MENU_ASK_CELL_INFO;
                }
                else if (key == 'i') {
                    printInventory();
                }
                else if (key == 'h') {
                    printHistory();
                }
                else if (key == 'n') {
                    printCounts();
                }
                else if (key == 'm') {
                    printMenu();
                }
                else if (key == 'p') {
                    printColor(COLOR_WHITE, "\tpositions: X = %d | Y = %d | Z = %d | box in cage: %s\n", getXPos(), getYPos(), getZPos(), (isBoxInCage() ? "YES" : "no"));
                }
                else if (key == 'b') {
                    stopAll();
                    manualMoving = false;
                    printColor(COLOR_WHITE, "\tstopped all axes\n");
                }
                else if ((key == 'a') || (key == 'd') || (key == 'w') || (key == 's')) {

                    if (idle == false) {
                        printColor(COLOR_YELLOW, "\tmanual movement only works when the system is idle\n");
                    }
                    else if (getYPos() != 2) {
                        printColor(COLOR_YELLOW, "\tY must be at the centre (2) to move X or Z\n");
                    }
                    else {
                        manualMoving = true;

                        if (key == 'a') {
                            moveXLeft();
                            printColor(COLOR_WHITE, "\tmoving X left...\n");
                        }
                        else if (key == 'd') {
                            moveXRight();
                            printColor(COLOR_WHITE, "\tmoving X right...\n");
                        }
                        else if (key == 'w') {
                            moveZUp();
                            printColor(COLOR_WHITE, "\tmoving Z up...\n");
                        }
                        else {
                            moveZDown();
                            printColor(COLOR_WHITE, "\tmoving Z down...\n");
                        }
                    }
                }
                else if (key == 'y') {

                    if (idle == false) {
                        printColor(COLOR_YELLOW, "\tmanual movement only works when the system is idle\n");
                    }
                    else {
                        manualMoving = true;
                        moveYInside();
                        printColor(COLOR_WHITE, "\tmoving Y inside...\n");
                    }
                }
                else if (key == 'u') {

                    if (idle == false) {
                        printColor(COLOR_YELLOW, "\tmanual movement only works when the system is idle\n");
                    }
                    else {
                        manualMoving = true;
                        moveYOutside();
                        printColor(COLOR_WHITE, "\tmoving Y outside...\n");
                    }
                }
                else if (key == 27) {
                    stopAll();
                    closeChannels();
                    exit(0);
                }
            }
        }
        // ---------- typing a number (ID, family or cell) ----------
        else {

            bool lineFinished = false;

            // read the characters that are waiting, without blocking
            while ((_kbhit()) && (lineFinished == false)) {

                key = _getch();

                if (key == '\r') {                      // Enter
                    line[lineLength] = '\0';
                    lineLength = 0;
                    lineFinished = true;
                    printColor(COLOR_WHITE, "\n");
                }
                else if (key == '\b') {                 // Backspace
                    if (lineLength > 0) {
                        lineLength--;
                        printColor(COLOR_WHITE, "\b \b");
                    }
                }
                else if ((key >= 32) && (lineLength < 31)) {
                    line[lineLength] = (char)key;
                    lineLength++;
                    printColor(COLOR_WHITE, "%c", key);
                }
            }

            if (lineFinished == true) {

                int a = 0;
                int b = 0;
                int numbers = sscanf(line, "%d %d", &a, &b);

                if (menuState == MENU_ASK_STORE_ID) {

                    if ((numbers == 1) && (a >= 1) && (a <= 9)) {

                        if (isBoxInCage() == false) {
                            printColor(COLOR_YELLOW, "\t(there is no component in the cage right now)\n");
                        }

                        if (submitRequest(REQ_STORE, a, getFamily(a)) == true) {
                            printColor(COLOR_GREEN, "\tstore ID %d (family %d) queued\n", a, getFamily(a));
                        }
                        else {
                            printColor(COLOR_RED, "\trequest mailbox is full\n");
                        }
                    }
                    else {
                        printColor(COLOR_RED, "\tinvalid ID (1 to 9)\n");
                    }
                }
                else if (menuState == MENU_ASK_DISPENSE) {

                    if ((numbers == 1) && (a >= 1) && (a <= 3)) {

                        if (submitRequest(REQ_DISPENSE, 0, a) == true) {
                            printColor(COLOR_GREEN, "\tdispense of family %d queued\n", a);
                        }
                        else {
                            printColor(COLOR_RED, "\trequest mailbox is full\n");
                        }
                    }
                    else {
                        printColor(COLOR_RED, "\tinvalid family (1 to 3)\n");
                    }
                }
                else if (menuState == MENU_ASK_FAMILY_INFO) {

                    if ((numbers == 1) && (a >= 1) && (a <= 3)) {
                        printFamilyInfo(a);
                    }
                    else {
                        printColor(COLOR_RED, "\tinvalid family (1 to 3)\n");
                    }
                }
                else if (menuState == MENU_ASK_CELL_INFO) {

                    if (numbers == 2) {
                        printCellInfo(a, b);
                    }
                    else {
                        printColor(COLOR_RED, "\tinvalid cell, write it like this: 2 3\n");
                    }
                }

                menuState = MENU_MAIN;
            }
        }

        vTaskDelay(MS_TO_TICKS(20));            // never spin, let the other tasks run
    }
}




  ////////////////////////////////////////////////////////////////////////////////////////
 //								START-UP AND MAIN									   //
////////////////////////////////////////////////////////////////////////////////////////


/*
*   Opens the ports of the simulator / kit: ports 0 and 1 are inputs, port 2 is output.
*/
void inicializarPortos() {

    printColor(COLOR_WHITE, "\nwaiting for hardware simulator...");

    createDigitalInput(0);
    createDigitalInput(1);
    createDigitalOutput(2);
    writeDigitalU8(2, 0);

    printColor(COLOR_GREEN, "\ngot access to simulator...");
}


/*
*   Runs only once, when the scheduler starts: creates the mutexes, mailboxes, semaphores and tasks.
*/
void myDaemonTaskStartupHook(void) {

    // the print mutex goes first because every print uses it
    printMutex = xSemaphoreCreateMutex();

    inicializarPortos();

    memset(cells, 0, sizeof(cells));

    // Mailboxes
    requestQueue = xQueueCreate(10, sizeof(Request));        // more than 1 so requests can pile up
    mbx_X = xQueueCreate(1, sizeof(XCommand));
    mbx_Y = xQueueCreate(1, sizeof(int));
    mbx_Z = xQueueCreate(1, sizeof(ZCommand));

    // Semaphores and mutexes
    sem_X_finished = xSemaphoreCreateBinary();
    sem_Y_finished = xSemaphoreCreateBinary();
    sem_Z_finished = xSemaphoreCreateBinary();
    inventoryMutex = xSemaphoreCreateMutex();

    configASSERT(printMutex && requestQueue && mbx_X && mbx_Y && mbx_Z);
    configASSERT(sem_X_finished && sem_Y_finished && sem_Z_finished && inventoryMutex);

    // Tasks
    createWorkerTasks();                // X, Y, Z and warehouse (the warehouse calibrates first)
    xTaskCreate(supervisorTask, "Supervisor", STACK_BIG, NULL, PRIO_SUPERVISOR, &hSupervisor);
    xTaskCreate(lightsTask, "Lights", STACK_SMALL, NULL, PRIO_LIGHTS, &hLights);
    xTaskCreate(menuTask, "Menu", STACK_BIG, NULL, PRIO_MENU, &hMenu);
}


int main()
{
    // code page 437 so the table characters of the menu show correctly
    SetConsoleOutputCP(437);

    initialiseHeap();
    vApplicationDaemonTaskStartupHook = &myDaemonTaskStartupHook;
    vTaskStartScheduler();
}
