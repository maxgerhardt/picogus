#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/pio.h"
#include "hardware/irq.h"
#include "hardware/regs/vreg_and_chip_reset.h"
#include "hardware/vreg.h"
#include "hardware/clocks.h"

#include "pico_reflash.h"

#ifdef PSRAM
#include "psram_spi.h"
psram_spi_inst_t psram_spi;
psram_spi_inst_t* async_spi_inst;
#endif
#include "isa_io.pio.h"

#ifdef ASYNC_UART
#include "stdio_async_uart.h"
#endif 
// UART_TX_PIN is defined in isa_io.pio.h
#define UART_RX_PIN (-1)
#define UART_ID     uart0
#define BAUD_RATE   115200

#ifdef SOUND_OPL
#include "opl.h"
static uint16_t basePort = 0x388u;

void play_adlib(void);
extern "C" int OPL_Pico_Init(unsigned int);
extern "C" void OPL_Pico_PortWrite(opl_port_t, unsigned int);
extern "C" unsigned int OPL_Pico_PortRead(opl_port_t);
#endif

#ifdef SOUND_GUS
#include "gus-x.cpp"

#include "isa_dma.h"
dma_inst_t dma_config;
static uint16_t basePort = GUS_DEFAULT_PORT;
static uint16_t gus_port_test = basePort >> 4 | 0x10;
void play_gus(void);
#endif

#ifdef SOUND_MPU
#include "mpu401/export.h"
static uint16_t basePort = 0x330u;
void play_mpu(void);
#endif

#ifdef SOUND_TANDY
#include "square/square.h"
static uint16_t basePort = 0x2c0u;
void play_tandy(void);

#include "cmd_buffers.h"
tandy_buffer_t tandy_buffer = { {0}, 0, 0 };
#endif

#ifdef SOUND_CMS
#include "square/square.h"
static uint16_t basePort = 0x220u;
void play_cms(void);
static uint8_t cms_detect = 0xFF;

#include "cmd_buffers.h"
cms_buffer_t cms_buffer = { {0}, 0, 0 };
#endif

#ifdef USB_JOYSTICK
static uint16_t basePort = 0x201u;
void play_usb(void);
#include "joy_hid/joy.h"
extern "C" joystate_struct_t joystate_struct;
uint8_t joystate_bin;
#include "hardware/pwm.h"
#endif

// PicoGUS control and data ports
// 1D0 chosen as the base port as nothing is listed in Ralf Brown's Port List (http://www.cs.cmu.edu/~ralf/files.html)
#define CONTROL_PORT 0x1D0
#define DATA_PORT_LOW  0x1D1
#define DATA_PORT_HIGH 0x1D2
#define PICOGUS_PROTOCOL_VER 1
static bool control_active = false;
static uint8_t sel_reg = 0;
static uint16_t cur_data = 0;
static uint32_t cur_read = 0;

constexpr uint LED_PIN = 1 << PICO_DEFAULT_LED_PIN;

static uint iow_sm;
static uint ior_sm;
static uint ior_write_sm;

const char* firmware_string = PICO_PROGRAM_NAME " v" PICO_PROGRAM_VERSION_STRING;

uint16_t basePort_tmp;

__force_inline void select_picogus(uint8_t value) {
    // printf("select picogus %x\n", value);
    sel_reg = value;
    switch (sel_reg) {
    case 0x00: // Magic string
    case 0x01: // Protocol version
        break;
    case 0x02: // Firmware string
        cur_read = 0;
        break;
    case 0x03: // Mode (GUS, OPL, MPU, etc...)
        break;
    case 0x04: // Base port
        basePort_tmp = 0;
        break;
#ifdef SOUND_GUS
    case 0x10: // Audio buffer size
    case 0x11: // DMA interval
        break;
#endif
    case 0xFF: // Firmware write mode
        pico_firmware_stop(PICO_FIRMWARE_IDLE);
        break;
    default:
        control_active = false;
        break;
    }
}

__force_inline void write_picogus_low(uint8_t value) {
    switch (sel_reg) {
    case 0x04: // Base port
        basePort_tmp = value;
        break;
    }
}

__force_inline void write_picogus_high(uint8_t value) {
    switch (sel_reg) {
    case 0x04: // Base port
        basePort = (value << 8) | basePort_tmp;
#ifdef SOUND_GUS
        gus_port_test = basePort >> 4 | 0x10;
        // GUS_SetPort(basePort);
#endif
        break;
#ifdef SOUND_GUS
    case 0x10: // Audio buffer size
        // Value is sent by pgusinit as the size - 1, so we need to add 1 back to it
        GUS_SetAudioBuffer(value + 1);
        break;
    case 0x11: // DMA interval
        GUS_SetDMAInterval(value);
        break;
#endif
    case 0xff: // Firmware write
        pico_firmware_write(value);
        break;
    }
}

__force_inline uint8_t read_picogus_low(void) {
    switch (sel_reg) {
    case 0x04: // Base port
#if defined(SOUND_GUS) || defined(SOUND_OPL) || defined(SOUND_MPU) || defined(SOUND_TANDY) || defined(SOUND_CMS)
        return basePort & 0xff;
#else
        return 0xff;
#endif
        break;
    default:
        return 0x0;
    }
}

__force_inline uint8_t read_picogus_high(void) {
    uint8_t ret;
    switch (sel_reg) {
    case 0x00:  // PicoGUS magic string
        return 0xdd;
        break;
    case 0x01:  // PicoGUS magic string
        return PICOGUS_PROTOCOL_VER;
        break;
    case 0x02: // Firmware string
        ret = firmware_string[cur_read++];
        if (ret == 0) { // Null terminated
            cur_read = 0;
        }
        return ret;
        break;
    case 0x03: // Mode (GUS, OPL, MPU, etc...)
#if defined(SOUND_GUS)
        return 0;
#elif defined(SOUND_OPL)
        return 1;
#elif defined(SOUND_MPU)
        return 2;
#elif defined(SOUND_TANDY)
        return 3;
#elif defined(SOUND_CMS)
        return 4;
#else
        return 0xff;
#endif
        break;
    case 0x04: // Base port
#if defined(SOUND_GUS) || defined(SOUND_OPL) || defined(SOUND_MPU) || defined(SOUND_TANDY) || defined(SOUND_CMS)
        return basePort >> 8;
#else
        return 0xff;
#endif
        break;
    case 0xff:
        // Get status of firmware write
        return pico_firmware_getStatus();
        break;
    default:
        return 0xff;
        break;
    }
}

static constexpr uint32_t IO_WAIT = 0xffffffffu;
static constexpr uint32_t IO_END = 0x0u;
// OR with 0x0000ff00 is required to set pindirs in the PIO
static constexpr uint32_t IOR_SET_VALUE = 0x0000ff00u;

__force_inline void handle_iow(void) {
    uint32_t iow_read = pio_sm_get(pio0, iow_sm); //>> 16;
    // printf("%x", iow_read);
    uint16_t port = (iow_read >> 8) & 0x3FF;
    // printf("IOW: %x %x\n", port, iow_read & 0xFF);
#ifdef SOUND_GUS
    if ((port >> 4 | 0x10) == gus_port_test) {
        bool fast_write = false;
        port -= basePort;
        switch (port) {
        case 0x8:
        case 0xb:
        case 0x102:
        case 0x103:
        case 0x104:
            // Fast write, don't set iochrdy by writing 0
            pio_sm_put(pio0, iow_sm, IO_END);
            fast_write = true;
            break;
        default:
            // gpio_xor_mask(LED_PIN);
            // Slow write, set iochrdy by writing non-0
            pio_sm_put(pio0, iow_sm, IO_WAIT);
            break;
        }
        // uint32_t write_begin = time_us_32();
        // __dsb();
        write_gus(port, iow_read & 0xFF);
        // uint32_t write_elapsed = time_us_32() - write_begin;
        // if (write_elapsed > 1) {
        //     printf("long write to port %x, (sel reg %x), took %d us\n", port, gus->selected_register, write_elapsed);
        // }
        if (fast_write) {
            // Fast write - return early as we've already written 0x0u to the PIO
            return;
        }
        // __dsb();
        // printf("GUS IOW: port: %x value: %x\n", port, value);
        // puts("IOW");
    } else // if follows down below
#endif // SOUND_GUS
#ifdef SOUND_OPL
    switch (port - basePort) {
    case 0:
        // Fast write
        pio_sm_put(pio0, iow_sm, IO_END);
        OPL_Pico_PortWrite(OPL_REGISTER_PORT, iow_read & 0xFF);
        // Fast write - return early as we've already written 0x0u to the PIO
        return;
        break;
    case 1:
        pio_sm_put(pio0, iow_sm, IO_WAIT);
        OPL_Pico_PortWrite(OPL_DATA_PORT, iow_read & 0xFF);
        // __dsb();
        break;
    }
#endif // SOUND_OPL
#ifdef SOUND_MPU
    switch (port - basePort) {
    case 0:
        pio_sm_put(pio0, iow_sm, IO_WAIT);
        // printf("MPU IOW: port: %x value: %x\n", port, iow_read & 0xFF);
        MPU401_WriteData(iow_read & 0xFF, true);
        break;
    case 1:
        pio_sm_put(pio0, iow_sm, IO_WAIT);
        MPU401_WriteCommand(iow_read & 0xFF, true);
        // printf("MPU IOW: port: %x value: %x\n", port, iow_read & 0xFF);
        // __dsb();
        break;
    }
#endif // SOUND_MPU
#ifdef SOUND_TANDY
    if (port == basePort) {
        pio_sm_put(pio0, iow_sm, IO_END);
        tandy_buffer.cmds[tandy_buffer.head++] = iow_read & 0xFF;
        return;
    } else // if follows down below
#endif // SOUND_TANDY
#ifdef SOUND_CMS
    switch (port - basePort) {
    // SAA data/address ports
    case 0x0:
    case 0x1:
    case 0x2:
    case 0x3:
        pio_sm_put(pio0, iow_sm, IO_END);
        cms_buffer.cmds[cms_buffer.head++] = {
            port,
            (uint8_t)(iow_read & 0xFF)
        };
        return;
        break;
    // CMS autodetect ports
    case 0x6:
    case 0x7:
        pio_sm_put(pio0, iow_sm, IO_END);
        cms_detect = iow_read & 0xFF;
        return;
        break;
    }
#endif // SOUND_CMS
#ifdef USB_JOYSTICK
    if (port == basePort) {
        pio_sm_put(pio0, iow_sm, IO_END);
        // Set times in # of cycles (affected by clkdiv) for each PWM slice to count up and wrap back to 0
        // TODO better calibrate this
        pwm_set_wrap(0, 2000 + (joystate_struct.joy1_x << 6));
        pwm_set_wrap(1, 2000 + (joystate_struct.joy1_y << 6));
        pwm_set_wrap(2, 2000 + (joystate_struct.joy2_x << 6));
        pwm_set_wrap(3, 2000 + (joystate_struct.joy2_y << 6));
        // Convince PWM to run as one-shot by immediately setting wrap to 0. This will take effect once the wrap
        // times set above hit, so after wrapping the counter value will stay at 0 instead of counting again
        pwm_set_wrap(0, 0);
        pwm_set_wrap(1, 0);
        pwm_set_wrap(2, 0);
        pwm_set_wrap(3, 0);
        return;
    } else // if follows down below
#endif
    // PicoGUS control
    if (port == CONTROL_PORT) {
        pio_sm_put(pio0, iow_sm, IO_WAIT);
        // printf("iow control port: %x %d\n", iow_read & 0xff, control_active);
        if ((iow_read & 0xFF) == 0xCC) {
            // printf("activate ");
            control_active = true;
        } else if (control_active) {
            select_picogus(iow_read & 0xFF);
        }
    } else if (port == DATA_PORT_LOW) {
        pio_sm_put(pio0, iow_sm, IO_END);
        if (control_active) {
            write_picogus_low(iow_read & 0xFF);
        }
        // Fast write - return early as we've already written 0x0u to the PIO
        return;
    } else if (port == DATA_PORT_HIGH) {
        // printf("iow data port: %x\n", iow_read & 0xff);
        pio_sm_put(pio0, iow_sm, IO_WAIT);
        if (control_active) {
            write_picogus_high(iow_read & 0xFF);
        }
    }
    // Fallthrough if no match, or for slow write, reset PIO
    pio_sm_put(pio0, iow_sm, IO_END);
}

__force_inline void handle_ior(void) {
    uint16_t port = pio_sm_get(pio0, ior_sm) & 0x3FF;
    // printf("IOR: %x\n", port);
    //
#if defined(SOUND_GUS)
    if ((port >> 4 | 0x10) == gus_port_test) {
        // Tell PIO to wait for data
        pio_sm_put(pio0, ior_sm, IO_WAIT);
        uint32_t value = read_gus(port - basePort) & 0xff;
        // OR with 0x0000ff00 is required to set pindirs in the PIO
        pio_sm_put(pio0, ior_sm, IOR_SET_VALUE | value);
        // printf("GUS IOR: port: %x value: %x\n", port, value);
        // gpio_xor_mask(LED_PIN);
    } else // if follows down below
#elif defined(SOUND_OPL)
    if (port == basePort) {
        // Tell PIO to wait for data
        pio_sm_put(pio0, ior_sm, IO_WAIT);
        uint32_t value = OPL_Pico_PortRead(OPL_REGISTER_PORT);
        // OR with 0x0000ff00 is required to set pindirs in the PIO
        pio_sm_put(pio0, ior_sm, IOR_SET_VALUE | value);
    } else // if follows down below
#elif defined(SOUND_MPU)
    if (port == basePort) {
        // Tell PIO to wait for data
        pio_sm_put(pio0, ior_sm, IO_WAIT);
        uint32_t value = MPU401_ReadData();
        // printf("MPU IOR: port: %x value: %x\n", port, value);
        // OR with 0x0000ff00 is required to set pindirs in the PIO
        pio_sm_put(pio0, ior_sm, IOR_SET_VALUE | value);
    } else if (port == basePort + 1) {
        // Tell PIO to wait for data
        pio_sm_put(pio0, ior_sm, IO_WAIT);
        uint32_t value = MPU401_ReadStatus();
        // printf("MPU IOR: port: %x value: %x\n", port, value);
        // OR with 0x0000ff00 is required to set pindirs in the PIO
        pio_sm_put(pio0, ior_sm, IOR_SET_VALUE | value);
    } else // if follows down below
#elif defined(SOUND_CMS)
    switch (port - basePort) {
    // CMS autodetect ports
    case 0x4:
        pio_sm_put(pio0, ior_sm, IO_WAIT);
        pio_sm_put(pio0, ior_sm, IOR_SET_VALUE | 0x7F);
        return;
    case 0xa:
    case 0xb:
        pio_sm_put(pio0, ior_sm, IO_WAIT);
        pio_sm_put(pio0, ior_sm, IOR_SET_VALUE | cms_detect);
        return;
    }
#endif // SOUND_CMS
#ifdef USB_JOYSTICK
    if (port == basePort) {
        pio_sm_put(pio0, ior_sm, IO_WAIT);
        uint8_t value =
            // Proportional bits: 1 if counter is still counting, 0 otherwise
            (bool)pwm_get_counter(0) |
            ((bool)pwm_get_counter(1) << 1) |
            ((bool)pwm_get_counter(2) << 2) |
            ((bool)pwm_get_counter(3) << 3) |
            joystate_struct.button_mask;
        pio_sm_put(pio0, ior_sm, IOR_SET_VALUE | value);
    } else // if follows down below
#endif
    if (port == CONTROL_PORT) {
        // Tell PIO to wait for data
        pio_sm_put(pio0, ior_sm, IO_WAIT);
        uint32_t value = sel_reg;
        // OR with 0x0000ff00 is required to set pindirs in the PIO
        pio_sm_put(pio0, ior_sm, IOR_SET_VALUE | value);
    } else if (port == DATA_PORT_LOW) {
        // Tell PIO to wait for data
        pio_sm_put(pio0, ior_sm, IO_WAIT);
        uint32_t value = read_picogus_low();
        // OR with 0x0000ff00 is required to set pindirs in the PIO
        pio_sm_put(pio0, ior_sm, IOR_SET_VALUE | value);
    } else if (port == DATA_PORT_HIGH) {
        pio_sm_put(pio0, ior_sm, IO_WAIT);
        uint32_t value = read_picogus_high();
        pio_sm_put(pio0, ior_sm, IOR_SET_VALUE | value);
    } else {
        // Reset PIO
        pio_sm_put(pio0, ior_sm, IO_END);
    }
}

#ifdef USE_IRQ
void iow_isr(void) {
    /* //printf("ints %x\n", pio0->ints0); */
    handle_iow();
    // pio_interrupt_clear(pio0, pio_intr_sm0_rxnempty_lsb);
    irq_clear(PIO0_IRQ_0);
}
void ior_isr(void) {
    handle_ior();
    // pio_interrupt_clear(pio0, PIO_INTR_SM0_RXNEMPTY_LSB);
    irq_clear(PIO0_IRQ_1);
}
#endif

void err_blink(void) {
    for (;;) {
        gpio_xor_mask(LED_PIN);
        busy_wait_ms(100);
    }
}

#ifndef USE_ALARM
#include "pico_pic.h"
#endif

int main()
{
    // Overclock!
    vreg_set_voltage(VREG_VOLTAGE_1_30);
    // set_sys_clock_khz(266000, true);
    // set_sys_clock_khz(280000, true);
    // set_sys_clock_khz(420000, true);
    set_sys_clock_khz(400000, true);

    // Set clk_peri to use the XOSC
    // clock_configure(clk_peri,
    //                 0,
    //                 CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_XOSC_CLKSRC,
    //                 12 * MHZ,
    //                 12 * MHZ);
    // clock_configure(clk_peri, 0, CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS,
    //         12 * MHZ, 12 * MHZ);

    // stdio_init_all();
#ifdef ASYNC_UART
    stdio_async_uart_init_full(UART_ID, BAUD_RATE, UART_TX_PIN, UART_RX_PIN);
#else
    stdio_init_all();
#endif
    puts(firmware_string);
    io_rw_32 *reset_reason = (io_rw_32 *) (VREG_AND_CHIP_RESET_BASE + VREG_AND_CHIP_RESET_CHIP_RESET_OFFSET);
    if (*reset_reason & VREG_AND_CHIP_RESET_CHIP_RESET_HAD_POR_BITS) {
        puts("I was reset due to power on reset or brownout detection.");
    } else if (*reset_reason & VREG_AND_CHIP_RESET_CHIP_RESET_HAD_RUN_BITS) {
        puts("I was reset due to the RUN pin (either manually or due to ISA RESET signal)");
    } else if(*reset_reason & VREG_AND_CHIP_RESET_CHIP_RESET_HAD_PSM_RESTART_BITS) {
        puts("I was reset due the debug port");
    }

    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);

    gpio_init(IRQ_PIN);
    gpio_set_dir(IRQ_PIN, GPIO_OUT);

#ifdef SOUND_MPU
    puts("Initing MIDI UART...");
    uart_init(UART_ID, 31250);
    uart_set_translate_crlf(UART_ID, false);
    uart_set_format(UART_ID, 8, 1, UART_PARITY_NONE);
    gpio_set_drive_strength(UART_TX_PIN, GPIO_DRIVE_STRENGTH_12MA);
    gpio_set_function(UART_TX_PIN, GPIO_FUNC_UART);
    busy_wait_ms(1000);
    MPU401_Init();
#endif // SOUND_MPU

#ifdef PSRAM_CORE0
#ifdef PSRAM
    puts("Initing PSRAM...");
    psram_spi = psram_spi_init_clkdiv(pio1, -1, 1.6);
#if TEST_PSRAM
    test_psram(&psram_spi);
#endif // TEST_PSRAM
#endif // PSRAM
#endif // PSRAM_CORE0

#ifdef SOUND_OPL
    puts("Creating OPL");
    OPL_Pico_Init(basePort);
    multicore_launch_core1(&play_adlib);
#endif // SOUND_OPL

#ifdef SOUND_GUS
    puts("Creating GUS");
    GUS_OnReset();
    multicore_launch_core1(&play_gus);
#endif // SOUND_GUS

#ifdef SOUND_MPU
    multicore_launch_core1(&play_mpu);
#endif // SOUND_MPU

#ifdef SOUND_TANDY
    puts("Creating tandysound");
    multicore_launch_core1(&play_tandy);
#endif // SOUND_TANDY

#ifdef SOUND_CMS
    puts("Creating CMS");
    multicore_launch_core1(&play_cms);
#endif // SOUND_CMS

#ifdef USB_JOYSTICK
    // Init joystick as centered with no buttons pressed
    joystate_struct = {127, 127, 127, 127, 0xf};
    puts("Config joystick PWM");
    pwm_config pwm_c = pwm_get_default_config();
    // TODO better calibrate this
    pwm_config_set_clkdiv(&pwm_c, 17.6);
    // Start the PWM off constantly looping at 0
    pwm_config_set_wrap(&pwm_c, 0);
    pwm_init(0, &pwm_c, true);
    pwm_init(1, &pwm_c, true);
    pwm_init(2, &pwm_c, true);
    pwm_init(3, &pwm_c, true);
    multicore_launch_core1(&play_usb);
#endif // SOUND_CMS

    for(int i=AD0_PIN; i<(AD0_PIN + 10); ++i) {
        gpio_disable_pulls(i);
    }
    gpio_disable_pulls(IOW_PIN);
    gpio_disable_pulls(IOR_PIN);
    gpio_pull_down(IOCHRDY_PIN);
    gpio_set_dir(IOCHRDY_PIN, GPIO_OUT);

    puts("Enabling bus transceivers...");
    // waggle ADS to set BUSOE latch
    gpio_init(ADS_PIN);
    gpio_set_dir(ADS_PIN, GPIO_OUT);
    gpio_put(ADS_PIN, 1);
    busy_wait_ms(10);
    gpio_put(ADS_PIN, 0);

    puts("Starting ISA bus PIO...");
    // gpio_set_drive_strength(ADS_PIN, GPIO_DRIVE_STRENGTH_12MA);
    gpio_set_slew_rate(ADS_PIN, GPIO_SLEW_RATE_FAST);

    uint iow_offset = pio_add_program(pio0, &iow_program);
    iow_sm = pio_claim_unused_sm(pio0, true);
    printf("iow sm: %u\n", iow_sm);

    uint ior_offset = pio_add_program(pio0, &ior_program);
    ior_sm = pio_claim_unused_sm(pio0, true);
    printf("ior sm: %u\n", ior_sm);

    ior_program_init(pio0, ior_sm, ior_offset);
    iow_program_init(pio0, iow_sm, iow_offset);

#ifdef USE_IRQ
    puts("Enabling IRQ on ISA IOR/IOW events");
    // iow irq
    irq_set_enabled(PIO0_IRQ_0, false);
    pio_set_irq0_source_enabled(pio0, pis_sm0_rx_fifo_not_empty, true);
    irq_set_priority(PIO0_IRQ_0, PICO_HIGHEST_IRQ_PRIORITY);
    irq_set_exclusive_handler(PIO0_IRQ_0, iow_isr);
    irq_set_enabled(PIO0_IRQ_0, true);
    // ior irq
    irq_set_enabled(PIO0_IRQ_1, false);
    pio_set_irq1_source_enabled(pio0, pis_sm1_rx_fifo_not_empty, true);
    irq_set_priority(PIO0_IRQ_1, PICO_HIGHEST_IRQ_PRIORITY);
    irq_set_exclusive_handler(PIO0_IRQ_1, ior_isr);
    irq_set_enabled(PIO0_IRQ_1, true);
#endif

    gpio_xor_mask(LED_PIN);

#ifndef USE_ALARM
    PIC_Init();
#endif

    for (;;) {
#ifndef USE_IRQ
        if (!pio_sm_is_rx_fifo_empty(pio0, iow_sm)) {
            handle_iow();
            // gpio_xor_mask(LED_PIN);
        }

        if (!pio_sm_is_rx_fifo_empty(pio0, ior_sm)) {
            handle_ior();
            // gpio_xor_mask(LED_PIN);
        }
#endif
#ifndef USE_ALARM
        PIC_HandleEvents();
#endif
#ifdef SOUND_MPU
        // send_midi_byte();				// see if we need to send a byte	
#endif
#ifdef POLLING_DMA
        process_dma();
#endif
    }
}
