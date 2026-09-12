/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#include "arch/x86_64/idt.h"
#include "printf.h"
#include "arch/x86_64/pic.h"
#include "arch/x86_64/apic.h"
#include "proc/sched.h"
#include "arch/x86_64/cpu.h"

extern void vmm_shootdown_ipi(void);
#define IPI_TLB_SHOOTDOWN 240

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static struct idt_entry idt[256];

extern void idt_load(struct idt_ptr *ptr);

#define ISR(n) extern void isr##n(void);
ISR(0)  ISR(1)  ISR(2)  ISR(3)  ISR(4)  ISR(5)  ISR(6)  ISR(7)
ISR(8)  ISR(9)  ISR(10) ISR(11) ISR(12) ISR(13) ISR(14) ISR(15)
ISR(16) ISR(17) ISR(18) ISR(19) ISR(20) ISR(21) ISR(22) ISR(23)
ISR(24) ISR(25) ISR(26) ISR(27) ISR(28) ISR(29) ISR(30) ISR(31)
ISR(32) ISR(33) ISR(34) ISR(35) ISR(36) ISR(37) ISR(38) ISR(39)
ISR(40) ISR(41) ISR(42) ISR(43) ISR(44) ISR(45) ISR(46) ISR(47)
ISR(240) ISR(255)
#undef ISR

#define NUM_STUBS 48
static void (*const isr_stubs[NUM_STUBS])(void) = {
    isr0,  isr1,  isr2,  isr3,  isr4,  isr5,  isr6,  isr7,
    isr8,  isr9,  isr10, isr11, isr12, isr13, isr14, isr15,
    isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23,
    isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31,
    isr32, isr33, isr34, isr35, isr36, isr37, isr38, isr39,
    isr40, isr41, isr42, isr43, isr44, isr45, isr46, isr47,
};

static irq_handler_t irq_handlers[16];
static int use_apic;

static const char *const exception_names[32] = {
    "Divide Error", "Debug", "NMI", "Breakpoint",
    "Overflow", "Bound Range Exceeded", "Invalid Opcode", "Device Not Available",
    "Double Fault", "Coprocessor Segment Overrun", "Invalid TSS", "Segment Not Present",
    "Stack-Segment Fault", "General Protection Fault", "Page Fault", "Reserved",
    "x87 FP Exception", "Alignment Check", "Machine Check", "SIMD FP Exception",
    "Virtualization Exception", "Control Protection Exception", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved",
    "Hypervisor Injection", "VMM Communication", "Security Exception", "Reserved",
};

static void idt_set_gate(uint8_t vec, void (*handler)(void), uint8_t type_attr)
{
    uint64_t addr = (uint64_t)handler;
    idt[vec].offset_low  = addr & 0xFFFF;
    idt[vec].selector    = 0x08;
    idt[vec].ist         = 0;
    idt[vec].type_attr   = type_attr;
    idt[vec].offset_mid  = (addr >> 16) & 0xFFFF;
    idt[vec].offset_high = (addr >> 32) & 0xFFFFFFFF;
    idt[vec].zero        = 0;
}

void idt_init(void)
{
    for (int i = 0; i < NUM_STUBS; i++)
        idt_set_gate((uint8_t)i, isr_stubs[i], 0x8E);   /* present, DPL0, interrupt gate */
    idt_set_gate(APIC_SPURIOUS_VECTOR, isr255, 0x8E);
    idt_set_gate(IPI_TLB_SHOOTDOWN, isr240, 0x8E);
    idt_load_current();
}

void idt_load_current(void)
{
    struct idt_ptr ptr = { sizeof(idt) - 1, (uint64_t)idt };
    idt_load(&ptr);
}

void irq_install(uint8_t irq, irq_handler_t handler)
{
    if (irq < 16)
        irq_handlers[irq] = handler;
}

void irq_use_apic(void)
{
    pic_disable();
    use_apic = 1;
}

void irq_mask(uint8_t irq)
{
    if (use_apic) ioapic_mask_irq(irq); else pic_mask(irq);
}

void irq_unmask(uint8_t irq)
{
    if (use_apic) {
        ioapic_route_irq(irq, IRQ_BASE + irq);
        ioapic_unmask_irq(irq);
    } else {
        pic_unmask(irq);
    }
}

static void handle_irq(struct interrupt_frame *f)
{
    uint8_t irq = (uint8_t)(f->vector - IRQ_BASE);

    if (!use_apic && pic_is_spurious(irq))
        return;
    if (irq_handlers[irq])
        irq_handlers[irq](f);
    else
        kprintf("[unhandled irq %u]\n", irq);

    if (use_apic) lapic_eoi(); else pic_eoi(irq);

    /* EOI is done, so it's safe to switch tasks here if the tick asked for it.
     * The interrupted task resumes through iretq when it's next scheduled. */
    sched_preempt();
}

void isr_handler(struct interrupt_frame *f)
{
    if (f->vector == APIC_SPURIOUS_VECTOR)
        return;                     /* no EOI for spurious LAPIC interrupts */
    if (f->vector == IPI_TLB_SHOOTDOWN) {
        vmm_shootdown_ipi();
        lapic_eoi();
        return;
    }
    if (f->vector >= IRQ_BASE && f->vector < IRQ_BASE + 16) {
        handle_irq(f);
        return;
    }

    const char *name = f->vector < 32 ? exception_names[f->vector] : "Unknown";

    if (f->vector == 3) {
        kprintf("  [breakpoint] rip=%p rsp=%p\n", (void *)f->rip, (void *)f->rsp);
        return;
    }

    if (f->cs & 3) {
        /* A user process faulted: kill it, the kernel is fine. */
        uint64_t cr2 = 0;
        if (f->vector == 14)
            __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        kprintf("[%s (pid %u) killed: %s at rip=%lx, addr=%lx, err=%lx]\n"
                "  rsp=%lx rdi=%lx rsi=%lx rdx=%lx rcx=%lx rax=%lx\n",
                task_current()->name, task_current()->id, name, f->rip, cr2, f->error_code,
                f->rsp, f->rdi, f->rsi, f->rdx, f->rcx, f->rax);
        task_current()->killed_sig = f->vector == 14 ? 11 : f->vector == 6 ? 4 : f->vector == 0 ? 8 : 7;
        task_exit_code(128 + task_current()->killed_sig);
    }

    kprintf("\n*** EXCEPTION %lu: %s (error code %lx) ***\n", f->vector, name, f->error_code);
    kprintf("rip=%016lx cs=%04lx rflags=%016lx rsp=%016lx ss=%04lx\n",
            f->rip, f->cs, f->rflags, f->rsp, f->ss);
    kprintf("rax=%016lx rbx=%016lx rcx=%016lx rdx=%016lx\n", f->rax, f->rbx, f->rcx, f->rdx);
    kprintf("rsi=%016lx rdi=%016lx rbp=%016lx\n", f->rsi, f->rdi, f->rbp);
    kprintf("r8 =%016lx r9 =%016lx r10=%016lx r11=%016lx\n", f->r8, f->r9, f->r10, f->r11);
    kprintf("r12=%016lx r13=%016lx r14=%016lx r15=%016lx\n", f->r12, f->r13, f->r14, f->r15);

    if (f->vector == 14) {
        uint64_t cr2;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        kprintf("cr2=%016lx\n", cr2);
    }

    kprintf("cpu %u, task %s; system halted.\n", this_cpu()->index, task_current()->name);
    for (;;)
        __asm__ volatile("cli; hlt");
}
