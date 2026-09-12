// Custom ARM Cortex-M33 hard-fault handler for real-hardware diagnosis.
//
// The Pico SDK's default isr_hardfault (pico_crt0/crt0.S) is a weak symbol
// that does nothing but `bkpt #0` -- with no debug probe attached (our
// case: this board runs standalone over its USB CDC serial console), that
// executes silently and leaves the core halted. No message, no reboot,
// nothing -- indistinguishable from an infinite loop over the serial
// console, which is exactly the symptom a months-long reproducible-freeze
// investigation (see docs/PLAN.md) had been chasing under the assumption
// it *was* an infinite loop, before this file existed.
//
// First version of this handler tried to print immediately and force the
// bytes out over USB CDC by calling tud_task() in a loop (HardFault runs at
// the highest exception priority, so the IRQ stdio_usb normally relies on
// can't preempt us to finish the transfer). That version shipped, was
// tested on hardware, and produced *no output* on a reproducible freeze --
// which, without more information, reads as "no fault happened here
// either." It took attaching a debug probe and breaking in with GDB
// (see debug_notes.md) to find out what was actually going on: a hard
// fault WAS happening every time (consistently in V_DrawPatch, called with
// a corrupted patch pointer from the status bar's ST_drawWidgets -- a
// separate, real bug to chase), our handler WAS being entered, but its own
// tud_task() flush loop was itself deadlocking: calling back into
// TinyUSB's device-stack task function from inside an asynchronous
// exception context re-enters state (queues, spinlock-guarded critical
// sections shared with core1's tuh_task() host stack) that can legitimately
// be mid-update at the exact instant a fault interrupts normal execution,
// and the fault handler's re-entrant call then blocks forever waiting on
// that same lock. So the very mechanism meant to make faults visible was
// itself hanging silently, indistinguishable from "no fault occurred" --
// actively misleading this investigation for a full round of hardware
// testing.
//
// Fixed by not touching USB (or anything else with cross-core/interrupt
// state) from fault context at all: stash the diagnostic registers in the
// watchdog's scratch registers (8 plain 32-bit words at a fixed hardware
// address, scratch[0..3] here -- scratch[4] is reserved by the SDK's own
// watchdog_enable()/watchdog_reboot() bookkeeping, see hardware_watchdog's
// watchdog.c) which -- unlike ordinary SRAM -- are guaranteed to survive a
// watchdog reset, then reboot via watchdog_reboot(), the same
// confirmed-working mechanism I_Quit() already uses. The *next* boot, with
// stdio/USB fully and normally initialized (no exception context, no
// reentrancy risk), checks those scratch registers and prints the stashed
// fault before starting the game -- see report_pending_hard_fault(),
// called from PicoDoom.cpp's main().

#include <cstdio>
#include <cstdint>

#include "pico/stdlib.h"
#include "hardware/watchdog.h"
#include "hardware/structs/watchdog.h"

namespace {

// Cortex-M33 System Control Block fault status/address registers -- same
// addresses/layout as every other Cortex-M with the optional fault
// registers implemented (M3/M4/M33). See the ARMv8-M Architecture
// Reference Manual, or any Cortex-M fault-handler writeup.
volatile uint32_t* const kCFSR = reinterpret_cast<volatile uint32_t*>(0xE000ED28);
volatile uint32_t* const kHFSR = reinterpret_cast<volatile uint32_t*>(0xE000ED2C);
volatile uint32_t* const kMMFAR = reinterpret_cast<volatile uint32_t*>(0xE000ED34);
volatile uint32_t* const kBFAR = reinterpret_cast<volatile uint32_t*>(0xE000ED38);

// Arbitrary marker distinguishing "a fault stashed this" from scratch
// registers' power-on-reset contents (undefined) or a previous boot's
// leftovers after report_pending_hard_fault() has already cleared it.
constexpr uint32_t kFaultMagic = 0xFA17FA17;

} // namespace

extern "C" void hard_fault_handler_c(uint32_t* stacked)
{
    // Exception entry auto-stacks these 8 words, in this order, on
    // whichever stack (MSP/PSP) was active -- the isr_hardfault trampoline
    // below figures out which and passes that pointer here.
    uint32_t pc = stacked[6];
    uint32_t lr = stacked[5];

    // scratch[1]/[2] carry PC/LR (the two addresses worth feeding to
    // arm-none-eabi-addr2line against PicoDoom.elf); scratch[3] carries
    // CFSR, whose bit pattern says *what kind* of fault this was (precise
    // bus fault, usage fault, MPU fault, etc. -- see the ARMv8-M ARM).
    // HFSR/MMFAR/BFAR would be nice too but only 4 scratch words are
    // ours to use (scratch[4] is the SDK's), and PC+LR+CFSR is enough to
    // find the faulting line.
    watchdog_hw->scratch[1] = pc;
    watchdog_hw->scratch[2] = lr;
    watchdog_hw->scratch[3] = *kCFSR;
    watchdog_hw->scratch[0] = kFaultMagic;

    // Same mechanism I_Quit() already uses successfully (doom/i_system.c) --
    // confirmed on hardware to actually reboot the board over a plain USB
    // CDC connection, no debug probe needed.
    watchdog_reboot(0, 0, 10);
    while (true)
        tight_loop_contents(); // watchdog_reboot() only arms the reset; wait for it to fire.
}

// Naked trampoline: the C calling convention needs the stacked-registers
// pointer in r0, but which stack pointer (MSP or PSP) that is depends on
// bit 2 of the EXC_RETURN value the CPU puts in LR on exception entry --
// only asm can read that before the (nonexistent, we're naked) C prologue
// would clobber it. Standard, widely-used ARM Cortex-M hard-fault-handler
// pattern -- see e.g. ARM's own application notes on the topic.
extern "C" __attribute__((naked)) void isr_hardfault(void)
{
    __asm volatile(
        "movs r0, #4        \n"
        "mov  r1, lr        \n"
        "tst  r0, r1        \n"
        "beq  1f            \n"
        "mrs  r0, psp       \n"
        "b    2f            \n"
        "1:                 \n"
        "mrs  r0, msp       \n"
        "2:                 \n"
        "b    hard_fault_handler_c \n"
    );
}

// Called once from PicoDoom.cpp's main(), after stdio_init_all() and the
// USB-CDC-attach wait but before anything else -- normal boot context, so
// printf actually reaches the terminal (unlike from fault context, see
// this file's header comment). No-op, silently, when no fault is pending.
extern "C" void report_pending_hard_fault(void)
{
    if (watchdog_hw->scratch[0] != kFaultMagic)
        return;

    uint32_t pc = watchdog_hw->scratch[1];
    uint32_t lr = watchdog_hw->scratch[2];
    uint32_t cfsr = watchdog_hw->scratch[3];
    watchdog_hw->scratch[0] = 0; // reported -- don't re-report on the next normal boot

    printf("\n*** PREVIOUS BOOT ENDED IN A HARD FAULT ***\n");
    printf("PicoDoom: PC=0x%08lx LR=0x%08lx CFSR=0x%08lx\n",
           (unsigned long)pc, (unsigned long)lr, (unsigned long)cfsr);
    printf("PicoDoom: PC is the faulting instruction's address -- look it up\n"
           "PicoDoom: in PicoDoom.elf.map or via arm-none-eabi-addr2line.\n");
}
