/*
 * File:   control_bus.c
 * Author: Alex
 *
 * Created on 16 October 2020, 13:13
 */


#include "../global.h"
#include "../crc.h"
#include "../ring-buffer/ring_buffer.h"
#include "bus_frame_details.h"
#include "bus_frame_handler_status.h"

#define WRITE_OUT_MAX_RETRIES   8

#define MARKERS_NONE            0
#define MARKERS_IN_PRESTART     1
#define MARKERS_STARTED         3
#define MARKERS_IN_PREFINISH    7
#define MARKERS_FINISHED        15

typedef enum {
    BUS_HANDLER_NONE = 0,
    BUS_HANDLER_WAIT_FOR_BYTES,
    BUS_HANDLER_GET_BYTES,
    BUS_HANDLER_HANDLE_BLOCK,
    BUS_HANDLER_CHECK_FINAL,
    BUS_HANDLER_WAIT_PROCESSED,
    BUS_HANDLER_COMPLETE_RESET,
    BUS_HANDLER_PROCESS_ERROR
} eBusHandlerStates;

typedef enum {
    BUS_HANDLE_NONE = 0,
    BUS_HANDLE_COULDNT_WRITE_BYTE_FULL,        
    BUS_HANDLE_OPERATION_OK
} eBusHandleOperationStatus;

typedef struct {
    unsigned char mask;
    unsigned char crc;
    unsigned char payloadBytes[6];
} tBlock;

typedef union {
    tBlock block;
    unsigned char bytes[8];
} tBrokenOutBlock;

typedef union {
    struct {
        unsigned preStart:1;
        unsigned started:1;
        unsigned preFinish:1;
        unsigned finished:1;
    };
    unsigned char markerByte;
} tBusHandlerMarkerFlags;

typedef enum {
    BHE_NONE,
    BHE_LISTENER_NOT_REGISTERED,
    BHE_WRITE_OUT_FAILED,
    BHE_GET_FOR_WRITE_OUT_FAILED,
    BHE_OUT_OF_BOUNDS_BLOCK,
    BHE_TOO_MANY_STUFF,
    BHE_SC1_WITHOUT_SC2,
    BHE_ALREADY_IN_BLOCK,
    BHE_I_SHOULDNT_BE_HERE,
    BHE_INVALID_B2F_IN_EC2,
    BHE_INVALID_B2F_IN_EC1,
    BHE_INVALID_B2F_IN_SC2,
    BHE_OUT_OF_POS_SC2,
    BHE_GOALPOST_OR_MASK_NOT_RECEIVED,
    BHE_GOALPOST_NOT_RECEIVED,
    BHE_MASK_NOT_RECEIVED
} eBusFrameHandlerError;

typedef enum {
    BLOCK_PHASE_NONE,
    BLOCK_BEGIN_BLOCK,
    BLOCK_CHECK_CRC,
    BLOCK_DEMASK,
    BLOCK_WAIT_ACKNOWLEDGE,
    BLOCK_FAIL_CRC_RESET,
} eBlockPhase;

eBlockPhase blockPhase;
tBrokenOutBlock workingBlock;
eBusFrameHandlerError busHandlerError;
tBusHandlerMarkerFlags busHandlerMarkerFlags;
tBuffer *ptrApplicationBuffer;
eBusHandlerStates busHandlerState;
eBufferOperationStatus bufferOpStatus;
tBuffer busHandleInboundBuffer;

extern unsigned int uartTotal;
unsigned char *ptrApplicationListener;
unsigned char bhErrorCtx = 0x00;
unsigned char tempBytes[20];
unsigned char cc;
unsigned char frameBytes;
unsigned char expectedFrameBytes;
unsigned char recount;
unsigned char blockCount;
unsigned char expectedBlocksToFollow;
unsigned char blockPosition;
unsigned int outputByteCount;
unsigned int success;
unsigned char dummyListener;
unsigned char dataReady;
unsigned char dataRequest;
unsigned char busHandleInboundBufferArray[HANDLER_INBOUND_BUFFER_SIZE]; //(15 blocks * 8 bytes) + (SC1,SC2,EC1,EC2) + (4 spare)
unsigned char blockProceed;
unsigned char handlingComplete;
unsigned char handleByte;

unsigned char frameByte[29];

void initialiseBusFrameHandler(void);
void runBusFrameHandler(void);
void registerApplicationBuffer(tBuffer *ptrAppBuffer);
void registerApplicationListener(unsigned char *ptrListener);
void putByteForHandling(eBusHandlerOperationStatus *ptrStatus, unsigned char byte);

void handleBlockData(unsigned char handleByte);
void handleByteSpecial(unsigned char handleByte);
unsigned char isStarvedOfData(void);

unsigned char areMarkersValid(tBusHandlerMarkerFlags flags);
unsigned char calculateCrc(unsigned char *ptrData , unsigned char length);

void initialiseBusFrameHandler(void) {
    busHandlerState = BUS_HANDLER_NONE;
    busHandlerError = BHE_NONE;
    //busHandlerFlags.byte = 0;
    
    ptrApplicationListener = &dummyListener; //Avoid fuckery involving pointers off into space
    
    initialiseBuffer(&busHandleInboundBuffer, &busHandleInboundBufferArray[0], HANDLER_INBOUND_BUFFER_SIZE);
}

void runBusFrameHandler(void) {
    switch (busHandlerState) {
        case BUS_HANDLER_NONE:
            //busHandlerFlags.byte = 0;
            expectedBlocksToFollow = 0;
            expectedFrameBytes = 0;
            frameBytes = 0;
            outputByteCount = 0;
            handlingComplete = 0;
            busHandlerState = BUS_HANDLER_WAIT_FOR_BYTES;
            break;
            
        case BUS_HANDLER_WAIT_FOR_BYTES:
            if(!isEmpty(&busHandleInboundBuffer)) {  
                busHandlerState = BUS_HANDLER_GET_BYTES;  
            }
            break;
            
        case BUS_HANDLER_GET_BYTES:
            if(dataReady == 0) {
                bufferOpStatus = BUFFER_OPERATION_NONE;
                getByte(&busHandleInboundBuffer, &bufferOpStatus, &handleByte);
            }
            if(bufferOpStatus == BUFFER_OPERATION_OK) {
                dataReady = 1;
                handleByteSpecial(handleByte);
                if(dataReady) {
                    if(busHandlerMarkerFlags.markerByte == MARKERS_STARTED) {
                        busHandlerState = BUS_HANDLER_HANDLE_BLOCK;  
                    } else {
                        dataReady = 0;
                    }
                } else {
                    if(blockProceed == 0 && busHandlerMarkerFlags.markerByte == MARKERS_FINISHED) {
                        busHandlerState = BUS_HANDLER_CHECK_FINAL; 
                    }
                }
            }
            break;
            
        case BUS_HANDLER_HANDLE_BLOCK:
            if(busHandlerMarkerFlags.markerByte == MARKERS_IN_PRESTART) {
                if(isEmpty(&busHandleInboundBuffer)) {  
                    busHandlerState = BUS_HANDLER_WAIT_FOR_BYTES;  
                } else {
                    busHandlerState = BUS_HANDLER_GET_BYTES; 
                }
            }
            if (blockProceed == 2) {
                blockProceed = 3;
                busHandlerState = BUS_HANDLER_CHECK_FINAL;  
            }
            handleBlockData(handleByte);
                    
            if (dataRequest && !dataReady) {
                dataRequest = 0;
                if(isEmpty(&busHandleInboundBuffer)) {  
                    busHandlerState = BUS_HANDLER_WAIT_FOR_BYTES;  
                } else {
                    busHandlerState = BUS_HANDLER_GET_BYTES; 
                }
            }
            break;
            
        case BUS_HANDLER_CHECK_FINAL:
            if(handlingComplete == 0) {
                busHandlerState = BUS_HANDLER_WAIT_FOR_BYTES;  
            }
            if(busHandlerMarkerFlags.markerByte == MARKERS_FINISHED) {
                handlingComplete = 1;
                *ptrApplicationListener = 1;
                completeReversibleWrite(ptrApplicationBuffer);
                busHandlerState = BUS_HANDLER_WAIT_PROCESSED; 
            }
            if(!areMarkersValid(busHandlerMarkerFlags)) {
                handlingComplete = 2; //ERROR!!
                busHandlerState = BUS_HANDLER_PROCESS_ERROR;
            }
            break;
            
        case BUS_HANDLER_WAIT_PROCESSED:
            if(!*ptrApplicationListener) {
                busHandlerState = BUS_HANDLER_COMPLETE_RESET;
            }
            break;
            
        case BUS_HANDLER_COMPLETE_RESET:
            busHandlerMarkerFlags.markerByte = 0;
            busHandlerState = BUS_HANDLER_NONE;
            break;
            
        case BUS_HANDLER_PROCESS_ERROR:
            reverseWrite(ptrApplicationBuffer);
            asm("nop");
            asm("nop");
            busHandlerState = BUS_HANDLER_COMPLETE_RESET;
            break;
    }
}

unsigned char isStarvedOfData(void) {
    return isEmpty(&busHandleInboundBuffer) && blockPhase == BLOCK_PHASE_NONE;
}

unsigned char areMarkersValid(tBusHandlerMarkerFlags flags) {
    if (flags.markerByte == 0 || 
            flags.markerByte == 1 || 
            flags.markerByte == 3 || 
            flags.markerByte == 7 || 
            flags.markerByte == 15) {
        return 1;
    } else {
        return 0;
    }
}

void handleByteSpecial(unsigned char handleByte) { 
    unsigned char nibbleHi = (handleByte & 0xF0) >> 4;
    unsigned char nibbleLo = handleByte & 0x0F;
    switch(nibbleHi) {
        case 0x0C:
            //StartCode1
            busHandlerMarkerFlags.markerByte = MARKERS_IN_PRESTART;
            dataReady = 0;
            break;

        case 0x0D:
            busHandlerMarkerFlags.started = 1;
            startReversibleWrite(ptrApplicationBuffer);
            blockPhase = BLOCK_PHASE_NONE;
            blockPosition = 0;
            dataReady = 0;
            break;

        case 0x0E:
            busHandlerMarkerFlags.preFinish = 1;
            dataReady = 0;
            break;

        case 0x0F:
            busHandlerMarkerFlags.finished = 1;
            dataReady = 0;
            break;
    }
}

void handleBlockData(unsigned char handleByte) {
    unsigned char calculatedCrc;
    unsigned char j;
    switch(blockPhase) {
        case BLOCK_PHASE_NONE:
            blockPhase = BLOCK_BEGIN_BLOCK;
            break;
        case BLOCK_BEGIN_BLOCK:
            if(dataReady) {
                blockProceed = 1;
                if((handleByte & 0b11000000) == 0b10000000) {
                    blockPosition = 0; //MASK POS RESET
                    blockCount++;
                }
                workingBlock.bytes[blockPosition] = handleByte;


                frameByte[frameBytes++] = handleByte;
                dataReady = 0;

                if(blockPosition == 7) {
                    blockPhase = BLOCK_CHECK_CRC;
                } else {
                    dataRequest = 1;
                    blockPosition++;
                }
            }
            break;
            
        case BLOCK_CHECK_CRC:
            calculatedCrc=calculateCrc(&workingBlock.block.payloadBytes[0], 6);
            if(calculatedCrc == workingBlock.block.crc) {
                blockPhase = BLOCK_DEMASK;
            } else {
                blockPhase = BLOCK_FAIL_CRC_RESET;
            }
            break;

        case BLOCK_DEMASK:
            for(j = 0; j < 6; j++) {
                if(workingBlock.block.mask & (1 << j)) {
                    workingBlock.block.payloadBytes[j] += 0b10000000;
                }
                outputByteCount++;
                bufferOpStatus = BUFFER_OPERATION_NONE;
                putByte(ptrApplicationBuffer, &bufferOpStatus, workingBlock.block.payloadBytes[j]);
                if(bufferOpStatus != BUFFER_OPERATION_OK) {
                    bhErrorCtx = 0x02;
                    busHandlerState = BUS_HANDLER_PROCESS_ERROR;
                    break;
                }
            }
            blockProceed = 2;
            blockPhase = BLOCK_WAIT_ACKNOWLEDGE;
            break;
            
        case BLOCK_WAIT_ACKNOWLEDGE:
            if(blockProceed == 3) {
                blockProceed = 0;
                blockPhase = BLOCK_BEGIN_BLOCK;
            }
            break;

        case BLOCK_FAIL_CRC_RESET:
            bhErrorCtx = 0x03;
            blockPhase = BLOCK_PHASE_NONE;
            busHandlerState = BUS_HANDLER_PROCESS_ERROR;
            break;
    }
}

void registerApplicationBuffer(tBuffer *ptrAppBuffer) {
    ptrApplicationBuffer = ptrAppBuffer;
}

void registerApplicationListener(unsigned char *ptrListener) {
    ptrApplicationListener = ptrListener;
}

void putByteForHandling(eBusHandlerOperationStatus *ptrStatus, unsigned char byte) {
    eBufferOperationStatus bufferExtOpStatus = BUFFER_OPERATION_NONE;
    putByte(&busHandleInboundBuffer,&bufferExtOpStatus,byte);
    *ptrStatus = bufferExtOpStatus == BUFFER_OPERATION_OK ? BUS_HANDLER_OPERATION_OK : BUS_HANDLER_CANT_WRITE;
    if(*ptrStatus != BUFFER_OPERATION_OK) {
        asm("nop");
        asm("nop");
    }
}