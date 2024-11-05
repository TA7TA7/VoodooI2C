//
//  VoodooI2CControllerDriver.cpp
//  VoodooI2C
//
//  Created by Alexandre on 03/08/2017.
//  Copyright © 2017 Alexandre Daoud. All rights reserved.
//
#include <libkern/OSDebug.h>

#include "VoodooI2CControllerDriver.hpp"
#include "VoodooI2CController.hpp"

#define readRegister(X) nub->readRegister(X)
#define writeRegister(X, Y) nub->writeRegister(X, Y)

#define super IOService
OSDefineMetaClassAndStructors(VoodooI2CControllerDriver, IOService);

void VoodooI2CControllerDriver::free() {
    OSSafeReleaseNULL(device_nubs);

    super::free();
}

constexpr uint64_t KILO = 1000;
constexpr uint64_t MICRO = 1000000;

#define do_div(n, base)                                                        \
  ({                                                                           \
    uint32_t __base = (base);                                                  \
    uint32_t __rem;                                                            \
    __rem = (static_cast<uint64_t>(n)) % __base;                               \
    (n) = (static_cast<uint64_t>(n)) / __base;                                 \
    __rem;                                                                     \
  })

/*
 * Same as above but for u64 dividends. divisor must be a 32-bit
 * number.
 */
#define DIV_ROUND_CLOSEST_ULL(x, divisor)                                      \
  ({                                                                           \
    __decltype(divisor) __d = divisor;                                         \
    unsigned long long _tmp = (x) + (__d) / 2;                                 \
    do_div(_tmp, __d);                                                         \
    _tmp;                                                                      \
  })

static inline SInt64 div_s64_rem(SInt64 dividend, SInt32 divisor,
                                 SInt32 *remainder) {
    *remainder = dividend % divisor;
    return dividend / divisor;
}

static inline SInt64 div_s64(SInt64 dividend, SInt32 divisor) {
    SInt32 remainder;
    return div_s64_rem(dividend, divisor, &remainder);
}

#define DIV_S64_ROUND_CLOSEST(dividend, divisor)                               \
  ({                                                                           \
    SInt64 __x = (dividend);                                                   \
    SInt32 __d = (divisor);                                                    \
    ((__x > 0) == (__d > 0)) ? div_s64((__x + (__d / 2)), __d)                 \
                             : div_s64((__x - (__d / 2)), __d);                \
  })

static UInt32 getClkRateFor(const char *name) {
    if (!strncmp(name, "AMD0010", sizeof("AMD0010"))) {
        return 133000000 / KILO;
    } else if (!strncmp(name, "AMDI0010", sizeof("AMDI0010")) || !strncmp(name, "AMDI0019", sizeof("AMDI0019"))) {
        return 150000000 / KILO;
    } else {
        return 0;
    }
}

IOReturn VoodooI2CControllerDriver::getBusConfig() {
    bool error = false;

    auto param = readRegister(DW_IC_COMP_PARAM_1);
    auto tx_fifo_depth = ((param >> 16) & 0xff) + 1;
    auto rx_fifo_depth = ((param >> 8)  & 0xff) + 1;
    bus_device.transaction_fifo_depth = tx_fifo_depth;
    bus_device.receive_fifo_depth = rx_fifo_depth;

    auto i2c_clk = getClkRateFor(nub->controller->physical_device.name);

    bool is_sunrise_point = !strncmp(nub->controller->physical_device.name, "INT344B", sizeof("INT344B"))
            || !strncmp(nub->controller->physical_device.name, "INT345D", sizeof("INT345D"));

    if (nub->getACPIParams((const char*)"SSCN", &bus_device.acpi_config.ss_hcnt, &bus_device.acpi_config.ss_lcnt, NULL) != kIOReturnSuccess) {
        if (i2c_clk) {
            bus_device.acpi_config.ss_hcnt = (UInt32)DIV_ROUND_CLOSEST_ULL((UInt64)i2c_clk * (4000 + 300), MICRO) - 3;
            bus_device.acpi_config.ss_lcnt = (UInt32)DIV_ROUND_CLOSEST_ULL((UInt64)i2c_clk * (4700 + 300), MICRO) - 1;
        } else {
            bus_device.acpi_config.ss_hcnt = is_sunrise_point ? 0x01B0 : 0x03F2;
            bus_device.acpi_config.ss_lcnt = is_sunrise_point ? 0x01FB : 0x043D;
            error = true;
        }
    }

    if (nub->getACPIParams((const char*)"FMCN", &bus_device.acpi_config.fs_hcnt, &bus_device.acpi_config.fs_lcnt, &bus_device.acpi_config.sda_hold) != kIOReturnSuccess) {
        if (i2c_clk) {
            bus_device.acpi_config.fs_hcnt = (UInt32)DIV_ROUND_CLOSEST_ULL((UInt64)i2c_clk * (600 + 300), MICRO) - 3;
            bus_device.acpi_config.fs_lcnt = (UInt32)DIV_ROUND_CLOSEST_ULL((UInt64)i2c_clk * (1300 + 300), MICRO) - 1;
        } else {
            bus_device.acpi_config.fs_hcnt = is_sunrise_point ? 0x48 : 0x0101;
            bus_device.acpi_config.fs_lcnt = is_sunrise_point ? 0xA0 : 0x012C;
            error = true;
        }
    }

    if (readRegister(DW_IC_COMP_VERSION) >= DW_IC_SDA_HOLD_MIN_VERS) {
        if (i2c_clk) {
            bus_device.acpi_config.sda_hold = (UInt32)DIV_S64_ROUND_CLOSEST((SInt64)i2c_clk * 300, MICRO);
        }

        if (!bus_device.acpi_config.sda_hold) {
            bus_device.acpi_config.sda_hold = readRegister(DW_IC_SDA_HOLD);
        }

        if (!bus_device.acpi_config.sda_hold) {
            bus_device.acpi_config.sda_hold = is_sunrise_point ? 0x1E : 0x62;
        }

        /*
        * Workaround for avoiding TX arbitration lost in case I2C
        * slave pulls SDA down "too quickly" after falling edge of
        * SCL by enabling non-zero SDA RX hold. Specification says it
        * extends incoming SDA low to high transition while SCL is
        * high but it appears to help also above issue.
        */
        if (!(bus_device.acpi_config.sda_hold & DW_IC_SDA_HOLD_RX_MASK)) {
            bus_device.acpi_config.sda_hold |= 1 << DW_IC_SDA_HOLD_RX_SHIFT;
        }
    } else {
        IOLog("%s::%s Warning: hardware too old to adjust SDA hold time\n", getName(), bus_device.name);
    }

    if (error)
        return kIOReturnNotFound;
    else
        return kIOReturnSuccess;
}

IOReturn VoodooI2CControllerDriver::setBusConfigProperties() {
    OSDictionary* properties = OSDictionary::withCapacity(5);
    if (!properties)
        return kIOReturnNoMemory;

    setOSDictionaryNumber(properties, "SS_HCNT",  bus_device.acpi_config.ss_hcnt);
    setOSDictionaryNumber(properties, "SS_LCNT",  bus_device.acpi_config.ss_lcnt);
    setOSDictionaryNumber(properties, "FS_HCNT",  bus_device.acpi_config.fs_hcnt);
    setOSDictionaryNumber(properties, "FS_LCNT",  bus_device.acpi_config.fs_lcnt);
    setOSDictionaryNumber(properties, "SDA_HOLD", bus_device.acpi_config.sda_hold);

    setProperty("BusConfig", properties);
    OSSafeReleaseNULL(properties);

    return kIOReturnSuccess;
}

void VoodooI2CControllerDriver::handleAbortI2C() {
    IOLog("%s::%s I2C Transaction error details\n", getName(), bus_device.name);

    if (bus_device.abort_source & DW_IC_TX_ABRT_7B_ADDR_NOACK)
        IOLog("%s::%s slave address not acknowledged (7bit mode)\n", getName(), bus_device.name);
    if (bus_device.abort_source & DW_IC_TX_ABRT_10ADDR1_NOACK)
        IOLog("%s::%s first address byte not acknowledged (10bit mode)\n", getName(), bus_device.name);
    if (bus_device.abort_source & DW_IC_TX_ABRT_10ADDR2_NOACK)
        IOLog("%s::%s second address byte not acknowledged (10bit mode)\n", getName(), bus_device.name);
    if (bus_device.abort_source & DW_IC_TX_ABRT_TXDATA_NOACK)
        IOLog("%s::%s data not acknowledged\n", getName(), bus_device.name);
    if (bus_device.abort_source & DW_IC_TX_ABRT_GCALL_NOACK)
        IOLog("%s::%s no acknowledgement for a general call\n", getName(), bus_device.name);
    if (bus_device.abort_source & DW_IC_TX_ABRT_GCALL_READ)
        IOLog("%s::%s read after general call\n", getName(), bus_device.name);
    if (bus_device.abort_source & DW_IC_TX_ABRT_SBYTE_ACKDET)
        IOLog("%s::%s start byte acknowledged\n", getName(), bus_device.name);
    if (bus_device.abort_source & DW_IC_TX_ABRT_SBYTE_NORSTRT)
        IOLog("%s::%s trying to send start byte when restart is disabled\n", getName(), bus_device.name);
    if (bus_device.abort_source & DW_IC_TX_ABRT_10B_RD_NORSTRT)
        IOLog("%s::%s trying to read when restart is disabled (10bit mode)\n", getName(), bus_device.name);
    if (bus_device.abort_source & DW_IC_TX_ABRT_MASTER_DIS)
        IOLog("%s::%s trying to use disabled adapter\n", getName(), bus_device.name);
    if (bus_device.abort_source & DW_IC_TX_ARB_LOST)
        IOLog("%s::%s lost arbitration\n", getName(), bus_device.name);

    IOLog("%s::%s I2C Transaction error: 0x%08x - aborting\n", getName(), bus_device.name, bus_device.abort_source);
}

void VoodooI2CControllerDriver::handleInterrupt(OSObject* target, void* refCon, IOService* nubDevice, int source) {
    /* Direct interrupt context. Do NOT block the thread by memory allocation, IOLog, IOLockLock, command_gate->runAction, ... */
    nub->disableInterrupt(0);

    UInt32 status, enabled;

    if (!bus_device.awake) {
        goto exit;
    }

    enabled = readRegister(DW_IC_ENABLE);
    status = readRegister(DW_IC_RAW_INTR_STAT);

    if (!enabled || !(status &~DW_IC_INTR_ACTIVITY) || status == 0xFFFFFFFF)
        goto exit;

    status = readClearInterruptBits();

    if (status & DW_IC_INTR_TX_ABRT) {
        bus_device.command_error |= DW_IC_ERR_TX_ABRT;
        bus_device.status = STATUS_IDLE;
        bus_device.receive_outstanding = 0;

        writeRegister(0, DW_IC_INTR_MASK);
        goto wakeup;
    }

    if (status & DW_IC_INTR_RX_FULL)
        readFromBus();

    if (status & DW_IC_INTR_TX_EMPTY)
        transferMessageToBus();

wakeup:
    if (((status & (DW_IC_INTR_TX_ABRT | DW_IC_INTR_STOP_DET)) || bus_device.message_error) && (bus_device.receive_outstanding == 0)) {
        command_gate->commandWakeup(&bus_device.command_complete);
    } else if (nub->controller->physical_device.access_intr_mask_workaround) {
        /* Workaround to trigger pending interrupt */
        status = readRegister(DW_IC_INTR_MASK);
        writeRegister(0, DW_IC_INTR_MASK);
        writeRegister(status, DW_IC_INTR_MASK);
    }

exit:
    nub->enableInterrupt(0);
}

bool VoodooI2CControllerDriver::init(OSDictionary* properties) {
    if (!super::init(properties))
        return false;

    memset(&bus_device, 0, sizeof(VoodooI2CControllerBusDevice));
    bus_device.awake = true;

    device_nubs = OSArray::withCapacity(1);

    return true;
}

IOReturn VoodooI2CControllerDriver::initialiseBus() {
    if (toggleBusState(kVoodooI2CStateOff) != kIOReturnSuccess)
        return kIOReturnError;

    writeRegister(bus_device.acpi_config.ss_hcnt, DW_IC_SS_SCL_HCNT);
    writeRegister(bus_device.acpi_config.ss_lcnt, DW_IC_SS_SCL_LCNT);
    writeRegister(bus_device.acpi_config.fs_hcnt, DW_IC_FS_SCL_HCNT);
    writeRegister(bus_device.acpi_config.fs_lcnt, DW_IC_FS_SCL_LCNT);
    if (bus_device.acpi_config.sda_hold) {
        writeRegister(bus_device.acpi_config.sda_hold, DW_IC_SDA_HOLD);
    }
    writeRegister(bus_device.transaction_fifo_depth / 2, DW_IC_TX_TL);
    writeRegister(0, DW_IC_RX_TL);
    writeRegister(bus_device.bus_config, DW_IC_CON);

    return kIOReturnSuccess;
}

IOReturn VoodooI2CControllerDriver::prepareTransferI2C(VoodooI2CControllerBusMessage* messages, int* number) {
    AbsoluteTime abstime, deadline;
    IOReturn sleep;

    if (!bus_device.awake || waitBusNotBusyI2C() != kIOReturnSuccess) {
        return kIOReturnBusy;
    }

    bus_device.messages = messages;
    bus_device.message_number = *number;
    bus_device.command_error = 0;
    bus_device.message_write_index = 0;
    bus_device.message_read_index = 0;
    bus_device.message_error = 0;
    bus_device.status = STATUS_IDLE;
    bus_device.abort_source = 0;
    bus_device.receive_outstanding = 0;

    requestTransferI2C();

    /*
     * Sleep timeout to prevent the caller from deadlock :
     *   10ms is required, for example, when reading the HID descriptor for the first time.
     *   Timeout is set to 100ms (10ms x 10 times)
     */
    nanoseconds_to_absolutetime(1000000000, &abstime);
    clock_absolutetime_interval_to_deadline(abstime, &deadline);
    sleep = command_gate->commandSleep(&bus_device.command_complete, deadline, THREAD_INTERRUPTIBLE);

    if (sleep == THREAD_TIMED_OUT) {
        IOLog("%s::%s Timeout waiting for bus to accept transfer request\n", getName(), bus_device.name);
        initialiseBus();
        return kIOReturnTimeout;
    }

    /*
     * We must disable the adapter before returning and signalling the end
     * of the current transfer. Otherwise the hardware might continue
     * generating interrupts which in turn causes a race condition with
     * the following transfer.  Needs some more investigation if the
     * additional interrupts are a hardware bug or this driver doesn't
     * handle them correctly yet.
     */
    toggleBusState(kVoodooI2CStateOff);

    if (bus_device.message_error)
        return kIOReturnError;

    if (!bus_device.command_error)
        return kIOReturnSuccess;

    if (bus_device.command_error == DW_IC_ERR_TX_ABRT) {
        handleAbortI2C();
        return kIOReturnError;
    }

    return kIOReturnNotReady;
}

VoodooI2CControllerDriver* VoodooI2CControllerDriver::probe(IOService* provider, SInt32* score) {
    UInt32 reg;

    if (!super::probe(provider, score)) {
        return NULL;
    }

    nub = OSDynamicCast(VoodooI2CControllerNub, provider);

    if (!nub) {
        IOLog("%s::%s VoodooI2CControllerNub not found\n", getName(), bus_device.name);
        return NULL;
    }

    bus_device.name = nub->name;

    IOLog("%s::%s Probing controller\n", getName(), bus_device.name);

    reg = readRegister(DW_IC_COMP_TYPE);

    if (reg == DW_IC_COMP_TYPE_VALUE) {
        IOLog("%s::%s Found valid Synopsys component, continuing with initialisation\n", getName(), bus_device.name);
    } else {
        IOLog("%s::%s Unknown Synopsys component type: 0x%08x\n", getName(), bus_device.name, reg);
        return NULL;
    }

    return this;
}

IOReturn VoodooI2CControllerDriver::publishNubs() {
    IOLog("%s::%s Publishing device nubs\n", getName(), bus_device.name);

    IOService* child;
    OSIterator* children = nub->controller->physical_device.acpi_device->getChildIterator(gIOACPIPlane);

    if (!children)
        return kIOReturnNoResources;

    while ((child = OSDynamicCast(IOService, children->getNextObject()))) {
        IOLog("%s::%s Found I2C device: %s\n", getName(), bus_device.name, getMatchedName(child));

        VoodooI2CDeviceNub* device_nub = OSTypeAlloc(VoodooI2CDeviceNub);
        OSDictionary* child_properties = child->dictionaryWithProperties();

        if (!device_nub ||
            !device_nub->init(child_properties) ||
            !device_nub->attach(this, child)) {
            IOLog("%s::%s Could not initialise nub for %s\n", getName(), bus_device.name, getMatchedName(child));
        } else if (!device_nub->start(this)) {
            device_nub->detach(this);
            IOLog("%s::%s Could not start nub for %s\n", getName(), bus_device.name, getMatchedName(child));
        } else {
            device_nubs->setObject(device_nub);
        }

        OSSafeReleaseNULL(child_properties);
        OSSafeReleaseNULL(device_nub);
    }
    children->release();

    return kIOReturnSuccess;
}

UInt32 VoodooI2CControllerDriver::readClearInterruptBits() {
    UInt32 stat;
    stat = readRegister(DW_IC_INTR_STAT);

    if (stat & DW_IC_INTR_RX_UNDER)
        readRegister(DW_IC_CLR_RX_UNDER);
    if (stat & DW_IC_INTR_RX_OVER)
        readRegister(DW_IC_CLR_RX_OVER);
    if (stat & DW_IC_INTR_TX_OVER)
        readRegister(DW_IC_CLR_TX_OVER);
    if (stat & DW_IC_INTR_RD_REQ)
        readRegister(DW_IC_CLR_RD_REQ);
    if (stat & DW_IC_INTR_TX_ABRT) {
        bus_device.abort_source = readRegister(DW_IC_TX_ABRT_SOURCE);
        readRegister(DW_IC_CLR_TX_ABRT);
    }
    if (stat & DW_IC_INTR_RX_DONE)
        readRegister(DW_IC_CLR_RX_DONE);
    if (stat & DW_IC_INTR_ACTIVITY)
        readRegister(DW_IC_CLR_ACTIVITY);
    if ((stat & DW_IC_INTR_STOP_DET) && ((bus_device.receive_outstanding == 0) || (stat & DW_IC_INTR_RX_FULL)))
        readRegister(DW_IC_CLR_STOP_DET);
    if (stat & DW_IC_INTR_START_DET)
        readRegister(DW_IC_CLR_START_DET);
    if (stat & DW_IC_INTR_GEN_CALL)
        readRegister(DW_IC_CLR_GEN_CALL);

    return stat;
}

void VoodooI2CControllerDriver::readFromBus() {
    VoodooI2CControllerBusMessage *messages = bus_device.messages;
    int receive_valid;

    for (; bus_device.message_read_index < bus_device.message_number; bus_device.message_read_index++) {
        UInt32 length;
        UInt8 *buffer;

        /** if current message is not a read, skip */
        if (!(messages[bus_device.message_read_index].flags & I2C_M_RD))
            continue;

        /** if a read is not in progress then take the length and the current message in the loop
            else just set the length and buffer to the previous length and buffer */
        if (!(bus_device.status & STATUS_READ_IN_PROGRESS)) {
            length = messages[bus_device.message_read_index].length;
            buffer = messages[bus_device.message_read_index].buffer;
        } else {
            length = bus_device.receive_buffer_length;
            buffer = bus_device.receive_buffer;
        }

        /** check how many items left in receive buffer */
        receive_valid = readRegister(DW_IC_RXFLR);

        /** collect data from receive buffer */
        for (; length > 0 && receive_valid > 0; length--, receive_valid--) {
            *buffer++ = readRegister(DW_IC_DATA_CMD);
            bus_device.receive_outstanding--;
        }

        /** if there are still more messages to read, set status to read in progress and continue
            else remove read in progress status */
        if (length > 0) {
            bus_device.status |= STATUS_READ_IN_PROGRESS;
            bus_device.receive_buffer_length = length;
            bus_device.receive_buffer = buffer;
            return;
        } else {
            bus_device.status &= ~STATUS_READ_IN_PROGRESS;
        }
    }
}

void VoodooI2CControllerDriver::releaseResources() {
    stopI2CInterrupt();

    if (command_gate) {
        work_loop->removeEventSource(command_gate);
    }

    OSSafeReleaseNULL(command_gate);
    OSSafeReleaseNULL(work_loop);

    if (i2c_bus_lock) {
        IOLockFree(i2c_bus_lock);
    }
}

void VoodooI2CControllerDriver::requestTransferI2C() {
    VoodooI2CControllerBusMessage *messages = bus_device.messages;
    UInt32 i2c_configuration, i2c_target = 0, orig;

    if (nub->controller->physical_device.access_intr_mask_workaround) {
        // Linux code works with black magic, on macOS with AMD I2C turning off the adapter
        // and rewriting the bus settings is required
        initialiseBus();
    } else {
        toggleBusState(kVoodooI2CStateOff);
    }

    /* if the slave address is ten bit address, enable 10BITADDR */
    orig = i2c_configuration = readRegister(DW_IC_CON);
    if (messages[bus_device.message_write_index].flags & I2C_M_TEN) {
        i2c_configuration |= DW_IC_CON_10BITADDR_MASTER;
        /*
         * If I2C_DYNAMIC_TAR_UPDATE is set, the 10-bit addressing
         * mode has to be enabled via bit 12 of IC_TAR register.
         * We set it always as I2C_DYNAMIC_TAR_UPDATE can't be
         * detected from registers.
         */
        i2c_target = DW_IC_TAR_10BITADDR_MASTER;
    } else {
        i2c_configuration &= ~DW_IC_CON_10BITADDR_MASTER;
    }

    if (i2c_configuration != orig)
        writeRegister(i2c_configuration, DW_IC_CON);

    /*
     * Set the slave (target) address and enable 10-bit addressing mode
     * if applicable.
     */
    writeRegister(messages[bus_device.message_write_index].address | i2c_target, DW_IC_TAR);

    toggleInterrupts(kVoodooI2CStateOff);

    toggleBusState(kVoodooI2CStateOn);

    /* Dummy read to avoid the register getting stuck on Bay Trail */
    readRegister(DW_IC_ENABLE_STATUS);

    toggleInterrupts(kVoodooI2CStateOn);
}

IOReturn VoodooI2CControllerDriver::setPowerState(unsigned long whichState, IOService *whatDevice) {
    if (whatDevice != this)
        return kIOPMAckImplied;

    // Ensure we are not in the middle of a i2c session.
    IOLockLock(i2c_bus_lock);
    if (whichState == 0) {  // index of kIOPMPowerOff state in VoodooI2CIOPMPowerStates
        if (bus_device.awake) {
            bus_device.awake = false;
            toggleBusState(kVoodooI2CStateOff);
            stopI2CInterrupt();
            IOLog("%s::%s Going to sleep\n", getName(), bus_device.name);
        }
    } else {
        if (!bus_device.awake) {
            toggleBusState(kVoodooI2CStateOn);
            initialiseBus();
            toggleInterrupts(kVoodooI2CStateOff);
            bus_device.awake = true;
            startI2CInterrupt();
            IOLog("%s::%s Woke up\n", getName(), bus_device.name);
        }
    }
    IOLockUnlock(i2c_bus_lock);
    return kIOPMAckImplied;
}

bool VoodooI2CControllerDriver::start(IOService* provider) {
    if (!super::start(provider))
        return false;
    i2c_bus_lock = IOLockAlloc();
    if (!i2c_bus_lock) {
        IOLog("%s::%s Could not allocate I2C bus lock\n", getName(), bus_device.name);
        goto exit;
    }

    work_loop = getWorkLoop();
    if (!work_loop) {
        IOLog("%s::%s Could not get work loop\n", getName(), bus_device.name);
        goto exit;
    }

    work_loop->retain();

    command_gate = IOCommandGate::commandGate(this);
    if (!command_gate || (work_loop->addEventSource(command_gate) != kIOReturnSuccess)) {
        IOLog("%s::%s Could not open command gate\n", getName(), bus_device.name);
        goto exit;
    }

    PMinit();
    nub->joinPMtree(this);
    registerPowerDriver(this, VoodooI2CIOPMPowerStates, kVoodooI2CIOPMNumberPowerStates);

    if (getBusConfig() != kIOReturnSuccess) {
        IOLog("%s::%s Warning: Error getting bus config, using defaults where necessary\n", getName(), bus_device.name);
    } else {
        IOLog("%s::%s Got bus configuration values\n", getName(), bus_device.name);
    }

    setBusConfigProperties();

    bus_device.functionality = I2C_FUNC_I2C | I2C_FUNC_10BIT_ADDR | I2C_FUNC_SMBUS_BYTE | I2C_FUNC_SMBUS_BYTE_DATA | I2C_FUNC_SMBUS_WORD_DATA | I2C_FUNC_SMBUS_I2C_BLOCK;
    bus_device.bus_config = DW_IC_CON_MASTER | DW_IC_CON_SLAVE_DISABLE | DW_IC_CON_RESTART_EN | DW_IC_CON_SPEED_FAST;

    /*
     * On AMD platforms BIOS advertises the bus clear feature
     * and enables the SCL/SDA stuck low. SMU FW does the
     * bus recovery process. Driver should not ignore this BIOS
     * advertisement of bus clear feature.
     */
    if (readRegister(DW_IC_CON) & DW_IC_CON_BUS_CLEAR_CTRL) {
        IOLog("%s::%s Bus clear is enabled", getName(), bus_device.name);
        bus_device.bus_config |= DW_IC_CON_BUS_CLEAR_CTRL;
    }

    if (initialiseBus() != kIOReturnSuccess) {
        IOLog("%s::%s Could not initialise bus\n", getName(), bus_device.name);
        return false;
    }

    toggleInterrupts(kVoodooI2CStateOff);

    if (startI2CInterrupt() != kIOReturnSuccess) {
        goto exit;
    }

    setProperty("VoodooI2CServices Supported", kOSBooleanTrue);

    registerService();

    publishNubs();

    return true;
exit:
    releaseResources();
    return false;
}

void VoodooI2CControllerDriver::stop(IOService* provider) {
    if (device_nubs) {
        while (device_nubs->getCount() > 0) {
            VoodooI2CDeviceNub *device_nub = OSDynamicCast(VoodooI2CDeviceNub, device_nubs->getLastObject());
            device_nub->stop(this);
            device_nub->detach(this);
            device_nubs->removeObject(device_nubs->getCount() - 1);
        }
    }

    OSSafeReleaseNULL(device_nubs);

    if (bus_device.awake) {
        toggleBusState(kVoodooI2CStateOff);
    }

    releaseResources();

    PMstop();

    super::stop(provider);
}

IOReturn VoodooI2CControllerDriver::toggleBusState(VoodooI2CState enabled) {
    int timeout = 1000;

    do {
        writeRegister(enabled, DW_IC_ENABLE);

        if ((readRegister(DW_IC_ENABLE_STATUS) & 1) == enabled) {
            toggleClockGating(enabled);
            return kIOReturnSuccess;
        }

        IODelay(250);
    } while (timeout--);

    IOLog("%s::%s Timed out waiting for bus to change state\n", getName(), bus_device.name);
    return kIOReturnTimeout;
}

inline void VoodooI2CControllerDriver::toggleClockGating(VoodooI2CState enabled) {
    const char *name = nub->controller->physical_device.name;
    if (name[0] == 'A' && name[1] == 'M' && name[2] == 'D') {
        writeRegister(enabled, LPSS_PRIVATE_CLOCK_GATING);
    }
}

void VoodooI2CControllerDriver::toggleInterrupts(VoodooI2CState enabled) {
    if (!enabled) {
        writeRegister(0, DW_IC_INTR_MASK);
    } else {
        readRegister(DW_IC_CLR_INTR);
        writeRegister(DW_IC_INTR_DEFAULT_MASK, DW_IC_INTR_MASK);
    }
}

IOReturn VoodooI2CControllerDriver::transferI2C(VoodooI2CControllerBusMessage* messages, int number) {
    IOLockLock(i2c_bus_lock);
    IOReturn ret = command_gate->runAction(OSMemberFunctionCast(IOCommandGate::Action, this, &VoodooI2CControllerDriver::transferI2CGated), messages, &number);
    IOLockUnlock(i2c_bus_lock);
    return ret;
}

IOReturn VoodooI2CControllerDriver::transferI2CGated(VoodooI2CControllerBusMessage* messages, int* number) {
    IOReturn ret;
    int tries;

    for (ret = 0, tries = 0; tries <= 5; tries++) {
        ret = prepareTransferI2C(messages, number);
        if (ret != kIOReturnNotReady)
            break;
    }

    return ret;
}

void VoodooI2CControllerDriver::transferMessageToBus() {
    VoodooI2CControllerBusMessage *messages = bus_device.messages;
    UInt32 interrupt_mask;
    int transaction_limit, receive_limit;
    UInt32 address = messages[bus_device.message_write_index].address;
    UInt32 buffer_length = bus_device.transaction_buffer_length;
    UInt8 *buffer = bus_device.transaction_buffer;
    bool need_restart = false;

    interrupt_mask = DW_IC_INTR_DEFAULT_MASK;

    for (; bus_device.message_write_index < bus_device.message_number; bus_device.message_write_index++) {
        /*
         * if target address has changed, we need to
         * reprogram the target address in the i2c
         * adapter when we are done with this transfer
         */
        if (messages[bus_device.message_write_index].address != address) {
            bus_device.message_error = -1;
            break;
        }

        if (messages[bus_device.message_write_index].length == 0) {
            bus_device.message_error = -1;
            break;
        }

        if (!(bus_device.status & STATUS_WRITE_IN_PROGRESS)) {
            buffer = messages[bus_device.message_write_index].buffer;
            buffer_length = messages[bus_device.message_write_index].length;

            /* If both IC_EMPTYFIFO_HOLD_MASTER_EN and
             * IC_RESTART_EN are set, we must manually
             * set restart bit between messages.
             */
            if ((bus_device.bus_config & DW_IC_CON_RESTART_EN) && (bus_device.message_write_index > 0)) {
                need_restart = true;
            }
        }

        transaction_limit = bus_device.transaction_fifo_depth - readRegister(DW_IC_TXFLR);
        receive_limit = bus_device.receive_fifo_depth - readRegister(DW_IC_RXFLR);

        while (buffer_length > 0 && transaction_limit > 0 && receive_limit > 0) {
            UInt32 command = 0;

            /*
             * If IC_EMPTYFIFO_HOLD_MASTER_EN is set we must
             * manually set the stop bit. However, it cannot be
             * detected from the registers so we set it always
             * when writing/reading the last byte.
             */

            if (bus_device.message_write_index == bus_device.message_number - 1 && buffer_length == 1) {
                command |= 0x200;
            }

            if (need_restart) {
                command |= 0x400;
                need_restart = false;
            }
            if (messages[bus_device.message_write_index].flags & I2C_M_RD) {
                /* avoid rx buffer overrun */
                if (receive_limit - bus_device.receive_outstanding <= 0) {
                    break;
                }
                writeRegister(command | 0x100, DW_IC_DATA_CMD);
                receive_limit--;
                bus_device.receive_outstanding++;
            } else {
                writeRegister(command | *buffer++, DW_IC_DATA_CMD);
            }
            transaction_limit--; buffer_length--;
        }

        bus_device.transaction_buffer = buffer;
        bus_device.transaction_buffer_length = buffer_length;

        if (buffer_length > 0) {
            bus_device.status |= STATUS_WRITE_IN_PROGRESS;
            break;
        } else {
            bus_device.status &= ~STATUS_WRITE_IN_PROGRESS;
        }
    }

    /*
     * If i2c_msg index search is completed, we don't need TX_EMPTY
     * interrupt any more.
     */
    if (bus_device.message_write_index == bus_device.message_number) {
        interrupt_mask &= ~DW_IC_INTR_TX_EMPTY;
    }

    if (bus_device.message_error) {
        interrupt_mask = 0;
    }

    writeRegister(interrupt_mask, DW_IC_INTR_MASK);
}

IOReturn VoodooI2CControllerDriver::waitBusNotBusyI2C() {
    int timeout = TIMEOUT * 150, firstDelay = 100;

    while (readRegister(DW_IC_STATUS) & DW_IC_STATUS_ACTIVITY) {
        if (timeout <= 0) {
            IOLog("%s::%s Warning: Timeout waiting for bus not to be busy\n", getName(), bus_device.name);
            return kIOReturnBusy;
        }
        timeout--;
        if (firstDelay-- >= 0)
            IODelay(100);
        else
            IOSleep(1);
    }

    return kIOReturnSuccess;
}

IOReturn VoodooI2CControllerDriver::startI2CInterrupt() {
    if (is_interrupt_registered) {
        return kIOReturnStillOpen;
    }
    IOReturn ret = nub->registerInterrupt(0, this, OSMemberFunctionCast(IOInterruptAction, this, &VoodooI2CControllerDriver::handleInterrupt), 0);
    if (ret == kIOReturnSuccess) {
        nub->enableInterrupt(0);
        is_interrupt_registered = true;
    } else {
        IOLog("%s::%s::Could not register I2C interrupt\n", getName(), bus_device.name);
    }
    return ret;
}

void VoodooI2CControllerDriver::stopI2CInterrupt() {
    if (is_interrupt_registered) {
        nub->disableInterrupt(0);
        nub->unregisterInterrupt(0);
        is_interrupt_registered = false;
    }
}
