

void spu_recompiler::UNK(spu_opcode_t op)
{
    fall(op);

    // Unknown/illegal instruction must not fall through to the next instruction,
    // return to the main loop instead (matches the x86 recompiler behavior).
    c->b(label_stop);
    m_pos = -1;
}

void spu_recompiler::STOP(spu_opcode_t op)
{
    fall(op);

    // STOP must return to the main loop (like the x86 recompiler's jmp label_stop),
    // otherwise execution continues into the next instruction.
    if (g_cfg.core.spu_block_size == spu_block_size_type::safe)
    {
        c->b(label_stop);
        m_pos = -1;
    }
}

void spu_recompiler::LNOP(spu_opcode_t op)
{
    // Load NOP
}

void spu_recompiler::SYNC(spu_opcode_t op)
{
    c->dmb(asmjit::Imm(0xBu));
    if (g_cfg.core.spu_block_size == spu_block_size_type::safe)
    {
        c->mov(*addr, asmjit::Imm(m_pos + 4 - m_base));
        c->add(*addr, *pc0, *addr);
        c->and_(*addr, *addr, asmjit::Imm(0x3fffc));
        c->str(addr->w(), SPU_OFF_32(pc));
        c->b(label_stop);
        m_pos = -1;
    }
}

void spu_recompiler::DSYNC(spu_opcode_t op)
{
    c->dmb(asmjit::Imm(0xBu));
}

void spu_recompiler::MFSPR(spu_opcode_t op)
{
    const VLink& vr = VAlloc();
    c->eor(vr.b16(), vr.b16(), vr.b16());
    c->str(vr, SPU_OFF_128(gpr, op.rt));
}

static u32 spu_rdch(spu_thread* _spu, u32 ch)
{
    const s64 result = _spu->get_ch_value(ch);

    if (result < 0 || _spu->state & cpu_flag::again)
    {
        spu_runtime::g_escape(_spu);
    }

    if (_spu->test_stopped())
    {
        _spu->pc += 4;
        spu_runtime::g_escape(_spu);
    }

    return static_cast<u32>(result & 0xffffffff);
}

void spu_recompiler::RDCH(spu_opcode_t op)
{
    using namespace asmjit;

    auto read_channel = [&](u32 ch_off, bool sync = true)
    {
        Label wait = c->newLabel();
        Label again = c->newLabel();
        Label ret = c->newLabel();
        c->add(*qw1, *cpu, Imm(static_cast<int32_t>(ch_off)));
        c->align(AlignMode::kCode, 16);
        c->bind(again);
        c->ldaxr(*qw0, arm::Mem(*qw1));
        c->mov(a64::x14, *qw0); // save original word (value | count<<off), returned as result like x64 (addr holds it)
        c->lsr(qw0->x(), qw0->x(), Imm(spu_channel::off_count));
        c->cbz(qw0->w(), wait);

        after.emplace_back([=, this, pos = m_pos]
                           {
                               c->bind(wait);
                               c->mov(*addr, Imm(pos - m_base));
                               c->add(*addr, *pc0, *addr);
                               c->and_(*addr, *addr, Imm(0x3fffc));
                               c->str(addr->w(), SPU_OFF_32(pc));
                               c->mov(arg1->w(), Imm(+op.ra));
                               c->mov(*arg0, *cpu);
                               c->mov(*qw1, Imm(reinterpret_cast<u64>(&spu_rdch)));
                               c->blr(*qw1);
                               c->b(ret);
                           });

        if (sync)
        {
            // Consume the channel value (write zero) atomically
            c->mov(a64::x12, Imm(0));
            c->stlxr(a64::x13, a64::x12, arm::Mem(*qw1));
            c->cbnz(a64::x13, again);
        }
        else
        {
            // Just write zero
            c->mov(*qw0, Imm(0));
            c->str(*qw0, arm::Mem(*qw1));
        }

        c->mov(*arg0, a64::x14); // fast path result = consumed channel value (low word)
        c->bind(ret);
        const VLink& vt = VAlloc();
        c->eor(vt.b16(), vt.b16(), vt.b16());
        c->ins(vt.s(3), arg0->w());
        c->str(vt, SPU_OFF_128(gpr, op.rt));
    };

    switch (op.ra)
    {
        case SPU_RdSRR0:
        {
            const VLink& vt = VAlloc();
            c->eor(vt.b16(), vt.b16(), vt.b16());
            c->ldr(qw0->w(), SPU_OFF_32(srr0));
            c->ins(vt.s(3), qw0->w());
            c->str(vt, SPU_OFF_128(gpr, op.rt));
            return;
        }
        case MFC_RdTagStat:
        {
            read_channel(::offset32(&spu_thread::ch_tag_stat), false);
            return;
        }
        case MFC_RdTagMask:
        {
            const VLink& vt = VAlloc();
            c->eor(vt.b16(), vt.b16(), vt.b16());
            c->ldr(qw0->w(), SPU_OFF_32(ch_tag_mask));
            c->ins(vt.s(3), qw0->w());
            c->str(vt, SPU_OFF_128(gpr, op.rt));
            return;
        }
        case SPU_RdSigNotify1:
        {
            read_channel(::offset32(&spu_thread::ch_snr1));
            return;
        }
        case SPU_RdSigNotify2:
        {
            read_channel(::offset32(&spu_thread::ch_snr2));
            return;
        }
        case MFC_RdAtomicStat:
        {
            read_channel(::offset32(&spu_thread::ch_atomic_stat), false);
            return;
        }
        case MFC_RdListStallStat:
        {
            read_channel(::offset32(&spu_thread::ch_stall_stat), false);
            return;
        }
        case SPU_RdDec:
        {
            auto sub1 = [](spu_thread* _spu, v128* _res)
            {
                const u32 out = _spu->ch_dec_value - static_cast<u32>(get_timebased_time() - _spu->ch_dec_start_timestamp);

                if (out > 1500)
                {
                    _spu->state += cpu_flag::wait;
                    std::this_thread::yield();

                    if (_spu->test_stopped())
                    {
                        _spu->pc += 4;
                        spu_runtime::g_escape(_spu);
                    }
                }

                *_res = v128::from32r(out);
            };

            auto sub2 = [](spu_thread* _spu, v128* _res)
            {
                const u32 out = _spu->read_dec().first;

                *_res = v128::from32r(out);
            };

            c->mov(*addr, Imm(m_pos - m_base));
            c->add(*addr, *pc0, *addr);
            c->and_(*addr, *addr, Imm(0x3fffc));
            c->str(addr->w(), SPU_OFF_32(pc));
            c->add(*arg1, *cpu, Imm(::offset32(&spu_thread::gpr, op.rt)));
            c->mov(*arg0, *cpu);
            c->mov(*qw1, Imm(reinterpret_cast<u64>(g_cfg.core.spu_loop_detection ? +sub1 : +sub2)));
            c->blr(*qw1);
            return;
        }
        case SPU_RdEventMask:
        {
            const VLink& vt = VAlloc();
            c->eor(vt.b16(), vt.b16(), vt.b16());
            c->ldr(*qw0, SPU_OFF_64(ch_events));
            c->lsr(qw0->x(), qw0->x(), Imm(32));
            c->ins(vt.s(3), qw0->w());
            c->str(vt, SPU_OFF_128(gpr, op.rt));
            return;
        }
        case SPU_RdEventStat:
        {
            fall(op); // TODO (matches the x86 recompiler)
            return;
        }
        case SPU_RdMachStat:
        {
            const VLink& vt = VAlloc();
            c->eor(vt.b16(), vt.b16(), vt.b16());
            c->ldrb(qw0->w(), SPU_OFF_8(interrupts_enabled));
            c->ldr(qw1->w(), SPU_OFF_32(thread_type));
            c->and_(qw1->w(), qw1->w(), Imm(2));
            c->orr(qw0->w(), qw0->w(), qw1->w());
            c->ins(vt.s(3), qw0->w());
            c->str(vt, SPU_OFF_128(gpr, op.rt));
            return;
        }
        default: break;
    }

    // Generic blocking read
    c->mov(*addr, Imm(m_pos - m_base));
    c->add(*addr, *pc0, *addr);
    c->and_(*addr, *addr, Imm(0x3fffc));
    c->str(addr->w(), SPU_OFF_32(pc));
    c->mov(arg1->w(), Imm(+op.ra));
    c->mov(*arg0, *cpu);
    c->mov(*qw1, Imm(reinterpret_cast<u64>(&spu_rdch)));
    c->blr(*qw1);
    const VLink& vt = VAlloc();
    c->eor(vt.b16(), vt.b16(), vt.b16());
    c->ins(vt.s(3), arg0->w());
    c->str(vt, SPU_OFF_128(gpr, op.rt));
}

static u32 spu_rchcnt(spu_thread* _spu, u32 ch)
{
    return _spu->get_ch_count(ch);
}

void spu_recompiler::RCHCNT(spu_opcode_t op)
{
    using namespace asmjit;

    auto ch_cnt = [&](u32 ch_off, bool inv = false)
    {
        const VLink& vt = VAlloc();
        c->eor(vt.b16(), vt.b16(), vt.b16());
        c->ldr(*qw0, arm::Mem(*cpu, static_cast<int32_t>(ch_off)));
        c->lsr(qw0->x(), qw0->x(), Imm(spu_channel::off_count));
        c->and_(qw0->w(), qw0->w(), Imm(1));
        if (inv)
            c->eor(qw0->w(), qw0->w(), Imm(1));
        c->ins(vt.s(3), qw0->w());
        c->str(vt, SPU_OFF_128(gpr, op.rt));
    };

    auto cnt_imm = [&](u32 v)
    {
        const VLink& vt = VAlloc();
        c->eor(vt.b16(), vt.b16(), vt.b16());
        c->mov(qw0->w(), Imm(v));
        c->ins(vt.s(3), qw0->w());
        c->str(vt, SPU_OFF_128(gpr, op.rt));
    };

    switch (op.ra)
    {
        case SPU_WrOutMbox:       return ch_cnt(::offset32(&spu_thread::ch_out_mbox), true);
        case SPU_WrOutIntrMbox:   return ch_cnt(::offset32(&spu_thread::ch_out_intr_mbox), true);
        case MFC_RdTagStat:       return ch_cnt(::offset32(&spu_thread::ch_tag_stat));
        case MFC_RdListStallStat: return ch_cnt(::offset32(&spu_thread::ch_stall_stat));
        case SPU_RdSigNotify1:    return ch_cnt(::offset32(&spu_thread::ch_snr1));
        case SPU_RdSigNotify2:    return ch_cnt(::offset32(&spu_thread::ch_snr2));
        case MFC_RdAtomicStat:    return ch_cnt(::offset32(&spu_thread::ch_atomic_stat));

        case MFC_WrTagUpdate:
        {
            cnt_imm(1);
            return;
        }

        case MFC_Cmd:
        {
            const VLink& vt = VAlloc();
            c->eor(vt.b16(), vt.b16(), vt.b16());
            c->mov(qw0->w(), Imm(16));
            c->ldr(qw1->w(), SPU_OFF_32(mfc_size));
            c->sub(qw0->w(), qw0->w(), qw1->w());
            c->ins(vt.s(3), qw0->w());
            c->str(vt, SPU_OFF_128(gpr, op.rt));
            return;
        }

            // Channels with a constant count of 1:
        case SPU_WrEventMask:
        case SPU_WrEventAck:
        case SPU_WrDec:
        case SPU_RdDec:
        case SPU_RdEventMask:
        case SPU_RdMachStat:
        case SPU_WrSRR0:
        case SPU_RdSRR0:
        case SPU_Set_Bkmk_Tag:
        case SPU_PM_Start_Ev:
        case SPU_PM_Stop_Ev:
        case MFC_RdTagMask:
        case MFC_LSA:
        case MFC_EAH:
        case MFC_EAL:
        case MFC_Size:
        case MFC_TagID:
        case MFC_WrTagMask:
        case MFC_WrListStallAck:
        {
            cnt_imm(1);
            return;
        }
        default: break;
    }

    // Generic (non-blocking)
    c->mov(*addr, Imm(m_pos - m_base));
    c->add(*addr, *pc0, *addr);
    c->and_(*addr, *addr, Imm(0x3fffc));
    c->str(addr->w(), SPU_OFF_32(pc));
    c->mov(arg1->w(), Imm(+op.ra));
    c->mov(*arg0, *cpu);
    c->mov(*qw1, Imm(reinterpret_cast<u64>(&spu_rchcnt)));
    c->blr(*qw1);
    const VLink& vt = VAlloc();
    c->eor(vt.b16(), vt.b16(), vt.b16());
    c->ins(vt.s(3), arg0->w());
    c->str(vt, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::SF(spu_opcode_t op)
{
    const VLink& va = VGet(op.ra, VType::Int);
    const VLink& vb = VGet(op.rb, VType::Int);
    c->sub(vb.s4(), vb.s4(), va.s4());
    c->str(vb, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::OR(spu_opcode_t op)
{
    const VLink& va = VGet(op.ra, VType::Int);
    const VLink& vb = VGet(op.rb, VType::Int);
    c->orr(va.b16(), va.b16(), vb.b16());
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::BG(spu_opcode_t op)
{
    const VLink& va = VGet(op.ra, VType::Int);
    const VLink& vb = VGet(op.rb, VType::Int);
    const VLink& vi = VAlloc();
    c->cmhs(vi.s4(), vb.s4(), va.s4());
    const asmjit::a64::VecV& vc_t0 = VAlloc();
    c->ldr(vc_t0, VConst(v128::from32p(1)));
    c->and_(vi.b16(), vi.b16(), vc_t0.b16());
    // Actually: BG = (ra > rb unsigned) ? 0 : 1 = carry from (rb - ra)
    // cmhs: va >= vb unsigned, but we need rb >= ra unsigned
    // Let me redo: result = (rb >= ra) ? 1 : 0
    c->cmhs(vi.s4(), vb.s4(), va.s4());
    const asmjit::a64::VecV& vc_t1 = VAlloc();
    c->ldr(vc_t1, VConst(v128::from32p(1)));
    c->and_(vi.b16(), vi.b16(), vc_t1.b16());
    c->str(vi, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::SFH(spu_opcode_t op)
{
    const VLink& va = VGet(op.ra, VType::Int);
    const VLink& vb = VGet(op.rb, VType::Int);
    c->sub(vb.h8(), vb.h8(), va.h8());
    c->str(vb, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::NOR(spu_opcode_t op)
{
    const VLink& va = VGet(op.ra, VType::Int);
    const VLink& vb = VGet(op.rb, VType::Int);
    c->orr(va.b16(), va.b16(), vb.b16());
    c->mvn(va.b16(), va.b16());
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::ABSDB(spu_opcode_t op)
{
    const VLink& va = VGet(op.ra, VType::Int);
    const VLink& vb = VGet(op.rb, VType::Int);
    const VLink& vm = VAlloc();
    c->mov(vm.b16(), va.b16());
    c->umax(va.b16(), va.b16(), vb.b16());
    c->umin(vb.b16(), vb.b16(), vm.b16());
    c->sub(va.b16(), va.b16(), vb.b16());
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::ROT(spu_opcode_t op)
{
    // rt = rotl(ra, rb) = (ra << rb) | (ra >> (32 - rb))
    const VLink& va = VGet(op.ra, VType::Int);
    const VLink& vb = VGet(op.rb, VType::Int);
    const VLink& vt = VAlloc();
    const VLink& vm = VAlloc();
    const asmjit::a64::VecV& vc_t2 = VAlloc();
    c->ldr(vc_t2, VConst(v128::from32p(0x1f)));
    c->and_(vb.b16(), vb.b16(), vc_t2.b16());
    c->ushl(vt.s4(), va.s4(), vb.s4());
    const asmjit::a64::VecV& vc_t3 = VAlloc();
    c->ldr(vc_t3, VConst(v128::from32p(32)));
    c->sub(vm.s4(), vb.s4(), vc_t3.s4());  // vm = rb - 32 (negative => right shift)
    c->ushl(va.s4(), va.s4(), vm.s4());    // va = ra >> (32 - rb)
    c->orr(va.b16(), vt.b16(), va.b16());
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::ROTM(spu_opcode_t op)
{
    // rt = ra >> ((-rb) & 0x3f)  (32-bit logical shift, count wraps to 0-63)
    const VLink& va = VGet(op.ra, VType::Int);
    const VLink& vb = VGet(op.rb, VType::Int);
    const VLink& vs = VAlloc();
    const asmjit::a64::VecV& vc_t4 = VAlloc();
    c->ldr(vc_t4, VConst(v128::from32p(0x3f)));
    c->neg(vs.s4(), vb.s4());              // vs = -rb
    c->and_(vs.b16(), vs.b16(), vc_t4.b16()); // vs = (-rb) & 0x3f
    c->neg(vs.s4(), vs.s4());              // vs = -((-rb) & 0x3f)
    c->ushl(va.s4(), va.s4(), vs.s4());    // va >> ((-rb) & 0x3f)
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::ROTMA(spu_opcode_t op)
{
    // rt = (s32)ra >> ((-rb) & 0x3f)  (32-bit arithmetic shift)
    const VLink& va = VGet(op.ra, VType::Int);
    const VLink& vb = VGet(op.rb, VType::Int);
    const VLink& vs = VAlloc();
    const asmjit::a64::VecV& vc_t6 = VAlloc();
    c->ldr(vc_t6, VConst(v128::from32p(0x3f)));
    c->neg(vs.s4(), vb.s4());              // vs = -rb
    c->and_(vs.b16(), vs.b16(), vc_t6.b16()); // vs = (-rb) & 0x3f
    c->neg(vs.s4(), vs.s4());              // vs = -((-rb) & 0x3f)
    c->sshl(va.s4(), va.s4(), vs.s4());    // arithmetic right shift
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::SHL(spu_opcode_t op)
{
    const VLink& va = VGet(op.ra, VType::Int);
    const VLink& vb = VGet(op.rb, VType::Int);
    const asmjit::a64::VecV& vc_t9 = VAlloc();
    c->ldr(vc_t9, VConst(v128::from32p(0x3f)));
    c->and_(vb.b16(), vb.b16(), vc_t9.b16());
    c->ushl(va.s4(), va.s4(), vb.s4());
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::ROTH(spu_opcode_t op)
{
    // rt = rothl(ra, rb) = (ra << rb) | (ra >> (16 - rb))
    const VLink& va = VGet(op.ra, VType::Int);
    const VLink& vb = VGet(op.rb, VType::Int);
    const VLink& vt = VAlloc();
    const VLink& vm = VAlloc();
    const asmjit::a64::VecV& vc_t10 = VAlloc();
    c->ldr(vc_t10, VConst(v128::from16p(0xf)));
    c->and_(vb.b16(), vb.b16(), vc_t10.b16());
    c->ushl(vt.h8(), va.h8(), vb.h8());
    const asmjit::a64::VecV& vc_t11 = VAlloc();
    c->ldr(vc_t11, VConst(v128::from16p(16)));
    c->sub(vm.h8(), vb.h8(), vc_t11.h8()); // vm = rb - 16 (negative => right shift)
    c->ushl(va.h8(), va.h8(), vm.h8());    // va = ra >> (16 - rb)
    c->orr(va.b16(), vt.b16(), va.b16());
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::ROTHM(spu_opcode_t op)
{
    // rt = ra >> ((-rb) & 0x1f)  (16-bit logical shift)
    const VLink& va = VGet(op.ra, VType::Int);
    const VLink& vb = VGet(op.rb, VType::Int);
    const VLink& vs = VAlloc();
    const asmjit::a64::VecV& vc_t12 = VAlloc();
    c->ldr(vc_t12, VConst(v128::from16p(0x1f)));
    c->neg(vs.h8(), vb.h8());              // vs = -rb
    c->and_(vs.b16(), vs.b16(), vc_t12.b16()); // vs = (-rb) & 0x1f
    c->neg(vs.h8(), vs.h8());              // vs = -((-rb) & 0x1f)
    c->ushl(va.h8(), va.h8(), vs.h8());    // va >> ((-rb) & 0x1f)
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::ROTMAH(spu_opcode_t op)
{
    // rt = (s16)ra >> ((-rb) & 0x1f)  (16-bit arithmetic shift)
    const VLink& va = VGet(op.ra, VType::Int);
    const VLink& vb = VGet(op.rb, VType::Int);
    const VLink& vs = VAlloc();
    const asmjit::a64::VecV& vc_t14 = VAlloc();
    c->ldr(vc_t14, VConst(v128::from16p(0x1f)));
    c->neg(vs.h8(), vb.h8());              // vs = -rb
    c->and_(vs.b16(), vs.b16(), vc_t14.b16()); // vs = (-rb) & 0x1f
    c->neg(vs.h8(), vs.h8());              // vs = -((-rb) & 0x1f)
    c->sshl(va.h8(), va.h8(), vs.h8());    // arithmetic right shift
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::SHLH(spu_opcode_t op)
{
    const VLink& va = VGet(op.ra, VType::Int);
    const VLink& vb = VGet(op.rb, VType::Int);
    const asmjit::a64::VecV& vc_t17 = VAlloc();
    c->ldr(vc_t17, VConst(v128::from16p(0x1f)));
    c->and_(vb.b16(), vb.b16(), vc_t17.b16());
    c->ushl(va.h8(), va.h8(), vb.h8());
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::ROTI(spu_opcode_t op)
{
    const u32 sh = op.i7 & 0x1f;
    const VLink& va = VGet(op.ra, VType::Int);
    if (sh == 0) { c->str(va, SPU_OFF_128(gpr, op.rt)); return; }
    const VLink& vt = VAlloc();
    c->shl(vt.s4(), va.s4(), asmjit::Imm(sh));
    c->ushr(va.s4(), va.s4(), asmjit::Imm(32 - sh));
    c->orr(va.b16(), vt.b16(), va.b16());
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::ROTMI(spu_opcode_t op)
{
    const u32 sh = (0 - op.i7) & 0x3f;
    const VLink& va = VGet(op.ra, VType::Int);
    if (sh >= 32) { c->eor(va.b16(), va.b16(), va.b16()); c->str(va, SPU_OFF_128(gpr, op.rt)); return; }
    if (sh == 0) { c->str(va, SPU_OFF_128(gpr, op.rt)); return; }
    c->ushr(va.s4(), va.s4(), asmjit::Imm(sh));
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::ROTMAI(spu_opcode_t op)
{
    const u32 sh = (0 - op.i7) & 0x3f;
    const VLink& va = VGet(op.ra, VType::Int);
    const u32 clamped = sh > 31 ? 31 : sh;
    if (clamped == 0) { c->str(va, SPU_OFF_128(gpr, op.rt)); return; }
    c->sshr(va.s4(), va.s4(), asmjit::Imm(clamped));
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::SHLI(spu_opcode_t op)
{
    const u32 sh = op.i7 & 0x3f;
    const VLink& va = VGet(op.ra, VType::Int);
    if (sh >= 32) { c->eor(va.b16(), va.b16(), va.b16()); c->str(va, SPU_OFF_128(gpr, op.rt)); return; }
    if (sh == 0) { c->str(va, SPU_OFF_128(gpr, op.rt)); return; }
    c->shl(va.s4(), va.s4(), asmjit::Imm(sh));
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::ROTHI(spu_opcode_t op)
{
    const u32 sh = op.i7 & 0xf;
    const VLink& va = VGet(op.ra, VType::Int);
    if (sh == 0) { c->str(va, SPU_OFF_128(gpr, op.rt)); return; }
    const VLink& vt = VAlloc();
    c->shl(vt.h8(), va.h8(), asmjit::Imm(sh));
    c->ushr(va.h8(), va.h8(), asmjit::Imm(16 - sh));
    c->orr(va.b16(), vt.b16(), va.b16());
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::ROTHMI(spu_opcode_t op)
{
    const u32 sh = (0 - op.i7) & 0x1f;
    const VLink& va = VGet(op.ra, VType::Int);
    if (sh >= 16) { c->eor(va.b16(), va.b16(), va.b16()); c->str(va, SPU_OFF_128(gpr, op.rt)); return; }
    if (sh == 0) { c->str(va, SPU_OFF_128(gpr, op.rt)); return; }
    c->ushr(va.h8(), va.h8(), asmjit::Imm(sh));
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::ROTMAHI(spu_opcode_t op)
{
    const u32 sh = (0 - op.i7) & 0x1f;
    const VLink& va = VGet(op.ra, VType::Int);
    const u32 clamped = sh > 15 ? 15 : sh;
    if (clamped == 0) { c->str(va, SPU_OFF_128(gpr, op.rt)); return; }
    c->sshr(va.h8(), va.h8(), asmjit::Imm(clamped));
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::SHLHI(spu_opcode_t op)
{
    const u32 sh = op.i7 & 0x1f;
    const VLink& va = VGet(op.ra, VType::Int);
    if (sh >= 16) { c->eor(va.b16(), va.b16(), va.b16()); c->str(va, SPU_OFF_128(gpr, op.rt)); return; }
    if (sh == 0) { c->str(va, SPU_OFF_128(gpr, op.rt)); return; }
    c->shl(va.h8(), va.h8(), asmjit::Imm(sh));
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::A(spu_opcode_t op)
{
    const VLink& va = VGet(op.ra, VType::Int);
    const VLink& vb = VGet(op.rb, VType::Int);
    c->add(va.s4(), va.s4(), vb.s4());
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::AND(spu_opcode_t op)
{
    const VLink& va = VGet(op.ra, VType::Int);
    const VLink& vb = VGet(op.rb, VType::Int);
    c->and_(va.b16(), va.b16(), vb.b16());
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::CG(spu_opcode_t op)
{
    // rt = carry(ra + rb) 闁?{0, 1}
    // carry = ((ra ^ 0x80000000) > ((ra + rb) ^ 0x80000000)) signed
    const VLink& va = VGet(op.ra, VType::Int);
    const VLink& vb = VGet(op.rb, VType::Int);
    const VLink& vt = VAlloc();
    const asmjit::a64::VecV& vc = VAlloc();
    c->ldr(vc, VConst(v128::from32p(0x80000000)));
    c->add(vt.s4(), vb.s4(), va.s4());   // vt = ra + rb
    c->eor(va.b16(), va.b16(), vc.b16());   // va = ra ^ 0x80000000
    c->eor(vt.b16(), vt.b16(), vc.b16());   // vt = (ra+rb) ^ 0x80000000
    c->cmgt(va.s4(), va.s4(), vt.s4());  // va = (va > vt) signed
    c->ushr(va.s4(), va.s4(), asmjit::Imm(31));
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::AH(spu_opcode_t op)
{
    const VLink& va = VGet(op.ra, VType::Int);
    const VLink& vb = VGet(op.rb, VType::Int);
    c->add(va.h8(), va.h8(), vb.h8());
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::NAND(spu_opcode_t op)
{
    const VLink& va = VGet(op.ra, VType::Int);
    const VLink& vb = VGet(op.rb, VType::Int);
    c->and_(va.b16(), va.b16(), vb.b16());
    c->mvn(va.b16(), va.b16());
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::AVGB(spu_opcode_t op)
{
    const VLink& va = VGet(op.ra, VType::Int);
    const VLink& vb = VGet(op.rb, VType::Int);
    c->urhadd(va.b16(), va.b16(), vb.b16());
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::MTSPR(spu_opcode_t op)
{
    // Move to SPR - no-op (SPR not readable)
}
static void spu_wrch(spu_thread* _spu, u32 ch, u32 value)
{
    if (!_spu->set_ch_value(ch, value) || _spu->state & cpu_flag::again)
    {
        spu_runtime::g_escape(_spu);
    }

    if (_spu->test_stopped())
    {
        _spu->pc += 4;
        spu_runtime::g_escape(_spu);
    }
}

static void spu_wrch_mfc(spu_thread* _spu)
{
    if (!_spu->process_mfc_cmd() || _spu->state & cpu_flag::again)
    {
        spu_runtime::g_escape(_spu);
    }

    if (_spu->test_stopped())
    {
        _spu->pc += 4;
        spu_runtime::g_escape(_spu);
    }
}
void spu_recompiler::WRCH(spu_opcode_t op)
{
    using namespace asmjit;

    switch (op.ra)
    {
        case SPU_WrSRR0:
        {
            c->ldr(addr->w(), SPU_OFF_32(gpr, op.rt, &v128::_u32, 3));
            c->and_(*addr, *addr, Imm(0x3fffc));
            c->str(addr->w(), SPU_OFF_32(srr0));
            return;
        }
        case SPU_WrOutIntrMbox:
        {
            // Can't seemingly be optimized
            break;
        }
        case SPU_WrOutMbox:
        {
            // The inlined fast path used here previously did a plain CAS on
            // (value | bit_count) while ignoring the bit_wait/bit_occupy flags.
            // When a PPU-side waiter had bit_wait set, that store silently cleared
            // the wait flag and never woke the waiter (spu_channel::push semantics),
            // hanging cross-processor handshakes (observed as the game stalling
            // right after SPURS init, with PPU threads stuck in lwcond wait).
            // Fall back to the generic spu_wrch call, exactly like the x64 recompiler.
            break;
        }
        case MFC_WrTagMask:
        {
            Label upd = c->newLabel();
            Label ret = c->newLabel();
            c->ldr(qw0->w(), SPU_OFF_32(gpr, op.rt, &v128::_u32, 3));
            c->str(qw0->w(), SPU_OFF_32(ch_tag_mask));
            c->ldrb(qw1->w(), SPU_OFF_8(ch_tag_upd));
            c->cmp(qw1->w(), Imm(MFC_TAG_UPDATE_IMMEDIATE));
            c->b_ne(upd);

            after.emplace_back([=, this, pos = m_pos]
                               {
                                   c->bind(upd);
                                   c->mov(*addr, Imm(pos - m_base));
                                   c->add(*addr, *pc0, *addr);
                                   c->and_(*addr, *addr, Imm(0x3fffc));
                                   c->str(addr->w(), SPU_OFF_32(pc));
                                   c->mov(arg1->w(), Imm(MFC_WrTagMask));
                                   c->ldr(asmjit::a64::x2.w(), SPU_OFF_32(gpr, op.rt, &v128::_u32, 3));
                                   c->mov(*arg0, *cpu);
                                   c->mov(*qw1, Imm(reinterpret_cast<u64>(&spu_wrch)));
                                   c->blr(*qw1);
                                   c->b(ret);
                               });

            c->bind(ret);
            return;
        }
        case MFC_WrTagUpdate:
        {
            Label fail = c->newLabel();
            Label ret = c->newLabel();
            c->ldr(qw0->w(), SPU_OFF_32(gpr, op.rt, &v128::_u32, 3));
            c->cmp(qw0->w(), Imm(2));
            c->b_hi(fail);

            after.emplace_back([=, this, pos = m_pos]
                               {
                                   c->bind(fail);
                                   c->mov(*addr, Imm(pos - m_base));
                                   c->add(*addr, *pc0, *addr);
                                   c->and_(*addr, *addr, Imm(0x3fffc));
                                   c->str(addr->w(), SPU_OFF_32(pc));
                                   c->mov(arg1->w(), Imm(+op.ra));
                                   // ARM64: qw0(x10) is NOT arg2; on x64 qw0==r8==arg2 so value passes implicitly
                                   c->mov(asmjit::a64::x2, *qw0);
                                   c->mov(*arg0, *cpu);
                                   c->mov(*qw1, Imm(reinterpret_cast<u64>(&spu_wrch)));
                                   c->blr(*qw1);
                                   c->b(ret);
                               });

            // Value <= 2: preserve rt value and call spu_wrch so that set_ch_value
            // applies the MFC_WrTagUpdate semantics (writes ch_tag_upd and, in
            // IMMEDIATE mode, updates ch_tag_stat). Skipping this (previous code)
            // left ch_tag_stat stale so MFC_RdTagStat blocked forever.
            c->mov(*arg0, *cpu);
            c->mov(arg1->w(), Imm(+op.ra));
            c->mov(asmjit::a64::x2, *qw0);
            c->mov(*qw1, Imm(reinterpret_cast<u64>(&spu_wrch)));
            c->blr(*qw1);
            c->bind(ret);
            return;
        }
        case MFC_LSA:
        {
            c->ldr(addr->w(), SPU_OFF_32(gpr, op.rt, &v128::_u32, 3));
            c->str(addr->w(), SPU_OFF_32(ch_mfc_cmd, &spu_mfc_cmd::lsa));
            return;
        }
        case MFC_EAH:
        {
            c->ldr(addr->w(), SPU_OFF_32(gpr, op.rt, &v128::_u32, 3));
            c->str(addr->w(), SPU_OFF_32(ch_mfc_cmd, &spu_mfc_cmd::eah));
            return;
        }
        case MFC_EAL:
        {
            c->ldr(addr->w(), SPU_OFF_32(gpr, op.rt, &v128::_u32, 3));
            c->str(addr->w(), SPU_OFF_32(ch_mfc_cmd, &spu_mfc_cmd::eal));
            return;
        }
        case MFC_Size:
        {
            c->ldr(addr->w(), SPU_OFF_32(gpr, op.rt, &v128::_u32, 3));
            c->and_(*addr, *addr, Imm(0x7fff));
            c->strh(addr->w(), SPU_OFF_16(ch_mfc_cmd, &spu_mfc_cmd::size));
            return;
        }
        case MFC_TagID:
        {
            c->ldr(addr->w(), SPU_OFF_32(gpr, op.rt, &v128::_u32, 3));
            c->and_(*addr, *addr, Imm(0x1f));
            c->strb(addr->w(), SPU_OFF_8(ch_mfc_cmd, &spu_mfc_cmd::tag));
            return;
        }
        case MFC_Cmd:
        {
            c->ldr(addr->w(), SPU_OFF_32(gpr, op.rt, &v128::_u32, 3));
            c->strb(addr->w(), SPU_OFF_8(ch_mfc_cmd, &spu_mfc_cmd::cmd));
            c->mov(*addr, Imm(m_pos - m_base));
            c->add(*addr, *pc0, *addr);
            c->and_(*addr, *addr, Imm(0x3fffc));
            c->str(addr->w(), SPU_OFF_32(pc));
            c->mov(*arg0, *cpu);
            c->mov(*qw1, Imm(reinterpret_cast<u64>(&spu_wrch_mfc)));
            c->blr(*qw1);
            return;
        }
        case MFC_WrListStallAck:
        {
            auto sub = [](spu_thread* _spu, u32 tag)
            {
                for (u32 i = 0; i < _spu->mfc_size; i++)
                {
                    if (_spu->mfc_queue[i].tag == (tag | 0x80))
                    {
                        // Unset stall bit
                        _spu->mfc_queue[i].tag &= 0x7f;
                    }
                }

                _spu->do_mfc(true);
            };

            Label ret = c->newLabel();
            c->ldr(qw0->w(), SPU_OFF_32(gpr, op.rt, &v128::_u32, 3));
            c->and_(qw0->w(), qw0->w(), Imm(0x1f));
            c->ldr(qw1->w(), SPU_OFF_32(ch_stall_mask));
            c->mov(a64::x12, Imm(1));
            c->lslv(a64::x12, a64::x12, qw0->w());
            c->tst(qw1->w(), a64::x12);
            c->bic(qw1->w(), qw1->w(), a64::x12);
            c->str(qw1->w(), SPU_OFF_32(ch_stall_mask));
            c->b_eq(ret);
            c->mov(*arg0, *cpu);
            c->mov(arg1->w(), qw0->w());
            c->mov(*qw1, Imm(reinterpret_cast<u64>(+sub)));
            c->blr(*qw1);
            c->bind(ret);
            return;
        }
        case SPU_WrDec:
        {
            auto sub = [](spu_thread* _spu)
            {
                _spu->get_events(SPU_EVENT_TM);
                _spu->ch_dec_start_timestamp = get_timebased_time();
            };

            c->mov(*arg0, *cpu);
            c->mov(*qw1, Imm(reinterpret_cast<u64>(+sub)));
            c->blr(*qw1);
            c->ldr(qw0->w(), SPU_OFF_32(gpr, op.rt, &v128::_u32, 3));
            c->str(qw0->w(), SPU_OFF_32(ch_dec_value));
            c->mov(qw0->w(), Imm(0));
            c->strb(qw0->w(), SPU_OFF_8(is_dec_frozen));
            return;
        }
        case SPU_WrEventMask:
        {
            // TODO
            break;
        }
        case SPU_WrEventAck:
        {
            // TODO
            break;
        }
        case SPU_Set_Bkmk_Tag:
        case SPU_PM_Start_Ev:
        case SPU_PM_Stop_Ev:
        {
            return;
        }
        default: break;
    }

    // Generic channel write
    c->mov(*addr, Imm(m_pos - m_base));
    c->add(*addr, *pc0, *addr);
    c->and_(*addr, *addr, Imm(0x3fffc));
    c->str(addr->w(), SPU_OFF_32(pc));
    c->mov(arg1->w(), Imm(+op.ra));
    c->ldr(asmjit::a64::x2.w(), SPU_OFF_32(gpr, op.rt, &v128::_u32, 3));
    c->mov(*arg0, *cpu);
    c->mov(*qw1, Imm(reinterpret_cast<u64>(&spu_wrch)));
    c->blr(*qw1);
}

void spu_recompiler::BIZ(spu_opcode_t op)
{
    asmjit::Label branch_label = c->newLabel();
    c->ldr(qw0->w(), SPU_OFF_32(gpr, op.rt, &v128::_u32, 3));
    c->cmp(qw0->w(), asmjit::Imm(0));
    c->b_eq(branch_label);
    after.emplace_back([=, this, jt = m_targets[m_pos].size() > 1]
                       {
                           c->align(asmjit::AlignMode::kCode, 16);
                           c->bind(branch_label);
                           c->ldr(addr->w(), SPU_OFF_32(gpr, op.ra, &v128::_u32, 3));
                           c->and_(*addr, *addr, asmjit::Imm(0x3fffc));
                           branch_indirect(op, jt);
                       });
}

void spu_recompiler::BINZ(spu_opcode_t op)
{
    asmjit::Label branch_label = c->newLabel();
    c->ldr(qw0->w(), SPU_OFF_32(gpr, op.rt, &v128::_u32, 3));
    c->cmp(qw0->w(), asmjit::Imm(0));
    c->b_ne(branch_label);
    after.emplace_back([=, this, jt = m_targets[m_pos].size() > 1]
                       {
                           c->align(asmjit::AlignMode::kCode, 16);
                           c->bind(branch_label);
                           c->ldr(addr->w(), SPU_OFF_32(gpr, op.ra, &v128::_u32, 3));
                           c->and_(*addr, *addr, asmjit::Imm(0x3fffc));
                           branch_indirect(op, jt);
                       });
}

void spu_recompiler::BIHZ(spu_opcode_t op)
{
    asmjit::Label branch_label = c->newLabel();
    c->ldrh(qw0->w(), SPU_OFF_16(gpr, op.rt, &v128::_u16, 6));
    c->cmp(qw0->w(), asmjit::Imm(0));
    c->b_eq(branch_label);
    after.emplace_back([=, this, jt = m_targets[m_pos].size() > 1]
                       {
                           c->align(asmjit::AlignMode::kCode, 16);
                           c->bind(branch_label);
                           c->ldr(addr->w(), SPU_OFF_32(gpr, op.ra, &v128::_u32, 3));
                           c->and_(*addr, *addr, asmjit::Imm(0x3fffc));
                           branch_indirect(op, jt);
                       });
}

void spu_recompiler::BIHNZ(spu_opcode_t op)
{
    asmjit::Label branch_label = c->newLabel();
    c->ldrh(qw0->w(), SPU_OFF_16(gpr, op.rt, &v128::_u16, 6));
    c->cmp(qw0->w(), asmjit::Imm(0));
    c->b_ne(branch_label);
    after.emplace_back([=, this, jt = m_targets[m_pos].size() > 1]
                       {
                           c->align(asmjit::AlignMode::kCode, 16);
                           c->bind(branch_label);
                           c->ldr(addr->w(), SPU_OFF_32(gpr, op.ra, &v128::_u32, 3));
                           c->and_(*addr, *addr, asmjit::Imm(0x3fffc));
                           branch_indirect(op, jt);
                       });
}

void spu_recompiler::STOPD(spu_opcode_t op)
{
    STOP(spu_opcode_t{0x3fff});
}

void spu_recompiler::STQX(spu_opcode_t op)
{
    c->ldr(addr->w(), SPU_OFF_32(gpr, op.ra, &v128::_u32, 3));
    c->ldr(arg1->w(), SPU_OFF_32(gpr, op.rb, &v128::_u32, 3));
    c->add(*addr, *addr, arg1->x());
    c->and_(*addr, *addr, asmjit::Imm(0x3fff0));
    c->add(*addr, *ls, *addr);
    const VLink& vt = VGet(op.rt, VType::Int);
    const VLink& vs = VAlloc();
    c->rev64(vs.b16(), vt.b16());
    c->ext(vs.b16(), vs.b16(), vs.b16(), 8);
    c->str(vs, asmjit::arm::Mem(*addr));
}

void spu_recompiler::BI(spu_opcode_t op)
{
    const auto found = m_targets.find(m_pos);
    const auto is_jt = found == m_targets.end() || found->second.size() > 1;
    if (found == m_targets.end()) { spu_log.error("[0x%x] BI: no targets", m_pos); }
    else if (op.d && found->second.size() == 1 && found->second[0] == spu_branch_target(m_pos, 1))
    {
        c->mov(*qw0, asmjit::Imm(0));
        c->strb(qw0->w(), SPU_OFF_8(interrupts_enabled));
        return;
    }
    c->ldr(addr->w(), SPU_OFF_32(gpr, op.ra, &v128::_u32, 3));
    c->and_(*addr, *addr, asmjit::Imm(0x3fffc));
    branch_indirect(op, is_jt, !is_jt);
    m_pos = -1;
}

void spu_recompiler::BISL(spu_opcode_t op)
{
    c->ldr(addr->w(), SPU_OFF_32(gpr, op.ra, &v128::_u32, 3));
    c->and_(*addr, *addr, asmjit::Imm(0x3fffc));
    const VLink& vr = VAlloc();
    c->mov(*qw0, asmjit::Imm((m_pos + 4) - m_base));
    c->add(qw0->w(), qw0->w(), pc0->w());
    c->and_(qw0->w(), qw0->w(), asmjit::Imm(0x3fffc));
    c->eor(vr.b16(), vr.b16(), vr.b16()); // clear stale pool data (rt = {0,0,0,ra})
    c->ins(vr.s(3), qw0->w());
    c->str(vr, SPU_OFF_128(gpr, op.rt));
    branch_set_link(m_pos + 4);
    branch_indirect(op, true, false);
    m_pos = -1;
}

void spu_recompiler::IRET(spu_opcode_t op)
{
    c->ldr(addr->w(), SPU_OFF_32(srr0));
    branch_indirect(op);
    m_pos = -1;
}

void spu_recompiler::BISLED(spu_opcode_t op)
{
    auto get_events = [](spu_thread* _spu) -> u32
    {
        return _spu->get_events(_spu->ch_events.load().mask).count;
    };

    // Load branch target from gpr[ra]
    c->ldr(addr->w(), SPU_OFF_32(gpr, op.ra, &v128::_u32, 3));

    // Write link: rt = {0, 0, 0, (pc + 4) & 0x3fffc}
    const VLink& vr = VAlloc();
    c->mov(*qw0, asmjit::Imm((m_pos + 4) - m_base));
    c->add(qw0->w(), qw0->w(), pc0->w());
    c->and_(qw0->w(), qw0->w(), asmjit::Imm(0x3fffc));
    c->eor(vr.b16(), vr.b16(), vr.b16()); // clear stale pool data (rt = {0,0,0,ra})
    c->ins(vr.s(3), qw0->w());
    c->str(vr, SPU_OFF_128(gpr, op.rt));

    // Check for external data: branch only if events are pending, otherwise
    // fall through to the next instruction.
    asmjit::Label branch_label = c->newLabel();
    c->mov(*arg0, *cpu);
    c->mov(*qw1, asmjit::Imm(reinterpret_cast<u64>(+get_events)));
    c->blr(*qw1);
    c->tst(arg0->w(), asmjit::Imm(1));
    c->b_ne(branch_label);

    after.emplace_back([=, this]()
                       {
                           c->align(asmjit::AlignMode::kCode, 16);
                           c->bind(branch_label);
                           c->and_(addr->w(), addr->w(), asmjit::Imm(0x3fffc));
                           branch_indirect(op, true, false);
                       });
}

void spu_recompiler::HBR(spu_opcode_t op)
{
    // Hint branch - no-op on ARM64
}

void spu_recompiler::GB(spu_opcode_t op)
{
    // rt word3 (lane0) low 4 bits: bit i = ra word i BIT0
    // (x86: pslld(va,31) moves bit0 into bit31, then movmskps gathers bit31 of each
    // lane => gathers ORIGINAL BIT0, not the sign bit!)
    const asmjit::a64::VecV va = VGet(op.ra, VType::Int);
    const asmjit::a64::GpW t = addr->w();
    const asmjit::a64::GpW acc = qw0->w();
    c->eor(acc, acc, acc);
    for (u32 i = 0; i < 4; i++)
    {
        c->umov(t, va.s(i));
        c->ubfx(t, t, asmjit::Imm(0), asmjit::Imm(1)); // bit0 -> mask bit i
        c->orr(acc, acc, t, asmjit::a64::lsl(i));
    }
    const VLink& vr = VAlloc();
    c->eor(vr.b16(), vr.b16(), vr.b16());
    c->ins(vr.s(3), acc); // SPU word0 (lane3)
    c->str(vr, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::GBH(spu_opcode_t op)
{
    // rt word3 (lane0) low 8 bits: bit i = ra halfword i sign bit
    const asmjit::a64::VecV va = VGet(op.ra, VType::Int);
    const asmjit::a64::GpW t = addr->w();
    const asmjit::a64::GpW acc = qw0->w();
    c->eor(acc, acc, acc);
    for (u32 i = 0; i < 8; i++)
    {
        c->umov(t, va.h(i));
        c->ubfx(t, t, asmjit::Imm(0), asmjit::Imm(1)); // halfword bit0 -> mask bit i
        c->orr(acc, acc, t, asmjit::a64::lsl(i));
    }
    const VLink& vr = VAlloc();
    c->eor(vr.b16(), vr.b16(), vr.b16());
    c->ins(vr.s(3), acc); // SPU word0 (lane3)
    c->str(vr, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::GBB(spu_opcode_t op)
{
    // rt word3 (lane0) low 16 bits: bit i = ra byte i bit7
    const asmjit::a64::VecV va = VGet(op.ra, VType::Int);
    const asmjit::a64::GpW t = addr->w();
    const asmjit::a64::GpW acc = qw0->w();
    c->eor(acc, acc, acc);
    for (u32 i = 0; i < 16; i++)
    {
        c->umov(t, va.b(i));
        c->ubfx(t, t, asmjit::Imm(0), asmjit::Imm(1)); // byte bit0 -> mask bit i
        c->orr(acc, acc, t, asmjit::a64::lsl(i)); // place bit i
    }
    const VLink& vr = VAlloc();
    c->eor(vr.b16(), vr.b16(), vr.b16());
    c->ins(vr.s(3), acc); // SPU word0 (lane3)
    c->str(vr, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::FSM(spu_opcode_t op)
{
    const VLink& va = VGet(op.ra, VType::Int);
    c->umov(addr->w(), va.s(3)); // SPU word0 (lane3)
    const VLink& vt = VAlloc();
    c->eor(vt.b16(), vt.b16(), vt.b16());
    c->ins(vt.s(0), addr->w());
    c->ins(vt.s(1), addr->w());
    c->ins(vt.s(2), addr->w());
    c->ins(vt.s(3), addr->w()); // broadcast word0
    const asmjit::a64::VecV vm = VAlloc();
    const v128 m{[]() { v128 r{}; r._u32[0] = 1; r._u32[1] = 2; r._u32[2] = 4; r._u32[3] = 8; return r; }()};
    c->ldr(vm, VConst(m));
    c->and_(vt.b16(), vt.b16(), vm.b16());
    c->cmeq(vt.s4(), vt.s4(), vm.s4());
    c->str(vt, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::FSMH(spu_opcode_t op)
{
    // x86: bits = h6 (word3 low halfword, _u16[6]) broadcast to all lanes;
    // mask lane j = 1 << j -> rt halfword lane j = (h6 & (1<<j)) ? 0xffff : 0
    const VLink& va = VGet(op.ra, VType::Int);
    const VLink& vt = VAlloc();
    const asmjit::a64::VecV vm = VAlloc();
    c->umov(qw0->w(), va.s(3)); // word3 (h6 = its low 16 bits)
    c->dup(vt.h8(), qw0->w());
    const v128 m{[]() { v128 r{}; for (u32 i = 0; i < 8; i++) r._u16[i] = static_cast<u16>(1u << i); return r; }()};
    c->ldr(vm, VConst(m));
    // NOTE: AND (vector) only exists in .16B/.8B arrangements; .8H would be
    // rejected by the assembler and silently dropped.
    c->and_(vt.b16(), vt.b16(), vm.b16());
    c->cmeq(vt.h8(), vt.h8(), vm.h8());
    c->str(vt, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::FSMB(spu_opcode_t op)
{
    // x86: bits lane 0-7 = ra._u8[12], lane 8-15 = ra._u8[13];
    // mask lane j = 1 << (j & 7)
    const VLink& va = VGet(op.ra, VType::Int);
    const VLink& vt = VAlloc();
    const asmjit::a64::VecV vm = VAlloc();
    c->umov(qw0->w(), va.s(3)); // word3: b12 = LSB, b13 = byte 1
    c->and_(qw1->w(), qw0->w(), asmjit::Imm(0xff)); // b12
    c->dup(vt.b16(), qw1->w());
    c->lsr(qw1->w(), qw0->w(), asmjit::Imm(8));
    c->and_(qw1->w(), qw1->w(), asmjit::Imm(0xff)); // b13
    c->dup(vm.b16(), qw1->w());
    // EXT Vd, Vn, Vm, #8 = bytes [Vn.b8..15, Vm.b0..7]: Vn=vt (b12) first
    c->ext(vt.b16(), vt.b16(), vm.b16(), 8); // [b12 x8, b13 x8]
    const v128 m{[]() { v128 r{}; for (u32 i = 0; i < 16; i++) r._u8[i] = static_cast<u8>(1u << (i & 7)); return r; }()};
    c->ldr(vm, VConst(m));
    c->and_(vt.b16(), vt.b16(), vm.b16());
    c->cmeq(vt.b16(), vt.b16(), vm.b16());
    c->str(vt, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::FREST(spu_opcode_t op)
{
    // r = frac_lut[(ra>>18)&0x1F] | exp_lut[(ra>>23)&0xFF] | (ra & 0x80000000)
    const VLink& va = VGet(op.ra, VType::Int);
    const VLink& vt = VAlloc();
    c->eor(vt.b16(), vt.b16(), vt.b16());
    c->mov(arg0->x(), asmjit::Imm(reinterpret_cast<u64>(spu_frest_fraction_lut)));
    c->mov(arg1->x(), asmjit::Imm(reinterpret_cast<u64>(spu_frest_exponent_lut)));
    for (u32 i = 0; i < 4; i++)
    {
        c->umov(addr->w(), va.s(i));
        c->lsr(qw0->w(), addr->w(), asmjit::Imm(18));
        c->and_(qw0->w(), qw0->w(), asmjit::Imm(0x1f));
        c->add(qw1->x(), arg0->x(), qw0->x(), asmjit::a64::lsl(2));
        c->ldr(qw1->w(), asmjit::arm::Mem(qw1->x())); // fraction value
        c->lsr(qw0->w(), addr->w(), asmjit::Imm(23));
        c->and_(qw0->w(), qw0->w(), asmjit::Imm(0xff));
        c->add(qw0->x(), arg1->x(), qw0->x(), asmjit::a64::lsl(2));
        c->ldr(qw0->w(), asmjit::arm::Mem(qw0->x())); // exponent value
        c->orr(qw1->w(), qw1->w(), qw0->w());
        c->bic(qw0->w(), addr->w(), asmjit::Imm(0x7fffffff)); // sign bit
        c->orr(qw1->w(), qw1->w(), qw0->w());
        c->ins(vt.s(i), qw1->w());
    }
    c->str(vt, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::FRSQEST(spu_opcode_t op)
{
    // r = frac_lut[(ra>>18)&0x3F] | exp_lut[(ra>>23)&0xFF]
    const VLink& va = VGet(op.ra, VType::Int);
    const VLink& vt = VAlloc();
    c->eor(vt.b16(), vt.b16(), vt.b16());
    c->mov(arg0->x(), asmjit::Imm(reinterpret_cast<u64>(spu_frsqest_fraction_lut)));
    c->mov(arg1->x(), asmjit::Imm(reinterpret_cast<u64>(spu_frsqest_exponent_lut)));
    for (u32 i = 0; i < 4; i++)
    {
        c->umov(addr->w(), va.s(i));
        c->lsr(qw0->w(), addr->w(), asmjit::Imm(18));
        c->and_(qw0->w(), qw0->w(), asmjit::Imm(0x3f));
        c->add(qw1->x(), arg0->x(), qw0->x(), asmjit::a64::lsl(2));
        c->ldr(qw1->w(), asmjit::arm::Mem(qw1->x())); // fraction value
        c->lsr(qw0->w(), addr->w(), asmjit::Imm(23));
        c->and_(qw0->w(), qw0->w(), asmjit::Imm(0xff));
        c->add(qw0->x(), arg1->x(), qw0->x(), asmjit::a64::lsl(2));
        c->ldr(qw0->w(), asmjit::arm::Mem(qw0->x())); // exponent value
        c->orr(qw1->w(), qw1->w(), qw0->w());
        c->ins(vt.s(i), qw1->w());
    }
    c->str(vt, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::LQX(spu_opcode_t op)
{
    c->ldr(addr->w(), SPU_OFF_32(gpr, op.ra, &v128::_u32, 3));
    c->ldr(arg1->w(), SPU_OFF_32(gpr, op.rb, &v128::_u32, 3));
    c->add(*addr, *addr, arg1->x());
    c->and_(*addr, *addr, asmjit::Imm(0x3fff0));
    c->add(*addr, *ls, *addr);
    const VLink& vt = VAlloc();
    c->ldr(vt, asmjit::arm::Mem(*addr));
    c->rev64(vt.b16(), vt.b16());
    c->ext(vt.b16(), vt.b16(), vt.b16(), 8);
    c->str(vt, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::ROTQBYBI(spu_opcode_t op)
{
    // rt = ra rotated right by n bytes, n = (rb._u32[3] >> 3) & 0xf
    // tbl with wrapped indices (i - n) & 0xf
    const VLink& va = VGet(op.ra, VType::Int);
    const VLink& vidx = VAlloc();
    const VLink& vn = VAlloc();
    const VLink& vt = VAlloc();
    const asmjit::a64::VecV vc = VAlloc();
    c->ldr(addr->w(), SPU_OFF_32(gpr, op.rb, &v128::_u32, 3));
    c->lsr(addr->w(), addr->w(), asmjit::Imm(3));
    c->and_(addr->w(), addr->w(), asmjit::Imm(0xf)); // n
    c->dup(vn.b16(), addr->w());
    const v128 base{[]() { v128 r{}; for (u32 i = 0; i < 16; i++) r._u8[i] = static_cast<u8>(i); return r; }()};
    c->ldr(vidx, VConst(base));
    c->sub(vidx.b16(), vidx.b16(), vn.b16());   // (i - n) mod 256
    c->ldr(vc, VConst(v128::from8p(0xf)));
    c->and_(vidx.b16(), vidx.b16(), vc.b16());  // wrap indices
    c->tbl(vt.b16(), va.b16(), vidx.b16());
    c->str(vt, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::ROTQMBYBI(spu_opcode_t op)
{
    // rt = ra shifted right by n bytes (zero fill), n = (0 - (rb._u32[3] >> 3)) & 0x1f
    // tbl with raw indices i - n (out-of-range -> 0)
    const VLink& va = VGet(op.ra, VType::Int);
    const VLink& vidx = VAlloc();
    const VLink& vn = VAlloc();
    const VLink& vt = VAlloc();
    c->ldr(addr->w(), SPU_OFF_32(gpr, op.rb, &v128::_u32, 3));
    c->lsr(addr->w(), addr->w(), asmjit::Imm(3));
    c->neg(addr->w(), addr->w());
    c->and_(addr->w(), addr->w(), asmjit::Imm(0x1f)); // n
    c->dup(vn.b16(), addr->w());
    const v128 base{[]() { v128 r{}; for (u32 i = 0; i < 16; i++) r._u8[i] = static_cast<u8>(i); return r; }()};
    c->ldr(vidx, VConst(base));
    c->add(vidx.b16(), vidx.b16(), vn.b16()); // i + n (> 15 -> tbl yields 0)
    c->tbl(vt.b16(), va.b16(), vidx.b16());
    c->str(vt, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::SHLQBYBI(spu_opcode_t op)
{
    // rt = ra shifted left by n bytes (zero fill), n = (rb._u32[3] >> 3) & 0x1f
    // tbl with raw indices i + n (out-of-range -> 0)
    const VLink& va = VGet(op.ra, VType::Int);
    const VLink& vidx = VAlloc();
    const VLink& vn = VAlloc();
    const VLink& vt = VAlloc();
    c->ldr(addr->w(), SPU_OFF_32(gpr, op.rb, &v128::_u32, 3));
    c->lsr(addr->w(), addr->w(), asmjit::Imm(3));
    c->and_(addr->w(), addr->w(), asmjit::Imm(0x1f)); // n
    c->dup(vn.b16(), addr->w());
    const v128 base{[]() { v128 r{}; for (u32 i = 0; i < 16; i++) r._u8[i] = static_cast<u8>(i); return r; }()};
    c->ldr(vidx, VConst(base));
    c->sub(vidx.b16(), vidx.b16(), vn.b16());   // i - n (negative -> tbl yields 0)
    c->tbl(vt.b16(), va.b16(), vidx.b16());
    c->str(vt, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::CBX(spu_opcode_t op)
{
    // rt = const 0x10..0x1f; rt._u8[~(rb._u32[3] + ra._u32[3]) & 0xf] = 0x03
    c->ldr(addr->w(), SPU_OFF_32(gpr, op.rb, &v128::_u32, 3));
    c->ldr(arg1->w(), SPU_OFF_32(gpr, op.ra, &v128::_u32, 3));
    c->add(addr->w(), addr->w(), arg1->w());
    c->mvn(addr->w(), addr->w());
    c->and_(addr->w(), addr->w(), asmjit::Imm(0xf));

    const VLink& vr = VAlloc();
    c->ldr(vr, VConst(v128::from64(0x18191A1B1C1D1E1Full, 0x1011121314151617ull)));
    c->str(vr, SPU_OFF_128(gpr, op.rt));
    c->mov(arg1->x(), *cpu);
    c->add(addr->x(), addr->x(), arg1->x());
    c->mov(qw1->x(), asmjit::Imm(s64(SPU_OFF_128(gpr, op.rt).offset()))); c->add(addr->x(), addr->x(), qw1->x());
    c->mov(qw1->w(), asmjit::Imm(3));
    c->strb(qw1->w(), asmjit::arm::Mem(*addr));
}

void spu_recompiler::CHX(spu_opcode_t op)
{
    // rt = const 0x10..0x1f; rt._u16[(~(rb._u32[3] + ra._u32[3]) & 0xe) >> 1] = 0x0203
    c->ldr(addr->w(), SPU_OFF_32(gpr, op.rb, &v128::_u32, 3));
    c->ldr(arg1->w(), SPU_OFF_32(gpr, op.ra, &v128::_u32, 3));
    c->add(addr->w(), addr->w(), arg1->w());
    c->mvn(addr->w(), addr->w());
    c->and_(addr->w(), addr->w(), asmjit::Imm(0xe));

    const VLink& vr = VAlloc();
    c->ldr(vr, VConst(v128::from64(0x18191A1B1C1D1E1Full, 0x1011121314151617ull)));
    c->str(vr, SPU_OFF_128(gpr, op.rt));
    c->mov(arg1->x(), *cpu);
    c->add(addr->x(), addr->x(), arg1->x());
    c->mov(qw1->x(), asmjit::Imm(s64(SPU_OFF_128(gpr, op.rt).offset()))); c->add(addr->x(), addr->x(), qw1->x());
    c->mov(qw1->w(), asmjit::Imm(0x0203));
    c->strh(qw1->w(), asmjit::arm::Mem(*addr));
}

void spu_recompiler::CWX(spu_opcode_t op)
{
    // rt = const 0x10..0x1f; rt._u32[(~(rb._u32[3] + ra._u32[3]) & 0xc) >> 2] = 0x00010203
    c->ldr(addr->w(), SPU_OFF_32(gpr, op.rb, &v128::_u32, 3));
    c->ldr(arg1->w(), SPU_OFF_32(gpr, op.ra, &v128::_u32, 3));
    c->add(addr->w(), addr->w(), arg1->w());
    c->mvn(addr->w(), addr->w());
    c->and_(addr->w(), addr->w(), asmjit::Imm(0xc));

    const VLink& vr = VAlloc();
    c->ldr(vr, VConst(v128::from64(0x18191A1B1C1D1E1Full, 0x1011121314151617ull)));
    c->str(vr, SPU_OFF_128(gpr, op.rt));
    c->mov(arg1->x(), *cpu);
    c->add(addr->x(), addr->x(), arg1->x());
    c->mov(qw1->x(), asmjit::Imm(s64(SPU_OFF_128(gpr, op.rt).offset()))); c->add(addr->x(), addr->x(), qw1->x());
    c->mov(qw1->w(), asmjit::Imm(0x00010203));
    c->str(qw1->w(), asmjit::arm::Mem(*addr));
}

void spu_recompiler::CDX(spu_opcode_t op)
{
    // rt = const 0x10..0x1f; rt._u64[(~(rb._u32[3] + ra._u32[3]) & 0x8) >> 3] = 0x0001020304050607
    c->ldr(addr->w(), SPU_OFF_32(gpr, op.rb, &v128::_u32, 3));
    c->ldr(arg1->w(), SPU_OFF_32(gpr, op.ra, &v128::_u32, 3));
    c->add(addr->w(), addr->w(), arg1->w());
    c->mvn(addr->w(), addr->w());
    c->and_(addr->w(), addr->w(), asmjit::Imm(0x8));

    const VLink& vr = VAlloc();
    c->ldr(vr, VConst(v128::from64(0x18191A1B1C1D1E1Full, 0x1011121314151617ull)));
    c->str(vr, SPU_OFF_128(gpr, op.rt));
    c->mov(arg1->x(), *cpu);
    c->add(addr->x(), addr->x(), arg1->x());
    c->mov(qw1->x(), asmjit::Imm(s64(SPU_OFF_128(gpr, op.rt).offset()))); c->add(addr->x(), addr->x(), qw1->x());
    c->mov(qw1->x(), asmjit::Imm(0x0001020304050607ull));
    c->str(qw1->x(), asmjit::arm::Mem(*addr));
}

void spu_recompiler::ROTQBI(spu_opcode_t op)
{
    // 128-bit rotate left by dynamic n = rb._s32[3] & 7 (vector ushl with dup'ed shift counts)
    const VLink& va = VGet(op.ra, VType::Int);
    const VLink& vn = VAlloc();
    const VLink& vm = VAlloc();
    const VLink& v1 = VAlloc();
    const VLink& v2 = VAlloc();
    c->ldr(addr->w(), SPU_OFF_32(gpr, op.rb, &v128::_s32, 3));
    c->and_(addr->w(), addr->w(), asmjit::Imm(0x7)); // n
    c->dup(vn.d2(), addr->x());
    c->ushl(v1.d2(), va.d2(), vn.d2()); // va << n
    c->ext(v2.b16(), va.b16(), va.b16(), 8); // swap halves
    c->mov(qw0->x(), asmjit::Imm(64));
    c->dup(vm.d2(), qw0->x());
    c->sub(vm.d2(), vm.d2(), vn.d2()); // 64 - n
    c->neg(vm.d2(), vm.d2()); // -(64-n)
    c->ushl(v2.d2(), v2.d2(), vm.d2()); // swapped >> (64-n)
    c->orr(va.b16(), v1.b16(), v2.b16());
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::ROTQMBI(spu_opcode_t op)
{
    // lane0 = (lo >> n) | (hi << (64-n)); lane1 = hi >> n; n = (0 - rb) & 7
    const VLink& va = VGet(op.ra, VType::Int);
    const VLink& vn = VAlloc();
    const VLink& vm = VAlloc();
    const VLink& v1 = VAlloc();
    const VLink& v2 = VAlloc();
    const VLink& mask = VAlloc();
    c->ldr(addr->w(), SPU_OFF_32(gpr, op.rb, &v128::_s32, 3));
    c->neg(addr->w(), addr->w());
    c->and_(addr->w(), addr->w(), asmjit::Imm(0x7)); // n
    c->dup(vn.d2(), addr->x());
    c->neg(vn.d2(), vn.d2()); // -n
    c->ushl(v1.d2(), va.d2(), vn.d2()); // va >> n
    c->ext(v2.b16(), va.b16(), va.b16(), 8); // swap halves
    c->mov(qw0->x(), asmjit::Imm(64));
    c->dup(vm.d2(), qw0->x());
    c->add(vm.d2(), vm.d2(), vn.d2()); // 64 + (-n) = 64 - n
    c->ldr(mask, VConst(v128::from64(0xFFFFFFFFFFFFFFFFull, 0))); // lane1 keep, lane0 clear
    c->ushl(v2.d2(), v2.d2(), vm.d2()); // swapped << (64-n)
    c->and_(v2.b16(), v2.b16(), mask.b16()); // lane0 only
    c->orr(va.b16(), v1.b16(), v2.b16());
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::SHLQBI(spu_opcode_t op)
{
    // 128-bit logical shift left, zero fill: lane0 = lo << n, lane1 = (hi << n) | (lo >> (64-n))
    const VLink& va = VGet(op.ra, VType::Int);
    const VLink& vn = VAlloc();
    const VLink& vm = VAlloc();
    const VLink& v1 = VAlloc();
    const VLink& v2 = VAlloc();
    const VLink& mask = VAlloc();
    c->ldr(addr->w(), SPU_OFF_32(gpr, op.rb, &v128::_s32, 3));
    c->and_(addr->w(), addr->w(), asmjit::Imm(0x7)); // n
    c->dup(vn.d2(), addr->x());
    c->ushl(v1.d2(), va.d2(), vn.d2()); // va << n
    c->ext(v2.b16(), va.b16(), va.b16(), 8); // swap halves
    c->mov(qw0->x(), asmjit::Imm(64));
    c->dup(vm.d2(), qw0->x());
    c->sub(vm.d2(), vm.d2(), vn.d2()); // 64 - n
    c->neg(vm.d2(), vm.d2()); // -(64-n)
    c->ushl(v2.d2(), v2.d2(), vm.d2()); // swapped >> (64-n)
    c->ldr(mask, VConst(v128::from64(0, 0xFFFFFFFFFFFFFFFFull))); // lane1 keep, lane0 clear
    c->and_(v2.b16(), v2.b16(), mask.b16()); // lane1 only
    c->orr(va.b16(), v1.b16(), v2.b16());
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::ROTQBY(spu_opcode_t op)
{
    // rt = ra rotated right by n bytes, n = rb._u32[3] & 0xf
    const VLink& va = VGet(op.ra, VType::Int);
    const VLink& vidx = VAlloc();
    const VLink& vn = VAlloc();
    const VLink& vt = VAlloc();
    const asmjit::a64::VecV vc = VAlloc();
    c->ldr(addr->w(), SPU_OFF_32(gpr, op.rb, &v128::_u32, 3));
    c->and_(addr->w(), addr->w(), asmjit::Imm(0xf)); // n
    c->dup(vn.b16(), addr->w());
    const v128 base{[]() { v128 r{}; for (u32 i = 0; i < 16; i++) r._u8[i] = static_cast<u8>(i); return r; }()};
    c->ldr(vidx, VConst(base));
    c->sub(vidx.b16(), vidx.b16(), vn.b16());
    c->ldr(vc, VConst(v128::from8p(0xf)));
    c->and_(vidx.b16(), vidx.b16(), vc.b16());
    c->tbl(vt.b16(), va.b16(), vidx.b16());
    c->str(vt, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::ROTQMBY(spu_opcode_t op)
{
    // rt = ra shifted right by n bytes (zero fill), n = (0 - rb._u32[3]) & 0x1f
    const VLink& va = VGet(op.ra, VType::Int);
    const VLink& vidx = VAlloc();
    const VLink& vn = VAlloc();
    const VLink& vt = VAlloc();
    c->ldr(addr->w(), SPU_OFF_32(gpr, op.rb, &v128::_u32, 3));
    c->neg(addr->w(), addr->w());
    c->and_(addr->w(), addr->w(), asmjit::Imm(0x1f)); // n
    c->dup(vn.b16(), addr->w());
    const v128 base{[]() { v128 r{}; for (u32 i = 0; i < 16; i++) r._u8[i] = static_cast<u8>(i); return r; }()};
    c->ldr(vidx, VConst(base));
    c->add(vidx.b16(), vidx.b16(), vn.b16()); // i + n (> 15 -> tbl yields 0)
    c->tbl(vt.b16(), va.b16(), vidx.b16());
    c->str(vt, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::SHLQBY(spu_opcode_t op)
{
    // rt = ra shifted left by n bytes (zero fill), n = rb._u32[3] & 0x1f
    // interpreter: buf[3]{0,0,a}, load from 32-n -> result[j] = (j>=n) ? a[j-n] : 0
    const VLink& va = VGet(op.ra, VType::Int);
    const VLink& vidx = VAlloc();
    const VLink& vn = VAlloc();
    const VLink& vt = VAlloc();
    c->ldr(addr->w(), SPU_OFF_32(gpr, op.rb, &v128::_u32, 3));
    c->and_(addr->w(), addr->w(), asmjit::Imm(0x1f)); // n
    c->dup(vn.b16(), addr->w());
    const v128 base{[]() { v128 r{}; for (u32 i = 0; i < 16; i++) r._u8[i] = static_cast<u8>(i); return r; }()};
    c->ldr(vidx, VConst(base));
    c->sub(vidx.b16(), vidx.b16(), vn.b16()); // i - n (negative -> tbl yields 0)
    c->tbl(vt.b16(), va.b16(), vidx.b16());
    c->str(vt, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::ORX(spu_opcode_t op)
{
    // OR across all 4 words; result in word 3, words 0-2 = 0 (matches x86 pslldq 12)
    c->ldr(qw0->w(), SPU_OFF_32(gpr, op.ra, &v128::_u32, 0));
    c->ldr(qw1->w(), SPU_OFF_32(gpr, op.ra, &v128::_u32, 1));
    c->orr(qw0->w(), qw0->w(), qw1->w());
    c->ldr(qw1->w(), SPU_OFF_32(gpr, op.ra, &v128::_u32, 2));
    c->orr(qw0->w(), qw0->w(), qw1->w());
    c->ldr(qw1->w(), SPU_OFF_32(gpr, op.ra, &v128::_u32, 3));
    c->orr(qw0->w(), qw0->w(), qw1->w());
    const VLink& vz = VAlloc();
    c->eor(vz.b16(), vz.b16(), vz.b16());
    c->str(vz, SPU_OFF_128(gpr, op.rt));
    c->str(qw0->w(), SPU_OFF_32(gpr, op.rt, &v128::_u32, 3));
}

void spu_recompiler::CBD(spu_opcode_t op)
{
    // rt = const 0x10..0x1f; rt._u8[~(i7 + ra._u32[3]) & 0xf] = 0x03
    c->ldr(addr->w(), SPU_OFF_32(gpr, op.ra, &v128::_u32, 3));
    if (op.i7) c->add(addr->w(), addr->w(), asmjit::Imm(static_cast<u32>(op.i7)));
    c->mvn(addr->w(), addr->w());
    c->and_(addr->w(), addr->w(), asmjit::Imm(0xf));

    const VLink& vr = VAlloc();
    c->ldr(vr, VConst(v128::from64(0x18191A1B1C1D1E1Full, 0x1011121314151617ull)));
    c->str(vr, SPU_OFF_128(gpr, op.rt));
    c->mov(arg1->x(), *cpu);
    c->add(addr->x(), addr->x(), arg1->x());
    c->mov(qw1->x(), asmjit::Imm(s64(SPU_OFF_128(gpr, op.rt).offset()))); c->add(addr->x(), addr->x(), qw1->x());
    c->mov(qw1->w(), asmjit::Imm(3));
    c->strb(qw1->w(), asmjit::arm::Mem(*addr));
}

void spu_recompiler::CHD(spu_opcode_t op)
{
    // rt = const 0x10..0x1f; rt._u16[(~(i7 + ra._u32[3]) & 0xe) >> 1] = 0x0203
    c->ldr(addr->w(), SPU_OFF_32(gpr, op.ra, &v128::_u32, 3));
    if (op.i7) c->add(addr->w(), addr->w(), asmjit::Imm(static_cast<u32>(op.i7)));
    c->mvn(addr->w(), addr->w());
    c->and_(addr->w(), addr->w(), asmjit::Imm(0xe));

    const VLink& vr = VAlloc();
    c->ldr(vr, VConst(v128::from64(0x18191A1B1C1D1E1Full, 0x1011121314151617ull)));
    c->str(vr, SPU_OFF_128(gpr, op.rt));
    c->mov(arg1->x(), *cpu);
    c->add(addr->x(), addr->x(), arg1->x());
    c->mov(qw1->x(), asmjit::Imm(s64(SPU_OFF_128(gpr, op.rt).offset()))); c->add(addr->x(), addr->x(), qw1->x());
    c->mov(qw1->w(), asmjit::Imm(0x0203));
    c->strh(qw1->w(), asmjit::arm::Mem(*addr));
}

void spu_recompiler::CWD(spu_opcode_t op)
{
    // rt = const 0x10..0x1f; rt._u32[(~(i7 + ra._u32[3]) & 0xc) >> 2] = 0x00010203
    c->ldr(addr->w(), SPU_OFF_32(gpr, op.ra, &v128::_u32, 3));
    if (op.i7) c->add(addr->w(), addr->w(), asmjit::Imm(static_cast<u32>(op.i7)));
    c->mvn(addr->w(), addr->w());
    c->and_(addr->w(), addr->w(), asmjit::Imm(0xc));

    const VLink& vr = VAlloc();
    c->ldr(vr, VConst(v128::from64(0x18191A1B1C1D1E1Full, 0x1011121314151617ull)));
    c->str(vr, SPU_OFF_128(gpr, op.rt));
    c->mov(arg1->x(), *cpu);
    c->add(addr->x(), addr->x(), arg1->x());
    c->mov(qw1->x(), asmjit::Imm(s64(SPU_OFF_128(gpr, op.rt).offset()))); c->add(addr->x(), addr->x(), qw1->x());
    c->mov(qw1->w(), asmjit::Imm(0x00010203));
    c->str(qw1->w(), asmjit::arm::Mem(*addr));
}

void spu_recompiler::CDD(spu_opcode_t op)
{
    // rt = const 0x10..0x1f; rt._u64[(~(i7 + ra._u32[3]) & 0x8) >> 3] = 0x0001020304050607
    c->ldr(addr->w(), SPU_OFF_32(gpr, op.ra, &v128::_u32, 3));
    if (op.i7) c->add(addr->w(), addr->w(), asmjit::Imm(static_cast<u32>(op.i7)));
    c->mvn(addr->w(), addr->w());
    c->and_(addr->w(), addr->w(), asmjit::Imm(0x8));

    const VLink& vr = VAlloc();
    c->ldr(vr, VConst(v128::from64(0x18191A1B1C1D1E1Full, 0x1011121314151617ull)));
    c->str(vr, SPU_OFF_128(gpr, op.rt));
    c->mov(arg1->x(), *cpu);
    c->add(addr->x(), addr->x(), arg1->x());
    c->mov(qw1->x(), asmjit::Imm(s64(SPU_OFF_128(gpr, op.rt).offset()))); c->add(addr->x(), addr->x(), qw1->x());
    c->mov(qw1->x(), asmjit::Imm(0x0001020304050607ull));
    c->str(qw1->x(), asmjit::arm::Mem(*addr));
}

void spu_recompiler::ROTQBII(spu_opcode_t op)
{
    // 128-bit rotate left by n = i7 & 7: (a << n) | (a >> (64-n)) across lanes
    const int n = op.i7 & 0x7;
    const VLink& va = VGet(op.ra, VType::Int);
    if (n == 0)
    {
        c->str(va, SPU_OFF_128(gpr, op.rt));
        return;
    }
    const VLink& v1 = VAlloc();
    const VLink& v2 = VAlloc();
    c->shl(v1.d2(), va.d2(), asmjit::Imm(n));
    c->ushr(v2.d2(), va.d2(), asmjit::Imm(64 - n));
    c->ext(v2.b16(), v2.b16(), v2.b16(), 8); // swap 64-bit halves
    c->orr(va.b16(), v1.b16(), v2.b16());
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::ROTQMBII(spu_opcode_t op)
{
    // x86: lane0 = (a.lo >> n) | (a.hi << (64-n)); lane1 = a.hi >> n (no wrap into lane1)
    const int n = (0 - op.i7) & 0x7;
    const VLink& va = VGet(op.ra, VType::Int);
    if (n == 0)
    {
        c->str(va, SPU_OFF_128(gpr, op.rt));
        return;
    }
    // Compute via memory: lo at off, hi at off+8 (NEON lane0 = lowest memory)
    c->str(va, SPU_OFF_128(gpr, op.rt));
    const s64 off = s64(SPU_OFF_128(gpr, op.rt).offset());
    c->ldr(qw0->x(), asmjit::arm::Mem(*cpu, off)); // a.lo
    c->ldr(qw1->x(), asmjit::arm::Mem(*cpu, off + 8)); // a.hi
    c->lsr(arg1->x(), qw0->x(), asmjit::Imm(n)); // lo >> n
    c->lsl(addr->x(), qw1->x(), asmjit::Imm(64 - n)); // hi << (64-n)
    c->orr(arg1->x(), arg1->x(), addr->x()); // lane0
    c->lsr(addr->x(), qw1->x(), asmjit::Imm(n)); // lane1 = hi >> n
    c->str(arg1->x(), asmjit::arm::Mem(*cpu, off));
    c->str(addr->x(), asmjit::arm::Mem(*cpu, off + 8));
}

void spu_recompiler::SHLQBII(spu_opcode_t op)
{
    // 128-bit logical shift left by n = i7 & 7, zero fill
    const int n = op.i7 & 0x7;
    const VLink& va = VGet(op.ra, VType::Int);
    if (n == 0)
    {
        c->str(va, SPU_OFF_128(gpr, op.rt));
        return;
    }
    const VLink& v1 = VAlloc();
    const VLink& v2 = VAlloc();
    const VLink& vz = VAlloc();
    c->eor(vz.b16(), vz.b16(), vz.b16());
    c->shl(v1.d2(), va.d2(), asmjit::Imm(n));
    c->ushr(v2.d2(), va.d2(), asmjit::Imm(64 - n));
    c->ext(v2.b16(), vz.b16(), v2.b16(), 8); // [0, lo >> (64-n)]
    c->orr(va.b16(), v1.b16(), v2.b16());
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::ROTQBYI(spu_opcode_t op)
{
    // Rotate the quadword right by (i7 & 0xf) bytes. EXT(Vd,Vn,Vm,idx) extracts from
    // concat = Vm(high):Vn(low); with Vn=Vm=va this is a byte rotate.
    const int s = op.i7 & 0xf;
    const VLink& va = VGet(op.ra, VType::Int);
    if (s == 0)
    {
        c->str(va, SPU_OFF_128(gpr, op.rt));
        return;
    }
    const VLink& vt = VAlloc();
    c->ext(vt.b16(), va.b16(), va.b16(), 16 - s);
    c->str(vt, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::ROTQMBYI(spu_opcode_t op)
{
    // Byte shift right by ((0 - i7) & 0x1f) bytes, zero fill (matches x86 psrldq).
    // EXT concat = Vm(high):Vn(low); for psrldq(va,s): Vn=va (low), Vm=zero (high), idx=s.
    const int s = (0 - op.i7) & 0x1f;
    const VLink& va = VGet(op.ra, VType::Int);
    if (s == 0)
    {
        c->str(va, SPU_OFF_128(gpr, op.rt));
        return;
    }
    const VLink& vz = VAlloc();
    c->eor(vz.b16(), vz.b16(), vz.b16());
    if (s >= 16)
    {
        c->str(vz, SPU_OFF_128(gpr, op.rt));
        return;
    }
    const VLink& vt = VAlloc();
    c->ext(vt.b16(), va.b16(), vz.b16(), s);
    c->str(vt, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::SHLQBYI(spu_opcode_t op)
{
    // Byte shift left by (i7 & 0x1f) bytes, zero fill (matches x86 pslldq).
    // EXT concat = Vm(high):Vn(low); for pslldq(va,s): Vn=zero (low), Vm=va (high), idx=16-s.
    const int s = op.i7 & 0x1f;
    const VLink& va = VGet(op.ra, VType::Int);
    if (s == 0)
    {
        c->str(va, SPU_OFF_128(gpr, op.rt));
        return;
    }
    const VLink& vz = VAlloc();
    c->eor(vz.b16(), vz.b16(), vz.b16());
    if (s >= 16)
    {
        c->str(vz, SPU_OFF_128(gpr, op.rt));
        return;
    }
    const VLink& vt = VAlloc();
    c->ext(vt.b16(), vz.b16(), va.b16(), 16 - s);
    c->str(vt, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::NOP(spu_opcode_t op)
{
    // No operation
}

void spu_recompiler::CGT(spu_opcode_t op)
{
    const VLink& va = VGet(op.ra, VType::Int);
    const VLink& vb = VGet(op.rb, VType::Int);
    c->cmgt(va.s4(), va.s4(), vb.s4());
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::XOR(spu_opcode_t op)
{
    const VLink& va = VGet(op.ra, VType::Int);
    const VLink& vb = VGet(op.rb, VType::Int);
    c->eor(va.b16(), va.b16(), vb.b16());
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::CGTH(spu_opcode_t op)
{
    const VLink& va = VGet(op.ra, VType::Int);
    const VLink& vb = VGet(op.rb, VType::Int);
    c->cmgt(va.h8(), va.h8(), vb.h8());
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::EQV(spu_opcode_t op)
{
    const VLink& va = VGet(op.ra, VType::Int);
    const VLink& vb = VGet(op.rb, VType::Int);
    c->eor(va.b16(), va.b16(), vb.b16());
    c->mvn(va.b16(), va.b16());
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::CGTB(spu_opcode_t op)
{
    const VLink& va = VGet(op.ra, VType::Int);
    const VLink& vb = VGet(op.rb, VType::Int);
    c->cmgt(va.b16(), va.b16(), vb.b16());
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::SUMB(spu_opcode_t op)
{
    // rt: for each 32-bit word, (rb 4-byte sum << 16) | (ra 4-byte sum)
    const VLink& va = VGet(op.ra, VType::Int);
    const VLink& vb = VGet(op.rb, VType::Int);
    const VLink& v1 = VAlloc();
    const VLink& v2 = VAlloc();
    c->uaddlp(v1.h8(), va.b16()); // pairwise byte sums, zero-extended
    c->uaddlp(v1.s4(), v1.h8()); // 4-byte sum per 32-bit lane
    c->uaddlp(v2.h8(), vb.b16());
    c->uaddlp(v2.s4(), v2.h8());
    c->shl(v2.s4(), v2.s4(), asmjit::Imm(16));
    c->orr(v1.b16(), v1.b16(), v2.b16());
    c->str(v1, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::HGT(spu_opcode_t op)
{
    // Halt if greater than (signed, word 3): emit the halt magic write (matches x86/LLVM)
    c->ldr(qw0->w(), SPU_OFF_32(gpr, op.ra, &v128::_u32, 3));
    c->ldr(qw1->w(), SPU_OFF_32(gpr, op.rb, &v128::_u32, 3));
    c->cmp(qw0->w(), qw1->w());

    asmjit::Label label = c->newLabel();
    c->b_gt(label);
    a64_halt_body(label, op);
}

void spu_recompiler::CLZ(spu_opcode_t op)
{
    const VLink& va = VGet(op.ra, VType::Int);
    c->clz(va.s4(), va.s4());
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::XSWD(spu_opcode_t op)
{
    // sign-extend word 0 -> dword 0, word 2 -> dword 1
    c->ldrsw(*qw0, SPU_OFF_32(gpr, op.ra, &v128::_s32, 0));
    c->str(*qw0, SPU_OFF_64(gpr, op.rt, &v128::_s64, 0));
    c->ldrsw(*qw0, SPU_OFF_32(gpr, op.ra, &v128::_s32, 2));
    c->str(*qw0, SPU_OFF_64(gpr, op.rt, &v128::_s64, 1));
}

void spu_recompiler::XSHW(spu_opcode_t op)
{
    const VLink& va = VGet(op.ra, VType::Int);
    c->shl(va.s4(), va.s4(), asmjit::Imm(16));
    c->sshr(va.s4(), va.s4(), asmjit::Imm(16));
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::CNTB(spu_opcode_t op)
{
    const VLink& va = VGet(op.ra, VType::Int);
    c->cnt(va.b16(), va.b16());
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::XSBH(spu_opcode_t op)
{
    const VLink& va = VGet(op.ra, VType::Int);
    c->shl(va.h8(), va.h8(), asmjit::Imm(8));
    c->sshr(va.h8(), va.h8(), asmjit::Imm(8));
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::CLGT(spu_opcode_t op)
{
    const VLink& va = VGet(op.ra, VType::Int);
    const VLink& vb = VGet(op.rb, VType::Int);
    c->cmhi(va.s4(), va.s4(), vb.s4());
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::ANDC(spu_opcode_t op)
{
    const VLink& va = VGet(op.ra, VType::Int);
    const VLink& vb = VGet(op.rb, VType::Int);
    c->bic(va.b16(), va.b16(), vb.b16());
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::FCGT(spu_opcode_t op)
{
    const VLink& va = VGet(op.ra, VType::Float);
    const VLink& vb = VGet(op.rb, VType::Float);
    // SPU DAZ semantics: denormal inputs compare as zero (x86 relies on MXCSR DAZ;
    // NEON compares denormals by real value which breaks SPU/FTZ behaviour)
    {
        const VLink& vm = VAlloc();
        const asmjit::a64::VecV& vc_sign = VAlloc();
        const asmjit::a64::VecV& vc_th = VAlloc();
        c->ldr(vc_sign, VConst(v128::from32p(0x7fffffff)));
        c->ldr(vc_th, VConst(v128::from32p(0x007fffff)));
        // va: mask = (va & 0x7fffffff) > 0x007fffff (exponent non-zero); va &= mask
        c->and_(vm.b16(), va.b16(), vc_sign.b16());
        c->cmhi(vm.s4(), vm.s4(), vc_th.s4());
        c->and_(va.b16(), va.b16(), vm.b16());
        // vb: same flush
        c->and_(vm.b16(), vb.b16(), vc_sign.b16());
        c->cmhi(vm.s4(), vm.s4(), vc_th.s4());
        c->and_(vb.b16(), vb.b16(), vm.b16());
    }
    c->fcmgt(va.s4(), va.s4(), vb.s4());
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::DFCGT(spu_opcode_t op)
{
    UNK(op);
}

void spu_recompiler::FA(spu_opcode_t op)
{
    const VLink& va = VGet(op.ra, VType::Float);
    const VLink& vb = VGet(op.rb, VType::Float);
    c->fadd(va.s4(), va.s4(), vb.s4());
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::FS(spu_opcode_t op)
{
    const VLink& va = VGet(op.ra, VType::Float);
    const VLink& vb = VGet(op.rb, VType::Float);
    c->fsub(va.s4(), va.s4(), vb.s4());
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::FM(spu_opcode_t op)
{
    const VLink& va = VGet(op.ra, VType::Float);
    const VLink& vb = VGet(op.rb, VType::Float);
    c->fmul(va.s4(), va.s4(), vb.s4());
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::CLGTH(spu_opcode_t op)
{
    const VLink& va = VGet(op.ra, VType::Int);
    const VLink& vb = VGet(op.rb, VType::Int);
    c->cmhi(va.h8(), va.h8(), vb.h8());
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::ORC(spu_opcode_t op)
{
    const VLink& va = VGet(op.ra, VType::Int);
    const VLink& vb = VGet(op.rb, VType::Int);
    c->orn(va.b16(), va.b16(), vb.b16());
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::FCMGT(spu_opcode_t op)
{
    const VLink& va = VGet(op.ra, VType::Float);
    const VLink& vb = VGet(op.rb, VType::Float);
    // SPU DAZ semantics: denormal inputs compare as zero
    {
        const VLink& vm = VAlloc();
        const asmjit::a64::VecV& vc_sign = VAlloc();
        const asmjit::a64::VecV& vc_th = VAlloc();
        c->ldr(vc_sign, VConst(v128::from32p(0x7fffffff)));
        c->ldr(vc_th, VConst(v128::from32p(0x007fffff)));
        c->and_(vm.b16(), va.b16(), vc_sign.b16());
        c->cmhi(vm.s4(), vm.s4(), vc_th.s4());
        c->and_(va.b16(), va.b16(), vm.b16());
        c->and_(vm.b16(), vb.b16(), vc_sign.b16());
        c->cmhi(vm.s4(), vm.s4(), vc_th.s4());
        c->and_(vb.b16(), vb.b16(), vm.b16());
    }
    c->facgt(va.s4(), va.s4(), vb.s4());
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::DFCMGT(spu_opcode_t op)
{
    const VLink& va = VGet(op.ra, VType::Double);
    const VLink& vb = VGet(op.rb, VType::Double);
    c->facgt(va.d2(), va.d2(), vb.d2());
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::DFA(spu_opcode_t op)
{
    const VLink& va = VGet(op.ra, VType::Double);
    const VLink& vb = VGet(op.rb, VType::Double);
    c->fadd(va.d2(), va.d2(), vb.d2());
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::DFS(spu_opcode_t op)
{
    const VLink& va = VGet(op.ra, VType::Double);
    const VLink& vb = VGet(op.rb, VType::Double);
    c->fsub(va.d2(), va.d2(), vb.d2());
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::DFM(spu_opcode_t op)
{
    const VLink& va = VGet(op.ra, VType::Double);
    const VLink& vb = VGet(op.rb, VType::Double);
    c->fmul(va.d2(), va.d2(), vb.d2());
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::CLGTB(spu_opcode_t op)
{
    const VLink& va = VGet(op.ra, VType::Int);
    const VLink& vb = VGet(op.rb, VType::Int);
    c->cmhi(va.b16(), va.b16(), vb.b16());
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::HLGT(spu_opcode_t op)
{
    // Halt if greater than (unsigned, word 3): emit the halt magic write (matches x86/LLVM)
    c->ldr(qw0->w(), SPU_OFF_32(gpr, op.ra, &v128::_u32, 3));
    c->ldr(qw1->w(), SPU_OFF_32(gpr, op.rb, &v128::_u32, 3));
    c->cmp(qw0->w(), qw1->w());

    asmjit::Label label = c->newLabel();
    c->b_hi(label);
    a64_halt_body(label, op);
}

void spu_recompiler::DFMA(spu_opcode_t op)
{
    // rt += ra*rb (note: rt == rc field in this encoding)
    const VLink& va = VGet(op.ra, VType::Double);
    const VLink& vb = VGet(op.rb, VType::Double);
    const VLink& vt = VGet(op.rt, VType::Double);
    c->fmul(va.d2(), va.d2(), vb.d2());
    c->fadd(vt.d2(), vt.d2(), va.d2());
    c->str(vt, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::DFMS(spu_opcode_t op)
{
    // rt = ra*rb - rt
    const VLink& va = VGet(op.ra, VType::Double);
    const VLink& vb = VGet(op.rb, VType::Double);
    const VLink& vt = VGet(op.rt, VType::Double);
    c->fmul(va.d2(), va.d2(), vb.d2());
    c->fsub(va.d2(), va.d2(), vt.d2());
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::DFNMS(spu_opcode_t op)
{
    // rt = rt - ra*rb
    const VLink& va = VGet(op.ra, VType::Double);
    const VLink& vb = VGet(op.rb, VType::Double);
    const VLink& vt = VGet(op.rt, VType::Double);
    c->fmul(va.d2(), va.d2(), vb.d2());
    c->fsub(vt.d2(), vt.d2(), va.d2());
    c->str(vt, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::DFNMA(spu_opcode_t op)
{
    // rt = -(ra*rb + rt)
    const VLink& va = VGet(op.ra, VType::Double);
    const VLink& vb = VGet(op.rb, VType::Double);
    const VLink& vt = VGet(op.rt, VType::Double);
    c->fmul(va.d2(), va.d2(), vb.d2());
    c->fadd(va.d2(), va.d2(), vt.d2());
    c->fneg(va.d2(), va.d2());
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::CEQ(spu_opcode_t op)
{
    const VLink& va = VGet(op.ra, VType::Int);
    const VLink& vb = VGet(op.rb, VType::Int);
    c->cmeq(va.s4(), va.s4(), vb.s4());
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::MPYHHU(spu_opcode_t op)
{
    // (pmulhuw(a,b) & 0xffff0000) | (pmullw(a,b) >> 16):
    // word i = full 32-bit product of odd halfwords h[2i+1] (unsigned)
    // smull/umull (non-2 form) fail to emit in this asmjit build; use
    // ext-swapped inputs + smull2/umull2 to compute the low-half products.
    const VLink& va = VGet(op.ra, VType::Int);
    const VLink& vb = VGet(op.rb, VType::Int);
    const VLink& v1 = VAlloc(); // high-half products [E2, O2, E3, O3]
    const VLink& v2 = VAlloc(); // low-half products [E0, O0, E1, O1]
    const VLink& v3 = VAlloc();
    const VLink& va2 = VAlloc();
    const VLink& vb2 = VAlloc();
    c->ext(va2.b16(), va.b16(), va.b16(), 8); // swap halves: [h4..h7, h0..h3]
    c->ext(vb2.b16(), vb.b16(), vb.b16(), 8);
    c->umull2(v2.s4(), va2.h8(), vb2.h8());   // low-half products [E0, O0, E1, O1]
    c->umull2(v1.s4(), va.h8(), vb.h8());     // high-half products [E2, O2, E3, O3]
    c->uzp2(v3.s4(), v2.s4(), v1.s4());       // odd products: [O0, O1, O2, O3]
    c->str(v3, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::ADDX(spu_opcode_t op)
{
    // rt = ra + rb + (old rt & 1)
    const VLink& va = VGet(op.ra, VType::Int);
    const VLink& vb = VGet(op.rb, VType::Int);
    const VLink& vt = VGet(op.rt, VType::Int);
    const asmjit::a64::VecV& vc_one = VAlloc();
    c->ldr(vc_one, VConst(v128::from32p(1)));
    c->and_(vt.b16(), vt.b16(), vc_one.b16());
    c->add(va.s4(), va.s4(), vb.s4());
    c->add(va.s4(), va.s4(), vt.s4());
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::SFX(spu_opcode_t op)
{
    // rt = rb - ra - (~old rt & 1)
    const VLink& va = VGet(op.ra, VType::Int);
    const VLink& vb = VGet(op.rb, VType::Int);
    const VLink& vt = VGet(op.rt, VType::Int);
    const asmjit::a64::VecV& vc_one = VAlloc();
    c->ldr(vc_one, VConst(v128::from32p(1)));
    c->bic(vt.b16(), vc_one.b16(), vt.b16());
    c->sub(vb.s4(), vb.s4(), va.s4());
    c->sub(vb.s4(), vb.s4(), vt.s4());
    c->str(vb, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::CGX(spu_opcode_t op)
{
    // rt[i] = (carry_in + ra[i] + rb[i]) >> 32, carry_in = old rt[i] & 1
    // carry-out = (a+b wraps) || (a+b+cin wraps), detected via wrapping adds
    const VLink& va = VGet(op.ra, VType::Int);
    const VLink& vb = VGet(op.rb, VType::Int);
    const VLink& vt = VGet(op.rt, VType::Int); // old rt: carry-in source
    const VLink& v1 = VAlloc();
    const VLink& v2 = VAlloc();
    const VLink& v3 = VAlloc();
    const asmjit::a64::VecV vc = VAlloc();
    c->ldr(vc, VConst(v128::from32p(1)));
    c->and_(vt.b16(), vt.b16(), vc.b16()); // carry-in (0 or 1 per lane)
    c->add(v1.s4(), va.s4(), vb.s4());     // sum (wrapping)
    c->cmhi(v2.s4(), va.s4(), v1.s4());    // a+b overflow
    c->add(v3.s4(), v1.s4(), vt.s4());     // sum + carry-in (wrapping)
    c->cmhi(v1.s4(), v1.s4(), v3.s4());    // sum+cin < sum -> carry-out
    c->orr(v2.b16(), v2.b16(), v1.b16());
    c->ushr(v2.s4(), v2.s4(), asmjit::Imm(31)); // 0/1
    c->str(v2, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::BGX(spu_opcode_t op)
{
    // rt[i] = (u64(rb[i]) - ra[i] - (1 - (old rt[i] & 1))) >= 0
    // borrow-out = (rb < ra) || (rb - ra == 0 && borrow_in); rt = !borrow
    const VLink& va = VGet(op.ra, VType::Int);
    const VLink& vb = VGet(op.rb, VType::Int);
    const VLink& vt = VGet(op.rt, VType::Int); // old rt: borrow-in source
    const VLink& v1 = VAlloc();
    const VLink& v2 = VAlloc();
    const VLink& v3 = VAlloc();
    const VLink& vz = VAlloc();
    const asmjit::a64::VecV vc = VAlloc();
    c->ldr(vc, VConst(v128::from32p(1)));
    c->and_(vt.b16(), vt.b16(), vc.b16()); // borrow-in (0 or 1 per lane)
    c->sub(v1.s4(), vb.s4(), va.s4());     // wrapping difference
    c->cmhi(v2.s4(), va.s4(), vb.s4());    // ra > rb (underflow of rb-ra)
    c->eor(vz.b16(), vz.b16(), vz.b16());
    c->cmeq(v3.s4(), v1.s4(), vz.s4());    // diff == 0
    c->and_(v3.b16(), v3.b16(), vt.b16()); // diff == 0 && borrow-in
    c->orr(v2.b16(), v2.b16(), v3.b16());  // borrow-out mask
    c->ldr(vc, VConst(v128::from32p(0xffffffff)));
    c->eor(v2.b16(), v2.b16(), vc.b16());  // !borrow
    c->ushr(v2.s4(), v2.s4(), asmjit::Imm(31)); // 0/1
    c->str(v2, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::MPYHHA(spu_opcode_t op)
{
    // rt += madd(ra >> 16, rb >> 16): odd halfword products, signed
    const VLink& va = VGet(op.ra, VType::Int);
    const VLink& vb = VGet(op.rb, VType::Int);
    const VLink& vt = VGet(op.rt, VType::Int);
    const VLink& v1 = VAlloc();
    const VLink& v2 = VAlloc();
    const VLink& v3 = VAlloc();
    const VLink& va2 = VAlloc();
    const VLink& vb2 = VAlloc();
    c->ext(va2.b16(), va.b16(), va.b16(), 8);
    c->ext(vb2.b16(), vb.b16(), vb.b16(), 8);
    c->smull2(v1.s4(), va2.h8(), vb2.h8()); // low: [E0, O0, E1, O1]
    c->smull2(v2.s4(), va.h8(), vb.h8());   // high: [E2, O2, E3, O3]
    c->uzp2(v3.s4(), v1.s4(), v2.s4());     // odd lanes: [O0, O1, O2, O3]
    c->add(vt.s4(), vt.s4(), v3.s4());
    c->str(vt, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::MPYHHAU(spu_opcode_t op)
{
    // rt += full 32-bit unsigned odd-halfword products
    const VLink& va = VGet(op.ra, VType::Int);
    const VLink& vb = VGet(op.rb, VType::Int);
    const VLink& vt = VGet(op.rt, VType::Int);
    const VLink& v1 = VAlloc();
    const VLink& v2 = VAlloc();
    const VLink& v3 = VAlloc();
    const VLink& va2 = VAlloc();
    const VLink& vb2 = VAlloc();
    c->ext(va2.b16(), va.b16(), va.b16(), 8);
    c->ext(vb2.b16(), vb.b16(), vb.b16(), 8);
    c->umull2(v2.s4(), va2.h8(), vb2.h8()); // low: [E0, O0, E1, O1]
    c->umull2(v1.s4(), va.h8(), vb.h8());   // high: [E2, O2, E3, O3]
    c->uzp2(v3.s4(), v2.s4(), v1.s4());     // odd: [O0, O1, O2, O3]
    c->add(vt.s4(), vt.s4(), v3.s4());
    c->str(vt, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::FSCRRD(spu_opcode_t op)
{
    // Zero (matches the interpreter)
    const VLink& vr = VAlloc();
    c->eor(vr.b16(), vr.b16(), vr.b16());
    c->str(vr, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::FESD(spu_opcode_t op)
{
    // Convert float words 1 and 3 to the two doubles
    const VLink& va = VGet(op.ra, VType::Float);
    const VLink& vt = VAlloc();
    const VLink& vd = VAlloc();
    c->uzp2(vt.s4(), va.s4(), va.s4()); // [f1, f3, f1, f3]
    c->fcvtl(vd.d2(), vt.s2());
    c->str(vd, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::FRDS(spu_opcode_t op)
{
    // Convert the two doubles to float words 1 and 3
    const VLink& va = VGet(op.ra, VType::Double);
    const VLink& vt = VAlloc();
    const VLink& vz = VAlloc();
    const VLink& vs = VAlloc();
    c->fcvtn(vt.s2(), va.d2()); // [f(d0), f(d1), 0, 0]
    c->eor(vz.b16(), vz.b16(), vz.b16());
    c->zip1(vs.s4(), vz.s4(), vt.s4()); // [0, f(d0), 0, f(d1)]
    c->str(vs, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::FSCRWR(spu_opcode_t op)
{
    // nop (not implemented), matches the x86 recompiler
}

void spu_recompiler::DFTSV(spu_opcode_t op)
{
    UNK(op);
}

void spu_recompiler::FCEQ(spu_opcode_t op)
{
    const VLink& va = VGet(op.ra, VType::Float);
    const VLink& vb = VGet(op.rb, VType::Float);
    // SPU DAZ semantics: denormal inputs compare as zero
    {
        const VLink& vm = VAlloc();
        const asmjit::a64::VecV& vc_sign = VAlloc();
        const asmjit::a64::VecV& vc_th = VAlloc();
        c->ldr(vc_sign, VConst(v128::from32p(0x7fffffff)));
        c->ldr(vc_th, VConst(v128::from32p(0x007fffff)));
        c->and_(vm.b16(), va.b16(), vc_sign.b16());
        c->cmhi(vm.s4(), vm.s4(), vc_th.s4());
        c->and_(va.b16(), va.b16(), vm.b16());
        c->and_(vm.b16(), vb.b16(), vc_sign.b16());
        c->cmhi(vm.s4(), vm.s4(), vc_th.s4());
        c->and_(vb.b16(), vb.b16(), vm.b16());
    }
    c->fcmeq(va.s4(), va.s4(), vb.s4());
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::DFCEQ(spu_opcode_t op)
{
    UNK(op);
}

void spu_recompiler::MPY(spu_opcode_t op)
{
    // madd_epi16(ra & 0xffff, rb & 0xffff): signed smull + pairwise sums
    const VLink& va = VGet(op.ra, VType::Int);
    const VLink& vb = VGet(op.rb, VType::Int);
    const VLink& v1 = VAlloc();
    const VLink& v2 = VAlloc();
    const VLink& v3 = VAlloc();
    const VLink& v4 = VAlloc();
    const VLink& va2 = VAlloc();
    const VLink& vb2 = VAlloc();
    const asmjit::a64::VecV vc_mask = VAlloc();
    c->ldr(vc_mask, VConst(v128::from32p(0xffff)));
    c->and_(va.b16(), va.b16(), vc_mask.b16());
    c->and_(vb.b16(), vb.b16(), vc_mask.b16());
    c->ext(va2.b16(), va.b16(), va.b16(), 8);
    c->ext(vb2.b16(), vb.b16(), vb.b16(), 8);
    c->smull2(v1.s4(), va2.h8(), vb2.h8()); // [p0,p1,p2,p3]
    c->smull2(v2.s4(), va.h8(), vb.h8());   // [p4,p5,p6,p7]
    c->uzp1(v3.s4(), v1.s4(), v1.s4()); // [p0,p2,p0,p2]
    c->uzp2(v4.s4(), v1.s4(), v1.s4()); // [p1,p3,p1,p3]
    c->add(v1.s4(), v3.s4(), v4.s4()); // [p0+p1, p2+p3, ...]
    c->uzp1(v3.s4(), v2.s4(), v2.s4());
    c->uzp2(v4.s4(), v2.s4(), v2.s4());
    c->add(v2.s4(), v3.s4(), v4.s4()); // [p4+p5, p6+p7, ...]
    c->uzp1(va.d2(), v1.d2(), v2.d2()); // [p0+p1, p2+p3, p4+p5, p6+p7]
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::MPYH(spu_opcode_t op)
{
    // ((s16(ra.h[2i+1]) * s16(rb.h[2i])) & 0xffff) << 16
    const VLink& va = VGet(op.ra, VType::Int);
    const VLink& vb = VGet(op.rb, VType::Int);
    const asmjit::a64::VecV& vc_mask = VAlloc();
    c->ldr(vc_mask, VConst(v128::from32p(0xffff)));
    c->ushr(va.s4(), va.s4(), asmjit::Imm(16));
    c->and_(vb.b16(), vb.b16(), vc_mask.b16());
    c->mul(va.s4(), va.s4(), vb.s4());
    c->shl(va.s4(), va.s4(), asmjit::Imm(16));
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::MPYHH(spu_opcode_t op)
{
    // madd_epi16(ra >> 16, rb >> 16): signed smull + pairwise sums
    const VLink& va = VGet(op.ra, VType::Int);
    const VLink& vb = VGet(op.rb, VType::Int);
    const VLink& v1 = VAlloc();
    const VLink& v2 = VAlloc();
    const VLink& v3 = VAlloc();
    const VLink& v4 = VAlloc();
    const VLink& va2 = VAlloc();
    const VLink& vb2 = VAlloc();
    c->ushr(va.s4(), va.s4(), asmjit::Imm(16));
    c->ushr(vb.s4(), vb.s4(), asmjit::Imm(16));
    c->ext(va2.b16(), va.b16(), va.b16(), 8);
    c->ext(vb2.b16(), vb.b16(), vb.b16(), 8);
    c->smull2(v1.s4(), va2.h8(), vb2.h8()); // [p0..p3]
    c->smull2(v2.s4(), va.h8(), vb.h8());   // [p4..p7]
    c->uzp1(v3.s4(), v1.s4(), v1.s4());
    c->uzp2(v4.s4(), v1.s4(), v1.s4());
    c->add(v1.s4(), v3.s4(), v4.s4());
    c->uzp1(v3.s4(), v2.s4(), v2.s4());
    c->uzp2(v4.s4(), v2.s4(), v2.s4());
    c->add(v2.s4(), v3.s4(), v4.s4());
    c->uzp1(va.d2(), v1.d2(), v2.d2());
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::MPYS(spu_opcode_t op)
{
    // pmulhw + pslld 16 + psrad 16: word i = sxt16((odd_product) >> 16)
    const VLink& va = VGet(op.ra, VType::Int);
    const VLink& vb = VGet(op.rb, VType::Int);
    const VLink& v1 = VAlloc();
    const VLink& v2 = VAlloc();
    const VLink& v3 = VAlloc();
    const VLink& va2 = VAlloc();
    const VLink& vb2 = VAlloc();
    c->ext(va2.b16(), va.b16(), va.b16(), 8);
    c->ext(vb2.b16(), vb.b16(), vb.b16(), 8);
    c->smull2(v1.s4(), va2.h8(), vb2.h8()); // [p0, p1, p2, p3]
    c->smull2(v2.s4(), va.h8(), vb.h8());   // [p4, p5, p6, p7]
    c->uzp1(v3.s4(), v1.s4(), v2.s4());     // even halfwords: [p0, p2, p4, p6]
    c->sshr(v3.s4(), v3.s4(), asmjit::Imm(16)); // sxt((even_product >> 16) & 0xffff)
    c->str(v3, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::CEQH(spu_opcode_t op)
{
    const VLink& va = VGet(op.ra, VType::Int);
    const VLink& vb = VGet(op.rb, VType::Int);
    c->cmeq(va.h8(), va.h8(), vb.h8());
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::FCMEQ(spu_opcode_t op)
{
    const VLink& va = VGet(op.ra, VType::Float);
    const VLink& vb = VGet(op.rb, VType::Float);
    // SPU DAZ semantics: denormal inputs compare as zero
    {
        const VLink& vm = VAlloc();
        const asmjit::a64::VecV& vc_sign = VAlloc();
        const asmjit::a64::VecV& vc_th = VAlloc();
        c->ldr(vc_sign, VConst(v128::from32p(0x7fffffff)));
        c->ldr(vc_th, VConst(v128::from32p(0x007fffff)));
        c->and_(vm.b16(), va.b16(), vc_sign.b16());
        c->cmhi(vm.s4(), vm.s4(), vc_th.s4());
        c->and_(va.b16(), va.b16(), vm.b16());
        c->and_(vm.b16(), vb.b16(), vc_sign.b16());
        c->cmhi(vm.s4(), vm.s4(), vc_th.s4());
        c->and_(vb.b16(), vb.b16(), vm.b16());
    }
    const VLink& vabs_a = VAlloc();
    const VLink& vabs_b = VAlloc();
    c->fabs(vabs_a.s4(), va.s4());
    c->fabs(vabs_b.s4(), vb.s4());
    c->fcmeq(vabs_a.s4(), vabs_a.s4(), vabs_b.s4());
    c->str(vabs_a, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::DFCMEQ(spu_opcode_t op)
{
    UNK(op);
}

void spu_recompiler::MPYU(spu_opcode_t op)
{
    // word i = (u16 ra.h[2i]) * (u16 rb.h[2i]) full 32-bit product (even halfwords)
    const VLink& va = VGet(op.ra, VType::Int);
    const VLink& vb = VGet(op.rb, VType::Int);
    const VLink& v1 = VAlloc();
    const VLink& v2 = VAlloc();
    const VLink& v3 = VAlloc();
    const VLink& va2 = VAlloc();
    const VLink& vb2 = VAlloc();
    c->ext(va2.b16(), va.b16(), va.b16(), 8);
    c->ext(vb2.b16(), vb.b16(), vb.b16(), 8);
    c->umull2(v1.s4(), va2.h8(), vb2.h8()); // [E0, O0, E1, O1]
    c->umull2(v2.s4(), va.h8(), vb.h8());   // [E2, O2, E3, O3]
    c->uzp1(v3.s4(), v1.s4(), v2.s4());     // even lanes: [E0, E1, E2, E3]
    c->str(v3, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::CEQB(spu_opcode_t op)
{
    const VLink& va = VGet(op.ra, VType::Int);
    const VLink& vb = VGet(op.rb, VType::Int);
    c->cmeq(va.b16(), va.b16(), vb.b16());
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::FI(spu_opcode_t op)
{
    // base = (rb & 0x007ffc00) | 0x3f800000
    // step = u32(rb & 0x3ff) * 2^-13, y = u32(ra & 0x7ffff) * 2^-19
    // rt = (rb & 0xff800000) | ((base - step*y) & 0x007fffff)
    const VLink& va = VGet(op.ra, VType::Int);
    const VLink& vb = VGet(op.rb, VType::Int);
    const VLink& v1 = VAlloc(); // step
    const VLink& v2 = VAlloc(); // y
    const VLink& v3 = VAlloc(); // base
    const asmjit::a64::VecV vc = VAlloc();
    c->ldr(vc, VConst(v128::from32p(0x007ffc00)));
    c->and_(v3.b16(), vb.b16(), vc.b16());
    c->ldr(vc, VConst(v128::from32p(0x3f800000)));
    c->orr(v3.b16(), v3.b16(), vc.b16());
    c->ldr(vc, VConst(v128::from32p(0x3ff)));
    c->and_(v1.b16(), vb.b16(), vc.b16());
    c->ucvtf(v1.s4(), v1.s4());
    c->ldr(vc, VConst(v128::fromf32p(1.0f / 8192.0f)));
    c->fmul(v1.s4(), v1.s4(), vc.s4()); // step
    c->ldr(vc, VConst(v128::from32p(0x7ffff)));
    c->and_(v2.b16(), va.b16(), vc.b16());
    c->ucvtf(v2.s4(), v2.s4());
    c->ldr(vc, VConst(v128::fromf32p(1.0f / 524288.0f)));
    c->fmul(v2.s4(), v2.s4(), vc.s4()); // y
    c->fmul(v1.s4(), v1.s4(), v2.s4()); // step * y
    c->fsub(v3.s4(), v3.s4(), v1.s4()); // base - step*y
    c->ldr(vc, VConst(v128::from32p(0xff800000)));
    c->and_(vb.b16(), vb.b16(), vc.b16());
    c->ldr(vc, VConst(v128::from32p(0x007fffff)));
    c->and_(v3.b16(), v3.b16(), vc.b16());
    c->orr(vb.b16(), vb.b16(), v3.b16());
    c->str(vb, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::HEQ(spu_opcode_t op)
{
    // Halt if equal (word 3): emit the halt magic write (matches x86/LLVM)
    c->ldr(qw0->w(), SPU_OFF_32(gpr, op.ra, &v128::_u32, 3));
    c->ldr(qw1->w(), SPU_OFF_32(gpr, op.rb, &v128::_u32, 3));
    c->cmp(qw0->w(), qw1->w());

    asmjit::Label label = c->newLabel();
    c->b_eq(label);
    a64_halt_body(label, op);
}

void spu_recompiler::CFLTS(spu_opcode_t op)
{
    // rt = cvtt(ra * exp2(173 - i8)) ^ (scaled >= 2^31)
    const VLink& va = VGet(op.ra, VType::Float);
    const VLink& vi = VAlloc();
    const asmjit::a64::VecV vc = VAlloc();
    if (op.i8 != 173)
    {
        c->ldr(vc, VConst(v128::fromf32p(std::exp2(static_cast<float>(static_cast<s16>(173 - op.i8))))));
        c->fmul(va.s4(), va.s4(), vc.s4());
    }
    c->ldr(vc, VConst(v128::fromf32p(std::exp2(31.f))));
    c->fcmge(vi.s4(), va.s4(), vc.s4()); // scaled >= 2^31
    c->fcvtzs(va.s4(), va.s4());         // saturating truncation (matches sse2neon)
    c->eor(va.b16(), va.b16(), vi.b16()); // fix saturation
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::CFLTU(spu_opcode_t op)
{
    // scaled1 = max(ra * exp2(173 - i8), 0)
    // scaled2 = (scaled1 - 2^31) & (scaled1 >= 2^31)
    // rt = cvtt(scaled1) | cvtt(scaled2) | (scaled1 >= 2^32)
    const VLink& va = VGet(op.ra, VType::Float);
    const VLink& v1 = VAlloc();
    const VLink& v2 = VAlloc();
    const asmjit::a64::VecV vc = VAlloc();
    if (op.i8 != 173)
    {
        c->ldr(vc, VConst(v128::fromf32p(std::exp2(static_cast<float>(static_cast<s16>(173 - op.i8))))));
        c->fmul(va.s4(), va.s4(), vc.s4());
    }
    c->ldr(vc, VConst(v128::fromf32p(0.0f)));
    c->fmax(va.s4(), va.s4(), vc.s4());   // scaled1
    c->ldr(vc, VConst(v128::fromf32p(std::exp2(31.f))));
    c->fcmge(v2.s4(), va.s4(), vc.s4());  // scaled1 >= 2^31
    c->fsub(v1.s4(), va.s4(), vc.s4());   // scaled1 - 2^31
    c->and_(v1.b16(), v1.b16(), v2.b16()); // scaled2
    c->ldr(vc, VConst(v128::fromf32p(std::exp2(32.f))));
    c->fcmge(v2.s4(), va.s4(), vc.s4());  // scaled1 >= 2^32
    c->fcvtzs(va.s4(), va.s4());
    c->fcvtzs(v1.s4(), v1.s4());
    c->orr(va.b16(), va.b16(), v1.b16());
    c->orr(va.b16(), va.b16(), v2.b16());
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::CSFLT(spu_opcode_t op)
{
    // rt = cvtepi32ps(ra) * exp2(i8 - 155)
    const VLink& va = VGet(op.ra, VType::Int);
    const asmjit::a64::VecV vc = VAlloc();
    c->scvtf(va.s4(), va.s4());
    if (op.i8 != 155)
    {
        c->ldr(vc, VConst(v128::fromf32p(std::exp2(static_cast<float>(static_cast<s16>(op.i8 - 155))))));
        c->fmul(va.s4(), va.s4(), vc.s4());
    }
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::CUFLT(spu_opcode_t op)
{
    // rt = ucvtf(ra) * exp2(i8 - 155)
    // (ucvtf matches the interpreter's (float)(a & 0x7fffffff) + (a < 0 ? 2^31 : 0))
    const VLink& va = VGet(op.ra, VType::Int);
    const asmjit::a64::VecV vc = VAlloc();
    c->ucvtf(va.s4(), va.s4());
    if (op.i8 != 155)
    {
        c->ldr(vc, VConst(v128::fromf32p(std::exp2(static_cast<float>(static_cast<s16>(op.i8 - 155))))));
        c->fmul(va.s4(), va.s4(), vc.s4());
    }
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::BRZ(spu_opcode_t op)
{
    const u32 target = spu_branch_target(m_pos, op.i16);
    asmjit::Label branch_label = c->newLabel();
    c->ldr(qw0->w(), SPU_OFF_32(gpr, op.rt, &v128::_u32, 3));
    c->cmp(qw0->w(), asmjit::Imm(0));
    c->b_eq(branch_label);
    after.emplace_back([=, this]
                       {
                           c->align(asmjit::AlignMode::kCode, 16);
                           c->bind(branch_label);
                           branch_fixed(target);
                       });
}

void spu_recompiler::STQA(spu_opcode_t op)
{
    c->mov(*addr, asmjit::Imm(spu_ls_target(0, op.i16)));
    c->add(*addr, *ls, *addr);
    const VLink& vt = VGet(op.rt, VType::Int);
    const VLink& vs = VAlloc();
    c->rev64(vs.b16(), vt.b16());
    c->ext(vs.b16(), vs.b16(), vs.b16(), 8);
    c->str(vs, asmjit::arm::Mem(*addr));
}

void spu_recompiler::BRNZ(spu_opcode_t op)
{
    const u32 target = spu_branch_target(m_pos, op.i16);
    asmjit::Label branch_label = c->newLabel();
    c->ldr(qw0->w(), SPU_OFF_32(gpr, op.rt, &v128::_u32, 3));
    c->cmp(qw0->w(), asmjit::Imm(0));
    c->b_ne(branch_label);
    after.emplace_back([=, this]
                       {
                           c->align(asmjit::AlignMode::kCode, 16);
                           c->bind(branch_label);
                           branch_fixed(target);
                       });
}

void spu_recompiler::BRHZ(spu_opcode_t op)
{
    const u32 target = spu_branch_target(m_pos, op.i16);
    asmjit::Label branch_label = c->newLabel();
    c->ldrh(qw0->w(), SPU_OFF_16(gpr, op.rt, &v128::_u16, 6));
    c->cmp(qw0->w(), asmjit::Imm(0));
    c->b_eq(branch_label);
    after.emplace_back([=, this]
                       {
                           c->align(asmjit::AlignMode::kCode, 16);
                           c->bind(branch_label);
                           branch_fixed(target);
                       });
}

void spu_recompiler::BRHNZ(spu_opcode_t op)
{
    const u32 target = spu_branch_target(m_pos, op.i16);
    asmjit::Label branch_label = c->newLabel();
    c->ldrh(qw0->w(), SPU_OFF_16(gpr, op.rt, &v128::_u16, 6));
    c->cmp(qw0->w(), asmjit::Imm(0));
    c->b_ne(branch_label);
    after.emplace_back([=, this]
                       {
                           c->align(asmjit::AlignMode::kCode, 16);
                           c->bind(branch_label);
                           branch_fixed(target);
                       });
}

void spu_recompiler::STQR(spu_opcode_t op)
{
    // Target depends on runtime pc: relocate via pc0 like x64 get_pc
    c->mov(*addr, asmjit::Imm(spu_ls_target(m_pos, op.i16) - m_base));
    c->add(*addr, *pc0, *addr);
    c->and_(*addr, *addr, asmjit::Imm(0x3fff0));
    c->add(*addr, *ls, *addr);
    const VLink& vt = VGet(op.rt, VType::Int);
    const VLink& vs = VAlloc();
    c->rev64(vs.b16(), vt.b16());
    c->ext(vs.b16(), vs.b16(), vs.b16(), 8);
    c->str(vs, asmjit::arm::Mem(*addr));
}

void spu_recompiler::BRA(spu_opcode_t op)
{
    // Absolute branch: target = (i16 << 2) & 0x3fffc
    branch_fixed(spu_branch_target(0, op.i16), true);
    m_pos = -1;
}

void spu_recompiler::LQA(spu_opcode_t op)
{
    c->mov(*addr, asmjit::Imm(spu_ls_target(0, op.i16)));
    c->add(*addr, *ls, *addr);
    const VLink& vt = VAlloc();
    c->ldr(vt, asmjit::arm::Mem(*addr));
    c->rev64(vt.b16(), vt.b16());
    c->ext(vt.b16(), vt.b16(), vt.b16(), 8);
    c->str(vt, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::BRASL(spu_opcode_t op)
{
    // Absolute branch and set link: rt = m_pos + 4, target = (i16 << 2) & 0x3fffc
    const u32 target = spu_branch_target(0, op.i16);

    // Set link register rt = return address
    const VLink& vr = VAlloc();
    c->mov(*qw0, asmjit::Imm((m_pos + 4) - m_base));
    c->add(qw0->w(), qw0->w(), pc0->w());
    c->and_(qw0->w(), qw0->w(), asmjit::Imm(0x3fffc));
    c->eor(vr.b16(), vr.b16(), vr.b16()); // clear stale pool data (rt = {0,0,0,ra})
    c->ins(vr.s(3), qw0->w());
    c->str(vr, SPU_OFF_128(gpr, op.rt));

    branch_set_link(m_pos + 4);
    branch_fixed(target, true);
    m_pos = -1;
}

void spu_recompiler::BR(spu_opcode_t op)
{
    // Relative branch: target = (m_pos + (i16 << 2)) & 0x3fffc
    const u32 target = spu_branch_target(m_pos, op.i16);

    if (target != m_pos + 4)
    {
        branch_fixed(target);
        m_pos = -1;
    }
}

void spu_recompiler::FSMBI(spu_opcode_t op)
{
    v128 data;
    for (u32 i = 0; i < 16; i++)
        data._u8[i] = op.i16 & (1u << i) ? 0xff : 0;

    const asmjit::a64::VecV& vr = VAlloc();
    c->ldr(vr, VConst(data));
    c->str(vr, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::BRSL(spu_opcode_t op)
{
    // Relative branch and set link: rt = m_pos + 4, target = (m_pos + (i16 << 2)) & 0x3fffc
    const u32 target = spu_branch_target(m_pos, op.i16);

    // Set link register rt = return address
    const VLink& vr = VAlloc();
    c->mov(*qw0, asmjit::Imm((m_pos + 4) - m_base));
    c->add(qw0->w(), qw0->w(), pc0->w());
    c->and_(qw0->w(), qw0->w(), asmjit::Imm(0x3fffc));
    c->eor(vr.b16(), vr.b16(), vr.b16()); // clear stale pool data (rt = {0,0,0,ra})
    c->ins(vr.s(3), qw0->w());
    c->str(vr, SPU_OFF_128(gpr, op.rt));

    if (target != m_pos + 4)
    {
        branch_set_link(m_pos + 4);
        branch_fixed(target);
        m_pos = -1;
    }
}

void spu_recompiler::LQR(spu_opcode_t op)
{
    // Target depends on runtime pc: relocate via pc0 like x64 get_pc
    c->mov(*addr, asmjit::Imm(spu_ls_target(m_pos, op.i16) - m_base));
    c->add(*addr, *pc0, *addr);
    c->and_(*addr, *addr, asmjit::Imm(0x3fff0));
    c->add(*addr, *ls, *addr);
    const VLink& vt = VAlloc();
    c->ldr(vt, asmjit::arm::Mem(*addr));
    c->rev64(vt.b16(), vt.b16());
    c->ext(vt.b16(), vt.b16(), vt.b16(), 8);
    c->str(vt, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::IL(spu_opcode_t op)
{
    const VLink& vr = VAlloc();
    const asmjit::a64::VecV& vc_t18 = VAlloc();
    c->ldr(vc_t18, VConst(v128::from32p(op.si16)));
    c->mov(vr.b16(), vc_t18.b16());
    c->str(vr, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::ILHU(spu_opcode_t op)
{
    const VLink& vr = VAlloc();
    const asmjit::a64::VecV& vc_t19 = VAlloc();
    c->ldr(vc_t19, VConst(v128::from32p(op.i16 << 16)));
    c->mov(vr.b16(), vc_t19.b16());
    c->str(vr, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::ILH(spu_opcode_t op)
{
    const VLink& vr = VAlloc();
    const asmjit::a64::VecV& vc_t20 = VAlloc();
    c->ldr(vc_t20, VConst(v128::from16p(op.i16)));
    c->mov(vr.b16(), vc_t20.b16());
    c->str(vr, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::IOHL(spu_opcode_t op)
{
    // Mirror the x64 implementation exactly: OR with the raw 16-bit immediate
    // (no << 16 shift).
    const VLink& vt = VGet(op.rt, VType::Int);
    const asmjit::a64::VecV& vc_t21 = VAlloc();
    c->ldr(vc_t21, VConst(v128::from32p(op.i16)));
    c->orr(vt.b16(), vt.b16(), vc_t21.b16());
    c->str(vt, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::ORI(spu_opcode_t op)
{
    const VLink& va = VGet(op.ra, VType::Int);
    const asmjit::a64::VecV& vc_t22 = VAlloc();
    c->ldr(vc_t22, VConst(v128::from32p(op.si10)));
    c->orr(va.b16(), va.b16(), vc_t22.b16());
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::ORHI(spu_opcode_t op)
{
    const VLink& va = VGet(op.ra, VType::Int);
    const asmjit::a64::VecV& vc_t23 = VAlloc();
    c->ldr(vc_t23, VConst(v128::from16p(op.si10)));
    c->orr(va.b16(), va.b16(), vc_t23.b16());
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::ORBI(spu_opcode_t op)
{
    const VLink& va = VGet(op.ra, VType::Int);
    const asmjit::a64::VecV& vc_t24 = VAlloc();
    c->ldr(vc_t24, VConst(v128::from8p(op.si10)));
    c->orr(va.b16(), va.b16(), vc_t24.b16());
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::SFI(spu_opcode_t op)
{
    const VLink& va = VGet(op.ra, VType::Int);
    const VLink& vi = VAlloc();
    const asmjit::a64::VecV& vc_t25 = VAlloc();
    c->ldr(vc_t25, VConst(v128::from32p(op.si10)));
    c->mov(vi.b16(), vc_t25.b16());
    c->sub(vi.s4(), vi.s4(), va.s4());
    c->str(vi, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::SFHI(spu_opcode_t op)
{
    const VLink& va = VGet(op.ra, VType::Int);
    const VLink& vi = VAlloc();
    const asmjit::a64::VecV& vc_t26 = VAlloc();
    c->ldr(vc_t26, VConst(v128::from16p(op.si10)));
    c->mov(vi.b16(), vc_t26.b16());
    c->sub(vi.h8(), vi.h8(), va.h8());
    c->str(vi, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::ANDI(spu_opcode_t op)
{
    const VLink& va = VGet(op.ra, VType::Int);
    const asmjit::a64::VecV& vc_t27 = VAlloc();
    c->ldr(vc_t27, VConst(v128::from32p(op.si10)));
    c->and_(va.b16(), va.b16(), vc_t27.b16());
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::ANDHI(spu_opcode_t op)
{
    const VLink& va = VGet(op.ra, VType::Int);
    const asmjit::a64::VecV& vc_t28 = VAlloc();
    c->ldr(vc_t28, VConst(v128::from16p(op.si10)));
    c->and_(va.b16(), va.b16(), vc_t28.b16());
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::ANDBI(spu_opcode_t op)
{
    const VLink& va = VGet(op.ra, VType::Int);
    const asmjit::a64::VecV& vc_t29 = VAlloc();
    c->ldr(vc_t29, VConst(v128::from8p(op.si10)));
    c->and_(va.b16(), va.b16(), vc_t29.b16());
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::AI(spu_opcode_t op)
{
    const VLink& va = VGet(op.ra, VType::Int);
    const asmjit::a64::VecV& vc_t30 = VAlloc();
    c->ldr(vc_t30, VConst(v128::from32p(op.si10)));
    c->add(va.s4(), va.s4(), vc_t30.s4());
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::AHI(spu_opcode_t op)
{
    const VLink& va = VGet(op.ra, VType::Int);
    const asmjit::a64::VecV& vc_t31 = VAlloc();
    c->ldr(vc_t31, VConst(v128::from16p(op.si10)));
    c->add(va.h8(), va.h8(), vc_t31.h8());
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::STQD(spu_opcode_t op)
{
    c->ldr(addr->w(), SPU_OFF_32(gpr, op.ra, &v128::_u32, 3));
    // ARM64 ADD imm12 is unsigned: a negative Imm() fails to encode and asmjit
    // silently drops the instruction (LS-zeroing root cause, see --r37 sweep).
    if (op.si10 > 0)
        c->add(*addr, *addr, asmjit::Imm(op.si10 << 4));
    else if (op.si10 < 0)
        c->sub(*addr, *addr, asmjit::Imm(-(op.si10 << 4)));
    c->and_(*addr, *addr, asmjit::Imm(0x3fff0));
    c->add(*addr, *ls, *addr);
    const VLink& vt = VGet(op.rt, VType::Int);
    const VLink& vs = VAlloc();
    c->rev64(vs.b16(), vt.b16());
    c->ext(vs.b16(), vs.b16(), vs.b16(), 8);
    c->str(vs, asmjit::arm::Mem(*addr));
}

void spu_recompiler::LQD(spu_opcode_t op)
{
    // addr = (ra._u32[3] + si10 * 16) & 0x3fff0, big-endian load
    c->ldr(addr->w(), SPU_OFF_32(gpr, op.ra, &v128::_u32, 3));
    // ARM64 ADD imm12 is unsigned: a negative Imm() fails to encode and asmjit
    // silently drops the instruction => LQD with si10<0 read [r1+0] (r36/r37 MISMATCH).
    // Use SUB with a positive immediate for negative si10.
    if (op.si10 > 0)
        c->add(*addr, *addr, asmjit::Imm(op.si10 << 4));
    else if (op.si10 < 0)
        c->sub(*addr, *addr, asmjit::Imm(-(op.si10 << 4)));
    c->and_(*addr, *addr, asmjit::Imm(0x3fff0));
    c->add(*addr, *ls, *addr);
    const VLink& vt = VAlloc();
    c->ldr(vt, asmjit::arm::Mem(*addr));
    c->rev64(vt.b16(), vt.b16());
    c->ext(vt.b16(), vt.b16(), vt.b16(), 8);
    c->str(vt, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::XORI(spu_opcode_t op)
{
    const VLink& va = VGet(op.ra, VType::Int);
    const asmjit::a64::VecV& vc_t32 = VAlloc();
    c->ldr(vc_t32, VConst(v128::from32p(op.si10)));
    c->eor(va.b16(), va.b16(), vc_t32.b16());
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::XORHI(spu_opcode_t op)
{
    const VLink& va = VGet(op.ra, VType::Int);
    const asmjit::a64::VecV& vc_t33 = VAlloc();
    c->ldr(vc_t33, VConst(v128::from16p(op.si10)));
    c->eor(va.b16(), va.b16(), vc_t33.b16());
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::XORBI(spu_opcode_t op)
{
    const VLink& va = VGet(op.ra, VType::Int);
    const asmjit::a64::VecV& vc_t34 = VAlloc();
    c->ldr(vc_t34, VConst(v128::from8p(op.si10)));
    c->eor(va.b16(), va.b16(), vc_t34.b16());
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::CGTI(spu_opcode_t op)
{
    const VLink& va = VGet(op.ra, VType::Int);
    const asmjit::a64::VecV& vc_t35 = VAlloc();
    c->ldr(vc_t35, VConst(v128::from32p(op.si10)));
    c->cmgt(va.s4(), va.s4(), vc_t35.s4());
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::CGTHI(spu_opcode_t op)
{
    const VLink& va = VGet(op.ra, VType::Int);
    const asmjit::a64::VecV& vc_t36 = VAlloc();
    c->ldr(vc_t36, VConst(v128::from16p(op.si10)));
    c->cmgt(va.h8(), va.h8(), vc_t36.h8());
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::CGTBI(spu_opcode_t op)
{
    const VLink& va = VGet(op.ra, VType::Int);
    const asmjit::a64::VecV& vc_t37 = VAlloc();
    c->ldr(vc_t37, VConst(v128::from8p(op.si10)));
    c->cmgt(va.b16(), va.b16(), vc_t37.b16());
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::HGTI(spu_opcode_t op)
{
    // Halt if greater than immediate (signed, word 3): emit the halt magic write (matches x86/LLVM)
    c->ldr(qw0->w(), SPU_OFF_32(gpr, op.ra, &v128::_u32, 3));
    c->mov(qw1->w(), asmjit::Imm(static_cast<u32>(op.si10)));
    c->cmp(qw0->w(), qw1->w());

    asmjit::Label label = c->newLabel();
    c->b_gt(label);
    a64_halt_body(label, op);
}

void spu_recompiler::CLGTI(spu_opcode_t op)
{
    const VLink& va = VGet(op.ra, VType::Int);
    const asmjit::a64::VecV& vc_t39 = VAlloc();
    c->ldr(vc_t39, VConst(v128::from32p(op.si10)));
    c->cmhi(va.s4(), va.s4(), vc_t39.s4());
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::CLGTHI(spu_opcode_t op)
{
    const VLink& va = VGet(op.ra, VType::Int);
    const asmjit::a64::VecV& vc_t40 = VAlloc();
    c->ldr(vc_t40, VConst(v128::from16p(op.si10)));
    c->cmhi(va.h8(), va.h8(), vc_t40.h8());
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::CLGTBI(spu_opcode_t op)
{
    const VLink& va = VGet(op.ra, VType::Int);
    const asmjit::a64::VecV& vc_t41 = VAlloc();
    c->ldr(vc_t41, VConst(v128::from8p(op.si10)));
    c->cmhi(va.b16(), va.b16(), vc_t41.b16());
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::HLGTI(spu_opcode_t op)
{
    // Halt if greater than immediate (unsigned, word 3): emit the halt magic write (matches x86/LLVM)
    c->ldr(qw0->w(), SPU_OFF_32(gpr, op.ra, &v128::_u32, 3));
    c->mov(qw1->w(), asmjit::Imm(static_cast<u32>(op.si10)));
    c->cmp(qw0->w(), qw1->w());

    asmjit::Label label = c->newLabel();
    c->b_hi(label);
    a64_halt_body(label, op);
}

void spu_recompiler::MPYI(spu_opcode_t op)
{
    // Interpreter: madd(ra, set1_epi32(si10 & 0xffff)) - the immediate word
    // broadcast makes odd halfwords zero, so only even halfwords multiply:
    // word i = (s16 ra.h[2i]) * imm
    const VLink& va = VGet(op.ra, VType::Int);
    const VLink& v1 = VAlloc();
    const VLink& v2 = VAlloc();
    const VLink& v3 = VAlloc();
    const VLink& va2 = VAlloc();
    const asmjit::a64::VecV vimm = VAlloc();
    c->mov(qw0->w(), asmjit::Imm(op.si10 & 0xffff));
    c->dup(vimm.h8(), qw0->w());
    c->ext(va2.b16(), va.b16(), va.b16(), 8);
    c->smull2(v1.s4(), va2.h8(), vimm.h8());  // [e0, o0, e1, o1]
    c->smull2(v2.s4(), va.h8(), vimm.h8());   // [e2, o2, e3, o3]
    c->uzp1(v3.s4(), v1.s4(), v2.s4());     // [e0, e1, e2, e3]
    c->str(v3, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::MPYUI(spu_opcode_t op)
{
    // word i = (u16 ra.h[2i]) * (u16)(si10 & 0xffff) full 32-bit product
    const VLink& va = VGet(op.ra, VType::Int);
    const VLink& v1 = VAlloc();
    const VLink& v2 = VAlloc();
    const VLink& v3 = VAlloc();
    const VLink& va2 = VAlloc();
    const asmjit::a64::VecV vimm = VAlloc();
    c->mov(qw0->w(), asmjit::Imm(op.si10 & 0xffff));
    c->dup(vimm.h8(), qw0->w());
    c->ext(va2.b16(), va.b16(), va.b16(), 8);
    c->umull2(v1.s4(), va2.h8(), vimm.h8()); // [e0, o0, e1, o1]
    c->umull2(v2.s4(), va.h8(), vimm.h8());  // [e2, o2, e3, o3]
    c->uzp1(v3.s4(), v1.s4(), v2.s4());     // [e0, e1, e2, e3]
    c->str(v3, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::CEQI(spu_opcode_t op)
{
    const VLink& va = VGet(op.ra, VType::Int);
    const asmjit::a64::VecV& vc_t44 = VAlloc();
    c->ldr(vc_t44, VConst(v128::from32p(op.si10)));
    c->cmeq(va.s4(), va.s4(), vc_t44.s4());
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::CEQHI(spu_opcode_t op)
{
    const VLink& va = VGet(op.ra, VType::Int);
    const asmjit::a64::VecV& vc_t45 = VAlloc();
    c->ldr(vc_t45, VConst(v128::from16p(op.si10)));
    c->cmeq(va.h8(), va.h8(), vc_t45.h8());
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::CEQBI(spu_opcode_t op)
{
    const VLink& va = VGet(op.ra, VType::Int);
    const asmjit::a64::VecV& vc_t46 = VAlloc();
    c->ldr(vc_t46, VConst(v128::from8p(op.si10)));
    c->cmeq(va.b16(), va.b16(), vc_t46.b16());
    c->str(va, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::HEQI(spu_opcode_t op)
{
    // Halt if equal immediate (word 3): emit the halt magic write (matches x86/LLVM)
    c->ldr(qw0->w(), SPU_OFF_32(gpr, op.ra, &v128::_u32, 3));
    c->mov(qw1->w(), asmjit::Imm(static_cast<u32>(op.si10)));
    c->cmp(qw0->w(), qw1->w());

    asmjit::Label label = c->newLabel();
    c->b_eq(label);
    a64_halt_body(label, op);
}

void spu_recompiler::HBRA(spu_opcode_t op)
{
    // Hint branch relative - no-op on ARM64
}

void spu_recompiler::HBRR(spu_opcode_t op)
{
    // Hint branch relative - no-op on ARM64
}

void spu_recompiler::ILA(spu_opcode_t op)
{
    const VLink& vr = VAlloc();
    const asmjit::a64::VecV& vc_t48 = VAlloc();
    c->ldr(vc_t48, VConst(v128::from32p(op.i18)));
    c->mov(vr.b16(), vc_t48.b16());
    c->str(vr, SPU_OFF_128(gpr, op.rt));
}

void spu_recompiler::SELB(spu_opcode_t op)
{
    const VLink& va = VGet(op.ra, VType::Int);
    const VLink& vb = VGet(op.rb, VType::Int);
    const VLink& vc = VGet(op.rc, VType::Int);
    c->bsl(vc.b16(), vb.b16(), va.b16()); // (rc & rb) | (~rc & ra)
    c->str(vc, SPU_OFF_128(gpr, op.rt4));
}

void spu_recompiler::SHUFB(spu_opcode_t op)
{
    // res = ab[~c & 0x1f] (ab: low 16 bytes = rb, high 16 bytes = ra)
    // bit7=1,bit6=1: bit5 ? 0x80 : 0xFF; bit7=1,bit6=0: 0; bit7=0: table result
    const VLink& va = VGet(op.ra, VType::Int);
    const VLink& vb = VGet(op.rb, VType::Int);
    const VLink& vc = VGet(op.rc, VType::Int);
    const VLink& vidx = VAlloc();
    const VLink& vra = VAlloc();
    const VLink& vrb = VAlloc();
    const VLink& v1 = VAlloc();
    const VLink& v2 = VAlloc();
    const VLink& vzero = VAlloc();
    const VLink& vmask = VAlloc();
    c->eor(vzero.b16(), vzero.b16(), vzero.b16());
    // idx = ~c & 0xf
    c->not_(vidx.b16(), vc.b16());
    c->ldr(vmask, VConst(v128::from8p(0xf)));
    c->and_(vidx.b16(), vidx.b16(), vmask.b16());
    c->tbl(vra.b16(), va.b16(), vidx.b16());
    c->tbl(vrb.b16(), vb.b16(), vidx.b16());
    // select ra (bit4 set) or rb (bit4 clear)
    c->ldr(vmask, VConst(v128::from8p(0x10)));
    c->and_(v1.b16(), vc.b16(), vmask.b16());
    // Interpreter truth: index = ~c & 0x1f over the 32-byte table {rb, ra}.
    // Index bit 4 (= ~c[4]) selects the half: c[4]==0 -> RA half, c[4]==1 -> RB half.
    c->cmeq(v1.b16(), v1.b16(), vzero.b16()); // bit4 == 0 -> mask 0xff (select RA)
    c->bsl(v1.b16(), vra.b16(), vrb.b16());   // v1 = (bit4==0) ? vra : vrb
    // special values: (c&0xe0)==0xc0 -> 0xFF marker, (c&0xc0)==0xc0 -> marker
    c->ldr(vmask, VConst(v128::from8p(static_cast<s8>(0xe0))));
    c->and_(v2.b16(), vc.b16(), vmask.b16());
    c->ldr(vmask, VConst(v128::from8p(static_cast<s8>(0xc0))));
    c->cmeq(v2.b16(), v2.b16(), vmask.b16()); // (c&0xe0)==0xc0
    c->and_(vra.b16(), vc.b16(), vmask.b16());
    c->cmeq(vra.b16(), vra.b16(), vmask.b16()); // (c&0xc0)==0xc0
    c->urhadd(v2.b16(), vra.b16(), v2.b16());  // 0xFF / 0x80 / 0
    // clear table result where bit7 set, then or
    c->cmlt(vra.b16(), vc.b16(), asmjit::Imm(0)); // bit7 mask (signed byte < 0)
    c->bic(v1.b16(), v1.b16(), vra.b16());
    c->orr(v1.b16(), v1.b16(), v2.b16());
    c->str(v1, SPU_OFF_128(gpr, op.rt4));
}

void spu_recompiler::MPYA(spu_opcode_t op)
{
    // rt = rc + madd(ra & 0xffff, rb & 0xffff) (even halfword products only)
    const VLink& va = VGet(op.ra, VType::Int);
    const VLink& vb = VGet(op.rb, VType::Int);
    const VLink& vrc = VGet(op.rc, VType::Int);
    const VLink& v1 = VAlloc();
    const VLink& v2 = VAlloc();
    const VLink& v3 = VAlloc();
    const VLink& va2 = VAlloc();
    const VLink& vb2 = VAlloc();
    const asmjit::a64::VecV vc_mask = VAlloc();
    c->ldr(vc_mask, VConst(v128::from32p(0xffff)));
    c->and_(va.b16(), va.b16(), vc_mask.b16());
    c->and_(vb.b16(), vb.b16(), vc_mask.b16());
    c->ext(va2.b16(), va.b16(), va.b16(), 8);
    c->ext(vb2.b16(), vb.b16(), vb.b16(), 8);
    c->smull2(v1.s4(), va2.h8(), vb2.h8());  // [e0, 0, e1, 0]
    c->smull2(v2.s4(), va.h8(), vb.h8());    // [e2, 0, e3, 0]
    c->uzp1(v3.s4(), v1.s4(), v2.s4());      // [e0, e1, e2, e3]
    c->add(v3.s4(), v3.s4(), vrc.s4());
    c->str(v3, SPU_OFF_128(gpr, op.rt4));
}

void spu_recompiler::FNMS(spu_opcode_t op)
{
    // rt4 = rc - a*b, extended floats (exp == 0xff) are treated as zero
    const VLink& va = VGet(op.ra, VType::Float);
    const VLink& vb = VGet(op.rb, VType::Float);
    const VLink& vc_ = VGet(op.rc, VType::Float);
    const VLink& vt = VAlloc();
    const asmjit::a64::VecV& vc_mask = VAlloc();
    c->ldr(vc_mask, VConst(v128::from32p(0x7f800000)));
    c->and_(vt.b16(), va.b16(), vc_mask.b16());
    c->cmeq(vt.s4(), vt.s4(), vc_mask.s4());
    c->bic(va.b16(), va.b16(), vt.b16());
    c->and_(vt.b16(), vb.b16(), vc_mask.b16());
    c->cmeq(vt.s4(), vt.s4(), vc_mask.s4());
    c->bic(vb.b16(), vb.b16(), vt.b16());
    c->fmul(va.s4(), va.s4(), vb.s4());
    c->fsub(vc_.s4(), vc_.s4(), va.s4());
    c->str(vc_, SPU_OFF_128(gpr, op.rt4));
}

void spu_recompiler::FMA(spu_opcode_t op)
{
    // rt4 = a*b + rc, extended floats (exp == 0xff) are treated as zero
    const VLink& va = VGet(op.ra, VType::Float);
    const VLink& vb = VGet(op.rb, VType::Float);
    const VLink& vc_ = VGet(op.rc, VType::Float);
    const VLink& vt = VAlloc();
    const asmjit::a64::VecV& vc_mask = VAlloc();
    c->ldr(vc_mask, VConst(v128::from32p(0x7f800000)));
    c->and_(vt.b16(), va.b16(), vc_mask.b16());
    c->cmeq(vt.s4(), vt.s4(), vc_mask.s4());
    c->bic(va.b16(), va.b16(), vt.b16());
    c->and_(vt.b16(), vb.b16(), vc_mask.b16());
    c->cmeq(vt.s4(), vt.s4(), vc_mask.s4());
    c->bic(vb.b16(), vb.b16(), vt.b16());
    c->fmul(va.s4(), va.s4(), vb.s4());
    c->fadd(va.s4(), va.s4(), vc_.s4());
    c->str(va, SPU_OFF_128(gpr, op.rt4));
}

void spu_recompiler::FMS(spu_opcode_t op)
{
    // rt4 = a*b - rc, extended floats (exp == 0xff) are treated as zero
    const VLink& va = VGet(op.ra, VType::Float);
    const VLink& vb = VGet(op.rb, VType::Float);
    const VLink& vc_ = VGet(op.rc, VType::Float);
    const VLink& vt = VAlloc();
    const asmjit::a64::VecV& vc_mask = VAlloc();
    c->ldr(vc_mask, VConst(v128::from32p(0x7f800000)));
    c->and_(vt.b16(), va.b16(), vc_mask.b16());
    c->cmeq(vt.s4(), vt.s4(), vc_mask.s4());
    c->bic(va.b16(), va.b16(), vt.b16());
    c->and_(vt.b16(), vb.b16(), vc_mask.b16());
    c->cmeq(vt.s4(), vt.s4(), vc_mask.s4());
    c->bic(vb.b16(), vb.b16(), vt.b16());
    c->fmul(va.s4(), va.s4(), vb.s4());
    c->fsub(va.s4(), va.s4(), vc_.s4());
    c->str(va, SPU_OFF_128(gpr, op.rt4));
}
