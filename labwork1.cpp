// labwork1.cpp - Automated Component Store with FIFO dispensing (FreeRTOS, Windows port)  [v2]
//
// TASK / OBJECT MAP
//   supervisorTask  (highest) switches: pulse entry, STOP / RESUME / RESET            (FR1.7, FR5-7)
//   warehouseTask             executes store / dispense requests one at a time        (FR3, FR4, NF1)
//   xTask, yTask, zTask       ONLY tasks that move the cage. X and Z run in PARALLEL.
//   lightsTask                flashes the two lights according to systemState         (FR1.8, FR8-10)
//   menuTask        (lowest)  keyboard, validation, submitRequest(), info queries     (FR1.6, FR11-15)
//
//   requestQueue        : menu + supervisor -> warehouse           (length 10, struct Request)
//   mbx_X / mbx_Y / mbx_Z : warehouse -> axis task                 (length 1, command mailbox)
//   sem_X/Y/Z_finished  : axis task -> warehouse                   (binary, "done" signal)
//   invMutex            : protects inventory + history             (warehouse writes, menu reads)
//
// WHY X AND Z REALLY RUN AT THE SAME TIME
//   1. warehouseTask has a HIGHER priority than the axis tasks, so it posts BOTH commands
//      (mbx_X and mbx_Z) before either axis task starts.
//   2. The goto loops in my_interaction_functions.cpp sleep (vTaskDelay) between sensor reads
//      instead of busy-waiting with taskYIELD(), so the two axis tasks time-slice with each
//      other and with the lights / menu tasks.

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <iostream>
#include <conio.h>
#include <stdlib.h>
#include <stdio.h>
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
//                                  CONFIGURATION                                     //
////////////////////////////////////////////////////////////////////////////////////////

#define NUM_X           3
#define NUM_Z           3
#define MAX_HISTORY     100

#define SUPERVISOR_PERIOD_MS   10
#define DEBOUNCE_SAMPLES       2
#define PULSE_TIMEOUT_MS       1500    // idle time that closes a pulse sequence
#define LIGHTS_PERIOD_MS       50
#define BLINK_SLOW_TICKS       10      // 10 * 50 ms = 500 ms half period
#define BLINK_FAST_TICKS       2       //  2 * 50 ms = 100 ms half period

#define STORE_BOX_WAIT_MS      0       // 0 = reject a store at once if there is no box in the cage
#define DISPENSE_CLEAR_WAIT_MS 30000   // a dispense waits this long for the cage to be emptied
#define CALIBRATION_RETRIES    3

// Priorities: warehouse > axis, so both X and Z commands are posted before any axis starts.
#define PRIO_MENU        (tskIDLE_PRIORITY + 1)
#define PRIO_LIGHTS      (tskIDLE_PRIORITY + 2)
#define PRIO_AXIS        (tskIDLE_PRIORITY + 3)
#define PRIO_WAREHOUSE   (tskIDLE_PRIORITY + 4)
#define PRIO_SUPERVISOR  (tskIDLE_PRIORITY + 5)
static_assert(PRIO_SUPERVISOR < configMAX_PRIORITIES, "increase configMAX_PRIORITIES or lower the priorities");

#define STACK_SMALL      (configMINIMAL_STACK_SIZE * 4)
#define STACK_BIG        (configMINIMAL_STACK_SIZE * 8)

// milliseconds -> ticks, never 0
static inline TickType_t ms2t(uint32_t ms) {
    TickType_t t = pdMS_TO_TICKS(ms);
    return t ? t : 1;
}


////////////////////////////////////////////////////////////////////////////////////////
//                                  TYPES AND GLOBALS                                 //
////////////////////////////////////////////////////////////////////////////////////////

enum ReqType { REQ_STORE, REQ_DISPENSE };
struct Request { ReqType type; int id; int family; };

enum XMode { X_GOTO, X_HOME };
struct XCmd { XMode mode; int x; };

enum ZMode { Z_GOTO_DOWN, Z_GOTO_UP, Z_FIND_UP, Z_FIND_DOWN, Z_HOME };
struct ZCmd { ZMode mode; int z; };

enum SysState { ST_IDLE, ST_STORING, ST_DISPENSING, ST_STOPPED };

// ---- mailboxes (queues) ----
static QueueHandle_t requestQueue;
static QueueHandle_t mbx_X;
static QueueHandle_t mbx_Y;
static QueueHandle_t mbx_Z;

// ---- semaphores / mutexes ----
static SemaphoreHandle_t sem_X_finished;
static SemaphoreHandle_t sem_Y_finished;
static SemaphoreHandle_t sem_Z_finished;
static SemaphoreHandle_t invMutex;

// ---- task handles (needed for suspend / resume / delete) ----
static TaskHandle_t hMenu, hSupervisor, hLights, hWarehouse, hX, hY, hZ;

// ---- shared state ----
static volatile SysState systemState = ST_IDLE;
static volatile SysState stateBeforeStop = ST_IDLE;
static volatile bool     warehouseBusy = false;
static volatile bool     manualMoving = false;   // a manual key started a movement
static volatile bool     resetPending = false;   // new warehouseTask was started by a RESET
static volatile int      lightsTest = 0;       // bit0 = upper light, bit1 = lower light (menu test)
static uInt8             savedMotionBits = 0;       // axis output bits saved at STOP

static int familyOfId(int id) { return ((id - 1) % 3) + 1; }   // 1,4,7 | 2,5,8 | 3,6,9


////////////////////////////////////////////////////////////////////////////////////////
//                          HEAP + ASSERT (unchanged from template)                   //
////////////////////////////////////////////////////////////////////////////////////////

void initialiseHeap(void)
{
    static uint8_t ucHeap[configTOTAL_HEAP_SIZE];
    volatile uint32_t ulAdditionalOffset = 19;
    const HeapRegion_t xHeapRegions[] =
    {
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
    (void)ulAdditionalOffset;
    vPortDefineHeapRegions(xHeapRegions);
}

void vAssertCalled(unsigned long ulLine, const char* const pcFileName)
{
    static BaseType_t xPrinted = pdFALSE;
    volatile uint32_t ulSetToNonZeroInDebuggerToContinue = 0;
    (void)ulLine;
    (void)pcFileName;

    printf("ASSERT! Line %ld, file %s, GetLastError() %ld\r\n", ulLine, pcFileName,
        GetLastError());
    taskENTER_CRITICAL();
    {
        __debugbreak();
        while (ulSetToNonZeroInDebuggerToContinue == 0)
        {
            __asm { NOP };
            __asm { NOP };
        }
    }
    taskEXIT_CRITICAL();
}


////////////////////////////////////////////////////////////////////////////////////////
//                                    SMALL HELPERS                                   //
////////////////////////////////////////////////////////////////////////////////////////

static void fmtTime(time_t t, char* out, size_t n) {
    struct tm tmv;
    localtime_s(&tmv, &t);
    strftime(out, n, "%d/%m/%Y %H:%M:%S", &tmv);
}

static void fmtDuration(long s, char* out, size_t n) {
    snprintf(out, n, "%ldm %02lds", s / 60, s % 60);
}

// State changes go through here. If a STOP is active only the "state to restore" changes,
// so a late warehouse update can never overwrite ST_STOPPED.
static void setSystemState(SysState s) {
    taskENTER_CRITICAL();
    if (systemState == ST_STOPPED) stateBeforeStop = s;
    else                           systemState = s;
    taskEXIT_CRITICAL();
}

// Polls the box sensor until it shows the wanted state or the timeout expires.
static bool waitCageState(bool wantBox, TickType_t timeout) {
    TickType_t start = xTaskGetTickCount();
    while (isBoxInCage() != wantBox) {
        if ((xTaskGetTickCount() - start) >= timeout) return false;
        vTaskDelay(ms2t(50));
    }
    return true;
}


////////////////////////////////////////////////////////////////////////////////////////
//                                INVENTORY (invMutex)                                //
////////////////////////////////////////////////////////////////////////////////////////

enum CellState { CELL_EMPTY, CELL_RESERVED, CELL_STORED, CELL_DISPENSING };
struct Cell { CellState st; int id; time_t stored; uint32_t seq; };
struct HistRec { int id; int family; time_t when; long duration; };
struct StoredItem { int id; int family; int x; int z; time_t stored; };

static Cell     cells[NUM_X][NUM_Z];       // [x-1][z-1]
static uint32_t seqCounter = 0;            // storage-COMPLETION order -> FIFO key
static HistRec  history[MAX_HISTORY];
static int      histCount = 0;
static int      countFamily[4] = { 0, 0, 0, 0 };
static int      countTotal = 0;

enum StoreResult { STORE_OK, STORE_DUPLICATE, STORE_FULL };

static void inv_init() {
    memset(cells, 0, sizeof(cells));
}

// Reserve an empty cell for 'id'. Rejects an ID that is already stored / in transit (NF3).
static StoreResult inv_reserveStore(int id, int* x, int* z) {
    StoreResult res = STORE_FULL;
    xSemaphoreTake(invMutex, portMAX_DELAY);
    for (int cx = 0; cx < NUM_X; cx++)
        for (int cz = 0; cz < NUM_Z; cz++)
            if (cells[cx][cz].st != CELL_EMPTY && cells[cx][cz].id == id) res = STORE_DUPLICATE;

    if (res != STORE_DUPLICATE) {
        for (int cz = 0; cz < NUM_Z && res != STORE_OK; cz++)
            for (int cx = 0; cx < NUM_X && res != STORE_OK; cx++)
                if (cells[cx][cz].st == CELL_EMPTY) {
                    cells[cx][cz].st = CELL_RESERVED;
                    cells[cx][cz].id = id;
                    *x = cx + 1; *z = cz + 1;
                    res = STORE_OK;
                }
    }
    xSemaphoreGive(invMutex);
    return res;
}

// Placement finished: record time and FIFO sequence number.
static time_t inv_commitStore(int x, int z) {
    time_t now = time(NULL);
    xSemaphoreTake(invMutex, portMAX_DELAY);
    Cell& c = cells[x - 1][z - 1];
    c.st = CELL_STORED;
    c.stored = now;
    c.seq = ++seqCounter;
    xSemaphoreGive(invMutex);
    return now;
}

// FIFO choice, made when the request STARTS. Marks the item "being dispensed" atomically.
static bool inv_reserveOldest(int family, StoredItem* out) {
    bool found = false;
    xSemaphoreTake(invMutex, portMAX_DELAY);
    Cell* best = NULL; int bx = 0, bz = 0;
    for (int cx = 0; cx < NUM_X; cx++)
        for (int cz = 0; cz < NUM_Z; cz++) {
            Cell& c = cells[cx][cz];
            if (c.st == CELL_STORED && familyOfId(c.id) == family)
                if (!best || c.seq < best->seq) { best = &c; bx = cx + 1; bz = cz + 1; }
        }
    if (best) {
        best->st = CELL_DISPENSING;
        out->id = best->id; out->family = family;
        out->x = bx; out->z = bz; out->stored = best->stored;
        found = true;
    }
    xSemaphoreGive(invMutex);
    return found;
}

// Gives a reserved component back (dispense abandoned before any movement). FIFO order is kept.
static void inv_releaseReserved(const StoredItem& it) {
    xSemaphoreTake(invMutex, portMAX_DELAY);
    cells[it.x - 1][it.z - 1].st = CELL_STORED;
    xSemaphoreGive(invMutex);
}

// Delivery at (1,1) finished: free the cell, log history, count the service.
static void inv_commitDispense(const StoredItem& it, time_t* when, long* duration) {
    time_t now = time(NULL);
    xSemaphoreTake(invMutex, portMAX_DELAY);
    cells[it.x - 1][it.z - 1].st = CELL_EMPTY;
    *when = now;
    *duration = (long)difftime(now, it.stored);
    if (histCount == MAX_HISTORY) {                       // drop oldest record
        memmove(&history[0], &history[1], sizeof(HistRec) * (MAX_HISTORY - 1));
        histCount--;
    }
    history[histCount++] = { it.id, it.family, now, *duration };
    countFamily[it.family]++;
    countTotal++;
    xSemaphoreGive(invMutex);
}

// Called by the supervisor on RESET, with invMutex ALREADY held. 'boxInCage' comes from the
// box sensor. Stored components keep their order/time; only interrupted operations are undone.
static void inv_recoverAfterResetLocked(bool boxInCage) {
    for (int cx = 0; cx < NUM_X; cx++)
        for (int cz = 0; cz < NUM_Z; cz++) {
            Cell& c = cells[cx][cz];
            if (c.st == CELL_RESERVED) c.st = CELL_EMPTY;                       // store interrupted
            else if (c.st == CELL_DISPENSING)
                c.st = boxInCage ? CELL_EMPTY : CELL_STORED;                    // removed from cage / still in cell
        }
}


////////////////////////////////////////////////////////////////////////////////////////
//            MOTION WRAPPERS (warehouse side: send command, wait "done")             //
////////////////////////////////////////////////////////////////////////////////////////

static void moveY(int y) {
    xQueueSend(mbx_Y, &y, portMAX_DELAY);
    xSemaphoreTake(sem_Y_finished, portMAX_DELAY);
}

static void moveZ(ZMode mode, int z) {
    ZCmd c = { mode, z };
    xQueueSend(mbx_Z, &c, portMAX_DELAY);
    xSemaphoreTake(sem_Z_finished, portMAX_DELAY);
}

static void ensureYCentre() {                              // NF5: X/Z only move with Y at the centre
    if (getYPos() != 2) moveY(2);
}

// X and Z run IN PARALLEL: both commands are posted first, then we wait for both "done" signals.
static void moveXZ(int x, int z, ZMode zmode) {
    ensureYCentre();
    XCmd xc = { X_GOTO, x };
    ZCmd zc = { zmode, z };
    xQueueSend(mbx_X, &xc, portMAX_DELAY);
    xQueueSend(mbx_Z, &zc, portMAX_DELAY);
    xSemaphoreTake(sem_X_finished, portMAX_DELAY);
    xSemaphoreTake(sem_Z_finished, portMAX_DELAY);
}

// FR2: Y to the centre, then X -> 1 and Z -> 1 in parallel. Verifies the result with the sensors.
static bool calibrateKit() {
    for (int attempt = 1; attempt <= CALIBRATION_RETRIES; attempt++) {
        printf("\n\tcalibrating (attempt %d): Y -> 2, then X -> 1 and Z -> 1...\n", attempt);

        ensureYCentre();
        XCmd xc = { X_HOME, 1 };
        ZCmd zc = { Z_HOME, 1 };
        xQueueSend(mbx_X, &xc, portMAX_DELAY);
        xQueueSend(mbx_Z, &zc, portMAX_DELAY);
        xSemaphoreTake(sem_X_finished, portMAX_DELAY);
        xSemaphoreTake(sem_Z_finished, portMAX_DELAY);

        if (isHomed()) {
            printf("\tcalibrated: cage at X = %d, Y = %d, Z = %d\n", getXPos(), getYPos(), getZPos());
            return true;
        }
        printf("\tcalibration check failed: X = %d, Y = %d, Z = %d\n", getXPos(), getYPos(), getZPos());
    }
    printf("\tCALIBRATION FAILED - check the kit / simulator\n");
    return false;
}


////////////////////////////////////////////////////////////////////////////////////////
//                                      AXIS TASKS                                    //
////////////////////////////////////////////////////////////////////////////////////////

void xTask(void* pvParameters) {
    XCmd c;
    for (;;) {
        xQueueReceive(mbx_X, &c, portMAX_DELAY);
        if (getYPos() != 2)                    printf("\t[X] move refused: Y is not at the centre (NF5)\n");
        else if (c.mode == X_HOME)             homeX();
        else if (c.x >= 1 && c.x <= 3)         gotoX(c.x);                    // NF4
        else                                   printf("\t[X] move refused (target %d)\n", c.x);
        xSemaphoreGive(sem_X_finished);
    }
}

void yTask(void* pvParameters) {
    int y;
    for (;;) {
        xQueueReceive(mbx_Y, &y, portMAX_DELAY);
        if (y >= 1 && y <= 3) gotoY(y);                                       // NF4
        else printf("\t[Y] move refused (target %d)\n", y);
        xSemaphoreGive(sem_Y_finished);
    }
}

void zTask(void* pvParameters) {
    ZCmd c;
    for (;;) {
        xQueueReceive(mbx_Z, &c, portMAX_DELAY);
        switch (c.mode) {
        case Z_GOTO_DOWN:
        case Z_GOTO_UP:
        case Z_HOME:
            // NF5 applies to cage travel. The short FIND_UP / FIND_DOWN lift of a pallet is done
            // with Y extended, so it is exempt (confirm with your lab professor if unsure).
            if (getYPos() != 2)                 printf("\t[Z] move refused: Y is not at the centre (NF5)\n");
            else if (c.mode == Z_HOME)          homeZ();
            else if (c.z < 1 || c.z > 3)        printf("\t[Z] move refused (target %d)\n", c.z);
            else if (c.mode == Z_GOTO_DOWN)     gotoZ(c.z);
            else                                gotoZUp(c.z);
            break;
        case Z_FIND_UP:   movetoZUp();   break;
        case Z_FIND_DOWN: movetoZDown(); break;
        }
        xSemaphoreGive(sem_Z_finished);
    }
}


////////////////////////////////////////////////////////////////////////////////////////
//                                    WAREHOUSE TASK                                  //
////////////////////////////////////////////////////////////////////////////////////////

// Single entry point used by the menu AND the supervisor (switches).
static bool submitRequest(ReqType type, int id, int family) {
    Request r = { type, id, family };
    return xQueueSend(requestQueue, &r, 0) == pdTRUE;      // never blocks the caller
}

static void handleStore(int id) {
    // No box in the cage -> reject, no movement.
    if (!waitCageState(true, ms2t(STORE_BOX_WAIT_MS))) {
        printf("\n\tstore %d REJECTED: no component in the cage\n", id);
        return;
    }

    int x = 0, z = 0;
    StoreResult r = inv_reserveStore(id, &x, &z);          // decided NOW, at execution time
    if (r == STORE_DUPLICATE) { printf("\n\tstore %d REJECTED: ID already stored or in transit\n", id); return; }
    if (r == STORE_FULL) { printf("\n\tstore %d REJECTED: warehouse full\n", id);                  return; }

    printf("\n\tstoring ID %d (family %d) in cell (%d,%d)...\n", id, familyOfId(id), x, z);
    setSystemState(ST_STORING);                            // FR8: upper light flashes

    moveXZ(x, z, Z_GOTO_UP);                               // X and Z in parallel, Z to the cell's "up" position
    moveY(3);                                              // extend into the cell
    moveZ(Z_FIND_DOWN, 0);                                 // lower: the pallet now rests on the cell
    time_t t = inv_commitStore(x, z);                      // placement complete -> timestamp + FIFO seq
    moveY(2);                                              // retract
    moveXZ(1, 1, Z_GOTO_DOWN);                             // back to the cage position

    char ts[32]; fmtTime(t, ts, sizeof(ts));
    printf("\tstored: ID %d | family %d | cell (%d,%d) | completed at %s\n", id, familyOfId(id), x, z, ts);
    setSystemState(ST_IDLE);
}

static void handleDispense(int family) {
    StoredItem it;
    if (!inv_reserveOldest(family, &it)) {                 // FIFO choice made when the request STARTS
        printf("\n\tfamily %d OUT OF STOCK (no movement)\n", family);
        return;
    }

    // The cage must be empty to receive the component (e.g. remove the previous delivery).
    if (isBoxInCage()) printf("\n\tremove the component from the cage to continue (dispense family %d)...\n", family);
    if (!waitCageState(false, ms2t(DISPENSE_CLEAR_WAIT_MS))) {
        inv_releaseReserved(it);                           // nothing moved, nothing lost
        printf("\tdispense family %d REJECTED: cage was not emptied\n", family);
        return;
    }

    printf("\n\tdispensing family %d: oldest is ID %d at cell (%d,%d)...\n", family, it.id, it.x, it.z);
    setSystemState(ST_DISPENSING);                         // FR9: lower light flashes

    moveXZ(it.x, it.z, Z_GOTO_DOWN);                       // X and Z in parallel
    moveY(3);                                              // extend under the pallet
    moveZ(Z_FIND_UP, 0);                                   // lift it
    moveY(2);                                              // pull it into the cage
    moveZ(Z_FIND_DOWN, 0);
    moveXZ(1, 1, Z_GOTO_DOWN);                             // deliver at (1,1)

    time_t when; long dur;
    inv_commitDispense(it, &when, &dur);                   // count the service only NOW

    char ts[32], ds[32];
    fmtTime(when, ts, sizeof(ts)); fmtDuration(dur, ds, sizeof(ds));
    printf("\tdelivered: ID %d | family %d | storage duration %s | at %s\n", it.id, it.family, ds, ts);
    setSystemState(ST_IDLE);
}

void warehouseTask(void* pvParameters) {
    calibrateKit();                                        // FR2: cage ends at X = 1, Z = 1

    if (resetPending) {                                    // FR7: component in the cage -> take it out
        resetPending = false;
        if (isBoxInCage()) {
            printf("\n\tRESET: remove the component from the cage to continue...\n");
            waitCageState(false, portMAX_DELAY);
            printf("\tcage clear\n");
        }
        printf("\tRESET complete - ready\n");
    }

    Request r;
    for (;;) {
        xQueueReceive(requestQueue, &r, portMAX_DELAY);    // arrival order (NF1)

        if (manualMoving) { stopAll(); manualMoving = false; }
        warehouseBusy = true;
        if (r.type == REQ_STORE)    handleStore(r.id);
        else                        handleDispense(r.family);
        warehouseBusy = false;
    }
}


////////////////////////////////////////////////////////////////////////////////////////
//                       SUPERVISOR TASK (switches, STOP / RESUME / RESET)            //
////////////////////////////////////////////////////////////////////////////////////////

static void createWorkerTasks() {
    xTaskCreate(xTask, "X", STACK_SMALL, NULL, PRIO_AXIS, &hX);
    xTaskCreate(yTask, "Y", STACK_SMALL, NULL, PRIO_AXIS, &hY);
    xTaskCreate(zTask, "Z", STACK_SMALL, NULL, PRIO_AXIS, &hZ);
    xTaskCreate(warehouseTask, "Warehouse", STACK_BIG, NULL, PRIO_WAREHOUSE, &hWarehouse);
}

// FR5: freeze the axis tasks first, THEN stop the motors (otherwise an axis task could re-assert a direction).
static void emergencyStop() {
    vTaskSuspend(hX); vTaskSuspend(hY); vTaskSuspend(hZ);

    savedMotionBits = saveMotionBits();                    // remember what was moving
    stopAll();

    taskENTER_CRITICAL();
    stateBeforeStop = systemState;
    systemState = ST_STOPPED;                              // lightsTask now flashes both lights fast (FR10)
    taskEXIT_CRITICAL();
    printf("\n\t*** EMERGENCY STOP *** switch 1 = RESUME, switch 2 = RESET\n");
}

// FR6: restore the motor bits (needed when stopped between sensors) and unfreeze the axis tasks.
static void resumeOperation() {
    restoreMotionBits(savedMotionBits);

    taskENTER_CRITICAL();
    systemState = stateBeforeStop;
    taskEXIT_CRITICAL();

    vTaskResume(hX); vTaskResume(hY); vTaskResume(hZ);
    printf("\n\tRESUME\n");
}

// FR7: abort the running operation, keep the stored components, calibrate, clear the cage, restart.
// The interrupted request is discarded (queued ones stay) - confirm the intended behaviour with your professor.
static void resetSystem() {
    printf("\n\tRESET...\n");
    bool box = isBoxInCage();

    // Never kill the warehouse task while it holds the inventory mutex.
    xSemaphoreTake(invMutex, portMAX_DELAY);
    vTaskDelete(hWarehouse);
    vTaskDelete(hX); vTaskDelete(hY); vTaskDelete(hZ);
    inv_recoverAfterResetLocked(box);
    xSemaphoreGive(invMutex);
    stopAll();

    xQueueReset(mbx_X); xQueueReset(mbx_Y); xQueueReset(mbx_Z);
    while (xSemaphoreTake(sem_X_finished, 0) == pdTRUE) {}
    while (xSemaphoreTake(sem_Y_finished, 0) == pdTRUE) {}
    while (xSemaphoreTake(sem_Z_finished, 0) == pdTRUE) {}

    taskENTER_CRITICAL();
    systemState = ST_IDLE;
    stateBeforeStop = ST_IDLE;
    taskEXIT_CRITICAL();
    warehouseBusy = false;
    manualMoving = false;

    resetPending = true;
    createWorkerTasks();      // the new warehouseTask calibrates, waits for the cage to be emptied, then serves the queue
}

struct Debounced { bool state; bool candidate; int count; };

static bool debounce(Debounced& d, bool raw) {
    if (raw == d.state) { d.count = 0; d.candidate = raw; }
    else if (raw == d.candidate) { if (++d.count >= DEBOUNCE_SAMPLES) { d.state = raw; d.count = 0; } }
    else { d.candidate = raw; d.count = 1; }
    return d.state;
}

void supervisorTask(void* pvParameters) {
    Debounced dUp = { false, false, 0 }, dLo = { false, false, 0 };
    bool prevUp = false, prevLo = false, bothSeen = false;
    int pulsesUp = 0, pulsesLo = 0;
    TickType_t lastUp = 0, lastLo = 0;
    TickType_t xLast = xTaskGetTickCount();

    for (;;) {
        vTaskDelayUntil(&xLast, ms2t(SUPERVISOR_PERIOD_MS));              // periodic

        bool rawU, rawL;
        readSwitches(&rawU, &rawL);                                       // switch 1 = upper, switch 2 = lower (active high)
        bool up = debounce(dUp, rawU);
        bool lo = debounce(dLo, rawL);
        TickType_t now = xTaskGetTickCount();
        SysState st = systemState;

        // ---- both switches while moving = emergency STOP (FR5) ----
        if (up && lo && !bothSeen) {
            bothSeen = true;
            pulsesUp = pulsesLo = 0;                                      // discard half-typed entries
            if (st == ST_STORING || st == ST_DISPENSING) emergencyStop();
        }

        bool relUp = prevUp && !up;                                       // release edges
        bool relLo = prevLo && !lo;

        if (!bothSeen) {
            if (st == ST_STOPPED) {
                if (relUp)      resumeOperation();                        // FR6
                else if (relLo) resetSystem();                            // FR7
            }
            else {
                if (relUp) { pulsesUp++; lastUp = now; }                  // pulse entry (ID)
                if (relLo) { pulsesLo++; lastLo = now; }                  // pulse entry (family)
            }
        }

        // ---- close pulse sequences after an idle timeout ----
        if (pulsesUp > 0 && (now - lastUp) >= ms2t(PULSE_TIMEOUT_MS)) {
            if (pulsesUp >= 1 && pulsesUp <= 9) {
                printf("\n\tswitches: STORE ID %d (family %d)\n", pulsesUp, familyOfId(pulsesUp));
                if (!submitRequest(REQ_STORE, pulsesUp, familyOfId(pulsesUp))) printf("\trequest queue full\n");
            }
            else printf("\n\tswitches: ID %d out of range (1-9) - rejected\n", pulsesUp);
            pulsesUp = 0;
        }
        if (pulsesLo > 0 && (now - lastLo) >= ms2t(PULSE_TIMEOUT_MS)) {
            if (pulsesLo >= 1 && pulsesLo <= 3) {
                printf("\n\tswitches: DISPENSE family %d\n", pulsesLo);
                if (!submitRequest(REQ_DISPENSE, 0, pulsesLo)) printf("\trequest queue full\n");
            }
            else printf("\n\tswitches: family %d out of range (1-3) - rejected\n", pulsesLo);
            pulsesLo = 0;
        }

        if (!up && !lo) bothSeen = false;
        prevUp = up; prevLo = lo;
    }
}


////////////////////////////////////////////////////////////////////////////////////////
//                                      LIGHTS TASK                                   //
////////////////////////////////////////////////////////////////////////////////////////

void lightsTask(void* pvParameters) {
    TickType_t xLast = xTaskGetTickCount();
    int ticks = 0;
    bool phase = false;

    for (;;) {
        vTaskDelayUntil(&xLast, ms2t(LIGHTS_PERIOD_MS));

        if (lightsTest) {                                                 // menu light test overrides the pattern
            setLights((lightsTest & 1) != 0, (lightsTest & 2) != 0);
            continue;
        }

        SysState st = systemState;
        int half = (st == ST_STOPPED) ? BLINK_FAST_TICKS : BLINK_SLOW_TICKS;
        if (++ticks >= half) { ticks = 0; phase = !phase; }

        bool upper = false, lower = false;
        switch (st) {
        case ST_STORING:     upper = phase;            break;     // FR8
        case ST_DISPENSING:  lower = phase;            break;     // FR9
        case ST_STOPPED:     upper = lower = phase;    break;     // FR10 (faster)
        default: break;
        }
        setLights(upper, lower);
    }
}


////////////////////////////////////////////////////////////////////////////////////////
//                         MENU TASK (non-blocking keyboard + queries)                //
////////////////////////////////////////////////////////////////////////////////////////

static void printMenu() {
    printf("\n\tCOMMANDS\n");
    printf("\t  t -> store component (asks ID 1-9)     r -> dispense (asks family 1-3)\n");
    printf("\t  i -> list stored components (FR11)     f -> family info (FR12)\n");
    printf("\t  c -> cell info (FR13)                  h -> history (FR14)\n");
    printf("\t  n -> dispense counts (FR15)            p -> positions + box sensor\n");
    printf("\t  e -> light test (upper 1 s, lower 1 s) m -> this menu\n");
    printf("\t  manual (idle only): a/d X left/right, w/s Z up/down, y/u Y inside/outside, b stop\n");
    printf("\t  switches: switch 1 pulses = store ID, switch 2 pulses = dispense family, both = STOP\n");
    printf("\t  ESC -> exit\n\n");
}

struct LineReader { char buf[32]; int len; };

// Collects characters without ever blocking. Returns true when Enter was pressed.
static bool readLineNB(LineReader& lr) {
    while (_kbhit()) {
        int c = _getch();
        if (c == '\r') { lr.buf[lr.len] = '\0'; lr.len = 0; putchar('\n'); return true; }
        if (c == '\b') { if (lr.len > 0) { lr.len--; printf("\b \b"); } }
        else if (c >= 32 && lr.len < (int)sizeof(lr.buf) - 1) { lr.buf[lr.len++] = (char)c; putchar(c); }
    }
    return false;
}

// ---- information queries: copy under the mutex, print AFTER releasing it ----
static void inv_snapshot(Cell out[NUM_X][NUM_Z]) {
    xSemaphoreTake(invMutex, portMAX_DELAY);
    memcpy(out, cells, sizeof(cells));
    xSemaphoreGive(invMutex);
}

static void printInventory() {                                     // FR11
    Cell snap[NUM_X][NUM_Z];
    inv_snapshot(snap);
    time_t now = time(NULL);
    int n = 0;
    printf("\n\tID  FAM  CELL   STORED AT            ELAPSED\n");
    for (int cx = 0; cx < NUM_X; cx++)
        for (int cz = 0; cz < NUM_Z; cz++) {
            Cell& c = snap[cx][cz];
            if (c.st != CELL_STORED && c.st != CELL_DISPENSING) continue;
            char ts[32], ds[32];
            fmtTime(c.stored, ts, sizeof(ts));
            fmtDuration((long)difftime(now, c.stored), ds, sizeof(ds));
            printf("\t%-3d %-4d (%d,%d)  %s  %s%s\n", c.id, familyOfId(c.id), cx + 1, cz + 1, ts, ds,
                c.st == CELL_DISPENSING ? "  [being dispensed]" : "");
            n++;
        }
    if (!n) printf("\t(warehouse empty)\n");
}

static void printFamilyInfo(int family) {                          // FR12
    Cell snap[NUM_X][NUM_Z];
    inv_snapshot(snap);
    int avail = 0; const Cell* best = NULL; int bx = 0, bz = 0;
    for (int cx = 0; cx < NUM_X; cx++)
        for (int cz = 0; cz < NUM_Z; cz++) {
            const Cell& c = snap[cx][cz];
            if (c.st == CELL_STORED && familyOfId(c.id) == family) {
                avail++;
                if (!best || c.seq < best->seq) { best = &c; bx = cx + 1; bz = cz + 1; }
            }
        }
    if (!best) { printf("\n\tfamily %d: OUT OF STOCK\n", family); return; }
    char ts[32]; fmtTime(best->stored, ts, sizeof(ts));
    printf("\n\tfamily %d: %d available | next: ID %d, cell (%d,%d), stored %s\n", family, avail, best->id, bx, bz, ts);
}

static void printCellInfo(int x, int z) {                          // FR13
    if (x < 1 || x > NUM_X || z < 1 || z > NUM_Z) { printf("\n\tinvalid cell\n"); return; }
    Cell snap[NUM_X][NUM_Z];
    inv_snapshot(snap);
    const Cell& c = snap[x - 1][z - 1];
    if (c.st == CELL_EMPTY || c.st == CELL_RESERVED) { printf("\n\tcell (%d,%d): EMPTY\n", x, z); return; }
    char ts[32], ds[32];
    fmtTime(c.stored, ts, sizeof(ts));
    fmtDuration((long)difftime(time(NULL), c.stored), ds, sizeof(ds));
    printf("\n\tcell (%d,%d): ID %d | family %d | stored %s | elapsed %s\n", x, z, c.id, familyOfId(c.id), ts, ds);
}

static void printHistory() {                                       // FR14
    static HistRec snap[MAX_HISTORY];                              // static: keep it off the task stack
    int n;
    xSemaphoreTake(invMutex, portMAX_DELAY);
    n = histCount;
    memcpy(snap, history, sizeof(HistRec) * n);
    xSemaphoreGive(invMutex);

    printf("\n\tID  FAM  DISPENSED AT         STORAGE DURATION\n");
    for (int i = 0; i < n; i++) {
        char ts[32], ds[32];
        fmtTime(snap[i].when, ts, sizeof(ts)); fmtDuration(snap[i].duration, ds, sizeof(ds));
        printf("\t%-3d %-4d %s  %s\n", snap[i].id, snap[i].family, ts, ds);
    }
    if (!n) printf("\t(no completed services)\n");
}

static void printCounts() {                                        // FR15
    int f1, f2, f3, tot;
    xSemaphoreTake(invMutex, portMAX_DELAY);
    f1 = countFamily[1]; f2 = countFamily[2]; f3 = countFamily[3]; tot = countTotal;
    xSemaphoreGive(invMutex);
    printf("\n\tcompleted services: family1=%d family2=%d family3=%d | total=%d\n", f1, f2, f3, tot);
}

enum MenuState { M_MAIN, M_ASK_STORE_ID, M_ASK_DISPENSE_FAMILY, M_ASK_FAMILY_INFO, M_ASK_CELL_INFO };

static bool manualAllowed() {
    if (systemState != ST_IDLE || warehouseBusy || uxQueueMessagesWaiting(requestQueue) > 0) {
        printf("\tmanual control only when the system is idle\n");
        return false;
    }
    return true;
}

static void manualKey(int k) {
    switch (k) {
    case 'a': case 'd': case 'w': case 's':
        if (!manualAllowed()) return;
        if (getYPos() != 2) { printf("\tY must be at centre (2) to move X/Z (NF5)\n"); return; }   // NF5
        manualMoving = true;
        if (k == 'a') moveXLeft();  else if (k == 'd') moveXRight();
        else if (k == 'w') moveZUp(); else moveZDown();
        break;
    case 'y': if (manualAllowed()) { manualMoving = true; moveYInside(); } break;
    case 'u': if (manualAllowed()) { manualMoving = true; moveYOutside(); } break;
    case 'b': stopAll(); manualMoving = false; printf("\tstopped all axes\n"); break;
    case 'p': printf("\tpositions: X=%d Y=%d Z=%d | box in cage: %s\n",
        getXPos(), getYPos(), getZPos(), isBoxInCage() ? "YES" : "no"); break;
    case 'e':                                                       // light test: find out which bits are the lights
        printf("\tlight test: upper (P2.%d) ...\n", P2_BIT_LIGHT_UPPER);
        lightsTest = 1; vTaskDelay(ms2t(1000));
        printf("\tlight test: lower (P2.%d) ...\n", P2_BIT_LIGHT_LOWER);
        lightsTest = 2; vTaskDelay(ms2t(1000));
        lightsTest = 0;
        break;
    }
}

void menuTask(void* pvParameters) {
    MenuState st = M_MAIN;
    LineReader lr = {};
    printMenu();

    for (;;) {
        if (st == M_MAIN) {
            if (_kbhit()) {
                int k = _getch();
                switch (k) {
                case 't': printf("\tstore - component ID (1-9): ");    st = M_ASK_STORE_ID;        break;
                case 'r': printf("\tdispense - family (1-3): ");       st = M_ASK_DISPENSE_FAMILY; break;
                case 'f': printf("\tfamily info (1-3): ");             st = M_ASK_FAMILY_INFO;     break;
                case 'c': printf("\tcell info (X Z): ");               st = M_ASK_CELL_INFO;       break;
                case 'i': printInventory(); break;
                case 'h': printHistory();   break;
                case 'n': printCounts();    break;
                case 'm': printMenu();      break;
                case 27:  stopAll(); closeChannels(); exit(0);
                default:  manualKey(k);     break;
                }
            }
        }
        else if (readLineNB(lr)) {
            int a = 0, b = 0;
            int n = sscanf(lr.buf, "%d %d", &a, &b);
            switch (st) {
            case M_ASK_STORE_ID:
                if (n == 1 && a >= 1 && a <= 9) {
                    if (!isBoxInCage()) printf("\t(note: no component in the cage right now)\n");
                    if (submitRequest(REQ_STORE, a, familyOfId(a))) printf("\tstore ID %d (family %d) queued\n", a, familyOfId(a));
                    else printf("\trequest queue full\n");
                }
                else printf("\tinvalid ID\n");
                break;
            case M_ASK_DISPENSE_FAMILY:
                if (n == 1 && a >= 1 && a <= 3) {
                    if (submitRequest(REQ_DISPENSE, 0, a)) printf("\tdispense family %d queued\n", a);
                    else printf("\trequest queue full\n");
                }
                else printf("\tinvalid family\n");
                break;
            case M_ASK_FAMILY_INFO:
                if (n == 1 && a >= 1 && a <= 3) printFamilyInfo(a); else printf("\tinvalid family\n");
                break;
            case M_ASK_CELL_INFO:
                if (n == 2) printCellInfo(a, b); else printf("\tinvalid cell\n");
                break;
            default: break;
            }
            st = M_MAIN;
        }

        vTaskDelay(ms2t(20));                   // never spin: lets every other task run
    }
}


////////////////////////////////////////////////////////////////////////////////////////
//                                  START-UP AND MAIN                                 //
////////////////////////////////////////////////////////////////////////////////////////

void inicializarPortos() {
    printf("\nwaiting for hardware simulator...");
    createDigitalInput(0);
    createDigitalInput(1);
    createDigitalOutput(2);
    writeDigitalU8(2, 0);
    printf("\ngot access to simulator...");
}

void myDaemonTaskStartupHook(void) {
    // runs once: create ports, kernel objects and tasks
    inicializarPortos();
    inv_init();

    // ---- mailboxes ----
    requestQueue = xQueueCreate(10, sizeof(Request));    // length > 1: requests accumulate (NF1)
    mbx_X = xQueueCreate(1, sizeof(XCmd));
    mbx_Y = xQueueCreate(1, sizeof(int));
    mbx_Z = xQueueCreate(1, sizeof(ZCmd));

    // ---- semaphores / mutex ----
    sem_X_finished = xSemaphoreCreateBinary();
    sem_Y_finished = xSemaphoreCreateBinary();
    sem_Z_finished = xSemaphoreCreateBinary();
    invMutex = xSemaphoreCreateMutex();

    configASSERT(requestQueue && mbx_X && mbx_Y && mbx_Z);
    configASSERT(sem_X_finished && sem_Y_finished && sem_Z_finished && invMutex);

    // ---- tasks ----
    createWorkerTasks();                                  // X, Y, Z axis tasks + warehouse (calibrates first)
    xTaskCreate(supervisorTask, "Supervisor", STACK_BIG, NULL, PRIO_SUPERVISOR, &hSupervisor);
    xTaskCreate(lightsTask, "Lights", STACK_SMALL, NULL, PRIO_LIGHTS, &hLights);
    xTaskCreate(menuTask, "Menu", STACK_BIG, NULL, PRIO_MENU, &hMenu);
}

int main()
{
    initialiseHeap();
    vApplicationDaemonTaskStartupHook = &myDaemonTaskStartupHook;
    vTaskStartScheduler();
}
