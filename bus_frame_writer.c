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

#include "bus_frame_writer_status.h"

typedef union {
    struct {
        unsigned char mask;
        unsigned char crc;
        unsigned char dataBytes[6];
    };
    unsigned char bytes[8];
} tBlockLayout;

typedef enum {
    BUS_FRAME_WRITER_NONE = 0,
    BUS_FRAME_WRITER_WAIT_FOR_WRITE_TRIGGER = 10,
    BUS_FRAME_WRITER_LOCK_BUFFER = 20,
    BUS_FRAME_WRITER_CALCULATE_BLOCKS = 40,
    BUS_FRAME_WRITER_WRITE_STARTCODE1 = 50,
    BUS_FRAME_WRITER_WRITE_STARTCODE2 = 70,
    BUS_FRAME_WRITER_INITIALISE_BLOCK = 90,
    BUS_FRAME_WRITER_BLOCK_FILL_GET_BYTE = 100,
    BUS_FRAME_WRITER_WRITE_BYTE_TO_BUFFER = 200,
    BUS_FRAME_WRITER_UNLOCK_BUFFER = 240,
    BUS_FRAME_WRITER_WRITE_ENDCODE1 = 260,
    BUS_FRAME_WRITER_WRITE_ENDCODE2 = 280,
    BUS_FRAME_WRITER_TRIGGER_LISTENER = 300,
    BUS_FRAME_WRITER_WAIT_PROCESSED = 310,
    BUS_FRAME_WRITER_COMPLETE_RESET = 320,
    BUS_FRAME_WRITER_PROCESS_ERROR = 330
} eBusFrameWriterState;

typedef union {
    struct {
        unsigned frameOpen: 1;
        unsigned writeTrigger: 1;
    };
    unsigned char byte;
} tBusFrameWriterFlags;

tBlockLayout tempBlock;
eBusFrameWriterState busFrameWriterState;
tBuffer frameWriterProcessBuffer;
unsigned char frameWriterProcessBufferArray[FRAME_WRITER_PROCESS_BUFFER_SIZE];
tBusFrameWriterFlags busFrameWriterFlags;
tBuffer *ptrSendBuffer;
eBufferOperationStatus bufferProcessStatus;
eBufferOperationStatus bufferWriteStatus;

unsigned char byteCount;
unsigned char frameWriterBlockCount;
unsigned char byteToWrite;
unsigned char tempMask;
unsigned char outputBlockCount;
unsigned char blockByteCount;
unsigned char *ptrSendListener;
unsigned char queuedFrameCount;

eBusFrameWriterState lastState;
unsigned char traceCount = 0;

void initialiseBusFrameWriter(void);
void registerSendFrameListener(unsigned char *ptrListener);
void runBusFrameWriter(void);
void openBusFrame(eBusFrameWriterOperationStatus *ptrStatus);
void writeToBusFrame(eBusFrameWriterOperationStatus *ptrStatus, unsigned char byte);
void closeBusFrame(eBusFrameWriterOperationStatus *ptrStatus);
void sendFramesInBuffer(eBusFrameWriterOperationStatus *ptrStatus);
void registerSendFrameBuffer(tBuffer *ptrBuffer);
unsigned char getQueuedFrameCount(void);

void initialiseBusFrameWriter(void) {
    initialiseBuffer(&frameWriterProcessBuffer, &frameWriterProcessBufferArray[0], FRAME_WRITER_PROCESS_BUFFER_SIZE);
}

void runBusFrameWriter(void) {
    unsigned char i;
    switch(busFrameWriterState) {    
        case BUS_FRAME_WRITER_NONE:
            busFrameWriterState = BUS_FRAME_WRITER_WAIT_FOR_WRITE_TRIGGER;
            break;

        case BUS_FRAME_WRITER_WAIT_FOR_WRITE_TRIGGER:
            if (busFrameWriterFlags.writeTrigger) {
                busFrameWriterState = BUS_FRAME_WRITER_LOCK_BUFFER;
            }
            break;

        case BUS_FRAME_WRITER_LOCK_BUFFER:
            bufferProcessStatus = BUFFER_OPERATION_NONE;
            writeLockMainBuffer(&frameWriterProcessBuffer,&bufferProcessStatus);
            busFrameWriterState = bufferProcessStatus == BUFFER_OPERATION_OK ? BUS_FRAME_WRITER_CALCULATE_BLOCKS : BUS_FRAME_WRITER_LOCK_BUFFER;
            break;

        case BUS_FRAME_WRITER_CALCULATE_BLOCKS:
            byteCount = getFillLevel(&frameWriterProcessBuffer);
            outputBlockCount = (byteCount / 6) + ((byteCount % 6) > 0);
            frameWriterBlockCount = 0;
            busFrameWriterState = outputBlockCount <= 0x0F ? BUS_FRAME_WRITER_WRITE_STARTCODE1 : BUS_FRAME_WRITER_PROCESS_ERROR;
            break;

        case BUS_FRAME_WRITER_WRITE_STARTCODE1:
            bufferProcessStatus = BUFFER_OPERATION_NONE;
            byteToWrite = 0xC0 + outputBlockCount;
            putByte(ptrSendBuffer,&bufferProcessStatus,byteToWrite);
            busFrameWriterState = bufferProcessStatus == BUFFER_OPERATION_OK ? BUS_FRAME_WRITER_WRITE_STARTCODE2 : BUS_FRAME_WRITER_WRITE_STARTCODE1;
            break;

        case BUS_FRAME_WRITER_WRITE_STARTCODE2:
            bufferProcessStatus = BUFFER_OPERATION_NONE;
            byteToWrite = 0xD0 + outputBlockCount;
            putByte(ptrSendBuffer,&bufferProcessStatus,byteToWrite);
            busFrameWriterState = bufferProcessStatus == BUFFER_OPERATION_OK ? BUS_FRAME_WRITER_INITIALISE_BLOCK : BUS_FRAME_WRITER_WRITE_STARTCODE2;
            break;

        case BUS_FRAME_WRITER_INITIALISE_BLOCK:
            tempBlock.crc = 0;
            tempBlock.mask = 0;
            blockByteCount = 0;
            frameWriterBlockCount++;
            for(i=0; i < 6; i++) {
                tempBlock.dataBytes[i] = 0;
            }
            busFrameWriterState = BUS_FRAME_WRITER_BLOCK_FILL_GET_BYTE;
            break;

        case BUS_FRAME_WRITER_BLOCK_FILL_GET_BYTE:
            do {
                do {
                    bufferProcessStatus = BUFFER_OPERATION_NONE;
                    getByte(&frameWriterProcessBuffer, &bufferProcessStatus, &byteToWrite);
                } while (bufferProcessStatus != BUFFER_OPERATION_OK);
                if(blockByteCount < 6) {
                    tempBlock.dataBytes[blockByteCount++] = byteToWrite;
                }
            } while(!isEmpty(&frameWriterProcessBuffer) && blockByteCount < 6);
            
            do {
                if(blockByteCount < 6) {
                    tempBlock.dataBytes[blockByteCount++] = 0xFF;
                }
            } while(blockByteCount < 6);
            tempMask = 0x80;
            blockByteCount = 0;
            do {
                
                if(tempBlock.dataBytes[blockByteCount] >= 0x80) {
                    tempBlock.dataBytes[blockByteCount] = tempBlock.dataBytes[blockByteCount] & 0b01111111;
                    tempMask = tempMask | (1 << blockByteCount);
                }
                blockByteCount++;
            } while (blockByteCount < 6); 
            blockByteCount = 0;
            tempBlock.mask = tempMask;
            tempBlock.crc = calculateCrc(&tempBlock.dataBytes[0],6);
            busFrameWriterState = BUS_FRAME_WRITER_WRITE_BYTE_TO_BUFFER;
            break;

        case BUS_FRAME_WRITER_WRITE_BYTE_TO_BUFFER:
            do {
                do {
                    bufferProcessStatus = BUFFER_OPERATION_NONE;
                    putByte(ptrSendBuffer, &bufferProcessStatus, tempBlock.bytes[blockByteCount]);
                } while (bufferProcessStatus != BUFFER_OPERATION_OK);
                blockByteCount++;
            } while (blockByteCount < 8);
            busFrameWriterState = frameWriterBlockCount < outputBlockCount ? BUS_FRAME_WRITER_INITIALISE_BLOCK : BUS_FRAME_WRITER_UNLOCK_BUFFER;
            break;

        case BUS_FRAME_WRITER_UNLOCK_BUFFER:
            bufferProcessStatus = BUFFER_OPERATION_NONE;
            unlockWriteMainBuffer(&frameWriterProcessBuffer,&bufferProcessStatus);
            if(bufferProcessStatus == BUFFER_OPERATION_OK) {
                busFrameWriterState = BUS_FRAME_WRITER_WRITE_ENDCODE1;
            } else {
                busFrameWriterState = BUS_FRAME_WRITER_UNLOCK_BUFFER;
            }
            break;

        case BUS_FRAME_WRITER_WRITE_ENDCODE1:
            bufferProcessStatus = BUFFER_OPERATION_NONE;
            byteToWrite = 0xE0 + outputBlockCount;
            putByte(ptrSendBuffer,&bufferProcessStatus,byteToWrite);
            busFrameWriterState = bufferProcessStatus == BUFFER_OPERATION_OK ? BUS_FRAME_WRITER_WRITE_ENDCODE2 : BUS_FRAME_WRITER_WRITE_ENDCODE1;
            break;

        case BUS_FRAME_WRITER_WRITE_ENDCODE2:
            bufferProcessStatus = BUFFER_OPERATION_NONE;
            byteToWrite = 0xF0 + outputBlockCount;
            putByte(ptrSendBuffer,&bufferProcessStatus,byteToWrite);
            busFrameWriterState = bufferProcessStatus == BUFFER_OPERATION_OK ? BUS_FRAME_WRITER_TRIGGER_LISTENER : BUS_FRAME_WRITER_WRITE_ENDCODE2;
            break;

        case BUS_FRAME_WRITER_TRIGGER_LISTENER:
            *ptrSendListener = 1;
            busFrameWriterState = BUS_FRAME_WRITER_WAIT_PROCESSED;

        case BUS_FRAME_WRITER_WAIT_PROCESSED:
            if(!*ptrSendListener) {
                busFrameWriterState = BUS_FRAME_WRITER_COMPLETE_RESET;
                queuedFrameCount--;
            }
            break;

        case BUS_FRAME_WRITER_COMPLETE_RESET:
            busFrameWriterFlags.writeTrigger = 0;
            busFrameWriterState = BUS_FRAME_WRITER_NONE;
            break;

        case BUS_FRAME_WRITER_PROCESS_ERROR:      
            break;
    }
}

void openBusFrame(eBusFrameWriterOperationStatus *ptrStatus) {
    if(busFrameWriterFlags.frameOpen) {
        *ptrStatus = BUS_FRAME_WRITER_ERROR_WRITING;
        return;
    }
    busFrameWriterFlags.frameOpen = 1;
    *ptrStatus = BUS_FRAME_WRITER_OPERATION_OK;
}

void writeToBusFrame(eBusFrameWriterOperationStatus *ptrStatus, unsigned char byte) {
    if(!busFrameWriterFlags.frameOpen) {
        *ptrStatus = BUS_FRAME_WRITER_ERROR_WRITING;
        return;
    }
    bufferWriteStatus = BUFFER_OPERATION_NONE;
    putByte(&frameWriterProcessBuffer,&bufferWriteStatus,byte);
    if(bufferWriteStatus != BUFFER_OPERATION_OK) {
        *ptrStatus = BUS_FRAME_WRITER_ERROR_WRITING;
    } else {
        *ptrStatus = BUS_FRAME_WRITER_OPERATION_OK;
    }
}

void closeBusFrame(eBusFrameWriterOperationStatus *ptrStatus) {
    if(!busFrameWriterFlags.frameOpen) {
        *ptrStatus = BUS_FRAME_WRITER_ERROR_WRITING;
        return;
    }
    busFrameWriterFlags.frameOpen = 0;
    queuedFrameCount++;
    *ptrStatus = BUS_FRAME_WRITER_OPERATION_OK;
}

unsigned char getQueuedFrameCount(void) {
    return queuedFrameCount;
}

unsigned char isWriteDone(void) {
    return busFrameWriterFlags.writeTrigger;
}

void sendFramesInBuffer(eBusFrameWriterOperationStatus *ptrStatus) {
    if(busFrameWriterFlags.writeTrigger) {
        *ptrStatus = BUS_FRAME_WRITER_ERROR_WRITING;
        return;
    }
    busFrameWriterFlags.writeTrigger = 1;
    *ptrStatus = BUS_FRAME_WRITER_OPERATION_OK;
}

void registerSendFrameBuffer(tBuffer *ptrBuffer) {
    ptrSendBuffer = ptrBuffer;
}
void registerSendFrameListener(unsigned char *ptrListener) {
    ptrSendListener = ptrListener;
}