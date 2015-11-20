/*
 * Copyright 2015  Mathieu Laurendeau (mat.lau [at] laposte [dot] net)
 * License: GPLv3
 */
 
#include "emu.h"

#include <LUFA/Drivers/Peripheral/Serial.h>
#include "../protocol.h"

#define MAX_CONTROL_TRANSFER_SIZE 64

#define USART_BAUDRATE 500000
#define USART_DOUBLE_SPEED false

/*
 * This is a block of memory to store all descriptors.
 */
static uint8_t descriptors[MAX_DESCRIPTORS_SIZE];
static s_descIndex descIndex[MAX_DESCRIPTORS]; // MAX_DESCRIPTORS x 5 bytes
static s_endpoint endpoints[MAX_ENDPOINTS]; // 48 bytes

static uint8_t * pdesc = descriptors;

#if MAX_DESCRIPTORS > 255 / 5
static uint8_t * pindex = descIndex;
#endif

static s_input input; // 66 bytes (should not exceed 255 bytes)

/*
 * These variables are used in both the main and serial interrupt,
 * therefore they have to be declared as volatile.
 */
static volatile unsigned char started = 0;
static volatile unsigned char controlReply = 0;
static volatile unsigned char controlReplyLen = 0;

static inline void forceHardReset(void) {

    cli(); // disable interrupts
    wdt_enable(WDTO_15MS); // enable watchdog
    while(1); // wait for watchdog to reset processor
}

static inline int8_t Serial_BlockingReceiveByte(void) {

    while(!Serial_IsCharReceived()) {}
    return UDR1;
}

static inline void send_control_header(void) {

    uint8_t len = sizeof(USB_ControlRequest);
    if( !(USB_ControlRequest.bmRequestType & REQDIR_DEVICETOHOST) ) {
        len += (USB_ControlRequest.wLength & 0xFF);
    }
    Serial_SendByte(E_TYPE_CONTROL);
    Serial_SendByte(len);
    Serial_SendData(&USB_ControlRequest, sizeof(USB_ControlRequest));
}

static unsigned char buf[MAX_CONTROL_TRANSFER_SIZE];

#define READ_VALUE_INC(TARGET) \
    while (value_len--) { \
        *(TARGET++) = Serial_BlockingReceiveByte(); \
    }

#define READ_VALUE(TARGET) \
    { \
        unsigned char * ptr = TARGET; \
        READ_VALUE_INC(ptr) \
    }

static inline void ack(const unsigned char type) {
    Serial_SendByte(type);
    Serial_SendByte(BYTE_LEN_0_BYTE);
}


ISR(USART1_RX_vect) {

    unsigned char packet_type = UDR1;
    unsigned char value_len = Serial_BlockingReceiveByte();
    static const void * labels[] = { &&l_descriptors, &&l_index, &&l_endpoints, &&l_reset, &&l_control, &&l_in };
    if(packet_type > E_TYPE_IN) {
        return;
    }
    goto *labels[packet_type];
    l_descriptors:
    READ_VALUE_INC(pdesc)
    ack(E_TYPE_DESCRIPTORS);
    return;
    l_index:
#if MAX_DESCRIPTORS > 255 / 5
    READ_VALUE_INC(pindex)
#else
    READ_VALUE((uint8_t*)&descIndex)
#endif
    ack(E_TYPE_INDEX);
    return;
    l_endpoints:
    READ_VALUE((uint8_t*)&endpoints)
    ack(E_TYPE_ENDPOINTS);
    started = 1;
    return;
    l_reset:
    forceHardReset();
    return;
    l_control:
    controlReplyLen = value_len;
    READ_VALUE(buf)
    controlReply = 1;
    return;
    l_in:
    READ_VALUE((uint8_t*)&input)
    return;
}

void serial_init(void) {

    Serial_Init(USART_BAUDRATE, USART_DOUBLE_SPEED);

    UCSR1B |= (1 << RXCIE1); // Enable the USART Receive Complete interrupt (USART_RXC)
}

void SetupHardware(void) {

    /* Disable watchdog */
    MCUSR = 0;
    wdt_disable();

    clock_prescale_set(clock_div_1);

    serial_init();

    GlobalInterruptEnable();

    LEDs_Init();

    while(!started) {}

    USB_Init();
}

void EVENT_USB_Device_Connect(void) {

}

void EVENT_USB_Device_Disconnect(void) {

}

void EVENT_USB_Device_ConfigurationChanged(void) {

    uint8_t i;
    for (i = 0; i < sizeof(endpoints) / sizeof(*endpoints) && endpoints[i].number; ++i) {
        if(endpoints[i].type == EP_TYPE_INTERRUPT) {
            Endpoint_ConfigureEndpoint(endpoints[i].number, endpoints[i].type, endpoints[i].size, 1);
        }
        //TODO MLA: other endpoint types
    }
}

uint16_t CALLBACK_USB_GetDescriptor(const uint16_t wValue, const uint8_t wIndex,
        const void** const DescriptorAddress) {

    const uint8_t DescriptorType = (wValue >> 8);
    const uint8_t DescriptorNumber = (wValue & 0xFF);

    uintDescIndex i;
    for (i = 0; i < sizeof(descIndex) / sizeof(*descIndex) && descIndex[i].type; ++i) {
        if(DescriptorType == descIndex[i].type && DescriptorNumber == descIndex[i].number) {
            *DescriptorAddress = descriptors + descIndex[i].offset;
            return descIndex[i].size;
        }
    }

    return 0;
}

void EVENT_USB_Device_ControlRequest(void) {

    //TODO MLA: handle interface requests here
}

void EVENT_USB_Device_UnhandledControlRequest(void) {

    if(USB_ControlRequest.bmRequestType | REQDIR_DEVICETOHOST) {
        controlReply = 0;
        send_control_header();
        while(!controlReply);
        Endpoint_ClearSETUP();
        Endpoint_Write_Control_Stream_LE(buf, controlReplyLen);
        Endpoint_ClearOUT();
    } else {
        static unsigned char buffer[MAX_CONTROL_TRANSFER_SIZE];
        Endpoint_ClearSETUP();
        Endpoint_Read_Control_Stream_LE(buffer, USB_ControlRequest.wLength);
        Endpoint_ClearIN();
        send_control_header();
        Serial_SendData(buffer, USB_ControlRequest.wLength);
    }
}

void SendNextInput(void) {

    if (input.endpoint) {

        Endpoint_SelectEndpoint(input.endpoint);

        while (!Endpoint_IsINReady()) {}

        Endpoint_Write_Stream_LE(input.data, input.size, NULL);

        Endpoint_ClearIN();

        input.endpoint = 0;

        ack(E_TYPE_IN);
    }
}

void ReceiveNextOutput(void) {

    uint8_t endpoint = 0; //TODO MLA

    if(endpoint) {

        Endpoint_SelectEndpoint(endpoint);

        if (Endpoint_IsOUTReceived()) {

            static struct {
                struct {
                    uint8_t type;
                    uint8_t length;
                } header;
                uint8_t buffer[MAX_EP_SIZE];
            } packet = { .header.type = E_TYPE_OUT };

            uint16_t length = 0;

            if (Endpoint_IsReadWriteAllowed()) {
                uint8_t ErrorCode = Endpoint_Read_Stream_LE(packet.buffer, sizeof(packet.buffer), &length);
                if(ErrorCode == ENDPOINT_RWSTREAM_NoError) {
                    length = sizeof(packet.buffer);
                }
            }

            Endpoint_ClearOUT();

            if(length) {
                packet.header.length = length & 0xFF;
                Serial_SendData(&packet, sizeof(packet.header) + packet.header.length);
            }
        }
    }
}

void ENDPOINT_Task(void) {

    if (USB_DeviceState != DEVICE_STATE_Configured) {
        return;
    }

    SendNextInput();

    ReceiveNextOutput();
}

int main(void) {

    SetupHardware();

    for (;;) {
        ENDPOINT_Task();
        USB_USBTask();
    }
}
