#ifndef CPU_H
#define CPU_H

#include "types.h"

// Forward declaration to avoid circular dependency with Bus
class Bus;

// Strongly-typed flag enum with no accidental integer conversions.
enum class Flag : u8 {
    C = 0x01,   // Carry
    Z = 0x02,   // Zero
    I = 0x04,   // Interrupt Disable
    D = 0x08,   // Decimal (unused on NES)
    B = 0x10,   // Break
    U = 0x20,   // Unused (always 1)
    V = 0x40,   // Overflow
    N = 0x80    // Negative
};

class CPU {
public:
    explicit CPU(Bus& bus);
    void reset();
    void irq();
    void nmi();
    void step();

    [[nodiscard]] u16 getPC()     const noexcept { return pc; }
    [[nodiscard]] u8  getSP()     const noexcept { return sp; }
    [[nodiscard]] u8  getA()      const noexcept { return a; }
    [[nodiscard]] u8  getX()      const noexcept { return x; }
    [[nodiscard]] u8  getY()      const noexcept { return y; }
    [[nodiscard]] u8  getStatus() const noexcept { return status; }
    [[nodiscard]] u64 getCycles() const noexcept { return cycles; }

    [[nodiscard]] bool getFlag(Flag f) const noexcept;

    void setPC(u16 v)    noexcept { pc = v; }
    void setSP(u8 v)     noexcept { sp = v; }
    void setA(u8 v)      noexcept { a = v; }
    void setX(u8 v)      noexcept { x = v; }
    void setY(u8 v)      noexcept { y = v; }
    void setStatus(u8 v) noexcept { status = v; }

private:
    [[nodiscard]] u8  read(u16 addr);
    void              write(u16 addr, u8 data);

    // Reads two consecutive bytes as a little-endian u16 (for interrupt vectors)
    [[nodiscard]] u16 readLE(u16 addr);

    // Stack explicit width at call site avoids the old push/push16 split
    template<typename T> void push(T data);
    template<typename T> [[nodiscard]] T pull();

    void setFlag(Flag f, bool v) noexcept;
    void updateZN(u8 value) noexcept;

    // Addressing modes
    [[nodiscard]] u16 addr_imm();
    [[nodiscard]] u16 addr_zp();
    [[nodiscard]] u16 addr_zpx();
    [[nodiscard]] u16 addr_zpy();
    [[nodiscard]] u16 addr_abs();
    [[nodiscard]] u16 addr_abx();
    [[nodiscard]] u16 addr_aby();
    [[nodiscard]] u16 addr_ind();
    [[nodiscard]] u16 addr_izx();
    [[nodiscard]] u16 addr_izy();

    // Bus reference
    Bus& bus;

    // Instructions
    void op_adc(u16 addr);
    void op_and(u16 addr);
    void op_asl_a();
    void op_asl(u16 addr);
    void op_bcc();
    void op_bcs();
    void op_beq();
    void op_bit(u16 addr);
    void op_bmi();
    void op_bne();
    void op_bpl();
    void op_brk();
    void op_bvc();
    void op_bvs();
    void op_clc();
    void op_cld();
    void op_cli();
    void op_clv();
    void op_cmp(u16 addr);
    void op_cpx(u16 addr);
    void op_cpy(u16 addr);
    void op_dec(u16 addr);
    void op_dex();
    void op_dey();
    void op_eor(u16 addr);
    void op_inc(u16 addr);
    void op_inx();
    void op_iny();
    void op_jmp(u16 addr);
    void op_jsr(u16 addr);
    void op_lda(u16 addr);
    void op_ldx(u16 addr);
    void op_ldy(u16 addr);
    void op_lsr_a();
    void op_lsr(u16 addr);
    void op_nop();
    void op_ora(u16 addr);
    void op_pha();
    void op_php();
    void op_pla();
    void op_plp();
    void op_rol_a();
    void op_rol(u16 addr);
    void op_ror_a();
    void op_ror(u16 addr);
    void op_rti();
    void op_rts();
    void op_sbc(u16 addr);
    void op_sec();
    void op_sed();
    void op_sei();
    void op_sta(u16 addr);
    void op_stx(u16 addr);
    void op_sty(u16 addr);
    void op_tax();
    void op_tay();
    void op_tsx();
    void op_txa();
    void op_txs();
    void op_tya();

    void branch(bool condition);

    // Registers
    u16 pc{};
    u8  sp{};
    u8  a{};
    u8  x{};
    u8  y{};
    u8  status{};

    u64  cycles{};
};

#endif // CPU_H