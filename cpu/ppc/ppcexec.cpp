/*
DingusPPC - The Experimental PowerPC Macintosh emulator
Copyright (C) 2018-20 divingkatae and maximum
                      (theweirdo)     spatium

(Contact divingkatae#1017 or powermax#2286 on Discord for more info)

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include <thirdparty/loguru/loguru.hpp>
#include <stdio.h>
#include <string>
#include <iostream>
#include <stdexcept>
#include <algorithm>
#include <map>
#include <unordered_map>
#include <chrono>
#include <setjmp.h>

#include "ppcemu.h"
#include "ppcmmu.h"

using namespace std;

MemCtrlBase *mem_ctrl_instance = 0;

bool power_on = 1;

SetPRS ppc_state;

bool grab_branch;
bool grab_exception;
bool grab_return;
bool grab_breakpoint;

bool rc_flag = 0;
bool oe_flag = 0;

uint32_t ppc_cur_instruction; //Current instruction for the PPC
uint32_t ppc_effective_address;
uint32_t ppc_next_instruction_address; //Used for branching, setting up the NIA

BB_end_kind bb_kind; /* basic block end */

uint64_t timebase_counter; /* internal timebase counter */

clock_t clock_test_begin; //Used to make sure the TBR does not increment so quickly.

/** Opcode lookup tables. */

/** Primary opcode (bits 0...5) lookup table. */
static PPCOpcode OpcodeGrabber[] = {
    ppc_illegalop, ppc_illegalop, ppc_illegalop, ppc_twi,       ppc_opcode4,
    ppc_illegalop, ppc_illegalop, ppc_mulli,     ppc_subfic,    power_dozi,
    ppc_cmpli,     ppc_cmpi,      ppc_addic,     ppc_addicdot,  ppc_addi,
    ppc_addis,     ppc_opcode16,  ppc_sc,        ppc_opcode18,  ppc_opcode19,
    ppc_rlwimi,    ppc_rlwinm,    power_rlmi,    ppc_rlwnm,     ppc_ori,
    ppc_oris,      ppc_xori,      ppc_xoris,     ppc_andidot,   ppc_andisdot,
    ppc_illegalop, ppc_opcode31,  ppc_lwz,       ppc_lwzu,      ppc_lbz,
    ppc_lbzu,      ppc_stw,       ppc_stwu,      ppc_stb,       ppc_stbu,
    ppc_lhz,       ppc_lhzu,      ppc_lha,       ppc_lhau,      ppc_sth,
    ppc_sthu,      ppc_lmw,       ppc_stmw,      ppc_lfs,       ppc_lfsu,
    ppc_lfd,       ppc_lfdu,      ppc_stfs,      ppc_stfsu,     ppc_stfd,
    ppc_stfdu,     ppc_psq_l,     ppc_psq_lu,    ppc_illegalop, ppc_illegalop,
    ppc_psq_st,    ppc_psq_stu,   ppc_illegalop, ppc_opcode63
};

/** Lookup tables for branch instructions. */
static PPCOpcode SubOpcode16Grabber[] = {
    ppc_bc, ppc_bcl, ppc_bca, ppc_bcla
};

static PPCOpcode SubOpcode18Grabber[] = {
    ppc_b, ppc_bl, ppc_ba, ppc_bla
};

/** General conditional register instructions decoding table. */

static PPCOpcode SubOpcode31Grabber[1024] = { ppc_illegalsubop31 };

/** Single-precision floating-point instructions decoding table. */
static PPCOpcode SubOpcode59Grabber[1024] = { ppc_illegalsubop59 };

/** Double-precision floating-point instructions decoding table. */
static PPCOpcode SubOpcode63Grabber[1024] = { ppc_illegalsubop63 };

/** Opcode decoding functions. */

void ppc_illegalop() {
    uint8_t illegal_code = ppc_cur_instruction >> 26;
    uint32_t grab_it = (uint32_t)illegal_code;
    LOG_F(ERROR, "Illegal opcode reported: %d Report this! \n", grab_it);
    exit(-1);
}

void ppc_illegalsubop19() {
    uint16_t illegal_subcode = ppc_cur_instruction & 0x7FF;
    uint32_t grab_it = (uint32_t)illegal_subcode;
    LOG_F(ERROR, "Illegal subopcode for 19 reported: %d Report this! \n", grab_it);
    exit(-1);
}

void ppc_illegalsubop31() {
    uint16_t illegal_subcode = ppc_cur_instruction & 0x7FF;
    uint32_t grab_it = (uint32_t)illegal_subcode;
    LOG_F(ERROR, "Illegal subopcode for 31 reported: %d Report this! \n", grab_it);
    exit(-1);
}

void ppc_illegalsubop59() {
    uint16_t illegal_subcode = ppc_cur_instruction & 0x7FF;
    uint32_t grab_it = (uint32_t)illegal_subcode;
    LOG_F(ERROR, "Illegal subopcode for 59 reported: %d Report this! \n", grab_it);
    exit(-1);
}

void ppc_illegalsubop63() {
    uint16_t illegal_subcode = ppc_cur_instruction & 0x7FF;
    uint32_t grab_it = (uint32_t)illegal_subcode;
    LOG_F(ERROR, "Illegal subopcode for 63 reported: %d Report this! \n", grab_it);
    exit(-1);
}


void ppc_opcode4() {
    LOG_F(INFO, "Reading from Opcode 4 table \n");
    uint8_t subop_grab = ppc_cur_instruction & 3;
    uint32_t regrab = (uint32_t)subop_grab;
    LOG_F(ERROR, "Executing subopcode entry %d \n"
           ".. or would if I bothered to implement it. SORRY!", regrab);
    exit(0);
}

void ppc_opcode16() {
    SubOpcode16Grabber[ppc_cur_instruction & 3]();
}

void ppc_opcode18() {
    SubOpcode18Grabber[ppc_cur_instruction & 3]();
}

void ppc_opcode19() {
    uint16_t subop_grab = ppc_cur_instruction & 0x7FF;

#ifdef EXHAUSTIVE_DEBUG
    uint32_t regrab = (uint32_t)subop_grab;
    LOG_F(INFO, "Executing Opcode 19 table subopcode entry \n", regrab);
#endif // EXHAUSTIVE_DEBUG

    if (subop_grab == 32) {
        ppc_bclr();
    }
    else if (subop_grab == 33) {
        ppc_bclrl();
}
    else if (subop_grab == 1056) {
        ppc_bcctr();
    }
    else if (subop_grab == 1057) {
        ppc_bcctrl();

    }
    else {
        switch (subop_grab) {
        case 66:
            ppc_crnor();
            break;
        case 100:
            ppc_rfi();
            break;
        case 258:
            ppc_crandc();
            break;
        case 300:
            ppc_isync();
            break;
        case 386:
            ppc_crxor();
            break;
        case 450:
            ppc_crnand();
            break;
        case 514:
            ppc_crand();
            break;
        case 578:
            ppc_creqv();
            break;
        case 834:
            ppc_crorc();
            break;
        case 898:
            ppc_cror();
            break;
        default: //Illegal opcode - should never happen
            ppc_illegalsubop19();
        }
    }
}

void ppc_opcode31() {
    uint16_t subop_grab = (ppc_cur_instruction & 0x7FF) >> 1;

    rc_flag = ppc_cur_instruction & 0x1;
    oe_flag = ppc_cur_instruction & 0x400;

#ifdef EXHAUSTIVE_DEBUG
    uint32_t regrab = (uint32_t)subop_grab;
    LOG_F(INFO, "Executing Opcode 63 table subopcode entry \n", regrab);
#endif // EXHAUSTIVE_DEBUG

    SubOpcode31Grabber[subop_grab]();
}

void ppc_opcode59() {
    uint16_t subop_grab = (ppc_cur_instruction & 0x7FF) >> 1;
    rc_flag = subop_grab & 1;
#ifdef EXHAUSTIVE_DEBUG
    uint32_t regrab = (uint32_t)subop_grab;
    LOG_F(INFO, "Executing Opcode 59 table subopcode entry \n", regrab);
#endif // EXHAUSTIVE_DEBUG
    SubOpcode59Grabber[subop_grab]();
}

void ppc_opcode63() {
    uint16_t subop_grab = (ppc_cur_instruction & 0x7FF) >> 1;
    rc_flag = subop_grab & 1;
#ifdef EXHAUSTIVE_DEBUG
    uint32_t regrab = (uint32_t)subop_grab;
    LOG_F(INFO, "Executing Opcode 63 table subopcode entry \n", regrab);
#endif // EXHAUSTIVE_DEBUG
    SubOpcode63Grabber[subop_grab]();
}

void ppc_main_opcode() {
    //Grab the main opcode
    uint8_t ppc_mainop = (ppc_cur_instruction >> 26) & 63;
    OpcodeGrabber[ppc_mainop]();
}

/** Old time base register (TBR) update code. */
void tbr_update()
{
    clock_t clock_test_current = clock();
    uint32_t test_clock = ((uint32_t)(clock_test_current - clock_test_begin)) / CLOCKS_PER_SEC;
    if (test_clock) {
        if (ppc_state.tbr[0] != 0xFFFFFFFF) {
            ppc_state.tbr[0]++;
        }
        else {
            ppc_state.tbr[0] = 0;
            if (ppc_state.tbr[1] != 0xFFFFFFFF) {
                ppc_state.tbr[1]++;
            }
            else {
                ppc_state.tbr[1] = 0;
            }
        }
        clock_test_begin = clock();
        //Placeholder Decrementing Code
        if (ppc_state.spr[22] > 0) {
            ppc_state.spr[22]--;
        }
    }
}

/** Execute PPC code as long as power is on. */
#if 0
void ppc_exec()
{
    while (power_on) {
        //printf("PowerPC Address: %x \n", ppc_state.pc);
        quickinstruction_translate(ppc_state.pc);
        ppc_main_opcode();
        if (grab_branch & !grab_exception) {
            ppc_state.pc = ppc_next_instruction_address;
            grab_branch = 0;
            tbr_update();
        }
        else if (grab_return | grab_exception) {
            ppc_state.pc = ppc_next_instruction_address;
            grab_exception = 0;
            grab_return = 0;
            tbr_update();
        }
        else {
            ppc_state.pc += 4;
            tbr_update();
        }
    }
}
#else
void ppc_exec()
{
    uint32_t bb_start_la, page_start;
    uint8_t* pc_real;

    /* start new basic block */
    bb_start_la = ppc_state.pc;
    bb_kind = BB_end_kind::BB_NONE;

    if (setjmp(exc_env)) {
        /* reaching here means we got a low-level exception */
        timebase_counter += (ppc_state.pc - bb_start_la) >> 2;
        bb_start_la = ppc_next_instruction_address;
        pc_real = quickinstruction_translate(bb_start_la);
        page_start = bb_start_la & 0xFFFFF000;
        ppc_state.pc = bb_start_la;
        bb_kind = BB_end_kind::BB_NONE;
        goto again;
    }

    /* initial MMU translation for the current code page. */
    pc_real = quickinstruction_translate(bb_start_la);

    /* set current code page limits */
    page_start = bb_start_la & 0xFFFFF000;

again:
    while (power_on) {
        ppc_main_opcode();
        if (bb_kind != BB_end_kind::BB_NONE) {
            timebase_counter += (ppc_state.pc - bb_start_la) >> 2;
            bb_start_la = ppc_next_instruction_address;
            if ((ppc_next_instruction_address & 0xFFFFF000) != page_start) {
                page_start = bb_start_la & 0xFFFFF000;
                pc_real = quickinstruction_translate(bb_start_la);
            }
            else {
                pc_real += (int)bb_start_la - (int)ppc_state.pc;
                ppc_set_cur_instruction(pc_real);
            }
            ppc_state.pc = bb_start_la;
            bb_kind = BB_end_kind::BB_NONE;
        }
        else {
            ppc_state.pc += 4;
            pc_real += 4;
            ppc_set_cur_instruction(pc_real);
        }
    }
}
#endif

/** Execute one PPC instruction. */
#if 0
void ppc_exec_single()
{
    quickinstruction_translate(ppc_state.pc);
    ppc_main_opcode();
    if (grab_branch && !grab_exception) {
        ppc_state.pc = ppc_next_instruction_address;
        grab_branch = 0;
        tbr_update();
    }
    else if (grab_return || grab_exception) {
        ppc_state.pc = ppc_next_instruction_address;
        grab_exception = 0;
        grab_return = 0;
        tbr_update();
    }
    else {
        ppc_state.pc += 4;
        tbr_update();
    }
}
#else
void ppc_exec_single()
{
    if (setjmp(exc_env)) {
        /* reaching here means we got a low-level exception */
        timebase_counter += 1;
        ppc_state.pc = ppc_next_instruction_address;
        bb_kind = BB_end_kind::BB_NONE;
        return;
    }

    quickinstruction_translate(ppc_state.pc);
    ppc_main_opcode();
    if (bb_kind != BB_end_kind::BB_NONE) {
        ppc_state.pc = ppc_next_instruction_address;
        bb_kind = BB_end_kind::BB_NONE;
    }
    else {
        ppc_state.pc += 4;
    }
    timebase_counter += 1;
}
#endif

/** Execute PPC code until goal_addr is reached. */
#if 0
void ppc_exec_until(uint32_t goal_addr)
{
    while (ppc_state.pc != goal_addr) {
        quickinstruction_translate(ppc_state.pc);
        ppc_main_opcode();
        if (grab_branch && !grab_exception) {
            ppc_state.pc = ppc_next_instruction_address;
            grab_branch = 0;
            tbr_update();
        }
        else if (grab_return || grab_exception) {
            ppc_state.pc = ppc_next_instruction_address;
            grab_exception = 0;
            grab_return = 0;
            tbr_update();
        }
        else {
            ppc_state.pc += 4;
            tbr_update();
        }
        ppc_cur_instruction = 0;
    }
}
#else
void ppc_exec_until(uint32_t goal_addr)
{
    uint32_t bb_start_la, page_start;
    uint8_t* pc_real;

    /* start new basic block */
    bb_start_la = ppc_state.pc;
    bb_kind = BB_end_kind::BB_NONE;

    if (setjmp(exc_env)) {
        /* reaching here means we got a low-level exception */
        timebase_counter += (ppc_state.pc - bb_start_la) >> 2;
        bb_start_la = ppc_next_instruction_address;
        pc_real = quickinstruction_translate(bb_start_la);
        page_start = bb_start_la & 0xFFFFF000;
        ppc_state.pc = bb_start_la;
        bb_kind = BB_end_kind::BB_NONE;
        goto again;
    }

    /* initial MMU translation for the current code page. */
    pc_real = quickinstruction_translate(bb_start_la);

    /* set current code page limits */
    page_start = bb_start_la & 0xFFFFF000;

again:
    while (ppc_state.pc != goal_addr) {
        ppc_main_opcode();
        if (bb_kind != BB_end_kind::BB_NONE) {
            timebase_counter += (ppc_state.pc - bb_start_la) >> 2;
            bb_start_la = ppc_next_instruction_address;
            if ((ppc_next_instruction_address & 0xFFFFF000) != page_start) {
                page_start = bb_start_la & 0xFFFFF000;
                pc_real = quickinstruction_translate(bb_start_la);
            }
            else {
                pc_real += (int)bb_start_la - (int)ppc_state.pc;
                ppc_set_cur_instruction(pc_real);
            }
            ppc_state.pc = bb_start_la;
            bb_kind = BB_end_kind::BB_NONE;
        }
        else {
            ppc_state.pc += 4;
            pc_real += 4;
            ppc_set_cur_instruction(pc_real);
        }
    }
}
#endif

void ppc_opcode31_init() {
    SubOpcode31Grabber[0] = ppc_cmp;
    SubOpcode31Grabber[4] = ppc_tw;
    SubOpcode31Grabber[32] = ppc_cmpl;

    SubOpcode31Grabber[8] = SubOpcode31Grabber[520] = ppc_subfc;
    SubOpcode31Grabber[40] = SubOpcode31Grabber[552] = ppc_subf;
    SubOpcode31Grabber[104] = SubOpcode31Grabber[616] = ppc_neg;
    SubOpcode31Grabber[136] = SubOpcode31Grabber[648] = ppc_subfe;
    SubOpcode31Grabber[200] = SubOpcode31Grabber[712] = ppc_subfze;
    SubOpcode31Grabber[232] = SubOpcode31Grabber[744] = ppc_subfme;

    SubOpcode31Grabber[10] = SubOpcode31Grabber[522] = ppc_addc;
    SubOpcode31Grabber[138] = SubOpcode31Grabber[650] = ppc_adde;
    SubOpcode31Grabber[202] = SubOpcode31Grabber[714] = ppc_addze;
    SubOpcode31Grabber[234] = SubOpcode31Grabber[746] = ppc_addme;
    SubOpcode31Grabber[266] = SubOpcode31Grabber[778] = ppc_add;

    SubOpcode31Grabber[11] = ppc_mulhwu;
    SubOpcode31Grabber[75] = ppc_mulhw;
    SubOpcode31Grabber[235] = SubOpcode31Grabber[747] = ppc_mullw;
    SubOpcode31Grabber[459] = SubOpcode31Grabber[971] = ppc_divwu;
    SubOpcode31Grabber[491] = SubOpcode31Grabber[1003] = ppc_divw;

    SubOpcode31Grabber[20] = ppc_lwarx;
    SubOpcode31Grabber[23] = ppc_lwzx;
    SubOpcode31Grabber[55] = ppc_lwzux;
    SubOpcode31Grabber[87] = ppc_lbzx;
    SubOpcode31Grabber[119] = ppc_lbzux;
    SubOpcode31Grabber[279] = ppc_lhzx;
    SubOpcode31Grabber[311] = ppc_lhzux;
    SubOpcode31Grabber[343] = ppc_lhax;
    SubOpcode31Grabber[375] = ppc_lhaux;
    SubOpcode31Grabber[533] = ppc_lswx;
    SubOpcode31Grabber[534] = ppc_lwbrx;
    SubOpcode31Grabber[535] = ppc_lfsx;
    SubOpcode31Grabber[567] = ppc_lfsux;
    SubOpcode31Grabber[597] = ppc_lswi;
    SubOpcode31Grabber[599] = ppc_lfdx;
    SubOpcode31Grabber[631] = ppc_lfdux;
    SubOpcode31Grabber[790] = ppc_lhbrx;

    SubOpcode31Grabber[150] = ppc_stwcx;
    SubOpcode31Grabber[151] = ppc_stwx;
    SubOpcode31Grabber[183] = ppc_stwux;
    SubOpcode31Grabber[215] = ppc_stbx;
    SubOpcode31Grabber[247] = ppc_stbux;
    SubOpcode31Grabber[407] = ppc_sthx;
    SubOpcode31Grabber[439] = ppc_sthux;
    SubOpcode31Grabber[661] = ppc_stswx;
    SubOpcode31Grabber[662] = ppc_stwbrx;
    SubOpcode31Grabber[663] = ppc_stfsx;
    SubOpcode31Grabber[695] = ppc_stfsux;
    SubOpcode31Grabber[725] = ppc_stswi;
    SubOpcode31Grabber[727] = ppc_stfdx;
    SubOpcode31Grabber[759] = ppc_stfdux;
    SubOpcode31Grabber[918] = ppc_sthbrx;
    SubOpcode31Grabber[983] = ppc_stfiwx;

    SubOpcode31Grabber[24] = ppc_slw;
    SubOpcode31Grabber[28] = ppc_and;
    SubOpcode31Grabber[60] = ppc_andc;
    SubOpcode31Grabber[124] = ppc_nor;
    SubOpcode31Grabber[284] = ppc_eqv;
    SubOpcode31Grabber[316] = ppc_xor;
    SubOpcode31Grabber[412] = ppc_orc;
    SubOpcode31Grabber[444] = ppc_or;
    SubOpcode31Grabber[476] = ppc_nand;
    SubOpcode31Grabber[536] = ppc_srw;
    SubOpcode31Grabber[792] = ppc_sraw;
    SubOpcode31Grabber[824] = ppc_srawi;
    SubOpcode31Grabber[922] = ppc_extsh;
    SubOpcode31Grabber[954] = ppc_extsb;

    SubOpcode31Grabber[26] = ppc_cntlzw;

    SubOpcode31Grabber[19] = ppc_mfcr;
    SubOpcode31Grabber[83] = ppc_mfmsr;
    SubOpcode31Grabber[144] = ppc_mtcrf;
    SubOpcode31Grabber[146] = ppc_mtmsr;
    SubOpcode31Grabber[210] = ppc_mtsr;
    SubOpcode31Grabber[242] = ppc_mtsrin;
    SubOpcode31Grabber[256] = ppc_mcrxr;
    SubOpcode31Grabber[339] = ppc_mfspr;
    SubOpcode31Grabber[371] = ppc_mftb;
    SubOpcode31Grabber[467] = ppc_mtspr;
    SubOpcode31Grabber[595] = ppc_mfsr;
    SubOpcode31Grabber[659] = ppc_mfsrin;

    SubOpcode31Grabber[54] = ppc_dcbst;
    SubOpcode31Grabber[86] = ppc_dcbf;
    SubOpcode31Grabber[246] = ppc_dcbtst;
    SubOpcode31Grabber[278] = ppc_dcbt;
    SubOpcode31Grabber[598] = ppc_sync;
    SubOpcode31Grabber[470] = ppc_dcbi;
    SubOpcode31Grabber[1014] = ppc_dcbz;

    SubOpcode31Grabber[29] = power_maskg;
    SubOpcode31Grabber[107] = SubOpcode31Grabber[619] = power_mul;
    SubOpcode31Grabber[152] = power_slq;
    SubOpcode31Grabber[153] = power_sle;
    SubOpcode31Grabber[184] = power_sliq;
    SubOpcode31Grabber[216] = power_sllq;
    SubOpcode31Grabber[217] = power_sleq;
    SubOpcode31Grabber[248] = power_slliq;
    SubOpcode31Grabber[264] = SubOpcode31Grabber[776] = power_doz;
    SubOpcode31Grabber[277] = power_lscbx;
    SubOpcode31Grabber[331] = SubOpcode31Grabber[843] = power_div;
    SubOpcode31Grabber[360] = SubOpcode31Grabber[872] = power_abs;
    SubOpcode31Grabber[363] = SubOpcode31Grabber[875] = power_divs;
    SubOpcode31Grabber[488] = SubOpcode31Grabber[1000] = power_nabs;
    SubOpcode31Grabber[531] = power_clcs;
    SubOpcode31Grabber[537] = power_rrib;
    SubOpcode31Grabber[541] = power_maskir;
    SubOpcode31Grabber[664] = power_srq;
    SubOpcode31Grabber[665] = power_sre;
    SubOpcode31Grabber[696] = power_sriq;
    SubOpcode31Grabber[728] = power_srlq;
    SubOpcode31Grabber[729] = power_sreq;
    SubOpcode31Grabber[760] = power_srliq;
    SubOpcode31Grabber[920] = power_sraq;
    SubOpcode31Grabber[921] = power_srea;
    SubOpcode31Grabber[952] = power_sraiq;

    SubOpcode31Grabber[306] = ppc_tlbie;
    SubOpcode31Grabber[370] = ppc_tlbia;
    SubOpcode31Grabber[566] = ppc_tlbsync;
    SubOpcode31Grabber[854] = ppc_eieio;
    SubOpcode31Grabber[982] = ppc_icbi;
    SubOpcode31Grabber[978] = ppc_tlbld;
    SubOpcode31Grabber[1010] = ppc_tlbli;
}

void ppc_opcode59_init() {
    SubOpcode63Grabber[18] = ppc_fdivs;
    SubOpcode63Grabber[20] = ppc_fsubs;
    SubOpcode63Grabber[22] = ppc_fsqrts;
    SubOpcode63Grabber[24] = ppc_fres;

    for (int i = 25; i < 1024; i += 32) {
        SubOpcode63Grabber[i] = ppc_fmults;
    }

    for (int i = 28; i < 1024; i += 32) {
        SubOpcode63Grabber[i] = ppc_fmsubs;
    }

    for (int i = 29; i < 1024; i += 32) {
        SubOpcode63Grabber[i] = ppc_fmadds;
    }

    for (int i = 30; i < 1024; i += 32) {
        SubOpcode63Grabber[i] = ppc_fnmsubs;
    }

    for (int i = 31; i < 1024; i += 32) {
        SubOpcode63Grabber[i] = ppc_fnmadds;
    }
}

void ppc_opcode63_init() {
    SubOpcode63Grabber[0] = ppc_fcmpu;
    SubOpcode63Grabber[12] = ppc_frsp;
    SubOpcode63Grabber[14] = ppc_fctiw;
    SubOpcode63Grabber[15] = ppc_fctiwz;
    SubOpcode63Grabber[18] = ppc_fdiv;
    SubOpcode63Grabber[20] = ppc_fsub;
    SubOpcode63Grabber[21] = ppc_fadd;
    SubOpcode63Grabber[22] = ppc_fsqrt;
    SubOpcode63Grabber[26] = ppc_frsqrte;
    SubOpcode63Grabber[32] = ppc_fcmpo;
    SubOpcode63Grabber[38] = ppc_mtfsb1;
    SubOpcode63Grabber[40] = ppc_fneg;
    SubOpcode63Grabber[64] = ppc_mcrfs;
    SubOpcode63Grabber[70] = ppc_mtfsb0;
    SubOpcode63Grabber[72] = ppc_fmr;
    SubOpcode63Grabber[134] = ppc_mtfsfi;
    SubOpcode63Grabber[136] = ppc_fnabs;
    SubOpcode63Grabber[264] = ppc_fabs;
    SubOpcode63Grabber[583] = ppc_mffs;
    SubOpcode63Grabber[711] = ppc_mtfsf;

    for (int i = 23; i < 1024; i += 32) {
        SubOpcode63Grabber[i] = ppc_fsel;
    }

    for (int i = 25; i < 1024; i += 32) {
        SubOpcode63Grabber[i] = ppc_fmult;
    }

    for (int i = 28; i < 1024; i += 32) {
        SubOpcode63Grabber[i] = ppc_fmsub;
    }

    for (int i = 29; i < 1024; i += 32) {
        SubOpcode63Grabber[i] = ppc_fmadd;
    }

    for (int i = 30; i < 1024; i += 32) {
        SubOpcode63Grabber[i] = ppc_fnmsub;
    }

    for (int i = 31; i < 1024; i += 32) {
        SubOpcode63Grabber[i] = ppc_fnmadd;
    }
}

void ppc_init_opcode_tables() {
    ppc_opcode31_init();
    ppc_opcode59_init();
    ppc_opcode63_init();
}

void ppc_cpu_init(MemCtrlBase *mem_ctrl, uint32_t proc_version)
{
    int i;

    mem_ctrl_instance = mem_ctrl;

    clock_test_begin = clock();
    timebase_counter = 0;

    /* zero all GPRs as prescribed for MPC601 */
    /* For later PPC CPUs, GPR content is undefined */
    for (i = 0; i < 32; i++) {
        ppc_state.gpr[i] = 0;
    }

    /* zero all FPRs as prescribed for MPC601 */
    /* For later PPC CPUs, GPR content is undefined */
    for (i = 0; i < 32; i++) {
        ppc_state.fpr[i].int64_r = 0;
    }

    /* zero all segment registers as prescribed for MPC601 */
    /* For later PPC CPUs, SR content is undefined */
    for (i = 0; i < 16; i++) {
        ppc_state.sr[i] = 0;
    }

    ppc_state.cr = 0;
    ppc_state.fpscr = 0;

    ppc_state.pc = 0;

    ppc_state.tbr[0] = 0;
    ppc_state.tbr[1] = 0;

    /* zero all SPRs */
    for (i = 0; i < 1024; i++) {
        ppc_state.spr[i] = 0;
    }

    ppc_state.spr[SPR::PVR] = proc_version;

    if ((proc_version & 0xFFFF0000) == 0x00010000) {
        /* MPC601 sets MSR[ME] bit during hard reset / Power-On */
        ppc_state.msr = 0x1040;
    } else {
        ppc_state.msr = 0x40;
        ppc_state.spr[SPR::DEC] = 0xFFFFFFFFUL;
    }

    ppc_mmu_init();

    ppc_init_opcode_tables();

    /* redirect code execution to reset vector */
    ppc_state.pc = 0xFFF00100;
}

void print_gprs()
{
    for (int i = 0; i < 32; i++)
        cout << "GPR " << dec << i << " : " << uppercase << hex
            << ppc_state.gpr[i] << endl;

    cout << "PC: " << uppercase << hex << ppc_state.pc << endl;
    cout << "LR: " << uppercase << hex << ppc_state.spr[SPR::LR] << endl;
    cout << "CR: " << uppercase << hex << ppc_state.cr << endl;
    cout << "CTR: " << uppercase << hex << ppc_state.spr[SPR::CTR] << endl;
    cout << "XER: " << uppercase << hex << ppc_state.spr[SPR::XER] << endl;
    cout << "MSR: " << uppercase << hex << ppc_state.msr << endl;
}

void print_fprs()
{
    for (int i = 0; i < 32; i++)
        cout << "FPR " << dec << i << " : " << ppc_state.fpr[i].dbl64_r << endl;
}

static map<string, int> SPRName2Num = {
    {"XER", SPR::XER}, {"LR", SPR::LR}, {"CTR", SPR::CTR}, {"DEC", SPR::DEC},
    {"PVR", SPR::PVR}
};

uint64_t reg_op(string &reg_name, uint64_t val, bool is_write)
{
    string reg_name_u, reg_num_str;
    unsigned reg_num;
    map<string, int>::iterator spr;

    if (reg_name.length() < 2)
        goto bail_out;

    reg_name_u = reg_name;

    /* convert reg_name string to uppercase */
    std::for_each(reg_name_u.begin(), reg_name_u.end(), [](char & c) {
        c = ::toupper(c);
    });

    try {
        if (reg_name_u == "PC") {
            if (is_write)
                ppc_state.pc = val;
            return ppc_state.pc;
        }
        if (reg_name_u == "MSR") {
            if (is_write)
                ppc_state.msr = val;
            return ppc_state.msr;
        }
        if (reg_name_u == "CR") {
            if (is_write)
                ppc_state.cr = val;
            return ppc_state.cr;
        }
        if (reg_name_u == "FPSCR") {
            if (is_write)
                ppc_state.fpscr = val;
            return ppc_state.fpscr;
        }

        if (reg_name_u.substr(0, 1) == "R") {
            reg_num_str = reg_name_u.substr(1);
            reg_num = stoul(reg_num_str, NULL, 0);
            if (reg_num < 32) {
                if (is_write)
                    ppc_state.gpr[reg_num] = val;
                return ppc_state.gpr[reg_num];
            }
        }

        if (reg_name_u.substr(0, 1) == "FR") {
            reg_num_str = reg_name_u.substr(2);
            reg_num = stoul(reg_num_str, NULL, 0);
            if (reg_num < 32) {
                if (is_write)
                    ppc_state.fpr[reg_num].int64_r = val;
                return ppc_state.fpr[reg_num].int64_r;
            }
        }

        if (reg_name_u.substr(0, 3) == "SPR") {
            reg_num_str = reg_name_u.substr(3);
            reg_num = stoul(reg_num_str, NULL, 0);
            if (reg_num < 1024) {
                if (is_write)
                    ppc_state.spr[reg_num] = val;
                return ppc_state.spr[reg_num];
            }
        }

        if (reg_name_u.substr(0, 2) == "SR") {
            reg_num_str = reg_name_u.substr(2);
            reg_num = stoul(reg_num_str, NULL, 0);
            if (reg_num < 16) {
                if (is_write)
                    ppc_state.sr[reg_num] = val;
                return ppc_state.sr[reg_num];
            }
        }

        spr = SPRName2Num.find(reg_name_u);
        if (spr != SPRName2Num.end()) {
            if (is_write)
                ppc_state.spr[spr->second] = val;
            return ppc_state.spr[spr->second];
        }
    }
    catch (...) {
    }

bail_out:
    throw std::invalid_argument(string("Unknown register ") + reg_name);
}

uint64_t get_reg(string &reg_name)
{
    return reg_op(reg_name, 0, false);
}

void set_reg(string &reg_name, uint64_t val)
{
    reg_op(reg_name, val, true);
}
