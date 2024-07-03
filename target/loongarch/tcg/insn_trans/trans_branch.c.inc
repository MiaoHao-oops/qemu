/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (c) 2021 Loongson Technology Corporation Limited
 */

static bool trans_b(DisasContext *ctx, arg_b *a)
{
    gen_goto_tb(ctx, 0, ctx->base.pc_next + a->offs);
    ctx->base.is_jmp = DISAS_NORETURN;
    return true;
}


static void handle_shadow_stack(DisasContext *ctx, TCGv dest, TCGv src1, int is_ret)
{
    TCGv tmp = tcg_temp_new();
    TCGv top = tcg_temp_new();
    TCGLabel *end = gen_new_label();

    tcg_gen_ld_tl(tmp, tcg_env,
            offsetof(CPULoongArchState, ss_en));
    //is shadow stack enabled?
    tcg_gen_brcond_tl(TCG_COND_EQ, tmp, ctx->zero, end);

    tcg_gen_ld32u_tl(top, tcg_env,
            offsetof(CPULoongArchState, ssbuf_top));

    if (is_ret) {
        TCGLabel *l1 = gen_new_label();
        TCGLabel *l2 = gen_new_label();

        //is shadow stack empty? top >= size means empty
        tcg_gen_movi_tl(tmp, SSBUF_SIZE);
        tcg_gen_brcond_tl(TCG_COND_GE, top, tmp, l1);

        // not empty, pop shadow stack and check
        // load current entry from safecfg addr 0xff800 + top
        tcg_gen_movi_tl(tmp, 0xff800);
        tcg_gen_add_tl(tmp, tmp, top);
        gen_helper_safecfgr_d(tmp, tcg_env, tmp);

        // ss entry val equal to ra?
        tcg_gen_brcond_tl(TCG_COND_EQ, src1, tmp, l2);

        generate_exception(ctx, EXCCODE_SSBAD);

        gen_set_label(l1);
        //stack empty, raise exception
        generate_exception(ctx, EXCCODE_SSUDF);

        gen_set_label(l2);
        // succeed. top = top + 8
        tcg_gen_ld32u_tl(top, tcg_env,
            offsetof(CPULoongArchState, ssbuf_top));
        tcg_gen_addi_tl(top, top, 8);
        tcg_gen_st32_tl(top, tcg_env,
                offsetof(CPULoongArchState, ssbuf_top));
    } else {
        TCGLabel *l1 = gen_new_label();

        tcg_gen_movi_tl(tmp, SSBUF_SIZE);

        //is shadow stack full? top <= zero means full
        tcg_gen_brcond_tl(TCG_COND_LE, top, ctx->zero, l1);

        // save current ra
        tcg_gen_addi_tl(top, top, -8);
        tcg_gen_movi_tl(tmp, 0xff800);
        tcg_gen_add_tl(tmp, tmp, top);
        gen_helper_safecfgw_d(tcg_env, tmp, dest);

        // helper might destroy top, reload
        tcg_gen_ld32u_tl(top, tcg_env,
            offsetof(CPULoongArchState, ssbuf_top));
        tcg_gen_addi_tl(top, top, -8);
        tcg_gen_st32_tl(top, tcg_env,
                offsetof(CPULoongArchState, ssbuf_top));

        //goto end
        tcg_gen_brcond_tl(TCG_COND_EQ, top, top, end);

        gen_set_label(l1);

        //stack full, raise overflow exception
        generate_exception(ctx, EXCCODE_SSOVF);
    }

    gen_set_label(end);
}

static bool trans_bl(DisasContext *ctx, arg_bl *a)
{
    tcg_gen_movi_tl(cpu_gpr[1], make_address_pc(ctx, ctx->base.pc_next + 4));

    handle_shadow_stack(ctx, cpu_gpr[1], 0, 0);

    gen_goto_tb(ctx, 0, ctx->base.pc_next + a->offs);
    ctx->base.is_jmp = DISAS_NORETURN;
    return true;
}

static bool trans_jirl(DisasContext *ctx, arg_jirl *a)
{
    TCGv dest = gpr_dst(ctx, a->rd, EXT_NONE);
    TCGv src1 = gpr_src(ctx, a->rj, EXT_NONE);
    int is_ret = (a->rd == 0 && a->rj == 1 && a->imm == 0) ? 1 : 0;
    int is_jr = (a->rd == 0 && a->rj != 1 && a->imm == 0) ? 1 : 0;

    TCGv addr = make_address_i(ctx, src1, a->imm);
    tcg_gen_mov_tl(cpu_pc, addr);
    tcg_gen_movi_tl(dest, make_address_pc(ctx, ctx->base.pc_next + 4));
    gen_set_gpr(a->rd, dest, EXT_NONE);

    //zero as dest will trigger tcg assertion in tcg_gen_code
    if (!is_jr) 
        handle_shadow_stack(ctx, dest, src1, is_ret);

    tcg_gen_lookup_and_goto_ptr();
    ctx->base.is_jmp = DISAS_NORETURN;
    return true;
}

static void gen_bc(DisasContext *ctx, TCGv src1, TCGv src2,
                   target_long offs, TCGCond cond)
{
    TCGLabel *l = gen_new_label();
    tcg_gen_brcond_tl(cond, src1, src2, l);
    gen_goto_tb(ctx, 1, ctx->base.pc_next + 4);
    gen_set_label(l);
    gen_goto_tb(ctx, 0, ctx->base.pc_next + offs);
    ctx->base.is_jmp = DISAS_NORETURN;
}

static bool gen_rr_bc(DisasContext *ctx, arg_rr_offs *a, TCGCond cond)
{
    TCGv src1 = gpr_src(ctx, a->rj, EXT_NONE);
    TCGv src2 = gpr_src(ctx, a->rd, EXT_NONE);

    gen_bc(ctx, src1, src2, a->offs, cond);
    return true;
}

static bool gen_rz_bc(DisasContext *ctx, arg_r_offs *a, TCGCond cond)
{
    TCGv src1 = gpr_src(ctx, a->rj, EXT_NONE);
    TCGv src2 = tcg_constant_tl(0);

    gen_bc(ctx, src1, src2, a->offs, cond);
    return true;
}

static bool gen_cz_bc(DisasContext *ctx, arg_c_offs *a, TCGCond cond)
{
    TCGv src1 = tcg_temp_new();
    TCGv src2 = tcg_constant_tl(0);

    tcg_gen_ld8u_tl(src1, tcg_env,
                    offsetof(CPULoongArchState, cf[a->cj]));
    gen_bc(ctx, src1, src2, a->offs, cond);
    return true;
}

TRANS(beq, ALL, gen_rr_bc, TCG_COND_EQ)
TRANS(bne, ALL, gen_rr_bc, TCG_COND_NE)
TRANS(blt, ALL, gen_rr_bc, TCG_COND_LT)
TRANS(bge, ALL, gen_rr_bc, TCG_COND_GE)
TRANS(bltu, ALL, gen_rr_bc, TCG_COND_LTU)
TRANS(bgeu, ALL, gen_rr_bc, TCG_COND_GEU)
TRANS(beqz, ALL, gen_rz_bc, TCG_COND_EQ)
TRANS(bnez, ALL, gen_rz_bc, TCG_COND_NE)
TRANS(bceqz, 64, gen_cz_bc, TCG_COND_EQ)
TRANS(bcnez, 64, gen_cz_bc, TCG_COND_NE)
