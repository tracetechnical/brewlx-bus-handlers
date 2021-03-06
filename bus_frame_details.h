#ifndef BUS_FRAME_DETAILS_H
#define	BUS_FRAME_DETAILS_H

#define MAX_FRAME_SIZE 4 + MAX_PAYLOAD
#define MAX_PAYLOAD 15 * 8
#define MAX_UNPACKED_PAYLOAD 15 * 6

#define APPLICATION_BUFFER_SIZE MAX_UNPACKED_PAYLOAD
#define SEND_BUFFER_SIZE MAX_FRAME_SIZE
#define UART_BUFFER_SIZE 64
#define BUS_TX_BUFFER_SIZE MAX_FRAME_SIZE
#define HANDLER_INBOUND_BUFFER_SIZE MAX_FRAME_SIZE
#define FRAME_WRITER_PROCESS_BUFFER_SIZE MAX_FRAME_SIZE

#endif	/* BUS_FRAME_DETAILS_H */
