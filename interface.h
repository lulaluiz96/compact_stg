#ifndef INTERFACE_H
#define INTERFACE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <Windows.h>
#include <stdint.h>

typedef uint8_t  uInt8;
typedef uint32_t uInt32;

/* BOOL and HANDLE are provided by Windows.h. */
/* TaskHandle is from DAQmx; when DAQmx is not used we keep it as a pointer. */
typedef void* TaskHandle;

/* Digital I/O interface */
void createDigitalInput(uInt32 port);
void createDigitalOutput(uInt32 port);
uInt8 readDigitalU8(uInt32 port);
void writeDigitalU8(uInt32 port, uInt8 data);
void closeChannels(void);

#ifdef __cplusplus
}
#endif

#endif /* INTERFACE_H */
