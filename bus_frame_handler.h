#ifndef BUS_FRAME_HANDLER_H
#define	BUS_FRAME_HANDLER_H

#include "../ring-buffer/ring_buffer_types.h"
#include "bus_frame_handler_status.h"

extern void initialiseBusFrameHandler(void);
extern void runBusFrameHandler(void);
extern void registerApplicationBuffer(tBuffer *ptrAppBuffer);
extern void registerApplicationListener(unsigned char *ptrListener);
extern void putByteForHandling(eBusHandlerOperationStatus *ptrStatus, unsigned char byte);

#endif	/* BUS_FRAME_HANDLER_H */

