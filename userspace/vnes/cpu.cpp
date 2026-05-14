#include "cpu.h"
#include "bus.h"

// ---------------------------------------------------------------------------
// Helper: strip the enum class wrapper for bitwise flag operations
// ---------------------------------------------------------------------------
static constexpr u8 asBit(Flag f) noexcept
{
    return static_cast<u8>(f);
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------
CPU::CPU(Bus& b)
    : bus(b), pc{ 0 }, sp{ 0 }, a{ 0 }, x{ 0 }, y{ 0 }, status{ 0 }, cycles{ 0 }
{
}

// ---------------------------------------------------------------------------
// Reset / IRQ / NMI
// ---------------------------------------------------------------------------
void CPU::reset()
{
    pc = readLE(0xFFFC);
    a = x = y = 0;
    sp = 0xFD;
    status = asBit(Flag::U) | asBit(Flag::I);
    cycles = 7;
}

void CPU::irq()
{
    if (getFlag(Flag::I)) return;

    push<u16>(pc);
    setFlag(Flag::B, false);
    setFlag(Flag::U, true);
    setFlag(Flag::I, true);
    push<u8>(status);
    pc = readLE(0xFFFE);
    cycles += 7;
}

void CPU::nmi()
{
    push<u16>(pc);
    setFlag(Flag::B, false);
    setFlag(Flag::U, true);
    setFlag(Flag::I, true);
    push<u8>(status);
    pc = readLE(0xFFFA);
    cycles += 7;
}

// ---------------------------------------------------------------------------
// Memory
// ---------------------------------------------------------------------------
u8 CPU::read(u16 addr)
{
    return bus.read(addr);
}

void CPU::write(u16 addr, u8 data)
{
    bus.write(addr, data);
}

// Little-endian 16-bit read — used only for fixed interrupt vectors
// where the address doesn't need to advance through pc
u16 CPU::readLE(u16 addr)
{
    const u16 lo = read(addr);
    const u16 hi = read(static_cast<u16>(addr + 1u));
    return static_cast<u16>((hi << 8) | lo);
}

// ---------------------------------------------------------------------------
// Stack
// Template specialisations keep the call sites explicit:
//   push<u8>(status)  push<u16>(pc)  pull<u8>()  pull<u16>()
// ---------------------------------------------------------------------------
template<typename T>
void CPU::push(T data)
{
    static_assert(is_same_v<T, u8> || is_same_v<T, u16>);

    if constexpr (is_same_v<T, u8>) {
        write(static_cast<u16>(0x0100u + sp), data);
        --sp;
    }
    else { // u16
        // push high then low (6502 pushes high byte first)
        push(static_cast<u8>(data >> 8));
        push(static_cast<u8>(data & 0xFF));
    }
}

template<typename T>
T CPU::pull()
{
    static_assert(is_same_v<T, u8> || is_same_v<T, u16>);

    if constexpr (is_same_v<T, u8>) {
        ++sp;
        return read(static_cast<u16>(0x0100u + sp));
    }
    else { // u16
        const u16 lo = pull<u8>();
        const u16 hi = pull<u8>();
        return static_cast<u16>((hi << 8) | lo);
    }
}

// ---------------------------------------------------------------------------
// Flags
// ---------------------------------------------------------------------------
void CPU::setFlag(Flag f, bool v) noexcept
{
    if (v) status |= asBit(f);
    else   status &= ~asBit(f);
}

bool CPU::getFlag(Flag f) const noexcept
{
    return (status & asBit(f)) != 0;
}

void CPU::updateZN(u8 value) noexcept
{
    setFlag(Flag::Z, value == 0);
    setFlag(Flag::N, value & 0x80);
}

// ---------------------------------------------------------------------------
// Addressing modes
// ---------------------------------------------------------------------------
u16 CPU::addr_imm() { return pc++; }
u16 CPU::addr_zp() { return read(pc++); }   // zero page is always 0x00xx

u16 CPU::addr_zpx()
{
    ++cycles;
    return static_cast<u8>(read(pc++) + x);  // wraps within zero page
}

u16 CPU::addr_zpy()
{
    ++cycles;
    return static_cast<u8>(read(pc++) + y);
}

u16 CPU::addr_abs()
{
    const u16 lo = read(pc++);
    const u16 hi = read(pc++);
    return static_cast<u16>((hi << 8) | lo);
}

u16 CPU::addr_abx()
{
    const u16 lo = read(pc++);
    const u16 hi = read(pc++);
    const u16 base = static_cast<u16>((hi << 8) | lo);
    const u16 addr = static_cast<u16>(base + x);
    if ((addr & 0xFF00) != (base & 0xFF00)) ++cycles;  // page cross
    return addr;
}

u16 CPU::addr_aby()
{
    const u16 lo = read(pc++);
    const u16 hi = read(pc++);
    const u16 base = static_cast<u16>((hi << 8) | lo);
    const u16 addr = static_cast<u16>(base + y);
    if ((addr & 0xFF00) != (base & 0xFF00)) ++cycles;
    return addr;
}

u16 CPU::addr_ind()
{
    const u16 plo = read(pc++);
    const u16 phi = read(pc++);
    const u16 ptr = static_cast<u16>((phi << 8) | plo);

    // 6502 hardware bug: page boundary crossed within the pointer read
    const u16 next = (plo == 0xFF) ? (ptr & 0xFF00) : (ptr + 1u);
    return static_cast<u16>((read(next) << 8) | read(ptr));
}

u16 CPU::addr_izx()
{
    const u8 ptr = static_cast<u8>(read(pc++) + x);
    ++cycles;
    const u16 lo = read(static_cast<u8>(ptr));
    const u16 hi = read(static_cast<u8>(ptr + 1u));
    return static_cast<u16>((hi << 8) | lo);
}

u16 CPU::addr_izy()
{
    const u8  ptr = read(pc++);
    const u16 lo = read(ptr);
    const u16 hi = read(static_cast<u8>(ptr + 1u));
    const u16 base = static_cast<u16>((hi << 8) | lo);
    const u16 addr = static_cast<u16>(base + y);
    if ((addr & 0xFF00) != (base & 0xFF00)) ++cycles;
    return addr;
}

// ---------------------------------------------------------------------------
// Branch helper
// std::bit_cast is the correct C++20 way to reinterpret a u8 as s8
// ---------------------------------------------------------------------------
void CPU::branch(bool condition)
{
    const s8 offset = static_cast<s8>(read(pc++));
    if (condition) {
        ++cycles;
        const u16 dst = static_cast<u16>(pc + offset);
        if ((dst & 0xFF00) != (pc & 0xFF00)) ++cycles;  // page cross
        pc = dst;
    }
}

// ---------------------------------------------------------------------------
// Instructions
// ---------------------------------------------------------------------------
void CPU::op_adc(u16 addr)
{
    const u8  m = read(addr);
    const u16 sum = static_cast<u16>(a + m + (getFlag(Flag::C) ? 1u : 0u));
    setFlag(Flag::C, sum > 0xFF);
    setFlag(Flag::V, (~(a ^ m) & (a ^ sum)) & 0x80);
    a = static_cast<u8>(sum);
    updateZN(a);
}

void CPU::op_and(u16 addr) { a &= read(addr); updateZN(a); }

void CPU::op_asl_a()
{
    setFlag(Flag::C, a & 0x80);
    a <<= 1;
    updateZN(a);
}

void CPU::op_asl(u16 addr)
{
    u8 m = read(addr);
    setFlag(Flag::C, m & 0x80);
    m <<= 1;
    write(addr, m);
    updateZN(m);
    ++cycles;
}

void CPU::op_bcc() { branch(!getFlag(Flag::C)); }
void CPU::op_bcs() { branch(getFlag(Flag::C)); }
void CPU::op_beq() { branch(getFlag(Flag::Z)); }
void CPU::op_bmi() { branch(getFlag(Flag::N)); }
void CPU::op_bne() { branch(!getFlag(Flag::Z)); }
void CPU::op_bpl() { branch(!getFlag(Flag::N)); }
void CPU::op_bvc() { branch(!getFlag(Flag::V)); }
void CPU::op_bvs() { branch(getFlag(Flag::V)); }

void CPU::op_bit(u16 addr)
{
    const u8 m = read(addr);
    setFlag(Flag::Z, (a & m) == 0);
    setFlag(Flag::V, m & 0x40);
    setFlag(Flag::N, m & 0x80);
}

void CPU::op_brk()
{
    ++pc;
    push<u16>(pc);
    setFlag(Flag::B, true);
    setFlag(Flag::U, true);
    push<u8>(status);
    setFlag(Flag::I, true);
    pc = readLE(0xFFFE);
}

void CPU::op_clc() { setFlag(Flag::C, false); }
void CPU::op_cld() { setFlag(Flag::D, false); }
void CPU::op_cli() { setFlag(Flag::I, false); }
void CPU::op_clv() { setFlag(Flag::V, false); }

void CPU::op_cmp(u16 addr) { const u8 m = read(addr); setFlag(Flag::C, a >= m); updateZN(static_cast<u8>(a - m)); }
void CPU::op_cpx(u16 addr) { const u8 m = read(addr); setFlag(Flag::C, x >= m); updateZN(static_cast<u8>(x - m)); }
void CPU::op_cpy(u16 addr) { const u8 m = read(addr); setFlag(Flag::C, y >= m); updateZN(static_cast<u8>(y - m)); }

void CPU::op_dec(u16 addr)
{
    const u8 m = static_cast<u8>(read(addr) - 1u);
    write(addr, m);
    updateZN(m);
    ++cycles;
}

void CPU::op_dex() { --x; updateZN(x); }
void CPU::op_dey() { --y; updateZN(y); }
void CPU::op_eor(u16 addr) { a ^= read(addr); updateZN(a); }
void CPU::op_inc(u16 addr)
{
    const u8 m = static_cast<u8>(read(addr) + 1u);
    write(addr, m);
    updateZN(m);
    ++cycles;
}

void CPU::op_inx() { ++x; updateZN(x); }
void CPU::op_iny() { ++y; updateZN(y); }
void CPU::op_jmp(u16 addr) { pc = addr; }
void CPU::op_jsr(u16 addr)
{
    push<u16>(static_cast<u16>(pc - 1u));
    pc = addr;
}

void CPU::op_lda(u16 addr) { a = read(addr); updateZN(a); }
void CPU::op_ldx(u16 addr) { x = read(addr); updateZN(x); }
void CPU::op_ldy(u16 addr) { y = read(addr); updateZN(y); }
void CPU::op_lsr_a()
{
    setFlag(Flag::C, a & 0x01);
    a >>= 1;
    updateZN(a);
}

void CPU::op_lsr(u16 addr)
{
    u8 m = read(addr);
    setFlag(Flag::C, m & 0x01);
    m >>= 1;
    write(addr, m);
    updateZN(m);
    ++cycles;
}

void CPU::op_nop() {}
void CPU::op_ora(u16 addr) { a |= read(addr); updateZN(a); }
void CPU::op_pha() { push<u8>(a); }
void CPU::op_php() { push<u8>(status | asBit(Flag::B) | asBit(Flag::U)); }
void CPU::op_pla() { a = pull<u8>(); updateZN(a); ++cycles; }
void CPU::op_plp()
{
    status = pull<u8>();
    setFlag(Flag::U, true);
    setFlag(Flag::B, false);
    ++cycles;
}

void CPU::op_rol_a()
{
    const u8 carry = getFlag(Flag::C) ? 1u : 0u;
    setFlag(Flag::C, a & 0x80);
    a = static_cast<u8>((a << 1) | carry);
    updateZN(a);
}

void CPU::op_rol(u16 addr)
{
    const u8 carry = getFlag(Flag::C) ? 1u : 0u;
    u8 m = read(addr);
    setFlag(Flag::C, m & 0x80);
    m = static_cast<u8>((m << 1) | carry);
    write(addr, m);
    updateZN(m);
    ++cycles;
}

void CPU::op_ror_a()
{
    const u8 carry = getFlag(Flag::C) ? 0x80u : 0u;
    setFlag(Flag::C, a & 0x01);
    a = static_cast<u8>((a >> 1) | carry);
    updateZN(a);
}

void CPU::op_ror(u16 addr)
{
    const u8 carry = getFlag(Flag::C) ? 0x80u : 0u;
    u8 m = read(addr);
    setFlag(Flag::C, m & 0x01);
    m = static_cast<u8>((m >> 1) | carry);
    write(addr, m);
    updateZN(m);
    ++cycles;
}

void CPU::op_rti()
{
    status = pull<u8>();
    setFlag(Flag::U, true);
    setFlag(Flag::B, false);
    pc = pull<u16>();
}

void CPU::op_rts() { pc = static_cast<u16>(pull<u16>() + 1u); }

void CPU::op_sbc(u16 addr)
{
    const u8  m = read(addr) ^ 0xFFu;   // invert for subtraction via ADC
    const u16 sum = static_cast<u16>(a + m + (getFlag(Flag::C) ? 1u : 0u));
    setFlag(Flag::C, sum > 0xFF);
    setFlag(Flag::V, (~(a ^ m) & (a ^ sum)) & 0x80);
    a = static_cast<u8>(sum);
    updateZN(a);
}

void CPU::op_sec() { setFlag(Flag::C, true); }
void CPU::op_sed() { setFlag(Flag::D, true); }
void CPU::op_sei() { setFlag(Flag::I, true); }

void CPU::op_sta(u16 addr) { write(addr, a); }
void CPU::op_stx(u16 addr) { write(addr, x); }
void CPU::op_sty(u16 addr) { write(addr, y); }

void CPU::op_tax() { x = a;  updateZN(x); }
void CPU::op_tay() { y = a;  updateZN(y); }
void CPU::op_tsx() { x = sp; updateZN(x); }
void CPU::op_txa() { a = x;  updateZN(a); }
void CPU::op_txs() { sp = x; }
void CPU::op_tya() { a = y;  updateZN(a); }

// ---------------------------------------------------------------------------
// Decode and execute one instruction
//
// The switch is intentionally kept rather than replaced with an indirect
// function-pointer table.  With -O2, GCC/Clang convert this to a native
// jump table — identical runtime behaviour, but every case body can be
// fully inlined and the compiler can see across the dispatch boundary.
// An indirect call through a stored pointer defeats both of those.
// ---------------------------------------------------------------------------
void CPU::step()
{
    switch (read(pc++)) {

    // ── ADC ────────────────────────────────────────────────────────────
    case 0x69: cycles += 2; op_adc(addr_imm()); break;
    case 0x65: cycles += 3; op_adc(addr_zp());  break;
    case 0x75: cycles += 4; op_adc(addr_zpx()); break;
    case 0x6D: cycles += 4; op_adc(addr_abs()); break;
    case 0x7D: cycles += 4; op_adc(addr_abx()); break;
    case 0x79: cycles += 4; op_adc(addr_aby()); break;
    case 0x61: cycles += 6; op_adc(addr_izx()); break;
    case 0x71: cycles += 5; op_adc(addr_izy()); break;

    // ── AND ────────────────────────────────────────────────────────────
    case 0x29: cycles += 2; op_and(addr_imm()); break;
    case 0x25: cycles += 3; op_and(addr_zp());  break;
    case 0x35: cycles += 4; op_and(addr_zpx()); break;
    case 0x2D: cycles += 4; op_and(addr_abs()); break;
    case 0x3D: cycles += 4; op_and(addr_abx()); break;
    case 0x39: cycles += 4; op_and(addr_aby()); break;
    case 0x21: cycles += 6; op_and(addr_izx()); break;
    case 0x31: cycles += 5; op_and(addr_izy()); break;

    // ── ASL ────────────────────────────────────────────────────────────
    case 0x0A: cycles += 2; op_asl_a();          break;
    case 0x06: cycles += 5; op_asl(addr_zp());   break;
    case 0x16: cycles += 6; op_asl(addr_zpx());  break;
    case 0x0E: cycles += 6; op_asl(addr_abs());  break;
    case 0x1E: cycles += 7; op_asl(addr_abx());  break;

    // ── Branches ───────────────────────────────────────────────────────
    case 0x90: cycles += 2; op_bcc(); break;
    case 0xB0: cycles += 2; op_bcs(); break;
    case 0xF0: cycles += 2; op_beq(); break;
    case 0x30: cycles += 2; op_bmi(); break;
    case 0xD0: cycles += 2; op_bne(); break;
    case 0x10: cycles += 2; op_bpl(); break;
    case 0x50: cycles += 2; op_bvc(); break;
    case 0x70: cycles += 2; op_bvs(); break;

    // ── BIT ────────────────────────────────────────────────────────────
    case 0x24: cycles += 3; op_bit(addr_zp());  break;
    case 0x2C: cycles += 4; op_bit(addr_abs()); break;

    // ── BRK ────────────────────────────────────────────────────────────
    case 0x00: cycles += 7; op_brk(); break;

    // ── Clear flags ────────────────────────────────────────────────────
    case 0x18: cycles += 2; op_clc(); break;
    case 0xD8: cycles += 2; op_cld(); break;
    case 0x58: cycles += 2; op_cli(); break;
    case 0xB8: cycles += 2; op_clv(); break;

    // ── CMP ────────────────────────────────────────────────────────────
    case 0xC9: cycles += 2; op_cmp(addr_imm()); break;
    case 0xC5: cycles += 3; op_cmp(addr_zp());  break;
    case 0xD5: cycles += 4; op_cmp(addr_zpx()); break;
    case 0xCD: cycles += 4; op_cmp(addr_abs()); break;
    case 0xDD: cycles += 4; op_cmp(addr_abx()); break;
    case 0xD9: cycles += 4; op_cmp(addr_aby()); break;
    case 0xC1: cycles += 6; op_cmp(addr_izx()); break;
    case 0xD1: cycles += 5; op_cmp(addr_izy()); break;

    // ── CPX ────────────────────────────────────────────────────────────
    case 0xE0: cycles += 2; op_cpx(addr_imm()); break;
    case 0xE4: cycles += 3; op_cpx(addr_zp());  break;
    case 0xEC: cycles += 4; op_cpx(addr_abs()); break;

    // ── CPY ────────────────────────────────────────────────────────────
    case 0xC0: cycles += 2; op_cpy(addr_imm()); break;
    case 0xC4: cycles += 3; op_cpy(addr_zp());  break;
    case 0xCC: cycles += 4; op_cpy(addr_abs()); break;

    // ── DEC ────────────────────────────────────────────────────────────
    case 0xC6: cycles += 5; op_dec(addr_zp());  break;
    case 0xD6: cycles += 6; op_dec(addr_zpx()); break;
    case 0xCE: cycles += 6; op_dec(addr_abs()); break;
    case 0xDE: cycles += 7; op_dec(addr_abx()); break;

    // ── DEX / DEY ──────────────────────────────────────────────────────
    case 0xCA: cycles += 2; op_dex(); break;
    case 0x88: cycles += 2; op_dey(); break;

    // ── EOR ────────────────────────────────────────────────────────────
    case 0x49: cycles += 2; op_eor(addr_imm()); break;
    case 0x45: cycles += 3; op_eor(addr_zp());  break;
    case 0x55: cycles += 4; op_eor(addr_zpx()); break;
    case 0x4D: cycles += 4; op_eor(addr_abs()); break;
    case 0x5D: cycles += 4; op_eor(addr_abx()); break;
    case 0x59: cycles += 4; op_eor(addr_aby()); break;
    case 0x41: cycles += 6; op_eor(addr_izx()); break;
    case 0x51: cycles += 5; op_eor(addr_izy()); break;

    // ── INC ────────────────────────────────────────────────────────────
    case 0xE6: cycles += 5; op_inc(addr_zp());  break;
    case 0xF6: cycles += 6; op_inc(addr_zpx()); break;
    case 0xEE: cycles += 6; op_inc(addr_abs()); break;
    case 0xFE: cycles += 7; op_inc(addr_abx()); break;

    // ── INX / INY ──────────────────────────────────────────────────────
    case 0xE8: cycles += 2; op_inx(); break;
    case 0xC8: cycles += 2; op_iny(); break;

    // ── JMP / JSR ──────────────────────────────────────────────────────
    case 0x4C: cycles += 3; op_jmp(addr_abs()); break;
    case 0x6C: cycles += 5; op_jmp(addr_ind()); break;
    case 0x20: cycles += 6; op_jsr(addr_abs()); break;

    // ── LDA ────────────────────────────────────────────────────────────
    case 0xA9: cycles += 2; op_lda(addr_imm()); break;
    case 0xA5: cycles += 3; op_lda(addr_zp());  break;
    case 0xB5: cycles += 4; op_lda(addr_zpx()); break;
    case 0xAD: cycles += 4; op_lda(addr_abs()); break;
    case 0xBD: cycles += 4; op_lda(addr_abx()); break;
    case 0xB9: cycles += 4; op_lda(addr_aby()); break;
    case 0xA1: cycles += 6; op_lda(addr_izx()); break;
    case 0xB1: cycles += 5; op_lda(addr_izy()); break;

    // ── LDX ────────────────────────────────────────────────────────────
    case 0xA2: cycles += 2; op_ldx(addr_imm()); break;
    case 0xA6: cycles += 3; op_ldx(addr_zp());  break;
    case 0xB6: cycles += 4; op_ldx(addr_zpy()); break;
    case 0xAE: cycles += 4; op_ldx(addr_abs()); break;
    case 0xBE: cycles += 4; op_ldx(addr_aby()); break;

    // ── LDY ────────────────────────────────────────────────────────────
    case 0xA0: cycles += 2; op_ldy(addr_imm()); break;
    case 0xA4: cycles += 3; op_ldy(addr_zp());  break;
    case 0xB4: cycles += 4; op_ldy(addr_zpx()); break;
    case 0xAC: cycles += 4; op_ldy(addr_abs()); break;
    case 0xBC: cycles += 4; op_ldy(addr_abx()); break;

    // ── LSR ────────────────────────────────────────────────────────────
    case 0x4A: cycles += 2; op_lsr_a();          break;
    case 0x46: cycles += 5; op_lsr(addr_zp());   break;
    case 0x56: cycles += 6; op_lsr(addr_zpx());  break;
    case 0x4E: cycles += 6; op_lsr(addr_abs());  break;
    case 0x5E: cycles += 7; op_lsr(addr_abx());  break;

    // ── NOP (official + common unofficial) ─────────────────────────────
    case 0xEA:
    case 0x1A: case 0x3A: case 0x5A:
    case 0x7A: case 0xDA: case 0xFA:
        cycles += 2; op_nop(); break;

    // ── ORA ────────────────────────────────────────────────────────────
    case 0x09: cycles += 2; op_ora(addr_imm()); break;
    case 0x05: cycles += 3; op_ora(addr_zp());  break;
    case 0x15: cycles += 4; op_ora(addr_zpx()); break;
    case 0x0D: cycles += 4; op_ora(addr_abs()); break;
    case 0x1D: cycles += 4; op_ora(addr_abx()); break;
    case 0x19: cycles += 4; op_ora(addr_aby()); break;
    case 0x01: cycles += 6; op_ora(addr_izx()); break;
    case 0x11: cycles += 5; op_ora(addr_izy()); break;

    // ── Stack ──────────────────────────────────────────────────────────
    case 0x48: cycles += 3; op_pha(); break;
    case 0x08: cycles += 3; op_php(); break;
    case 0x68: cycles += 4; op_pla(); break;
    case 0x28: cycles += 4; op_plp(); break;

    // ── ROL ────────────────────────────────────────────────────────────
    case 0x2A: cycles += 2; op_rol_a();          break;
    case 0x26: cycles += 5; op_rol(addr_zp());   break;
    case 0x36: cycles += 6; op_rol(addr_zpx());  break;
    case 0x2E: cycles += 6; op_rol(addr_abs());  break;
    case 0x3E: cycles += 7; op_rol(addr_abx());  break;

    // ── ROR ────────────────────────────────────────────────────────────
    case 0x6A: cycles += 2; op_ror_a();          break;
    case 0x66: cycles += 5; op_ror(addr_zp());   break;
    case 0x76: cycles += 6; op_ror(addr_zpx());  break;
    case 0x6E: cycles += 6; op_ror(addr_abs());  break;
    case 0x7E: cycles += 7; op_ror(addr_abx());  break;

    // ── RTI / RTS ──────────────────────────────────────────────────────
    case 0x40: cycles += 6; op_rti(); break;
    case 0x60: cycles += 6; op_rts(); break;

    // ── SBC ────────────────────────────────────────────────────────────
    case 0xE9: cycles += 2; op_sbc(addr_imm()); break;
    case 0xE5: cycles += 3; op_sbc(addr_zp());  break;
    case 0xF5: cycles += 4; op_sbc(addr_zpx()); break;
    case 0xED: cycles += 4; op_sbc(addr_abs()); break;
    case 0xFD: cycles += 4; op_sbc(addr_abx()); break;
    case 0xF9: cycles += 4; op_sbc(addr_aby()); break;
    case 0xE1: cycles += 6; op_sbc(addr_izx()); break;
    case 0xF1: cycles += 5; op_sbc(addr_izy()); break;

    // ── Set flags ──────────────────────────────────────────────────────
    case 0x38: cycles += 2; op_sec(); break;
    case 0xF8: cycles += 2; op_sed(); break;
    case 0x78: cycles += 2; op_sei(); break;

    // ── STA ────────────────────────────────────────────────────────────
    case 0x85: cycles += 3; op_sta(addr_zp());  break;
    case 0x95: cycles += 4; op_sta(addr_zpx()); break;
    case 0x8D: cycles += 4; op_sta(addr_abs()); break;
    case 0x9D: cycles += 5; op_sta(addr_abx()); break;
    case 0x99: cycles += 5; op_sta(addr_aby()); break;
    case 0x81: cycles += 6; op_sta(addr_izx()); break;
    case 0x91: cycles += 6; op_sta(addr_izy()); break;

    // ── STX ────────────────────────────────────────────────────────────
    case 0x86: cycles += 3; op_stx(addr_zp());  break;
    case 0x96: cycles += 4; op_stx(addr_zpy()); break;
    case 0x8E: cycles += 4; op_stx(addr_abs()); break;

    // ── STY ────────────────────────────────────────────────────────────
    case 0x84: cycles += 3; op_sty(addr_zp());  break;
    case 0x94: cycles += 4; op_sty(addr_zpx()); break;
    case 0x8C: cycles += 4; op_sty(addr_abs()); break;

    // ── Transfer ───────────────────────────────────────────────────────
    case 0xAA: cycles += 2; op_tax(); break;
    case 0xA8: cycles += 2; op_tay(); break;
    case 0xBA: cycles += 2; op_tsx(); break;
    case 0x8A: cycles += 2; op_txa(); break;
    case 0x9A: cycles += 2; op_txs(); break;
    case 0x98: cycles += 2; op_tya(); break;

    // ── Unknown opcode — treat as 2-cycle NOP ──────────────────────────
    default: cycles += 2; break;
    }
}