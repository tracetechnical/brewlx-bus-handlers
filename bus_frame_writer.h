#ifndef BUS_FRAME_WRITER_H
#define	BUS_FRAME_WRITER_H

#include "ring-buffer/ring_buffer_types.h"
#include "bus_frame_writer_status.h"

extern void initialiseBusFrameWriter(void);
extern void runBusFrameWriter(void);
extern void registerSendFrameBuffer(tBuffer *ptrBuffer);
extern void registerSendFrameListener(unsigned char *ptrListener);
extern void openBusFrame(eBusFrameWriterOperationStatus *ptrStatus);
extern void writeToBusFrame(eBusFrameWriterOperationStatus *ptrStatus, unsigned char byte);
extern void closeBusFrame(eBusFrameWriterOperationStatus *ptrStatus);
extern void sendFramesInBuffer(eBusFrameWriterOperationStatus *ptrStatus);
extern unsigned char getQueuedFrameCount(void);

#endif	/* BUS_FRAME_WRITER_H */

